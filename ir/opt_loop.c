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

/* Run strength reduction on all MUL instructions in function
 * Returns number of instructions transformed
 */
int tcc_ir_opt_strength_reduction(TCCIRState *ir)
{
  int changes = 0;

  if (ir->next_instruction_index == 0)
    return 0;

  LOG_IR_GEN("=== STRENGTH REDUCTION START ===");

  for (int i = 0; i < ir->next_instruction_index; i++)
  {
    changes += tcc_ir_strength_reduce_mul(ir, i);
  }

  LOG_IR_GEN("=== STRENGTH REDUCTION END: %d multiplies reduced ===", changes);

  return changes;
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

int tcc_ir_opt_iv_strength_reduction(TCCIRState *ir)
{
  if (!ir || ir->next_instruction_index == 0)
    return 0;

  int total = 0;
  for (int iter = 0; iter < 8; iter++)
  {
    IRLoops *loops = tcc_ir_detect_loops(ir);
    if (!loops || loops->num_loops == 0)
    {
      tcc_ir_free_loops(loops);
      break;
    }
    int changes = iv_strength_reduction_core(ir, loops);
    tcc_ir_free_loops(loops);
    total += changes;
    if (changes == 0)
      break;
  }
  return total;
}

int tcc_ir_opt_iv_strength_reduction_with_loops(TCCIRState *ir, IRLoops *loops)
{
  if (!ir || ir->next_instruction_index == 0 || !loops || loops->num_loops == 0)
    return 0;

  LOG_IV_SR("=== IV STRENGTH REDUCTION START (with pre-detected loops) ===");

  int total = iv_strength_reduction_core(ir, loops);
  if (total == 0)
    return 0;

  /* First pass used LICM-provided loops.  If it transformed a loop and
   * broke out early, remaining loops need processing with fresh detection. */
  for (int iter = 0; iter < 7; iter++)
  {
    IRLoops *fresh = tcc_ir_detect_loops(ir);
    if (!fresh || fresh->num_loops == 0)
    {
      tcc_ir_free_loops(fresh);
      break;
    }
    int changes = iv_strength_reduction_core(ir, fresh);
    tcc_ir_free_loops(fresh);
    total += changes;
    if (changes == 0)
      break;
  }
  return total;
}

/* ============================================================================
 * Loop Bound Rematerialization
 * ============================================================================
 *
 * After IV strength reduction, the loop exit test compares the induction
 * pointer against an end-pointer vreg that was hoisted into the preheader:
 *
 *   [preheader]
 *   ASSIGN end_vreg, STACKOFF(base_off)
 *   ADD    end_vreg, end_vreg, #offset   (optional)
 *   ...
 *   [loop body with function calls]
 *   CMP    ptr_vreg, end_vreg
 *   JUMPIF ...
 *
 * Because end_vreg is live across the entire loop (including calls), the
 * register allocator must place it in a callee-saved register (R4-R11),
 * which costs a PUSH/POP pair in the prologue/epilogue.
 *
 * When the end pointer is a simple SP+constant computation, it is cheaper
 * to recompute it just before each CMP use inside the loop.  This shrinks
 * the live range so it no longer crosses calls, allowing a caller-saved
 * register (R0-R3) or a scratch register to be used instead.
 *
 * GCC does exactly this: it emits `ADD r3, sp, #offset` inside the loop
 * rather than keeping the end pointer in a callee-saved register.
 */

/* Maximum number of rematerialization candidates per loop */
#define REMAT_MAX_CANDIDATES 8

int tcc_ir_opt_loop_bound_remat(TCCIRState *ir)
{
  if (!ir || ir->next_instruction_index == 0)
    return 0;

  IRLoops *loops = tcc_ir_detect_loops(ir);
  if (!loops || loops->num_loops == 0)
  {
    tcc_ir_free_loops(loops);
    return 0;
  }

  int total_changes = 0;
  int n = ir->next_instruction_index;

  for (int li = 0; li < loops->num_loops; li++)
  {
    IRLoop *loop = &loops->loops[li];

    /* Only worthwhile if the loop contains function calls — otherwise the
     * end pointer can live in a caller-saved register anyway. */
    int has_calls = 0;
    for (int i = loop->start_idx; i <= loop->end_idx && i < n; i++)
    {
      TccIrOp op = ir->compact_instructions[i].op;
      if (op == TCCIR_OP_FUNCCALLVAL || op == TCCIR_OP_FUNCCALLVOID)
      {
        has_calls = 1;
        break;
      }
    }
    if (!has_calls)
      continue;

    /* Scan preheader for TEMP vregs defined by ASSIGN from STACKOFF,
     * optionally followed by ADD with immediate.  These are candidates
     * for rematerialization. */
    int preheader_end = loop->header_idx; /* exclusive */
    int preheader_start = loop->preheader_idx;
    if (preheader_start < 0)
      continue;

    /* Expand preheader backwards to find the full basic block that flows
     * into the loop header.  The preheader_idx from loop detection is just
     * a single instruction; we need to scan further back for definitions
     * that were inserted before the loop (e.g., by IV strength reduction). */
    for (int i = preheader_start - 1; i >= 0; i--)
    {
      IRQuadCompact *q = &ir->compact_instructions[i];
      /* Stop at jump targets — they start a new basic block */
      if (q->is_jump_target)
        break;
      /* Stop at control flow instructions */
      if (q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_JUMPIF || q->op == TCCIR_OP_IJUMP ||
          q->op == TCCIR_OP_RETURNVALUE)
        break;
      /* Stop at calls — we don't want to look before function calls */
      if (q->op == TCCIR_OP_FUNCCALLVAL || q->op == TCCIR_OP_FUNCCALLVOID)
        break;
      /* Skip NOPs, expand for everything else */
      preheader_start = i;
    }

    /* Collect candidates: TEMP vregs with cheap rematerializable definitions */
    struct
    {
      int vreg;          /* The TEMP vreg */
      int assign_idx;    /* ASSIGN instruction index */
      int add_idx;       /* ADD instruction index (-1 if none) */
      int32_t stack_off; /* Stack offset from ASSIGN's STACKOFF src */
      int32_t add_imm;   /* Immediate from ADD (0 if no ADD) */
      int is_param;      /* is_param flag from STACKOFF operand */
      int is_lval;       /* is_lval flag — 1 for value loads, 0 for address-of */
    } candidates[REMAT_MAX_CANDIDATES];
    int num_candidates = 0;

    for (int i = preheader_start; i < preheader_end && i < n; i++)
    {
      IRQuadCompact *q = &ir->compact_instructions[i];
      if (q->op != TCCIR_OP_ASSIGN)
        continue;

      IROperand dest = tcc_ir_op_get_dest(ir, q);
      int vr = irop_get_vreg(dest);
      if (vr < 0)
        continue;

      /* Must be a TEMP vreg */
      if (TCCIR_DECODE_VREG_TYPE(vr) != TCCIR_VREG_TYPE_TEMP)
        continue;

      IROperand src = tcc_ir_op_get_src1(ir, q);
      if (irop_get_tag(src) != IROP_TAG_STACKOFF)
        continue;

      /* Only rematerialize an address-of-stack computation (`Addr[StackLoc]`,
       * is_lval=0) — the SP-relative *end pointer* this pass targets.  A
       * value LOAD from a stack slot (is_lval=1) is NOT an end pointer: it
       * reads memory whose content can differ from a fresh anonymous-slot
       * load.  In particular a value-load of a named local VAR (is_local=1,
       * carrying a live VAR vreg) is a register/SSA value with no guaranteed
       * physical home at that offset; rematerializing it as a raw
       * `StackLoc[off]` load (vreg=-1) reads uninitialized stack.  (fuzz
       * seed 6214: pre-loop `u8 <= ~cs` test read `u8` from an unwritten
       * StackLoc[0].)  Recomputing a stack ADDRESS, by contrast, is always
       * sound, so keep those. */
      if (src.is_lval)
        continue;

      int32_t stack_off = (int32_t)irop_get_imm64_ex(ir, src);
      int is_param = src.is_param;
      int is_lval = src.is_lval;
      int add_idx = -1;
      int32_t add_imm = 0;

      /* Check if the next non-NOP instruction is ADD tmpN, tmpN, #imm */
      for (int j = i + 1; j < preheader_end && j < n; j++)
      {
        IRQuadCompact *aq = &ir->compact_instructions[j];
        if (aq->op == TCCIR_OP_NOP)
          continue;
        if (aq->op == TCCIR_OP_ADD)
        {
          IROperand adest = tcc_ir_op_get_dest(ir, aq);
          IROperand asrc1 = tcc_ir_op_get_src1(ir, aq);
          IROperand asrc2 = tcc_ir_op_get_src2(ir, aq);
          if (irop_get_vreg(adest) == vr && irop_get_vreg(asrc1) == vr && irop_is_immediate(asrc2))
          {
            add_idx = j;
            add_imm = (int32_t)irop_get_imm64_ex(ir, asrc2);
          }
        }
        break; /* Only check the immediately following non-NOP */
      }

      if (num_candidates < REMAT_MAX_CANDIDATES)
      {
        candidates[num_candidates].vreg = vr;
        candidates[num_candidates].assign_idx = i;
        candidates[num_candidates].add_idx = add_idx;
        candidates[num_candidates].stack_off = stack_off;
        candidates[num_candidates].add_imm = add_imm;
        candidates[num_candidates].is_param = is_param;
        candidates[num_candidates].is_lval = is_lval;
        num_candidates++;
      }
    }

    if (num_candidates == 0)
      continue;

    /* For each candidate, verify it is only used in CMP instructions inside
     * the loop (or in the header guard area).  Count uses. */
    for (int ci = 0; ci < num_candidates; ci++)
    {
      int vr = candidates[ci].vreg;
      int use_count = 0;
      int bad_use = 0;

      /* Collect CMP use sites inside the loop */
      int cmp_indices[4];
      int num_cmp_uses = 0;

      for (int i = 0; i < n; i++)
      {
        IRQuadCompact *q = &ir->compact_instructions[i];
        if (q->op == TCCIR_OP_NOP)
          continue;
        /* Skip the defining instructions */
        if (i == candidates[ci].assign_idx || i == candidates[ci].add_idx)
          continue;

        /* Check if this instruction uses the vreg.  Track whether the use
         * dereferences it (is_lval operand = `*vr`): rematerialization only
         * reproduces the pointer VALUE, so redirecting a `*vr` operand to the
         * remat vreg silently drops the load and compares the address instead
         * of the pointed-to value (turns `*ptr` into `ptr`).  Such uses are not
         * the end-pointer pattern this pass targets, so they disqualify the
         * candidate. */
        int uses_vr = 0, deref_use = 0;
        if (irop_config[q->op].has_src1)
        {
          IROperand s1 = tcc_ir_op_get_src1(ir, q);
          if (irop_get_vreg(s1) == vr)
          {
            uses_vr = 1;
            if (s1.is_lval)
              deref_use = 1;
          }
        }
        if (irop_config[q->op].has_src2)
        {
          IROperand s2 = tcc_ir_op_get_src2(ir, q);
          if (irop_get_vreg(s2) == vr)
          {
            uses_vr = 1;
            if (s2.is_lval)
              deref_use = 1;
          }
        }

        if (!uses_vr)
          continue;

        use_count++;

        /* Must be a CMP that uses the pointer value directly (no deref). */
        if (q->op != TCCIR_OP_CMP || deref_use)
        {
          bad_use = 1;
          break;
        }

        /* Record CMP site if inside or near loop bounds */
        if (i >= preheader_start && i <= loop->end_idx + 2)
        {
          if (num_cmp_uses < 4)
            cmp_indices[num_cmp_uses++] = i;
        }
        else
        {
          bad_use = 1;
          break;
        }
      }

      if (bad_use || use_count == 0 || num_cmp_uses == 0)
        continue;

      /* Rematerialize: insert ASSIGN from combined STACKOFF just before
       * each CMP use, then NOP the preheader definitions.
       *
       * Process CMP sites from last to first so insertion indices remain
       * valid (inserting at a later position doesn't shift earlier ones). */
      int32_t combined_off = candidates[ci].stack_off + candidates[ci].add_imm;
      int remat_shift = 0;

      for (int ui = num_cmp_uses - 1; ui >= 0; ui--)
      {
        int cmp_idx = cmp_indices[ui] + remat_shift;

        /* Allocate a fresh TEMP vreg for each rematerialization site */
        int remat_vreg = tcc_ir_vreg_alloc_temp(ir);
        if (remat_vreg < 0)
          break;

        IROperand remat_dest = irop_make_vreg(remat_vreg, IROP_BTYPE_INT32);
        IROperand remat_src =
            irop_make_stackoff(-1, combined_off, candidates[ci].is_lval, 0, candidates[ci].is_param, IROP_BTYPE_INT32);
        IROperand null_op = {0};

        int inserted = insert_instr_at(ir, cmp_idx, TCCIR_OP_ASSIGN, remat_dest, remat_src, null_op);
        if (inserted < 0)
          break;

        n = ir->next_instruction_index;
        remat_shift++;

        /* Update the CMP (now at cmp_idx+1) to use the new remat vreg */
        IRQuadCompact *cmp_q = &ir->compact_instructions[cmp_idx + 1];
        IROperand cmp_src2 = tcc_ir_op_get_src2(ir, cmp_q);
        if (irop_get_vreg(cmp_src2) == vr)
        {
          IROperand new_src2 = irop_make_vreg(remat_vreg, IROP_BTYPE_INT32);
          tcc_ir_op_set_src2(ir, cmp_q, new_src2);
        }
        else
        {
          /* The vreg might be in src1 */
          IROperand cmp_src1 = tcc_ir_op_get_src1(ir, cmp_q);
          if (irop_get_vreg(cmp_src1) == vr)
          {
            IROperand new_src1 = irop_make_vreg(remat_vreg, IROP_BTYPE_INT32);
            tcc_ir_op_set_src1(ir, cmp_q, new_src1);
          }
        }
      }

      /* NOP the preheader definitions — the vreg is now dead */
      ir->compact_instructions[candidates[ci].assign_idx].op = TCCIR_OP_NOP;
      if (candidates[ci].add_idx >= 0)
        ir->compact_instructions[candidates[ci].add_idx].op = TCCIR_OP_NOP;

      total_changes++;
    }
  }

  tcc_ir_free_loops(loops);
  return total_changes;
}

/* ============================================================================
 * Loop Unrolling - Fully unroll small constant-trip-count loops
 * ============================================================================
 *
 * For loops like: for (i=0; i<5; i++) sum += 5;
 * After unrolling, the loop body is replicated trip_count times and the
 * loop control flow is eliminated.  Subsequent constant propagation can
 * then collapse the result (e.g. 0+5+5+5+5+5 → 25).
 */

/* Try to replace a loop with a closed-form accumulator computation when the
 * limit is symbolic (a vreg).
 *
 * Pattern recognized:
 *   for (i = 0; i < limit; i++) acc += const;
 * Transformed to:
 *   if (0 < limit) acc = init_acc + const * limit; else acc = init_acc;
 * The pre-loop guard already in the IR (CMP iv,limit / JUMPIF >= exit) acts
 * as the runtime test, so we only emit the closed-form body and leave the
 * pre-existing IV initializers and guard intact.
 *
 * Only handles: init_iv == 0, step_iv == 1, exit condition GE/LT/GT/LE
 * (typical signed-counted for/while loops).
 *
 * Returns 1 if transformed, 0 otherwise. */
static int tcc_ir_opt_loop_unroll__timed(TCCIRState *ir);
int tcc_ir_opt_loop_unroll(TCCIRState *ir)
{
  tcc_pass_timing_init();
  if (!tcc_pass_timing_on) return tcc_ir_opt_loop_unroll__timed(ir);
  unsigned long _t = tcc_pass_clk_us();
  int _r = tcc_ir_opt_loop_unroll__timed(ir);
  tcc_pass_timing_add("loop_unroll", tcc_pass_clk_us() - _t);
  return _r;
}
static int tcc_ir_opt_loop_unroll__timed(TCCIRState *ir)
{
  if (!ir || ir->next_instruction_index == 0)
    return 0;

  IRLoops *loops = tcc_ir_detect_loops(ir);
  if (!loops || loops->num_loops == 0)
  {
    tcc_ir_free_loops(loops);
    return 0;
  }

  /* Merge overlapping loops.  A C for-loop often produces two backward
   * edges (header<-latch and increment<-body), creating two detected loops
   * that are really one.  Merge them so the unroller sees a single loop.
   * Mark absorbed loops with start_idx = -1. */
  for (int i = 0; i < loops->num_loops; i++)
  {
    if (loops->loops[i].start_idx < 0)
      continue;
    int merged;
    do
    {
      merged = 0;
      for (int j = 0; j < loops->num_loops; j++)
      {
        if (j == i || loops->loops[j].start_idx < 0)
          continue;
        IRLoop *a = &loops->loops[i];
        IRLoop *b = &loops->loops[j];
        if (a->start_idx <= b->end_idx && b->start_idx <= a->end_idx)
        {
          /* Merge b into a: keep the earlier header (has the CMP) */
          if (b->start_idx < a->start_idx)
          {
            a->header_idx = b->header_idx;
            a->start_idx = b->start_idx;
            a->preheader_idx = b->preheader_idx;
          }
          if (b->end_idx > a->end_idx)
            a->end_idx = b->end_idx;
          /* Rebuild body_instrs for the merged range */
          tcc_free(a->body_instrs);
          int new_size = a->end_idx - a->start_idx + 1;
          a->body_instrs = tcc_mallocz(sizeof(int) * new_size);
          a->body_instrs_capacity = new_size;
          a->num_body_instrs = 0;
          for (int k = a->start_idx; k <= a->end_idx; k++)
            a->body_instrs[a->num_body_instrs++] = k;
          /* Mark b as absorbed */
          b->start_idx = -1;
          merged = 1;
        }
      }
    } while (merged);
  }

  LOG_LOOP_OPT("=== LOOP UNROLL/ELIM START: %d loop(s) detected ===", loops->num_loops);

  int unrolled = 0;
  for (int i = 0; i < loops->num_loops; i++)
  {
    IRLoop *loop = &loops->loops[i];
    if (loop->start_idx < 0)
    {
      LOG_LOOP_OPT("Loop %d: absorbed by merge, skipping", i);
      continue; /* absorbed by merge */
    }

    LOG_LOOP_OPT("Loop %d: header=%d start=%d end=%d preheader=%d", i, loop->header_idx, loop->start_idx, loop->end_idx,
                 loop->preheader_idx);

    /* Dump loop body instructions for debugging */
#ifdef DEBUG_LOOP_OPT
    for (int di = loop->start_idx; di <= loop->end_idx; di++)
    {
      IRQuadCompact *dq = &ir->compact_instructions[di];
      if (dq->op != TCCIR_OP_NOP)
        LOG_LOOP_OPT("[%d] op=%d%s", di, dq->op, di == loop->header_idx ? " (header)" : "");
    }
#endif

    /* Skip loops that have external entries into the body (not to the header).
     * After jump threading, an outer loop's back-edge may jump directly into
     * the inner loop body.  Unrolling would NOP those targets and break the
     * outer loop. */
    int ext_entry = 0;
    for (int j = 0; j < ir->next_instruction_index && !ext_entry; j++)
    {
      if (j >= loop->start_idx && j <= loop->end_idx)
        continue; /* skip instructions inside the loop itself */
      IRQuadCompact *jq = &ir->compact_instructions[j];
      if (jq->op == TCCIR_OP_JUMP || jq->op == TCCIR_OP_JUMPIF)
      {
        IROperand jdest = tcc_ir_op_get_dest(ir, jq);
        int jtarget = (int)irop_get_imm64_ex(ir, jdest);
        /* Target is inside the loop body (not at the header) */
        if (jtarget > loop->start_idx && jtarget <= loop->end_idx)
        {
          LOG_LOOP_OPT("Loop %d: external entry from [%d] to [%d], skipping", i, j, jtarget);
          ext_entry = 1;
        }
      }
    }
    if (ext_entry)
      continue;

    /* Try elimination first (cheaper than unrolling), fall back to symbolic
     * closed-form (for vreg-limit accumulator loops), then unrolling. */
    if (try_eliminate_loop(ir, loop))
      unrolled++;
    else if (try_eliminate_loop_symbolic(ir, loop))
      unrolled++;
    else
      unrolled += try_unroll_loop_ex(ir, loop, loops, i);
  }

  LOG_LOOP_OPT("=== LOOP UNROLL/ELIM END: %d loop(s) processed ===", unrolled);
  tcc_ir_free_loops(loops);
  return unrolled;
}

/* ============================================================================
 * Loop Rotation - Convert top-tested loops to bottom-tested
 * ============================================================================
 *
 * TCC generates for/while loops as top-tested with a 3-block layout:
 *
 *   HEADER:  CMP iv, limit       ; test at top
 *            JUMPIF exit if COND
 *            JUMP body_start     ; skip latch on first iteration
 *   LATCH:   save_iv             ; latch block
 *            iv = save + step
 *            JUMP header         ; back-edge
 *   BODY:    ...work...          ; body block
 *            JUMP latch          ; to latch
 *
 * This causes 3 branches per iteration.  Loop rotation converts this to a
 * bottom-tested do-while with a guard, producing 1 branch per iteration:
 *
 *   GUARD:   CMP iv, limit       ; guard (once)
 *            JUMPIF exit if COND
 *   BODY:    ...work...          ; body (back-edge target)
 *   LATCH:   save_iv             ; latch inlined
 *            iv = save + step
 *            CMP iv, limit       ; tail test
 *            JUMPIF body if !COND ; single back-edge
 *   EXIT:    ...
 */


/* Try to rotate a single loop.  Returns 1 if rotated, 0 otherwise.
 *
 * Expected IR pattern (header_idx..body_end_jmp):
 *   [header_idx]:   CMP iv, limit
 *   [header_idx+1]: JUMPIF exit if COND      (exit_target > end_idx)
 *   [header_idx+2]: JUMP body_start           (body_start > end_idx)
 *   [header_idx+3 .. end_idx-1]: latch instrs (IV save + increment)
 *   [end_idx]:      JUMP header_idx            (back-edge)
 *   [body_start .. body_end]: body instrs
 *   [body_end+1]:   JUMP latch_start           (body→latch) */

static int tcc_ir_opt_loop_rotation__timed(TCCIRState *ir);
int tcc_ir_opt_loop_rotation(TCCIRState *ir)
{
  tcc_pass_timing_init();
  if (!tcc_pass_timing_on) return tcc_ir_opt_loop_rotation__timed(ir);
  unsigned long _t = tcc_pass_clk_us();
  int _r = tcc_ir_opt_loop_rotation__timed(ir);
  tcc_pass_timing_add("loop_rotation", tcc_pass_clk_us() - _t);
  return _r;
}
static int tcc_ir_opt_loop_rotation__timed(TCCIRState *ir)
{
  if (!ir || ir->next_instruction_index == 0)
    return 0;

  int total_rotated = 0;
  for (int pass = 0; pass < 4; pass++)
  {
    IRLoops *loops = tcc_ir_detect_loops(ir);
    if (!loops || loops->num_loops == 0)
    {
      tcc_ir_free_loops(loops);
      break;
    }

    /* Sort smallest-first so inner loops rotate before outer ones */
    qsort(loops->loops, loops->num_loops, sizeof(IRLoop), loop_size_cmp);

    LOG_LOOP_OPT("=== LOOP ROTATION PASS %d: %d loop(s) ===", pass, loops->num_loops);

    int pass_rotated = 0;
    for (int i = 0; i < loops->num_loops; i++)
    {
      IRLoop *loop = &loops->loops[i];
      if (loop->start_idx < 0)
        continue;

      LOG_LOOP_OPT("Rotation: trying loop %d header=%d start=%d end=%d", i, loop->header_idx, loop->start_idx,
                   loop->end_idx);
      int did_rotate = try_rotate_loop(ir, loop);
      LOG_LOOP_OPT("Rotation: loop %d %s", i, did_rotate ? "ROTATED" : "not rotated");
      pass_rotated += did_rotate;
    }

    LOG_LOOP_OPT("=== LOOP ROTATION PASS %d END: %d rotated ===", pass, pass_rotated);
    tcc_ir_free_loops(loops);
    total_rotated += pass_rotated;
    if (pass_rotated == 0)
      break;
  }
  return total_rotated;
}

/* ============================================================================
 * Decrement-to-Zero Loop Transformation
 * ============================================================================
 *
 * Transforms count-up loops whose IV is only used for counting into
 * count-down-to-zero loops.  This enables the backend to fuse the
 * SUB + CMP #0 into a single flag-setting SUBS instruction.
 *
 * Before:  V = 0; ... V = V + 1; CMP V, #limit; JUMPIF <S body
 * After:   V = limit; ... V = V + #-1; CMP V, #0; JUMPIF >S body
 *
 * Requirements:
 * - IV has init=0, step=+1, limit > 0 (constant)
 * - IV has no uses in loop body other than increment + CMP
 * - Back-edge condition is <S (signed less-than)
 */
int tcc_ir_opt_decrement_to_zero(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  int total_changes = 0;

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

    /* Find a simple count-up IV: V = V + 1 in the latch */
    int iv_def_idx = -1;
    int32_t iv_vr = -1;

    for (int i = loop->end_idx; i >= loop->start_idx; i--)
    {
      IRQuadCompact *q = &ir->compact_instructions[i];
      if (q->op == TCCIR_OP_NOP || q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_CMP || q->op == TCCIR_OP_JUMPIF)
        continue;
      if (q->op != TCCIR_OP_ADD)
        break;

      IROperand dest = tcc_ir_op_get_dest(ir, q);
      IROperand src1 = tcc_ir_op_get_src1(ir, q);
      IROperand src2 = tcc_ir_op_get_src2(ir, q);

      int d_vr = irop_get_vreg(dest);
      int s1_vr = irop_get_vreg(src1);
      if (d_vr >= 0 && d_vr == s1_vr && irop_is_immediate(src2) && irop_get_imm64_ex(ir, src2) == 1 &&
          TCCIR_DECODE_VREG_TYPE(d_vr) == TCCIR_VREG_TYPE_VAR)
      {
        iv_def_idx = i;
        iv_vr = d_vr;
        break;
      }
      break;
    }

    if (iv_def_idx < 0)
      continue;

    /* Find the back-edge CMP: CMP V, #limit; JUMPIF <S body */
    int be_cmp_idx = -1;
    int be_jmpif_idx = -1;
    int limit_val = 0;

    for (int i = loop->end_idx; i >= loop->end_idx - 5 && i >= 0; i--)
    {
      IRQuadCompact *q = &ir->compact_instructions[i];
      if (q->op != TCCIR_OP_CMP)
        continue;

      IROperand s1 = tcc_ir_op_get_src1(ir, q);
      IROperand s2 = tcc_ir_op_get_src2(ir, q);
      if (irop_get_vreg(s1) != iv_vr || !irop_is_immediate(s2))
        continue;

      int jq_idx = i + 1;
      while (jq_idx < n && ir->compact_instructions[jq_idx].op == TCCIR_OP_NOP)
        jq_idx++;
      if (jq_idx >= n || ir->compact_instructions[jq_idx].op != TCCIR_OP_JUMPIF)
        continue;

      IROperand cond = tcc_ir_op_get_src1(ir, &ir->compact_instructions[jq_idx]);
      int cond_tok = (int)irop_get_imm64_ex(ir, cond);
      if (cond_tok != 0x9c) /* TOK_LT (<S) */
        continue;

      limit_val = (int)irop_get_imm64_ex(ir, s2);
      if (limit_val <= 0)
        continue;

      be_cmp_idx = i;
      be_jmpif_idx = jq_idx;
      break;
    }

    if (be_cmp_idx < 0)
      continue;

    /* Find the IV init: V = #0 in the preheader */
    int init_idx = -1;
    for (int i = loop->preheader_idx; i >= 0 && i >= loop->preheader_idx - 5; i--)
    {
      IRQuadCompact *q = &ir->compact_instructions[i];
      if (q->op == TCCIR_OP_NOP)
        continue;
      if (q->op != TCCIR_OP_ASSIGN)
        continue;

      IROperand dest = tcc_ir_op_get_dest(ir, q);
      IROperand src1 = tcc_ir_op_get_src1(ir, q);
      if (irop_get_vreg(dest) == iv_vr && irop_is_immediate(src1) && irop_get_imm64_ex(ir, src1) == 0)
      {
        init_idx = i;
        break;
      }
    }

    if (init_idx < 0)
      continue;

    /* Find pre-test guard: CMP V, #limit near header.
     * We'll NOP it since limit > 0 means the loop always executes. */
    int hdr_cmp_idx = -1, hdr_jmpif_idx = -1;
    {
      int scan_start = loop->preheader_idx;
      if (scan_start < 0)
        scan_start = 0;
      for (int i = scan_start; i <= loop->header_idx + 2 && i < n; i++)
      {
        IRQuadCompact *q = &ir->compact_instructions[i];
        if (q->op != TCCIR_OP_CMP)
          continue;
        /* A bottom-tested / rotated loop with no pre-test guard distinct from
         * its own back-edge CMP/JUMPIF exposes the back-edge test itself inside
         * this header-window scan.  Never accept it as the "pre-test guard":
         * the apply step below rewrites be_cmp_idx/be_jmpif_idx (CMP #0, != 0)
         * and then unconditionally NOPs hdr_cmp_idx/hdr_jmpif_idx.  If those
         * coincide, step 5 would delete the loop's only remaining back-edge
         * test, degenerating the loop to a single iteration.  Skipping it here
         * leaves hdr_cmp_idx == -1, so the transform bails at the guard below.
         * See docs/bugs.md #12. */
        if (i == be_cmp_idx)
          continue;
        IROperand s1 = tcc_ir_op_get_src1(ir, q);
        if (irop_get_vreg(s1) != iv_vr)
          continue;

        int jq_idx = i + 1;
        while (jq_idx < n && (ir->compact_instructions[jq_idx].op == TCCIR_OP_NOP ||
                              ir->compact_instructions[jq_idx].op == TCCIR_OP_ASSIGN))
          jq_idx++;
        if (jq_idx < n && ir->compact_instructions[jq_idx].op == TCCIR_OP_JUMPIF)
        {
          if (jq_idx == be_jmpif_idx)
            continue; /* same guard-coincidence hazard as above */
          hdr_cmp_idx = i;
          hdr_jmpif_idx = jq_idx;
          break;
        }
      }
    }

    /* Verify IV has NO uses anywhere besides: init, increment, back-edge
     * CMP, pre-test CMP, and copy-through temp (T=V before V=T+step,
     * but ONLY if T is unused outside the increment). */
    {
      int other_uses = 0;
      int copy_through_vr = -1;

      /* Find the copy-through temp if it exists */
      for (int k = iv_def_idx - 1; k >= iv_def_idx - 3 && k >= 0; k--)
      {
        IRQuadCompact *q = &ir->compact_instructions[k];
        if (q->op == TCCIR_OP_NOP)
          continue;
        if (q->op == TCCIR_OP_ASSIGN)
        {
          IROperand s = tcc_ir_op_get_src1(ir, q);
          if (irop_get_vreg(s) == iv_vr)
          {
            IROperand d = tcc_ir_op_get_dest(ir, q);
            copy_through_vr = irop_get_vreg(d);
          }
        }
        break;
      }

      /* Check if the copy-through temp is used outside the increment */
      if (copy_through_vr >= 0)
      {
        for (int i = 0; i < n; i++)
        {
          IRQuadCompact *q = &ir->compact_instructions[i];
          if (q->op == TCCIR_OP_NOP || i == iv_def_idx)
            continue;
          if (irop_config[q->op].has_src1 && irop_get_vreg(tcc_ir_op_get_src1(ir, q)) == copy_through_vr)
          {
            other_uses++;
            break;
          }
          if (irop_config[q->op].has_src2 && irop_get_vreg(tcc_ir_op_get_src2(ir, q)) == copy_through_vr)
          {
            other_uses++;
            break;
          }
        }
      }

      /* Find the extent of the IV's live range: from init_idx to the next
       * redefinition of iv_vr after the loop (exclusive).  Uses of iv_vr
       * after a redefinition belong to a different live range. */
      int live_end = n;
      for (int i = loop->end_idx + 1; i < n; i++)
      {
        IRQuadCompact *q = &ir->compact_instructions[i];
        if (q->op == TCCIR_OP_NOP)
          continue;
        if (irop_config[q->op].has_dest)
        {
          IROperand d = tcc_ir_op_get_dest(ir, q);
          if (irop_get_vreg(d) == iv_vr && !irop_op_is_lval(d))
          {
            live_end = i;
            break;
          }
        }
      }

      /* Check uses of IV within its live range */
      for (int i = 0; i < live_end && other_uses == 0; i++)
      {
        IRQuadCompact *q = &ir->compact_instructions[i];
        if (q->op == TCCIR_OP_NOP)
          continue;
        if (i == init_idx || i == iv_def_idx || i == be_cmp_idx || i == hdr_cmp_idx)
          continue;

        /* Allow copy-through temp only if it has no external uses */
        if (copy_through_vr >= 0 && q->op == TCCIR_OP_ASSIGN && i >= iv_def_idx - 3 && i < iv_def_idx)
        {
          IROperand s = tcc_ir_op_get_src1(ir, q);
          if (irop_get_vreg(s) == iv_vr)
            continue;
        }

        if (irop_config[q->op].has_src1 && irop_get_vreg(tcc_ir_op_get_src1(ir, q)) == iv_vr)
          other_uses++;
        if (irop_config[q->op].has_src2 && irop_get_vreg(tcc_ir_op_get_src2(ir, q)) == iv_vr)
          other_uses++;
      }

      if (other_uses > 0)
        continue;
    }

    /* Must have found the pre-test guard — the transform changes the init
     * value, which would make an unfound/unpatched guard skip the loop. */
    if (hdr_cmp_idx < 0)
      continue;

    /* === Apply transformation === */

    /* 1. Init: V = #0  →  V = #limit */
    {
      IRQuadCompact *q = &ir->compact_instructions[init_idx];
      IROperand new_init = irop_make_imm32(-1, limit_val, IROP_BTYPE_INT32);
      tcc_ir_op_set_src1(ir, q, new_init);
    }

    /* 2. Increment: V = V + 1  →  V = V - 1 */
    {
      IRQuadCompact *q = &ir->compact_instructions[iv_def_idx];
      q->op = TCCIR_OP_SUB;
      IROperand new_step = irop_make_imm32(-1, 1, IROP_BTYPE_INT32);
      tcc_ir_op_set_src2(ir, q, new_step);
    }

    /* 3. Back-edge: CMP V, #limit  →  CMP V, #0 */
    {
      IRQuadCompact *q = &ir->compact_instructions[be_cmp_idx];
      IROperand zero = irop_make_imm32(-1, 0, IROP_BTYPE_INT32);
      tcc_ir_op_set_src2(ir, q, zero);
    }

    /* 4. Back-edge condition: <S  →  != (not-equal zero)
     * Using != instead of >S because the codegen peephole can fuse
     * SUB + CMP #0 only for EQ/NE conditions (Z flag only). */
    {
      IRQuadCompact *q = &ir->compact_instructions[be_jmpif_idx];
      IROperand new_cond = irop_make_imm32(-1, 0x95, IROP_BTYPE_INT32); /* TOK_NE (!=) */
      tcc_ir_op_set_src1(ir, q, new_cond);
    }

    /* 5. NOP the pre-test guard (always passes since limit > 0) */
    if (hdr_cmp_idx >= 0)
    {
      ir->compact_instructions[hdr_cmp_idx].op = TCCIR_OP_NOP;
      ir->compact_instructions[hdr_jmpif_idx].op = TCCIR_OP_NOP;
    }

    total_changes++;
  }

  tcc_ir_free_loops(loops);
  return total_changes;
}

