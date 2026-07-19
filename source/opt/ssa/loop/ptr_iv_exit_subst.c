/*
 *  TCC IR - SSA loop: pointer-IV exit-value substitution
 *
 *  Copyright (c) 2025 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#include "ir.h"
#include "ssa_opt.h"
#include "opt_loop_utils.h"
#include "opt_utils.h"
#include "loop_cand.h"

typedef struct PtrIV
{
  int32_t vreg;       /* the pointer VAR */
  int32_t init_off;   /* StackLoc offset at preheader */
  int     step;       /* increment per iteration */
  int     init_idx;   /* index of the init ASSIGN */
  int     def_idx;    /* index of the in-loop self-add */
  int     is_llocal;  /* preserve llocal flag from init */
  int     is_param;   /* preserve param flag from init */
  int     btype;
} PtrIV;

#define PTRIV_MAX 8

static int ptr_iv_find_loop_step(TCCIRState *ir, IRLoop *loop, int instr_idx,
                                 int32_t *out_vreg, int *out_step)
{
  IRQuadCompact *q = &ir->compact_instructions[instr_idx];
  if (q->op != TCCIR_OP_ADD)
    return 0;
  IROperand dest = tcc_ir_op_get_dest(ir, q);
  IROperand src1 = tcc_ir_op_get_src1(ir, q);
  IROperand src2 = tcc_ir_op_get_src2(ir, q);
  int32_t d_vr = irop_get_vreg(dest);
  int32_t s1_vr = irop_get_vreg(src1);
  if (d_vr < 0 || TCCIR_DECODE_VREG_TYPE(d_vr) != TCCIR_VREG_TYPE_VAR)
    return 0;
  if (!irop_is_immediate(src2))
    return 0;
  int step = (int)irop_get_imm64_ex(ir, src2);
  if (step == 0)
    return 0;

  if (s1_vr == d_vr) {
    *out_vreg = d_vr;
    *out_step = step;
    return 1;
  }

  /* Copy-through form: `T = V; V = T + #step`. */
  for (int k = instr_idx - 1; k >= loop->start_idx && k >= instr_idx - 3; k--) {
    IRQuadCompact *aq = &ir->compact_instructions[k];
    if (aq->op == TCCIR_OP_NOP)
      continue;
    if (aq->op != TCCIR_OP_ASSIGN)
      return 0;
    IROperand adest = tcc_ir_op_get_dest(ir, aq);
    IROperand asrc = tcc_ir_op_get_src1(ir, aq);
    if (irop_get_vreg(adest) == s1_vr && irop_get_vreg(asrc) == d_vr) {
      *out_vreg = d_vr;
      *out_step = step;
      return 1;
    }
    return 0;
  }
  return 0;
}

static int ptr_iv_find_init(TCCIRState *ir, int vreg, int preheader_idx,
                            int *out_off, int *out_is_llocal, int *out_is_param,
                            int *out_init_idx, int *out_btype)
{
  for (int j = preheader_idx; j >= 0; j--) {
    IRQuadCompact *q = &ir->compact_instructions[j];
    if (q->op == TCCIR_OP_NOP)
      continue;
    if (j < preheader_idx && q->is_jump_target)
      return 0;
    if (!irop_config[q->op].has_dest)
      continue;
    IROperand dest = tcc_ir_op_get_dest(ir, q);
    if (irop_get_vreg(dest) != vreg)
      continue;
    /* STOREs through a vreg-deref do not redefine vreg itself. */
    if (q->op == TCCIR_OP_STORE && dest.is_lval && !dest.is_local)
      continue;
    if (q->op != TCCIR_OP_ASSIGN)
      return 0;
    IROperand src1 = tcc_ir_op_get_src1(ir, q);
    if (irop_get_tag(src1) != IROP_TAG_STACKOFF || src1.is_lval || !src1.is_local)
      return 0;
    *out_off = (int)irop_get_imm64_ex(ir, src1);
    *out_is_llocal = src1.is_llocal;
    *out_is_param = src1.is_param;
    *out_init_idx = j;
    *out_btype = irop_get_btype(src1);
    return 1;
  }
  return 0;
}

