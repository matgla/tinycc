/*
 *  TCC IR - Sink loop-computed values into the loop's exit block, before RA
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

/*
 * A pure value computed inside a loop and read only in one of the loop's exit
 * blocks -- Zig's `for (list[start..], start..) |e, i| { if (...) return i; }`
 * leaves `i = start + k` at the top of the body with its one read in the
 * early-return block -- costs an instruction every iteration and a register
 * held around the whole loop for a value needed once.  LLVM sinks it into the
 * exit block; so does this pass, on the de-SSA'd IR right before the live
 * intervals are built, so the allocator sees the shrunken live range.
 * docs/bugs/loop-exit-value-computed-every-iteration.md.
 *
 * An instruction I in block E sinks across E's exit edge into block B when:
 *   - I is a pure integer ALU op with a plain vreg dest D (no memory, no call,
 *     no flags dependency), D its only definition in the function, and D != any
 *     source vreg of I;
 *   - every read of D lies in B, and B's only predecessor is E (a single-edge
 *     exit block: no edge splitting, and no path can reach B carrying a
 *     pre-sink value of D that the sunk def would overwrite);
 *   - no instruction between I and E's terminator defines a vreg source of I
 *     (I and the terminator share E, so that straight-line window is the only
 *     path from I to the edge -- the check is exact, not conservative);
 *   - B is not entered under live condition flags (its first real instruction
 *     is not a flags reader): the sunk ALU op may encode flag-setting.
 * Then I is moved to B's head: the value D had at the edge (from I's last
 * execution) equals what the sunk I computes there, because D has no other
 * definition, no source changes between I and the edge, and nothing but E's
 * terminator edge enters B.
 *
 * One sink per CFG rebuild: the insertion shifts indices, so every decision is
 * made on the cfg it was derived from.  Disableable via TCC_DISABLE_PASS=
 * ra:exit_sink; gated -O1+ like its ra: neighbours.
 */

#define USING_GLOBALS

#include "ir.h"
#include "regalloc.h"
#include "memory/unique_ptr.h"

#define RA_EXIT_SINK_MAX_PASSES 16

static int ra_exit_sink_op(TccIrOp op)
{
  switch (op) {
  case TCCIR_OP_ADD:
  case TCCIR_OP_SUB:
  case TCCIR_OP_MUL:
  case TCCIR_OP_AND:
  case TCCIR_OP_OR:
  case TCCIR_OP_XOR:
  case TCCIR_OP_SHL:
  case TCCIR_OP_SHR:
  case TCCIR_OP_SAR:
    return 1;
  default:
    return 0;
  }
}

/* An immediate or a non-dereferenced vreg value: never a memory access. */
static int ra_exit_sink_plain(IROperand o)
{
  if (irop_is_immediate(o))
    return 1;
  return irop_has_vreg(o) && irop_get_tag(o) == IROP_TAG_VREG && !o.is_lval && !o.is_local &&
         !o.is_llocal && !o.is_sym && !o.is_complex;
}

/* Classify instruction idx's relationship to vreg: 1 defines it, 2 reads it. */
static int ra_exit_sink_ref(TCCIRState *ir, int idx, int32_t vreg)
{
  IRQuadCompact *q = &ir->compact_instructions[idx];
  if (q->op == TCCIR_OP_NOP)
    return 0;
  if (irop_config[q->op].has_dest) {
    IROperand d = tcc_ir_op_get_dest(ir, q);
    if (irop_has_vreg(d) && irop_get_vreg(d) == vreg) {
      /* a STORE through the vreg's deref reads it; only the frame-slot spill form defines it */
      if (q->op == TCCIR_OP_STORE && d.is_lval && !(d.is_local && !d.is_llocal))
        return 2;
      return 1;
    }
  }
  if (irop_config[q->op].has_src1) {
    IROperand s = tcc_ir_op_get_src1(ir, q);
    if (irop_has_vreg(s) && irop_get_vreg(s) == vreg)
      return 2;
  }
  if (irop_config[q->op].has_src2) {
    IROperand s = tcc_ir_op_get_src2(ir, q);
    if (irop_has_vreg(s) && irop_get_vreg(s) == vreg)
      return 2;
  }
  if (tcc_ir_op_is_mac(q->op)) {
    IROperand s = tcc_ir_op_get_accum(ir, q);
    if (irop_has_vreg(s) && irop_get_vreg(s) == vreg)
      return 2;
  }
  return 0;
}

static void ra_exit_sink_members(IRCFG *cfg, int header_b, int latch_b, uint8_t *member, int *stack)
{
  int sp = 0;
  memset(member, 0, (size_t)cfg->num_blocks);
  member[header_b] = 1;
  if (latch_b == header_b)
    return;
  member[latch_b] = 1;
  stack[sp++] = latch_b;
  while (sp > 0) {
    IRBasicBlock *bb = &cfg->blocks[stack[--sp]];
    for (int pi = 0; pi < bb->num_preds; pi++) {
      int p = bb->preds[pi];
      if (p >= 0 && p < cfg->num_blocks && !member[p]) {
        member[p] = 1;
        stack[sp++] = p;
      }
    }
  }
}

