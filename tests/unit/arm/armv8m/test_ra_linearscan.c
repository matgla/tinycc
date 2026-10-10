/*
 *  test_ra_linearscan.c - suite for tccls.c / ir/regalloc.c linear scan,
 *  spill decisions, and register assignment.
 */

#define USING_GLOBALS
#include "ir.h"
#include "cfg.h"
#include "source/ir/ssa.h"
#include "source/ir/vreg.h"
#include "source/ir/regalloc.h"
#include "source/ir/regalloc_priv.h"
#include "source/backend/arch/arm/arm_regalloc.h"
#include "ut.h"

static void setup_allocator_state(void)
{
  tcc_state->registers_for_allocator = 13;
  /* Required by ra_linear_scan: int_avail is masked with this bitmap. */
  tcc_state->registers_map_for_allocator = (1ull << 13) - 1;
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

static SValue sv_call_param(int call_id)
{
  return svalue_call_id(call_id);
}

static SValue sv_call_id(int call_id, int argc)
{
  return svalue_call_id_argc(call_id, argc);
}

/* -------------------------------------------------------------------------- */
/* Spill under pressure                                                       */
/* -------------------------------------------------------------------------- */

UT_TEST(test_spill_under_pressure)
{
  TCCIRState *ir = tcc_ir_alloc();
  setup_allocator_state();

  /* Build 16 leaf temps, then combine them in a balanced ADD tree.
   * After all 16 ASSIGNs the leaves are simultaneously live (16 intervals
   * for 13 allocatable integer registers), so at least one must spill. */
  int leaves[16];
  for (int i = 0; i < 16; i++)
  {
    leaves[i] = tcc_ir_vreg_alloc_temp(ir);
    SValue s_leaf = sv_var(leaves[i]);
    SValue s_val = sv_const(i);
    tcc_ir_put(ir, TCCIR_OP_ASSIGN, &s_val, NULL, &s_leaf);
  }

  int a[8];
  for (int i = 0; i < 8; i++)
  {
    a[i] = tcc_ir_vreg_alloc_temp(ir);
    SValue s_a = sv_var(a[i]);
    SValue s_l0 = sv_var(leaves[2 * i]);
    SValue s_l1 = sv_var(leaves[2 * i + 1]);
    tcc_ir_put(ir, TCCIR_OP_ADD, &s_l0, &s_l1, &s_a);
  }

  int b[4];
  for (int i = 0; i < 4; i++)
  {
    b[i] = tcc_ir_vreg_alloc_temp(ir);
    SValue s_b = sv_var(b[i]);
    SValue s_a0 = sv_var(a[2 * i]);
    SValue s_a1 = sv_var(a[2 * i + 1]);
    tcc_ir_put(ir, TCCIR_OP_ADD, &s_a0, &s_a1, &s_b);
  }

  int c[2];
  for (int i = 0; i < 2; i++)
  {
    c[i] = tcc_ir_vreg_alloc_temp(ir);
    SValue s_c = sv_var(c[i]);
    SValue s_b0 = sv_var(b[2 * i]);
    SValue s_b1 = sv_var(b[2 * i + 1]);
    tcc_ir_put(ir, TCCIR_OP_ADD, &s_b0, &s_b1, &s_c);
  }

  int final = tcc_ir_vreg_alloc_temp(ir);
  SValue s_final = sv_var(final);
  SValue s_c0 = sv_var(c[0]);
  SValue s_c1 = sv_var(c[1]);
  tcc_ir_put(ir, TCCIR_OP_ADD, &s_c0, &s_c1, &s_final);

  tcc_ir_put(ir, TCCIR_OP_RETURNVALUE, &s_final, NULL, NULL);

  tcc_ir_ssa_regalloc(ir, arm_get_regalloc_target(), 0);

  int spills = 0;
  for (int i = 0; i < 16; i++)
  {
    IRLiveInterval *li = tcc_ir_vreg_live_interval(ir, leaves[i]);
    if (li && li->allocation.offset != 0)
      spills++;
  }
  UT_ASSERT(spills > 0);

  tcc_ir_free(ir);
  return 0;
}

/* -------------------------------------------------------------------------- */
/* Callee-saved use across call                                               */
/* -------------------------------------------------------------------------- */

UT_TEST(test_callee_saved_across_call)
{
  TCCIRState *ir = tcc_ir_alloc();
  setup_allocator_state();

  /* t0 is defined before the call and used after it, so its interval crosses
   * the call. The allocator must keep it in a callee-saved register or spill. */
  int t0 = tcc_ir_vreg_alloc_temp(ir);
  SValue s_t0 = sv_var(t0);
  SValue s_five = sv_const(5);
  tcc_ir_put(ir, TCCIR_OP_ASSIGN, &s_five, NULL, &s_t0);

  SValue s_param = sv_call_param(0);
  SValue s_arg = sv_const(1);
  tcc_ir_put(ir, TCCIR_OP_FUNCPARAMVAL, &s_arg, &s_param, NULL);

  int t_call = tcc_ir_vreg_alloc_temp(ir);
  SValue s_t_call = sv_var(t_call);
  SValue s_call_id = sv_call_id(0, 1);
  tcc_ir_put(ir, TCCIR_OP_FUNCCALLVAL, &s_param, &s_call_id, &s_t_call);

  /* Use both t0 and the call result so neither is dead and the call stays. */
  int t_sum = tcc_ir_vreg_alloc_temp(ir);
  SValue s_sum = sv_var(t_sum);
  tcc_ir_put(ir, TCCIR_OP_ADD, &s_t0, &s_t_call, &s_sum);

  tcc_ir_put(ir, TCCIR_OP_RETURNVALUE, &s_sum, NULL, NULL);

  tcc_ir_ssa_regalloc(ir, arm_get_regalloc_target(), 0);

  IRLiveInterval *li = tcc_ir_vreg_live_interval(ir, t0);
  UT_ASSERT(li != NULL);
  UT_ASSERT(li->crosses_call == 1);

  int r0 = li->allocation.r0;
  int is_callee_saved = (r0 >= 4 && r0 <= 11);
  int is_spilled = (li->allocation.offset != 0);
  UT_ASSERT(is_callee_saved || is_spilled);

  tcc_ir_free(ir);
  return 0;
}

/* -------------------------------------------------------------------------- */
/* Simple assignment                                                          */
/* -------------------------------------------------------------------------- */

UT_TEST(test_simple_assignment)
{
  TCCIRState *ir = tcc_ir_alloc();
  setup_allocator_state();

  int t0 = tcc_ir_vreg_alloc_temp(ir);
  SValue s_t0 = sv_var(t0);
  SValue s_one = sv_const(1);
  tcc_ir_put(ir, TCCIR_OP_ASSIGN, &s_one, NULL, &s_t0);
  tcc_ir_put(ir, TCCIR_OP_RETURNVALUE, &s_t0, NULL, NULL);

  tcc_ir_ssa_regalloc(ir, arm_get_regalloc_target(), 0);

  IRLiveInterval *li = tcc_ir_vreg_live_interval(ir, t0);
  UT_ASSERT(li != NULL);
  UT_ASSERT(li->allocation.r0 < PREG_NONE);

  tcc_ir_free(ir);
  return 0;
}

static SSAInterval spill_swap_interval(int pos, int end, int uses)
{
  SSAInterval iv = {0};
  iv.vreg = TCCIR_ENCODE_VREG(TCCIR_VREG_TYPE_TEMP, pos);
  iv.end = end;
  iv.use_count = uses;
  iv.reg_type = LS_REG_TYPE_INT;
  iv.r0 = iv.r1 = iv.precolored = iv.pref_reg = -1;
  iv.hint_vreg = iv.coalesce_to = -1;
  return iv;
}

static void scan_spill_swap(SSAInterval *ivs, int count, int optimize, int caller_reg)
{
  TCCIRState ir = {0};
  RegAllocTarget target = *arm_get_regalloc_target();
  int saved[] = {4, 5};
  int caller[] = {0};
  setup_allocator_state();
  tcc_state->optimize = optimize;
  target.frame_pointer_reg = target.rodata_anchor_reg = -1;
  target.int_class.callee_saved = saved;
  target.int_class.num_callee_saved = caller_reg ? 0 : 2;
  target.int_class.caller_saved = caller;
  target.int_class.num_caller_saved = caller_reg ? 1 : 0;
  ir.next_temporary_variable = count;
  uint64_t dirty_int = 0, dirty_fp = 0;
  ra_linear_scan(&ir, ivs, count, &target, 0, &dirty_int, &dirty_fp, count, 0, NULL);
}

UT_TEST(test_spill_swap_cheapest_short_active)
{
  for (int opt = 1; opt <= 2; opt++) {
    for (int end = 10; end <= 40; end += 30) {
      SSAInterval ivs[] = {
        spill_swap_interval(0, end, 2),
        spill_swap_interval(1, 9, 1),
        spill_swap_interval(2, 40, 4),
      };
      ivs[2].crosses_call = opt == 2;
      scan_spill_swap(ivs, 3, opt, 0);
      UT_ASSERT_EQ(ivs[0].stack_location, 0);
      UT_ASSERT_NE(ivs[1].stack_location, 0);
      UT_ASSERT_EQ(ivs[1].r0, -1);
      UT_ASSERT_EQ(ivs[2].stack_location, 0);
      UT_ASSERT_EQ(ivs[2].r0, 5);
    }
  }
  return 0;
}

UT_TEST(test_spill_swap_requires_strictly_lower_cost)
{
  for (int uses = 4; uses <= 5; uses++) {
    SSAInterval ivs[] = {
      spill_swap_interval(0, 10, uses),
      spill_swap_interval(1, 9, uses),
      spill_swap_interval(2, 40, 4),
    };
    scan_spill_swap(ivs, 3, 2, 0);
    UT_ASSERT_EQ(ivs[0].stack_location, 0);
    UT_ASSERT_EQ(ivs[1].stack_location, 0);
    UT_ASSERT_NE(ivs[2].stack_location, 0);
  }
  return 0;
}

UT_TEST(test_spill_swap_prefers_outliving_victim)
{
  SSAInterval ivs[] = {
    spill_swap_interval(0, 50, 8),
    spill_swap_interval(1, 9, 1),
    spill_swap_interval(2, 40, 4),
  };
  scan_spill_swap(ivs, 3, 2, 0);
  UT_ASSERT_NE(ivs[0].stack_location, 0);
  UT_ASSERT_EQ(ivs[1].stack_location, 0);
  UT_ASSERT_EQ(ivs[2].r0, 4);
  return 0;
}

UT_TEST(test_spill_swap_respects_protected_actives)
{
  for (int protection = 0; protection < 3; protection++) {
    SSAInterval ivs[] = {
      spill_swap_interval(0, 10, 1),
      spill_swap_interval(1, 9, 4),
      spill_swap_interval(2, 40, 4),
    };
    if (protection == 0) ivs[0].precolored = 4;
    if (protection == 1) ivs[0].caller_save = 1;
    if (protection == 2) ivs[0].loop_phi_locked = 1;
    scan_spill_swap(ivs, 3, 2, 0);
    UT_ASSERT_EQ(ivs[0].stack_location, 0);
    UT_ASSERT_NE(ivs[2].stack_location, 0);
  }
  return 0;
}

UT_TEST(test_spill_swap_keeps_call_crossing_value_out_of_caller_register)
{
  SSAInterval ivs[] = {
    spill_swap_interval(0, 10, 1),
    spill_swap_interval(1, 40, 4),
  };
  ivs[1].crosses_call = 1;
  scan_spill_swap(ivs, 2, 2, 1);
  UT_ASSERT_EQ(ivs[0].r0, 0);
  UT_ASSERT_EQ(ivs[0].stack_location, 0);
  UT_ASSERT_NE(ivs[1].stack_location, 0);
  return 0;
}

UT_TEST(test_spill_swap_disabled_at_o0)
{
  SSAInterval ivs[] = {
    spill_swap_interval(0, 10, 1),
    spill_swap_interval(1, 9, 2),
    spill_swap_interval(2, 40, 4),
  };
  scan_spill_swap(ivs, 3, 0, 0);
  UT_ASSERT_EQ(ivs[0].stack_location, 0);
  UT_ASSERT_EQ(ivs[1].stack_location, 0);
  UT_ASSERT_NE(ivs[2].stack_location, 0);
  return 0;
}

UT_TEST(test_spill_swap_does_not_evict_coheld_register)
{
  SSAInterval ivs[] = {
    spill_swap_interval(0, 10, 1),
    spill_swap_interval(1, 10, 1),
    spill_swap_interval(2, 9, 4),
    spill_swap_interval(3, 40, 4),
  };
  ivs[1].start = 1;
  ivs[1].precolored = 4;
  ivs[2].start = ivs[3].start = 2;
  scan_spill_swap(ivs, 4, 2, 0);
  UT_ASSERT_EQ(ivs[0].r0, 4);
  UT_ASSERT_EQ(ivs[1].r0, 4);
  UT_ASSERT_EQ(ivs[2].r0, 5);
  UT_ASSERT_NE(ivs[3].stack_location, 0);
  return 0;
}

UT_TEST(test_spill_swap_pair_frees_high_register)
{
  SSAInterval ivs[] = {
    spill_swap_interval(0, 10, 1),
    spill_swap_interval(1, 40, 4),
    spill_swap_interval(2, 30, 4),
  };
  ivs[0].reg_type = LS_REG_TYPE_LLONG;
  ivs[1].crosses_call = 1;
  ivs[1].start = 1;
  ivs[2].start = 2;
  scan_spill_swap(ivs, 3, 2, 0);
  UT_ASSERT_NE(ivs[0].stack_location, 0);
  UT_ASSERT_EQ(ivs[0].r0, -1);
  UT_ASSERT_EQ(ivs[0].r1, -1);
  UT_ASSERT_EQ(ivs[0].stack_location % 8, 0);
  UT_ASSERT_EQ(ivs[1].r0, 4);
  UT_ASSERT_EQ(ivs[2].r0, 5);
  UT_ASSERT_EQ(ivs[1].stack_location, 0);
  UT_ASSERT_EQ(ivs[2].stack_location, 0);
  return 0;
}
