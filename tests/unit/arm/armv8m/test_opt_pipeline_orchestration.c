/*
 *  test_opt_pipeline_orchestration.c - suite for ir/opt_pipeline.c's
 *  orchestration machinery itself (tcc_ir_opt_run_group / run_pipeline /
 *  get_pipeline / run_default / gen_pass_adapter), as opposed to the
 *  individual optimizer passes it drives (already covered per-pass by
 *  test_opt_*.c -- see docs/plan_ut_next_steps.md, 89/89 registered passes)
 *  or the gens_*_ex adapter wrappers already exercised by test_opt_fusion.c.
 *
 *  Per tests/unit/source_coverage_map.json's note on this file: "only the
 *  gens_*_ex adapter wrappers are exercised (test_opt_fusion.c); the pipeline
 *  orchestration/registration arrays are not." This suite targets exactly
 *  that gap: the generic IRPassGroup/IROptPass driver logic (trigger-pass
 *  short-circuit, per-pass flag gating, fixpoint iteration, compact_after,
 *  invalidation propagation) using small *locally-defined* pass tables (the
 *  real propagation_passes[]/memory_passes[]/etc. arrays are `static` and
 *  cannot be reached directly -- see the four PASS_GATED cascade wrappers
 *  documented as golden-IR-only in test_opt_branch_cascade.c for the same
 *  linkage constraint), plus the two gens_*_ex adapters that are NOT part
 *  of any PASS/PASS_GATED table and so were never exercised by
 *  test_opt_fusion.c: gens_call_result_ex (called directly from tccgen.c,
 *  three call sites) and gens_branch_ex (zero call sites anywhere outside
 *  their own definitions -- this suite is the only coverage either gets).
 */

#include <stddef.h> /* offsetof */

#include "ir_build.h"
#include "opt_engine.h"
#include "opt/flat/branch.h"
#include "opt_pipeline.h"

#include "ut.h"

/* Pipeline orchestration entry points (ir/opt_pipeline.c) are already
 * declared by opt_pipeline.h, included above: tcc_ir_opt_run_group,
 * tcc_ir_opt_run_pipeline, tcc_ir_opt_get_pipeline, tcc_ir_opt_run_default,
 * tcc_ir_opt_gen_pass_adapter, tcc_ir_opt_gens_call_result_ex,
 * tcc_ir_opt_gens_branch_ex. */

/* Legacy pass entry used only in a doc comment cross-reference below
 * (tcc_ir_opt_run_default's O0 pipeline calls this internally via its
 * dce_ex wrapper -- not called directly by this suite). */

#define I32 IROP_BTYPE_INT32
#define TOK_EQ 0x94 /* == */

/* ------------------------------------------------------------------ helpers */

static TCCIRState *utb_pool_new(void)
{
  TCCIRState *ir = utb_new();
  ir->iroperand_pool_capacity = UTB_MAX_OPERANDS;
  return ir;
}

/* A SYMREF operand referencing `sym` (used as a FUNCCALLVAL/FUNCCALLVOID
 * callee). Mirrors test_opt_dead_init_call.c's utb_callee_ref(). */
static IROperand utb_callee_ref(TCCIRState *ir, Sym *sym)
{
  uint32_t sidx = tcc_ir_pool_add_symref(ir, sym, 0, 0);
  return irop_make_symref(0, sidx, 0, 0, 0, I32);
}

/* A TEMP_LOCAL slot operand (anonymous compiler-generated stack temp),
 * identified by vreg in [-9,-2] rather than the usual positive vreg
 * encoding -- mirrors test_opt_store_fwd.c's utb_templocal(). Used both as
 * a FUNCCALLVAL dest (call result spilled to an anonymous slot) and as the
 * matching LOAD/STORE operand that later reads it back. */
static IROperand utb_templocal(int32_t vreg, int32_t off, int is_lval, int btype)
{
  return irop_make_stackoff(vreg, off, is_lval, /*is_llocal*/ 0, /*is_param*/ 0, btype);
}

/* Run a gens table via the generic tcc_ir_opt_gen_pass_adapter, mirroring
 * how a PASS_GATED entry using tcc_ir_opt_gen_pass_adapter with an
 * IROptGenPassData* would be invoked (the adapter itself is a one-line
 * `return tcc_ir_opt_run_gens(ctx, data->gens, data->count);`, currently
 * 0%-covered since every real pass instead uses a per-table _ex wrapper). */
static int run_gen_pass_adapter(TCCIRState *ir, const IROptGen *gens, int count)
{
  IROptCtx ctx;
  tcc_ir_opt_ctx_init(&ctx, ir);
  IROptGenPassData data = { gens, count };
  int changes = tcc_ir_opt_gen_pass_adapter(&ctx, &data);
  tcc_ir_opt_ctx_free(&ctx);
  return changes;
}

/* ============================================================== fake passes
 *
 * tcc_ir_opt_run_group drives *any* IRPassGroup, but the real tables in
 * ir/opt_pipeline.c (propagation_passes[], memory_passes[], ...) are file
 * `static` and unreachable from a unit TU. To test the driver logic itself
 * (trigger short-circuit, flag gating, fixpoint, compact_after,
 * invalidation) in isolation from any specific optimizer semantics, build
 * tiny local IRPassGroup tables out of counter/marker fake passes that
 * record exactly how many times -- and in what ir-state -- they were
 * invoked. This is oracle-precise (exact call counts) rather than
 * characterization of a real pass's behavior. */

/* Shared counters, reset before each test. */
static int g_calls_a, g_calls_b, g_calls_trigger;
static int g_last_seen_n_a; /* ir->next_instruction_index seen by pass A */

static void reset_fake_counters(void)
{
  g_calls_a = g_calls_b = g_calls_trigger = 0;
  g_last_seen_n_a = -1;
}

/* Pass A: always reports 1 change on its first two calls, then 0 (so a
 * group converges after a bounded number of rounds without an outer
 * iteration cap masking the convergence check). */
static int fake_pass_a(IROptCtx *ctx)
{
  g_calls_a++;
  g_last_seen_n_a = ctx->ir->next_instruction_index;
  return (g_calls_a <= 2) ? 1 : 0;
}

/* Pass B: counts calls, always reports 0 changes. */
static int fake_pass_b(IROptCtx *ctx)
{
  (void)ctx;
  g_calls_b++;
  return 0;
}

/* Pass B, NOP-emitting variant: converts instruction 0 to NOP on its first
 * call (to exercise group->compact_after), 0 changes thereafter. */
static int fake_pass_b_nops_first_instr(IROptCtx *ctx)
{
  g_calls_b++;
  if (g_calls_b == 1 && ctx->ir->next_instruction_index > 0) {
    ctx->ir->compact_instructions[0].op = TCCIR_OP_NOP;
    return 1;
  }
  return 0;
}

/* Trigger pass: returns the value pointed to by a static int, so a test can
 * flip it between "found work" (>0, group continues) and "no work" (0,
 * group exits immediately per tcc_ir_opt_run_group's trigger contract). */
