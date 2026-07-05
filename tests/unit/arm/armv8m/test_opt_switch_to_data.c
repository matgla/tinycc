/*
 *  test_opt_switch_to_data.c - suite for ir/opt_switch_data.c:switch_to_data
 *
 *  The pass rewrites a SWITCH_TABLE whose every case body is a single
 *  "ASSIGN dest <- const; JUMP merge" (same dest, same merge across all
 *  cases) into a SWITCH_LOAD backed by a TCCIRSwitchValueTable materialized
 *  into a real Section via elfsec_stubs.c (see that file for why the
 *  section/symbol layer is real-for-bytes but call-logged for
 *  symbols/relocations).
 *
 *  These tests build IR by hand (mirroring test_opt_switch_collapse.c) and
 *  inspect both the rewritten instruction stream and the materialized
 *  .rodata/.data bytes.
 */

#include "elfsec_stubs.h"
#include "ir_build.h"

#include "ut.h"

/* Pass entry point (defined in ir/opt_switch_data.c). */
int tcc_ir_opt_switch_to_data(TCCIRState *ir);

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

/* Emit a case body: `ASSIGN dest_temp_pos <- imm_val; JUMP merge`. Returns
 * the ASSIGN's IR index (== the case target to record in targets[]). */
static int emit_case_body_imm(TCCIRState *ir, int dest_temp_pos, int32_t imm_val, int merge)
{
  int assign_idx = utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(dest_temp_pos, I32), utb_imm(imm_val, I32), UTB_NONE);
  emit_jump(ir, merge);
  return assign_idx;
}

/* Emit a case body whose source is a SYMREF constant instead of an IMM32. */
static int emit_case_body_symref(TCCIRState *ir, int dest_temp_pos, Sym *sym, int merge)
{
  IROperand src = utb_symref(ir, sym, 0, 0, 0, I32);
  int assign_idx = utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(dest_temp_pos, I32), src, UTB_NONE);
  emit_jump(ir, merge);
  return assign_idx;
}

static int emit_return_value(TCCIRState *ir, int32_t val)
{
  return utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_imm(val, I32), UTB_NONE);
}

static uint32_t read_le32(const unsigned char *p)
{
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* ---------------------------------------------------------------- basic rewrite */

UT_TEST(test_switch_to_data_basic_rewrite)
{
  elfsec_reset();
  rodata_section = elfsec_new_section(".rodata");

  TCCIRState *ir = utb_new();
  ir->iroperand_pool_capacity = UTB_MAX_OPERANDS; /* pure-IMM32 case: no symref pool needed */

  int targets[2];
  int sw = emit_switch_table(ir, 0);   /* 0 */
  targets[0] = emit_case_body_imm(ir, 1, 100, 5);   /* 1: V1<-100; 2: JUMP 5 */
  targets[1] = emit_case_body_imm(ir, 1, 200, 5);   /* 3: V1<-200; 4: JUMP 5 */
  emit_return_value(ir, 0);            /* 5 (merge) */

  TCCIRSwitchTable tables[1];
  tables[0].min_val = 0;
  tables[0].max_val = 1;
  tables[0].default_target = 5;
  tables[0].targets = targets;
  tables[0].num_entries = 2;
  tables[0].table_code_addr = 0;
  ir->switch_tables = tables;
  ir->num_switch_tables = 1;

  int changes = tcc_ir_opt_switch_to_data(ir);

  UT_ASSERT_EQ(changes, 1);
  UT_ASSERT_EQ(utb_op(ir, sw), TCCIR_OP_SWITCH_LOAD);
  /* Both case bodies (ASSIGN+JUMP) are NOPed -- neither shares the default target. */
  UT_ASSERT_EQ(utb_op(ir, 1), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, 2), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, 3), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, 4), TCCIR_OP_NOP);

  UT_ASSERT_EQ(ir->num_switch_value_tables, 1);
  UT_ASSERT_EQ(ir->switch_value_tables[0].num_entries, 2);

  /* Exact bytes materialized into .rodata, little-endian. */
  UT_ASSERT_EQ(read_le32(rodata_section->data + 0), 100u);
  UT_ASSERT_EQ(read_le32(rodata_section->data + 4), 200u);

  UT_ASSERT_EQ(elfsec_sym_ref_call_count(), 1);
  const ElfSecSymRefCall *sc = elfsec_nth_sym_ref_call(0);
  UT_ASSERT(sc != NULL);
  UT_ASSERT_EQ((long long)sc->offset, 0);
  UT_ASSERT_EQ((long long)sc->size, 8);
  UT_ASSERT_EQ(sc->sec == rodata_section, 1);
  UT_ASSERT_EQ(elfsec_reloc_call_count(), 0);

  utb_free(ir);
  return 0;
}

/* ---------------------------------------------------------------- SYMREF values */