/* ============================================================================
 * Pointer-IV Exit-Value Substitution
 * ============================================================================
 *
 * For loops with constant trip count, replace post-loop uses of pointer
 * induction variables with their closed-form exit value.
 *
 * Pattern recognized:
 *   preheader: V = Addr[StackLoc[X]]    (pointer IV init)
 *   loop body: V = V + step             (pure linear step)
 *              (or copy-through: T = V; V = T + step)
 *   exit:      ...CMP V, Addr[StackLoc[Y]]...    (post-loop use)
 *
 * If trip_count is statically known to be N (from a counter IV with constant
 * bounds in the same loop), V's exit value is `Addr[StackLoc[X + step*N]]`.
 * Substitute that operand directly in post-loop instructions; cmp_stack_addr_fold
 * (run immediately after) collapses the comparison when it becomes
 * `CMP Addr[StackLoc[K]], Addr[StackLoc[K]]`.
 *
 * Why this matters: idiomatic post-loop checks like `if (p != &a[N]) abort();`
 * (e.g. pr49644.c) become dead, exposing the abort() branch as unreachable and
 * eventually letting DCE/dead-store kill the loop body.
 *
 * Conservative scope:
 *   - Trip count must be statically known > 0 (so the IV is actually stepped).
 *   - V must have a unique self-add inside the loop (no other defs).
 *   - The preheader-side def of V must be `V = Addr[StackLoc[X]]` reachable
 *     from preheader_idx without an intervening def of V or jump_target merge.
 *   - V's btype must be 32-bit (where stack offsets live).
 *   - Substitute only into instructions reachable from the loop's exit_target
 *     and dominated by it (we approximate: walk forward from exit_target, stop
 *     at any redefinition of V, any jump backward, or any jump target reached
 *     from an external source — for safety we bail on any is_jump_target seen
 *     after the first instruction).
 */

