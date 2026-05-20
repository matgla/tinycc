/*
 *  TCC IR - Dead Code & Cleanup Passes
 *
 *  Copyright (c) 2025 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#define USING_GLOBALS

#include "ir.h"
#include "opt_engine.h"
#include "opt_xform.h"
#include "opt_alias.h"
#include "opt_utils.h"
#include "opt_du.h"
#include "opt_loop_utils.h"

/* Dead Code Elimination pass
 * Removes unreachable instructions by following control flow from entry.
 * Returns 1 if any instructions were eliminated, 0 otherwise.
 */
int tcc_ir_opt_dce(TCCIRState *ir)
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

/* ============================================================================
 * Useless Function Body - if no instruction in the function has an observable
 * side effect, NOP the entire body.
 * ============================================================================
 *
 * After all the data-flow folds (const prop, store-load fwd, DCE/DSE) run,
 * a function body can be reduced to a forest of LOAD/CMP/JUMPIF chains whose
 * eventual consumer (a STORE, CALL, RETURNVALUE, ...) has been killed.  None
 * of the surviving ops can be eliminated individually:
 *   - LOAD/ASSIGN from a non-volatile sym: still has a "use" via the temp
 *   - CMP: uses the temp from the LOAD
 *   - JUMPIF: keeps the CMP alive (control op, no temp dest)
 * Each link of the chain is alive because the next link reads it, but the
 * tail of the chain (the JUMPIF) leads nowhere observable.
 *
 * If we can prove the *entire function* has no observable side effect, we
 * can drop the whole body.  This is the GCC -O2 behavior for things like
 * compile/20040304-2.c — a void function whose only "work" is comparing
 * cast-folded zeros and conditionally storing into a dead path.
 *
 * Essential ops (function has observable behavior; skip the pass):
 *   - STORE / STORE_INDEXED / STORE_POSTINC (memory write)
 *   - FUNCCALLVAL / FUNCCALLVOID (call — may have side effects)
 *   - FUNCPARAMVAL / FUNCPARAMVOID (call argument plumbing — kept with calls)
 *   - RETURNVALUE / TRAP / IJUMP
 *   - INLINE_ASM / ASM_INPUT / ASM_OUTPUT
 *   - CALLSEQ_BEGIN / CALLARG_REG / CALLARG_STACK / CALLSEQ_END
 *   - INIT_CHAIN_SLOT (writes nested-fn chain slot)
 *   - PREFETCH (architectural hint — keep conservatively)
 *   - SETJMP / LONGJMP / NL_SETJMP / NL_LONGJMP
 *   - BUILTIN_APPLY_ARGS / BUILTIN_APPLY / BUILTIN_RETURN
 *   - VLA_ALLOC / VLA_SP_SAVE / VLA_SP_RESTORE (stack manipulation)
 *   - BLOCK_COPY (memory write)
 *   - SWITCH_TABLE / SWITCH_LOAD (jumps + loads)
 *   - any LOAD/ASSIGN/CMP/... whose src1/src2 references a sym with VT_VOLATILE
 *
 * Everything else is "non-essential": pure arithmetic, comparisons, jumps,
 * loads from non-volatile memory, etc.  When the entire body is non-essential,
 * the function is observationally a no-op and we NOP everything.
 */
static int ir_opt_op_is_essential(TCCIRState *ir, IRQuadCompact *q, int idx)
{
  switch (q->op)
  {
  case TCCIR_OP_JUMP:
  case TCCIR_OP_JUMPIF:
  {
    /* Backward / self-targeting jumps form loops whose non-termination is
     * itself observable.  uninit_local_ub deliberately collapses functions
     * that read uninit locals to a single JUMP-to-self ("b ."); forward
     * jumps just route over NOPs and can be safely dropped. */
    IROperand dest = tcc_ir_op_get_dest(ir, q);
    int target = (int)dest.u.imm32;
    if (target >= 0 && target <= idx)
      return 1;
    return 0;
  }
  case TCCIR_OP_STORE:
  case TCCIR_OP_STORE_INDEXED:
  case TCCIR_OP_STORE_POSTINC:
  case TCCIR_OP_FUNCCALLVAL:
  case TCCIR_OP_FUNCCALLVOID:
  case TCCIR_OP_FUNCPARAMVAL:
  case TCCIR_OP_FUNCPARAMVOID:
  case TCCIR_OP_RETURNVALUE:
  case TCCIR_OP_TRAP:
  case TCCIR_OP_IJUMP:
  case TCCIR_OP_INLINE_ASM:
  case TCCIR_OP_ASM_INPUT:
  case TCCIR_OP_ASM_OUTPUT:
  case TCCIR_OP_CALLSEQ_BEGIN:
  case TCCIR_OP_CALLARG_REG:
  case TCCIR_OP_CALLARG_STACK:
  case TCCIR_OP_CALLSEQ_END:
  case TCCIR_OP_INIT_CHAIN_SLOT:
  case TCCIR_OP_PREFETCH:
  case TCCIR_OP_SETJMP:
  case TCCIR_OP_LONGJMP:
  case TCCIR_OP_NL_SETJMP:
  case TCCIR_OP_NL_LONGJMP:
  case TCCIR_OP_BUILTIN_APPLY_ARGS:
  case TCCIR_OP_BUILTIN_APPLY:
  case TCCIR_OP_BUILTIN_RETURN:
  case TCCIR_OP_VLA_ALLOC:
  case TCCIR_OP_VLA_SP_SAVE:
  case TCCIR_OP_VLA_SP_RESTORE:
  case TCCIR_OP_BLOCK_COPY:
  case TCCIR_OP_SWITCH_TABLE:
  case TCCIR_OP_SWITCH_LOAD:
    return 1;
  default:
    break;
  }

  /* Volatile sym read on any source: keep the function alive. */
  if (irop_config[q->op].has_src1)
  {
    IROperand s = tcc_ir_op_get_src1(ir, q);
    if (s.is_sym)
    {
      Sym *sym = irop_get_sym_ex(ir, s);
      if (sym && (sym->type.t & VT_VOLATILE))
        return 1;
    }
  }
  if (irop_config[q->op].has_src2)
  {
    IROperand s = tcc_ir_op_get_src2(ir, q);
    if (s.is_sym)
    {
      Sym *sym = irop_get_sym_ex(ir, s);
      if (sym && (sym->type.t & VT_VOLATILE))
        return 1;
    }
  }
  return 0;
}

int tcc_ir_opt_useless_function_body(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  if (n == 0)
    return 0;

  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;
    if (ir_opt_op_is_essential(ir, q, i))
      return 0;
  }

  int changes = 0;
  for (int i = 0; i < n; i++)
  {
    if (ir->compact_instructions[i].op != TCCIR_OP_NOP)
    {
      ir->compact_instructions[i].op = TCCIR_OP_NOP;
      changes++;
    }
  }

  /* Regalloc has already run by the time this pass fires, so the dirty
   * register bitmap reflects the pre-NOP IR.  With every op now NOP, no
   * register is actually live — clearing the bitmap (and per-instruction
   * live-reg map, if any) lets the prologue skip the otherwise-spurious
   * `push/pop {rN}` for a "saved" register the body never touches.  Also
   * mark the function as a leaf so save_lr drops away. */
  ir->ls.dirty_registers = 0;
  ir->ls.dirty_float_registers = 0;
  if (ir->ls.live_regs_by_instruction && ir->ls.live_regs_by_instruction_size > 0)
    memset(ir->ls.live_regs_by_instruction, 0,
           ir->ls.live_regs_by_instruction_size * sizeof(ir->ls.live_regs_by_instruction[0]));
  ir->leaffunc = 1;

  LOG_IR_GEN("USELESS-BODY: NOPed %d instructions (no observable side effects)", changes);
  return changes;
}

int tcc_ir_opt_useless_function_body_ex(IROptCtx *ctx)
{
  return tcc_ir_opt_useless_function_body(ctx->ir);
}

/* No-Return Function Collapse
 *
 * If a function provably never returns (no RETURNVALUE/RETURNVOID anywhere)
 * and has no other observable-from-outside effects (no FUNCCALL whose callee
 * could publish state, no inline asm, no volatile sym access, no setjmp/
 * longjmp/trap/VLA primitives, no computed goto), then no caller can observe
 * any of its writes — the function's effect is indistinguishable from
 * `b .`.  Collapse the whole body to a single self-jump, matching GCC's
 * -O2 behavior on patterns like gcc.c-torture/compile/pr70916.c where every
 * path bottoms out in an infinite loop.
 *
 * Pre-condition: useless_function_body left this body alone because it
 * contains STOREs (or other essential-but-elidable ops); we accept those
 * here because the function's non-return makes them unobservable.
 */
int tcc_ir_opt_noreturn_collapse(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  if (n == 0)
    return 0;
  /* O2-only: aggressive elision of observable-but-unreachable side effects
   * (caller can't see them because we never return).  Matches the gating
   * philosophy of uninit_local_ub. */
  if (!tcc_state || tcc_state->optimize < 2)
    return 0;

  int has_jump = 0;
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;

    switch (q->op)
    {
    /* Any return op means the function CAN return; collapse would change
     * semantics by skipping the side effects on the returning path. */
    case TCCIR_OP_RETURNVALUE:
    case TCCIR_OP_RETURNVOID:
    /* Calls publish state we can't elide: the callee may write through
     * pointers we passed, run signal handlers, etc. */
    case TCCIR_OP_FUNCCALLVAL:
    case TCCIR_OP_FUNCCALLVOID:
    case TCCIR_OP_CALLSEQ_BEGIN:
    case TCCIR_OP_CALLARG_REG:
    case TCCIR_OP_CALLARG_STACK:
    case TCCIR_OP_CALLSEQ_END:
    /* Inline asm could do anything (including exit / sync). */
    case TCCIR_OP_INLINE_ASM:
    case TCCIR_OP_ASM_INPUT:
    case TCCIR_OP_ASM_OUTPUT:
    /* Non-local control flow can return to the caller through a longjmp. */
    case TCCIR_OP_SETJMP:
    case TCCIR_OP_LONGJMP:
    case TCCIR_OP_NL_SETJMP:
    case TCCIR_OP_NL_LONGJMP:
    case TCCIR_OP_BUILTIN_APPLY_ARGS:
    case TCCIR_OP_BUILTIN_APPLY:
    case TCCIR_OP_BUILTIN_RETURN:
    /* Trap may dispatch to a fault/signal handler that observes state. */
    case TCCIR_OP_TRAP:
    /* VLA / SP juggling: stack-pointer side effects the prologue tracks. */
    case TCCIR_OP_VLA_ALLOC:
    case TCCIR_OP_VLA_SP_SAVE:
    case TCCIR_OP_VLA_SP_RESTORE:
    /* Computed goto: unknown-at-compile-time target — defensive bail. */
    case TCCIR_OP_IJUMP:
      return 0;
    case TCCIR_OP_JUMP:
    case TCCIR_OP_JUMPIF:
    case TCCIR_OP_SWITCH_TABLE:
    case TCCIR_OP_SWITCH_LOAD:
      has_jump = 1;
      break;
    default:
      break;
    }

    /* Volatile sym read/write on any operand: keep the function alive — the
     * write is observable through the volatile memory model regardless of
     * whether we return. */
    for (int k = 0; k <= 2; k++)
    {
      IROperand op;
      if (k == 0)
      {
        if (!irop_config[q->op].has_dest)
          continue;
        op = tcc_ir_op_get_dest(ir, q);
      }
      else if (k == 1)
      {
        if (!irop_config[q->op].has_src1)
          continue;
        op = tcc_ir_op_get_src1(ir, q);
      }
      else
      {
        if (!irop_config[q->op].has_src2)
          continue;
        op = tcc_ir_op_get_src2(ir, q);
      }
      if (op.is_sym)
      {
        Sym *sym = irop_get_sym_ex(ir, op);
        if (sym && (sym->type.t & VT_VOLATILE))
          return 0;
      }
    }
  }

  /* Need at least one JUMP — otherwise the function would fall off the end,
   * which is an implicit return. */
  if (!has_jump)
    return 0;

  /* Implicit-return detection: TCC does not always emit an explicit
   * RETURNVOID for void functions — control simply falls off the end of the
   * IR, and the backend emits a `bx lr` after the last op.  If the last
   * non-NOP instruction can fall through (anything other than an
   * unconditional JUMP), the function still returns and we must not
   * collapse.  SWITCH_TABLE / IJUMP could in principle exit cleanly, but
   * have unknown-at-this-pass targets; we already bailed on IJUMP above,
   * and we conservatively also refuse to collapse when the last op is
   * SWITCH_TABLE. */
  int last_idx = -1;
  for (int i = n - 1; i >= 0; i--)
  {
    if (ir->compact_instructions[i].op != TCCIR_OP_NOP)
    {
      last_idx = i;
      break;
    }
  }
  if (last_idx < 0)
    return 0;
  if (ir->compact_instructions[last_idx].op != TCCIR_OP_JUMP)
    return 0;

  LOG_IR_GEN("NORETURN-COLLAPSE: collapsing function body to infinite loop "
             "(no RETURN, no calls/asm/volatile — side effects unobservable)");

  for (int i = 0; i < n; i++)
  {
    ir->compact_instructions[i].op = TCCIR_OP_NOP;
    ir->compact_instructions[i].is_jump_target = 0;
  }

  ir->compact_instructions[0].op = TCCIR_OP_JUMP;
  ir->compact_instructions[0].is_jump_target = 1;
  IROperand self = irop_make_imm32(-1, 0, IROP_BTYPE_INT32);
  tcc_ir_set_dest(ir, 0, self);
  tcc_ir_set_src1(ir, 0, IROP_NONE);
  tcc_ir_set_src2(ir, 0, IROP_NONE);

  ir->ls.dirty_registers = 0;
  ir->ls.dirty_float_registers = 0;
  if (ir->ls.live_regs_by_instruction && ir->ls.live_regs_by_instruction_size > 0)
    memset(ir->ls.live_regs_by_instruction, 0,
           ir->ls.live_regs_by_instruction_size * sizeof(ir->ls.live_regs_by_instruction[0]));
  ir->leaffunc = 1;
  /* Suppress the unreachable `bx lr` after the self-jump: control never
   * reaches the epilogue, so emitting it just wastes 2 bytes. */
  ir->noreturn = 1;

  return 1;
}

int tcc_ir_opt_noreturn_collapse_ex(IROptCtx *ctx)
{
  return tcc_ir_opt_noreturn_collapse(ctx->ir);
}

/* ============================================================================
 * NOP Compaction - remove NOP instructions from the instruction array
 * ============================================================================
 *
 * After multiple optimization passes, many instructions are marked as NOP.
 * Every subsequent pass still iterates over them.  This pass removes NOPs
 * in a single O(n) sweep, shrinks the array, and fixes all jump targets.
 *
 * Invariants preserved:
 *   - orig_index on each IRQuadCompact is NOT modified (codegen needs it)
 *   - operand_base indices into iroperand_pool are stable (pool is append-only)
 *   - switch_table targets are remapped
 *   - is_jump_target flags are re-derived from remapped jumps
 */
int tcc_ir_opt_compact_nops(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  if (n == 0)
    return 0;

  IRQuadCompact *instr = ir->compact_instructions;

  /* Quick check: any NOPs at all? */
  int has_nops = 0;
  for (int i = 0; i < n; i++)
  {
    if (instr[i].op == TCCIR_OP_NOP)
    {
      has_nops = 1;
      break;
    }
  }
  if (!has_nops)
    return 0;

  /* Build old_to_new mapping and compact in one forward pass */
  int *old_to_new = tcc_malloc(n * sizeof(int));
  int write_pos = 0;

  for (int i = 0; i < n; i++)
  {
    if (instr[i].op == TCCIR_OP_NOP)
    {
      old_to_new[i] = -1;
    }
    else
    {
      old_to_new[i] = write_pos;
      if (write_pos != i)
        instr[write_pos] = instr[i];
      write_pos++;
    }
  }

  int removed = n - write_pos;
  if (removed == 0)
  {
    tcc_free(old_to_new);
    return 0;
  }

  /* Fix jump targets in JUMP / JUMPIF instructions.
   * Targets can be in [0, n] — target == n means "epilogue" (one past the
   * last instruction), set by tcc_ir_backpatch_to_here for return jumps. */
  for (int i = 0; i < write_pos; i++)
  {
    IRQuadCompact *q = &instr[i];
    if (q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_JUMPIF)
    {
      IROperand dest = tcc_ir_op_get_dest(ir, q);
      int old_target = (int)irop_get_imm64_ex(ir, dest);
      if (old_target < 0)
        continue;

      int new_target;
      if (old_target >= n)
      {
        /* Epilogue target (past-end): remap to new past-end */
        new_target = write_pos + (old_target - n);
      }
      else
      {
        new_target = old_to_new[old_target];
        if (new_target < 0)
        {
          /* Target was a NOP — find the next non-NOP instruction after it.
           * This shouldn't normally happen (DCE + jump threading should have
           * fixed dangling targets), but handle it defensively. */
          for (int j = old_target + 1; j < n; j++)
          {
            if (old_to_new[j] >= 0)
            {
              new_target = old_to_new[j];
              break;
            }
          }
          if (new_target < 0)
            new_target = write_pos; /* fall through to epilogue */
        }
      }
      if (new_target != old_target)
      {
        IROperand new_dest = irop_make_imm32(-1, new_target, IROP_BTYPE_INT32);
        tcc_ir_op_set_dest(ir, q, new_dest);
      }
    }
  }

  /* Fix switch table targets — same epilogue-aware remapping as jumps */
  for (int t = 0; t < ir->num_switch_tables; t++)
  {
    TCCIRSwitchTable *table = &ir->switch_tables[t];
    for (int j = 0; j < table->num_entries; j++)
    {
      int old_t = table->targets[j];
      if (old_t < 0)
        continue;
      if (old_t >= n)
      {
        table->targets[j] = write_pos + (old_t - n);
      }
      else
      {
        int new_t = old_to_new[old_t];
        if (new_t < 0)
        {
          for (int k = old_t + 1; k < n; k++)
          {
            if (old_to_new[k] >= 0)
            {
              new_t = old_to_new[k];
              break;
            }
          }
          if (new_t < 0)
            new_t = write_pos;
        }
        table->targets[j] = new_t;
      }
    }
    {
      int old_dt = table->default_target;
      if (old_dt >= 0)
      {
        if (old_dt >= n)
        {
          table->default_target = write_pos + (old_dt - n);
        }
        else
        {
          int new_dt = old_to_new[old_dt];
          if (new_dt < 0)
          {
            for (int k = old_dt + 1; k < n; k++)
            {
              if (old_to_new[k] >= 0)
              {
                new_dt = old_to_new[k];
                break;
              }
            }
            if (new_dt < 0)
              new_dt = write_pos;
          }
          table->default_target = new_dt;
        }
      }
    }
  }

  /* Re-derive is_jump_target flags from scratch */
  for (int i = 0; i < write_pos; i++)
    instr[i].is_jump_target = 0;

  for (int i = 0; i < write_pos; i++)
  {
    IRQuadCompact *q = &instr[i];
    if (q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_JUMPIF)
    {
      IROperand dest = tcc_ir_op_get_dest(ir, q);
      int target = (int)irop_get_imm64_ex(ir, dest);
      if (target >= 0 && target < write_pos)
        instr[target].is_jump_target = 1;
    }
  }
  for (int t = 0; t < ir->num_switch_tables; t++)
  {
    TCCIRSwitchTable *table = &ir->switch_tables[t];
    for (int j = 0; j < table->num_entries; j++)
    {
      if (table->targets[j] >= 0 && table->targets[j] < write_pos)
        instr[table->targets[j]].is_jump_target = 1;
    }
    if (table->default_target >= 0 && table->default_target < write_pos)
      instr[table->default_target].is_jump_target = 1;
  }

  ir->next_instruction_index = write_pos;

  tcc_free(old_to_new);

  return removed;
}

int tcc_ir_opt_compact_nops_ex(IROptCtx *ctx)
{
  int removed = tcc_ir_opt_compact_nops(ctx->ir);
  if (removed > 0)
    tcc_ir_opt_ctx_invalidate(ctx);
  return removed;
}

/* Constant VAR Propagation - propagate constant-assigned VAR vregs into uses.
 * Designed to run after store-load forwarding which may convert stack loads
 * into constant assignments (e.g. V0 <-- #34 [ASSIGN]) that the main
 * optimization loop's const_prop never saw.
 *
 * Unlike const_prop, this pass:
 *   - Does not check is_local/is_lval flags (safe since we verified single def)
 *   - Converts LOAD→ASSIGN when replacing a local-variable source with a constant
 *   - Handles both src1 and src2 operands
 *
 * This enables the register allocator to avoid callee-saved registers for
 * values that are cheap to rematerialize (small immediates).
 */

