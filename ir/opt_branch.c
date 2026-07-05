/*
 *  TCC IR - Branch & Boolean Optimization
 *
 *  Copyright (c) 2025 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#define USING_GLOBALS

#include "ir.h"
#include "opt_utils.h"
#include "opt_du.h"
#include "opt_alias.h"
#include "opt_engine.h"
#include "opt_gens_branch.h"

#define VRP_MAX_POS 256

typedef struct
{
  int valid;
  int64_t min_val;
  int64_t max_val;
} VRPRange;

/* Map (vreg_type, position) to a flat slot index.
 * PARAM positions 0..VRP_MAX_POS-1 → slots 0..VRP_MAX_POS-1
 * TEMP  positions 0..VRP_MAX_POS-1 → slots VRP_MAX_POS..2*VRP_MAX_POS-1
 * VAR   positions 0..VRP_MAX_POS-1 → slots 2*VRP_MAX_POS..3*VRP_MAX_POS-1
 * Returns -1 if not tracked. */
static int vrp_get_slot(int vr_type, int pos)
{
  if (pos < 0 || pos >= VRP_MAX_POS)
    return -1;
  if (vr_type == TCCIR_VREG_TYPE_PARAM)
    return pos;
  if (vr_type == TCCIR_VREG_TYPE_TEMP)
    return VRP_MAX_POS + pos;
  if (vr_type == TCCIR_VREG_TYPE_VAR)
    return 2 * VRP_MAX_POS + pos;
  return -1;
}

/* VRP models 32-bit values as sign-extended int32 (the IMM32 operand
 * encoding, which the range table and its ADD/SUB arithmetic use).  A 32-bit
 * unsigned constant can instead arrive as a pool-stored I64 holding the
 * ZERO-extended value (e.g. #3435266601, printed as #-859700695), and mixing
 * the two encodings in one int64 comparison flips unsigned compares (ptr
 * fuzz seed 35289: `T <u #c` folded to 0 when the true answer is 1).  Read
 * every constant through this helper: it normalizes 32-bit-typed operands
 * into the sign-extended domain and rejects genuinely 64-bit-typed ones,
 * which this domain cannot represent. */
static int vrp_read_const32(const TCCIRState *ir, IROperand op, int64_t *out)
{
  if (op.btype == IROP_BTYPE_INT64)
    return 0;
  *out = (int64_t)(int32_t)irop_get_imm64_ex(ir, op);
  return 1;
}

/* Check whether a comparison yields a constant result over [rmin, rmax].
 * Returns 1 if always taken, 0 if never taken, -1 if undetermined.
 * For unsigned comparisons, only safe when both endpoints have the same sign
 * (both >= 0 or both < 0 as int64), so the uint32 ordering is monotone. */
static int vrp_fold_cmp(int64_t rmin, int64_t rmax, int64_t cmp_val, int tok)
{
  /* Enforce the precondition above instead of trusting every caller: a
   * mixed-sign range covers both halves of the uint32 space, so endpoint
   * checks say nothing about the values in between. */
  if ((tok == 0x92 || tok == 0x93 || tok == 0x96 || tok == 0x97) &&
      (rmin < 0) != (rmax < 0))
    return -1;
  int res_min = evaluate_compare_condition(rmin, cmp_val, tok);
  int res_max = evaluate_compare_condition(rmax, cmp_val, tok);
  if (res_min < 0 || res_max < 0 || res_min != res_max)
    return -1;
  return res_min;
}




static int ir_opt_match_zero_test(TCCIRState *ir, int idx, IROperand *expr_out)
{
  IRQuadCompact *q;
  IROperand src1;
  IROperand src2;

  if (!ir || idx < 0 || idx >= ir->next_instruction_index || !expr_out)
    return 0;

  q = &ir->compact_instructions[idx];
  if (q->op == TCCIR_OP_TEST_ZERO)
  {
    *expr_out = tcc_ir_op_get_src1(ir, q);
    return 1;
  }

  if (q->op != TCCIR_OP_CMP)
    return 0;

  src1 = tcc_ir_op_get_src1(ir, q);
  src2 = tcc_ir_op_get_src2(ir, q);
  if (irop_is_immediate(src2) && irop_get_imm64_ex(ir, src2) == 0)
  {
    *expr_out = src1;
    return 1;
  }
  if (irop_is_immediate(src1) && irop_get_imm64_ex(ir, src1) == 0)
  {
    *expr_out = src2;
    return 1;
  }

  return 0;
}

int tcc_ir_opt_float_branch_fold(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  int changes = 0;
  uint8_t *is_merge;

  if (n < 4)
    return 0;

  is_merge = ir_opt_build_merge_bitmap(ir, n);

  for (int i = 0; i < n; ++i)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];

    if (q->op == TCCIR_OP_FUNCCALLVOID || q->op == TCCIR_OP_FUNCCALLVAL)
    {
      Sym *callee;
      const char *name;
      int jump1_idx = ir_opt_next_non_nop(ir, i + 1);
      int cmp2_idx;
      int jump2_idx;
      IRQuadCompact *jump1;
      IRQuadCompact *cmp2;
      IRQuadCompact *jump2;
      IROperand arg0;
      IROperand arg1;
      IROperand cmp2_arg0;
      IROperand cmp2_arg1;
      int tok1;
      int tok2;
      int known_fact;
      int effective_tok2 = -1;
      int is_swapped = 0;

      callee = irop_get_sym_ex(ir, tcc_ir_op_get_src1(ir, q));
      if (!callee)
        continue;
      name = get_tok_str(callee->v, NULL);
      if (!ir_opt_is_flag_cmp_helper_name(name))
        continue;
      if (!ir_opt_get_call_param_operand(ir, i, 0, &arg0) || !ir_opt_get_call_param_operand(ir, i, 1, &arg1))
        continue;

      if (jump1_idx < 0)
        continue;
      jump1 = &ir->compact_instructions[jump1_idx];
      if (jump1->op != TCCIR_OP_JUMPIF)
        continue;

      cmp2_idx = -1;
      jump2_idx = -1;
      for (int scan_idx = ir_opt_next_non_nop(ir, jump1_idx + 1); scan_idx >= 0 && scan_idx < n;
           scan_idx = ir_opt_next_non_nop(ir, scan_idx + 1))
      {
        IRQuadCompact *scan_q;
        Sym *scan_callee;
        const char *scan_name;

        if (is_merge[scan_idx / 8] & (1 << (scan_idx % 8)))
          break;

        scan_q = &ir->compact_instructions[scan_idx];
        if (scan_q->op != TCCIR_OP_FUNCCALLVOID && scan_q->op != TCCIR_OP_FUNCCALLVAL)
        {
          if (!ir_opt_is_pure_fallthrough_instruction(ir, scan_idx))
            break;
          continue;
        }

        scan_callee = irop_get_sym_ex(ir, tcc_ir_op_get_src1(ir, scan_q));
        scan_name = scan_callee ? get_tok_str(scan_callee->v, NULL) : NULL;
        if (!ir_opt_is_flag_cmp_helper_name(scan_name))
        {
          if (!ir_opt_is_pure_fallthrough_instruction(ir, scan_idx))
            break;
          continue;
        }

        cmp2_idx = scan_idx;
        jump2_idx = ir_opt_next_non_nop(ir, cmp2_idx + 1);
        break;
      }

      if (cmp2_idx < 0 || jump2_idx < 0)
        continue;

      cmp2 = &ir->compact_instructions[cmp2_idx];
      jump2 = &ir->compact_instructions[jump2_idx];
      if (jump2->op != TCCIR_OP_JUMPIF)
        continue;

      callee = irop_get_sym_ex(ir, tcc_ir_op_get_src1(ir, cmp2));
      if (!callee)
        continue;
      name = get_tok_str(callee->v, NULL);
      if (!ir_opt_is_flag_cmp_helper_name(name))
        continue;
      if (!ir_opt_get_call_param_operand(ir, cmp2_idx, 0, &cmp2_arg0) ||
          !ir_opt_get_call_param_operand(ir, cmp2_idx, 1, &cmp2_arg1))
      {
        continue;
      }

      tok1 = (int)irop_get_imm64_ex(ir, tcc_ir_op_get_src1(ir, jump1));
      tok2 = (int)irop_get_imm64_ex(ir, tcc_ir_op_get_src1(ir, jump2));
      known_fact = vrp_negate_cmp_tok(tok1);
      if (known_fact < 0)
      {
        continue;
      }

      int eq1 = ir_opt_pure_expr_equal(ir, arg0, i, cmp2_arg0, cmp2_idx, 0);
      int eq2 = ir_opt_pure_expr_equal(ir, arg1, i, cmp2_arg1, cmp2_idx, 0);
      if (eq1 && eq2)
        effective_tok2 = tok2;
      else if (ir_opt_pure_expr_equal(ir, arg0, i, cmp2_arg1, cmp2_idx, 0) &&
               ir_opt_pure_expr_equal(ir, arg1, i, cmp2_arg0, cmp2_idx, 0))
      {
        is_swapped = 1;
        effective_tok2 = vrp_swap_cmp_tok(tok2);
      }

      if (effective_tok2 < 0)
      {
        continue;
      }

      if (is_swapped)
      {
        IROperand jmp1_dest = tcc_ir_op_get_dest(ir, jump1);
        IROperand jmp2_dest = tcc_ir_op_get_dest(ir, jump2);
        if (jmp1_dest.u.imm32 != jmp2_dest.u.imm32)
        {
          switch (known_fact)
          {
          case TOK_LT:
          case TOK_GT:
          case TOK_ULT:
          case TOK_UGT:
            break;
          default:
            continue;
          }
        }
      }

      if (fcmp_cmp_implies(known_fact, effective_tok2))
      {
        IROperand jmp2_dest = tcc_ir_op_get_dest(ir, jump2);
        cmp2->op = TCCIR_OP_NOP;
        jump2->op = TCCIR_OP_JUMP;
        tcc_ir_set_dest(ir, jump2_idx, jmp2_dest);
        changes++;
      }
      else if (fcmp_cmp_implies(known_fact, vrp_negate_cmp_tok(effective_tok2)))
      {
        cmp2->op = TCCIR_OP_NOP;
        jump2->op = TCCIR_OP_NOP;
        changes++;
      }

      continue;
    }

    if (q->op == TCCIR_OP_TEST_ZERO || q->op == TCCIR_OP_CMP)
    {
      IRQuadCompact *jump1;
      int jump1_idx = ir_opt_next_non_nop(ir, i + 1);
      int known_zero = -1;
      IROperand expr1;

      if (!ir_opt_match_zero_test(ir, i, &expr1))
        continue;

      if (jump1_idx < 0)
        continue;
      jump1 = &ir->compact_instructions[jump1_idx];
      if (jump1->op != TCCIR_OP_JUMPIF)
        continue;

      switch ((int)irop_get_imm64_ex(ir, tcc_ir_op_get_src1(ir, jump1)))
      {
      case TOK_NE:
        known_zero = 1;
        break;
      case TOK_EQ:
        known_zero = 0;
        break;
      default:
        break;
      }
      if (known_zero < 0)
        continue;

      for (int test2_idx = ir_opt_next_non_nop(ir, jump1_idx + 1); test2_idx >= 0 && test2_idx + 1 < n;
           test2_idx = ir_opt_next_non_nop(ir, test2_idx + 1))
      {
        IRQuadCompact *test2;
        IRQuadCompact *jump2;
        int jump2_idx;
        int tok2;
        IROperand expr2;
        int is_zero_test_candidate;

        if (is_merge[test2_idx / 8] & (1 << (test2_idx % 8)))
          break;

        test2 = &ir->compact_instructions[test2_idx];
        is_zero_test_candidate = ir_opt_match_zero_test(ir, test2_idx, &expr2);
        if (!is_zero_test_candidate)
        {
          if (!ir_opt_is_pure_fallthrough_instruction(ir, test2_idx))
            break;
          continue;
        }

        jump2_idx = ir_opt_next_non_nop(ir, test2_idx + 1);
        if (jump2_idx < 0)
          break;

        jump2 = &ir->compact_instructions[jump2_idx];
        if (jump2->op != TCCIR_OP_JUMPIF)
          break;

        if (!ir_opt_pure_expr_equal(ir, expr1, i, expr2, test2_idx, 0))
          continue;

        tok2 = (int)irop_get_imm64_ex(ir, tcc_ir_op_get_src1(ir, jump2));
        if ((known_zero && tok2 == TOK_EQ) || (!known_zero && tok2 == TOK_NE))
        {
          IROperand jmp2_dest = tcc_ir_op_get_dest(ir, jump2);
          test2->op = TCCIR_OP_NOP;
          jump2->op = TCCIR_OP_JUMP;
          tcc_ir_set_dest(ir, jump2_idx, jmp2_dest);
          changes++;
        }
        else if ((known_zero && tok2 == TOK_NE) || (!known_zero && tok2 == TOK_EQ))
        {
          test2->op = TCCIR_OP_NOP;
          jump2->op = TCCIR_OP_NOP;
          changes++;
        }
        break;
      }
    }
  }

  tcc_free(is_merge);
  return changes;
}