typedef struct PtrIV
{
  int32_t vreg;       /* the pointer VAR */
  int32_t init_off;   /* StackLoc offset at preheader */
  int     step;       /* increment per iteration */
  int     init_idx;   /* index of the init ASSIGN */
  int     def_idx;    /* index of the in-loop self-add */
  int     is_llocal;  /* preserve llocal flag from init */
  int     is_param;   /* preserve param flag from init */
  int     btype;
} PtrIV;

#define PTRIV_MAX 8

/* Try to recognize an instruction range inside `loop` as a pointer-IV self-add
 * pattern; if so, populate `*out` and return 1.  Handles both shapes:
 *   direct:        V = V + #step
 *   copy-through:  T = V;  V = T + #step
 */
static int ptr_iv_find_loop_step(TCCIRState *ir, IRLoop *loop, int instr_idx,
                                 int32_t *out_vreg, int *out_step)
{
  IRQuadCompact *q = &ir->compact_instructions[instr_idx];
  if (q->op != TCCIR_OP_ADD)
    return 0;
  IROperand dest = tcc_ir_op_get_dest(ir, q);
  IROperand src1 = tcc_ir_op_get_src1(ir, q);
  IROperand src2 = tcc_ir_op_get_src2(ir, q);
  int32_t d_vr = irop_get_vreg(dest);
  int32_t s1_vr = irop_get_vreg(src1);
  if (d_vr < 0 || TCCIR_DECODE_VREG_TYPE(d_vr) != TCCIR_VREG_TYPE_VAR)
    return 0;
  if (!irop_is_immediate(src2))
    return 0;
  int step = (int)irop_get_imm64_ex(ir, src2);
  if (step == 0)
    return 0;

  /* Direct pattern: dest_vr == src1_vr */
  if (s1_vr == d_vr) {
    *out_vreg = d_vr;
    *out_step = step;
    return 1;
  }

  /* Copy-through: scan back a few NOP-skipped instructions for `T = V`. */
  for (int k = instr_idx - 1; k >= loop->start_idx && k >= instr_idx - 3; k--) {
    IRQuadCompact *aq = &ir->compact_instructions[k];
    if (aq->op == TCCIR_OP_NOP)
      continue;
    if (aq->op != TCCIR_OP_ASSIGN)
      return 0;
    IROperand adest = tcc_ir_op_get_dest(ir, aq);
    IROperand asrc = tcc_ir_op_get_src1(ir, aq);
    if (irop_get_vreg(adest) == s1_vr && irop_get_vreg(asrc) == d_vr) {
      *out_vreg = d_vr;
      *out_step = step;
      return 1;
    }
    return 0;
  }
  return 0;
}

