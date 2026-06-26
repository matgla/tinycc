/*
 *  test_opt_constfold.c - suite for ir/opt_constfold.c
 *
 *  Covers two name-gated call-folding passes from ir/opt_constfold.c:
 *
 *    - tcc_ir_opt_self_copy_elim   : NOPs a memcpy/memmove (or AAPCS aligned
 *      variant) call whose dst and src arguments are the same pure expression
 *      (a self-copy).  FUNCCALLVAL is rewritten to `ASSIGN dst`, FUNCCALLVOID
 *      to NOP, and the param marshalling is NOP'd.
 *    - tcc_ir_opt_float_narrowing  : collects __aeabi_f2d / __aeabi_d2f
 *      conversion calls and narrows floor()/ceil()/fabs()/... double helpers to
 *      their float variant when an argument is an f2d result.
 *
 *  HARNESS NOTES:
 *  Both passes are name-gated via get_tok_str(callee->v).  The unit-test harness
 *  now provides a settable token->name table (utb_set_tok_str), so self_copy_elim
 *  can be driven to its positive fold.  float_narrowing still cannot complete a
 *  true positive fold in isolation because change_callee_sym() calls
 *  external_global_sym(), which is a stubs.c link stub that returns NULL; the
 *  production pass does not check the return value of change_callee_sym(), so the
 *  transform is applied partially and is recorded as a suspected bug.
 *
 *  These are isolated tests: a hand-built IR sequence is run through the bare
 *  pass entry point and the resulting instructions are inspected directly.
 */

#include "ir_build.h"

#include "ut.h"

/* Pass entry points (declared in ir/opt.h; forward-declared to avoid pulling in
 * the optimizer engine headers). */
int tcc_ir_opt_self_copy_elim(TCCIRState *ir);
int tcc_ir_opt_float_narrowing(TCCIRState *ir);
int tcc_ir_opt_const_string_calls(TCCIRState *ir);
int tcc_ir_opt_const_call_replace(TCCIRState *ir);
int tcc_ir_opt_switch_call_replace(TCCIRState *ir);
int tcc_ir_opt_param_addrof_const_fold(TCCIRState *ir);
int tcc_ir_opt_local_addrof_const_fold(TCCIRState *ir);

/* Frontend link stubs (sym_push2 / external_global_sym / tok_alloc_const /
 * global_stack / elfsym) now live in stubs.c so the combined unit-test link
 * has a single definition.  external_global_sym() returning NULL prevents
 * float_narrowing from completing its callee swap in isolation. */

#define I32 IROP_BTYPE_INT32
#define F32 IROP_BTYPE_FLOAT32
#define F64 IROP_BTYPE_FLOAT64

#define VR_TMP(p) TCCIR_ENCODE_VREG(TCCIR_VREG_TYPE_TEMP, (p))

/* ----------------------------------------------------------- helpers */

/* Build a SYMREF operand whose token is `tok`, so multiple callees in one test
 * can be mapped to distinct names via utb_set_tok_str(). */
static IROperand utb_callee_named(TCCIRState *ir, Sym *sym, int tok)
{
  sym->v = tok;
  uint32_t sidx = tcc_ir_pool_add_symref(ir, sym, 0, 0);
  return irop_make_symref(0, sidx, 0, 0, 0, I32);
}

/* Build a SYMREF operand carrying a freshly-pooled callee Sym.  By default the
 * token maps to "?" (no fold), but callers may use utb_set_tok_str(sym->v, name)
 * before running the pass to exercise the real positive fold. */
static IROperand utb_callee(TCCIRState *ir, Sym *sym)
{
  return utb_callee_named(ir, sym, 0);
}

/* ----------------------------------------------------- self_copy_elim tests */

/* GUARD (would-fold-if-name-matched): a FUNCCALLVAL whose callee resolves to a
 * valid Sym and whose param0 (dst) and param1 (src) are the *identical* pure
 * value T0 — i.e. exactly the self-copy shape the pass targets.  The only thing
 * stopping the fold is that get_tok_str returns "?", which is not memcpy-like.
 * The pass must leave the call (and its params) untouched and return 0.
 *
 * This exercises the full path: the FUNCCALLVAL is detected, the callee Sym is
 * resolved, the name is looked up, and the memcpy-name gate rejects it BEFORE
 * the (would-succeed) param-equality test.  If the name gate were removed the
 * call would be rewritten to ASSIGN and this test would FAIL.
 *
 *   FUNCPARAMVAL  T0, param0   (dst)
 *   FUNCPARAMVAL  T0, param1   (src == dst, same pure expr)
 *   FUNCPARAMVAL  #16, param2  (n)
 *   T1 = FUNCCALLVAL <sym "?">, call_id=1 argc=3 */
UT_TEST(test_self_copy_elim_non_memcpy_name_no_fold)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir); /* needs the symref pool for the callee operand */

  static Sym callee_sym;
  IROperand callee = utb_callee(ir, &callee_sym);

  const int call_id = 1;
  int i_p0 = utb_emit(ir, TCCIR_OP_FUNCPARAMVAL, UTB_NONE, utb_temp(0, I32),
                      utb_imm((int32_t)TCCIR_ENCODE_PARAM(call_id, 0), I32));
  int i_p1 = utb_emit(ir, TCCIR_OP_FUNCPARAMVAL, UTB_NONE, utb_temp(0, I32),
                      utb_imm((int32_t)TCCIR_ENCODE_PARAM(call_id, 1), I32));
  utb_emit(ir, TCCIR_OP_FUNCPARAMVAL, UTB_NONE, utb_imm(16, I32),
           utb_imm((int32_t)TCCIR_ENCODE_PARAM(call_id, 2), I32));
  int i_call = utb_emit(ir, TCCIR_OP_FUNCCALLVAL, utb_temp(1, I32), callee,
                        utb_imm((int32_t)TCCIR_ENCODE_CALL(call_id, 3), I32));

  int changes = tcc_ir_opt_self_copy_elim(ir);

  /* Name does not match memcpy-like -> nothing rewritten or NOP'd. */
  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, i_call), TCCIR_OP_FUNCCALLVAL);
  UT_ASSERT_EQ(utb_op(ir, i_p0), TCCIR_OP_FUNCPARAMVAL);
  UT_ASSERT_EQ(utb_op(ir, i_p1), TCCIR_OP_FUNCPARAMVAL);

  utb_free(ir);
  return 0;
}

/* GUARD: a non-call instruction stream contains no FUNCCALLVAL/FUNCCALLVOID, so
 * the pass's outer loop never enters the body.  Pure structural negative: even a
 * trivial ASSIGN/RETURNVALUE pair must be returned untouched with 0 changes.
 * This pins the "no calls -> no work" behaviour independent of get_tok_str. */
UT_TEST(test_self_copy_elim_no_calls_no_fold)
{
  TCCIRState *ir = utb_new();

  int i_assign = utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(7, I32), UTB_NONE);
  int i_ret = utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_temp(0, I32), UTB_NONE);

  int changes = tcc_ir_opt_self_copy_elim(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, i_assign), TCCIR_OP_ASSIGN);
  UT_ASSERT_EQ(utb_op(ir, i_ret), TCCIR_OP_RETURNVALUE);

  utb_free(ir);
  return 0;
}

/* GUARD: a FUNCCALLVAL whose src1 is NOT a SYMREF (here an immediate) — so
 * irop_get_sym_ex() returns NULL and the pass `continue`s at the !callee check,
 * before any name lookup.  Confirms the null-callee early-out leaves the call
 * intact and reports 0 changes.  Also a NULL-deref smoke check for the helper. */
UT_TEST(test_self_copy_elim_null_callee_no_fold)
{
  TCCIRState *ir = utb_new();

  const int call_id = 2;
  utb_emit(ir, TCCIR_OP_FUNCPARAMVAL, UTB_NONE, utb_temp(0, I32),
           utb_imm((int32_t)TCCIR_ENCODE_PARAM(call_id, 0), I32));
  utb_emit(ir, TCCIR_OP_FUNCPARAMVAL, UTB_NONE, utb_temp(0, I32),
           utb_imm((int32_t)TCCIR_ENCODE_PARAM(call_id, 1), I32));
  /* src1 is an immediate, not a SYMREF -> no callee Sym. */
  int i_call = utb_emit(ir, TCCIR_OP_FUNCCALLVAL, utb_temp(1, I32), utb_imm(0, I32),
                        utb_imm((int32_t)TCCIR_ENCODE_CALL(call_id, 2), I32));

  int changes = tcc_ir_opt_self_copy_elim(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, i_call), TCCIR_OP_FUNCCALLVAL);

  utb_free(ir);
  return 0;
}

/* NULL-IR guard: tcc_ir_opt_self_copy_elim(NULL) must early-return 0 and not
 * dereference the state pointer. */
UT_TEST(test_self_copy_elim_null_ir)
{
  UT_ASSERT_EQ(tcc_ir_opt_self_copy_elim(NULL), 0);
  return 0;
}

/* POSITIVE: a real memcpy self-copy is rewritten to ASSIGN dst (== src).  This
 * exercises the settable get_tok_str table added in the Phase B2 harness
 * extensions: token 0 is mapped to "memcpy", so the name gate matches and the
 * param-equality check succeeds.
 *
 *   FUNCPARAMVAL  T0, param0   (dst)
 *   FUNCPARAMVAL  T0, param1   (src == dst)
 *   FUNCPARAMVAL  #16, param2  (n)
 *   T1 = FUNCCALLVAL <sym "memcpy">, call_id=1 argc=3
 *      -> T1 = ASSIGN T0 */
