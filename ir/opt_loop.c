/*
 *  TCC IR - Loop optimization passes (pre-SSA)
 *
 *  Strength reduction, induction variable analysis, loop unrolling,
 *  loop rotation, decrement-to-zero transform.
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
#include "opt_du.h"
#include "opt_xform.h"
#include "opt_utils.h"
#include "opt_loop_utils.h"


/* ============================================================================
 * Strength Reduction for Multiply (Phase 3 of FUNCTION_CALLS_OPTIMIZATION_PLAN)
 * ============================================================================
 *
 * Transform MUL by constant into shift/add/sub sequences.
 * This reduces instruction latency on ARM where MUL is slower than shifts.
 *
 * Patterns:
 *   x * 2   -> x << 1
 *   x * 3   -> x + (x << 1)
 *   x * 4   -> x << 2
 *   x * 5   -> x + (x << 2)
 *   x * 7   -> (x << 3) - x
 *   x * 8   -> x << 3
 *   x * 9   -> x + (x << 3)
 *   x * 10  -> (x + (x << 2)) << 1
 *
 * For now, we only handle multipliers that can be expressed as:
 *   - Power of 2: use single shift
 *   - 2^n + 1: use add + shift (e.g., x*5 = x + x*4)
 *   - 2^n - 1: use shift + sub (e.g., x*7 = x*8 - x)
 *   - 2^n + 2^m: use two shifts + add
 *
 * Returns: 1 if transformation applied, 0 otherwise
 */


/* Transform a single MUL instruction
 * Returns 1 if transformed, 0 otherwise
 */
int tcc_ir_strength_reduce_mul(TCCIRState *ir, int instr_idx)
{
  IRQuadCompact *q = &ir->compact_instructions[instr_idx];

  if (q->op != TCCIR_OP_MUL)
    return 0;

  IROperand src1 = tcc_ir_op_get_src1(ir, q);
  IROperand src2 = tcc_ir_op_get_src2(ir, q);
  IROperand dest = tcc_ir_op_get_dest(ir, q);

  /* Find the constant operand (if any) */
  IROperand *value_op = NULL;
  int64_t multiplier = 0;

  if (irop_is_immediate(src1))
  {
    multiplier = irop_get_imm64_ex(ir, src1);
    value_op = &src2; /* The variable operand */
  }
  else if (irop_is_immediate(src2))
  {
    multiplier = irop_get_imm64_ex(ir, src2);
    value_op = &src1;
  }
  else
  {
    /* Both operands are variables - can't strength reduce */
    return 0;
  }

  /* Get the vreg for the value being multiplied */
  int32_t value_vreg = irop_get_vreg(*value_op);
  if (value_vreg < 0)
    return 0; /* No vreg - probably a constant expression */

  /* Get the destination vreg */
  int32_t dest_vreg = irop_get_vreg(dest);
  if (dest_vreg < 0)
    return 0;

  int btype = irop_get_btype(*value_op);

  /* Handle special cases */
  if (multiplier == 0)
  {
    /* x * 0 = 0 */
    q->op = TCCIR_OP_ASSIGN;
    IROperand zero = irop_make_imm32(-1, 0, btype);
    tcc_ir_set_src1(ir, instr_idx, zero);
    tcc_ir_set_src2(ir, instr_idx, IROP_NONE);
    LOG_IR_GEN("STRENGTH_RED: x * 0 -> 0 at i=%d", instr_idx);
    return 1;
  }

  if (multiplier == 1)
  {
    /* x * 1 = x (should have been handled by const prop, but be safe) */
    q->op = TCCIR_OP_ASSIGN;
    tcc_ir_set_src1(ir, instr_idx, *value_op);
    tcc_ir_set_src2(ir, instr_idx, IROP_NONE);
    LOG_IR_GEN("STRENGTH_RED: x * 1 -> x at i=%d", instr_idx);
    return 1;
  }

  /* Check for power of 2: x * (2^n) -> x << n */
  int log2_val = is_power_of_2(multiplier);
  if (log2_val >= 0 && log2_val <= 31)
  {
    q->op = TCCIR_OP_SHL;
    IROperand shift_amount = irop_make_imm32(-1, log2_val, btype);
    tcc_ir_set_src1(ir, instr_idx, *value_op);
    tcc_ir_set_src2(ir, instr_idx, shift_amount);
    LOG_IR_GEN("STRENGTH_RED: x * %lld -> x << %d at i=%d", (long long)multiplier, log2_val, instr_idx);
    return 1;
  }

  /* TODO: Multi-instruction patterns (2^n+1, 2^n-1, composite) require
   * inserting new instructions via insert_instr_at. This conflicts with
   * prior IV strength reduction transformations — the instruction indices
   * and liveness info become inconsistent, causing miscompilation.
   * These patterns need a dedicated pre-regalloc insertion mechanism. */

  return 0;
}

