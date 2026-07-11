/*
 *  TCC IR - SSA CMP Equality-Fact Propagation
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
#include "opt_ssa_domwalk.h"
#include "opt/ssa/cmp_eq.h"

#define TOK_EQ 0x94
#define TOK_NE 0x95

typedef struct CmpFact
{
  int32_t a_vr;
  int32_t b_vr;
  uint8_t equal;    /* 1 = A==B known, 0 = A!=B known */
} CmpFact;

/* Dominator-scoped fact stack, threaded through the domwalk engine's `state`. */
typedef struct CmpEqState
{
  CmpFact *stack;
  int count;
  int cap;
} CmpEqState;

static void fact_push(CmpEqState *st, int32_t a, int32_t b, int eq)
{
  if (st->count >= st->cap) {
    int nc = st->cap ? st->cap * 2 : 32;
    st->stack = tcc_realloc(st->stack, nc * sizeof(CmpFact));
    st->cap = nc;
  }
  st->stack[st->count].a_vr = a;
  st->stack[st->count].b_vr = b;
  st->stack[st->count].equal = (uint8_t)eq;
  st->count++;
}

static int fact_lookup(CmpEqState *st, int32_t a, int32_t b)
{
  for (int i = st->count - 1; i >= 0; i--) {
    CmpFact *f = &st->stack[i];
    if ((f->a_vr == a && f->b_vr == b) ||
        (f->a_vr == b && f->b_vr == a))
      return f->equal ? 1 : 0;
  }
  return -1;
}

static int extract_eq_operand(IROperand op, int32_t *out_vr)
{
  if (op.is_lval || op.is_local || op.is_llocal || op.is_sym)
    return 0;
  if (op.tag != IROP_TAG_VREG)
    return 0;
  int32_t vr = irop_get_vreg(op);
  if (vr < 0 || TCCIR_DECODE_VREG_TYPE(vr) != TCCIR_VREG_TYPE_TEMP)
    return 0;
  *out_vr = vr;
  return 1;
}

static int last_real_in_range(TCCIRState *ir, int start, int end)
{
  for (int j = end - 1; j >= start; j--)
    if (ir->compact_instructions[j].op != TCCIR_OP_NOP)
      return j;
  return -1;
}

static int try_push_edge_fact(IRSSAOptCtx *ctx, CmpEqState *st,
                              int pred_block, int block)
{
  IRCFG *cfg = ctx->cfg;
  TCCIRState *ir = ctx->ir;
  IRBasicBlock *pred = &cfg->blocks[pred_block];
  (void)block;

  int term = last_real_in_range(ir, pred->start_idx, pred->end_idx);
  if (term < 0)
    return 0;
  IRQuadCompact *jq = &ir->compact_instructions[term];
  if (jq->op != TCCIR_OP_JUMPIF)
    return 0;

  int cmp_idx = last_real_in_range(ir, pred->start_idx, term);
  if (cmp_idx < 0)
    return 0;
  IRQuadCompact *cq = &ir->compact_instructions[cmp_idx];
  if (cq->op != TCCIR_OP_CMP)
    return 0;

  IROperand cs1 = tcc_ir_op_get_src1(ir, cq);
  IROperand cs2 = tcc_ir_op_get_src2(ir, cq);
  int32_t a_vr, b_vr;
  if (!extract_eq_operand(cs1, &a_vr) || !extract_eq_operand(cs2, &b_vr))
    return 0;

  IROperand cond_op = tcc_ir_op_get_src1(ir, jq);
  int tok = (int)irop_get_imm64_ex(ir, cond_op);
  if (tok != TOK_EQ && tok != TOK_NE)
    return 0;

  IROperand jdst = tcc_ir_op_get_dest(ir, jq);
  int target_instr = (int)jdst.u.imm32;
  if (target_instr < 0 || target_instr >= cfg->num_instrs)
    return 0;
  int target_block = cfg->instr_to_block[target_instr];

  int ft = term + 1;
  while (ft < cfg->num_instrs && ir->compact_instructions[ft].op == TCCIR_OP_NOP)
    ft++;
  int ft_block = (ft < cfg->num_instrs) ? cfg->instr_to_block[ft] : -1;

  int is_target = (block == target_block);
  int is_ft = (block == ft_block);
  if (!is_target && !is_ft)
    return 0;
  /* If the JUMPIF could reach `block` via both edges, no fact. */
  if (is_target && is_ft)
    return 0;

  int branch_taken_eq = (tok == TOK_EQ) ? is_target : is_ft;
  fact_push(st, a_vr, b_vr, branch_taken_eq);
  return 1;
}

