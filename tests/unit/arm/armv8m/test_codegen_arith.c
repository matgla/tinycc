/*
 *  test_codegen_arith.c - backend unit tests for integer arithmetic IR ops
 *
 *  Covers the IR->machine-operand lowering path for arithmetic operations
 *  (ADD/SUB/MUL/DIV/IMOD and bitwise/shifts) plus the codegen helper
 *  accessors in ir/codegen.c (dest/src getters/setters and register queries).
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

static SValue sv_var(int vreg)
{
  SValue sv;
  svalue_init(&sv);
  sv.vr = vreg;
  sv.type.t = VT_INT;
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

static TCCIRState *build_arith(TccIrOp op, int lhs, int rhs)
{
  TCCIRState *ir = tcc_ir_alloc();
  setup_tcc_state();

  int a = tcc_ir_vreg_alloc_temp(ir);
  int b = tcc_ir_vreg_alloc_temp(ir);
  int c = tcc_ir_vreg_alloc_temp(ir);

  SValue s_a = sv_var(a);
  SValue s_b = sv_var(b);
  SValue s_c = sv_var(c);
  SValue s_lhs = sv_const(lhs);
  SValue s_rhs = sv_const(rhs);

  tcc_ir_put(ir, TCCIR_OP_ASSIGN, &s_lhs, NULL, &s_a);
  tcc_ir_put(ir, TCCIR_OP_ASSIGN, &s_rhs, NULL, &s_b);
  tcc_ir_put(ir, op, &s_a, &s_b, &s_c);
  tcc_ir_put(ir, TCCIR_OP_RETURNVALUE, &s_c, NULL, NULL);

  tcc_ir_ssa_regalloc(ir, arm_get_regalloc_target(), 0);
  return ir;
}

/* -------------------------------------------------------------------------- */
/* Codegen operand accessors                                                  */
/* -------------------------------------------------------------------------- */

UT_TEST(test_codegen_arith_accessors)
{
  TCCIRState *ir = build_arith(TCCIR_OP_ADD, 5, 3);

  int add_idx = -1;
  for (int i = 0; i < ir->next_instruction_index; i++)
  {
    if (ir->compact_instructions[i].op == TCCIR_OP_ADD)
    {
      add_idx = i;
      break;
    }
  }
  UT_ASSERT(add_idx >= 0);

  IRQuadCompact *q = &ir->compact_instructions[add_idx];

  /* tcc_ir_codegen_* accessors must agree with tcc_ir_op_get_*. */
  IROperand dest = tcc_ir_codegen_dest_get(ir, q);
  IROperand src1 = tcc_ir_codegen_src1_get(ir, q);
  IROperand src2 = tcc_ir_codegen_src2_get(ir, q);

  UT_ASSERT(irop_has_vreg(dest));
  UT_ASSERT(irop_has_vreg(src1));
  UT_ASSERT(irop_has_vreg(src2));

  IROperand expected_dest = tcc_ir_op_get_dest(ir, q);
  IROperand expected_src1 = tcc_ir_op_get_src1(ir, q);
  IROperand expected_src2 = tcc_ir_op_get_src2(ir, q);

  UT_ASSERT_EQ(dest.vr, expected_dest.vr);
  UT_ASSERT_EQ(src1.vr, expected_src1.vr);
  UT_ASSERT_EQ(src2.vr, expected_src2.vr);

  /* dest-set round-trip */
  IROperand saved = dest;
  tcc_ir_codegen_dest_set(ir, q, IROP_NONE);
  UT_ASSERT(irop_is_none(tcc_ir_codegen_dest_get(ir, q)));
  tcc_ir_codegen_dest_set(ir, q, saved);
  UT_ASSERT_EQ(tcc_ir_codegen_dest_get(ir, q).vr, saved.vr);

  tcc_ir_free(ir);
  return 0;
}

/* -------------------------------------------------------------------------- */
/* Machine-operand lowering for immediate vs register operands                */
/* -------------------------------------------------------------------------- */

UT_TEST(test_arith_immediate_and_register_operands)
{
  TCCIRState *ir = build_arith(TCCIR_OP_ADD, 5, 3);

  int add_idx = -1;
  for (int i = 0; i < ir->next_instruction_index; i++)
  {
    if (ir->compact_instructions[i].op == TCCIR_OP_ADD)
    {
      add_idx = i;
      break;
    }
  }
  UT_ASSERT(add_idx >= 0);

  IRQuadCompact *q = &ir->compact_instructions[add_idx];
  IROperand src1 = tcc_ir_codegen_src1_get(ir, q);
  IROperand src2 = tcc_ir_codegen_src2_get(ir, q);
  IROperand dest = tcc_ir_codegen_dest_get(ir, q);

  /* ADD src1 and src2 are register-resident temporaries (constants were folded
   * into ASSIGNs).  Verify machine_op_from_ir reflects the allocation. */
  MachineOperand m1 = machine_op_from_ir(ir, &src1);
  MachineOperand m2 = machine_op_from_ir(ir, &src2);
  MachineOperand md = machine_op_from_ir(ir, &dest);

  UT_ASSERT_EQ(md.kind, MACH_OP_REG);
  UT_ASSERT(md.u.reg.r0 < PREG_NONE);

  UT_ASSERT_EQ(m1.kind, MACH_OP_REG);
  UT_ASSERT_EQ(m2.kind, MACH_OP_REG);

  /* Register getters/setters */
  int dest_vr = irop_get_vreg(dest);
  UT_ASSERT_EQ(tcc_ir_codegen_reg_get(ir, dest_vr), md.u.reg.r0);
  tcc_ir_codegen_reg_set(ir, dest_vr, 7);
  UT_ASSERT_EQ(tcc_ir_codegen_reg_get(ir, dest_vr), 7);
  tcc_ir_codegen_reg_set(ir, dest_vr, md.u.reg.r0);

  tcc_ir_free(ir);
  return 0;
}

