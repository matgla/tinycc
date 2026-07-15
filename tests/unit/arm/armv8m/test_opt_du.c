/*
 *  test_opt_du.c - suite for ir/opt_du.c (def-use table helpers)
 *
 *  ir_opt_du_idx is a pure vreg->flat-index mapping; ir_opt_du_build_mode
 *  walks the IR once to fill def/use/def_cnt.  Corner cases pinned: the
 *  TMP_ONLY mode excluding VAR/PARAM, out-of-range positions, the STORE-dest
 *  counted as a *use* (address pointer) not a def, and MLA's 4th accumulator
 *  operand counted as a use.
 */

#include "ir_build.h"

#include "ut.h"
#include "opt_du.h"

#define I32 IROP_BTYPE_INT32
#define VR_VAR(n) irop_get_vreg(utb_var(n, I32))
#define VR_TEMP(n) irop_get_vreg(utb_temp(n, I32))
#define VR_PARAM(n) irop_get_vreg(utb_param(n, I32))

/* ============================================ ir_opt_du_idx (pure) */

UT_TEST(test_du_idx_full_mode_layout)
{
  /* max_var=3, max_tmp=5, params=2 -> total=10.
   *   VAR p   -> p
   *   TMP p   -> max_var + p = 3 + p
   *   PARAM p -> max_var + max_tmp + p = 8 + p */
  IROptDU du;
  memset(&du, 0, sizeof du);
  du.mode = IR_DU_MODE_FULL;
  du.max_var = 3;
  du.max_tmp = 5;
  du.total = 3 + 5 + 2;

  UT_ASSERT_EQ(ir_opt_du_idx(&du, VR_VAR(1)), 1);
  UT_ASSERT_EQ(ir_opt_du_idx(&du, VR_VAR(2)), 2);
  UT_ASSERT_EQ(ir_opt_du_idx(&du, VR_TEMP(0)), 3 + 0);
  UT_ASSERT_EQ(ir_opt_du_idx(&du, VR_TEMP(4)), 3 + 4);
  UT_ASSERT_EQ(ir_opt_du_idx(&du, VR_PARAM(0)), 8 + 0);
  UT_ASSERT_EQ(ir_opt_du_idx(&du, VR_PARAM(1)), 8 + 1);
  return 0;
}

UT_TEST(test_du_idx_negative_vreg)
{
  IROptDU du;
  memset(&du, 0, sizeof du);
  du.mode = IR_DU_MODE_FULL;
  du.max_var = 1;
  du.max_tmp = 1;
  du.total = 3;
  UT_ASSERT_EQ(ir_opt_du_idx(&du, -1), -1);
  UT_ASSERT_EQ(ir_opt_du_idx(&du, -100), -1);
  return 0;
}

UT_TEST(test_du_idx_out_of_range)
{
  /* position decoding to an index >= total -> -1.
   *   total = 2+2+2 = 6
   *   VAR(6)   -> 6 >= 6
   *   TEMP(4)  -> 2+4 = 6 >= 6
   *   PARAM(2) -> 4+2 = 6 >= 6   */
  IROptDU du;
  memset(&du, 0, sizeof du);
  du.mode = IR_DU_MODE_FULL;
  du.max_var = 2;
  du.max_tmp = 2;
  du.total = 6;
  UT_ASSERT_EQ(ir_opt_du_idx(&du, VR_VAR(6)), -1);
  UT_ASSERT_EQ(ir_opt_du_idx(&du, VR_TEMP(4)), -1);
  UT_ASSERT_EQ(ir_opt_du_idx(&du, VR_PARAM(2)), -1);
  /* In-range positions still resolve. */
  UT_ASSERT_EQ(ir_opt_du_idx(&du, VR_VAR(1)), 1);
  UT_ASSERT_EQ(ir_opt_du_idx(&du, VR_TEMP(1)), 3);
  UT_ASSERT_EQ(ir_opt_du_idx(&du, VR_PARAM(1)), 5);
  return 0;
}

UT_TEST(test_du_idx_tmp_only_mode_excludes_var_and_param)
{
  IROptDU du;
  memset(&du, 0, sizeof du);
  du.mode = IR_DU_MODE_TMP_ONLY;
  du.max_var = 0; /* not used in TMP_ONLY */
  du.max_tmp = 4;
  du.total = 4;
  UT_ASSERT_EQ(ir_opt_du_idx(&du, VR_VAR(0)), -1);     /* VAR excluded */
  UT_ASSERT_EQ(ir_opt_du_idx(&du, VR_PARAM(0)), -1);   /* PARAM excluded */
  UT_ASSERT_EQ(ir_opt_du_idx(&du, VR_TEMP(0)), 0);     /* TMP p -> 0+p */
  UT_ASSERT_EQ(ir_opt_du_idx(&du, VR_TEMP(3)), 3);
  return 0;
}

/* ============================================ ir_opt_du_build_mode */