UT_TEST(test_switch_to_data_symref_value_default_rodata)
{
  elfsec_reset();
  rodata_section = elfsec_new_section(".rodata");
  data_section = elfsec_new_section(".data");
  tcc_state->share_rodata = 0;

  TCCIRState *ir = utb_new();
  utb_pools_init(ir);

  static Sym callee;
  int targets[1];
  int sw = emit_switch_table(ir, 0);                       /* 0 */
  targets[0] = emit_case_body_symref(ir, 1, &callee, 3);   /* 1: V1<-&callee; 2: JUMP 3 */
  emit_return_value(ir, 0);                                 /* 3 (merge) */

  TCCIRSwitchTable tables[1];
  tables[0].min_val = 0;
  tables[0].max_val = 0;
  tables[0].default_target = 3;
  tables[0].targets = targets;
  tables[0].num_entries = 1;
  tables[0].table_code_addr = 0;
  ir->switch_tables = tables;
  ir->num_switch_tables = 1;

  int changes = tcc_ir_opt_switch_to_data(ir);

  UT_ASSERT_EQ(changes, 1);
  UT_ASSERT_EQ(utb_op(ir, sw), TCCIR_OP_SWITCH_LOAD);

  UT_ASSERT_EQ(elfsec_reloc_call_count(), 1);
  const ElfSecRelocCall *rc = elfsec_nth_reloc_call(0);
  UT_ASSERT(rc != NULL);
  UT_ASSERT_EQ(rc->sec == rodata_section, 1);
  UT_ASSERT_EQ(rc->sym == &callee, 1);
  UT_ASSERT_EQ(rc->type, R_ARM_ABS32);

  const ElfSecSymRefCall *sc = elfsec_nth_sym_ref_call(0);
  UT_ASSERT(sc != NULL);
  UT_ASSERT_EQ(sc->sec == rodata_section, 1);

  utb_free(ir);
  return 0;
}

UT_TEST(test_switch_to_data_symref_value_share_rodata_uses_data)
{
  elfsec_reset();
  rodata_section = elfsec_new_section(".rodata");
  data_section = elfsec_new_section(".data");
  tcc_state->share_rodata = 1;

  TCCIRState *ir = utb_new();
  utb_pools_init(ir);

  static Sym callee;
  int targets[1];
  emit_switch_table(ir, 0);                                /* 0 */
  targets[0] = emit_case_body_symref(ir, 1, &callee, 3);   /* 1,2 */
  emit_return_value(ir, 0);                                 /* 3 */

  TCCIRSwitchTable tables[1];
  tables[0].min_val = 0;
  tables[0].max_val = 0;
  tables[0].default_target = 3;
  tables[0].targets = targets;
  tables[0].num_entries = 1;
  tables[0].table_code_addr = 0;
  ir->switch_tables = tables;
  ir->num_switch_tables = 1;

  int changes = tcc_ir_opt_switch_to_data(ir);

  UT_ASSERT_EQ(changes, 1);
  UT_ASSERT_EQ(elfsec_sym_ref_call_count(), 1);
  const ElfSecSymRefCall *sc = elfsec_nth_sym_ref_call(0);
  UT_ASSERT(sc != NULL);
  UT_ASSERT_EQ(sc->sec == data_section, 1);
  const ElfSecRelocCall *rc = elfsec_nth_reloc_call(0);
  UT_ASSERT(rc != NULL);
  UT_ASSERT_EQ(rc->sec == data_section, 1);

  utb_free(ir);
  return 0;
}

/* ---------------------------------------------------------------- label sharing */

/* Case 0's target IS the table's default_target: its ASSIGN+JUMP body must
 * survive un-NOPed since the out-of-range dispatch edge still branches to
 * it. Case 1's non-shared body is NOPed as usual. */
UT_TEST(test_switch_to_data_default_shared_body_preserved)
{
  elfsec_reset();
  rodata_section = elfsec_new_section(".rodata");

  TCCIRState *ir = utb_new();
  ir->iroperand_pool_capacity = UTB_MAX_OPERANDS;

  int targets[2];
  int sw = emit_switch_table(ir, 0);                     /* 0 */
  targets[0] = emit_case_body_imm(ir, 1, 100, 5);        /* 1,2: shared w/ default */
  targets[1] = emit_case_body_imm(ir, 1, 200, 5);        /* 3,4 */
  emit_return_value(ir, 0);                               /* 5 */

  TCCIRSwitchTable tables[1];
  tables[0].min_val = 0;
  tables[0].max_val = 1;
  tables[0].default_target = targets[0]; /* case 0 doubles as the default */
  tables[0].targets = targets;
  tables[0].num_entries = 2;
  tables[0].table_code_addr = 0;
  ir->switch_tables = tables;
  ir->num_switch_tables = 1;

  int changes = tcc_ir_opt_switch_to_data(ir);

  UT_ASSERT_EQ(changes, 1);
  UT_ASSERT_EQ(utb_op(ir, sw), TCCIR_OP_SWITCH_LOAD);
  /* Case 0's body (the shared default) survives. */
  UT_ASSERT_EQ(utb_op(ir, 1), TCCIR_OP_ASSIGN);
  UT_ASSERT_EQ(utb_op(ir, 2), TCCIR_OP_JUMP);
  /* Case 1's body is NOPed as usual. */
  UT_ASSERT_EQ(utb_op(ir, 3), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, 4), TCCIR_OP_NOP);

  utb_free(ir);
  return 0;
}

