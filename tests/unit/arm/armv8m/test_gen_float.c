/*
 *  test_gen_float.c - suite for ir/gen/float.c, the IR-generation layer for
 *  floating point (backend/ binary, build_backend/run_unit_tests_backend).
 *
 *  Covers all ten exported entry points:
 *    - tcc_ir_gen_fadd/fsub/fmul/fdiv(): binary FP ops -> TCCIR_OP_FADD/FSUB/
 *      FMUL/FDIV with a fresh TEMP dest, float/double vreg marking, and the
 *      "result into vtop[-1], pop one" value-stack contract.
 *    - tcc_ir_gen_f(): the generic dispatcher -- arithmetic tokens
 *      ('+','-','*','/'), 'n' (negate), 'c' (compare), 't'/'i'/'f'
 *      (conversions), and the TOK_ULT..TOK_GT comparison range with the
 *      GT/GE operand-swap NaN fix.
 *    - tcc_ir_gen_fneg(): unary TCCIR_OP_FNEG (src1 = vtop[0], no pop).
 *    - tcc_ir_gen_fcmp(): TCCIR_OP_FCMP with the TOK_LT default cmp_op.
 *    - tcc_ir_gen_cvt_ftof/itof/ftoi(): conversion wrappers.
 *    - the complex-operand path (single IR op, complex-flagged dest, and the
 *      tcc_ir_put soft-call gate bypassed).
 *    - the soft-float path through tcc_ir_put_soft_call() (FUNCPARAMVAL/
 *      FUNCCALL* sequence) when the installed FPU config lacks the op.
 *
 *  HARNESS NOTES:
 *    - This suite links into the BACKEND binary (UT2), next to test_gen_fp.c
 *      (which covers the arm-thumb-gen.c emitter layer one level below).
 *      UT2 links the real arm-thumb-gen.c, so the soft-float helper-name
 *      lookup (tcc_get_abi_softcall_name) is REAL here -- unlike the main
 *      binary's NULL-returning stub in stubs_gen_machine_fallback.c, which
 *      would route any soft-call into stubs.c's aborting _tcc_error.
 *    - vtop/_vstack/vswap are NOT defined anywhere in UT2 (tccgen.c is not
 *      linked; the main binary's cgstub fake stack lives in
 *      codegen_mop_stubs.c, deliberately excluded from UT2). This file
 *      provides its own minimal definitions. vswap() mirrors tccgen.c's,
 *      minus vcheck_cmp() (frontend cmp-chain fixup; no cmp chains exist in
 *      these fixtures).
 *    - ir_put_soft_call_fpu_if_needed() (ir/gen/softfloat.c, called from
 *      tcc_ir_put for every FPU op) dereferences architecture_config.fpu,
 *      which arm_target_init() always leaves NULL (arm.c's arm_resolve_fpu
 *      is a NULL-returning TODO stub). Every test therefore installs a
 *      test-local FloatingPointConfig (all-capable => native path,
 *      zero-capability => soft-call path) and restores the previous pointer
 *      before returning. float_abi is likewise pinned to ARM_SOFT_FLOAT
 *      (sibling UT2 suites set ARM_HARD_FLOAT and don't restore; use_vfp in
 *      the vreg intervals depends on it).
 *    - tcc_is_64bit_operand() is a stubs.c stub returning 0, so the
 *      soft-float gate always consults the single-precision has_f* flags,
 *      even for VT_DOUBLE operands: the native/soft decision in this harness
 *      is driven purely by the installed config.
 *    - CVT_FTOF can never take the soft-call path in this harness: the
 *      same-size check in ir_put_soft_call_fpu_if_needed uses type_size()
 *      (stubs.c stub, always 4), so src_size == dst_size always holds and
 *      the gate returns "native/copy". The soft-CVT_FTOF shape is therefore
 *      untestable here (pinned by test_soft_cvt_ftof_stays_native...).
 *    - The default-case tcc_error("unknown floating point operation") guard
 *      in tcc_ir_gen_f() is untestable in this harness: stubs.c's _tcc_error
 *      abort()s the whole binary. Not a coverage gap we can close without a
 *      production change (see the guide's untestability clause).
 *    - tcc_ir_gen_fadd/fsub/fmul/fdiv/fneg/fcmp and tcc_ir_gen_cvt_* have NO
 *      callers in the product: the frontend reaches float.c only through
 *      tcc_ir_gen_f() (tccgen.c:4897 'n', tccgen.c:5240 gen_opif general
 *      case), and gen_cast() inlines its own conversion tcc_ir_put with
 *      vtop as src1. The wrappers are tested here as exported API. The
 *      CVT wrappers' &vtop[-1]-as-source behavior is a suspected bug pinned
 *      by characterization tests (see docs/bugs.md).
 */

#define USING_GLOBALS
#include "ir.h"

#include "ut.h"

/* The nine wrapper entry points are defined non-static in ir/gen/float.c but
 * declared in no header (only tcc_ir_gen_f is, via ir/core.h) -- declare
 * them locally rather than editing a production header (same pattern as
 * test_ir_core.c's tcc_irop_from_token). */
extern void tcc_ir_gen_fadd(TCCIRState *ir);
extern void tcc_ir_gen_fsub(TCCIRState *ir);
extern void tcc_ir_gen_fmul(TCCIRState *ir);
extern void tcc_ir_gen_fdiv(TCCIRState *ir);
extern void tcc_ir_gen_fneg(TCCIRState *ir);
extern void tcc_ir_gen_fcmp(TCCIRState *ir);
extern void tcc_ir_gen_cvt_ftof(TCCIRState *ir);
extern void tcc_ir_gen_cvt_itof(TCCIRState *ir);
extern void tcc_ir_gen_cvt_ftoi(TCCIRState *ir);

/* ---------------------------------------------------------------------------
 * Minimal frontend value stack (see HARNESS NOTES).
 * ------------------------------------------------------------------------- */

SValue _vstack[VSTACK_SIZE];
SValue *vtop = _vstack; /* empty: vtop == _vstack */

void vswap(void)
{
  SValue tmp = vtop[0];
  vtop[0] = vtop[-1];
  vtop[-1] = tmp;
}

