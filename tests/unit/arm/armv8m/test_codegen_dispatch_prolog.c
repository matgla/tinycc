/*
 *  test_codegen_dispatch_prolog.c - Phase 7: prolog/epilog + two-pass
 *  bookkeeping dispatch tests for tcc_ir_codegen_generate().
 *
 *  Unlike Phases 1-6 (one IR op family each), this targets ir/codegen.c's
 *  meta-logic: tcc_gen_machine_prolog()/epilog() call-count invariants and
 *  how the dry-run's LR-push detection (tcc_gen_machine_dry_run_get_lr_push_
 *  count) feeds into the real prolog's extra_prologue_regs argument.
 *
 *  Spike finding (see docs/plan_codegen_unit_tests.md): ir/codegen.c calls
 *  tcc_gen_machine_prolog() from TWO call sites -- once before the pass loop
 *  when can_skip_dry_run is true (~line 2285), once after dry-run analysis
 *  in the normal two-pass path (~line 4414) -- so "prolog called exactly
 *  once" is a real invariant regardless of which path runs. epilog() is also
 *  called exactly once per function: multiple RETURNVALUE/RETURNVOID
 *  instructions each emit a JUMP (via jump_mop) to one shared epilogue
 *  instead of each getting their own epilog() call (~line 3828) -- so
 *  "epilog once per return path" (the plan's original phrasing) was wrong;
 *  the corrected invariant tested below is "epilog exactly once, regardless
 *  of return count". The phase-3 scratch-conflict-reassignment fixup
 *  (~4284-4357) mutates regalloc intervals directly rather than through a
 *  mop call, so it isn't observable via the cgstub call log -- Phase 11
 *  (see docs/plan_codegen_unit_tests.md) closed it anyway, by reading
 *  IRLiveInterval.allocation.r0 directly before/after
 *  tcc_ir_codegen_generate(), the "assert on `ir->` state directly" this
 *  comment used to say would be needed.
 *
 *  Also covers ir->scratch_save_size sizing's "global bitmap as safety net
 *  for dry/real divergence" branch (~4377-4386): with dry_insn_saves[] left
 *  all-zero (no cgstub_set_next_insn_scratch() call) and only the standing
 *  cgstub_set_scratch_regs_pushed() knob providing a nonzero bitmask, the
 *  global-bitmap popcount is the sole contributor to max_scratch_depth --
 *  read directly off ir->scratch_save_size after tcc_ir_codegen_generate(),
 *  same "assert on ir-> state directly" technique as the phase-3 tests above.
 */

#define USING_GLOBALS
#include "ir.h"
#include "cfg.h"
#include "source/ir/ssa.h"
#include "source/ir/vreg.h"
#include "source/ir/regalloc.h"
#include "source/ir/codegen.h"
#include "source/ir/machine_op.h"
#include "source/backend/arch/arm/arm_regalloc.h"
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

static SValue sv_jump_target(int target_idx)
{
  SValue sv;
  svalue_init(&sv);
  sv.r = VT_CONST;
  sv.c.i = target_idx;
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
  tcc_state->need_frame_pointer = 0; /* not reset between tests; be explicit */
  tcc_state->force_frame_pointer = 0;
}

/* Same >=12-live-temporaries construction established in
 * test_codegen_dispatch_smoke.c to force codegen.c's non-skip (two-pass)
 * path; returns the accumulator SValue so callers can extend the function
 * before the final RETURNVALUE. */
static TCCIRState *build_two_pass_forcing_ir(SValue *out_acc)
{
  TCCIRState *ir = tcc_ir_alloc();
  setup_tcc_state();
  /* -O0 skips the rehearsal walk and drops the forward-branch term that forces
   * the discovery pass (see `cg_skip_rehearsal` / `fwd_branch_may_force_dry` in
   * ir/codegen.c), so register pressure alone no longer buys two passes. */
  tcc_state->optimize = 1;

  enum
  {
    NPARAM = 12
  };
  SValue s[NPARAM];
  for (int i = 0; i < NPARAM; i++)
  {
    int t = tcc_ir_vreg_alloc_temp(ir);
    s[i] = sv_var(t);
    SValue s_imm = sv_const(i + 1);
    tcc_ir_put(ir, TCCIR_OP_ASSIGN, &s_imm, NULL, &s[i]);
  }
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
  *out_acc = s_acc;
  return ir;
}

/* -------------------------------------------------------------------------- */
/* prolog/epilog call-count invariants                                        */
/* -------------------------------------------------------------------------- */

