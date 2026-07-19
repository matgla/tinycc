/*
 *  TCC IR - Pass-group and pipeline driver
 *
 *  Copyright (c) 2025 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#define USING_GLOBALS

#include "ir.h"
#include "opt_pipeline.h"
#include "opt_utils.h"

static void pipeline_trace_pass(const IRPassGroup *group, const IROptPass *pass,
                                int iter, int changes)
{
  if (tcc_state->verbose >= 2 && changes > 0)
    fprintf(stderr, "[OPT %s/%s iter=%d] %d changes\n",
            group->name, pass->name, iter + 1, changes);
}

static void pipeline_trace_group(const IRPassGroup *group, int iterations,
                                 int total_changes)
{
  if (tcc_state->verbose >= 2)
    fprintf(stderr, "[OPT %s] %s after %d iteration%s (%d total changes)\n",
            group->name,
            total_changes ? "stopped" : "converged",
            iterations, iterations == 1 ? "" : "s", total_changes);
}

static void pipeline_ensure_requirements(IROptCtx *ctx, uint32_t requires)
{
  if (requires & IR_PASS_REQUIRES_DU)
    tcc_ir_opt_ctx_require_du(ctx);
  else if (requires & IR_PASS_REQUIRES_DU_TMP_ONLY)
    tcc_ir_opt_ctx_require_du_mode(ctx, IR_DU_MODE_TMP_ONLY);
  if (requires & IR_PASS_REQUIRES_MERGE)
    tcc_ir_opt_ctx_require_merge(ctx);
  if (requires & IR_PASS_REQUIRES_BLOCKS)
    tcc_ir_opt_ctx_require_block_starts(ctx);
  if (requires & IR_PASS_REQUIRES_LOOPS)
    tcc_ir_opt_ctx_require_loops(ctx);
}

/* Analysis rebuilds are charged to a synthetic pass so they do not inflate
 * whichever pass happened to be the first consumer after an invalidation. */
static void pipeline_time_requirements(IROptCtx *ctx, uint32_t requires)
{
  if (tcc_pass_timing_on == 0) {
    pipeline_ensure_requirements(ctx, requires);
    return;
  }
  TCCPassTimer t;
  tcc_pass_timing_begin(&t, "P:requirements");
  pipeline_ensure_requirements(ctx, requires);
  tcc_pass_timing_end(&t, -1);
}

/* Called only after a pass reported changes > 0, which means the IR was
 * mutated -- so the cached analyses are dropped unconditionally, whatever the
 * table entry declared.  Honouring `invalidates` here instead would make the
 * per-pass dirty tracking below depend on every table entry having an accurate
 * mask: an entry that reports changes while declaring 0 would leave
 * ctx->generation stale and let the driver skip passes with real work.  The
 * four INVALIDATES_* bits are runtime-identical today in any case
 * (docs/plans/opt_pass_dedup_and_perf.md, RC2). */
static void pipeline_apply_invalidations(IROptCtx *ctx, uint32_t invalidates)
{
  (void)invalidates;
  tcc_ir_opt_ctx_invalidate(ctx);
}

void dbg_scan_overlap(TCCIRState *ir, const char *pass);
void dbg_scan_overlap(TCCIRState *ir, const char *pass)
{
  if (!getenv("SCAN_OVERLAP"))
    return;
  int n = ir->next_instruction_index;
  for (int a = 0; a < n; a++) {
    IRQuadCompact *qa = &ir->compact_instructions[a];
    if (qa->op == TCCIR_OP_NOP) continue;
    int na = irop_config[qa->op].has_dest + irop_config[qa->op].has_src1 + irop_config[qa->op].has_src2;
    if (na == 0) continue;
    int a0 = qa->operand_base, a1 = qa->operand_base + na - 1;
    for (int b = a + 1; b < n; b++) {
      IRQuadCompact *qb = &ir->compact_instructions[b];
      if (qb->op == TCCIR_OP_NOP) continue;
      int nb = irop_config[qb->op].has_dest + irop_config[qb->op].has_src1 + irop_config[qb->op].has_src2;
      if (nb == 0) continue;
      int b0 = qb->operand_base, b1 = qb->operand_base + nb - 1;
      if (a0 <= b1 && b0 <= a1) {
        fprintf(stderr, "OVERLAP after '%s': insn %d slots[%d..%d] (op %d) <> insn %d slots[%d..%d] (op %d)\n",
                pass ? pass : "?", a, a0, a1, (int)qa->op, b, b0, b1, (int)qb->op);
        return;
      }
    }
  }
}

void dbg_scan_imm_dest(TCCIRState *ir, const char *pass);
void dbg_scan_imm_dest(TCCIRState *ir, const char *pass)
{
  if (!getenv("SCAN_IMM_DEST"))
    return;
  for (int i = 0; i < ir->next_instruction_index; i++) {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_ASSIGN)
      continue;
    IROperand d = tcc_ir_op_get_dest(ir, q);
    if (irop_get_tag(d) == IROP_TAG_IMM32 || irop_get_tag(d) == IROP_TAG_I64 ||
        irop_get_tag(d) == IROP_TAG_F32 || irop_get_tag(d) == IROP_TAG_F64) {
      IROperand sc = tcc_ir_op_get_src1(ir, q);
      fprintf(stderr, "ASSIGN-IMM-DEST after '%s' insn %d: dest{tag=%d vr=0x%x imm=%d} <- src{tag=%d vr=0x%x}\n",
              pass ? pass : "?", i, irop_get_tag(d), (unsigned)d.vr, (int)d.u.imm32, irop_get_tag(sc), (unsigned)sc.vr);
      return;
    }
  }
}

