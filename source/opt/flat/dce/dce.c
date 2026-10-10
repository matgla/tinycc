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
  return name && ir_opt_name_in(name, "abort\0exit\0_Exit\0quick_exit\0");
}

/* Dead Code Elimination pass
 * Removes unreachable instructions by following control flow from entry.
 * Returns 1 if any instructions were eliminated, 0 otherwise.
 */
static int tcc_ir_opt_dce__timed(TCCIRState *ir);
int tcc_ir_opt_dce(TCCIRState *ir)
{
  int r;
  TCC_PASS_TIMED(r, "dce", tcc_ir_opt_dce__timed(ir));
  return r;
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
  /* Likewise for `asm goto`: its labels are edges the IR does not record. */
  if (ir->func_has_asm_goto)
    return 0;

  uint8_t *reachable = tcc_mallocz((n + 7) / 8);
  int *worklist = tcc_malloc(n * sizeof(int));
  IrReachWorklist rw = {reachable, worklist, 0, 0, n};

  /* Start from instruction 0 */
  ir_opt_reach_mark(&rw, 0);

  while (rw.head < rw.tail)
  {
    int i = worklist[rw.head++];
    IRQuadCompact *q = &ir->compact_instructions[i];
    switch (q->op)
    {
    case TCCIR_OP_JUMP:
      /* Unconditional jump - only the target is reachable */
      ir_opt_reach_mark(&rw, (int)tcc_ir_op_dest_u_imm32(ir, q));
      break;
    case TCCIR_OP_JUMPIF:
      /* Conditional jump - both target and fall-through are reachable */
      ir_opt_reach_mark(&rw, (int)tcc_ir_op_dest_u_imm32(ir, q));
      ir_opt_reach_mark(&rw, i + 1);
      break;
    case TCCIR_OP_SWITCH_TABLE:
    {
      /* Switch table - all targets are reachable */
      int table_id = (int)tcc_ir_op_src2_imm(ir, q);
      if (table_id >= 0 && table_id < ir->num_switch_tables)
      {
        TCCIRSwitchTable *table = &ir->switch_tables[table_id];
        for (int j = 0; j < table->num_entries; j++)
          ir_opt_reach_mark(&rw, table->targets[j]);
        /* Also mark the default target */
        ir_opt_reach_mark(&rw, table->default_target);
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
      ir_opt_reach_mark(&rw, i + 1);
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
      Sym *callee = tcc_ir_op_src1_sym(ir, q);
      if (tcc_ir_callee_is_noreturn(callee))
        break; /* terminator: no fall-through */
      ir_opt_reach_mark(&rw, i + 1);
      break;
    }
    default:
      /* All other instructions fall through to the next */
      ir_opt_reach_mark(&rw, i + 1);
      break;
    }
  }


  /* Mark unreachable instructions as NOP (no array compaction needed).
   * Already-NOP instructions must not count as changes: groups that do not
   * compact NOPs between iterations (propagation) would otherwise see the
   * same unreachable NOPs "eliminated" again every round, spinning the group
   * to its iteration cap and invalidating every other pass's dirty state. */
  int changes = 0;
  for (int i = 0; i < n; i++)
  {
    if (!(reachable[i / 8] & (1 << (i % 8))) &&
        ir->compact_instructions[i].op != TCCIR_OP_NOP)
    {
      ir->compact_instructions[i].op = TCCIR_OP_NOP;
      changes++;
    }
  }

  tcc_free(reachable);
  tcc_free(worklist);

  return changes;
}
