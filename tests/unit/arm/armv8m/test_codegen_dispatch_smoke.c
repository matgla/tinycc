/*
 *  test_codegen_dispatch_smoke.c - Phase 0 feasibility spike for
 *  tcc_ir_codegen_generate() dispatch-loop unit tests.
 *
 *  Proves the codegen_mop_stubs.c link works, that the dispatch loop runs
 *  end-to-end without crashing on hand-built IR, and settles empirically
 *  which IR shape forces codegen.c's can_skip_dry_run branch each way (see
 *  ir/codegen.c ~line 2145: dry-run is skipped when
 *  popcount(ir->ls.dirty_registers) <= registers_for_allocator - 2, i.e.
 *  <= 11 with the standard setup_tcc_state() below).
 */

#define USING_GLOBALS
#include "ir.h"
#include "cfg.h"
#include "ir/ssa.h"
#include "ir/vreg.h"
#include "ir/regalloc.h"
#include "ir/codegen.h"
#include "ir/machine_op.h"
#include "arch/arm/arm_regalloc.h"
#include "codegen_mop_stubs.h"
#include "ut.h"

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

static void setup_tcc_state(void)
{
  tcc_state->registers_for_allocator = 13;
  tcc_state->registers_map_for_allocator = (1ull << 13) - 1;
  tcc_state->float_abi = ARM_HARD_FLOAT;
  tcc_state->float_registers_for_allocator = 32;
  tcc_state->float_registers_map_for_allocator = (1ull << 32) - 1;
  tcc_state->optimize = 0;
}

/* -------------------------------------------------------------------------- */
/* Step 1/2: link + one real call through tcc_ir_codegen_generate()           */
/* -------------------------------------------------------------------------- */

UT_TEST(test_dispatch_smoke_minimal_function_generates)
{
  cgstub_reset();

  TCCIRState *ir = tcc_ir_alloc();
  setup_tcc_state();

  int a = tcc_ir_vreg_alloc_temp(ir);
  int b = tcc_ir_vreg_alloc_temp(ir);
  int c = tcc_ir_vreg_alloc_temp(ir);

  SValue s_a = sv_var(a);
  SValue s_b = sv_var(b);
  SValue s_c = sv_var(c);
  SValue s_lhs = sv_const(5);
  SValue s_rhs = sv_const(3);

  tcc_ir_put(ir, TCCIR_OP_ASSIGN, &s_lhs, NULL, &s_a);
  tcc_ir_put(ir, TCCIR_OP_ASSIGN, &s_rhs, NULL, &s_b);
  tcc_ir_put(ir, TCCIR_OP_ADD, &s_a, &s_b, &s_c);
  tcc_ir_put(ir, TCCIR_OP_RETURNVALUE, &s_c, NULL, NULL);

  tcc_ir_ssa_regalloc(ir, arm_get_regalloc_target(), 0);
  ir->leaffunc = 1;

  tcc_ir_codegen_generate(ir);

  UT_ASSERT(cgstub_total_calls() > 0);
  UT_ASSERT(cgstub_call_count("data_processing_mop") >= 1);
  UT_ASSERT(cgstub_call_count("prolog") == 1);
  UT_ASSERT(cgstub_call_count("epilog") == 1);

  tcc_ir_free(ir);
  return 0;
}

/* -------------------------------------------------------------------------- */
/* Step 3: can_skip_dry_run experiment -- skip-path                           */
/* -------------------------------------------------------------------------- */

/* Small function (few live temporaries): trivially satisfies
 * popcount(dirty_registers) <= 11, so codegen.c takes the can_skip_dry_run
 * shortcut. dry_run_start/dry_run_end must never fire. */
