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
#include "opt/flat/branch.h"

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

#define NUM_NONNEG_FUNCS (sizeof(nonneg_func_names) / sizeof(nonneg_func_names[0]))

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

  /* Mark instructions inside a loop body (spanned by a back-edge).  The
   * address-vs-address distinctness fold below is unsound for a CMP that
   * re-executes: a walking stack pointer (p compared to a base while p is
   * decremented on the back-edge) is distinct on the first iteration but
   * becomes equal, so folding it drops the loop-exit test (990513-1). */
  uint8_t *in_loop = (uint8_t *)tcc_mallocz((size_t)n);
  for (int j = 0; j < n; j++)
  {
    IRQuadCompact *qj = &ir->compact_instructions[j];
    if (qj->op != TCCIR_OP_JUMP && qj->op != TCCIR_OP_JUMPIF)
      continue;
    int t = (int)irop_get_imm64_ex(ir, tcc_ir_op_get_dest(ir, qj));
    if (t < 0 || t > j)
      continue;
    for (int k = t; k <= j; k++)
      in_loop[k] = 1;
  }

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
      /* A CMP inside a loop body may re-execute with an operand that walked
       * to a different address, so its tracked (first-iteration) offset does
       * not prove distinctness — decline both folds there. */
      if (in_loop[i])
        ; /* leave unmatched */
      else if (sa1_valid && sa2_valid && off1 == off2)
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

  tcc_free(in_loop);
  return changes;
}

/* setif_branch_fuse: see source/opt/flat/scalar/branch.c for generator implementation */
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

/* ssa:or_bool_diamond — fold `acc |= (cond ? 1 : 0)` slot materialization
 * into per-arm ORs; see docs/plan_legacy_or_bool_diamond_ssa.md. */
int ssa_opt_or_bool_diamond(TCCIRState *ir)
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

    /* One OR operand must be a no-vreg STACKOFF slot, the other the accumulator. */
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

    /* Unique JUMP targeting i_or; the STORE just before it writes the true-arm value. */
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

    /* Slot used only at i_st_t/i_st_f/i_or; VARs sharing the offset don't count (non-overlapping slot reuse). */
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

    LOG_IR_GEN("OPTIMIZE: OR bool diamond at i_or=%d (jmpif=%d, st_t=%d, jmp=%d, st_f=%d)", i_or, i_jmpif, i_st_t, i_jmp,
               i_st_f);

    IROperand acc_dest = or_dest;
    /* Accumulator may itself be a vreg-carrying STACKOFF VAR — don't filter on tag. */
    IROperand acc_src = s2_is_slot ? or_src1 : or_src2;
    int acc_btype = irop_get_btype(acc_dest);
    IROperand one_imm = irop_make_imm32(-1, 1, acc_btype);

    /* True arm: STORE (2 ops) becomes OR (3 ops) — needs fresh pool operand slots. */
    tcc_ir_pool_ensure(ir, 3);
    uint32_t new_base = (uint32_t)ir->iroperand_pool_count;
    ir->iroperand_pool[new_base + 0] = acc_dest;
    ir->iroperand_pool[new_base + 1] = acc_src;
    ir->iroperand_pool[new_base + 2] = one_imm;
    ir->iroperand_pool_count += 3;
    q_st_t->op = TCCIR_OP_OR;
    q_st_t->operand_base = new_base;

    /* False arm: dst = src (src|0); ASSIGN and STORE share {dest,src1}, edit in place. */
    q_st_f->op = TCCIR_OP_ASSIGN;
    tcc_ir_set_dest(ir, i_st_f, acc_dest);
    tcc_ir_set_src1(ir, i_st_f, acc_src);

    ir->compact_instructions[i_or].op = TCCIR_OP_NOP;
    changes++;
  }

  return changes;
}

int tcc_ir_opt_float_branch_fold_ex(IROptCtx *ctx) { return tcc_ir_opt_float_branch_fold(ctx->ir); }
int tcc_ir_opt_stack_addr_nonnull_fold_ex(IROptCtx *ctx) { return tcc_ir_opt_stack_addr_nonnull_fold(ctx->ir); }
int tcc_ir_opt_setif_branch_fuse_ex(IROptCtx *ctx) { return tcc_ir_opt_setif_branch_fuse(ctx->ir); }
int tcc_ir_opt_stack_bool_diamond_ex(IROptCtx *ctx) { return tcc_ir_opt_stack_bool_diamond(ctx->ir); }
