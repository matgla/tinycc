/*
 *  test_codegen_fp.c - backend unit tests for floating-point IR ops
 *
 *  Exercises FP vreg type metadata, machine-operand lowering for FP values,
 *  and the register-allocation hints used by hard-float codegen.
 */

#define USING_GLOBALS
#include "ir.h"
#include "cfg.h"
#include "ir/ssa.h"
#include "ir/vreg.h"
#include "ir/regalloc.h"
#include "ir/codegen.h"
#include "ir/machine_op.h"
#include "source/backend/arch/arm/arm_regalloc.h"
#include "codegen_mop_stubs.h"
#include "ut.h"

static SValue sv_var(int vreg, int vt)
{
  SValue sv;
  svalue_init(&sv);
  sv.vr = vreg;
  sv.type.t = vt;
  return sv;
}

static SValue sv_const(int v)
{
  SValue sv;
  svalue_init(&sv);
  sv.r = VT_CONST;
  sv.c.i = v;
  sv.type.t = VT_INT;
  return sv;
}

static void setup_tcc_state(void)
{
  tcc_state->registers_for_allocator = 13;
  tcc_state->registers_map_for_allocator = (1ull << 13) - 1;
  tcc_state->float_abi = ARM_HARD_FLOAT;
  tcc_state->float_registers_for_allocator = 32;
  tcc_state->float_registers_map_for_allocator = (1ull << 32) - 1;
  tcc_state->optimize = 0;
}

/* -------------------------------------------------------------------------- */
/* FP vreg metadata                                                            */
/* -------------------------------------------------------------------------- */

UT_TEST(test_fp_interval_metadata)
{
  TCCIRState *ir = tcc_ir_alloc();
  setup_tcc_state();

  int f0 = tcc_ir_vreg_alloc_temp(ir);
  int d0 = tcc_ir_vreg_alloc_temp(ir);

  tcc_ir_vreg_type_set_fp(ir, f0, 1, 0);
  tcc_ir_vreg_type_set_fp(ir, d0, 0, 1);

  IRLiveInterval *li_f = tcc_ir_vreg_live_interval(ir, f0);
  IRLiveInterval *li_d = tcc_ir_vreg_live_interval(ir, d0);

  UT_ASSERT(li_f != NULL);
  UT_ASSERT(li_d != NULL);

  UT_ASSERT(li_f->is_float);
  UT_ASSERT(!li_f->is_double);
  UT_ASSERT(li_f->use_vfp);

  UT_ASSERT(!li_d->is_float);
  UT_ASSERT(li_d->is_double);
  UT_ASSERT(li_d->use_vfp);

  tcc_ir_free(ir);
  return 0;
}

/* -------------------------------------------------------------------------- */
/* machine_op_from_ir for FP register-resident values                          */
/* -------------------------------------------------------------------------- */

UT_TEST(test_fp_machine_operand)
{
  TCCIRState *ir = tcc_ir_alloc();
  setup_tcc_state();

  int f0 = tcc_ir_vreg_alloc_temp(ir);
  tcc_ir_vreg_type_set_fp(ir, f0, 1, 0);

  IRLiveInterval *li = tcc_ir_vreg_live_interval(ir, f0);
  UT_ASSERT(li != NULL);

  /* Simulate hard-float allocation to s0. */
  li->start = 0;
  li->end = 1;
  li->allocation.r0 = 16; /* s0 = 16 in the unified reg numbering used here */
  li->allocation.r1 = PREG_NONE;
  li->allocation.offset = 0;

  IROperand op = irop_make_vreg(f0, IROP_BTYPE_FLOAT32);
  MachineOperand m = machine_op_from_ir(ir, &op);

  UT_ASSERT_EQ(m.kind, MACH_OP_REG);
  UT_ASSERT_EQ(m.btype, IROP_BTYPE_FLOAT32);
  UT_ASSERT_EQ(m.u.reg.r0, 16);
  UT_ASSERT_EQ(m.u.reg.r1, -1);
  UT_ASSERT(!m.is_64bit);

  tcc_ir_free(ir);
  return 0;
}

/* -------------------------------------------------------------------------- */
/* Double-precision produces register pairs                                    */
/* -------------------------------------------------------------------------- */