int tcc_ir_opt_vrp(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  int changes = 0;

  if (n < 3)
    return 0;

  /* Precompute merge points (multiple predecessors or back-edge targets) */
  uint8_t *is_merge = ir_opt_build_merge_bitmap(ir, n);

  /* Range table: PARAM in 0..VRP_MAX_POS-1, TEMP in VRP_MAX_POS..2*VRP_MAX_POS-1,
   * VAR in 2*VRP_MAX_POS..3*VRP_MAX_POS-1.  Heap-allocated: VRP_MAX_POS*3
   * VRPRange entries are ~18 KB each, far too large for the target's process
   * stack (these two arrays alone would blow a 32 KB stack). */
  const size_t vrp_ranges_bytes = sizeof(VRPRange) * (VRP_MAX_POS * 3);
  VRPRange *ranges = tcc_mallocz(vrp_ranges_bytes);
  VRPRange *deferred_ranges = tcc_mallocz(vrp_ranges_bytes);

  /* `ranges`/`deferred_ranges` are 18 KB each and were unconditionally
   * memset/memcpy'd at every branch, merge, and dominating jump — hundreds of
   * times per function.  On the target's slow PSRAM heap that 18 KB-per-event
   * clearing was the single largest compile cost (~44%) even though most
   * functions never populate a single range (e.g. builtin-bitops' `main`, whose
   * useful vrp work is reg-reg CMP folding that doesn't touch `ranges`).  Track
   * whether each buffer currently holds any valid entry and skip the wipe/copy
   * when it is already all-zero; fall back to the full operation when populated.
   * ranges_dirty == 0 is the invariant "`ranges` is entirely .valid==0". */
  int ranges_dirty = 0;
  int deferred_dirty = 0;

  /* Pending fall-through constraint: applied at instruction pending_apply_at */
  int pending_apply_at = -1;
  int pending_slot = -1;
  int64_t pending_min = 0;
  int64_t pending_max = 0;

  /* Scoped equality constraint: after CMP X, #A / JUMPIF != target,
   * the constraint X==[A,A] holds until we reach target.  Unlike
   * pending_*, this survives merge points within the fall-through.
   * eq_scope_src_slot tracks the source PARAM/VAR if X was loaded
   * from one, so loads from the same source inherit the range. */
  int eq_scope_end = -1;
  int eq_scope_slot = -1;
  int eq_scope_src_slot = -1;
  int64_t eq_scope_val = 0;

  /* Ranges deferred through a dominating unconditional jump.  When an
   * unconditional JUMP targets a block T whose *only* predecessor is that
   * jump (T is not a merge point, so pred_count[T]==1 and the jump itself is
   * that predecessor), every fact valid at the jump is valid at entry to T.
   * The linear scan otherwise drops these facts: the JUMP clears all ranges
   * (its linear successor belongs to a different path) and T re-derives
   * nothing.  We snapshot the ranges at the jump and reinstall them when the
   * scan reaches T.  This carries a loop-guard's fall-through bound (e.g.
   * `s<=1`) into a switch-dispatch block reached by the guard's taken edge —
   * letting the dead `case`s on out-of-range values fold away. */
  int deferred_target = -1;

  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];

    /* End the scoped equality constraint when we reach the JUMPIF target */
    if (i == eq_scope_end) {
      if (eq_scope_slot >= 0)
        ranges[eq_scope_slot].valid = 0;
      if (eq_scope_src_slot >= 0)
        ranges[eq_scope_src_slot].valid = 0;
      eq_scope_end = -1;
      eq_scope_slot = -1;
      eq_scope_src_slot = -1;
    }

    /* Reinstall ranges carried through a dominating unconditional jump.
     * deferred_target is only set for a non-merge (sole-predecessor) block,
     * so this is the block's true entry state — it takes precedence over the
     * merge/pending handling below (neither of which can apply to it). */
    if (i == deferred_target)
    {
      if (deferred_dirty)
      {
        memcpy(ranges, deferred_ranges, vrp_ranges_bytes);
        ranges_dirty = 1;
      }
      else if (ranges_dirty)
      {
        memset(ranges, 0, vrp_ranges_bytes);
        ranges_dirty = 0;
      }
      deferred_target = -1;
    }
    /* At merge points: clear all ranges and discard pending constraint,
     * but re-apply the scoped equality constraint if still active. */
    else if (is_merge[i / 8] & (1 << (i % 8)))
    {
      if (ranges_dirty)
      {
        memset(ranges, 0, vrp_ranges_bytes);
        ranges_dirty = 0;
      }
      pending_apply_at = -1;
      pending_slot = -1;
      /* Scoped constraint re-apply disabled: not all merge points
       * within [JUMPIF+2, target) are dominated by the fall-through.
       * The CMP+SETIF backward scan handles the target case directly. */
    }
    else if (pending_apply_at == i && pending_slot >= 0)
    {
      /* Apply fall-through constraint (intersect with any existing range) */
      VRPRange *r = &ranges[pending_slot];
      if (r->valid)
      {
        pending_min = pending_min > r->min_val ? pending_min : r->min_val;
        pending_max = pending_max < r->max_val ? pending_max : r->max_val;
      }
      if (pending_min <= pending_max)
      {
        r->valid = 1;
        r->min_val = pending_min;
        r->max_val = pending_max;
        ranges_dirty = 1;
      }
      pending_apply_at = -1;
      pending_slot = -1;
    }

    if (q->op == TCCIR_OP_NOP)
      continue;

    IROperand dest = tcc_ir_op_get_dest(ir, q);
    IROperand src1 = tcc_ir_op_get_src1(ir, q);
    IROperand src2 = tcc_ir_op_get_src2(ir, q);

    /* Track arithmetic: T/P_dest = T/P_src1 +/- #imm → propagate range */
    if ((q->op == TCCIR_OP_ADD || q->op == TCCIR_OP_SUB) && irop_is_immediate(src2))
    {
      int32_t src1_vr = irop_get_vreg(src1);
      int32_t dest_vr = irop_get_vreg(dest);
      if (src1_vr >= 0 && dest_vr >= 0)
      {
        int src_slot = vrp_get_slot(TCCIR_DECODE_VREG_TYPE(src1_vr), TCCIR_DECODE_VREG_POSITION(src1_vr));
        int dst_slot = vrp_get_slot(TCCIR_DECODE_VREG_TYPE(dest_vr), TCCIR_DECODE_VREG_POSITION(dest_vr));
        int64_t imm;
        if (src_slot >= 0 && ranges[src_slot].valid && dst_slot >= 0 &&
            vrp_read_const32(ir, src2, &imm))
        {
          int64_t new_min = (q->op == TCCIR_OP_ADD) ? ranges[src_slot].min_val + imm : ranges[src_slot].min_val - imm;
          int64_t new_max = (q->op == TCCIR_OP_ADD) ? ranges[src_slot].max_val + imm : ranges[src_slot].max_val - imm;
          /* A result outside int32 wraps in 32-bit arithmetic and the wrapped
           * value set is not an interval in this domain, so drop the range
           * rather than clamp (a clamped endpoint asserts a value the program
           * never actually takes). */
          if (new_min < (int64_t)INT32_MIN || new_max > (int64_t)INT32_MAX)
          {
            ranges[dst_slot].valid = 0;
          }
          else
          {
            ranges[dst_slot].valid = 1;
            ranges[dst_slot].min_val = new_min;
            ranges[dst_slot].max_val = new_max;
            ranges_dirty = 1;
          }
        }
        else if (dst_slot >= 0)
        {
          ranges[dst_slot].valid = 0;
        }
      }
      continue;
    }

    /* Propagate ranges through an ASSIGN whose source forwards a value range.
     * A non-lval source forwards its tracked range directly.  An lval source
     * that simply names a local/parameter (VAR/PARAM vreg, not a pointer
     * deref, double indirection, or symbol) also forwards a value range: the
     * slot we track for a VAR/PARAM holds that variable's value, which is
     * exactly what the lval load reads.  TEMP lvals are pointer dereferences
     * whose pointer range is unrelated to the loaded value, so they are
     * excluded. */
    if (q->op == TCCIR_OP_ASSIGN && irop_config[q->op].has_dest)
    {
      int32_t s1_vr = irop_get_vreg(src1);
      int32_t d_vr = irop_get_vreg(dest);

      /* Seed a singleton range from a plain immediate assignment: T = #imm
       * gives the destination the range [imm, imm].  Without this, the very
       * first range in an "assign a constant, then compare it" chain is never
       * established -- every other range source (fall-through constraints from
       * a prior CMP, ADD/SUB propagation, and vreg-to-vreg copy propagation
       * below) can only forward a range that already exists, none can create
       * one from a bare immediate -- so `T = #5; CMP T,#20; JUMPIF LT` never
       * folds.  See docs/bugs.md #6.  Constants are normalized to the pass's
       * sign-extended-int32 domain by vrp_read_const32 (64-bit-typed values
       * are rejected); a non-vreg or lval/sym destination is not a plain
       * value definition and is left to the generic invalidation. */
      if (irop_is_immediate(src1) && d_vr >= 0 && !dest.is_lval && !dest.is_sym)
      {
        int d_slot = vrp_get_slot(TCCIR_DECODE_VREG_TYPE(d_vr), TCCIR_DECODE_VREG_POSITION(d_vr));
        if (d_slot >= 0)
        {
          int64_t imm;
          if (vrp_read_const32(ir, src1, &imm))
          {
            ranges[d_slot].valid = 1;
            ranges[d_slot].min_val = imm;
            ranges[d_slot].max_val = imm;
            ranges_dirty = 1;
          }
          else
          {
            ranges[d_slot].valid = 0;
          }
          continue;
        }
      }

      int src_type = (s1_vr >= 0) ? TCCIR_DECODE_VREG_TYPE(s1_vr) : -1;
      int src_forwards_value =
          s1_vr >= 0 &&
          (!src1.is_lval ||
           ((src_type == TCCIR_VREG_TYPE_VAR || src_type == TCCIR_VREG_TYPE_PARAM) &&
            !src1.is_llocal && !src1.is_sym));
      if (src_forwards_value && d_vr >= 0) {
        int s_slot = vrp_get_slot(src_type, TCCIR_DECODE_VREG_POSITION(s1_vr));
        int d_slot = vrp_get_slot(TCCIR_DECODE_VREG_TYPE(d_vr), TCCIR_DECODE_VREG_POSITION(d_vr));
        if (s_slot >= 0 && d_slot >= 0 && ranges[s_slot].valid) {
          ranges[d_slot] = ranges[s_slot];
          /* Preserve the copied range: the generic dest-invalidation at the
           * bottom of the loop would otherwise immediately clear it (as it
           * does for unhandled ops), defeating this propagation.  The ADD/SUB
           * case above `continue`s for the same reason. */
          continue;
        } else if (d_slot >= 0) {
          ranges[d_slot].valid = 0;
        }
      }
    }

    /* CMP + JUMPIF: try to fold using range, or derive fall-through constraint */
    if (q->op == TCCIR_OP_CMP && i + 1 < n)
    {
      IRQuadCompact *jump_q = &ir->compact_instructions[i + 1];
      if (jump_q->op == TCCIR_OP_JUMPIF && irop_is_immediate(src2))
      {
        int32_t src1_vr = irop_get_vreg(src1);
        if (src1_vr >= 0)
        {
          int src_slot = vrp_get_slot(TCCIR_DECODE_VREG_TYPE(src1_vr), TCCIR_DECODE_VREG_POSITION(src1_vr));
          int64_t cmp_val = 0;
          int cmp_val_ok = vrp_read_const32(ir, src2, &cmp_val);
          IROperand cond_op = tcc_ir_op_get_src1(ir, jump_q);
          int tok = (int)irop_get_imm64_ex(ir, cond_op);
          IROperand jmp_dest = tcc_ir_op_get_dest(ir, jump_q);

          /* Tautology fold: unsigned compare against zero is always-true
           * (>=U 0) or always-false (<U 0) regardless of the operand's value.
           * No range info required (zero is zero in any width, so the raw
           * immediate is checked without the const32 normalization). */
          if (irop_get_imm64_ex(ir, src2) == 0)
          {
            int fold_taut = -1;
            if (tok == 0x93) /* TOK_UGE */
              fold_taut = 1;
            else if (tok == 0x92) /* TOK_ULT */
              fold_taut = 0;
            if (fold_taut == 1)
            {
              q->op = TCCIR_OP_NOP;
              jump_q->op = TCCIR_OP_JUMP;
              tcc_ir_set_dest(ir, i + 1, jmp_dest);
              changes++;
              continue;
            }
            else if (fold_taut == 0)
            {
              q->op = TCCIR_OP_NOP;
              jump_q->op = TCCIR_OP_NOP;
              changes++;
              continue;
            }
          }

          /* Try to fold using known range */
          if (src_slot >= 0 && ranges[src_slot].valid && cmp_val_ok)
          {
            int64_t rmin = ranges[src_slot].min_val;
            int64_t rmax = ranges[src_slot].max_val;
            int fold_result = -1;
            /* Monotone signed conditions: checking endpoints suffices */
            int is_monotone_signed = (tok == 0x9c || tok == 0x9d || tok == 0x9e || tok == 0x9f);
            /* TOK_ULT=0x92, TOK_UGE=0x93, TOK_ULE=0x96, TOK_UGT=0x97 per tcc.h */
            int is_unsigned_cond = (tok == 0x92 || tok == 0x93 || tok == 0x96 || tok == 0x97);
            /* EQ/NE are NOT monotone — special handling below */
            int is_eq_ne = (tok == 0x94 || tok == 0x95);

            if (is_monotone_signed)
            {
              fold_result = vrp_fold_cmp(rmin, rmax, cmp_val, tok);
            }
            else if (is_unsigned_cond && rmin >= 0 && rmax >= 0)
            {
              /* Both endpoints non-negative: uint32 ordering matches int64 ordering */
              fold_result = vrp_fold_cmp(rmin, rmax, cmp_val, tok);
            }
            else if (is_unsigned_cond && rmin < 0 && rmax < 0)
            {
              /* Both endpoints negative as int32: uint32 ordering preserved in int64.
               * (For two negative int32 a < b: uint32(a) = a+2^32 < uint32(b) = b+2^32,
               * and uint64(int64(a)) = a+2^64 < uint64(int64(b)) = b+2^64 — same order.) */
              fold_result = vrp_fold_cmp(rmin, rmax, cmp_val, tok);
            }
            else if (is_eq_ne)
            {
              /* For == and !=, endpoint checking alone is insufficient since
               * these are not monotone. We can only fold when:
               * (a) cmp_val is outside [rmin, rmax] → value can never/always match
               * (b) rmin == rmax → singleton range, exact comparison */
              if (cmp_val < rmin || cmp_val > rmax)
              {
                /* cmp_val outside range: == is never true, != is always true */
                fold_result = (tok == 0x95) ? 1 : 0;
              }
              else if (rmin == rmax)
              {
                /* Singleton: cmp_val == rmin, so == is true, != is false */
                fold_result = (tok == 0x94) ? 1 : 0;
              }
            }

            if (fold_result == 1)
            {
              /* Branch always taken → unconditional JUMP */
              q->op = TCCIR_OP_NOP;
              jump_q->op = TCCIR_OP_JUMP;
              tcc_ir_set_dest(ir, i + 1, jmp_dest);
              changes++;
              continue;
            }
            else if (fold_result == 0)
            {
              /* Branch never taken → NOP both */
              q->op = TCCIR_OP_NOP;
              jump_q->op = TCCIR_OP_NOP;
              changes++;
              continue;
            }
          }

          /* Set pending fall-through constraint: NOT(cond) holds after JUMPIF not-taken */
          if (src_slot >= 0 && i + 2 < n && cmp_val_ok)
          {
            int64_t new_min = INT32_MIN;
            int64_t new_max = INT32_MAX;
            int set_constraint = 0;

            /* Fall-through means cond is FALSE for (src1 vs cmp_val) */
            switch (tok)
            {
            case 0x9e: /* TOK_LE (<=S): fall-through: src1 > cmp_val */
              if (cmp_val < (int64_t)INT32_MAX)
              {
                new_min = cmp_val + 1;
                new_max = INT32_MAX;
                set_constraint = 1;
              }
              break;
            case 0x9c: /* TOK_LT (<S): fall-through: src1 >= cmp_val */
              new_min = cmp_val < (int64_t)INT32_MIN ? INT32_MIN : cmp_val;
              new_max = INT32_MAX;
              set_constraint = 1;
              break;
            case 0x9d: /* TOK_GE (>=S): fall-through: src1 < cmp_val */
              new_min = INT32_MIN;
              new_max = cmp_val > (int64_t)INT32_MAX ? INT32_MAX : cmp_val - 1;
              set_constraint = (new_max >= (int64_t)INT32_MIN);
              break;
            case 0x9f: /* TOK_GT (>S): fall-through: src1 <= cmp_val */
              new_min = INT32_MIN;
              new_max = cmp_val > (int64_t)INT32_MAX ? INT32_MAX : cmp_val;
              set_constraint = 1;
              break;
            case 0x95: /* TOK_NE (!=): fall-through: src1 == cmp_val */
              new_min = cmp_val;
              new_max = cmp_val;
              set_constraint = (cmp_val >= INT32_MIN && cmp_val <= INT32_MAX);
              break;
            default:
              break;
            }

            if (set_constraint && new_min <= new_max)
            {
              /* Schedule constraint application at instruction i+2 (after the JUMPIF) */
              pending_apply_at = i + 2;
              pending_slot = src_slot;
              pending_min = new_min;
              pending_max = new_max;

              /* For equality constraints (fall-through of !=), set up
               * a scoped constraint that survives merge points until
               * the JUMPIF target.  All merge points within the
               * fall-through region are internal branches that still
               * satisfy the equality.
               * Also back-propagate: if the CMP source was loaded from
               * a PARAM, constrain the PARAM too so that subsequent
               * loads from the same PARAM inherit the range. */
              if (new_min == new_max) {
                IROperand jdst = tcc_ir_op_get_dest(ir, jump_q);
                int jtarget = (int)irop_get_imm64_ex(ir, jdst);
                eq_scope_end = jtarget;
                eq_scope_slot = src_slot;
                eq_scope_src_slot = -1;
                eq_scope_val = new_min;

                /* Back-propagate equality to source PARAM.
                 * Only valid when the CMP compares a vreg directly (not a
                 * deref): CMP T,#K constrains T, but CMP T***DEREF***,#K
                 * constrains *T, and propagating #K to T's source PARAM
                 * would confuse a pointer address with a pointed-to value.
                 * Scan ALL definitions of src1_vr before the CMP.
                 * Safe when every def is either:
                 *   (a) immediate constant != equality value (impossible path), or
                 *   (b) non-lval PARAM load (same PARAM across all defs).
                 * Case (a) paths are dead on the fall-through (constant != value
                 * but we know vreg == value), so the value must come from (b). */
                if (!src1.is_lval) {
                  int32_t bp_param_vr = -1;
                  int bp_param_slot = -1;
                  int bp_safe = 1;
                  for (int bi = 0; bi < i && bp_safe; bi++) {
                    IRQuadCompact *bq = &ir->compact_instructions[bi];
                    if (bq->op == TCCIR_OP_NOP || !irop_config[bq->op].has_dest)
                      continue;
                    IROperand bd = tcc_ir_op_get_dest(ir, bq);
                    if (irop_get_vreg(bd) != src1_vr)
                      continue;
                    IROperand bs = tcc_ir_op_get_src1(ir, bq);
                    if ((bq->op == TCCIR_OP_ASSIGN || bq->op == TCCIR_OP_LOAD) &&
                        irop_is_immediate(bs)) {
                      int64_t bs_val;
                      if (!vrp_read_const32(ir, bs, &bs_val) || bs_val == new_min)
                        bp_safe = 0;
                      continue;
                    }
                    if (bq->op == TCCIR_OP_ASSIGN || bq->op == TCCIR_OP_LOAD) {
                      int32_t bsv = irop_get_vreg(bs);
                      if (bsv >= 0 && !bs.is_lval &&
                          TCCIR_DECODE_VREG_TYPE(bsv) == TCCIR_VREG_TYPE_PARAM) {
                        int bs_slot = vrp_get_slot(TCCIR_DECODE_VREG_TYPE(bsv),
                                                   TCCIR_DECODE_VREG_POSITION(bsv));
                        if (bp_param_vr >= 0 && bp_param_vr != bsv) {
                          bp_safe = 0;
                        } else {
                          bp_param_vr = bsv;
                          bp_param_slot = bs_slot;
                        }
                        continue;
                      }
                    }
                    bp_safe = 0;
                  }
                  if (bp_safe && bp_param_vr >= 0 && bp_param_slot >= 0) {
                    ranges[bp_param_slot].valid = 1;
                    ranges[bp_param_slot].min_val = new_min;
                    ranges[bp_param_slot].max_val = new_max;
                    ranges_dirty = 1;
                    eq_scope_src_slot = bp_param_slot;
                  }
                }
                }
            }
          }
        }
      }
      /* Register-register comparison constraint propagation.
       * Pattern: CMP A,B; JUMPIF c1 (falls through → !c1 holds for A vs B)
       *          CMP A,B; JUMPIF c2 (or CMP B,A; JUMPIF c2)
       * If !c1 implies c2 → second branch always taken → unconditional JUMP.
       * If !c1 implies !c2 → second branch never taken → NOP both. */
      else if (jump_q->op == TCCIR_OP_JUMPIF)
      {
        int32_t cmp_vr1 = irop_get_vreg(src1);
        int32_t cmp_vr2 = irop_get_vreg(src2);
        if (cmp_vr1 >= 0 && cmp_vr2 >= 0 && i + 3 < n)
        {
          IROperand cond_op = tcc_ir_op_get_src1(ir, jump_q);
          int tok1 = (int)irop_get_imm64_ex(ir, cond_op);
          int known_fact = vrp_negate_cmp_tok(tok1);

          /* Only proceed if the fall-through target is not a merge point */
          if (known_fact >= 0 && !(is_merge[(i + 2) / 8] & (1 << ((i + 2) % 8))))
          {
            IRQuadCompact *cmp2 = &ir->compact_instructions[i + 2];
            if (cmp2->op == TCCIR_OP_CMP)
            {
              IRQuadCompact *jump2 = &ir->compact_instructions[i + 3];
              if (jump2->op == TCCIR_OP_JUMPIF)
              {
                IROperand cmp2_src1 = tcc_ir_op_get_src1(ir, cmp2);
                IROperand cmp2_src2 = tcc_ir_op_get_src2(ir, cmp2);
                int32_t cmp2_vr1 = irop_get_vreg(cmp2_src1);
                int32_t cmp2_vr2 = irop_get_vreg(cmp2_src2);

                IROperand cond2_op = tcc_ir_op_get_src1(ir, jump2);
                int tok2 = (int)irop_get_imm64_ex(ir, cond2_op);
                IROperand jmp2_dest = tcc_ir_op_get_dest(ir, jump2);

                int effective_tok2 = -1;
                if (cmp2_vr1 == cmp_vr1 && cmp2_vr2 == cmp_vr2)
                  effective_tok2 = tok2; /* same operand order */
                else if (cmp2_vr1 == cmp_vr2 && cmp2_vr2 == cmp_vr1)
                  effective_tok2 = vrp_swap_cmp_tok(tok2); /* swapped operands */

                if (effective_tok2 >= 0)
                {
                  if (vrp_cmp_implies(known_fact, effective_tok2))
                  {
                    /* Second branch always taken → unconditional JUMP */
                    cmp2->op = TCCIR_OP_NOP;
                    jump2->op = TCCIR_OP_JUMP;
                    tcc_ir_set_dest(ir, i + 3, jmp2_dest);
                    changes++;
                  }
                  else if (vrp_cmp_implies(known_fact, vrp_negate_cmp_tok(effective_tok2)))
                  {
                    /* Second branch never taken → NOP both */
                    cmp2->op = TCCIR_OP_NOP;
                    jump2->op = TCCIR_OP_NOP;
                    changes++;
                  }
                }
              }
            }
          }
        }
      }
      /* CMP + SETIF: fold conditional set to constant when range proves result */
      if (jump_q->op == TCCIR_OP_SETIF && irop_is_immediate(src2))
      {
        int32_t cmp_vr = irop_get_vreg(src1);
        if (cmp_vr >= 0)
        {
          int cmp_slot = vrp_get_slot(TCCIR_DECODE_VREG_TYPE(cmp_vr), TCCIR_DECODE_VREG_POSITION(cmp_vr));
          int have_range = (cmp_slot >= 0 && ranges[cmp_slot].valid);
          /* Check if ALL reaching definitions of cmp_vr produce the
           * same fold result for this comparison, given the scoped
           * equality constraint on eq_scope_src_slot.  Each def must
           * be either an immediate constant or a non-lval load from
           * the constrained PARAM; any other def shape is unknown.
           * Skip when the CMP dereferences its source (is_lval) — the
           * constraint tracks the scalar value, not the pointed-to. */
          int64_t sf_cmp_val;
          if (!have_range && !src1.is_lval && eq_scope_src_slot >= 0 &&
              i < eq_scope_end && cmp_slot >= 0 &&
              vrp_read_const32(ir, src2, &sf_cmp_val)) {
            IROperand sf_cond_op = tcc_ir_op_get_src1(ir, jump_q);
            int sf_tok = (int)irop_get_imm64_ex(ir, sf_cond_op);
            int sf_unified = -2;
            int sf_safe = 1;
            for (int bi = 0; bi < i && sf_safe; bi++) {
              IRQuadCompact *bq = &ir->compact_instructions[bi];
              if (bq->op == TCCIR_OP_NOP || !irop_config[bq->op].has_dest)
                continue;
              IROperand bd = tcc_ir_op_get_dest(ir, bq);
              if (irop_get_vreg(bd) != cmp_vr)
                continue;
              int64_t def_val;
              IROperand bs = tcc_ir_op_get_src1(ir, bq);
              if ((bq->op == TCCIR_OP_ASSIGN || bq->op == TCCIR_OP_LOAD) &&
                  irop_is_immediate(bs)) {
                if (!vrp_read_const32(ir, bs, &def_val)) { sf_safe = 0; continue; }
              } else if (bq->op == TCCIR_OP_ASSIGN || bq->op == TCCIR_OP_LOAD) {
                int32_t bsv = irop_get_vreg(bs);
                if (bsv >= 0 && !bs.is_lval) {
                  int bs_slot = vrp_get_slot(TCCIR_DECODE_VREG_TYPE(bsv),
                                              TCCIR_DECODE_VREG_POSITION(bsv));
                  if (bs_slot == eq_scope_src_slot)
                    def_val = eq_scope_val;
                  else { sf_safe = 0; continue; }
                } else { sf_safe = 0; continue; }
              } else { sf_safe = 0; continue; }
              int sf_fold = evaluate_compare_condition(def_val, sf_cmp_val, sf_tok);
              if (sf_fold < 0) { sf_safe = 0; continue; }
              if (sf_unified == -2) sf_unified = sf_fold;
              else if (sf_unified != sf_fold) sf_safe = 0;
            }
            if (sf_safe && sf_unified >= 0) {
              ranges[cmp_slot].valid = 1;
              ranges[cmp_slot].min_val = eq_scope_val;
              ranges[cmp_slot].max_val = eq_scope_val;
              ranges_dirty = 1;
              have_range = 1;
            }
          }
          int64_t cmp_val;
          if (have_range && vrp_read_const32(ir, src2, &cmp_val))
          {
            int64_t rmin = ranges[cmp_slot].min_val;
            int64_t rmax = ranges[cmp_slot].max_val;
            IROperand set_src1_op = tcc_ir_op_get_src1(ir, jump_q);
            int tok = (int)irop_get_imm64_ex(ir, set_src1_op);
            int fold_result = -1;
            int is_eq_ne = (tok == 0x94 || tok == 0x95);

            if (is_eq_ne) {
              if (cmp_val < rmin || cmp_val > rmax)
                fold_result = (tok == 0x95) ? 1 : 0;
              else if (rmin == rmax)
                fold_result = (tok == 0x94) ? 1 : 0;
            } else {
              fold_result = vrp_fold_cmp(rmin, rmax, cmp_val, tok);
            }

            if (fold_result >= 0)
            {
              IROperand set_dest = tcc_ir_op_get_dest(ir, jump_q);
              q->op = TCCIR_OP_NOP;
              jump_q->op = TCCIR_OP_ASSIGN;
              IROperand const_val = irop_make_imm32(-1, fold_result, IROP_BTYPE_INT32);
              tcc_ir_set_src1(ir, i + 1, const_val);
              tcc_ir_op_set_dest(ir, jump_q, set_dest);
              changes++;
            }
          }
        }
      }
      continue;
    }

    /* Any other instruction writing to a tracked slot invalidates its range */
    int32_t dest_vr = irop_get_vreg(dest);
    if (dest_vr >= 0 && irop_config[q->op].has_dest)
    {
      int dst_slot = vrp_get_slot(TCCIR_DECODE_VREG_TYPE(dest_vr), TCCIR_DECODE_VREG_POSITION(dest_vr));
      if (dst_slot >= 0)
        ranges[dst_slot].valid = 0;
    }

    /* After instructions with no fall-through (JUMP, RETURN), clear all ranges
     * and discard pending constraints. The next linear instruction (if any) is
     * only reachable via its own predecessors, not from here. Without this,
     * constraints from one path leak to dead code or to instructions reached
     * from a different branch. */
    if (q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_RETURNVALUE || q->op == TCCIR_OP_RETURNVOID)
    {
      /* Before discarding the ranges, hand them to a forward target block
       * that this jump uniquely dominates (sole predecessor → not a merge
       * point), so the scan can reuse them when it gets there. */
      if (q->op == TCCIR_OP_JUMP)
      {
        int t = (int)irop_get_imm64_ex(ir, tcc_ir_op_get_dest(ir, q));
        if (t > i && t < n && !(is_merge[t / 8] & (1 << (t % 8))))
        {
          if (ranges_dirty)
          {
            memcpy(deferred_ranges, ranges, vrp_ranges_bytes);
            deferred_dirty = 1;
          }
          else
            deferred_dirty = 0;
          deferred_target = t;
        }
      }
      if (ranges_dirty)
      {
        memset(ranges, 0, vrp_ranges_bytes);
        ranges_dirty = 0;
      }
      pending_apply_at = -1;
      pending_slot = -1;
    }
  }

  tcc_free(is_merge);
  tcc_free(ranges);
  tcc_free(deferred_ranges);

  if (changes)
    changes += tcc_ir_opt_dce(ir);

  return changes;
}

