/*
 *  test_gen_softfloat.c - suite for ir/gen/softfloat.c
 *
 *  Covers:
 *    - tcc_ir_put_soft_call(): lowering of an FP IR op to an __aeabi_* library
 *      call — one FUNCPARAMVAL per operand (call_id/param_idx packed in src2),
 *      then FUNCCALLVAL (valued result) or FUNCCALLVOID (FCMP, which has no
 *      dest in irop_config) referencing the callee Sym resolved from the
 *      tcc_get_abi_softcall_name() lookup.  Also the "no ABI function" guard.
 *    - ir_put_soft_call_fpu_if_needed(): the hardware-FPU gate.  Returns 0 and
 *      emits nothing when architecture_config.fpu advertises the op for the
 *      operand width (per-op has_fX/has_dX bits), for non-FP ops, and for
 *      same-size CVT_FTOF; otherwise delegates to tcc_ir_put_soft_call() and
 *      returns 1.
 *
 *  HARNESS NOTES:
 *    - ir/gen/softfloat.c is already linked into this binary (IR_GEN_FILES),
 *      but its extern dependencies are stubbed in ways that block the paths
 *      under test: tcc_get_abi_softcall_name() always returns NULL
 *      (stubs_gen_machine_fallback.c), tcc_is_64bit_operand() always returns 0
 *      and external_global_sym() returns NULL (stubs.c), and _tcc_error()
 *      aborts (stubs.c).  Per the include-the-.c pattern (guide section 7,
 *      same as test_tccasm.c), this file #includes ir/gen/softfloat.c with
 *      its two exports and those dependencies macro-renamed to local fakes,
 *      module's logic runs against faithful oracles:
 *        - ut_soft_get_abi_softcall_name() mirrors the REAL name table in
 *          source/backend/arch/arm/thumb/arm-thumb-gen.c:13258-13396 and
 *          records its arguments (it is non-static because softfloat.c:15
 *          re-declares it extern under the renamed macro);
 *        - ut_soft_is_64bit_operand() mirrors tcc.c:585
 *          (VT_LLONG/VT_DOUBLE/VT_LDOUBLE => 64-bit);
 *        - ut_soft_type_size() maps FLOAT=4, DOUBLE/LDOUBLE/LLONG=8, else 4
 *          (matches tcc_get_type_size, arm-thumb-gen.c:13230);
 *        - ut_soft_external_global_sym() returns a recording fake Sym;
 *        - ut_soft_error_recorder() replaces the aborting _tcc_error stub so
 *          the "no ABI function" guard path can run and be asserted.
 *      The REAL tcc_get_abi_softcall_name() itself lives in arm-thumb-gen.c
 *      (backend binary only); its own name-table internals are out of scope
 *      for this suite.
 *    - architecture_config is the REAL global (arm.c is linked); each gate
 *      test points architecture_config.fpu at a local FloatingPointConfig and
 *      restores the previous value afterwards (fpu_install/fpu_restore).
 *    - The gate consults architecture_config.fpu ONLY.  tcc_state->float_abi
 *      (ARM_SOFT_FLOAT/ARM_SOFTFP_FLOAT/ARM_HARD_FLOAT) selects the calling
 *      convention downstream (VFP register allocation, ir/vreg.c:289 use_vfp);
 *      it does not influence ir_put_soft_call_fpu_if_needed's decision.
 *      test_fpu_gate_ignores_float_abi pins this.
 *    - The `ir ? ir->next_call_id++ : 0` guard in tcc_ir_put_soft_call is
 *      cosmetic: with ir == NULL and a mappable op the function would still
 *      dereference ir inside tcc_ir_put.  No production caller passes NULL
 *      (the module is only called from tcc_ir_put), so the NULL-ir path is
 *      deliberately not tested.
 */

#define USING_GLOBALS
#include "ir.h"

#include "ut.h"

/* -------------------------------------------------------- renamed fakes */
/* These #defines must be in effect while ir/gen/softfloat.c is compiled into
 * this TU below; they keep its symbols from clashing with the linked
 * softfloat.o and bind its extern calls to the fakes here. */

static int ut_soft_error_count;
static char ut_soft_error_msg[256];

static void ut_soft_error_recorder(const char *fmt, ...)
{
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(ut_soft_error_msg, sizeof(ut_soft_error_msg), fmt, ap);
  va_end(ap);
  ut_soft_error_count++;
}

/* Mirrors tcc.c:585 tcc_is_64bit_operand(). */
static int ut_soft_is_64bit_operand(SValue *sv)
{
  int vt;
  if (sv == NULL)
    return 0;
  vt = sv->type.t & VT_BTYPE;
  return (vt == VT_LLONG || vt == VT_DOUBLE || vt == VT_LDOUBLE) ? 1 : 0;
}

/* Mirrors tcc_get_type_size() (arm-thumb-gen.c:13230) for the types used here. */
static int ut_soft_type_size(CType *type, int *a)
{
  int vt = type->t & VT_BTYPE;
  if (a)
    *a = 4;
  switch (vt)
  {
  case VT_FLOAT:
    return 4;
  case VT_DOUBLE:
  case VT_LDOUBLE:
  case VT_LLONG:
    return 8;
  default:
    return 4;
  }
}

static int ut_soft_name_calls;
static TccIrOp ut_soft_name_op;
static SValue *ut_soft_name_src1;
static SValue *ut_soft_name_src2;
static SValue *ut_soft_name_dest;

/* Mirrors the REAL tcc_get_abi_softcall_name() name table
 * (arm-thumb-gen.c:13258-13396) and records the arguments it was called with. */
