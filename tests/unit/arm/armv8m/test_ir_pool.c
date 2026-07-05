/*
 *  test_ir_pool.c - smoke tests for ir/pool.c operand pool management
 *
 *  Phase 1 of the tinycc unit-test framework. Exercises the pool add/get
 *  API plus automatic capacity growth, without pulling in the full
 *  libtcc runtime (see tests/unit/stubs.c for the tcc_realloc shim).
 */

#define USING_GLOBALS
#include "ir.h"

#include "ut.h"

static TCCIRState *ut_pool_new(int initial_capacity)
{
  TCCIRState *ir = (TCCIRState *)tcc_mallocz(sizeof(*ir));
  ir->iroperand_pool_capacity = initial_capacity;
  ir->iroperand_pool_count = 0;
  ir->iroperand_pool =
      (IROperand *)tcc_mallocz(sizeof(IROperand) * initial_capacity);
  return ir;
}

static void ut_pool_free(TCCIRState *ir)
{
  tcc_free(ir->iroperand_pool);
  tcc_free(ir);
}

static IROperand ut_irop_with_imm(int32_t imm)
{
  IROperand op = {0};
  op.vr = (IROP_TAG_IMM32 << 18);
  op.u.imm32 = imm;
  return op;
}

UT_TEST(test_pool_add_returns_sequential_indices)
{
  TCCIRState *ir = ut_pool_new(4);

  int i0 = tcc_ir_pool_add(ir, ut_irop_with_imm(100));
  int i1 = tcc_ir_pool_add(ir, ut_irop_with_imm(200));
  int i2 = tcc_ir_pool_add(ir, ut_irop_with_imm(300));

  UT_ASSERT_EQ(i0, 0);
  UT_ASSERT_EQ(i1, 1);
  UT_ASSERT_EQ(i2, 2);
  UT_ASSERT_EQ(ir->iroperand_pool_count, 3);

  ut_pool_free(ir);
  return 0;
}

UT_TEST(test_pool_get_returns_stored_value)
{
  TCCIRState *ir = ut_pool_new(4);

  int idx = tcc_ir_pool_add(ir, ut_irop_with_imm(42));
  IROperand got = tcc_ir_pool_get(ir, idx);

  UT_ASSERT_EQ(got.u.imm32, 42);

  ut_pool_free(ir);
  return 0;
}

UT_TEST(test_pool_get_out_of_range_returns_zero)
{
  TCCIRState *ir = ut_pool_new(4);
  tcc_ir_pool_add(ir, ut_irop_with_imm(7));

  IROperand oob_low = tcc_ir_pool_get(ir, -1);
  IROperand oob_high = tcc_ir_pool_get(ir, 999);

  UT_ASSERT_EQ(oob_low.vr, 0);
  UT_ASSERT_EQ(oob_low.u.imm32, 0);
  UT_ASSERT_EQ(oob_high.vr, 0);
  UT_ASSERT_EQ(oob_high.u.imm32, 0);

  ut_pool_free(ir);
  return 0;
}

UT_TEST(test_pool_set_overwrites_entry)
{
  TCCIRState *ir = ut_pool_new(4);
  int idx = tcc_ir_pool_add(ir, ut_irop_with_imm(1));

  tcc_ir_pool_set(ir, idx, ut_irop_with_imm(999));
  UT_ASSERT_EQ(tcc_ir_pool_get(ir, idx).u.imm32, 999);

  ut_pool_free(ir);
  return 0;
}

UT_TEST(test_pool_add_grows_capacity)
{
  TCCIRState *ir = ut_pool_new(2);
  UT_ASSERT_EQ(ir->iroperand_pool_capacity, 2);

  for (int i = 0; i < 10; ++i)
    tcc_ir_pool_add(ir, ut_irop_with_imm(i));

  UT_ASSERT_EQ(ir->iroperand_pool_count, 10);
  UT_ASSERT(ir->iroperand_pool_capacity >= 10);
  for (int i = 0; i < 10; ++i)
    UT_ASSERT_EQ(tcc_ir_pool_get(ir, i).u.imm32, i);

  ut_pool_free(ir);
  return 0;
}

/* Regression lock for bugs.md #3 (fixed): a zero-capacity pool must grow
 * instead of hanging (tcc_ir_pool_ensure's `capacity *= 2` loop is `0*2==0`
 * forever) or overflowing a zero-size buffer (tcc_ir_pool_add's single
 * `*= 2`). Both now seed capacity to 1 before doubling. */
UT_TEST(test_pool_add_from_zero_capacity_grows)
{
  TCCIRState *ir = (TCCIRState *)tcc_mallocz(sizeof(*ir));
  ir->iroperand_pool_capacity = 0;
  ir->iroperand_pool_count = 0;
  ir->iroperand_pool = NULL;

  int idx = tcc_ir_pool_add(ir, ut_irop_with_imm(42));
  UT_ASSERT_EQ(idx, 0);
  UT_ASSERT(ir->iroperand_pool_capacity >= 1);
  UT_ASSERT_EQ(tcc_ir_pool_get(ir, 0).u.imm32, 42);

  tcc_free(ir->iroperand_pool);
  tcc_free(ir);
  return 0;
}

UT_TEST(test_pool_ensure_from_zero_capacity_terminates)
{
  TCCIRState *ir = (TCCIRState *)tcc_mallocz(sizeof(*ir));
  ir->iroperand_pool_capacity = 0;
  ir->iroperand_pool_count = 0;
  ir->iroperand_pool = NULL;

  tcc_ir_pool_ensure(ir, 5); /* would spin forever (0*2==0) before the fix */
  UT_ASSERT(ir->iroperand_pool_capacity >= 5);

  tcc_free(ir->iroperand_pool);
  tcc_free(ir);
  return 0;
}

UT_SUITE(ir_pool)
{
  UT_RUN(test_pool_add_returns_sequential_indices);
  UT_RUN(test_pool_get_returns_stored_value);
  UT_RUN(test_pool_get_out_of_range_returns_zero);
  UT_RUN(test_pool_set_overwrites_entry);
  UT_RUN(test_pool_add_grows_capacity);
  UT_RUN(test_pool_add_from_zero_capacity_grows);
  UT_RUN(test_pool_ensure_from_zero_capacity_terminates);
}
