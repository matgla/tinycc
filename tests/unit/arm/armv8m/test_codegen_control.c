/*
 *  test_codegen_control.c - backend unit tests for control-flow IR ops
 *
 *  Exercises JUMP/JUMPIF/IJUMP operand accessors, switch-table layout helpers,
 *  and basic-block marking in ir/codegen.c.
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

static SValue sv_jump_target(int target_idx)
{
  SValue sv;
  svalue_init(&sv);
  sv.r = VT_CONST;
  sv.c.i = target_idx;
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
/* JUMPIF operand layout                                                       */
/* -------------------------------------------------------------------------- */

UT_TEST(test_jumpif_operands)
{
  TCCIRState *ir = tcc_ir_alloc();
  setup_tcc_state();

  int cond = tcc_ir_vreg_alloc_temp(ir);
  SValue s_cond = sv_var(cond);
  SValue s_one = sv_const(1);
  tcc_ir_put(ir, TCCIR_OP_ASSIGN, &s_one, NULL, &s_cond);

  SValue jelse = sv_jump_target(5);
  int jif = tcc_ir_put(ir, TCCIR_OP_JUMPIF, &s_cond, NULL, &jelse);
  tcc_ir_put(ir, TCCIR_OP_RETURNVALUE, &s_cond, NULL, NULL);

  IRQuadCompact *q = &ir->compact_instructions[jif];
  UT_ASSERT_EQ(q->op, TCCIR_OP_JUMPIF);

  IROperand src = tcc_ir_codegen_src1_get(ir, q);
  IROperand dst = tcc_ir_codegen_dest_get(ir, q);

  UT_ASSERT(irop_has_vreg(src));
  UT_ASSERT(irop_is_immediate(dst));
  UT_ASSERT_EQ(irop_get_imm32(dst), 5);

  tcc_ir_free(ir);
  return 0;
}

/* -------------------------------------------------------------------------- */
/* JUMP / IJUMP operand layout                                                 */
/* -------------------------------------------------------------------------- */

UT_TEST(test_jump_and_ijump_operands)
{
  TCCIRState *ir = tcc_ir_alloc();
  setup_tcc_state();

  int target = tcc_ir_vreg_alloc_temp(ir);
  SValue s_target = sv_var(target);
  SValue s_seven = sv_const(7);
  tcc_ir_put(ir, TCCIR_OP_ASSIGN, &s_seven, NULL, &s_target);

  SValue jend = sv_jump_target(9);
  int j = tcc_ir_put(ir, TCCIR_OP_JUMP, NULL, NULL, &jend);
  int ij = tcc_ir_put(ir, TCCIR_OP_IJUMP, &s_target, NULL, NULL);

  tcc_ir_put(ir, TCCIR_OP_RETURNVALUE, &s_target, NULL, NULL);

  IRQuadCompact *qj = &ir->compact_instructions[j];
  IRQuadCompact *qij = &ir->compact_instructions[ij];

  UT_ASSERT_EQ(qj->op, TCCIR_OP_JUMP);
  UT_ASSERT_EQ(qij->op, TCCIR_OP_IJUMP);

  IROperand jdst = tcc_ir_codegen_dest_get(ir, qj);
  IROperand ijsrc = tcc_ir_codegen_src1_get(ir, qij);

  UT_ASSERT(irop_is_immediate(jdst));
  UT_ASSERT_EQ(irop_get_imm32(jdst), 9);
  UT_ASSERT(irop_has_vreg(ijsrc));

  tcc_ir_free(ir);
  return 0;
}

/* -------------------------------------------------------------------------- */
/* Diamond CFG: JUMPIF + JUMP backpatching                                     */
/* -------------------------------------------------------------------------- */

UT_TEST(test_diamond_backpatch)
{
  TCCIRState *ir = tcc_ir_alloc();
  setup_tcc_state();

  int v = tcc_ir_vreg_alloc_temp(ir);
  SValue s_v = sv_var(v);
  SValue s_one = sv_const(1);
  SValue s_ten = sv_const(10);
  SValue s_twenty = sv_const(20);
  tcc_ir_put(ir, TCCIR_OP_ASSIGN, &s_one, NULL, &s_v);

  SValue jelse = sv_jump_target(-1);
  int branch = tcc_ir_put(ir, TCCIR_OP_JUMPIF, &s_v, NULL, &jelse);

  int then_val = tcc_ir_vreg_alloc_temp(ir);
  SValue s_then = sv_var(then_val);
  tcc_ir_put(ir, TCCIR_OP_ASSIGN, &s_ten, NULL, &s_then);

  SValue jmerge = sv_jump_target(-1);
  int skip = tcc_ir_put(ir, TCCIR_OP_JUMP, NULL, NULL, &jmerge);

  int else_label = ir->next_instruction_index;
  int else_val = tcc_ir_vreg_alloc_temp(ir);
  SValue s_else = sv_var(else_val);
  tcc_ir_put(ir, TCCIR_OP_ASSIGN, &s_twenty, NULL, &s_else);

  int merge_label = ir->next_instruction_index;
  tcc_ir_put(ir, TCCIR_OP_RETURNVALUE, &s_v, NULL, NULL);

  tcc_ir_codegen_backpatch(ir, branch, else_label);
  tcc_ir_codegen_backpatch(ir, skip, merge_label);

  UT_ASSERT_EQ(tcc_ir_op_get_dest(ir, &ir->compact_instructions[branch]).u.imm32, else_label);
  UT_ASSERT_EQ(tcc_ir_op_get_dest(ir, &ir->compact_instructions[skip]).u.imm32, merge_label);

  tcc_ir_free(ir);
  return 0;
}

