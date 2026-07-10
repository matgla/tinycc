/*
 *  test_ir_vreg.c - suite for ir/vreg.c virtual register management
 *
 *  Initialises a minimal TCCIRState (only the fields vreg.c touches)
 *  without calling tcc_ir_alloc() so we avoid pulling in ir/gen/state.c,
 *  tccls.c, and the machine-specific backend.
 */

#define USING_GLOBALS
#include "ir.h"

#include "ut.h"

#define UT_INTERVAL_INIT_SIZE 4

/* Initialise the three live-interval pools in an otherwise-zeroed state */
static void ut_init_intervals(IRLiveInterval **arr, int *size, int *next)
{
  *size = UT_INTERVAL_INIT_SIZE;
  *next = 0;
  *arr = (IRLiveInterval *)tcc_mallocz(sizeof(IRLiveInterval) * UT_INTERVAL_INIT_SIZE);
  for (int i = 0; i < UT_INTERVAL_INIT_SIZE; ++i)
  {
    (*arr)[i].start = INTERVAL_NOT_STARTED;
    (*arr)[i].incoming_reg0 = -1;
    (*arr)[i].incoming_reg1 = -1;
    (*arr)[i].stack_slot_index = -1;
    (*arr)[i].allocation.r0 = PREG_NONE;
    (*arr)[i].allocation.r1 = PREG_NONE;
  }
}

static TCCIRState *ut_vreg_new(void)
{
  TCCIRState *ir = (TCCIRState *)tcc_mallocz(sizeof(*ir));
  ut_init_intervals(&ir->temporary_variables_live_intervals,
                    &ir->temporary_variables_live_intervals_size,
                    &ir->next_temporary_variable);
  ut_init_intervals(&ir->variables_live_intervals,
                    &ir->variables_live_intervals_size,
                    &ir->next_local_variable);
  ut_init_intervals(&ir->parameters_live_intervals,
                    &ir->parameters_live_intervals_size,
                    &ir->next_parameter);
  return ir;
}

static void ut_vreg_free(TCCIRState *ir)
{
  tcc_free(ir->temporary_variables_live_intervals);
  tcc_free(ir->variables_live_intervals);
  tcc_free(ir->parameters_live_intervals);
  tcc_free(ir);
}

/* ------------------------------------------------------------------ tests */

UT_TEST(test_vreg_alloc_temp_sequential)
{
  TCCIRState *ir = ut_vreg_new();

  int vr0 = tcc_ir_vreg_alloc_temp(ir);
  int vr1 = tcc_ir_vreg_alloc_temp(ir);
  int vr2 = tcc_ir_vreg_alloc_temp(ir);

  UT_ASSERT_EQ(TCCIR_DECODE_VREG_TYPE(vr0), TCCIR_VREG_TYPE_TEMP);
  UT_ASSERT_EQ(TCCIR_DECODE_VREG_TYPE(vr1), TCCIR_VREG_TYPE_TEMP);
  UT_ASSERT_EQ(TCCIR_DECODE_VREG_POSITION(vr0), 0);
  UT_ASSERT_EQ(TCCIR_DECODE_VREG_POSITION(vr1), 1);
  UT_ASSERT_EQ(TCCIR_DECODE_VREG_POSITION(vr2), 2);
  UT_ASSERT_EQ(ir->next_temporary_variable, 3);

  ut_vreg_free(ir);
  return 0;
}

UT_TEST(test_vreg_alloc_var_sequential)
{
  TCCIRState *ir = ut_vreg_new();

  int vr0 = tcc_ir_vreg_alloc_var(ir);
  int vr1 = tcc_ir_vreg_alloc_var(ir);

  UT_ASSERT_EQ(TCCIR_DECODE_VREG_TYPE(vr0), TCCIR_VREG_TYPE_VAR);
  UT_ASSERT_EQ(TCCIR_DECODE_VREG_TYPE(vr1), TCCIR_VREG_TYPE_VAR);
  UT_ASSERT_EQ(TCCIR_DECODE_VREG_POSITION(vr0), 0);
  UT_ASSERT_EQ(TCCIR_DECODE_VREG_POSITION(vr1), 1);

  ut_vreg_free(ir);
  return 0;
}

