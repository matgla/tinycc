/*
 *  test_ra_arm.c - ARMv8-M register-allocation target-descriptor tests
 *
 *  Exercises the ARM register-allocation target descriptor and ARM-specific
 *  allocation constraints (integer register range, hard-float type hints).
 */

#define USING_GLOBALS
#include "ir.h"
#include "cfg.h"
#include "ir/ssa.h"
#include "ir/vreg.h"
#include "ir/regalloc.h"
#include "source/backend/arch/arm/arm_regalloc.h"
#include "ut.h"

/* -------------------------------------------------------------------------- */
/* Helpers                                                                    */
/* -------------------------------------------------------------------------- */

static SValue sv_var_int(int vreg)
{
  SValue sv;
  svalue_init(&sv);
  sv.vr = vreg;
  sv.type.t = VT_INT;
  return sv;
}

static SValue sv_var_fp(int vreg, int vt)
{
  SValue sv;
  svalue_init(&sv);
  sv.vr = vreg;
  sv.type.t = vt;
  return sv;
}

static SValue sv_const_int(int v)
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
  tcc_state->float_abi = ARM_HARD_FLOAT;
  tcc_state->float_registers_for_allocator = 32;
  tcc_state->float_registers_map_for_allocator = (1ull << 32) - 1;
  tcc_state->optimize = 0;
}

/* -------------------------------------------------------------------------- */
/* Tests                                                                      */
/* -------------------------------------------------------------------------- */

UT_TEST(test_arm_target_descriptor)
{
  const RegAllocTarget *target = arm_get_regalloc_target();
  UT_ASSERT(target != NULL);

  UT_ASSERT_EQ(target->int_class.num_regs, 13);
  UT_ASSERT_EQ(target->int_class.num_caller_saved, 5);
  UT_ASSERT_EQ(target->int_class.num_callee_saved, 8);
  UT_ASSERT_EQ(target->fp_class.num_regs, 32);
  UT_ASSERT_EQ(target->param_regs, 4);
  UT_ASSERT_EQ(target->static_chain_reg, 10);

  UT_ASSERT(target->int_class.caller_saved != NULL);
  UT_ASSERT(target->int_class.callee_saved != NULL);

  int caller_seen[13] = {0};
  for (int i = 0; i < target->int_class.num_caller_saved; i++)
  {
    int r = target->int_class.caller_saved[i];
    UT_ASSERT(r >= 0 && r < 13);
    caller_seen[r] = 1;
  }
  UT_ASSERT(caller_seen[0] && caller_seen[1] && caller_seen[2] &&
            caller_seen[3] && caller_seen[12]);

  int callee_seen[13] = {0};
  for (int i = 0; i < target->int_class.num_callee_saved; i++)
  {
    int r = target->int_class.callee_saved[i];
    UT_ASSERT(r >= 0 && r < 13);
    callee_seen[r] = 1;
  }
  UT_ASSERT(callee_seen[4] && callee_seen[5] && callee_seen[6] &&
            callee_seen[7] && callee_seen[8] && callee_seen[9] &&
            callee_seen[10] && callee_seen[11]);

  return 0;
}

UT_TEST(test_arm_fp_interval_types)
{
  /* The unit-test harness does not initialise the ARM architecture_config
   * FPU table, so IR ops that consult it (CVT_ITOF etc.) would segfault.
   * Instead, exercise the RA layer's FP-type interval metadata directly via
   * the public vreg helpers that the allocator uses. */
  setup_tcc_state();

  TCCIRState *ir = tcc_ir_alloc();
  UT_ASSERT(ir != NULL);

  int f0 = tcc_ir_vreg_alloc_temp(ir);
  int d0 = tcc_ir_vreg_alloc_temp(ir);
  int ll0 = tcc_ir_vreg_alloc_temp(ir);

  tcc_ir_vreg_type_set_fp(ir, f0, 1, 0);
  tcc_ir_vreg_type_set_fp(ir, d0, 0, 1);
  tcc_ir_vreg_type_set_64bit(ir, ll0);

  IRLiveInterval *li_f0 = tcc_ir_vreg_live_interval(ir, f0);
  IRLiveInterval *li_d0 = tcc_ir_vreg_live_interval(ir, d0);
  IRLiveInterval *li_ll0 = tcc_ir_vreg_live_interval(ir, ll0);
  UT_ASSERT(li_f0 != NULL);
  UT_ASSERT(li_d0 != NULL);
  UT_ASSERT(li_ll0 != NULL);

  UT_ASSERT(li_f0->is_float == 1);
  UT_ASSERT(li_f0->is_double == 0);
  UT_ASSERT(li_f0->use_vfp == 1);

  UT_ASSERT(li_d0->is_float == 0);
  UT_ASSERT(li_d0->is_double == 1);
  UT_ASSERT(li_d0->use_vfp == 1);

  UT_ASSERT(li_ll0->is_llong == 1);

  tcc_ir_free(ir);
  return 0;
}

UT_TEST(test_arm_register_range)
{
  setup_tcc_state();

  TCCIRState *ir = tcc_ir_alloc();
  UT_ASSERT(ir != NULL);

  int v0 = tcc_ir_vreg_alloc_var(ir);
  int t0 = tcc_ir_vreg_alloc_temp(ir);

  SValue s_v0 = sv_var_int(v0);
  SValue s_t0 = sv_var_int(t0);
  SValue s_one = sv_const_int(1);
  SValue s_two = sv_const_int(2);

  tcc_ir_put(ir, TCCIR_OP_ASSIGN, &s_one, NULL, &s_v0);
  tcc_ir_put(ir, TCCIR_OP_ADD, &s_v0, &s_two, &s_t0);
  tcc_ir_put(ir, TCCIR_OP_RETURNVALUE, &s_t0, NULL, NULL);

  tcc_ir_ssa_regalloc(ir, arm_get_regalloc_target(), 0);

  for (int i = 0; i < ir->next_local_variable; i++)
  {
    IRLiveInterval *li = &ir->variables_live_intervals[i];
    if (li->is_float || li->is_double || li->use_vfp || li->is_complex)
      continue;
    if (li->allocation.offset != 0)
      continue;
    if (li->allocation.r0 == PREG_NONE || li->allocation.r0 == 0xffff)
      continue;
    UT_ASSERT(li->allocation.r0 < 13);
  }

  for (int i = 0; i < ir->next_temporary_variable; i++)
  {
    IRLiveInterval *li = &ir->temporary_variables_live_intervals[i];
    if (li->is_float || li->is_double || li->use_vfp || li->is_complex)
      continue;
    if (li->allocation.offset != 0)
      continue;
    if (li->allocation.r0 == PREG_NONE || li->allocation.r0 == 0xffff)
      continue;
    UT_ASSERT(li->allocation.r0 < 13);
  }

  tcc_ir_free(ir);
  return 0;
}
