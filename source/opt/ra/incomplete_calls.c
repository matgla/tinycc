/*
 *  TCC IR - Incomplete-call repair (pre-RA)
 *
 *  Copyright (c) 2025 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#define USING_GLOBALS
#include "ir.h"
#include "regalloc.h"

/* A param dominates its call in live code, so a FUNCCALL can only lose a FUNCPARAMVAL in dead code: DCE stripped the param off a dead fall-through while the call survived via another edge. */
int ra_repair_incomplete_calls(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  int changed = 0;
  for (int i = 0; i < n; i++) {
    IRQuadCompact *cq = &ir->compact_instructions[i];
    if (cq->op != TCCIR_OP_FUNCCALLVAL && cq->op != TCCIR_OP_FUNCCALLVOID) continue;
    IROperand cs2 = tcc_ir_op_get_src2(ir, cq);
    if (irop_is_none(cs2)) continue;
    int call_id = TCCIR_DECODE_CALL_ID((uint32_t)cs2.u.imm32);
    int argc = TCCIR_DECODE_CALL_ARGC((uint32_t)cs2.u.imm32);
    if (argc <= 0) continue;
    int found = 0;
    for (int j = i - 1; j >= 0 && found < argc; j--) {
      IRQuadCompact *pq = &ir->compact_instructions[j];
      if (pq->op != TCCIR_OP_FUNCPARAMVAL) continue;
      IROperand ps2 = tcc_ir_op_get_src2(ir, pq);
      if (irop_is_none(ps2)) continue;
      if (TCCIR_DECODE_CALL_ID((uint32_t)ps2.u.imm32) == call_id) found++;
    }
    if (found >= argc) continue;
    cq->op = TCCIR_OP_NOP;
    for (int j = i - 1; j >= 0; j--) {
      IRQuadCompact *pq = &ir->compact_instructions[j];
      if (pq->op != TCCIR_OP_FUNCPARAMVAL && pq->op != TCCIR_OP_FUNCPARAMVOID) continue;
      IROperand ps2 = tcc_ir_op_get_src2(ir, pq);
      if (irop_is_none(ps2)) continue;
      if (TCCIR_DECODE_CALL_ID((uint32_t)ps2.u.imm32) == call_id) pq->op = TCCIR_OP_NOP;
    }
    changed++;
  }
  return changed;
}
