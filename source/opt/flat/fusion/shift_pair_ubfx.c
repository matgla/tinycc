/*
 *  TCC IR - Two-shift extract to UBFX / SBFX (runs just before codegen)
 *
 *  Copyright (c) 2025 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#define USING_GLOBALS

#include "ir.h"
#include "opt_engine.h"
#include "opt_du.h"
#include "opt_loop_utils.h"
#include "opt_alias.h"
#include "opt_utils.h"


/* ============================================================================
 * Two-shift extract → UBFX / SBFX  (tcc_ir_opt_shift_pair_to_ubfx)
 * ============================================================================
 *
 * The canonical unsigned bitfield extract `(x << a) >> b` (b >= a, both
 * logical) isolates the (32-b)-bit field at bit offset (b-a) of x.  ARM
 * Thumb-2 does this in one instruction: `UBFX Rd, Rx, #(b-a), #(32-b)`.
 * The signed analog `(x << a) >> b` with an *arithmetic* outer shift (SAR)
 * sign-extends the same field and lowers to `SBFX Rd, Rx, #(b-a), #(32-b)`.
 *
 * MUST run AFTER tcc_ir_barrel_shift_fusion: that pass folds a single-use shift
 * into its consuming ALU op (ADD/SUB/AND/OR/XOR/CMP) for free via the barrel
 * shifter and NOPs the shift.  So a SHL+SHR pair that SURVIVES as real ops was
 * NOT foldable — its SHR feeds something that can't take a shifted operand (a
 * store, multiply, call arg, return value, or a value used more than once).
 * There the pair costs two instructions (`lsls`+`lsrs`) and UBFX is one — a
 * strict win.  A pair the barrel pass DID fold no longer has a real SHR for us
 * to match, so we never undo that (equal-cost) fusion and never grow code.
 *
 * Gate — each clause keeps the rewrite provably non-increasing:
 *   - inner is SHL #a, outer is SHR #b (UBFX) or SAR #b (SBFX), 1<=a<=b<=31,
 *     both 32-bit (the 64-bit shift-extract idiom is handled by
 *     shift64_dead_half);
 *   - the SHL result is single-use (only the outer shift), so NOPing the SHL
 *     drops exactly one instruction;
 *   - the SHL source is a plain (non-lval) register value, not redefined
 *     between the SHL and the outer shift (the BFX reads it at the outer
 *     shift's position) and with no control-flow edge between the two (same
 *     basic block).
 */
int tcc_ir_opt_shift_pair_to_ubfx(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  int changes = 0;

  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *shr_q = &ir->compact_instructions[i];
    if (shr_q->op != TCCIR_OP_SHR && shr_q->op != TCCIR_OP_SAR)
      continue;
    int is_signed = (shr_q->op == TCCIR_OP_SAR);
    if (tcc_ir_op_get_dest(ir, shr_q).btype == IROP_BTYPE_INT64)
      continue;
    IROperand shr_n = tcc_ir_op_get_src2(ir, shr_q);
    if (!irop_is_immediate(shr_n) || shr_n.is_sym)
      continue;
    int b = (int)irop_get_imm64_ex(ir, shr_n);
    if (b < 1 || b > 31)
      continue;

    IROperand shr_src1 = tcc_ir_op_get_src1(ir, shr_q);
    if (shr_src1.is_lval || !irop_has_vreg(shr_src1))
      continue;
    int32_t t1 = irop_get_vreg(shr_src1);
    if (t1 < 0 || TCCIR_DECODE_VREG_TYPE(t1) != TCCIR_VREG_TYPE_TEMP)
      continue;

    int shl_idx = tcc_ir_find_defining_instruction(ir, t1, i);
    if (shl_idx < 0)
      continue;
    IRQuadCompact *shl_q = &ir->compact_instructions[shl_idx];
    if (shl_q->op != TCCIR_OP_SHL)
      continue;
    if (tcc_ir_op_get_dest(ir, shl_q).btype == IROP_BTYPE_INT64)
      continue;
    IROperand shl_n = tcc_ir_op_get_src2(ir, shl_q);
    if (!irop_is_immediate(shl_n) || shl_n.is_sym)
      continue;
    int a = (int)irop_get_imm64_ex(ir, shl_n);
    if (a < 1 || a > b)
      continue;

    /* SHL result must feed only this SHR, so NOPing it is safe. */
    if (!tcc_ir_vreg_has_single_use(ir, t1, shl_idx))
      continue;

    IROperand t0 = tcc_ir_op_get_src1(ir, shl_q);
    if (t0.is_lval || !irop_has_vreg(t0))
      continue;
    int32_t t0_vr = irop_get_vreg(t0);

    /* T0 must be unchanged between the SHL and the SHR, and no control-flow
     * edge may separate them (UBFX recomputes from T0 at the SHR's site). */
    int safe = 1;
    for (int j = shl_idx + 1; j < i && safe; j++)
    {
      IRQuadCompact *jq = &ir->compact_instructions[j];
      if (jq->op == TCCIR_OP_NOP)
        continue;
      if (jq->op == TCCIR_OP_JUMP || jq->op == TCCIR_OP_JUMPIF ||
          jq->op == TCCIR_OP_IJUMP || jq->op == TCCIR_OP_SWITCH_TABLE || jq->is_jump_target)
      {
        safe = 0;
        break;
      }
      if (irop_config[jq->op].has_dest)
      {
        IROperand jd = tcc_ir_op_get_dest(ir, jq);
        if (irop_has_vreg(jd) && irop_get_vreg(jd) == t0_vr)
        {
          safe = 0;
          break;
        }
      }
    }
    if (!safe || shr_q->is_jump_target)
      continue;

    int lsb = b - a;
    int width = 32 - b;
    int32_t param = lsb | (width << 5);
    shr_q->op = is_signed ? TCCIR_OP_SBFX : TCCIR_OP_UBFX;
    tcc_ir_set_src1(ir, i, t0);
    tcc_ir_set_src2(ir, i, irop_make_imm32(-1, param, IROP_BTYPE_INT32));
    shl_q->op = TCCIR_OP_NOP;
    changes++;
    LOG_IR_GEN("SHIFT-PAIR->%s @%d: (x<<%d)>>%d -> lsb=%d width=%d (SHL@%d NOP)", is_signed ? "SBFX" : "UBFX", i, a, b,
               lsb, width, shl_idx);
  }

  return changes;
}