const char *ut_soft_get_abi_softcall_name(SValue *src1, SValue *src2, SValue *dest, TccIrOp op)
{
  const int src1_64bit = ut_soft_is_64bit_operand(src1);
  const int src2_64bit = src2 ? ut_soft_is_64bit_operand(src2) : 0;
  const int dest_64bit = dest ? ut_soft_is_64bit_operand(dest) : 0;
  const int src1_size = src1 ? ut_soft_type_size(&src1->type, NULL) : 0;
  const int dest_size = dest ? ut_soft_type_size(&dest->type, NULL) : 0;

  ut_soft_name_calls++;
  ut_soft_name_op = op;
  ut_soft_name_src1 = src1;
  ut_soft_name_src2 = src2;
  ut_soft_name_dest = dest;

  if (src1_64bit || src2_64bit || dest_64bit)
  {
    switch (op)
    {
    case TCCIR_OP_FADD:
      return "__aeabi_dadd";
    case TCCIR_OP_FSUB:
      return "__aeabi_dsub";
    case TCCIR_OP_FMUL:
      return "__aeabi_dmul";
    case TCCIR_OP_FDIV:
      return "__aeabi_ddiv";
    case TCCIR_OP_FNEG:
      return "__aeabi_dneg";
    default:
      break;
    }
  }
  else
  {
    switch (op)
    {
    case TCCIR_OP_FADD:
      return "__aeabi_fadd";
    case TCCIR_OP_FSUB:
      return "__aeabi_fsub";
    case TCCIR_OP_FMUL:
      return "__aeabi_fmul";
    case TCCIR_OP_FDIV:
      return "__aeabi_fdiv";
    case TCCIR_OP_FNEG:
      return "__aeabi_fneg";
    default:
      break;
    }
  }

  switch (op)
  {
  case TCCIR_OP_CVT_FTOF:
    if (src1_size == 4 && dest_size == 8)
      return "__aeabi_f2d";
    if (src1_size == 8 && dest_size == 4)
      return "__aeabi_d2f";
    return NULL; /* same-size conversion is a no-op */
  case TCCIR_OP_CVT_FTOI:
  {
    const int is_float = (src1_size == 4);
    const int is_unsigned = (dest && (dest->type.t & VT_UNSIGNED)) ? 1 : 0;
    if (dest_size == 8)
      return is_unsigned ? (is_float ? "__aeabi_f2ulz" : "__aeabi_d2ulz")
                         : (is_float ? "__aeabi_f2lz" : "__aeabi_d2lz");
    return is_unsigned ? (is_float ? "__aeabi_f2uiz" : "__aeabi_d2uiz") : (is_float ? "__aeabi_f2iz" : "__aeabi_d2iz");
  }
  case TCCIR_OP_FCMP:
  {
    const int cmp_op = src2 ? (int)src2->c.i : 0;
    const int is_float = (src1_size == 4);
    switch (cmp_op)
    {
    case TOK_EQ:
    case TOK_NE:
      return is_float ? "__aeabi_fcmpeq" : "__aeabi_dcmpeq";
    case TOK_LT:
    case TOK_ULT:
      return is_float ? "__aeabi_fcmplt" : "__aeabi_dcmplt";
    case TOK_LE:
    case TOK_ULE:
      return is_float ? "__aeabi_fcmple" : "__aeabi_dcmple";
    case TOK_GT:
    case TOK_UGT:
      return is_float ? "__aeabi_fcmpgt" : "__aeabi_dcmpgt";
    case TOK_GE:
    case TOK_UGE:
      return is_float ? "__aeabi_fcmpge" : "__aeabi_dcmpge";
    default:
      return is_float ? "__aeabi_cfcmple" : "__aeabi_cdcmple";
    }
  }
  case TCCIR_OP_CVT_ITOF:
  {
    const int is_unsigned = (src1 && (src1->type.t & VT_UNSIGNED)) ? 1 : 0;
    if (src1_size == 8)
      return is_unsigned ? (dest_64bit ? "__aeabi_ul2d" : "__aeabi_ul2f")
                         : (dest_64bit ? "__aeabi_l2d" : "__aeabi_l2f");
    return is_unsigned ? (dest_64bit ? "__aeabi_ui2d" : "__aeabi_ui2f")
                       : (dest_64bit ? "__aeabi_i2d" : "__aeabi_i2f");
  }
  default:
    break;
  }
  return NULL;
}

#define UT_SOFT_SYM_RING 8
static Sym ut_soft_syms[UT_SOFT_SYM_RING];
static int ut_soft_egs_count;
static Sym *ut_soft_egs_last_sym;
static CType *ut_soft_egs_last_type;

static Sym *ut_soft_external_global_sym(int v, CType *type)
{
  Sym *s = &ut_soft_syms[ut_soft_egs_count % UT_SOFT_SYM_RING];
  memset(s, 0, sizeof(*s));
  s->v = v;
  s->c = 1; /* not an anonymous c==0 sym: skips put_extern_sym2 in ir_ensure_sym_registered */
  ut_soft_egs_count++;
  ut_soft_egs_last_sym = s;
  ut_soft_egs_last_type = type;
  return s;
}

#define tcc_ir_put_soft_call ut_soft_tcc_ir_put_soft_call
#define ir_put_soft_call_fpu_if_needed ut_soft_ir_put_soft_call_fpu_if_needed
#define tcc_get_abi_softcall_name ut_soft_get_abi_softcall_name
#define tcc_is_64bit_operand ut_soft_is_64bit_operand
#define type_size ut_soft_type_size
#define external_global_sym ut_soft_external_global_sym
#define _tcc_error ut_soft_error_recorder
#include "source/ir/gen/softfloat.c"
#undef _tcc_error
#undef external_global_sym
#undef type_size
#undef tcc_is_64bit_operand
#undef tcc_get_abi_softcall_name
#undef ir_put_soft_call_fpu_if_needed
#undef tcc_ir_put_soft_call
#undef USING_GLOBALS /* softfloat.c re-#defined it; keep the rest of this file clean */

/* -------------------------------------------------------- fixtures */

#define UT_CHECK(call)                                                                                                 \
  do                                                                                                                   \
  {                                                                                                                    \
    if ((call) != 0)                                                                                                   \
      return -1;                                                                                                       \
  } while (0)