static int g_trigger_return = 1;
static int fake_trigger(IROptCtx *ctx)
{
  (void)ctx;
  g_calls_trigger++;
  return g_trigger_return;
}

/* A pass that must never run (used as a negative oracle: flag-gated off,
 * or skipped because the trigger returned 0). Calling it at all is a
 * hard test failure via UT_ASSERT_EQ(calls, 0) at the call site, but we
 * also abort loudly so a bug is unmistakable even if the assert is
 * miscounted. */
static int g_calls_never = 0;
static int fake_pass_never(IROptCtx *ctx)
{
  (void)ctx;
  g_calls_never++;
  return 0;
}

/* ================================================================== run_group: trigger */

/* POSITIVE: trigger pass returns >0 -- the group proceeds to run the
 * remaining (non-trigger) passes in the same round. */
UT_TEST(test_run_group_trigger_nonzero_runs_remaining_passes)
{
  reset_fake_counters();
  g_trigger_return = 1;
  TCCIRState *ir = utb_new();
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_imm(0, I32), UTB_NONE);

  IROptPass passes[] = {
    { "trigger", fake_trigger, 0, 0, 0 },
    { "b",       fake_pass_b,  0, 0, 0 },
  };
  IRPassGroup group = { "grp", passes, 2, /*max_iterations*/ 1, /*compact_after*/ 0, /*trigger_idx*/ 0 };

  IROptCtx ctx;
  tcc_ir_opt_ctx_init(&ctx, ir);
  int total = tcc_ir_opt_run_group(&ctx, &group);
  tcc_ir_opt_ctx_free(&ctx);

  UT_ASSERT_EQ(g_calls_trigger, 1);
  UT_ASSERT_EQ(g_calls_b, 1);
  UT_ASSERT_EQ(total, 1); /* trigger's own return value counted as round_changes */

  utb_free(ir);
  return 0;
}

/* NEGATIVE (guard): trigger pass returns 0 -- the group exits immediately;
 * no other pass in the group runs at all, on the very first iteration. */
UT_TEST(test_run_group_trigger_zero_skips_remaining_passes)
{
  reset_fake_counters();
  g_trigger_return = 0;
  g_calls_never = 0;
  TCCIRState *ir = utb_new();
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_imm(0, I32), UTB_NONE);

  IROptPass passes[] = {
    { "trigger", fake_trigger,     0, 0, 0 },
    { "never",   fake_pass_never,  0, 0, 0 },
  };
  IRPassGroup group = { "grp", passes, 2, /*max_iterations*/ 3, 0, /*trigger_idx*/ 0 };

  IROptCtx ctx;
  tcc_ir_opt_ctx_init(&ctx, ir);
  int total = tcc_ir_opt_run_group(&ctx, &group);
  tcc_ir_opt_ctx_free(&ctx);

  UT_ASSERT_EQ(g_calls_trigger, 1); /* only the first iteration's trigger call happens */
  UT_ASSERT_EQ(g_calls_never, 0);
  UT_ASSERT_EQ(total, 0);

  utb_free(ir);
  return 0;
}

/* ================================================================== run_group: flag gating */

/* NEGATIVE (guard): a PASS_GATED-style entry whose flag_offset points at a
 * zero TCCState byte is skipped entirely -- flag gating is checked before
 * the pass ever runs. */
UT_TEST(test_run_group_flag_gate_zero_skips_pass)
{
  reset_fake_counters();
  tcc_state->opt_dce = 0;
  TCCIRState *ir = utb_new();
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_imm(0, I32), UTB_NONE);

  IROptPass passes[] = {
    { "gated_b", fake_pass_b, 0, 0, (uint16_t)offsetof(TCCState, opt_dce) },
  };
  IRPassGroup group = { "grp", passes, 1, 1, 0, -1 };

  IROptCtx ctx;
  tcc_ir_opt_ctx_init(&ctx, ir);
  int total = tcc_ir_opt_run_group(&ctx, &group);
  tcc_ir_opt_ctx_free(&ctx);

  UT_ASSERT_EQ(g_calls_b, 0);
  UT_ASSERT_EQ(total, 0);

  utb_free(ir);
  return 0;
}

/* POSITIVE: same gated entry, flag byte set to 1 -- the pass runs. */
UT_TEST(test_run_group_flag_gate_nonzero_runs_pass)
{
  reset_fake_counters();
  tcc_state->opt_dce = 1;
  TCCIRState *ir = utb_new();
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_imm(0, I32), UTB_NONE);

  IROptPass passes[] = {
    { "gated_b", fake_pass_b, 0, 0, (uint16_t)offsetof(TCCState, opt_dce) },
  };
  IRPassGroup group = { "grp", passes, 1, 1, 0, -1 };

  IROptCtx ctx;
  tcc_ir_opt_ctx_init(&ctx, ir);
  tcc_ir_opt_run_group(&ctx, &group);
  tcc_ir_opt_ctx_free(&ctx);

  UT_ASSERT_EQ(g_calls_b, 1);

  tcc_state->opt_dce = 0;
  utb_free(ir);
  return 0;
}

/* NEGATIVE (guard): flag gating also applies to the *trigger* slot -- a
 * gated trigger with its flag byte 0 breaks the group before even calling
 * tcc_ir_opt_pass_disabled or run() on it. */
UT_TEST(test_run_group_gated_trigger_zero_breaks_before_running)
{
  reset_fake_counters();
  tcc_state->opt_const_prop = 0;
  TCCIRState *ir = utb_new();
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_imm(0, I32), UTB_NONE);

  IROptPass passes[] = {
    { "trigger", fake_trigger, 0, 0, (uint16_t)offsetof(TCCState, opt_const_prop) },
    { "never",   fake_pass_never, 0, 0, 0 },
  };
  IRPassGroup group = { "grp", passes, 2, 5, 0, 0 };

  IROptCtx ctx;
  tcc_ir_opt_ctx_init(&ctx, ir);
  int total = tcc_ir_opt_run_group(&ctx, &group);
  tcc_ir_opt_ctx_free(&ctx);

  UT_ASSERT_EQ(g_calls_trigger, 0); /* trigger->run never called: gate checked first */
  UT_ASSERT_EQ(g_calls_never, 0);
  UT_ASSERT_EQ(total, 0);

  utb_free(ir);
  return 0;
}

/* ================================================================== run_group: fixpoint iteration */

/* POSITIVE: a non-trigger group with max_iterations=5 iterates pass A to
 * its own fixpoint (converges after round 3, once fake_pass_a starts
 * returning 0) rather than always running the full 5 rounds. */