UT_TEST(test_dispatch_prolog_and_epilog_called_exactly_once_two_pass_path)
{
  cgstub_reset();
  SValue s_acc;
  TCCIRState *ir = build_two_pass_forcing_ir(&s_acc);
  tcc_ir_put(ir, TCCIR_OP_RETURNVALUE, &s_acc, NULL, NULL);

  tcc_ir_ssa_regalloc(ir, arm_get_regalloc_target(), 0);
  ir->leaffunc = 1;
  tcc_ir_codegen_generate(ir);

  UT_ASSERT_EQ(cgstub_call_count("dry_run_start"), 2); /* discovery + rehearsal dry pass */ /* confirms two-pass ran */
  UT_ASSERT_EQ(cgstub_call_count("prolog"), 1);
  UT_ASSERT_EQ(cgstub_call_count("epilog"), 1);

  tcc_ir_free(ir);
  return 0;
}

UT_TEST(test_dispatch_prolog_and_epilog_called_exactly_once_skip_path)
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

  UT_ASSERT_EQ(cgstub_call_count("dry_run_start"), 0); /* confirms skip-path ran */
  UT_ASSERT_EQ(cgstub_call_count("prolog"), 1);
  UT_ASSERT_EQ(cgstub_call_count("epilog"), 1);

  tcc_ir_free(ir);
  return 0;
}

/* Diamond CFG: JUMPIF branches to two independent RETURNVALUEs. Both share
 * the one epilogue codegen.c emits; the earlier-in-instruction-order return
 * needs an extra JUMP (jump_mop) to reach it, the trailing one (immediately
 * followed by the epilogue) doesn't -- see the has_trailing_code check at
 * ir/codegen.c ~3828. */
UT_TEST(test_dispatch_epilog_called_once_with_two_return_paths)
{
  cgstub_reset();
  TCCIRState *ir = tcc_ir_alloc();
  setup_tcc_state();

  int cond = tcc_ir_vreg_alloc_temp(ir);
  SValue s_cond = sv_var(cond);
  SValue s_one = sv_const(1);
  tcc_ir_put(ir, TCCIR_OP_ASSIGN, &s_one, NULL, &s_cond);

  SValue jelse = sv_jump_target(-1);
  int branch = tcc_ir_put(ir, TCCIR_OP_JUMPIF, &s_cond, NULL, &jelse);

  SValue s_ten = sv_const(10);
  tcc_ir_put(ir, TCCIR_OP_RETURNVALUE, &s_ten, NULL, NULL); /* early return */

  int else_label = ir->next_instruction_index;
  SValue s_twenty = sv_const(20);
  tcc_ir_put(ir, TCCIR_OP_RETURNVALUE, &s_twenty, NULL, NULL); /* trailing return */

  tcc_ir_codegen_backpatch(ir, branch, else_label);

  tcc_ir_ssa_regalloc(ir, arm_get_regalloc_target(), 0);
  ir->leaffunc = 1;
  tcc_ir_codegen_generate(ir);

  UT_ASSERT_EQ(cgstub_call_count("prolog"), 1);
  UT_ASSERT_EQ(cgstub_call_count("epilog"), 1);
  UT_ASSERT(cgstub_call_count("jump_mop") >= 1); /* the early return's jump to the epilogue */

  tcc_ir_free(ir);
  return 0;
}

/* -------------------------------------------------------------------------- */
/* dry-run LR-push detection feeds the real prolog's extra_prologue_regs      */
/* -------------------------------------------------------------------------- */

UT_TEST(test_dispatch_prolog_forces_lr_when_dry_run_reports_lr_push_in_leaf_fn)
{
  cgstub_reset();
  cgstub_set_lr_push_count(1); /* simulate: dry run needed a scratch PUSH {LR} */

  SValue s_acc;
  TCCIRState *ir = build_two_pass_forcing_ir(&s_acc);
  tcc_ir_put(ir, TCCIR_OP_RETURNVALUE, &s_acc, NULL, NULL);

  tcc_ir_ssa_regalloc(ir, arm_get_regalloc_target(), 0);
  ir->leaffunc = 1; /* original_leaffunc gate: only leaf functions apply this */
  tcc_ir_codegen_generate(ir);

  UT_ASSERT_EQ(cgstub_call_count("dry_run_start"), 2); /* discovery + rehearsal dry pass */ /* two-pass path required */
  UT_ASSERT_EQ(cgstub_call_count("prolog"), 1);
  const CgStubLastProlog *p = cgstub_get_last_prolog();
  UT_ASSERT(p->called);
  UT_ASSERT((p->extra_prologue_regs & (1u << 14)) != 0); /* R_LR forced in */

  tcc_ir_free(ir);
  return 0;
}

