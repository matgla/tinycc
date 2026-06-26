/*
 *  test_opt_const_aggregate.c - suite for ir/opt_const_aggregate.c
 *
 *  tcc_ir_opt_const_aggregate_fold folds the deterministic read-modify-write
 *  chain on a NON-ESCAPING local 8-byte (double) aggregate slot (the unrolled
 *  `u.e.a++` sequence from gcc.c-torture pr92904).  In one forward walk it:
 *    - tracks the constant value held in each non-escaped 8-byte stack slot,
 *      across intervening calls (because the slot's address never escapes);
 *    - rewrites `T = __aeabi_dadd(known_const, imm)` (and dsub) to `T = #const`
 *      (an ASSIGN of an F64 immediate), NOP-ing the call's PARAMs;
 *    - propagates the folded constant back into the slot via the following
 *      STORE, so a whole depth-N RMW chain converges in a single pass.
 *
 *  Object identity is rooted at the LEA chain `Addr[StackLoc[base]]`: a slot is
 *  only trackable if at least one access resolves a real (non-NONE) root base
 *  and that root NEVER escapes.  Address-as-value uses (other than a memmove/
 *  memcpy read-only source) taint the root and block the fold.
 *
 *  These are isolated tests: a hand-built IR sequence is run through the bare
 *  pass entry point and the resulting instructions are inspected directly.  The
 *  expected folded constant is computed independently (an oracle) so a real
 *  miscompile in the fold arithmetic is caught.
 *
 *  HARNESS NOTES:
 *  The pass is name-gated via get_tok_str(callee->v) (for __aeabi_dadd/dsub and
 *  for memcpy/memmove).  The unit-test harness provides a settable token->name
 *  table (utb_set_tok_str), so the positive folds are reachable in isolation.
 */

#include "ir_build.h"

#include "ut.h"

/* Pass entry point (declared in ir/opt.h; forward-declared to avoid pulling in
 * the optimizer engine headers). */
int tcc_ir_opt_const_aggregate_fold(TCCIRState *ir);

#define I32 IROP_BTYPE_INT32
#define F64 IROP_BTYPE_FLOAT64

/* Distinct token ids mapped to callee names per test via utb_set_tok_str. */
#define TOK_DADD 11
#define TOK_DSUB 12
#define TOK_MEMCPY 13
#define TOK_FOO 14

/* ----------------------------------------------------------------- helpers */

/* Build a SYMREF callee operand whose token is `tok`. */
static IROperand utb_callee_named(TCCIRState *ir, Sym *sym, int tok)
{
  sym->v = tok;
  uint32_t sidx = tcc_ir_pool_add_symref(ir, sym, 0, 0);
  return irop_make_symref(0, sidx, 0, 0, 0, I32);
}

/* Build an F64 immediate operand carrying the bit pattern of `d`. */
static IROperand utb_f64imm(TCCIRState *ir, double d)
{
  union { double d; uint64_t u; } c;
  c.d = d;
  return irop_make_f64(0, tcc_ir_pool_add_f64(ir, c.u));
}

/* Reinterpret a raw 64-bit pattern as a double (for oracle comparisons). */
static double utb_bits_to_double(int64_t bits)
{
  union { double d; uint64_t u; } c;
  c.u = (uint64_t)bits;
  return c.d;
}

/* Read the double constant an ASSIGN-folded call now produces (src1 must be an
 * F64 immediate). */
static double utb_folded_double(TCCIRState *ir, int i)
{
  return utb_bits_to_double(irop_get_imm64_ex(ir, utb_src1(ir, i)));
}

/* Emit  Tdst = LEA StackLoc[off]   (roots the object at base `off`). */
static int utb_emit_lea_slot(TCCIRState *ir, int dst_tmp, int32_t off)
{
  return utb_emit(ir, TCCIR_OP_LEA, utb_temp(dst_tmp, I32),
                  utb_stackoff(off, 0, 0, 0, I32), UTB_NONE);
}

/* Emit one RMW step:
 *     Tld   = LOAD [Taddr]                (double)
 *     PARAM Tld,  (call_id,0)
 *     PARAM #k,   (call_id,1)             (k as an f64 immediate)
 *     Tres  = FUNCCALLVAL <callee>, (call_id, argc=2)
 *     STORE [Taddr] = Tres
 * Returns the call instruction index. */