/* Walk back from preheader_idx looking for an unconditional def of `vreg`
 * of the form `vreg <- Addr[StackLoc[X]]`.  Returns 1 on success and writes
 * the offset/flags/init index. */
static int ptr_iv_find_init(TCCIRState *ir, int vreg, int preheader_idx,
                            int *out_off, int *out_is_llocal, int *out_is_param,
                            int *out_init_idx, int *out_btype)
{
  for (int j = preheader_idx; j >= 0; j--) {
    IRQuadCompact *q = &ir->compact_instructions[j];
    if (q->op == TCCIR_OP_NOP)
      continue;
    /* Stop at any jump target / merge point before the preheader. */
    if (j < preheader_idx && q->is_jump_target)
      return 0;
    /* Stop at any other def of vreg (we want the most recent). */
    if (!irop_config[q->op].has_dest)
      continue;
    IROperand dest = tcc_ir_op_get_dest(ir, q);
    if (irop_get_vreg(dest) != vreg)
      continue;
    /* STOREs through a vreg-deref do not redefine vreg itself. */
    if (q->op == TCCIR_OP_STORE && dest.is_lval && !dest.is_local)
      continue;
    if (q->op != TCCIR_OP_ASSIGN)
      return 0;
    IROperand src1 = tcc_ir_op_get_src1(ir, q);
    if (irop_get_tag(src1) != IROP_TAG_STACKOFF || src1.is_lval || !src1.is_local)
      return 0;
    *out_off = (int)irop_get_imm64_ex(ir, src1);
    *out_is_llocal = src1.is_llocal;
    *out_is_param = src1.is_param;
    *out_init_idx = j;
    *out_btype = irop_get_btype(src1);
    return 1;
  }
  return 0;
}