static void ut_soft_reset(void)
{
  ut_soft_error_count = 0;
  ut_soft_error_msg[0] = '\0';
  ut_soft_name_calls = 0;
  ut_soft_name_op = 0;
  ut_soft_name_src1 = NULL;
  ut_soft_name_src2 = NULL;
  ut_soft_name_dest = NULL;
  ut_soft_egs_count = 0;
  ut_soft_egs_last_sym = NULL;
  ut_soft_egs_last_type = NULL;
}

/* Install a zeroed ("arm_soft_fpu_config"-equivalent, arm-thumb-gen.c:2332)
 * FloatingPointConfig as architecture_config.fpu; fpu_restore() puts the
 * previous value back.  Mutate ut_soft_fpu_cfg directly to model real FPUs. */
static FloatingPointConfig ut_soft_fpu_cfg;
static const FloatingPointConfig *ut_soft_fpu_saved;

static void fpu_install(void)
{
  ut_soft_fpu_saved = architecture_config.fpu;
  memset(&ut_soft_fpu_cfg, 0, sizeof(ut_soft_fpu_cfg));
  architecture_config.fpu = &ut_soft_fpu_cfg;
}

static void fpu_restore(void)
{
  architecture_config.fpu = ut_soft_fpu_saved;
}

/* Set all single-precision capability bits (fpv5-sp-d16.c shape). */
static void fpu_cfg_set_sp(void)
{
  ut_soft_fpu_cfg.has_fadd = 1;
  ut_soft_fpu_cfg.has_fsub = 1;
  ut_soft_fpu_cfg.has_fmul = 1;
  ut_soft_fpu_cfg.has_fdiv = 1;
  ut_soft_fpu_cfg.has_fcmp = 1;
  ut_soft_fpu_cfg.has_itof = 1;
  ut_soft_fpu_cfg.has_ftoi = 1;
  ut_soft_fpu_cfg.has_fneg = 1;
}

static void fpu_set_sp_bit(TccIrOp op)
{
  switch (op)
  {
  case TCCIR_OP_FADD:
    ut_soft_fpu_cfg.has_fadd = 1;
    break;
  case TCCIR_OP_FSUB:
    ut_soft_fpu_cfg.has_fsub = 1;
    break;
  case TCCIR_OP_FMUL:
    ut_soft_fpu_cfg.has_fmul = 1;
    break;
  case TCCIR_OP_FDIV:
    ut_soft_fpu_cfg.has_fdiv = 1;
    break;
  case TCCIR_OP_FNEG:
    ut_soft_fpu_cfg.has_fneg = 1;
    break;
  case TCCIR_OP_FCMP:
    ut_soft_fpu_cfg.has_fcmp = 1;
    break;
  case TCCIR_OP_CVT_ITOF:
    ut_soft_fpu_cfg.has_itof = 1;
    break;
  case TCCIR_OP_CVT_FTOI:
    ut_soft_fpu_cfg.has_ftoi = 1;
    break;
  default:
    break;
  }
}

static void fpu_set_dp_bit(TccIrOp op)
{
  switch (op)
  {
  case TCCIR_OP_FADD:
    ut_soft_fpu_cfg.has_dadd = 1;
    break;
  case TCCIR_OP_FSUB:
    ut_soft_fpu_cfg.has_dsub = 1;
    break;
  case TCCIR_OP_FMUL:
    ut_soft_fpu_cfg.has_dmul = 1;
    break;
  case TCCIR_OP_FDIV:
    ut_soft_fpu_cfg.has_ddiv = 1;
    break;
  case TCCIR_OP_FNEG:
    ut_soft_fpu_cfg.has_dneg = 1;
    break;
  case TCCIR_OP_FCMP:
    ut_soft_fpu_cfg.has_dcmp = 1;
    break;
  case TCCIR_OP_CVT_ITOF:
    ut_soft_fpu_cfg.has_itod = 1;
    break;
  case TCCIR_OP_CVT_FTOI:
    ut_soft_fpu_cfg.has_dtoi = 1;
    break;
  default:
    break;
  }
}

static SValue sv_typed(int vreg, int t)
{
  SValue sv;
  svalue_init(&sv);
  sv.vr = vreg;
  sv.type.t = t;
  return sv;
}

/* Operands that put the gate on the 32-bit (is64bit == 0) path: all VT_FLOAT
 * except CVT_ITOF (int source) and CVT_FTOI (int dest).  FCMP's src2 carries
 * the comparison token like production (arm-thumb-gen.c:13342). */
static void make_ops_32(TCCIRState *ir, TccIrOp op, SValue *s1, SValue *s2, SValue *d)
{
  *s1 = sv_typed(tcc_ir_vreg_alloc_temp(ir), VT_FLOAT);
  *s2 = sv_typed(tcc_ir_vreg_alloc_temp(ir), VT_FLOAT);
  *d = sv_typed(tcc_ir_vreg_alloc_temp(ir), VT_FLOAT);
  if (op == TCCIR_OP_CVT_ITOF)
    s1->type.t = VT_INT;
  if (op == TCCIR_OP_CVT_FTOI)
    d->type.t = VT_INT;
  if (op == TCCIR_OP_FCMP)
    s2->c.i = TOK_EQ;
}

/* Operands for the 64-bit (is64bit == 1) path: all VT_DOUBLE, except
 * CVT_ITOF (long long source) and CVT_FTOI (long long dest). */
static void make_ops_64(TCCIRState *ir, TccIrOp op, SValue *s1, SValue *s2, SValue *d)
{
  *s1 = sv_typed(tcc_ir_vreg_alloc_temp(ir), VT_DOUBLE);
  *s2 = sv_typed(tcc_ir_vreg_alloc_temp(ir), VT_DOUBLE);
  *d = sv_typed(tcc_ir_vreg_alloc_temp(ir), VT_DOUBLE);
  if (op == TCCIR_OP_CVT_ITOF)
    s1->type.t = VT_LLONG;
  if (op == TCCIR_OP_CVT_FTOI)
    d->type.t = VT_LLONG;
  if (op == TCCIR_OP_FCMP)
    s2->c.i = TOK_EQ;
}