UT_TEST(test_run_group_converges_before_max_iterations)
{
  reset_fake_counters();
  TCCIRState *ir = utb_new();
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_imm(0, I32), UTB_NONE);

  IROptPass passes[] = {
    { "a", fake_pass_a, 0, 0, 0 },
  };
  IRPassGroup group = { "grp", passes, 1, /*max_iterations*/ 5, 0, -1 };

  IROptCtx ctx;
  tcc_ir_opt_ctx_init(&ctx, ir);
  int total = tcc_ir_opt_run_group(&ctx, &group);
  tcc_ir_opt_ctx_free(&ctx);

  /* fake_pass_a returns 1,1,0 on calls 1,2,3 -- round 3 has round_changes==0
   * and no trigger, so the loop breaks right there: exactly 3 calls, not 5. */
  UT_ASSERT_EQ(g_calls_a, 3);
  UT_ASSERT_EQ(total, 2);

  utb_free(ir);
  return 0;
}

/* NEGATIVE (guard): a pass that never converges is capped at exactly
 * max_iterations calls -- the driver does not loop forever. */
static int fake_pass_always_changes(IROptCtx *ctx)
{
  (void)ctx;
  g_calls_a++;
  return 1;
}

UT_TEST(test_run_group_stops_at_max_iterations_when_never_converging)
{
  reset_fake_counters();
  TCCIRState *ir = utb_new();
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_imm(0, I32), UTB_NONE);

  IROptPass passes[] = {
    { "a", fake_pass_always_changes, 0, 0, 0 },
  };
  IRPassGroup group = { "grp", passes, 1, /*max_iterations*/ 4, 0, -1 };

  IROptCtx ctx;
  tcc_ir_opt_ctx_init(&ctx, ir);
  int total = tcc_ir_opt_run_group(&ctx, &group);
  tcc_ir_opt_ctx_free(&ctx);

  UT_ASSERT_EQ(g_calls_a, 4);
  UT_ASSERT_EQ(total, 4);

  utb_free(ir);
  return 0;
}

/* NEGATIVE (guard): with a trigger present, the group's continuation is
 * controlled *solely* by the trigger's own return value, never by whether
 * the other (non-trigger) passes in the group are still finding work. A
 * trigger that keeps reporting 1 change every round, paired with a
 * companion pass that reports 0 changes on every single call, still drives
 * the group through the full max_iterations. The bottom-of-loop fixpoint
 * exit `if (round_changes == 0) break;` never fires for a trigger group
 * because round_changes always includes the trigger's tch > 0 (the trigger's
 * own `tch <= 0` check breaks first once it stops finding work). This is the
 * behavior the (now-removed) redundant `&& group->trigger_idx < 0` clause was
 * relied upon for -- see docs/bugs.md #4. */
UT_TEST(test_run_group_trigger_present_ignores_zero_round_changes_shortcut)
{
  reset_fake_counters();
  g_trigger_return = 1;
  TCCIRState *ir = utb_new();
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_imm(0, I32), UTB_NONE);

  IROptPass passes[] = {
    { "trigger", fake_trigger, 0, 0, 0 },
    { "b",       fake_pass_b,  0, 0, 0 }, /* always 0 changes */
  };
  IRPassGroup group = { "grp", passes, 2, /*max_iterations*/ 3, 0, 0 };

  IROptCtx ctx;
  tcc_ir_opt_ctx_init(&ctx, ir);
  tcc_ir_opt_run_group(&ctx, &group);
  tcc_ir_opt_ctx_free(&ctx);

  UT_ASSERT_EQ(g_calls_trigger, 3);
  UT_ASSERT_EQ(g_calls_b, 3);

  utb_free(ir);
  return 0;
}

/* ================================================================== run_group: compact_after */

/* POSITIVE: group->compact_after with round_changes>0 compacts NOPs out of
 * ir->compact_instructions and shrinks next_instruction_index, and the next
 * pass in a later round observes the compacted instruction count (proving
 * ctx was invalidated/refreshed, not just the IR array). */
UT_TEST(test_run_group_compact_after_shrinks_instruction_count)
{
  reset_fake_counters();
  TCCIRState *ir = utb_new();
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(1, I32), UTB_NONE); /* [0]: NOPed by pass B */
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_imm(0, I32), UTB_NONE);    /* [1] */
  int before_n = ir->next_instruction_index;

  IROptPass passes[] = {
    { "b", fake_pass_b_nops_first_instr, 0, 0, 0 },
    { "a", fake_pass_a,                  0, 0, 0 }, /* records ctx->ir->next_instruction_index it observes */
  };
  IRPassGroup group = { "grp", passes, 2, /*max_iterations*/ 3, /*compact_after*/ 1, -1 };

  IROptCtx ctx;
  tcc_ir_opt_ctx_init(&ctx, ir);
  tcc_ir_opt_run_group(&ctx, &group);
  tcc_ir_opt_ctx_free(&ctx);

  UT_ASSERT_EQ(before_n, 2);
  UT_ASSERT_EQ(ir->next_instruction_index, 1); /* the NOP was compacted away */
  /* Pass A's *last* call (round 2 or 3) must see the post-compaction count
   * (1), not round 1's pre-compaction snapshot (2) -- proves compact_after
   * ran (and invalidated ctx->n) before the next round started, not just
   * once at the very end. */
  UT_ASSERT_EQ(g_last_seen_n_a, 1);

  utb_free(ir);
  return 0;
}

/* NEGATIVE (guard): compact_after set, but no pass ever reports a change
 * (round_changes stays 0 every round) -- compaction must not run, and an
 * existing NOP in the IR survives untouched. */
UT_TEST(test_run_group_compact_after_skipped_when_no_changes)
{
  reset_fake_counters();
  TCCIRState *ir = utb_new();
  utb_emit(ir, TCCIR_OP_NOP, UTB_NONE, UTB_NONE, UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_imm(0, I32), UTB_NONE);

  IROptPass passes[] = {
    { "b", fake_pass_b, 0, 0, 0 }, /* always reports 0 changes, never touches the IR */
  };
  IRPassGroup group = { "grp", passes, 1, 2, /*compact_after*/ 1, -1 };

  IROptCtx ctx;
  tcc_ir_opt_ctx_init(&ctx, ir);
  tcc_ir_opt_run_group(&ctx, &group);
  tcc_ir_opt_ctx_free(&ctx);

  UT_ASSERT_EQ(ir->next_instruction_index, 2); /* untouched: no compaction happened */
  UT_ASSERT_EQ(utb_op(ir, 0), TCCIR_OP_NOP);

  utb_free(ir);
  return 0;
}

/* ================================================================== run_group: requirements */

/* POSITIVE: a pass declaring IR_PASS_REQUIRES_DU causes tcc_ir_opt_run_group
 * to lazily build ctx->du (via pipeline_ensure_requirements) before the pass
 * runs -- observable as ctx->du.def being non-NULL inside the pass body. */
static int g_saw_du_built;
static int fake_pass_checks_du(IROptCtx *ctx)
{
  g_saw_du_built = (ctx->du.def != NULL);
  return 0;
}

