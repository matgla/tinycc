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

        /* Check if this instruction uses the vreg */
        int uses_vr = 0;
        if (irop_config[q->op].has_src1)
        {
          IROperand s1 = tcc_ir_op_get_src1(ir, q);
          if (irop_get_vreg(s1) == vr)
            uses_vr = 1;
        }
        if (irop_config[q->op].has_src2)
        {
          IROperand s2 = tcc_ir_op_get_src2(ir, q);
          if (irop_get_vreg(s2) == vr)
            uses_vr = 1;
        }

        if (!uses_vr)
          continue;

        use_count++;

        /* Must be a CMP instruction within or near the loop */
        if (q->op != TCCIR_OP_CMP)
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
int tcc_ir_opt_loop_unroll(TCCIRState *ir)
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

int tcc_ir_opt_loop_rotation(TCCIRState *ir)
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
        IROperand s1 = tcc_ir_op_get_src1(ir, q);
        if (irop_get_vreg(s1) != iv_vr)
          continue;

        int jq_idx = i + 1;
        while (jq_idx < n && (ir->compact_instructions[jq_idx].op == TCCIR_OP_NOP ||
                              ir->compact_instructions[jq_idx].op == TCCIR_OP_ASSIGN))
          jq_idx++;
        if (jq_idx < n && ir->compact_instructions[jq_idx].op == TCCIR_OP_JUMPIF)
        {
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
