/*
 *  test_opt_switch_collapse.c - suite for ir/opt_switch_data.c:switch_collapse
 *
 *  The pass collapses a SWITCH_TABLE to NOP when its default target and every
 *  case target resolve (through NOPs and unconditional JUMPs) to the same
 *  control-flow endpoint.  The surrounding CMP+JUMPIF bounds check is then
 *  expected to fold away in later branch passes.
 *
 *  These tests build IR by hand, populate ir->switch_tables[] directly, and
 *  inspect the resulting instruction stream.
 */

#include "ir_build.h"

#include "ut.h"

/* Pass entry point (defined in ir/opt_switch_data.c). */
int tcc_ir_opt_switch_collapse(TCCIRState *ir);

#define I32 IROP_BTYPE_INT32

/* Emit an unconditional JUMP to the given IR index. */
static int emit_jump(TCCIRState *ir, int target)
{
  return utb_emit(ir, TCCIR_OP_JUMP, utb_imm(target, I32), UTB_NONE, UTB_NONE);
}

/* Emit a SWITCH_TABLE using the supplied table id and a dummy index operand. */
static int emit_switch_table(TCCIRState *ir, int table_id)
{
  return utb_emit(ir, TCCIR_OP_SWITCH_TABLE, UTB_NONE,
                  utb_temp(0, I32), utb_imm(table_id, I32));
}

/* Emit a RETURNVALUE carrying an immediate. */
static int emit_return_value(TCCIRState *ir, int32_t val)
{
  return utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE,
                  utb_imm(val, I32), UTB_NONE);
}

/* ---------------------------------------------------------------- positive */

/* All case targets and the default target jump straight to the same merge
 * block.  The SWITCH_TABLE must become a NOP. */
UT_TEST(test_switch_collapse_all_targets_same_merge)
{
  TCCIRState *ir = utb_new();

  int targets[3] = { 1, 2, 3 };
  TCCIRSwitchTable tables[1];
  tables[0].min_val = 0;
  tables[0].max_val = 2;
  tables[0].default_target = 4;
  tables[0].targets = targets;
  tables[0].num_entries = 3;
  tables[0].table_code_addr = 0;
  ir->switch_tables = tables;
  ir->num_switch_tables = 1;

  int sw = emit_switch_table(ir, 0);
  emit_jump(ir, 5);
  emit_jump(ir, 5);
  emit_jump(ir, 5);
  emit_jump(ir, 5);
  emit_return_value(ir, 42);

  int changes = tcc_ir_opt_switch_collapse(ir);

  UT_ASSERT_EQ(changes, 1);
  UT_ASSERT_EQ(utb_op(ir, sw), TCCIR_OP_NOP);

  utb_free(ir);
  return 0;
}

/* Cases reach the same endpoint through NOP chains and unconditional JUMPs. */
UT_TEST(test_switch_collapse_follows_nop_chains)
{
  TCCIRState *ir = utb_new();

  int targets[3] = { 2, 3, 4 };
  TCCIRSwitchTable tables[1];
  tables[0].min_val = 0;
  tables[0].max_val = 2;
  tables[0].default_target = 1;
  tables[0].targets = targets;
  tables[0].num_entries = 3;
  tables[0].table_code_addr = 0;
  ir->switch_tables = tables;
  ir->num_switch_tables = 1;

  int sw = emit_switch_table(ir, 0);
  utb_emit(ir, TCCIR_OP_NOP, UTB_NONE, UTB_NONE, UTB_NONE);          /* 1 */
  emit_jump(ir, 6);                                                  /* 2 */
  emit_jump(ir, 6);                                                  /* 3 */
  utb_emit(ir, TCCIR_OP_NOP, UTB_NONE, UTB_NONE, UTB_NONE);          /* 4 */
  utb_emit(ir, TCCIR_OP_NOP, UTB_NONE, UTB_NONE, UTB_NONE);          /* 5 */
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);   /* 6 */

  int changes = tcc_ir_opt_switch_collapse(ir);

  UT_ASSERT_EQ(changes, 1);
  UT_ASSERT_EQ(utb_op(ir, sw), TCCIR_OP_NOP);

  utb_free(ir);
  return 0;
}

/* ---------------------------------------------------------------- guard */

/* A single case resolves to a different RETURNVALUE from the rest; the
 * SWITCH_TABLE must be left untouched. */
UT_TEST(test_switch_collapse_mismatched_case_preserved)
{
  TCCIRState *ir = utb_new();

  int targets[3] = { 1, 2, 3 };
  TCCIRSwitchTable tables[1];
  tables[0].min_val = 0;
  tables[0].max_val = 2;
  tables[0].default_target = 4;
  tables[0].targets = targets;
  tables[0].num_entries = 3;
  tables[0].table_code_addr = 0;
  ir->switch_tables = tables;
  ir->num_switch_tables = 1;

  int sw = emit_switch_table(ir, 0);
  emit_jump(ir, 5);              /* case 0 -> return 1 */
  emit_jump(ir, 5);              /* case 1 -> return 1 */
  emit_jump(ir, 6);              /* case 2 -> return 2 (different) */
  emit_jump(ir, 5);              /* default -> return 1 */
  emit_return_value(ir, 1);      /* 5 */
  emit_return_value(ir, 2);      /* 6 */

  int changes = tcc_ir_opt_switch_collapse(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, sw), TCCIR_OP_SWITCH_TABLE);

  utb_free(ir);
  return 0;
}

/* No switch tables in the IR at all; the pass must report zero changes. */
UT_TEST(test_switch_collapse_no_tables_returns_zero)
{
  TCCIRState *ir = utb_new();

  utb_emit(ir, TCCIR_OP_ADD, utb_temp(0, I32),
           utb_imm(1, I32), utb_imm(2, I32));
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);

  int changes = tcc_ir_opt_switch_collapse(ir);

  UT_ASSERT_EQ(changes, 0);

  utb_free(ir);
  return 0;
}

UT_COVERS("switch_collapse");