/* ---------------------------------------------------------------------------
 * FPU configs + fixture
 * ------------------------------------------------------------------------- */

/* Every has_* capability present: ir_put_soft_call_fpu_if_needed() always
 * answers "hardware can do it" and tcc_ir_put emits the plain FP op. */
static const FloatingPointConfig ut_fpu_all = {
    .reg_size = 4,  .reg_count = 32, .stack_align = 8,
    .has_fadd = 1,  .has_fsub = 1,   .has_fmul = 1,   .has_fdiv = 1,
    .has_fcmp = 1,  .has_ftof = 1,   .has_itof = 1,   .has_ftod = 1,
    .has_ftoi = 1,  .has_dadd = 1,   .has_dsub = 1,   .has_dmul = 1,
    .has_ddiv = 1,  .has_dcmp = 1,   .has_dtof = 1,   .has_itod = 1,
    .has_dtoi = 1,  .has_ltod = 1,   .has_ltof = 1,   .has_dtol = 1,
    .has_ftol = 1,  .has_fneg = 1,   .has_dneg = 1,
};

/* No hardware capability: every FPU op routes to tcc_ir_put_soft_call(). */
static const FloatingPointConfig ut_fpu_soft;

static const FloatingPointConfig *ut_saved_fpu;
static int ut_saved_float_abi;

static void ut_setup(const FloatingPointConfig *cfg)
{
  ut_saved_fpu = architecture_config.fpu;
  architecture_config.fpu = cfg;
  ut_saved_float_abi = tcc_state->float_abi;
  tcc_state->float_abi = ARM_SOFT_FLOAT;
  vtop = _vstack; /* reset the value stack to empty */
}

static void ut_teardown(TCCIRState *ir)
{
  architecture_config.fpu = ut_saved_fpu;
  tcc_state->float_abi = ut_saved_float_abi;
  vtop = _vstack;
  if (ir)
    tcc_ir_free(ir);
}

/* Push a plain vreg value (r = 0, register-class) of the given type;
 * returns the freshly allocated TEMP vreg. */
static int ut_push_var(TCCIRState *ir, int type_t)
{
  int vr = tcc_ir_vreg_alloc_temp(ir);
  vtop++;
  svalue_init(vtop);
  vtop->vr = vr;
  vtop->type.t = type_t;
  return vr;
}

static void ut_push_const_f32(float f)
{
  vtop++;
  svalue_init(vtop);
  vtop->r = VT_CONST;
  vtop->c.f = f;
  vtop->type.t = VT_FLOAT;
}

/* ---------------------------------------------------------------------------
 * Shared oracle for the native binary-op path: exactly one instruction with
 * the expected opcode; dest is a fresh TEMP of the expected btype, flagged
 * float (and double when want_is_double); src1/src2 are the two pushed
 * vregs; the result lands in vtop[-1] and the stack is popped by one.
 * Returns 0 on success, -1 on the first failed assertion.
 * ------------------------------------------------------------------------- */
static int check_native_fbinop(TCCIRState *ir, int a_vr, int b_vr, TccIrOp want_op, int want_btype,
                               int want_is_double)
{
  UT_ASSERT_EQ(tcc_ir_count(ir), 1);
  IRQuadCompact *q = &ir->compact_instructions[0];
  UT_ASSERT_EQ(q->op, want_op);

  IROperand d = tcc_ir_op_get_dest(ir, q);
  IROperand s1 = tcc_ir_op_get_src1(ir, q);
  IROperand s2 = tcc_ir_op_get_src2(ir, q);

  UT_ASSERT_EQ(irop_get_vreg(s1), a_vr);
  UT_ASSERT_EQ(irop_get_vreg(s2), b_vr);
  UT_ASSERT_EQ(irop_get_btype(s1), want_btype);
  UT_ASSERT_EQ(irop_get_btype(s2), want_btype);

  int dvr = irop_get_vreg(d);
  UT_ASSERT_EQ(TCCIR_DECODE_VREG_TYPE(dvr), TCCIR_VREG_TYPE_TEMP);
  UT_ASSERT(dvr != a_vr && dvr != b_vr);
  UT_ASSERT_EQ(irop_get_btype(d), want_btype);

  IRLiveInterval *iv = tcc_ir_vreg_live_interval(ir, dvr);
  UT_ASSERT(iv != NULL);
  UT_ASSERT_EQ(iv->is_float, 1);
  UT_ASSERT_EQ(iv->is_double, want_is_double);
  UT_ASSERT_EQ(iv->use_vfp, 0); /* float_abi pinned to ARM_SOFT_FLOAT by ut_setup */

  /* value-stack contract: result into vtop[-1], one value popped */
  UT_ASSERT_EQ(vtop, _vstack + 1);
  UT_ASSERT_EQ(vtop->vr, dvr);
  UT_ASSERT_EQ(vtop->r, 0);
  return 0;
}

/* Shared oracle for a comparison token through tcc_ir_gen_f(): one FCMP, no
 * dest, operands in original or swapped order, and vtop rewritten to a
 * VT_CMP pending-comparison value. Returns 0 on success, -1 on failure. */
static int check_fcmp_token(int tok, int expect_swap, int want_cmp_op)
{
  ut_setup(&ut_fpu_all);
  TCCIRState *ir = tcc_ir_alloc();
  int a = ut_push_var(ir, VT_FLOAT);
  int b = ut_push_var(ir, VT_FLOAT);

  tcc_ir_gen_f(ir, tok);

  UT_ASSERT(!irop_config[TCCIR_OP_FCMP].has_dest);
  UT_ASSERT_EQ(tcc_ir_count(ir), 1);
  IRQuadCompact *q = &ir->compact_instructions[0];
  UT_ASSERT_EQ(q->op, TCCIR_OP_FCMP);

  int src1 = irop_get_vreg(tcc_ir_op_get_src1(ir, q));
  int src2 = irop_get_vreg(tcc_ir_op_get_src2(ir, q));
  if (expect_swap)
  {
    UT_ASSERT_EQ(src1, b);
    UT_ASSERT_EQ(src2, a);
  }
  else
  {
    UT_ASSERT_EQ(src1, a);
    UT_ASSERT_EQ(src2, b);
  }

  UT_ASSERT_EQ(vtop, _vstack + 1);
  UT_ASSERT_EQ(vtop->r, VT_CMP);
  UT_ASSERT_EQ(vtop->cmp_op, want_cmp_op);
  UT_ASSERT_EQ(vtop->jtrue, -1);
  UT_ASSERT_EQ(vtop->jfalse, -1);
  UT_ASSERT_EQ(vtop->vr, -1); /* stale vreg cleared so gv() materializes the CMP */

  ut_teardown(ir);
  return 0;
}

