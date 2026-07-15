/*
 *  TCC IR - Branch-folding generator table (pre-SSA engine)
 *
 *  Generators:
 *    branch_fold_test_zero — fold TEST_ZERO #const + JUMPIF to unconditional/NOP
 *    branch_fold_cmp       — fold CMP #const,#const + JUMPIF to unconditional/NOP
 *    setif_branch_fuse     — fuse CMP+SETIF+TEST_ZERO+JUMPIF → CMP+JUMPIF
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
#include "opt_utils.h"
#include "opt_dsl.h"
#include "opt/flat/branch.h"

static int ir_branch_cmp_width(IROperand src1, IROperand src2)
{
  return (irop_get_btype(src1) == IROP_BTYPE_INT64 ||
          irop_get_btype(src2) == IROP_BTYPE_INT64)
             ? 64
             : 32;
}

static int ir_branch_eval_const_cmp(int64_t val1, int64_t val2, int cond,
                                    IROperand src1, IROperand src2)
{
  if (ir_branch_cmp_width(src1, src2) != 64)
  {
    uint32_t u1 = (uint32_t)val1;
    uint32_t u2 = (uint32_t)val2;
    int32_t s1 = (int32_t)u1;
    int32_t s2 = (int32_t)u2;
    switch (cond)
    {
    case TOK_EQ:
      return u1 == u2;
    case TOK_NE:
      return u1 != u2;
    case TOK_LT:
      return s1 < s2;
    case TOK_GE:
      return s1 >= s2;
    case TOK_LE:
      return s1 <= s2;
    case TOK_GT:
      return s1 > s2;
    case TOK_ULT:
      return u1 < u2;
    case TOK_UGE:
      return u1 >= u2;
    case TOK_ULE:
      return u1 <= u2;
    case TOK_UGT:
      return u1 > u2;
    default:
      break;
    }
  }
  return evaluate_compare_condition(val1, val2, cond);
}

OPT_GEN_FLAT(branch_fold_test_zero, TCCIR_OP_TEST_ZERO)
{
  PATTERN(.constraints = { .src1 = IR_CONSTRAINT_ANY });

  if (!irop_is_immediate(src1))
    return 0;

  int j = ir_skip_nops_forward(ir, i + 1, ir->next_instruction_index);
  if (j >= ir->next_instruction_index)
    return 0;
  IRQuadCompact *jump_q = &ir->compact_instructions[j];
  if (jump_q->op != TCCIR_OP_JUMPIF)
    return 0;

  int64_t val = irop_get_imm64_ex(ir, src1);
  IROperand cond = tcc_ir_op_get_src1(ir, jump_q);
  int tok = (int)irop_get_imm64_ex(ir, cond);

  int branch_taken;
  if (tok == 0x94)
    branch_taken = (val == 0);
  else if (tok == 0x95)
    branch_taken = (val != 0);
  else
    return 0;

  if (branch_taken) {
    IROperand jump_dest = tcc_ir_op_get_dest(ir, jump_q);
    q->op = TCCIR_OP_NOP;
    jump_q->op = TCCIR_OP_JUMP;
    tcc_ir_set_dest(ir, j, jump_dest);
  } else {
    q->op = TCCIR_OP_NOP;
    jump_q->op = TCCIR_OP_NOP;
    /* not-taken JUMPIF leaves stale flags: fold a dependent SETIF to its known constant */
    int k = ir_skip_nops_forward(ir, j + 1, ir->next_instruction_index);
    if (k < ir->next_instruction_index)
    {
      IRQuadCompact *setif_q = &ir->compact_instructions[k];
      if (setif_q->op == TCCIR_OP_SETIF && !setif_q->is_jump_target)
      {
        IROperand setif_cond = tcc_ir_op_get_src1(ir, setif_q);
        int setif_tok = (int)irop_get_imm64_ex(ir, setif_cond);
        int setif_result = -1;
        if (setif_tok == 0x95)
          setif_result = (val != 0) ? 1 : 0;
        else if (setif_tok == 0x94)
          setif_result = (val == 0) ? 1 : 0;
        if (setif_result >= 0)
        {
          IROperand setif_dest = tcc_ir_op_get_dest(ir, setif_q);
          IROperand imm = irop_make_imm32(-1, setif_result, irop_get_btype(setif_dest));
          setif_q->op = TCCIR_OP_ASSIGN;
          tcc_ir_set_src1(ir, k, imm);
          tcc_ir_set_src2(ir, k, IROP_NONE);
        }
      }
    }
  }

  return 1;
}

