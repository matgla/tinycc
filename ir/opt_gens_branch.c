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
#include "opt_gens_branch.h"


static int ir_gen_branch_fold_test_zero(IROptCtx *ctx, int i)
{
  TCCIRState *ir = ctx->ir;
  IRQuadCompact *test_q = &ir->compact_instructions[i];

  IROperand src1 = tcc_ir_op_get_src1(ir, test_q);
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
    IROperand dest = tcc_ir_op_get_dest(ir, jump_q);
    test_q->op = TCCIR_OP_NOP;
    jump_q->op = TCCIR_OP_JUMP;
    tcc_ir_set_dest(ir, j, dest);
  } else {
    test_q->op = TCCIR_OP_NOP;
    jump_q->op = TCCIR_OP_NOP;
    /* The JUMPIF wasn't taken — control falls through.  If the next op
     * is a SETIF that reads the same flag state we just NOPed, codegen
     * would lower it consuming garbage flags.  Fold it to a constant
     * based on the known value comparison.
     *
     * NE (0x95): set iff value != 0  → fold to (val != 0 ? 1 : 0)
     * EQ (0x94): set iff value == 0  → fold to (val == 0 ? 1 : 0)
     */
    int k = ir_skip_nops_forward(ir, j + 1, ir->next_instruction_index);
    if (k < ir->next_instruction_index)
    {
      IRQuadCompact *setif_q = &ir->compact_instructions[k];
      if (setif_q->op == TCCIR_OP_SETIF && !setif_q->is_jump_target)
      {
        IROperand setif_cond = tcc_ir_op_get_src1(ir, setif_q);
        int setif_tok = (int)irop_get_imm64_ex(ir, setif_cond);
        int setif_result = -1;
        if (setif_tok == 0x95)        /* NE */
          setif_result = (val != 0) ? 1 : 0;
        else if (setif_tok == 0x94)   /* EQ */
          setif_result = (val == 0) ? 1 : 0;
        if (setif_result >= 0)
        {
          IROperand dest = tcc_ir_op_get_dest(ir, setif_q);
          IROperand imm = irop_make_imm32(-1, setif_result, irop_get_btype(dest));
          setif_q->op = TCCIR_OP_ASSIGN;
          tcc_ir_set_src1(ir, k, imm);
          tcc_ir_set_src2(ir, k, IROP_NONE);
        }
      }
    }
  }

  LOG_IR_GEN("BRANCH FOLD: TEST_ZERO #%lld with cond 0x%x -> %s at i=%d",
             (long long)val, tok, branch_taken ? "JUMP" : "NOP", i);
  return 1;
}

static int ir_gen_branch_fold_cmp(IROptCtx *ctx, int i)
{
  TCCIRState *ir = ctx->ir;
  IRQuadCompact *cmp_q = &ir->compact_instructions[i];

  IROperand src1 = tcc_ir_op_get_src1(ir, cmp_q);
  IROperand src2 = tcc_ir_op_get_src2(ir, cmp_q);
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

  int result = evaluate_compare_condition(val1, val2, tok);
  if (result < 0)
    return 0;

  if (result) {
    IROperand dest = tcc_ir_op_get_dest(ir, jump_q);
    cmp_q->op = TCCIR_OP_NOP;
    jump_q->op = TCCIR_OP_JUMP;
    tcc_ir_set_dest(ir, j, dest);
  } else {
    cmp_q->op = TCCIR_OP_NOP;
    jump_q->op = TCCIR_OP_NOP;
  }

  LOG_IR_GEN("BRANCH FOLD: CMP %lld,%lld cond 0x%x -> %s at i=%d",
             (long long)val1, (long long)val2, tok, result ? "JUMP" : "NOP", i);
  return 1;
}

static int ir_gen_setif_branch_fuse(IROptCtx *ctx, int i)
{
  TCCIRState *ir = ctx->ir;
  int n = ir->next_instruction_index;

  if (i + 3 >= n)
    return 0;

  IRQuadCompact *setif_q = &ir->compact_instructions[i + 1];
  IRQuadCompact *test_q = &ir->compact_instructions[i + 2];
  IRQuadCompact *jump_q = &ir->compact_instructions[i + 3];

  if (setif_q->op != TCCIR_OP_SETIF)
    return 0;
  if (test_q->op != TCCIR_OP_TEST_ZERO)
    return 0;
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

  LOG_IR_GEN("SETIF FUSE: CMP+SETIF(0x%x)+TEST_ZERO+JUMPIF(0x%x) -> CMP+JUMPIF(0x%x) at i=%d",
             setif_tok, jump_tok, new_tok, i);
  return 1;
}

const IROptGen branch_gens[] = {
    {TCCIR_OP_CMP, ir_gen_setif_branch_fuse, "setif_branch_fuse", 0},
    {TCCIR_OP_CMP, ir_gen_branch_fold_cmp, "branch_fold_cmp", 0},
    {TCCIR_OP_TEST_ZERO, ir_gen_branch_fold_test_zero, "branch_fold_test_zero", 0},
};

const int branch_gens_count = sizeof(branch_gens) / sizeof(branch_gens[0]);
