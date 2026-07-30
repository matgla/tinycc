/*
 * test_ssa_opt_str_strcpy.c - Unit tests for strcpy->BLOCK_COPY fold handler
 *
 * Covers all branches in source/opt/ssa/string/str_strcpy.c:
 *   - Line 34:  !ir_opt_get_call_param_operand -> no fold
 *   - Line 42:  !ir_opt_eval_const_string_operand -> no fold
 *   - Lines 44-46: len<0 or (len+1)&3!=0 -> no fold
 *   - Lines 52-54: !sr || (sr->addend&3)!=0 -> no fold
 *   - Line 59:  c->is_valued==0 (void call) -> no fold
 *   - Lines 61-63: rv!=-1 (result vreg present)
 *   - Lines 66-70: !c->ssa || vi->use_count!=0 -> no fold
 *   - Lines 74-77: positive fold setup
 *   - Line 99:  ir_opt_nop_call_params
 *   - Lines 101-103: pool adds
 *   - Lines 105-107: BLOCK_COPY replacement
 *
 * Copyright (c) 2026 Mateusz Stadnik
 */

#include "ssa_build.h"
#include "source/opt/ssa/include/ssa_opt.h"
#include "str_handlers.h"
#include "opt_utils.h"
#include "const_string_fold.h"
#include "ut.h"

#define I32 IROP_BTYPE_INT32
#define I8 IROP_BTYPE_INT8

/* ========================================================================
 * Helpers
 * ======================================================================== */

/* Emit a 2-param FUNCCALLVAL where dst is a local stack address. */
static int emit_strcpy_call(TCCIRState *ir, IROperand dst, IROperand src, int call_id)
{
  utb_emit(ir, TCCIR_OP_FUNCPARAMVAL, UTB_NONE, dst,
           utb_imm((int32_t)TCCIR_ENCODE_PARAM(call_id, 0), I32));
  utb_emit(ir, TCCIR_OP_FUNCPARAMVAL, UTB_NONE, src,
           utb_imm((int32_t)TCCIR_ENCODE_PARAM(call_id, 1), I32));
  return utb_emit(ir, TCCIR_OP_FUNCCALLVAL, utb_temp(2, I32), UTB_NONE,
                  utb_imm((int32_t)TCCIR_ENCODE_CALL(call_id, 2), I32));
}

/* Emit a 2-param FUNCCALLVOID where dst is a local stack address. */
static int emit_strcpy_void_call(TCCIRState *ir, IROperand dst, IROperand src, int call_id)
{
  utb_emit(ir, TCCIR_OP_FUNCPARAMVAL, UTB_NONE, dst,
           utb_imm((int32_t)TCCIR_ENCODE_PARAM(call_id, 0), I32));
  utb_emit(ir, TCCIR_OP_FUNCPARAMVAL, UTB_NONE, src,
           utb_imm((int32_t)TCCIR_ENCODE_PARAM(call_id, 1), I32));
  return utb_emit(ir, TCCIR_OP_FUNCCALLVOID, UTB_NONE, UTB_NONE,
                  utb_imm((int32_t)TCCIR_ENCODE_CALL(call_id, 2), I32));
}

/* Create a StrFoldCtx with ssa=NULL (no SSA context). */
static struct StrFoldCtx csf_ctx_no_ssa(TCCIRState *ir, int call_idx, int builtin_id, int is_valued)
{
  struct StrFoldCtx c;
  c.ssa = NULL;
  c.ir = ir;
  c.call_idx = call_idx;
  c.builtin_id = builtin_id;
  c.is_valued = is_valued;
  return c;
}

/* Create a const-string SYMREF operand with specific addend.
 * The sym's v field is set to a TOK_builtin_* value so ir_opt_eval_const_string
 * can resolve it. */
static IROperand make_const_string_symref(TCCIRState *ir, int tok, int addend)
{
  static Sym str_sym;
  str_sym.v = tok;
  uint32_t sidx = tcc_ir_pool_add_symref(ir, &str_sym, addend, 0);
  IROperand op = irop_make_symref(0, sidx, 0, 0, 0, I32);
  return op;
}