int tcc_ir_opt_dse(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  if (n == 0)
    return 0;

  /* Orphaned PARAM elimination: NOP FUNCPARAMVAL/FUNCPARAMVOID instructions
   * whose call_id has no matching FUNCCALLVAL/FUNCCALLVOID.
   * This happens when a function is inlined: the CALL is replaced with inline
   * code but the PARAM (e.g. struct return buffer address) is left behind.
   * Eliminating them removes false address-takes that prevent dead store elim.
   *
   * Combined pass 1+2: single scan that tracks max_call_id AND builds has_call[]
   * (grows dynamically as new call_ids are observed). */
  {
    uint8_t *has_call = NULL;
    int has_call_bytes = 0;
    int max_call_id = 0;
    int saw_any = 0;

    for (int i = 0; i < n; i++)
    {
      IRQuadCompact *q = &ir->compact_instructions[i];
      if (q->op == TCCIR_OP_NOP)
        continue;
      if (q->op != TCCIR_OP_FUNCPARAMVAL && q->op != TCCIR_OP_FUNCPARAMVOID && q->op != TCCIR_OP_FUNCCALLVAL &&
          q->op != TCCIR_OP_FUNCCALLVOID)
        continue;

      saw_any = 1;
      IROperand src2 = tcc_ir_op_get_src2(ir, q);
      int cid = TCCIR_DECODE_CALL_ID((int32_t)irop_get_imm64_ex(ir, src2));
      if (cid > max_call_id)
        max_call_id = cid;

      if (q->op == TCCIR_OP_FUNCCALLVAL || q->op == TCCIR_OP_FUNCCALLVOID)
      {
        int needed_bytes = (cid / 8) + 1;
        if (needed_bytes > has_call_bytes)
        {
          int new_bytes = has_call_bytes ? has_call_bytes * 2 : 32;
          while (new_bytes < needed_bytes)
            new_bytes *= 2;
          has_call = tcc_realloc(has_call, new_bytes);
          memset(has_call + has_call_bytes, 0, new_bytes - has_call_bytes);
          has_call_bytes = new_bytes;
        }
        has_call[cid / 8] |= (1 << (cid % 8));
      }
    }

    if (saw_any && max_call_id > 0)
    {
      /* NOP orphaned PARAMs */
      for (int i = 0; i < n; i++)
      {
        IRQuadCompact *q = &ir->compact_instructions[i];
        if (q->op == TCCIR_OP_FUNCPARAMVAL || q->op == TCCIR_OP_FUNCPARAMVOID)
        {
          IROperand src2 = tcc_ir_op_get_src2(ir, q);
          int cid = TCCIR_DECODE_CALL_ID((int32_t)irop_get_imm64_ex(ir, src2));
          int byte_idx = cid / 8;
          int has = (byte_idx < has_call_bytes) && (has_call[byte_idx] & (1 << (cid % 8)));
          if (cid <= max_call_id && !has)
          {
            LOG_IR_GEN("OPTIMIZE: Orphaned PARAM at i=%d (call_id=%d has no CALL)", i, cid);
            q->op = TCCIR_OP_NOP;
          }
        }
      }
    }

    if (has_call)
      tcc_free(has_call);
  }

  /* Pre-scan: eliminate pure FUNCCALLVOIDs and their PARAMs.  After
   * dead_call_result demotes an unused-result FUNCCALLVAL → FUNCCALLVOID,
   * the call has no dest TMP and therefore won't be seeded by the
   * use_count-driven loop below.  Doing this before use_count[] is built
   * means the cascading loop will see the lowered use counts of any TMPs
   * that fed PARAMs and naturally eliminate them. */
  int pure_call_changes = 0;
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_FUNCCALLVOID)
      continue;
    Sym *callee = irop_get_sym_ex(ir, tcc_ir_op_get_src1(ir, q));
    if (!callee)
      continue;
    /* Only the curated aeabi soft-float/long-int helpers qualify here.
     * They are known to return by value (in registers), so a FUNCCALLVOID
     * — produced by dead_call_result when the result TMP was unused —
     * is safely dead.  We deliberately exclude attribute((pure)) user
     * functions because they may return a struct/_Complex via an sret
     * pointer arg; the sret target may still be live.  Those cases are
     * the dedicated dead_sret_call pass's job. */
    const char *name = get_tok_str(callee->v, NULL);
    if (!name || !tcc_ir_is_pure_aeabi(name))
      continue;
    LOG_IR_GEN("DCE PURE-CALL: nop FUNCCALLVOID at i=%d (callee=%s)", i,
               get_tok_str(callee->v, NULL) ? get_tok_str(callee->v, NULL) : "?");
    ir_opt_nop_call_params(ir, i);
    q->op = TCCIR_OP_NOP;
    pure_call_changes++;
  }

  /* Worklist-based cascading DSE.
   * Single O(n) combined pass: find max_tmp_pos, build use_count[] and
   * def_idx[] simultaneously, with dynamic growth of the per-TMP tables. */
  int max_tmp_pos = 0;
  int table_cap = 32;
  uint16_t *use_count = tcc_mallocz(table_cap * sizeof(uint16_t));
  int *def_idx = tcc_malloc(table_cap * sizeof(int));
  for (int i = 0; i < table_cap; i++)
    def_idx[i] = -1;

  /* Ensure use_count[] / def_idx[] can index position _p (grows on demand) */
#define DSE_ENSURE_CAP(_p)                                                                                             \
  do                                                                                                                   \
  {                                                                                                                    \
    int _pp = (_p);                                                                                                    \
    if (_pp >= table_cap)                                                                                              \
    {                                                                                                                  \
      int _new_cap = table_cap * 2;                                                                                    \
      while (_new_cap <= _pp)                                                                                          \
        _new_cap *= 2;                                                                                                 \
      use_count = tcc_realloc(use_count, _new_cap * sizeof(uint16_t));                                                 \
      memset(use_count + table_cap, 0, (_new_cap - table_cap) * sizeof(uint16_t));                                     \
      def_idx = tcc_realloc(def_idx, _new_cap * sizeof(int));                                                          \
      for (int _k = table_cap; _k < _new_cap; _k++)                                                                    \
        def_idx[_k] = -1;                                                                                              \
      table_cap = _new_cap;                                                                                            \
    }                                                                                                                  \
    if (_pp > max_tmp_pos)                                                                                             \
      max_tmp_pos = _pp;                                                                                               \
  } while (0)

#define DSE_INC_USE(_pos)                                                                                              \
  do                                                                                                                   \
  {                                                                                                                    \
    int _p = (_pos);                                                                                                   \
    if (_p >= 0)                                                                                                       \
    {                                                                                                                  \
      DSE_ENSURE_CAP(_p);                                                                                              \
      if (use_count[_p] < 0xFFFF)                                                                                      \
        use_count[_p]++;                                                                                               \
    }                                                                                                                  \
  } while (0)

  /* Single O(n) pass: build use_count[] and def_idx[] */
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;

    if (irop_config[q->op].has_src1)
    {
      const IROperand s = tcc_ir_op_get_src1(ir, q);
      if (TCCIR_DECODE_VREG_TYPE(irop_get_vreg(s)) == TCCIR_VREG_TYPE_TEMP)
        DSE_INC_USE(TCCIR_DECODE_VREG_POSITION(irop_get_vreg(s)));
    }
    if (irop_config[q->op].has_src2)
    {
      const IROperand s = tcc_ir_op_get_src2(ir, q);
      if (TCCIR_DECODE_VREG_TYPE(irop_get_vreg(s)) == TCCIR_VREG_TYPE_TEMP)
        DSE_INC_USE(TCCIR_DECODE_VREG_POSITION(irop_get_vreg(s)));
    }

    const IROperand dest = tcc_ir_op_get_dest(ir, q);
    /* STORE/STORE_INDEXED dest is a pointer use, not a def */
    if ((q->op == TCCIR_OP_STORE || q->op == TCCIR_OP_STORE_INDEXED) &&
        TCCIR_DECODE_VREG_TYPE(irop_get_vreg(dest)) == TCCIR_VREG_TYPE_TEMP)
      DSE_INC_USE(TCCIR_DECODE_VREG_POSITION(irop_get_vreg(dest)));
    /* FUNCPARAMVAL dest carries the parameter value — it's a use */
    if (q->op == TCCIR_OP_FUNCPARAMVAL && TCCIR_DECODE_VREG_TYPE(irop_get_vreg(dest)) == TCCIR_VREG_TYPE_TEMP)
      DSE_INC_USE(TCCIR_DECODE_VREG_POSITION(irop_get_vreg(dest)));
    /* MLA accumulator is a use */
    if (q->op == TCCIR_OP_MLA)
    {
      const IROperand acc = tcc_ir_op_get_accum(ir, q);
      if (TCCIR_DECODE_VREG_TYPE(irop_get_vreg(acc)) == TCCIR_VREG_TYPE_TEMP)
        DSE_INC_USE(TCCIR_DECODE_VREG_POSITION(irop_get_vreg(acc)));
    }

    /* Record def site for TEMP-destination ops where dest is a real def */
    if (irop_config[q->op].has_dest && TCCIR_DECODE_VREG_TYPE(irop_get_vreg(dest)) == TCCIR_VREG_TYPE_TEMP &&
        q->op != TCCIR_OP_STORE && q->op != TCCIR_OP_STORE_INDEXED && q->op != TCCIR_OP_FUNCPARAMVAL)
    {
      int pos = TCCIR_DECODE_VREG_POSITION(irop_get_vreg(dest));
      if (pos >= 0)
      {
        DSE_ENSURE_CAP(pos);
        def_idx[pos] = i; /* last def wins; OK since we only eliminate when use_count hits 0 */
      }
    }
  }

  if (max_tmp_pos == 0)
  {
    tcc_free(use_count);
    tcc_free(def_idx);
    return pure_call_changes;
  }

  int changes = pure_call_changes;
  LOG_IR_GEN("=== DEAD STORE ELIMINATION START ===");

  /* True iff this CALL targets a by-value-returning, side-effect-free
   * callee.  Restricted to the curated aeabi soft-float/long-int helper
   * list — those are known to never use an sret arg.  See the matching
   * comment in the FUNCCALLVOID pre-scan above. */
#define DSE_IS_PURE_CALL(_q)                                                                                           \
  ({                                                                                                                   \
    int _pure = 0;                                                                                                     \
    if ((_q)->op == TCCIR_OP_FUNCCALLVAL || (_q)->op == TCCIR_OP_FUNCCALLVOID)                                         \
    {                                                                                                                  \
      Sym *_callee = irop_get_sym_ex(ir, tcc_ir_op_get_src1(ir, (_q)));                                                \
      if (_callee)                                                                                                     \
      {                                                                                                                \
        const char *_name = get_tok_str(_callee->v, NULL);                                                             \
        if (_name && tcc_ir_is_pure_aeabi(_name))                                                                      \
          _pure = 1;                                                                                                   \
      }                                                                                                                \
    }                                                                                                                  \
    _pure;                                                                                                             \
  })

  /* Reusable dead-eligibility predicate.
   *
   * A LOAD is eligible (dead-result kills the op) when its source has no
   * possible side effect of being read: immediates, symref reads, reads
   * from anonymous TEMP_LOCAL stack slots (vreg in [-9,-2]), and reads
   * from any LOCAL stack address (IROP_TAG_STACKOFF + is_local) all
   * qualify — none can be observed by another agent.  Without the local
   * stack case, a chain of write → LOAD-local + dead consumer leaves the
   * LOAD undead, which keeps the source slot artificially alive in the
   * downstream dead_local_slot_elim pass. */
#define DSE_IS_DEAD_ELIGIBLE(_q)                                                                                       \
  (((_q)->op != TCCIR_OP_STORE && (_q)->op != TCCIR_OP_STORE_INDEXED && (_q)->op != TCCIR_OP_STORE_POSTINC &&          \
    (_q)->op != TCCIR_OP_LOAD_POSTINC && (_q)->op != TCCIR_OP_LOAD && (_q)->op != TCCIR_OP_FUNCCALLVAL &&              \
    (_q)->op != TCCIR_OP_FUNCCALLVOID && (_q)->op != TCCIR_OP_FUNCPARAMVAL && (_q)->op != TCCIR_OP_FUNCPARAMVOID) ||   \
   ((_q)->op == TCCIR_OP_LOAD &&                                                                                       \
    (irop_is_immediate(tcc_ir_op_get_src1(ir, (_q))) || tcc_ir_op_get_src1(ir, (_q)).is_sym ||                         \
     (irop_get_vreg(tcc_ir_op_get_src1(ir, (_q))) <= -2 && irop_get_vreg(tcc_ir_op_get_src1(ir, (_q))) >= -9) ||       \
     (irop_get_tag(tcc_ir_op_get_src1(ir, (_q))) == IROP_TAG_STACKOFF &&                                               \
      tcc_ir_op_get_src1(ir, (_q)).is_local))) ||                                                                      \
   DSE_IS_PURE_CALL(_q))

  /* When NOP'ing a pure call, also NOP its matching PARAMs and decrement
   * use_count for any TMPs they consumed. Pushes newly-dead TMPs to the
   * worklist so the cascade can continue. */
#define DSE_CASCADE_PURE_CALL_PARAMS(_call_idx)                                                                        \
  do                                                                                                                   \
  {                                                                                                                    \
    int _ci = (_call_idx);                                                                                             \
    IRQuadCompact *_cq = &ir->compact_instructions[_ci];                                                               \
    int _cid = TCCIR_DECODE_CALL_ID((uint32_t)irop_get_imm64_ex(ir, tcc_ir_op_get_src2(ir, _cq)));                     \
    for (int _pi = _ci - 1; _pi >= 0; --_pi)                                                                           \
    {                                                                                                                  \
      IRQuadCompact *_pq = &ir->compact_instructions[_pi];                                                             \
      if (_pq->op == TCCIR_OP_NOP)                                                                                     \
        continue;                                                                                                      \
      if (_pq->op != TCCIR_OP_FUNCPARAMVAL && _pq->op != TCCIR_OP_FUNCPARAMVOID)                                       \
        continue;                                                                                                      \
      int _pid = TCCIR_DECODE_CALL_ID((uint32_t)irop_get_imm64_ex(ir, tcc_ir_op_get_src2(ir, _pq)));                   \
      if (_pid != _cid)                                                                                                \
        continue;                                                                                                      \
      if (irop_config[_pq->op].has_src1)                                                                               \
      {                                                                                                                \
        const IROperand _s = tcc_ir_op_get_src1(ir, _pq);                                                              \
        if (TCCIR_DECODE_VREG_TYPE(irop_get_vreg(_s)) == TCCIR_VREG_TYPE_TEMP)                                         \
        {                                                                                                              \
          int _pp = TCCIR_DECODE_VREG_POSITION(irop_get_vreg(_s));                                                     \
          if (_pp >= 0 && _pp <= max_tmp_pos && use_count[_pp] > 0)                                                    \
            if (--use_count[_pp] == 0)                                                                                 \
              worklist[wl_top++] = _pp;                                                                                \
        }                                                                                                              \
      }                                                                                                                \
      if (_pq->op == TCCIR_OP_FUNCPARAMVAL)                                                                            \
      {                                                                                                                \
        const IROperand _d = tcc_ir_op_get_dest(ir, _pq);                                                              \
        if (TCCIR_DECODE_VREG_TYPE(irop_get_vreg(_d)) == TCCIR_VREG_TYPE_TEMP)                                         \
        {                                                                                                              \
          int _pp = TCCIR_DECODE_VREG_POSITION(irop_get_vreg(_d));                                                     \
          if (_pp >= 0 && _pp <= max_tmp_pos && use_count[_pp] > 0)                                                    \
            if (--use_count[_pp] == 0)                                                                                 \
              worklist[wl_top++] = _pp;                                                                                \
        }                                                                                                              \
      }                                                                                                                \
      LOG_IR_GEN("DCE PURE-CALL: nop PARAM i=%d (call_id=%d)", _pi, _cid);                                             \
      _pq->op = TCCIR_OP_NOP;                                                                                          \
      changes++;                                                                                                       \
    }                                                                                                                  \
  } while (0)

  /* Worklist of TMP positions whose use_count just dropped to 0 */
  int *worklist = tcc_malloc((max_tmp_pos + 1) * sizeof(int));
  int wl_top = 0;

  /* Seed the worklist by eliminating all initially-dead instructions */
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;
    const IROperand dest = tcc_ir_op_get_dest(ir, q);
    if (!irop_config[q->op].has_dest)
      continue;
    if (TCCIR_DECODE_VREG_TYPE(irop_get_vreg(dest)) != TCCIR_VREG_TYPE_TEMP)
      continue;
    int pos = TCCIR_DECODE_VREG_POSITION(irop_get_vreg(dest));
    if (pos > max_tmp_pos || use_count[pos] != 0)
      continue;
    if (!DSE_IS_DEAD_ELIGIBLE(q))
      continue;

    /* Decrement use_count for this instruction's sources before NOP'ing it */
    if (irop_config[q->op].has_src1)
    {
      const IROperand s = tcc_ir_op_get_src1(ir, q);
      if (TCCIR_DECODE_VREG_TYPE(irop_get_vreg(s)) == TCCIR_VREG_TYPE_TEMP)
      {
        int p = TCCIR_DECODE_VREG_POSITION(irop_get_vreg(s));
        if (p >= 0 && p <= max_tmp_pos && use_count[p] > 0)
        {
          if (--use_count[p] == 0)
            worklist[wl_top++] = p;
        }
      }
    }
    if (irop_config[q->op].has_src2)
    {
      const IROperand s = tcc_ir_op_get_src2(ir, q);
      if (TCCIR_DECODE_VREG_TYPE(irop_get_vreg(s)) == TCCIR_VREG_TYPE_TEMP)
      {
        int p = TCCIR_DECODE_VREG_POSITION(irop_get_vreg(s));
        if (p >= 0 && p <= max_tmp_pos && use_count[p] > 0)
        {
          if (--use_count[p] == 0)
            worklist[wl_top++] = p;
        }
      }
    }
    if (q->op == TCCIR_OP_MLA)
    {
      const IROperand acc = tcc_ir_op_get_accum(ir, q);
      if (TCCIR_DECODE_VREG_TYPE(irop_get_vreg(acc)) == TCCIR_VREG_TYPE_TEMP)
      {
        int p = TCCIR_DECODE_VREG_POSITION(irop_get_vreg(acc));
        if (p >= 0 && p <= max_tmp_pos && use_count[p] > 0)
        {
          if (--use_count[p] == 0)
            worklist[wl_top++] = p;
        }
      }
    }
    if (q->op == TCCIR_OP_FUNCCALLVAL)
    {
      LOG_IR_GEN("DCE PURE-CALL: nop CALL at i=%d (result tmp dead)", i);
      DSE_CASCADE_PURE_CALL_PARAMS(i);
    }
    q->op = TCCIR_OP_NOP;
    def_idx[pos] = -1;
    changes++;
  }

  /* Drain the worklist: cascade eliminations */
  while (wl_top > 0)
  {
    int pos = worklist[--wl_top];
    int di = def_idx[pos];
    if (di < 0 || di >= n)
      continue;
    IRQuadCompact *q = &ir->compact_instructions[di];
    if (q->op == TCCIR_OP_NOP)
      continue;
    if (use_count[pos] != 0)
      continue; /* someone used it again via a different def */
    if (!DSE_IS_DEAD_ELIGIBLE(q))
      continue;
    const IROperand dest = tcc_ir_op_get_dest(ir, q);
    if (!irop_config[q->op].has_dest || TCCIR_DECODE_VREG_TYPE(irop_get_vreg(dest)) != TCCIR_VREG_TYPE_TEMP)
      continue;

    if (irop_config[q->op].has_src1)
    {
      const IROperand s = tcc_ir_op_get_src1(ir, q);
      if (TCCIR_DECODE_VREG_TYPE(irop_get_vreg(s)) == TCCIR_VREG_TYPE_TEMP)
      {
        int p = TCCIR_DECODE_VREG_POSITION(irop_get_vreg(s));
        if (p >= 0 && p <= max_tmp_pos && use_count[p] > 0)
        {
          if (--use_count[p] == 0)
            worklist[wl_top++] = p;
        }
      }
    }
    if (irop_config[q->op].has_src2)
    {
      const IROperand s = tcc_ir_op_get_src2(ir, q);
      if (TCCIR_DECODE_VREG_TYPE(irop_get_vreg(s)) == TCCIR_VREG_TYPE_TEMP)
      {
        int p = TCCIR_DECODE_VREG_POSITION(irop_get_vreg(s));
        if (p >= 0 && p <= max_tmp_pos && use_count[p] > 0)
        {
          if (--use_count[p] == 0)
            worklist[wl_top++] = p;
        }
      }
    }
    if (q->op == TCCIR_OP_MLA)
    {
      const IROperand acc = tcc_ir_op_get_accum(ir, q);
      if (TCCIR_DECODE_VREG_TYPE(irop_get_vreg(acc)) == TCCIR_VREG_TYPE_TEMP)
      {
        int p = TCCIR_DECODE_VREG_POSITION(irop_get_vreg(acc));
        if (p >= 0 && p <= max_tmp_pos && use_count[p] > 0)
        {
          if (--use_count[p] == 0)
            worklist[wl_top++] = p;
        }
      }
    }
    if (q->op == TCCIR_OP_FUNCCALLVAL)
    {
      LOG_IR_GEN("DCE PURE-CALL: nop CALL at i=%d (cascade)", di);
      DSE_CASCADE_PURE_CALL_PARAMS(di);
    }
    q->op = TCCIR_OP_NOP;
    def_idx[pos] = -1;
    changes++;
  }

