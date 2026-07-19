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
#include <ir.h>

/* Generous fixed pools — unit-test functions are tiny. */
#define UTB_MAX_INSTR 256
#define UTB_MAX_OPERANDS 1024

static inline TCCIRState *utb_new(void)
{
  TCCIRState *ir = (TCCIRState *)tcc_mallocz(sizeof(*ir));
  ir->compact_instructions = (IRQuadCompact *)tcc_mallocz(sizeof(IRQuadCompact) * UTB_MAX_INSTR);
  ir->compact_instructions_size = UTB_MAX_INSTR;
  ir->iroperand_pool = (IROperand *)tcc_mallocz(sizeof(IROperand) * UTB_MAX_OPERANDS);
  ir->iroperand_pool_count = 0;
  ir->next_instruction_index = 0;
  return ir;
}

/* Initialize the operand/symref pools.  This wraps `tcc_ir_pools_init` but
 * frees the placeholder iroperand_pool allocated by `utb_new()` first, so
 * ASAN does not report a leak from the overwritten pointer. */
static inline void utb_pools_init(TCCIRState *ir)
{
  tcc_free(ir->iroperand_pool);
  ir->iroperand_pool = NULL;
  tcc_ir_pools_init(ir);
}

static inline void utb_free(TCCIRState *ir)
{
  if (!ir)
    return;
  tcc_free(ir->compact_instructions);
  tcc_free(ir->iroperand_pool);
  tcc_free(ir->temporary_variables_live_intervals);
  tcc_free(ir->variables_live_intervals);
  tcc_free(ir->parameters_live_intervals);
  tcc_free(ir->pool_i64);
  tcc_free(ir->pool_f64);
  tcc_free(ir->pool_symref);
  tcc_free(ir->pool_ctype);
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

/* Stack-offset operand.  Tests must ensure the IR's stack layout is set up if
 * the pass under test reads slot metadata; for purely structural tests the
 * default (offset=0, no flags) is enough. */
static inline IROperand utb_stackoff(int32_t offset, int is_lval, int is_llocal, int is_param, int btype)
{
  return irop_make_stackoff(0, offset, is_lval, is_llocal, is_param, btype);
}

/* Symbol-reference operand.  The caller must have called utb_pools_init(ir)
 * so the symref pool exists; `sym->v` is the token looked up by get_tok_str(). */
static inline IROperand utb_symref(TCCIRState *ir, Sym *sym, int is_lval, int is_local, int is_const, int btype)
{
  uint32_t sidx = tcc_ir_pool_add_symref(ir, sym, 0, 0);
  return irop_make_symref(0, sidx, is_lval, is_local, is_const, btype);
}

/* ---- flag helpers (return modified copies) ---- */

static inline IROperand utb_lval(IROperand op)
{
  op.is_lval = 1;
  return op;
}

static inline IROperand utb_llocal(IROperand op)
{
  op.is_llocal = 1;
  return op;
}

static inline IROperand utb_unsigned(IROperand op)
{
  op.is_unsigned = 1;
  return op;
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

/* Append an instruction with a 4th operand at pool[operand_base+3].
 * Used by MLA (accumulator), SELECT (condition code), and indexed memory
 * ops (scale).  The first three operand slots follow irop_config; missing
 * slots are padded with IROP_NONE so op4 is always at operand_base+3. */
static inline int utb_emit4(TCCIRState *ir, TccIrOp op, IROperand dest, IROperand src1, IROperand src2,
                            IROperand op4)
{
  int i = ir->next_instruction_index++;
  IRQuadCompact *q = &ir->compact_instructions[i];
  q->orig_index = i;
  q->op = op;
  q->operand_base = (uint32_t)ir->iroperand_pool_count;
  q->line_num = 0;
  q->is_jump_target = 0;
  q->no_unroll = 0;

  ir->iroperand_pool[ir->iroperand_pool_count++] = irop_config[op].has_dest ? dest : UTB_NONE;
  ir->iroperand_pool[ir->iroperand_pool_count++] = irop_config[op].has_src1 ? src1 : UTB_NONE;
  ir->iroperand_pool[ir->iroperand_pool_count++] = irop_config[op].has_src2 ? src2 : UTB_NONE;
  ir->iroperand_pool[ir->iroperand_pool_count++] = op4;
  return i;
}

/* ---- pass execution helpers ---- */

/* Run `passfn(ir)` until it reports no changes (fixpoint).  Asserts convergence
 * within `max_iter` and returns the total number of changes. */
static inline int utb_run_to_fixpoint(TCCIRState *ir, int (*passfn)(TCCIRState *ir), int max_iter)
{
  int total = 0;
  for (int i = 0; i < max_iter; ++i)
  {
    int changes = passfn(ir);
    if (changes == 0)
      return total;
    total += changes;
  }
  fprintf(stderr, "utb_run_to_fixpoint: did not converge in %d iterations\n", max_iter);
  return -1;
}

/* ---- read-back accessors for assertions ---- */

static inline TccIrOp utb_op(TCCIRState *ir, int i) { return ir->compact_instructions[i].op; }
static inline IROperand utb_dest(TCCIRState *ir, int i) { return tcc_ir_op_get_dest(ir, &ir->compact_instructions[i]); }
static inline IROperand utb_src1(TCCIRState *ir, int i) { return tcc_ir_op_get_src1(ir, &ir->compact_instructions[i]); }
static inline IROperand utb_src2(TCCIRState *ir, int i) { return tcc_ir_op_get_src2(ir, &ir->compact_instructions[i]); }
static inline IROperand utb_op4(TCCIRState *ir, int i) { return ir->iroperand_pool[ir->compact_instructions[i].operand_base + 3]; }
static inline int utb_vreg(IROperand op) { return irop_get_vreg(op); }

/* ---- structural sanity checks ---- */

/* Extract the raw vreg position for a pure VREG operand.  Returns -1 for
 * immediates, symrefs, stack offsets, NONE, etc. */
static inline int utb_vreg_pos(IROperand op)
{
  if (irop_get_tag(op) != IROP_TAG_VREG)
    return -1;
  return (int)op.position;
}

/* Verify that every emitted instruction has its required operands present,
 * that pure VREG positions are below `max_vreg_pos`, and that jump targets
 * land inside the function.  Call after a pass that may rewrite IR. */
static inline int utb_assert_wellformed(TCCIRState *ir, int max_vreg_pos)
{
  for (int i = 0; i < ir->next_instruction_index; ++i)
  {
    const IRQuadCompact *q = &ir->compact_instructions[i];
    IROperand dest = tcc_ir_op_get_dest(ir, q);
    IROperand s1 = tcc_ir_op_get_src1(ir, q);
    IROperand s2 = tcc_ir_op_get_src2(ir, q);

    if (irop_config[q->op].has_dest && utb_vreg_pos(dest) > max_vreg_pos)
      return -1;
    if (irop_config[q->op].has_src1 && utb_vreg_pos(s1) > max_vreg_pos)
      return -1;
    if (irop_config[q->op].has_src2 && utb_vreg_pos(s2) > max_vreg_pos)
      return -1;

    if (q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_JUMPIF)
    {
      IROperand jdest = tcc_ir_op_get_dest(ir, q);
      int target = (int)irop_get_imm64_ex(ir, jdest);
      if (target < 0 || target >= ir->next_instruction_index)
        return -1;
    }
  }
  return 0;
}

/* ---- settable token-name table (stubs.c) ---- */

/* Map token `tok` to string `name` for get_tok_str() used by name-gated
 * constfold passes.  Pass tests call this before building a SYMREF callee.
 * A token may be reset by passing NULL for name (falls back to "?"). */
void utb_set_tok_str(int tok, const char *name);

#endif /* TCC_UT_IR_BUILD_H */
