/*
 *  TCC IR - SSA CMP Equality-Fact Propagation
 *
 *  Walks the dominator tree pushing equality facts derived from CMP+JEQ
 *  (and inequality facts from CMP+JNE).  When a later CMP in a dominated
 *  block uses the same operand pair, the fact lets us fold the following
 *  JUMPIF to an unconditional JUMP (always taken) or NOP (never taken).
 *
 *  Targets the goto-chain idiom in gcc.c-torture/compile/961126-1.c, where
 *  after the negation-chain fold leaves only two distinct compare operands
 *  alternating, every iteration past the second is statically resolved.
 *
 *  Only handles equality conditions (EQ/NE) and TEMP operands.  Both
 *  operands must be non-lvalue TEMPs whose values are stable in SSA form.
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

#define TOK_EQ 0x94
#define TOK_NE 0x95

typedef struct CmpFact
{
  int32_t a_vr;
  int32_t b_vr;
  uint8_t equal;    /* 1 = A==B known, 0 = A!=B known */
} CmpFact;

static CmpFact *fact_stack;
static int fact_count;
static int fact_cap;

static void fact_push(int32_t a, int32_t b, int eq)
{
  if (fact_count >= fact_cap) {
    int nc = fact_cap ? fact_cap * 2 : 32;
    fact_stack = tcc_realloc(fact_stack, nc * sizeof(CmpFact));
    fact_cap = nc;
  }
  fact_stack[fact_count].a_vr = a;
  fact_stack[fact_count].b_vr = b;
  fact_stack[fact_count].equal = (uint8_t)eq;
  fact_count++;
}

static void fact_pop_to(int saved)
{
  fact_count = saved;
}

/* Return: 1 if A==B is known, 0 if A!=B is known, -1 if unknown. */
static int fact_lookup(int32_t a, int32_t b)
{
  for (int i = fact_count - 1; i >= 0; i--) {
    CmpFact *f = &fact_stack[i];
    if ((f->a_vr == a && f->b_vr == b) ||
        (f->a_vr == b && f->b_vr == a))
      return f->equal ? 1 : 0;
  }
  return -1;
}

/* Extract a stable equality-eligible operand: must be a non-lval TEMP. */
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

/* Find the last non-NOP instruction index in [start, end). */
static int last_real_in_range(TCCIRState *ir, int start, int end)
{
  for (int j = end - 1; j >= start; j--)
    if (ir->compact_instructions[j].op != TCCIR_OP_NOP)
      return j;
  return -1;
}

/* Try to derive the fact that holds on the edge from `pred_block` into
 * `block` by inspecting the predecessor's terminator.  Returns 1 on
 * success (and pushes a fact) or 0 if no fact can be derived. */
static int try_push_edge_fact(IRSSAOptCtx *ctx, int pred_block, int block)
{
  IRCFG *cfg = ctx->cfg;
  TCCIRState *ir = ctx->ir;
  IRBasicBlock *pred = &cfg->blocks[pred_block];
  (void)block;

  /* Terminator must be a JUMPIF (CMP+JUMPIF pair). */
  int term = last_real_in_range(ir, pred->start_idx, pred->end_idx);
  if (term < 0)
    return 0;
  IRQuadCompact *jq = &ir->compact_instructions[term];
  if (jq->op != TCCIR_OP_JUMPIF)
    return 0;

  /* The CMP must be the previous non-NOP instruction in pred. */
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

  /* Fall-through block: the block containing the instruction immediately
   * after the JUMPIF.  Skip NOPs. */
  int ft = term + 1;
  while (ft < cfg->num_instrs && ir->compact_instructions[ft].op == TCCIR_OP_NOP)
    ft++;
  int ft_block = (ft < cfg->num_instrs) ? cfg->instr_to_block[ft] : -1;

  /* Determine the polarity for `block` and push the matching fact. */
  int is_target = (block == target_block);
  int is_ft = (block == ft_block);
  if (!is_target && !is_ft)
    return 0;
  /* If the JUMPIF could reach `block` via both edges, no fact. */
  if (is_target && is_ft)
    return 0;

  int branch_taken_eq = (tok == TOK_EQ) ? is_target : is_ft;
  fact_push(a_vr, b_vr, branch_taken_eq);
  return 1;
}

/* Try to fold a CMP+JUMPIF at instruction index cmp_idx using active facts.
 * Returns 1 if folded, 0 otherwise. */
static int try_fold_cmp(IRSSAOptCtx *ctx, int cmp_idx, int jmp_idx)
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

  int known = fact_lookup(a_vr, b_vr);
  if (known < 0)
    return 0;

  int branch_taken = (tok == TOK_EQ) ? known : !known;
  IROperand jdst = tcc_ir_op_get_dest(ir, jq);

  if (branch_taken) {
    /* Always taken: NOP the CMP, turn JUMPIF into unconditional JUMP. */
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
    /* Never taken: NOP both CMP and JUMPIF. */
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

static int process_block(IRSSAOptCtx *ctx, int b)
{
  TCCIRState *ir = ctx->ir;
  IRCFG *cfg = ctx->cfg;
  IRBasicBlock *bb = &cfg->blocks[b];
  int saved_count = fact_count;
  int changes = 0;

  /* Push edge fact if this block has a unique predecessor. */
  if (bb->num_preds == 1) {
    try_push_edge_fact(ctx, bb->preds[0], b);
  }

  /* Process CMP+JUMPIF pairs in this block. */
  for (int i = bb->start_idx; i < bb->end_idx - 1; i++) {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_CMP)
      continue;
    int j = i + 1;
    while (j < bb->end_idx && ir->compact_instructions[j].op == TCCIR_OP_NOP)
      j++;
    if (j >= bb->end_idx)
      break;
    IRQuadCompact *nq = &ir->compact_instructions[j];
    if (nq->op != TCCIR_OP_JUMPIF)
      continue;
    if (try_fold_cmp(ctx, i, j))
      changes++;
  }

  /* Recurse into dom children. */
  for (int ci = 0; ci < bb->num_dom_children; ci++)
    changes += process_block(ctx, bb->dom_children[ci]);

  fact_pop_to(saved_count);
  return changes;
}

int ssa_opt_cmp_eq_prop(IRSSAOptCtx *ctx)
{
  IRCFG *cfg = ctx->cfg;
  if (!cfg || cfg->num_blocks == 0)
    return 0;

  fact_count = 0;
  if (!fact_stack) {
    fact_cap = 64;
    fact_stack = tcc_malloc(fact_cap * sizeof(CmpFact));
  }

  int changes = process_block(ctx, 0);

  /* Keep fact_stack allocated across pass invocations; reset on next entry. */
  return changes;
}