static int utb_emit_dadd_rmw(TCCIRState *ir, IROperand callee, int call_id, int addr_tmp,
                             int ld_tmp, int res_tmp, double k)
{
  utb_emit(ir, TCCIR_OP_LOAD, utb_temp(ld_tmp, F64), utb_lval(utb_temp(addr_tmp, F64)), UTB_NONE);
  utb_emit(ir, TCCIR_OP_FUNCPARAMVAL, UTB_NONE, utb_temp(ld_tmp, F64),
           utb_imm((int32_t)TCCIR_ENCODE_PARAM(call_id, 0), I32));
  utb_emit(ir, TCCIR_OP_FUNCPARAMVAL, UTB_NONE, utb_f64imm(ir, k),
           utb_imm((int32_t)TCCIR_ENCODE_PARAM(call_id, 1), I32));
  int icall = utb_emit(ir, TCCIR_OP_FUNCCALLVAL, utb_temp(res_tmp, F64), callee,
                       utb_imm((int32_t)TCCIR_ENCODE_CALL(call_id, 2), I32));
  utb_emit(ir, TCCIR_OP_STORE, utb_lval(utb_temp(addr_tmp, F64)), utb_temp(res_tmp, F64), UTB_NONE);
  return icall;
}

/* ------------------------------------------------------------------ tests */

/* Positive: a single u.a = 1.25; u.a += 1.0 folds the __aeabi_dadd to a #2.25
 * ASSIGN.  The slot is rooted via a LEA and never escapes. */
UT_TEST(test_const_agg_single_dadd_folds)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);

  static Sym dadd;
  IROperand callee = utb_callee_named(ir, &dadd, TOK_DADD);
  utb_set_tok_str(TOK_DADD, "__aeabi_dadd");

  utb_emit_lea_slot(ir, 0, 64);
  utb_emit(ir, TCCIR_OP_STORE, utb_lval(utb_temp(0, F64)), utb_f64imm(ir, 1.25), UTB_NONE);
  int icall = utb_emit_dadd_rmw(ir, callee, 1, /*addr*/ 0, /*ld*/ 1, /*res*/ 2, 1.0);

  int changes = tcc_ir_opt_const_aggregate_fold(ir);

  UT_ASSERT_EQ(changes, 1);
  UT_ASSERT_EQ(utb_op(ir, icall), TCCIR_OP_ASSIGN);
  /* Oracle: 1.25 + 1.0 == 2.25. */
  UT_ASSERT(utb_folded_double(ir, icall) == 2.25);

  utb_free(ir);
  return 0;
}

/* Positive: __aeabi_dsub folds with the correct (minuend - subtrahend) sign. */
UT_TEST(test_const_agg_single_dsub_folds)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);

  static Sym dsub;
  IROperand callee = utb_callee_named(ir, &dsub, TOK_DSUB);
  utb_set_tok_str(TOK_DSUB, "__aeabi_dsub");

  utb_emit_lea_slot(ir, 0, 32);
  utb_emit(ir, TCCIR_OP_STORE, utb_lval(utb_temp(0, F64)), utb_f64imm(ir, 5.0), UTB_NONE);
  int icall = utb_emit_dadd_rmw(ir, callee, 1, 0, 1, 2, 1.5);

  int changes = tcc_ir_opt_const_aggregate_fold(ir);

  UT_ASSERT_EQ(changes, 1);
  UT_ASSERT_EQ(utb_op(ir, icall), TCCIR_OP_ASSIGN);
  /* Oracle: dsub is a - b == 5.0 - 1.5 == 3.5 (NOT 1.5 - 5.0). */
  UT_ASSERT(utb_folded_double(ir, icall) == 3.5);

  utb_free(ir);
  return 0;
}

/* Positive chain (depth 2): u.a = 1.25; u.a++; u.a++ converges to 2.25 then
 * 3.25 in a SINGLE pass — the first fold updates the slot lattice so the second
 * RMW sees the new constant. */