UT_TEST(test_run_group_requires_du_builds_before_pass_runs)
{
  reset_fake_counters();
  g_saw_du_built = -1;
  TCCIRState *ir = utb_new();
  ir->next_temporary_variable = 1;
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(1, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_temp(0, I32), UTB_NONE);

  IROptPass passes[] = {
    { "du_check", fake_pass_checks_du, IR_PASS_REQUIRES_DU, 0, 0 },
  };
  IRPassGroup group = { "grp", passes, 1, 1, 0, -1 };

  IROptCtx ctx;
  tcc_ir_opt_ctx_init(&ctx, ir);
  UT_ASSERT(ctx.du.def == NULL); /* not built yet at ctx_init */
  tcc_ir_opt_run_group(&ctx, &group);

  UT_ASSERT_EQ(g_saw_du_built, 1);

  tcc_ir_opt_ctx_free(&ctx);
  utb_free(ir);
  return 0;
}

/* ================================================================== run_pipeline */

/* POSITIVE: tcc_ir_opt_run_pipeline drives multiple groups in sequence
 * (group g1 fully converging before g2 ever runs -- g_calls_a reaches its
 * final count of 3 entirely within g1) and sums their change counts across
 * groups. g2's own compact_after=1 both shrinks ir->compact_instructions
 * (verified directly) and additionally triggers
 * tcc_ir_opt_run_pipeline's own post-group
 * `if (groups[g].compact_after) tcc_ir_opt_ctx_invalidate(&ctx);` (this
 * second invalidation is redundant with run_group's own internal one in
 * this case -- both fire off the same compact_after flag -- but is still
 * real code executed on every compact_after group, so covering it here is
 * not vacuous). */
UT_TEST(test_run_pipeline_runs_groups_in_order_and_sums_changes)
{
  reset_fake_counters();
  TCCIRState *ir = utb_new();
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(1, I32), UTB_NONE); /* [0] */
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_imm(0, I32), UTB_NONE);    /* [1] */

  IROptPass passes1[] = { { "a", fake_pass_a, 0, 0, 0 } };            /* 1,1,0 -> 2 changes, 3 calls */
  IROptPass passes2[] = { { "b_nop", fake_pass_b_nops_first_instr, 0, 0, 0 } }; /* 1 change (NOPs instr 0) */
  IRPassGroup groups[] = {
    { "g1", passes1, 1, 5, 0, -1 },
    { "g2", passes2, 1, 1, /*compact_after*/ 1, -1 },
  };

  int total = tcc_ir_opt_run_pipeline(ir, groups, 2);

  UT_ASSERT_EQ(g_calls_a, 3);
  UT_ASSERT_EQ(g_calls_b, 1);
  UT_ASSERT_EQ(total, 2 + 1);
  UT_ASSERT_EQ(ir->next_instruction_index, 1); /* g2's compact_after removed the NOPed instr */

  utb_free(ir);
  return 0;
}

/* NEGATIVE (guard): an empty group list is a legal no-op -- returns 0
 * changes, IR untouched. */
UT_TEST(test_run_pipeline_zero_groups_is_noop)
{
  TCCIRState *ir = utb_new();
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_imm(0, I32), UTB_NONE);

  int total = tcc_ir_opt_run_pipeline(ir, NULL, 0);

  UT_ASSERT_EQ(total, 0);
  UT_ASSERT_EQ(ir->next_instruction_index, 1);

  utb_free(ir);
  return 0;
}

/* ================================================================== get_pipeline / run_default */

/* POSITIVE: tcc_ir_opt_get_pipeline reports the documented (group-table,
 * count) pair per level -- oracle asserts on the exact group names/counts
 * rather than just "non-NULL", since those names/counts are exactly what
 * the O0/O1/O2/Os preset tables promise (ir/opt_pipeline.c:521-548). */
UT_TEST(test_get_pipeline_level_0_is_single_cleanup_group)
{
  const IRPassGroup *groups;
  int count;
  tcc_ir_opt_get_pipeline(IR_OPT_LEVEL_0, &groups, &count);
  UT_ASSERT_EQ(count, 1);
  UT_ASSERT_STREQ(groups[0].name, "cleanup");
  UT_ASSERT_EQ(groups[0].count, 1); /* o0_passes[] has exactly one entry: dce */
  return 0;
}

UT_TEST(test_get_pipeline_level_1_is_two_groups)
{
  const IRPassGroup *groups;
  int count;
  tcc_ir_opt_get_pipeline(IR_OPT_LEVEL_1, &groups, &count);
  UT_ASSERT_EQ(count, 2);
  UT_ASSERT_STREQ(groups[0].name, "propagation");
  UT_ASSERT_STREQ(groups[1].name, "late_cleanup");
  return 0;
}

UT_TEST(test_get_pipeline_level_2_is_four_groups)
{
  const IRPassGroup *groups;
  int count;
  tcc_ir_opt_get_pipeline(IR_OPT_LEVEL_2, &groups, &count);
  UT_ASSERT_EQ(count, 4);
  UT_ASSERT_STREQ(groups[0].name, "propagation");
  UT_ASSERT_STREQ(groups[1].name, "memory");
  UT_ASSERT_STREQ(groups[2].name, "fusion");
  UT_ASSERT_STREQ(groups[3].name, "late_cleanup");
  return 0;
}

UT_TEST(test_get_pipeline_level_s_is_three_groups_no_fusion)
{
  const IRPassGroup *groups;
  int count;
  tcc_ir_opt_get_pipeline(IR_OPT_LEVEL_S, &groups, &count);
  UT_ASSERT_EQ(count, 3);
  UT_ASSERT_STREQ(groups[0].name, "propagation");
  UT_ASSERT_STREQ(groups[1].name, "memory");
  UT_ASSERT_STREQ(groups[2].name, "late_cleanup");
  /* Os intentionally skips "fusion" (kept smaller code size) -- assert its
   * absence by name, not just the count above. */
  for (int i = 0; i < count; i++)
    UT_ASSERT(strcmp(groups[i].name, "fusion") != 0);
  return 0;
}

/* NEGATIVE (guard): an out-of-range level value falls through to the
 * `default:` arm, which is the O2 (4-group) pipeline -- same as passing
 * IR_OPT_LEVEL_2 explicitly. Documents the switch's fallback behavior. */
UT_TEST(test_get_pipeline_unknown_level_falls_back_to_o2)
{
  const IRPassGroup *groups;
  int count;
  tcc_ir_opt_get_pipeline((IROptLevel)99, &groups, &count);
  UT_ASSERT_EQ(count, 4);
  UT_ASSERT_STREQ(groups[0].name, "propagation");
  return 0;
}

/* POSITIVE (integration): tcc_ir_opt_run_default(ir, IR_OPT_LEVEL_0) drives
 * get_pipeline + run_pipeline end-to-end through the *real* (non-fake) O0
 * table -- o0_passes[]'s single "dce" entry has flag_offset==0 (unconditional,
 * not PASS_GATED), so it is reachable without any tcc_state flag setup. DCE
 * on a function with a provably-dead instruction after an unconditional jump
 * over it NOPs the dead one; run_default must report that 1 change. */