UT_TEST(test_self_copy_elim_memcpy_positive)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);

  static Sym callee_sym;
  IROperand callee = utb_callee(ir, &callee_sym);
  utb_set_tok_str(callee_sym.v, "memcpy");

  const int call_id = 1;
  utb_emit(ir, TCCIR_OP_FUNCPARAMVAL, UTB_NONE, utb_temp(0, I32),
           utb_imm((int32_t)TCCIR_ENCODE_PARAM(call_id, 0), I32));
  utb_emit(ir, TCCIR_OP_FUNCPARAMVAL, UTB_NONE, utb_temp(0, I32),
           utb_imm((int32_t)TCCIR_ENCODE_PARAM(call_id, 1), I32));
  utb_emit(ir, TCCIR_OP_FUNCPARAMVAL, UTB_NONE, utb_imm(16, I32),
           utb_imm((int32_t)TCCIR_ENCODE_PARAM(call_id, 2), I32));
  int i_call = utb_emit(ir, TCCIR_OP_FUNCCALLVAL, utb_temp(1, I32), callee,
                        utb_imm((int32_t)TCCIR_ENCODE_CALL(call_id, 3), I32));

  int changes = tcc_ir_opt_self_copy_elim(ir);

  UT_ASSERT_EQ(changes, 1);
  UT_ASSERT_EQ(utb_op(ir, i_call), TCCIR_OP_ASSIGN);
  UT_ASSERT_EQ(utb_vreg(utb_src1(ir, i_call)), VR_TMP(0));

  /* Reset token table so later tests are not affected. */
  utb_set_tok_str(callee_sym.v, NULL);

  utb_free(ir);
  return 0;
}

/* POSITIVE: memmove self-copy folds exactly like memcpy. */
UT_TEST(test_self_copy_elim_memmove_positive)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);

  static Sym callee_sym;
  IROperand callee = utb_callee(ir, &callee_sym);
  utb_set_tok_str(callee_sym.v, "memmove");

  const int call_id = 3;
  utb_emit(ir, TCCIR_OP_FUNCPARAMVAL, UTB_NONE, utb_temp(0, I32),
           utb_imm((int32_t)TCCIR_ENCODE_PARAM(call_id, 0), I32));
  utb_emit(ir, TCCIR_OP_FUNCPARAMVAL, UTB_NONE, utb_temp(0, I32),
           utb_imm((int32_t)TCCIR_ENCODE_PARAM(call_id, 1), I32));
  utb_emit(ir, TCCIR_OP_FUNCPARAMVAL, UTB_NONE, utb_imm(16, I32),
           utb_imm((int32_t)TCCIR_ENCODE_PARAM(call_id, 2), I32));
  int i_call = utb_emit(ir, TCCIR_OP_FUNCCALLVAL, utb_temp(1, I32), callee,
                        utb_imm((int32_t)TCCIR_ENCODE_CALL(call_id, 3), I32));

  int changes = tcc_ir_opt_self_copy_elim(ir);

  UT_ASSERT_EQ(changes, 1);
  UT_ASSERT_EQ(utb_op(ir, i_call), TCCIR_OP_ASSIGN);
  UT_ASSERT_EQ(utb_vreg(utb_src1(ir, i_call)), VR_TMP(0));
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 16), 0);

  utb_set_tok_str(callee_sym.v, NULL);
  utb_free(ir);
  return 0;
}

/* POSITIVE: AAPCS aligned memcpy variant self-copy folds. */
UT_TEST(test_self_copy_elim_aeabi_memcpy8_positive)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);

  static Sym callee_sym;
  IROperand callee = utb_callee(ir, &callee_sym);
  utb_set_tok_str(callee_sym.v, "__aeabi_memcpy8");

  const int call_id = 4;
  utb_emit(ir, TCCIR_OP_FUNCPARAMVAL, UTB_NONE, utb_temp(0, I32),
           utb_imm((int32_t)TCCIR_ENCODE_PARAM(call_id, 0), I32));
  utb_emit(ir, TCCIR_OP_FUNCPARAMVAL, UTB_NONE, utb_temp(0, I32),
           utb_imm((int32_t)TCCIR_ENCODE_PARAM(call_id, 1), I32));
  utb_emit(ir, TCCIR_OP_FUNCPARAMVAL, UTB_NONE, utb_imm(8, I32),
           utb_imm((int32_t)TCCIR_ENCODE_PARAM(call_id, 2), I32));
  int i_call = utb_emit(ir, TCCIR_OP_FUNCCALLVAL, utb_temp(1, I32), callee,
                        utb_imm((int32_t)TCCIR_ENCODE_CALL(call_id, 3), I32));

  int changes = tcc_ir_opt_self_copy_elim(ir);

  UT_ASSERT_EQ(changes, 1);
  UT_ASSERT_EQ(utb_op(ir, i_call), TCCIR_OP_ASSIGN);
  UT_ASSERT_EQ(utb_vreg(utb_src1(ir, i_call)), VR_TMP(0));
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 16), 0);

  utb_set_tok_str(callee_sym.v, NULL);
  utb_free(ir);
  return 0;
}

/* POSITIVE: a FUNCCALLVOID self-copy is rewritten to NOP and its params are
 * NOP'd.  The result value is discarded, so ASSIGN is not appropriate. */
UT_TEST(test_self_copy_elim_void_call_positive)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);

  static Sym callee_sym;
  IROperand callee = utb_callee(ir, &callee_sym);
  utb_set_tok_str(callee_sym.v, "memcpy");

  const int call_id = 5;
  int i_p0 = utb_emit(ir, TCCIR_OP_FUNCPARAMVAL, UTB_NONE, utb_temp(0, I32),
                      utb_imm((int32_t)TCCIR_ENCODE_PARAM(call_id, 0), I32));
  int i_p1 = utb_emit(ir, TCCIR_OP_FUNCPARAMVAL, UTB_NONE, utb_temp(0, I32),
                      utb_imm((int32_t)TCCIR_ENCODE_PARAM(call_id, 1), I32));
  int i_call = utb_emit(ir, TCCIR_OP_FUNCCALLVOID, UTB_NONE, callee,
                        utb_imm((int32_t)TCCIR_ENCODE_CALL(call_id, 2), I32));

  int changes = tcc_ir_opt_self_copy_elim(ir);

  UT_ASSERT_EQ(changes, 1);
  UT_ASSERT_EQ(utb_op(ir, i_call), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, i_p0), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, i_p1), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 16), 0);

  utb_set_tok_str(callee_sym.v, NULL);
  utb_free(ir);
  return 0;
}

/* Negative: dst and src are different vregs, so the copy is not a self-copy
 * even though the callee name matches. */
UT_TEST(test_self_copy_elim_dst_src_differ_no_fold)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);

  static Sym callee_sym;
  IROperand callee = utb_callee(ir, &callee_sym);
  utb_set_tok_str(callee_sym.v, "memcpy");

  const int call_id = 6;
  utb_emit(ir, TCCIR_OP_FUNCPARAMVAL, UTB_NONE, utb_temp(0, I32),
           utb_imm((int32_t)TCCIR_ENCODE_PARAM(call_id, 0), I32));
  utb_emit(ir, TCCIR_OP_FUNCPARAMVAL, UTB_NONE, utb_temp(1, I32),
           utb_imm((int32_t)TCCIR_ENCODE_PARAM(call_id, 1), I32));
  int i_call = utb_emit(ir, TCCIR_OP_FUNCCALLVAL, utb_temp(2, I32), callee,
                        utb_imm((int32_t)TCCIR_ENCODE_CALL(call_id, 2), I32));

  int changes = tcc_ir_opt_self_copy_elim(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, i_call), TCCIR_OP_FUNCCALLVAL);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 16), 0);

  utb_set_tok_str(callee_sym.v, NULL);
  utb_free(ir);
  return 0;
}

/* FIXED: the temp used for dst is redefined before src, so the two uses no
 * longer refer to the same definition.  self_copy_elim now resolves each param
 * at its own FUNCPARAMVAL marshalling site (not the call index), so param0
 * (T0==10) and param1 (T0==20) resolve to different reaching definitions and
 * the self-copy fold correctly does NOT fire. */
UT_TEST(test_self_copy_elim_redefined_temp_suspected_bug)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);

  static Sym callee_sym;
  IROperand callee = utb_callee(ir, &callee_sym);
  utb_set_tok_str(callee_sym.v, "memcpy");

  const int call_id = 7;
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(10, I32), UTB_NONE);
  int i_p0 = utb_emit(ir, TCCIR_OP_FUNCPARAMVAL, UTB_NONE, utb_temp(0, I32),
                      utb_imm((int32_t)TCCIR_ENCODE_PARAM(call_id, 0), I32));
  /* Redefine T0 between the two param uses. */
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(20, I32), UTB_NONE);
  int i_p1 = utb_emit(ir, TCCIR_OP_FUNCPARAMVAL, UTB_NONE, utb_temp(0, I32),
                      utb_imm((int32_t)TCCIR_ENCODE_PARAM(call_id, 1), I32));
  int i_call = utb_emit(ir, TCCIR_OP_FUNCCALLVAL, utb_temp(1, I32), callee,
                        utb_imm((int32_t)TCCIR_ENCODE_CALL(call_id, 2), I32));

  int changes = tcc_ir_opt_self_copy_elim(ir);

  /* FIXED: dst/src are not the same value (T0 was redefined between params),
   * so the fold must not fire — the call and both params are left intact. */
  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, i_call), TCCIR_OP_FUNCCALLVAL);
  UT_ASSERT_EQ(utb_op(ir, i_p0), TCCIR_OP_FUNCPARAMVAL);
  UT_ASSERT_EQ(utb_op(ir, i_p1), TCCIR_OP_FUNCPARAMVAL);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 16), 0);

  utb_set_tok_str(callee_sym.v, NULL);
  utb_free(ir);
  return 0;
}