static int ptr_iv_unique_loop_def(TCCIRState *ir, IRLoop *loop, int vreg, int def_idx)
{
  for (int j = loop->start_idx; j <= loop->end_idx; j++) {
    IRQuadCompact *q = &ir->compact_instructions[j];
    if (q->op == TCCIR_OP_NOP)
      continue;
    if (!irop_config[q->op].has_dest)
      continue;
    IROperand dest = tcc_ir_op_get_dest(ir, q);
    if (irop_get_vreg(dest) != vreg)
      continue;
    /* A STORE through vreg's deref does not redefine vreg. */
    if (q->op == TCCIR_OP_STORE && dest.is_lval && !dest.is_local)
      continue;
    if (j != def_idx)
      return 0;
  }
  return 1;
}

/* Only the STACKOFF-tagged form is a value-read; a VREG tag means a deref. */
static int ptr_iv_subst_uses_in_instr(TCCIRState *ir, int idx, int vreg, IROperand repl)
{
  IRQuadCompact *q = &ir->compact_instructions[idx];
  int subs = 0;
  if (irop_config[q->op].has_src1) {
    IROperand s = tcc_ir_op_get_src1(ir, q);
    if (irop_get_vreg(s) == vreg && irop_get_tag(s) == IROP_TAG_STACKOFF) {
      tcc_ir_op_set_src1(ir, q, repl);
      subs++;
    }
  }
  if (irop_config[q->op].has_src2) {
    IROperand s = tcc_ir_op_get_src2(ir, q);
    if (irop_get_vreg(s) == vreg && irop_get_tag(s) == IROP_TAG_STACKOFF) {
      tcc_ir_op_set_src2(ir, q, repl);
      subs++;
    }
  }
  return subs;
}

/* Chase reaching defs only within [exit_target, at): defs below exit_target may be loop-carried. */
static int piv_resolve_frame_addr(TCCIRState *ir, IROperand op, int exit_target,
                                  int at, int depth,
                                  int32_t *out_off, int *out_is_param)
{
  if (depth > 8)
    return 0;

  if (irop_get_tag(op) == IROP_TAG_STACKOFF && irop_get_vreg(op) == -1 &&
      !op.is_lval && op.is_local) {
    *out_off = (int32_t)irop_get_imm64_ex(ir, op);
    *out_is_param = op.is_param;
    return 1;
  }

  int32_t vr = irop_get_vreg(op);
  if (vr < 0 || op.is_lval)
    return 0;

  for (int j = at - 1; j >= exit_target; j--) {
    IRQuadCompact *q = &ir->compact_instructions[j];
    if (q->op == TCCIR_OP_NOP || !irop_config[q->op].has_dest)
      continue;
    IROperand dest = tcc_ir_op_get_dest(ir, q);
    if (irop_get_vreg(dest) != vr)
      continue;
    /* A STORE through vr's deref is a use of vr, not a def. */
    if (q->op == TCCIR_OP_STORE && dest.is_lval && !dest.is_local)
      continue;
    if (dest.is_lval)
      return 0; /* memory-form write — not a modeled vreg def */
    if (q->op == TCCIR_OP_LEA) {
      IROperand src = tcc_ir_op_get_src1(ir, q);
      if (irop_get_tag(src) != IROP_TAG_STACKOFF || irop_get_vreg(src) != -1 ||
          src.is_lval || !src.is_local)
        return 0;
      *out_off = (int32_t)irop_get_imm64_ex(ir, src);
      *out_is_param = src.is_param;
      return 1;
    }
    if (q->op == TCCIR_OP_ASSIGN)
      return piv_resolve_frame_addr(ir, tcc_ir_op_get_src1(ir, q), exit_target,
                                    j, depth + 1, out_off, out_is_param);
    if (q->op == TCCIR_OP_ADD || q->op == TCCIR_OP_SUB) {
      IROperand s2 = tcc_ir_op_get_src2(ir, q);
      int32_t base_off;
      int base_is_param;
      if (!irop_is_immediate(s2))
        return 0;
      if (!piv_resolve_frame_addr(ir, tcc_ir_op_get_src1(ir, q), exit_target,
                                  j, depth + 1, &base_off, &base_is_param))
        return 0;
      int64_t c = irop_get_imm64_ex(ir, s2);
      if (q->op == TCCIR_OP_SUB)
        c = -c;
      c += base_off;
      if (c != (int32_t)c)
        return 0;
      *out_off = (int32_t)c;
      *out_is_param = base_is_param;
      return 1;
    }
    return 0;
  }
  return 0;
}