/* -------------------------------------------------------------------------- */
/* Basic block start marker                                                    */
/* -------------------------------------------------------------------------- */

UT_TEST(test_bb_start)
{
  TCCIRState *ir = tcc_ir_alloc();
  setup_tcc_state();

  int before = ir->basic_block_start;
  tcc_ir_codegen_bb_start(ir);
  UT_ASSERT_EQ(ir->basic_block_start, 1);
  (void)before;

  tcc_ir_free(ir);
  return 0;
}

/* -------------------------------------------------------------------------- */
/* SWITCH_TABLE / SWITCH_LOAD operand layout                                   */
/* -------------------------------------------------------------------------- */

UT_TEST(test_switch_operands)
{
  TCCIRState *ir = tcc_ir_alloc();
  setup_tcc_state();

  int idx = tcc_ir_vreg_alloc_temp(ir);
  SValue s_idx = sv_var(idx);
  SValue s_zero = sv_const(0);
  tcc_ir_put(ir, TCCIR_OP_ASSIGN, &s_zero, NULL, &s_idx);

  /* SWITCH_TABLE: dest = table address, src1 = index. */
  int pool_base = ir->iroperand_pool_count;
  tcc_ir_pool_add(ir, irop_make_vreg(tcc_ir_vreg_alloc_temp(ir), IROP_BTYPE_INT32));
  tcc_ir_pool_add(ir, irop_make_vreg(idx, IROP_BTYPE_INT32));

  int st_idx = ir->next_instruction_index;
  IRQuadCompact *qst = &ir->compact_instructions[st_idx];
  qst->op = TCCIR_OP_SWITCH_TABLE;
  qst->operand_base = pool_base;
  ir->next_instruction_index++;

  /* SWITCH_LOAD: dest = value, src1 = table address. */
  int pool_base2 = ir->iroperand_pool_count;
  tcc_ir_pool_add(ir, irop_make_vreg(tcc_ir_vreg_alloc_temp(ir), IROP_BTYPE_INT32));
  tcc_ir_pool_add(ir, irop_make_vreg(tcc_ir_vreg_alloc_temp(ir), IROP_BTYPE_INT32));

  int sl_idx = ir->next_instruction_index;
  IRQuadCompact *qsl = &ir->compact_instructions[sl_idx];
  qsl->op = TCCIR_OP_SWITCH_LOAD;
  qsl->operand_base = pool_base2;
  ir->next_instruction_index++;

  tcc_ir_put(ir, TCCIR_OP_RETURNVALUE, &s_idx, NULL, NULL);

  tcc_ir_ssa_regalloc(ir, arm_get_regalloc_target(), 0);

  IRQuadCompact *qst2 = &ir->compact_instructions[st_idx];
  IRQuadCompact *qsl2 = &ir->compact_instructions[sl_idx];

  UT_ASSERT_EQ(qst2->op, TCCIR_OP_SWITCH_TABLE);
  UT_ASSERT_EQ(qsl2->op, TCCIR_OP_SWITCH_LOAD);

  IROperand st_src = tcc_ir_codegen_src1_get(ir, qst2);
  IROperand sl_src = tcc_ir_codegen_src1_get(ir, qsl2);
  MachineOperand mst_src = machine_op_from_ir(ir, &st_src);
  MachineOperand msl_src = machine_op_from_ir(ir, &sl_src);

  UT_ASSERT(mst_src.kind == MACH_OP_REG);
  UT_ASSERT(msl_src.kind == MACH_OP_REG);

  tcc_ir_free(ir);
  return 0;
}

/* -------------------------------------------------------------------------- */
/* Suite                                                                      */
/* -------------------------------------------------------------------------- */

UT_SUITE(codegen_control)
{
  UT_RUN(test_jumpif_operands);
  UT_RUN(test_jump_and_ijump_operands);
  UT_RUN(test_diamond_backpatch);
  UT_RUN(test_bb_start);
  UT_RUN(test_switch_operands);
}
