/*
 *  TCC IR - Dead code elimination (unreachable-code sweep)
 *
 *  Copyright (c) 2025 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#define USING_GLOBALS

#include "ir.h"
#include "opt.h"
#include "opt_engine.h"
#include "opt_xform.h"
#include "opt_alias.h"
#include "opt_utils.h"
#include "opt_du.h"
#include "opt_loop_utils.h"
#include "cfg.h"
#include "licm.h"

int tcc_ir_callee_is_noreturn(Sym *callee)
{
  if (!callee)
    return 0;
  if (callee->type.ref && callee->type.ref->f.func_noreturn)
    return 1;

  ElfSym *esym = elfsym(callee);
  if (esym && esym->st_shndx != SHN_UNDEF)
    return 0;

  const char *name = get_tok_str(callee->asm_label ? callee->asm_label : callee->v, NULL);
  return name && (!strcmp(name, "abort") || !strcmp(name, "exit") || !strcmp(name, "_Exit") ||
                  !strcmp(name, "quick_exit"));
}

/* Dead Code Elimination pass
 * Removes unreachable instructions by following control flow from entry.
 * Returns 1 if any instructions were eliminated, 0 otherwise.
 */
static int tcc_ir_opt_dce__timed(TCCIRState *ir);
int tcc_ir_opt_dce(TCCIRState *ir)
{
  tcc_pass_timing_init();
  if (!tcc_pass_timing_on) return tcc_ir_opt_dce__timed(ir);
  unsigned long _t = tcc_pass_clk_us();
  int _r = tcc_ir_opt_dce__timed(ir);
  tcc_pass_timing_add("dce", tcc_pass_clk_us() - _t);
  return _r;
}
static int tcc_ir_opt_dce__timed(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  if (n == 0)
    return 0;

  /* If the function contains any IJUMP (computed goto / indirect jump),
   * skip DCE entirely.  The targets of an IJUMP are determined at runtime
   * (typically via labels-as-values stored in arrays), so we cannot
   * statically determine which basic blocks are reachable from them.
   * Attempting to do DCE would incorrectly eliminate label target blocks
   * that are only reachable through the computed goto. */
  for (int i = 0; i < n; i++)
  {
    if (ir->compact_instructions[i].op == TCCIR_OP_IJUMP)
      return 0;
  }

  uint8_t *reachable = tcc_mallocz((n + 7) / 8);
  int *worklist = tcc_malloc(n * sizeof(int));
  int worklist_head = 0, worklist_tail = 0;

/* Mark instruction as reachable if not already marked */
#define MARK_REACHABLE(idx)                                                                                            \
  do                                                                                                                   \
  {                                                                                                                    \
    if ((idx) >= 0 && (idx) < n && !(reachable[(idx) / 8] & (1 << ((idx) % 8))))                                       \
    {                                                                                                                  \
      reachable[(idx) / 8] |= (1 << ((idx) % 8));                                                                      \
      worklist[worklist_tail++] = (idx);                                                                               \
    }                                                                                                                  \
  } while (0)

  /* Start from instruction 0 */
  MARK_REACHABLE(0);

  while (worklist_head < worklist_tail)
  {
    int i = worklist[worklist_head++];
    IRQuadCompact *q = &ir->compact_instructions[i];
    IROperand dest = tcc_ir_op_get_dest(ir, q);
    switch (q->op)
    {
    case TCCIR_OP_JUMP:
      /* Unconditional jump - only the target is reachable */
      MARK_REACHABLE((int)dest.u.imm32);
      break;
    case TCCIR_OP_JUMPIF:
      /* Conditional jump - both target and fall-through are reachable */
      MARK_REACHABLE((int)dest.u.imm32);
      MARK_REACHABLE(i + 1);
      break;
    case TCCIR_OP_SWITCH_TABLE:
    {
      /* Switch table - all targets are reachable */
      IROperand src2 = tcc_ir_op_get_src2(ir, q);
      int table_id = (int)irop_get_imm64_ex(ir, src2);
      if (table_id >= 0 && table_id < ir->num_switch_tables)
      {
        TCCIRSwitchTable *table = &ir->switch_tables[table_id];
        for (int j = 0; j < table->num_entries; j++)
          MARK_REACHABLE(table->targets[j]);
        /* Also mark the default target */
        MARK_REACHABLE(table->default_target);
      }
      /* SWITCH_TABLE is a terminator - no fall-through */
      break;
    }
    case TCCIR_OP_IJUMP:
      /* Indirect jump (computed goto).
         The successor set is not statically known, but in typical patterns
         (like GCC's labels-as-values jump tables) targets are within the same
         function and code continues at/after those labels.
         Conservatively keep fall-through reachable to avoid deleting label
         blocks and subsequent code. */
      MARK_REACHABLE(i + 1);
      break;
    case TCCIR_OP_RETURNVALUE:
    case TCCIR_OP_RETURNVOID:
    case TCCIR_OP_TRAP:
      /* Return/trap - no successor (epilogue is implicit, trap never returns) */
      break;
    case TCCIR_OP_FUNCCALLVAL:
    case TCCIR_OP_FUNCCALLVOID:
    {
      /* If the callee is provably noreturn (either attributed or inferred by
       * the inter-procedural noreturn_collapse / infinite_self_recursion /
       * uninit_dom_return passes — they set sym->f.func_noreturn at end of
       * gen_function), the call never returns and code after it is dead. */
      Sym *callee = irop_get_sym_ex(ir, tcc_ir_op_get_src1(ir, q));
      if (tcc_ir_callee_is_noreturn(callee))
        break; /* terminator: no fall-through */
      MARK_REACHABLE(i + 1);
      break;
    }
    default:
      /* All other instructions fall through to the next */
      MARK_REACHABLE(i + 1);
      break;
    }
  }

#undef MARK_REACHABLE

  /* Mark unreachable instructions as NOP (no array compaction needed) */
  int changes = 0;
  for (int i = 0; i < n; i++)
  {
    if (!(reachable[i / 8] & (1 << (i % 8))))
    {
      ir->compact_instructions[i].op = TCCIR_OP_NOP;
      changes++;
    }
  }

  tcc_free(reachable);
  tcc_free(worklist);

  return changes;
}

int tcc_ir_opt_dce_ex(IROptCtx *ctx)
{
  return tcc_ir_opt_dce(ctx->ir);
}