UT_TEST(test_fp_double_pair)
{
  TCCIRState *ir = tcc_ir_alloc();
  setup_tcc_state();

  int d0 = tcc_ir_vreg_alloc_temp(ir);
  tcc_ir_vreg_type_set_fp(ir, d0, 0, 1);

  IRLiveInterval *li = tcc_ir_vreg_live_interval(ir, d0);
  UT_ASSERT(li != NULL);

  li->start = 0;
  li->end = 1;
  li->allocation.r0 = 16; /* d0 starts at s0/s1 pair */
  li->allocation.r1 = 17;
  li->allocation.offset = 0;

  IROperand op = irop_make_vreg(d0, IROP_BTYPE_FLOAT64);
  MachineOperand m = machine_op_from_ir(ir, &op);

  UT_ASSERT(m.is_64bit);
  UT_ASSERT_EQ(m.u.reg.r0, 16);
  UT_ASSERT_EQ(m.u.reg.r1, 17);

  tcc_ir_free(ir);
  return 0;
}

/* -------------------------------------------------------------------------- */
/* FP IR op construction preserves type                                        */
/* -------------------------------------------------------------------------- */

UT_TEST(test_fp_op_construction)
{
  TCCIRState *ir = tcc_ir_alloc();
  setup_tcc_state();

  int a = tcc_ir_vreg_alloc_temp(ir);
  int b = tcc_ir_vreg_alloc_temp(ir);
  int c = tcc_ir_vreg_alloc_temp(ir);

  /* tcc_ir_put for FP ops consults architecture_config.fpu which is not
   * initialized in the unit-test harness, so build the instruction manually. */
  int pool_base = ir->iroperand_pool_count;
  tcc_ir_pool_add(ir, irop_make_vreg(c, IROP_BTYPE_FLOAT32));
  tcc_ir_pool_add(ir, irop_make_vreg(a, IROP_BTYPE_FLOAT32));
  tcc_ir_pool_add(ir, irop_make_vreg(b, IROP_BTYPE_FLOAT32));

  int idx = ir->next_instruction_index;
  IRQuadCompact *q = &ir->compact_instructions[idx];
  q->op = TCCIR_OP_FADD;
  q->operand_base = pool_base;
  ir->next_instruction_index++;

  UT_ASSERT_EQ(q->op, TCCIR_OP_FADD);

  IROperand dst = tcc_ir_codegen_dest_get(ir, q);
  UT_ASSERT_EQ(irop_get_btype(dst), IROP_BTYPE_FLOAT32);
  UT_ASSERT_EQ(TCCIR_DECODE_VREG_TYPE(irop_get_vreg(dst)), TCCIR_VREG_TYPE_TEMP);

  tcc_ir_free(ir);
  return 0;
}

/* -------------------------------------------------------------------------- */
/* Complex float forces pair allocation in machine operand                     */
/* -------------------------------------------------------------------------- */

UT_TEST(test_complex_float_pair)
{
  TCCIRState *ir = tcc_ir_alloc();
  setup_tcc_state();

  int cf = tcc_ir_vreg_alloc_temp(ir);
  tcc_ir_vreg_type_set_fp(ir, cf, 1, 0);

  IRLiveInterval *li = tcc_ir_vreg_live_interval(ir, cf);
  UT_ASSERT(li != NULL);
  li->is_complex = 1;
  li->start = 0;
  li->end = 1;
  li->allocation.r0 = 16;
  li->allocation.r1 = 17;
  li->allocation.offset = 0;

  IROperand op = irop_make_vreg(cf, IROP_BTYPE_FLOAT32);
  op.is_complex = 1;
  MachineOperand m = machine_op_from_ir(ir, &op);

  UT_ASSERT(m.is_complex);
  UT_ASSERT(m.is_64bit);
  UT_ASSERT_EQ(m.u.reg.r0, 16);
  UT_ASSERT_EQ(m.u.reg.r1, 17);

  tcc_ir_free(ir);
  return 0;
}

/* ============================================================================
 * Dispatch-level tests (tcc_ir_codegen_generate)
 *
 * See test_codegen_arith.c's dispatch-level section header for the overall
 * rationale. FADD/FSUB/FMUL/FDIV/CVT_FTOF/CVT_ITOF/CVT_FTOI all share one
 * case label in ir/codegen.c (~2999-3007), ending in exactly one
 * tcc_gen_machine_fp_mop() call per instruction. As test_fp_op_construction
 * above notes, tcc_ir_put() can't build these (it consults
 * architecture_config.fpu, uninitialized here) -- built manually via
 * tcc_ir_pool_add, same as that test, but followed through real regalloc +
 * codegen so the dispatch loop actually runs.
 * ============================================================================ */