/* Verify vreg has exactly one def in [loop.start..loop.end] (the self-add at
 * def_idx) and no other write that could perturb its value. */
static int ptr_iv_unique_loop_def(TCCIRState *ir, IRLoop *loop, int vreg, int def_idx)
{
  for (int j = loop->start_idx; j <= loop->end_idx; j++) {
    IRQuadCompact *q = &ir->compact_instructions[j];
    if (q->op == TCCIR_OP_NOP)
      continue;
    if (!irop_config[q->op].has_dest)
      continue;
    IROperand dest = tcc_ir_op_get_dest(ir, q);
    if (irop_get_vreg(dest) != vreg)
      continue;
    /* STORE through V's deref doesn't redefine V. */
    if (q->op == TCCIR_OP_STORE && dest.is_lval && !dest.is_local)
      continue;
    if (j != def_idx)
      return 0;
  }
  return 1;
}

/* Replace reads of VAR `vreg`'s value with `repl` in the instruction at
 * `idx`.  Returns the number of substitutions performed (0, 1, or 2).
 *
 * A VAR value-read encodes as `tag=STACKOFF, vreg=V, is_lval=1, is_local=1`
 * — the is_lval bit here means "load V from its spill home", not "deref
 * through V" (that form uses tag=VREG instead).  Only the STACKOFF form is
 * a true VAR value-read; we restrict substitution to it. */
