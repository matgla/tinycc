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
 *  HARNESS LIMITATION (important — see FINDINGS in the agent report):
 *  Both passes decide whether to fire SOLELY from the callee name returned by
 *  get_tok_str(callee->v).  In this isolated host harness get_tok_str() is the
 *  shared base stub (tests/unit/arm/armv8m/stubs.c -> built as extra_stubs.o)
 *  which returns the constant "?" for EVERY token and is a strong, unoverridable
 *  global.  Consequently neither pass can ever match a real helper name here, so
 *  a "true positive" (the pass actually folding) is structurally unreachable in
 *  this harness.  These two passes are therefore exercised as GUARD passes: a
 *  fully-formed self-copy / f2d->func->d2f sequence is built (so the entire
 *  collection + param-equality + call-id machinery runs), and we assert the pass
 *  correctly declines to fire because the name does not match.  Each test would
 *  FAIL (changed != 0, op rewritten) if the name gate were ever dropped or the
 *  pass over-fired, so the assertions are non-vacuous.
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

/* Frontend link stubs (sym_push2 / external_global_sym / tok_alloc_const /
 * global_stack / elfsym) now live in stubs.c so the combined unit-test link
 * has a single definition.  They are unreachable at runtime here because the
 * name gate in float_narrowing never matches under the "?" get_tok_str stub. */

#define I32 IROP_BTYPE_INT32
#define F32 IROP_BTYPE_FLOAT32
#define F64 IROP_BTYPE_FLOAT64

/* ----------------------------------------------------------- helpers */

/* Build a SYMREF operand carrying a freshly-pooled callee Sym.  The Sym's `v`
 * token is irrelevant in this harness (get_tok_str ignores it and returns "?"),
 * but irop_get_sym_ex() must resolve to a non-NULL Sym for the pass to even
 * reach the name check, so a real pool entry is required. */
static IROperand utb_callee(TCCIRState *ir, Sym *sym)
{
  sym->v = 0;
  uint32_t sidx = tcc_ir_pool_add_symref(ir, sym, 0, 0);
  return irop_make_symref(0, sidx, 0, 0, 0, I32);
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
  tcc_ir_pools_init(ir); /* needs the symref pool for the callee operand */

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
  tcc_ir_pools_init(ir);

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
  tcc_ir_pools_init(ir);

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

/* ------------------------------------------------------------------ suite */

UT_SUITE(opt_constfold)
{
  UT_COVERS("self_copy_elim");
  UT_COVERS("float_narrowing");
  UT_RUN(test_self_copy_elim_non_memcpy_name_no_fold);
  UT_RUN(test_self_copy_elim_no_calls_no_fold);
  UT_RUN(test_self_copy_elim_null_callee_no_fold);
  UT_RUN(test_self_copy_elim_null_ir);
  UT_RUN(test_float_narrowing_unmatched_names_no_fold);
  UT_RUN(test_float_narrowing_too_few_instructions_no_fold);
}