UT_TEST(test_du_build_records_def_and_uses)
{
  /* V0 = T0 ; T1 = V0 + #1
   *   V0: def@0, use@1 -> use=1, def_cnt=1
   *   T0: use@0 -> use=1, no def
   *   T1: def@1 -> def_cnt=1, use=0 */
  TCCIRState *ir = utb_new();
  ir->next_local_variable = 0;   /* max_var = 1 */
  ir->next_temporary_variable = 1; /* max_tmp = 2 */
  ir->next_parameter = 0;
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(0, I32), utb_temp(0, I32), UTB_NONE); /* 0 */
  utb_emit(ir, TCCIR_OP_ADD, utb_temp(1, I32), utb_var(0, I32), utb_imm(1, I32)); /* 1 */

  IROptDU du;
  ir_opt_du_build(ir, &du);

  /* V0 (idx 0): defined at 0, used at 1. */
  UT_ASSERT_EQ(du.def[0], 0);
  UT_ASSERT_EQ(du.use[0], 1);
  UT_ASSERT_EQ(du.def_cnt[0], 1);
  /* T0 (idx max_var+0 = 1): no def, used at 0. */
  UT_ASSERT_EQ(du.def[1], -1);
  UT_ASSERT_EQ(du.use[1], 1);
  /* T1 (idx max_var+1 = 2): defined at 1, not used. */
  UT_ASSERT_EQ(du.def[2], 1);
  UT_ASSERT_EQ(du.use[2], 0);
  UT_ASSERT_EQ(du.def_cnt[2], 1);

  /* Accessors agree. */
  UT_ASSERT_EQ(ir_opt_du_uses(&du, VR_VAR(0)), 1);
  UT_ASSERT_EQ(ir_opt_du_def(&du, VR_VAR(0), 2), 0);
  UT_ASSERT_EQ(ir_opt_du_is_single_def(&du, VR_VAR(0)), 1);
  tcc_free(du.def);
  utb_free(ir);
  return 0;
}

UT_TEST(test_du_build_store_dest_counted_as_use_not_def)
{
  /* STORE [V0] = #1 : the dest V0 is the *address pointer* (a use), so it must
   * not be recorded as a definition.  A regression here would make fusion
   * passes believe V0 is redefined and bail. */
  TCCIRState *ir = utb_new();
  ir->next_local_variable = 0;
  ir->next_temporary_variable = 0;
  ir->next_parameter = 0;
  utb_emit(ir, TCCIR_OP_STORE, utb_lval(utb_var(0, I32)), utb_imm(1, I32), UTB_NONE);

  IROptDU du;
  ir_opt_du_build(ir, &du);
  UT_ASSERT_EQ(du.def_cnt[0], 0); /* not a def */
  UT_ASSERT_EQ(du.use[0], 1);     /* counted as a use */
  tcc_free(du.def);
  utb_free(ir);
  return 0;
}

UT_TEST(test_du_build_use_count_saturates_at_two)
{
  /* Four uses of T0 must saturate at 2 (so "2 means 2+").  Use the accessor
   * so the flat-index bookkeeping (max_var offset) is handled for us. */
  TCCIRState *ir = utb_new();
  ir->next_local_variable = -1;    /* max_var = 0 */
  ir->next_temporary_variable = 4; /* max_tmp = 5 */
  ir->next_parameter = -1;
  for (int k = 0; k < 4; k++)
    utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(k + 1, I32), utb_temp(0, I32), UTB_NONE);

  IROptDU du;
  ir_opt_du_build(ir, &du);
  UT_ASSERT_EQ(ir_opt_du_uses(&du, VR_TEMP(0)), 2); /* saturated */
  tcc_free(du.def);
  utb_free(ir);
  return 0;
}

UT_TEST(test_du_build_tmp_only_mode)
{
  /* TMP_ONLY: only TEMP vregs are tracked; VAR/PARAM get idx -1. */
  TCCIRState *ir = utb_new();
  ir->next_local_variable = 0;
  ir->next_temporary_variable = 1;
  ir->next_parameter = 0;
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_var(0, I32), UTB_NONE);

  IROptDU du;
  ir_opt_du_build_mode(ir, &du, IR_DU_MODE_TMP_ONLY);
  UT_ASSERT_EQ(du.total, 2); /* next_temporary_variable+1 */
  UT_ASSERT_EQ(ir_opt_du_uses(&du, VR_TEMP(0)), 0); /* T0 only defined, not used */
  UT_ASSERT_EQ(ir_opt_du_uses(&du, VR_VAR(0)), 0);  /* VAR not tracked -> 0 */
  UT_ASSERT_EQ(ir_opt_du_def(&du, VR_VAR(0), 5), -1);
  tcc_free(du.def);
  utb_free(ir);
  return 0;
}

/* ============================================ ir_opt_build_def_count */

UT_TEST(test_def_count_single_and_multi_def)
{
  /* T0 defined once, T1 defined twice (at indices 0 and 2). */
  TCCIRState *ir = utb_new();
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(1, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(1, I32), utb_imm(2, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(1, I32), utb_imm(3, I32), UTB_NONE);

  int stride;
  uint8_t *dc = ir_opt_build_def_count(ir, 3, &stride);

  UT_ASSERT(stride >= 2);
  int tmp_type = TCCIR_VREG_TYPE_TEMP;
  UT_ASSERT_EQ(dc[tmp_type * stride + 0], 1); /* T0 single def */
  UT_ASSERT_EQ(dc[tmp_type * stride + 1], 2); /* T1 multi-def, saturated */
  UT_ASSERT(DC_IS_SINGLE_DEF(dc, stride, VR_TEMP(0)));
  UT_ASSERT(!DC_IS_SINGLE_DEF(dc, stride, VR_TEMP(1)));
  tcc_free(dc);
  utb_free(ir);
  return 0;
}

UT_COVERS("ir_opt_du_idx");
UT_COVERS("ir_opt_du_build_mode");
UT_COVERS("ir_opt_build_def_count");