static int piv_fold_substituted_cmp(TCCIRState *ir, int cmp_idx, int exit_target,
                                    int *out_cfg_changed)
{
  IRQuadCompact *q = &ir->compact_instructions[cmp_idx];
  if (cmp_idx + 1 >= ir->next_instruction_index)
    return 0;
  IRQuadCompact *next = &ir->compact_instructions[cmp_idx + 1];
  if (next->op != TCCIR_OP_JUMPIF)
    return 0;

  int32_t off1, off2;
  int p1, p2;
  if (!piv_resolve_frame_addr(ir, tcc_ir_op_get_src1(ir, q), exit_target,
                              cmp_idx, 0, &off1, &p1))
    return 0;
  if (!piv_resolve_frame_addr(ir, tcc_ir_op_get_src2(ir, q), exit_target,
                              cmp_idx, 0, &off2, &p2))
    return 0;
  if (off1 != off2 || p1 != p2)
    return 0; /* proven-unequal is foldable too, but left alone */

  IROperand cond = tcc_ir_op_get_src1(ir, next);
  int tok = (int)irop_get_imm64_ex(ir, cond);
  int result = evaluate_compare_condition(0, 0, tok); /* equal-equal */
  if (result < 0)
    return 0;
  IROperand jmp_dest = tcc_ir_op_get_dest(ir, next);
  LOG_LOOP_OPT("ptr_iv_exit_subst: CMP fold at %d (off=%d, %s)",
               cmp_idx, off1, result ? "taken" : "not taken");
  q->op = TCCIR_OP_NOP;
  if (result) {
    next->op = TCCIR_OP_JUMP;
    tcc_ir_set_dest(ir, cmp_idx + 1, jmp_dest);
  } else {
    next->op = TCCIR_OP_NOP;
  }
  *out_cfg_changed = 1;
  return 1;
}

