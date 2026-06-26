/*
 *  ir_build.h - hand-built IR construction for isolated optimization-pass tests
 *
 *  Optimization passes have a clean signature `int tcc_ir_opt_<name>(TCCIRState*)`
 *  that mutates ir->compact_instructions[] / ir->iroperand_pool[] in place. This
 *  header lets a unit test build a tiny instruction sequence by hand, run one
 *  pass, and assert on the result — without the frontend-coupled tcc_ir_put()
 *  (which needs SValue/CType/file state) and without QEMU.
 *
 *  Layout mirrors what the inline accessors in tccir.h expect: per instruction,
 *  operands are appended to iroperand_pool as [dest?, src1?, src2?] according to
 *  irop_config[op]. See tests/unit/README.md (Pattern B) and PASS_COVERAGE.md.
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or modify it under
 * the terms of the GNU Lesser General Public License.
 */

#ifndef TCC_UT_IR_BUILD_H
#define TCC_UT_IR_BUILD_H

#define USING_GLOBALS
#include "ir.h"

/* Generous fixed pools — unit-test functions are tiny. */
#define UTB_MAX_INSTR 256
#define UTB_MAX_OPERANDS 1024

static inline TCCIRState *utb_new(void)
{
  TCCIRState *ir = (TCCIRState *)tcc_mallocz(sizeof(*ir));
  ir->compact_instructions = (IRQuadCompact *)tcc_mallocz(sizeof(IRQuadCompact) * UTB_MAX_INSTR);
  ir->iroperand_pool = (IROperand *)tcc_mallocz(sizeof(IROperand) * UTB_MAX_OPERANDS);
  ir->iroperand_pool_count = 0;
  ir->next_instruction_index = 0;
  return ir;
}

static inline void utb_free(TCCIRState *ir)
{
  if (!ir)
    return;
  tcc_free(ir->compact_instructions);
  tcc_free(ir->iroperand_pool);
  tcc_free(ir);
}

/* ---- operand constructors ---- */

static inline IROperand utb_temp(int pos, int btype)
{
  return irop_make_vreg(TCCIR_ENCODE_VREG(TCCIR_VREG_TYPE_TEMP, pos), btype);
}

static inline IROperand utb_var(int pos, int btype)
{
  return irop_make_vreg(TCCIR_ENCODE_VREG(TCCIR_VREG_TYPE_VAR, pos), btype);
}

static inline IROperand utb_param(int pos, int btype)
{
  return irop_make_vreg(TCCIR_ENCODE_VREG(TCCIR_VREG_TYPE_PARAM, pos), btype);
}

/* Immediate: vreg arg 0 -> vreg_type 0 -> irop_get_vreg() returns "no vreg",
 * matching the production convention `irop_make_imm32(0, val, btype)`. */
static inline IROperand utb_imm(int32_t val, int btype)
{
  return irop_make_imm32(0, val, btype);
}

#define UTB_NONE IROP_NONE

/* ---- instruction emission ---- */

/* Append one instruction; returns its index. Operands present per irop_config. */
static inline int utb_emit(TCCIRState *ir, TccIrOp op, IROperand dest, IROperand src1, IROperand src2)
{
  int i = ir->next_instruction_index++;
  IRQuadCompact *q = &ir->compact_instructions[i];
  q->orig_index = i;
  q->op = op;
  q->operand_base = (uint32_t)ir->iroperand_pool_count;
  q->line_num = 0;
  q->is_jump_target = 0;
  q->no_unroll = 0;
  if (irop_config[op].has_dest)
    ir->iroperand_pool[ir->iroperand_pool_count++] = dest;
  if (irop_config[op].has_src1)
    ir->iroperand_pool[ir->iroperand_pool_count++] = src1;
  if (irop_config[op].has_src2)
    ir->iroperand_pool[ir->iroperand_pool_count++] = src2;
  return i;
}

/* ---- read-back accessors for assertions ---- */

static inline TccIrOp utb_op(TCCIRState *ir, int i) { return ir->compact_instructions[i].op; }
static inline IROperand utb_dest(TCCIRState *ir, int i) { return tcc_ir_op_get_dest(ir, &ir->compact_instructions[i]); }
static inline IROperand utb_src1(TCCIRState *ir, int i) { return tcc_ir_op_get_src1(ir, &ir->compact_instructions[i]); }
static inline IROperand utb_src2(TCCIRState *ir, int i) { return tcc_ir_op_get_src2(ir, &ir->compact_instructions[i]); }
static inline int utb_vreg(IROperand op) { return irop_get_vreg(op); }

#endif /* TCC_UT_IR_BUILD_H */