/* Builds `dest <op> src1, src2` (or `dest <op> src1` with src2 = IROP_NONE
 * for the CVT_* unary conversions) with the given per-operand float-ness,
 * then runs regalloc + tcc_ir_codegen_generate(). Caller must cgstub_reset()
 * first and tcc_ir_free(ir) after. */
static TCCIRState *build_fp_op(TccIrOp op, int dest_is_fp, int src1_is_fp, int src2_is_fp)
{
  TCCIRState *ir = tcc_ir_alloc();
  setup_tcc_state();

  int dest = tcc_ir_vreg_alloc_temp(ir);
  int src1 = tcc_ir_vreg_alloc_temp(ir);
  if (dest_is_fp)
    tcc_ir_vreg_type_set_fp(ir, dest, 1, 0);
  if (src1_is_fp)
    tcc_ir_vreg_type_set_fp(ir, src1, 1, 0);

  int pool_base = ir->iroperand_pool_count;
  tcc_ir_pool_add(ir, irop_make_vreg(dest, dest_is_fp ? IROP_BTYPE_FLOAT32 : IROP_BTYPE_INT32));
  tcc_ir_pool_add(ir, irop_make_vreg(src1, src1_is_fp ? IROP_BTYPE_FLOAT32 : IROP_BTYPE_INT32));
  if (src2_is_fp >= 0)
  {
    int src2 = tcc_ir_vreg_alloc_temp(ir);
    if (src2_is_fp)
      tcc_ir_vreg_type_set_fp(ir, src2, 1, 0);
    tcc_ir_pool_add(ir, irop_make_vreg(src2, src2_is_fp ? IROP_BTYPE_FLOAT32 : IROP_BTYPE_INT32));
  }
  else
  {
    tcc_ir_pool_add(ir, IROP_NONE);
  }

  int idx = ir->next_instruction_index;
  IRQuadCompact *q = &ir->compact_instructions[idx];
  q->op = op;
  q->operand_base = pool_base;
  ir->next_instruction_index++;

  tcc_ir_ssa_regalloc(ir, arm_get_regalloc_target(), 0);
  ir->leaffunc = 1;
  return ir;
}

UT_TEST(test_dispatch_fp_binops_route_to_fp_mop)
{
  static const TccIrOp ops[] = {
      TCCIR_OP_FADD,
      TCCIR_OP_FSUB,
      TCCIR_OP_FMUL,
      TCCIR_OP_FDIV,
  };

  for (size_t k = 0; k < sizeof(ops) / sizeof(ops[0]); k++)
  {
    cgstub_reset();
    TCCIRState *ir = build_fp_op(ops[k], /*dest*/ 1, /*src1*/ 1, /*src2*/ 1);
    tcc_ir_codegen_generate(ir);

    UT_ASSERT_EQ(cgstub_call_count("fp_mop"), 1);
    const CgStubCall *c = cgstub_nth_call("fp_mop", 0);
    UT_ASSERT(c != NULL);
    UT_ASSERT_EQ(c->ir_op, ops[k]);
    UT_ASSERT_EQ(c->aux0, 0); /* is_complex: plain (non-complex) float operands */

    tcc_ir_free(ir);
  }
  return 0;
}

UT_TEST(test_dispatch_cvt_itof_and_ftoi_route_to_fp_mop)
{
  cgstub_reset();
  TCCIRState *ir_itof = build_fp_op(TCCIR_OP_CVT_ITOF, /*dest*/ 1, /*src1*/ 0, /*src2*/ -1);
  tcc_ir_codegen_generate(ir_itof);
  UT_ASSERT_EQ(cgstub_call_count("fp_mop"), 1);
  UT_ASSERT_EQ(cgstub_nth_call("fp_mop", 0)->ir_op, TCCIR_OP_CVT_ITOF);
  tcc_ir_free(ir_itof);

  cgstub_reset();
  TCCIRState *ir_ftoi = build_fp_op(TCCIR_OP_CVT_FTOI, /*dest*/ 0, /*src1*/ 1, /*src2*/ -1);
  tcc_ir_codegen_generate(ir_ftoi);
  UT_ASSERT_EQ(cgstub_call_count("fp_mop"), 1);
  UT_ASSERT_EQ(cgstub_nth_call("fp_mop", 0)->ir_op, TCCIR_OP_CVT_FTOI);
  tcc_ir_free(ir_ftoi);

  return 0;
}