/* Must confirm the matched IV self-add executes exactly once per iteration. */
typedef int (*ptr_iv_instr_check_fn)(void *ctx, int instr_idx);
static int ptr_iv_exit_subst_loop(TCCIRState *ir, IRLoop *loop,
                                  ptr_iv_instr_check_fn step_ok, void *step_ok_ctx,
                                  int *out_cfg_changes)
{
  if (out_cfg_changes)
    *out_cfg_changes = 0;
  if (loop->start_idx < 0)
    return 0;

  InductionVar ivs[MAX_IV];
  int num_ivs = find_induction_vars_ex(ir, loop, ivs, MAX_IV, 1);
  int cmp_idx, jmpif_idx, limit, cond, exit_target;
  InductionVar *primary = NULL;
  for (int k = 0; k < num_ivs; k++) {
    if (step_ok && !step_ok(step_ok_ctx, ivs[k].def_idx))
      continue;
    if (find_loop_exit_condition(ir, loop, ivs[k].vreg, &cmp_idx, &jmpif_idx,
                                 &limit, &cond, &exit_target)) {
      primary = &ivs[k];
      break;
    }
  }
  if (!primary)
    return 0;
  int trip_count = compute_trip_count(primary->init_val, limit, primary->step, cond);
  if (trip_count <= 0)
    return 0;

  PtrIV pivs[PTRIV_MAX];
  int n_pivs = 0;
  for (int j = loop->start_idx; j <= loop->end_idx && n_pivs < PTRIV_MAX; j++) {
    int32_t v_vr;
    int v_step;
    if (!ptr_iv_find_loop_step(ir, loop, j, &v_vr, &v_step))
      continue;
    int is_counter = 0;
    for (int k = 0; k < num_ivs; k++) {
      if (ivs[k].vreg == v_vr) { is_counter = 1; break; }
    }
    if (is_counter)
      continue;

    if (step_ok && !step_ok(step_ok_ctx, j))
      continue;

    if (!ptr_iv_unique_loop_def(ir, loop, v_vr, j))
      continue;

    int init_off, is_llocal, is_param, init_idx, btype;
    if (!ptr_iv_find_init(ir, v_vr, loop->preheader_idx,
                          &init_off, &is_llocal, &is_param, &init_idx, &btype))
      continue;

    int64_t final_off64 = (int64_t)init_off + (int64_t)v_step * (int64_t)trip_count;
    if (final_off64 != (int32_t)final_off64)
      continue;

    pivs[n_pivs].vreg      = v_vr;
    pivs[n_pivs].init_off  = init_off;
    pivs[n_pivs].step      = v_step;
    pivs[n_pivs].init_idx  = init_idx;
    pivs[n_pivs].def_idx   = j;
    pivs[n_pivs].is_llocal = is_llocal;
    pivs[n_pivs].is_param  = is_param;
    pivs[n_pivs].btype     = btype;
    n_pivs++;
  }

  if (n_pivs == 0)
    return 0;

  /* Trip count > 0 proves a rotated loop's zero-trip guard never fires. */
  for (int g = primary->init_idx + 1; g < loop->start_idx; g++) {
    IRQuadCompact *gq = &ir->compact_instructions[g];
    if (gq->op != TCCIR_OP_CMP)
      continue;
    IROperand gs1 = tcc_ir_op_get_src1(ir, gq);
    if (irop_get_vreg(gs1) != primary->vreg)
      continue;
    if (g + 1 >= loop->start_idx)
      break;
    IRQuadCompact *gjq = &ir->compact_instructions[g + 1];
    if (gjq->op != TCCIR_OP_JUMPIF)
      continue;
    IROperand gjd = tcc_ir_op_get_dest(ir, gjq);
    int gjt = (int)irop_get_imm64_ex(ir, gjd);
    if (gjt < loop->end_idx)
      continue; /* entry guard must target past the loop */
    gq->op = TCCIR_OP_NOP;
    gjq->op = TCCIR_OP_NOP;
    if (out_cfg_changes)
      (*out_cfg_changes)++;
    if (gjt >= 0 && gjt < ir->next_instruction_index) {
      int has_other_in_edge = 0;
      for (int s = 0; s < ir->next_instruction_index && !has_other_in_edge; s++) {
        if (s == g + 1) continue;
        IRQuadCompact *sq = &ir->compact_instructions[s];
        if (sq->op != TCCIR_OP_JUMP && sq->op != TCCIR_OP_JUMPIF)
          continue;
        IROperand sd = tcc_ir_op_get_dest(ir, sq);
        int st = (int)irop_get_imm64_ex(ir, sd);
        if (st == gjt)
          has_other_in_edge = 1;
      }
      if (!has_other_in_edge)
        ir->compact_instructions[gjt].is_jump_target = 0;
    }
  }

  /* A merge or backward jump after exit_target may carry a different V: retire all. */
  int live[PTRIV_MAX];
  for (int p = 0; p < n_pivs; p++) live[p] = 1;

  int total = 0;
  int n = ir->next_instruction_index;
  for (int j = exit_target; j < n; j++) {
    IRQuadCompact *q = &ir->compact_instructions[j];
    if (q->op == TCCIR_OP_NOP)
      continue;

    if (j > exit_target && q->is_jump_target) {
      for (int p = 0; p < n_pivs; p++) live[p] = 0;
    }

    int any_live = 0;
    for (int p = 0; p < n_pivs; p++) if (live[p]) { any_live = 1; break; }
    if (!any_live)
      break;

    /* Reads must be substituted before the redef check below. */
    int subs_here = 0;
    for (int p = 0; p < n_pivs; p++) {
      if (!live[p]) continue;
      int32_t final_off = (int32_t)((int64_t)pivs[p].init_off +
                                    (int64_t)pivs[p].step * (int64_t)trip_count);
      IROperand repl = irop_make_stackoff(-1, final_off, /*is_lval*/ 0,
                                          pivs[p].is_llocal, pivs[p].is_param,
                                          pivs[p].btype);
      subs_here += ptr_iv_subst_uses_in_instr(ir, j, pivs[p].vreg, repl);
    }
    total += subs_here;

    if (subs_here > 0 && q->op == TCCIR_OP_CMP) {
      int folded_cfg = 0;
      piv_fold_substituted_cmp(ir, j, exit_target, &folded_cfg);
      if (folded_cfg && out_cfg_changes)
        (*out_cfg_changes)++;
    }

    /* A STORE through a vreg-deref does not redefine it. */
    if (irop_config[q->op].has_dest) {
      IROperand dest = tcc_ir_op_get_dest(ir, q);
      if (!(q->op == TCCIR_OP_STORE && dest.is_lval && !dest.is_local)) {
        int32_t dvr = irop_get_vreg(dest);
        if (dvr >= 0) {
          for (int p = 0; p < n_pivs; p++) {
            if (live[p] && pivs[p].vreg == dvr)
              live[p] = 0;
          }
        }
      }
    }

    if (q->op == TCCIR_OP_JUMP) {
      IROperand jd = tcc_ir_op_get_dest(ir, q);
      int t = (int)irop_get_imm64_ex(ir, jd);
      if (t <= j) {
        for (int p = 0; p < n_pivs; p++) live[p] = 0;
      }
    }
  }

  return total;
}

