/*
 *  TCC IR - Decrement-to-carry loop rewrite
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#define USING_GLOBALS

#include "ir.h"
#include "licm.h"
#include "opt.h"
#include "opt_du.h"
#include "opt_xform.h"
#include "opt_utils.h"
#include "opt_loop_utils.h"

/* Ops allowed in the latch reschedule window between the counter SUB and the
 * latch CMP: no control flow (the window ops keep their relative order, and a
 * branch target inside it would let a path reach the CMP without running the
 * SUB), and nothing that mentions the counter vreg. */
static int dtc_window_op_ok(TCCIRState *ir, IRQuadCompact *q, int32_t v)
{
  switch (q->op)
  {
  case TCCIR_OP_JUMP:
  case TCCIR_OP_JUMPIF:
  case TCCIR_OP_IJUMP:
  case TCCIR_OP_SWITCH_TABLE:
  case TCCIR_OP_SWITCH_LOAD:
  case TCCIR_OP_RETURNVALUE:
  case TCCIR_OP_RETURNVOID:
  case TCCIR_OP_INLINE_ASM:
  case TCCIR_OP_FUNCCALLVAL:
  case TCCIR_OP_FUNCCALLVOID:
  case TCCIR_OP_FUNCPARAMVAL:
  case TCCIR_OP_FUNCPARAMVOID:
    return 0;
  default:
    break;
  }
  if (q->is_jump_target)
    return 0;
  if (irop_config[q->op].has_dest && tcc_ir_op_dest_vreg(ir, q) == v)
    return 0;
  if (irop_config[q->op].has_src1 && tcc_ir_op_src1_vreg(ir, q) == v)
    return 0;
  if (irop_config[q->op].has_src2 && tcc_ir_op_src2_vreg(ir, q) == v)
    return 0;
  return 1;
}

/* The counter operand as the pattern requires it: a plain (non-deref)
 * 32-bit non-volatile vreg. */
static int dtc_counter_operand_ok(TCCIRState *ir, IROperand op)
{
  return irop_has_vreg(op) && !irop_op_is_lval(op) && op.btype != IROP_BTYPE_INT64 &&
         !tcc_ir_access_is_volatile(ir, op);
}

/* A CFG block whose instructions are all NOPs: the layout padding between a
 * guard/latch and the following block, which the CFG may keep as its own block
 * on the fall-through path. */
static int dtc_block_is_nop_pad(TCCIRState *ir, IRCFG *cfg, int b)
{
  if (b < 0 || b >= cfg->num_blocks)
    return 0;
  for (int i = cfg->blocks[b].start_idx; i < cfg->blocks[b].end_idx; i++)
    if (ir->compact_instructions[i].op != TCCIR_OP_NOP)
      return 0;
  return 1;
}

/* The block `b`, resolved up through NOP-only fall-through padding to the
 * first real block that reaches it; -1 when the chain is not a simple
 * single-predecessor padding chain. */
static int dtc_resolve_through_nop_pad(TCCIRState *ir, IRCFG *cfg, int b)
{
  int hops = 0;
  while (dtc_block_is_nop_pad(ir, cfg, b))
  {
    if (++hops > 4 || cfg->blocks[b].num_preds != 1)
      return -1;
    b = cfg->blocks[b].preds[0];
  }
  return b;
}

/* Counted-down `for (; n >= K; n -= K)` loop -> guard-predecremented form whose
 * latch CMP reads the value the body just used, so the SUBS that computes the
 * next value re-derives identical flags and the codegen CMP skip applies:
 *
 *   CMP V,#K ; JMP exit if "<K"      ->   CMP V,#K ; V <-- V SUB #K ; JMP exit if "<K"
 *   body... ; V <-- V SUB #K ;            body... ; <window ops> ;
 *   CMP V,#K ; JMP body if ">=K"          CMP V,#K ; V <-- V SUB #K ; JMP body if ">=K"
 *
 * Both CMPs now compare the pre-decrement value against K -- exactly the
 * subtraction the flag-setting SUBS performs -- and each guards its JUMPIF.
 * The restructure is iteration-count preserving: the guard tests V0, iteration
 * i's latch tests the counter value the body saw, in both forms the body runs
 * while every test so far held (for any predicate, since the counter only
 * feeds the tests).  The body sees the counter shifted one K lower and every
 * exit leaves it one SUB lower, so the rewrite demands the counter be read
 * nowhere else and defined nowhere inside the loop.
 * Returns 1 if rewritten. */
