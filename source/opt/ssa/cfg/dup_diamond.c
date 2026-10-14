/*
 *  TCC IR - SSA duplicate diamond reuse
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#define USING_GLOBALS
#include "ir.h"
#include "ssa_opt.h"
#include "opt_range.h"
#include "opt_utils.h"
#include "opt_dsl_phi.h"
#include "opt/ssa/dup_diamond.h"

/* Shape and proof obligations: docs/optimizations/ssa_passes.md, ssa:dup_diamond. */

#define DD_MAX_ARM 16
#define DD_MAX_OUT 8

typedef struct DDiamond
{
  int head, test, jif, tok, join;
  int arm[2]; /* [0] the fall-through (condition false), [1] the taken block */
} DDiamond;

typedef struct DDMatch
{
  int32_t from[DD_MAX_ARM * 2], to[DD_MAX_ARM * 2];
  int nmap;
  int32_t w[DD_MAX_OUT], v[DD_MAX_OUT];
  int bt[DD_MAX_OUT];
  int nvar;
  IRPhiNode *phi2[DD_MAX_OUT], *phi1[DD_MAX_OUT];
  int nphi;
  int side[2]; /* d1 arm matched by d2 arm k */
  uint8_t *param_mut;
  int nparam;
} DDMatch;

static int dd_last_real(TCCIRState *ir, int lo, int hi)
{
  for (int i = hi; i >= lo; i--)
    if (ir->compact_instructions[i].op != TCCIR_OP_NOP)
      return i;
  return -1;
}

static int dd_annotated(TCCIRState *ir, IRQuadCompact *q)
{
  return tcc_ir_barrel_shift_at(ir, q) || tcc_ir_shift64_dead_half_at(ir, q) || tcc_ir_zero_half64_at(ir, q) ||
         tcc_ir_bfi_params_at(ir, q);
}

/* Does arm block `x` (single pred `head`) end by reaching `join`, with no other way out? */
static int dd_arm_reaches(IRSSAOptCtx *ctx, int x, int head, int join)
{
  IRCFG *cfg = ctx->cfg;
  TCCIRState *ir = ctx->ir;
  IRBasicBlock *bb = &cfg->blocks[x];
  if (bb->num_preds != 1 || bb->preds[0] != head || bb->num_succs != 1 || bb->succs[0] != join)
    return 0;
  if (ctx->ssa->block_phis && ctx->ssa->block_phis[x])
    return 0;
  int last = dd_last_real(ir, bb->start_idx, bb->end_idx - 1);
  if (last >= 0 && ir->compact_instructions[last].op == TCCIR_OP_JUMP)
    return (int)tcc_ir_op_dest_imm(ir, &ir->compact_instructions[last]) == cfg->blocks[join].start_idx;
  if (last >= 0 && ir_op_has(ir->compact_instructions[last].op, IROP_ENDS_BLOCK))
    return 0;
  return bb->end_idx == cfg->blocks[join].start_idx;
}

static int dd_diamond_at(IRSSAOptCtx *ctx, int b, DDiamond *d)
{
  IRCFG *cfg = ctx->cfg;
  TCCIRState *ir = ctx->ir;
  IRBasicBlock *bb = &cfg->blocks[b];
  if (bb->num_succs != 2)
    return 0;
  int jif = dd_last_real(ir, bb->start_idx, bb->end_idx - 1);
  if (jif < 0 || jif != bb->end_idx - 1 || ir->compact_instructions[jif].op != TCCIR_OP_JUMPIF)
    return 0;
  int test = dd_last_real(ir, bb->start_idx, jif - 1);
  if (test < 0)
    return 0;
  int top = ir->compact_instructions[test].op;
  if (top != TCCIR_OP_CMP && top != TCCIR_OP_TEST_ZERO)
    return 0;
  int target = (int)tcc_ir_op_dest_imm(ir, &ir->compact_instructions[jif]);
  if (target < 0 || target >= cfg->num_instrs || jif + 1 >= cfg->num_instrs)
    return 0;
  d->head = b;
  d->test = test;
  d->jif = jif;
  d->tok = (int)tcc_ir_op_src1_imm(ir, &ir->compact_instructions[jif]);
  d->arm[1] = cfg->instr_to_block[target];
  d->arm[0] = cfg->instr_to_block[jif + 1];
  if (d->arm[0] == d->arm[1] || cfg->blocks[d->arm[1]].start_idx != target || d->arm[0] == b || d->arm[1] == b)
    return 0;
  IRBasicBlock *a0 = &cfg->blocks[d->arm[0]];
  if (a0->num_succs != 1)
    return 0;
  d->join = a0->succs[0];
  if (d->join == b || d->join == d->arm[0] || d->join == d->arm[1] || cfg->blocks[d->join].num_preds != 2)
    return 0;
  return dd_arm_reaches(ctx, d->arm[0], b, d->join) && dd_arm_reaches(ctx, d->arm[1], b, d->join);
}