/* ============================================================================
 * Redundant Loop Check Elimination
 *
 * When a loop guard ensures a condition (e.g., i < 4), any CMP+JUMPIF inside
 * the loop body that tests the same variable against the same constant with
 * an implied condition is redundant and can be folded.
 *
 * Pattern:
 *   header:  CMP V1, #4; JUMPIF >=U, exit    (guard: V1 < 4 in body)
 *   body:    V4 = V1; CMP V4, #4; JUMPIF <U  (redundant: always taken)
 * ============================================================================ */

static const char *nonneg_func_names[] = {
    "fabs", "fabsf", "abs", "labs", "llabs", "strlen", "sizeof",
};
#define NUM_NONNEG_FUNCS (sizeof(nonneg_func_names) / sizeof(nonneg_func_names[0]))

/* Flag-setting soft-float comparison function names.
 * __aeabi_cdcmple / __aeabi_cfcmple set ARM condition flags for a CMP-like
 * operation. The subsequent JUMPIF tests those flags with a TOK_* condition.
 * This is the default path used by TCC's soft-float FCMP lowering.
 */
static const char *flag_cmp_funcs[] = {
    "__aeabi_cdcmple",
    "__aeabi_cfcmple",
};
#define NUM_FLAG_CMP_FUNCS (sizeof(flag_cmp_funcs) / sizeof(flag_cmp_funcs[0]))