/* Negative: one param is an lval and the other is not.  The two operands have
 * different semantics (deref vs address/value), so equality fails. */
UT_TEST(test_self_copy_elim_lval_mismatch_no_fold)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);

  static Sym callee_sym;
  IROperand callee = utb_callee(ir, &callee_sym);
  utb_set_tok_str(callee_sym.v, "memcpy");

  const int call_id = 8;
  utb_emit(ir, TCCIR_OP_FUNCPARAMVAL, UTB_NONE, utb_lval(utb_temp(0, I32)),
           utb_imm((int32_t)TCCIR_ENCODE_PARAM(call_id, 0), I32));
  utb_emit(ir, TCCIR_OP_FUNCPARAMVAL, UTB_NONE, utb_temp(0, I32),
           utb_imm((int32_t)TCCIR_ENCODE_PARAM(call_id, 1), I32));
  int i_call = utb_emit(ir, TCCIR_OP_FUNCCALLVAL, utb_temp(1, I32), callee,
                        utb_imm((int32_t)TCCIR_ENCODE_CALL(call_id, 2), I32));

  int changes = tcc_ir_opt_self_copy_elim(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, i_call), TCCIR_OP_FUNCCALLVAL);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 16), 0);

  utb_set_tok_str(callee_sym.v, NULL);
  utb_free(ir);
  return 0;
}

/* Idempotence: a second run after a successful fold reports no changes. */
UT_TEST(test_self_copy_elim_idempotent)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);

  static Sym callee_sym;
  IROperand callee = utb_callee(ir, &callee_sym);
  utb_set_tok_str(callee_sym.v, "memcpy");

  const int call_id = 9;
  utb_emit(ir, TCCIR_OP_FUNCPARAMVAL, UTB_NONE, utb_temp(0, I32),
           utb_imm((int32_t)TCCIR_ENCODE_PARAM(call_id, 0), I32));
  utb_emit(ir, TCCIR_OP_FUNCPARAMVAL, UTB_NONE, utb_temp(0, I32),
           utb_imm((int32_t)TCCIR_ENCODE_PARAM(call_id, 1), I32));
  utb_emit(ir, TCCIR_OP_FUNCPARAMVAL, UTB_NONE, utb_imm(16, I32),
           utb_imm((int32_t)TCCIR_ENCODE_PARAM(call_id, 2), I32));
  utb_emit(ir, TCCIR_OP_FUNCCALLVAL, utb_temp(1, I32), callee,
           utb_imm((int32_t)TCCIR_ENCODE_CALL(call_id, 3), I32));

  int total = utb_run_to_fixpoint(ir, tcc_ir_opt_self_copy_elim, 10);

  UT_ASSERT_EQ(total, 1);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 16), 0);

  utb_set_tok_str(callee_sym.v, NULL);
  utb_free(ir);
  return 0;
}

/* ---------------------------------------------------- float_narrowing tests */

/* GUARD (would-narrow-if-names-matched): a textbook f2d -> floor -> d2f chain
 * (the exact Case-1 shape tcc_ir_opt_float_narrowing rewrites).  Phase 1 scans
 * for __aeabi_f2d / __aeabi_d2f by name; under the "?" stub it finds none, so
 * num_f2d == 0 and the pass returns 0 at the early-out, leaving every call and
 * param intact.  If the f2d/d2f name gate were dropped, the floor call would be
 * narrowed and the f2d/d2f calls NOP'd, failing these assertions.
 *
 *   FUNCPARAMVAL  Tf(float), param0   (call_id 1)
 *   Td = FUNCCALLVAL <sym "?">         (would be __aeabi_f2d: float->double)
 *   FUNCPARAMVAL  Td(double), param0  (call_id 2)
 *   Tr = FUNCCALLVAL <sym "?">         (would be floor: double->double)
 *   FUNCPARAMVAL  Tr(double), param0  (call_id 3)
 *   Tf2 = FUNCCALLVAL <sym "?">        (would be __aeabi_d2f: double->float) */
UT_TEST(test_float_narrowing_unmatched_names_no_fold)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);

  static Sym f2d_sym, floor_sym, d2f_sym;
  IROperand f2d_callee = utb_callee(ir, &f2d_sym);
  IROperand floor_callee = utb_callee(ir, &floor_sym);
  IROperand d2f_callee = utb_callee(ir, &d2f_sym);

  /* f2d: float Tf(0) -> double Td(1) */
  utb_emit(ir, TCCIR_OP_FUNCPARAMVAL, UTB_NONE, utb_temp(0, F32),
           utb_imm((int32_t)TCCIR_ENCODE_PARAM(1, 0), I32));
  int i_f2d = utb_emit(ir, TCCIR_OP_FUNCCALLVAL, utb_temp(1, F64), f2d_callee,
                       utb_imm((int32_t)TCCIR_ENCODE_CALL(1, 1), I32));
  /* floor: double Td(1) -> double Tr(2) */
  utb_emit(ir, TCCIR_OP_FUNCPARAMVAL, UTB_NONE, utb_temp(1, F64),
           utb_imm((int32_t)TCCIR_ENCODE_PARAM(2, 0), I32));
  int i_floor = utb_emit(ir, TCCIR_OP_FUNCCALLVAL, utb_temp(2, F64), floor_callee,
                         utb_imm((int32_t)TCCIR_ENCODE_CALL(2, 1), I32));
  /* d2f: double Tr(2) -> float Tf2(3) */
  utb_emit(ir, TCCIR_OP_FUNCPARAMVAL, UTB_NONE, utb_temp(2, F64),
           utb_imm((int32_t)TCCIR_ENCODE_PARAM(3, 0), I32));
  int i_d2f = utb_emit(ir, TCCIR_OP_FUNCCALLVAL, utb_temp(3, F32), d2f_callee,
                       utb_imm((int32_t)TCCIR_ENCODE_CALL(3, 1), I32));

  int changes = tcc_ir_opt_float_narrowing(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, i_f2d), TCCIR_OP_FUNCCALLVAL);
  UT_ASSERT_EQ(utb_op(ir, i_floor), TCCIR_OP_FUNCCALLVAL);
  UT_ASSERT_EQ(utb_op(ir, i_d2f), TCCIR_OP_FUNCCALLVAL);

  utb_free(ir);
  return 0;
}

/* GUARD: too few instructions.  tcc_ir_opt_float_narrowing requires at least 4
 * instructions (n < 4 -> return 0) before doing any scanning.  A 2-instruction
 * f2d-shaped pair must short-circuit to 0 with the IR untouched. */
UT_TEST(test_float_narrowing_too_few_instructions_no_fold)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);

  static Sym f2d_sym;
  IROperand f2d_callee = utb_callee(ir, &f2d_sym);

  utb_emit(ir, TCCIR_OP_FUNCPARAMVAL, UTB_NONE, utb_temp(0, F32),
           utb_imm((int32_t)TCCIR_ENCODE_PARAM(1, 0), I32));
  int i_call = utb_emit(ir, TCCIR_OP_FUNCCALLVAL, utb_temp(1, F64), f2d_callee,
                        utb_imm((int32_t)TCCIR_ENCODE_CALL(1, 1), I32));

  int changes = tcc_ir_opt_float_narrowing(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, i_call), TCCIR_OP_FUNCCALLVAL);

  utb_free(ir);
  return 0;
}

/* NOTE: a real f2d -> floor -> d2f narrowing positive cannot be tested in this
 * isolated harness.  Once Phase 2 matches the narrowable middle function it
 * calls change_callee_sym(), which calls sym_push2() / external_global_sym();
 * both are stubs in tests/unit/arm/armv8m/stubs.c that return NULL, and
 * change_callee_sym() dereferences the NULL sym_push2() result before it can
 * report failure.  In a real compilation the helper exists and the transform
 * completes.  This limitation is recorded in the agent conclusion. */

/* Negative: f2d and d2f names match but the middle function is not in the
 * narrowable table, so Phase 2 never triggers. */
UT_TEST(test_float_narrowing_non_narrowable_middle_no_fold)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);

  static Sym f2d_sym, middle_sym, d2f_sym;
  IROperand f2d_callee = utb_callee_named(ir, &f2d_sym, 20);
  IROperand middle_callee = utb_callee_named(ir, &middle_sym, 21);
  IROperand d2f_callee = utb_callee_named(ir, &d2f_sym, 22);

  utb_set_tok_str(20, "__aeabi_f2d");
  utb_set_tok_str(21, "some_non_narrowable_func");
  utb_set_tok_str(22, "__aeabi_d2f");

  /* f2d: T0 -> T1, call_id 1 */
  utb_emit(ir, TCCIR_OP_FUNCPARAMVAL, UTB_NONE, utb_temp(0, F32),
           utb_imm((int32_t)TCCIR_ENCODE_PARAM(1, 0), I32));
  int i_f2d = utb_emit(ir, TCCIR_OP_FUNCCALLVAL, utb_temp(1, F64), f2d_callee,
                       utb_imm((int32_t)TCCIR_ENCODE_CALL(1, 1), I32));
  /* middle: T1 -> T2, call_id 2 */
  utb_emit(ir, TCCIR_OP_FUNCPARAMVAL, UTB_NONE, utb_temp(1, F64),
           utb_imm((int32_t)TCCIR_ENCODE_PARAM(2, 0), I32));
  int i_middle = utb_emit(ir, TCCIR_OP_FUNCCALLVAL, utb_temp(2, F64), middle_callee,
                          utb_imm((int32_t)TCCIR_ENCODE_CALL(2, 1), I32));
  /* d2f: T2 -> T3, call_id 3 */
  utb_emit(ir, TCCIR_OP_FUNCPARAMVAL, UTB_NONE, utb_temp(2, F64),
           utb_imm((int32_t)TCCIR_ENCODE_PARAM(3, 0), I32));
  int i_d2f = utb_emit(ir, TCCIR_OP_FUNCCALLVAL, utb_temp(3, F32), d2f_callee,
                       utb_imm((int32_t)TCCIR_ENCODE_CALL(3, 1), I32));

  int changes = tcc_ir_opt_float_narrowing(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, i_f2d), TCCIR_OP_FUNCCALLVAL);
  UT_ASSERT_EQ(utb_op(ir, i_middle), TCCIR_OP_FUNCCALLVAL);
  UT_ASSERT_EQ(utb_op(ir, i_d2f), TCCIR_OP_FUNCCALLVAL);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 16), 0);

  utb_set_tok_str(20, NULL);
  utb_set_tok_str(21, NULL);
  utb_set_tok_str(22, NULL);
  utb_free(ir);
  return 0;
}