static int ptr_iv_subst_uses_in_instr(TCCIRState *ir, int idx, int vreg, IROperand repl)
{
  IRQuadCompact *q = &ir->compact_instructions[idx];
  int subs = 0;
  if (irop_config[q->op].has_src1) {
    IROperand s = tcc_ir_op_get_src1(ir, q);
    if (irop_get_vreg(s) == vreg && irop_get_tag(s) == IROP_TAG_STACKOFF) {
      tcc_ir_op_set_src1(ir, q, repl);
      subs++;
    }
  }
  if (irop_config[q->op].has_src2) {
    IROperand s = tcc_ir_op_get_src2(ir, q);
    if (irop_get_vreg(s) == vreg && irop_get_tag(s) == IROP_TAG_STACKOFF) {
      tcc_ir_op_set_src2(ir, q, repl);
      subs++;
    }
  }
  return subs;
}

int tcc_ir_opt_loop_ptr_iv_exit_subst(TCCIRState *ir)
{
  if (!ir || ir->next_instruction_index == 0)
    return 0;

  IRLoops *loops = tcc_ir_detect_loops(ir);
  if (!loops || loops->num_loops == 0) {
    tcc_ir_free_loops(loops);
    return 0;
  }

  int total = 0;
  LOG_LOOP_OPT("[PTR_IV_SUBST] entered, %d loop(s)", loops->num_loops);

  for (int li = 0; li < loops->num_loops; li++) {
    IRLoop *loop = &loops->loops[li];
    if (loop->start_idx < 0)
      continue;

    /* Need a counter IV with known trip count to anchor exit-value computation. */
    InductionVar ivs[MAX_IV];
    int num_ivs = find_induction_vars_ex(ir, loop, ivs, MAX_IV, 1);
    int cmp_idx, jmpif_idx, limit, cond, exit_target;
    InductionVar *primary = NULL;
    for (int k = 0; k < num_ivs; k++) {
      if (find_loop_exit_condition(ir, loop, ivs[k].vreg, &cmp_idx, &jmpif_idx,
                                   &limit, &cond, &exit_target)) {
        primary = &ivs[k];
        break;
      }
    }
    if (!primary)
      continue;
    int trip_count = compute_trip_count(primary->init_val, limit, primary->step, cond);
    if (trip_count <= 0)
      continue;

    /* Scan loop body for pointer IVs. */
    PtrIV pivs[PTRIV_MAX];
    int n_pivs = 0;
    for (int j = loop->start_idx; j <= loop->end_idx && n_pivs < PTRIV_MAX; j++) {
      int32_t v_vr;
      int v_step;
      if (!ptr_iv_find_loop_step(ir, loop, j, &v_vr, &v_step))
        continue;
      /* Skip the counter IV — it's an integer IV, not a pointer one. */
      int is_counter = 0;
      for (int k = 0; k < num_ivs; k++) {
        if (ivs[k].vreg == v_vr) { is_counter = 1; break; }
      }
      if (is_counter)
        continue;

      /* Must have exactly one def in the loop (the self-add at j). */
      if (!ptr_iv_unique_loop_def(ir, loop, v_vr, j))
        continue;

      /* Find preheader init `V = Addr[StackLoc[off]]`. */
      int init_off, is_llocal, is_param, init_idx, btype;
      if (!ptr_iv_find_init(ir, v_vr, loop->preheader_idx,
                            &init_off, &is_llocal, &is_param, &init_idx, &btype))
        continue;

      /* Stack offsets are 32-bit; reject anything that won't fit. */
      int64_t final_off64 = (int64_t)init_off + (int64_t)v_step * (int64_t)trip_count;
      if (final_off64 != (int32_t)final_off64)
        continue;

      pivs[n_pivs].vreg      = v_vr;
      pivs[n_pivs].init_off  = init_off;
      pivs[n_pivs].step      = v_step;
      pivs[n_pivs].init_idx  = init_idx;
      pivs[n_pivs].def_idx   = j;
      pivs[n_pivs].is_llocal = is_llocal;
      pivs[n_pivs].is_param  = is_param;
      pivs[n_pivs].btype     = btype;
      n_pivs++;
    }

    if (n_pivs == 0)
      continue;

    /* NOP the pre-loop entry guard if present.  In rotated loop layout, the
     * guard is `CMP iv, #limit; JUMPIF skip_loop` between the primary IV
     * init and loop->start_idx.  Since trip_count > 0, the guard never
     * fires, so dropping it removes the stale is_jump_target on exit_target
     * — letting our forward-walk substitute V freely.  Mirror the NOP logic
     * in try_eliminate_loop (opt_loop_utils.c).
     *
     * Only run this when we have at least one pointer IV to substitute, so
     * the side effect is proportional to the gain. */
    for (int g = primary->init_idx + 1; g < loop->start_idx; g++) {
      IRQuadCompact *gq = &ir->compact_instructions[g];
      if (gq->op != TCCIR_OP_CMP)
        continue;
      IROperand gs1 = tcc_ir_op_get_src1(ir, gq);
      if (irop_get_vreg(gs1) != primary->vreg)
        continue;
      if (g + 1 >= loop->start_idx)
        break;
      IRQuadCompact *gjq = &ir->compact_instructions[g + 1];
      if (gjq->op != TCCIR_OP_JUMPIF)
        continue;
      IROperand gjd = tcc_ir_op_get_dest(ir, gjq);
      int gjt = (int)irop_get_imm64_ex(ir, gjd);
      /* Only NOP a guard whose JUMPIF target is past the loop body —
       * matches the rotated-loop entry-guard shape. */
      if (gjt < loop->end_idx)
        continue;
      gq->op = TCCIR_OP_NOP;
      gjq->op = TCCIR_OP_NOP;
      /* The guard was the only outside edge into its target; clear the
       * stale is_jump_target so our forward-walk doesn't bail thinking the
       * target receives V via an alternate path. */
      if (gjt >= 0 && gjt < ir->next_instruction_index) {
        int has_other_in_edge = 0;
        for (int s = 0; s < ir->next_instruction_index && !has_other_in_edge; s++) {
          if (s == g + 1) continue;
          IRQuadCompact *sq = &ir->compact_instructions[s];
          if (sq->op != TCCIR_OP_JUMP && sq->op != TCCIR_OP_JUMPIF)
            continue;
          IROperand sd = tcc_ir_op_get_dest(ir, sq);
          int st = (int)irop_get_imm64_ex(ir, sd);
          if (st == gjt)
            has_other_in_edge = 1;
        }
        if (!has_other_in_edge)
          ir->compact_instructions[gjt].is_jump_target = 0;
      }
    }

    /* Walk forward from exit_target, substituting V → Addr[StackLoc[final_off]]
     * in non-lval uses.  Stop scanning V on:
     *   - any redef of V (subsequent uses see a different value)
     *   - reaching the function end
     *   - a backward jump (would loop us back into V's old domain)
     * Conservatively stop ALL substitutions for a vreg at any is_jump_target
     * reached after the exit_target, since the merge could see a different V
     * via an alternate path. */
    int live[PTRIV_MAX];
    for (int p = 0; p < n_pivs; p++) live[p] = 1;

    int n = ir->next_instruction_index;
    for (int j = exit_target; j < n; j++) {
      IRQuadCompact *q = &ir->compact_instructions[j];
      if (q->op == TCCIR_OP_NOP)
        continue;

      /* On merge points after exit_target, conservatively retire any IV that
       * could be reached via an alternate edge. */
      if (j > exit_target && q->is_jump_target) {
        for (int p = 0; p < n_pivs; p++) live[p] = 0;
      }

      int any_live = 0;
      for (int p = 0; p < n_pivs; p++) if (live[p]) { any_live = 1; break; }
      if (!any_live)
        break;

      /* Substitute uses first (reads), then check for redef. */
      for (int p = 0; p < n_pivs; p++) {
        if (!live[p]) continue;
        int32_t final_off = (int32_t)((int64_t)pivs[p].init_off +
                                      (int64_t)pivs[p].step * (int64_t)trip_count);
        IROperand repl = irop_make_stackoff(-1, final_off, /*is_lval*/ 0,
                                            pivs[p].is_llocal, pivs[p].is_param,
                                            pivs[p].btype);
        total += ptr_iv_subst_uses_in_instr(ir, j, pivs[p].vreg, repl);
      }

      /* Now check whether this instruction redefines any tracked V. */
      if (irop_config[q->op].has_dest) {
        IROperand dest = tcc_ir_op_get_dest(ir, q);
        /* STOREs through a vreg-deref don't redefine the vreg itself. */
        if (!(q->op == TCCIR_OP_STORE && dest.is_lval && !dest.is_local)) {
          int32_t dvr = irop_get_vreg(dest);
          if (dvr >= 0) {
            for (int p = 0; p < n_pivs; p++) {
              if (live[p] && pivs[p].vreg == dvr)
                live[p] = 0;
            }
          }
        }
      }

      /* Backward JUMP: bail on all remaining tracked IVs. */
      if (q->op == TCCIR_OP_JUMP) {
        IROperand jd = tcc_ir_op_get_dest(ir, q);
        int t = (int)irop_get_imm64_ex(ir, jd);
        if (t <= j) {
          for (int p = 0; p < n_pivs; p++) live[p] = 0;
        }
      }
    }
  }

  tcc_ir_free_loops(loops);
  return total;
}

