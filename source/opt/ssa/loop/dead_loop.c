/*
 *  TCC IR - SSA Dead Loop Elimination
 *
 *  Copyright (c) 2025 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#define USING_GLOBALS
#include "ir.h"
#include "ssa_opt.h"
#include "opt/ssa/ssa_opt_helpers.h"
#include "licm.h"

/* Both arms of this pass are shape matchers, and every bail is silent.  The
 * block/phi/loop-bounds dump is what tells you which one you tripped -- the
 * rotated arm was written against it. */
TCC_DBG_ENV_FLAG(dbg_dead_loop, "TCC_DBG_DEAD_LOOP")

static void dl_dump(IRSSAOptCtx *ctx, IRLoops *loops)
{
  TCCIRState *ir = ctx->ir;
  IRCFG *cfg = ctx->cfg;
  fprintf(stderr, "[DL] ---- func, %d instrs, %d blocks, %d loops\n",
          ir->next_instruction_index, cfg ? cfg->num_blocks : -1,
          loops ? loops->num_loops : 0);
  for (int b = 0; cfg && b < cfg->num_blocks; b++) {
    fprintf(stderr, "[DL] block %d: [%d..%d] preds:", b,
            cfg->blocks[b].start_idx, cfg->blocks[b].end_idx);
    for (int p = 0; p < cfg->blocks[b].num_preds; p++)
      fprintf(stderr, " %d", cfg->blocks[b].preds[p]);
    fprintf(stderr, "\n");
    for (IRPhiNode *phi = ctx->ssa->block_phis[b]; phi; phi = phi->next) {
      fprintf(stderr, "[DL]   phi T%d <-", TCCIR_DECODE_VREG_POSITION(phi->dest_vreg));
      for (int o = 0; o < phi->num_operands; o++)
        fprintf(stderr, " [bb%d: T%d]", phi->operands[o].pred_block,
                TCCIR_DECODE_VREG_POSITION(phi->operands[o].vreg));
      fprintf(stderr, "\n");
    }
  }
  for (int li = 0; loops && li < loops->num_loops; li++) {
    IRLoop *l = &loops->loops[li];
    fprintf(stderr, "[DL] loop %d: header=%d start=%d end=%d pre=%d nbody=%d depth=%d\n",
            li, l->header_idx, l->start_idx, l->end_idx, l->preheader_idx,
            l->num_body_instrs, l->depth);
  }
}

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

/* end_idx only reaches the back-edge; forward-jumped body tails live in body_instrs. */
static int loop_max_idx(IRLoop *loop)
{
  int m = loop->end_idx;
  for (int k = 0; k < loop->num_body_instrs; k++) {
    if (loop->body_instrs[k] > m)
      m = loop->body_instrs[k];
  }
  return m;
}

/* Body upper bound: loop_max_idx can span past the exit target, so clamp. */
static int dead_loop_body_hi(TCCIRState *ir, IRLoop *loop)
{
  int hi = loop_max_idx(loop);

  int cmp_idx = -1;
  for (int j = loop->header_idx; j <= hi && j < ir->next_instruction_index; j++) {
    int op = ir->compact_instructions[j].op;
    if (op == TCCIR_OP_NOP)
      continue;
    if (op == TCCIR_OP_CMP) {
      cmp_idx = j;
      break;
    }
    break; /* header doesn't open with a compare — leave the bound as-is */
  }
  if (cmp_idx < 0)
    return hi;

  int jpf_idx = cmp_idx + 1;
  while (jpf_idx <= hi && ir->compact_instructions[jpf_idx].op == TCCIR_OP_NOP)
    jpf_idx++;
  if (jpf_idx > hi || ir->compact_instructions[jpf_idx].op != TCCIR_OP_JUMPIF)
    return hi;

  IROperand exit_dest = tcc_ir_op_get_dest(ir, &ir->compact_instructions[jpf_idx]);
  int exit_target = (int)irop_get_imm64_ex(ir, exit_dest);

  /* Body is exactly [header_idx, exit_target); bound in BOTH directions. */
  if (exit_target > loop->header_idx)
    hi = exit_target - 1;
  if (hi >= ir->next_instruction_index)
    hi = ir->next_instruction_index - 1;
  return hi;
}

