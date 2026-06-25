/*
 *  TCC IR - SSA Dead Loop Elimination
 *
 *  Copyright (c) 2025 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

/* Replace post-loop uses of header phis with their loop-body constant value
 * when the loop is pure and provably runs at least once.
 *
 * Pattern (after SCCP folding):
 *   preheader:    T_o = #c0           (initial value)
 *                 T_iv = #i0           (counter init)
 *   header B_h:   T_o_phi = phi(T_o from preheader, T_v from latch)
 *                 T_iv_phi = phi(T_iv from preheader, T_iv2 from latch)
 *                 cmp T_iv_phi, #BOUND
 *                 jumpif ">=" exit
 *   body:         T_v = #c1            (constant; loop-invariant)
 *                 T_iv2 = T_iv_phi + 1 (counter step)
 *                 jmp header
 *   exit:         use T_o_phi          (== c1 because loop ran ≥1 time)
 *
 * If the body has no observable side effects and trip count is provably >0,
 * post-loop reads of T_o_phi can be replaced by T_v.  Subsequent passes
 * (cprop_imm, branch fold, dce) then collapse the post-loop comparison
 * branches and finally the loop body itself.
 *
 * Guarded variant: when trip count is not provably >=1 (e.g. the bound is
 * a runtime parameter), the post-loop value of T_o_phi is c0 if the loop
 * was skipped and c1 if it ran.  We materialize this as a SELECT consuming
 * the entry condition's flags, then kill the body the same way.  The
 * resulting code matches GCC's `cmp; ite; mov...; mov...; bx lr` shape.
 */

#define USING_GLOBALS
#include "ir.h"
#include "ssa_opt.h"
#include "licm.h"

/* Resolve a vreg through ASSIGN copies and verify the root is a constant.
 * Returns 1 with *out_const set if found, 0 otherwise. */
static int resolve_const_through_copies(IRSSAOptCtx *ctx, int32_t vr, int64_t *out_const)
{
  TCCIRState *ir = ctx->ir;
  for (int hop = 0; hop < 8 && vr >= 0; hop++) {
    if (TCCIR_DECODE_VREG_TYPE(vr) != TCCIR_VREG_TYPE_TEMP)
      return 0;
    IRSSAVregInfo *vi = ssa_opt_vinfo(ctx, vr);
    if (!vi || vi->def_instr < 0 || vi->def_count > 1)
      return 0;
    IRQuadCompact *q = &ir->compact_instructions[vi->def_instr];
    if (q->op != TCCIR_OP_ASSIGN)
      return 0;
    IROperand src = tcc_ir_op_get_src1(ir, q);
    if (src.is_lval)
      return 0;
    if (irop_is_immediate(src)) {
      *out_const = irop_get_imm64_ex(ir, src);
      return 1;
    }
    if (src.tag != IROP_TAG_VREG)
      return 0;
    vr = irop_get_vreg(src);
  }
  return 0;
}

/* tcc_ir_detect_loops fills body_instrs[] with every instruction in the loop,
 * but only sets end_idx to the back-edge index — when forward jumps push the
 * body range past end_idx (e.g., the `if (cond) break;` shape, or inlined
 * helper bodies), the trailing instructions live in body_instrs only.  Use
 * the maximum of end_idx and the largest body index to bound the loop. */
static int loop_max_idx(IRLoop *loop)
{
  int m = loop->end_idx;
  for (int k = 0; k < loop->num_body_instrs; k++) {
    if (loop->body_instrs[k] > m)
      m = loop->body_instrs[k];
  }
  return m;
}

static int loop_body_has_side_effects(IRSSAOptCtx *ctx, IRLoop *loop)
{
  TCCIRState *ir = ctx->ir;
  int hi = loop_max_idx(loop);
  for (int idx = loop->start_idx; idx <= hi && idx < ir->next_instruction_index; idx++) {
    IRQuadCompact *q = &ir->compact_instructions[idx];
    if (q->op == TCCIR_OP_NOP)
      continue;
    /* Only the loop control flow is allowed to be a "side effect". */
    if (q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_JUMPIF)
      continue;
    if (q->op == TCCIR_OP_CMP || q->op == TCCIR_OP_TEST_ZERO)
      continue;
    if (ssa_opt_has_side_effects(q->op))
      return 1;
  }
  return 0;
}

