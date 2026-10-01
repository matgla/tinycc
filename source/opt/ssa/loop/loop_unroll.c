/*
 *  TCC IR - SSA loop: unrolling / constant-trip elimination
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
#include "loop_cand.h"

#define SSA_LOOP_UNROLL_MAX_PASSES 4
#define SSA_UNROLL_MAX_SPAN 256

/* Whether instruction Q names vreg VR in any operand slot. */
static int insn_mentions_vreg(TCCIRState *ir, IRQuadCompact *q, int32_t vr)
{
  if (q->op == TCCIR_OP_NOP)
    return 0;
  if (irop_config[q->op].has_dest && irop_get_vreg(tcc_ir_op_get_dest(ir, q)) == vr)
    return 1;
  if (irop_config[q->op].has_src1 && irop_get_vreg(tcc_ir_op_get_src1(ir, q)) == vr)
    return 1;
  if (irop_config[q->op].has_src2 && irop_get_vreg(tcc_ir_op_get_src2(ir, q)) == vr)
    return 1;
  if ((tcc_ir_op_is_mac(q->op) || q->op == TCCIR_OP_LOAD_INDEXED || q->op == TCCIR_OP_STORE_INDEXED) &&
      irop_get_vreg(ir->iroperand_pool[q->operand_base + 3]) == vr)
    return 1;
  return 0;
}

/* Whether source operand S reads a vreg's own value: a register vreg, or a
 * variable in its stack slot (a STACKOFF lvalue naming it) -- not a load
 * through the pointer it holds. */
static int src_reads_vreg_value(IROperand s)
{
  if (irop_get_vreg(s) < 0 || s.is_llocal)
    return 0;
  int tag = irop_get_tag(s);
  return tag == IROP_TAG_VREG ? !s.is_lval : tag == IROP_TAG_STACKOFF;
}

/* `DST <- SRC` between two plain 32-bit VARs (a VAR read is a LOAD or an ASSIGN). */
static int is_var_copy(TCCIRState *ir, IRQuadCompact *q, int32_t *dst, int32_t *src)
{
  if (q->op != TCCIR_OP_ASSIGN && q->op != TCCIR_OP_LOAD)
    return 0;
  IROperand d = tcc_ir_op_get_dest(ir, q), s = tcc_ir_op_get_src1(ir, q);
  int32_t dv = irop_get_vreg(d), sv = irop_get_vreg(s);
  if (dv < 0 || sv < 0 || TCCIR_DECODE_VREG_TYPE(dv) != TCCIR_VREG_TYPE_VAR ||
      TCCIR_DECODE_VREG_TYPE(sv) != TCCIR_VREG_TYPE_VAR || !irop_dest_defines_vreg(d) || !src_reads_vreg_value(s) ||
      irop_get_btype(d) != IROP_BTYPE_INT32 || irop_get_btype(s) != IROP_BTYPE_INT32)
    return 0;
  *dst = dv;
  *src = sv;
  return 1;
}

static int next_real(TCCIRState *ir, int i, int end)
{
  while (i <= end && ir->compact_instructions[i].op == TCCIR_OP_NOP)
    i++;
  return i;
}

static int prev_real(TCCIRState *ir, int i, int start)
{
  while (i >= start && ir->compact_instructions[i].op == TCCIR_OP_NOP)
    i--;
  return i;
}

/* Q computes a value equal to one of SAME[0..N): a copy of it, or it ANDed
 * with all ones (an immediate or a TEMP whose only def is `#-1`). */
static int cz_value_step(TCCIRState *ir, IRQuadCompact *q, const int32_t *same, int n)
{
  if (q->op != TCCIR_OP_ASSIGN && q->op != TCCIR_OP_LOAD && q->op != TCCIR_OP_STORE && q->op != TCCIR_OP_AND)
    return 0;
  IROperand d = tcc_ir_op_get_dest(ir, q), s = tcc_ir_op_get_src1(ir, q);
  int32_t dv = irop_get_vreg(d);
  if (dv < 0 || !irop_dest_defines_vreg(d) || !src_reads_vreg_value(s) ||
      irop_get_btype(d) != IROP_BTYPE_INT32)
    return 0;
  int in = 0;
  for (int k = 0; k < n; k++)
    in |= irop_get_vreg(s) == same[k];
  if (!in)
    return 0;
  if (q->op != TCCIR_OP_AND)
    return 1;
  IROperand m = tcc_ir_op_get_src2(ir, q);
  if (irop_is_immediate(m))
    return (uint32_t)irop_get_imm64_ex(ir, m) == 0xffffffffu;
  int32_t mv = irop_get_vreg(m);
  if (mv < 0 || TCCIR_DECODE_VREG_TYPE(mv) != TCCIR_VREG_TYPE_TEMP || !src_reads_vreg_value(m))
    return 0;
  int defs = 0, ones = 0;
  for (int i = 0; i < ir->next_instruction_index; i++)
  {
    IRQuadCompact *dq = &ir->compact_instructions[i];
    if (dq->op == TCCIR_OP_NOP || !irop_config[dq->op].has_dest)
      continue;
    IROperand dd = tcc_ir_op_get_dest(ir, dq);
    if (irop_get_vreg(dd) != mv || !irop_dest_defines_vreg(dd))
      continue;
    defs++;
    IROperand ds = tcc_ir_op_get_src1(ir, dq);
    ones = dq->op == TCCIR_OP_ASSIGN && irop_is_immediate(ds) && (uint32_t)irop_get_imm64_ex(ir, ds) == 0xffffffffu;
  }
  return defs == 1 && ones;
}
#define CZ_MAX_SLOTS 12