/* Assert instruction i is FUNCPARAMVAL passing vreg `vr` as parameter `pidx`
 * of call `cid`. */
static int assert_param(TCCIRState *ir, int i, int vr, int cid, int pidx)
{
  IRQuadCompact *q = &ir->compact_instructions[i];
  UT_ASSERT_EQ(q->op, TCCIR_OP_FUNCPARAMVAL);

  IROperand s1 = tcc_ir_op_get_src1(ir, q);
  UT_ASSERT_EQ(irop_get_tag(s1), IROP_TAG_VREG);
  UT_ASSERT_EQ(irop_get_vreg(s1), vr);

  IROperand s2 = tcc_ir_op_get_src2(ir, q);
  UT_ASSERT_EQ(irop_get_tag(s2), IROP_TAG_IMM32);
  int32_t enc = irop_get_imm32(s2);
  UT_ASSERT_EQ(TCCIR_DECODE_CALL_ID(enc), cid);
  UT_ASSERT_EQ(TCCIR_DECODE_PARAM_IDX(enc), pidx);
  return 0;
}

/* Shared tail for FUNCCALLVAL/FUNCCALLVOID: src1 is a SYMREF to a callee
 * named `name`, src2 packs call_id/argc. */
static int assert_call_common(TCCIRState *ir, int i, int cid, int argc, const char *name)
{
  IRQuadCompact *q = &ir->compact_instructions[i];

  IROperand s1 = tcc_ir_op_get_src1(ir, q);
  UT_ASSERT_EQ(irop_get_tag(s1), IROP_TAG_SYMREF);
  IRPoolSymref *sr = irop_get_symref_ex(ir, s1);
  UT_ASSERT(sr != NULL);
  UT_ASSERT(sr->sym != NULL);
  UT_ASSERT_EQ(sr->addend, 0);
  UT_ASSERT_STREQ(get_tok_str(sr->sym->v, NULL), name);

  IROperand s2 = tcc_ir_op_get_src2(ir, q);
  UT_ASSERT_EQ(irop_get_tag(s2), IROP_TAG_IMM32);
  int32_t enc = irop_get_imm32(s2);
  UT_ASSERT_EQ(TCCIR_DECODE_CALL_ID(enc), cid);
  UT_ASSERT_EQ(TCCIR_DECODE_CALL_ARGC(enc), argc);
  return 0;
}

static int assert_callval(TCCIRState *ir, int i, int dest_vr, int cid, int argc, const char *name)
{
  IRQuadCompact *q = &ir->compact_instructions[i];
  UT_ASSERT_EQ(q->op, TCCIR_OP_FUNCCALLVAL);

  IROperand d = tcc_ir_op_get_dest(ir, q);
  UT_ASSERT_EQ(irop_get_tag(d), IROP_TAG_VREG);
  UT_ASSERT_EQ(irop_get_vreg(d), dest_vr);

  UT_CHECK(assert_call_common(ir, i, cid, argc, name));
  return 0;
}

static int assert_callvoid(TCCIRState *ir, int i, int cid, int argc, const char *name)
{
  IRQuadCompact *q = &ir->compact_instructions[i];
  UT_ASSERT_EQ(q->op, TCCIR_OP_FUNCCALLVOID);
  UT_CHECK(assert_call_common(ir, i, cid, argc, name));
  return 0;
}

/* Safety net: never leak our fpu/float_abi fixture state into sibling suites,
 * even when a test fails midway (per-test code restores on the happy path). */
static void gen_softfloat_teardown(void)
{
  architecture_config.fpu = NULL;
  tcc_state->float_abi = ARM_SOFT_FLOAT;
}
UT_SUITE_TEARDOWN(gen_softfloat_teardown);

/* -------------------------------------------------------- tests */
/* tcc_ir_put_soft_call (direct entry point) */

UT_TEST(test_soft_call_fadd_f32_emits_params_and_callval)
{
  ut_soft_reset();
  TCCIRState *ir = tcc_ir_alloc();
  ir->next_call_id = 7; /* make the call_id visible in every encoding */

  int a = tcc_ir_vreg_alloc_temp(ir);
  int b = tcc_ir_vreg_alloc_temp(ir);
  int c = tcc_ir_vreg_alloc_temp(ir);
  SValue s1 = sv_typed(a, VT_FLOAT);
  SValue s2 = sv_typed(b, VT_FLOAT);
  SValue d = sv_typed(c, VT_FLOAT);

  ut_soft_tcc_ir_put_soft_call(ir, TCCIR_OP_FADD, &s1, &s2, &d);

  /* The name lookup saw the exact operands/op passed in. */
  UT_ASSERT_EQ(ut_soft_name_calls, 1);
  UT_ASSERT_EQ(ut_soft_name_op, TCCIR_OP_FADD);
  UT_ASSERT(ut_soft_name_src1 == &s1);
  UT_ASSERT(ut_soft_name_src2 == &s2);
  UT_ASSERT(ut_soft_name_dest == &d);
  UT_ASSERT_EQ(ut_soft_error_count, 0);

  /* The callee sym came from external_global_sym(tok, &func_old_type). */
  UT_ASSERT_EQ(ut_soft_egs_count, 1);
  UT_ASSERT(ut_soft_egs_last_type == &func_old_type);

  /* FUNCPARAMVAL src1, FUNCPARAMVAL src2, FUNCCALLVAL — in order. */
  UT_ASSERT_EQ(tcc_ir_count(ir), 3);
  UT_CHECK(assert_param(ir, 0, a, 7, 0));
  UT_CHECK(assert_param(ir, 1, b, 7, 1));
  UT_CHECK(assert_callval(ir, 2, c, 7, 2, "__aeabi_fadd"));

  /* 32-bit float operands keep their btype through the params/result. */
  IROperand p0 = tcc_ir_op_get_src1(ir, &ir->compact_instructions[0]);
  UT_ASSERT_EQ(irop_get_btype(p0), IROP_BTYPE_FLOAT32);
  IROperand rd = tcc_ir_op_get_dest(ir, &ir->compact_instructions[2]);
  UT_ASSERT_EQ(irop_get_btype(rd), IROP_BTYPE_FLOAT32);

  /* The call_id counter advanced exactly once. */
  UT_ASSERT_EQ(ir->next_call_id, 8);

  tcc_ir_free(ir);
  return 0;
}