int dtc_try_region(TCCIRState *ir, IRCFG *cfg, const uint8_t *member,
                   int start, int end, int header_idx)
{
  int n = ir->next_instruction_index;

  /* Latch tail: [NOPs] JUMPIF <loop-cond> back to the header. */
  int j = end;
  while (j >= start && ir->compact_instructions[j].op == TCCIR_OP_NOP)
    j--;
  if (j < start || ir->compact_instructions[j].op != TCCIR_OP_JUMPIF)
    return 0;
  int latch_jmpif = j;

  int guard_cond;
  switch ((int)tcc_ir_op_src1_imm(ir, &ir->compact_instructions[latch_jmpif]))
  {
  case TOK_UGE:
    guard_cond = TOK_ULT;
    break;
  case TOK_UGT:
    guard_cond = TOK_ULE;
    break;
  case TOK_GE:
    guard_cond = TOK_LT;
    break;
  case TOK_GT:
    guard_cond = TOK_LE;
    break;
  default:
    return 0;
  }
  if ((int)tcc_ir_op_dest_imm(ir, &ir->compact_instructions[latch_jmpif]) != header_idx)
    return 0;

  /* Latch CMP: [NOP/ASSIGN moves] CMP V,#K feeding the back-edge. */
  int c = latch_jmpif - 1;
  while (c > start && (ir->compact_instructions[c].op == TCCIR_OP_NOP ||
                       ir->compact_instructions[c].op == TCCIR_OP_ASSIGN))
    c--;
  if (c < start || ir->compact_instructions[c].op != TCCIR_OP_CMP)
    return 0;
  IRQuadCompact *latch_cmp = &ir->compact_instructions[c];
  IROperand cmp_s1 = tcc_ir_op_get_src1(ir, latch_cmp);
  if (!dtc_counter_operand_ok(ir, cmp_s1) || !irop_is_immediate(tcc_ir_op_get_src2(ir, latch_cmp)))
    return 0;
  int32_t v = irop_get_vreg(cmp_s1);
  int64_t k = irop_get_imm64_ex(ir, tcc_ir_op_get_src2(ir, latch_cmp));
  if (k == 0)
    return 0;

  /* Latch SUB: above the CMP, separated only by ops that neither branch,
 * mention V, nor are branched to. */
  int s = -1;
  for (int i = c - 1; i >= start && i >= c - 8; i--)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;
    if (q->op == TCCIR_OP_SUB && tcc_ir_op_dest_vreg(ir, q) == v &&
        tcc_ir_op_src1_vreg(ir, q) == v && tcc_ir_op_src2_is_imm(ir, q) &&
        tcc_ir_op_src2_imm(ir, q) == k)
    {
      s = i;
      break;
    }
    if (!dtc_window_op_ok(ir, q, v))
      return 0;
  }
  if (s < 0)
    return 0;
  IRQuadCompact *latch_sub = &ir->compact_instructions[s];
  if (tcc_ir_op_get_dest(ir, latch_sub).btype == IROP_BTYPE_INT64)
    return 0;

  /* Zero-trip guard: [NOPs] JUMPIF <exit-cond> past the loop, fed by
   * CMP V,#K.  The whole pair sits right before the loop body. */
  int g = start - 1;
  while (g >= 0 && ir->compact_instructions[g].op == TCCIR_OP_NOP)
    g--;
  if (g < 0 || ir->compact_instructions[g].op != TCCIR_OP_JUMPIF)
    return 0;
  int guard_jmpif = g;
  if ((int)tcc_ir_op_src1_imm(ir, &ir->compact_instructions[guard_jmpif]) != guard_cond)
    return 0;
  if ((int)tcc_ir_op_dest_imm(ir, &ir->compact_instructions[guard_jmpif]) <= end)
    return 0;

  g--;
  while (g >= 0 && ir->compact_instructions[g].op == TCCIR_OP_NOP)
    g--;
  if (g < 0 || ir->compact_instructions[g].op != TCCIR_OP_CMP)
    return 0;
  IRQuadCompact *guard_cmp = &ir->compact_instructions[g];
  if (tcc_ir_op_src1_vreg(ir, guard_cmp) != v || !tcc_ir_op_src2_is_imm(ir, guard_cmp) ||
      tcc_ir_op_src2_imm(ir, guard_cmp) != k)
    return 0;
  if (!dtc_counter_operand_ok(ir, tcc_ir_op_get_src1(ir, guard_cmp)))
    return 0;

  /* Single entry: the header's predecessors are exactly the guard's block and
   * the latch's block (NOP padding between blocks resolves to one of them), so
   * every entry runs the guard. */
  {
    int hb = cfg->instr_to_block[header_idx];
    IRBasicBlock *hbb = &cfg->blocks[hb];
    int latch_b = cfg->instr_to_block[latch_jmpif];
    int guard_b = cfg->instr_to_block[guard_jmpif];
    if (hbb->num_preds != 2)
      return 0;
    int latch_seen = 0, entry_pred = -1;
    for (int p = 0; p < hbb->num_preds; p++)
    {
      int pred = hbb->preds[p];
      if (pred == latch_b && !latch_seen)
        latch_seen = 1;
      else
        entry_pred = pred;
    }
    entry_pred = dtc_resolve_through_nop_pad(ir, cfg, entry_pred);
    if (!latch_seen || entry_pred != guard_b || member[entry_pred])
      return 0;
  }

  /* Counter audit.  Both exits (guard and latch) leave V exactly one K below
   * the source loop's exit value, so when nothing after the loop reads V the
   * rewrite is finished as-is.  When something does (memset's counter feeding
   * the next narrower loop), a single `V <-- V ADD #K` at the common exit
   * block restores the source value on both paths — but then the loop may
   * have no other exit (a break would leave the shifted value) and the exit
   * block may not be reachable from anywhere else (the fixup would run
   * spuriously). */
  int reads_after = 0;
  for (int i = end + 1; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;
    if ((irop_config[q->op].has_src1 && tcc_ir_op_src1_vreg(ir, q) == v) ||
        (irop_config[q->op].has_src2 && tcc_ir_op_src2_vreg(ir, q) == v))
    {
      reads_after = 1;
      break;
    }
    if (irop_config[q->op].has_dest && tcc_ir_op_dest_vreg(ir, q) == v &&
        !tcc_ir_op_dest_is_lval(ir, q))
      break; /* redefinition: nothing after reads the loop's exit value */
  }

  int exit_idx = -1; /* fixup insertion point, -1 = no fixup needed */
  if (reads_after)
  {
    /* The common exit: the guard's jump target, which must also be where the
     * latch falls through. */
    exit_idx = (int)tcc_ir_op_dest_imm(ir, &ir->compact_instructions[guard_jmpif]);
    int ft = latch_jmpif + 1;
    while (ft < n && ir->compact_instructions[ft].op == TCCIR_OP_NOP)
      ft++;
    if (ft != exit_idx)
      return 0;
    /* Its only predecessors are the guard's and latch's blocks (a latch's
     * fall-through may cross NOP padding blocks first). */
    {
      int eb = cfg->instr_to_block[exit_idx];
      IRBasicBlock *ebb = &cfg->blocks[eb];
      int guard_b2 = cfg->instr_to_block[guard_jmpif];
      int latch_b2 = cfg->instr_to_block[latch_jmpif];
      if (ebb->num_preds != 2)
        return 0;
      for (int q = 0; q < ebb->num_preds; q++)
      {
        int pred = dtc_resolve_through_nop_pad(ir, cfg, ebb->preds[q]);
        if (pred != guard_b2 && pred != latch_b2)
          return 0;
      }
    }
    /* No other edge leaves the loop: every out-range jump would exit with the
     * shifted counter and no fixup. */
    for (int i = start; i <= end; i++)
    {
      IRQuadCompact *q = &ir->compact_instructions[i];
      if (q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_JUMPIF)
      {
        int t = (int)tcc_ir_op_dest_imm(ir, q);
        if (t < start || t > end)
          return 0;
      }
      else if (q->op == TCCIR_OP_SWITCH_TABLE || q->op == TCCIR_OP_SWITCH_LOAD ||
               q->op == TCCIR_OP_IJUMP)
        return 0;
    }
  }

  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;
    if (irop_config[q->op].has_src1 && tcc_ir_op_src1_vreg(ir, q) == v &&
        !(i == c || i == g || i == s || i > end || i < g))
      return 0;
    if (irop_config[q->op].has_src2 && tcc_ir_op_src2_vreg(ir, q) == v && i <= end)
      return 0;
    if (irop_config[q->op].has_dest && tcc_ir_op_dest_vreg(ir, q) == v &&
        !(i == s || i < g || i > end))
      return 0;
  }

  /* Apply: the old latch SUB is NOPed FIRST (an insert at the guard would
   * shift its index), and the pool is pre-ensured so no insert fails halfway. */
  IROperand sub_dest = tcc_ir_op_get_dest(ir, latch_sub);
  IROperand sub_src1 = tcc_ir_op_get_src1(ir, latch_sub);
  IROperand sub_src2 = tcc_ir_op_get_src2(ir, latch_sub);
  ir->compact_instructions[s].op = TCCIR_OP_NOP;
  tcc_ir_pool_ensure(ir, exit_idx >= 0 ? 9 : 6);
  /* Latch SUB moves after the latch CMP. */
  if (insert_instr_at(ir, c + 1, TCCIR_OP_SUB, sub_dest, sub_src1, sub_src2) < 0)
    return 0;
  /* Guard gains the same pre-decrement between its CMP and JUMPIF. */
  if (insert_instr_at(ir, guard_jmpif, TCCIR_OP_SUB, sub_dest, sub_src1, sub_src2) < 0)
    return 0;
  /* Counter read after the loop: restore the source exit value (+K) at the
   * common exit, before the first reader.  The guard JUMPIF sits one below its
   * SUB insert; its target was kept current by the inserts above, so the fixup
   * goes exactly there — and insert_instr_at's automatic retarget pushed the
   * guard's edge past the new instruction, so repoint it at the fixup to make
   * both exits run it. */
  if (exit_idx >= 0)
  {
    IRQuadCompact *gq = &ir->compact_instructions[guard_jmpif + 1];
    if (gq->op != TCCIR_OP_JUMPIF)
      return 0;
    int fixup_pos = (int)tcc_ir_op_dest_imm(ir, gq);
    if (insert_instr_at(ir, fixup_pos, TCCIR_OP_ADD, sub_dest, sub_src1, sub_src2) < 0)
      return 0;
    gq = &ir->compact_instructions[guard_jmpif + 1]; /* the insert may realloc */
    ir->compact_instructions[fixup_pos].is_jump_target = 1;
    IROperand tgt;
    memset(&tgt, 0, sizeof(tgt));
    tgt.tag = IROP_TAG_IMM32;
    tgt.u.imm32 = fixup_pos;
    tcc_ir_op_set_dest(ir, gq, tgt);
  }
  return 1;
}