/* -------------------------------------------------------------------------- */
/* Binary arithmetic: native path                                             */
/* -------------------------------------------------------------------------- */

UT_TEST(test_fadd_f32_native_shape)
{
  ut_setup(&ut_fpu_all);
  TCCIRState *ir = tcc_ir_alloc();
  int a = ut_push_var(ir, VT_FLOAT);
  int b = ut_push_var(ir, VT_FLOAT);

  tcc_ir_gen_fadd(ir);

  if (check_native_fbinop(ir, a, b, TCCIR_OP_FADD, IROP_BTYPE_FLOAT32, 0))
    return -1;
  ut_teardown(ir);
  return 0;
}

UT_TEST(test_fadd_f64_native_marks_double)
{
  ut_setup(&ut_fpu_all);
  TCCIRState *ir = tcc_ir_alloc();
  int a = ut_push_var(ir, VT_DOUBLE);
  int b = ut_push_var(ir, VT_DOUBLE);

  tcc_ir_gen_fadd(ir);

  if (check_native_fbinop(ir, a, b, TCCIR_OP_FADD, IROP_BTYPE_FLOAT64, 1))
    return -1;
  ut_teardown(ir);
  return 0;
}

UT_TEST(test_fadd_ldouble_treated_as_double)
{
  /* float.c's is_double checks VT_DOUBLE || VT_LDOUBLE; the operand layer
   * maps VT_LDOUBLE to IROP_BTYPE_FLOAT64 (ARM: long double == double). */
  ut_setup(&ut_fpu_all);
  TCCIRState *ir = tcc_ir_alloc();
  int a = ut_push_var(ir, VT_LDOUBLE);
  int b = ut_push_var(ir, VT_LDOUBLE);

  tcc_ir_gen_fadd(ir);

  if (check_native_fbinop(ir, a, b, TCCIR_OP_FADD, IROP_BTYPE_FLOAT64, 1))
    return -1;
  ut_teardown(ir);
  return 0;
}

UT_TEST(test_fsub_wrapper_emits_fsub)
{
  ut_setup(&ut_fpu_all);
  TCCIRState *ir = tcc_ir_alloc();
  int a = ut_push_var(ir, VT_FLOAT);
  int b = ut_push_var(ir, VT_FLOAT);

  tcc_ir_gen_fsub(ir);

  if (check_native_fbinop(ir, a, b, TCCIR_OP_FSUB, IROP_BTYPE_FLOAT32, 0))
    return -1;
  ut_teardown(ir);
  return 0;
}

UT_TEST(test_fmul_wrapper_emits_fmul)
{
  ut_setup(&ut_fpu_all);
  TCCIRState *ir = tcc_ir_alloc();
  int a = ut_push_var(ir, VT_FLOAT);
  int b = ut_push_var(ir, VT_FLOAT);

  tcc_ir_gen_fmul(ir);

  if (check_native_fbinop(ir, a, b, TCCIR_OP_FMUL, IROP_BTYPE_FLOAT32, 0))
    return -1;
  ut_teardown(ir);
  return 0;
}

UT_TEST(test_fdiv_wrapper_emits_fdiv)
{
  ut_setup(&ut_fpu_all);
  TCCIRState *ir = tcc_ir_alloc();
  int a = ut_push_var(ir, VT_FLOAT);
  int b = ut_push_var(ir, VT_FLOAT);

  tcc_ir_gen_fdiv(ir);

  if (check_native_fbinop(ir, a, b, TCCIR_OP_FDIV, IROP_BTYPE_FLOAT32, 0))
    return -1;
  ut_teardown(ir);
  return 0;
}

UT_TEST(test_gen_f_dispatches_arith_tokens)
{
  /* The live frontend entry (tccgen.c:5240) reaches float.c through the
   * dispatcher; the wrappers above are one-line forwarders over it. */
  static const struct
  {
    int tok;
    TccIrOp op;
  } MAP[] = {
      {'+', TCCIR_OP_FADD},
      {'-', TCCIR_OP_FSUB},
      {'*', TCCIR_OP_FMUL},
      {'/', TCCIR_OP_FDIV},
  };
  for (int i = 0; i < (int)(sizeof(MAP) / sizeof(MAP[0])); i++)
  {
    ut_setup(&ut_fpu_all);
    TCCIRState *ir = tcc_ir_alloc();
    int a = ut_push_var(ir, VT_FLOAT);
    int b = ut_push_var(ir, VT_FLOAT);

    tcc_ir_gen_f(ir, MAP[i].tok);

    if (check_native_fbinop(ir, a, b, MAP[i].op, IROP_BTYPE_FLOAT32, 0))
      return -1;
    ut_teardown(ir);
  }
  return 0;
}

UT_TEST(test_fadd_const_f32_src2_inline_operand)
{
  /* A float constant operand is inlined as an IROP_TAG_F32 immediate
   * (svalue_to_iroperand Case 4) rather than a vreg. */
  ut_setup(&ut_fpu_all);
  TCCIRState *ir = tcc_ir_alloc();
  int a = ut_push_var(ir, VT_FLOAT);
  ut_push_const_f32(2.5f);

  tcc_ir_gen_fadd(ir);

  UT_ASSERT_EQ(tcc_ir_count(ir), 1);
  IRQuadCompact *q = &ir->compact_instructions[0];
  UT_ASSERT_EQ(q->op, TCCIR_OP_FADD);
  UT_ASSERT_EQ(irop_get_vreg(tcc_ir_op_get_src1(ir, q)), a);

  IROperand s2 = tcc_ir_op_get_src2(ir, q);
  UT_ASSERT_EQ(irop_get_tag(s2), IROP_TAG_F32);
  union
  {
    float f;
    uint32_t u;
  } want;
  want.f = 2.5f;
  UT_ASSERT_EQ((uint32_t)s2.u.f32_bits, want.u);

  ut_teardown(ir);
  return 0;
}