#undef DSE_INC_USE
#undef DSE_ENSURE_CAP
#undef DSE_IS_DEAD_ELIGIBLE
#undef DSE_IS_PURE_CALL
#undef DSE_CASCADE_PURE_CALL_PARAMS

  LOG_IR_GEN("=== DEAD STORE ELIMINATION END (marked %d as NOP) ===", changes);
  tcc_free(worklist);
  tcc_free(def_idx);
  tcc_free(use_count);

  /* Also eliminate dead VAR vreg definitions.
   * A VAR that is defined (ASSIGN) but never used as a source operand
   * anywhere in the function is dead — provided it's not address-taken. */
  {
    int max_var_pos = 0;
    for (int i = 0; i < n; i++)
    {
      IRQuadCompact *q = &ir->compact_instructions[i];
      if (q->op == TCCIR_OP_NOP)
        continue;
      const IROperand dest = tcc_ir_op_get_dest(ir, q);
      int32_t vr = irop_get_vreg(dest);
      if (irop_config[q->op].has_dest && vr >= 0 && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_VAR)
      {
        int pos = TCCIR_DECODE_VREG_POSITION(vr);
        if (pos > max_var_pos)
          max_var_pos = pos;
      }
    }

    if (max_var_pos > 0)
    {
      uint8_t *var_used = tcc_mallocz((max_var_pos + 8) / 8);

      for (int i = 0; i < n; i++)
      {
        IRQuadCompact *q = &ir->compact_instructions[i];
        if (q->op == TCCIR_OP_NOP)
          continue;

        /* Check src1 */
        if (irop_config[q->op].has_src1)
        {
          const IROperand s = tcc_ir_op_get_src1(ir, q);
          int32_t vr = irop_get_vreg(s);
          if (vr >= 0 && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_VAR)
          {
            int pos = TCCIR_DECODE_VREG_POSITION(vr);
            if (pos <= max_var_pos)
              var_used[pos / 8] |= (1 << (pos % 8));
          }
        }

        /* Check src2 */
        if (irop_config[q->op].has_src2)
        {
          const IROperand s = tcc_ir_op_get_src2(ir, q);
          int32_t vr = irop_get_vreg(s);
          if (vr >= 0 && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_VAR)
          {
            int pos = TCCIR_DECODE_VREG_POSITION(vr);
            if (pos <= max_var_pos)
              var_used[pos / 8] |= (1 << (pos % 8));
          }
        }

        /* STORE/STORE_INDEXED dest: only a use when it's a pointer dereference
         * (non-local), not when it's a direct local store (which is a define). */
        if (q->op == TCCIR_OP_STORE || q->op == TCCIR_OP_STORE_INDEXED)
        {
          const IROperand d = tcc_ir_op_get_dest(ir, q);
          if (!d.is_local)
          {
            int32_t vr = irop_get_vreg(d);
            if (vr >= 0 && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_VAR)
            {
              int pos = TCCIR_DECODE_VREG_POSITION(vr);
              if (pos <= max_var_pos)
                var_used[pos / 8] |= (1 << (pos % 8));
            }
          }
        }

        /* FUNCPARAMVAL dest carries the parameter value — it's a use, not a def */
        if (q->op == TCCIR_OP_FUNCPARAMVAL)
        {
          const IROperand d = tcc_ir_op_get_dest(ir, q);
          int32_t vr = irop_get_vreg(d);
          if (vr >= 0 && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_VAR)
          {
            int pos = TCCIR_DECODE_VREG_POSITION(vr);
            if (pos <= max_var_pos)
              var_used[pos / 8] |= (1 << (pos % 8));
          }
        }
      }

      /* NOP ASSIGN/STORE to unused VARs (skip address-taken) */
      for (int i = 0; i < n; i++)
      {
        IRQuadCompact *q = &ir->compact_instructions[i];
        if (q->op != TCCIR_OP_ASSIGN && q->op != TCCIR_OP_STORE)
          continue;
        const IROperand dest = tcc_ir_op_get_dest(ir, q);
        /* For STORE, only eliminate local stores (not pointer dereferences) */
        if (q->op == TCCIR_OP_STORE && !dest.is_local)
          continue;
        /* Skip stores to parent frame via static chain — externally visible */
        if (dest.is_llocal)
          continue;
        int32_t vr = irop_get_vreg(dest);
        if (vr < 0 || TCCIR_DECODE_VREG_TYPE(vr) != TCCIR_VREG_TYPE_VAR)
          continue;
        int pos = TCCIR_DECODE_VREG_POSITION(vr);
        if (pos > max_var_pos)
          continue;
        if (var_used[pos / 8] & (1 << (pos % 8)))
          continue; /* VAR is used */
        /* Skip address-taken variables */
        IRLiveInterval *interval = tcc_ir_get_live_interval(ir, vr);
        if (interval && interval->addrtaken)
          continue;
        q->op = TCCIR_OP_NOP;
        changes++;
      }

      tcc_free(var_used);
    }
  }

  /* Dead StackLoc store elimination.
   * STORE to anonymous StackLoc offsets (not VAR vregs) that are never read
   * and whose addresses are never taken can be safely eliminated.
   * Uses a hash set of (sym, offset) pairs to track read/address-taken locations.
   *
   * Skip if function uses static chain — two cases:
   * 1. Function is a nested function (has_static_chain=1): it may write to
   *    the parent's frame via the chain pointer, and those writes are
   *    externally visible even though they look dead within this function.
   * 2. Function contains SET_CHAIN instructions: it is a parent function
   *    that sets up the chain for nested function calls, meaning its own
   *    stack variables may be read by the nested functions. */
  {
    if (ir->has_static_chain)
      goto skip_dead_stackloc;

    for (int i = 0; i < n; i++)
    {
      if (ir->compact_instructions[i].op == TCCIR_OP_SET_CHAIN)
        goto skip_dead_stackloc;
    }
#define STACKLOC_HASH_SIZE 256
    uint8_t stackloc_read[STACKLOC_HASH_SIZE];
    memset(stackloc_read, 0, sizeof(stackloc_read));

    /* Pre-scan: find the maximum StackLoc offset used in any STORE.
     * This determines how far an address-of range needs to extend. */
    int64_t max_stackloc_off = 0;
    for (int i = 0; i < n; i++)
    {
      IRQuadCompact *q = &ir->compact_instructions[i];
      if (q->op != TCCIR_OP_STORE)
        continue;
      IROperand dest = tcc_ir_op_get_dest(ir, q);
      if (!dest.is_local || irop_get_vreg(dest) >= 0)
        continue;
      int64_t off;
      if (irop_get_tag(dest) == IROP_TAG_SYMREF)
      {
        IRPoolSymref *sr = irop_get_symref_ex(ir, dest);
        off = sr ? sr->addend : 0;
      }
      else
      {
        off = irop_get_stack_offset(dest);
      }
      if (off > max_stackloc_off)
        max_stackloc_off = off;
    }

    /* Helper: hash a (sym, offset) pair to a bit index */
#define STACKLOC_HASH(sym, off) (((uintptr_t)(sym) * 31 + (uint32_t)(off) * 17) % (STACKLOC_HASH_SIZE * 8))
#define STACKLOC_SET(sym, off)                                                                                         \
  do                                                                                                                   \
  {                                                                                                                    \
    uint32_t _h = STACKLOC_HASH(sym, off);                                                                             \
    stackloc_read[_h / 8] |= (1 << (_h % 8));                                                                          \
  } while (0)
#define STACKLOC_TEST(sym, off) (stackloc_read[STACKLOC_HASH(sym, off) / 8] & (1 << (STACKLOC_HASH(sym, off) % 8)))

    /* Pre-scan: identify write-only address-of TEMPs.
     * An addr-TMP is "write-only" if the entire chain from the Addr[StackLoc]
     * through VAR intermediaries down to final uses consists only of:
     *   - STORE addr-prop-TMP → VAR  (address pipeline flow)
     *   - ASSIGN addr-prop-VAR → TMP (address pipeline flow)
     *   - ADD addr-prop-TMP, offset → TMP (pointer arithmetic)
     *   - STORE value → *addr-prop-TMP  (deref write — safe)
     * Any other use (LOAD, FUNCPARAM, TEST_ZERO, CMP, etc.) means the address
     * or pointed-to data is observable, so the addr-TMP is marked "read". */
    int max_tmp_stackloc = 0;
    int max_var_stackloc = -1;
    for (int i = 0; i < n; i++)
    {
      IRQuadCompact *q = &ir->compact_instructions[i];
      if (q->op == TCCIR_OP_NOP)
        continue;
      IROperand ops[3];
      int nops = 0;
      if (irop_config[q->op].has_dest)
        ops[nops++] = tcc_ir_op_get_dest(ir, q);
      if (irop_config[q->op].has_src1)
        ops[nops++] = tcc_ir_op_get_src1(ir, q);
      if (irop_config[q->op].has_src2)
        ops[nops++] = tcc_ir_op_get_src2(ir, q);
      for (int k = 0; k < nops; k++)
      {
        int32_t vr = irop_get_vreg(ops[k]);
        if (vr >= 0)
        {
          int pos = TCCIR_DECODE_VREG_POSITION(vr);
          if (TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_TEMP && pos > max_tmp_stackloc)
            max_tmp_stackloc = pos;
          else if (TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_VAR && pos > max_var_stackloc)
            max_var_stackloc = pos;
        }
      }
    }

    /* addr_tmp[pos] = 1 if TMP pos was defined from an Addr[StackLoc] */
    /* addr_tmp_read[pos] = 1 if the addr or pointed-to data is observable */
    uint8_t *addr_tmp = NULL;
    uint8_t *addr_tmp_read = NULL;

    if (max_tmp_stackloc > 0)
    {
      addr_tmp = tcc_mallocz((max_tmp_stackloc + 8) / 8);
      addr_tmp_read = tcc_mallocz((max_tmp_stackloc + 8) / 8);

      /* Phase 1: Find TEMPs defined from Addr[StackLoc] (is_local=1, is_lval=0) */
      for (int i = 0; i < n; i++)
      {
        IRQuadCompact *q = &ir->compact_instructions[i];
        if (q->op == TCCIR_OP_NOP)
          continue;
        if (irop_config[q->op].has_src1)
        {
          IROperand s = tcc_ir_op_get_src1(ir, q);
          if (s.is_local && !s.is_lval && irop_get_vreg(s) < 0)
          {
            IROperand d = tcc_ir_op_get_dest(ir, q);
            int32_t dvr = irop_get_vreg(d);
            if (dvr >= 0 && TCCIR_DECODE_VREG_TYPE(dvr) == TCCIR_VREG_TYPE_TEMP)
            {
              int dpos = TCCIR_DECODE_VREG_POSITION(dvr);
              if (dpos <= max_tmp_stackloc)
                addr_tmp[dpos / 8] |= (1 << (dpos % 8));
            }
          }
        }
      }

      /* Phase 2: Propagate addr-prop through STORE→VAR, ASSIGN→TMP, ADD→TMP.
       * prop_tmp[pos] and prop_var[pos] record the origin addr-TMP position,
       * or -1 (not addr-prop) or -2 (ambiguous / multiple origins). */
      int *prop_tmp = tcc_mallocz((max_tmp_stackloc + 1) * sizeof(int));
      int *prop_var = max_var_stackloc >= 0 ? tcc_mallocz((max_var_stackloc + 1) * sizeof(int)) : NULL;
      memset(prop_tmp, 0xFF, (max_tmp_stackloc + 1) * sizeof(int)); /* -1 */
      if (prop_var)
        memset(prop_var, 0xFF, (max_var_stackloc + 1) * sizeof(int));

      /* Seed from addr-TMPs */
      for (int pos = 0; pos <= max_tmp_stackloc; pos++)
        if (addr_tmp[pos / 8] & (1 << (pos % 8)))
          prop_tmp[pos] = pos;

      /* Propagate through the chain */
      int prop_changed = 1;
      while (prop_changed)
      {
        prop_changed = 0;
        for (int i = 0; i < n; i++)
        {
          IRQuadCompact *q = &ir->compact_instructions[i];
          if (q->op == TCCIR_OP_NOP)
            continue;

          if (!irop_config[q->op].has_src1 || !irop_config[q->op].has_dest)
            continue;
          IROperand src = tcc_ir_op_get_src1(ir, q);
          IROperand dest = tcc_ir_op_get_dest(ir, q);
          int32_t svr = irop_get_vreg(src);
          int32_t dvr = irop_get_vreg(dest);
          if (svr < 0 || dvr < 0)
            continue;

          int stype = TCCIR_DECODE_VREG_TYPE(svr);
          int dtype = TCCIR_DECODE_VREG_TYPE(dvr);
          int spos = TCCIR_DECODE_VREG_POSITION(svr);
          int dpos = TCCIR_DECODE_VREG_POSITION(dvr);
          int origin = -1;

          /* STORE/ASSIGN TMP→VAR: addr flows from TMP to VAR.
           * ASSIGN case arises after copy propagation rewrites STORE to ASSIGN
           * or propagates an addr-TMP through a TMP→VAR assignment chain. */
          if ((q->op == TCCIR_OP_STORE || q->op == TCCIR_OP_ASSIGN) && stype == TCCIR_VREG_TYPE_TEMP &&
              dtype == TCCIR_VREG_TYPE_VAR && spos <= max_tmp_stackloc && prop_var && dpos <= max_var_stackloc)
            origin = prop_tmp[spos];
          /* ASSIGN VAR→TMP: addr flows from VAR to TMP */
          else if ((q->op == TCCIR_OP_ASSIGN || q->op == TCCIR_OP_LOAD) && stype == TCCIR_VREG_TYPE_VAR &&
                   dtype == TCCIR_VREG_TYPE_TEMP && prop_var && spos <= max_var_stackloc && dpos <= max_tmp_stackloc)
            origin = prop_var[spos];
          /* ASSIGN/LOAD VAR→VAR: addr flows from one VAR to another */
          else if ((q->op == TCCIR_OP_ASSIGN || q->op == TCCIR_OP_LOAD) && stype == TCCIR_VREG_TYPE_VAR &&
                   dtype == TCCIR_VREG_TYPE_VAR && prop_var && spos <= max_var_stackloc && dpos <= max_var_stackloc)
            origin = prop_var[spos];
          /* ADD/SUB/ASSIGN TMP→TMP: pointer arithmetic and copies preserve addr-prop */
          else if ((q->op == TCCIR_OP_ADD || q->op == TCCIR_OP_SUB || q->op == TCCIR_OP_ASSIGN) &&
                   stype == TCCIR_VREG_TYPE_TEMP && dtype == TCCIR_VREG_TYPE_TEMP && spos <= max_tmp_stackloc &&
                   dpos <= max_tmp_stackloc)
            origin = prop_tmp[spos];

          if (origin < 0)
            continue;

          /* Propagate to dest */
          if (dtype == TCCIR_VREG_TYPE_TEMP && dpos <= max_tmp_stackloc)
          {
            if (prop_tmp[dpos] == -1)
            {
              prop_tmp[dpos] = origin;
              prop_changed = 1;
            }
            else if (prop_tmp[dpos] != origin && prop_tmp[dpos] != -2)
            {
              prop_tmp[dpos] = -2; /* ambiguous */
              prop_changed = 1;
            }
          }
          else if (dtype == TCCIR_VREG_TYPE_VAR && prop_var && dpos <= max_var_stackloc)
          {
            if (prop_var[dpos] == -1)
            {
              prop_var[dpos] = origin;
              prop_changed = 1;
            }
            else if (prop_var[dpos] != origin && prop_var[dpos] != -2)
            {
              prop_var[dpos] = -2;
              prop_changed = 1;
            }
          }
        }
      }

      /* Phase 3: Check uses of all addr-prop values. Mark origin addr-TMP
       * as "read" if any propagated value is used outside the write pipeline. */
      for (int i = 0; i < n; i++)
      {
        IRQuadCompact *q = &ir->compact_instructions[i];
        if (q->op == TCCIR_OP_NOP)
          continue;

        /* Helper: get origin of a vreg, or -1 if not addr-prop */
#define GET_ORIGIN(vr)                                                                                                 \
  ({                                                                                                                   \
    int _o = -1;                                                                                                       \
    int _t = TCCIR_DECODE_VREG_TYPE(vr);                                                                               \
    int _p = TCCIR_DECODE_VREG_POSITION(vr);                                                                           \
    if (_t == TCCIR_VREG_TYPE_TEMP && _p <= max_tmp_stackloc)                                                          \
      _o = prop_tmp[_p];                                                                                               \
    else if (_t == TCCIR_VREG_TYPE_VAR && prop_var && _p <= max_var_stackloc)                                          \
      _o = prop_var[_p];                                                                                               \
    _o;                                                                                                                \
  })

#define MARK_ORIGIN_READ(origin)                                                                                       \
  do                                                                                                                   \
  {                                                                                                                    \
    if ((origin) == -2)                                                                                                \
      memset(addr_tmp_read, 0xFF, (max_tmp_stackloc + 8) / 8);                                                         \
    else if ((origin) >= 0 && (origin) <= max_tmp_stackloc)                                                            \
      addr_tmp_read[(origin) / 8] |= (1 << ((origin) % 8));                                                            \
  } while (0)

        /* Check src1 */
        if (irop_config[q->op].has_src1)
        {
          IROperand s = tcc_ir_op_get_src1(ir, q);
          int32_t vr = irop_get_vreg(s);
          if (vr >= 0)
          {
            int origin = GET_ORIGIN(vr);
            if (origin != -1)
            {
              /* Is this use safe (within the write pipeline)? */
              int safe = 0;
              if (q->op == TCCIR_OP_STORE && (!s.is_lval || TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_VAR))
              {
                /* Storing addr value into a VAR = pipeline flow.
                 * For TMP src, is_lval=1 means deref read (*ptr) — NOT safe.
                 * For VAR src, is_lval=1 just means "load variable" — safe. */
                IROperand d = tcc_ir_op_get_dest(ir, q);
                int32_t dvr = irop_get_vreg(d);
                if (dvr >= 0 && TCCIR_DECODE_VREG_TYPE(dvr) == TCCIR_VREG_TYPE_VAR)
                  safe = 1;
              }
              else if (q->op == TCCIR_OP_ASSIGN || q->op == TCCIR_OP_ADD || q->op == TCCIR_OP_SUB)
              {
                /* Pipeline flow or pointer arithmetic — but a deref read
                 * (is_lval=1 on a TMP) means memory at the address is being
                 * read, making the pointed-to data observable.
                 * For VAR sources, is_lval=1 just means "load the variable's
                 * value" (always set for VAR reads) — this is a pointer copy,
                 * NOT a dereference through the pointed-to data. */
                int stype = TCCIR_DECODE_VREG_TYPE(vr);
                if (!s.is_lval || stype == TCCIR_VREG_TYPE_VAR)
                  safe = 1;
              }
              if (!safe)
              {
                LOG_IR_GEN("DSE-SL: Phase3 MARK READ origin=%d at i=%d op=%d src1 is_lval=%d", origin, i, q->op,
                           s.is_lval);
                MARK_ORIGIN_READ(origin);
              }
            }
          }
        }

        /* Check src2: addr-prop as src2 is unusual; conservatively mark read */
        if (irop_config[q->op].has_src2)
        {
          IROperand s = tcc_ir_op_get_src2(ir, q);
          int32_t vr = irop_get_vreg(s);
          if (vr >= 0)
          {
            int origin = GET_ORIGIN(vr);
            if (origin != -1)
            {
              LOG_IR_GEN("DSE-SL: Phase3 MARK READ origin=%d at i=%d op=%d src2", origin, i, q->op);
              MARK_ORIGIN_READ(origin);
            }
          }
        }

        /* STORE dest: if dest is an addr-prop TMP (deref write), that's safe.
         * No marking needed — this is a write through the pointer. */

#undef GET_ORIGIN
#undef MARK_ORIGIN_READ
      }

      tcc_free(prop_tmp);
      if (prop_var)
        tcc_free(prop_var);
    }

    /* Pass 1: Mark StackLoc offsets that are read or address-taken */
    for (int i = 0; i < n; i++)
    {
      IRQuadCompact *q = &ir->compact_instructions[i];
      if (q->op == TCCIR_OP_NOP)
        continue;

      /* Helper: mark a StackLoc operand as read or address-taken.
       * is_lval=true means direct memory read → mark exact offset.
       * is_lval=false means address-of → mark range up to max store offset.
       * Write-only address-of (pointer only used for writes) is skipped. */
#define MARK_STACKLOC_OP(op)                                                                                           \
  do                                                                                                                   \
  {                                                                                                                    \
    if ((op).is_local && irop_get_vreg(op) < 0)                                                                        \
    {                                                                                                                  \
      const Sym *_sym = NULL;                                                                                          \
      int64_t _off;                                                                                                    \
      if (irop_get_tag(op) == IROP_TAG_SYMREF)                                                                         \
      {                                                                                                                \
        IRPoolSymref *_sr = irop_get_symref_ex(ir, op);                                                                \
        _sym = _sr ? _sr->sym : NULL;                                                                                  \
        _off = _sr ? _sr->addend : 0;                                                                                  \
      }                                                                                                                \
      else                                                                                                             \
      {                                                                                                                \
        _off = irop_get_stack_offset(op);                                                                              \
      }                                                                                                                \
      if ((op).is_lval)                                                                                                \
      {                                                                                                                \
        /* Mark all byte offsets within the access width so that stores                                                \
         * at sub-offsets (e.g. _Complex short imag part at _off+2)                                                    \
         * are not incorrectly eliminated as dead. */                                                                  \
        int _width;                                                                                                    \
        switch ((op).btype)                                                                                            \
        {                                                                                                              \
        case IROP_BTYPE_INT8:                                                                                          \
          _width = 1;                                                                                                  \
          break;                                                                                                       \
        case IROP_BTYPE_INT16:                                                                                         \
          _width = 2;                                                                                                  \
          break;                                                                                                       \
        case IROP_BTYPE_FLOAT32:                                                                                       \
          _width = 4;                                                                                                  \
          break;                                                                                                       \
        case IROP_BTYPE_INT64:                                                                                         \
        case IROP_BTYPE_FLOAT64:                                                                                       \
          _width = 8;                                                                                                  \
          break;                                                                                                       \
        case IROP_BTYPE_STRUCT:                                                                                        \
        {                                                                                                              \
          /* Struct access — conservatively mark range up to max store offset */                                       \
          int64_t _send = max_stackloc_off + 4;                                                                        \
          for (int64_t _s = _off; _s <= _send; _s++)                                                                   \
            STACKLOC_SET(_sym, _s);                                                                                    \
          _width = 0; /* already handled */                                                                            \
          break;                                                                                                       \
        }                                                                                                              \
        default:                                                                                                       \
          _width = 4;                                                                                                  \
          break;                                                                                                       \
        }                                                                                                              \
        /* Complex types implicitly read both real and imag halves —                                                   \
         * double the width so the imag store at +elem_size isn't DSE'd. */                                            \
        if ((op).is_complex)                                                                                           \
          _width *= 2;                                                                                                 \
        for (int _b = 0; _b < _width; _b++)                                                                            \
          STACKLOC_SET(_sym, _off + _b);                                                                               \
      }                                                                                                                \
      else                                                                                                             \
      {                                                                                                                \
        /* Address-of: mark from base offset to max store offset (+ margin for field access).                          \
         * Cap range to avoid excessive iteration; if too large, mark all bits. */                                     \
        int64_t _range_end = max_stackloc_off + 4;                                                                     \
        int64_t _range_len = _range_end - _off + 1;                                                                    \
        if (_range_len > STACKLOC_HASH_SIZE * 8)                                                                       \
        {                                                                                                              \
          memset(stackloc_read, 0xFF, sizeof(stackloc_read));                                                          \
        }                                                                                                              \
        else if (_range_len > 0)                                                                                       \
        {                                                                                                              \
          for (int64_t _k = _off; _k <= _range_end; _k++)                                                              \
            STACKLOC_SET(_sym, _k);                                                                                    \
        }                                                                                                              \
      }                                                                                                                \
    }                                                                                                                  \
  } while (0)

      /* Check all operands for StackLoc reads / address-taken */
      if (irop_config[q->op].has_src1)
      {
        IROperand s = tcc_ir_op_get_src1(ir, q);
        /* Skip range marking for address-of StackLoc that feeds a write-only TMP.
         * If the TMP is only used for STORE destinations (pointer writes), the
         * address-of doesn't constitute a "read" of the StackLoc range. */
        if (s.is_local && !s.is_lval && irop_get_vreg(s) < 0 && addr_tmp != NULL)
        {
          IROperand d = tcc_ir_op_get_dest(ir, q);
          int32_t dvr = irop_get_vreg(d);
          if (TCC_LOG_IR_GEN)
          {
            fprintf(stderr, "[IR_GEN] DSE-SL: addr-of check i=%d dvr=0x%x type=%d pos=%d max=%d", i, (unsigned)dvr,
                    dvr >= 0 ? TCCIR_DECODE_VREG_TYPE(dvr) : -1, dvr >= 0 ? TCCIR_DECODE_VREG_POSITION(dvr) : -1,
                    max_tmp_stackloc);
            if (dvr >= 0 && TCCIR_DECODE_VREG_TYPE(dvr) == TCCIR_VREG_TYPE_TEMP)
            {
              int _dp = TCCIR_DECODE_VREG_POSITION(dvr);
              fprintf(stderr, " addr_tmp=%d addr_tmp_read=%d",
                      !!(_dp <= max_tmp_stackloc && (addr_tmp[_dp / 8] & (1 << (_dp % 8)))),
                      !!(_dp <= max_tmp_stackloc && (addr_tmp_read[_dp / 8] & (1 << (_dp % 8)))));
            }
            fprintf(stderr, "\n");
          }
          if (dvr >= 0 && TCCIR_DECODE_VREG_TYPE(dvr) == TCCIR_VREG_TYPE_TEMP)
          {
            int dpos = TCCIR_DECODE_VREG_POSITION(dvr);
            if (dpos <= max_tmp_stackloc && (addr_tmp[dpos / 8] & (1 << (dpos % 8))) &&
                !(addr_tmp_read[dpos / 8] & (1 << (dpos % 8))))
            {
              LOG_IR_GEN("DSE-SL: SKIP addr-of at i=%d (write-only T%d)", i, dpos);
              goto after_src1_mark;
            }
          }
        }
        LOG_IR_GEN("DSE-SL: MARK src1 at i=%d op=%d is_lval=%d is_local=%d", i, q->op, s.is_lval, s.is_local);
        /* FUNCPARAMVAL/FUNCPARAMVOID src1 passing a STRUCT from a StackLoc
         * base may span multiple consecutive words (size not encoded in
         * btype) — force range marking for that case only. Scalar value
         * params (INT/FLOAT) have a well-defined width and use the normal
         * is_lval=1 width-based marking; over-marking them as full-range
         * masks legitimately dead stores at higher offsets. */
        if ((q->op == TCCIR_OP_FUNCPARAMVAL || q->op == TCCIR_OP_FUNCPARAMVOID) && s.is_local &&
            irop_get_vreg(s) < 0 && s.btype == IROP_BTYPE_STRUCT)
        {
          IROperand s_range = s;
          s_range.is_lval = 0;
          MARK_STACKLOC_OP(s_range);
        }
        else
        {
          MARK_STACKLOC_OP(s);
        }
      after_src1_mark:;
      }
      if (irop_config[q->op].has_src2)
      {
        IROperand s = tcc_ir_op_get_src2(ir, q);
        LOG_IR_GEN("DSE-SL: MARK src2 at i=%d op=%d is_lval=%d is_local=%d", i, q->op, s.is_lval, s.is_local);
        MARK_STACKLOC_OP(s);
      }
      /* dest operands are not uses for most instructions (they are defines).
       * Special cases (FUNCPARAMVAL src1) are handled above. */
      /* MLA accumulator (4th operand) may reference a StackLoc */
      if (q->op == TCCIR_OP_MLA)
      {
        IROperand acc = tcc_ir_op_get_accum(ir, q);
        MARK_STACKLOC_OP(acc);
      }
#undef MARK_STACKLOC_OP
    }

    /* Pass 2: Eliminate STORE to unread StackLoc offsets */
#if TCC_LOG_IR_GEN
    {
      int any_set = 0;
      for (int bi = 0; bi <= max_stackloc_off / 8; bi++)
        if (stackloc_read[bi])
        {
          any_set = 1;
          break;
        }
      LOG_IR_GEN("DSE-SL: stackloc_read has %s bits set, max_stackloc_off=%lld", any_set ? "some" : "NO",
                 (long long)max_stackloc_off);
    }
#endif
    for (int i = 0; i < n; i++)
    {
      IRQuadCompact *q = &ir->compact_instructions[i];
      if (q->op != TCCIR_OP_STORE)
        continue;
      IROperand dest = tcc_ir_op_get_dest(ir, q);
      if (!dest.is_local || irop_get_vreg(dest) >= 0)
        continue; /* Not an anonymous StackLoc */
      if (dest.is_llocal)
        continue; /* Store to parent frame via static chain — externally visible */

      const Sym *sym = NULL;
      int64_t off;
      if (irop_get_tag(dest) == IROP_TAG_SYMREF)
      {
        IRPoolSymref *sr = irop_get_symref_ex(ir, dest);
        sym = sr ? sr->sym : NULL;
        off = sr ? sr->addend : 0;
      }
      else
      {
        off = irop_get_stack_offset(dest);
      }

      LOG_IR_GEN("DSE-SL: i=%d off=%lld sym=%p test=%d", i, (long long)off, (void *)sym, !!STACKLOC_TEST(sym, off));
      if (!STACKLOC_TEST(sym, off))
      {
        q->op = TCCIR_OP_NOP;
        changes++;
      }
    }

    /* Pass 3: Eliminate dead pointer chains from write-only addr-of.
     * If a write-only addr-TMP produced by Addr[StackLoc] feeds only
     * pointer writes, and the StackLoc range has no surviving reads,
     * NOP the LEA and forward-propagate: NOP instructions that use
     * only dead TMPs/VARs as sources or pointer bases (deref stores). */
    if (addr_tmp != NULL)
    {
      /* Find max VAR position for dead_var tracking */
      int max_var_pos = -1;
      for (int i = 0; i < n; i++)
      {
        IRQuadCompact *q = &ir->compact_instructions[i];
        if (q->op == TCCIR_OP_NOP)
          continue;
        if (irop_config[q->op].has_dest)
        {
          IROperand d = tcc_ir_op_get_dest(ir, q);
          int32_t vr = irop_get_vreg(d);
          if (vr >= 0 && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_VAR)
          {
            int pos = TCCIR_DECODE_VREG_POSITION(vr);
            if (pos > max_var_pos)
              max_var_pos = pos;
          }
        }
      }

      uint8_t *dead_tmp = tcc_mallocz((max_tmp_stackloc + 8) / 8);
      uint8_t *dead_var = max_var_pos >= 0 ? tcc_mallocz((max_var_pos + 8) / 8) : NULL;

      /* Step 1: NOP LEA/Addr instructions for write-only addr-TMPs whose
       * StackLoc range is actually dead (no surviving reads). */
      for (int i = 0; i < n; i++)
      {
        IRQuadCompact *q = &ir->compact_instructions[i];
        if (q->op == TCCIR_OP_NOP || !irop_config[q->op].has_src1)
          continue;
        IROperand s = tcc_ir_op_get_src1(ir, q);
        if (!s.is_local || s.is_lval || irop_get_vreg(s) >= 0)
          continue;
        IROperand d = tcc_ir_op_get_dest(ir, q);
        int32_t dvr = irop_get_vreg(d);
        if (dvr < 0 || TCCIR_DECODE_VREG_TYPE(dvr) != TCCIR_VREG_TYPE_TEMP)
          continue;
        int dpos = TCCIR_DECODE_VREG_POSITION(dvr);
        if (dpos > max_tmp_stackloc)
          continue;
        if (!(addr_tmp[dpos / 8] & (1 << (dpos % 8))))
          continue;
        if (addr_tmp_read[dpos / 8] & (1 << (dpos % 8)))
          continue;

        /* Verify the pointed-to StackLoc range is dead: check if any offset
         * in [base_off, max_stackloc_off+4] is still marked as read.
         * This catches cases where different Addr instructions to the same
         * StackLoc produce both read and write-only TMPs. */
        const Sym *addr_sym = NULL;
        int64_t addr_off;
        if (irop_get_tag(s) == IROP_TAG_SYMREF)
        {
          IRPoolSymref *sr = irop_get_symref_ex(ir, s);
          addr_sym = sr ? sr->sym : NULL;
          addr_off = sr ? sr->addend : 0;
        }
        else
        {
          addr_off = irop_get_stack_offset(s);
        }
        int range_is_read = 0;
        int64_t range_end = max_stackloc_off + 4;
        int64_t range_len = range_end - addr_off + 1;
        if (range_len > STACKLOC_HASH_SIZE * 8)
        {
          range_is_read = 1; /* Too large — conservatively assume read */
        }
        else
        {
          for (int64_t k = addr_off; k <= range_end; k++)
          {
            if (STACKLOC_TEST(addr_sym, k))
            {
              range_is_read = 1;
              break;
            }
          }
        }
        if (range_is_read)
          continue;

        q->op = TCCIR_OP_NOP;
        changes++;
        dead_tmp[dpos / 8] |= (1 << (dpos % 8));
      }

      /* Step 2: Forward-propagate dead values through the pointer chain.
       * NOP instructions whose source values are all dead (defined only
       * by NOP'd instructions), then mark their dests as dead too. */
      int prop_changed = 1;
      while (prop_changed)
      {
        prop_changed = 0;
        for (int i = 0; i < n; i++)
        {
          IRQuadCompact *q = &ir->compact_instructions[i];
          if (q->op == TCCIR_OP_NOP)
            continue;
          /* Only propagate through data-flow instructions */
          if (q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_JUMPIF || q->op == TCCIR_OP_IJUMP ||
              q->op == TCCIR_OP_RETURNVALUE || q->op == TCCIR_OP_FUNCCALLVAL || q->op == TCCIR_OP_FUNCCALLVOID ||
              q->op == TCCIR_OP_FUNCPARAMVAL || q->op == TCCIR_OP_FUNCPARAMVOID || q->op == TCCIR_OP_SWITCH_TABLE)
            continue;

          int has_dead_src = 0;

          if (irop_config[q->op].has_src1)
          {
            IROperand s = tcc_ir_op_get_src1(ir, q);
            int32_t vr = irop_get_vreg(s);
            if (vr >= 0)
            {
              int pos = TCCIR_DECODE_VREG_POSITION(vr);
              if (TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_TEMP && pos <= max_tmp_stackloc &&
                  (dead_tmp[pos / 8] & (1 << (pos % 8))))
                has_dead_src = 1;
              else if (dead_var && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_VAR && pos <= max_var_pos &&
                       (dead_var[pos / 8] & (1 << (pos % 8))))
                has_dead_src = 1;
            }
          }

          /* For STORE / STORE_INDEXED: check if dest is a dead TMP/VAR
           * used as pointer base.  STORE_INDEXED has the same dest-as-base
           * semantics as STORE — the dest carries the address, src1 the
           * value — so the same dead-base check kills the store. */
          if (!has_dead_src && (q->op == TCCIR_OP_STORE || q->op == TCCIR_OP_STORE_INDEXED))
          {
            IROperand d = tcc_ir_op_get_dest(ir, q);
            int32_t vr = irop_get_vreg(d);
            if (vr >= 0)
            {
              int pos = TCCIR_DECODE_VREG_POSITION(vr);
              if (TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_TEMP && pos <= max_tmp_stackloc &&
                  (dead_tmp[pos / 8] & (1 << (pos % 8))))
                has_dead_src = 1;
            }
          }

          if (has_dead_src)
          {
            /* Mark dest as dead */
            if (irop_config[q->op].has_dest)
            {
              IROperand d = tcc_ir_op_get_dest(ir, q);
              int32_t dvr = irop_get_vreg(d);
              if (dvr >= 0)
              {
                int dpos = TCCIR_DECODE_VREG_POSITION(dvr);
                if (TCCIR_DECODE_VREG_TYPE(dvr) == TCCIR_VREG_TYPE_TEMP && dpos <= max_tmp_stackloc)
                  dead_tmp[dpos / 8] |= (1 << (dpos % 8));
                else if (dead_var && TCCIR_DECODE_VREG_TYPE(dvr) == TCCIR_VREG_TYPE_VAR && dpos <= max_var_pos)
                  dead_var[dpos / 8] |= (1 << (dpos % 8));
              }
            }
            q->op = TCCIR_OP_NOP;
            changes++;
            prop_changed = 1;
          }
        }
      }

      tcc_free(dead_tmp);
      if (dead_var)
        tcc_free(dead_var);
    }

    if (addr_tmp)
      tcc_free(addr_tmp);
    if (addr_tmp_read)
      tcc_free(addr_tmp_read);

#undef STACKLOC_HASH_SIZE
#undef STACKLOC_HASH
#undef STACKLOC_SET
#undef STACKLOC_TEST
  skip_dead_stackloc:;
  }

  /* Second pass: dead TMP elimination after VAR + StackLoc store elimination.
   * The first TMP pass (above) cannot eliminate e.g. T0 in:
   *   T0 <-- #7 [LOAD]          (defines T0)
   *   StackLoc[-32] <-- T0      (only use of T0)
   * because the STORE still uses T0.  After StackLoc store elimination removes
   * the STORE, T0 is dead but the first TMP pass already finished.
   * Re-run the same iterative elimination to catch these cascading deaths. */
  {
    uint8_t *used2 = tcc_mallocz((max_tmp_pos + 8) / 8);
    int iter2_changes;
    do
    {
      iter2_changes = 0;
      memset(used2, 0, (max_tmp_pos + 8) / 8);
      for (int i = 0; i < n; i++)
      {
        IRQuadCompact *q = &ir->compact_instructions[i];
        if (q->op == TCCIR_OP_NOP)
          continue;
        if (irop_config[q->op].has_src1)
        {
          const IROperand s = tcc_ir_op_get_src1(ir, q);
          if (TCCIR_DECODE_VREG_TYPE(irop_get_vreg(s)) == TCCIR_VREG_TYPE_TEMP)
          {
            const int pos = TCCIR_DECODE_VREG_POSITION(irop_get_vreg(s));
            if (pos <= max_tmp_pos)
              used2[pos / 8] |= (1 << (pos % 8));
          }
        }
        if (irop_config[q->op].has_src2)
        {
          const IROperand s = tcc_ir_op_get_src2(ir, q);
          if (TCCIR_DECODE_VREG_TYPE(irop_get_vreg(s)) == TCCIR_VREG_TYPE_TEMP)
          {
            const int pos = TCCIR_DECODE_VREG_POSITION(irop_get_vreg(s));
            if (pos <= max_tmp_pos)
              used2[pos / 8] |= (1 << (pos % 8));
          }
        }
        /* STORE/STORE_INDEXED dest is a use (pointer deref) */
        {
          const IROperand d = tcc_ir_op_get_dest(ir, q);
          if ((q->op == TCCIR_OP_STORE || q->op == TCCIR_OP_STORE_INDEXED) &&
              TCCIR_DECODE_VREG_TYPE(irop_get_vreg(d)) == TCCIR_VREG_TYPE_TEMP)
          {
            const int pos = TCCIR_DECODE_VREG_POSITION(irop_get_vreg(d));
            if (pos <= max_tmp_pos)
              used2[pos / 8] |= (1 << (pos % 8));
          }
          if (q->op == TCCIR_OP_FUNCPARAMVAL && TCCIR_DECODE_VREG_TYPE(irop_get_vreg(d)) == TCCIR_VREG_TYPE_TEMP)
          {
            const int pos = TCCIR_DECODE_VREG_POSITION(irop_get_vreg(d));
            if (pos <= max_tmp_pos)
              used2[pos / 8] |= (1 << (pos % 8));
          }
        }
        if (q->op == TCCIR_OP_MLA)
        {
          const IROperand acc = tcc_ir_op_get_accum(ir, q);
          if (TCCIR_DECODE_VREG_TYPE(irop_get_vreg(acc)) == TCCIR_VREG_TYPE_TEMP)
          {
            const int pos = TCCIR_DECODE_VREG_POSITION(irop_get_vreg(acc));
            if (pos <= max_tmp_pos)
              used2[pos / 8] |= (1 << (pos % 8));
          }
        }
      }

      for (int i = 0; i < n; i++)
      {
        IRQuadCompact *q = &ir->compact_instructions[i];
        if (q->op == TCCIR_OP_NOP)
          continue;
        const IROperand dest = tcc_ir_op_get_dest(ir, q);
        if (irop_config[q->op].has_dest && TCCIR_DECODE_VREG_TYPE(irop_get_vreg(dest)) == TCCIR_VREG_TYPE_TEMP)
        {
          int is_dead_eligible =
              (q->op != TCCIR_OP_STORE && q->op != TCCIR_OP_STORE_INDEXED && q->op != TCCIR_OP_STORE_POSTINC &&
               q->op != TCCIR_OP_LOAD_POSTINC && q->op != TCCIR_OP_LOAD && q->op != TCCIR_OP_FUNCCALLVAL &&
               q->op != TCCIR_OP_FUNCCALLVOID && q->op != TCCIR_OP_FUNCPARAMVAL && q->op != TCCIR_OP_FUNCPARAMVOID);
          if (!is_dead_eligible && q->op == TCCIR_OP_LOAD)
          {
            const IROperand src1 = tcc_ir_op_get_src1(ir, q);
            if (irop_is_immediate(src1))
              is_dead_eligible = 1;
          }
          if (is_dead_eligible)
          {
            const int pos = TCCIR_DECODE_VREG_POSITION(irop_get_vreg(dest));
            if (pos <= max_tmp_pos && !(used2[pos / 8] & (1 << (pos % 8))))
            {
              q->op = TCCIR_OP_NOP;
              iter2_changes++;
            }
          }
        }
      }
      changes += iter2_changes;
    } while (iter2_changes > 0);
    tcc_free(used2);
  }

  return changes;
}

