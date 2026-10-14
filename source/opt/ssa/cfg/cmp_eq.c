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

/* b_vr == CMP_EQ_IMM: the fact compares a_vr with the constant imm, a value
 * of btype `btype` (a 32-bit compare against a constant). */
#define CMP_EQ_IMM (-2)

typedef struct CmpFact
{
  int32_t a_vr;
  int32_t b_vr;
  int32_t imm;
  uint8_t btype;
  uint8_t equal;    /* 1 = A==B known, 0 = A!=B known */
} CmpFact;

/* One side of an equality compare: a TEMP vreg, or (b only) a constant. */
typedef struct CmpEqOperands
{
  int32_t a_vr;
  int32_t b_vr; /* CMP_EQ_IMM: compared with imm */
  int32_t imm;
  uint8_t btype;
} CmpEqOperands;

/* Dominator-scoped fact stack, threaded through the domwalk engine's `state`. */
typedef struct CmpEqState
{
  CmpFact *stack;
  int count;
  int cap;
} CmpEqState;

static void fact_push(CmpEqState *st, const CmpEqOperands *o, int eq)
{
  if (st->count >= st->cap) {
    int nc = st->cap ? st->cap * 2 : 32;
    st->stack = tcc_realloc(st->stack, nc * sizeof(CmpFact));
    st->cap = nc;
  }
  st->stack[st->count].a_vr = o->a_vr;
  st->stack[st->count].b_vr = o->b_vr;
  st->stack[st->count].imm = o->imm;
  st->stack[st->count].btype = o->btype;
  st->stack[st->count].equal = (uint8_t)eq;
  st->count++;
}

/* 1 / 0: the dominating facts prove A==B / A!=B; -1: they say nothing.
 * Against constants: x==c answers x==c and (32-bit) every x==k, x!=c only
 * x==c. */
static int fact_lookup(CmpEqState *st, const CmpEqOperands *o)
{
  for (int i = st->count - 1; i >= 0; i--) {
    CmpFact *f = &st->stack[i];
    if (o->b_vr == CMP_EQ_IMM) {
      if (f->b_vr != CMP_EQ_IMM || f->a_vr != o->a_vr || f->btype != o->btype)
        continue;
      if (f->imm == o->imm)
        return f->equal ? 1 : 0;
      /* Distinct constants differ only at full width: a narrow compare may
       * look at the low bits alone (255 and -1 as a byte). */
      if (f->equal && f->btype == IROP_BTYPE_INT32)
        return 0;
      continue;
    }
    if ((f->a_vr == o->a_vr && f->b_vr == o->b_vr) ||
        (f->a_vr == o->b_vr && f->b_vr == o->a_vr))
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

static int extract_eq_imm(IROperand op, int32_t *out)
{
  if (irop_get_tag(op) != IROP_TAG_IMM32 || !irop_is_plain_imm(op) || op.is_local || op.is_llocal)
    return 0;
  *out = op.u.imm32;
  return 1;
}

/* The operands of an equality compare: two TEMPs, or a TEMP and a constant
 * on either side (32-bit only: an I64 constant names a register pair). */
static int extract_eq_operands(TCCIRState *ir, IRQuadCompact *cq, CmpEqOperands *o)
{
  IROperand cs1 = tcc_ir_op_get_src1(ir, cq);
  IROperand cs2 = tcc_ir_op_get_src2(ir, cq);
  if (extract_eq_operand(cs1, &o->a_vr)) {
    if (extract_eq_operand(cs2, &o->b_vr))
      return 1;
    if (irop_is_64bit(cs1) || !extract_eq_imm(cs2, &o->imm))
      return 0;
    o->btype = (uint8_t)cs1.btype;
  } else if (extract_eq_operand(cs2, &o->a_vr)) {
    if (irop_is_64bit(cs2) || !extract_eq_imm(cs1, &o->imm))
      return 0;
    o->btype = (uint8_t)cs2.btype;
  } else {
    return 0;
  }
  o->b_vr = CMP_EQ_IMM;
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

  CmpEqOperands o = {0};
  if (!extract_eq_operands(ir, cq, &o))
    return 0;

  int tok = (int)tcc_ir_op_src1_imm(ir, jq);
  if (tok != TOK_EQ && tok != TOK_NE)
    return 0;

  int target_instr = (int)tcc_ir_op_dest_u_imm32(ir, jq);
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
  fact_push(st, &o, branch_taken_eq);
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

  CmpEqOperands o = {0};
  if (!extract_eq_operands(ir, cq, &o))
    return 0;

  int tok = (int)tcc_ir_op_src1_imm(ir, jq);
  if (tok != TOK_EQ && tok != TOK_NE)
    return 0;

  int known = fact_lookup(st, &o);
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
        int jdst_idx = (int)jdst.u.imm32;   /* target may index past instr_to_block */
        int tgt_block = (jdst_idx >= 0 && jdst_idx < cfg->num_instrs)
                          ? cfg->instr_to_block[jdst_idx] : -1;
        int pred_block = cfg->instr_to_block[jmp_idx];
        int ft_block = cfg->instr_to_block[ft];
        if (ft_block != tgt_block)
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
  IRSSAVregInfo *avi = ssa_opt_vinfo(ctx, o.a_vr);
  if (avi)
    ssa_opt_remove_use_instr(avi, cmp_idx);
  IRSSAVregInfo *bvi = o.b_vr != CMP_EQ_IMM ? ssa_opt_vinfo(ctx, o.b_vr) : NULL;
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
  /* The entry block is its own idom: a lone backedge into it (a one-block
   * loop at function entry) is not the only way in. */
  if (bb->num_preds == 1 && bb->preds[0] == bb->idom && bb->idom != block)
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