/* -------------------------------------------------------------------------- */
/* Negation: unary                                                            */
/* -------------------------------------------------------------------------- */

UT_TEST(test_fneg_f32_native_shape)
{
  ut_setup(&ut_fpu_all);
  TCCIRState *ir = tcc_ir_alloc();
  int a = ut_push_var(ir, VT_FLOAT);

  tcc_ir_gen_fneg(ir);

  UT_ASSERT(irop_config[TCCIR_OP_FNEG].has_dest);
  UT_ASSERT(!irop_config[TCCIR_OP_FNEG].has_src2); /* unary shape */
  UT_ASSERT_EQ(tcc_ir_count(ir), 1);
  IRQuadCompact *q = &ir->compact_instructions[0];
  UT_ASSERT_EQ(q->op, TCCIR_OP_FNEG);

  IROperand d = tcc_ir_op_get_dest(ir, q);
  IROperand s1 = tcc_ir_op_get_src1(ir, q);
  UT_ASSERT_EQ(irop_get_vreg(s1), a); /* src1 = vtop[0], the value itself */

  int dvr = irop_get_vreg(d);
  UT_ASSERT_EQ(TCCIR_DECODE_VREG_TYPE(dvr), TCCIR_VREG_TYPE_TEMP);
  UT_ASSERT(dvr != a);
  UT_ASSERT_EQ(irop_get_btype(d), IROP_BTYPE_FLOAT32);
  IRLiveInterval *iv = tcc_ir_vreg_live_interval(ir, dvr);
  UT_ASSERT_EQ(iv->is_float, 1);
  UT_ASSERT_EQ(iv->is_double, 0);

  /* unary contract: stack depth unchanged, top rewritten to the result */
  UT_ASSERT_EQ(vtop, _vstack + 1);
  UT_ASSERT_EQ(vtop->vr, dvr);
  UT_ASSERT_EQ(vtop->r, 0);
  UT_ASSERT_EQ(vtop->type.t, VT_FLOAT);

  ut_teardown(ir);
  return 0;
}

UT_TEST(test_fneg_f64_marks_double)
{
  ut_setup(&ut_fpu_all);
  TCCIRState *ir = tcc_ir_alloc();
  int a = ut_push_var(ir, VT_DOUBLE);

  tcc_ir_gen_fneg(ir);

  UT_ASSERT_EQ(tcc_ir_count(ir), 1);
  IRQuadCompact *q = &ir->compact_instructions[0];
  UT_ASSERT_EQ(q->op, TCCIR_OP_FNEG);
  UT_ASSERT_EQ(irop_get_vreg(tcc_ir_op_get_src1(ir, q)), a);
  IROperand d = tcc_ir_op_get_dest(ir, q);
  UT_ASSERT_EQ(irop_get_btype(d), IROP_BTYPE_FLOAT64);
  IRLiveInterval *iv = tcc_ir_vreg_live_interval(ir, irop_get_vreg(d));
  UT_ASSERT_EQ(iv->is_float, 1);
  UT_ASSERT_EQ(iv->is_double, 1);

  ut_teardown(ir);
  return 0;
}

UT_TEST(test_gen_f_neg_token_emits_fneg)
{
  /* tccgen.c's gen_negf() calls tcc_ir_gen_f(ir, 'n') -- the live path. */
  ut_setup(&ut_fpu_all);
  TCCIRState *ir = tcc_ir_alloc();
  int a = ut_push_var(ir, VT_FLOAT);

  tcc_ir_gen_f(ir, 'n');

  UT_ASSERT_EQ(tcc_ir_count(ir), 1);
  IRQuadCompact *q = &ir->compact_instructions[0];
  UT_ASSERT_EQ(q->op, TCCIR_OP_FNEG);
  UT_ASSERT_EQ(irop_get_vreg(tcc_ir_op_get_src1(ir, q)), a);
  UT_ASSERT_EQ(vtop, _vstack + 1);

  ut_teardown(ir);
  return 0;
}

/* -------------------------------------------------------------------------- */
/* Comparisons                                                                */
/* -------------------------------------------------------------------------- */

UT_TEST(test_fcmp_wrapper_emits_fcmp_vt_cmp_default_lt)
{
  /* The 'c' case (dead wrapper -- the frontend always passes a concrete
   * comparison token instead): cmp_op defaults to TOK_LT "fixed up later". */
  ut_setup(&ut_fpu_all);
  TCCIRState *ir = tcc_ir_alloc();
  int a = ut_push_var(ir, VT_FLOAT);
  int b = ut_push_var(ir, VT_FLOAT);

  tcc_ir_gen_fcmp(ir);

  UT_ASSERT_EQ(tcc_ir_count(ir), 1);
  IRQuadCompact *q = &ir->compact_instructions[0];
  UT_ASSERT_EQ(q->op, TCCIR_OP_FCMP);
  UT_ASSERT_EQ(irop_get_vreg(tcc_ir_op_get_src1(ir, q)), a);
  UT_ASSERT_EQ(irop_get_vreg(tcc_ir_op_get_src2(ir, q)), b);

  UT_ASSERT_EQ(vtop, _vstack + 1);
  UT_ASSERT_EQ(vtop->r, VT_CMP);
  UT_ASSERT_EQ(vtop->cmp_op, TOK_LT); /* documented default */
  UT_ASSERT_EQ(vtop->jtrue, -1);
  UT_ASSERT_EQ(vtop->jfalse, -1);
  UT_ASSERT_EQ(vtop->vr, -1);

  ut_teardown(ir);
  return 0;
}