int tcc_ir_opt_dse_ex(IROptCtx *ctx)
{
  return tcc_ir_opt_dse(ctx->ir);
}

/* Returns 1 if the only effect of `q` is the def of its dest — safe to NOP
 * when the dest is unread.  Mirrors DSE_IS_DEAD_ELIGIBLE in tcc_ir_opt_dse,
 * but tuned for VAR-dest ops where the dead-var consumer of a CALL/LOAD
 * chain is often a narrowing arithmetic op (AND/SHL/SHR/etc.) rather than
 * a plain ASSIGN.  Without this, e.g. `V = T & 0xFF` keeps T alive even
 * when V is unread, blocking the cascading kill of the producer call. */
static int ir_op_pure_for_dead_var_dest(TCCIRState *ir, IRQuadCompact *q)
{
  switch (q->op) {
  /* Side-effecting / control-flow / call-related — never safe to drop here. */
  case TCCIR_OP_NOP:
  case TCCIR_OP_STORE_INDEXED:
  case TCCIR_OP_STORE_POSTINC:
  case TCCIR_OP_LOAD_POSTINC:
  case TCCIR_OP_FUNCPARAMVAL:
  case TCCIR_OP_FUNCPARAMVOID:
  case TCCIR_OP_JUMP:
  case TCCIR_OP_JUMPIF:
  case TCCIR_OP_IJUMP:
  case TCCIR_OP_SWITCH_TABLE:
  case TCCIR_OP_RETURNVOID:
  case TCCIR_OP_RETURNVALUE:
  case TCCIR_OP_SET_CHAIN:
  case TCCIR_OP_INIT_CHAIN_SLOT:
  case TCCIR_OP_INLINE_ASM:
  case TCCIR_OP_ASM_INPUT:
  case TCCIR_OP_ASM_OUTPUT:
  case TCCIR_OP_VLA_ALLOC:
  case TCCIR_OP_VLA_SP_SAVE:
  case TCCIR_OP_VLA_SP_RESTORE:
  case TCCIR_OP_PREFETCH:
  case TCCIR_OP_TRAP:
  case TCCIR_OP_SETJMP:
  case TCCIR_OP_LONGJMP:
  case TCCIR_OP_CALLSEQ_BEGIN:
  case TCCIR_OP_CALLARG_REG:
  case TCCIR_OP_CALLARG_STACK:
  case TCCIR_OP_CALLSEQ_END:
  case TCCIR_OP_CMP:
    return 0;
  case TCCIR_OP_LOAD: {
    /* Same safe-source set as DSE_IS_DEAD_ELIGIBLE: no MMIO / volatile risk. */
    IROperand s = tcc_ir_op_get_src1(ir, q);
    return irop_is_immediate(s) || s.is_sym ||
           (irop_get_vreg(s) <= -2 && irop_get_vreg(s) >= -9) ||
           (irop_get_tag(s) == IROP_TAG_STACKOFF && s.is_local);
  }
  case TCCIR_OP_STORE: {
    /* Only local stores (direct register writes) are safe — pointer stores
     * are observable. */
    IROperand d = tcc_ir_op_get_dest(ir, q);
    return d.is_local;
  }
  case TCCIR_OP_FUNCCALLVAL:
  case TCCIR_OP_FUNCCALLVOID: {
    Sym *callee = irop_get_sym_ex(ir, tcc_ir_op_get_src1(ir, q));
    if (!callee) return 0;
    const char *name = get_tok_str(callee->v, NULL);
    return name && tcc_ir_is_pure_aeabi(name);
  }
  default:
    /* Pure arithmetic/bitwise/conversion: ADD, SUB, MUL, AND, OR, XOR, SHL,
     * SHR, SAR, ZEXT, UBFX, FADD/FSUB/..., CVT_*, LEA, SETIF, etc. */
    return 1;
  }
}