/* Negative: f2d -> func shape with no trailing d2f.  The middle function name
 * is not set, so the pass declines even though an f2d call is present. */
UT_TEST(test_float_narrowing_missing_d2f_no_fold)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);

  static Sym f2d_sym, floor_sym;
  IROperand f2d_callee = utb_callee_named(ir, &f2d_sym, 30);
  IROperand floor_callee = utb_callee_named(ir, &floor_sym, 31);

  utb_set_tok_str(30, "__aeabi_f2d");
  /* leave floor name as "?" so it does not match the narrowable table */

  /* f2d: T0 -> T1, call_id 1 */
  utb_emit(ir, TCCIR_OP_FUNCPARAMVAL, UTB_NONE, utb_temp(0, F32),
           utb_imm((int32_t)TCCIR_ENCODE_PARAM(1, 0), I32));
  int i_f2d = utb_emit(ir, TCCIR_OP_FUNCCALLVAL, utb_temp(1, F64), f2d_callee,
                       utb_imm((int32_t)TCCIR_ENCODE_CALL(1, 1), I32));
  /* floor: T1 -> T2, call_id 2 */
  utb_emit(ir, TCCIR_OP_FUNCPARAMVAL, UTB_NONE, utb_temp(1, F64),
           utb_imm((int32_t)TCCIR_ENCODE_PARAM(2, 0), I32));
  int i_floor = utb_emit(ir, TCCIR_OP_FUNCCALLVAL, utb_temp(2, F64), floor_callee,
                         utb_imm((int32_t)TCCIR_ENCODE_CALL(2, 1), I32));

  int changes = tcc_ir_opt_float_narrowing(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, i_f2d), TCCIR_OP_FUNCCALLVAL);
  UT_ASSERT_EQ(utb_op(ir, i_floor), TCCIR_OP_FUNCCALLVAL);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 16), 0);

  utb_set_tok_str(30, NULL);
  utb_free(ir);
  return 0;
}

/* Negative: the narrowable function's param0 is not the f2d result, so no
 * narrowing candidate is found. */
UT_TEST(test_float_narrowing_f2d_not_consumed_no_fold)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);

  static Sym f2d_sym, floor_sym, d2f_sym;
  IROperand f2d_callee = utb_callee_named(ir, &f2d_sym, 40);
  IROperand floor_callee = utb_callee_named(ir, &floor_sym, 41);
  IROperand d2f_callee = utb_callee_named(ir, &d2f_sym, 42);

  utb_set_tok_str(40, "__aeabi_f2d");
  utb_set_tok_str(41, "floor");
  utb_set_tok_str(42, "__aeabi_d2f");

  /* f2d: T0 -> T1, call_id 1 (result T1 is unused by floor) */
  utb_emit(ir, TCCIR_OP_FUNCPARAMVAL, UTB_NONE, utb_temp(0, F32),
           utb_imm((int32_t)TCCIR_ENCODE_PARAM(1, 0), I32));
  int i_f2d = utb_emit(ir, TCCIR_OP_FUNCCALLVAL, utb_temp(1, F64), f2d_callee,
                       utb_imm((int32_t)TCCIR_ENCODE_CALL(1, 1), I32));
  /* floor: T4 -> T2, call_id 2 (param0 is T4, not T1) */
  utb_emit(ir, TCCIR_OP_FUNCPARAMVAL, UTB_NONE, utb_temp(4, F64),
           utb_imm((int32_t)TCCIR_ENCODE_PARAM(2, 0), I32));
  int i_floor = utb_emit(ir, TCCIR_OP_FUNCCALLVAL, utb_temp(2, F64), floor_callee,
                         utb_imm((int32_t)TCCIR_ENCODE_CALL(2, 1), I32));
  /* d2f: T2 -> T3, call_id 3 */
  utb_emit(ir, TCCIR_OP_FUNCPARAMVAL, UTB_NONE, utb_temp(2, F64),
           utb_imm((int32_t)TCCIR_ENCODE_PARAM(3, 0), I32));
  int i_d2f = utb_emit(ir, TCCIR_OP_FUNCCALLVAL, utb_temp(3, F32), d2f_callee,
                       utb_imm((int32_t)TCCIR_ENCODE_CALL(3, 1), I32));

  int changes = tcc_ir_opt_float_narrowing(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, i_f2d), TCCIR_OP_FUNCCALLVAL);
  UT_ASSERT_EQ(utb_op(ir, i_floor), TCCIR_OP_FUNCCALLVAL);
  UT_ASSERT_EQ(utb_op(ir, i_d2f), TCCIR_OP_FUNCCALLVAL);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 16), 0);

  utb_set_tok_str(40, NULL);
  utb_set_tok_str(41, NULL);
  utb_set_tok_str(42, NULL);
  utb_free(ir);
  return 0;
}

/* Negative: no f2d call at all -> num_f2d == 0 and the pass early-outs. */
UT_TEST(test_float_narrowing_no_f2d_no_fold)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);

  static Sym floor_sym, d2f_sym;
  IROperand floor_callee = utb_callee_named(ir, &floor_sym, 50);
  IROperand d2f_callee = utb_callee_named(ir, &d2f_sym, 51);

  utb_set_tok_str(50, "floor");
  utb_set_tok_str(51, "__aeabi_d2f");

  /* floor: T0 -> T1, call_id 1 */
  utb_emit(ir, TCCIR_OP_FUNCPARAMVAL, UTB_NONE, utb_temp(0, F64),
           utb_imm((int32_t)TCCIR_ENCODE_PARAM(1, 0), I32));
  int i_floor = utb_emit(ir, TCCIR_OP_FUNCCALLVAL, utb_temp(1, F64), floor_callee,
                         utb_imm((int32_t)TCCIR_ENCODE_CALL(1, 1), I32));
  /* d2f: T1 -> T2, call_id 2 */
  utb_emit(ir, TCCIR_OP_FUNCPARAMVAL, UTB_NONE, utb_temp(1, F64),
           utb_imm((int32_t)TCCIR_ENCODE_PARAM(2, 0), I32));
  int i_d2f = utb_emit(ir, TCCIR_OP_FUNCCALLVAL, utb_temp(2, F32), d2f_callee,
                       utb_imm((int32_t)TCCIR_ENCODE_CALL(2, 1), I32));

  int changes = tcc_ir_opt_float_narrowing(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, i_floor), TCCIR_OP_FUNCCALLVAL);
  UT_ASSERT_EQ(utb_op(ir, i_d2f), TCCIR_OP_FUNCCALLVAL);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 16), 0);

  utb_set_tok_str(50, NULL);
  utb_set_tok_str(51, NULL);
  utb_free(ir);
  return 0;
}

/* Idempotence: the pass converges on a chain that does not match. */
UT_TEST(test_float_narrowing_idempotent)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);

  static Sym f2d_sym, floor_sym, d2f_sym;
  IROperand f2d_callee = utb_callee(ir, &f2d_sym);
  IROperand floor_callee = utb_callee(ir, &floor_sym);
  IROperand d2f_callee = utb_callee(ir, &d2f_sym);

  /* f2d: T0 -> T1, call_id 1 */
  utb_emit(ir, TCCIR_OP_FUNCPARAMVAL, UTB_NONE, utb_temp(0, F32),
           utb_imm((int32_t)TCCIR_ENCODE_PARAM(1, 0), I32));
  utb_emit(ir, TCCIR_OP_FUNCCALLVAL, utb_temp(1, F64), f2d_callee,
           utb_imm((int32_t)TCCIR_ENCODE_CALL(1, 1), I32));
  /* floor: T1 -> T2, call_id 2 */
  utb_emit(ir, TCCIR_OP_FUNCPARAMVAL, UTB_NONE, utb_temp(1, F64),
           utb_imm((int32_t)TCCIR_ENCODE_PARAM(2, 0), I32));
  utb_emit(ir, TCCIR_OP_FUNCCALLVAL, utb_temp(2, F64), floor_callee,
           utb_imm((int32_t)TCCIR_ENCODE_CALL(2, 1), I32));
  /* d2f: T2 -> T3, call_id 3 */
  utb_emit(ir, TCCIR_OP_FUNCPARAMVAL, UTB_NONE, utb_temp(2, F64),
           utb_imm((int32_t)TCCIR_ENCODE_PARAM(3, 0), I32));
  utb_emit(ir, TCCIR_OP_FUNCCALLVAL, utb_temp(3, F32), d2f_callee,
           utb_imm((int32_t)TCCIR_ENCODE_CALL(3, 1), I32));

  int total = utb_run_to_fixpoint(ir, tcc_ir_opt_float_narrowing, 10);

  UT_ASSERT_EQ(total, 0);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 16), 0);

  utb_free(ir);
  return 0;
}

/* ================================================================
 *  const_string_calls / const_call_replace / switch_call_replace /
 *  param_addrof_const_fold / local_addrof_const_fold
 * ================================================================ */