/* Maximum number of non-negative vregs to track simultaneously */
#define MAX_NONNEG_VREGS 32

/* Maximum number of pending call parameters to track */
#define MAX_PENDING_PARAMS 16

typedef struct
{
  int call_id;
  int param_idx;
  int32_t vreg;     /* -1 if immediate */
  int is_immediate; /* 1 if the parameter is an immediate value */
  int64_t imm_val;  /* immediate value (if is_immediate) */
} PendingParam;

int tcc_ir_opt_nonneg_branch_fold(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  int changes = 0;

  if (n < 3)
    return 0;

  /* Phase 1: Identify which vregs hold non-negative values.
   * We track full 32-bit vreg IDs (type + position). */
  int32_t nonneg_vregs[MAX_NONNEG_VREGS];
  int nonneg_count = 0;

  int pending_p0_is_imm = 0;
  int64_t pending_p0_imm = 0;
  int pending_p0_call_id = -1;

  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];

    if (q->op == TCCIR_OP_FUNCPARAMVAL)
    {
      IROperand ps1 = tcc_ir_op_get_src1(ir, q);
      IROperand ps2 = tcc_ir_op_get_src2(ir, q);
      uint32_t encoded = (uint32_t)irop_get_imm64_ex(ir, ps2);
      if (TCCIR_DECODE_PARAM_IDX(encoded) == 0)
      {
        pending_p0_is_imm = irop_is_immediate(ps1);
        pending_p0_imm = pending_p0_is_imm ? irop_get_imm64_ex(ir, ps1) : 0;
        pending_p0_call_id = TCCIR_DECODE_CALL_ID(encoded);
      }
      continue;
    }

    if (q->op != TCCIR_OP_FUNCCALLVAL)
    {
      if (q->op != TCCIR_OP_NOP && q->op != TCCIR_OP_FUNCPARAMVOID)
        pending_p0_call_id = -1;
      continue;
    }

    IROperand src1 = tcc_ir_op_get_src1(ir, q);
    Sym *callee = irop_get_sym_ex(ir, src1);
    if (!callee)
    {
      pending_p0_call_id = -1;
      continue;
    }

    const char *name = get_tok_str(callee->v, NULL);
    if (!name)
    {
      pending_p0_call_id = -1;
      continue;
    }

    int is_nonneg = 0;
    for (size_t j = 0; j < NUM_NONNEG_FUNCS; j++)
    {
      if (strcmp(name, nonneg_func_names[j]) == 0)
      {
        is_nonneg = 1;
        break;
      }
    }

    if (!is_nonneg && pending_p0_call_id >= 0 && pending_p0_is_imm)
    {
      IROperand src2 = tcc_ir_op_get_src2(ir, q);
      uint32_t call_encoded = (uint32_t)irop_get_imm64_ex(ir, src2);
      int this_call_id = TCCIR_DECODE_CALL_ID(call_encoded);
      if (this_call_id == pending_p0_call_id && strcmp(name, "__aeabi_f2d") == 0)
      {
        uint32_t fbits = (uint32_t)pending_p0_imm;
        uint32_t sign = (fbits >> 31) & 1;
        uint32_t exp = (fbits >> 23) & 0xFF;
        uint32_t mant = fbits & 0x7FFFFF;
        if (!sign && !(exp == 0xFF && mant != 0))
          is_nonneg = 1;
      }
    }

    pending_p0_call_id = -1;

    if (is_nonneg)
    {
      IROperand dest = tcc_ir_op_get_dest(ir, q);
      int32_t vreg = irop_get_vreg(dest);
      if (vreg >= 0 && nonneg_count < MAX_NONNEG_VREGS)
      {
        nonneg_vregs[nonneg_count++] = vreg;
        LOG_IR_GEN("NONNEG: vreg 0x%x is non-negative from call to '%s' at i=%d", vreg, name, i);
      }
    }
  }

  if (nonneg_count == 0)
    return 0;

  /* Phase 2: Find flag-setting soft-float comparison calls
   * (__aeabi_cdcmple / __aeabi_cfcmple) where:
   *   - Parameter 0 is a non-negative vreg and parameter 1 is zero (or vice versa)
   * Then determine the JUMPIF outcome from the condition token.
   *
   * cdcmple(a, b) sets flags as if CMP a, b. The JUMPIF condition token
   * directly encodes the comparison semantics (GE, LT, etc.).
   *
   * When a = nonneg >= 0 and b = 0:
   *   TOK_GE / TOK_UGE: nonneg >= 0 → ALWAYS TRUE  → jump always taken
   *   TOK_LT / TOK_ULT: nonneg <  0 → ALWAYS FALSE → jump never taken
   *   Others (EQ, NE, GT, LE): result depends on whether nonneg == 0 → UNKNOWN
   *
   * When a = 0 and b = nonneg >= 0 (reversed):
   *   TOK_LE / TOK_ULE: 0 <= nonneg → ALWAYS TRUE  → jump always taken
   *   TOK_GT / TOK_UGT: 0 >  nonneg → ALWAYS FALSE → jump never taken
   *   Others: UNKNOWN
   */

  PendingParam params[MAX_PENDING_PARAMS];
  int param_count = 0;

  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];

    /* Collect FUNCPARAMVAL instructions */
    if (q->op == TCCIR_OP_FUNCPARAMVAL)
    {
      IROperand src1 = tcc_ir_op_get_src1(ir, q);
      IROperand src2 = tcc_ir_op_get_src2(ir, q);
      uint32_t encoded = (uint32_t)irop_get_imm64_ex(ir, src2);
      int call_id = TCCIR_DECODE_CALL_ID(encoded);
      int param_idx = TCCIR_DECODE_PARAM_IDX(encoded);

      if (param_count < MAX_PENDING_PARAMS)
      {
        PendingParam *pp = &params[param_count++];
        pp->call_id = call_id;
        pp->param_idx = param_idx;
        pp->is_immediate = irop_is_immediate(src1);
        if (pp->is_immediate)
        {
          pp->vreg = -1;
          pp->imm_val = irop_get_imm64_ex(ir, src1);
        }
        else
        {
          pp->vreg = irop_get_vreg(src1);
          pp->imm_val = 0;
        }
      }
      continue;
    }

    /* Check FUNCCALLVOID for flag-setting soft-float comparison. */
    if (q->op != TCCIR_OP_FUNCCALLVOID)
    {
      if (q->op != TCCIR_OP_FUNCPARAMVOID && q->op != TCCIR_OP_NOP && q->op != TCCIR_OP_FUNCCALLVAL)
        param_count = 0;
      continue;
    }

    IROperand call_src1 = tcc_ir_op_get_src1(ir, q);
    IROperand call_src2 = tcc_ir_op_get_src2(ir, q);
    Sym *callee = irop_get_sym_ex(ir, call_src1);
    if (!callee)
    {
      param_count = 0;
      continue;
    }

    const char *cmp_name = get_tok_str(callee->v, NULL);
    if (!cmp_name)
    {
      param_count = 0;
      continue;
    }

    /* Check if this is a flag-setting comparison function */
    int is_flag_cmp = 0;
    for (size_t j = 0; j < NUM_FLAG_CMP_FUNCS; j++)
    {
      if (strcmp(cmp_name, flag_cmp_funcs[j]) == 0)
      {
        is_flag_cmp = 1;
        break;
      }
    }

    if (!is_flag_cmp)
    {
      param_count = 0;
      continue;
    }

    /* Found a flag-setting comparison. Extract call_id to match params. */
    uint32_t call_encoded = (uint32_t)irop_get_imm64_ex(ir, call_src2);
    int call_id = TCCIR_DECODE_CALL_ID(call_encoded);

    /* Find param 0 and param 1 for this call_id */
    PendingParam *p0 = NULL, *p1 = NULL;
    for (int p = 0; p < param_count; p++)
    {
      if (params[p].call_id == call_id)
      {
        if (params[p].param_idx == 0)
          p0 = &params[p];
        else if (params[p].param_idx == 1)
          p1 = &params[p];
      }
    }

    if (!p0 || !p1)
    {
      param_count = 0;
      continue;
    }

    /* Determine argument layout: which is nonneg and which is zero */
    int nonneg_is_arg0 = 0; /* 1 if cdcmple(nonneg, 0), 0 if cdcmple(0, nonneg) */
    int pattern_found = 0;

    /* Check pattern: param0 is non-negative vreg, param1 is zero */
    if (!p0->is_immediate && p0->vreg >= 0 && p1->is_immediate && p1->imm_val == 0)
    {
      for (int k = 0; k < nonneg_count; k++)
      {
        if (nonneg_vregs[k] == p0->vreg)
        {
          nonneg_is_arg0 = 1;
          pattern_found = 1;
          break;
        }
      }
    }
    /* Check reverse: param0 is zero, param1 is non-negative vreg */
    else if (p0->is_immediate && p0->imm_val == 0 && !p1->is_immediate && p1->vreg >= 0)
    {
      for (int k = 0; k < nonneg_count; k++)
      {
        if (nonneg_vregs[k] == p1->vreg)
        {
          nonneg_is_arg0 = 0;
          pattern_found = 1;
          break;
        }
      }
    }

    if (!pattern_found)
    {
      param_count = 0;
      continue;
    }

    /* Find the JUMPIF that follows this FUNCCALLVOID.
     * It should be the very next non-NOP instruction. */
    int jumpif_idx = -1;
    for (int j = i + 1; j < n && j <= i + 3; j++)
    {
      if (ir->compact_instructions[j].op == TCCIR_OP_NOP)
        continue;
      if (ir->compact_instructions[j].op == TCCIR_OP_JUMPIF)
      {
        jumpif_idx = j;
        break;
      }
      break;
    }

    if (jumpif_idx < 0)
    {
      param_count = 0;
      continue;
    }

    IRQuadCompact *jump_q = &ir->compact_instructions[jumpif_idx];
    IROperand jmp_cond = tcc_ir_op_get_src1(ir, jump_q);
    IROperand jmp_dest = tcc_ir_op_get_dest(ir, jump_q);
    int cond_tok = (int)irop_get_imm64_ex(ir, jmp_cond);

    /* Determine if the branch is always/never taken based on
     * the condition token and which argument is non-negative.
     *
     * cdcmple(a, b) sets flags for "a CMP b".
     * JUMPIF condition tests those flags. */
    int fold_result = -1; /* -1 = unknown, 0 = never taken, 1 = always taken */

    if (nonneg_is_arg0)
    {
      /* cdcmple(nonneg, 0): flags for "nonneg CMP 0" */
      switch (cond_tok)
      {
      case TOK_GE:
      case TOK_UGE:
        fold_result = 1; /* nonneg >= 0: always true */
        break;
      case TOK_LT:
      case TOK_ULT:
        fold_result = 0; /* nonneg < 0: always false */
        break;
      default:
        fold_result = -1; /* unknown */
        break;
      }
    }
    else
    {
      /* cdcmple(0, nonneg): flags for "0 CMP nonneg" */
      switch (cond_tok)
      {
      case TOK_LE:
      case TOK_ULE:
        fold_result = 1; /* 0 <= nonneg: always true */
        break;
      case TOK_GT:
      case TOK_UGT:
        fold_result = 0; /* 0 > nonneg: always false */
        break;
      default:
        fold_result = -1;
        break;
      }
    }

    if (fold_result < 0)
    {
      param_count = 0;
      continue;
    }

    if (fold_result == 1)
    {
      /* Branch always taken → convert JUMPIF to unconditional JUMP. */
      jump_q->op = TCCIR_OP_JUMP;
      tcc_ir_set_dest(ir, jumpif_idx, jmp_dest);
      LOG_IR_GEN("NONNEG FOLD: %s(nonneg, 0) at i=%d, JUMPIF cond=0x%x at %d "
                 "-> always taken, unconditional JUMP to %d",
                 cmp_name, i, cond_tok, jumpif_idx, (int)jmp_dest.u.imm32);
      changes++;
    }
    else
    {
      /* Branch never taken → NOP out the JUMPIF. */
      jump_q->op = TCCIR_OP_NOP;
      LOG_IR_GEN("NONNEG FOLD: %s(nonneg, 0) at i=%d, JUMPIF cond=0x%x at %d "
                 "-> never taken, eliminated",
                 cmp_name, i, cond_tok, jumpif_idx);
      changes++;
    }

    param_count = 0;
  }

  /* Run DCE to clean up dead code after folded branches */
  if (changes)
    changes += tcc_ir_opt_dce(ir);

  return changes;
}