/* The Zig C backend spells every loop counter with two locals:
 *
 *     B = init;
 *   loop:
 *     A = B;  C = A; ...  if (!(C < n)) goto out;
 *     ... A ...
 *     A = A + step;  B = A;  goto loop;
 *
 * (C and the steps before the test are value copies: an inlined
 * zig_u32_bitCast_u32 binds its parameter and masks it with all ones.)  So A
 * is defined twice in the loop and B never incremented: no induction
 * variable.  When B is mentioned nowhere else and A, and its copies, are not
 * read after the loop, B only carries A round the back edge, and the loop is
 * the classic `A = init; loop: if (!(A < n)) goto out; ...; A += step;`.
 * Rewrite it so: the init defines A, both copies of the carrier go, the exit
 * test reads A and comes first in the loop, where find_loop_exit_condition
 * looks for it.  Returns 1 if rewritten.  Done only on a loop the unroller is
 * about to consider: the parked loop_var_coalesce pass merged such counters
 * in every loop of every function and shifted inline decisions. */
static int canonicalize_zig_counter(TCCIRState *ir, int start, int end)
{
  const int n = ir->next_instruction_index;
  int h = next_real(ir, start, end);
  int32_t a, b;
  if (h > end || !is_var_copy(ir, &ir->compact_instructions[h], &a, &b) || a == b)
    return 0;

  /* Between the copy and the exit test, only steps that pass A's value on
   * unchanged: copies (an inlined zig_u32_bitCast_u32 parameter binds it by
   * `V <- A [STORE]`) and an AND with all ones (its mask).  They stay; the
   * test reads A itself. */
#define CZ_MAX_CHAIN 8
  int32_t same[CZ_MAX_CHAIN + 1];
  int nsame = 0;
  same[nsame++] = a;
  int cmp = next_real(ir, h + 1, end);
  while (cmp <= end && ir->compact_instructions[cmp].op != TCCIR_OP_CMP)
  {
    IRQuadCompact *q = &ir->compact_instructions[cmp];
    if (nsame > CZ_MAX_CHAIN || !cz_value_step(ir, q, same, nsame))
      return 0;
    int32_t d = irop_get_vreg(tcc_ir_op_get_dest(ir, q));
    if (d == a || d == b)
      return 0;
    same[nsame++] = d;
    cmp = next_real(ir, cmp + 1, end);
  }
  if (cmp > end)
    return 0;
  IRQuadCompact *cq = &ir->compact_instructions[cmp];
  IROperand cmp_src = tcc_ir_op_get_src1(ir, cq);
  int cmp_ok = 0;
  for (int k = 0; k < nsame; k++)
    cmp_ok |= irop_get_vreg(cmp_src) == same[k];
  if (!cmp_ok || !src_reads_vreg_value(cmp_src) || !irop_is_immediate(tcc_ir_op_get_src2(ir, cq)))
    return 0;
#undef CZ_MAX_CHAIN
  int jif = next_real(ir, cmp + 1, end);
  if (jif > end || ir->compact_instructions[jif].op != TCCIR_OP_JUMPIF ||
      (int)irop_get_imm64_ex(ir, tcc_ir_op_get_dest(ir, &ir->compact_instructions[jif])) <= end)
    return 0;

  /* Latch: A <- A + #step; B <- A; JUMP header. */
  int back = prev_real(ir, end, start);
  if (back < start || ir->compact_instructions[back].op != TCCIR_OP_JUMP)
    return 0;
  int l1 = prev_real(ir, back - 1, start);
  int32_t l1d, l1s;
  if (l1 <= jif || !is_var_copy(ir, &ir->compact_instructions[l1], &l1d, &l1s) || l1d != b || l1s != a)
    return 0;
  int l0 = prev_real(ir, l1 - 1, start);
  if (l0 <= jif)
    return 0;
  IRQuadCompact *iq = &ir->compact_instructions[l0];
  if (iq->op != TCCIR_OP_ADD || irop_get_vreg(tcc_ir_op_get_dest(ir, iq)) != a ||
      irop_get_vreg(tcc_ir_op_get_src1(ir, iq)) != a || !irop_is_immediate(tcc_ir_op_get_src2(ir, iq)))
    return 0;

  /* Init `B <- #imm` just before the loop (constants may be set up after it). */
  int init = -1;
  for (int i = prev_real(ir, start - 1, 0), k = 0; i >= 0 && k < 6; i = prev_real(ir, i - 1, 0), k++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_JUMPIF || ir->compact_instructions[i].is_jump_target)
      break;
    if (irop_config[q->op].has_dest && irop_get_vreg(tcc_ir_op_get_dest(ir, q)) == b)
    {
      init = i;
      break;
    }
  }
  if (init < 0 || ir->compact_instructions[init].op != TCCIR_OP_ASSIGN)
    return 0;
  IRQuadCompact *initq = &ir->compact_instructions[init];
  IROperand init_d = tcc_ir_op_get_dest(ir, initq);
  if (!irop_dest_defines_vreg(init_d) || !irop_is_immediate(tcc_ir_op_get_src1(ir, initq)))
    return 0;

  /* B: only the init, the header copy and the latch copy.  A: defined in the
   * loop only by the header copy and the increment, never read after it. */
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;
    if (i != init && i != h && i != l1 && insn_mentions_vreg(ir, q, b))
      return 0;
    /* A and the values copied from it keep their exit value, which
     * unrolling does not reproduce, so none may be read after the loop. */
    for (int k = 0; k < nsame; k++)
      if (i > end && insn_mentions_vreg(ir, q, same[k]))
        return 0;
    if (i >= start && i <= end && i != h && i != l0 && irop_config[q->op].has_dest &&
        irop_get_vreg(tcc_ir_op_get_dest(ir, q)) == a)
      return 0;
  }
  /* A jump into the loop other than to its header would skip `A = B`, and
   * one to a step between the copy and the exit test would land in the
   * reordered run below. */
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_JUMP && q->op != TCCIR_OP_JUMPIF)
      continue;
    int t = next_real(ir, (int)irop_get_imm64_ex(ir, tcc_ir_op_get_dest(ir, q)), end);
    if ((i < start || i > end) && t > h && t <= end)
      return 0;
    if (t > h && t <= jif)
      return 0;
  }

  IROperand nd = init_d;
  irop_set_vreg(&nd, a);
  tcc_ir_op_set_dest(ir, initq, nd);
  IROperand ns = cmp_src;
  irop_set_vreg(&ns, a);
  tcc_ir_op_set_src1(ir, cq, ns);
  ir->compact_instructions[h].op = TCCIR_OP_NOP;
  ir->compact_instructions[l1].op = TCCIR_OP_NOP;
  /* A value step spelled `V <- A [STORE]` is a copy between variables: say
   * so, since a STORE-bound VAR reads as a load bound to a parameter, which
   * the unroller must keep as a loop. */
  for (int i = h + 1; i < cmp; i++)
    if (ir->compact_instructions[i].op == TCCIR_OP_STORE)
      ir->compact_instructions[i].op = TCCIR_OP_ASSIGN;
  /* The exit test goes first, the value steps after it (they are dead on the
   * exit path, nothing reads them after the loop): the loop is then the
   * classic top-tested shape whose test find_loop_exit_condition looks for at
   * the header.  Slots keep their jump-target flags; the quads move. */
  {
    int slots[CZ_MAX_SLOTS], ns = 0;
    IRQuadCompact moved[CZ_MAX_SLOTS];
    for (int i = h; i <= jif && ns < CZ_MAX_SLOTS; i++)
      if (ir->compact_instructions[i].op != TCCIR_OP_NOP || i == h)
        slots[ns++] = i;
    if (ns < CZ_MAX_SLOTS)
    {
      int m = 0;
      moved[m++] = ir->compact_instructions[cmp];
      moved[m++] = ir->compact_instructions[jif];
      for (int k = 1; k < ns; k++)
        if (slots[k] != cmp && slots[k] != jif)
          moved[m++] = ir->compact_instructions[slots[k]];
      for (; m < ns; m++)
      {
        moved[m] = ir->compact_instructions[h];
        moved[m].op = TCCIR_OP_NOP;
      }
      for (int k = 0; k < ns; k++)
      {
        IRQuadCompact *q = &ir->compact_instructions[slots[k]];
        int jt = q->is_jump_target;
        *q = moved[k];
        q->is_jump_target = jt;
      }
    }
  }
  LOG_LOOP_OPT("canonicalize_zig_counter: loop [%d..%d] counter VAR%d", start, end, TCCIR_DECODE_VREG_POSITION(a));
  return 1;
}