/* The sweep stops at any CFG-invalidating change and rebuilds; this caps the retries. */
#define SSA_PTR_IV_EXIT_SUBST_MAX_PASSES 4

typedef struct {
  IRCFG *cfg;
  const uint8_t *member;
  int latch_b;
} PivStepCtx;

static int piv_step_dominates_latch(void *vctx, int instr_idx)
{
  PivStepCtx *ctx = vctx;
  if (instr_idx < 0 || instr_idx >= ctx->cfg->num_instrs)
    return 0;
  int b = ctx->cfg->instr_to_block[instr_idx];
  if (b < 0 || b >= ctx->cfg->num_blocks || !ctx->member[b])
    return 0;
  return tcc_ir_cfg_dominates(ctx->cfg, b, ctx->latch_b);
}

/* `member` must be num_blocks bytes of caller-owned scratch. */
static int piv_try_candidate(TCCIRState *ir, IRCFG *cfg, int header_b,
                             int latch_b, uint8_t *member, int *out_cfg_changes)
{
  IRBasicBlock *hb = &cfg->blocks[header_b];

  *out_cfg_changes = 0;
  fie_collect_members(cfg, header_b, latch_b, member);

  /* Single-entry: header preds must be exactly {out-of-loop pred, latch}. */
  if (hb->num_preds != 2)
    return 0;
  int latch_seen = 0, entry_pred = -1;
  for (int i = 0; i < hb->num_preds; i++) {
    int p = hb->preds[i];
    if (p == latch_b && !latch_seen)
      latch_seen = 1;
    else
      entry_pred = p;
  }
  if (!latch_seen || entry_pred < 0 || member[entry_pred])
    return 0;

  for (int b = 0; b < cfg->num_blocks; b++) {
    if (!member[b] || b == header_b)
      continue;
    IRBasicBlock *bb = &cfg->blocks[b];
    for (int i = 0; i < bb->num_preds; i++)
      if (!member[bb->preds[i]])
        return 0;
  }

  /* The preheader must lie in the entry pred so inits come from the entering path. */
  int preheader = hb->start_idx - 1;
  while (preheader >= 0) {
    IRQuadCompact *ph = &ir->compact_instructions[preheader];
    if (ph->op != TCCIR_OP_JUMP && ph->op != TCCIR_OP_JUMPIF)
      break;
    preheader--;
  }
  if (preheader < 0)
    return 0;
  if (cfg->instr_to_block[preheader] != entry_pred)
    return 0;

  /* end_idx spans all members: bodies may be laid out beyond the latch in flat order. */
  IRLoop loop;
  memset(&loop, 0, sizeof(loop));
  loop.header_idx = hb->start_idx;
  loop.start_idx = hb->start_idx;
  loop.end_idx = cfg->blocks[latch_b].end_idx - 1;
  for (int b = 0; b < cfg->num_blocks; b++) {
    if (member[b] && cfg->blocks[b].end_idx - 1 > loop.end_idx)
      loop.end_idx = cfg->blocks[b].end_idx - 1;
  }
  loop.preheader_idx = preheader;

  PivStepCtx sctx = {cfg, member, latch_b};
  return ptr_iv_exit_subst_loop(ir, &loop, piv_step_dominates_latch, &sctx,
                                out_cfg_changes);
}