UT_TEST(test_run_default_level_0_runs_real_dce_pass)
{
  TCCIRState *ir = utb_new();
  /* [0] JUMP 2            -- always taken
   * [1] ASSIGN T0 <- #99  -- unreachable, dead
   * [2] RETURNVALUE #0 */
  utb_emit(ir, TCCIR_OP_JUMP, utb_imm(2, I32), UTB_NONE, UTB_NONE);
  int dead = utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(99, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_imm(0, I32), UTB_NONE);

  int total = tcc_ir_opt_run_default(ir, IR_OPT_LEVEL_0);

  UT_ASSERT(total >= 1);
  UT_ASSERT_EQ(utb_op(ir, dead), TCCIR_OP_NOP);

  utb_free(ir);
  return 0;
}

/* ================================================================== gen_pass_adapter */

/* POSITIVE: tcc_ir_opt_gen_pass_adapter is a one-line forwarding shim
 * (`return tcc_ir_opt_run_gens(ctx, data->gens, data->count);`) -- exercise
 * it directly with the same branch_gens table the setif_fuse/branch_fold
 * passes already cover via their own dedicated entry points, proving the
 * *adapter* forwards both the table pointer and count correctly (a count
 * transposition bug, e.g., would still "work" for a 1-entry table but not
 * a 3-entry one). Reuses setif fusion's CMP+SETIF+TEST_ZERO+JUMPIF shape
 * from test_opt_branch_cascade.c since it's a real, non-trivial branch_gens
 * transform. */
UT_TEST(test_gen_pass_adapter_forwards_table_and_count)
{
  TCCIRState *ir = utb_pool_new();
  ir->temporary_variables_live_intervals_size = 16;
  ir->temporary_variables_live_intervals = (IRLiveInterval *)tcc_mallocz(sizeof(IRLiveInterval) * 16);
  ir->next_temporary_variable = 4;

  /* Minimal branch_fold_cmp shape: CMP #3,#3 + JUMPIF EQ -- both operands
   * constant, so the branch_gens table folds JUMPIF to unconditional JUMP
   * and NOPs the CMP. */
  int cmp = utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_imm(3, I32), utb_imm(3, I32));
  int jumpif = utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(3, I32), utb_imm(TOK_EQ, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_imm(0, I32), UTB_NONE);

  int changes = run_gen_pass_adapter(ir, branch_gens, branch_gens_count);

  UT_ASSERT(changes > 0);
  UT_ASSERT_EQ(utb_op(ir, cmp), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, jumpif), TCCIR_OP_JUMP);

  utb_free(ir);
  return 0;
}

/* NEGATIVE (guard): an empty gens table (count=0) is a legal no-op --
 * confirms the adapter doesn't dereference gens[0] when count is 0. */
UT_TEST(test_gen_pass_adapter_empty_table_is_noop)
{
  TCCIRState *ir = utb_new();
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_imm(0, I32), UTB_NONE);

  int changes = run_gen_pass_adapter(ir, NULL, 0);

  UT_ASSERT_EQ(changes, 0);

  utb_free(ir);
  return 0;
}

/* ================================================================== gens_branch_ex */

/* POSITIVE: tcc_ir_opt_gens_branch_ex (the dedicated pipeline adapter, as
 * opposed to the generic gen_pass_adapter exercised above) drives the same
 * branch_gens table through its own _ex wrapper. Unlike setif_fuse/branch_fold
 * (which call tcc_ir_opt_run_gens(&ctx, branch_gens, ...) directly from
 * ir/opt_branch.c and never touch this adapter), tcc_ir_opt_gens_branch_ex
 * itself has no other call site anywhere in the tree (grep confirms it is
 * declared + defined but never invoked outside this test) -- so this is the
 * only coverage this specific function will ever get. */
UT_TEST(test_gens_branch_ex_folds_constant_cmp_via_dedicated_adapter)
{
  TCCIRState *ir = utb_pool_new();
  ir->temporary_variables_live_intervals_size = 16;
  ir->temporary_variables_live_intervals = (IRLiveInterval *)tcc_mallocz(sizeof(IRLiveInterval) * 16);
  ir->next_temporary_variable = 4;

  int cmp = utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_imm(5, I32), utb_imm(9, I32));
  int jumpif = utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(3, I32), utb_imm(TOK_EQ, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_imm(0, I32), UTB_NONE);

  IROptCtx ctx;
  tcc_ir_opt_ctx_init(&ctx, ir);
  int changes = tcc_ir_opt_gens_branch_ex(&ctx);
  tcc_ir_opt_ctx_free(&ctx);

  /* 5 == 9 is statically false -- JUMPIF is dead (NOPed), CMP orphaned
   * (also NOPed by the same branch_fold_cmp generator once its consumer
   * is gone -- branch_gens folds both in one pass here since CMP has no
   * other use). */
  UT_ASSERT(changes > 0);
  UT_ASSERT_EQ(utb_op(ir, jumpif), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, cmp), TCCIR_OP_NOP);

  utb_free(ir);
  return 0;
}

/* ================================================================== gens_call_result_ex */

/* POSITIVE: dead_call_result generator -- a FUNCCALLVAL whose result TEMP
 * has zero uses demotes to FUNCCALLVOID (dropping the dest operand). */
UT_TEST(test_gens_call_result_ex_dead_result_demotes_to_funccallvoid)
{
  static Sym callee;
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);
  ir->temporary_variables_live_intervals_size = 16;
  ir->temporary_variables_live_intervals = (IRLiveInterval *)tcc_mallocz(sizeof(IRLiveInterval) * 16);
  ir->next_temporary_variable = 1;

  int call = utb_emit(ir, TCCIR_OP_FUNCCALLVAL, utb_temp(0, I32), utb_callee_ref(ir, &callee),
                       utb_imm((int32_t)TCCIR_ENCODE_CALL(1, 0), I32));
  /* T0 (the call result) is never read anywhere below. */
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_imm(0, I32), UTB_NONE);

  IROptCtx ctx;
  tcc_ir_opt_ctx_init(&ctx, ir);
  int changes = tcc_ir_opt_gens_call_result_ex(&ctx);
  tcc_ir_opt_ctx_free(&ctx);

  UT_ASSERT_EQ(changes, 1);
  UT_ASSERT_EQ(utb_op(ir, call), TCCIR_OP_FUNCCALLVOID);

  utb_free(ir);
  return 0;
}

/* NEGATIVE (guard): the call result IS read later -- dead_call_result must
 * not fire, and the call stays FUNCCALLVAL. */
