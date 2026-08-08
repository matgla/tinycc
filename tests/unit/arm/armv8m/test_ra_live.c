/*
 *  test_ra_live.c - unit tests for live-interval construction in the
 *  ARMv8-M SSA register allocator (ir/regalloc.c's ra_build_intervals).
 */

#define USING_GLOBALS
#include "ir.h"
#include "cfg.h"
#include "source/ir/ssa.h"
#include "source/ir/vreg.h"
#include "source/ir/regalloc.h"
#include "source/backend/arch/arm/arm_regalloc.h"
#include "ut.h"

static void setup_allocator_state(void)
{
  tcc_state->registers_for_allocator = 13;
  tcc_state->float_abi = ARM_HARD_FLOAT;
  tcc_state->float_registers_for_allocator = 32;
  tcc_state->float_registers_map_for_allocator = (1ull << 32) - 1;
  tcc_state->optimize = 0;
}

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

/* -------------------------------------------------------------------------- */
/* 1. Straight-line live interval                                             */
/* -------------------------------------------------------------------------- */

UT_TEST(test_live_straight_line)
{
  TCCIRState *ir = tcc_ir_alloc();
  setup_allocator_state();

  int v0 = tcc_ir_vreg_alloc_temp(ir);
  int t0 = tcc_ir_vreg_alloc_temp(ir);
  SValue s_v0 = sv_var(v0);
  SValue s_t0 = sv_var(t0);
  SValue s_one = sv_const(1);
  SValue s_two = sv_const(2);

  tcc_ir_put(ir, TCCIR_OP_ASSIGN, &s_one, NULL, &s_v0);
  tcc_ir_put(ir, TCCIR_OP_ADD, &s_v0, &s_two, &s_t0);
  tcc_ir_put(ir, TCCIR_OP_RETURNVALUE, &s_t0, NULL, NULL);

  tcc_ir_ssa_regalloc(ir, arm_get_regalloc_target(), 0);

  IRLiveInterval *li = tcc_ir_vreg_live_interval(ir, v0);
  UT_ASSERT(li != NULL);
  UT_ASSERT(li->start != INTERVAL_NOT_STARTED);
  UT_ASSERT(li->end != INTERVAL_NOT_STARTED);
  UT_ASSERT(li->start < li->end);

  tcc_ir_free(ir);
  return 0;
}

/* -------------------------------------------------------------------------- */
/* 2. Loop back-edge extends the live interval                                */
/* -------------------------------------------------------------------------- */

UT_TEST(test_live_loop_backedge)
{
  TCCIRState *ir = tcc_ir_alloc();
  setup_allocator_state();

  int v0 = tcc_ir_vreg_alloc_temp(ir);
  int v1 = tcc_ir_vreg_alloc_temp(ir);
  SValue s_v0 = sv_var(v0);
  SValue s_v1 = sv_var(v1);
  SValue s_one = sv_const(1);

  tcc_ir_put(ir, TCCIR_OP_ASSIGN, &s_one, NULL, &s_v0);
  int loop_start = tcc_ir_put(ir, TCCIR_OP_ADD, &s_v0, &s_one, &s_v1);

  SValue s_v1_cond = sv_var(v1);
  SValue j_loop = sv_jump_target(loop_start);
  tcc_ir_put(ir, TCCIR_OP_JUMPIF, &s_v1_cond, NULL, &j_loop);

  SValue s_v0_ret = sv_var(v0);
  tcc_ir_put(ir, TCCIR_OP_RETURNVALUE, &s_v0_ret, NULL, NULL);

  tcc_ir_ssa_regalloc(ir, arm_get_regalloc_target(), 0);

  IRLiveInterval *li = tcc_ir_vreg_live_interval(ir, v0);
  UT_ASSERT(li != NULL);
  UT_ASSERT(li->start != INTERVAL_NOT_STARTED);
  UT_ASSERT(li->end != INTERVAL_NOT_STARTED);
  /* The return is two instructions after loop_start; living that far means
   * the interval has crossed the back-edge at loop_start+1. */
  UT_ASSERT(li->end >= (uint32_t)(loop_start + 2));

  tcc_ir_free(ir);
  return 0;
}

/* -------------------------------------------------------------------------- */
/* 3. Live value crossing a function call                                     */
/* -------------------------------------------------------------------------- */

UT_TEST(test_live_call_crossing)
{
  TCCIRState *ir = tcc_ir_alloc();
  setup_allocator_state();

  int v0 = tcc_ir_vreg_alloc_temp(ir);
  int arg = tcc_ir_vreg_alloc_temp(ir);
  SValue s_v0 = sv_var(v0);
  SValue s_arg = sv_var(arg);
  SValue s_one = sv_const(1);
  SValue s_two = sv_const(2);

  tcc_ir_put(ir, TCCIR_OP_ASSIGN, &s_one, NULL, &s_v0);
  tcc_ir_put(ir, TCCIR_OP_ASSIGN, &s_two, NULL, &s_arg);

  SValue param_enc = sv_const((int)TCCIR_ENCODE_PARAM(1, 0));
  tcc_ir_put(ir, TCCIR_OP_FUNCPARAMVAL, &s_arg, &param_enc, NULL);

  SValue s_target = sv_const(0);
  SValue call_enc = sv_const((int)TCCIR_ENCODE_CALL(1, 1));
  tcc_ir_put(ir, TCCIR_OP_FUNCCALLVOID, &s_target, &call_enc, NULL);

  tcc_ir_put(ir, TCCIR_OP_RETURNVALUE, &s_v0, NULL, NULL);

  tcc_ir_ssa_regalloc(ir, arm_get_regalloc_target(), 0);

  IRLiveInterval *li = tcc_ir_vreg_live_interval(ir, v0);
  UT_ASSERT(li != NULL);
  UT_ASSERT(li->crosses_call == 1);

  tcc_ir_free(ir);
  return 0;
}