/* Apply at most one sink; returns 1 when the IR changed. */
static int ra_exit_sink_try(TCCIRState *ir, IRCFG *cfg)
{
  const int nb = cfg->num_blocks;
  unique_ptr(uint8_t) member = tcc_mallocz((size_t)nb);
  unique_ptr(int) stack = tcc_malloc(sizeof(int) * (size_t)nb);

  for (int b = 0; b < nb; b++) {
    IRBasicBlock *bb = &cfg->blocks[b];
    for (int si = 0; si < bb->num_succs; si++) {
      int h = bb->succs[si];
      if (h < 0 || h >= nb || !tcc_ir_cfg_dominates(cfg, h, b))
        continue; /* not a back edge */
      ra_exit_sink_members(cfg, h, b, member, stack);

      for (int e = 0; e < nb; e++) {
        if (!member[e])
          continue;
        IRBasicBlock *eb = &cfg->blocks[e];
        int term_idx = eb->end_idx - 1;
        if (term_idx < eb->start_idx)
          continue;
        TccIrOp term_op = ir->compact_instructions[term_idx].op;
        if (term_op != TCCIR_OP_JUMPIF && term_op != TCCIR_OP_JUMP)
          continue;
        int term_target = (int)tcc_ir_op_dest_imm(ir, &ir->compact_instructions[term_idx]);

        for (int xsi = 0; xsi < eb->num_succs; xsi++) {
          int xb_i = eb->succs[xsi];
          if (member[xb_i])
            continue;
          IRBasicBlock *xb = &cfg->blocks[xb_i];
          if (xb->num_preds != 1)
            continue; /* only E enters; a second entry could carry a stale value */
          int b_start = xb->start_idx;
          /* the edge is either the terminator's target or its fall-through */
          int taken = (term_target == b_start);
          if (!taken && !(term_op == TCCIR_OP_JUMPIF && b_start == term_idx + 1))
            continue;
          /* a sunk flag-setting ALU op must not land before a flags reader */
          int flags_entry = 0;
          for (int k = b_start; k < xb->end_idx; k++) {
            IRQuadCompact *kq = &ir->compact_instructions[k];
            if (kq->op == TCCIR_OP_NOP)
              continue;
            if (ir_op_has(kq->op, IR_HZ_FLAGS_READ))
              flags_entry = 1;
            break;
          }
          if (flags_entry)
            continue;

          for (int i = eb->start_idx; i < term_idx; i++) {
            IRQuadCompact *q = &ir->compact_instructions[i];
            if (!ra_exit_sink_op(q->op))
              continue;
            IROperand d = tcc_ir_op_get_dest(ir, q);
            if (!ra_exit_sink_plain(d) || irop_is_immediate(d))
              continue;
            if (irop_get_btype(d) != IROP_BTYPE_INT32)
              continue;
            IROperand s1 = tcc_ir_op_get_src1(ir, q);
            IROperand s2 = tcc_ir_op_get_src2(ir, q);
            if (!ra_exit_sink_plain(s1))
              continue;
            if (irop_config[q->op].has_src2 && !ra_exit_sink_plain(s2))
              continue;
            int32_t dv = irop_get_vreg(d);
            int32_t sv[2] = {-1, -1};
            if (!irop_is_immediate(s1))
              sv[0] = irop_get_vreg(s1);
            if (irop_config[q->op].has_src2 && !irop_is_immediate(s2))
              sv[1] = irop_get_vreg(s2);
            if (dv == sv[0] || dv == sv[1])
              continue; /* in-place update: the loop body needs every iteration's */

            int n = ir->next_instruction_index;
            int other_def = 0, uses = 0, use_outside = 0;
            for (int j = 0; j < n && !other_def; j++) {
              int r = ra_exit_sink_ref(ir, j, dv);
              if (r == 1) {
                if (j != i)
                  other_def = 1;
              } else if (r == 2) {
                uses++;
                if (j < b_start || j >= xb->end_idx)
                  use_outside = 1;
              }
            }
            if (other_def || uses == 0 || use_outside)
              continue;

            int src_killed = 0;
            for (int k = i + 1; k < term_idx && !src_killed; k++) {
              IRQuadCompact *kq = &ir->compact_instructions[k];
              if (kq->op == TCCIR_OP_NOP)
                continue;
              if (ir_op_has(kq->op, IR_HZ_ASM)) {
                src_killed = 1; /* inline asm may clobber anything it does not name */
                break;
              }
              if (sv[0] >= 0 && ra_exit_sink_ref(ir, k, sv[0]) == 1)
                src_killed = 1;
              if (sv[1] >= 0 && ra_exit_sink_ref(ir, k, sv[1]) == 1)
                src_killed = 1;
            }
            if (src_killed)
              continue;

            IRQuadCompact moved = *q;
            moved.is_jump_target = 0;
            moved.align_target = 0;
            tcc_ir_insert_instruction_before(ir, b_start, &moved);
            /* the insertion shifts E only when B is laid out before it */
            int shift = (b_start <= term_idx) ? 1 : 0;
            if (taken) {
              /* the entering branch was retargeted past the sunk instruction */
              tcc_ir_op_set_dest(ir, &ir->compact_instructions[term_idx + shift],
                                 irop_make_imm32(-1, b_start, IROP_BTYPE_INT32));
              ir->compact_instructions[b_start].is_jump_target = 1;
              ir->compact_instructions[b_start + 1].is_jump_target = 0;
            }
            ir->compact_instructions[i + shift].op = TCCIR_OP_NOP;
            return 1;
          }
        }
      }
    }
  }
  return 0;
}

int ra_exit_sink(TCCIRState *ir)
{
  if (!ir || ir->next_instruction_index == 0)
    return 0;
  if (!tcc_ir_cfg_flat_has_backedge(ir))
    return 0;

  int total = 0;
  for (int pass = 0; pass < RA_EXIT_SINK_MAX_PASSES; pass++) {
    unique_ptr(IRCFG) cfg = tcc_ir_cfg_build(ir);
    if (!cfg || cfg->num_blocks <= 1)
      break;
    tcc_ir_cfg_compute_dominators(cfg);
    if (!ra_exit_sink_try(ir, cfg))
      break;
    total++;
  }
  return total;
}