/* ============================================================================
 * Float Narrowing Optimization
 * ============================================================================
 *
 * Replaces double-precision math function calls with float-precision variants
 * when the argument was promoted from float and/or the result is demoted back
 * to float.
 *
 * This is valid for functions where (float)func((double)x) == funcf(x) for
 * all float x. These are "integer-valued" or "magnitude-preserving" functions:
 *   floor → floorf, ceil → ceilf, trunc → truncf, round → roundf,
 *   fabs → fabsf, nearbyint → nearbyintf, rint → rintf
 *
 * NOT valid for: sin, cos, tan, sqrt, exp, log, pow (precision-dependent).
 *
 * Pattern detected in IR (soft-float):
 *
 * Case 1: Result demoted back to float
 *   FUNCPARAMVAL float_arg, [call_A, 0]
 *   FUNCCALLVAL __aeabi_f2d → T_double      ; float-to-double
 *   FUNCPARAMVAL T_double, [call_B, 0]
 *   FUNCCALLVAL floor → T_result            ; double-precision math func
 *   FUNCPARAMVAL T_result, [call_C, 0]
 *   FUNCCALLVAL __aeabi_d2f → T_float       ; double-to-float
 *
 *   Transformed to:
 *   FUNCPARAMVAL float_arg, [call_B, 0]
 *   FUNCCALLVAL floorf → T_float             ; float-precision variant
 *   (f2d and d2f calls NOP'd out)
 *
 * Case 2: Result stays double (e.g., double q1(float a) { return floor(a); })
 *   FUNCPARAMVAL float_arg, [call_A, 0]
 *   FUNCCALLVAL __aeabi_f2d → T_double
 *   FUNCPARAMVAL T_double, [call_B, 0]
 *   FUNCCALLVAL floor → T_result
 *
 *   Transformed by swapping callees (f2d moves after the function):
 *   FUNCPARAMVAL float_arg, [call_A, 0]
 *   FUNCCALLVAL floorf → T_float_result      ; now calls floorf
 *   FUNCPARAMVAL T_float_result, [call_B, 0]
 *   FUNCCALLVAL __aeabi_f2d → T_result       ; now widens result to double
 */

#define STACK_CSE_MAX_ENTRIES 32


int tcc_ir_opt_branch_folding(TCCIRState *ir)
{
  if (tcc_ir_opt_pass_disabled("branch_fold")) return 0;
  if (ir->next_instruction_index < 2)
    return 0;
  IROptCtx ctx;
  tcc_ir_opt_ctx_init(&ctx, ir);
  int changes = tcc_ir_opt_run_gens(&ctx, branch_gens, branch_gens_count);
  tcc_ir_opt_ctx_free(&ctx);
  return changes;
}

/* ============================================================================
 * Stack Address Non-Null Branch Folding
 * ============================================================================
 *
 * Stack addresses (Addr[StackLoc[X]]) are always non-zero on any real target.
 * When inlined code null-checks a pointer that is actually a stack address,
 * the CMP + JUMPIF is dead and can be folded away.
 *
 * Phase 1: Identify vregs that hold stack addresses by scanning for:
 *   - ASSIGN/LEA with src1 having is_local=1, is_lval=0 (address-of-stack)
 *   - STORE from a tracked temp into a VAR (propagate through store)
 *
 * Phase 2: Fold CMP(tracked_vreg, #0) + JUMPIF:
 *   - EQ: always false  (stack addr != 0) → NOP both
 *   - NE: always true   (stack addr != 0) → unconditional JUMP
 */
#define MAX_STACKADDR_VREGS 64