UT_TEST(test_gen_f_cmp_lt_le_eq_ne_no_swap)
{
  /* LT/LE/EQ/NE and the unsigned LT/LE forms keep operand order. */
  if (check_fcmp_token(TOK_LT, 0, TOK_LT))
    return -1;
  if (check_fcmp_token(TOK_LE, 0, TOK_LE))
    return -1;
  if (check_fcmp_token(TOK_EQ, 0, TOK_EQ))
    return -1;
  if (check_fcmp_token(TOK_NE, 0, TOK_NE))
    return -1;
  if (check_fcmp_token(TOK_ULT, 0, TOK_ULT))
    return -1;
  if (check_fcmp_token(TOK_ULE, 0, TOK_ULE))
    return -1;
  return 0;
}

UT_TEST(test_gen_f_cmp_gt_ge_swap_operands_for_nan)
{
  /* IEEE 754 NaN fix (ir/gen/float.c:77-98): __aeabi_{c,d}dcmple-style
   * helpers only produce correct flags for LE/LT/EQ/NE, so for GT/GE the
   * dispatcher swaps the operands (via vswap) and mirrors the condition:
   *   a >  b  ->  FCMP(b, a) tested as LT
   *   a >= b  ->  FCMP(b, a) tested as LE */
  if (check_fcmp_token(TOK_GT, 1, TOK_LT))
    return -1;
  if (check_fcmp_token(TOK_GE, 1, TOK_LE))
    return -1;
  if (check_fcmp_token(TOK_UGT, 1, TOK_ULT))
    return -1;
  if (check_fcmp_token(TOK_UGE, 1, TOK_ULE))
    return -1;
  return 0;
}

/* -------------------------------------------------------------------------- */
/* Conversions (dead wrappers -- pinned characterization, see HARNESS NOTES)  */
/* -------------------------------------------------------------------------- */

/* float.c's conversion path calls tcc_ir_put(ir, op, &vtop[-1], &vtop[0],
 * &dest) -- the binary-op call shape -- but irop_config for CVT_* is
 * {dest, src1} with NO src2. So the emitted instruction's source operand is
 * vtop[-1] (the entry BELOW the top), the &vtop[0] argument is silently
 * ignored, the result is written into vtop[0], and nothing is popped. The
 * fixtures below push the source at vtop[-1] and a "destination type"
 * marker at vtop[0] (float.c derives dest.type from vtop[0].type for
 * itof/ftof) -- the only stack layout under which these wrappers produce a
 * sensible instruction. */

UT_TEST(test_cvt_itof_source_taken_from_below_top)
{
  ut_setup(&ut_fpu_all);
  TCCIRState *ir = tcc_ir_alloc();
  int src = ut_push_var(ir, VT_INT);      /* vtop[-1]: value to convert */
  int marker = ut_push_var(ir, VT_FLOAT); /* vtop[0]: destination type holder */

  tcc_ir_gen_cvt_itof(ir);

  UT_ASSERT_EQ(tcc_ir_count(ir), 1);
  IRQuadCompact *q = &ir->compact_instructions[0];
  UT_ASSERT_EQ(q->op, TCCIR_OP_CVT_ITOF);
  UT_ASSERT(!irop_config[TCCIR_OP_CVT_ITOF].has_src2);

  IROperand s1 = tcc_ir_op_get_src1(ir, q);
  UT_ASSERT_EQ(irop_get_vreg(s1), src);
  UT_ASSERT_EQ(irop_get_btype(s1), IROP_BTYPE_INT32);

  IROperand d = tcc_ir_op_get_dest(ir, q);
  int dvr = irop_get_vreg(d);
  UT_ASSERT_EQ(TCCIR_DECODE_VREG_TYPE(dvr), TCCIR_VREG_TYPE_TEMP);
  UT_ASSERT(dvr != src && dvr != marker);
  UT_ASSERT_EQ(irop_get_btype(d), IROP_BTYPE_FLOAT32);
  IRLiveInterval *iv = tcc_ir_vreg_live_interval(ir, dvr);
  UT_ASSERT_EQ(iv->is_float, 1);
  UT_ASSERT_EQ(iv->is_double, 0);

  /* result lands in vtop[0]; vtop[-1] is left untouched (no pop, no rewrite) */
  UT_ASSERT_EQ(vtop, _vstack + 2);
  UT_ASSERT_EQ(vtop[-1].vr, src);
  UT_ASSERT_EQ(vtop[0].vr, dvr);
  UT_ASSERT_EQ(vtop[0].r, 0);
  UT_ASSERT_EQ(vtop[0].type.t, VT_FLOAT);

  ut_teardown(ir);
  return 0;
}

UT_TEST(test_cvt_itof_double_marker_marks_double_dest)
{
  ut_setup(&ut_fpu_all);
  TCCIRState *ir = tcc_ir_alloc();
  int src = ut_push_var(ir, VT_INT);
  ut_push_var(ir, VT_DOUBLE); /* destination type holder */

  tcc_ir_gen_cvt_itof(ir);

  IRQuadCompact *q = &ir->compact_instructions[0];
  UT_ASSERT_EQ(irop_get_vreg(tcc_ir_op_get_src1(ir, q)), src);
  IROperand d = tcc_ir_op_get_dest(ir, q);
  UT_ASSERT_EQ(irop_get_btype(d), IROP_BTYPE_FLOAT64);
  IRLiveInterval *iv = tcc_ir_vreg_live_interval(ir, irop_get_vreg(d));
  UT_ASSERT_EQ(iv->is_float, 1);
  UT_ASSERT_EQ(iv->is_double, 1);
  UT_ASSERT_EQ(vtop[0].type.t, VT_DOUBLE);

  ut_teardown(ir);
  return 0;
}