UT_TEST(test_gens_call_result_ex_used_result_kept_funccallval)
{
  static Sym callee;
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);
  ir->temporary_variables_live_intervals_size = 16;
  ir->temporary_variables_live_intervals = (IRLiveInterval *)tcc_mallocz(sizeof(IRLiveInterval) * 16);
  ir->next_temporary_variable = 1;

  int call = utb_emit(ir, TCCIR_OP_FUNCCALLVAL, utb_temp(0, I32), utb_callee_ref(ir, &callee),
                       utb_imm((int32_t)TCCIR_ENCODE_CALL(1, 0), I32));
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_temp(0, I32), UTB_NONE); /* reads T0 */

  IROptCtx ctx;
  tcc_ir_opt_ctx_init(&ctx, ir);
  int changes = tcc_ir_opt_gens_call_result_ex(&ctx);
  tcc_ir_opt_ctx_free(&ctx);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, call), TCCIR_OP_FUNCCALLVAL);

  utb_free(ir);
  return 0;
}

/* ================================================================== dead_call_result: TEMP_LOCAL dest */

/* POSITIVE: a FUNCCALLVAL whose result is spilled directly to a TEMP_LOCAL
 * slot (vr in [-9,-2], e.g. a call returning a struct-by-value temp) with no
 * subsequent reference anywhere -- the manual forward scan (DU doesn't cover
 * TEMP_LOCAL vregs) finds nothing and the call demotes to FUNCCALLVOID. */
UT_TEST(test_gens_call_result_ex_dead_templocal_result_demotes)
{
  static Sym callee;
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);

  int call = utb_emit(ir, TCCIR_OP_FUNCCALLVAL, utb_templocal(-2, 0, /*is_lval*/ 1, I32),
                      utb_callee_ref(ir, &callee), utb_imm((int32_t)TCCIR_ENCODE_CALL(1, 0), I32));
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_imm(0, I32), UTB_NONE);

  IROptCtx ctx;
  tcc_ir_opt_ctx_init(&ctx, ir);
  int changes = tcc_ir_opt_gens_call_result_ex(&ctx);
  tcc_ir_opt_ctx_free(&ctx);

  UT_ASSERT_EQ(changes, 1);
  UT_ASSERT_EQ(utb_op(ir, call), TCCIR_OP_FUNCCALLVOID);

  utb_free(ir);
  return 0;
}

/* NEGATIVE (guard): the TEMP_LOCAL slot IS read afterward (a LOAD from the
 * same vreg+offset) -- the manual forward scan must find it and keep the
 * call as FUNCCALLVAL. */
UT_TEST(test_gens_call_result_ex_used_templocal_result_kept)
{
  static Sym callee;
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);

  int call = utb_emit(ir, TCCIR_OP_FUNCCALLVAL, utb_templocal(-2, 0, /*is_lval*/ 1, I32),
                      utb_callee_ref(ir, &callee), utb_imm((int32_t)TCCIR_ENCODE_CALL(1, 0), I32));
  utb_emit(ir, TCCIR_OP_LOAD, utb_temp(0, I32), utb_templocal(-2, 0, /*is_lval*/ 1, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_temp(0, I32), UTB_NONE);

  IROptCtx ctx;
  tcc_ir_opt_ctx_init(&ctx, ir);
  int changes = tcc_ir_opt_gens_call_result_ex(&ctx);
  tcc_ir_opt_ctx_free(&ctx);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, call), TCCIR_OP_FUNCCALLVAL);

  utb_free(ir);
  return 0;
}

/* NEGATIVE (guard): the TEMP_LOCAL slot is consumed only as an MLA
 * accumulator (operand_base+3, missed by the three-slot dest/src1/src2
 * scan) -- the dedicated MLA accumulator check must still catch it and keep
 * the call. Without that check this would be a false "dead result" and the
 * accumulate would silently read garbage/zero after the CALL is voided. */
UT_TEST(test_gens_call_result_ex_templocal_used_as_mla_accum_kept)
{
  static Sym callee;
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);

  int call = utb_emit(ir, TCCIR_OP_FUNCCALLVAL, utb_templocal(-2, 0, /*is_lval*/ 1, I32),
                      utb_callee_ref(ir, &callee), utb_imm((int32_t)TCCIR_ENCODE_CALL(1, 0), I32));
  /* MLA: T1 = T2 * T3 + StackLoc[-2@0] (accumulator slot 4) */
  utb_emit4(ir, TCCIR_OP_MLA, utb_temp(1, I32), utb_temp(2, I32), utb_temp(3, I32),
           utb_templocal(-2, 0, /*is_lval*/ 1, I32));
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_temp(1, I32), UTB_NONE);

  IROptCtx ctx;
  tcc_ir_opt_ctx_init(&ctx, ir);
  int changes = tcc_ir_opt_gens_call_result_ex(&ctx);
  tcc_ir_opt_ctx_free(&ctx);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, call), TCCIR_OP_FUNCCALLVAL);

  utb_free(ir);
  return 0;
}

/* NEGATIVE (guard): dest_vr is outside the TEMP_LOCAL sentinel range
 * (< -9, i.e. beyond the 4-bit negative-index encoding's practical carve-out
 * that this generator specifically recognizes) -- `dest_vr > -2 || dest_vr <
 * -9` rejects it up front and the call is left untouched (no manual scan, no
 * crash) even though nothing references it. */
UT_TEST(test_gens_call_result_ex_templocal_out_of_range_kept)
{
  static Sym callee;
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);

  int call = utb_emit(ir, TCCIR_OP_FUNCCALLVAL, utb_templocal(-10, 0, /*is_lval*/ 1, I32),
                      utb_callee_ref(ir, &callee), utb_imm((int32_t)TCCIR_ENCODE_CALL(1, 0), I32));
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_imm(0, I32), UTB_NONE);

  IROptCtx ctx;
  tcc_ir_opt_ctx_init(&ctx, ir);
  int changes = tcc_ir_opt_gens_call_result_ex(&ctx);
  tcc_ir_opt_ctx_free(&ctx);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, call), TCCIR_OP_FUNCCALLVAL);

  utb_free(ir);
  return 0;
}

/* NEGATIVE (guard): dest vreg type is PARAM -- dead_call_result must never
 * fire on a call whose "result" is written directly into a parameter slot
 * (the TCCIR_DECODE_VREG_TYPE(dest_vr) == TCCIR_VREG_TYPE_PARAM guard at the
 * very top), regardless of DU use count. */
UT_TEST(test_gens_call_result_ex_param_dest_kept)
{
  static Sym callee;
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);
  ir->temporary_variables_live_intervals_size = 16;
  ir->temporary_variables_live_intervals = (IRLiveInterval *)tcc_mallocz(sizeof(IRLiveInterval) * 16);
  ir->next_temporary_variable = 1;

  int call = utb_emit(ir, TCCIR_OP_FUNCCALLVAL, utb_param(0, I32), utb_callee_ref(ir, &callee),
                      utb_imm((int32_t)TCCIR_ENCODE_CALL(1, 0), I32));
  /* No use of P0 anywhere below -- if the PARAM guard were missing this
   * would otherwise look dead-result eligible. */
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_imm(0, I32), UTB_NONE);

  IROptCtx ctx;
  tcc_ir_opt_ctx_init(&ctx, ir);
  int changes = tcc_ir_opt_gens_call_result_ex(&ctx);
  tcc_ir_opt_ctx_free(&ctx);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, call), TCCIR_OP_FUNCCALLVAL);

  utb_free(ir);
  return 0;
}