UT_TEST(test_dispatch_smoke_can_skip_dry_run_when_register_pressure_low)
{
  cgstub_reset();

  TCCIRState *ir = tcc_ir_alloc();
  setup_tcc_state();

  int a = tcc_ir_vreg_alloc_temp(ir);
  int b = tcc_ir_vreg_alloc_temp(ir);
  int c = tcc_ir_vreg_alloc_temp(ir);

  SValue s_a = sv_var(a);
  SValue s_b = sv_var(b);
  SValue s_c = sv_var(c);
  SValue s_lhs = sv_const(5);
  SValue s_rhs = sv_const(3);

  tcc_ir_put(ir, TCCIR_OP_ASSIGN, &s_lhs, NULL, &s_a);
  tcc_ir_put(ir, TCCIR_OP_ASSIGN, &s_rhs, NULL, &s_b);
  tcc_ir_put(ir, TCCIR_OP_ADD, &s_a, &s_b, &s_c);
  tcc_ir_put(ir, TCCIR_OP_RETURNVALUE, &s_c, NULL, NULL);

  tcc_ir_ssa_regalloc(ir, arm_get_regalloc_target(), 0);
  ir->leaffunc = 1;

  tcc_ir_codegen_generate(ir);

  UT_ASSERT_EQ(cgstub_call_count("dry_run_start"), 0);
  UT_ASSERT_EQ(cgstub_call_count("dry_run_end"), 0);
  /* Single (real) pass only -- every mop call is tagged pass=1. */
  UT_ASSERT(cgstub_call_count_pass("data_processing_mop", 1) >= 1);
  UT_ASSERT_EQ(cgstub_call_count_pass("data_processing_mop", 0), 0);

  tcc_ir_free(ir);
  return 0;
}

/* -------------------------------------------------------------------------- */
/* Step 3: can_skip_dry_run experiment -- non-skip path                       */
/* -------------------------------------------------------------------------- */

/* 12 simultaneously-live temporaries (all kept live into one final combining
 * chain of ADDs) force popcount(dirty_registers) > 11, so codegen.c must run
 * the full two-pass (dry-run + real-run) loop. Both dry_run_start/end fire,
 * and the same mop shows up tagged with both pass=0 and pass=1. */
UT_TEST(test_dispatch_smoke_forces_two_pass_when_register_pressure_high)
{
  cgstub_reset();

  TCCIRState *ir = tcc_ir_alloc();
  setup_tcc_state();

  enum
  {
    NPARAM = 12
  };
  int t[NPARAM];
  SValue s[NPARAM];
  for (int i = 0; i < NPARAM; i++)
  {
    t[i] = tcc_ir_vreg_alloc_temp(ir);
    s[i] = sv_var(t[i]);
    SValue s_imm = sv_const(i + 1);
    tcc_ir_put(ir, TCCIR_OP_ASSIGN, &s_imm, NULL, &s[i]);
  }

  /* Chain all 12 into one running total so every one of them stays live from
   * its definition through to (near) the end of the function. */
  int acc = tcc_ir_vreg_alloc_temp(ir);
  SValue s_acc = sv_var(acc);
  tcc_ir_put(ir, TCCIR_OP_ASSIGN, &s[0], NULL, &s_acc);
  for (int i = 1; i < NPARAM; i++)
  {
    int next_acc = tcc_ir_vreg_alloc_temp(ir);
    SValue s_next = sv_var(next_acc);
    tcc_ir_put(ir, TCCIR_OP_ADD, &s_acc, &s[i], &s_next);
    s_acc = s_next;
  }

  tcc_ir_put(ir, TCCIR_OP_RETURNVALUE, &s_acc, NULL, NULL);

  tcc_ir_ssa_regalloc(ir, arm_get_regalloc_target(), 0);
  ir->leaffunc = 1;

  tcc_ir_codegen_generate(ir);

  UT_ASSERT_EQ(cgstub_call_count("dry_run_start"), 1);
  UT_ASSERT_EQ(cgstub_call_count("dry_run_end"), 1);
  UT_ASSERT(cgstub_call_count_pass("data_processing_mop", 0) > 0);
  UT_ASSERT(cgstub_call_count_pass("data_processing_mop", 1) > 0);

  tcc_ir_free(ir);
  return 0;
}

/* -------------------------------------------------------------------------- */
/* Suite                                                                      */
/* -------------------------------------------------------------------------- */

UT_SUITE(codegen_dispatch_smoke)
{
  UT_RUN(test_dispatch_smoke_minimal_function_generates);
  UT_RUN(test_dispatch_smoke_can_skip_dry_run_when_register_pressure_low);
  UT_RUN(test_dispatch_smoke_forces_two_pass_when_register_pressure_high);
}