static int try_fold_cmp(IRSSAOptCtx *ctx, CmpEqState *st,
                        int cmp_idx, int jmp_idx)
{
  TCCIRState *ir = ctx->ir;
  IRQuadCompact *cq = &ir->compact_instructions[cmp_idx];
  IRQuadCompact *jq = &ir->compact_instructions[jmp_idx];
  if (cq->op != TCCIR_OP_CMP || jq->op != TCCIR_OP_JUMPIF)
    return 0;

  IROperand cs1 = tcc_ir_op_get_src1(ir, cq);
  IROperand cs2 = tcc_ir_op_get_src2(ir, cq);
  int32_t a_vr, b_vr;
  if (!extract_eq_operand(cs1, &a_vr) || !extract_eq_operand(cs2, &b_vr))
    return 0;

  IROperand cond_op = tcc_ir_op_get_src1(ir, jq);
  int tok = (int)irop_get_imm64_ex(ir, cond_op);
  if (tok != TOK_EQ && tok != TOK_NE)
    return 0;

  int known = fact_lookup(st, a_vr, b_vr);
  if (known < 0)
    return 0;

  int branch_taken = (tok == TOK_EQ) ? known : !known;
  IROperand jdst = tcc_ir_op_get_dest(ir, jq);

  if (branch_taken) {
    cq->op = TCCIR_OP_NOP;
    jq->op = TCCIR_OP_JUMP;
    tcc_ir_set_dest(ir, jmp_idx, jdst);
    /* Drop phi edge on the dead fall-through path. */
    IRCFG *cfg = ctx->cfg;
    if (cfg && jmp_idx + 1 < cfg->num_instrs) {
      int ft = jmp_idx + 1;
      while (ft < cfg->num_instrs && ir->compact_instructions[ft].op == TCCIR_OP_NOP)
        ft++;
      if (ft < cfg->num_instrs) {
        int pred_block = cfg->instr_to_block[jmp_idx];
        int ft_block = cfg->instr_to_block[ft];
        if (ft_block != cfg->instr_to_block[(int)jdst.u.imm32])
          ssa_drop_phi_edge(ctx, pred_block, ft_block);
      }
    }
  } else {
    IRCFG *cfg = ctx->cfg;
    int target_block = -1;
    if (cfg) {
      int target_instr = (int)jdst.u.imm32;
      if (target_instr >= 0 && target_instr < cfg->num_instrs)
        target_block = cfg->instr_to_block[target_instr];
    }
    cq->op = TCCIR_OP_NOP;
    jq->op = TCCIR_OP_NOP;
    if (cfg && target_block >= 0) {
      int pred_block = cfg->instr_to_block[jmp_idx];
      ssa_drop_phi_edge(ctx, pred_block, target_block);
    }
  }

  /* Decrement use counts. */
  IRSSAVregInfo *avi = ssa_opt_vinfo(ctx, a_vr);
  if (avi)
    ssa_opt_remove_use_instr(avi, cmp_idx);
  IRSSAVregInfo *bvi = ssa_opt_vinfo(ctx, b_vr);
  if (bvi)
    ssa_opt_remove_use_instr(bvi, cmp_idx);
  return 1;
}

static int cmp_eq_mark(void *state)
{
  return ((CmpEqState *)state)->count;
}

static void cmp_eq_reset(void *state, int watermark)
{
  ((CmpEqState *)state)->count = watermark;
}

static int cmp_eq_enter(IRSSAOptCtx *ctx, int block, void *state)
{
  IRBasicBlock *bb = &ctx->cfg->blocks[block];
  if (bb->num_preds == 1 && bb->preds[0] == bb->idom)
    try_push_edge_fact(ctx, state, bb->preds[0], block);
  return 0;
}

static int cmp_eq_visit(IRSSAOptCtx *ctx, int block, void *state)
{
  TCCIRState *ir = ctx->ir;
  IRBasicBlock *bb = &ctx->cfg->blocks[block];
  int changes = 0;

  for (int i = bb->start_idx; i < bb->end_idx - 1; i++) {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_CMP)
      continue;
    int j = i + 1;
    while (j < bb->end_idx && ir->compact_instructions[j].op == TCCIR_OP_NOP)
      j++;
    if (j >= bb->end_idx)
      break;
    if (ir->compact_instructions[j].op != TCCIR_OP_JUMPIF)
      continue;
    if (try_fold_cmp(ctx, state, i, j))
      changes++;
  }
  return changes;
}

int ssa_opt_cmp_eq_prop(IRSSAOptCtx *ctx)
{
  IRCFG *cfg = ctx->cfg;
  if (!cfg || cfg->num_blocks == 0)
    return 0;

  CmpEqState st = {0};
  OptSSADomWalk walk = {
    .state = &st,
    .mark = cmp_eq_mark,
    .reset = cmp_eq_reset,
    .enter = cmp_eq_enter,
    .visit = cmp_eq_visit,
  };

  int changes = opt_ssa_domwalk(ctx, &walk);
  tcc_free(st.stack);
  return changes;
}