/* Inverse of a JUMPIF cond token, i.e. the token that's true exactly when the
 * original is false.  Mirrors invert_cond_token in opt.c (kept local to
 * avoid pulling in that header). */
static int dl_invert_cond_token(int tok)
{
  switch (tok) {
  case 0x94: return 0x95;
  case 0x95: return 0x94;
  case 0x9c: return 0x9d;
  case 0x9d: return 0x9c;
  case 0x9e: return 0x9f;
  case 0x9f: return 0x9e;
  case 0x92: return 0x93;
  case 0x93: return 0x92;
  case 0x96: return 0x97;
  case 0x97: return 0x96;
  default:   return tok ^ 1;
  }
}

/* Components extracted from the header CMP+JUMPIF for the dead-loop transform.
 * Populated by analyze_loop_entry; consumed by the constant-rewrite path
 * (when proven_runs is set) and the guarded SELECT path (when not). */
typedef struct LoopEntryInfo {
  int cmp_idx;
  int jpf_idx;
  int header_block;
  int latch_block;
  int going_up;        /* iv steps up toward bound */
  int going_down;      /* iv steps down toward bound */
  int exit_tok;        /* JUMPIF cond: true when loop NOT entered */
  int entry_tok;       /* inverse: cond true when loop ENTERED — for SELECT */
  int init_is_const;
  int64_t init_val;
  int bound_is_const;
  int64_t bound_val;
  IROperand bound_op;
  int32_t iv_vr;
  IRPhiNode *iv_phi;
  int proven_runs;     /* 1 iff trip count is provably >= 1 with constant bound */
} LoopEntryInfo;

/* Generalized form of loop_runs_at_least_once: extracts the entry-condition
 * components without requiring the bound to be a compile-time constant.
 * Returns 1 if the pattern matched and `out` is filled, 0 if we should bail. */
