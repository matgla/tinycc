/*
 *  test_opt_nonneg_fold.c - suite for ir/opt_branch.c::tcc_ir_opt_nonneg_branch_fold
 *
 *  The non-negative branch fold tracks values known to be >= 0 (notably the
 *  result of calls such as `fabs`, `abs`, `strlen`, `sizeof`) and folds
 *  flag-setting soft-float comparisons against zero when the outcome is
 *  statically determined:
 *
 *      __aeabi_cdcmple(nonneg, 0)  + JUMPIF(GE/UGE/LT/ULT)
 *      __aeabi_cdcmple(0, nonneg)  + JUMPIF(LE/ULE/GT/UGT)
 *
 *  Always-true branches become unconditional JUMP; always-false branches have
 *  their JUMPIF NOP'd out.  The pass then runs DCE to clean up unreachable
 *  fall-through code.
 *
 *  These tests drive the bare pass entry point on hand-built IR.
 */

#include "ir_build.h"
#include "ut.h"

/* Pass entry point (declared in ir/opt.h). */
int tcc_ir_opt_nonneg_branch_fold(TCCIRState *ir);

#define I32 IROP_BTYPE_INT32

/* Token ids for the callees we synthesize.  They must be inside the
 * harness's settable get_tok_str table (0..255) and distinct from tcc.h
 * predefined tokens that happen to have string names. */
#define TOK_FABS      200
#define TOK_CDCMPLE   201
#define TOK_UNKNOWN   202

/* ------------------------------------------------------------------ helpers */

static IROperand utb_callee_named(TCCIRState *ir, Sym *sym, int tok)
{
  sym->v = tok;
  uint32_t sidx = tcc_ir_pool_add_symref(ir, sym, 0, 0);
  return irop_make_symref(0, sidx, 0, 0, 0, I32);
}

static int utb_emit_param(TCCIRState *ir, IROperand value, int call_id, int idx)
{
  return utb_emit(ir, TCCIR_OP_FUNCPARAMVAL, UTB_NONE, value,
                  utb_imm((int32_t)TCCIR_ENCODE_PARAM(call_id, idx), I32));
}

static int utb_emit_call1_val(TCCIRState *ir, IROperand callee, int call_id,
                              IROperand dest)
{
  return utb_emit(ir, TCCIR_OP_FUNCCALLVAL, dest, callee,
                  utb_imm((int32_t)TCCIR_ENCODE_CALL(call_id, 1), I32));
}

static int utb_emit_call2_void(TCCIRState *ir, IROperand callee, int call_id)
{
  return utb_emit(ir, TCCIR_OP_FUNCCALLVOID, UTB_NONE, callee,
                  utb_imm((int32_t)TCCIR_ENCODE_CALL(call_id, 2), I32));
}

/* ------------------------------------------------------------------ tests */

/* POSITIVE: fabs(x) is non-negative, so cdcmple(fabs(x), 0) with a GE branch
 * is always true.  The JUMPIF becomes an unconditional JUMP and the dead
 * fall-through RETURNVOID is eliminated by the follow-up DCE.
 *
 *   0: FUNCPARAMVAL  T0,           (call 1, param 0)
 *   1: T1 = FUNCCALLVAL fabs,      (call 1, argc 1)
 *   2: FUNCPARAMVAL  T1,           (call 2, param 0)
 *   3: FUNCPARAMVAL  #0,           (call 2, param 1)
 *   4: FUNCCALLVOID  __aeabi_cdcmple, (call 2, argc 2)
 *   5: JUMPIF        GE -> 7
 *   6: RETURNVOID
 *   7: RETURNVOID
 */
UT_TEST(test_nonneg_fold_fabs_ge_becomes_unconditional_jump)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);

  static Sym fabs_sym, cmp_sym;
  IROperand fabs_callee = utb_callee_named(ir, &fabs_sym, TOK_FABS);
  IROperand cmp_callee  = utb_callee_named(ir, &cmp_sym, TOK_CDCMPLE);
  utb_set_tok_str(TOK_FABS, "fabs");
  utb_set_tok_str(TOK_CDCMPLE, "__aeabi_cdcmple");

  utb_emit_param(ir, utb_temp(0, I32), 1, 0);
  utb_emit_call1_val(ir, fabs_callee, 1, utb_temp(1, I32));
  utb_emit_param(ir, utb_temp(1, I32), 2, 0);
  utb_emit_param(ir, utb_imm(0, I32), 2, 1);
  utb_emit_call2_void(ir, cmp_callee, 2);
  int ijmp = utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(7, I32), utb_imm(TOK_GE, I32), UTB_NONE);
  int ifall = utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);

  int changes = tcc_ir_opt_nonneg_branch_fold(ir);

  UT_ASSERT(changes > 0);
  UT_ASSERT_EQ(utb_op(ir, ijmp), TCCIR_OP_JUMP);
  UT_ASSERT_EQ((int)irop_get_imm64_ex(ir, utb_dest(ir, ijmp)), 7);
  UT_ASSERT_EQ(utb_op(ir, ifall), TCCIR_OP_NOP);

  utb_free(ir);
  return 0;
}