/* ================================================================== fold_call_result_store */

/* POSITIVE: the classic CALL->TEMP_LOCAL, LOAD, STORE triple folds into a
 * direct CALL->*dest. CALL result spilled to StackLoc[-2@0]; a single LOAD
 * reads it into T0; T0's only use is a STORE into P0 (a PARAM, so
 * unconditionally "available at the call" -- avail_at_call's first branch).
 * The LOAD and STORE both NOP out and the CALL's dest becomes *P0 directly. */
UT_TEST(test_gens_call_result_ex_fold_store_into_param_dest)
{
  static Sym callee;
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);

  int call = utb_emit(ir, TCCIR_OP_FUNCCALLVAL, utb_templocal(-2, 0, /*is_lval*/ 1, I32),
                      utb_callee_ref(ir, &callee), utb_imm((int32_t)TCCIR_ENCODE_CALL(1, 0), I32));
  int load = utb_emit(ir, TCCIR_OP_LOAD, utb_temp(0, I32), utb_templocal(-2, 0, /*is_lval*/ 1, I32), UTB_NONE);
  int store = utb_emit(ir, TCCIR_OP_STORE, utb_lval(utb_param(0, I32)), utb_temp(0, I32), UTB_NONE);

  IROptCtx ctx;
  tcc_ir_opt_ctx_init(&ctx, ir);
  int changes = tcc_ir_opt_gens_call_result_ex(&ctx);
  tcc_ir_opt_ctx_free(&ctx);

  UT_ASSERT_EQ(changes, 1);
  UT_ASSERT_EQ(utb_op(ir, call), TCCIR_OP_FUNCCALLVAL); /* still a value-call, just re-targeted */
  UT_ASSERT_EQ(utb_op(ir, load), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, store), TCCIR_OP_NOP);
  IROperand new_dest = utb_dest(ir, call);
  UT_ASSERT_EQ(utb_vreg(new_dest), utb_vreg(utb_param(0, I32)));
  UT_ASSERT(new_dest.is_lval);

  utb_free(ir);
  return 0;
}

/* POSITIVE: store_dst is a VAR defined *before* the call (k < i branch of
 * avail_at_call's scan, distinct from the PARAM-type-is-always-available
 * shortcut exercised above). */
UT_TEST(test_gens_call_result_ex_fold_store_into_var_defined_before_call)
{
  static Sym callee;
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);

  /* V0 defined before the call (any def -- an ASSIGN from a constant). */
  int def = utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(0, I32), utb_imm(0, I32), UTB_NONE);
  int call = utb_emit(ir, TCCIR_OP_FUNCCALLVAL, utb_templocal(-2, 0, /*is_lval*/ 1, I32),
                      utb_callee_ref(ir, &callee), utb_imm((int32_t)TCCIR_ENCODE_CALL(1, 0), I32));
  int load = utb_emit(ir, TCCIR_OP_LOAD, utb_temp(0, I32), utb_templocal(-2, 0, /*is_lval*/ 1, I32), UTB_NONE);
  int store = utb_emit(ir, TCCIR_OP_STORE, utb_lval(utb_var(0, I32)), utb_temp(0, I32), UTB_NONE);

  IROptCtx ctx;
  tcc_ir_opt_ctx_init(&ctx, ir);
  int changes = tcc_ir_opt_gens_call_result_ex(&ctx);
  tcc_ir_opt_ctx_free(&ctx);

  UT_ASSERT_EQ(changes, 1);
  UT_ASSERT_EQ(utb_op(ir, def), TCCIR_OP_ASSIGN); /* untouched */
  UT_ASSERT_EQ(utb_op(ir, load), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, store), TCCIR_OP_NOP);
  IROperand new_dest = utb_dest(ir, call);
  UT_ASSERT_EQ(utb_vreg(new_dest), utb_vreg(utb_var(0, I32)));

  utb_free(ir);
  return 0;
}

/* POSITIVE: the ASSIGN-forwarding branch of avail_at_call -- store_dst is a
 * VAR whose *only* def is an ASSIGN (after the call) copying a non-lval
 * PARAM vreg verbatim. Since PARAMs are always available, the pass rewrites
 * store_dst to the PARAM operand directly (skipping the ASSIGN's VAR
 * entirely) rather than bailing. This is the `p->op == TCCIR_OP_ASSIGN`
 * special case inside the store_dst_vr >= i scan. */
UT_TEST(test_gens_call_result_ex_fold_store_forwards_through_param_assign)
{
  static Sym callee;
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);

  int call = utb_emit(ir, TCCIR_OP_FUNCCALLVAL, utb_templocal(-2, 0, /*is_lval*/ 1, I32),
                      utb_callee_ref(ir, &callee), utb_imm((int32_t)TCCIR_ENCODE_CALL(1, 0), I32));
  /* V0's only def: V0 = P1 (plain, non-lval PARAM copy) -- placed AFTER the
   * call so the `k < i` branch cannot apply; only the ASSIGN-forwarding
   * branch can make this available. */
  int assign = utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(0, I32), utb_param(1, I32), UTB_NONE);
  int load = utb_emit(ir, TCCIR_OP_LOAD, utb_temp(0, I32), utb_templocal(-2, 0, /*is_lval*/ 1, I32), UTB_NONE);
  int store = utb_emit(ir, TCCIR_OP_STORE, utb_lval(utb_var(0, I32)), utb_temp(0, I32), UTB_NONE);

  IROptCtx ctx;
  tcc_ir_opt_ctx_init(&ctx, ir);
  int changes = tcc_ir_opt_gens_call_result_ex(&ctx);
  tcc_ir_opt_ctx_free(&ctx);

  UT_ASSERT_EQ(changes, 1);
  UT_ASSERT_EQ(utb_op(ir, assign), TCCIR_OP_ASSIGN); /* untouched */
  UT_ASSERT_EQ(utb_op(ir, load), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, store), TCCIR_OP_NOP);
  IROperand new_dest = utb_dest(ir, call);
  /* Rewritten straight to P1 (the ASSIGN's source), not V0. */
  UT_ASSERT_EQ(utb_vreg(new_dest), utb_vreg(utb_param(1, I32)));
  UT_ASSERT(new_dest.is_lval);

  utb_free(ir);
  return 0;
}

/* NEGATIVE (guard): multi_use -- the TEMP_LOCAL slot is read by two
 * different LOADs (or otherwise referenced twice), so folding into a single
 * direct store target would be unsound; the pass must decline. */
