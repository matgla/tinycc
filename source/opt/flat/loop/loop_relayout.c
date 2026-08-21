/*
 *  TCC IR - Loop body relayout (trampoline removal)
 *
 *  Copyright (c) 2025 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

/* The frontend lays a `for` loop out with the increment ("latch") physically
 * between the header test and the body:
 *
 *      H  : CMP iv, bound
 *      H+1: JUMPIF cond -> EXIT
 *      H+2: JUMP -> BODY            <- forward trampoline
 *      L  : <increment>
 *      K  : JUMP -> H               <- back edge
 *      B  : <body>
 *      E  : JUMP -> L               <- back to the increment
 *      EXIT:
 *
 * Two unconditional jumps per iteration exist only to bridge that split.
 * Permuting the region into `body, increment, back-edge` removes both:
 *
 *      H  : CMP iv, bound
 *      H+1: JUMPIF cond -> EXIT
 *      H+2: <body>
 *         : <increment>
 *         : JUMP -> H
 *      EXIT:
 *
 * Unlike loop rotation this is a pure *placement* change: no test is
 * duplicated, no condition inverted, no instruction rewritten.  The dynamic
 * sequence of executed operations is identical minus the two jumps, which is
 * why it needs none of rotation's semantic gates (calls, indexed memory and
 * pointer lvalues in the body are all fine here).
 *
 * The header need not be a single `CMP / JUMPIF`.  A short-circuit condition
 * (`while (x && i < 32)`) emits one test/JUMPIF pair per conjunct, so the
 * trampoline is found by scanning the header's fall-through chain rather than
 * assumed at hi + 2.  Everything ahead of the trampoline keeps its index and
 * is never permuted, so only an unconditional transfer inside that chain, or
 * a test branching into the middle of the region, disqualifies the loop.
 *
 * The rewrite permutes IRQuadCompact records in place.  Each record carries
 * its own operand_base, so moving the record moves its operands with it and
 * the operand pool is never touched.  Every old index in the permuted region
 * has an exact new index, so jump targets are remapped rather than rejected.
 */

#define USING_GLOBALS

#include "ir.h"
#include "licm.h"
#include "opt.h"
#include "opt_utils.h"
#include "opt_loop_utils.h"

/* Old index -> new index over the permuted region [hi+2, region_end].
 * Indices outside the region map to themselves. */
typedef struct RelayoutMap
{
  int region_start; /* the header's body trampoline */
  int region_end;   /* body_end_jmp */
  int body_start;
  int body_end; /* last body instruction, excludes the body->latch JUMP */
  int latch_start;
  int eff_latch_start;
  int latch_end;
  int backedge_idx;
  int new_body_start;
  int new_latch_start;
  int new_backedge;
} RelayoutMap;

static int relayout_map(const RelayoutMap *m, int old)
{
  if (old < m->region_start || old > m->region_end)
    return old;
  if (old >= m->body_start && old <= m->body_end)
    return m->new_body_start + (old - m->body_start);
  if (old >= m->eff_latch_start && old <= m->latch_end)
    return m->new_latch_start + (old - m->eff_latch_start);
  if (old == m->backedge_idx)
    return m->new_backedge;
  /* the body->latch JUMP and the latch's skipped leading NOPs both stand for
   * "control continues at the increment" */
  if (old == m->region_end || (old >= m->latch_start && old < m->eff_latch_start))
    return m->new_latch_start;
  /* the header's body trampoline, and the dead gap after the back edge, both
   * stand for "control continues at the body" */
  return m->new_body_start;
}