static int dd_param_stable(DDMatch *m, int32_t vr)
{
  int pos = TCCIR_DECODE_VREG_POSITION(vr);
  return pos < m->nparam && !m->param_mut[pos];
}

static int dd_stable(IRSSAOptCtx *ctx, DDMatch *m, int32_t vr)
{
  if (TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_PARAM)
    return dd_param_stable(m, vr);
  if (TCCIR_DECODE_VREG_TYPE(vr) != TCCIR_VREG_TYPE_TEMP)
    return 0;
  return ssa_opt_def_total(ssa_opt_vinfo(ctx, vr)) == 1;
}

/* The stable value a vreg copies, through same-width TEMP copies. */
static int32_t dd_root(IRSSAOptCtx *ctx, DDMatch *m, int32_t vr)
{
  TCCIRState *ir = ctx->ir;
  for (int hop = 0; hop < 8; hop++) {
    if (!dd_stable(ctx, m, vr) || TCCIR_DECODE_VREG_TYPE(vr) != TCCIR_VREG_TYPE_TEMP)
      break;
    IRSSAVregInfo *vi = ssa_opt_vinfo(ctx, vr);
    if (vi->def_instr < 0)
      break;
    IRQuadCompact *dq = &ir->compact_instructions[vi->def_instr];
    if (dq->op != TCCIR_OP_ASSIGN || dd_annotated(ir, dq))
      break;
    IROperand d = tcc_ir_op_get_dest(ir, dq), s = tcc_ir_op_get_src1(ir, dq);
    int32_t sv = irop_get_vreg(s);
    if (sv < 0 || irop_get_tag(s) != IROP_TAG_VREG || s.is_lval || s.is_local || s.is_llocal ||
        irop_get_btype(s) != irop_get_btype(d) || !dd_stable(ctx, m, sv))
      break;
    vr = sv;
  }
  return vr;
}

static int32_t dd_mapped(DDMatch *m, int32_t vr)
{
  for (int i = 0; i < m->nmap; i++)
    if (m->from[i] == vr)
      return m->to[i];
  return -1;
}

/* Is operand o2 (read in diamond 2) the same value as o1 (read at the matching place in diamond 1)? */
static int dd_operand_equiv(IRSSAOptCtx *ctx, DDMatch *m, IROperand o2, IROperand o1)
{
  int32_t v2 = irop_get_vreg(o2), v1 = irop_get_vreg(o1);
  if (v2 < 0 || v1 < 0)
    return v2 == v1 && o2.vr == o1.vr && o2.u.imm32 == o1.u.imm32 &&
           memcmp((const char *)&o2 + 8, (const char *)&o1 + 8, sizeof(IROperand) - 8) == 0;
  IROperand a = o2, b = o1;
  irop_set_vreg(&a, 0);
  irop_set_vreg(&b, 0);
  if (a.vr != b.vr || irop_get_tag(o2) != IROP_TAG_VREG || o2.u.imm32 != o1.u.imm32 ||
      memcmp((const char *)&a + 8, (const char *)&b + 8, sizeof(IROperand) - 8) != 0)
    return 0;
  int32_t mv = dd_mapped(m, v2);
  if (mv >= 0)
    return mv == v1;
  for (int i = 0; i < m->nmap; i++)
    if (m->to[i] == v1)
      return 0;
  int32_t r = dd_root(ctx, m, v2);
  return r == dd_root(ctx, m, v1) && dd_stable(ctx, m, r);
}