static int loop_body_has_side_effects(IRSSAOptCtx *ctx, IRLoop *loop)
{
  TCCIRState *ir = ctx->ir;
  int hi = dead_loop_body_hi(ir, loop);
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

typedef struct LoopEntryInfo {
  int cmp_idx;
  int jpf_idx;
  int header_block;
  int latch_block;
  int going_up;        /* iv steps up toward bound */
  int going_down;      /* iv steps down toward bound */
  int exit_tok;        /* JUMPIF cond: true when loop NOT entered */
  int entry_tok;       /* inverse: true when loop ENTERED */
  int init_is_const;
  int64_t init_val;
  int bound_is_const;
  int64_t bound_val;
  IROperand bound_op;
  int32_t iv_vr;
  IRPhiNode *iv_phi;
  int proven_runs;     /* 1 iff trip count is provably >= 1 with constant bound */
} LoopEntryInfo;

/* Extracts entry-condition components; the bound need not be constant. */
static int analyze_loop_entry(IRSSAOptCtx *ctx, IRLoop *loop, LoopEntryInfo *out)
{
  TCCIRState *ir = ctx->ir;
  int hi = dead_loop_body_hi(ir, loop);
  memset(out, 0, sizeof(*out));

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

  /* CMP iv, bound: bound may be immediate or a loop-invariant vreg. */
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

  /* IV phi preheader operand must resolve to a constant for both paths. */
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

  /* Latch operand must be ADD/SUB of the iv with a positive step. */
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

  /* Trip count is provable only when the bound is a compile-time constant. */
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

/* Header phi with constant latch operand and no in-loop use: all uses are post-loop. */
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
  int hi = dead_loop_body_hi(ir, loop);

  int changes = 0;

  for (IRPhiNode *phi = ssa->block_phis[header_block]; phi; phi = phi->next) {
    if (phi->num_operands != 2)
      continue;

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

    /* In-loop use means the first iteration still needs the preheader value. */
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
      } else { /* SSA_USE_PHI: use sits at its block; header means in-loop. */
        if (use->idx == header_block) {
          has_in_loop_use = 1;
          break;
        }
      }
    }
    if (has_in_loop_use)
      continue;

    IROperand imm_op = irop_make_imm32(0, (int32_t)latch_const, phi->btype);
    if (phi->btype == IROP_BTYPE_INT64 || phi->btype == IROP_BTYPE_FLOAT64 ||
        phi->btype == IROP_BTYPE_FLOAT32) {
      /* irop_make_imm32 stores only 32 bits (a FLOAT64 latch constant is a
       * pair — truncating it rewrote a double to garbage, fuzz seed
       * fp_round:46, test 390), and FP consumers expect an F32/F64-tagged
       * operand, not TAG_IMM32. */
      continue;
    }

    /* Rewrite instruction uses only; phi uses keep the vreg. */
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
          /* Adopt the USE-site width: the phi may be narrower (char-typed
           * latch value) than the use — a STORE/STORE_INDEXED value operand
           * sets the store width, so installing the phi-btyped immediate
           * shrinks a word store to strb (fuzz seed bitfield:310). */
          tcc_ir_op_set_src1(ir, uq, ssa_cprop_imm_for_use(imm_op, s));
          rewritten_here = 1;
        }
      }
      if (irop_config[uq->op].has_src2) {
        IROperand s = tcc_ir_op_get_src2(ir, uq);
        if (irop_get_vreg(s) == phi->dest_vreg && !s.is_lval) {
          tcc_ir_op_set_src2(ir, uq, ssa_cprop_imm_for_use(imm_op, s));
          rewritten_here = 1;
        }
      }
      if (rewritten_here) {
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

/* Requires: loop pure, trip count >= 1, no escape except rewritten header phis. */
static int try_kill_loop_body(IRSSAOptCtx *ctx, IRLoop *loop)
{
  TCCIRState *ir = ctx->ir;
  IRSSAState *ssa = ctx->ssa;
  IRCFG *cfg = ctx->cfg;

  int hi = dead_loop_body_hi(ir, loop);

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

  /* Bail on any body TEMP used outside the loop range or in a non-header phi. */
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
      return 0; /* purity check ran already */

    IROperand d = tcc_ir_op_get_dest(ir, q);
    int32_t vr = irop_get_vreg(d);
    if (vr < 0)
      continue;
    /* Non-TEMP defs inside a loop body imply observable state. */
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

  /* Header phis must have no INSTR use outside the loop, no phi use outside the header. */
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

  /* Drop dying-edge phi operands before NOPing: ssa_drop_phi_edge walks use lists. */
  int body_first_block = -1;
  if (jpf_idx + 1 < ir->next_instruction_index)
    body_first_block = cfg->instr_to_block[jpf_idx + 1];
  if (body_first_block >= 0 && body_first_block != header_block &&
      body_first_block != exit_target /* never the exit block */)
    ssa_drop_phi_edge(ctx, header_block, body_first_block);
  if (latch_block >= 0)
    ssa_drop_phi_edge(ctx, latch_block, header_block);

  /* Convert JUMPIF to unconditional JUMP and NOP the CMP. */
  ssa_opt_nop_instr(ctx, cmp_idx);
  jpf->op = TCCIR_OP_JUMP;
  tcc_ir_set_src1(ir, jpf_idx, IROP_NONE);
  tcc_ir_set_src2(ir, jpf_idx, IROP_NONE);
  tcc_ir_set_dest(ir, jpf_idx, exit_dest);

  /* NOP the whole body up to hi, including the back-edge JUMP. */
  for (int idx = jpf_idx + 1; idx <= hi; idx++) {
    if (ir->compact_instructions[idx].op != TCCIR_OP_NOP)
      ssa_opt_nop_instr(ctx, idx);
  }

  return 1;
}

/* Trip count not provable: materialize header phis as SELECTs on the header CMP flags. */
static int rewrite_loop_exit_phis_guarded(IRSSAOptCtx *ctx, IRLoop *loop, LoopEntryInfo *info)
{
  TCCIRState *ir = ctx->ir;
  IRSSAState *ssa = ctx->ssa;
  IRCFG *cfg = ctx->cfg;
  if (!ssa || !ssa->block_phis || !cfg)
    return 0;

  int hi = dead_loop_body_hi(ir, loop);

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
    /* irop_make_imm32 stores only 32 bits, and the SELECT this becomes only
     * moves a single core register (no pairs, no VFP). */
    if (!irop_btype_select_lowerable(phi->btype)) continue;

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

    /* Phi must have no in-loop INSTR use and no phi-use outside the header. */
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

    if (num_cands >= MAX_CANDS) return 0;
    cands[num_cands].phi      = phi;
    cands[num_cands].c_pre    = c_pre;
    cands[num_cands].c_latch  = c_latch;
    cands[num_cands].btype    = phi->btype;
    cands[num_cands].new_vr   = -1;
    num_cands++;
  }
  if (num_cands == 0)
    return 0;

  /* num_cands SELECT slots plus one JUMP slot, all inside the body from jpf_idx. */
  int needed = num_cands + 1;
  if (info->jpf_idx + needed - 1 > hi) {
    fprintf(stderr, "[DLOOPG] not enough slots: jpf=%d needed=%d hi=%d\n",
            info->jpf_idx, needed, hi);
    return 0;
  }

  /* Body TEMPs past the overwritten slots may not escape; phi-uses at any in-loop block are ok. */
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
      } else { /* SSA_USE_PHI: in-loop blocks only; those phis die with the body. */
        int b = use->idx;
        if (b < 0 || b >= cfg->num_blocks)
          return 0;
        int bs = cfg->blocks[b].start_idx;
        int be = cfg->blocks[b].end_idx;
        if (bs < loop->start_idx || be > hi) {
          if (b != info->header_block)
            return 0;
        }
      }
    }
  }

  for (int i = 0; i < num_cands; i++) {
    cands[i].new_vr = tcc_ir_vreg_alloc_temp(ir);
    if (cands[i].new_vr < 0)
      return 0;
  }

  /* Grow vinfo for the new TEMPs. */
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

  /* Capture the exit target before clobbering the JUMPIF. */
  IRQuadCompact *jpf_q = &ir->compact_instructions[info->jpf_idx];
  IROperand exit_dest = tcc_ir_op_get_dest(ir, jpf_q);
  int exit_target = (int)irop_get_imm64_ex(ir, exit_dest);

  /* Drop dead-edge phi operands before NOPing or overwriting body instructions. */
  int body_first_block = -1;
  if (info->jpf_idx + 1 < ir->next_instruction_index)
    body_first_block = cfg->instr_to_block[info->jpf_idx + 1];
  if (body_first_block >= 0 && body_first_block != info->header_block &&
      body_first_block != exit_target)
    ssa_drop_phi_edge(ctx, info->header_block, body_first_block);
  if (info->latch_block >= 0)
    ssa_drop_phi_edge(ctx, info->latch_block, info->header_block);

  /* Slot 0 is the JUMPIF (no vreg def); later slots overlap real instructions. */
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

  /* JUMP to exit in the slot after the last SELECT. */
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

  /* Rewrite post-loop INSTR uses of phi.dest_vreg to cand.new_vr. */
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