static int analyze_loop_entry(IRSSAOptCtx *ctx, IRLoop *loop, LoopEntryInfo *out)
{
  TCCIRState *ir = ctx->ir;
  int hi = loop_max_idx(loop);
  memset(out, 0, sizeof(*out));

  /* Walk the header forward to find the controlling CMP. */
  out->cmp_idx = -1;
  for (int j = loop->header_idx; j <= hi; j++) {
    int op = ir->compact_instructions[j].op;
    if (op == TCCIR_OP_NOP)
      continue;
    if (op == TCCIR_OP_CMP) {
      out->cmp_idx = j;
      break;
    }
    return 0;
  }
  if (out->cmp_idx < 0)
    return 0;

  IRQuadCompact *cmp = &ir->compact_instructions[out->cmp_idx];
  IROperand src1 = tcc_ir_op_get_src1(ir, cmp);
  IROperand src2 = tcc_ir_op_get_src2(ir, cmp);

  /* CMP iv, <bound> — bound may be immediate or a vreg (loop-invariant). */
  out->bound_op = src2;
  if (irop_is_immediate(src2)) {
    out->bound_is_const = 1;
    out->bound_val = irop_get_imm64_ex(ir, src2);
  } else if (src2.tag == IROP_TAG_VREG && !src2.is_lval) {
    out->bound_is_const = 0;
  } else {
    return 0;
  }

  out->iv_vr = irop_get_vreg(src1);
  if (out->iv_vr < 0 || TCCIR_DECODE_VREG_TYPE(out->iv_vr) != TCCIR_VREG_TYPE_TEMP)
    return 0;

  /* Locate the JUMPIF immediately after (skipping NOPs). */
  int j = out->cmp_idx + 1;
  while (j < ir->next_instruction_index && ir->compact_instructions[j].op == TCCIR_OP_NOP)
    j++;
  if (j >= ir->next_instruction_index)
    return 0;
  IRQuadCompact *jpf = &ir->compact_instructions[j];
  if (jpf->op != TCCIR_OP_JUMPIF)
    return 0;
  out->jpf_idx = j;
  out->exit_tok = (int)irop_get_imm64_ex(ir, tcc_ir_op_get_src1(ir, jpf));
  out->entry_tok = dl_invert_cond_token(out->exit_tok);

  out->header_block = ctx->cfg ? ctx->cfg->instr_to_block[loop->header_idx] : -1;
  if (out->header_block < 0)
    return 0;
  out->latch_block = ctx->cfg->instr_to_block[loop->end_idx];

  /* IV phi — needs preheader operand resolvable to a constant for either
   * path (constant rewrite uses init_val for bounds check; SELECT path uses
   * it as the "loop skipped" value). */
  IRPhiNode *iv_phi = NULL;
  for (IRPhiNode *p = ctx->ssa->block_phis[out->header_block]; p; p = p->next) {
    if (p->dest_vreg == out->iv_vr) {
      iv_phi = p;
      break;
    }
  }
  if (!iv_phi || iv_phi->num_operands != 2)
    return 0;
  out->iv_phi = iv_phi;

  for (int oi = 0; oi < iv_phi->num_operands; oi++) {
    if (iv_phi->operands[oi].pred_block == out->latch_block)
      continue;
    int64_t v;
    if (resolve_const_through_copies(ctx, iv_phi->operands[oi].vreg, &v)) {
      out->init_val = v;
      out->init_is_const = 1;
    }
    break;
  }
  if (!out->init_is_const)
    return 0;

  /* Latch operand must be ADD/SUB of the iv with a positive step (going_up
   * or going_down toward bound).  Same logic as loop_runs_at_least_once. */
  for (int oi = 0; oi < iv_phi->num_operands; oi++) {
    if (iv_phi->operands[oi].pred_block != out->latch_block)
      continue;
    int32_t lvr = iv_phi->operands[oi].vreg;
    if (lvr < 0)
      return 0;
    for (int hop = 0; hop < 8 && lvr >= 0; hop++) {
      IRSSAVregInfo *lvi = ssa_opt_vinfo(ctx, lvr);
      if (!lvi || lvi->def_instr < 0 || lvi->def_count > 1)
        return 0;
      IRQuadCompact *dq = &ir->compact_instructions[lvi->def_instr];
      if (dq->op == TCCIR_OP_ASSIGN) {
        IROperand s = tcc_ir_op_get_src1(ir, dq);
        if (s.tag != IROP_TAG_VREG || s.is_lval)
          return 0;
        lvr = irop_get_vreg(s);
        continue;
      }
      if (dq->op == TCCIR_OP_ADD || dq->op == TCCIR_OP_SUB) {
        IROperand a = tcc_ir_op_get_src1(ir, dq);
        IROperand b = tcc_ir_op_get_src2(ir, dq);
        if (irop_get_vreg(a) != out->iv_vr || !irop_is_immediate(b))
          return 0;
        int64_t step = irop_get_imm64_ex(ir, b);
        if (step <= 0)
          return 0;
        if (dq->op == TCCIR_OP_ADD)
          out->going_up = 1;
        else
          out->going_down = 1;
        break;
      }
      return 0;
    }
    break;
  }
  if (!out->going_up && !out->going_down)
    return 0;

  /* Decide if trip count is provably >= 1 — only possible when bound is also
   * a compile-time constant.  Same arithmetic as the original predicate. */
  if (out->bound_is_const) {
    int tok = out->exit_tok;
    int64_t init_val = out->init_val, bound = out->bound_val;
    if (out->going_up) {
      if (tok == 0x9d || tok == 0x93) out->proven_runs = (init_val < bound);
      else if (tok == 0x9f || tok == 0x97) out->proven_runs = (init_val <= bound);
    }
    if (out->going_down) {
      if (tok == 0x9c || tok == 0x92) out->proven_runs = (init_val > bound);
      else if (tok == 0x9e || tok == 0x96) out->proven_runs = (init_val >= bound);
    }
  }

  return 1;
}

/* For each header phi whose latch operand resolves to a constant AND whose
 * destination is not used inside the loop body, replace all uses (which are
 * therefore post-loop) with that constant. */