UT_TEST(test_dispatch_prolog_no_lr_forced_when_knob_is_zero)
{
  cgstub_reset();
  cgstub_set_lr_push_count(0); /* explicit default */

  SValue s_acc;
  TCCIRState *ir = build_two_pass_forcing_ir(&s_acc);
  tcc_ir_put(ir, TCCIR_OP_RETURNVALUE, &s_acc, NULL, NULL);

  tcc_ir_ssa_regalloc(ir, arm_get_regalloc_target(), 0);
  ir->leaffunc = 1;
  tcc_ir_codegen_generate(ir);

  const CgStubLastProlog *p = cgstub_get_last_prolog();
  UT_ASSERT(p->called);
  UT_ASSERT((p->extra_prologue_regs & (1u << 14)) == 0);

  tcc_ir_free(ir);
  return 0;
}

/* -------------------------------------------------------------------------- */
/* Phase-3 scratch-conflict-reassignment fixup (exploratory)                   */
/* -------------------------------------------------------------------------- */

/* Forcing the two-pass path (can_skip_dry_run needs
 * popcount(dirty_registers) >= registers_for_allocator - 1) normally means
 * the "12 simultaneously-live temps" construction elsewhere in this project
 * -- but that occupies nearly the entire allocatable range for the whole
 * span any one of those temps is live, which is exactly the condition that
 * makes try_reassign_scratch_conflict() find no free callee-saved register
 * anywhere (see docs/plan_codegen_unit_tests.md's Phase 10 writeup).
 *
 * The fix: shrink registers_for_allocator to 6 (R0-R5). The linear-scan
 * allocator then physically never touches R6-R11 -- those bits never appear
 * in live_regs_by_instruction regardless of how much register pressure the
 * test IR creates, leaving them genuinely free for the fixup to target.
 * try_reassign_scratch_conflict()'s hardcoded ARM callee-saved range is
 * R4-R11 (minus reserved R7), which overlaps R4/R5 with this allocator's own
 * pool -- confirmed empirically (not guessed) that t[0]'s live range [0,6]
 * blocks R4/R5 too (t[4] and the accumulator chain use them concurrently),
 * so the fixup lands on R6, the lowest free register outside the small pool. */