/* ---------------------------------------------------------------------------
 * Rotated (bottom-test) loops
 *
 * analyze_loop_entry above only matches the un-rotated shape — CMP as the
 * first instruction of the header.  A loop that came out of rotation
 *
 *     guard:  CMP init, bound ; JUMPIF !cond -> exit
 *     body:   <pure>
 *     latch:  iv' = iv + step ; CMP iv', bound ; JUMPIF cond -> body
 *
 * has no CMP at its header, so it fell straight through the pass.  That is
 * every counting loop the pipeline actually produces: bench_function_calls
 * folds its five calls to one constant and then still spent 5035 cycles
 * running the empty 1000-iteration shell gcc deletes in 62.
 *
 * The rewrite kills the back-edge only and leaves the body in place, so the
 * body runs exactly once and the exit-block phis keep both of their edges.
 * That is sound when the body is pure, the trip count is finite, and every
 * value the body defines that is read after the loop is a compile-time
 * constant — identical on iteration 1 and iteration N.  DCE removes whatever
 * the straight-line remains no longer feed.
 * ------------------------------------------------------------------------ */

#define DL_MAX_ESCAPE_CANDS 64

static int dl_block_in_loop(IRCFG *cfg, IRLoop *loop, int b)
{
  if (!cfg || b < 0 || b >= cfg->num_blocks)
    return 0;
  return cfg->blocks[b].start_idx >= loop->start_idx &&
         cfg->blocks[b].start_idx <= loop->end_idx;
}