static int dd_slot_count(int op)
{
  return ir_op_has(op, IROP_A_SLOT3) ? 4 : 3;
}

static IROperand dd_slot(TCCIRState *ir, IRQuadCompact *q, int s)
{
  if (s == 1 && irop_config[q->op].has_src1)
    return tcc_ir_op_get_src1(ir, q);
  if (s == 2 && irop_config[q->op].has_src2)
    return tcc_ir_op_get_src2(ir, q);
  if (s == 3 && ir_op_has(q->op, IROP_A_SLOT3))
    return ir->iroperand_pool[q->operand_base + 3];
  return IROP_NONE;
}

/* A whole, direct access to a VAR's own slot. */
static int dd_var_slot(TCCIRState *ir, IROperand o)
{
  int32_t vr = irop_get_vreg(o);
  if (vr < 0 || TCCIR_DECODE_VREG_TYPE(vr) != TCCIR_VREG_TYPE_VAR)
    return 0;
  int pos = TCCIR_DECODE_VREG_POSITION(vr);
  return pos < ir->variables_live_intervals_size && irop_get_tag(o) == IROP_TAG_STACKOFF && o.is_local &&
         o.is_lval && !o.is_llocal && o.u.imm32 == ir->variables_live_intervals[pos].original_offset;
}

static int dd_var_ok(TCCIRState *ir, int32_t vr)
{
  IRLiveInterval *li = &ir->variables_live_intervals[TCCIR_DECODE_VREG_POSITION(vr)];
  return !li->addrtaken && !li->is_volatile && !li->is_complex && !li->is_struct;
}

static int dd_is_var_store(TCCIRState *ir, IRQuadCompact *q)
{
  return q->op == TCCIR_OP_STORE && dd_var_slot(ir, tcc_ir_op_get_dest(ir, q));
}

#define DD_HZ_READS (IR_HZ_MEM_READ | IR_HZ_SRC_LVAL | IR_HZ_DEST_STACKOFF | IR_HZ_JOIN)
#define DD_HZ_VAR_WRITE (IR_HZ_MEM_WRITE | IR_HZ_DEST_LVAL)

/* Pure computation, a plain load, or a VAR value store: what a matched arm may hold. */
static int dd_arm_op_ok(TCCIRState *ir, IRQuadCompact *q)
{
  if (dd_annotated(ir, q) || !irop_config[q->op].has_dest)
    return 0;
  uint32_t allow = DD_HZ_READS | (dd_is_var_store(ir, q) ? DD_HZ_VAR_WRITE : 0);
  return !ir_q_hazards(ir, q, IR_HZ_ALL & ~allow);
}

static int dd_collect_arm(TCCIRState *ir, IRBasicBlock *bb, int *out)
{
  int n = 0;
  for (int i = bb->start_idx; i < bb->end_idx; i++) {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP || (q->op == TCCIR_OP_JUMP && i == bb->end_idx - 1))
      continue;
    if (q->op == TCCIR_OP_JUMP || n >= DD_MAX_ARM || !dd_arm_op_ok(ir, q))
      return -1;
    out[n++] = i;
  }
  return n;
}

static int dd_add_var_pair(DDMatch *m, int32_t w, int32_t v, int bt)
{
  for (int i = 0; i < m->nvar; i++)
    if (m->w[i] == w || m->v[i] == v)
      return m->w[i] == w && m->v[i] == v;
  if (m->nvar >= DD_MAX_OUT || w == v)
    return 0;
  m->w[m->nvar] = w;
  m->bt[m->nvar] = bt;
  m->v[m->nvar++] = v;
  return 1;
}