/* POSITIVE (reversed operands): cdcmple(0, nonneg) with a LE branch is also
 * always true.  This exercises the `nonneg_is_arg0 == 0` path.
 *
 *   0: FUNCPARAMVAL  T0,           (call 1, param 0)
 *   1: T1 = FUNCCALLVAL fabs,      (call 1, argc 1)
 *   2: FUNCPARAMVAL  #0,           (call 2, param 0)
 *   3: FUNCPARAMVAL  T1,           (call 2, param 1)
 *   4: FUNCCALLVOID  __aeabi_cdcmple, (call 2, argc 2)
 *   5: JUMPIF        LE -> 7
 *   6: RETURNVOID
 *   7: RETURNVOID
 */
UT_TEST(test_nonneg_fold_zero_le_nonneg_becomes_jump)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);

  static Sym fabs_sym, cmp_sym;
  IROperand fabs_callee = utb_callee_named(ir, &fabs_sym, TOK_FABS);
  IROperand cmp_callee  = utb_callee_named(ir, &cmp_sym, TOK_CDCMPLE);
  utb_set_tok_str(TOK_FABS, "fabs");
  utb_set_tok_str(TOK_CDCMPLE, "__aeabi_cdcmple");

  utb_emit_param(ir, utb_temp(0, I32), 1, 0);
  utb_emit_call1_val(ir, fabs_callee, 1, utb_temp(1, I32));
  utb_emit_param(ir, utb_imm(0, I32), 2, 0);
  utb_emit_param(ir, utb_temp(1, I32), 2, 1);
  utb_emit_call2_void(ir, cmp_callee, 2);
  int ijmp = utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(7, I32), utb_imm(TOK_LE, I32), UTB_NONE);
  int ifall = utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);

  int changes = tcc_ir_opt_nonneg_branch_fold(ir);

  UT_ASSERT(changes > 0);
  UT_ASSERT_EQ(utb_op(ir, ijmp), TCCIR_OP_JUMP);
  UT_ASSERT_EQ((int)irop_get_imm64_ex(ir, utb_dest(ir, ijmp)), 7);
  UT_ASSERT_EQ(utb_op(ir, ifall), TCCIR_OP_NOP);

  utb_free(ir);
  return 0;
}

/* NEGATIVE: the value feeding the comparison comes from a function that is not
 * in the non-negative whitelist, so the pass has no tracked nonneg vreg and
 * must leave the JUMPIF untouched. */
UT_TEST(test_nonneg_fold_unknown_source_does_not_fold)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);

  static Sym unk_sym, cmp_sym;
  IROperand unk_callee = utb_callee_named(ir, &unk_sym, TOK_UNKNOWN);
  IROperand cmp_callee = utb_callee_named(ir, &cmp_sym, TOK_CDCMPLE);
  utb_set_tok_str(TOK_UNKNOWN, "some_unknown_func");
  utb_set_tok_str(TOK_CDCMPLE, "__aeabi_cdcmple");

  utb_emit_param(ir, utb_temp(0, I32), 1, 0);
  utb_emit_call1_val(ir, unk_callee, 1, utb_temp(1, I32));
  utb_emit_param(ir, utb_temp(1, I32), 2, 0);
  utb_emit_param(ir, utb_imm(0, I32), 2, 1);
  utb_emit_call2_void(ir, cmp_callee, 2);
  int ijmp = utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(6, I32), utb_imm(TOK_GE, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);

  int changes = tcc_ir_opt_nonneg_branch_fold(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, ijmp), TCCIR_OP_JUMPIF);

  utb_free(ir);
  return 0;
}

/* FIXPOINT: after the first fold the IR is stable; a second invocation makes no
 * further changes and the result stays structurally well-formed. */
UT_TEST(test_nonneg_fold_idempotent)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);

  static Sym fabs_sym, cmp_sym;
  IROperand fabs_callee = utb_callee_named(ir, &fabs_sym, TOK_FABS);
  IROperand cmp_callee  = utb_callee_named(ir, &cmp_sym, TOK_CDCMPLE);
  utb_set_tok_str(TOK_FABS, "fabs");
  utb_set_tok_str(TOK_CDCMPLE, "__aeabi_cdcmple");

  utb_emit_param(ir, utb_temp(0, I32), 1, 0);
  utb_emit_call1_val(ir, fabs_callee, 1, utb_temp(1, I32));
  utb_emit_param(ir, utb_temp(1, I32), 2, 0);
  utb_emit_param(ir, utb_imm(0, I32), 2, 1);
  utb_emit_call2_void(ir, cmp_callee, 2);
  utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(7, I32), utb_imm(TOK_GE, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);

  int total = utb_run_to_fixpoint(ir, tcc_ir_opt_nonneg_branch_fold, 5);
  UT_ASSERT(total > 0);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 8), 0);

  utb_free(ir);
  return 0;
}

/* ------------------------------------------------------------------ suite */

UT_SUITE(opt_nonneg_fold)
{
  UT_COVERS("nonneg_fold");

  UT_RUN(test_nonneg_fold_fabs_ge_becomes_unconditional_jump);
  UT_RUN(test_nonneg_fold_zero_le_nonneg_becomes_jump);
  UT_RUN(test_nonneg_fold_unknown_source_does_not_fold);
  UT_RUN(test_nonneg_fold_idempotent);
}