/* Dead address-taken VAR elimination.
 * After value tracking folds branches that read address-taken VARs (e.g.,
 * overflow builtin results), the ASSIGN + LEA + STORE sequences writing to
 * those VARs become dead. The regular DSE skips address-taken VARs, but this
 * pass can safely eliminate them by proving no live reads remain.
 *
 * A VAR is "dead" if:
 * 1. It's never read directly (not src1/src2 of any non-NOP, non-LEA instruction)
 * 2. All LEA pointers to it are only used as STORE destinations (write-only)
 */
int tcc_ir_opt_dead_var_store_elim(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  if (n == 0)
    return 0;

  for (int i = 0; i < n; i++)
    if (ir->compact_instructions[i].op == TCCIR_OP_IJUMP)
      return 0;

  int max_var = 0;
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;
    for (int k = 0; k < 3; k++)
    {
      IROperand op = (k == 0)   ? tcc_ir_op_get_dest(ir, q)
                     : (k == 1) ? tcc_ir_op_get_src1(ir, q)
                                : tcc_ir_op_get_src2(ir, q);
      int32_t vr = irop_get_vreg(op);
      if (vr >= 0 && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_VAR)
      {
        int pos = TCCIR_DECODE_VREG_POSITION(vr);
        if (pos > max_var)
          max_var = pos;
      }
    }
  }
  if (max_var == 0)
    return 0;

  uint8_t *var_read = tcc_mallocz((max_var + 8) / 8);
  uint8_t *var_has_lea = tcc_mallocz((max_var + 8) / 8);
  int has_set_chain = 0;

  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;
    if (q->op == TCCIR_OP_SET_CHAIN || q->op == TCCIR_OP_INIT_CHAIN_SLOT)
      has_set_chain = 1;
    /* Track LEA instructions that take the address of a VAR */
    if (q->op == TCCIR_OP_LEA)
    {
      IROperand src1 = tcc_ir_op_get_src1(ir, q);
      int32_t vr = irop_get_vreg(src1);
      if (vr >= 0 && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_VAR)
      {
        int pos = TCCIR_DECODE_VREG_POSITION(vr);
        if (pos <= max_var)
          var_has_lea[pos / 8] |= (1 << (pos % 8));
      }
    }
    if (irop_config[q->op].has_src1)
    {
      IROperand src1 = tcc_ir_op_get_src1(ir, q);
      int32_t vr = irop_get_vreg(src1);
      if (vr >= 0 && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_VAR)
      {
        int pos = TCCIR_DECODE_VREG_POSITION(vr);
        if (pos <= max_var)
          var_read[pos / 8] |= (1 << (pos % 8));
      }
    }
    if (irop_config[q->op].has_src2)
    {
      IROperand src2 = tcc_ir_op_get_src2(ir, q);
      int32_t vr = irop_get_vreg(src2);
      if (vr >= 0 && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_VAR)
      {
        int pos = TCCIR_DECODE_VREG_POSITION(vr);
        if (pos <= max_var)
          var_read[pos / 8] |= (1 << (pos % 8));
      }
    }
  }

  int changes = 0;
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    /* Any pure op writing a VAR may be dropped when the VAR is unread.
     * Broader than just ASSIGN/STORE so e.g. `V = T & 0xFF` (byte truncation
     * of a CALL result into a dead local) gets killed, which lets the
     * downstream call_result→DCE cascade reach the producer call. */
    if (!ir_op_pure_for_dead_var_dest(ir, q))
      continue;
    if (!irop_config[q->op].has_dest)
      continue;
    IROperand dest = tcc_ir_op_get_dest(ir, q);
    int32_t vr = irop_get_vreg(dest);
    if (vr < 0 || TCCIR_DECODE_VREG_TYPE(vr) != TCCIR_VREG_TYPE_VAR)
      continue;
    int pos = TCCIR_DECODE_VREG_POSITION(vr);
    if (pos > max_var)
      continue;
    if (var_read[pos / 8] & (1 << (pos % 8)))
      continue;
    /* If addrtaken, check whether the VAR could actually be read through
     * a pointer.  Two cases:
     * 1) LEA exists for this VAR — pointer alias is live, skip.
     * 2) SET_CHAIN exists — explicit nested call in this function makes
     *    captured VARs reachable via the static chain.
     * If neither applies, the addrtaken flag is a stale frontend
     * annotation (e.g. capture by a now-fully-inlined nested function)
     * and the VAR is safe to eliminate. */
    {
      IRLiveInterval *interval = tcc_ir_get_live_interval(ir, vr);
      if (interval && interval->addrtaken &&
          ((var_has_lea[pos / 8] & (1 << (pos % 8))) || has_set_chain))
        continue;
    }
    LOG_IR_GEN("=== DEAD VAR STORE: eliminating V%d at i=%d (op=%d) ===", pos, i, q->op);
    q->op = TCCIR_OP_NOP;
    changes++;
  }

  LOG_IR_GEN("=== DEAD VAR STORE ELIM: eliminated %d dead VAR stores ===", changes);
  tcc_free(var_has_lea);
  tcc_free(var_read);
  return changes;
}

/* Fold ADD-immediate + DEREF into LOAD_INDEXED with offset.
 *
 * Pattern:  T = base ADD #imm          (T is single-use TEMP)
 *           ... T***DEREF***            (only use, as lval/deref)
 *
 * Becomes:  T = LOAD_INDEXED base, #imm, scale=0   (T = *(base + imm))
 *           ... T                       (plain value, deref cleared)
 *
 * This lets the codegen emit `ldr Rd, [Rbase, #imm]` instead of
 * `add Rt, Rbase, #imm; ldr Rd, [Rt, #0]`, saving one instruction
 * and one register. */

int tcc_ir_opt_dead_addrvar_elim(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  if (n == 0)
    return 0;

  /* Find max VAR and TMP positions */
  int max_var = 0, max_tmp = 0;
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;
    IROperand ops[3];
    ops[0] = tcc_ir_op_get_dest(ir, q);
    ops[1] = tcc_ir_op_get_src1(ir, q);
    ops[2] = tcc_ir_op_get_src2(ir, q);
    for (int k = 0; k < 3; k++)
    {
      int32_t vr = irop_get_vreg(ops[k]);
      if (vr >= 0)
      {
        int type = TCCIR_DECODE_VREG_TYPE(vr);
        int pos = TCCIR_DECODE_VREG_POSITION(vr);
        if (type == TCCIR_VREG_TYPE_VAR && pos > max_var)
          max_var = pos;
        else if (type == TCCIR_VREG_TYPE_TEMP && pos > max_tmp)
          max_tmp = pos;
      }
    }
  }

  if (max_var == 0)
    return 0;

  uint8_t *var_read = tcc_mallocz((max_var + 8) / 8);
  uint8_t *var_has_lea = tcc_mallocz((max_var + 8) / 8);
  int *lea_map = tcc_mallocz(sizeof(int) * (max_tmp + 1));
  int *var_lea = tcc_mallocz(sizeof(int) * (max_var + 1));
  for (int i = 0; i <= max_tmp; i++)
    lea_map[i] = -1;
  for (int i = 0; i <= max_var; i++)
    var_lea[i] = -1;

  /* Pass 1: Build LEA map and mark directly-read VARs.
   * LEA src1 (address-take) is NOT a value read.
   * STORE dest (pointer) is NOT a value read of the pointed-to VAR.
   * Everything else that references a VAR as src1/src2 is a value read. */
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;

    if (q->op == TCCIR_OP_LEA)
    {
      IROperand dest = tcc_ir_op_get_dest(ir, q);
      IROperand src1 = tcc_ir_op_get_src1(ir, q);
      int32_t d_vr = irop_get_vreg(dest);
      int32_t s_vr = irop_get_vreg(src1);
      if (s_vr >= 0 && TCCIR_DECODE_VREG_TYPE(s_vr) == TCCIR_VREG_TYPE_VAR)
      {
        int var_pos = TCCIR_DECODE_VREG_POSITION(s_vr);
        if (d_vr >= 0 && TCCIR_DECODE_VREG_TYPE(d_vr) == TCCIR_VREG_TYPE_TEMP)
        {
          /* LEA with TMP dest: trackable — record in lea_map */
          int tmp_pos = TCCIR_DECODE_VREG_POSITION(d_vr);
          if (tmp_pos <= max_tmp && var_pos <= max_var)
          {
            lea_map[tmp_pos] = var_pos;
            var_has_lea[var_pos / 8] |= (1 << (var_pos % 8));
          }
        }
        else if (var_pos <= max_var)
        {
          /* LEA with VAR dest: pointer escapes into a VAR, conservatively mark as read */
          var_read[var_pos / 8] |= (1 << (var_pos % 8));
        }
      }
      continue;
    }

    /* LEA propagation through VARs: STORE V = T where T is LEA result */
    if (q->op == TCCIR_OP_STORE)
    {
      IROperand dest_op = tcc_ir_op_get_dest(ir, q);
      IROperand src1_op = tcc_ir_op_get_src1(ir, q);
      int32_t d_vr = irop_get_vreg(dest_op);
      int32_t s_vr = irop_get_vreg(src1_op);
      if (d_vr >= 0 && TCCIR_DECODE_VREG_TYPE(d_vr) == TCCIR_VREG_TYPE_VAR && s_vr >= 0 &&
          TCCIR_DECODE_VREG_TYPE(s_vr) == TCCIR_VREG_TYPE_TEMP)
      {
        int d_pos = TCCIR_DECODE_VREG_POSITION(d_vr);
        int s_tmp = TCCIR_DECODE_VREG_POSITION(s_vr);
        if (d_pos <= max_var && s_tmp <= max_tmp && lea_map[s_tmp] >= 0)
          var_lea[d_pos] = lea_map[s_tmp];
      }
    }

    /* LEA propagation: ASSIGN T = V where V holds a LEA result */
    if (q->op == TCCIR_OP_ASSIGN || q->op == TCCIR_OP_LOAD)
    {
      IROperand dest_op = tcc_ir_op_get_dest(ir, q);
      IROperand src1_op = tcc_ir_op_get_src1(ir, q);
      int32_t d_vr = irop_get_vreg(dest_op);
      int32_t s_vr = irop_get_vreg(src1_op);
      if (d_vr >= 0 && TCCIR_DECODE_VREG_TYPE(d_vr) == TCCIR_VREG_TYPE_TEMP && s_vr >= 0 &&
          TCCIR_DECODE_VREG_TYPE(s_vr) == TCCIR_VREG_TYPE_VAR)
      {
        int d_tmp = TCCIR_DECODE_VREG_POSITION(d_vr);
        int s_pos = TCCIR_DECODE_VREG_POSITION(s_vr);
        if (d_tmp <= max_tmp && s_pos <= max_var && var_lea[s_pos] >= 0)
          lea_map[d_tmp] = var_lea[s_pos];
      }
    }

    /* Mark VARs read as src1 (for all instructions including STORE) */
    if (irop_config[q->op].has_src1)
    {
      IROperand src1 = tcc_ir_op_get_src1(ir, q);
      int32_t vr = irop_get_vreg(src1);
      if (vr >= 0 && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_VAR)
      {
        int pos = TCCIR_DECODE_VREG_POSITION(vr);
        if (pos <= max_var)
          var_read[pos / 8] |= (1 << (pos % 8));
      }
    }

    /* Mark VARs read as src2 */
    if (irop_config[q->op].has_src2)
    {
      IROperand src2 = tcc_ir_op_get_src2(ir, q);
      int32_t vr = irop_get_vreg(src2);
      if (vr >= 0 && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_VAR)
      {
        int pos = TCCIR_DECODE_VREG_POSITION(vr);
        if (pos <= max_var)
          var_read[pos / 8] |= (1 << (pos % 8));
      }
    }
  }

  /* Pass 2: Mark VARs whose LEA pointers escape (used outside STORE dest).
   * If a LEA TMP appears as src1/src2 of any instruction, the VAR's address
   * may be used to read the VAR elsewhere — mark it as read.
   * Exception: STORE V = T (pointer copy to VAR) is tracked in Pass 1 and
   * does not constitute a read of the pointed-to VAR. */
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;

    if (irop_config[q->op].has_src1)
    {
      IROperand src1 = tcc_ir_op_get_src1(ir, q);
      int32_t vr = irop_get_vreg(src1);
      if (vr >= 0 && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_TEMP)
      {
        int tmp_pos = TCCIR_DECODE_VREG_POSITION(vr);
        if (tmp_pos <= max_tmp && lea_map[tmp_pos] >= 0)
        {
          int is_ptr_copy = 0;
          if (q->op == TCCIR_OP_STORE)
          {
            IROperand d = tcc_ir_op_get_dest(ir, q);
            int32_t d_vr = irop_get_vreg(d);
            if (d_vr >= 0 && TCCIR_DECODE_VREG_TYPE(d_vr) == TCCIR_VREG_TYPE_VAR)
              is_ptr_copy = 1;
          }
          if (!is_ptr_copy)
          {
            int var_pos = lea_map[tmp_pos];
            if (var_pos <= max_var)
              var_read[var_pos / 8] |= (1 << (var_pos % 8));
          }
        }
      }
    }

    if (irop_config[q->op].has_src2)
    {
      IROperand src2 = tcc_ir_op_get_src2(ir, q);
      int32_t vr = irop_get_vreg(src2);
      if (vr >= 0 && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_TEMP)
      {
        int tmp_pos = TCCIR_DECODE_VREG_POSITION(vr);
        if (tmp_pos <= max_tmp && lea_map[tmp_pos] >= 0)
        {
          int var_pos = lea_map[tmp_pos];
          if (var_pos <= max_var)
            var_read[var_pos / 8] |= (1 << (var_pos % 8));
        }
      }
    }
  }

  /* Pass 2b: Mark VARs as read when their pointer escapes via FUNCPARAMVAL.
   * If a VAR in var_lea is passed as a function argument, the pointer
   * escapes to the callee which may read through it. */
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_FUNCPARAMVAL)
      continue;
    IROperand param_val = tcc_ir_op_get_src1(ir, q);
    int32_t vr = irop_get_vreg(param_val);
    if (vr >= 0 && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_VAR)
    {
      int pos = TCCIR_DECODE_VREG_POSITION(vr);
      if (pos <= max_var && var_lea[pos] >= 0)
      {
        int target = var_lea[pos];
        if (target <= max_var)
          var_read[target / 8] |= (1 << (target % 8));
      }
    }
    if (vr >= 0 && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_TEMP)
    {
      int tmp_pos = TCCIR_DECODE_VREG_POSITION(vr);
      if (tmp_pos <= max_tmp && lea_map[tmp_pos] >= 0)
      {
        int target = lea_map[tmp_pos];
        if (target <= max_var)
          var_read[target / 8] |= (1 << (target % 8));
      }
    }
  }

  /* Pass 3: Eliminate dead writes to unread VARs */
  int changes = 0;
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;

    /* ASSIGN to dead VAR -> NOP (only if VAR has LEA, proving we track all accesses) */
    if (q->op == TCCIR_OP_ASSIGN && irop_config[q->op].has_dest)
    {
      IROperand dest = tcc_ir_op_get_dest(ir, q);
      int32_t vr = irop_get_vreg(dest);
      if (vr >= 0 && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_VAR)
      {
        int pos = TCCIR_DECODE_VREG_POSITION(vr);
        if (pos <= max_var && (var_has_lea[pos / 8] & (1 << (pos % 8))) && !(var_read[pos / 8] & (1 << (pos % 8))))
        {
          q->op = TCCIR_OP_NOP;
          changes++;
        }
      }
    }

    /* LEA from dead VAR -> NOP (only for TMP destinations; VAR dests handled by regular DSE) */
    if (q->op == TCCIR_OP_LEA)
    {
      IROperand dest = tcc_ir_op_get_dest(ir, q);
      int32_t d_vr = irop_get_vreg(dest);
      if (d_vr >= 0 && TCCIR_DECODE_VREG_TYPE(d_vr) == TCCIR_VREG_TYPE_TEMP)
      {
        IROperand src1 = tcc_ir_op_get_src1(ir, q);
        int32_t vr = irop_get_vreg(src1);
        if (vr >= 0 && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_VAR)
        {
          int pos = TCCIR_DECODE_VREG_POSITION(vr);
          if (pos <= max_var && (var_has_lea[pos / 8] & (1 << (pos % 8))) && !(var_read[pos / 8] & (1 << (pos % 8))))
          {
            q->op = TCCIR_OP_NOP;
            changes++;
          }
        }
      }
    }

    /* STORE through LEA to dead VAR -> NOP */
    if (q->op == TCCIR_OP_STORE)
    {
      IROperand dest = tcc_ir_op_get_dest(ir, q);
      int32_t vr = irop_get_vreg(dest);
      if (vr >= 0 && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_TEMP)
      {
        int tmp_pos = TCCIR_DECODE_VREG_POSITION(vr);
        if (tmp_pos <= max_tmp && lea_map[tmp_pos] >= 0)
        {
          int var_pos = lea_map[tmp_pos];
          if (var_pos <= max_var && !(var_read[var_pos / 8] & (1 << (var_pos % 8))))
          {
            q->op = TCCIR_OP_NOP;
            changes++;
          }
        }
      }
    }
  }

  /* STORE to dead VAR (non-deref): V = val where V is unread */
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_STORE)
      continue;
    IROperand dest = tcc_ir_op_get_dest(ir, q);
    int32_t vr = irop_get_vreg(dest);
    if (vr >= 0 && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_VAR)
    {
      int pos = TCCIR_DECODE_VREG_POSITION(vr);
      if (pos <= max_var && (var_has_lea[pos / 8] & (1 << (pos % 8))) && !(var_read[pos / 8] & (1 << (pos % 8))))
      {
        q->op = TCCIR_OP_NOP;
        changes++;
      }
    }
  }

  LOG_IR_GEN("=== DEAD ADDRVAR ELIM: eliminated %d dead writes ===", changes);

  tcc_free(var_lea);
  tcc_free(lea_map);
  tcc_free(var_has_lea);
  tcc_free(var_read);
  return changes;
}

/* Redundant VAR ASSIGN elimination.
 * Forward scan within basic blocks: if a VAR is assigned and then assigned
 * again without being read in between, the first assign is dead → NOP it.
 * This catches patterns like repeated overflow flag stores where earlier
 * values are overwritten before use.
 */