UT_TEST(test_const_agg_chain_depth2_one_pass)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);

  static Sym dadd;
  IROperand callee = utb_callee_named(ir, &dadd, TOK_DADD);
  utb_set_tok_str(TOK_DADD, "__aeabi_dadd");

  utb_emit_lea_slot(ir, 0, 64);
  utb_emit(ir, TCCIR_OP_STORE, utb_lval(utb_temp(0, F64)), utb_f64imm(ir, 1.25), UTB_NONE);
  int c1 = utb_emit_dadd_rmw(ir, callee, 1, 0, 1, 2, 1.0);
  int c2 = utb_emit_dadd_rmw(ir, callee, 2, 0, 3, 4, 1.0);

  int changes = tcc_ir_opt_const_aggregate_fold(ir);

  UT_ASSERT_EQ(changes, 2);
  UT_ASSERT_EQ(utb_op(ir, c1), TCCIR_OP_ASSIGN);
  UT_ASSERT_EQ(utb_op(ir, c2), TCCIR_OP_ASSIGN);
  UT_ASSERT(utb_folded_double(ir, c1) == 2.25);
  UT_ASSERT(utb_folded_double(ir, c2) == 3.25);

  utb_free(ir);
  return 0;
}

/* Idempotence: after the depth-2 chain folds, a second application reports no
 * further changes. */
UT_TEST(test_const_agg_idempotent)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);

  static Sym dadd;
  IROperand callee = utb_callee_named(ir, &dadd, TOK_DADD);
  utb_set_tok_str(TOK_DADD, "__aeabi_dadd");

  utb_emit_lea_slot(ir, 0, 64);
  utb_emit(ir, TCCIR_OP_STORE, utb_lval(utb_temp(0, F64)), utb_f64imm(ir, 1.25), UTB_NONE);
  utb_emit_dadd_rmw(ir, callee, 1, 0, 1, 2, 1.0);
  utb_emit_dadd_rmw(ir, callee, 2, 0, 3, 4, 1.0);

  int total = utb_run_to_fixpoint(ir, tcc_ir_opt_const_aggregate_fold, 10);
  UT_ASSERT_EQ(total, 2);
  UT_ASSERT_EQ(tcc_ir_opt_const_aggregate_fold(ir), 0);

  utb_free(ir);
  return 0;
}

/* Negative (escape): the slot's address is passed by value as an ordinary call
 * argument (not a memmove source).  The root base escapes, so the later dadd of
 * the loaded value must NOT fold. */
UT_TEST(test_const_agg_address_escape_no_fold)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);

  static Sym dadd, foo;
  IROperand callee = utb_callee_named(ir, &dadd, TOK_DADD);
  IROperand foofn = utb_callee_named(ir, &foo, TOK_FOO);
  utb_set_tok_str(TOK_DADD, "__aeabi_dadd");
  utb_set_tok_str(TOK_FOO, "foo");

  utb_emit_lea_slot(ir, 0, 64);
  utb_emit(ir, TCCIR_OP_STORE, utb_lval(utb_temp(0, F64)), utb_f64imm(ir, 1.25), UTB_NONE);
  /* foo(&u): the address value T0 is param 0 of an ordinary call -> escapes. */
  utb_emit(ir, TCCIR_OP_FUNCPARAMVAL, UTB_NONE, utb_temp(0, I32),
           utb_imm((int32_t)TCCIR_ENCODE_PARAM(3, 0), I32));
  utb_emit(ir, TCCIR_OP_FUNCCALLVOID, UTB_NONE, foofn,
           utb_imm((int32_t)TCCIR_ENCODE_CALL(3, 1), I32));
  int icall = utb_emit_dadd_rmw(ir, callee, 1, 0, 1, 2, 1.0);

  int changes = tcc_ir_opt_const_aggregate_fold(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, icall), TCCIR_OP_FUNCCALLVAL);

  utb_free(ir);
  return 0;
}

/* Negative-of-the-negative (memmove source does NOT escape): the slot's address
 * is passed as the read-only SOURCE (param idx 1) of a memcpy.  That use is
 * explicitly exempt from escape, so the subsequent dadd still folds. */