static void relayout_retarget(TCCIRState *ir, const RelayoutMap *m)
{
  int n = ir->next_instruction_index;

  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_JUMP && q->op != TCCIR_OP_JUMPIF)
      continue;
    IROperand *dest = &ir->iroperand_pool[q->operand_base];
    int old_target = dest->u.imm32;
    if (old_target < m->region_start || old_target > m->region_end)
      continue;
    dest->u.imm32 = relayout_map(m, old_target);
  }

  for (int t = 0; t < ir->num_switch_tables; t++)
  {
    TCCIRSwitchTable *table = &ir->switch_tables[t];
    table->default_target = relayout_map(m, table->default_target);
    for (int j = 0; table->targets && j < table->num_entries; j++)
      table->targets[j] = relayout_map(m, table->targets[j]);
  }

  /* Recompute is_jump_target over the permuted region: the two elided jumps
   * may have carried the flag, and the body's first instruction no longer
   * needs it unless a surviving branch still targets it. */
  for (int i = m->region_start; i <= m->region_end; i++)
    ir->compact_instructions[i].is_jump_target = 0;
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_JUMP && q->op != TCCIR_OP_JUMPIF)
      continue;
    int target = ir->iroperand_pool[q->operand_base].u.imm32;
    if (target >= m->region_start && target <= m->region_end)
      ir->compact_instructions[target].is_jump_target = 1;
  }
  for (int t = 0; t < ir->num_switch_tables; t++)
  {
    TCCIRSwitchTable *table = &ir->switch_tables[t];
    if (table->default_target >= m->region_start && table->default_target <= m->region_end)
      ir->compact_instructions[table->default_target].is_jump_target = 1;
    for (int j = 0; table->targets && j < table->num_entries; j++)
      if (table->targets[j] >= m->region_start && table->targets[j] <= m->region_end)
        ir->compact_instructions[table->targets[j]].is_jump_target = 1;
  }
}

#define RJ(why)                                                                                                       \
  do                                                                                                                   \
  {                                                                                                                    \
    LOG_LOOP_OPT("Relayout: reject hi=%d - %s", hi, why);                                                              \
    return 0;                                                                                                          \
  } while (0)