int tcc_ir_opt_redundant_var_assign(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  if (n == 0)
    return 0;

  /* Find max VAR position */
  int max_var = 0;
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;
    IROperand ops[3];
    ops[0] = tcc_ir_op_get_dest(ir, q);
    ops[1] = tcc_ir_op_get_src1(ir, q);
    ops[2] = tcc_ir_op_get_src2(ir, q);
    for (int k = 0; k < 3; k++)
    {
      int32_t vr = irop_get_vreg(ops[k]);
      if (vr >= 0 && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_VAR)
      {
        int pos = TCCIR_DECODE_VREG_POSITION(vr);
        if (pos > max_var)
          max_var = pos;
      }
    }
  }

  if (max_var == 0)
    return 0;

  /* Mark jump targets as merge points — must flush pending at these */
  uint8_t *is_target = tcc_mallocz((n + 7) / 8);
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_JUMPIF)
    {
      IROperand dest = tcc_ir_op_get_dest(ir, q);
      int target = (int)dest.u.imm32;
      if (target >= 0 && target < n)
        is_target[target / 8] |= (1 << (target % 8));
    }
  }

  /* pending[v] = instruction index of last unread ASSIGN to VAR v, or -1 */
  int *pending = tcc_malloc(sizeof(int) * (max_var + 1));
  for (int v = 0; v <= max_var; v++)
    pending[v] = -1;

  int changes = 0;
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;

    /* Flush at merge points (jump targets) */
    if (is_target[i / 8] & (1 << (i % 8)))
    {
      for (int v = 0; v <= max_var; v++)
        pending[v] = -1;
    }

    /* Block boundary: flush all pending */
    if (q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_JUMPIF || q->op == TCCIR_OP_FUNCCALLVOID ||
        q->op == TCCIR_OP_FUNCCALLVAL || q->op == TCCIR_OP_RETURNVALUE || q->op == TCCIR_OP_RETURNVOID ||
        q->op == TCCIR_OP_FUNCPARAMVAL || q->op == TCCIR_OP_SWITCH_TABLE)
    {
      /* First process reads in this instruction (src1/src2) */
      if (irop_config[q->op].has_src1)
      {
        IROperand src1 = tcc_ir_op_get_src1(ir, q);
        int32_t vr = irop_get_vreg(src1);
        if (vr >= 0 && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_VAR)
        {
          int pos = TCCIR_DECODE_VREG_POSITION(vr);
          if (pos <= max_var)
            pending[pos] = -1;
        }
      }
      /* Flush all */
      for (int v = 0; v <= max_var; v++)
        pending[v] = -1;
      continue;
    }

    /* Process reads: src1 and src2 clear pending for read VARs */
    if (irop_config[q->op].has_src1)
    {
      IROperand src1 = tcc_ir_op_get_src1(ir, q);
      int32_t vr = irop_get_vreg(src1);
      if (vr >= 0 && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_VAR)
      {
        int pos = TCCIR_DECODE_VREG_POSITION(vr);
        if (pos <= max_var)
          pending[pos] = -1;
      }
    }
    if (irop_config[q->op].has_src2)
    {
      IROperand src2 = tcc_ir_op_get_src2(ir, q);
      int32_t vr = irop_get_vreg(src2);
      if (vr >= 0 && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_VAR)
      {
        int pos = TCCIR_DECODE_VREG_POSITION(vr);
        if (pos <= max_var)
          pending[pos] = -1;
      }
    }

    /* STORE dest is a pointer USE — if it's a VAR, count as read */
    if (q->op == TCCIR_OP_STORE || q->op == TCCIR_OP_STORE_INDEXED)
    {
      IROperand dest = tcc_ir_op_get_dest(ir, q);
      int32_t vr = irop_get_vreg(dest);
      if (vr >= 0 && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_VAR)
      {
        int pos = TCCIR_DECODE_VREG_POSITION(vr);
        if (pos <= max_var)
          pending[pos] = -1;
      }
      continue;
    }

    /* Process write: if dest is VAR, check for redundant prior assign */
    if (irop_config[q->op].has_dest)
    {
      IROperand dest = tcc_ir_op_get_dest(ir, q);
      int32_t vr = irop_get_vreg(dest);
      if (vr >= 0 && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_VAR)
      {
        int pos = TCCIR_DECODE_VREG_POSITION(vr);
        if (pos <= max_var)
        {
          if (pending[pos] >= 0)
          {
            /* Previous assign to this VAR is dead — overwritten before read */
            ir->compact_instructions[pending[pos]].op = TCCIR_OP_NOP;
            changes++;
          }
          pending[pos] = i;
        }
      }
    }
  }

  LOG_IR_GEN("=== REDUNDANT VAR ASSIGN: eliminated %d dead assigns ===", changes);

  tcc_free(pending);
  tcc_free(is_target);
  return changes;
}

/* vrp_swap_cmp_tok now in opt_utils.h */


int tcc_ir_opt_redundant_init_elim(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  int changes = 0;

  if (n <= 1)
    return 0;

  /* Bail if function has indirect jumps (setjmp/longjmp, computed goto).
   * These create hidden control flow that our BFS doesn't follow. */
  for (int i = 0; i < n; i++)
  {
    if (ir->compact_instructions[i].op == TCCIR_OP_IJUMP)
      return 0;
  }

  /* Find function-entry VAR inits: instructions before any jump target
   * that assign a constant to a VAR. */
  for (int init_idx = 0; init_idx < n; init_idx++)
  {
    IRQuadCompact *q = &ir->compact_instructions[init_idx];
    if (q->is_jump_target)
      break;
    if (q->op != TCCIR_OP_ASSIGN)
      continue;

    IROperand dest = tcc_ir_op_get_dest(ir, q);
    int32_t vr = irop_get_vreg(dest);
    if (vr < 0 || TCCIR_DECODE_VREG_TYPE(vr) != TCCIR_VREG_TYPE_VAR)
      continue;

    IROperand src = tcc_ir_op_get_src1(ir, q);
    if (!irop_is_immediate(src))
      continue;

    IRLiveInterval *interval = tcc_ir_get_live_interval(ir, vr);
    if (interval && interval->addrtaken)
      continue;

    /* Forward reachability: check if V is killed on all paths before use.
     * State per instruction: 0=unvisited, 1=V-alive (init not yet killed),
     * 2=V-killed (redef seen on this path). */
    uint8_t *state = tcc_mallocz(n);
    int found_use_before_kill = 0;

    /* Worklist-based BFS from init_idx+1 */
    int *worklist = tcc_malloc(n * sizeof(int));
    int wl_head = 0, wl_tail = 0;
    worklist[wl_tail++] = init_idx + 1;
    state[init_idx] = 1;

    while (wl_head < wl_tail && !found_use_before_kill)
    {
      int idx = worklist[wl_head++];
      if (idx < 0 || idx >= n)
        continue;
      if (state[idx] == 2)
        continue; /* already killed on this path */
      if (state[idx] == 1)
        continue;     /* already queued as alive */
      state[idx] = 1; /* mark as V-alive */

      IRQuadCompact *iq = &ir->compact_instructions[idx];
      if (iq->op == TCCIR_OP_NOP)
      {
        if (idx + 1 < n)
          worklist[wl_tail++] = idx + 1;
        continue;
      }

      /* Check if this instruction USES V */
      if (irop_config[iq->op].has_src1 && irop_get_vreg(tcc_ir_op_get_src1(ir, iq)) == vr)
      {
        found_use_before_kill = 1;
        break;
      }
      if (irop_config[iq->op].has_src2 && irop_get_vreg(tcc_ir_op_get_src2(ir, iq)) == vr)
      {
        found_use_before_kill = 1;
        break;
      }
      /* STORE/FUNCPARAMVAL dest is a use */
      if ((iq->op == TCCIR_OP_STORE || iq->op == TCCIR_OP_STORE_INDEXED || iq->op == TCCIR_OP_FUNCPARAMVAL) &&
          irop_get_vreg(tcc_ir_op_get_dest(ir, iq)) == vr)
      {
        found_use_before_kill = 1;
        break;
      }

      /* Check if this instruction DEFINES (kills) V.
       * Only treat as kill if the source is explicit (immediate or vreg).
       * Bare defs like ASM outputs (V <-- with no visible source) may
       * implicitly depend on V's prior value via register constraints. */
      if (irop_config[iq->op].has_dest && iq->op != TCCIR_OP_STORE && iq->op != TCCIR_OP_STORE_INDEXED &&
          iq->op != TCCIR_OP_FUNCPARAMVAL)
      {
        IROperand d = tcc_ir_op_get_dest(ir, iq);
        if (irop_get_vreg(d) == vr)
        {
          int has_explicit_src = 0;
          if (irop_config[iq->op].has_src1)
          {
            IROperand s = tcc_ir_op_get_src1(ir, iq);
            if (irop_is_immediate(s) || irop_has_vreg(s))
              has_explicit_src = 1;
          }
          if (has_explicit_src)
          {
            state[idx] = 2; /* killed */
            continue;       /* don't follow successors — V is dead on this path */
          }
        }
      }

      /* Follow successors */
      if (iq->op == TCCIR_OP_JUMP)
      {
        IROperand jd = tcc_ir_op_get_dest(ir, iq);
        int target = (int)irop_get_imm64_ex(ir, jd);
        if (target >= 0 && target < n && state[target] == 0)
          worklist[wl_tail++] = target;
      }
      else if (iq->op == TCCIR_OP_JUMPIF)
      {
        IROperand jd = tcc_ir_op_get_dest(ir, iq);
        int target = (int)irop_get_imm64_ex(ir, jd);
        if (target >= 0 && target < n && state[target] == 0)
          worklist[wl_tail++] = target;
        if (idx + 1 < n && state[idx + 1] == 0)
          worklist[wl_tail++] = idx + 1;
      }
      else if (iq->op == TCCIR_OP_RETURNVALUE)
      {
        /* Return with V alive but unused = V is dead (return doesn't use V
         * as an argument here since we checked src1 above) */
      }
      else
      {
        if (idx + 1 < n && state[idx + 1] == 0)
          worklist[wl_tail++] = idx + 1;
      }
    }

    tcc_free(worklist);
    tcc_free(state);

    if (!found_use_before_kill)
    {
      q->op = TCCIR_OP_NOP;
      changes++;
    }
  }

  return changes;
}


int tcc_ir_opt_dead_loop_elim(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  int changes = 0;
  if (n == 0)
    return 0;

  IRLoops *loops = tcc_ir_detect_loops(ir);
  if (!loops || loops->num_loops == 0)
  {
    tcc_ir_free_loops(loops);
    return 0;
  }

  for (int li = 0; li < loops->num_loops; li++)
  {
    IRLoop *loop = &loops->loops[li];
    if (loop->num_body_instrs == 0)
      continue;

    int has_side_effects = 0;
    int num_const_assigns = 0; (void)num_const_assigns;
    int has_loop_counter = 0;

    /* Track which VARs get constant assignments inside the loop body */
    typedef struct
    {
      int var_pos;
      int64_t value;
      int btype;
    } ConstVar;
    ConstVar const_vars[8];
    int num_const_vars = 0;

    for (int idx = loop->start_idx; idx <= loop->end_idx && idx < n; idx++)
    {
      IRQuadCompact *q = &ir->compact_instructions[idx];

      if (q->op == TCCIR_OP_NOP)
        continue;
      if (q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_JUMPIF)
        continue;
      if (q->op == TCCIR_OP_CMP || q->op == TCCIR_OP_TEST_ZERO)
        continue;

      /* Calls are side effects */
      if (q->op == TCCIR_OP_FUNCCALLVAL || q->op == TCCIR_OP_FUNCCALLVOID)
      {
        has_side_effects = 1;
        break;
      }

      /* Stores to memory (non-local) are side effects */
      if (q->op == TCCIR_OP_STORE || q->op == TCCIR_OP_STORE_INDEXED)
      {
        has_side_effects = 1;
        break;
      }

      /* PARAM instructions (function call setup) */
      if (q->op == TCCIR_OP_FUNCPARAMVOID || q->op == TCCIR_OP_FUNCPARAMVAL)
        continue;

      /* VAR <- immediate constant assignment: safe, track it */
      if (q->op == TCCIR_OP_ASSIGN)
      {
        IROperand dest = tcc_ir_op_get_dest(ir, q);
        IROperand src1 = tcc_ir_op_get_src1(ir, q);
        int32_t dest_vr = irop_get_vreg(dest);

        if (TCCIR_DECODE_VREG_TYPE(dest_vr) == TCCIR_VREG_TYPE_VAR && irop_is_immediate(src1))
        {
          int var_pos = TCCIR_DECODE_VREG_POSITION(dest_vr);
          int64_t val = irop_get_imm64_ex(ir, src1);
          int btype = irop_get_btype(src1);

          /* Check if we already track this VAR */
          int found = 0;
          for (int vi = 0; vi < num_const_vars; vi++)
          {
            if (const_vars[vi].var_pos == var_pos)
            {
              if (const_vars[vi].value != val)
              {
                has_side_effects = 1; /* Different values on different paths */
              }
              found = 1;
              break;
            }
          }
          if (has_side_effects)
            break;
          if (!found && num_const_vars < 8)
          {
            const_vars[num_const_vars].var_pos = var_pos;
            const_vars[num_const_vars].value = val;
            const_vars[num_const_vars].btype = btype;
            num_const_vars++;
          }
          num_const_assigns++;
          continue;
        }

        /* TMP <- anything (loop counter, etc): OK, no side effect */
        if (TCCIR_DECODE_VREG_TYPE(dest_vr) == TCCIR_VREG_TYPE_TEMP)
          continue;
      }

      /* ADD/SUB on TMPs or dead VARs (loop counters): safe.
       * A VAR modified by ADD/SUB is safe only if not used after the loop
       * (just a dead counter). If used after the loop, it's meaningful
       * accumulation (like sum += 1) and the loop is NOT dead. */
      if ((q->op == TCCIR_OP_ADD || q->op == TCCIR_OP_SUB) && irop_is_immediate(tcc_ir_op_get_src2(ir, q)))
      {
        IROperand dest = tcc_ir_op_get_dest(ir, q);
        int32_t dest_vr = irop_get_vreg(dest);
        if (TCCIR_DECODE_VREG_TYPE(dest_vr) == TCCIR_VREG_TYPE_TEMP)
        {
          has_loop_counter = 1;
          continue;
        }
        if (TCCIR_DECODE_VREG_TYPE(dest_vr) == TCCIR_VREG_TYPE_VAR)
        {
          int var_used_after = 0;
          for (int post = loop->end_idx + 1; post < n; post++)
          {
            IRQuadCompact *pq = &ir->compact_instructions[post];
            if (pq->op == TCCIR_OP_NOP)
              continue;
            if (irop_config[pq->op].has_src1)
            {
              int32_t s1 = irop_get_vreg(tcc_ir_op_get_src1(ir, pq));
              if (s1 == dest_vr)
              {
                var_used_after = 1;
                break;
              }
            }
            if (irop_config[pq->op].has_src2)
            {
              int32_t s2 = irop_get_vreg(tcc_ir_op_get_src2(ir, pq));
              if (s2 == dest_vr)
              {
                var_used_after = 1;
                break;
              }
            }
          }
          if (var_used_after)
          {
            has_side_effects = 1;
            break;
          }
          has_loop_counter = 1;
          continue;
        }
      }

      /* LOAD of a VAR (reading the result): safe */
      if (q->op == TCCIR_OP_LOAD)
      {
        IROperand dest = tcc_ir_op_get_dest(ir, q);
        if (!dest.is_lval)
          continue;
      }

      /* Anything else is a potential side effect */
      has_side_effects = 1;
      break;
    }

    if (has_side_effects || num_const_vars == 0 || !has_loop_counter)
      continue;

    /* The loop body only contains constant VAR assignments and counter updates.
     * NOP all body instructions and place constant assignments in the preheader. */
    LOG_IR_GEN("OPTIMIZE: Dead loop elimination at header=%d (%d const vars)", loop->header_idx, num_const_vars);

    /* NOP loop body instructions within [start_idx, end_idx] only.
     * Instructions outside this range (exit targets, returns) must not be touched. */
    for (int idx = loop->start_idx; idx <= loop->end_idx && idx < n; idx++)
    {
      ir->compact_instructions[idx].op = TCCIR_OP_NOP;
    }

    /* Place constant assignments in the preheader (or at loop header).
     * Use the first available NOP slot at or before the header. */
    int insert_at = loop->preheader_idx >= 0 ? loop->preheader_idx : loop->header_idx;
    for (int vi = 0; vi < num_const_vars; vi++)
    {
      /* Find a NOP slot at or after insert_at */
      int slot = -1;
      for (int j = insert_at; j < n; j++)
      {
        if (ir->compact_instructions[j].op == TCCIR_OP_NOP)
        {
          slot = j;
          break;
        }
      }
      if (slot < 0)
        continue;

      int32_t dest_vr = TCCIR_ENCODE_VREG(TCCIR_VREG_TYPE_VAR, const_vars[vi].var_pos);
      ir->compact_instructions[slot].op = TCCIR_OP_ASSIGN;
      IROperand dest_op = irop_make_vreg(dest_vr, const_vars[vi].btype);
      tcc_ir_set_dest(ir, slot, dest_op);
      if (const_vars[vi].value == (int32_t)const_vars[vi].value)
        tcc_ir_set_src1(ir, slot, irop_make_imm32(-1, (int32_t)const_vars[vi].value, const_vars[vi].btype));
      else
      {
        uint32_t pool_idx = tcc_ir_pool_add_i64(ir, const_vars[vi].value);
        tcc_ir_set_src1(ir, slot, irop_make_i64(-1, pool_idx, const_vars[vi].btype));
      }
      tcc_ir_set_src2(ir, slot, IROP_NONE);
    }

    changes++;
  }

  tcc_ir_free_loops(loops);
  return changes;
}

int tcc_ir_opt_dead_loop_elim_ex(IROptCtx *ctx) { return tcc_ir_opt_dead_loop_elim(ctx->ir); }
int tcc_ir_opt_redundant_var_assign_ex(IROptCtx *ctx) { return tcc_ir_opt_redundant_var_assign(ctx->ir); }
int tcc_ir_opt_dead_var_store_elim_ex(IROptCtx *ctx) { return tcc_ir_opt_dead_var_store_elim(ctx->ir); }
int tcc_ir_opt_dead_addrvar_elim_ex(IROptCtx *ctx) { return tcc_ir_opt_dead_addrvar_elim(ctx->ir); }

/* Unconditional Uninitialized Local UB Exploit
 *
 * If the entry basic block unconditionally reads a TCCIR_VREG_TYPE_VAR (local C
 * variable) before any write to it, that read is undefined behavior per C11.
 * Under UB the implementation may do anything; we choose to collapse the entire
 * function body to a single self-jump (`b .`), matching GCC's behavior on
 * gcc.c-torture/compile/931102-1.c.
 *
 * Conservative: scan stops at the first branch/call/return or any subsequent
 * jump-target, so conditional reads (where some path may not exercise the UB)
 * do not trigger the fold.
 */
int tcc_ir_opt_uninit_local_ub(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  if (n == 0)
    return 0;
  /* O2-only: UB exploitation is too aggressive for lower levels where users
   * may rely on "whatever happens to be in the stack slot" semantics. */
  if (!tcc_state || tcc_state->optimize < 2)
    return 0;

  /* Bail on any function containing inline asm / computed goto.  Inline asm
   * operand semantics (outputs written, inputs read, clobbers) aren't modeled
   * with simple has_dest/has_src1/src2 tracking, so a UB read could be hidden
   * inside an asm output and we'd mistakenly collapse the body.  IJUMP has
   * unknown-at-compile-time targets, same risk. */
  for (int i = 0; i < n; i++)
  {
    TccIrOp op = ir->compact_instructions[i].op;
    if (op == TCCIR_OP_ASM_INPUT || op == TCCIR_OP_ASM_OUTPUT || op == TCCIR_OP_INLINE_ASM || op == TCCIR_OP_IJUMP)
      return 0;
  }

#define UNINIT_MAX_VAR_POS 1024
  uint8_t written[(UNINIT_MAX_VAR_POS + 7) / 8] = {0};
  uint8_t addr_taken[(UNINIT_MAX_VAR_POS + 7) / 8] = {0};

  /* Pre-scan: identify any VAR whose address is taken anywhere in the function.
   * Address-taken VARs may be written through pointer aliases we cannot
   * statically track, so we conservatively exclude them. */
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;
    for (int k = 0; k <= 2; k++)
    {
      IROperand op;
      if (k == 0)
      {
        if (!irop_config[q->op].has_dest)
          continue;
        op = tcc_ir_op_get_dest(ir, q);
      }
      else if (k == 1)
      {
        if (!irop_config[q->op].has_src1)
          continue;
        op = tcc_ir_op_get_src1(ir, q);
      }
      else
      {
        if (!irop_config[q->op].has_src2)
          continue;
        op = tcc_ir_op_get_src2(ir, q);
      }
      int32_t vr = irop_get_vreg(op);
      if (vr < 0)
        continue;
      if (TCCIR_DECODE_VREG_TYPE(vr) != TCCIR_VREG_TYPE_VAR)
        continue;
      /* Address-of pattern: is_local=1, is_lval=0 (the operand carries the
       * VAR's address, not its value). */
      if (op.is_local && !op.is_lval)
      {
        int pos = TCCIR_DECODE_VREG_POSITION(vr);
        if (pos >= 0 && pos < UNINIT_MAX_VAR_POS)
          addr_taken[pos >> 3] |= (uint8_t)(1u << (pos & 7));
      }
    }
    /* LEA on a VAR is also address-of even if the operand encoding isn't the
     * direct is_local+!is_lval pattern. */
    if (q->op == TCCIR_OP_LEA && irop_config[q->op].has_src1)
    {
      IROperand op = tcc_ir_op_get_src1(ir, q);
      int32_t vr = irop_get_vreg(op);
      if (vr >= 0 && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_VAR)
      {
        int pos = TCCIR_DECODE_VREG_POSITION(vr);
        if (pos >= 0 && pos < UNINIT_MAX_VAR_POS)
          addr_taken[pos >> 3] |= (uint8_t)(1u << (pos & 7));
      }
    }
  }

  int found_uninit = 0;

  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;
    /* Subsequent jump targets end the entry block. */
    if (i > 0 && q->is_jump_target)
      break;

    /* Check src1 / src2 for read of an unwritten VAR. */
    for (int k = 1; k <= 2 && !found_uninit; k++)
    {
      if (k == 1 && !irop_config[q->op].has_src1)
        continue;
      if (k == 2 && !irop_config[q->op].has_src2)
        continue;
      IROperand sop = (k == 1) ? tcc_ir_op_get_src1(ir, q) : tcc_ir_op_get_src2(ir, q);
      int32_t svr = irop_get_vreg(sop);
      if (svr < 0)
        continue;
      if (TCCIR_DECODE_VREG_TYPE(svr) != TCCIR_VREG_TYPE_VAR)
        continue;
      /* Pure address-of (is_local && !is_lval) is not a value read. */
      if (sop.is_local && !sop.is_lval)
        continue;
      int pos = TCCIR_DECODE_VREG_POSITION(svr);
      if (pos < 0 || pos >= UNINIT_MAX_VAR_POS)
        continue;
      /* Skip address-taken VARs — pointer writes may have initialized them. */
      if (addr_taken[pos >> 3] & (uint8_t)(1u << (pos & 7)))
        continue;
      if (!(written[pos >> 3] & (uint8_t)(1u << (pos & 7))))
      {
        found_uninit = 1;
        break;
      }
    }
    if (found_uninit)
      break;

    /* Apply this op's WRITE after the read check (program order). */
    if (irop_config[q->op].has_dest)
    {
      IROperand dop = tcc_ir_op_get_dest(ir, q);
      int32_t dvr = irop_get_vreg(dop);
      if (dvr >= 0 && TCCIR_DECODE_VREG_TYPE(dvr) == TCCIR_VREG_TYPE_VAR)
      {
        int pos = TCCIR_DECODE_VREG_POSITION(dvr);
        if (pos >= 0 && pos < UNINIT_MAX_VAR_POS)
          written[pos >> 3] |= (uint8_t)(1u << (pos & 7));
      }
    }

    /* Entry-block terminators: anything that splits control flow or may not
     * return.  CALL ends the scan because the callee may initialize VARs whose
     * addresses escaped before the call (we don't track aliasing here). */
    if (q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_JUMPIF || q->op == TCCIR_OP_IJUMP ||
        q->op == TCCIR_OP_SWITCH_TABLE || q->op == TCCIR_OP_FUNCCALLVAL || q->op == TCCIR_OP_FUNCCALLVOID ||
        q->op == TCCIR_OP_RETURNVALUE || q->op == TCCIR_OP_RETURNVOID)
      break;
  }
