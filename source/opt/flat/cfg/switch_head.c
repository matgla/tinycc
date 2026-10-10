/*
 *  TCC IR - Switch dispatch in front of the case bodies
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

/* The frontend meets the cases of a switch only while it parses the body, so
 * it lays the switch out the way one pass allows:
 *
 *      B  : JUMP -> D               the switch
 *           <case bodies>
 *           JUMP -> E               the implicit break
 *      D  : <dispatch>              compares (or a table) to the cases,
 *           [JUMP -> default]       then default, or fall through to E
 *      E  :
 *
 * Every switch costs two jumps it does not need, and the switch value lives
 * across all the bodies to reach the dispatch.  Once the function is complete
 * the dispatch is moved to B, where control goes anyway:
 *
 *      B  : <dispatch>
 *           [JUMP -> default]       or, falling through, JUMP -> E
 *           <case bodies>           the break now falls through to E
 *      E  :
 *
 * Like loop_relayout this is a pure permutation of the records in [B, E):
 * nothing executes in a different order, and every branch into the region is
 * renumbered.  The frontend records (B, D, E) per switch as it closes it,
 * inner switches first, so an inner permutation never moves an outer switch's
 * indices.  A record that no longer describes that shape is left alone.
 */

#define USING_GLOBALS

#include "ir.h"
#include "opt.h"

TCC_DBG_ENV_INT(sh_limit, "TCC_SH_LIMIT", -1)
TCC_DBG_ENV_FLAG(sh_trace, "TCC_SH_TRACE")
static int sh_count;

static int sh_uncond(int op)
{
  switch (op)
  {
  case TCCIR_OP_JUMP:
  case TCCIR_OP_IJUMP:
  case TCCIR_OP_SWITCH_TABLE:
  case TCCIR_OP_RETURNVALUE:
  case TCCIR_OP_RETURNVOID:
  case TCCIR_OP_TRAP:
  case TCCIR_OP_LONGJMP:
  case TCCIR_OP_NL_LONGJMP:
  case TCCIR_OP_BUILTIN_RETURN:
    return 1;
  default:
    return 0;
  }
}

static int sh_target(TCCIRState *ir, int i)
{
  return tcc_ir_op_dest_imm32(ir, &ir->compact_instructions[i]);
}

/* Old index -> new for the region [b, e) with the dispatch at [d, e). */
static int sh_map(int b, int d, int e, int x)
{
  if (x < b || x >= e)
    return x;
  if (x >= d)
    return b + (x - d);
  if (x == b)
    return b; /* the jump to the dispatch: now the dispatch itself */
  return x + (e - d);
}

/* Is the recorded triple still `JUMP -> d; ...; <uncond>; d: ...` with no
 * branch from outside [d, e) into the dispatch past its first instruction? */
static int sh_shape_ok(TCCIRState *ir, int b, int d, int e)
{
  const int n = ir->next_instruction_index;
  if (b < 0 || b + 1 >= d || d >= e || e > n)
    return 0;
  IRQuadCompact *jq = &ir->compact_instructions[b];
  if (jq->op != TCCIR_OP_JUMP || sh_target(ir, b) != d)
    return 0;
  int p = d - 1;
  while (p > b && ir->compact_instructions[p].op == TCCIR_OP_NOP)
    p--;
  if (p <= b || !sh_uncond(ir->compact_instructions[p].op))
    return 0;
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_JUMPIF)
    {
      int t = sh_target(ir, i);
      if (t > d && t < e && (i < d || i >= e))
        return 0;
      if (t == b && i != b)
        return 0;
    }
    else if (q->op == TCCIR_OP_IJUMP)
      return 0;
  }
  for (int t = 0; t < ir->num_switch_tables; t++)
  {
    TCCIRSwitchTable *tbl = &ir->switch_tables[t];
    for (int j = -1; j < tbl->num_entries; j++)
    {
      int x = j < 0 ? tbl->default_target : tbl->targets[j];
      if (x == b || (x > d && x < e))
        return 0; /* a table outside this dispatch reaching into it */
    }
  }
  return 1;
}

int tcc_ir_opt_switch_head(TCCIRState *ir)
{
  if (!ir || ir->switch_heads_n < 3 || ir->func_has_label_addr)
    return 0;
  int moved = 0;
  for (int k = 0; k + 2 < ir->switch_heads_n; k += 3)
  {
    int b = ir->switch_heads[k], d = ir->switch_heads[k + 1], e = ir->switch_heads[k + 2];
    if (!sh_shape_ok(ir, b, d, e))
      continue;
    if (sh_limit() >= 0 && sh_count >= sh_limit())
      continue;
    if (sh_trace())
      fprintf(stderr, "[sh] %d %s %d %d %d\n", sh_count, funcname, b, d, e);
    sh_count++;
    const int n = ir->next_instruction_index;
    int ld = e - d;
    /* Does the dispatch end by falling through to E? */
    int last = e - 1;
    while (last >= d && ir->compact_instructions[last].op == TCCIR_OP_NOP)
      last--;
    int falls = last < d || !sh_uncond(ir->compact_instructions[last].op);

    for (int i = 0; i < n; i++)
    {
      IRQuadCompact *q = &ir->compact_instructions[i];
      if ((q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_JUMPIF) && i != b)
      {
        int t = sh_target(ir, i);
        if (t >= b && t < e)
          tcc_ir_set_dest_imm32(ir, i, sh_map(b, d, e, t), IROP_BTYPE_INT32);
      }
    }
    for (int t = 0; t < ir->num_switch_tables; t++)
    {
      TCCIRSwitchTable *tbl = &ir->switch_tables[t];
      tbl->default_target = sh_map(b, d, e, tbl->default_target);
      for (int j = 0; tbl->targets && j < tbl->num_entries; j++)
        tbl->targets[j] = sh_map(b, d, e, tbl->targets[j]);
    }
    /* The jump at B becomes the dispatch's exit to E, or goes. */
    IRQuadCompact head = ir->compact_instructions[b];
    if (falls)
    {
      tcc_ir_set_dest_imm32(ir, b, e, IROP_BTYPE_INT32);
      head = ir->compact_instructions[b];
    }
    else
      head.op = TCCIR_OP_NOP;
    head.is_jump_target = 0;

    IRQuadCompact *buf = tcc_malloc(sizeof(IRQuadCompact) * (size_t)(e - b));
    int w = 0;
    for (int i = d; i < e; i++)
      buf[w++] = ir->compact_instructions[i];
    buf[w++] = head;
    for (int i = b + 1; i < d; i++)
      buf[w++] = ir->compact_instructions[i];
    memcpy(&ir->compact_instructions[b], buf, sizeof(IRQuadCompact) * (size_t)(e - b));
    tcc_free(buf);
    /* the dispatch's first instruction was entered by the jump alone */
    ir->compact_instructions[b].is_jump_target = 0;
    for (int i = 0; i < n; i++)
    {
      IRQuadCompact *q = &ir->compact_instructions[i];
      if ((q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_JUMPIF) && sh_target(ir, i) == b)
        ir->compact_instructions[b].is_jump_target = 1;
    }
    (void)ld;
    moved++;
  }
  ir->switch_heads_n = 0;
  return moved;
}