OPT_GEN_FLAT(branch_fold_cmp, TCCIR_OP_CMP)
{
  PATTERN(.constraints = { .src1 = IR_CONSTRAINT_ANY, .src2 = IR_CONSTRAINT_ANY });

  if (!irop_is_immediate(src1) || !irop_is_immediate(src2))
    return 0;

  int j = ir_skip_nops_forward(ir, i + 1, ir->next_instruction_index);
  if (j >= ir->next_instruction_index)
    return 0;
  IRQuadCompact *jump_q = &ir->compact_instructions[j];
  if (jump_q->op != TCCIR_OP_JUMPIF)
    return 0;

  int64_t val1 = irop_get_imm64_ex(ir, src1);
  int64_t val2 = irop_get_imm64_ex(ir, src2);
  IROperand cond = tcc_ir_op_get_src1(ir, jump_q);
  int tok = (int)irop_get_imm64_ex(ir, cond);

  int result = ir_branch_eval_const_cmp(val1, val2, tok, src1, src2);
  if (result < 0)
    return 0;

  if (result) {
    IROperand jump_dest = tcc_ir_op_get_dest(ir, jump_q);
    q->op = TCCIR_OP_NOP;
    jump_q->op = TCCIR_OP_JUMP;
    tcc_ir_set_dest(ir, j, jump_dest);
  } else {
    q->op = TCCIR_OP_NOP;
    jump_q->op = TCCIR_OP_NOP;
  }

  return 1;
}

OPT_GEN_FLAT(setif_branch_fuse, TCCIR_OP_CMP)
{
  PATTERN(.constraints = { .dest = IR_CONSTRAINT_ANY });

  int n = ir->next_instruction_index;
  if (i + 3 >= n)
    return 0;

  IRQuadCompact *setif_q = &ir->compact_instructions[i + 1];
  IRQuadCompact *test_q = &ir->compact_instructions[i + 2];
  IRQuadCompact *jump_q = &ir->compact_instructions[i + 3];

  if (setif_q->op != TCCIR_OP_SETIF)
    return 0;
  /* 64-bit EQ/NE emit CMP T,#0 instead of TEST_ZERO T; both set Z from T==0 */
  if (test_q->op == TCCIR_OP_TEST_ZERO)
  {
  }
  else if (test_q->op == TCCIR_OP_CMP)
  {
    IROperand test_src2 = tcc_ir_op_get_src2(ir, test_q);
    if (!irop_is_immediate(test_src2) || irop_get_imm64_ex(ir, test_src2) != 0)
      return 0;
  }
  else
  {
    return 0;
  }
  if (jump_q->op != TCCIR_OP_JUMPIF)
    return 0;

  if (setif_q->is_jump_target || test_q->is_jump_target || jump_q->is_jump_target)
    return 0;

  IROperand setif_dest = tcc_ir_op_get_dest(ir, setif_q);
  IROperand test_src1 = tcc_ir_op_get_src1(ir, test_q);
  int32_t setif_vr = irop_get_vreg(setif_dest);
  int32_t test_vr = irop_get_vreg(test_src1);

  if (setif_vr < 0 || setif_vr != test_vr)
    return 0;

  if (!tcc_ir_vreg_has_single_use(ir, setif_vr, -1))
    return 0;

  IROperand setif_src1 = tcc_ir_op_get_src1(ir, setif_q);
  IROperand jump_src1 = tcc_ir_op_get_src1(ir, jump_q);
  int setif_tok = (int)irop_get_imm64_ex(ir, setif_src1);
  int jump_tok = (int)irop_get_imm64_ex(ir, jump_src1);

  int new_tok;
  if (jump_tok == 0x94)
    new_tok = invert_cond_token(setif_tok);
  else if (jump_tok == 0x95)
    new_tok = setif_tok;
  else
    return 0;

  if (new_tok < 0)
    return 0;

  int btype = irop_get_btype(jump_src1);
  IROperand new_cond = irop_make_imm32(-1, new_tok, btype);
  tcc_ir_set_src1(ir, i + 3, new_cond);

  setif_q->op = TCCIR_OP_NOP;
  test_q->op = TCCIR_OP_NOP;

  return 1;
}

const IROptGen branch_gens[] = {
    {TCCIR_OP_CMP, opt_dsl_dispatch_setif_branch_fuse_flat, "setif_branch_fuse", 0},
    {TCCIR_OP_CMP, opt_dsl_dispatch_branch_fold_cmp_flat, "branch_fold_cmp", 0},
    {TCCIR_OP_TEST_ZERO, opt_dsl_dispatch_branch_fold_test_zero_flat, "branch_fold_test_zero", 0},
};

const int branch_gens_count = sizeof(branch_gens) / sizeof(branch_gens[0]);
