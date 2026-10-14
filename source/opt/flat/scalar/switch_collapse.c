/*
 *  TCC IR - Collapse a SWITCH_TABLE whose every arm is control-flow equivalent
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#define USING_GLOBALS

#include "ir.h"
#include "opt.h"
#include "opt_engine.h"
#include "opt_utils.h"

/* Follow NOPs and unconditional JUMPs from `start`; return the settled real instr, -1 on cycle/OOB. */
static int sc_resolve_chain(TCCIRState *ir, int start, uint8_t *visited)
{
  int n = ir->next_instruction_index;
  int cur = start;
  while (cur >= 0 && cur < n) {
    if (visited[cur])
      return -1;
    visited[cur] = 1;
    IRQuadCompact *q = &ir->compact_instructions[cur];
    if (q->op == TCCIR_OP_NOP) {
      cur++;
      continue;
    }
    if (q->op == TCCIR_OP_JUMP) {
      cur = (int)tcc_ir_op_dest_imm(ir, q);
      continue;
    }
    return cur;
  }
  return -1;
}

/* Same exit effect: equal index, both RETURNVOID, or both RETURNVALUE with the same IMM32 source. */
static int sc_endpoints_equiv(TCCIRState *ir, int a, int b)
{
  int n = ir->next_instruction_index;
  if (a == b)
    return 1;
  if (a < 0 || a >= n || b < 0 || b >= n)
    return 0;
  IRQuadCompact *qa = &ir->compact_instructions[a];
  IRQuadCompact *qb = &ir->compact_instructions[b];
  if (qa->op != qb->op)
    return 0;
  if (qa->op == TCCIR_OP_RETURNVOID)
    return 1;
  if (qa->op != TCCIR_OP_RETURNVALUE)
    return 0;
  IROperand sa = tcc_ir_op_get_src1(ir, qa);
  IROperand sb = tcc_ir_op_get_src1(ir, qb);
  if (sa.tag != sb.tag)
    return 0;
  if (sa.tag == IROP_TAG_IMM32)
    return sa.u.imm32 == sb.u.imm32;
  return 0;
}

/* All case + default targets resolve to equivalent endpoints -> NOP the dispatch (branch fold cleans up). */
int tcc_ir_opt_switch_collapse(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  if (n < 2 || ir->num_switch_tables == 0)
    return 0;

  int changes = 0;
  uint8_t *visited = tcc_mallocz(n);

  for (int i = 0; i < n; i++) {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_SWITCH_TABLE)
      continue;

    int table_id = (int)tcc_ir_op_src2_imm(ir, q);
    if (table_id < 0 || table_id >= ir->num_switch_tables)
      continue;
    TCCIRSwitchTable *table = &ir->switch_tables[table_id];
    if (!table || table->num_entries <= 0 || !table->targets)
      continue;

    memset(visited, 0, n);
    int common = sc_resolve_chain(ir, table->default_target, visited);
    if (common < 0)
      continue;

    int uniform = 1;
    for (int k = 0; k < table->num_entries; k++) {
      memset(visited, 0, n);
      int r = sc_resolve_chain(ir, table->targets[k], visited);
      if (r < 0 || !sc_endpoints_equiv(ir, r, common)) {
        uniform = 0;
        break;
      }
    }
    if (!uniform)
      continue;

    /* The dispatch is a terminator: dropping it makes control fall through to i + 1.  NOP it only when
     * that resolves to the common endpoint, otherwise branch there explicitly. */
    memset(visited, 0, n);
    int fall = sc_resolve_chain(ir, i + 1, visited);
    if (fall >= 0 && sc_endpoints_equiv(ir, fall, common)) {
      q->op = TCCIR_OP_NOP;
    } else {
      q->op = TCCIR_OP_JUMP;
      tcc_ir_op_set_dest_imm32(ir, q, table->default_target, IROP_BTYPE_INT32);
      tcc_ir_op_set_src1_none(ir, q);
      tcc_ir_op_set_src2_none(ir, q);
    }
    LOG_IR_GEN("switch_collapse: SWITCH_TABLE at %d -> %s (all targets resolve to %d)", i,
               q->op == TCCIR_OP_NOP ? "NOP" : "JUMP", common);
    changes++;
  }

  tcc_free(visited);
  return changes;
}
