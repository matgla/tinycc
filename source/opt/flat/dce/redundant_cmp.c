/*
 *  TCC IR - Redundant compare elimination (post-RA)
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#define USING_GLOBALS

#include "ir.h"
#include "opt_engine.h"
#include "opt_utils.h"

/* `if (a == b) ... ; if (a < b) ...` -- two tests of one pair, which C source
 * spells out constantly (search loops, three-way compares, strcmp bodies) --
 * compiles to two identical CMPs.  The second recomputes flags the first
 * already left in the APSR, and when an operand is still a dereference it
 * re-reads memory to do it.  gcc emits one `cmp` and hangs both branches off
 * it; this pass deletes the second CMP so the same is true here.
 *
 * Only the shape whose reachability is provable by inspection is taken:
 *
 *     i:   CMP a, b
 *     i+1: JUMPIF cond -> L
 *     ...
 *     L:   CMP a, b          <- deleted; its reader uses the flags from i
 *
 * for L on either of the JUMPIF's two successors.  On the taken edge the
 * requirement is that the JUMPIF is the ONLY way into L: no other branch
 * targets anything in [L, the CMP], and control cannot fall into L from the
 * instruction above it.  On the fall-through edge the requirement is that
 * nothing between the JUMPIF and the CMP is a branch target.  Either way the
 * gap must hold NOPs only, which is what makes the flags, the operand
 * registers and the memory the operands may name all provably unchanged.
 *
 * Placement is after jump threading, deliberately: threading retargets
 * branches, and a branch redirected into L afterwards would arrive carrying
 * somebody else's flags.  It must also follow ra:cmp_imm_swap, which rewrites
 * a compare's operands together with its readers -- and finds those readers by
 * a forward walk that stops at the first JUMPIF, so it must never be handed a
 * compare whose flags are read past one.
 */

/* Bit-identical operands: same vreg/immediate, same lvalue and locality flags,
 * same width and signedness, same access marks.  Nothing weaker is wanted --
 * a CMP is only redundant if it would recompute the very same flags. */
static int rcmp_operand_same(IROperand a, IROperand b)
{
  return a.vr == b.vr && a.u.imm32 == b.u.imm32 && a.is_unsigned == b.is_unsigned &&
         a.is_static == b.is_static && a.is_sym == b.is_sym && a.is_param == b.is_param &&
         a.aux == b.aux;
}

static int rcmp_same_compare(TCCIRState *ir, int a, int b)
{
  IRQuadCompact *qa = &ir->compact_instructions[a];
  IRQuadCompact *qb = &ir->compact_instructions[b];
  if (qa->op != TCCIR_OP_CMP || qb->op != TCCIR_OP_CMP)
    return 0;
  return rcmp_operand_same(tcc_ir_op_get_src1(ir, qa), tcc_ir_op_get_src1(ir, qb)) &&
         rcmp_operand_same(tcc_ir_op_get_src2(ir, qa), tcc_ir_op_get_src2(ir, qb));
}

/* First index at or after `from` holding something other than a NOP. */
static int rcmp_next_real(TCCIRState *ir, int from)
{
  int n = ir->next_instruction_index;
  while (from < n && ir->compact_instructions[from].op == TCCIR_OP_NOP)
    from++;
  return from < n ? from : -1;
}

/* Does control leave for good at `idx`, so the instruction after it is not
 * reachable by fall-through? */
static int rcmp_is_terminator(TccIrOp op)
{
  return op == TCCIR_OP_JUMP || op == TCCIR_OP_IJUMP || op == TCCIR_OP_SWITCH_TABLE ||
         op == TCCIR_OP_RETURNVALUE || op == TCCIR_OP_RETURNVOID || op == TCCIR_OP_TRAP;
}

/* Branch targets, recomputed from the branches actually present.  The
 * IRQuadCompact is_jump_target bit outlives the branch that set it, so it is a
 * superset -- fine as the fallback for a computed jump, whose target list
 * cannot be enumerated, but too coarse to gate on by itself. */