static int dl_instr_in_loop(IRLoop *loop, int idx)
{
  return idx >= loop->start_idx && idx <= loop->end_idx;
}

/* Hop ASSIGN copies to the value's origin; returns vr itself when it isn't one. */
static int32_t dl_skip_copies(IRSSAOptCtx *ctx, int32_t vr)
{
  TCCIRState *ir = ctx->ir;
  for (int hop = 0; hop < 8 && vr >= 0; hop++) {
    if (TCCIR_DECODE_VREG_TYPE(vr) != TCCIR_VREG_TYPE_TEMP)
      return vr;
    IRSSAVregInfo *vi = ssa_opt_vinfo(ctx, vr);
    if (!vi || vi->def_instr < 0 || ssa_opt_def_total(vi) != 1)
      return vr;
    IRQuadCompact *q = &ir->compact_instructions[vi->def_instr];
    if (q->op != TCCIR_OP_ASSIGN)
      return vr;
    IROperand s = tcc_ir_op_get_src1(ir, q);
    if (s.is_lval || s.tag != IROP_TAG_VREG)
      return vr;
    vr = irop_get_vreg(s);
  }
  return vr;
}

/* Direct IR scan, not use-def chains: the bound only has to survive the body. */
static int dl_vreg_untouched_in_loop(IRSSAOptCtx *ctx, IRLoop *loop, int32_t vr)
{
  TCCIRState *ir = ctx->ir;
  if (vr < 0)
    return 0;
  for (int i = loop->start_idx; i <= loop->end_idx && i < ir->next_instruction_index; i++) {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP || !irop_config[q->op].has_dest)
      continue;
    if (irop_get_vreg(tcc_ir_op_get_dest(ir, q)) == vr)
      return 0;
  }
  for (int b = 0; ctx->cfg && b < ctx->cfg->num_blocks; b++) {
    if (!dl_block_in_loop(ctx->cfg, loop, b))
      continue;
    for (IRPhiNode *p = ctx->ssa->block_phis[b]; p; p = p->next)
      if (p->dest_vreg == vr)
        return 0;
  }
  return 1;
}

/* The latch counts up toward the bound (or down toward it), so the back-edge
 * condition goes false after finitely many iterations.  Unsigned <= / >= are
 * excluded: with the bound at the type's extreme they wrap round forever. */
static int dl_cond_terminates(int tok, int going_up)
{
  if (going_up)
    return tok == TOK_LT || tok == TOK_ULT || tok == TOK_LE;
  return tok == TOK_GT || tok == TOK_UGT || tok == TOK_GE;
}