UT_TEST(test_cvt_ftoi_dest_is_plain_int)
{
  /* ftoi hardcodes dest.type.t = VT_INT (float.c:183) and never marks the
   * dest vreg as float. */
  ut_setup(&ut_fpu_all);
  TCCIRState *ir = tcc_ir_alloc();
  int src = ut_push_var(ir, VT_FLOAT); /* vtop[-1]: value to convert */
  ut_push_var(ir, VT_FLOAT);           /* vtop[0]: ignored by the ftoi branch */

  tcc_ir_gen_cvt_ftoi(ir);

  UT_ASSERT_EQ(tcc_ir_count(ir), 1);
  IRQuadCompact *q = &ir->compact_instructions[0];
  UT_ASSERT_EQ(q->op, TCCIR_OP_CVT_FTOI);
  UT_ASSERT_EQ(irop_get_vreg(tcc_ir_op_get_src1(ir, q)), src);
  UT_ASSERT_EQ(irop_get_btype(tcc_ir_op_get_src1(ir, q)), IROP_BTYPE_FLOAT32);

  IROperand d = tcc_ir_op_get_dest(ir, q);
  UT_ASSERT_EQ(irop_get_btype(d), IROP_BTYPE_INT32);
  IRLiveInterval *iv = tcc_ir_vreg_live_interval(ir, irop_get_vreg(d));
  UT_ASSERT_EQ(iv->is_float, 0);
  UT_ASSERT_EQ(iv->is_double, 0);

  UT_ASSERT_EQ(vtop, _vstack + 2);
  UT_ASSERT_EQ(vtop[0].vr, irop_get_vreg(d));
  UT_ASSERT_EQ(vtop[0].type.t, VT_INT);

  ut_teardown(ir);
  return 0;
}

UT_TEST(test_cvt_ftof_float_to_double)
{
  ut_setup(&ut_fpu_all);
  TCCIRState *ir = tcc_ir_alloc();
  int src = ut_push_var(ir, VT_FLOAT); /* vtop[-1]: value to convert */
  ut_push_var(ir, VT_DOUBLE);          /* vtop[0]: destination type holder */

  tcc_ir_gen_cvt_ftof(ir);

  UT_ASSERT_EQ(tcc_ir_count(ir), 1);
  IRQuadCompact *q = &ir->compact_instructions[0];
  UT_ASSERT_EQ(q->op, TCCIR_OP_CVT_FTOF);
  UT_ASSERT_EQ(irop_get_vreg(tcc_ir_op_get_src1(ir, q)), src);
  UT_ASSERT_EQ(irop_get_btype(tcc_ir_op_get_src1(ir, q)), IROP_BTYPE_FLOAT32);

  IROperand d = tcc_ir_op_get_dest(ir, q);
  UT_ASSERT_EQ(irop_get_btype(d), IROP_BTYPE_FLOAT64);
  IRLiveInterval *iv = tcc_ir_vreg_live_interval(ir, irop_get_vreg(d));
  UT_ASSERT_EQ(iv->is_float, 1);
  UT_ASSERT_EQ(iv->is_double, 1);
  UT_ASSERT_EQ(vtop[0].type.t, VT_DOUBLE);

  ut_teardown(ir);
  return 0;
}

/* Regression lock for docs/bugs.md "tcc_ir_gen_cvt_* read the conversion
 * source from below the top of stack": under the natural one-value cast
 * convention (the value to convert sits at vtop, exactly what gen_cast's
 * inline expansion assumes when it passes `vtop` as src1), the wrapper
 * reads the STALE slot below the top instead. This pins the CURRENT
 * (buggy) behavior -- flip the assertion when float.c passes &vtop[0]. */
UT_TEST(test_cvt_itof_single_value_stack_reads_stale_slot_below)
{
  ut_setup(&ut_fpu_all);
  TCCIRState *ir = tcc_ir_alloc();

  /* Seed the slot below the top with a recognizable (stale) value. */
  int stale = tcc_ir_vreg_alloc_temp(ir);
  svalue_init(&_vstack[0]);
  _vstack[0].vr = stale;
  _vstack[0].type.t = VT_INT;

  int real = ut_push_var(ir, VT_INT); /* vtop[0]: the value to convert */
  vtop->type.t = VT_FLOAT;            /* cast target type, per wrapper convention */

  tcc_ir_gen_cvt_itof(ir);

  UT_ASSERT_EQ(tcc_ir_count(ir), 1);
  IRQuadCompact *q = &ir->compact_instructions[0];
  UT_ASSERT_EQ(q->op, TCCIR_OP_CVT_ITOF);

  /* BUG: src1 is the stale slot below the top, NOT the value at vtop. */
  UT_ASSERT_EQ(irop_get_vreg(tcc_ir_op_get_src1(ir, q)), stale);
  UT_ASSERT_NE(irop_get_vreg(tcc_ir_op_get_src1(ir, q)), real);

  /* ...while the result is written into the top entry as if it had been
   * the source. */
  UT_ASSERT_EQ(vtop, _vstack + 1);
  UT_ASSERT_EQ(vtop[0].vr, irop_get_vreg(tcc_ir_op_get_dest(ir, q)));

  ut_teardown(ir);
  return 0;
}

/* -------------------------------------------------------------------------- */
/* Complex operands: single IR op, soft-call gate bypassed                    */
/* -------------------------------------------------------------------------- */

UT_TEST(test_fadd_complex_float_single_op_skips_soft_gate)
{
  /* Installed with the ZERO-capability FPU config on purpose: tcc_ir_put's
   * soft-call gate skips complex operands outright (put.c:69-70), so a
   * complex FADD still emits a single plain FADD (the code generator lowers
   * it to two __aeabi calls later). */
  ut_setup(&ut_fpu_soft);
  TCCIRState *ir = tcc_ir_alloc();
  int a = ut_push_var(ir, VT_FLOAT | VT_COMPLEX);
  int b = ut_push_var(ir, VT_FLOAT | VT_COMPLEX);

  tcc_ir_gen_fadd(ir);

  UT_ASSERT_EQ(tcc_ir_count(ir), 1);
  IRQuadCompact *q = &ir->compact_instructions[0];
  UT_ASSERT_EQ(q->op, TCCIR_OP_FADD);

  IROperand d = tcc_ir_op_get_dest(ir, q);
  IROperand s1 = tcc_ir_op_get_src1(ir, q);
  IROperand s2 = tcc_ir_op_get_src2(ir, q);
  UT_ASSERT_EQ(irop_get_vreg(s1), a);
  UT_ASSERT_EQ(irop_get_vreg(s2), b);
  UT_ASSERT(s1.is_complex);
  UT_ASSERT(s2.is_complex);
  UT_ASSERT(d.is_complex);

  int dvr = irop_get_vreg(d);
  IRLiveInterval *iv = tcc_ir_vreg_live_interval(ir, dvr);
  UT_ASSERT_EQ(iv->is_complex, 1);
  UT_ASSERT_EQ(iv->is_float, 1);
  UT_ASSERT_EQ(iv->is_double, 0); /* complex float base */

  UT_ASSERT_EQ(vtop, _vstack + 1);
  UT_ASSERT_EQ(vtop->vr, dvr);
  UT_ASSERT_EQ(vtop->r, 0);
  UT_ASSERT_EQ(vtop->type.t, VT_FLOAT | VT_COMPLEX);

  ut_teardown(ir);
  return 0;
}