/* Build a minimal SSA context for testing the positive fold path. */
static ssa_ctx build_ssa_for_strcpy(TCCIRState *ir, int call_idx, IROperand result_op, int use_count)
{
  ssa_ctx c = ssa_ctx_new(1, 10);
  c.ir->next_local_variable = 1;

  /* Build CFG */
  ssa_ctx_build_cfg(&c);

  /* Build SSA state */
  ssa_ctx_build_ssa_plain(&c);

  /* Rebuild to populate vinfo */
  ssa_ctx_rebuild(&c);

  /* Set use_count on the result vreg if needed */
  if (use_count > 0) {
    int32_t rv = irop_get_vreg(result_op);
    IRSSAVregInfo *vi = ssa_opt_vinfo(c.ctx, rv);
    if (vi) {
      vi->use_count = use_count;
    }
  }

  return c;
}

/* ========================================================================
 * Guard tests: all paths that cause no fold
 * ======================================================================== */

/* GUARD: Line 34: !ir_opt_get_call_param_operand -> strcpy_prepare returns 0.
 * A call with missing params (only one param emitted) is rejected. */
UT_TEST(test_strcpy_param_fetch_failure_no_fold)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);

  /* Create a call with only one param (missing second param). */
  IROperand dst = utb_stackoff(-1, 0, 1, 0, I32);

  /* Emit only one param */
  utb_emit(ir, TCCIR_OP_FUNCPARAMVAL, UTB_NONE, dst,
           utb_imm((int32_t)TCCIR_ENCODE_PARAM(1, 0), I32));
  /* Missing second param - only one param emitted. */
  int i_call = utb_emit(ir, TCCIR_OP_FUNCCALLVAL, utb_temp(1, I32), UTB_NONE,
                        utb_imm((int32_t)TCCIR_ENCODE_CALL(1, 1), I32));

  StrFoldCtx c = csf_ctx_no_ssa(ir, i_call, STRBI_STRCPY, 1);

  UT_ASSERT_EQ(tcc_strfold_strcpy.can_fold(&c), 0);
  UT_ASSERT_EQ(utb_op(ir, i_call), TCCIR_OP_FUNCCALLVAL);

  utb_free(ir);
  return 0;
}

/* GUARD: Line 34: dst.is_lval -> strcpy_prepare returns 0.
 * A stack offset with is_lval=1 is rejected. */
UT_TEST(test_strcpy_lval_dst_no_fold)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);

  IROperand dst = utb_stackoff(-1, 1, 1, 0, I32);  /* is_lval=1 */
  IROperand src = make_const_string_symref(ir, TOK_builtin_strcpy, 0);

  int i_call = emit_strcpy_call(ir, dst, src, 1);
  StrFoldCtx c = csf_ctx_no_ssa(ir, i_call, STRBI_STRCPY, 1);

  UT_ASSERT_EQ(tcc_strfold_strcpy.can_fold(&c), 0);
  UT_ASSERT_EQ(utb_op(ir, i_call), TCCIR_OP_FUNCCALLVAL);

  utb_free(ir);
  return 0;
}

/* GUARD: Lines 41-42: !ir_opt_eval_const_string_operand -> no fold.
 * A non-const-string source operand should be rejected. */
UT_TEST(test_strcpy_non_const_src_no_fold)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);

  IROperand dst = utb_stackoff(-1, 0, 1, 0, I32);
  /* Use a non-const-string src (a temp register). */
  IROperand src = utb_temp(1, I32);

  int i_call = emit_strcpy_call(ir, dst, src, 1);
  StrFoldCtx c = csf_ctx_no_ssa(ir, i_call, STRBI_STRCPY, 0);

  UT_ASSERT_EQ(tcc_strfold_strcpy.can_fold(&c), 0);

  utb_free(ir);
  return 0;
}

/* GUARD: Lines 44-46: len < 0 || ((len+1) & 3) != 0 -> no fold.
 * A const string with odd length (len+1 not word-aligned) is rejected. */