static int rewrite_loop_exit_phis(IRSSAOptCtx *ctx, IRLoop *loop)
{
  TCCIRState *ir = ctx->ir;
  IRSSAState *ssa = ctx->ssa;
  IRCFG *cfg = ctx->cfg;
  if (!ssa || !ssa->block_phis || !cfg)
    return 0;

  int header_block = cfg->instr_to_block[loop->header_idx];
  if (header_block < 0)
    return 0;
  int latch_block = cfg->instr_to_block[loop->end_idx];
  int hi = loop_max_idx(loop);

  int changes = 0;

  for (IRPhiNode *phi = ssa->block_phis[header_block]; phi; phi = phi->next) {
    if (phi->num_operands != 2)
      continue;

    /* Find the latch operand and resolve it to a constant. */
    int latch_slot = -1;
    for (int oi = 0; oi < phi->num_operands; oi++) {
      if (phi->operands[oi].pred_block == latch_block) {
        latch_slot = oi;
        break;
      }
    }
    if (latch_slot < 0)
      continue;

    int64_t latch_const;
    if (!resolve_const_through_copies(ctx, phi->operands[latch_slot].vreg, &latch_const))
      continue;

    /* Skip if the phi is used inside the loop body (we'd be changing its value
     * during the first iteration where it should still be the preheader value). */
    IRSSAVregInfo *vi = ssa_opt_vinfo(ctx, phi->dest_vreg);
    if (!vi)
      continue;

    int has_in_loop_use = 0;
    for (int u = 0; u < vi->use_count; u++) {
      IRSSAUse *use = &vi->uses[u];
      if (use->kind == SSA_USE_INSTR) {
        if (use->idx >= loop->start_idx && use->idx <= hi) {
          has_in_loop_use = 1;
          break;
        }
      } else { /* SSA_USE_PHI */
        /* A phi use of phi.dest is still "at" the use's block. If that block
         * is the header itself, treat as in-loop. */
        if (use->idx == header_block) {
          has_in_loop_use = 1;
          break;
        }
      }
    }
    if (has_in_loop_use)
      continue;

    /* Build an immediate operand of the right btype and rewrite all instr
     * uses to use the constant directly. */
    IROperand imm_op = irop_make_imm32(0, (int32_t)latch_const, phi->btype);
    if (phi->btype == IROP_BTYPE_INT64) {
      /* irop_make_imm32 only stores a 32-bit immediate; for int64 we'd need
       * a different path.  Skip int64 for safety. */
      continue;
    }

    /* Rewrite all instruction uses (not phi uses, those keep the vreg). */
    int rewrote = 0;
    int u = 0;
    while (u < vi->use_count) {
      IRSSAUse *use = &vi->uses[u];
      if (use->kind != SSA_USE_INSTR) {
        u++;
        continue;
      }
      int uidx = use->idx;
      IRQuadCompact *uq = &ir->compact_instructions[uidx];
      int rewritten_here = 0;
      if (irop_config[uq->op].has_src1) {
        IROperand s = tcc_ir_op_get_src1(ir, uq);
        if (irop_get_vreg(s) == phi->dest_vreg && !s.is_lval) {
          tcc_ir_op_set_src1(ir, uq, imm_op);
          rewritten_here = 1;
        }
      }
      if (irop_config[uq->op].has_src2) {
        IROperand s = tcc_ir_op_get_src2(ir, uq);
        if (irop_get_vreg(s) == phi->dest_vreg && !s.is_lval) {
          tcc_ir_op_set_src2(ir, uq, imm_op);
          rewritten_here = 1;
        }
      }
      if (rewritten_here) {
        /* Remove this use from vi; replace current with last. */
        vi->uses[u] = vi->uses[--vi->use_count];
        rewrote++;
      } else {
        u++;
      }
    }
    if (rewrote > 0)
      changes++;
  }

  return changes;
}

/* After exit-value rewriting, attempt to short-circuit the entire loop:
 * convert the header JUMPIF into an unconditional JUMP to the exit and NOP
 * the body.  Safe only when no live value escapes the loop except through
 * already-rewritten header phis, the loop is pure, and trip count ≥ 1.
 *
 * Returns 1 if the body was eliminated, 0 otherwise. */