/* ---------------------------------------------------------------- guards: no rewrite */

/* An extra ADD between the ASSIGN and the JUMP breaks the shape match for
 * that one case -- the WHOLE table must be left untouched (all-or-nothing). */
UT_TEST(test_switch_to_data_non_matching_body_not_rewritten)
{
  elfsec_reset();
  rodata_section = elfsec_new_section(".rodata");

  TCCIRState *ir = utb_new();
  ir->iroperand_pool_capacity = UTB_MAX_OPERANDS;

  int targets[2];
  int sw = emit_switch_table(ir, 0);                              /* 0 */
  targets[0] = utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(1, I32), utb_imm(100, I32), UTB_NONE); /* 1 */
  utb_emit(ir, TCCIR_OP_ADD, utb_temp(2, I32), utb_temp(1, I32), utb_imm(1, I32));            /* 2: extra op */
  emit_jump(ir, 6);                                                                            /* 3 */
  targets[1] = emit_case_body_imm(ir, 1, 200, 6);                 /* 4,5 */
  emit_return_value(ir, 0);                                        /* 6 */

  TCCIRSwitchTable tables[1];
  tables[0].min_val = 0;
  tables[0].max_val = 1;
  tables[0].default_target = 6;
  tables[0].targets = targets;
  tables[0].num_entries = 2;
  tables[0].table_code_addr = 0;
  ir->switch_tables = tables;
  ir->num_switch_tables = 1;

  int changes = tcc_ir_opt_switch_to_data(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, sw), TCCIR_OP_SWITCH_TABLE);
  UT_ASSERT_EQ(utb_op(ir, 1), TCCIR_OP_ASSIGN);
  UT_ASSERT_EQ(utb_op(ir, 2), TCCIR_OP_ADD);

  utb_free(ir);
  return 0;
}

/* Case bodies write to different dest vregs -> no common destination. */
UT_TEST(test_switch_to_data_dest_mismatch_not_rewritten)
{
  elfsec_reset();
  rodata_section = elfsec_new_section(".rodata");

  TCCIRState *ir = utb_new();
  ir->iroperand_pool_capacity = UTB_MAX_OPERANDS;

  int targets[2];
  int sw = emit_switch_table(ir, 0);              /* 0 */
  targets[0] = emit_case_body_imm(ir, 1, 100, 5); /* 1,2: V1 */
  targets[1] = emit_case_body_imm(ir, 2, 200, 5); /* 3,4: V2 (different dest) */
  emit_return_value(ir, 0);                        /* 5 */

  TCCIRSwitchTable tables[1];
  tables[0].min_val = 0;
  tables[0].max_val = 1;
  tables[0].default_target = 5;
  tables[0].targets = targets;
  tables[0].num_entries = 2;
  tables[0].table_code_addr = 0;
  ir->switch_tables = tables;
  ir->num_switch_tables = 1;

  int changes = tcc_ir_opt_switch_to_data(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, sw), TCCIR_OP_SWITCH_TABLE);

  utb_free(ir);
  return 0;
}

/* No switch tables in the IR at all; the pass must report zero changes. */
UT_TEST(test_switch_to_data_no_tables_returns_zero)
{
  elfsec_reset();

  TCCIRState *ir = utb_new();
  ir->iroperand_pool_capacity = UTB_MAX_OPERANDS;

  utb_emit(ir, TCCIR_OP_ADD, utb_temp(0, I32), utb_imm(1, I32), utb_imm(2, I32));
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);

  int changes = tcc_ir_opt_switch_to_data(ir);

  UT_ASSERT_EQ(changes, 0);

  utb_free(ir);
  return 0;
}

/* ------------------------------------------------------------------ suite */

UT_SUITE(opt_switch_to_data)
{
  UT_COVERS("switch_to_data");

  UT_RUN(test_switch_to_data_basic_rewrite);
  UT_RUN(test_switch_to_data_symref_value_default_rodata);
  UT_RUN(test_switch_to_data_symref_value_share_rodata_uses_data);
  UT_RUN(test_switch_to_data_default_shared_body_preserved);
  UT_RUN(test_switch_to_data_non_matching_body_not_rewritten);
  UT_RUN(test_switch_to_data_dest_mismatch_not_rewritten);
  UT_RUN(test_switch_to_data_no_tables_returns_zero);
}