UT_TEST(test_gens_call_result_ex_fold_store_multi_use_kept)
{
  static Sym callee;
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);

  int call = utb_emit(ir, TCCIR_OP_FUNCCALLVAL, utb_templocal(-2, 0, /*is_lval*/ 1, I32),
                      utb_callee_ref(ir, &callee), utb_imm((int32_t)TCCIR_ENCODE_CALL(1, 0), I32));
  int load1 = utb_emit(ir, TCCIR_OP_LOAD, utb_temp(0, I32), utb_templocal(-2, 0, /*is_lval*/ 1, I32), UTB_NONE);
  int load2 = utb_emit(ir, TCCIR_OP_LOAD, utb_temp(1, I32), utb_templocal(-2, 0, /*is_lval*/ 1, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_STORE, utb_lval(utb_param(0, I32)), utb_temp(0, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_STORE, utb_lval(utb_param(1, I32)), utb_temp(1, I32), UTB_NONE);

  IROptCtx ctx;
  tcc_ir_opt_ctx_init(&ctx, ir);
  int changes = tcc_ir_opt_gens_call_result_ex(&ctx);
  tcc_ir_opt_ctx_free(&ctx);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, call), TCCIR_OP_FUNCCALLVAL);
  UT_ASSERT_EQ(utb_op(ir, load1), TCCIR_OP_LOAD);
  UT_ASSERT_EQ(utb_op(ir, load2), TCCIR_OP_LOAD);

  utb_free(ir);
  return 0;
}

/* NEGATIVE (guard): load_idx stays -1 because the only reference to the
 * TEMP_LOCAL(-2) vreg afterward is at a DIFFERENT byte offset (@4, not the
 * call's own @0) -- fold_call_result_store's scan requires an exact
 * `irop_get_imm64_ex(...) == call_dest_off` match before it counts as a use
 * at all, so it never sees any use and declines via `load_idx < 0`. This is
 * deliberately distinguished from the "no reference anywhere" case (which
 * dead_call_result -- run earlier in the same gens table -- would instead
 * catch and demote to FUNCCALLVOID): here the vreg IS referenced, at a
 * different offset, which is enough to make dead_call_result's own coarser
 * (vreg-only, not vreg+offset) manual scan see a hit and keep the call as
 * FUNCCALLVAL, letting control reach fold_call_result_store's own decline. */
UT_TEST(test_gens_call_result_ex_fold_store_no_load_kept)
{
  static Sym callee;
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);

  int call = utb_emit(ir, TCCIR_OP_FUNCCALLVAL, utb_templocal(-2, 0, /*is_lval*/ 1, I32),
                      utb_callee_ref(ir, &callee), utb_imm((int32_t)TCCIR_ENCODE_CALL(1, 0), I32));
  /* Same TEMP_LOCAL vreg (-2), but offset 4 (not 0): dead_call_result's scan
   * matches on vreg alone and bails; fold_call_result_store's scan requires
   * the offset to also match call_dest_off (0) and does not. */
  int load = utb_emit(ir, TCCIR_OP_LOAD, utb_temp(0, I32), utb_templocal(-2, 4, /*is_lval*/ 1, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_temp(0, I32), UTB_NONE);

  IROptCtx ctx;
  tcc_ir_opt_ctx_init(&ctx, ir);
  int changes = tcc_ir_opt_gens_call_result_ex(&ctx);
  tcc_ir_opt_ctx_free(&ctx);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, call), TCCIR_OP_FUNCCALLVAL);
  UT_ASSERT_EQ(utb_op(ir, load), TCCIR_OP_LOAD);

  utb_free(ir);
  return 0;
}

/* NEGATIVE (guard): load_dst_misuse -- the LOAD's destination temp (T0) is
 * used twice (e.g. added to itself), so it is not a single clean
 * store-through; the generator must decline (uses != 1 || store_idx >= 0
 * once the second use is seen). */
UT_TEST(test_gens_call_result_ex_fold_store_load_dst_multi_use_kept)
{
  static Sym callee;
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);

  int call = utb_emit(ir, TCCIR_OP_FUNCCALLVAL, utb_templocal(-2, 0, /*is_lval*/ 1, I32),
                      utb_callee_ref(ir, &callee), utb_imm((int32_t)TCCIR_ENCODE_CALL(1, 0), I32));
  int load = utb_emit(ir, TCCIR_OP_LOAD, utb_temp(0, I32), utb_templocal(-2, 0, /*is_lval*/ 1, I32), UTB_NONE);
  /* T0 used twice: once in an ADD, once in the STORE below. */
  int add = utb_emit(ir, TCCIR_OP_ADD, utb_temp(1, I32), utb_temp(0, I32), utb_temp(0, I32));
  utb_emit(ir, TCCIR_OP_STORE, utb_lval(utb_param(0, I32)), utb_temp(1, I32), UTB_NONE);

  IROptCtx ctx;
  tcc_ir_opt_ctx_init(&ctx, ir);
  int changes = tcc_ir_opt_gens_call_result_ex(&ctx);
  tcc_ir_opt_ctx_free(&ctx);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, call), TCCIR_OP_FUNCCALLVAL);
  UT_ASSERT_EQ(utb_op(ir, load), TCCIR_OP_LOAD);
  UT_ASSERT_EQ(utb_op(ir, add), TCCIR_OP_ADD);

  utb_free(ir);
  return 0;
}

/* NEGATIVE (guard): the store_dst VAR's only def is after the call and is
 * NOT a PARAM-forwarding ASSIGN (it's a plain constant ASSIGN) -- so
 * avail_at_call never becomes true and the fold must decline, leaving the
 * TEMP_LOCAL round-trip intact. */
UT_TEST(test_gens_call_result_ex_fold_store_dest_not_available_kept)
{
  static Sym callee;
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);

  int call = utb_emit(ir, TCCIR_OP_FUNCCALLVAL, utb_templocal(-2, 0, /*is_lval*/ 1, I32),
                      utb_callee_ref(ir, &callee), utb_imm((int32_t)TCCIR_ENCODE_CALL(1, 0), I32));
  int load = utb_emit(ir, TCCIR_OP_LOAD, utb_temp(0, I32), utb_templocal(-2, 0, /*is_lval*/ 1, I32), UTB_NONE);
  /* V0's only def, after the call, is a constant -- not PARAM-forwarding. */
  int def = utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(0, I32), utb_imm(7, I32), UTB_NONE);
  int store = utb_emit(ir, TCCIR_OP_STORE, utb_lval(utb_var(0, I32)), utb_temp(0, I32), UTB_NONE);

  IROptCtx ctx;
  tcc_ir_opt_ctx_init(&ctx, ir);
  int changes = tcc_ir_opt_gens_call_result_ex(&ctx);
  tcc_ir_opt_ctx_free(&ctx);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, call), TCCIR_OP_FUNCCALLVAL);
  UT_ASSERT_EQ(utb_op(ir, load), TCCIR_OP_LOAD);
  UT_ASSERT_EQ(utb_op(ir, def), TCCIR_OP_ASSIGN);
  UT_ASSERT_EQ(utb_op(ir, store), TCCIR_OP_STORE);

  utb_free(ir);
  return 0;
}