int tcc_ir_opt_stack_addr_nonnull_fold(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  int changes = 0;

  if (n < 3)
    return 0;

  LOG_IR_GEN("=== STACK ADDR NONNULL FOLD START ===");

  /* Phase 1: Identify TEMP vregs that hold stack addresses (single-assignment,
   * always safe). Also build a flow-sensitive bitmap for VAR vregs. */
  int32_t sa_vregs[MAX_STACKADDR_VREGS];
  int64_t sa_offsets[MAX_STACKADDR_VREGS];
  /* Parallel: the VAR position this TEMP points at (-1 if unknown, e.g. when
   * the TEMP holds a SP-after-alloca address that isn't &V for any V).  Used
   * to propagate non-null status through STORE through a LEA pointer. */
  int32_t sa_target_var[MAX_STACKADDR_VREGS];
  int sa_count = 0;

  /* Flow-sensitive VAR tracking: var_holds_stackaddr[pos] is 1 if the VAR
   * at that position currently holds a stack address, 0 otherwise.
   * Reset at jump targets (control flow merge points) for safety. */
#define MAX_TRACKED_VARS 256
  uint8_t var_holds_stackaddr[MAX_TRACKED_VARS];
  int64_t var_stackaddr_offset[MAX_TRACKED_VARS];
  /* var_target_var[pos] is the VAR position this VAR's value points AT, when
   * known (e.g. after `V = &W`).  -1 if the address is non-null but doesn't
   * resolve to a known VAR (e.g. SP-after-alloca). */
  int32_t var_target_var[MAX_TRACKED_VARS];
  memset(var_holds_stackaddr, 0, sizeof(var_holds_stackaddr));
  for (int k = 0; k < MAX_TRACKED_VARS; k++)
    var_target_var[k] = -1;

  /* Single forward pass: track TEMPs and VARs, fold CMP+JUMPIF inline. */
  for (int i = 0; i < n - 1; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;

    /* At jump targets, conservatively reset all VAR tracking.
     * A jump target is a merge point where different paths may have
     * assigned different values to the same VAR. */
    if (q->is_jump_target)
    {
      memset(var_holds_stackaddr, 0, sizeof(var_holds_stackaddr));
      for (int k = 0; k < MAX_TRACKED_VARS; k++)
        var_target_var[k] = -1;
    }

    /* Function calls may rewrite any address-taken VAR through pointers passed
     * by reference.  Invalidate all VAR stack-addr tracking before processing
     * the call body so the post-call CMPs don't assume stale stack-addr state. */
    if (q->op == TCCIR_OP_FUNCCALLVAL || q->op == TCCIR_OP_FUNCCALLVOID)
    {
      memset(var_holds_stackaddr, 0, sizeof(var_holds_stackaddr));
      for (int k = 0; k < MAX_TRACKED_VARS; k++)
        var_target_var[k] = -1;
    }

    /* Track TEMPs assigned stack addresses */
    if (q->op == TCCIR_OP_ASSIGN || q->op == TCCIR_OP_LEA || q->op == TCCIR_OP_LOAD)
    {
      IROperand src1 = tcc_ir_op_get_src1(ir, q);
      IROperand dest = tcc_ir_op_get_dest(ir, q);
      int32_t dvr = irop_get_vreg(dest);
      if (is_stack_address_operand(src1))
      {
        if (dvr >= 0 && TCCIR_DECODE_VREG_TYPE(dvr) == TCCIR_VREG_TYPE_TEMP &&
            sa_count < MAX_STACKADDR_VREGS)
        {
          int32_t src_vr = irop_get_vreg(src1);
          int32_t tgt_var = -1;
          if (src_vr >= 0 && TCCIR_DECODE_VREG_TYPE(src_vr) == TCCIR_VREG_TYPE_VAR)
            tgt_var = TCCIR_DECODE_VREG_POSITION(src_vr);
          sa_offsets[sa_count] = irop_get_stack_offset(src1);
          sa_target_var[sa_count] = tgt_var;
          sa_vregs[sa_count++] = dvr;
          LOG_IR_GEN("STACKADDR: TEMP 0x%x = &VAR (target=%d) at i=%d", dvr, tgt_var, i);
        }
      }
      else if (dvr >= 0 && TCCIR_DECODE_VREG_TYPE(dvr) == TCCIR_VREG_TYPE_TEMP &&
               sa_count < MAX_STACKADDR_VREGS)
      {
        /* TEMP-dest copy: propagate from src if it's a tracked VAR or TEMP. */
        int32_t svr = irop_get_vreg(src1);
        if (svr >= 0)
        {
          int skind = TCCIR_DECODE_VREG_TYPE(svr);
          if (skind == TCCIR_VREG_TYPE_VAR)
          {
            int spos = TCCIR_DECODE_VREG_POSITION(svr);
            if (spos < MAX_TRACKED_VARS && var_holds_stackaddr[spos])
            {
              sa_offsets[sa_count] = var_stackaddr_offset[spos];
              sa_target_var[sa_count] = var_target_var[spos];
              sa_vregs[sa_count++] = dvr;
              LOG_IR_GEN("STACKADDR: TEMP 0x%x inherits from VAR V%d (target=%d) at i=%d",
                         dvr, spos, var_target_var[spos], i);
            }
          }
          else if (skind == TCCIR_VREG_TYPE_TEMP && !src1.is_lval)
          {
            for (int k = 0; k < sa_count; k++)
            {
              if (sa_vregs[k] == svr)
              {
                sa_offsets[sa_count] = sa_offsets[k];
                sa_target_var[sa_count] = sa_target_var[k];
                sa_vregs[sa_count++] = dvr;
                LOG_IR_GEN("STACKADDR: TEMP 0x%x inherits from TEMP at i=%d", dvr, i);
                break;
              }
            }
          }
        }
      }
    }

    /* VLA_SP_SAVE writes the current SP to its destination.  After the
     * alloca-load-fwd pass, the destination may be a vreg (rather than a
     * stack slot).  The current SP is by construction a stack address —
     * track it as non-null so subsequent users of the alloca pointer can
     * fold null checks. */
    if (q->op == TCCIR_OP_VLA_SP_SAVE)
    {
      IROperand dest = tcc_ir_op_get_dest(ir, q);
      int32_t dvr = irop_get_vreg(dest);
      if (dvr >= 0 && sa_count < MAX_STACKADDR_VREGS)
      {
        int dkind = TCCIR_DECODE_VREG_TYPE(dvr);
        if (dkind == TCCIR_VREG_TYPE_TEMP)
        {
          sa_offsets[sa_count] = 0; /* dynamic offset */
          sa_target_var[sa_count] = -1;
          sa_vregs[sa_count++] = dvr;
          LOG_IR_GEN("STACKADDR: VLA_SP_SAVE -> TEMP 0x%x at i=%d", dvr, i);
        }
        else if (dkind == TCCIR_VREG_TYPE_VAR)
        {
          int dpos = TCCIR_DECODE_VREG_POSITION(dvr);
          if (dpos < MAX_TRACKED_VARS)
          {
            var_holds_stackaddr[dpos] = 1;
            var_stackaddr_offset[dpos] = 0;
            LOG_IR_GEN("STACKADDR: VLA_SP_SAVE -> VAR V%d at i=%d", dpos, i);
          }
        }
      }
    }

    /* STORE through a LEA pointer: *T = src where T is in sa_vregs with a
     * known target VAR, and src is itself a stack address.  Propagate the
     * stack-address property into the target VAR's slot — this captures
     * the `*p = n` pattern (writing an alloca result through a known
     * pointer-to-local) so the subsequent null check on the local folds. */
    if (q->op == TCCIR_OP_STORE)
    {
      IROperand dest = tcc_ir_op_get_dest(ir, q);
      int32_t dvr = irop_get_vreg(dest);
      if (dvr >= 0 && TCCIR_DECODE_VREG_TYPE(dvr) == TCCIR_VREG_TYPE_TEMP && dest.is_lval)
      {
        int32_t tgt_var = -1;
        int64_t base_off = 0;
        int dest_is_sa = 0;
        for (int k = 0; k < sa_count; k++)
        {
          if (sa_vregs[k] == dvr)
          {
            tgt_var = sa_target_var[k];
            base_off = sa_offsets[k];
            dest_is_sa = 1;
            break;
          }
        }
        if (tgt_var >= 0 && tgt_var < MAX_TRACKED_VARS)
        {
          IROperand src1 = tcc_ir_op_get_src1(ir, q);
          int src_is_sa = is_stack_address_operand(src1);
          int64_t src_off = src_is_sa ? irop_get_stack_offset(src1) : 0;
          if (!src_is_sa)
          {
            int32_t svr = irop_get_vreg(src1);
            if (svr >= 0)
            {
              if (TCCIR_DECODE_VREG_TYPE(svr) == TCCIR_VREG_TYPE_TEMP && !src1.is_lval)
              {
                for (int k = 0; k < sa_count; k++)
                {
                  if (sa_vregs[k] == svr)
                  {
                    src_is_sa = 1;
                    src_off = sa_offsets[k];
                    break;
                  }
                }
              }
              else if (TCCIR_DECODE_VREG_TYPE(svr) == TCCIR_VREG_TYPE_VAR)
              {
                int sp = TCCIR_DECODE_VREG_POSITION(svr);
                if (sp < MAX_TRACKED_VARS && var_holds_stackaddr[sp])
                {
                  src_is_sa = 1;
                  src_off = var_stackaddr_offset[sp];
                }
              }
            }
          }
          if (src_is_sa)
          {
            var_holds_stackaddr[tgt_var] = 1;
            var_stackaddr_offset[tgt_var] = src_off;
            LOG_IR_GEN("STACKADDR: indirect STORE *T(0x%x) at i=%d marks V%d non-null", dvr, i, tgt_var);
            (void)base_off;
          }
        }
        else if (!dest_is_sa)
        {
          /* STORE through a TEMP we couldn't resolve at all (not in sa_vregs)
           * — the pointer is unknown and might alias any address-taken local,
           * so invalidate VAR stack-addr tracking conservatively.  When the
           * TEMP IS in sa_vregs but with target_var=-1 (e.g. alloca result),
           * the destination is a known stack region that doesn't alias any
           * local VAR's slot, so no invalidation is required. */
          memset(var_holds_stackaddr, 0, sizeof(var_holds_stackaddr));
          for (int k = 0; k < MAX_TRACKED_VARS; k++)
            var_target_var[k] = -1;
        }
      }
    }

    /* ADD: stack_addr + constant → result is also a stack address (non-null).
     * Covers patterns like &arr + 24 (element offset).
     * src1 can be either a tracked TEMP or a direct Addr[StackLoc] operand. */
    if (q->op == TCCIR_OP_ADD)
    {
      IROperand dest = tcc_ir_op_get_dest(ir, q);
      int32_t d_vr = irop_get_vreg(dest);
      if (d_vr >= 0 && TCCIR_DECODE_VREG_TYPE(d_vr) == TCCIR_VREG_TYPE_TEMP)
      {
        IROperand s1 = tcc_ir_op_get_src1(ir, q);
        IROperand s2 = tcc_ir_op_get_src2(ir, q);
        int s1_is_sa = is_stack_address_operand(s1);
        if (!s1_is_sa)
        {
          int32_t s1_vr = irop_get_vreg(s1);
          if (s1_vr >= 0 && TCCIR_DECODE_VREG_TYPE(s1_vr) == TCCIR_VREG_TYPE_TEMP)
          {
            for (int k = 0; k < sa_count; k++)
            {
              if (sa_vregs[k] == s1_vr)
              {
                s1_is_sa = 1;
                break;
              }
            }
          }
        }
        if (s1_is_sa && sa_count < MAX_STACKADDR_VREGS)
        {
          int s2_nonneg = irop_is_immediate(s2);
          if (!s2_nonneg)
          {
            int32_t s2_vr = irop_get_vreg(s2);
            if (s2_vr >= 0 && TCCIR_DECODE_VREG_TYPE(s2_vr) == TCCIR_VREG_TYPE_TEMP)
            {
              int def = tcc_ir_find_defining_instruction(ir, s2_vr, i);
              if (def >= 0 && ir->compact_instructions[def].op == TCCIR_OP_MUL)
              {
                IROperand mul_s2 = tcc_ir_op_get_src2(ir, &ir->compact_instructions[def]);
                if (irop_is_immediate(mul_s2) && irop_get_imm64_ex(ir, mul_s2) > 0)
                  s2_nonneg = 1;
              }
            }
          }
          if (!s2_nonneg)
            goto skip_sa_add;
          {
            int64_t base_off = 0;
            if (is_stack_address_operand(s1))
            {
              base_off = irop_get_stack_offset(s1);
            }
            else
            {
              int32_t s1_vr = irop_get_vreg(s1);
              for (int k = 0; k < sa_count; k++)
              {
                if (sa_vregs[k] == s1_vr)
                {
                  base_off = sa_offsets[k];
                  break;
                }
              }
            }
            if (irop_is_immediate(s2))
              sa_offsets[sa_count] = base_off + irop_get_imm64_ex(ir, s2);
            else
              sa_offsets[sa_count] = base_off;
            sa_vregs[sa_count++] = d_vr;
          }
        skip_sa_add:;
        }
      }
    }

    /* Flow-sensitive VAR tracking through STORE, ASSIGN, LOAD, and LEA.
     * After SL-FWD, a LOAD V=StackLoc may become ASSIGN V=T (forwarded). */
    if (q->op == TCCIR_OP_STORE || q->op == TCCIR_OP_ASSIGN || q->op == TCCIR_OP_LOAD ||
        q->op == TCCIR_OP_LEA)
    {
      IROperand dest = tcc_ir_op_get_dest(ir, q);
      int32_t dvreg = irop_get_vreg(dest);
      /* `STORE V***DEREF*** = src` with VREG-tagged dest is a deref-store
       * (writing through V's pointer value), not a slot-write to V.  The
       * VAR's value is unchanged, so don't touch its tracking; the STORE
       * may still alias other addrtaken VARs, handled elsewhere. */
      int skip_var_track = (q->op == TCCIR_OP_STORE && dest.is_lval &&
                            irop_get_tag(dest) == IROP_TAG_VREG &&
                            dvreg >= 0 && TCCIR_DECODE_VREG_TYPE(dvreg) == TCCIR_VREG_TYPE_VAR);
      if (!skip_var_track && dvreg >= 0 && TCCIR_DECODE_VREG_TYPE(dvreg) == TCCIR_VREG_TYPE_VAR)
      {
        int pos = TCCIR_DECODE_VREG_POSITION(dvreg);
        if (pos < MAX_TRACKED_VARS)
        {
          IROperand src1 = tcc_ir_op_get_src1(ir, q);
          int src_is_stackaddr = 0;
          int64_t src_offset = 0;
          int32_t src_target_var = -1;
          if (is_stack_address_operand(src1))
          {
            src_is_stackaddr = 1;
            src_offset = irop_get_stack_offset(src1);
            int32_t src1_vr = irop_get_vreg(src1);
            if (src1_vr >= 0 && TCCIR_DECODE_VREG_TYPE(src1_vr) == TCCIR_VREG_TYPE_VAR)
              src_target_var = TCCIR_DECODE_VREG_POSITION(src1_vr);
          }
          else
          {
            int32_t svreg = irop_get_vreg(src1);
            if (svreg >= 0)
            {
              if (TCCIR_DECODE_VREG_TYPE(svreg) == TCCIR_VREG_TYPE_TEMP && !src1.is_lval)
              {
                /* TEMP with is_lval=0: src1's value IS the TEMP's value.
                 * TEMP with is_lval=1 is `*T` — a memory load that does NOT
                 * yield the TEMP's value, so it doesn't inherit stack-addr. */
                for (int k = 0; k < sa_count; k++)
                {
                  if (sa_vregs[k] == svreg)
                  {
                    src_is_stackaddr = 1;
                    src_offset = sa_offsets[k];
                    src_target_var = sa_target_var[k];
                    break;
                  }
                }
              }
              else if (TCCIR_DECODE_VREG_TYPE(svreg) == TCCIR_VREG_TYPE_VAR)
              {
                int spos = TCCIR_DECODE_VREG_POSITION(svreg);
                if (spos < MAX_TRACKED_VARS && var_holds_stackaddr[spos])
                {
                  src_is_stackaddr = 1;
                  src_offset = var_stackaddr_offset[spos];
                  src_target_var = var_target_var[spos];
                }
              }
            }
          }
          var_holds_stackaddr[pos] = src_is_stackaddr;
          var_stackaddr_offset[pos] = src_offset;
          var_target_var[pos] = src_is_stackaddr ? src_target_var : -1;
        }
      }
    }

    /* ADD/SUB on a VAR: update tracked stack address offset, or invalidate. */
    if (q->op == TCCIR_OP_ADD || q->op == TCCIR_OP_SUB)
    {
      IROperand dest = tcc_ir_op_get_dest(ir, q);
      int32_t dvreg = irop_get_vreg(dest);
      if (dvreg >= 0 && TCCIR_DECODE_VREG_TYPE(dvreg) == TCCIR_VREG_TYPE_VAR)
      {
        int pos = TCCIR_DECODE_VREG_POSITION(dvreg);
        if (pos < MAX_TRACKED_VARS)
        {
          IROperand s1 = tcc_ir_op_get_src1(ir, q);
          IROperand s2 = tcc_ir_op_get_src2(ir, q);
          int32_t s1vr = irop_get_vreg(s1);
          int updated = 0;
          if (s1vr >= 0 && TCCIR_DECODE_VREG_TYPE(s1vr) == TCCIR_VREG_TYPE_VAR &&
              TCCIR_DECODE_VREG_POSITION(s1vr) == pos && var_holds_stackaddr[pos] && irop_is_immediate(s2))
          {
            int64_t delta = irop_get_imm64_ex(ir, s2);
            if (q->op == TCCIR_OP_SUB)
              delta = -delta;
            var_stackaddr_offset[pos] += delta;
            updated = 1;
          }
          if (!updated)
            var_holds_stackaddr[pos] = 0;
        }
      }
    }

    /* Check for CMP(stackaddr_vreg, #0) + JUMPIF pattern */
    if (q->op != TCCIR_OP_CMP)
      continue;

    IROperand src1 = tcc_ir_op_get_src1(ir, q);
    IROperand src2 = tcc_ir_op_get_src2(ir, q);

    /* Helper: check if a non-immediate operand holds a stack address */
    int matched = 0;
    int nonnull_is_src1 = 0;

    /* For TEMPs, is_lval=1 means pointer dereference (comparing *ptr, not ptr).
     * For VARs, is_lval=1 is normal — reading the VAR's value. A VAR tracked
     * as holding a stack address has that address AS its value, so comparing
     * it against 0 is a null-pointer check we can fold. */
    if (irop_is_immediate(src2) && irop_get_imm64_ex(ir, src2) == 0 && !irop_is_immediate(src1))
    {
      int32_t vr = irop_get_vreg(src1);
      if (vr >= 0)
      {
        if (TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_TEMP && !src1.is_lval)
        {
          for (int k = 0; k < sa_count; k++)
          {
            if (sa_vregs[k] == vr)
            {
              matched = 1;
              break;
            }
          }
        }
        else if (TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_VAR)
        {
          int pos = TCCIR_DECODE_VREG_POSITION(vr);
          if (pos < MAX_TRACKED_VARS && var_holds_stackaddr[pos])
            matched = 1;
        }
        if (matched)
          nonnull_is_src1 = 1;
      }
    }
    else if (irop_is_immediate(src1) && irop_get_imm64_ex(ir, src1) == 0 && !irop_is_immediate(src2))
    {
      int32_t vr = irop_get_vreg(src2);
      if (vr >= 0)
      {
        if (TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_TEMP && !src2.is_lval)
        {
          for (int k = 0; k < sa_count; k++)
          {
            if (sa_vregs[k] == vr)
            {
              matched = 1;
              break;
            }
          }
        }
        else if (TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_VAR)
        {
          int pos = TCCIR_DECODE_VREG_POSITION(vr);
          if (pos < MAX_TRACKED_VARS && var_holds_stackaddr[pos])
            matched = 1;
        }
        if (matched)
          nonnull_is_src1 = 0;
      }
    }

    /* Same-address comparison: CMP(sa_A, sa_B) where both are the same
     * known stack address. */
    if (!matched)
    {
      int sa1_valid = 0, sa2_valid = 0;
      int64_t off1 = 0, off2 = 0;
      if (is_stack_address_operand(src1))
      {
        sa1_valid = 1;
        off1 = irop_get_stack_offset(src1);
      }
      else
      {
        int32_t vr1 = irop_get_vreg(src1);
        if (vr1 >= 0)
        {
          if (TCCIR_DECODE_VREG_TYPE(vr1) == TCCIR_VREG_TYPE_TEMP && !src1.is_lval)
          {
            for (int k = 0; k < sa_count; k++)
            {
              if (sa_vregs[k] == vr1)
              {
                sa1_valid = 1;
                off1 = sa_offsets[k];
                break;
              }
            }
          }
          else if (TCCIR_DECODE_VREG_TYPE(vr1) == TCCIR_VREG_TYPE_VAR)
          {
            int pos = TCCIR_DECODE_VREG_POSITION(vr1);
            if (pos < MAX_TRACKED_VARS && var_holds_stackaddr[pos])
            {
              sa1_valid = 1;
              off1 = var_stackaddr_offset[pos];
            }
          }
        }
      }
      if (is_stack_address_operand(src2))
      {
        sa2_valid = 1;
        off2 = irop_get_stack_offset(src2);
      }
      else
      {
        int32_t vr2 = irop_get_vreg(src2);
        if (vr2 >= 0)
        {
          if (TCCIR_DECODE_VREG_TYPE(vr2) == TCCIR_VREG_TYPE_TEMP && !src2.is_lval)
          {
            for (int k = 0; k < sa_count; k++)
            {
              if (sa_vregs[k] == vr2)
              {
                sa2_valid = 1;
                off2 = sa_offsets[k];
                break;
              }
            }
          }
          else if (TCCIR_DECODE_VREG_TYPE(vr2) == TCCIR_VREG_TYPE_VAR)
          {
            int pos = TCCIR_DECODE_VREG_POSITION(vr2);
            if (pos < MAX_TRACKED_VARS && var_holds_stackaddr[pos])
            {
              sa2_valid = 1;
              off2 = var_stackaddr_offset[pos];
            }
          }
        }
      }
      if (sa1_valid && sa2_valid && off1 == off2)
        matched = 2;
      /* Different stack addresses are provably distinct: distinct VARs each
       * occupy their own slot, so EQ/NE can be folded.  Only safe for EQ/NE
       * (relative orderings on pointers from different objects are UB in C). */
      else if (sa1_valid && sa2_valid && off1 != off2)
        matched = 3;
    }

    if (!matched)
      continue;

    /* Find next JUMPIF (skip NOPs) */
    int j = i + 1;
    while (j < n && ir->compact_instructions[j].op == TCCIR_OP_NOP)
      j++;
    if (j >= n)
      continue;
    IRQuadCompact *jump_q = &ir->compact_instructions[j];
    if (jump_q->op != TCCIR_OP_JUMPIF)
      continue;

    IROperand cond = tcc_ir_op_get_src1(ir, jump_q);
    int tok = (int)irop_get_imm64_ex(ir, cond);

    /* Stack address is always > 0 (unsigned), always != 0 */
    int fold_result = -1;

    if (matched == 2)
    {
      fold_result = evaluate_compare_condition(0, 0, tok);
    }
    else if (matched == 3)
    {
      /* Distinct stack addresses: EQ→0, NE→1.  Skip ordering tokens. */
      if (tok == 0x94)
        fold_result = 0;
      else if (tok == 0x95)
        fold_result = 1;
    }
    else
    {
      (void)nonnull_is_src1;
      switch (tok)
      {
      case 0x94:
        fold_result = 0;
        break;
      case 0x95:
        fold_result = 1;
        break;
      default:
        break;
      }
    }

    if (fold_result < 0)
      continue;

    if (fold_result)
    {
      IROperand dest = tcc_ir_op_get_dest(ir, jump_q);
      q->op = TCCIR_OP_NOP;
      jump_q->op = TCCIR_OP_JUMP;
      tcc_ir_set_dest(ir, j, dest);
      LOG_IR_GEN("STACKADDR FOLD: CMP(stackaddr, 0) with EQ/NE -> unconditional JUMP at i=%d", i);
    }
    else
    {
      q->op = TCCIR_OP_NOP;
      jump_q->op = TCCIR_OP_NOP;
      LOG_IR_GEN("STACKADDR FOLD: CMP(stackaddr, 0) == 0 -> NOP both at i=%d", i);
    }
    changes++;
  }

  LOG_IR_GEN("=== STACK ADDR NONNULL FOLD END: %d branches folded ===", changes);

  return changes;
}