int ssa_opt_ptr_iv_exit_subst(TCCIRState *ir)
{
  if (!ir || ir->next_instruction_index == 0)
    return 0;
  if (!tcc_ir_cfg_flat_has_backedge(ir))
    return 0;

  int total = 0;
  for (int pass = 0; pass < SSA_PTR_IV_EXIT_SUBST_MAX_PASSES; pass++) {
    IRCFG *cfg = tcc_ir_cfg_build(ir);
    if (!cfg || cfg->num_blocks <= 1) {
      tcc_ir_cfg_free(cfg);
      break;
    }
    tcc_ir_cfg_compute_dominators(cfg);

    int cap = cfg->num_blocks;
    FieCand *cands = tcc_mallocz(sizeof(FieCand) * (size_t)cap);
    int nc = 0;
    for (int b = 0; b < cfg->num_blocks && nc < cap; b++) {
      IRBasicBlock *bb = &cfg->blocks[b];
      for (int si = 0; si < bb->num_succs && nc < cap; si++) {
        int h = bb->succs[si];
        if (h < 0 || h >= cfg->num_blocks)
          continue;
        if (!tcc_ir_cfg_dominates(cfg, h, b))
          continue;
        cands[nc].header_b = h;
        cands[nc].latch_b = b;
        cands[nc].size = cfg->blocks[b].end_idx - cfg->blocks[h].start_idx;
        nc++;
      }
    }

    int cfg_changed = 0;
    if (nc > 0) {
      qsort(cands, nc, sizeof(FieCand), fie_cand_cmp);
      uint8_t *member = tcc_malloc((size_t)cfg->num_blocks);
      for (int i = 0; i < nc && !cfg_changed; i++) {
        int cfg_changes = 0;
        int subs = piv_try_candidate(ir, cfg, cands[i].header_b,
                                     cands[i].latch_b, member, &cfg_changes);
        if (subs > 0 || cfg_changes > 0)
          LOG_LOOP_OPT("ssa:ptr_iv_exit_subst: header_b=%d latch_b=%d "
                       "subs=%d cfg_changes=%d",
                       cands[i].header_b, cands[i].latch_b, subs, cfg_changes);
        total += subs + cfg_changes;
        if (cfg_changes > 0)
          cfg_changed = 1;
      }
      tcc_free(member);
    }
    tcc_free(cands);
    tcc_ir_cfg_free(cfg);

    if (!cfg_changed)
      break;
  }
  return total;
}