static int unroll_try_candidate(TCCIRState *ir, IRCFG *cfg, int header_b,
                                uint8_t *member, uint8_t *scratch)
{
  IRBasicBlock *hb = &cfg->blocks[header_b];
  lcs_collect_header_members(cfg, header_b, member, scratch);

  int eff_start = ir->next_instruction_index;
  int eff_end = -1;
  for (int b = 0; b < cfg->num_blocks; b++) {
    if (!member[b])
      continue;
    if (cfg->blocks[b].start_idx < eff_start)
      eff_start = cfg->blocks[b].start_idx;
    if (cfg->blocks[b].end_idx - 1 > eff_end)
      eff_end = cfg->blocks[b].end_idx - 1;
  }
  if (eff_end < eff_start)
    return 0;
  if (eff_end - eff_start > SSA_UNROLL_MAX_SPAN)
    return 0;

  /* Contiguity: every non-NOP in the span must be a member. */
  for (int i = eff_start; i <= eff_end; i++) {
    if (ir->compact_instructions[i].op == TCCIR_OP_NOP)
      continue;
    int b = cfg->instr_to_block[i];
    if (b < 0 || b >= cfg->num_blocks || !member[b])
      return 0;
  }
  /* Single-entry: header must dominate every member. */
  for (int b = 0; b < cfg->num_blocks; b++) {
    if (member[b] && !tcc_ir_cfg_dominates(cfg, header_b, b))
      return 0;
  }

  int preheader = eff_start - 1;
  while (preheader >= 0) {
    int pop = ir->compact_instructions[preheader].op;
    if (pop != TCCIR_OP_JUMP && pop != TCCIR_OP_JUMPIF)
      break;
    preheader--;
  }

  IRLoop loop = {0};
  loop.header_idx = hb->start_idx;
  loop.start_idx = eff_start;
  loop.end_idx = eff_end; /* latch back-edge instruction */
  loop.preheader_idx = preheader;
  loop.depth = 1;

  canonicalize_zig_counter(ir, eff_start, eff_end);

  if (try_eliminate_loop(ir, &loop))
    return 1;
  if (try_eliminate_loop_symbolic(ir, &loop))
    return 1;

  /* try_unroll_loop_ex enables its IR-growth path only when num_loops==1. */
  IRLoops one = {0};
  one.loops = &loop;
  one.num_loops = 1;
  one.capacity = 1;
  return try_unroll_loop_ex(ir, &loop, &one, 0);
}