static int try_kill_loop_body(IRSSAOptCtx *ctx, IRLoop *loop)
{
  TCCIRState *ir = ctx->ir;
  IRSSAState *ssa = ctx->ssa;
  IRCFG *cfg = ctx->cfg;

  int hi = loop_max_idx(loop);

  /* Re-locate the header CMP+JUMPIF; the IR may have been modified above. */
  int cmp_idx = -1;
  for (int j = loop->header_idx; j <= hi; j++) {
    int op = ir->compact_instructions[j].op;
    if (op == TCCIR_OP_NOP)
      continue;
    if (op == TCCIR_OP_CMP) {
      cmp_idx = j;
      break;
    }
    return 0;
  }
  if (cmp_idx < 0)
    return 0;

  int jpf_idx = cmp_idx + 1;
  while (jpf_idx <= hi && ir->compact_instructions[jpf_idx].op == TCCIR_OP_NOP)
    jpf_idx++;
  if (jpf_idx > hi)
    return 0;
  IRQuadCompact *jpf = &ir->compact_instructions[jpf_idx];
  if (jpf->op != TCCIR_OP_JUMPIF)
    return 0;

  IROperand exit_dest = tcc_ir_op_get_dest(ir, jpf);
  int exit_target = (int)irop_get_imm64_ex(ir, exit_dest);

  int header_block = cfg->instr_to_block[loop->header_idx];
  int latch_block = cfg->instr_to_block[loop->end_idx];
  if (header_block < 0)
    return 0;

  /* Bail if any TEMP defined inside the body has a use outside the loop range,
   * or in a phi at a block other than the header. */
  for (int idx = jpf_idx + 1; idx <= hi; idx++) {
    IRQuadCompact *q = &ir->compact_instructions[idx];
    if (q->op == TCCIR_OP_NOP)
      continue;
    if (q->op == TCCIR_OP_JUMP)
      continue;
    if (!irop_config[q->op].has_dest)
      continue;
    if (q->op == TCCIR_OP_STORE || q->op == TCCIR_OP_STORE_INDEXED ||
        q->op == TCCIR_OP_STORE_POSTINC)
      return 0; /* shouldn't happen — purity check ran already */

    IROperand d = tcc_ir_op_get_dest(ir, q);
    int32_t vr = irop_get_vreg(d);
    if (vr < 0)
      continue;
    /* Non-TEMP defs (VAR/PARAM) inside a loop body imply observable state. */
    if (TCCIR_DECODE_VREG_TYPE(vr) != TCCIR_VREG_TYPE_TEMP)
      return 0;

    IRSSAVregInfo *vi = ssa_opt_vinfo(ctx, vr);
    if (!vi)
      continue;
    for (int u = 0; u < vi->use_count; u++) {
      IRSSAUse *use = &vi->uses[u];
      if (use->kind == SSA_USE_INSTR) {
        if (use->idx < loop->start_idx || use->idx > hi)
          return 0;
      } else { /* SSA_USE_PHI */
        if (use->idx != header_block)
          return 0;
      }
    }
  }

  /* Header phis must have no INSTR uses outside the loop and no phi uses
   * outside the header.  Exit-value rewriting should have already removed
   * external INSTR uses for any phi we want to kill. */
  for (IRPhiNode *phi = ssa->block_phis[header_block]; phi; phi = phi->next) {
    IRSSAVregInfo *vi = ssa_opt_vinfo(ctx, phi->dest_vreg);
    if (!vi)
      continue;
    for (int u = 0; u < vi->use_count; u++) {
      IRSSAUse *use = &vi->uses[u];
      if (use->kind == SSA_USE_INSTR) {
        if (use->idx < loop->start_idx || use->idx > hi)
          return 0;
      } else {
        if (use->idx != header_block)
          return 0;
      }
    }
  }

  /* Drop phi operands on the dying edges before NOPing instructions —
   * ssa_drop_phi_edge walks the vinfo use lists and we don't want to be
   * mutating those mid-NOP. */
  int body_first_block = -1;
  if (jpf_idx + 1 < ir->next_instruction_index)
    body_first_block = cfg->instr_to_block[jpf_idx + 1];
  if (body_first_block >= 0 && body_first_block != header_block &&
      body_first_block != exit_target /* paranoia */)
    ssa_drop_phi_edge(ctx, header_block, body_first_block);
  if (latch_block >= 0)
    ssa_drop_phi_edge(ctx, latch_block, header_block);

  /* Convert JUMPIF to unconditional JUMP and NOP the CMP. */
  ssa_opt_nop_instr(ctx, cmp_idx);
  jpf->op = TCCIR_OP_JUMP;
  tcc_ir_set_src1(ir, jpf_idx, IROP_NONE);
  tcc_ir_set_src2(ir, jpf_idx, IROP_NONE);
  tcc_ir_set_dest(ir, jpf_idx, exit_dest);

  /* NOP every body instruction (including the back-edge JUMP).  The body
   * extends up to `hi`, not just `loop->end_idx`, when forward jumps reach
   * past the back-edge index (e.g., inlined helper bodies). */
  for (int idx = jpf_idx + 1; idx <= hi; idx++) {
    if (ir->compact_instructions[idx].op != TCCIR_OP_NOP)
      ssa_opt_nop_instr(ctx, idx);
  }

  return 1;
}