/* ------------------------------------------------------------------------
 * Redundant zero-trip entry-guard elimination via loop-exit-value carry.
 *
 * memclr-style code chains sequential counted loops over a shared counter i:
 *   for (i = 0; i < A; i++) ...        // immediate init i = 0
 *   for (;      i < B; i++) ...        // i carries in = A  (loop1 exit value)
 *   for (;      i < C; i++) ...        // i carries in = B  (loop2 exit value)
 * After loop rotation every loop gets a pre-loop zero-trip guard
 *   CMP i,#lim ; JUMPIF (i >= lim) skip_loop
 * The first guard folds (i = 0 is an immediate init), but the 2nd/3rd survive:
 * find_induction_vars_ex only sees an IV whose preheader init is an immediate,
 * so it never learns that i equals the *previous loop's exit value* — the trip
 * count is uncomputable and the guard stays, costing ~2 insns/loop vs GCC.
 *
 * This pass walks loops in program order, tracking the IV vreg's known
 * constant value.  For a loop whose entry value (immediate init OR carried
 * exit value of the preceding loop) and guard limit are constants and whose
 * guard is provably never taken, it NOPs the guard.  When the loop is a
 * single-exit counted loop it records the constant exit value
 * (entry + trip_count*step) for the next loop reusing the same vreg.
 *
 * Soundness: removing the guard depends ONLY on the entry value at the guard
 * (a value established *before* the loop body) making the guard predicate
 * false — independent of the loop body.  A carried value is consumed only when
 * (a) the source loop has a single exit (so completion implies i == final),
 * (b) the source loop's own guard was absent/removed (so its exit_target is
 * reached only via completion, never via a guard-skip with i == entry), and
 * (c) nothing redefines i and no foreign edge merges in between (clean check).
 * Gated off whole-function on un-enumerable control flow (IJUMP/SWITCH_*).
 * Default-ON at -O1+; disable with TCC_NO_GUARD_ELIM=1.
 * ---------------------------------------------------------------------- */

/* Evaluate "a <cond> b" for a JUMPIF condition token.  1 = taken, 0 = not
 * taken, -1 = condition not understood. */
static int guard_eval_cond(int a, int cond, int b)
{
  switch (cond)
  {
  case TOK_EQ:  return a == b;
  case TOK_NE:  return a != b;
  case TOK_LT:  return a < b;
  case TOK_LE:  return a <= b;
  case TOK_GT:  return a > b;
  case TOK_GE:  return a >= b;
  case TOK_ULT: return (unsigned)a <  (unsigned)b;
  case TOK_ULE: return (unsigned)a <= (unsigned)b;
  case TOK_UGT: return (unsigned)a >  (unsigned)b;
  case TOK_UGE: return (unsigned)a >= (unsigned)b;
  default:      return -1;
  }
}

/* Verify iv has exactly one in-loop definition and it is a step-by-constant
 * update (`iv <- iv + #s` or copy-through `T <- iv; iv <- T + #s`).  Returns
 * the positive step, or 0 if iv is not a simple additive counter. */
static int guard_iv_step(TCCIRState *ir, IRLoop *loop, int iv)
{
  int def_idx = -1, defs = 0;
  for (int i = loop->start_idx; i <= loop->end_idx; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP || !irop_config[q->op].has_dest)
      continue;
    if (irop_get_vreg(tcc_ir_op_get_dest(ir, q)) != iv)
      continue;
    def_idx = i;
    defs++;
  }
  if (defs != 1)
    return 0;
  IRQuadCompact *dq = &ir->compact_instructions[def_idx];
  if (dq->op != TCCIR_OP_ADD)
    return 0;
  IROperand b = tcc_ir_op_get_src2(ir, dq);
  if (!irop_is_immediate(b))
    return 0;
  int step = (int)irop_get_imm64_ex(ir, b);
  if (step <= 0)
    return 0;
  int32_t av = irop_get_vreg(tcc_ir_op_get_src1(ir, dq));
  if (av == iv)
    return step;
  if (av < 0)
    return 0;
  /* copy-through: av must have a single in-loop def `av <- iv [ASSIGN]`. */
  int copy_idx = -1, copies = 0;
  for (int i = loop->start_idx; i <= loop->end_idx; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP || !irop_config[q->op].has_dest)
      continue;
    if (irop_get_vreg(tcc_ir_op_get_dest(ir, q)) != av)
      continue;
    copy_idx = i;
    copies++;
  }
  if (copies != 1)
    return 0;
  IRQuadCompact *cq = &ir->compact_instructions[copy_idx];
  if (cq->op != TCCIR_OP_ASSIGN || irop_get_vreg(tcc_ir_op_get_src1(ir, cq)) != iv)
    return 0;
  return step;
}