#define VR_PARAM(p) TCCIR_ENCODE_VREG(TCCIR_VREG_TYPE_PARAM, (p))
#define VR_VAR(v)   TCCIR_ENCODE_VREG(TCCIR_VREG_TYPE_VAR, (v))

/* A VREG operand carrying an is_local flag (the form a LEA's src1 takes for an
 * address-of-a-PARAM/VAR; production sets is_local on the addressed operand). */
static IROperand utb_local_vreg(int32_t vreg, int btype)
{
  IROperand op = irop_make_vreg(vreg, btype);
  op.is_local = 1;
  return op;
}

/* A STACKOFF operand whose encoded vreg is `vreg` (so irop_get_vreg() decodes the
 * PARAM/VAR position the addrof passes look for) and which is an lval (the
 * "value at the spill slot" read form). */
static IROperand utb_slot_lval(int32_t vreg, int32_t offset, int btype)
{
  return irop_make_stackoff(vreg, offset, 1 /* is_lval */, 0 /* is_llocal */, 0 /* is_param */, btype);
}

/* Reset the shared tcc_state IPC caches between tests (they live on the global
 * TCCState provided by tcc_state_stub.c). */
static void utb_reset_ipc_caches(void)
{
  tcc_state->func_const_result_cache_count = 0;
  tcc_ir_free_switch_func_cache(tcc_state);
}

/* Allocate zeroed live-interval arrays so the addrof passes' Phase-3
 * tcc_ir_get_live_interval() (which exit(1)s on an out-of-bounds vreg) is safe.
 * All flags default to 0 (scalar, not addrtaken). */
static void utb_alloc_param_intervals(TCCIRState *ir, int count)
{
  ir->parameters_live_intervals = (IRLiveInterval *)tcc_mallocz(sizeof(IRLiveInterval) * count);
  ir->parameters_live_intervals_size = count;
}

static void utb_alloc_var_intervals(TCCIRState *ir, int count)
{
  ir->variables_live_intervals = (IRLiveInterval *)tcc_mallocz(sizeof(IRLiveInterval) * count);
  ir->variables_live_intervals_size = count;
}

/* --------------------------------------------------- const_string_calls */

/* GUARD: callee resolves to a Sym but its name is not a string builtin, so
 * resolve_str_builtin_id() returns STRBI_UNKNOWN and the call is left intact. */
UT_TEST(test_const_string_calls_unknown_builtin_no_fold)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);

  static Sym callee_sym;
  IROperand callee = utb_callee_named(ir, &callee_sym, 60);
  utb_set_tok_str(60, "not_a_builtin");

  const int call_id = 1;
  utb_emit(ir, TCCIR_OP_FUNCPARAMVAL, UTB_NONE, utb_temp(0, I32),
           utb_imm((int32_t)TCCIR_ENCODE_PARAM(call_id, 0), I32));
  int i_call = utb_emit(ir, TCCIR_OP_FUNCCALLVAL, utb_temp(1, I32), callee,
                        utb_imm((int32_t)TCCIR_ENCODE_CALL(call_id, 1), I32));

  int changes = tcc_ir_opt_const_string_calls(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, i_call), TCCIR_OP_FUNCCALLVAL);

  utb_set_tok_str(60, NULL);
  utb_free(ir);
  return 0;
}

/* GUARD: src1 of the call is an immediate (not a SYMREF) so irop_get_sym_ex()
 * returns NULL; the !callee early-out leaves the call untouched. */
UT_TEST(test_const_string_calls_null_callee_no_fold)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);

  const int call_id = 1;
  utb_emit(ir, TCCIR_OP_FUNCPARAMVAL, UTB_NONE, utb_imm(0, I32),
           utb_imm((int32_t)TCCIR_ENCODE_PARAM(call_id, 0), I32));
  int i_call = utb_emit(ir, TCCIR_OP_FUNCCALLVAL, utb_temp(1, I32), utb_imm(0, I32),
                        utb_imm((int32_t)TCCIR_ENCODE_CALL(call_id, 1), I32));

  int changes = tcc_ir_opt_const_string_calls(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, i_call), TCCIR_OP_FUNCCALLVAL);

  utb_free(ir);
  return 0;
}

/* NULL-IR guard. */
UT_TEST(test_const_string_calls_null_ir)
{
  UT_ASSERT_EQ(tcc_ir_opt_const_string_calls(NULL), 0);
  return 0;
}

/* POSITIVE: memcmp(a, b, 0) folds to ASSIGN #0 regardless of the (here
 * non-constant) string args — n==0 is handled before any string evaluation,
 * so this fires in isolation without ELF section data.  Independently, two
 * memory regions compared over 0 bytes are equal, hence 0. */
UT_TEST(test_const_string_calls_memcmp_zero_len_positive)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);

  static Sym callee_sym;
  IROperand callee = utb_callee_named(ir, &callee_sym, 61);
  utb_set_tok_str(61, "memcmp");

  const int call_id = 2;
  int i_p0 = utb_emit(ir, TCCIR_OP_FUNCPARAMVAL, UTB_NONE, utb_temp(0, I32),
                      utb_imm((int32_t)TCCIR_ENCODE_PARAM(call_id, 0), I32));
  int i_p1 = utb_emit(ir, TCCIR_OP_FUNCPARAMVAL, UTB_NONE, utb_temp(1, I32),
                      utb_imm((int32_t)TCCIR_ENCODE_PARAM(call_id, 1), I32));
  int i_p2 = utb_emit(ir, TCCIR_OP_FUNCPARAMVAL, UTB_NONE, utb_imm(0, I32),
                      utb_imm((int32_t)TCCIR_ENCODE_PARAM(call_id, 2), I32));
  int i_call = utb_emit(ir, TCCIR_OP_FUNCCALLVAL, utb_temp(2, I32), callee,
                        utb_imm((int32_t)TCCIR_ENCODE_CALL(call_id, 3), I32));

  int changes = tcc_ir_opt_const_string_calls(ir);

  UT_ASSERT_EQ(changes, 1);
  UT_ASSERT_EQ(utb_op(ir, i_call), TCCIR_OP_ASSIGN);
  UT_ASSERT(irop_is_immediate(utb_src1(ir, i_call)));
  UT_ASSERT_EQ((int)irop_get_imm64_ex(ir, utb_src1(ir, i_call)), 0);
  /* params NOP'd */
  UT_ASSERT_EQ(utb_op(ir, i_p0), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, i_p1), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, i_p2), TCCIR_OP_NOP);

  utb_set_tok_str(61, NULL);
  utb_free(ir);
  return 0;
}

/* POSITIVE: strncmp(a, b, 0) folds to ASSIGN #0 (n==0 path, independent of
 * string contents). */
UT_TEST(test_const_string_calls_strncmp_zero_len_positive)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);

  static Sym callee_sym;
  IROperand callee = utb_callee_named(ir, &callee_sym, 62);
  utb_set_tok_str(62, "strncmp");

  const int call_id = 3;
  utb_emit(ir, TCCIR_OP_FUNCPARAMVAL, UTB_NONE, utb_temp(0, I32),
           utb_imm((int32_t)TCCIR_ENCODE_PARAM(call_id, 0), I32));
  utb_emit(ir, TCCIR_OP_FUNCPARAMVAL, UTB_NONE, utb_temp(1, I32),
           utb_imm((int32_t)TCCIR_ENCODE_PARAM(call_id, 1), I32));
  utb_emit(ir, TCCIR_OP_FUNCPARAMVAL, UTB_NONE, utb_imm(0, I32),
           utb_imm((int32_t)TCCIR_ENCODE_PARAM(call_id, 2), I32));
  int i_call = utb_emit(ir, TCCIR_OP_FUNCCALLVAL, utb_temp(2, I32), callee,
                        utb_imm((int32_t)TCCIR_ENCODE_CALL(call_id, 3), I32));

  int changes = tcc_ir_opt_const_string_calls(ir);

  UT_ASSERT_EQ(changes, 1);
  UT_ASSERT_EQ(utb_op(ir, i_call), TCCIR_OP_ASSIGN);
  UT_ASSERT_EQ((int)irop_get_imm64_ex(ir, utb_src1(ir, i_call)), 0);

  /* Idempotent: a second pass over the rewritten ASSIGN reports no change. */
  UT_ASSERT_EQ(tcc_ir_opt_const_string_calls(ir), 0);

  utb_set_tok_str(62, NULL);
  utb_free(ir);
  return 0;
}

/* GUARD: a FUNCCALLVOID strlen is not foldable (the strlen fold path is gated
 * on FUNCCALLVAL).  With external_global_sym stubbed to NULL, the redirect to
 * __tcc_strlen via change_callee_sym_keep_type also cannot complete, so the
 * pass reports 0 changes and leaves the call as a void call. */
UT_TEST(test_const_string_calls_strlen_void_no_fold)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);

  static Sym callee_sym;
  IROperand callee = utb_callee_named(ir, &callee_sym, 63);
  utb_set_tok_str(63, "strlen");

  const int call_id = 4;
  utb_emit(ir, TCCIR_OP_FUNCPARAMVAL, UTB_NONE, utb_temp(0, I32),
           utb_imm((int32_t)TCCIR_ENCODE_PARAM(call_id, 0), I32));
  int i_call = utb_emit(ir, TCCIR_OP_FUNCCALLVOID, UTB_NONE, callee,
                        utb_imm((int32_t)TCCIR_ENCODE_CALL(call_id, 1), I32));

  int changes = tcc_ir_opt_const_string_calls(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, i_call), TCCIR_OP_FUNCCALLVOID);

  utb_set_tok_str(63, NULL);
  utb_free(ir);
  return 0;
}

/* --------------------------------------------------- const_call_replace */

