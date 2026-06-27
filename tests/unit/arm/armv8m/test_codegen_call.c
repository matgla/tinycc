/*
 *  test_codegen_call.c - backend unit tests for call/return IR ops
 *
 *  Exercises AAPCS incoming parameter setup (tcc_ir_codegen_params_setup),
 *  outgoing FUNCPARAMVAL/FUNCCALLVAL operand lowering, and RETURNVALUE /
 *  drop_return helpers in ir/codegen.c.
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

static SValue sv_param_marker(int call_id, int idx)
{
  SValue sv;
  svalue_init(&sv);
  sv.r = VT_CONST;
  sv.c.i = (int64_t)TCCIR_ENCODE_PARAM(call_id, idx);
  sv.type.t = VT_INT;
  return sv;
}

static SValue sv_call_id(int call_id, int argc)
{
  SValue sv;
  svalue_init(&sv);
  sv.r = VT_CONST;
  sv.c.i = (int64_t)TCCIR_ENCODE_CALL(call_id, argc);
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
/* Incoming parameters: first four in r0-r3, fifth on stack                   */
/* -------------------------------------------------------------------------- */

UT_TEST(test_aapcs_incoming_params)
{
  TCCIRState *ir = tcc_ir_alloc();
  setup_tcc_state();

  int p0 = tcc_ir_vreg_alloc_param(ir);
  int p1 = tcc_ir_vreg_alloc_param(ir);
  int p2 = tcc_ir_vreg_alloc_param(ir);
  int p3 = tcc_ir_vreg_alloc_param(ir);
  int p4 = tcc_ir_vreg_alloc_param(ir);

  /* Use every parameter so they stay live and get an interval. */
  int sum = tcc_ir_vreg_alloc_temp(ir);
  SValue s_sum = sv_var(sum, VT_INT);
  SValue s_p0 = sv_var(p0, VT_INT);
  SValue s_p1 = sv_var(p1, VT_INT);
  SValue s_p2 = sv_var(p2, VT_INT);
  SValue s_p3 = sv_var(p3, VT_INT);
  SValue s_p4 = sv_var(p4, VT_INT);

  tcc_ir_put(ir, TCCIR_OP_ADD, &s_p0, &s_p1, &s_sum);
  tcc_ir_put(ir, TCCIR_OP_ADD, &s_sum, &s_p2, &s_sum);
  tcc_ir_put(ir, TCCIR_OP_ADD, &s_sum, &s_p3, &s_sum);
  tcc_ir_put(ir, TCCIR_OP_ADD, &s_sum, &s_p4, &s_sum);
  tcc_ir_put(ir, TCCIR_OP_RETURNVALUE, &s_sum, NULL, NULL);

  tcc_ir_ssa_regalloc(ir, arm_get_regalloc_target(), 0);
  tcc_ir_codegen_params_setup(ir);

  UT_ASSERT_EQ(tcc_ir_codegen_reg_get(ir, p0), 0);
  UT_ASSERT_EQ(tcc_ir_codegen_reg_get(ir, p1), 1);
  UT_ASSERT_EQ(tcc_ir_codegen_reg_get(ir, p2), 2);
  UT_ASSERT_EQ(tcc_ir_codegen_reg_get(ir, p3), 3);

  IRLiveInterval *li4 = tcc_ir_vreg_live_interval(ir, p4);
  UT_ASSERT(li4 != NULL);
  UT_ASSERT_EQ(li4->incoming_reg0, -1);
  UT_ASSERT_EQ(li4->original_offset, 0); /* first stack argument */

  tcc_ir_free(ir);
  return 0;
}

/* -------------------------------------------------------------------------- */
/* 64-bit incoming parameter uses an even register pair                        */
/* -------------------------------------------------------------------------- */

UT_TEST(test_aapcs_64bit_param)
{
  TCCIRState *ir = tcc_ir_alloc();
  setup_tcc_state();

  int p0 = tcc_ir_vreg_alloc_param(ir);
  int p1 = tcc_ir_vreg_alloc_param(ir);

  tcc_ir_vreg_type_set_64bit(ir, p0);

  SValue s_p0 = sv_var(p0, VT_LLONG);
  (void)p1;

  tcc_ir_put(ir, TCCIR_OP_RETURNVALUE, &s_p0, NULL, NULL);

  tcc_ir_ssa_regalloc(ir, arm_get_regalloc_target(), 0);
  tcc_ir_codegen_params_setup(ir);

  IRLiveInterval *li0 = tcc_ir_vreg_live_interval(ir, p0);
  UT_ASSERT(li0 != NULL);
  UT_ASSERT_EQ(li0->incoming_reg0, 0);
  UT_ASSERT_EQ(li0->incoming_reg1, 1);

  IRLiveInterval *li1 = tcc_ir_vreg_live_interval(ir, p1);
  UT_ASSERT(li1 != NULL);
  UT_ASSERT_EQ(li1->incoming_reg0, 2);

  tcc_ir_free(ir);
  return 0;
}

