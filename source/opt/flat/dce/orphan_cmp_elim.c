/*
 *  TCC IR - Orphan CMP elimination (post-RA dead flag-setter cleanup)
 *
 *  Copyright (c) 2025 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#define USING_GLOBALS

#include "ir.h"
#include "opt.h"
#include "opt_engine.h"
#include "opt_xform.h"
#include "opt_alias.h"
#include "opt_utils.h"
#include "opt_du.h"
#include "opt_loop_utils.h"
#include "cfg.h"
#include "licm.h"

/* Follow unconditional jumps; a flag consumer, clobber, or cycle ends the scan. */
static int orphan_cmp_scan(TCCIRState *ir, int from_idx, uint8_t *visited, int *dirty, int *ndirty)
{
  int n = ir->next_instruction_index;
  int j = from_idx;
  while (j < n)
  {
    if (visited[j / 8] & (1 << (j % 8)))
      return 0;
    if (!visited[j / 8])
      dirty[(*ndirty)++] = j / 8;
    visited[j / 8] |= (1 << (j % 8));

    IRQuadCompact *nq = &ir->compact_instructions[j];
    if (nq->op == TCCIR_OP_NOP)
    {
      j++;
      continue;
    }

    switch (nq->op)
    {
    case TCCIR_OP_SETIF:
    case TCCIR_OP_JUMPIF:
    case TCCIR_OP_SELECT:
      return 0;
    case TCCIR_OP_JUMP:
    {
      int target = (int)tcc_ir_op_dest_u_imm32(ir, nq);
      if (target < 0)
        return 0;
      if (target >= n)
        return 1;
      j = target;
      continue;
    }
    case TCCIR_OP_CMP:
    case TCCIR_OP_TEST_ZERO:
    case TCCIR_OP_RETURNVALUE:
    case TCCIR_OP_RETURNVOID:
    case TCCIR_OP_TRAP:
    case TCCIR_OP_IJUMP:
    case TCCIR_OP_SWITCH_TABLE:
    case TCCIR_OP_FUNCCALLVAL:
    case TCCIR_OP_FUNCCALLVOID:
      return 1;
    default:
      break;
    }
    j++;
  }
  return 1;
}

int tcc_ir_opt_orphan_cmp_elim_ex(TCCIRState *ir, void (*nop)(void *, int), void *opaque)
{
  int n = ir->next_instruction_index;
  if (n == 0)
    return 0;

  int changes = 0;
  int bytes = (n + 7) / 8;
  uint8_t *visited = tcc_mallocz(bytes);
  int *dirty = NULL; /* at most one entry per bitmap byte; allocated on first scan */
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    int is_flag_cmp_call = 0;

    if (q->op == TCCIR_OP_FUNCCALLVOID)
    {
      Sym *callee = tcc_ir_op_src1_sym(ir, q);
      const char *name = callee ? get_tok_str(callee->v, NULL) : NULL;
      if (nop || !ir_opt_is_flag_cmp_helper_name(name))
        continue;
      is_flag_cmp_call = 1;
    }
    else if (q->op != TCCIR_OP_CMP && q->op != TCCIR_OP_TEST_ZERO)
      continue;

    /* Only helper calls can lose reachable parameter setup when a target is removed. */
    if (q->is_jump_target && (is_flag_cmp_call || tcc_ir_opt_pass_disabled("orphan_cmp_target")))
      continue;

    /* Dead flags can still require volatile operand reads. */
    if (tcc_ir_instr_access_is_volatile(ir, q))
      continue;

    if (!dirty)
      dirty = tcc_malloc(sizeof(int) * bytes);
    int ndirty = 0;
    int orphan = orphan_cmp_scan(ir, i + 1, visited, dirty, &ndirty);
    for (int k = 0; k < ndirty; k++)
      visited[dirty[k]] = 0;

    if (orphan)
    {
      if (is_flag_cmp_call)
        ir_opt_nop_call_params(ir, i);
      if (nop)
        nop(opaque, i);
      else
        q->op = TCCIR_OP_NOP;
      changes++;
    }
  }
  tcc_free(dirty);
  tcc_free(visited);
  return changes;
}

int tcc_ir_opt_orphan_cmp_elim(TCCIRState *ir)
{
  return tcc_ir_opt_orphan_cmp_elim_ex(ir, NULL, NULL);
}