/* GUARD: empty const-result cache -> early return 0 even with a matching call. */
UT_TEST(test_const_call_replace_empty_cache_no_fold)
{
  utb_reset_ipc_caches();

  TCCIRState *ir = utb_new();
  utb_pools_init(ir);

  static Sym callee_sym;
  IROperand callee = utb_callee_named(ir, &callee_sym, 70);

  int i_call = utb_emit(ir, TCCIR_OP_FUNCCALLVAL, utb_temp(0, I32), callee,
                        utb_imm((int32_t)TCCIR_ENCODE_CALL(1, 0), I32));

  int changes = tcc_ir_opt_const_call_replace(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, i_call), TCCIR_OP_FUNCCALLVAL);

  utb_free(ir);
  return 0;
}

/* POSITIVE: a function token cached as always-returning #42 -> the call is
 * rewritten to ASSIGN #42 and its params NOP'd.  Oracle value 42 chosen by the
 * test and asserted independently. */
UT_TEST(test_const_call_replace_cached_const_positive)
{
  utb_reset_ipc_caches();

  TCCIRState *ir = utb_new();
  utb_pools_init(ir);

  const int func_tok = 71;
  tcc_ir_cache_const_result(tcc_state, func_tok, 42, VT_INT);

  static Sym callee_sym;
  IROperand callee = utb_callee_named(ir, &callee_sym, func_tok);

  const int call_id = 1;
  int i_p0 = utb_emit(ir, TCCIR_OP_FUNCPARAMVAL, UTB_NONE, utb_imm(9, I32),
                      utb_imm((int32_t)TCCIR_ENCODE_PARAM(call_id, 0), I32));
  int i_call = utb_emit(ir, TCCIR_OP_FUNCCALLVAL, utb_temp(0, I32), callee,
                        utb_imm((int32_t)TCCIR_ENCODE_CALL(call_id, 1), I32));

  int changes = tcc_ir_opt_const_call_replace(ir);

  UT_ASSERT_EQ(changes, 1);
  UT_ASSERT_EQ(utb_op(ir, i_call), TCCIR_OP_ASSIGN);
  UT_ASSERT(irop_is_immediate(utb_src1(ir, i_call)));
  UT_ASSERT_EQ((int)irop_get_imm64_ex(ir, utb_src1(ir, i_call)), 42);
  UT_ASSERT_EQ(utb_vreg(utb_dest(ir, i_call)), VR_TMP(0));
  UT_ASSERT_EQ(utb_op(ir, i_p0), TCCIR_OP_NOP);

  /* Idempotent: rewritten ASSIGN is no longer a FUNCCALLVAL. */
  UT_ASSERT_EQ(tcc_ir_opt_const_call_replace(ir), 0);

  utb_reset_ipc_caches();
  utb_free(ir);
  return 0;
}

/* POSITIVE: the result vreg is discarded (call dest has no vreg) -> the call is
 * NOP'd rather than rewritten to an ASSIGN-to-nowhere. */
UT_TEST(test_const_call_replace_discarded_result_nops)
{
  utb_reset_ipc_caches();

  TCCIRState *ir = utb_new();
  utb_pools_init(ir);

  const int func_tok = 72;
  tcc_ir_cache_const_result(tcc_state, func_tok, 7, VT_INT);

  static Sym callee_sym;
  IROperand callee = utb_callee_named(ir, &callee_sym, func_tok);

  const int call_id = 1;
  /* dest has no vreg (immediate sentinel -> irop_get_vreg < 0). */
  int i_call = utb_emit(ir, TCCIR_OP_FUNCCALLVAL, utb_imm(0, I32), callee,
                        utb_imm((int32_t)TCCIR_ENCODE_CALL(call_id, 0), I32));

  int changes = tcc_ir_opt_const_call_replace(ir);

  UT_ASSERT_EQ(changes, 1);
  UT_ASSERT_EQ(utb_op(ir, i_call), TCCIR_OP_NOP);

  utb_reset_ipc_caches();
  utb_free(ir);
  return 0;
}

/* GUARD: a cached token that does not match the callee token -> no fold. */
UT_TEST(test_const_call_replace_token_mismatch_no_fold)
{
  utb_reset_ipc_caches();

  TCCIRState *ir = utb_new();
  utb_pools_init(ir);

  tcc_ir_cache_const_result(tcc_state, 73 /* some other token */, 5, VT_INT);

  static Sym callee_sym;
  IROperand callee = utb_callee_named(ir, &callee_sym, 74 /* call this one */);

  int i_call = utb_emit(ir, TCCIR_OP_FUNCCALLVAL, utb_temp(0, I32), callee,
                        utb_imm((int32_t)TCCIR_ENCODE_CALL(1, 0), I32));

  int changes = tcc_ir_opt_const_call_replace(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, i_call), TCCIR_OP_FUNCCALLVAL);

  utb_reset_ipc_caches();
  utb_free(ir);
  return 0;
}

/* --------------------------------------------------- switch_call_replace */

/* Build a one-param "switch function" callee IR and cache its snapshot under
 * `func_tok`.  Body: `int f(int x) { return x; }` — accepted by
 * tcc_ir_detect_switch_func (param + RETURNVALUE, no stores/calls).
 * Returns 1 if detection succeeded and the snapshot was cached, else 0. */
static int utb_cache_identity_switch_func(int func_tok)
{
  TCCIRState *cir = utb_new();
  cir->parameters_count = 1;
  cir->next_parameter = 1;
  utb_alloc_param_intervals(cir, 1); /* zeroed: scalar, not addrtaken */

  utb_emit(cir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_param(0, I32), UTB_NONE);

  TCCFuncSwitchSnapshot *snap = NULL;
  int ok = tcc_ir_detect_switch_func(cir, &snap);
  if (ok)
    tcc_ir_cache_switch_func(tcc_state, func_tok, snap);

  utb_free(cir);
  return ok;
}

/* GUARD: empty switch cache -> early return 0. */
UT_TEST(test_switch_call_replace_empty_cache_no_fold)
{
  utb_reset_ipc_caches();

  TCCIRState *ir = utb_new();
  utb_pools_init(ir);

  static Sym callee_sym;
  IROperand callee = utb_callee_named(ir, &callee_sym, 80);

  const int call_id = 1;
  utb_emit(ir, TCCIR_OP_FUNCPARAMVAL, UTB_NONE, utb_imm(7, I32),
           utb_imm((int32_t)TCCIR_ENCODE_PARAM(call_id, 0), I32));
  int i_call = utb_emit(ir, TCCIR_OP_FUNCCALLVAL, utb_temp(0, I32), callee,
                        utb_imm((int32_t)TCCIR_ENCODE_CALL(call_id, 1), I32));

  int changes = tcc_ir_opt_switch_call_replace(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, i_call), TCCIR_OP_FUNCCALLVAL);

  utb_free(ir);
  return 0;
}

/* POSITIVE: identity switch func cached, called with constant 7 -> the call is
 * simulated to #7, rewritten to ASSIGN #7, param NOP'd.  Oracle: f(x)=x so
 * f(7)==7. */
UT_TEST(test_switch_call_replace_identity_positive)
{
  utb_reset_ipc_caches();

  const int func_tok = 81;
  UT_ASSERT_EQ(utb_cache_identity_switch_func(func_tok), 1);

  TCCIRState *ir = utb_new();
  utb_pools_init(ir);

  static Sym callee_sym;
  IROperand callee = utb_callee_named(ir, &callee_sym, func_tok);

  const int call_id = 1;
  int i_p0 = utb_emit(ir, TCCIR_OP_FUNCPARAMVAL, UTB_NONE, utb_imm(7, I32),
                      utb_imm((int32_t)TCCIR_ENCODE_PARAM(call_id, 0), I32));
  int i_call = utb_emit(ir, TCCIR_OP_FUNCCALLVAL, utb_temp(0, I32), callee,
                        utb_imm((int32_t)TCCIR_ENCODE_CALL(call_id, 1), I32));

  int changes = tcc_ir_opt_switch_call_replace(ir);

  UT_ASSERT_EQ(changes, 1);
  UT_ASSERT_EQ(utb_op(ir, i_call), TCCIR_OP_ASSIGN);
  UT_ASSERT(irop_is_immediate(utb_src1(ir, i_call)));
  UT_ASSERT_EQ((int)irop_get_imm64_ex(ir, utb_src1(ir, i_call)), 7);
  UT_ASSERT_EQ(utb_vreg(utb_dest(ir, i_call)), VR_TMP(0));
  UT_ASSERT_EQ(utb_op(ir, i_p0), TCCIR_OP_NOP);

  /* Idempotent: no FUNCCALLVAL remains to fold. */
  UT_ASSERT_EQ(tcc_ir_opt_switch_call_replace(ir), 0);

  utb_reset_ipc_caches();
  utb_free(ir);
  return 0;
}

/* GUARD: switch func cached but the call's single argument is NOT a constant
 * (it's a temp), so simulation cannot proceed and the call is left intact. */
UT_TEST(test_switch_call_replace_nonconst_arg_no_fold)
{
  utb_reset_ipc_caches();

  const int func_tok = 82;
  UT_ASSERT_EQ(utb_cache_identity_switch_func(func_tok), 1);

  TCCIRState *ir = utb_new();
  utb_pools_init(ir);

  static Sym callee_sym;
  IROperand callee = utb_callee_named(ir, &callee_sym, func_tok);

  const int call_id = 1;
  /* param value is a temp, not an immediate */
  utb_emit(ir, TCCIR_OP_FUNCPARAMVAL, UTB_NONE, utb_temp(5, I32),
           utb_imm((int32_t)TCCIR_ENCODE_PARAM(call_id, 0), I32));
  int i_call = utb_emit(ir, TCCIR_OP_FUNCCALLVAL, utb_temp(0, I32), callee,
                        utb_imm((int32_t)TCCIR_ENCODE_CALL(call_id, 1), I32));

  int changes = tcc_ir_opt_switch_call_replace(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, i_call), TCCIR_OP_FUNCCALLVAL);

  utb_reset_ipc_caches();
  utb_free(ir);
  return 0;
}