/* setif_branch_fuse: see ir/opt_gens_branch.c for generator implementation */
int tcc_ir_opt_setif_branch_fuse(TCCIRState *ir)
{
  if (ir->next_instruction_index < 4)
    return 0;
  IROptCtx ctx;
  tcc_ir_opt_ctx_init(&ctx, ir);
  int changes = tcc_ir_opt_run_gens(&ctx, branch_gens, branch_gens_count);
  tcc_ir_opt_ctx_free(&ctx);
  return changes;
}

/* ============================================================================
 * Stack-Boolean-Diamond Peephole
 * ============================================================================
 *
 * An inlined bool-returning helper typically lowers to a pair of constant
 * stores feeding a single reload+test:
 *
 *   i:   STORE slot,#A
 *   i+1: JUMP to L                 (L = i+3)
 *   i+2: STORE slot,#B             (target of some earlier JUMPIF)
 *   i+3: TEST_ZERO slot            (L)
 *   i+4: JUMPIF cond, T
 *
 * When the slot has no other references, both stores and the reload are dead.
 * Each arm's exit is pre-determined by its constant and the final condition,
 * so both can branch directly to the correct landing:
 *
 *   i:   NOP
 *   i+1: JUMP to (cond(A) ? T : i+5)
 *   i+2: NOP
 *   i+3: NOP
 *   i+4: JUMP to (cond(B) ? T : i+5)
 *
 * Because the slot is single-purpose (only our 3 references), no live value
 * depends on it; any other predecessor reaching i+3 without writing the slot
 * is already undefined behavior.
 */

int tcc_ir_opt_stack_bool_diamond(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  int changes = 0;

  if (n < 6)
    return 0;

  LOG_IR_GEN("=== STACK BOOL DIAMOND START ===");

  /* Iterate on `merge` (= q_d's index, the TEST_ZERO).  The required shape:
   *
   *   q_a = ir[q_b_idx - 1]   STORE slot, #A           (jump-true arm)
   *   q_b = ir[q_b_idx]       JUMP to merge
   *   ... arbitrary straight-line code, may include CALL ...
   *   q_c = ir[merge - 1]     STORE slot, #B           (fallthrough arm)
   *   q_d = ir[merge]         TEST_ZERO slot
   *   q_e = ir[merge + 1]     JUMPIF cond, T
   *
   * The strict-adjacent layout (q_b_idx = merge - 3) is one instance of
   * this; the spread-out form arises when the fallthrough arm contains an
   * inlined helper's body (e.g. PARAM setup + CALL printf). */
  for (int merge = 2; merge + 1 < n; merge++)
  {
    IRQuadCompact *q3 = &ir->compact_instructions[merge];
    IRQuadCompact *q4 = &ir->compact_instructions[merge + 1];

    if (q3->op != TCCIR_OP_TEST_ZERO)
      continue;
    if (q4->op != TCCIR_OP_JUMPIF)
      continue;
    if (q4->is_jump_target)
      continue;

    /* Locate q_c (the fall-into-merge STORE).  Two layouts:
     *   post-rotation: ir[merge-1] is the STORE; falls through to merge.
     *   pre-rotation:  ir[merge-1] is a redundant `JUMP merge`, ir[merge-2]
     *                  is the STORE.  Loop rotation + fall-through
     *                  elimination would normally collapse this, but the
     *                  diamond pass also runs earlier in the pipeline. */
    int q_c_idx = -1;
    int extra_jmp = -1; /* idx of the redundant `JUMP merge` (pre-rotation) */
    if (ir->compact_instructions[merge - 1].op == TCCIR_OP_STORE)
    {
      q_c_idx = merge - 1;
    }
    else if (merge >= 2 && ir->compact_instructions[merge - 1].op == TCCIR_OP_JUMP)
    {
      IROperand jd = tcc_ir_op_get_dest(ir, &ir->compact_instructions[merge - 1]);
      if ((int)jd.u.imm32 == merge && ir->compact_instructions[merge - 2].op == TCCIR_OP_STORE)
      {
        q_c_idx = merge - 2;
        extra_jmp = merge - 1;
      }
    }
    if (q_c_idx < 0)
      continue;

    IRQuadCompact *q2 = &ir->compact_instructions[q_c_idx];

    /* q_c's dest slot must match q_d's source slot. */
    IROperand d2 = tcc_ir_op_get_dest(ir, q2);
    IROperand r3 = tcc_ir_op_get_src1(ir, q3);
    if (!stackoff_same_slot(d2, r3))
      continue;

    /* q_c's stored value must be an immediate. */
    IROperand sb = tcc_ir_op_get_src1(ir, q2);
    if (!irop_is_immediate(sb))
      continue;
    int64_t val_b = irop_get_imm64_ex(ir, sb);

    /* JUMPIF condition must be EQ or NE (anything else is nonsensical
     * for a TEST_ZERO result). */
    IROperand q4_cond = tcc_ir_op_get_src1(ir, q4);
    int cond_tok = (int)irop_get_imm64_ex(ir, q4_cond);
    if (cond_tok != 0x94 && cond_tok != 0x95)
      continue;

    /* Locate q_b: a unique unconditional JUMP whose target is `merge`,
     * excluding the redundant `extra_jmp` if present.  A JUMPIF that
     * targets merge means there's a third arm we can't fold. */
    int q_b_idx = -1;
    int multi = 0;
    for (int j = 0; j < n; j++)
    {
      IRQuadCompact *qj = &ir->compact_instructions[j];
      if (qj->op != TCCIR_OP_JUMP && qj->op != TCCIR_OP_JUMPIF)
        continue;
      int tgt = (int)tcc_ir_op_get_dest(ir, qj).u.imm32;
      if (tgt != merge)
        continue;
      if (j == extra_jmp)
        continue;
      if (qj->op != TCCIR_OP_JUMP)
      {
        multi = 1;
        break;
      }
      if (q_b_idx >= 0)
      {
        multi = 1;
        break;
      }
      q_b_idx = j;
    }
    if (multi || q_b_idx <= 0)
      continue;

    IRQuadCompact *q1 = &ir->compact_instructions[q_b_idx];
    IRQuadCompact *q0 = &ir->compact_instructions[q_b_idx - 1];

    /* q_b must not itself be a jump target (otherwise other paths could
     * reach merge without writing the slot). */
    if (q1->is_jump_target)
      continue;
    if (q0->op != TCCIR_OP_STORE)
      continue;

    /* q_a must store the same slot with an immediate value. */
    IROperand d0 = tcc_ir_op_get_dest(ir, q0);
    if (!stackoff_same_slot(d0, d2))
      continue;
    IROperand sa = tcc_ir_op_get_src1(ir, q0);
    if (!irop_is_immediate(sa))
      continue;
    int64_t val_a = irop_get_imm64_ex(ir, sa);

    /* Full function scan: verify
     *   1) the slot is referenced ONLY at q_a (dest), q_c (dest), q_d (src1)
     *   2) the only jumps into q_d are q_b and extra_jmp
     *   3) nothing jumps to q_b, q_e, or extra_jmp */
    int bail = 0;
    for (int j = 0; j < n && !bail; j++)
    {
      if (j == q_b_idx - 1 || j == q_c_idx || j == merge)
        continue;
      if (extra_jmp >= 0 && j == extra_jmp)
        continue;
      IRQuadCompact *qj = &ir->compact_instructions[j];
      if (qj->op == TCCIR_OP_NOP)
        continue;

      if (irop_config[qj->op].has_dest)
      {
        IROperand op = tcc_ir_op_get_dest(ir, qj);
        if (operand_references_slot(op, d0))
        {
          bail = 1;
          break;
        }
      }
      if (irop_config[qj->op].has_src1)
      {
        IROperand op = tcc_ir_op_get_src1(ir, qj);
        if (operand_references_slot(op, d0))
        {
          bail = 1;
          break;
        }
      }
      if (irop_config[qj->op].has_src2)
      {
        IROperand op = tcc_ir_op_get_src2(ir, qj);
        if (operand_references_slot(op, d0))
        {
          bail = 1;
          break;
        }
      }

      if (qj->op == TCCIR_OP_JUMP || qj->op == TCCIR_OP_JUMPIF)
      {
        int tgt = (int)tcc_ir_op_get_dest(ir, qj).u.imm32;
        if (j != q_b_idx && j != extra_jmp && tgt == merge)
        {
          bail = 1;
          break;
        }
        if (tgt == q_b_idx || tgt == merge + 1)
        {
          bail = 1;
          break;
        }
        if (extra_jmp >= 0 && tgt == extra_jmp)
        {
          bail = 1;
          break;
        }
      }
    }
    if (bail)
      continue;

    /* Evaluate: JUMPIF EQ jumps when reload == 0; JUMPIF NE jumps when != 0. */
    IROperand q4_dest = tcc_ir_op_get_dest(ir, q4);
    int target_T = (int)q4_dest.u.imm32;
    int target_next = merge + 2;

    int a_jumps = (cond_tok == 0x94) ? (val_a == 0) : (val_a != 0);
    int b_jumps = (cond_tok == 0x94) ? (val_b == 0) : (val_b != 0);

    int a_target = a_jumps ? target_T : target_next;
    int b_target = b_jumps ? target_T : target_next;

    /* Rewrite q_b to JUMP directly to a_target. */
    IROperand q1_dest = tcc_ir_op_get_dest(ir, q1);
    q1_dest.u.imm32 = a_target;
    tcc_ir_set_dest(ir, q_b_idx, q1_dest);

    if (extra_jmp >= 0)
    {
      /* Pre-rotation: rewrite the redundant `JUMP merge` to JUMP b_target,
       * and NOP q_e — both arms now jump directly. */
      IRQuadCompact *jq = &ir->compact_instructions[extra_jmp];
      IROperand jd = tcc_ir_op_get_dest(ir, jq);
      jd.u.imm32 = b_target;
      tcc_ir_set_dest(ir, extra_jmp, jd);
      q4->op = TCCIR_OP_NOP;
    }
    else
    {
      /* Post-rotation: the fall-through arm from q_c lands on q_e via NOPs;
       * rewrite q_e from JUMPIF to unconditional JUMP b_target. */
      q4_dest.u.imm32 = b_target;
      q4->op = TCCIR_OP_JUMP;
      tcc_ir_set_dest(ir, merge + 1, q4_dest);
    }

    /* NOP the scaffolding. */
    q0->op = TCCIR_OP_NOP;
    q2->op = TCCIR_OP_NOP;
    q3->op = TCCIR_OP_NOP;

    LOG_IR_GEN(
        "STACK BOOL DIAMOND: slot=%d A=%lld B=%lld cond=0x%x T=%d next=%d a_tgt=%d b_tgt=%d a_idx=%d merge=%d %s",
        (int)d0.u.imm32, (long long)val_a, (long long)val_b, cond_tok, target_T, target_next, a_target, b_target,
        q_b_idx - 1, merge, extra_jmp >= 0 ? "(pre-rot)" : "(post-rot)");
    changes++;
  }

  LOG_IR_GEN("=== STACK BOOL DIAMOND END: %d fused ===", changes);
  return changes;
}

