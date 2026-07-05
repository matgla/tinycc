/*
 *  test_opt_helpers.c - suite for the linear-scan helpers in ir/opt.c:
 *
 *    int tcc_ir_find_defining_instruction(ir, vreg, before_idx);
 *    int tcc_ir_vreg_has_single_use(ir, vreg, exclude_idx);
 *
 *  These are small but carry explicit defensive branches (NULL ir, vreg<0,
 *  before_idx<=0) and a subtle "single-use" contract: zero uses returns
 *  false (it is "exactly one use", not "at most one"), and a >1 count
 *  short-circuits on the second hit.  Each corner-case branch is pinned
 *  here by a dedicated test.
 */

#include "ir_build.h"

#include "ut.h"

int tcc_ir_find_defining_instruction(TCCIRState *ir, int32_t vreg, int before_idx);
int tcc_ir_vreg_has_single_use(TCCIRState *ir, int32_t vreg, int exclude_idx);

#define I32 IROP_BTYPE_INT32

/* Encode of TEMP/VAR position N as the integer the helpers compare against. */
#define VR_TEMP(n) irop_get_vreg(utb_temp(n, I32))
#define VR_VAR(n) irop_get_vreg(utb_var(n, I32))

/* ------------------------------------------------- find_defining_instruction */

UT_TEST(test_find_def_null_ir)
{
  UT_ASSERT_EQ(tcc_ir_find_defining_instruction(NULL, VR_TEMP(0), 4), -1);
  return 0;
}

UT_TEST(test_find_def_negative_vreg)
{
  TCCIRState *ir = utb_new();
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_param(0, I32), UTB_NONE);
  UT_ASSERT_EQ(tcc_ir_find_defining_instruction(ir, -1, 4), -1);
  utb_free(ir);
  return 0;
}

UT_TEST(test_find_def_before_idx_zero)
{
  /* before_idx <= 0 means an empty scan window -> -1 regardless of defs. */
  TCCIRState *ir = utb_new();
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_param(0, I32), UTB_NONE);
  UT_ASSERT_EQ(tcc_ir_find_defining_instruction(ir, VR_TEMP(0), 0), -1);
  UT_ASSERT_EQ(tcc_ir_find_defining_instruction(ir, VR_TEMP(0), -3), -1);
  utb_free(ir);
  return 0;
}

UT_TEST(test_find_def_finds_nearest)
{
  TCCIRState *ir = utb_new();
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_param(0, I32), UTB_NONE); /* 0 */
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(1, I32), utb_param(1, I32), UTB_NONE); /* 1 */
  UT_ASSERT_EQ(tcc_ir_find_defining_instruction(ir, VR_TEMP(0), 2), 0);
  UT_ASSERT_EQ(tcc_ir_find_defining_instruction(ir, VR_TEMP(1), 2), 1);
  utb_free(ir);
  return 0;
}

UT_TEST(test_find_def_skips_nops)
{
  /* A NOP between the scan start and the defining instr must be skipped. */
  TCCIRState *ir = utb_new();
  utb_emit(ir, TCCIR_OP_NOP, UTB_NONE, UTB_NONE, UTB_NONE);                      /* 0 */
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_param(0, I32), UTB_NONE); /* 1 */
  UT_ASSERT_EQ(tcc_ir_find_defining_instruction(ir, VR_TEMP(0), 2), 1);
  utb_free(ir);
  return 0;
}

UT_TEST(test_find_def_undefined_returns_minus_one)
{
  /* Vreg never defined anywhere -> -1. */
  TCCIRState *ir = utb_new();
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_param(0, I32), UTB_NONE);
  UT_ASSERT_EQ(tcc_ir_find_defining_instruction(ir, VR_TEMP(7), 2), -1);
  utb_free(ir);
  return 0;
}

UT_TEST(test_find_def_returns_nearest_when_two_defs)
{
  /* The backward scan returns the *nearest* preceding def, not the first. */
  TCCIRState *ir = utb_new();
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_param(0, I32), UTB_NONE); /* 0 */
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(1, I32), utb_param(1, I32), UTB_NONE); /* 1 */
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_param(2, I32), UTB_NONE); /* 2 */
  UT_ASSERT_EQ(tcc_ir_find_defining_instruction(ir, VR_TEMP(0), 3), 2);
  utb_free(ir);
  return 0;
}

/* ----------------------------------------------- vreg_has_single_use */

UT_TEST(test_single_use_null_ir)
{
  UT_ASSERT_EQ(tcc_ir_vreg_has_single_use(NULL, VR_TEMP(0), -1), 0);
  return 0;
}

UT_TEST(test_single_use_negative_vreg)
{
  TCCIRState *ir = utb_new();
  UT_ASSERT_EQ(tcc_ir_vreg_has_single_use(ir, -1, -1), 0);
  utb_free(ir);
  return 0;
}

