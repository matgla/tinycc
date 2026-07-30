/*
 *  TCC IR - Float-branch folding (flat pass)
 *
 *  Copyright (c) 2025 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */
/* Relocated flat pass, Branch A [A*]; see docs/plan_legacy_flat_ir_ssa_retire.md */

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


int tcc_ir_opt_float_branch_fold_ex(IROptCtx *ctx) { return tcc_ir_opt_float_branch_fold(ctx->ir); }