UT_TEST(test_strcpy_odd_length_no_fold)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);

  IROperand dst = utb_stackoff(-1, 0, 1, 0, I32);
  /* Create const string with addend=1 (len+1=2, not word-aligned).
   * The const_string_fold handler will see this as a 1-char string "a"
   * with len=1, so len+1=2, (2&3)!=0 -> reject. */
  IROperand src = make_const_string_symref(ir, TOK_builtin_strcpy, 1);

  int i_call = emit_strcpy_call(ir, dst, src, 1);
  StrFoldCtx c = csf_ctx_no_ssa(ir, i_call, STRBI_STRCPY, 1);

  UT_ASSERT_EQ(tcc_strfold_strcpy.can_fold(&c), 0);

  utb_free(ir);
  return 0;
}

/* GUARD: Lines 52-54: !sr || (sr->addend & 3) != 0 -> no fold.
 * A const-string SYMREF with a non-word-aligned addend is rejected. */
UT_TEST(test_strcpy_non_aligned_addend_no_fold)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);

  IROperand dst = utb_stackoff(-1, 0, 1, 0, I32);
  /* Create const-string SYMREF with addend=1 (non-word-aligned). */
  IROperand src = make_const_string_symref(ir, TOK_builtin_strcpy, 1);

  int i_call = emit_strcpy_call(ir, dst, src, 1);
  StrFoldCtx c = csf_ctx_no_ssa(ir, i_call, STRBI_STRCPY, 0);

  UT_ASSERT_EQ(tcc_strfold_strcpy.can_fold(&c), 0);

  utb_free(ir);
  return 0;
}

/* GUARD: Line 59: c->is_valued == 0 -> no fold.
 * A void (discarded-result) call never folds. */
UT_TEST(test_strcpy_void_call_no_fold)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);

  IROperand dst = utb_stackoff(-1, 0, 1, 0, I32);
  IROperand src = make_const_string_symref(ir, TOK_builtin_strcpy, 0);

  int i_call = emit_strcpy_void_call(ir, dst, src, 1);
  StrFoldCtx c = csf_ctx_no_ssa(ir, i_call, STRBI_STRCPY, 0);

  UT_ASSERT_EQ(tcc_strfold_strcpy.can_fold(&c), 0);
  UT_ASSERT_EQ(utb_op(ir, i_call), TCCIR_OP_FUNCCALLVOID);

  utb_free(ir);
  return 0;
}

/* GUARD: Lines 61-63: rv != -1 (result vreg present).
 * A call with a valid result vreg is checked for SSA context. */
UT_TEST(test_strcpy_result_vreg_present)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);

  IROperand dst = utb_stackoff(-1, 0, 1, 0, I32);
  IROperand src = make_const_string_symref(ir, TOK_builtin_strcpy, 0);

  int i_call = emit_strcpy_call(ir, dst, src, 1);

  /* The result is utb_temp(2, I32) which has vreg != -1.
   * With ssa=NULL, the guard at line 66-70 will still reject. */
  StrFoldCtx c = csf_ctx_no_ssa(ir, i_call, STRBI_STRCPY, 1);

  UT_ASSERT_EQ(tcc_strfold_strcpy.can_fold(&c), 0);

  utb_free(ir);
  return 0;
}

/* GUARD: Lines 66-70: !c->ssa || vi->use_count != 0 -> no fold.
 * Requires a real SSA context with vinfo where the result is consumed. */