/* tcc_ir_opt_or_bool_diamond: fold the common `acc |= (cond ? 1 : 0)`
 * diamond.  Source pattern (post-loop-rotation):
 *
 *   i_jmpif: JUMPIF cond → i_st_f                 (skip the true arm)
 *   ... true arm (any straight-line code, e.g. inlined printf) ...
 *   i_st_t:  STORE slot, #1
 *   i_jmp:   JUMP i_or                            (skip false arm)
 *   i_st_f:  STORE slot, #0                       (false arm)
 *   i_or:    dst = src OR slot                    (merge OR)
 *
 * The boolean is materialized to a stack slot and then OR-merged.  We can
 * skip the slot entirely by computing the OR-result directly into dst on
 * each arm:
 *
 *   i_jmpif: unchanged
 *   ... true arm ...
 *   i_st_t:  dst = src OR #1                      (true-arm result)
 *   i_jmp:   unchanged
 *   i_st_f:  dst = src ASSIGN                     (false-arm result; same as src|0)
 *   i_or:    NOP
 *
 * Constraints checked: `slot` is used only at i_st_t, i_st_f, i_or; stored
 * values are {0, 1}; no other jumps target i_or or i_st_f. */
int tcc_ir_opt_or_bool_diamond(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  int changes = 0;
  if (n < 6)
    return 0;

  for (int i_or = 3; i_or < n; i_or++)
  {
    IRQuadCompact *q_or = &ir->compact_instructions[i_or];
    if (q_or->op != TCCIR_OP_OR)
      continue;

    /* The OR's two operands: one is the bool's stack slot (a STACKOFF
     * with no vreg — pure compiler-temp storage), the other is the
     * accumulator (VAR or TEMP, which has a vreg). */
    IROperand or_dest = tcc_ir_op_get_dest(ir, q_or);
    IROperand or_src1 = tcc_ir_op_get_src1(ir, q_or);
    IROperand or_src2 = tcc_ir_op_get_src2(ir, q_or);
    int s1_is_slot = (irop_get_tag(or_src1) == IROP_TAG_STACKOFF && irop_get_vreg(or_src1) < 0);
    int s2_is_slot = (irop_get_tag(or_src2) == IROP_TAG_STACKOFF && irop_get_vreg(or_src2) < 0);
    IROperand slot;
    if (s2_is_slot && !s1_is_slot)
      slot = or_src2;
    else if (s1_is_slot && !s2_is_slot)
      slot = or_src1;
    else
      continue;

    /* Falling-into-merge STORE: ir[i_or - 1] writes `slot` with an immediate. */
    int i_st_f = i_or - 1;
    if (i_st_f < 0)
      continue;
    IRQuadCompact *q_st_f = &ir->compact_instructions[i_st_f];
    if (q_st_f->op != TCCIR_OP_STORE)
      continue;
    if (!stackoff_same_slot(tcc_ir_op_get_dest(ir, q_st_f), slot))
      continue;
    IROperand st_f_src = tcc_ir_op_get_src1(ir, q_st_f);
    if (!irop_is_immediate(st_f_src))
      continue;
    int64_t val_f = irop_get_imm64_ex(ir, st_f_src);

    /* Find the unique JUMP whose target is i_or.  The STORE immediately
     * before that JUMP writes `slot` with the other value. */
    int i_jmp = -1;
    int multi = 0;
    for (int j = 0; j < n; j++)
    {
      IRQuadCompact *qj = &ir->compact_instructions[j];
      if (qj->op != TCCIR_OP_JUMP && qj->op != TCCIR_OP_JUMPIF)
        continue;
      int tgt = (int)tcc_ir_op_get_dest(ir, qj).u.imm32;
      if (tgt != i_or)
        continue;
      if (qj->op != TCCIR_OP_JUMP)
      {
        multi = 1;
        break;
      }
      if (i_jmp >= 0)
      {
        multi = 1;
        break;
      }
      i_jmp = j;
    }
    if (multi || i_jmp <= 0)
      continue;

    int i_st_t = i_jmp - 1;
    IRQuadCompact *q_st_t = &ir->compact_instructions[i_st_t];
    if (q_st_t->op != TCCIR_OP_STORE)
      continue;
    if (!stackoff_same_slot(tcc_ir_op_get_dest(ir, q_st_t), slot))
      continue;
    IROperand st_t_src = tcc_ir_op_get_src1(ir, q_st_t);
    if (!irop_is_immediate(st_t_src))
      continue;
    int64_t val_t = irop_get_imm64_ex(ir, st_t_src);

    /* Only handle val_t=1, val_f=0 for now (the common `bool |= 1` shape). */
    if (val_t != 1 || val_f != 0)
      continue;

    /* Find the JUMPIF whose target is i_st_f (the false-branch STORE). */
    int i_jmpif = -1;
    for (int j = 0; j < i_st_t; j++)
    {
      IRQuadCompact *qj = &ir->compact_instructions[j];
      if (qj->op != TCCIR_OP_JUMPIF)
        continue;
      int tgt = (int)tcc_ir_op_get_dest(ir, qj).u.imm32;
      if (tgt == i_st_f)
      {
        i_jmpif = j;
        break;
      }
    }
    if (i_jmpif < 0)
      continue;

    /* i_st_f must be a jump target only from i_jmpif (no other jumps in). */
    int extra_target = 0;
    for (int j = 0; j < n && !extra_target; j++)
    {
      if (j == i_jmpif || j == i_jmp)
        continue;
      IRQuadCompact *qj = &ir->compact_instructions[j];
      if (qj->op != TCCIR_OP_JUMP && qj->op != TCCIR_OP_JUMPIF)
        continue;
      int tgt = (int)tcc_ir_op_get_dest(ir, qj).u.imm32;
      if (tgt == i_st_f || tgt == i_or)
        extra_target = 1;
    }
    if (extra_target)
      continue;
    /* SWITCH_TABLE targets too. */
    for (int t = 0; t < ir->num_switch_tables && !extra_target; t++)
    {
      TCCIRSwitchTable *st = &ir->switch_tables[t];
      for (int k = 0; k < st->num_entries; k++)
        if (st->targets[k] == i_st_f || st->targets[k] == i_or)
          extra_target = 1;
      if (st->default_target == i_st_f || st->default_target == i_or)
        extra_target = 1;
    }
    if (extra_target)
      continue;

    /* Verify the slot is used only at i_st_t, i_st_f, i_or.
     * "References" here mean the raw stack slot (STACKOFF tag with no
     * vreg).  References to a VAR at the same stack offset don't count:
     * TCC's stack allocator reuses a dead VAR's slot for short-lived
     * temporaries, so the same offset can host two non-overlapping
     * entities — the VAR's last use is always before the slot's first
     * use here. */
    int extra_use = 0;
#define ORBD_REFS_SLOT(op_) (operand_references_slot((op_), slot) && irop_get_vreg(op_) < 0)
    for (int j = 0; j < n && !extra_use; j++)
    {
      if (j == i_st_t || j == i_st_f || j == i_or)
        continue;
      IRQuadCompact *qj = &ir->compact_instructions[j];
      if (qj->op == TCCIR_OP_NOP)
        continue;
      if (irop_config[qj->op].has_dest && ORBD_REFS_SLOT(tcc_ir_op_get_dest(ir, qj)))
        extra_use = 1;
      if (irop_config[qj->op].has_src1 && ORBD_REFS_SLOT(tcc_ir_op_get_src1(ir, qj)))
        extra_use = 1;
      if (irop_config[qj->op].has_src2 && ORBD_REFS_SLOT(tcc_ir_op_get_src2(ir, qj)))
        extra_use = 1;
    }
#undef ORBD_REFS_SLOT
    if (extra_use)
      continue;

    /* Sanity: i_st_t and the true arm must come after i_jmpif. */
    if (i_st_t <= i_jmpif)
      continue;

    /* Apply transformation:
     *   - i_st_t: STORE slot, #1   →  dst = src OR #1
     *   - i_st_f: STORE slot, #0   →  dst = src        (ASSIGN; same as |0)
     *   - i_or:   dst = src OR slot → NOP
     * The JUMPIF and the JMP from the true arm stay in place; both arms
     * now produce the same dst directly, no stack slot needed.
     *
     * STORE (2 operands) → OR (3 operands) needs a new operand_base in
     * the pool, since the old slot has no room for src2.  ASSIGN keeps
     * the same operand count as STORE so we can edit in place. */
    LOG_IR_GEN("OPTIMIZE: OR bool diamond at i_or=%d (jmpif=%d, st_t=%d, jmp=%d, st_f=%d)", i_or, i_jmpif, i_st_t, i_jmp,
               i_st_f);

    IROperand acc_dest = or_dest;
    /* `slot` is the no-vreg STACKOFF; the accumulator is whichever
     * operand isn't the slot.  Don't filter on tag here — the accumulator
     * may itself be a stack-allocated VAR (also STACKOFF-tagged but with
     * a vreg). */
    IROperand acc_src = s2_is_slot ? or_src1 : or_src2;
    int acc_btype = irop_get_btype(acc_dest);
    IROperand one_imm = irop_make_imm32(-1, 1, acc_btype);

    /* True arm: dst = src OR #1.  Allocate 3 fresh operand slots at the
     * end of the pool and point i_st_t's operand_base there. */
    tcc_ir_pool_ensure(ir, 3);
    uint32_t new_base = (uint32_t)ir->iroperand_pool_count;
    ir->iroperand_pool[new_base + 0] = acc_dest;
    ir->iroperand_pool[new_base + 1] = acc_src;
    ir->iroperand_pool[new_base + 2] = one_imm;
    ir->iroperand_pool_count += 3;
    q_st_t->op = TCCIR_OP_OR;
    q_st_t->operand_base = new_base;

    /* False arm: dst = src (plain ASSIGN; equivalent to src | 0).  ASSIGN
     * and STORE both use {dest, src1}, so we can edit in place. */
    q_st_f->op = TCCIR_OP_ASSIGN;
    tcc_ir_set_dest(ir, i_st_f, acc_dest);
    tcc_ir_set_src1(ir, i_st_f, acc_src);

    /* Merge OR is no longer needed — both arms produce the final value. */
    ir->compact_instructions[i_or].op = TCCIR_OP_NOP;
    changes++;
  }

  return changes;
}

int tcc_ir_opt_branch_folding_ex(IROptCtx *ctx) { return tcc_ir_opt_branch_folding(ctx->ir); }
int tcc_ir_opt_vrp_ex(IROptCtx *ctx) { return tcc_ir_opt_vrp(ctx->ir); }
int tcc_ir_opt_nonneg_branch_fold_ex(IROptCtx *ctx) { return tcc_ir_opt_nonneg_branch_fold(ctx->ir); }
int tcc_ir_opt_float_branch_fold_ex(IROptCtx *ctx) { return tcc_ir_opt_float_branch_fold(ctx->ir); }
int tcc_ir_opt_stack_addr_nonnull_fold_ex(IROptCtx *ctx) { return tcc_ir_opt_stack_addr_nonnull_fold(ctx->ir); }
int tcc_ir_opt_setif_branch_fuse_ex(IROptCtx *ctx) { return tcc_ir_opt_setif_branch_fuse(ctx->ir); }
int tcc_ir_opt_stack_bool_diamond_ex(IROptCtx *ctx) { return tcc_ir_opt_stack_bool_diamond(ctx->ir); }
int tcc_ir_opt_or_bool_diamond_ex(IROptCtx *ctx) { return tcc_ir_opt_or_bool_diamond(ctx->ir); }