UT_TEST(test_vreg_alloc_param_sequential)
{
  TCCIRState *ir = ut_vreg_new();

  int vr0 = tcc_ir_vreg_alloc_param(ir);
  int vr1 = tcc_ir_vreg_alloc_param(ir);

  UT_ASSERT_EQ(TCCIR_DECODE_VREG_TYPE(vr0), TCCIR_VREG_TYPE_PARAM);
  UT_ASSERT_EQ(TCCIR_DECODE_VREG_TYPE(vr1), TCCIR_VREG_TYPE_PARAM);
  UT_ASSERT_EQ(TCCIR_DECODE_VREG_POSITION(vr0), 0);
  UT_ASSERT_EQ(TCCIR_DECODE_VREG_POSITION(vr1), 1);

  ut_vreg_free(ir);
  return 0;
}

UT_TEST(test_vreg_types_independent)
{
  TCCIRState *ir = ut_vreg_new();

  int var = tcc_ir_vreg_alloc_var(ir);
  int tmp = tcc_ir_vreg_alloc_temp(ir);
  int par = tcc_ir_vreg_alloc_param(ir);

  /* All at position 0 but different types */
  UT_ASSERT_EQ(TCCIR_DECODE_VREG_POSITION(var), 0);
  UT_ASSERT_EQ(TCCIR_DECODE_VREG_POSITION(tmp), 0);
  UT_ASSERT_EQ(TCCIR_DECODE_VREG_POSITION(par), 0);

  UT_ASSERT_EQ(TCCIR_DECODE_VREG_TYPE(var), TCCIR_VREG_TYPE_VAR);
  UT_ASSERT_EQ(TCCIR_DECODE_VREG_TYPE(tmp), TCCIR_VREG_TYPE_TEMP);
  UT_ASSERT_EQ(TCCIR_DECODE_VREG_TYPE(par), TCCIR_VREG_TYPE_PARAM);

  ut_vreg_free(ir);
  return 0;
}

UT_TEST(test_vreg_is_valid)
{
  TCCIRState *ir = ut_vreg_new();

  int vr = tcc_ir_vreg_alloc_temp(ir);
  UT_ASSERT(tcc_ir_vreg_is_valid(ir, vr));

  /* Position out of range (next_temporary_variable == 1, so pos 1 is invalid) */
  int bad = TCCIR_ENCODE_VREG(TCCIR_VREG_TYPE_TEMP, 999);
  UT_ASSERT(!tcc_ir_vreg_is_valid(ir, bad));

  /* Type 0 (unset) is always invalid */
  UT_ASSERT(!tcc_ir_vreg_is_valid(ir, 0));

  ut_vreg_free(ir);
  return 0;
}

UT_TEST(test_vreg_alloc_temp_grows_capacity)
{
  TCCIRState *ir = ut_vreg_new();
  int initial_size = ir->temporary_variables_live_intervals_size;

  /* Exhaust initial capacity and force a realloc */
  for (int i = 0; i < initial_size + 2; ++i)
    tcc_ir_vreg_alloc_temp(ir);

  UT_ASSERT(ir->temporary_variables_live_intervals_size > initial_size);
  UT_ASSERT_EQ(ir->next_temporary_variable, initial_size + 2);

  /* All allocated vregs must still be valid */
  for (int i = 0; i < initial_size + 2; ++i)
  {
    int vr = TCCIR_ENCODE_VREG(TCCIR_VREG_TYPE_TEMP, i);
    UT_ASSERT(tcc_ir_vreg_is_valid(ir, vr));
  }

  ut_vreg_free(ir);
  return 0;
}

UT_TEST(test_vreg_is_ignored_no_table)
{
  TCCIRState *ir = ut_vreg_new();
  /* ignored_vregs NULL means nothing is ignored */
  int vr = tcc_ir_vreg_alloc_temp(ir);
  UT_ASSERT(!tcc_ir_vreg_is_ignored(ir, vr));

  ut_vreg_free(ir);
  return 0;
}

UT_TEST(test_vreg_null_alloc_temp_returns_minus1)
{
  UT_ASSERT_EQ(tcc_ir_vreg_alloc_temp(NULL), -1);
  UT_ASSERT_EQ(tcc_ir_vreg_alloc_var(NULL), -1);
  return 0;
}

/* ------------------------------------------------------------------ suite */

UT_SUITE(ir_vreg)
{
  UT_RUN(test_vreg_alloc_temp_sequential);
  UT_RUN(test_vreg_alloc_var_sequential);
  UT_RUN(test_vreg_alloc_param_sequential);
  UT_RUN(test_vreg_types_independent);
  UT_RUN(test_vreg_is_valid);
  UT_RUN(test_vreg_alloc_temp_grows_capacity);
  UT_RUN(test_vreg_is_ignored_no_table);
  UT_RUN(test_vreg_null_alloc_temp_returns_minus1);
}