UT_TEST(test_fmul_complex_double_marks_double)
{
  ut_setup(&ut_fpu_soft); /* gate bypass applies to all four complex arith ops */
  TCCIRState *ir = tcc_ir_alloc();
  int a = ut_push_var(ir, VT_DOUBLE | VT_COMPLEX);
  int b = ut_push_var(ir, VT_DOUBLE | VT_COMPLEX);

  tcc_ir_gen_fmul(ir);

  UT_ASSERT_EQ(tcc_ir_count(ir), 1);
  IRQuadCompact *q = &ir->compact_instructions[0];
  UT_ASSERT_EQ(q->op, TCCIR_OP_FMUL);
  UT_ASSERT_EQ(irop_get_vreg(tcc_ir_op_get_src1(ir, q)), a);
  UT_ASSERT_EQ(irop_get_vreg(tcc_ir_op_get_src2(ir, q)), b);

  IROperand d = tcc_ir_op_get_dest(ir, q);
  UT_ASSERT(d.is_complex);
  IRLiveInterval *iv = tcc_ir_vreg_live_interval(ir, irop_get_vreg(d));
  UT_ASSERT_EQ(iv->is_complex, 1);
  UT_ASSERT_EQ(iv->is_float, 1);
  UT_ASSERT_EQ(iv->is_double, 1); /* complex double base */

  UT_ASSERT_EQ(vtop->type.t, VT_DOUBLE | VT_COMPLEX);

  ut_teardown(ir);
  return 0;
}

/* -------------------------------------------------------------------------- */
/* Soft-float call path (zero-capability FPU config)                          */
/* -------------------------------------------------------------------------- */

UT_TEST(test_soft_fadd_emits_param_param_call_sequence)
{
  ut_setup(&ut_fpu_soft);
  TCCIRState *ir = tcc_ir_alloc();
  int a = ut_push_var(ir, VT_FLOAT);
  int b = ut_push_var(ir, VT_FLOAT);

  tcc_ir_gen_fadd(ir);

  /* tcc_ir_put_soft_call emits FUNCPARAMVAL x2 + FUNCCALLVAL (no FADD). */
  UT_ASSERT_EQ(tcc_ir_count(ir), 3);
  IRQuadCompact *p0 = &ir->compact_instructions[0];
  IRQuadCompact *p1 = &ir->compact_instructions[1];
  IRQuadCompact *call = &ir->compact_instructions[2];
  UT_ASSERT_EQ(p0->op, TCCIR_OP_FUNCPARAMVAL);
  UT_ASSERT_EQ(p1->op, TCCIR_OP_FUNCPARAMVAL);
  UT_ASSERT_EQ(call->op, TCCIR_OP_FUNCCALLVAL);

  /* first call in this function: call_id 0, params tagged 0 and 1 */
  UT_ASSERT_EQ(irop_get_vreg(tcc_ir_op_get_src1(ir, p0)), a);
  UT_ASSERT_EQ(irop_get_imm32(tcc_ir_op_get_src2(ir, p0)), (int32_t)TCCIR_ENCODE_PARAM(0, 0));
  UT_ASSERT_EQ(irop_get_vreg(tcc_ir_op_get_src1(ir, p1)), b);
  UT_ASSERT_EQ(irop_get_imm32(tcc_ir_op_get_src2(ir, p1)), (int32_t)TCCIR_ENCODE_PARAM(0, 1));

  /* callee is a symref; the Sym is NULL because external_global_sym() is a
   * stubs.c stub in this harness (the real __aeabi_fadd name lookup via
   * tcc_get_abi_softcall_name is real here, but its result only feeds the
   * symbol table). src2 encodes call_id 0 / argc 2. */
  IROperand callee = tcc_ir_op_get_src1(ir, call);
  UT_ASSERT_EQ(irop_get_tag(callee), IROP_TAG_SYMREF);
  UT_ASSERT(irop_get_sym_ex(ir, callee) == NULL);
  UT_ASSERT_EQ(irop_get_imm32(tcc_ir_op_get_src2(ir, call)), (int32_t)TCCIR_ENCODE_CALL(0, 2));

  /* dest is the fresh float temp float.c allocated before the put */
  IROperand d = tcc_ir_op_get_dest(ir, call);
  int dvr = irop_get_vreg(d);
  UT_ASSERT_EQ(TCCIR_DECODE_VREG_TYPE(dvr), TCCIR_VREG_TYPE_TEMP);
  UT_ASSERT_EQ(irop_get_btype(d), IROP_BTYPE_FLOAT32);
  IRLiveInterval *iv = tcc_ir_vreg_live_interval(ir, dvr);
  UT_ASSERT_EQ(iv->is_float, 1);

  /* bookkeeping + the same value-stack contract as the native path */
  UT_ASSERT_EQ(ir->next_call_id, 1);
  UT_ASSERT(!tcc_ir_is_leaf(ir));
  UT_ASSERT_EQ(vtop, _vstack + 1);
  UT_ASSERT_EQ(vtop->vr, dvr);
  UT_ASSERT_EQ(vtop->r, 0);

  ut_teardown(ir);
  return 0;
}