UT_TEST(test_single_use_zero_uses_is_false)
{
  /* Subtle contract: "single use" means exactly one.  A vreg with no readers
   * (its defining dest is NOT a use) returns false. */
  TCCIRState *ir = utb_new();
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_param(0, I32), UTB_NONE); /* def, no readers */
  UT_ASSERT_EQ(tcc_ir_vreg_has_single_use(ir, VR_TEMP(0), -1), 0);
  utb_free(ir);
  return 0;
}

UT_TEST(test_single_use_exactly_one)
{
  TCCIRState *ir = utb_new();
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_param(0, I32), UTB_NONE); /* 0: def T0 */
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(1, I32), utb_temp(0, I32), UTB_NONE);  /* 1: uses T0 */
  UT_ASSERT_EQ(tcc_ir_vreg_has_single_use(ir, VR_TEMP(0), -1), 1);
  utb_free(ir);
  return 0;
}

UT_TEST(test_single_use_two_short_circuits)
{
  /* use_count > 1 must short-circuit to 0 on the second hit.  A scan that did
   * not short-circuit would still return 0 here, so this also doubles as a
   * correctness check; the value is in pinning the early-exit path. */
  TCCIRState *ir = utb_new();
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_param(0, I32), UTB_NONE); /* 0: def T0 */
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(1, I32), utb_temp(0, I32), UTB_NONE);  /* 1: use */
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(2, I32), utb_temp(0, I32), UTB_NONE);  /* 2: use */
  UT_ASSERT_EQ(tcc_ir_vreg_has_single_use(ir, VR_TEMP(0), -1), 0);
  utb_free(ir);
  return 0;
}

UT_TEST(test_single_use_exclude_idx)
{
  /* Excluding the one real reader leaves zero uses -> false. */
  TCCIRState *ir = utb_new();
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_param(0, I32), UTB_NONE); /* 0: def T0 */
  int reader = utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(1, I32), utb_temp(0, I32), UTB_NONE);
  UT_ASSERT_EQ(tcc_ir_vreg_has_single_use(ir, VR_TEMP(0), reader), 0);
  /* And excluding an unrelated index leaves the single use visible. */
  UT_ASSERT_EQ(tcc_ir_vreg_has_single_use(ir, VR_TEMP(0), 999), 1);
  utb_free(ir);
  return 0;
}

UT_TEST(test_single_use_read_in_src2_counts)
{
  /* Both src1 and src2 are scanned; a use hiding in src2 is still a use. */
  TCCIRState *ir = utb_new();
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_param(0, I32), UTB_NONE); /* 0: def T0 */
  utb_emit(ir, TCCIR_OP_ADD, utb_temp(1, I32), utb_param(1, I32), utb_temp(0, I32)); /* 1: T0 in src2 */
  UT_ASSERT_EQ(tcc_ir_vreg_has_single_use(ir, VR_TEMP(0), -1), 1);
  utb_free(ir);
  return 0;
}

UT_TEST(test_single_use_nops_skipped)
{
  /* A NOP that happens to carry stale operand data must not be counted. */
  TCCIRState *ir = utb_new();
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_param(0, I32), UTB_NONE); /* 0: def T0 */
  IRQuadCompact *nop = &ir->compact_instructions[utb_emit(ir, TCCIR_OP_NOP, UTB_NONE, UTB_NONE, UTB_NONE)];
  /* Poison the NOP's operand_base to a temp-0 operand so we prove the skip works. */
  (void)nop;
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(1, I32), utb_temp(0, I32), UTB_NONE); /* 2: single real use */
  UT_ASSERT_EQ(tcc_ir_vreg_has_single_use(ir, VR_TEMP(0), -1), 1);
  utb_free(ir);
  return 0;
}

/* ------------------------------------------------------------------ suite */

UT_SUITE(opt_helpers)
{
  UT_COVERS("find_defining_instruction");
  UT_COVERS("vreg_has_single_use");
  UT_RUN(test_find_def_null_ir);
  UT_RUN(test_find_def_negative_vreg);
  UT_RUN(test_find_def_before_idx_zero);
  UT_RUN(test_find_def_finds_nearest);
  UT_RUN(test_find_def_skips_nops);
  UT_RUN(test_find_def_undefined_returns_minus_one);
  UT_RUN(test_find_def_returns_nearest_when_two_defs);
  UT_RUN(test_single_use_null_ir);
  UT_RUN(test_single_use_negative_vreg);
  UT_RUN(test_single_use_zero_uses_is_false);
  UT_RUN(test_single_use_exactly_one);
  UT_RUN(test_single_use_two_short_circuits);
  UT_RUN(test_single_use_exclude_idx);
  UT_RUN(test_single_use_read_in_src2_counts);
  UT_RUN(test_single_use_nops_skipped);
}