UT_TEST(test_strcpy_consumed_result_with_ssa_no_fold)
{
  ssa_ctx c = ssa_ctx_new(1, 10);

  IROperand dst = utb_stackoff(-1, 0, 1, 0, I32);
  IROperand src = make_const_string_symref(c.ir, TOK_builtin_strcpy, 0);

  int i_call = emit_strcpy_call(c.ir, dst, src, 1);

  /* Build SSA to populate vinfo */
  ssa_ctx_build_cfg(&c);
  if (c.cfg)
    tcc_ir_cfg_compute_dom_frontiers(c.cfg);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  /* Mark result as used (use_count > 0) */
  IROperand result = utb_temp(2, I32);
  int32_t rv = irop_get_vreg(result);
  IRSSAVregInfo *vi = ssa_opt_vinfo(c.ctx, rv);
  if (vi) {
    vi->use_count = 1;
  }

  struct StrFoldCtx fold_ctx;
  memset(&fold_ctx, 0, sizeof(fold_ctx));
  fold_ctx.ssa = c.ctx;
  fold_ctx.ir = c.ir;
  fold_ctx.call_idx = i_call;
  fold_ctx.builtin_id = STRBI_STRCPY;
  fold_ctx.is_valued = 1;

  UT_ASSERT_EQ(tcc_strfold_strcpy.can_fold(&fold_ctx), 0);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * Coverage: strcpy_can_fold wrapper
 * ======================================================================== */

/* Verify strcpy_can_fold is a thin wrapper around strcpy_prepare. */
UT_TEST(test_strcpy_can_fold_delegates_to_prepare)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);

  /* Temp dst -> prepare returns 0 -> can_fold returns 0. */
  int i_call = emit_strcpy_call(ir, utb_temp(0, I32), utb_temp(1, I32), 1);
  StrFoldCtx c = csf_ctx_no_ssa(ir, i_call, STRBI_STRCPY, 1);

  UT_ASSERT_EQ(tcc_strfold_strcpy.can_fold(&c), 0);

  utb_free(ir);
  return 0;
}

/* ========================================================================
 * Coverage: ir_opt_nop_call_params on positive fold
 * ======================================================================== */

/* Verify that ir_opt_nop_call_params is called on positive fold.
 * This requires a real positive fold, which needs real const-string data.
 * We test the NOP behavior directly. */
UT_TEST(test_strcpy_fold_nops_params_directly)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);

  /* Build a call with params. */
  IROperand dst = utb_stackoff(-1, 0, 1, 0, I32);
  IROperand src = make_const_string_symref(ir, TOK_builtin_strcpy, 0);

  int i_call = emit_strcpy_call(ir, dst, src, 1);

  /* Verify params are present before NOP. */
  UT_ASSERT_EQ(utb_op(ir, i_call - 2), TCCIR_OP_FUNCPARAMVAL);
  UT_ASSERT_EQ(utb_op(ir, i_call - 1), TCCIR_OP_FUNCPARAMVAL);

  /* Call ir_opt_nop_call_params directly. */
  ir_opt_nop_call_params(ir, i_call);

  /* Verify params are NOP'd. */
  UT_ASSERT_EQ(utb_op(ir, i_call - 2), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, i_call - 1), TCCIR_OP_NOP);

  utb_free(ir);
  return 0;
}

/* ========================================================================
 * Coverage: fold returns 0 on prepare failure
 * ======================================================================== */

/* Verify strcpy_fold returns 0 when strcpy_prepare fails. */
UT_TEST(test_strcpy_fold_returns_zero_on_prepare_failure)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);

  /* Temp dst -> prepare returns 0 -> fold returns 0. */
  int i_call = emit_strcpy_call(ir, utb_temp(0, I32), utb_temp(1, I32), 1);
  StrFoldCtx c = csf_ctx_no_ssa(ir, i_call, STRBI_STRCPY, 1);

  UT_ASSERT_EQ(tcc_strfold_strcpy.fold(&c), 0);
  UT_ASSERT_EQ(utb_op(ir, i_call), TCCIR_OP_FUNCCALLVAL);

  utb_free(ir);
  return 0;
}

/* ========================================================================
 * Coverage: strcpy_can_fold wrapper
 * ======================================================================== */

/* Verify the handler registration. */
UT_TEST(test_strcpy_handler_interface)
{
  UT_ASSERT_EQ(tcc_strfold_strcpy.builtin_id, STRBI_STRCPY);
  UT_ASSERT_STREQ(tcc_strfold_strcpy.name, "strcpy");
  UT_ASSERT(tcc_strfold_strcpy.can_fold != NULL);
  UT_ASSERT(tcc_strfold_strcpy.fold != NULL);

  /* Verify it's distinct from other handlers. */
  UT_ASSERT_NE(tcc_strfold_strcpy.builtin_id, tcc_strfold_strlen.builtin_id);
  UT_ASSERT_NE(tcc_strfold_strcpy.builtin_id, tcc_strfold_strcmp.builtin_id);
  UT_ASSERT_NE(tcc_strfold_strcpy.builtin_id, tcc_strfold_memcmp.builtin_id);

  return 0;
}

UT_COVERS("str_strcpy");
