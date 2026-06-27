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
#include "arch/arm/arm_regalloc.h"
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

/* -------------------------------------------------------------------------- */
/* Suite                                                                      */
/* -------------------------------------------------------------------------- */

UT_SUITE(codegen_fp)
{
  UT_RUN(test_fp_interval_metadata);
  UT_RUN(test_fp_machine_operand);
  UT_RUN(test_fp_double_pair);
  UT_RUN(test_fp_op_construction);
  UT_RUN(test_complex_float_pair);
}
