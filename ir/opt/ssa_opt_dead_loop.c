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

/* Find the CMP+JUMPIF at the loop header and check that the loop is a counted
 * loop with provable trip count ≥ 1.  Pattern:
 *   CMP iv, #BOUND
 *   JUMPIF cond → exit
 * where iv has constant init #INIT in the preheader and cond exits when
 * iv ≥/> BOUND, with INIT < BOUND (or ≤ for >).
 */
static int loop_runs_at_least_once(IRSSAOptCtx *ctx, IRLoop *loop)
{
  TCCIRState *ir = ctx->ir;
  int hi = loop_max_idx(loop);

  /* Walk the header forward to find the controlling CMP. */
  int cmp_idx = -1;
  for (int j = loop->header_idx; j <= hi; j++) {
    int op = ir->compact_instructions[j].op;
    if (op == TCCIR_OP_NOP)
      continue;
    if (op == TCCIR_OP_CMP) {
      cmp_idx = j;
      break;
    }
    /* Anything else before the CMP (e.g. body) means this isn't a top-tested
     * counted loop in the form we recognize. */
    return 0;
  }
  if (cmp_idx < 0)
    return 0;

  IRQuadCompact *cmp = &ir->compact_instructions[cmp_idx];
  IROperand src1 = tcc_ir_op_get_src1(ir, cmp);
  IROperand src2 = tcc_ir_op_get_src2(ir, cmp);

  /* Need: CMP iv, #imm */
  int64_t bound;
  if (irop_is_immediate(src2))
    bound = irop_get_imm64_ex(ir, src2);
  else
    return 0;

  int32_t iv_vr = irop_get_vreg(src1);
  if (iv_vr < 0 || TCCIR_DECODE_VREG_TYPE(iv_vr) != TCCIR_VREG_TYPE_TEMP)
    return 0;

  /* The next non-NOP must be JUMPIF that exits the loop. */
  int j = cmp_idx + 1;
  while (j < ir->next_instruction_index && ir->compact_instructions[j].op == TCCIR_OP_NOP)
    j++;
  if (j >= ir->next_instruction_index)
    return 0;
  IRQuadCompact *jpf = &ir->compact_instructions[j];
  if (jpf->op != TCCIR_OP_JUMPIF)
    return 0;

  IROperand cond = tcc_ir_op_get_src1(ir, jpf);
  int tok = (int)irop_get_imm64_ex(ir, cond);

  /* The IV at the header is the phi's destination.  Look up its phi at the
   * header block and resolve the preheader operand to a constant. */
  int header_block = ctx->cfg ? ctx->cfg->instr_to_block[loop->header_idx] : -1;
  if (header_block < 0)
    return 0;

  int latch_block = ctx->cfg->instr_to_block[loop->end_idx];

  IRPhiNode *iv_phi = NULL;
  for (IRPhiNode *p = ctx->ssa->block_phis[header_block]; p; p = p->next) {
    if (p->dest_vreg == iv_vr) {
      iv_phi = p;
      break;
    }
  }
  if (!iv_phi || iv_phi->num_operands != 2)
    return 0;

  int64_t init_val = 0;
  int got_init = 0;
  for (int oi = 0; oi < iv_phi->num_operands; oi++) {
    if (iv_phi->operands[oi].pred_block == latch_block)
      continue; /* latch operand */
    /* preheader operand */
    int64_t v;
    if (resolve_const_through_copies(ctx, iv_phi->operands[oi].vreg, &v)) {
      init_val = v;
      got_init = 1;
    }
    break;
  }
  if (!got_init)
    return 0;

  /* Conservatively check that the latch operand is iv_phi advancing toward
   * bound: latch_op = ADD(iv_phi, +k) with k>0 (going up to bound), or
   * SUB(iv_phi, +k) with k>0 (going down to bound).  Then trip count > 0
   * iff init_val is on the "loop-runs" side of bound for the exit token. */
  int going_up = 0, going_down = 0;
  for (int oi = 0; oi < iv_phi->num_operands; oi++) {
    if (iv_phi->operands[oi].pred_block != latch_block)
      continue;
    int32_t lvr = iv_phi->operands[oi].vreg;
    if (lvr < 0)
      return 0;
    /* Walk through copies to find the actual computation. */
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
      if ((dq->op == TCCIR_OP_ADD || dq->op == TCCIR_OP_SUB)) {
        IROperand a = tcc_ir_op_get_src1(ir, dq);
        IROperand b = tcc_ir_op_get_src2(ir, dq);
        if (irop_get_vreg(a) != iv_vr || !irop_is_immediate(b))
          return 0;
        int64_t step = irop_get_imm64_ex(ir, b);
        if (step <= 0)
          return 0;
        if (dq->op == TCCIR_OP_ADD)
          going_up = 1;
        else
          going_down = 1;
        break;
      }
      return 0;
    }
    break;
  }
  if (!going_up && !going_down)
    return 0;

  /* Check the exit condition.  Tokens (from ssa_opt_branch.c):
   *   0x9c = <S, 0x9d = >=S, 0x9e = <=S, 0x9f = >S
   *   0x92 = <U, 0x93 = >=U, 0x96 = <=U, 0x97 = >U
   *   0x94 = ==,  0x95 = !=
   * The JUMPIF jumps to "exit" when cond is true. */
  if (going_up) {
    /* Going up to bound; loop exits when iv reaches bound.
     * Common: cond = ">=" → exits when iv ≥ bound.  Trip count > 0 iff init < bound. */
    if (tok == 0x9d || tok == 0x93)
      return init_val < bound;
    if (tok == 0x9f || tok == 0x97)
      return init_val <= bound;
  }
  if (going_down) {
    if (tok == 0x9c || tok == 0x92)
      return init_val > bound;
    if (tok == 0x9e || tok == 0x96)
      return init_val >= bound;
  }
  return 0;
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
    if (!loop_runs_at_least_once(ctx, loop))
      continue;

    total += rewrite_loop_exit_phis(ctx, loop);
    /* After rewriting, the body may have no live-out values. Try to delete it. */
    total += try_kill_loop_body(ctx, loop);
  }

  tcc_ir_free_loops(loops);
  return total;
}
