/*
 *  test_svalue.c - suite for tinycc SValue helpers
 *
 *  Tests the small SValue construction utilities in svalue.c.
 */

#define USING_GLOBALS
#include "ir.h"
#include "ut.h"

UT_TEST(test_init_clears_fields)
{
  SValue sv;
  /* Initialise with garbage so we can verify svalue_init really clears. */
  memset(&sv, 0xAB, sizeof(sv));

  svalue_init(&sv);

  UT_ASSERT_EQ(sv.vr, -1);
  UT_ASSERT_EQ(sv.pr0_reg, PREG_REG_NONE);
  UT_ASSERT_EQ(sv.pr1_reg, PREG_REG_NONE);
  UT_ASSERT_EQ(sv.type.t, 0);
  UT_ASSERT_EQ(sv.type.ref, (void *)NULL);
  UT_ASSERT_EQ(sv.c.i, 0);
  UT_ASSERT_EQ(sv.sym, (void *)NULL);
  UT_ASSERT_EQ(sv.r, 0);
  return 0;
}

UT_TEST(test_const_i64)
{
  SValue sv = svalue_const_i64(42);

  UT_ASSERT_EQ(sv.r, VT_CONST);
  UT_ASSERT_EQ(sv.c.i, 42);

  /* A const should still be a well-formed, otherwise-clean SValue. */
  UT_ASSERT_EQ(sv.vr, -1);
  return 0;
}

UT_TEST(test_call_id_encoding)
{
  SValue sv = svalue_call_id(5);

  UT_ASSERT_EQ(sv.r, VT_CONST);
  UT_ASSERT_EQ((uint32_t)sv.c.i, TCCIR_ENCODE_PARAM(5, 0));
  UT_ASSERT_EQ(TCCIR_DECODE_CALL_ID((uint32_t)sv.c.i), 5);
  UT_ASSERT_EQ(TCCIR_DECODE_PARAM_IDX((uint32_t)sv.c.i), 0);
  return 0;
}

UT_TEST(test_call_id_argc_encoding)
{
  SValue sv = svalue_call_id_argc(3, 4);

  UT_ASSERT_EQ(sv.r, VT_CONST);
  UT_ASSERT_EQ((uint32_t)sv.c.i, TCCIR_ENCODE_CALL(3, 4));
  UT_ASSERT_EQ(TCCIR_DECODE_CALL_ID((uint32_t)sv.c.i), 3);
  UT_ASSERT_EQ(TCCIR_DECODE_PARAM_IDX((uint32_t)sv.c.i), 4);
  return 0;
}

UT_TEST(test_call_id_zero)
{
  SValue sv = svalue_call_id(0);

  UT_ASSERT_EQ(sv.r, VT_CONST);
  UT_ASSERT_EQ(TCCIR_DECODE_CALL_ID((uint32_t)sv.c.i), 0);
  UT_ASSERT_EQ(TCCIR_DECODE_PARAM_IDX((uint32_t)sv.c.i), 0);
  return 0;
}

UT_TEST(test_const_negative)
{
  SValue sv = svalue_const_i64((int64_t)-123456789);

  UT_ASSERT_EQ(sv.r, VT_CONST);
  UT_ASSERT_EQ((int64_t)sv.c.i, (int64_t)-123456789);
  return 0;
}

UT_SUITE(svalue)
{
  UT_RUN(test_init_clears_fields);
  UT_RUN(test_const_i64);
  UT_RUN(test_call_id_encoding);
  UT_RUN(test_call_id_argc_encoding);
  UT_RUN(test_call_id_zero);
  UT_RUN(test_const_negative);
}