/* -------------------------------------------------------------------------- */
/* Outgoing call operand layout                                                */
/* -------------------------------------------------------------------------- */

UT_TEST(test_outgoing_call_operands)
{
  TCCIRState *ir = tcc_ir_alloc();
  setup_tcc_state();

  int arg = tcc_ir_vreg_alloc_temp(ir);
  int ret = tcc_ir_vreg_alloc_temp(ir);

  SValue s_arg = sv_var(arg, VT_INT);
  SValue s_ret = sv_var(ret, VT_INT);
  SValue s_param = sv_param_marker(0, 0);
  SValue s_call = sv_call_id(0, 1);

  SValue s_const123 = sv_const(123);
  tcc_ir_put(ir, TCCIR_OP_ASSIGN, &s_const123, NULL, &s_arg);
  tcc_ir_put(ir, TCCIR_OP_FUNCPARAMVAL, &s_arg, &s_param, NULL);
  tcc_ir_put(ir, TCCIR_OP_FUNCCALLVAL, &s_param, &s_call, &s_ret);
  tcc_ir_put(ir, TCCIR_OP_RETURNVALUE, &s_ret, NULL, NULL);

  tcc_ir_ssa_regalloc(ir, arm_get_regalloc_target(), 0);

  int call_idx = -1;
  for (int i = 0; i < ir->next_instruction_index; i++)
  {
    if (ir->compact_instructions[i].op == TCCIR_OP_FUNCCALLVAL)
    {
      call_idx = i;
      break;
    }
  }
  UT_ASSERT(call_idx >= 0);

  IRQuadCompact *q = &ir->compact_instructions[call_idx];
  IROperand d = tcc_ir_codegen_dest_get(ir, q);
  IROperand s1 = tcc_ir_codegen_src1_get(ir, q);
  MachineOperand mret = machine_op_from_ir(ir, &d);
  MachineOperand mparam = machine_op_from_ir(ir, &s1);

  /* Return value arrives in r0; the marker operand is an immediate constant. */
  UT_ASSERT(mret.kind == MACH_OP_REG || mret.kind == MACH_OP_SPILL);
  UT_ASSERT(mparam.kind == MACH_OP_IMM);

  tcc_ir_free(ir);
  return 0;
}

/* -------------------------------------------------------------------------- */
/* drop_return kills an unused FUNCCALLVAL return value                        */
/* -------------------------------------------------------------------------- */

UT_TEST(test_drop_return)
{
  TCCIRState *ir = tcc_ir_alloc();
  setup_tcc_state();

  int arg = tcc_ir_vreg_alloc_temp(ir);
  int ret = tcc_ir_vreg_alloc_temp(ir);

  SValue s_arg = sv_var(arg, VT_INT);
  SValue s_ret = sv_var(ret, VT_INT);
  SValue s_param = sv_param_marker(0, 0);
  SValue s_call = sv_call_id(0, 1);

  SValue s_one = sv_const(1);
  tcc_ir_put(ir, TCCIR_OP_ASSIGN, &s_one, NULL, &s_arg);
  tcc_ir_put(ir, TCCIR_OP_FUNCPARAMVAL, &s_arg, &s_param, NULL);
  int call_idx = tcc_ir_put(ir, TCCIR_OP_FUNCCALLVAL, &s_param, &s_call, &s_ret);

  tcc_ir_codegen_drop_return(ir);

  IRQuadCompact *q = &ir->compact_instructions[call_idx];
  IROperand dst = tcc_ir_codegen_dest_get(ir, q);
  UT_ASSERT(!irop_has_vreg(dst));

  tcc_ir_free(ir);
  return 0;
}

/* -------------------------------------------------------------------------- */
/* Suite                                                                      */
/* -------------------------------------------------------------------------- */

UT_SUITE(codegen_call)
{
  UT_RUN(test_aapcs_incoming_params);
  UT_RUN(test_aapcs_64bit_param);
  UT_RUN(test_outgoing_call_operands);
  UT_RUN(test_drop_return);
}