UT_TEST(test_soft_call_fadd_f64_uses_dadd)
{
  ut_soft_reset();
  TCCIRState *ir = tcc_ir_alloc();
  ir->next_call_id = 3;

  int a = tcc_ir_vreg_alloc_temp(ir);
  int b = tcc_ir_vreg_alloc_temp(ir);
  int c = tcc_ir_vreg_alloc_temp(ir);
  SValue s1 = sv_typed(a, VT_DOUBLE);
  SValue s2 = sv_typed(b, VT_DOUBLE);
  SValue d = sv_typed(c, VT_DOUBLE);

  ut_soft_tcc_ir_put_soft_call(ir, TCCIR_OP_FADD, &s1, &s2, &d);

  UT_ASSERT_EQ(tcc_ir_count(ir), 3);
  UT_CHECK(assert_param(ir, 0, a, 3, 0));
  UT_CHECK(assert_param(ir, 1, b, 3, 1));
  UT_CHECK(assert_callval(ir, 2, c, 3, 2, "__aeabi_dadd"));

  IROperand p0 = tcc_ir_op_get_src1(ir, &ir->compact_instructions[0]);
  UT_ASSERT_EQ(irop_get_btype(p0), IROP_BTYPE_FLOAT64);

  tcc_ir_free(ir);
  return 0;
}

UT_TEST(test_soft_call_fneg_emits_single_param)
{
  ut_soft_reset();
  TCCIRState *ir = tcc_ir_alloc();

  int a = tcc_ir_vreg_alloc_temp(ir);
  int c = tcc_ir_vreg_alloc_temp(ir);
  SValue s1 = sv_typed(a, VT_FLOAT);
  SValue d = sv_typed(c, VT_FLOAT);

  /* FNEG is unary (irop_config has_src2 == 0): exactly one FUNCPARAMVAL and
   * a FUNCCALLVAL with argc == 1. */
  ut_soft_tcc_ir_put_soft_call(ir, TCCIR_OP_FNEG, &s1, NULL, &d);

  UT_ASSERT_EQ(tcc_ir_count(ir), 2);
  UT_CHECK(assert_param(ir, 0, a, 0, 0));
  UT_CHECK(assert_callval(ir, 1, c, 0, 1, "__aeabi_fneg"));

  tcc_ir_free(ir);
  return 0;
}

UT_TEST(test_soft_call_fcmp_emits_funcallvoid)
{
  ut_soft_reset();
  TCCIRState *ir = tcc_ir_alloc();

  int a = tcc_ir_vreg_alloc_temp(ir);
  int b = tcc_ir_vreg_alloc_temp(ir);
  SValue s1 = sv_typed(a, VT_FLOAT);
  SValue s2 = sv_typed(b, VT_FLOAT);
  s2.c.i = TOK_EQ; /* comparison selector, consumed by the name lookup */

  /* FCMP has no dest in irop_config, so the module emits FUNCCALLVOID. */
  ut_soft_tcc_ir_put_soft_call(ir, TCCIR_OP_FCMP, &s1, &s2, NULL);

  UT_ASSERT_EQ(tcc_ir_count(ir), 3);
  UT_CHECK(assert_param(ir, 0, a, 0, 0));
  UT_CHECK(assert_param(ir, 1, b, 0, 1));
  UT_CHECK(assert_callvoid(ir, 2, 0, 2, "__aeabi_fcmpeq"));

  tcc_ir_free(ir);
  return 0;
}

UT_TEST(test_soft_call_unmapped_op_reports_error_emits_nothing)
{
  ut_soft_reset();
  TCCIRState *ir = tcc_ir_alloc();

  int a = tcc_ir_vreg_alloc_temp(ir);
  int b = tcc_ir_vreg_alloc_temp(ir);
  int c = tcc_ir_vreg_alloc_temp(ir);
  SValue s1 = sv_typed(a, VT_INT);
  SValue s2 = sv_typed(b, VT_INT);
  SValue d = sv_typed(c, VT_INT);

  /* TCCIR_OP_ADD has no soft-float ABI function: the module reports the
   * error and returns before emitting any IR or resolving a callee. */
  ut_soft_tcc_ir_put_soft_call(ir, TCCIR_OP_ADD, &s1, &s2, &d);

  UT_ASSERT_EQ(ut_soft_name_calls, 1);
  UT_ASSERT_EQ(ut_soft_name_op, TCCIR_OP_ADD);
  UT_ASSERT_EQ(ut_soft_error_count, 1);
  UT_ASSERT(strstr(ut_soft_error_msg, "No soft-float ABI function") != NULL);
  UT_ASSERT_EQ(tcc_ir_count(ir), 0);
  UT_ASSERT_EQ(ut_soft_egs_count, 0);
  /* The call_id is consumed before the lookup, so a failed soft call still
   * burns one id (observable ordering, pinned — ids only need uniqueness). */
  UT_ASSERT_EQ(ir->next_call_id, 1);

  tcc_ir_free(ir);
  return 0;
}

/* -------------------------------------------------------- tests */
/* ir_put_soft_call_fpu_if_needed (hardware-FPU gate) */