/* GUARD: switch func cached but the call has argc != 1 -> the pass skips it.
 * (The detector only models single-param functions.) */
UT_TEST(test_switch_call_replace_wrong_argc_no_fold)
{
  utb_reset_ipc_caches();

  const int func_tok = 83;
  UT_ASSERT_EQ(utb_cache_identity_switch_func(func_tok), 1);

  TCCIRState *ir = utb_new();
  utb_pools_init(ir);

  static Sym callee_sym;
  IROperand callee = utb_callee_named(ir, &callee_sym, func_tok);

  const int call_id = 1;
  utb_emit(ir, TCCIR_OP_FUNCPARAMVAL, UTB_NONE, utb_imm(7, I32),
           utb_imm((int32_t)TCCIR_ENCODE_PARAM(call_id, 0), I32));
  utb_emit(ir, TCCIR_OP_FUNCPARAMVAL, UTB_NONE, utb_imm(8, I32),
           utb_imm((int32_t)TCCIR_ENCODE_PARAM(call_id, 1), I32));
  int i_call = utb_emit(ir, TCCIR_OP_FUNCCALLVAL, utb_temp(0, I32), callee,
                        utb_imm((int32_t)TCCIR_ENCODE_CALL(call_id, 2), I32));

  int changes = tcc_ir_opt_switch_call_replace(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, i_call), TCCIR_OP_FUNCCALLVAL);

  utb_reset_ipc_caches();
  utb_free(ir);
  return 0;
}

/* --------------------------------------------- param_addrof_const_fold */

/* NULL-pattern guard: no params (max_par <= 0) -> early return 0. */
UT_TEST(test_param_addrof_no_params_no_fold)
{
  TCCIRState *ir = utb_new();
  ir->next_parameter = 0;
  int i_assign = utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(1, I32), UTB_NONE);

  int changes = tcc_ir_opt_param_addrof_const_fold(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, i_assign), TCCIR_OP_ASSIGN);

  utb_free(ir);
  return 0;
}

/* GUARD: the single-BB restriction.  A JUMP anywhere in the function makes the
 * pass bail before doing any analysis, even on an otherwise-foldable pattern. */
UT_TEST(test_param_addrof_multi_bb_no_fold)
{
  TCCIRState *ir = utb_new();
  ir->next_parameter = 1;
  ir->next_temporary_variable = 1;
  ir->next_local_variable = 0;
  utb_alloc_param_intervals(ir, 1);

  /* T0 = &P0 ; STORE *T0 <- #5 ; JUMP -> end */
  utb_emit(ir, TCCIR_OP_LEA, utb_temp(0, I32), utb_local_vreg(VR_PARAM(0), I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_STORE, utb_lval(utb_temp(0, I32)), utb_imm(5, I32), UTB_NONE);
  int i_jump = utb_emit(ir, TCCIR_OP_JUMP, utb_imm(3, I32), UTB_NONE, UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);

  int changes = tcc_ir_opt_param_addrof_const_fold(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, i_jump), TCCIR_OP_JUMP);

  utb_free(ir);
  return 0;
}

/* POSITIVE: `T0 = &P0; *T0 = #5; ... read P0` -> reads of P0 past the store are
 * rewritten to #5; the LEA and STORE become NOP.  Oracle: after `*(&p)=5`, p==5.
 *
 *   LEA   T0  = &P0
 *   STORE *T0 = #5
 *   ADD   T1 = P0 + #3        (read of P0)   -> T1 = 5 + 3
 *   RETURNVALUE T1 */
UT_TEST(test_param_addrof_const_fold_positive)
{
  TCCIRState *ir = utb_new();
  ir->next_parameter = 1;
  ir->next_temporary_variable = 2;
  ir->next_local_variable = 0;
  utb_alloc_param_intervals(ir, 1);

  int i_lea = utb_emit(ir, TCCIR_OP_LEA, utb_temp(0, I32), utb_local_vreg(VR_PARAM(0), I32), UTB_NONE);
  int i_store = utb_emit(ir, TCCIR_OP_STORE, utb_lval(utb_temp(0, I32)), utb_imm(5, I32), UTB_NONE);
  int i_add = utb_emit(ir, TCCIR_OP_ADD, utb_temp(1, I32), irop_make_vreg(VR_PARAM(0), I32), utb_imm(3, I32));
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_temp(1, I32), UTB_NONE);

  int changes = tcc_ir_opt_param_addrof_const_fold(ir);

  /* rewrote (1 read) + LEA NOP + STORE NOP = 3 changes */
  UT_ASSERT_EQ(changes, 3);
  UT_ASSERT_EQ(utb_op(ir, i_lea), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, i_store), TCCIR_OP_NOP);
  /* The ADD's src1 (was P0) is now the stored constant #5. */
  UT_ASSERT_EQ(utb_op(ir, i_add), TCCIR_OP_ADD);
  UT_ASSERT(irop_is_immediate(utb_src1(ir, i_add)));
  UT_ASSERT_EQ((int)irop_get_imm64_ex(ir, utb_src1(ir, i_add)), 5);
  /* addrtaken flag cleared on the param interval. */
  UT_ASSERT_EQ((int)ir->parameters_live_intervals[0].addrtaken, 0);

  /* Idempotent: nothing left to rewrite. */
  UT_ASSERT_EQ(tcc_ir_opt_param_addrof_const_fold(ir), 0);

  utb_free(ir);
  return 0;
}

/* POSITIVE (LOAD form): a read of P0 via its spill slot `LOAD T2 <- *(slot P0)`
 * is rewritten to `ASSIGN T2 <- #5` (LOAD becomes ASSIGN). */
UT_TEST(test_param_addrof_const_fold_load_read_positive)
{
  TCCIRState *ir = utb_new();
  ir->next_parameter = 1;
  ir->next_temporary_variable = 3;
  ir->next_local_variable = 0;
  utb_alloc_param_intervals(ir, 1);

  int i_lea = utb_emit(ir, TCCIR_OP_LEA, utb_temp(0, I32), utb_local_vreg(VR_PARAM(0), I32), UTB_NONE);
  int i_store = utb_emit(ir, TCCIR_OP_STORE, utb_lval(utb_temp(0, I32)), utb_imm(9, I32), UTB_NONE);
  int i_load = utb_emit(ir, TCCIR_OP_LOAD, utb_temp(2, I32), utb_slot_lval(VR_PARAM(0), 0, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_temp(2, I32), UTB_NONE);

  int changes = tcc_ir_opt_param_addrof_const_fold(ir);

  UT_ASSERT_EQ(changes, 3);
  UT_ASSERT_EQ(utb_op(ir, i_lea), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, i_store), TCCIR_OP_NOP);
  /* LOAD of the spill slot is now an ASSIGN of the constant. */
  UT_ASSERT_EQ(utb_op(ir, i_load), TCCIR_OP_ASSIGN);
  UT_ASSERT(irop_is_immediate(utb_src1(ir, i_load)));
  UT_ASSERT_EQ((int)irop_get_imm64_ex(ir, utb_src1(ir, i_load)), 9);

  utb_free(ir);
  return 0;
}

/* GUARD: P0 is read BEFORE the modify STORE, which must disqualify the fold
 * (the pre-store value of P0 differs from the stored constant).  The read of P0
 * before the store is left intact and the LEA/STORE are not NOP'd. */
UT_TEST(test_param_addrof_pre_store_read_no_fold)
{
  TCCIRState *ir = utb_new();
  ir->next_parameter = 1;
  ir->next_temporary_variable = 3;
  ir->next_local_variable = 0;
  utb_alloc_param_intervals(ir, 1);

  int i_lea = utb_emit(ir, TCCIR_OP_LEA, utb_temp(0, I32), utb_local_vreg(VR_PARAM(0), I32), UTB_NONE);
  /* Read P0 before the modify store. */
  int i_pre = utb_emit(ir, TCCIR_OP_ADD, utb_temp(1, I32), irop_make_vreg(VR_PARAM(0), I32), utb_imm(1, I32));
  int i_store = utb_emit(ir, TCCIR_OP_STORE, utb_lval(utb_temp(0, I32)), utb_imm(5, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_ADD, utb_temp(2, I32), irop_make_vreg(VR_PARAM(0), I32), utb_imm(3, I32));

  int changes = tcc_ir_opt_param_addrof_const_fold(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, i_lea), TCCIR_OP_LEA);
  UT_ASSERT_EQ(utb_op(ir, i_store), TCCIR_OP_STORE);
  /* The pre-store read still references P0 (not folded). */
  UT_ASSERT_EQ(utb_vreg(utb_src1(ir, i_pre)), VR_PARAM(0));

  utb_free(ir);
  return 0;
}

/* GUARD: the address of P0 escapes (T0 is used as a value, not just as a
 * STORE-through dest), so the pass must NOT fold P0 to a constant. */
UT_TEST(test_param_addrof_escaped_pointer_no_fold)
{
  TCCIRState *ir = utb_new();
  ir->next_parameter = 1;
  ir->next_temporary_variable = 3;
  ir->next_local_variable = 0;
  utb_alloc_param_intervals(ir, 1);

  int i_lea = utb_emit(ir, TCCIR_OP_LEA, utb_temp(0, I32), utb_local_vreg(VR_PARAM(0), I32), UTB_NONE);
  int i_store = utb_emit(ir, TCCIR_OP_STORE, utb_lval(utb_temp(0, I32)), utb_imm(5, I32), UTB_NONE);
  /* T0 used as a plain value (the pointer escapes) -> disqualify. */
  utb_emit(ir, TCCIR_OP_ADD, utb_temp(1, I32), utb_temp(0, I32), utb_imm(0, I32));
  utb_emit(ir, TCCIR_OP_ADD, utb_temp(2, I32), irop_make_vreg(VR_PARAM(0), I32), utb_imm(3, I32));

  int changes = tcc_ir_opt_param_addrof_const_fold(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, i_lea), TCCIR_OP_LEA);
  UT_ASSERT_EQ(utb_op(ir, i_store), TCCIR_OP_STORE);

  utb_free(ir);
  return 0;
}