static int dd_match_arm(IRSSAOptCtx *ctx, DDMatch *m, IRBasicBlock *b2, IRBasicBlock *b1)
{
  TCCIRState *ir = ctx->ir;
  int l2[DD_MAX_ARM], l1[DD_MAX_ARM];
  int n2 = dd_collect_arm(ir, b2, l2), n1 = dd_collect_arm(ir, b1, l1);
  if (n2 < 0 || n2 != n1)
    return 0;
  for (int i = 0; i < n2; i++) {
    IRQuadCompact *q2 = &ir->compact_instructions[l2[i]], *q1 = &ir->compact_instructions[l1[i]];
    if (q2->op != q1->op)
      return 0;
    for (int s = 1; s < dd_slot_count(q2->op); s++)
      if (!dd_operand_equiv(ctx, m, dd_slot(ir, q2, s), dd_slot(ir, q1, s)))
        return 0;
    IROperand d2 = tcc_ir_op_get_dest(ir, q2), d1 = tcc_ir_op_get_dest(ir, q1);
    int32_t w = irop_get_vreg(d2), v = irop_get_vreg(d1);
    if (dd_is_var_store(ir, q2)) {
      if (!dd_is_var_store(ir, q1) || irop_get_btype(d2) != irop_get_btype(d1) || !dd_add_var_pair(m, w, v, irop_get_btype(d2)))
        return 0;
      continue;
    }
    if (w < 0 || v < 0 || TCCIR_DECODE_VREG_TYPE(w) != TCCIR_VREG_TYPE_TEMP ||
        TCCIR_DECODE_VREG_TYPE(v) != TCCIR_VREG_TYPE_TEMP || d2.is_lval || d1.is_lval ||
        irop_get_btype(d2) != irop_get_btype(d1) || ssa_opt_def_total(ssa_opt_vinfo(ctx, w)) != 1 ||
        ssa_opt_def_total(ssa_opt_vinfo(ctx, v)) != 1 || m->nmap >= DD_MAX_ARM * 2)
      return 0;
    m->from[m->nmap] = w;
    m->to[m->nmap++] = v;
  }
  return 1;
}

static int32_t dd_phi_operand(IRPhiNode *phi, int pred)
{
  for (int i = 0; i < phi->num_operands; i++)
    if (phi->operands[i].pred_block == pred)
      return phi->operands[i].vreg;
  return -1;
}

static int dd_vreg_equiv(IRSSAOptCtx *ctx, DDMatch *m, int32_t v2, int32_t v1)
{
  if (v2 < 0 || v1 < 0)
    return 0;
  return dd_operand_equiv(ctx, m, irop_make_vreg(v2, IROP_BTYPE_INT32), irop_make_vreg(v1, IROP_BTYPE_INT32));
}

/* Each phi at join2 is a phi at join1 over the matching arms. */
static int dd_match_phis(IRSSAOptCtx *ctx, DDMatch *m, DDiamond *d2, DDiamond *d1)
{
  if (!ctx->ssa->block_phis)
    return 1;
  for (IRPhiNode *p2 = ctx->ssa->block_phis[d2->join]; p2; p2 = p2->next) {
    int32_t c0 = dd_phi_operand(p2, d2->arm[0]), c1 = dd_phi_operand(p2, d2->arm[1]);
    if (p2->num_operands != 2 || c0 < 0 || c1 < 0)
      return 0;
    IRPhiNode *hit = NULL;
    for (IRPhiNode *p1 = ctx->ssa->block_phis[d1->join]; p1 && !hit; p1 = p1->next) {
      if (p1->btype != p2->btype || p1->num_operands != 2)
        continue;
      if (dd_vreg_equiv(ctx, m, c0, dd_phi_operand(p1, d1->arm[m->side[0]])) &&
          dd_vreg_equiv(ctx, m, c1, dd_phi_operand(p1, d1->arm[m->side[1]])))
        hit = p1;
    }
    IRSSAVregInfo *vi2 = ssa_opt_vinfo(ctx, p2->dest_vreg);
    if (!hit || m->nphi >= DD_MAX_OUT || !vi2 || vi2->def_count != 0 || vi2->def_phi_block != d2->join ||
        !ssa_opt_can_replace_all_uses(ctx, p2->dest_vreg) || !opt_dsl_phi_metadata_valid(ctx, d2->join, p2))
      return 0;
    IRSSAVregInfo *vi1 = ssa_opt_vinfo(ctx, hit->dest_vreg);
    if (!vi1 || vi1->def_count != 0 || vi1->def_phi_block != d1->join)
      return 0;
    m->phi2[m->nphi] = p2;
    m->phi1[m->nphi++] = hit;
  }
  return 1;
}