UT_TEST(test_fpu_gate_non_fp_op_returns_0)
{
  ut_soft_reset();
  TCCIRState *ir = tcc_ir_alloc();

  SValue s1 = sv_typed(tcc_ir_vreg_alloc_temp(ir), VT_INT);
  SValue s2 = sv_typed(tcc_ir_vreg_alloc_temp(ir), VT_INT);
  SValue d = sv_typed(tcc_ir_vreg_alloc_temp(ir), VT_INT);

  /* default: case — returns 0 without ever dereferencing
   * architecture_config.fpu, so fpu == NULL is safe here. */
  const FloatingPointConfig *saved = architecture_config.fpu;
  architecture_config.fpu = NULL;

  int rc = ut_soft_ir_put_soft_call_fpu_if_needed(ir, TCCIR_OP_ADD, &s1, &s2, &d);

  UT_ASSERT_EQ(rc, 0);
  UT_ASSERT_EQ(tcc_ir_count(ir), 0);
  UT_ASSERT_EQ(ut_soft_name_calls, 0);
  UT_ASSERT_EQ(ut_soft_error_count, 0);

  architecture_config.fpu = saved;
  tcc_ir_free(ir);
  return 0;
}

UT_TEST(test_fpu_gate_sp_ops_with_hw_bit_return_0)
{
  static const TccIrOp ops[] = {
      TCCIR_OP_FADD, TCCIR_OP_FSUB, TCCIR_OP_FMUL, TCCIR_OP_FDIV,
      TCCIR_OP_FNEG, TCCIR_OP_FCMP, TCCIR_OP_CVT_ITOF, TCCIR_OP_CVT_FTOI,
  };

  for (size_t k = 0; k < sizeof(ops) / sizeof(ops[0]); k++)
  {
    ut_soft_reset();
    fpu_install();
    fpu_set_sp_bit(ops[k]); /* exactly the bit this op consults */

    TCCIRState *ir = tcc_ir_alloc();
    SValue s1, s2, d;
    make_ops_32(ir, ops[k], &s1, &s2, &d);

    int rc = ut_soft_ir_put_soft_call_fpu_if_needed(ir, ops[k], &s1, &s2, &d);

    UT_ASSERT_EQ(rc, 0);
    UT_ASSERT_EQ(tcc_ir_count(ir), 0);
    UT_ASSERT_EQ(ut_soft_name_calls, 0);

    tcc_ir_free(ir);
    fpu_restore();
  }
  return 0;
}

UT_TEST(test_fpu_gate_dp_ops_with_hw_bit_return_0)
{
  static const TccIrOp ops[] = {
      TCCIR_OP_FADD, TCCIR_OP_FSUB, TCCIR_OP_FMUL, TCCIR_OP_FDIV,
      TCCIR_OP_FNEG, TCCIR_OP_FCMP, TCCIR_OP_CVT_ITOF, TCCIR_OP_CVT_FTOI,
  };

  for (size_t k = 0; k < sizeof(ops) / sizeof(ops[0]); k++)
  {
    ut_soft_reset();
    fpu_install();
    fpu_set_dp_bit(ops[k]); /* exactly the 64-bit bit this op consults */

    TCCIRState *ir = tcc_ir_alloc();
    SValue s1, s2, d;
    make_ops_64(ir, ops[k], &s1, &s2, &d);

    int rc = ut_soft_ir_put_soft_call_fpu_if_needed(ir, ops[k], &s1, &s2, &d);

    UT_ASSERT_EQ(rc, 0);
    UT_ASSERT_EQ(tcc_ir_count(ir), 0);
    UT_ASSERT_EQ(ut_soft_name_calls, 0);

    tcc_ir_free(ir);
    fpu_restore();
  }
  return 0;
}

UT_TEST(test_fpu_gate_soft_config_emits_soft_call_returns_1)
{
  ut_soft_reset();
  fpu_install(); /* all-zero config: no hardware FP at all */

  TCCIRState *ir = tcc_ir_alloc();
  int a = tcc_ir_vreg_alloc_temp(ir);
  int b = tcc_ir_vreg_alloc_temp(ir);
  int c = tcc_ir_vreg_alloc_temp(ir);
  SValue s1 = sv_typed(a, VT_FLOAT);
  SValue s2 = sv_typed(b, VT_FLOAT);
  SValue d = sv_typed(c, VT_FLOAT);

  int rc = ut_soft_ir_put_soft_call_fpu_if_needed(ir, TCCIR_OP_FADD, &s1, &s2, &d);

  /* Delegated to tcc_ir_put_soft_call and reported 1. */
  UT_ASSERT_EQ(rc, 1);
  UT_ASSERT_EQ(tcc_ir_count(ir), 3);
  UT_CHECK(assert_param(ir, 0, a, 0, 0));
  UT_CHECK(assert_param(ir, 1, b, 0, 1));
  UT_CHECK(assert_callval(ir, 2, c, 0, 2, "__aeabi_fadd"));

  tcc_ir_free(ir);
  fpu_restore();
  return 0;
}

UT_TEST(test_fpu_gate_sp_only_fpu_double_falls_back_to_soft)
{
  /* fpv5-sp-d16 shape (single-precision hardware only): 32-bit FADD uses the
   * FPU, 64-bit FADD falls back to __aeabi_dadd. */
  ut_soft_reset();
  fpu_install();
  fpu_cfg_set_sp();

  TCCIRState *ir = tcc_ir_alloc();
  SValue s1, s2, d;
  make_ops_32(ir, TCCIR_OP_FADD, &s1, &s2, &d);

  int rc32 = ut_soft_ir_put_soft_call_fpu_if_needed(ir, TCCIR_OP_FADD, &s1, &s2, &d);
  UT_ASSERT_EQ(rc32, 0);
  UT_ASSERT_EQ(tcc_ir_count(ir), 0);
  tcc_ir_free(ir);

  ut_soft_reset();
  ir = tcc_ir_alloc();
  int a = tcc_ir_vreg_alloc_temp(ir);
  int b = tcc_ir_vreg_alloc_temp(ir);
  int c = tcc_ir_vreg_alloc_temp(ir);
  s1 = sv_typed(a, VT_DOUBLE);
  s2 = sv_typed(b, VT_DOUBLE);
  d = sv_typed(c, VT_DOUBLE);

  int rc64 = ut_soft_ir_put_soft_call_fpu_if_needed(ir, TCCIR_OP_FADD, &s1, &s2, &d);
  UT_ASSERT_EQ(rc64, 1);
  UT_ASSERT_EQ(tcc_ir_count(ir), 3);
  UT_CHECK(assert_callval(ir, 2, c, 0, 2, "__aeabi_dadd"));

  tcc_ir_free(ir);
  fpu_restore();
  return 0;
}