static int dl_operand_is_int(IROperand op)
{
  return op.btype != IROP_BTYPE_FLOAT32 && op.btype != IROP_BTYPE_FLOAT64;
}

/* Exactly one back-edge, and no path from outside lands past the header:
 * removing the back-edge of one of two latches, or of a loop a switch jumps
 * into, changes where the other entry falls out to. */
static int dl_single_entry_single_latch(IRSSAOptCtx *ctx, IRLoop *loop, int jpf_idx)
{
  TCCIRState *ir = ctx->ir;
  IRCFG *cfg = ctx->cfg;

  int back_edges = 0;
  for (int i = 0; i < ir->next_instruction_index; i++) {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_IJUMP || q->op == TCCIR_OP_SWITCH_TABLE) {
      /* Targets are not in the dest operand, so they cannot be checked. */
      if (!dl_instr_in_loop(loop, i))
        return 0;
      continue;
    }
    if (q->op != TCCIR_OP_JUMP && q->op != TCCIR_OP_JUMPIF)
      continue;
    int target = (int)irop_get_imm64_ex(ir, tcc_ir_op_get_dest(ir, q));
    if (target == loop->header_idx) {
      if (i >= loop->header_idx) {
        back_edges++;
        if (i != jpf_idx)
          return 0;
      }
      continue;
    }
    /* A jump from outside into the body proper re-enters past the header. */
    if (target > loop->start_idx && target <= loop->end_idx && !dl_instr_in_loop(loop, i))
      return 0;
  }
  if (back_edges != 1)
    return 0;

  int header_block = cfg->instr_to_block[loop->header_idx];
  for (int b = 0; b < cfg->num_blocks; b++) {
    if (b == header_block || !dl_block_in_loop(cfg, loop, b))
      continue;
    for (int p = 0; p < cfg->blocks[b].num_preds; p++)
      if (!dl_block_in_loop(cfg, loop, cfg->blocks[b].preds[p]))
        return 0;
  }
  return 1;
}

/* Values the body defines that are not compile-time constants.  Collected by
 * direct scan rather than off the use lists, which under-report once a pass
 * has NOP'd an instruction without unlinking its uses — here that would read
 * as "nothing escapes" and delete a loop whose result is still wanted. */
static int dl_collect_varying_defs(IRSSAOptCtx *ctx, IRLoop *loop,
                                   int32_t *cands, int *num_cands)
{
  TCCIRState *ir = ctx->ir;
  IRCFG *cfg = ctx->cfg;

  for (int idx = loop->start_idx; idx <= loop->end_idx; idx++) {
    IRQuadCompact *q = &ir->compact_instructions[idx];
    if (q->op == TCCIR_OP_NOP || q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_JUMPIF)
      continue;
    if (!irop_config[q->op].has_dest)
      continue;
    IROperand d = tcc_ir_op_get_dest(ir, q);
    int32_t vr = irop_get_vreg(d);
    if (vr < 0)
      continue;
    /* Non-TEMP defs inside a loop body imply observable state. */
    if (d.is_lval || TCCIR_DECODE_VREG_TYPE(vr) != TCCIR_VREG_TYPE_TEMP)
      return 0;

    int64_t cv;
    IRSSAVregInfo *vi = ssa_opt_vinfo(ctx, vr);
    if (vi && ssa_opt_def_total(vi) == 1 && resolve_const_through_copies(ctx, vr, &cv))
      continue;
    if (*num_cands >= DL_MAX_ESCAPE_CANDS)
      return 0;
    cands[(*num_cands)++] = vr;
  }

  for (int b = 0; b < cfg->num_blocks; b++) {
    if (!dl_block_in_loop(cfg, loop, b))
      continue;
    for (IRPhiNode *p = ctx->ssa->block_phis[b]; p; p = p->next) {
      if (*num_cands >= DL_MAX_ESCAPE_CANDS)
        return 0;
      cands[(*num_cands)++] = p->dest_vreg;
    }
  }
  return 1;
}

static int dl_in_cands(const int32_t *cands, int num_cands, int32_t vr)
{
  if (vr < 0)
    return 0;
  for (int i = 0; i < num_cands; i++)
    if (cands[i] == vr)
      return 1;
  return 0;
}