/* A TEMP defined in diamond 2's arms is read only there or by a matched join2 phi. */
static int dd_arm_temps_local(IRSSAOptCtx *ctx, DDMatch *m, DDiamond *d2)
{
  IRCFG *cfg = ctx->cfg;
  for (int i = 0; i < m->nmap; i++) {
    IRSSAVregInfo *vi = ssa_opt_vinfo(ctx, m->from[i]);
    for (int u = 0; u < vi->use_count; u++) {
      IRSSAUse use = vi->uses[u];
      if (use.kind == SSA_USE_PHI) {
        if (use.idx != d2->join)
          return 0;
        continue;
      }
      int blk = use.idx < cfg->num_instrs ? cfg->instr_to_block[use.idx] : -1;
      if (blk != d2->arm[0] && blk != d2->arm[1])
        return 0;
    }
  }
  return 1;
}

/* Nothing between the two tests writes memory or leaves the block. */
static int dd_middle_ok(TCCIRState *ir, int lo, int hi)
{
  for (int i = lo; i < hi; i++) {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;
    uint32_t allow = DD_HZ_READS;
    if (irop_config[q->op].has_dest && q->op != TCCIR_OP_STORE_INDEXED && q->op != TCCIR_OP_STORE_POSTINC) {
      IROperand d = tcc_ir_op_get_dest(ir, q);
      int32_t vr = irop_get_vreg(d);
      if (dd_var_slot(ir, d) && dd_var_ok(ir, vr))
        allow |= DD_HZ_VAR_WRITE;
      else if (q->op == TCCIR_OP_STORE && vr >= 0 && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_TEMP &&
               !d.is_lval && !d.is_llocal)
        allow |= IR_HZ_MEM_WRITE;
    }
    if (ir_q_hazards(ir, q, IR_HZ_ALL & ~allow))
      return 0;
  }
  return 1;
}

/* W (diamond 2's result) and V (diamond 1's) are written only by their arms and
 * accessed whole at one width; every read of W is dominated by join2. */