UT_TEST(test_fpu_gate_cvt_ftof_same_size_is_noop)
{
  ut_soft_reset();
  TCCIRState *ir = tcc_ir_alloc();

  SValue f1 = sv_typed(tcc_ir_vreg_alloc_temp(ir), VT_FLOAT);
  SValue f2 = sv_typed(tcc_ir_vreg_alloc_temp(ir), VT_FLOAT);
  SValue d1 = sv_typed(tcc_ir_vreg_alloc_temp(ir), VT_DOUBLE);
  SValue d2 = sv_typed(tcc_ir_vreg_alloc_temp(ir), VT_DOUBLE);

  /* Same-size CVT_FTOF is a copy, not a call: returns 0 before consulting
   * the FPU config, so fpu == NULL must be safe. */
  const FloatingPointConfig *saved = architecture_config.fpu;
  architecture_config.fpu = NULL;

  UT_ASSERT_EQ(ut_soft_ir_put_soft_call_fpu_if_needed(ir, TCCIR_OP_CVT_FTOF, &f1, NULL, &f2), 0);
  UT_ASSERT_EQ(ut_soft_ir_put_soft_call_fpu_if_needed(ir, TCCIR_OP_CVT_FTOF, &d1, NULL, &d2), 0);

  UT_ASSERT_EQ(tcc_ir_count(ir), 0);
  UT_ASSERT_EQ(ut_soft_name_calls, 0);
  UT_ASSERT_EQ(ut_soft_error_count, 0);

  architecture_config.fpu = saved;
  tcc_ir_free(ir);
  return 0;
}

UT_TEST(test_fpu_gate_cvt_ftof_f2d_soft_emits_f2d)
{
  ut_soft_reset();
  fpu_install();

  TCCIRState *ir = tcc_ir_alloc();
  int a = tcc_ir_vreg_alloc_temp(ir);
  int c = tcc_ir_vreg_alloc_temp(ir);
  SValue s1 = sv_typed(a, VT_FLOAT);
  SValue d = sv_typed(c, VT_DOUBLE);

  int rc = ut_soft_ir_put_soft_call_fpu_if_needed(ir, TCCIR_OP_CVT_FTOF, &s1, NULL, &d);

  UT_ASSERT_EQ(rc, 1);
  UT_ASSERT_EQ(tcc_ir_count(ir), 2); /* unary op: one param + call */
  UT_CHECK(assert_param(ir, 0, a, 0, 0));
  UT_CHECK(assert_callval(ir, 1, c, 0, 1, "__aeabi_f2d"));

  tcc_ir_free(ir);
  fpu_restore();
  return 0;
}

UT_TEST(test_fpu_gate_cvt_ftof_d2f_soft_emits_d2f)
{
  ut_soft_reset();
  fpu_install();

  TCCIRState *ir = tcc_ir_alloc();
  int a = tcc_ir_vreg_alloc_temp(ir);
  int c = tcc_ir_vreg_alloc_temp(ir);
  SValue s1 = sv_typed(a, VT_DOUBLE);
  SValue d = sv_typed(c, VT_FLOAT);

  int rc = ut_soft_ir_put_soft_call_fpu_if_needed(ir, TCCIR_OP_CVT_FTOF, &s1, NULL, &d);

  UT_ASSERT_EQ(rc, 1);
  UT_ASSERT_EQ(tcc_ir_count(ir), 2);
  UT_CHECK(assert_param(ir, 0, a, 0, 0));
  UT_CHECK(assert_callval(ir, 1, c, 0, 1, "__aeabi_d2f"));

  tcc_ir_free(ir);
  fpu_restore();
  return 0;
}

UT_TEST(test_fpu_gate_cvt_ftof_hw_path_requires_both_bits)
{
  /* The CVT_FTOF hardware early-out requires has_dtof AND has_ftod together
   * (unlike the single-bit checks of the other ops) — pinned. */
  ut_soft_reset();
  fpu_install();
  ut_soft_fpu_cfg.has_dtof = 1; /* only one of the two bits */

  TCCIRState *ir = tcc_ir_alloc();
  int a = tcc_ir_vreg_alloc_temp(ir);
  int c = tcc_ir_vreg_alloc_temp(ir);
  SValue s1 = sv_typed(a, VT_FLOAT);
  SValue d = sv_typed(c, VT_DOUBLE);

  int rc = ut_soft_ir_put_soft_call_fpu_if_needed(ir, TCCIR_OP_CVT_FTOF, &s1, NULL, &d);
  UT_ASSERT_EQ(rc, 1);
  UT_CHECK(assert_callval(ir, 1, c, 0, 1, "__aeabi_f2d"));
  tcc_ir_free(ir);

  ut_soft_reset();
  ut_soft_fpu_cfg.has_ftod = 1; /* now both bits */

  ir = tcc_ir_alloc();
  s1 = sv_typed(tcc_ir_vreg_alloc_temp(ir), VT_FLOAT);
  d = sv_typed(tcc_ir_vreg_alloc_temp(ir), VT_DOUBLE);

  rc = ut_soft_ir_put_soft_call_fpu_if_needed(ir, TCCIR_OP_CVT_FTOF, &s1, NULL, &d);
  UT_ASSERT_EQ(rc, 0);
  UT_ASSERT_EQ(tcc_ir_count(ir), 0);

  tcc_ir_free(ir);
  fpu_restore();
  return 0;
}