/* -------------------------------------------------------------------------- */
/* Coverage of arithmetic op shapes                                           */
/* -------------------------------------------------------------------------- */

UT_TEST(test_arith_op_family_lowering)
{
  static const TccIrOp ops[] = {
      TCCIR_OP_SUB, TCCIR_OP_MUL, TCCIR_OP_DIV, TCCIR_OP_IMOD,
      TCCIR_OP_AND, TCCIR_OP_OR,  TCCIR_OP_XOR, TCCIR_OP_SHL,
      TCCIR_OP_SAR, TCCIR_OP_SHR,
  };

  for (size_t k = 0; k < sizeof(ops) / sizeof(ops[0]); k++)
  {
    TCCIRState *ir = build_arith(ops[k], 10, 4);

    int idx = -1;
    for (int i = 0; i < ir->next_instruction_index; i++)
    {
      if (ir->compact_instructions[i].op == ops[k])
      {
        idx = i;
        break;
      }
    }
    UT_ASSERT(idx >= 0);

    IRQuadCompact *q = &ir->compact_instructions[idx];
    IROperand d = tcc_ir_codegen_dest_get(ir, q);
    IROperand s1 = tcc_ir_codegen_src1_get(ir, q);
    IROperand s2 = tcc_ir_codegen_src2_get(ir, q);
    MachineOperand md = machine_op_from_ir(ir, &d);
    MachineOperand m1 = machine_op_from_ir(ir, &s1);
    MachineOperand m2 = machine_op_from_ir(ir, &s2);

    /* Destination is always allocated to a register or stack slot. */
    UT_ASSERT(md.kind == MACH_OP_REG || md.kind == MACH_OP_SPILL || md.kind == MACH_OP_FRAME_ADDR);
    /* Sources are register operands (after regalloc). */
    UT_ASSERT(m1.kind == MACH_OP_REG);
    UT_ASSERT(m2.kind == MACH_OP_REG);

    tcc_ir_free(ir);
  }
  return 0;
}

/* -------------------------------------------------------------------------- */
/* 64-bit arithmetic produces register pairs                                   */
/* -------------------------------------------------------------------------- */

UT_TEST(test_arith_64bit_pair)
{
  TCCIRState *ir = tcc_ir_alloc();
  setup_tcc_state();

  int a = tcc_ir_vreg_alloc_temp(ir);
  int b = tcc_ir_vreg_alloc_temp(ir);
  int c = tcc_ir_vreg_alloc_temp(ir);

  tcc_ir_vreg_type_set_64bit(ir, a);
  tcc_ir_vreg_type_set_64bit(ir, b);
  tcc_ir_vreg_type_set_64bit(ir, c);

  SValue s_a = sv_var(a);
  SValue s_b = sv_var(b);
  SValue s_c = sv_var(c);
  s_a.type.t = VT_LLONG;
  s_b.type.t = VT_LLONG;
  s_c.type.t = VT_LLONG;

  SValue s_l = sv_const(0x12345678);
  SValue s_r = sv_const(0x9abcdef0);
  s_l.type.t = VT_LLONG;
  s_r.type.t = VT_LLONG;

  tcc_ir_put(ir, TCCIR_OP_ASSIGN, &s_l, NULL, &s_a);
  tcc_ir_put(ir, TCCIR_OP_ASSIGN, &s_r, NULL, &s_b);
  tcc_ir_put(ir, TCCIR_OP_ADD, &s_a, &s_b, &s_c);
  tcc_ir_put(ir, TCCIR_OP_RETURNVALUE, &s_c, NULL, NULL);

  tcc_ir_ssa_regalloc(ir, arm_get_regalloc_target(), 0);

  int add_idx = -1;
  for (int i = 0; i < ir->next_instruction_index; i++)
  {
    if (ir->compact_instructions[i].op == TCCIR_OP_ADD)
    {
      add_idx = i;
      break;
    }
  }
  UT_ASSERT(add_idx >= 0);

  IRQuadCompact *q = &ir->compact_instructions[add_idx];
  IROperand d = tcc_ir_codegen_dest_get(ir, q);
  MachineOperand md = machine_op_from_ir(ir, &d);

  UT_ASSERT(md.is_64bit);
  /* Either spilled or allocated to an even register pair. */
  if (md.kind == MACH_OP_REG)
  {
    UT_ASSERT(md.u.reg.r1 != PREG_NONE);
    UT_ASSERT((md.u.reg.r0 & 1) == 0);
  }

  tcc_ir_free(ir);
  return 0;
}

/* -------------------------------------------------------------------------- */
/* Suite                                                                      */
/* -------------------------------------------------------------------------- */

UT_SUITE(codegen_arith)
{
  UT_RUN(test_codegen_arith_accessors);
  UT_RUN(test_arith_immediate_and_register_operands);
  UT_RUN(test_arith_op_family_lowering);
  UT_RUN(test_arith_64bit_pair);
}
