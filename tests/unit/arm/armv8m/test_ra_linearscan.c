/*
 *  test_ra_linearscan.c - suite for tccls.c / ir/regalloc.c linear scan,
 *  spill decisions, and register assignment.
 */

#define USING_GLOBALS
#include "ir.h"
#include "cfg.h"
#include "ir/ssa.h"
#include "ir/vreg.h"
#include "ir/regalloc.h"
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