static uint8_t *rcmp_build_entry_map(TCCIRState *ir, int *out_indirect)
{
  int n = ir->next_instruction_index;
  uint8_t *entry = tcc_mallocz((size_t)(n + 7) / 8);
  int indirect = 0;

  for (int j = 0; j < n; j++)
  {
    IRQuadCompact *q = &ir->compact_instructions[j];
    if (q->op == TCCIR_OP_IJUMP || q->op == TCCIR_OP_SWITCH_TABLE)
    {
      indirect = 1;
      continue;
    }
    if (q->op != TCCIR_OP_JUMP && q->op != TCCIR_OP_JUMPIF)
      continue;
    IROperand d = tcc_ir_op_get_dest(ir, q);
    if (irop_is_none(d))
      continue;
    int t = (int)d.u.imm32;
    if (t >= 0 && t < n)
      entry[t / 8] |= (uint8_t)(1 << (t % 8));
  }

  if (indirect)
    for (int j = 0; j < n; j++)
      if (ir->compact_instructions[j].is_jump_target)
        entry[j / 8] |= (uint8_t)(1 << (j % 8));

  *out_indirect = indirect;
  return entry;
}

static inline int rcmp_is_entry(const uint8_t *entry, int idx)
{
  return (entry[idx / 8] >> (idx % 8)) & 1;
}

/* Count the branches that target `idx`, so "reached only by our JUMPIF" can be
 * distinguished from "reached by several branches". */
static int rcmp_target_count(TCCIRState *ir, int idx)
{
  int n = ir->next_instruction_index;
  int count = 0;
  for (int j = 0; j < n; j++)
  {
    IRQuadCompact *q = &ir->compact_instructions[j];
    if (q->op != TCCIR_OP_JUMP && q->op != TCCIR_OP_JUMPIF)
      continue;
    IROperand d = tcc_ir_op_get_dest(ir, q);
    if (!irop_is_none(d) && (int)d.u.imm32 == idx)
      count++;
  }
  return count;
}

int tcc_ir_opt_redundant_cmp(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  if (n < 3)
    return 0;

  int indirect = 0;
  uint8_t *entry = rcmp_build_entry_map(ir, &indirect);
  int changes = 0;

  for (int i = 0; i < n; i++)
  {
    if (ir->compact_instructions[i].op != TCCIR_OP_CMP)
      continue;

    int j = rcmp_next_real(ir, i + 1);
    if (j < 0 || ir->compact_instructions[j].op != TCCIR_OP_JUMPIF)
      continue;
    /* Anything that can be branched to between the compare and its branch
     * would let control reach the branch without the compare. */
    int blocked = 0;
    for (int k = i + 1; k <= j; k++)
      if (rcmp_is_entry(entry, k))
        blocked = 1;
    if (blocked)
      continue;

    IROperand tgt = tcc_ir_op_get_dest(ir, &ir->compact_instructions[j]);
    if (irop_is_none(tgt))
      continue;
    int L = (int)tgt.u.imm32;

    /* Taken edge.  A computed jump anywhere in the function makes the target
     * set unknowable, so refuse the whole shape then. */
    if (!indirect && L > j && L < n)
    {
      int m = rcmp_next_real(ir, L);
      int ok = (m >= 0 && rcmp_target_count(ir, L) == 1);
      /* No other branch may land inside the NOP run we walk over. */
      for (int k = L + 1; ok && k <= m; k++)
        if (rcmp_is_entry(entry, k))
          ok = 0;
      /* Nor may control fall into L from above. */
      if (ok)
      {
        int prev = L - 1;
        while (prev >= 0 && ir->compact_instructions[prev].op == TCCIR_OP_NOP)
          prev--;
        if (prev < 0 || !rcmp_is_terminator(ir->compact_instructions[prev].op))
          ok = 0;
      }
      if (ok && m != i && rcmp_same_compare(ir, i, m))
      {
        LOG_IR_GEN("OPTIMIZE: redundant_cmp at %d (flags from %d, taken edge)", m, i);
        ir->compact_instructions[m].op = TCCIR_OP_NOP;
        changes++;
      }
    }

    /* Fall-through edge: the flags survive to the next real instruction as
     * long as nothing in between can be entered from elsewhere. */
    {
      int m = rcmp_next_real(ir, j + 1);
      int ok = (m >= 0);
      for (int k = j + 1; ok && k <= m; k++)
        if (rcmp_is_entry(entry, k))
          ok = 0;
      if (ok && m != i && rcmp_same_compare(ir, i, m))
      {
        LOG_IR_GEN("OPTIMIZE: redundant_cmp at %d (flags from %d, fall-through)", m, i);
        ir->compact_instructions[m].op = TCCIR_OP_NOP;
        changes++;
      }
    }
  }

  tcc_free(entry);
  return changes;
}