/* ============================================================================
 * Induction Variable Strength Reduction
 * ============================================================================
 *
 * This optimization transforms array indexing patterns:
 *   for (i = 0; i < n; i++) sum += arr[i];
 *
 * From: base + i*stride (SHL + ADD every iteration)
 * To:   ptr += stride (single ADD, enabling post-increment addressing)
 *
 * Key insight: Instead of computing the address each iteration, we maintain
 * a pointer that we increment by the stride.
 */

/* Induction-variable strength reduction: the legacy pre-SSA drivers
 * tcc_ir_opt_iv_strength_reduction / _with_loops that lived here have been
 * removed.  The SSA/CFG-era driver ssa_opt_iv_strength_reduction
 * (ir/opt/ssa_opt_loop.c) reuses the shared transform engine
 * iv_strength_reduction_core (ir/opt_loop_utils.c) on synthetic contiguous
 * single-entry IRLoops built from CFG facts, relocated to the regalloc-time flat
 * region after ssa:loop_unroll and before ssa:decrement_to_zero.
 * See docs/plan_legacy_loop_iv_strength_reduction_ssa.md. */

/* Loop unrolling / constant-trip elimination: the legacy pre-SSA driver
 * tcc_ir_opt_loop_unroll that lived here has been removed; the SSA/CFG-era
 * driver ssa_opt_loop_unroll (ir/opt/ssa_opt_loop.c) reuses the shared
 * try_eliminate_loop / try_eliminate_loop_symbolic / try_unroll_loop_ex
 * mutators (ir/opt_loop_utils.c) with CFG/dominator-based candidate detection.
 * See docs/plan_legacy_loop_unroll_ssa.md. */

/* Loop rotation (top-tested → bottom-tested) now lives in the SSA/CFG-era pass
 * ssa_opt_loop_rotate() (ir/opt/ssa_opt_loop.c), which reuses the shared
 * try_rotate_loop() mutator in ir/opt_loop_utils.c with CFG/dominator-based
 * candidate detection.  The legacy pre-SSA driver tcc_ir_opt_loop_rotation()
 * that used to live here has been removed; see
 * docs/plan_legacy_loop_rotation_ssa.md. */

/* Decrement-to-zero: the legacy pre-SSA driver that lived here has been retired.
 * It was inert at its tccgen call site once loop rotation moved to regalloc (the
 * flat range no longer held the rotated bottom-tested + separate-guard shape the
 * transform needs).  The per-loop detection + in-place rewrite engine is kept
 * verbatim as dtz_try_region (ir/opt_loop_utils.c), driven by the CFG/dominator
 * front-end ssa_opt_decrement_to_zero (ir/opt/ssa_opt_loop.c) after ssa:loop_unroll.
 * See docs/plan_legacy_loop_decrement_to_zero_ssa.md. */

/* Pointer-IV exit-value substitution: the legacy pre-SSA driver that lived
 * here has been retired; the per-loop core and the SSA/CFG-era driver
 * (ssa_opt_ptr_iv_exit_subst) live in ir/opt/ssa_opt_loop.c.
 * See docs/plan_legacy_loop_ptr_iv_exit_subst_ssa.md. */

/* Redundant zero-trip entry-guard elimination: the legacy pre-SSA pass that
 * lived here has been retired.  It was sound but inert at its call site once
 * loop rotation moved to ir/regalloc.c (the flat range no longer holds a
 * separate pre-loop guard); SCCP + ssa:branch fold any dead constant guard a
 * rotated loop leaves.  See docs/plan_legacy_loop_guard_elim_ssa.md. */