int ssa_opt_loop_unroll(TCCIRState *ir)
{
  if (!ir || ir->next_instruction_index == 0)
    return 0;
  if (!tcc_ir_cfg_flat_has_backedge(ir))
    return 0;

  int total = 0;
  for (int pass = 0; pass < SSA_LOOP_UNROLL_MAX_PASSES; pass++) {
    IRCFG *cfg = tcc_ir_cfg_build(ir);
    if (!cfg || cfg->num_blocks <= 1) {
      tcc_ir_cfg_free(cfg);
      break;
    }
    tcc_ir_cfg_compute_dominators(cfg);

    int nb = cfg->num_blocks;
    uint8_t *is_header = tcc_mallocz((size_t)nb);
    for (int b = 0; b < nb; b++) {
      IRBasicBlock *bb = &cfg->blocks[b];
      for (int si = 0; si < bb->num_succs; si++) {
        int h = bb->succs[si];
        if (h >= 0 && h < nb && tcc_ir_cfg_dominates(cfg, h, b))
          is_header[h] = 1;
      }
    }

    uint8_t *member = tcc_malloc((size_t)nb);
    uint8_t *scratch = tcc_malloc((size_t)nb);
    uint8_t *other = tcc_malloc((size_t)nb);

    int changed = 0;
    for (int h = 0; h < nb && !changed; h++) {
      if (!is_header[h])
        continue;
      /* Outermost only, for soundness: a nested loop's IV init would be read from the enclosing preheader. */
      int inner = 0;
      for (int h2 = 0; h2 < nb && !inner; h2++) {
        if (h2 == h || !is_header[h2])
          continue;
        lcs_collect_header_members(cfg, h2, other, scratch);
        if (other[h])
          inner = 1;
      }
      if (inner)
        continue;
      changed = unroll_try_candidate(ir, cfg, h, member, scratch);
      if (changed)
        LOG_IR_GEN("[ssa:loop_unroll] transformed header_b=%d", h);
    }

    tcc_free(is_header);
    tcc_free(member);
    tcc_free(scratch);
    tcc_free(other);
    tcc_ir_cfg_free(cfg);

    total += changed;
    if (!changed)
      break;
  }
  return total;
}