UT_TEST(test_const_agg_memcpy_source_still_folds)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);

  static Sym dadd, mc;
  IROperand callee = utb_callee_named(ir, &dadd, TOK_DADD);
  IROperand memcpy_fn = utb_callee_named(ir, &mc, TOK_MEMCPY);
  utb_set_tok_str(TOK_DADD, "__aeabi_dadd");
  utb_set_tok_str(TOK_MEMCPY, "memcpy");

  utb_emit_lea_slot(ir, 0, 64);
  utb_emit(ir, TCCIR_OP_STORE, utb_lval(utb_temp(0, F64)), utb_f64imm(ir, 1.25), UTB_NONE);
  /* memcpy(dst=#100, src=&u (T0, param idx 1), n=8): src is read-only. */
  utb_emit(ir, TCCIR_OP_FUNCPARAMVAL, UTB_NONE, utb_imm(100, I32),
           utb_imm((int32_t)TCCIR_ENCODE_PARAM(4, 0), I32));
  utb_emit(ir, TCCIR_OP_FUNCPARAMVAL, UTB_NONE, utb_temp(0, I32),
           utb_imm((int32_t)TCCIR_ENCODE_PARAM(4, 1), I32));
  utb_emit(ir, TCCIR_OP_FUNCPARAMVAL, UTB_NONE, utb_imm(8, I32),
           utb_imm((int32_t)TCCIR_ENCODE_PARAM(4, 2), I32));
  utb_emit(ir, TCCIR_OP_FUNCCALLVOID, UTB_NONE, memcpy_fn,
           utb_imm((int32_t)TCCIR_ENCODE_CALL(4, 3), I32));
  int icall = utb_emit_dadd_rmw(ir, callee, 1, 0, 1, 2, 1.0);

  int changes = tcc_ir_opt_const_aggregate_fold(ir);

  UT_ASSERT_EQ(changes, 1);
  UT_ASSERT_EQ(utb_op(ir, icall), TCCIR_OP_ASSIGN);
  UT_ASSERT(utb_folded_double(ir, icall) == 2.25);

  utb_free(ir);
  return 0;
}

/* Negative (no root): every access to the slot is a DIRECT StackLoc deref with
 * no LEA chain, so the resolved root stays CAF_ROOT_NONE and the candidate is
 * dropped -> the dadd does not fold. */
UT_TEST(test_const_agg_no_lea_root_no_fold)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);

  static Sym dadd;
  IROperand callee = utb_callee_named(ir, &dadd, TOK_DADD);
  utb_set_tok_str(TOK_DADD, "__aeabi_dadd");

  /* STORE/LOAD via direct lval StackLoc[64] (no LEA temp roots the object). */
  utb_emit(ir, TCCIR_OP_STORE, utb_stackoff(64, 1, 0, 0, F64), utb_f64imm(ir, 1.25), UTB_NONE);
  utb_emit(ir, TCCIR_OP_LOAD, utb_temp(1, F64), utb_stackoff(64, 1, 0, 0, F64), UTB_NONE);
  utb_emit(ir, TCCIR_OP_FUNCPARAMVAL, UTB_NONE, utb_temp(1, F64),
           utb_imm((int32_t)TCCIR_ENCODE_PARAM(1, 0), I32));
  utb_emit(ir, TCCIR_OP_FUNCPARAMVAL, UTB_NONE, utb_f64imm(ir, 1.0),
           utb_imm((int32_t)TCCIR_ENCODE_PARAM(1, 1), I32));
  int icall = utb_emit(ir, TCCIR_OP_FUNCCALLVAL, utb_temp(2, F64), callee,
                       utb_imm((int32_t)TCCIR_ENCODE_CALL(1, 2), I32));

  int changes = tcc_ir_opt_const_aggregate_fold(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, icall), TCCIR_OP_FUNCCALLVAL);

  utb_free(ir);
  return 0;
}

/* Negative (non-constant operand): the dadd's first argument is an unknown TEMP
 * (never loaded from a tracked slot), so the fold cannot compute a value and
 * leaves the call intact. */
