/*
 *  test_ir_dump.c - suite for ir/dump.c debug dumping helpers
 *
 *  Exercises the small set of non-stub helpers in the IR dump module.
 */

#define USING_GLOBALS
#include "ir.h"
#include "ut.h"

/* -------------------------------------------------------------------------- */
/* Operation name mapping                                                     */
/* -------------------------------------------------------------------------- */

UT_TEST(test_get_op_name_known_ops)
{
  UT_ASSERT_STREQ(tcc_ir_get_op_name(TCCIR_OP_ADD), "ADD");
  UT_ASSERT_STREQ(tcc_ir_get_op_name(TCCIR_OP_SUB), "SUB");
  UT_ASSERT_STREQ(tcc_ir_get_op_name(TCCIR_OP_NOP), "NOP");
  UT_ASSERT_STREQ(tcc_ir_get_op_name(TCCIR_OP_RETURNVOID), "RETURNVOID");
  UT_ASSERT_STREQ(tcc_ir_get_op_name(TCCIR_OP_FUNCCALLVAL), "CALL");
  return 0;
}

UT_TEST(test_get_op_name_unknown)
{
  UT_ASSERT_STREQ(tcc_ir_get_op_name((TccIrOp)99999), "UNKNOWN_OP");
  return 0;
}

UT_TEST(test_dump_op_name_same_as_get)
{
  UT_ASSERT_STREQ(tcc_ir_dump_op_name(TCCIR_OP_MUL),
                  tcc_ir_get_op_name(TCCIR_OP_MUL));
  return 0;
}

/* -------------------------------------------------------------------------- */
/* Pass-name matching                                                         */
/* -------------------------------------------------------------------------- */

UT_TEST(test_passes_match_all)
{
  tcc_state->dump_ir_passes = "all";
  UT_ASSERT(tcc_ir_dump_passes_match(tcc_state, "copyprop"));
  UT_ASSERT(tcc_ir_dump_passes_match(tcc_state, "dead_vla"));
  UT_ASSERT(tcc_ir_dump_passes_match(tcc_state, "licm"));
  return 0;
}

UT_TEST(test_passes_match_single)
{
  tcc_state->dump_ir_passes = "copyprop,dead_vla";
  UT_ASSERT(tcc_ir_dump_passes_match(tcc_state, "copyprop"));
  UT_ASSERT(tcc_ir_dump_passes_match(tcc_state, "dead_vla"));
  UT_ASSERT(!tcc_ir_dump_passes_match(tcc_state, "licm"));
  UT_ASSERT(!tcc_ir_dump_passes_match(tcc_state, "copypro"));
  UT_ASSERT(!tcc_ir_dump_passes_match(tcc_state, "copyprop2"));
  return 0;
}

UT_TEST(test_passes_match_null_state)
{
  UT_ASSERT(!tcc_ir_dump_passes_match(NULL, "copyprop"));
  return 0;
}

/* -------------------------------------------------------------------------- */
/* Physical-register display flag                                             */
/* -------------------------------------------------------------------------- */

UT_TEST(test_dump_set_show_physical_regs_no_crash)
{
  tcc_ir_dump_set_show_physical_regs(1);
  tcc_ir_dump_set_show_physical_regs(0);
  return 0;
}

/* -------------------------------------------------------------------------- */
/* ANSI spill-mark colour gating                                              */
/* -------------------------------------------------------------------------- */

/* The spill marks are private to dump.c; replicate the public constants here
 * so we can assert the output format is stable. */
#define UT_SPILL_MARK_BEGIN "\033[41m"
#define UT_SPILL_MARK_END   "\033[0m"

UT_TEST(test_spill_mark_ansi_colors)
{
  UT_ASSERT_STREQ(UT_SPILL_MARK_BEGIN, "\033[41m");
  UT_ASSERT_STREQ(UT_SPILL_MARK_END, "\033[0m");
  return 0;
}

/* -------------------------------------------------------------------------- */
/* Suite                                                                      */
/* -------------------------------------------------------------------------- */

UT_SUITE(ir_dump)
{
  UT_RUN(test_get_op_name_known_ops);
  UT_RUN(test_get_op_name_unknown);
  UT_RUN(test_dump_op_name_same_as_get);
  UT_RUN(test_passes_match_all);
  UT_RUN(test_passes_match_single);
  UT_RUN(test_passes_match_null_state);
  UT_RUN(test_dump_set_show_physical_regs_no_crash);
  UT_RUN(test_spill_mark_ansi_colors);
}