/* Guarded variant: when trip count isn't provably >=1, materialize each
 * qualifying header phi as a SELECT consuming the header CMP's flags.
 *
 * The transform overwrites the header JUMPIF (and `num_cands - 1` body slots
 * just after it) with SELECTs, then writes a JUMP-to-exit in the next slot,
 * and finally NOPs the rest of the body — yielding `cmp; select...; b exit`
 * which the backend lowers to `cmp; ite ...; movXX; movYY; b ...`.
 *
 * Returns 1 if the loop was successfully rewritten, 0 otherwise. */
static int rewrite_loop_exit_phis_guarded(IRSSAOptCtx *ctx, IRLoop *loop, LoopEntryInfo *info)
{
  TCCIRState *ir = ctx->ir;
  IRSSAState *ssa = ctx->ssa;
  IRCFG *cfg = ctx->cfg;
  if (!ssa || !ssa->block_phis || !cfg)
    return 0;

  int hi = loop_max_idx(loop);

  /* Collect qualifying value phis. */
  enum { MAX_CANDS = 4 };
  struct {
    IRPhiNode *phi;
    int64_t c_pre;
    int64_t c_latch;
    int btype;
    int32_t new_vr;
  } cands[MAX_CANDS];
  int num_cands = 0;

  for (IRPhiNode *phi = ssa->block_phis[info->header_block]; phi; phi = phi->next) {
    if (phi == info->iv_phi) continue;       /* IV phi handled by edge-drop */
    if (phi->num_operands != 2) continue;
    /* irop_make_imm32 only stores 32-bit immediates; skip wider phis. */
    if (phi->btype == IROP_BTYPE_INT64) continue;

    int pre_slot = -1, latch_slot = -1;
    for (int oi = 0; oi < phi->num_operands; oi++) {
      if (phi->operands[oi].pred_block == info->latch_block) latch_slot = oi;
      else                                                    pre_slot   = oi;
    }
    if (pre_slot < 0 || latch_slot < 0) continue;

    int64_t c_pre, c_latch;
    if (!resolve_const_through_copies(ctx, phi->operands[pre_slot].vreg, &c_pre))
      continue;
    if (!resolve_const_through_copies(ctx, phi->operands[latch_slot].vreg, &c_latch))
      continue;

    /* Phi must have no in-loop INSTR uses and no phi-use outside the header
     * (matches the constraints try_kill_loop_body checks before NOPing). */
    IRSSAVregInfo *vi = ssa_opt_vinfo(ctx, phi->dest_vreg);
    if (!vi) continue;
    int reject = 0;
    for (int u = 0; u < vi->use_count; u++) {
      IRSSAUse *use = &vi->uses[u];
      if (use->kind == SSA_USE_INSTR) {
        if (use->idx >= loop->start_idx && use->idx <= hi) { reject = 1; break; }
      } else { /* SSA_USE_PHI */
        if (use->idx != info->header_block) { reject = 1; break; }
      }
    }
    if (reject) continue;

    if (num_cands >= MAX_CANDS) return 0; /* bail; too many phis to fit */
    cands[num_cands].phi      = phi;
    cands[num_cands].c_pre    = c_pre;
    cands[num_cands].c_latch  = c_latch;
    cands[num_cands].btype    = phi->btype;
    cands[num_cands].new_vr   = -1;
    num_cands++;
  }
  if (num_cands == 0)
    return 0;

  /* Need num_cands SELECT slots plus one JUMP slot, all within the loop body
   * range starting at jpf_idx (the JUMPIF and subsequent body instructions
   * we'll overwrite). */
  int needed = num_cands + 1;
  if (info->jpf_idx + needed - 1 > hi) {
    fprintf(stderr, "[DLOOPG] not enough slots: jpf=%d needed=%d hi=%d\n",
            info->jpf_idx, needed, hi);
    return 0;
  }

  /* Also bail if any in-body TEMP defined past the slots we're about to
   * overwrite has uses outside the loop range — try_kill_loop_body's
   * analogous check, but generalized to allow phi-uses at any in-loop
   * block (since we'll NOP the whole body, those phis become dead too). */
  for (int idx = info->jpf_idx + needed; idx <= hi; idx++) {
    IRQuadCompact *q = &ir->compact_instructions[idx];
    if (q->op == TCCIR_OP_NOP || q->op == TCCIR_OP_JUMP) continue;
    if (!irop_config[q->op].has_dest) continue;
    if (q->op == TCCIR_OP_STORE || q->op == TCCIR_OP_STORE_INDEXED ||
        q->op == TCCIR_OP_STORE_POSTINC)
      return 0;
    IROperand d = tcc_ir_op_get_dest(ir, q);
    int32_t vr = irop_get_vreg(d);
    if (vr < 0) continue;
    if (TCCIR_DECODE_VREG_TYPE(vr) != TCCIR_VREG_TYPE_TEMP)
      return 0;
    IRSSAVregInfo *vi = ssa_opt_vinfo(ctx, vr);
    if (!vi) continue;
    for (int u = 0; u < vi->use_count; u++) {
      IRSSAUse *use = &vi->uses[u];
      if (use->kind == SSA_USE_INSTR) {
        if (use->idx < loop->start_idx || use->idx > hi)
          return 0;
      } else { /* SSA_USE_PHI: allow any block whose instructions live
                  inside the loop range — those phis die with the body. */
        int b = use->idx;
        if (b < 0 || b >= cfg->num_blocks)
          return 0;
        int bs = cfg->blocks[b].start_idx;
        int be = cfg->blocks[b].end_idx;
        if (bs < loop->start_idx || be > hi) {
          /* Phi-use outside the loop — that's a real escape, bail. */
          if (b != info->header_block)
            return 0;
        }
      }
    }
  }

  /* All checks passed — commit. Allocate fresh TEMPs. */
  for (int i = 0; i < num_cands; i++) {
    cands[i].new_vr = tcc_ir_vreg_alloc_temp(ir);
    if (cands[i].new_vr < 0)
      return 0;
  }

  /* Grow vinfo if needed for the new TEMPs. */
  int max_pos = 0;
  for (int i = 0; i < num_cands; i++) {
    int p = TCCIR_DECODE_VREG_POSITION(cands[i].new_vr);
    if (p > max_pos) max_pos = p;
  }
  if (max_pos >= ctx->vinfo_cap) {
    int new_cap = max_pos + 16;
    ctx->vinfo = tcc_realloc(ctx->vinfo, new_cap * sizeof(IRSSAVregInfo));
    memset(&ctx->vinfo[ctx->vinfo_cap], 0,
           (new_cap - ctx->vinfo_cap) * sizeof(IRSSAVregInfo));
    ctx->vinfo_cap = new_cap;
  }

  /* Capture exit target before clobbering the JUMPIF. */
  IRQuadCompact *jpf_q = &ir->compact_instructions[info->jpf_idx];
  IROperand exit_dest = tcc_ir_op_get_dest(ir, jpf_q);
  int exit_target = (int)irop_get_imm64_ex(ir, exit_dest);

  /* Drop phi operands flowing on dead edges before NOPing/overwriting body
   * instructions — same precaution as try_kill_loop_body. */
  int body_first_block = -1;
  if (info->jpf_idx + 1 < ir->next_instruction_index)
    body_first_block = cfg->instr_to_block[info->jpf_idx + 1];
  if (body_first_block >= 0 && body_first_block != info->header_block &&
      body_first_block != exit_target)
    ssa_drop_phi_edge(ctx, info->header_block, body_first_block);
  if (info->latch_block >= 0)
    ssa_drop_phi_edge(ctx, info->latch_block, info->header_block);

  /* Write SELECTs over JUMPIF and subsequent slots.  Slot 0 is the JUMPIF
   * (already not registered as a vreg def, so no use-list cleanup needed);
   * later slots may overlap real body instructions, so clear their uses
   * via ssa_opt_nop_instr first. */
  for (int i = 0; i < num_cands; i++) {
    int slot = info->jpf_idx + i;
    if (i > 0)
      ssa_opt_nop_instr(ctx, slot);

    IRQuadCompact *q = &ir->compact_instructions[slot];
    IROperand sel_dest = irop_make_vreg(cands[i].new_vr, cands[i].btype);
    IROperand sel_then = irop_make_imm32(-1, (int32_t)cands[i].c_latch, cands[i].btype);
    IROperand sel_else = irop_make_imm32(-1, (int32_t)cands[i].c_pre,   cands[i].btype);
    IROperand sel_cond = irop_make_imm32(-1, info->entry_tok, VT_INT);

    int pool_base = tcc_ir_iroperand_pool_add(ir, sel_dest);
    tcc_ir_iroperand_pool_add(ir, sel_then);
    tcc_ir_iroperand_pool_add(ir, sel_else);
    tcc_ir_iroperand_pool_add(ir, sel_cond);
    q->op = TCCIR_OP_SELECT;
    q->operand_base = pool_base;

    IRSSAVregInfo *vi = ssa_opt_vinfo(ctx, cands[i].new_vr);
    if (vi) {
      vi->def_instr     = slot;
      vi->def_phi_block = -1;
      vi->def_count     = 1;
    }
  }

  /* Place JUMP exit in the slot after the last SELECT. */
  {
    int slot = info->jpf_idx + num_cands;
    ssa_opt_nop_instr(ctx, slot);
    IRQuadCompact *q = &ir->compact_instructions[slot];
    IROperand jdest = irop_make_imm32(-1, exit_target, IROP_BTYPE_INT32);
    int pool_base = tcc_ir_iroperand_pool_add(ir, jdest);
    q->op = TCCIR_OP_JUMP;
    q->operand_base = pool_base;
  }

  /* NOP every body instruction past the JUMP. */
  for (int idx = info->jpf_idx + num_cands + 1; idx <= hi; idx++) {
    if (ir->compact_instructions[idx].op != TCCIR_OP_NOP)
      ssa_opt_nop_instr(ctx, idx);
  }

  /* Rewrite all post-loop INSTR uses of phi.dest_vreg → cand.new_vr. */
  for (int i = 0; i < num_cands; i++) {
    IRSSAVregInfo *old_vi = ssa_opt_vinfo(ctx, cands[i].phi->dest_vreg);
    IRSSAVregInfo *new_vi = ssa_opt_vinfo(ctx, cands[i].new_vr);
    if (!old_vi) continue;
    IROperand new_op = irop_make_vreg(cands[i].new_vr, cands[i].btype);

    int u = 0;
    while (u < old_vi->use_count) {
      IRSSAUse use = old_vi->uses[u];
      if (use.kind != SSA_USE_INSTR) { u++; continue; }
      IRQuadCompact *uq = &ir->compact_instructions[use.idx];
      int rewrote = 0;
      if (irop_config[uq->op].has_src1) {
        IROperand s = tcc_ir_op_get_src1(ir, uq);
        if (irop_get_vreg(s) == cands[i].phi->dest_vreg && !s.is_lval) {
          tcc_ir_op_set_src1(ir, uq, new_op);
          rewrote = 1;
        }
      }
      if (irop_config[uq->op].has_src2) {
        IROperand s = tcc_ir_op_get_src2(ir, uq);
        if (irop_get_vreg(s) == cands[i].phi->dest_vreg && !s.is_lval) {
          tcc_ir_op_set_src2(ir, uq, new_op);
          rewrote = 1;
        }
      }
      if (rewrote) {
        old_vi->uses[u] = old_vi->uses[--old_vi->use_count];
        if (new_vi)
          ssa_opt_add_use_instr(new_vi, use.idx);
      } else {
        u++;
      }
    }
  }

  return num_cands;
}