UT_TEST(test_const_agg_nonconst_arg_no_fold)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);

  static Sym dadd;
  IROperand callee = utb_callee_named(ir, &dadd, TOK_DADD);
  utb_set_tok_str(TOK_DADD, "__aeabi_dadd");

  utb_emit_lea_slot(ir, 0, 64);
  utb_emit(ir, TCCIR_OP_STORE, utb_lval(utb_temp(0, F64)), utb_f64imm(ir, 1.25), UTB_NONE);
  /* The LOAD result is dropped; param 0 is an unrelated, unknown TEMP T9. */
  utb_emit(ir, TCCIR_OP_LOAD, utb_temp(1, F64), utb_lval(utb_temp(0, F64)), UTB_NONE);
  utb_emit(ir, TCCIR_OP_FUNCPARAMVAL, UTB_NONE, utb_temp(9, F64),
           utb_imm((int32_t)TCCIR_ENCODE_PARAM(1, 0), I32));
  utb_emit(ir, TCCIR_OP_FUNCPARAMVAL, UTB_NONE, utb_f64imm(ir, 1.0),
           utb_imm((int32_t)TCCIR_ENCODE_PARAM(1, 1), I32));
  int icall = utb_emit(ir, TCCIR_OP_FUNCCALLVAL, utb_temp(2, F64), callee,
                       utb_imm((int32_t)TCCIR_ENCODE_CALL(1, 2), I32));

  int changes = tcc_ir_opt_const_aggregate_fold(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, icall), TCCIR_OP_FUNCCALLVAL);

  utb_free(ir);
  return 0;
}

/* Negative (wrong callee): an unrelated double-helper name (here __aeabi_dmul)
 * is not a dadd/dsub, so caf_dop returns 0 and nothing is rewritten even though
 * both arguments are known constants. */
UT_TEST(test_const_agg_non_dadd_callee_no_fold)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);

  static Sym dmul;
  IROperand callee = utb_callee_named(ir, &dmul, TOK_DADD);
  utb_set_tok_str(TOK_DADD, "__aeabi_dmul");

  utb_emit_lea_slot(ir, 0, 64);
  utb_emit(ir, TCCIR_OP_STORE, utb_lval(utb_temp(0, F64)), utb_f64imm(ir, 1.25), UTB_NONE);
  int icall = utb_emit_dadd_rmw(ir, callee, 1, 0, 1, 2, 2.0);

  int changes = tcc_ir_opt_const_aggregate_fold(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, icall), TCCIR_OP_FUNCCALLVAL);

  utb_free(ir);
  return 0;
}

/* Negative (IJUMP bail): the presence of any IJUMP forces the whole pass to
 * bail (return 0) — the target set of an indirect jump is not statically known,
 * so cross-call propagation would be unsound. */
UT_TEST(test_const_agg_ijump_bails)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);

  static Sym dadd;
  IROperand callee = utb_callee_named(ir, &dadd, TOK_DADD);
  utb_set_tok_str(TOK_DADD, "__aeabi_dadd");

  utb_emit_lea_slot(ir, 0, 64);
  utb_emit(ir, TCCIR_OP_STORE, utb_lval(utb_temp(0, F64)), utb_f64imm(ir, 1.25), UTB_NONE);
  utb_emit(ir, TCCIR_OP_IJUMP, UTB_NONE, utb_temp(8, I32), UTB_NONE);
  int icall = utb_emit_dadd_rmw(ir, callee, 1, 0, 1, 2, 1.0);

  int changes = tcc_ir_opt_const_aggregate_fold(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, icall), TCCIR_OP_FUNCCALLVAL);

  utb_free(ir);
  return 0;
}

/* Negative (out-of-range jump target): a JUMP whose target is outside the
 * function bails the whole pass. */
UT_TEST(test_const_agg_oob_jump_target_bails)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);

  static Sym dadd;
  IROperand callee = utb_callee_named(ir, &dadd, TOK_DADD);
  utb_set_tok_str(TOK_DADD, "__aeabi_dadd");

  utb_emit_lea_slot(ir, 0, 64);
  utb_emit(ir, TCCIR_OP_STORE, utb_lval(utb_temp(0, F64)), utb_f64imm(ir, 1.25), UTB_NONE);
  utb_emit(ir, TCCIR_OP_JUMP, utb_imm(999, I32), UTB_NONE, UTB_NONE); /* target out of range */
  int icall = utb_emit_dadd_rmw(ir, callee, 1, 0, 1, 2, 1.0);

  int changes = tcc_ir_opt_const_aggregate_fold(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, icall), TCCIR_OP_FUNCCALLVAL);

  utb_free(ir);
  return 0;
}