#undef UNINIT_MAX_VAR_POS

  if (!found_uninit)
    return 0;

  LOG_IR_GEN("UNINIT-UB: collapsing function body to infinite loop (read of uninit local in entry block)");

  /* Replace the whole IR with a single self-jump.  Other passes (compact_nops,
   * codegen) will see only the JUMP and emit `b .` with a minimal prologue. */
  for (int i = 0; i < n; i++)
  {
    ir->compact_instructions[i].op = TCCIR_OP_NOP;
    ir->compact_instructions[i].is_jump_target = 0;
  }

  ir->compact_instructions[0].op = TCCIR_OP_JUMP;
  ir->compact_instructions[0].is_jump_target = 1;
  IROperand self = irop_make_imm32(-1, 0, IROP_BTYPE_INT32);
  tcc_ir_set_dest(ir, 0, self);
  tcc_ir_set_src1(ir, 0, IROP_NONE);
  tcc_ir_set_src2(ir, 0, IROP_NONE);

  /* Function is now a leaf (no calls left). */
  ir->leaffunc = 1;

  return 1;
}

int tcc_ir_opt_uninit_local_ub_ex(IROptCtx *ctx) { return tcc_ir_opt_uninit_local_ub(ctx->ir); }

/* UB-Only Function Body Elide
 *
 * Generalises uninit_local_ub: if every STORE in the function has an address
 * that traces back through arithmetic / loads to a read of a never-initialized
 * local VAR, every observable effect of the function is UB.  Per C11 we may
 * choose any behaviour; choosing "return immediately" matches GCC -O2 on
 * gcc.c-torture/compile/pr24883.c (where the only side effect is a STORE
 * through an uninitialised `stl` pointer guarded by reads of other uninit
 * locals).  uninit_local_ub can't catch this case because the entry block
 * writes the loop counter before any uninit read; useless_function_body can't
 * either because the body contains essential STOREs and backward loop jumps.
 *
 * Unlike uninit_local_ub which collapses to `b .` (preserving non-termination
 * as the "chosen" UB behaviour for unconditional entry-block UB), this collapse
 * picks "fall through to epilogue" because the function is `void` and has no
 * other side effects worth preserving — matching GCC's choice.
 *
 * Loops with no remaining observable effect are permitted to be assumed
 * terminating (C11 6.8.5/6), so dropping backward jumps here is sound once
 * every STORE is UB-tainted.
 */
int tcc_ir_opt_ub_only_body_elide(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  if (n == 0)
    return 0;
  /* O2-only: same gating philosophy as uninit_local_ub. */
  if (!tcc_state || tcc_state->optimize < 2)
    return 0;

  /* First pass: scan for ops that make whole-function elision unsafe and
   * inventory the STOREs we'll need to prove are UB. */
  int has_store = 0;
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;

    switch (q->op)
    {
    /* Externally observable / unmodellable — can't elide. */
    case TCCIR_OP_FUNCCALLVAL:
    case TCCIR_OP_FUNCCALLVOID:
    case TCCIR_OP_FUNCPARAMVAL:
    case TCCIR_OP_FUNCPARAMVOID:
    case TCCIR_OP_CALLSEQ_BEGIN:
    case TCCIR_OP_CALLARG_REG:
    case TCCIR_OP_CALLARG_STACK:
    case TCCIR_OP_CALLSEQ_END:
    case TCCIR_OP_INLINE_ASM:
    case TCCIR_OP_ASM_INPUT:
    case TCCIR_OP_ASM_OUTPUT:
    case TCCIR_OP_RETURNVALUE: /* non-void return: can't drop the return value */
    case TCCIR_OP_TRAP:
    case TCCIR_OP_IJUMP:
    case TCCIR_OP_SETJMP:
    case TCCIR_OP_LONGJMP:
    case TCCIR_OP_NL_SETJMP:
    case TCCIR_OP_NL_LONGJMP:
    case TCCIR_OP_BUILTIN_APPLY_ARGS:
    case TCCIR_OP_BUILTIN_APPLY:
    case TCCIR_OP_BUILTIN_RETURN:
    case TCCIR_OP_VLA_ALLOC:
    case TCCIR_OP_VLA_SP_SAVE:
    case TCCIR_OP_VLA_SP_RESTORE:
    case TCCIR_OP_BLOCK_COPY:
    case TCCIR_OP_INIT_CHAIN_SLOT:
    case TCCIR_OP_PREFETCH:
    case TCCIR_OP_SWITCH_TABLE:
    case TCCIR_OP_SWITCH_LOAD:
      return 0;
    case TCCIR_OP_STORE:
    case TCCIR_OP_STORE_INDEXED:
    case TCCIR_OP_STORE_POSTINC:
      has_store = 1;
      break;
    default:
      break;
    }

    /* Volatile sym access on any operand keeps the body alive. */
    for (int k = 0; k <= 2; k++)
    {
      IROperand op;
      if (k == 0)
      {
        if (!irop_config[q->op].has_dest)
          continue;
        op = tcc_ir_op_get_dest(ir, q);
      }
      else if (k == 1)
      {
        if (!irop_config[q->op].has_src1)
          continue;
        op = tcc_ir_op_get_src1(ir, q);
      }
      else
      {
        if (!irop_config[q->op].has_src2)
          continue;
        op = tcc_ir_op_get_src2(ir, q);
      }
      if (op.is_sym)
      {
        Sym *sym = irop_get_sym_ex(ir, op);
        if (sym && (sym->type.t & VT_VOLATILE))
          return 0;
      }
    }
  }

  /* No STOREs → useless_function_body already handles this; nothing to do. */
  if (!has_store)
    return 0;

#define UB_ELIDE_MAX_VAR_POS 1024
#define UB_ELIDE_MAX_TEMPS 8192
  uint8_t var_written[(UB_ELIDE_MAX_VAR_POS + 7) / 8] = {0};
  uint8_t var_addr_taken[(UB_ELIDE_MAX_VAR_POS + 7) / 8] = {0};
  uint8_t temp_tainted[(UB_ELIDE_MAX_TEMPS + 7) / 8] = {0};

  /* Inventory VAR writes and address-takes (same logic as uninit_local_ub). */
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;
    for (int k = 0; k <= 2; k++)
    {
      IROperand op;
      if (k == 0)
      {
        if (!irop_config[q->op].has_dest)
          continue;
        op = tcc_ir_op_get_dest(ir, q);
      }
      else if (k == 1)
      {
        if (!irop_config[q->op].has_src1)
          continue;
        op = tcc_ir_op_get_src1(ir, q);
      }
      else
      {
        if (!irop_config[q->op].has_src2)
          continue;
        op = tcc_ir_op_get_src2(ir, q);
      }
      int32_t vr = irop_get_vreg(op);
      if (vr < 0)
        continue;
      if (TCCIR_DECODE_VREG_TYPE(vr) != TCCIR_VREG_TYPE_VAR)
        continue;
      if (op.is_local && !op.is_lval)
      {
        int pos = TCCIR_DECODE_VREG_POSITION(vr);
        if (pos >= 0 && pos < UB_ELIDE_MAX_VAR_POS)
          var_addr_taken[pos >> 3] |= (uint8_t)(1u << (pos & 7));
      }
    }
    if (q->op == TCCIR_OP_LEA && irop_config[q->op].has_src1)
    {
      IROperand op = tcc_ir_op_get_src1(ir, q);
      int32_t vr = irop_get_vreg(op);
      if (vr >= 0 && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_VAR)
      {
        int pos = TCCIR_DECODE_VREG_POSITION(vr);
        if (pos >= 0 && pos < UB_ELIDE_MAX_VAR_POS)
          var_addr_taken[pos >> 3] |= (uint8_t)(1u << (pos & 7));
      }
    }
    /* Any VAR appearing as dest is "written" at some point. */
    if (irop_config[q->op].has_dest)
    {
      IROperand dop = tcc_ir_op_get_dest(ir, q);
      int32_t dvr = irop_get_vreg(dop);
      if (dvr >= 0 && TCCIR_DECODE_VREG_TYPE(dvr) == TCCIR_VREG_TYPE_VAR)
      {
        int pos = TCCIR_DECODE_VREG_POSITION(dvr);
        /* Position out of range: conservatively skip — we have no record of it
         * either way, so reads of out-of-range VARs won't be classified as
         * uninit either (see below).  Safe. */
        if (pos >= 0 && pos < UB_ELIDE_MAX_VAR_POS)
          var_written[pos >> 3] |= (uint8_t)(1u << (pos & 7));
      }
    }
  }

  /* Helper: classify a source operand as "tainted by reading uninit VAR". */
#define SRC_IS_UNINIT_READ(sop) ({                                                                                     \
    int _t = 0;                                                                                                        \
    int32_t _vr = irop_get_vreg(sop);                                                                                  \
    if (_vr >= 0 && TCCIR_DECODE_VREG_TYPE(_vr) == TCCIR_VREG_TYPE_VAR && !((sop).is_local && !(sop).is_lval))         \
    {                                                                                                                  \
      int _p = TCCIR_DECODE_VREG_POSITION(_vr);                                                                        \
      if (_p >= 0 && _p < UB_ELIDE_MAX_VAR_POS &&                                                                      \
          !(var_written[_p >> 3] & (uint8_t)(1u << (_p & 7))) &&                                                       \
          !(var_addr_taken[_p >> 3] & (uint8_t)(1u << (_p & 7))))                                                      \
        _t = 1;                                                                                                        \
    }                                                                                                                  \
    _t;                                                                                                                \
  })

#define TEMP_IS_TAINTED(sop) ({                                                                                        \
    int _t = 0;                                                                                                        \
    int32_t _vr = irop_get_vreg(sop);                                                                                  \
    if (_vr >= 0 && TCCIR_DECODE_VREG_TYPE(_vr) == TCCIR_VREG_TYPE_TEMP)                                               \
    {                                                                                                                  \
      int _p = TCCIR_DECODE_VREG_POSITION(_vr);                                                                        \
      if (_p >= 0 && _p < UB_ELIDE_MAX_TEMPS &&                                                                        \
          (temp_tainted[_p >> 3] & (uint8_t)(1u << (_p & 7))))                                                         \
        _t = 1;                                                                                                        \
    }                                                                                                                  \
    _t;                                                                                                                \
  })

  /* Forward fixpoint: propagate taint through TEMP defs.  TEMPs are usually
   * single-def in TCC's IR, so a couple of passes suffice; bound iterations
   * defensively. */
  for (int iter = 0; iter < 16; iter++)
  {
    int changed = 0;
    for (int i = 0; i < n; i++)
    {
      IRQuadCompact *q = &ir->compact_instructions[i];
      if (q->op == TCCIR_OP_NOP)
        continue;
      if (!irop_config[q->op].has_dest)
        continue;
      IROperand dop = tcc_ir_op_get_dest(ir, q);
      int32_t dvr = irop_get_vreg(dop);
      if (dvr < 0)
        continue;
      if (TCCIR_DECODE_VREG_TYPE(dvr) != TCCIR_VREG_TYPE_TEMP)
        continue;
      /* Skip STORE-through-TEMP forms: dest with is_lval means "address",
       * not "this TEMP gets a new value". */
      if (dop.is_lval)
        continue;
      int dpos = TCCIR_DECODE_VREG_POSITION(dvr);
      if (dpos < 0 || dpos >= UB_ELIDE_MAX_TEMPS)
        continue;
      if (temp_tainted[dpos >> 3] & (uint8_t)(1u << (dpos & 7)))
        continue;

      int taint = 0;
      if (irop_config[q->op].has_src1)
      {
        IROperand s = tcc_ir_op_get_src1(ir, q);
        if (SRC_IS_UNINIT_READ(s) || TEMP_IS_TAINTED(s))
          taint = 1;
      }
      if (!taint && irop_config[q->op].has_src2)
      {
        IROperand s = tcc_ir_op_get_src2(ir, q);
        if (SRC_IS_UNINIT_READ(s) || TEMP_IS_TAINTED(s))
          taint = 1;
      }

      if (taint)
      {
        temp_tainted[dpos >> 3] |= (uint8_t)(1u << (dpos & 7));
        changed = 1;
      }
    }
    if (!changed)
      break;
  }

  /* Verify every STORE has a tainted address. */
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_STORE && q->op != TCCIR_OP_STORE_INDEXED && q->op != TCCIR_OP_STORE_POSTINC)
      continue;

    /* Address operand: dest for all three STORE forms (the base pointer). */
    IROperand dop = tcc_ir_op_get_dest(ir, q);
    int32_t dvr = irop_get_vreg(dop);
    int tainted = 0;

    if (dvr >= 0 && TCCIR_DECODE_VREG_TYPE(dvr) == TCCIR_VREG_TYPE_TEMP)
    {
      if (TEMP_IS_TAINTED(dop))
        tainted = 1;
    }
    else if (dvr >= 0 && TCCIR_DECODE_VREG_TYPE(dvr) == TCCIR_VREG_TYPE_VAR && dop.is_lval && !dop.is_local)
    {
      /* Store-through a non-local VAR slot (unusual): if the VAR is uninit,
       * the address is garbage. */
      int p = TCCIR_DECODE_VREG_POSITION(dvr);
      if (p >= 0 && p < UB_ELIDE_MAX_VAR_POS &&
          !(var_written[p >> 3] & (uint8_t)(1u << (p & 7))) &&
          !(var_addr_taken[p >> 3] & (uint8_t)(1u << (p & 7))))
        tainted = 1;
    }

    if (!tainted)
      return 0; /* a STORE has a real, well-defined address — can't elide */
  }

#undef SRC_IS_UNINIT_READ
#undef TEMP_IS_TAINTED
#undef UB_ELIDE_MAX_VAR_POS
#undef UB_ELIDE_MAX_TEMPS

  LOG_IR_GEN("UB-ELIDE: collapsing function body to empty "
             "(every STORE goes through uninit-pointer address — whole-function UB)");

  /* NOP everything — leave codegen to emit a bare prologue + bx lr.  Mirrors
   * useless_function_body's bookkeeping. */
  for (int i = 0; i < n; i++)
  {
    ir->compact_instructions[i].op = TCCIR_OP_NOP;
    ir->compact_instructions[i].is_jump_target = 0;
  }

  ir->ls.dirty_registers = 0;
  ir->ls.dirty_float_registers = 0;
  if (ir->ls.live_regs_by_instruction && ir->ls.live_regs_by_instruction_size > 0)
    memset(ir->ls.live_regs_by_instruction, 0,
           ir->ls.live_regs_by_instruction_size * sizeof(ir->ls.live_regs_by_instruction[0]));
  ir->leaffunc = 1;

  return 1;
}

int tcc_ir_opt_ub_only_body_elide_ex(IROptCtx *ctx) { return tcc_ir_opt_ub_only_body_elide(ctx->ir); }

/* Local-only body elide.
 *
 * Sister of ub_only_body_elide.  Where that pass collapses a void function
 * whose every STORE goes through an uninit pointer (so the only effect is
 * UB), this one collapses a void function whose every effect is confined to
 * the function's own stack frame and the curated set of pure soft-float /
 * long-int helpers.  No caller can observe such a function's work —
 * everything dies on return — so we can legally emit just `bx lr`.
 *
 * Closes the gcc.c-torture compile/991213-1 gap (48 → 1): `void p(t, n) {
 * double s = ...t[n/2]...; for (...) s += 2*...t[i]...; }`.  TCC keeps `s`
 * alive across the loop via __aeabi_dadd, even though `s` itself never
 * escapes; useless_function_body bails on the calls, ub_only_body_elide
 * bails because the STOREs aren't UB.
 *
 * What we allow:
 *   - STOREs to local-pointer TEMPs (LEA of stack-local, propagated via
 *     ASSIGN/ADD/SUB)
 *   - calls to tcc_ir_is_pure_aeabi() helpers
 *   - calls to memmove/memcpy/memset family iff the destination arg
 *     (FUNCPARAMVAL/FUNCPARAMVOID param_idx 0) is also a local-pointer TEMP
 *   - everything else useless_function_body allows
 *
 * What we bail on (same conservative gating as ub_only_body_elide plus the
 * call/store filters above): RETURNVALUE, inline asm, IJUMP, TRAP, setjmp/
 * longjmp, VLA, BLOCK_COPY, init-chain, switch tables, prefetch, volatile
 * sym access, any non-allowlisted FUNCCALL, any STORE whose dest isn't
 * provably local.
 */
int tcc_ir_opt_local_only_body_elide(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  if (n == 0)
    return 0;
  if (!tcc_state || tcc_state->optimize < 2)
    return 0;

  /* Static-chain functions are off-limits in both directions:
   *   - has_static_chain=1: this is a nested function.  Its `StackLoc[N]`
   *     operands encode offsets in the PARENT frame (machine_op.c rewrites
   *     them to chain-relative addresses via R10), so what looks like a
   *     local STORE is actually a write to the parent's stack — externally
   *     observable.  See tests/gcctestsuite execute/20061220-1,
   *     execute/nest-align-1, and tests/ir_tests/nested_capture_*.
   *   - SET_CHAIN present: this is a parent function that exposes its frame
   *     to a nested callee.  The callee can read/write the parent's locals,
   *     so even the parent's `local-only` writes are observable.  The CALL
   *     to the nested function would already trip the non-pure-aeabi guard,
   *     but bail explicitly to keep the contract obvious. */
  if (ir->has_static_chain)
    return 0;

#define LOCAL_ONLY_MAX_MEMMOVE_CALLS 256
#define LOCAL_ONLY_MAX_TEMPS 8192

  /* Pass 0: scan for unconditional bails, record allowed memmove-like
   * calls along with their call_ids for later first-arg verification. */
  int memmove_call_ids[LOCAL_ONLY_MAX_MEMMOVE_CALLS];
  int n_memmove_calls = 0;
  int has_observable_op = 0;

  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;

    switch (q->op)
    {
    /* Hard bails: same set as ub_only_body_elide; these can publish state
     * or do non-local control flow we cannot reason about. */
    case TCCIR_OP_RETURNVALUE:
    case TCCIR_OP_INLINE_ASM:
    case TCCIR_OP_ASM_INPUT:
    case TCCIR_OP_ASM_OUTPUT:
    case TCCIR_OP_TRAP:
    case TCCIR_OP_IJUMP:
    case TCCIR_OP_SETJMP:
    case TCCIR_OP_LONGJMP:
    case TCCIR_OP_NL_SETJMP:
    case TCCIR_OP_NL_LONGJMP:
    case TCCIR_OP_BUILTIN_APPLY_ARGS:
    case TCCIR_OP_BUILTIN_APPLY:
    case TCCIR_OP_BUILTIN_RETURN:
    case TCCIR_OP_VLA_ALLOC:
    case TCCIR_OP_VLA_SP_SAVE:
    case TCCIR_OP_VLA_SP_RESTORE:
    case TCCIR_OP_BLOCK_COPY:
    case TCCIR_OP_INIT_CHAIN_SLOT:
    case TCCIR_OP_SET_CHAIN:
    case TCCIR_OP_PREFETCH:
    case TCCIR_OP_SWITCH_TABLE:
    case TCCIR_OP_SWITCH_LOAD:
      return 0;
    case TCCIR_OP_FUNCCALLVAL:
    case TCCIR_OP_FUNCCALLVOID:
    {
      Sym *callee = irop_get_sym_ex(ir, tcc_ir_op_get_src1(ir, q));
      if (!callee)
        return 0;
      const char *name = get_tok_str(callee->v, NULL);
      if (!name)
        return 0;
      has_observable_op = 1;
      if (tcc_ir_is_pure_aeabi(name))
        break;
      /* memmove/memcpy/memset family: writes through their first arg only.
       * If that destination points into our own stack frame, the writes
       * are unobservable; we'll verify later. */
      int is_memlike = strcmp(name, "__aeabi_memmove4") == 0 || strcmp(name, "__aeabi_memmove8") == 0 ||
                       strcmp(name, "__aeabi_memmove") == 0 || strcmp(name, "__aeabi_memcpy4") == 0 ||
                       strcmp(name, "__aeabi_memcpy8") == 0 || strcmp(name, "__aeabi_memcpy") == 0 ||
                       strcmp(name, "__aeabi_memset") == 0 || strcmp(name, "__aeabi_memset4") == 0 ||
                       strcmp(name, "__aeabi_memset8") == 0 || strcmp(name, "__aeabi_memclr") == 0 ||
                       strcmp(name, "__aeabi_memclr4") == 0 || strcmp(name, "__aeabi_memclr8") == 0 ||
                       strcmp(name, "memmove") == 0 || strcmp(name, "memcpy") == 0 || strcmp(name, "memset") == 0;
      if (!is_memlike)
        return 0;
      if (n_memmove_calls >= LOCAL_ONLY_MAX_MEMMOVE_CALLS)
        return 0;
      IROperand call_id_op = tcc_ir_op_get_src2(ir, q);
      int call_id = TCCIR_DECODE_CALL_ID(irop_get_imm64_ex(ir, call_id_op));
      memmove_call_ids[n_memmove_calls++] = call_id;
      break;
    }
    case TCCIR_OP_STORE:
    case TCCIR_OP_STORE_INDEXED:
    case TCCIR_OP_STORE_POSTINC:
      has_observable_op = 1;
      break;
    default:
      break;
    }

    /* Volatile sym access on any operand keeps the body alive. */
    for (int k = 0; k <= 2; k++)
    {
      IROperand op;
      if (k == 0)
      {
        if (!irop_config[q->op].has_dest)
          continue;
        op = tcc_ir_op_get_dest(ir, q);
      }
      else if (k == 1)
      {
        if (!irop_config[q->op].has_src1)
          continue;
        op = tcc_ir_op_get_src1(ir, q);
      }
      else
      {
        if (!irop_config[q->op].has_src2)
          continue;
        op = tcc_ir_op_get_src2(ir, q);
      }
      if (op.is_sym)
      {
        Sym *sym = irop_get_sym_ex(ir, op);
        if (sym && (sym->type.t & VT_VOLATILE))
          return 0;
      }
    }
  }

  /* useless_function_body already covers the no-call-no-store case and
   * runs after us — leave it alone to avoid double-counting. */
  if (!has_observable_op)
    return 0;

  /* Pass 1: forward fixpoint marking TEMPs that hold a local-stack-frame
   * pointer.  Seed from LEA of any stack-local; propagate through ASSIGN,
   * ADD, SUB (pointer + integer offset stays local). */
  uint8_t local_ptr[(LOCAL_ONLY_MAX_TEMPS + 7) / 8] = {0};