static int dl_any_cand_read_outside(IRSSAOptCtx *ctx, IRLoop *loop,
                                    const int32_t *cands, int num_cands)
{
  TCCIRState *ir = ctx->ir;
  IRCFG *cfg = ctx->cfg;

  for (int i = 0; i < ir->next_instruction_index; i++) {
    if (dl_instr_in_loop(loop, i))
      continue;
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;
    if (irop_config[q->op].has_src1 &&
        dl_in_cands(cands, num_cands, irop_get_vreg(tcc_ir_op_get_src1(ir, q))))
      return 1;
    if (irop_config[q->op].has_src2 &&
        dl_in_cands(cands, num_cands, irop_get_vreg(tcc_ir_op_get_src2(ir, q))))
      return 1;
    if (q->op == TCCIR_OP_MLA &&
        dl_in_cands(cands, num_cands, irop_get_vreg(tcc_ir_op_get_accum(ir, q))))
      return 1;
    if (irop_config[q->op].has_dest &&
        dl_in_cands(cands, num_cands, irop_get_vreg(tcc_ir_op_get_dest(ir, q))))
      return 1; /* re-def or memory write through the value */
  }

  for (int b = 0; b < cfg->num_blocks; b++) {
    if (dl_block_in_loop(cfg, loop, b))
      continue;
    for (IRPhiNode *p = ctx->ssa->block_phis[b]; p; p = p->next)
      for (int o = 0; o < p->num_operands; o++)
        if (dl_in_cands(cands, num_cands, p->operands[o].vreg))
          return 1;
  }
  return 0;
}

static int try_kill_rotated_loop(IRSSAOptCtx *ctx, IRLoop *loop)
{
  TCCIRState *ir = ctx->ir;
  IRSSAState *ssa = ctx->ssa;
  IRCFG *cfg = ctx->cfg;

  /* Body must end at the back-edge; a forward-jumped tail past it would become
   * the fall-through target once the JUMPIF goes away. */
  if (loop_max_idx(loop) != loop->end_idx)
    return 0;

  int jpf_idx = loop->end_idx;
  if (jpf_idx + 1 >= ir->next_instruction_index)
    return 0;
  if (jpf_idx >= cfg->num_instrs || loop->header_idx >= cfg->num_instrs)
    return 0;
  if (!ssa->block_phis)
    return 0;

  IRQuadCompact *jpf = &ir->compact_instructions[jpf_idx];
  if (jpf->op != TCCIR_OP_JUMPIF)
    return 0;
  if ((int)irop_get_imm64_ex(ir, tcc_ir_op_get_dest(ir, jpf)) != loop->header_idx)
    return 0;

  int cmp_idx = jpf_idx - 1;
  while (cmp_idx >= loop->start_idx && ir->compact_instructions[cmp_idx].op == TCCIR_OP_NOP)
    cmp_idx--;
  if (cmp_idx < loop->start_idx)
    return 0;
  IRQuadCompact *cmp = &ir->compact_instructions[cmp_idx];
  if (cmp->op != TCCIR_OP_CMP)
    return 0;

  int header_block = cfg->instr_to_block[loop->header_idx];
  int latch_block = cfg->instr_to_block[jpf_idx];
  int fallthru_block = cfg->instr_to_block[jpf_idx + 1];
  if (header_block < 0 || latch_block < 0 || fallthru_block < 0)
    return 0;
  if (fallthru_block == header_block || dl_block_in_loop(cfg, loop, fallthru_block))
    return 0;

  /* --- the compare drives a monotone induction variable ------------------ */
  IROperand c1 = tcc_ir_op_get_src1(ir, cmp);
  IROperand c2 = tcc_ir_op_get_src2(ir, cmp);
  if (c1.is_lval || c2.is_lval || !dl_operand_is_int(c1) || !dl_operand_is_int(c2))
    return 0;
  if (tcc_ir_barrel_shift_at(ir, cmp) != 0)
    return 0;

  int32_t next_vr = irop_get_vreg(c1);
  if (next_vr < 0)
    return 0;
  if (!irop_is_immediate(c2) && !dl_vreg_untouched_in_loop(ctx, loop, irop_get_vreg(c2)))
    return 0;

  int32_t next_base = dl_skip_copies(ctx, next_vr);
  IRPhiNode *iv_phi = NULL;
  int going_up = 0;

  for (IRPhiNode *p = ssa->block_phis[header_block]; p && !iv_phi; p = p->next) {
    if (p->num_operands != 2)
      continue;
    int32_t latch_vr = -1;
    for (int oi = 0; oi < p->num_operands; oi++)
      if (p->operands[oi].pred_block == latch_block)
        latch_vr = p->operands[oi].vreg;
    if (latch_vr < 0 || dl_skip_copies(ctx, latch_vr) != next_base)
      continue;

    IRSSAVregInfo *bvi = ssa_opt_vinfo(ctx, next_base);
    if (!bvi || bvi->def_instr < 0 || ssa_opt_def_total(bvi) != 1)
      continue;
    if (!dl_instr_in_loop(loop, bvi->def_instr))
      continue;
    IRQuadCompact *dq = &ir->compact_instructions[bvi->def_instr];
    if (dq->op != TCCIR_OP_ADD && dq->op != TCCIR_OP_SUB)
      continue;
    if (tcc_ir_barrel_shift_at(ir, dq) != 0)
      continue;
    IROperand a = tcc_ir_op_get_src1(ir, dq);
    IROperand step_op = tcc_ir_op_get_src2(ir, dq);
    if (a.is_lval || step_op.is_lval)
      continue;
    if (dl_skip_copies(ctx, irop_get_vreg(a)) != p->dest_vreg)
      continue;
    if (!irop_is_immediate(step_op) || !dl_operand_is_int(step_op))
      continue;
    if (irop_get_imm64_ex(ir, step_op) <= 0)
      continue;

    going_up = (dq->op == TCCIR_OP_ADD);
    iv_phi = p;
  }
  if (!iv_phi)
    return 0;

  int back_tok = (int)irop_get_imm64_ex(ir, tcc_ir_op_get_src1(ir, jpf));
  if (!dl_cond_terminates(back_tok, going_up))
    return 0;

  /* Whole-IR scans last: by here the loop is known to be a pure counting one,
   * which is a handful of loops per translation unit rather than every one. */
  if (!dl_single_entry_single_latch(ctx, loop, jpf_idx))
    return 0;

  /* --- nothing the body computes may differ between iteration 1 and N ---- */
  int32_t cands[DL_MAX_ESCAPE_CANDS];
  int num_cands = 0;
  if (!dl_collect_varying_defs(ctx, loop, cands, &num_cands))
    return 0;
  if (dl_any_cand_read_outside(ctx, loop, cands, num_cands))
    return 0;

  /* Back-edge dies, fall-through survives — the same rewrite as folding the
   * latch JUMPIF not-taken.  Drop the phi operands first: ssa_drop_phi_edge
   * walks the use lists the NOPs are about to invalidate. */
  ssa_drop_phi_edge(ctx, latch_block, header_block);
  ssa_opt_nop_instr(ctx, cmp_idx);
  ssa_opt_nop_instr(ctx, jpf_idx);
  return 1;
}