/* Control-flow JOIN (conflict): two predecessor paths store DIFFERENT constants
 * to the slot, so at the merge the slot value is unknown and the post-merge dadd
 * must NOT fold. */
UT_TEST(test_const_agg_merge_conflict_no_fold)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);

  static Sym dadd;
  IROperand callee = utb_callee_named(ir, &dadd, TOK_DADD);
  utb_set_tok_str(TOK_DADD, "__aeabi_dadd");

  utb_emit_lea_slot(ir, 0, 64);                                                   /* 0 */
  utb_emit(ir, TCCIR_OP_STORE, utb_lval(utb_temp(0, F64)), utb_f64imm(ir, 1.25), UTB_NONE); /* 1 */
  utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(5, I32), utb_temp(7, I32), UTB_NONE);     /* 2 -> 5 */
  utb_emit(ir, TCCIR_OP_STORE, utb_lval(utb_temp(0, F64)), utb_f64imm(ir, 9.0), UTB_NONE);  /* 3 */
  utb_emit(ir, TCCIR_OP_JUMP, utb_imm(5, I32), UTB_NONE, UTB_NONE);               /* 4 -> 5 */
  utb_emit(ir, TCCIR_OP_LOAD, utb_temp(1, F64), utb_lval(utb_temp(0, F64)), UTB_NONE); /* 5 */
  utb_emit(ir, TCCIR_OP_FUNCPARAMVAL, UTB_NONE, utb_temp(1, F64),
           utb_imm((int32_t)TCCIR_ENCODE_PARAM(1, 0), I32));                      /* 6 */
  utb_emit(ir, TCCIR_OP_FUNCPARAMVAL, UTB_NONE, utb_f64imm(ir, 1.0),
           utb_imm((int32_t)TCCIR_ENCODE_PARAM(1, 1), I32));                      /* 7 */
  int icall = utb_emit(ir, TCCIR_OP_FUNCCALLVAL, utb_temp(2, F64), callee,
                       utb_imm((int32_t)TCCIR_ENCODE_CALL(1, 2), I32));           /* 8 */

  int changes = tcc_ir_opt_const_aggregate_fold(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, icall), TCCIR_OP_FUNCCALLVAL);

  utb_free(ir);
  return 0;
}

/* Control-flow JOIN (agreement): both predecessor paths store the SAME constant
 * to the slot, so the meet keeps the value known at the merge and the post-merge
 * dadd folds. */
UT_TEST(test_const_agg_merge_agree_folds)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);

  static Sym dadd;
  IROperand callee = utb_callee_named(ir, &dadd, TOK_DADD);
  utb_set_tok_str(TOK_DADD, "__aeabi_dadd");

  utb_emit_lea_slot(ir, 0, 64);                                                   /* 0 */
  utb_emit(ir, TCCIR_OP_STORE, utb_lval(utb_temp(0, F64)), utb_f64imm(ir, 1.25), UTB_NONE); /* 1 */
  utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(5, I32), utb_temp(7, I32), UTB_NONE);     /* 2 -> 5 */
  utb_emit(ir, TCCIR_OP_STORE, utb_lval(utb_temp(0, F64)), utb_f64imm(ir, 1.25), UTB_NONE); /* 3 same */
  utb_emit(ir, TCCIR_OP_JUMP, utb_imm(5, I32), UTB_NONE, UTB_NONE);               /* 4 -> 5 */
  utb_emit(ir, TCCIR_OP_LOAD, utb_temp(1, F64), utb_lval(utb_temp(0, F64)), UTB_NONE); /* 5 */
  utb_emit(ir, TCCIR_OP_FUNCPARAMVAL, UTB_NONE, utb_temp(1, F64),
           utb_imm((int32_t)TCCIR_ENCODE_PARAM(1, 0), I32));                      /* 6 */
  utb_emit(ir, TCCIR_OP_FUNCPARAMVAL, UTB_NONE, utb_f64imm(ir, 1.0),
           utb_imm((int32_t)TCCIR_ENCODE_PARAM(1, 1), I32));                      /* 7 */
  int icall = utb_emit(ir, TCCIR_OP_FUNCCALLVAL, utb_temp(2, F64), callee,
                       utb_imm((int32_t)TCCIR_ENCODE_CALL(1, 2), I32));           /* 8 */

  int changes = tcc_ir_opt_const_aggregate_fold(ir);

  UT_ASSERT_EQ(changes, 1);
  UT_ASSERT_EQ(utb_op(ir, icall), TCCIR_OP_ASSIGN);
  UT_ASSERT(utb_folded_double(ir, icall) == 2.25);

  utb_free(ir);
  return 0;
}