static int dd_vars_ok(IRSSAOptCtx *ctx, DDMatch *m, DDiamond *d2, DDiamond *d1)
{
  TCCIRState *ir = ctx->ir;
  IRCFG *cfg = ctx->cfg;
  for (int k = 0; k < m->nvar; k++) {
    if (!dd_var_ok(ir, m->w[k]) || !dd_var_ok(ir, m->v[k]))
      return 0;
    IRLiveInterval *lw = &ir->variables_live_intervals[TCCIR_DECODE_VREG_POSITION(m->w[k])];
    IRLiveInterval *lv = &ir->variables_live_intervals[TCCIR_DECODE_VREG_POSITION(m->v[k])];
    if (lw->is_float != lv->is_float || lw->is_double != lv->is_double || lw->is_llong != lv->is_llong)
      return 0;
  }
  int n = ir->next_instruction_index < cfg->num_instrs ? ir->next_instruction_index : cfg->num_instrs;
  for (int i = 0; m->nvar && i < n; i++) {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;
    if (q->op == TCCIR_OP_SET_CHAIN || q->op == TCCIR_OP_INIT_CHAIN_SLOT)
      return 0;
    int blk = cfg->instr_to_block[i];
    int writes = irop_config[q->op].has_dest && q->op != TCCIR_OP_STORE_INDEXED && q->op != TCCIR_OP_STORE_POSTINC;
    for (int s = 0; s < 4; s++) {
      IROperand o = s == 0 ? (irop_config[q->op].has_dest ? tcc_ir_op_get_dest(ir, q) : IROP_NONE)
                           : (s < dd_slot_count(q->op) ? dd_slot(ir, q, s) : IROP_NONE);
      int32_t ov = irop_get_vreg(o);
      for (int k = 0; k < m->nvar; k++) {
        if (ov != m->w[k] && ov != m->v[k])
          continue;
        if (!dd_var_slot(ir, o) || irop_get_btype(o) != m->bt[k] || q->op == TCCIR_OP_LEA ||
            q->op == TCCIR_OP_ASM_INPUT || q->op == TCCIR_OP_ASM_OUTPUT || q->op == TCCIR_OP_INLINE_ASM)
          return 0;
        if (s == 0 && writes) {
          int own = ov == m->w[k] ? (blk == d2->arm[0] || blk == d2->arm[1]) : (blk == d1->arm[0] || blk == d1->arm[1]);
          if (!own || !dd_is_var_store(ir, q))
            return 0;
        } else if (ov == m->w[k] && !tcc_ir_cfg_dominates(cfg, d2->join, blk)) {
          return 0;
        }
      }
    }
  }
  return 1;
}

static void dd_rename_var_reads(IRSSAOptCtx *ctx, DDMatch *m, DDiamond *d2)
{
  TCCIRState *ir = ctx->ir;
  IRCFG *cfg = ctx->cfg;
  int n = ir->next_instruction_index < cfg->num_instrs ? ir->next_instruction_index : cfg->num_instrs;
  for (int i = 0; i < n; i++) {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;
    int blk = cfg->instr_to_block[i];
    if (blk == d2->arm[0] || blk == d2->arm[1])
      continue;
    for (int k = 0; k < m->nvar; k++) {
      int32_t off = ir->variables_live_intervals[TCCIR_DECODE_VREG_POSITION(m->v[k])].original_offset;
      IROperand *slots[4] = {0};
      if (irop_config[q->op].has_dest)
        slots[0] = &ir->iroperand_pool[q->operand_base];
      for (int s = 1; s < dd_slot_count(q->op); s++)
        if ((s == 1 && irop_config[q->op].has_src1) || (s == 2 && irop_config[q->op].has_src2) ||
            (s == 3 && ir_op_has(q->op, IROP_A_SLOT3)))
          slots[s] = &ir->iroperand_pool[q->operand_base + s];
      for (int s = 0; s < 4; s++) {
        if (!slots[s] || irop_get_vreg(*slots[s]) != m->w[k])
          continue;
        irop_set_vreg(slots[s], m->v[k]);
        if (irop_get_tag(*slots[s]) == IROP_TAG_STACKOFF)
          slots[s]->u.imm32 = off;
      }
    }
  }
}

static void dd_apply(IRSSAOptCtx *ctx, DDMatch *m, DDiamond *d2)
{
  TCCIRState *ir = ctx->ir;
  IRCFG *cfg = ctx->cfg;
  for (int k = 0; k < m->nphi; k++) {
    ssa_opt_replace_all_uses(ctx, m->phi2[k]->dest_vreg, m->phi1[k]->dest_vreg);
    IRPhiNode **link = &ctx->ssa->block_phis[d2->join];
    while (*link && *link != m->phi2[k])
      link = &(*link)->next;
    opt_dsl_phi_remove(ctx, d2->join, link);
  }
  dd_rename_var_reads(ctx, m, d2);
  for (int a = 0; a < 2; a++) {
    IRBasicBlock *bb = &cfg->blocks[d2->arm[a]];
    for (int i = bb->start_idx; i < bb->end_idx; i++)
      if (ir->compact_instructions[i].op != TCCIR_OP_JUMP)
        ssa_opt_nop_instr(ctx, i);
  }
  ssa_opt_nop_instr(ctx, d2->test);
  ssa_opt_nop_instr(ctx, d2->jif);
  ssa_drop_phi_edge(ctx, d2->head, d2->arm[1]);
  ssa_drop_phi_edge(ctx, d2->arm[1], d2->join);
}