UT_TEST(test_phase3_scratch_conflict_reassignment_frees_scratch_register)
{
  cgstub_reset();
  TCCIRState *ir = tcc_ir_alloc();
  tcc_state->registers_for_allocator = 6;
  tcc_state->registers_map_for_allocator = (1ull << 6) - 1;
  tcc_state->float_abi = ARM_HARD_FLOAT;
  tcc_state->float_registers_for_allocator = 32;
  tcc_state->float_registers_map_for_allocator = (1ull << 32) - 1;
  tcc_state->optimize = 1; /* -O1: at -O0 the dry-run walk this asserts on is skipped */
  tcc_state->need_frame_pointer = 0; /* not reset between tests; be explicit */
  tcc_state->force_frame_pointer = 0;

  enum
  {
    NPARAM = 6
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

  /* t[0] must be register-resident (not spilled) so there's a real
   * LSLiveInterval for the fixup to find and relocate. */
  IRLiveInterval *li_before = tcc_ir_vreg_live_interval(ir, t[0]);
  UT_ASSERT(li_before != NULL);
  UT_ASSERT_EQ(li_before->allocation.offset, 0); /* register-resident, not spilled */
  int r0_before = li_before->allocation.r0;
  UT_ASSERT(r0_before >= 0 && r0_before < 6);

  /* Fake "instruction 0 needed to push r0_before" during the dry run --
   * consumed by the first SCRATCH_WRAP'd dispatch, which is the first
   * ASSIGN (t[0] <- #1) at instruction 0. */
  cgstub_set_next_insn_scratch(1, (unsigned short)(1u << r0_before));

  ir->leaffunc = 1;
  tcc_ir_codegen_generate(ir);

  UT_ASSERT_EQ(cgstub_call_count("dry_run_start"), 2); /* discovery + rehearsal dry pass */ /* confirms two-pass ran */

  /* The fixup should have relocated t[0] off r0_before onto a free
   * callee-saved register (R4-R11 minus R7) -- observable directly via the
   * live interval the fixup mutates in place. */
  IRLiveInterval *li_after = tcc_ir_vreg_live_interval(ir, t[0]);
  UT_ASSERT(li_after != NULL);
  UT_ASSERT(li_after->allocation.r0 != r0_before);
  UT_ASSERT(li_after->allocation.r0 >= 4 && li_after->allocation.r0 <= 11);
  UT_ASSERT(li_after->allocation.r0 != 7); /* R_FP, never reassignable */

  tcc_ir_free(ir);
  return 0;
}

/* Regression for the fixup's *fallback* path (ir/codegen.c ~4310-4343): when
 * the specific register a dry-run push was recorded against can't itself be
 * freed (here, R0 holds an ABI-pinned parameter -- incoming_reg0 >= 0 makes
 * try_reassign_scratch_conflict() refuse it, per the ABI-pinned exclusion at
 * ~1087-1088), the code falls back to scanning R0-R3 for *any* other
 * reassignable occupant. Three ABI-pinned params (R0-R2) block the simple
 * "is any R0-R3 already free" check, forcing the R0-R3 scan loop itself;
 * within it, p1/p2 fail the same ABI-pinned way, and t3 (a plain temp,
 * confirmed via test_phase3_alt_reassign_explore's empirical check to also
 * start at instruction 0, occupying R3) is the one candidate the loop can
 * actually relocate -- landing on the success branch other tests here don't
 * reach. */
UT_TEST(test_phase3_alt_reassign_relocates_unpinned_r0_r3_occupant)
{
  cgstub_reset();
  TCCIRState *ir = tcc_ir_alloc();
  tcc_state->registers_for_allocator = 6;
  tcc_state->registers_map_for_allocator = (1ull << 6) - 1;
  tcc_state->float_abi = ARM_HARD_FLOAT;
  tcc_state->float_registers_for_allocator = 32;
  tcc_state->float_registers_map_for_allocator = (1ull << 32) - 1;
  tcc_state->optimize = 1; /* -O1: at -O0 the dry-run walk this asserts on is skipped */
  tcc_state->need_frame_pointer = 0; /* not reset between tests; be explicit */
  tcc_state->force_frame_pointer = 0;

  int p0 = tcc_ir_vreg_alloc_param(ir);
  int p1 = tcc_ir_vreg_alloc_param(ir);
  int p2 = tcc_ir_vreg_alloc_param(ir);
  int t3 = tcc_ir_vreg_alloc_temp(ir);
  SValue s_p0 = sv_var(p0);
  SValue s_p1 = sv_var(p1);
  SValue s_p2 = sv_var(p2);
  SValue s_t3 = sv_var(t3);
  SValue s_three = sv_const(3);
  tcc_ir_put(ir, TCCIR_OP_ASSIGN, &s_three, NULL, &s_t3);
  int acc1 = tcc_ir_vreg_alloc_temp(ir);
  SValue s_acc1 = sv_var(acc1);
  tcc_ir_put(ir, TCCIR_OP_ADD, &s_p0, &s_p1, &s_acc1);
  int acc2 = tcc_ir_vreg_alloc_temp(ir);
  SValue s_acc2 = sv_var(acc2);
  tcc_ir_put(ir, TCCIR_OP_ADD, &s_acc1, &s_p2, &s_acc2);
  int acc3 = tcc_ir_vreg_alloc_temp(ir);
  SValue s_acc3 = sv_var(acc3);
  tcc_ir_put(ir, TCCIR_OP_ADD, &s_acc2, &s_t3, &s_acc3);
  tcc_ir_put(ir, TCCIR_OP_RETURNVALUE, &s_acc3, NULL, NULL);

  tcc_ir_ssa_regalloc(ir, arm_get_regalloc_target(), 0);
  tcc_ir_codegen_params_setup(ir);

  IRLiveInterval *li_p0 = tcc_ir_vreg_live_interval(ir, p0);
  IRLiveInterval *li_t3_before = tcc_ir_vreg_live_interval(ir, t3);
  UT_ASSERT(li_p0 != NULL && li_t3_before != NULL);
  UT_ASSERT_EQ(li_p0->allocation.r0, 0);      /* p0 pinned to R0 */
  UT_ASSERT_EQ(li_t3_before->allocation.r0, 3); /* t3 got R3 */

  /* Fake "instruction 0 needed to push R0" -- consumed by the first
   * SCRATCH_WRAP'd dispatch, t3's ASSIGN. R0 holds p0 (ABI-pinned, can't be
   * freed), forcing the R0-R3 fallback scan. */
  cgstub_set_next_insn_scratch(1, 1u << 0);

  ir->leaffunc = 1;
  tcc_ir_codegen_generate(ir);

  UT_ASSERT_EQ(cgstub_call_count("dry_run_start"), 2); /* discovery + rehearsal dry pass */

  /* p0/p1/p2 stay put (ABI-pinned, never reassignable). */
  UT_ASSERT_EQ(tcc_ir_vreg_live_interval(ir, p0)->allocation.r0, 0);
  UT_ASSERT_EQ(tcc_ir_vreg_live_interval(ir, p1)->allocation.r0, 1);
  UT_ASSERT_EQ(tcc_ir_vreg_live_interval(ir, p2)->allocation.r0, 2);
  /* t3 is the one the fallback loop could relocate -- moved off R3 onto a
   * free callee-saved register. */
  IRLiveInterval *li_t3_after = tcc_ir_vreg_live_interval(ir, t3);
  UT_ASSERT(li_t3_after != NULL);
  UT_ASSERT(li_t3_after->allocation.r0 != 3);
  UT_ASSERT(li_t3_after->allocation.r0 >= 4 && li_t3_after->allocation.r0 <= 11);
  UT_ASSERT(li_t3_after->allocation.r0 != 7); /* R_FP */

  tcc_ir_free(ir);
  return 0;
}

/* -------------------------------------------------------------------------- */
/* Scratch-save-area sizing: the "global bitmap as safety net for dry/real    */
/* divergence" branch (ir/codegen.c ~4377-4386), a case the doc's own         */
/* "out of scope" list named as tractable via the (already-existing)         */
/* cgstub_set_scratch_regs_pushed knob but never actually exercised.         */
/* -------------------------------------------------------------------------- */

/* No cgstub_set_next_insn_scratch() call in this test -> dry_insn_saves[i]
 * stays all-zero (one-shot knob, default 0) for every instruction, so the
 * per-instruction max_scratch_depth scan (~4368-4376) contributes 0. The
 * global-bitmap safety net (~4379-4385) is the ONLY source of a nonzero
 * max_scratch_depth here: cgstub_set_scratch_regs_pushed(0x7) has 3 bits
 * set, so ir->scratch_save_size must come out as (3*4+7)&~7 == 16, and
 * *not* stay 0 as it would if the global bitmap were never consulted. */
UT_TEST(test_dispatch_scratch_save_size_uses_global_bitmap_when_no_per_insn_saves)
{
  cgstub_reset();
  TCCIRState *ir = tcc_ir_alloc();
  tcc_state->registers_for_allocator = 6;
  tcc_state->registers_map_for_allocator = (1ull << 6) - 1;
  tcc_state->float_abi = ARM_HARD_FLOAT;
  tcc_state->float_registers_for_allocator = 32;
  tcc_state->float_registers_map_for_allocator = (1ull << 32) - 1;
  tcc_state->optimize = 1; /* -O1: at -O0 the dry-run walk this asserts on is skipped */
  tcc_state->need_frame_pointer = 0;
  tcc_state->force_frame_pointer = 0;

  enum
  {
    NPARAM = 6
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

  /* No cgstub_set_next_insn_scratch() -- deliberately leave per-instruction
   * dry_insn_saves at its all-zero default. */
  cgstub_set_scratch_regs_pushed(0x7u); /* 3 bits set: popcount == 3 */

  ir->leaffunc = 1;
  tcc_ir_codegen_generate(ir);

  UT_ASSERT_EQ(cgstub_call_count("dry_run_start"), 2); /* discovery + rehearsal dry pass */ /* confirms two-pass ran */
  UT_ASSERT_EQ(ir->scratch_save_size, 16);              /* (3*4+7) & ~7 == 16 */

  tcc_ir_free(ir);
  return 0;
}

/* Same construction, but the global-bitmap knob is left at its default (0,
 * i.e. "no scratch pushes observed at all") -- max_scratch_depth stays 0
 * from both sources, so the `if (max_scratch_depth > 0)` guard (~4387)
 * must NOT fire: scratch_save_size stays at its tcc_ir_alloc() default. */
UT_TEST(test_dispatch_scratch_save_size_stays_zero_when_no_scratch_pushes_at_all)
{
  cgstub_reset();
  TCCIRState *ir = tcc_ir_alloc();
  tcc_state->registers_for_allocator = 6;
  tcc_state->registers_map_for_allocator = (1ull << 6) - 1;
  tcc_state->float_abi = ARM_HARD_FLOAT;
  tcc_state->float_registers_for_allocator = 32;
  tcc_state->float_registers_map_for_allocator = (1ull << 32) - 1;
  tcc_state->optimize = 1; /* -O1: at -O0 the dry-run walk this asserts on is skipped */
  tcc_state->need_frame_pointer = 0;
  tcc_state->force_frame_pointer = 0;

  enum
  {
    NPARAM = 6
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

  UT_ASSERT_EQ(cgstub_call_count("dry_run_start"), 2); /* discovery + rehearsal dry pass */
  UT_ASSERT_EQ(ir->scratch_save_size, 0);

  tcc_ir_free(ir);
  return 0;
}