UT_TEST(test_soft_fneg_emits_single_param_call)
{
  ut_setup(&ut_fpu_soft);
  TCCIRState *ir = tcc_ir_alloc();
  int a = ut_push_var(ir, VT_FLOAT);

  tcc_ir_gen_fneg(ir);

  UT_ASSERT_EQ(tcc_ir_count(ir), 2);
  IRQuadCompact *p0 = &ir->compact_instructions[0];
  IRQuadCompact *call = &ir->compact_instructions[1];
  UT_ASSERT_EQ(p0->op, TCCIR_OP_FUNCPARAMVAL);
  UT_ASSERT_EQ(call->op, TCCIR_OP_FUNCCALLVAL);
  UT_ASSERT_EQ(irop_get_vreg(tcc_ir_op_get_src1(ir, p0)), a);
  UT_ASSERT_EQ(irop_get_imm32(tcc_ir_op_get_src2(ir, p0)), (int32_t)TCCIR_ENCODE_PARAM(0, 0));
  UT_ASSERT_EQ(irop_get_imm32(tcc_ir_op_get_src2(ir, call)), (int32_t)TCCIR_ENCODE_CALL(0, 1));

  /* unary contract preserved: stack depth unchanged, top holds the result */
  UT_ASSERT_EQ(vtop, _vstack + 1);
  UT_ASSERT_EQ(vtop->vr, irop_get_vreg(tcc_ir_op_get_dest(ir, call)));

  ut_teardown(ir);
  return 0;
}

UT_TEST(test_soft_fcmp_emits_callvoid_and_sets_vt_cmp)
{
  ut_setup(&ut_fpu_soft);
  TCCIRState *ir = tcc_ir_alloc();
  int a = ut_push_var(ir, VT_FLOAT);
  int b = ut_push_var(ir, VT_FLOAT);

  tcc_ir_gen_f(ir, TOK_LT);

  /* FCMP has no dest, so the soft call is a FUNCCALLVOID (the helper sets
   * CPSR flags directly; codegen reads them later). */
  UT_ASSERT_EQ(tcc_ir_count(ir), 3);
  IRQuadCompact *p0 = &ir->compact_instructions[0];
  IRQuadCompact *p1 = &ir->compact_instructions[1];
  IRQuadCompact *call = &ir->compact_instructions[2];
  UT_ASSERT_EQ(p0->op, TCCIR_OP_FUNCPARAMVAL);
  UT_ASSERT_EQ(p1->op, TCCIR_OP_FUNCPARAMVAL);
  UT_ASSERT_EQ(call->op, TCCIR_OP_FUNCCALLVOID);
  UT_ASSERT_EQ(irop_get_vreg(tcc_ir_op_get_src1(ir, p0)), a);
  UT_ASSERT_EQ(irop_get_vreg(tcc_ir_op_get_src1(ir, p1)), b);
  UT_ASSERT_EQ(irop_get_imm32(tcc_ir_op_get_src2(ir, call)), (int32_t)TCCIR_ENCODE_CALL(0, 2));

  /* the pending-comparison value-stack rewrite happens regardless of the
   * native/soft choice */
  UT_ASSERT_EQ(vtop, _vstack + 1);
  UT_ASSERT_EQ(vtop->r, VT_CMP);
  UT_ASSERT_EQ(vtop->cmp_op, TOK_LT);
  UT_ASSERT_EQ(vtop->vr, -1);

  ut_teardown(ir);
  return 0;
}

UT_TEST(test_soft_cvt_itof_emits_param_call)
{
  ut_setup(&ut_fpu_soft);
  TCCIRState *ir = tcc_ir_alloc();
  int src = ut_push_var(ir, VT_INT); /* vtop[-1]: value to convert */
  ut_push_var(ir, VT_FLOAT);         /* vtop[0]: destination type holder */

  tcc_ir_gen_cvt_itof(ir);

  UT_ASSERT_EQ(tcc_ir_count(ir), 2);
  IRQuadCompact *p0 = &ir->compact_instructions[0];
  IRQuadCompact *call = &ir->compact_instructions[1];
  UT_ASSERT_EQ(p0->op, TCCIR_OP_FUNCPARAMVAL);
  UT_ASSERT_EQ(call->op, TCCIR_OP_FUNCCALLVAL);
  UT_ASSERT_EQ(irop_get_vreg(tcc_ir_op_get_src1(ir, p0)), src);
  UT_ASSERT_EQ(irop_get_imm32(tcc_ir_op_get_src2(ir, call)), (int32_t)TCCIR_ENCODE_CALL(0, 1));

  IROperand d = tcc_ir_op_get_dest(ir, call);
  UT_ASSERT_EQ(irop_get_btype(d), IROP_BTYPE_FLOAT32);
  UT_ASSERT_EQ(vtop[0].vr, irop_get_vreg(d));
  UT_ASSERT_EQ(vtop[0].type.t, VT_FLOAT);

  ut_teardown(ir);
  return 0;
}

UT_TEST(test_soft_cvt_ftof_stays_native_via_type_size_stub)
{
  /* Characterization of a HARNESS property, not of float.c itself:
   * ir_put_soft_call_fpu_if_needed's CVT_FTOF case calls type_size() on
   * src/dest and returns "native" when they match -- stubs.c's type_size
   * always returns 4, so in this harness CVT_FTOF NEVER takes the soft-call
   * path, even with a zero-capability FPU config. Pinned so a future reader
   * doesn't expect a call sequence here (see HARNESS NOTES). */
  ut_setup(&ut_fpu_soft);
  TCCIRState *ir = tcc_ir_alloc();
  int src = ut_push_var(ir, VT_FLOAT);
  ut_push_var(ir, VT_DOUBLE);

  tcc_ir_gen_cvt_ftof(ir);

  UT_ASSERT_EQ(tcc_ir_count(ir), 1);
  IRQuadCompact *q = &ir->compact_instructions[0];
  UT_ASSERT_EQ(q->op, TCCIR_OP_CVT_FTOF); /* NOT a FUNCPARAMVAL/FUNCCALLVAL pair */
  UT_ASSERT_EQ(irop_get_vreg(tcc_ir_op_get_src1(ir, q)), src);

  ut_teardown(ir);
  return 0;
}