/* Returns 1 when the loop was relayouted, 0 when its shape does not match. */
int try_relayout_loop(TCCIRState *ir, IRLoop *loop)
{
  int hi = loop->header_idx;
  int n = ir->next_instruction_index;

  if (hi + 2 > loop->end_idx)
    return 0;

  /* The header is a fall-through chain of tests ending in the body
   * trampoline.  A single `CMP / JUMPIF` is the common case, but a
   * short-circuit condition (`while (x && i < 32)`) emits one test/JUMPIF
   * pair per conjunct, so scan for the trampoline rather than assuming it
   * sits at hi + 2.  Everything ahead of the trampoline keeps its index and
   * is never permuted; all that matters is that control still reaches the
   * trampoline by fall-through, so only an unconditional transfer in the
   * chain disqualifies it. */
  int tramp_idx = -1;
  int num_tests = 0;
  for (int i = hi; i <= loop->end_idx; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_JUMP)
    {
      tramp_idx = i;
      break;
    }
    if (q->op == TCCIR_OP_JUMPIF)
    {
      num_tests++;
      continue;
    }
    if (q->op == TCCIR_OP_IJUMP || q->op == TCCIR_OP_RETURNVOID || q->op == TCCIR_OP_RETURNVALUE)
      RJ("header-transfer");
  }
  if (tramp_idx < 0 || num_tests == 0 || tramp_idx <= hi || tramp_idx + 2 > loop->end_idx)
    RJ("header-shape");

  IRQuadCompact *jmp_q = &ir->compact_instructions[tramp_idx];
  int body_start = (int)irop_get_imm64_ex(ir, tcc_ir_op_get_dest(ir, jmp_q));

  /* back edge: first JUMP to the header below the trampoline */
  int backedge_idx = -1;
  for (int i = tramp_idx + 1; i <= loop->end_idx; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_JUMP && (int)irop_get_imm64_ex(ir, tcc_ir_op_get_dest(ir, q)) == hi)
    {
      backedge_idx = i;
      break;
    }
  }
  if (backedge_idx < 0)
    RJ("no-backedge");

  int latch_start = tramp_idx + 1;
  int latch_end = backedge_idx - 1;
  if (latch_end < latch_start)
    RJ("empty-latch");

  /* body must follow the back edge immediately (modulo dead NOPs): anything
   * live in the gap would be destroyed by the permutation */
  if (body_start <= backedge_idx || body_start >= n)
    RJ("body-not-after-backedge");
  for (int i = backedge_idx + 1; i < body_start; i++)
    if (ir->compact_instructions[i].op != TCCIR_OP_NOP)
      RJ("live-gap");

  /* the body ends with an explicit JUMP back into the increment */
  int body_end_jmp = -1;
  int eff_latch_start = -1;
  for (int i = body_start; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_JUMP)
      continue;
    int jt = (int)irop_get_imm64_ex(ir, tcc_ir_op_get_dest(ir, q));
    if (jt >= latch_start && jt <= latch_end)
    {
      body_end_jmp = i;
      eff_latch_start = jt;
      break;
    }
    if (jt == backedge_idx)
      RJ("body-jumps-to-backedge");
  }
  if (body_end_jmp < 0)
    RJ("no-body-latch-jump");

  int region_start = tramp_idx;
  int region_end = body_end_jmp;
  int body_end = body_end_jmp - 1;
  int body_count = body_end - body_start + 1;
  int eff_latch_count = latch_end - eff_latch_start + 1;
  if (body_count <= 0 || eff_latch_count <= 0)
    RJ("degenerate-counts");

  /* every header exit, and everything reachable after it, must lie outside
   * the region: an exit into the middle of it is a shape this pass does not
   * model.  A header test that branches to the body itself is fine -- that
   * target is remapped like any other. */
  for (int i = hi; i < tramp_idx; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_JUMPIF)
      continue;
    int t = (int)irop_get_imm64_ex(ir, tcc_ir_op_get_dest(ir, q));
    if (t >= region_start && t <= region_end && t != body_start)
      RJ("exit-inside-region");
  }

  /* an IJUMP anywhere in the region has un-enumerable targets that cannot be
   * remapped */
  for (int i = region_start; i <= region_end; i++)
    if (ir->compact_instructions[i].op == TCCIR_OP_IJUMP)
      RJ("ijump");

  int needed = body_count + eff_latch_count + 1; /* + the back-edge JUMP */
  int avail = region_end - region_start + 1;
  if (needed > avail)
    RJ("no-room");

  RelayoutMap m;
  m.region_start = region_start;
  m.region_end = region_end;
  m.body_start = body_start;
  m.body_end = body_end;
  m.latch_start = latch_start;
  m.eff_latch_start = eff_latch_start;
  m.latch_end = latch_end;
  m.backedge_idx = backedge_idx;
  m.new_body_start = region_start;
  m.new_latch_start = region_start + body_count;
  m.new_backedge = m.new_latch_start + eff_latch_count;

  /* Permute the records.  Copy first, then write back: the source and
   * destination ranges overlap. */
  IRQuadCompact *buf = tcc_malloc(sizeof(IRQuadCompact) * (size_t)needed);
  int w = 0;
  for (int b = 0; b < body_count; b++)
    buf[w++] = ir->compact_instructions[body_start + b];
  for (int l = 0; l < eff_latch_count; l++)
    buf[w++] = ir->compact_instructions[eff_latch_start + l];
  buf[w++] = ir->compact_instructions[backedge_idx];

  for (int i = region_start; i <= region_end; i++)
  {
    ir->compact_instructions[i].op = TCCIR_OP_NOP;
    ir->compact_instructions[i].is_jump_target = 0;
  }
  for (int i = 0; i < needed; i++)
    ir->compact_instructions[region_start + i] = buf[i];
  tcc_free(buf);

  relayout_retarget(ir, &m);

  LOG_IR_GEN("[LOOP-RELAYOUT] header=%d body=[%d..%d] latch=[%d..%d] -> body at %d, back-edge at %d", hi, body_start,
             body_end, eff_latch_start, latch_end, m.new_body_start, m.new_backedge);
  return 1;
}
