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

/* Orphan CMP elimination — NOP CMP/TEST_ZERO (and FUNCCALLVOID to flag-setting
 * soft-float compare helpers) whose flag result is not consumed by a SETIF or
 * JUMPIF before the next flag-clobbering op or basic-block boundary.  Post-RA
 * cleanup (no SSA analog; out of scope for the flat->SSA retirement): SETIF/
 * JUMPIF consumers folded away by earlier passes / out-of-SSA leave orphan flag
 * setters that plain DCE keeps (they have no dest vreg).  Flags propagate across
 * unconditional JUMPs, so we follow them (visited bitmap bounds the work) but
 * stop at JUMPIF, which consumes our flags. */
static int orphan_cmp_scan(TCCIRState *ir, int from_idx, uint8_t *visited)
{
  int n = ir->next_instruction_index;
  int j = from_idx;
  while (j < n)
  {
    if (visited[j / 8] & (1 << (j % 8)))
      return 0;
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
      IROperand dest = tcc_ir_op_get_dest(ir, nq);
      int target = (int)dest.u.imm32;
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

int tcc_ir_opt_orphan_cmp_elim(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  if (n == 0)
    return 0;

  int changes = 0;
  int bytes = (n + 7) / 8;
  uint8_t *visited = tcc_mallocz(bytes);
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    int is_flag_cmp_call = 0;

    if (q->op == TCCIR_OP_FUNCCALLVOID)
    {
      Sym *callee = irop_get_sym_ex(ir, tcc_ir_op_get_src1(ir, q));
      const char *name = callee ? get_tok_str(callee->v, NULL) : NULL;
      if (!ir_opt_is_flag_cmp_helper_name(name))
        continue;
      is_flag_cmp_call = 1;
    }
    else if (q->op != TCCIR_OP_CMP && q->op != TCCIR_OP_TEST_ZERO)
      continue;

    if (q->is_jump_target)
      continue;

    for (int b = 0; b < bytes; b++)
      visited[b] = 0;

    if (orphan_cmp_scan(ir, i + 1, visited))
    {
      if (is_flag_cmp_call)
        ir_opt_nop_call_params(ir, i);
      q->op = TCCIR_OP_NOP;
      changes++;
    }
  }
  tcc_free(visited);
  return changes;
}