/* Negative (overlapping wide store kills slot): after the constant store, an
 * overlapping store of an UNKNOWN value to the same slot clears the lattice, so
 * the dadd of the reloaded (now-unknown) value does not fold. */
UT_TEST(test_const_agg_unknown_overstore_kills)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);

  static Sym dadd;
  IROperand callee = utb_callee_named(ir, &dadd, TOK_DADD);
  utb_set_tok_str(TOK_DADD, "__aeabi_dadd");

  utb_emit_lea_slot(ir, 0, 64);
  utb_emit(ir, TCCIR_OP_STORE, utb_lval(utb_temp(0, F64)), utb_f64imm(ir, 1.25), UTB_NONE);
  /* Overwrite the slot with an unknown 64-bit TEMP -> slot becomes unknown. */
  utb_emit(ir, TCCIR_OP_STORE, utb_lval(utb_temp(0, F64)), utb_temp(5, F64), UTB_NONE);
  int icall = utb_emit_dadd_rmw(ir, callee, 1, 0, 1, 2, 1.0);

  int changes = tcc_ir_opt_const_aggregate_fold(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, icall), TCCIR_OP_FUNCCALLVAL);

  utb_free(ir);
  return 0;
}

/* Boundary (LEA + constant offset arithmetic roots the same object): the field
 * address is reached as `Taddr = LEA base; Tfield = Taddr + 0`, which still
 * resolves to the same non-escaped root, so the dadd folds. */
UT_TEST(test_const_agg_lea_plus_offset_root_folds)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);

  static Sym dadd;
  IROperand callee = utb_callee_named(ir, &dadd, TOK_DADD);
  utb_set_tok_str(TOK_DADD, "__aeabi_dadd");

  utb_emit_lea_slot(ir, 0, 64);                                /* T0 = &base, root 64 */
  /* T1 = T0 + 0  (address-propagation arithmetic, same root). */
  utb_emit(ir, TCCIR_OP_ADD, utb_temp(1, I32), utb_temp(0, I32), utb_imm(0, I32));
  utb_emit(ir, TCCIR_OP_STORE, utb_lval(utb_temp(1, F64)), utb_f64imm(ir, 1.25), UTB_NONE);
  int icall = utb_emit_dadd_rmw(ir, callee, 1, /*addr*/ 1, /*ld*/ 2, /*res*/ 3, 1.0);

  int changes = tcc_ir_opt_const_aggregate_fold(ir);

  UT_ASSERT_EQ(changes, 1);
  UT_ASSERT_EQ(utb_op(ir, icall), TCCIR_OP_ASSIGN);
  UT_ASSERT(utb_folded_double(ir, icall) == 2.25);

  utb_free(ir);
  return 0;
}

/* ------------------------------------------------------------------ suite */

UT_SUITE(opt_const_aggregate)
{
  UT_COVERS("const_aggregate_fold");
  UT_RUN(test_const_agg_single_dadd_folds);
  UT_RUN(test_const_agg_single_dsub_folds);
  UT_RUN(test_const_agg_chain_depth2_one_pass);
  UT_RUN(test_const_agg_idempotent);
  UT_RUN(test_const_agg_address_escape_no_fold);
  UT_RUN(test_const_agg_memcpy_source_still_folds);
  UT_RUN(test_const_agg_no_lea_root_no_fold);
  UT_RUN(test_const_agg_nonconst_arg_no_fold);
  UT_RUN(test_const_agg_non_dadd_callee_no_fold);
  UT_RUN(test_const_agg_ijump_bails);
  UT_RUN(test_const_agg_oob_jump_target_bails);
  UT_RUN(test_const_agg_merge_conflict_no_fold);
  UT_RUN(test_const_agg_merge_agree_folds);
  UT_RUN(test_const_agg_unknown_overstore_kills);
  UT_RUN(test_const_agg_lea_plus_offset_root_folds);
}