/* --------------------------------------------- local_addrof_const_fold */

/* NULL-pattern guard: no locals (max_var <= 0) -> early return 0. */
UT_TEST(test_local_addrof_no_vars_no_fold)
{
  TCCIRState *ir = utb_new();
  ir->next_local_variable = 0;
  int i_assign = utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(1, I32), UTB_NONE);

  int changes = tcc_ir_opt_local_addrof_const_fold(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, i_assign), TCCIR_OP_ASSIGN);

  utb_free(ir);
  return 0;
}

/* POSITIVE: the full local pattern.
 *
 *   STORE V0 <- #1          ; init
 *   LEA   T0  = &V0
 *   STORE *T0 = #5          ; modify through pointer
 *   ADD   T1 = V0 + #3      ; read of V0 past the modify  -> 5 + 3
 *   RETURNVALUE T1
 *
 * Reads of V0 past the modify STORE are rewritten to #5; init STORE, LEA and
 * modify STORE become NOP.  Oracle: after `*(&v)=5`, v==5. */
UT_TEST(test_local_addrof_const_fold_positive)
{
  TCCIRState *ir = utb_new();
  ir->next_parameter = 0;
  ir->next_temporary_variable = 2;
  ir->next_local_variable = 1;
  utb_alloc_var_intervals(ir, 1);

  int i_init = utb_emit(ir, TCCIR_OP_STORE, utb_var(0, I32), utb_imm(1, I32), UTB_NONE);
  int i_lea = utb_emit(ir, TCCIR_OP_LEA, utb_temp(0, I32), utb_local_vreg(VR_VAR(0), I32), UTB_NONE);
  int i_store = utb_emit(ir, TCCIR_OP_STORE, utb_lval(utb_temp(0, I32)), utb_imm(5, I32), UTB_NONE);
  int i_add = utb_emit(ir, TCCIR_OP_ADD, utb_temp(1, I32), irop_make_vreg(VR_VAR(0), I32), utb_imm(3, I32));
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_temp(1, I32), UTB_NONE);

  int changes = tcc_ir_opt_local_addrof_const_fold(ir);

  /* rewrote (1) + init NOP + LEA NOP + STORE NOP = 4 changes */
  UT_ASSERT_EQ(changes, 4);
  UT_ASSERT_EQ(utb_op(ir, i_init), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, i_lea), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, i_store), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, i_add), TCCIR_OP_ADD);
  UT_ASSERT(irop_is_immediate(utb_src1(ir, i_add)));
  UT_ASSERT_EQ((int)irop_get_imm64_ex(ir, utb_src1(ir, i_add)), 5);
  UT_ASSERT_EQ((int)ir->variables_live_intervals[0].addrtaken, 0);

  /* Idempotent. */
  UT_ASSERT_EQ(tcc_ir_opt_local_addrof_const_fold(ir), 0);

  utb_free(ir);
  return 0;
}

/* GUARD: no init STORE for V0 (the pattern requires a pre-LEA constant init).
 * Without it, init_idx stays -1 and Phase 3 skips the fold. */
UT_TEST(test_local_addrof_missing_init_no_fold)
{
  TCCIRState *ir = utb_new();
  ir->next_parameter = 0;
  ir->next_temporary_variable = 2;
  ir->next_local_variable = 1;
  utb_alloc_var_intervals(ir, 1);

  int i_lea = utb_emit(ir, TCCIR_OP_LEA, utb_temp(0, I32), utb_local_vreg(VR_VAR(0), I32), UTB_NONE);
  int i_store = utb_emit(ir, TCCIR_OP_STORE, utb_lval(utb_temp(0, I32)), utb_imm(5, I32), UTB_NONE);
  int i_add = utb_emit(ir, TCCIR_OP_ADD, utb_temp(1, I32), irop_make_vreg(VR_VAR(0), I32), utb_imm(3, I32));
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_temp(1, I32), UTB_NONE);

  int changes = tcc_ir_opt_local_addrof_const_fold(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, i_lea), TCCIR_OP_LEA);
  UT_ASSERT_EQ(utb_op(ir, i_store), TCCIR_OP_STORE);
  UT_ASSERT_EQ(utb_vreg(utb_src1(ir, i_add)), VR_VAR(0));

  utb_free(ir);
  return 0;
}

/* GUARD: V0 is read before the modify STORE (between init and modify), which
 * must disqualify the fold — the pre-modify value (#1) is observable. */
UT_TEST(test_local_addrof_pre_modify_read_no_fold)
{
  TCCIRState *ir = utb_new();
  ir->next_parameter = 0;
  ir->next_temporary_variable = 3;
  ir->next_local_variable = 1;
  utb_alloc_var_intervals(ir, 1);

  utb_emit(ir, TCCIR_OP_STORE, utb_var(0, I32), utb_imm(1, I32), UTB_NONE);
  int i_lea = utb_emit(ir, TCCIR_OP_LEA, utb_temp(0, I32), utb_local_vreg(VR_VAR(0), I32), UTB_NONE);
  /* Read V0 before the modify store. */
  utb_emit(ir, TCCIR_OP_ADD, utb_temp(1, I32), irop_make_vreg(VR_VAR(0), I32), utb_imm(7, I32));
  int i_store = utb_emit(ir, TCCIR_OP_STORE, utb_lval(utb_temp(0, I32)), utb_imm(5, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_ADD, utb_temp(2, I32), irop_make_vreg(VR_VAR(0), I32), utb_imm(3, I32));

  int changes = tcc_ir_opt_local_addrof_const_fold(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, i_lea), TCCIR_OP_LEA);
  UT_ASSERT_EQ(utb_op(ir, i_store), TCCIR_OP_STORE);

  utb_free(ir);
  return 0;
}

/* ------------------------------------------------------------------ suite */

UT_SUITE(opt_constfold)
{
  UT_COVERS("self_copy_elim");
  UT_COVERS("float_narrowing");
  UT_COVERS("const_string_calls");
  UT_COVERS("const_call_replace");
  UT_COVERS("switch_call_replace");
  UT_COVERS("param_addrof_const_fold");
  UT_COVERS("local_addrof_const_fold");
  UT_RUN(test_self_copy_elim_non_memcpy_name_no_fold);
  UT_RUN(test_self_copy_elim_no_calls_no_fold);
  UT_RUN(test_self_copy_elim_null_callee_no_fold);
  UT_RUN(test_self_copy_elim_null_ir);
  UT_RUN(test_self_copy_elim_memcpy_positive);
  UT_RUN(test_self_copy_elim_memmove_positive);
  UT_RUN(test_self_copy_elim_aeabi_memcpy8_positive);
  UT_RUN(test_self_copy_elim_void_call_positive);
  UT_RUN(test_self_copy_elim_dst_src_differ_no_fold);
  UT_RUN(test_self_copy_elim_redefined_temp_suspected_bug);
  UT_RUN(test_self_copy_elim_lval_mismatch_no_fold);
  UT_RUN(test_self_copy_elim_idempotent);
  UT_RUN(test_float_narrowing_unmatched_names_no_fold);
  UT_RUN(test_float_narrowing_too_few_instructions_no_fold);
  UT_RUN(test_float_narrowing_non_narrowable_middle_no_fold);
  UT_RUN(test_float_narrowing_missing_d2f_no_fold);
  UT_RUN(test_float_narrowing_f2d_not_consumed_no_fold);
  UT_RUN(test_float_narrowing_no_f2d_no_fold);
  UT_RUN(test_float_narrowing_idempotent);

  UT_RUN(test_const_string_calls_unknown_builtin_no_fold);
  UT_RUN(test_const_string_calls_null_callee_no_fold);
  UT_RUN(test_const_string_calls_null_ir);
  UT_RUN(test_const_string_calls_memcmp_zero_len_positive);
  UT_RUN(test_const_string_calls_strncmp_zero_len_positive);
  UT_RUN(test_const_string_calls_strlen_void_no_fold);

  UT_RUN(test_const_call_replace_empty_cache_no_fold);
  UT_RUN(test_const_call_replace_cached_const_positive);
  UT_RUN(test_const_call_replace_discarded_result_nops);
  UT_RUN(test_const_call_replace_token_mismatch_no_fold);

  UT_RUN(test_switch_call_replace_empty_cache_no_fold);
  UT_RUN(test_switch_call_replace_identity_positive);
  UT_RUN(test_switch_call_replace_nonconst_arg_no_fold);
  UT_RUN(test_switch_call_replace_wrong_argc_no_fold);

  UT_RUN(test_param_addrof_no_params_no_fold);
  UT_RUN(test_param_addrof_multi_bb_no_fold);
  UT_RUN(test_param_addrof_const_fold_positive);
  UT_RUN(test_param_addrof_const_fold_load_read_positive);
  UT_RUN(test_param_addrof_pre_store_read_no_fold);
  UT_RUN(test_param_addrof_escaped_pointer_no_fold);

  UT_RUN(test_local_addrof_no_vars_no_fold);
  UT_RUN(test_local_addrof_const_fold_positive);
  UT_RUN(test_local_addrof_missing_init_no_fold);
  UT_RUN(test_local_addrof_pre_modify_read_no_fold);
}