int ssa_opt_dead_loop(IRSSAOptCtx *ctx)
{
  TCCIRState *ir = ctx->ir;
  if (!ctx->ssa || !ctx->ssa->block_phis || !ctx->cfg)
    return 0;
  if (ir->next_instruction_index == 0)
    return 0;

  IRLoops *loops = tcc_ir_detect_loops(ir);
  if (!loops || loops->num_loops == 0) {
    tcc_ir_free_loops(loops);
    return 0;
  }

  int total = 0;
  for (int li = 0; li < loops->num_loops; li++) {
    IRLoop *loop = &loops->loops[li];
    if (loop->num_body_instrs == 0)
      continue;
    if (loop_body_has_side_effects(ctx, loop))
      continue;

    LoopEntryInfo info;
    if (!analyze_loop_entry(ctx, loop, &info))
      continue;

    if (info.proven_runs) {
      total += rewrite_loop_exit_phis(ctx, loop);
      total += try_kill_loop_body(ctx, loop);
    } else {
      /* Trip count not provable: emit a SELECT-based guard instead.  This
       * variant rewrites and kills the body in one step (the SELECT replaces
       * the phi value materialization, the JUMP replaces the back-edge). */
      total += rewrite_loop_exit_phis_guarded(ctx, loop, &info);
    }
  }

  tcc_ir_free_loops(loops);
  return total;
}