UT_TEST(test_fpu_gate_cvt_itof_soft_names)
{
  ut_soft_reset();
  fpu_install();

  /* signed int -> float */
  TCCIRState *ir = tcc_ir_alloc();
  int a = tcc_ir_vreg_alloc_temp(ir);
  int c = tcc_ir_vreg_alloc_temp(ir);
  SValue s1 = sv_typed(a, VT_INT);
  SValue d = sv_typed(c, VT_FLOAT);

  int rc = ut_soft_ir_put_soft_call_fpu_if_needed(ir, TCCIR_OP_CVT_ITOF, &s1, NULL, &d);
  UT_ASSERT_EQ(rc, 1);
  UT_ASSERT_EQ(tcc_ir_count(ir), 2);
  UT_CHECK(assert_param(ir, 0, a, 0, 0));
  UT_CHECK(assert_callval(ir, 1, c, 0, 1, "__aeabi_i2f"));
  tcc_ir_free(ir);

  /* unsigned int -> float */
  ut_soft_reset();
  ir = tcc_ir_alloc();
  a = tcc_ir_vreg_alloc_temp(ir);
  c = tcc_ir_vreg_alloc_temp(ir);
  s1 = sv_typed(a, VT_INT | VT_UNSIGNED);
  d = sv_typed(c, VT_FLOAT);

  rc = ut_soft_ir_put_soft_call_fpu_if_needed(ir, TCCIR_OP_CVT_ITOF, &s1, NULL, &d);
  UT_ASSERT_EQ(rc, 1);
  UT_CHECK(assert_callval(ir, 1, c, 0, 1, "__aeabi_ui2f"));

  tcc_ir_free(ir);
  fpu_restore();
  return 0;
}

UT_TEST(test_fpu_gate_cvt_ftoi_soft_names)
{
  ut_soft_reset();
  fpu_install();

  /* float -> signed int (32-bit dest) */
  TCCIRState *ir = tcc_ir_alloc();
  int a = tcc_ir_vreg_alloc_temp(ir);
  int c = tcc_ir_vreg_alloc_temp(ir);
  SValue s1 = sv_typed(a, VT_FLOAT);
  SValue d = sv_typed(c, VT_INT);

  int rc = ut_soft_ir_put_soft_call_fpu_if_needed(ir, TCCIR_OP_CVT_FTOI, &s1, NULL, &d);
  UT_ASSERT_EQ(rc, 1);
  UT_CHECK(assert_callval(ir, 1, c, 0, 1, "__aeabi_f2iz"));
  tcc_ir_free(ir);

  /* float -> long long (64-bit dest selects the lz helper) */
  ut_soft_reset();
  ir = tcc_ir_alloc();
  a = tcc_ir_vreg_alloc_temp(ir);
  c = tcc_ir_vreg_alloc_temp(ir);
  s1 = sv_typed(a, VT_FLOAT);
  d = sv_typed(c, VT_LLONG);

  rc = ut_soft_ir_put_soft_call_fpu_if_needed(ir, TCCIR_OP_CVT_FTOI, &s1, NULL, &d);
  UT_ASSERT_EQ(rc, 1);
  UT_CHECK(assert_callval(ir, 1, c, 0, 1, "__aeabi_f2lz"));

  tcc_ir_free(ir);
  fpu_restore();
  return 0;
}

UT_TEST(test_fpu_gate_cvt_itof_i64_to_double_soft)
{
  ut_soft_reset();
  fpu_install();

  /* 64-bit source puts the gate on the has_itod branch (0 here) and the name
   * lookup on the 64-bit integer branch: l2d. */
  TCCIRState *ir = tcc_ir_alloc();
  int a = tcc_ir_vreg_alloc_temp(ir);
  int c = tcc_ir_vreg_alloc_temp(ir);
  SValue s1 = sv_typed(a, VT_LLONG);
  SValue d = sv_typed(c, VT_DOUBLE);

  int rc = ut_soft_ir_put_soft_call_fpu_if_needed(ir, TCCIR_OP_CVT_ITOF, &s1, NULL, &d);

  UT_ASSERT_EQ(rc, 1);
  UT_ASSERT_EQ(tcc_ir_count(ir), 2);
  UT_CHECK(assert_param(ir, 0, a, 0, 0));
  UT_CHECK(assert_callval(ir, 1, c, 0, 1, "__aeabi_l2d"));

  tcc_ir_free(ir);
  fpu_restore();
  return 0;
}

UT_TEST(test_fpu_gate_ignores_float_abi)
{
  /* The gate reads architecture_config.fpu only; tcc_state->float_abi (the
   * calling-convention selector) plays no role in the soft-call decision. */
  const int saved_abi = tcc_state->float_abi;

  /* ARM_HARD_FLOAT selected, but the FPU config says no hardware: still a
   * soft call. */
  ut_soft_reset();
  fpu_install();
  tcc_state->float_abi = ARM_HARD_FLOAT;

  TCCIRState *ir = tcc_ir_alloc();
  SValue s1, s2, d;
  make_ops_32(ir, TCCIR_OP_FADD, &s1, &s2, &d);

  int rc = ut_soft_ir_put_soft_call_fpu_if_needed(ir, TCCIR_OP_FADD, &s1, &s2, &d);
  UT_ASSERT_EQ(rc, 1);
  UT_ASSERT_EQ(tcc_ir_count(ir), 3);
  tcc_ir_free(ir);

  /* ARM_SOFT_FLOAT selected, but the FPU config advertises hardware FADD:
   * hardware path. */
  ut_soft_reset();
  ut_soft_fpu_cfg.has_fadd = 1;
  tcc_state->float_abi = ARM_SOFT_FLOAT;

  ir = tcc_ir_alloc();
  make_ops_32(ir, TCCIR_OP_FADD, &s1, &s2, &d);

  rc = ut_soft_ir_put_soft_call_fpu_if_needed(ir, TCCIR_OP_FADD, &s1, &s2, &d);
  UT_ASSERT_EQ(rc, 0);
  UT_ASSERT_EQ(tcc_ir_count(ir), 0);

  tcc_ir_free(ir);
  tcc_state->float_abi = saved_abi;
  fpu_restore();
  return 0;
}