/* Per-pass dirty tracking (docs/plans/opt_pass_dedup_and_perf.md, 1.2).
 *
 * A pass that reported no changes cannot find any on a later round unless
 * something has touched the IR since.  ctx->generation is the token:
 * pipeline_apply_invalidations bumps it on every reported change, the
 * compact_after block bumps it, and the cascade slots bump it for the
 * mutations they deliberately do not report (see CASCADE_END in
 * pipeline_table.c).  So "ran clean at generation G, and generation is still
 * G" is a sound skip condition.
 *
 * Measured productivity is ~2%, so this removes the large majority of pass
 * invocations.  The skip is behaviour-preserving only as long as every mutation
 * reaches the generation counter -- that invariant is what
 * `make test-golden-ir` gates. */
#define PIPELINE_MAX_TRACKED 64

/* Dumping every pass here is what keeps group-only passes visible to the golden-IR harness. */
int tcc_ir_opt_run_group(IROptCtx *ctx, const IRPassGroup *group)
{
  int total_changes = 0;
  int iterations = group->max_iterations > 0 ? group->max_iterations : 1;
  int iter;
  /* generation starts at 1, so 0 reliably means "has not yet run clean". */
  uint32_t last_clean_gen[PIPELINE_MAX_TRACKED];
  int track = group->count <= PIPELINE_MAX_TRACKED;
  if (track)
    memset(last_clean_gen, 0, (size_t)group->count * sizeof(last_clean_gen[0]));
  tcc_pass_timing_init();

  dbg_scan_imm_dest(ctx->ir, "<before-group>");
  dbg_scan_overlap(ctx->ir, "<before-group>");

  for (iter = 0; iter < iterations; iter++) {
    int round_changes = 0;

    if (group->trigger_idx >= 0) {
      const IROptPass *trigger = &group->passes[group->trigger_idx];
      if (trigger->flag_offset && !*((unsigned char *)tcc_state + trigger->flag_offset))
        break;
      if (tcc_ir_opt_pass_disabled(trigger->name))
        break;
      pipeline_time_requirements(ctx, trigger->requires);
      int tch;
      TCC_PASS_TIMED(tch, trigger->name ? trigger->name : "P:trigger",
                     trigger->run(ctx));
      dbg_scan_imm_dest(ctx->ir, trigger->name);
      dbg_scan_overlap(ctx->ir, trigger->name);
      tcc_ir_dump_after_pass(ctx->ir, trigger->name);
      pipeline_trace_pass(group, trigger, iter, tch);
      /* An idle trigger ends the group even if other passes made new work; cascades wrap own fixpoint. */
      if (tch <= 0)
        break;
      round_changes += tch;
      pipeline_apply_invalidations(ctx, trigger->invalidates);
    }

    for (int p = 0; p < group->count; p++) {
      if (p == group->trigger_idx)
        continue;
      const IROptPass *pass = &group->passes[p];
      if (!pass->run)
        continue;
      if (pass->flag_offset && !*((unsigned char *)tcc_state + pass->flag_offset))
        continue;
      if (tcc_ir_opt_pass_disabled(pass->name))
        continue;

      /* Still dump: a skipped pass must leave the golden-IR harness seeing
       * exactly what running it would have, which is what proves the skip is
       * output-neutral rather than merely assuming it. */
      if (track && last_clean_gen[p] == ctx->generation) {
        tcc_ir_dump_after_pass(ctx->ir, pass->name);
        continue;
      }

      pipeline_time_requirements(ctx, pass->requires);

      int changes;
      TCC_PASS_TIMED(changes, pass->name ? pass->name : "P:pass", pass->run(ctx));
      dbg_scan_imm_dest(ctx->ir, pass->name);
      dbg_scan_overlap(ctx->ir, pass->name);
      tcc_ir_dump_after_pass(ctx->ir, pass->name);
      if (changes > 0) {
        round_changes += changes;
        pipeline_apply_invalidations(ctx, pass->invalidates);
      } else if (track) {
        /* No change reported and no invalidation ran, so ctx->generation is
         * still the value this pass just proved clean. */
        last_clean_gen[p] = ctx->generation;
      }
      pipeline_trace_pass(group, pass, iter, changes);
    }

    total_changes += round_changes;

    if (group->compact_after && round_changes > 0) {
      tcc_ir_opt_compact_nops(ctx->ir);
      tcc_ir_opt_ctx_invalidate(ctx);
    }

    if (round_changes == 0)
      break;
  }

  pipeline_trace_group(group, iter, total_changes);
  return total_changes;
}

int tcc_ir_opt_run_pipeline(TCCIRState *ir, const IRPassGroup *groups,
                            int group_count)
{
  IROptCtx ctx;
  tcc_ir_opt_ctx_init(&ctx, ir);

  int total_changes = 0;

  for (int g = 0; g < group_count; g++) {
    total_changes += tcc_ir_opt_run_group(&ctx, &groups[g]);

    if (groups[g].compact_after)
      tcc_ir_opt_ctx_invalidate(&ctx);
  }

  tcc_ir_opt_ctx_free(&ctx);
  return total_changes;
}

int tcc_ir_opt_gen_pass_adapter(IROptCtx *ctx, const IROptGenPassData *data)
{
  return tcc_ir_opt_run_gens(ctx, data->gens, data->count);
}