#define LP_GET(p) ((local_ptr[(p) >> 3] & (uint8_t)(1u << ((p) & 7))) != 0)
#define LP_SET(p)                                                                                                      \
  do                                                                                                                   \
  {                                                                                                                    \
    if ((p) >= 0 && (p) < LOCAL_ONLY_MAX_TEMPS)                                                                        \
      local_ptr[(p) >> 3] |= (uint8_t)(1u << ((p) & 7));                                                               \
  } while (0)

#define IS_LOCAL_PTR_OP(sop)                                                                                           \
  ({                                                                                                                   \
    int _r = 0;                                                                                                        \
    int32_t _vr = irop_get_vreg(sop);                                                                                  \
    if (_vr >= 0 && TCCIR_DECODE_VREG_TYPE(_vr) == TCCIR_VREG_TYPE_TEMP)                                               \
    {                                                                                                                  \
      int _p = TCCIR_DECODE_VREG_POSITION(_vr);                                                                        \
      if (_p >= 0 && _p < LOCAL_ONLY_MAX_TEMPS && LP_GET(_p))                                                          \
        _r = 1;                                                                                                        \
    }                                                                                                                  \
    _r;                                                                                                                \
  })

  for (int iter = 0; iter < 16; iter++)
  {
    int changed = 0;
    for (int i = 0; i < n; i++)
    {
      IRQuadCompact *q = &ir->compact_instructions[i];
      if (q->op == TCCIR_OP_NOP)
        continue;
      if (!irop_config[q->op].has_dest)
        continue;
      IROperand dop = tcc_ir_op_get_dest(ir, q);
      int32_t dvr = irop_get_vreg(dop);
      if (dvr < 0 || TCCIR_DECODE_VREG_TYPE(dvr) != TCCIR_VREG_TYPE_TEMP)
        continue;
      if (dop.is_lval) /* STORE-through-TEMP form is not a def of this TEMP */
        continue;
      int dpos = TCCIR_DECODE_VREG_POSITION(dvr);
      if (dpos < 0 || dpos >= LOCAL_ONLY_MAX_TEMPS)
        continue;
      if (LP_GET(dpos))
        continue;

      int set = 0;
      switch (q->op)
      {
      case TCCIR_OP_LEA:
      {
        /* Address of a stack-local (anonymous offset or local VAR) — its
         * lifetime is bounded by the function, so the resulting pointer
         * is local-only.  Reject parameter-area addresses (is_param):
         * those are caller-owned, and writes there can be observed by
         * the caller. */
        IROperand s = tcc_ir_op_get_src1(ir, q);
        if (s.is_param)
          break;
        int tag = irop_get_tag(s);
        if (tag == IROP_TAG_STACKOFF && s.is_local)
          set = 1;
        else
        {
          int32_t svr = irop_get_vreg(s);
          if (svr >= 0 && TCCIR_DECODE_VREG_TYPE(svr) == TCCIR_VREG_TYPE_VAR && s.is_local)
            set = 1;
        }
        break;
      }
      case TCCIR_OP_ASSIGN:
      {
        IROperand s = tcc_ir_op_get_src1(ir, q);
        if (IS_LOCAL_PTR_OP(s))
          set = 1;
        break;
      }
      case TCCIR_OP_ADD:
      case TCCIR_OP_SUB:
      {
        /* local-pointer ± non-pointer is still local-pointer.  We don't
         * track non-pointerness here, but for the purpose of "writes via
         * this address only hit our frame," it suffices that one operand
         * is a local-pointer. */
        IROperand s1 = tcc_ir_op_get_src1(ir, q);
        IROperand s2 = tcc_ir_op_get_src2(ir, q);
        if (IS_LOCAL_PTR_OP(s1) || IS_LOCAL_PTR_OP(s2))
          set = 1;
        break;
      }
      default:
        break;
      }

      if (set)
      {
        LP_SET(dpos);
        changed = 1;
      }
    }
    if (!changed)
      break;
  }

  /* Pass 2a: every STORE/STORE_INDEXED/STORE_POSTINC must write through
   * a local-pointer address. */
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_STORE && q->op != TCCIR_OP_STORE_INDEXED && q->op != TCCIR_OP_STORE_POSTINC)
      continue;
    IROperand dop = tcc_ir_op_get_dest(ir, q);
    int32_t dvr = irop_get_vreg(dop);
    int ok = 0;
    if (dvr >= 0 && TCCIR_DECODE_VREG_TYPE(dvr) == TCCIR_VREG_TYPE_TEMP)
    {
      if (IS_LOCAL_PTR_OP(dop))
        ok = 1;
    }
    else
    {
      /* STACKOFF/local dest (direct stack write) is local-only. */
      int tag = irop_get_tag(dop);
      if (tag == IROP_TAG_STACKOFF && dop.is_local && !dop.is_param)
        ok = 1;
    }
    if (!ok)
      return 0;
  }

  /* Pass 2b: each memmove-like call's first-arg PARAM must be a local
   * pointer (so the writes the callee does land in our frame). */
  for (int j = 0; j < n_memmove_calls; j++)
  {
    int target_call_id = memmove_call_ids[j];
    int verified = 0;
    for (int i = 0; i < n; i++)
    {
      IRQuadCompact *q = &ir->compact_instructions[i];
      if (q->op != TCCIR_OP_FUNCPARAMVAL && q->op != TCCIR_OP_FUNCPARAMVOID)
        continue;
      IROperand src2 = tcc_ir_op_get_src2(ir, q);
      int64_t encoded = irop_get_imm64_ex(ir, src2);
      if (TCCIR_DECODE_CALL_ID(encoded) != target_call_id)
        continue;
      if (TCCIR_DECODE_PARAM_IDX(encoded) != 0)
        continue;
      IROperand val = tcc_ir_op_get_src1(ir, q);
      if (IS_LOCAL_PTR_OP(val))
        verified = 1;
      break;
    }
    if (!verified)
      return 0;
  }

#undef LP_GET
#undef LP_SET
#undef IS_LOCAL_PTR_OP
#undef LOCAL_ONLY_MAX_MEMMOVE_CALLS
#undef LOCAL_ONLY_MAX_TEMPS

  LOG_IR_GEN("LOCAL-ONLY-ELIDE: collapsing function body — every side effect "
             "is confined to the local stack frame (no caller-visible state)");

  for (int i = 0; i < n; i++)
  {
    ir->compact_instructions[i].op = TCCIR_OP_NOP;
    ir->compact_instructions[i].is_jump_target = 0;
  }

  ir->ls.dirty_registers = 0;
  ir->ls.dirty_float_registers = 0;
  if (ir->ls.live_regs_by_instruction && ir->ls.live_regs_by_instruction_size > 0)
    memset(ir->ls.live_regs_by_instruction, 0,
           ir->ls.live_regs_by_instruction_size * sizeof(ir->ls.live_regs_by_instruction[0]));
  ir->leaffunc = 1;

  /* The body referenced P0/P1/... so linear-scan parked them in callee-
   * saved registers (r4+); the prologue's parameter-setup pass would still
   * emit `mov r4, r0; mov r5, r1` based on those allocations.  With every
   * IR op NOPed the params are dead, so clear the allocations.  This
   * mirrors ir->leaffunc=1 + dirty_registers=0 above: tell every consumer
   * that the body needs nothing. */
  for (int p = 0; p < ir->next_parameter; p++)
  {
    IRLiveInterval *iv = &ir->parameters_live_intervals[p];
    iv->allocation.r0 = PREG_NONE;
    iv->allocation.r1 = PREG_NONE;
    iv->allocation.offset = 0;
  }

  return 1;
}

int tcc_ir_opt_local_only_body_elide_ex(IROptCtx *ctx) { return tcc_ir_opt_local_only_body_elide(ctx->ir); }

/* Const-return UB elide.
 *
 * Sister of ub_only_body_elide / local_only_body_elide.  Those passes only
 * fire on void functions whose every STORE is UB / local-confined.  This one
 * collapses a *non-void* function whose entry block executes UB (reads from
 * an untouched local stack slot) before any observable side effect, and
 * whose every RETURNVALUE returns the same constant.
 *
 * Per C11 6.3.2.1 / 4.3, reading an uninitialised auto with never-taken
 * address is UB; once UB has been executed the program's entire behaviour
 * is undefined and the implementation may choose any continuation.  GCC -O2
 * picks "return the constant immediately, skipping the body."  Closes the
 * gcc.c-torture compile/20011109-1 gap (`die`: 140 -> 3): the body's `for
 * (x=0; x < n.e; ...)` reads uninit `n.e` in the loop guard, all paths
 * eventually flow to `return o` where `o=0` was never reassigned, so GCC
 * emits a 3-insn `return 0` and skips every call/store in between.
 *
 * Gating (conservative):
 *   - O2-only.
 *   - Function must have >=1 RETURNVALUE, all with the same constant src
 *     (IMM32 / I64).  RETURNVOID anywhere -> bail (mixed-return ambiguity).
 *   - No inline asm, IJUMP, TRAP, setjmp/longjmp, VLA primitives, SET_CHAIN
 *     / INIT_CHAIN_SLOT, BUILTIN_APPLY*, SWITCH_TABLE / SWITCH_LOAD.
 *   - No volatile sym access anywhere.
 *   - No nested-function context (has_static_chain).
 *   - No LEA of a STACKOFF and no `Addr[StackLoc[...]]` operand anywhere —
 *     if the address of any local escapes, we can't tell if that slot was
 *     written through a pointer, so we can't safely call any STACKOFF read
 *     "uninit".
 *
 * Firing condition (in the entry block, in program order, skipping NOPs):
 *   - We must encounter at least one STACKOFF read (is_lval, is_local,
 *     !is_param) whose offset matches no STORE/STORE_INDEXED/STORE_POSTINC
 *     dest in the function — i.e. an uninit local stack read.
 *   - That uninit read must happen BEFORE any observable side effect
 *     (STORE / FUNCCALL* / BLOCK_COPY / PREFETCH / CALLSEQ_*) in the entry
 *     block — otherwise the side effect was well-defined and we'd lose it.
 *   - Entry block ends at JUMP / JUMPIF / RETURN; if we exit without
 *     finding uninit, bail.
 *
 * Output: NOP every instruction, write a single RETURNVALUE-with-constant
 * at index 0.  Codegen emits a bare prologue + `movs r0, #c; bx lr`. */
int tcc_ir_opt_const_return_uninit_elide(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  if (n == 0)
    return 0;
  if (!tcc_state || tcc_state->optimize < 2)
    return 0;
  if (ir->has_static_chain)
    return 0;

#define CRUE_MAX_STORE_OFFSETS 512
  int store_offsets[CRUE_MAX_STORE_OFFSETS];
  int n_store_offsets = 0;

  IROperand rv_src = IROP_NONE;
  int rv_count = 0;
  int64_t rv_val = 0;
  int rv_const_tag = 0;
  int rv_btype = 0;

  /* Pass 1: hard-bail scan + inventory of STORE dest offsets + RETURNVALUE
   * source validation + Addr[StackLoc]/LEA-of-STACKOFF detection. */
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;

    switch (q->op)
    {
    case TCCIR_OP_INLINE_ASM:
    case TCCIR_OP_ASM_INPUT:
    case TCCIR_OP_ASM_OUTPUT:
    case TCCIR_OP_IJUMP:
    case TCCIR_OP_TRAP:
    case TCCIR_OP_SETJMP:
    case TCCIR_OP_LONGJMP:
    case TCCIR_OP_NL_SETJMP:
    case TCCIR_OP_NL_LONGJMP:
    case TCCIR_OP_VLA_ALLOC:
    case TCCIR_OP_VLA_SP_SAVE:
    case TCCIR_OP_VLA_SP_RESTORE:
    case TCCIR_OP_SET_CHAIN:
    case TCCIR_OP_INIT_CHAIN_SLOT:
    case TCCIR_OP_BUILTIN_APPLY_ARGS:
    case TCCIR_OP_BUILTIN_APPLY:
    case TCCIR_OP_BUILTIN_RETURN:
    case TCCIR_OP_SWITCH_TABLE:
    case TCCIR_OP_SWITCH_LOAD:
    case TCCIR_OP_RETURNVOID:
      return 0;
    case TCCIR_OP_RETURNVALUE:
    {
      IROperand s = tcc_ir_op_get_src1(ir, q);
      int tag = irop_get_tag(s);
      if (tag != IROP_TAG_IMM32 && tag != IROP_TAG_I64)
        return 0;
      int64_t v = irop_get_imm64_ex(ir, s);
      if (rv_count == 0)
      {
        rv_val = v;
        rv_btype = s.btype;
        rv_const_tag = tag;
        rv_src = s;
      }
      else if (v != rv_val || tag != rv_const_tag || s.btype != rv_btype)
        return 0;
      rv_count++;
      break;
    }
    case TCCIR_OP_STORE:
    case TCCIR_OP_STORE_INDEXED:
    case TCCIR_OP_STORE_POSTINC:
    {
      IROperand dop = tcc_ir_op_get_dest(ir, q);
      if (irop_get_tag(dop) == IROP_TAG_STACKOFF && dop.is_local && !dop.is_param)
      {
        int off = (int)irop_get_stack_offset(dop);
        if (n_store_offsets >= CRUE_MAX_STORE_OFFSETS)
          return 0;
        store_offsets[n_store_offsets++] = off;
      }
      break;
    }
    default:
      break;
    }

    /* Scan all operands: bail on Addr[StackLoc] (stack address escapes —
     * any STACKOFF read could be initialized via that alias) and on volatile
     * sym access. */
    for (int k = 0; k <= 2; k++)
    {
      IROperand op;
      if (k == 0)
      {
        if (!irop_config[q->op].has_dest)
          continue;
        op = tcc_ir_op_get_dest(ir, q);
      }
      else if (k == 1)
      {
        if (!irop_config[q->op].has_src1)
          continue;
        op = tcc_ir_op_get_src1(ir, q);
      }
      else
      {
        if (!irop_config[q->op].has_src2)
          continue;
        op = tcc_ir_op_get_src2(ir, q);
      }
      if (irop_get_tag(op) == IROP_TAG_STACKOFF && op.is_local && !op.is_lval && !op.is_param)
        return 0;
      if (op.is_sym)
      {
        Sym *sym = irop_get_sym_ex(ir, op);
        if (sym && (sym->type.t & VT_VOLATILE))
          return 0;
      }
    }

    /* LEA with a STACKOFF source materialises a stack address — same risk
     * as a direct Addr[StackLoc] operand. */
    if (q->op == TCCIR_OP_LEA && irop_config[q->op].has_src1)
    {
      IROperand s = tcc_ir_op_get_src1(ir, q);
      if (irop_get_tag(s) == IROP_TAG_STACKOFF)
        return 0;
    }
  }

  if (rv_count == 0)
    return 0;

  /* Pass 2: walk the linear program prefix from entry looking for the
   * first uninit STACKOFF read vs. the first observable side effect.  We
   * do NOT stop at jump targets — on the first iteration through any loop
   * the back-edge isn't yet taken, so control reaches the loop header via
   * fall-through from entry, and a UB read in the loop header IS executed
   * on entry. */
  int found_uninit = 0;
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;

    /* Check src1/src2 for uninit STACKOFF read. */
    for (int k = 1; k <= 2 && !found_uninit; k++)
    {
      if (k == 1 && !irop_config[q->op].has_src1)
        continue;
      if (k == 2 && !irop_config[q->op].has_src2)
        continue;
      IROperand sop = (k == 1) ? tcc_ir_op_get_src1(ir, q) : tcc_ir_op_get_src2(ir, q);
      if (irop_get_tag(sop) != IROP_TAG_STACKOFF)
        continue;
      if (!sop.is_lval)
        continue;
      if (!sop.is_local)
        continue;
      if (sop.is_param)
        continue;
      /* Spilled vregs (assigned vreg in the STACKOFF operand) carry a
       * value that was defined by an earlier ASSIGN/etc.  The spill code
       * is injected during codegen, not visible as an IR STORE here, so
       * our store-offset table won't list it — yet the read is well
       * defined.  Only treat raw frontend STACKOFFs (vreg == -1) as
       * potentially uninit. */
      if (irop_has_vreg(sop) && irop_get_vreg(sop) >= 0)
        continue;
      int off = (int)irop_get_stack_offset(sop);
      int touched = 0;
      for (int j = 0; j < n_store_offsets; j++)
      {
        if (store_offsets[j] == off)
        {
          touched = 1;
          break;
        }
      }
      if (!touched)
      {
        found_uninit = 1;
        break;
      }
    }

    /* If we hit an observable op before any uninit read, the observable
     * effect would be lost — bail. */
    if (!found_uninit)
    {
      switch (q->op)
      {
      case TCCIR_OP_STORE:
      case TCCIR_OP_STORE_INDEXED:
      case TCCIR_OP_STORE_POSTINC:
      case TCCIR_OP_FUNCCALLVAL:
      case TCCIR_OP_FUNCCALLVOID:
      case TCCIR_OP_FUNCPARAMVAL:
      case TCCIR_OP_FUNCPARAMVOID:
      case TCCIR_OP_CALLSEQ_BEGIN:
      case TCCIR_OP_CALLARG_REG:
      case TCCIR_OP_CALLARG_STACK:
      case TCCIR_OP_CALLSEQ_END:
      case TCCIR_OP_BLOCK_COPY:
      case TCCIR_OP_PREFETCH:
        return 0;
      default:
        break;
      }
    }

    /* Entry-block terminators. */
    if (q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_JUMPIF || q->op == TCCIR_OP_RETURNVALUE ||
        q->op == TCCIR_OP_RETURNVOID)
      break;
  }

#undef CRUE_MAX_STORE_OFFSETS

  if (!found_uninit)
    return 0;

  LOG_IR_GEN("CONST-RETURN-UNINIT-ELIDE: collapsing function to a single "
             "RETURNVALUE constant (entry-block UB read poisons all paths; "
             "every RETURNVALUE returns the same constant)");

  /* NOP everything, then place a single RETURNVALUE-const at index 0. */
  for (int i = 0; i < n; i++)
  {
    ir->compact_instructions[i].op = TCCIR_OP_NOP;
    ir->compact_instructions[i].is_jump_target = 0;
  }

  ir->compact_instructions[0].op = TCCIR_OP_RETURNVALUE;
  tcc_ir_set_dest(ir, 0, IROP_NONE);
  tcc_ir_set_src1(ir, 0, rv_src);
  tcc_ir_set_src2(ir, 0, IROP_NONE);

  ir->ls.dirty_registers = 0;
  ir->ls.dirty_float_registers = 0;
  if (ir->ls.live_regs_by_instruction && ir->ls.live_regs_by_instruction_size > 0)
    memset(ir->ls.live_regs_by_instruction, 0,
           ir->ls.live_regs_by_instruction_size * sizeof(ir->ls.live_regs_by_instruction[0]));
  ir->leaffunc = 1;

  /* Clear param allocations: the body no longer references any param, so
   * the prologue's `mov r4, r0` (etc) parameter-setup should be skipped.
   * Mirrors local_only_body_elide. */
  for (int p = 0; p < ir->next_parameter; p++)
  {
    IRLiveInterval *iv = &ir->parameters_live_intervals[p];
    iv->allocation.r0 = PREG_NONE;
    iv->allocation.r1 = PREG_NONE;
    iv->allocation.offset = 0;
  }

  return 1;
}

int tcc_ir_opt_const_return_uninit_elide_ex(IROptCtx *ctx) { return tcc_ir_opt_const_return_uninit_elide(ctx->ir); }