static int dd_try(IRSSAOptCtx *ctx, DDMatch *m, DDiamond *d1)
{
  TCCIRState *ir = ctx->ir;
  DDiamond d2;
  if (!dd_diamond_at(ctx, d1->join, &d2) || d2.join == d1->head || d2.arm[0] == d1->head || d2.arm[1] == d1->head)
    return 0;
  IRQuadCompact *t1 = &ir->compact_instructions[d1->test], *t2 = &ir->compact_instructions[d2.test];
  if (t1->op != t2->op || dd_annotated(ir, t1) || dd_annotated(ir, t2))
    return 0;
  if (d2.tok == d1->tok) {
    m->side[0] = 0;
    m->side[1] = 1;
  } else if (d2.tok == invert_cond_token(d1->tok) && d2.tok >= 0) {
    m->side[0] = 1;
    m->side[1] = 0;
  } else {
    return 0;
  }
  m->nmap = m->nvar = m->nphi = 0;
  for (int s = 1; s < dd_slot_count(t2->op); s++)
    if (!dd_operand_equiv(ctx, m, dd_slot(ir, t2, s), dd_slot(ir, t1, s)))
      return 0;
  if (!dd_middle_ok(ir, ctx->cfg->blocks[d1->join].start_idx, d2.test))
    return 0;
  for (int k = 0; k < 2; k++)
    if (!dd_match_arm(ctx, m, &ctx->cfg->blocks[d2.arm[k]], &ctx->cfg->blocks[d1->arm[m->side[k]]]))
      return 0;
  if (!dd_match_phis(ctx, m, &d2, d1) || m->nvar + m->nphi == 0 || !dd_arm_temps_local(ctx, m, &d2) ||
      !dd_vars_ok(ctx, m, &d2, d1))
    return 0;
  dd_apply(ctx, m, &d2);
  return 1;
}

static void dd_param_scan(TCCIRState *ir, DDMatch *m)
{
  for (int i = 0; i < ir->next_instruction_index; i++) {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_SET_CHAIN || q->op == TCCIR_OP_INIT_CHAIN_SLOT) {
      memset(m->param_mut, 1, m->nparam);
      return;
    }
    int32_t vr = -1;
    if (q->op == TCCIR_OP_LEA)
      vr = irop_get_vreg(tcc_ir_op_get_src1(ir, q));
    else if (irop_config[q->op].has_dest && q->op != TCCIR_OP_NOP)
      vr = irop_get_vreg(tcc_ir_op_get_dest(ir, q));
    if (vr >= 0 && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_PARAM && TCCIR_DECODE_VREG_POSITION(vr) < m->nparam)
      m->param_mut[TCCIR_DECODE_VREG_POSITION(vr)] = 1;
  }
}

int ssa_opt_dup_diamond(IRSSAOptCtx *ctx)
{
  TCCIRState *ir = ctx->ir;
  IRCFG *cfg = ctx->cfg;
  if (!cfg || !ctx->ssa || cfg->num_blocks < 7 || !cfg->instr_to_block)
    return 0;
  DDMatch m = {0};
  m.nparam = ir->next_parameter > 0 ? ir->next_parameter : 0;
  m.param_mut = tcc_mallocz(m.nparam + 1);
  dd_param_scan(ir, &m);
  int changes = 0;
  for (int b = 0; b < cfg->num_blocks; b++) {
    DDiamond d1;
    if (dd_diamond_at(ctx, b, &d1) && dd_try(ctx, &m, &d1)) {
      changes++;
      tcc_ir_ssa_opt_rebuild(ctx);
    }
  }
  tcc_free(m.param_mut);
  return changes;
}