/* True if [start_idx,end_idx] has no jump leaving the loop range other than
 * the back-edge (i.e. the only loop exit is fall-through past the latch). */
static int guard_loop_single_exit(TCCIRState *ir, IRLoop *loop)
{
  for (int i = loop->start_idx; i <= loop->end_idx; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_JUMP && q->op != TCCIR_OP_JUMPIF)
      continue;
    int t = (int)irop_get_imm64_ex(ir, tcc_ir_op_get_dest(ir, q));
    if (t < loop->start_idx || t > loop->end_idx)
      return 0; /* an edge leaves the loop body -> extra exit */
  }
  return 1;
}

int tcc_ir_opt_loop_guard_elim(TCCIRState *ir)
{
  if (!ir || ir->next_instruction_index == 0)
    return 0;

  /* Bail on un-enumerable control flow: the program-order exit-value carry
   * assumes straight-line fall-through between sequential loops. */
  for (int i = 0; i < ir->next_instruction_index; i++)
  {
    TccIrOp op = ir->compact_instructions[i].op;
    if (op == TCCIR_OP_IJUMP || op == TCCIR_OP_SWITCH_TABLE || op == TCCIR_OP_SWITCH_LOAD)
      return 0;
  }

  IRLoops *loops = tcc_ir_detect_loops(ir);
  if (!loops || loops->num_loops == 0)
  {
    tcc_ir_free_loops(loops);
    return 0;
  }

  int total = 0;
  int carry_vreg = -1;  /* IV vreg whose constant value is known after a loop */
  long carry_val = 0;   /* that constant value */
  int carry_from = -1;  /* program index at which carry_val first holds */

  for (int li = 0; li < loops->num_loops; li++)
  {
    IRLoop *loop = &loops->loops[li];
    if (loop->start_idx < 0)
      continue;

    /* Identify the counted IV + its (normalized) exit condition.  The latch
     * is the CMP closest to the back-edge whose exit target is past the loop. */
    int iv = -1, limit = 0, cond = 0, exit_target = -1;
    {
      int lo = loop->end_idx - 4;
      if (lo < loop->start_idx)
        lo = loop->start_idx;
      for (int i = loop->end_idx; i >= lo && iv < 0; i--)
      {
        IRQuadCompact *cq = &ir->compact_instructions[i];
        if (cq->op != TCCIR_OP_CMP)
          continue;
        if (!irop_is_immediate(tcc_ir_op_get_src2(ir, cq)))
          continue;
        int32_t cand = irop_get_vreg(tcc_ir_op_get_src1(ir, cq));
        if (cand < 0)
          continue;
        int ci, ji, lim, cnd, et;
        if (find_loop_exit_condition(ir, loop, cand, &ci, &ji, &lim, &cnd, &et) && et > loop->end_idx)
        {
          iv = cand;
          limit = lim;
          cond = cnd;
          exit_target = et;
        }
      }
    }
    if (iv < 0)
    {
      carry_vreg = -1; /* unknown loop shape invalidates any pending carry */
      continue;
    }

    /* Determine the IV's constant value on entry to this loop. */
    int have_entry = 0;
    long entry = 0;
    for (int j = loop->preheader_idx; j >= 0 && j >= loop->preheader_idx - 6; j--)
    {
      IRQuadCompact *pq = &ir->compact_instructions[j];
      if (pq->op == TCCIR_OP_NOP)
        continue;
      if (!irop_config[pq->op].has_dest)
        continue;
      if (irop_get_vreg(tcc_ir_op_get_dest(ir, pq)) != iv)
        continue;
      /* Closest preheader def of iv decides: an immediate ASSIGN gives a known
       * entry; anything else (e.g. a prior loop's increment) does not. */
      if (pq->op == TCCIR_OP_ASSIGN && irop_is_immediate(tcc_ir_op_get_src1(ir, pq)))
      {
        entry = (long)irop_get_imm64_ex(ir, tcc_ir_op_get_src1(ir, pq));
        have_entry = 1;
      }
      break;
    }
    if (!have_entry && carry_vreg == iv && carry_from >= 0)
    {
      /* Consume the carried exit value of the previous loop only if i is not
       * redefined and no foreign edge merges in between its source and here. */
      int clean = 1;
      for (int j = carry_from; j < loop->start_idx && clean; j++)
      {
        IRQuadCompact *q = &ir->compact_instructions[j];
        if (q->op == TCCIR_OP_NOP)
          continue;
        if (j > carry_from && q->is_jump_target)
          clean = 0;
        else if (irop_config[q->op].has_dest && irop_get_vreg(tcc_ir_op_get_dest(ir, q)) == iv)
          clean = 0;
      }
      if (clean)
      {
        entry = carry_val;
        have_entry = 1;
      }
    }

    /* Remove the pre-loop entry guard if it is provably never taken. */
    int guard_removed = 0, guard_found = 0;
    if (have_entry)
    {
      int glo = loop->start_idx - 6;
      if (glo < 0)
        glo = 0;
      if (carry_from >= 0 && glo < carry_from)
        glo = carry_from;
      for (int g = glo; g + 1 < loop->start_idx; g++)
      {
        IRQuadCompact *gq = &ir->compact_instructions[g];
        if (gq->op != TCCIR_OP_CMP)
          continue;
        if (irop_get_vreg(tcc_ir_op_get_src1(ir, gq)) != iv)
          continue;
        IROperand gs2 = tcc_ir_op_get_src2(ir, gq);
        if (!irop_is_immediate(gs2))
          continue;
        IRQuadCompact *gjq = &ir->compact_instructions[g + 1];
        if (gjq->op != TCCIR_OP_JUMPIF)
          continue;
        int gtarget = (int)irop_get_imm64_ex(ir, tcc_ir_op_get_dest(ir, gjq));
        if (gtarget <= loop->end_idx)
          continue; /* not a skip-past-the-loop guard */
        guard_found = 1;
        int gcond = (int)irop_get_imm64_ex(ir, tcc_ir_op_get_src1(ir, gjq));
        long glim = (long)irop_get_imm64_ex(ir, gs2);
        if (guard_eval_cond((int)entry, gcond, (int)glim) != 0)
          break; /* taken or unknown: do not remove */
        gq->op = TCCIR_OP_NOP;
        gjq->op = TCCIR_OP_NOP;
        guard_removed = 1;
        total++;
        /* Clear stale is_jump_target on the skip target if the guard was its
         * only jump in-edge (so the carried-value clean check stays valid). */
        if (gtarget >= 0 && gtarget < ir->next_instruction_index)
        {
          int other_edge = 0;
          for (int s = 0; s < ir->next_instruction_index && !other_edge; s++)
          {
            if (s == g + 1)
              continue;
            IRQuadCompact *sq = &ir->compact_instructions[s];
            if (sq->op != TCCIR_OP_JUMP && sq->op != TCCIR_OP_JUMPIF)
              continue;
            if ((int)irop_get_imm64_ex(ir, tcc_ir_op_get_dest(ir, sq)) == gtarget)
              other_edge = 1;
          }
          if (!other_edge)
            ir->compact_instructions[gtarget].is_jump_target = 0;
        }
        break;
      }
    }

    /* Record the constant exit value for the next loop reusing this IV. */
    int step = guard_iv_step(ir, loop, iv);
    if (have_entry && step > 0 && (guard_removed || !guard_found) && guard_loop_single_exit(ir, loop))
    {
      int trip = compute_trip_count((int)entry, limit, step, cond);
      if (trip > 0)
      {
        carry_vreg = iv;
        carry_val = entry + (long)trip * step;
        carry_from = exit_target;
        continue;
      }
    }
    carry_vreg = -1; /* cannot prove a clean constant exit value */
  }

  tcc_ir_free_loops(loops);
  return total;
}