int ssa_opt_dead_loop(IRSSAOptCtx *ctx)
{
  TCCIRState *ir = ctx->ir;
  if (!ctx->ssa || !ctx->ssa->block_phis || !ctx->cfg)
    return 0;
  if (ir->next_instruction_index == 0)
    return 0;

  /* A volatile access is not a side effect in ssa_opt_has_side_effects, so the
   * purity test below would happily delete a `while (n--) x = *mmio;` poll. */
  if (ir->func_has_volatile_access)
    return 0;

  IRLoops *loops = tcc_ir_detect_loops(ir);
  if (!loops || loops->num_loops == 0) {
    tcc_ir_free_loops(loops);
    return 0;
  }

  TCC_DBG_BLOCK(dbg_dead_loop) { dl_dump(ctx, loops); }

  int total = 0;
  for (int li = 0; li < loops->num_loops; li++) {
    IRLoop *loop = &loops->loops[li];
    if (loop->num_body_instrs == 0)
      continue;
    if (loop_body_has_side_effects(ctx, loop))
      continue;

    LoopEntryInfo info;
    if (!analyze_loop_entry(ctx, loop, &info)) {
      total += try_kill_rotated_loop(ctx, loop);
      continue;
    }

    if (info.proven_runs) {
      total += rewrite_loop_exit_phis(ctx, loop);
      total += try_kill_loop_body(ctx, loop);
    } else {
      total += rewrite_loop_exit_phis_guarded(ctx, loop, &info);
    }
  }

  tcc_ir_free_loops(loops);
  return total;
}
