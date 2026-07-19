/*
 *  TCC IR - Late Barrel Shift Fusion (runs just before codegen)
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


/*
 * Folds a single-use shift/rotate into the consuming ALU instruction's src2
 * using the ARM barrel shifter.  Results are written to ir->barrel_shifts[]
 * (a side-table), not into IRQuadCompact, so no intermediate pass can corrupt them.
 *
 * Pattern:
 *   t = SHL/SHR/SAR/ROR(x, #n)     -- single use, 32-bit
 *   result = ADD/SUB/AND/OR/XOR/CMP(y, t)
 *
 * Encoding: barrel_shifts[i] = (type<<5)|amount
 *   type: 1=SHL, 2=SHR, 3=SAR, 4=ROR.  amount: 0-31.
 */
void tcc_ir_barrel_shift_fusion(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  if (n == 0)
    return;

  ir->barrel_shifts = tcc_mallocz(ir->max_orig_index + 1);
  ir->barrel_shifts_len = ir->max_orig_index + 1;

  IROptDU du;
  ir_opt_du_build(ir, &du);

  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];

    int commutative = 0;
    switch (q->op)
    {
    case TCCIR_OP_ADD: case TCCIR_OP_AND: case TCCIR_OP_OR: case TCCIR_OP_XOR:
      commutative = 1;
      break;
    case TCCIR_OP_SUB: case TCCIR_OP_CMP:
      break;
    default:
      continue;
    }

    /* Try fusing on src2 first; for commutative ops, also try src1 (swapping
     * operands so the shift lands on src2 where the backend expects it). */
    for (int attempt = 0; attempt < (commutative ? 2 : 1); attempt++) {
      IROperand src2 = (attempt == 0) ? tcc_ir_op_get_src2(ir, q)
                                      : tcc_ir_op_get_src1(ir, q);
      if (!irop_has_vreg(src2))
        continue;

      int32_t vr2 = irop_get_vreg(src2);
      int shift_idx = ir_opt_du_def(&du, vr2, i);
      if (shift_idx < 0)
        continue;

      IRQuadCompact *sq = &ir->compact_instructions[shift_idx];
      int stype;
      switch (sq->op) {
      case TCCIR_OP_SHL:
        if (q->op == TCCIR_OP_ADD) continue;
        stype = 1; break;
      case TCCIR_OP_SHR: stype = 2; break;
      case TCCIR_OP_SAR: stype = 3; break;
      case TCCIR_OP_ROR: stype = 4; break;
      default: continue;
      }

      if (ir_opt_du_uses(&du, vr2) != 1)
        continue;

      IROperand shift_dest = tcc_ir_op_get_dest(ir, sq);
      if (shift_dest.btype == IROP_BTYPE_INT64)
        continue;

      IROperand consumer_dest = tcc_ir_op_get_dest(ir, q);
      if (consumer_dest.btype == IROP_BTYPE_INT64)
        continue;
      if (src2.btype == IROP_BTYPE_INT64)
        continue;

      IROperand shift_src2 = tcc_ir_op_get_src2(ir, sq);
      if (!irop_is_immediate(shift_src2))
        continue;

      int64_t amount = irop_get_imm64_ex(ir, shift_src2);
      if (amount < 0 || amount > 31)
        continue;

      /* A zero-amount right shift/rotate is an identity in the IR (x >> 0 == x),
       * but ARM's barrel shifter encodes an immediate field of 0 for LSR/ASR as
       * shift-by-32 (yielding 0 / sign-extend) and for ROR as RRX — NOT the
       * shift-by-0 we mean.  Only LSL #0 (stype 1) is a true no-op operand, so
       * refuse to fuse `x SHR/SAR/ROR #0`; leave the standalone shift for the
       * backend's shift-by-0 identity fold (arm-thumb-gen.c) to lower as MOV. */
      if (amount == 0 && stype != 1)
        continue;

      IROperand shift_src1 = tcc_ir_op_get_src1(ir, sq);
      if (!irop_has_vreg(shift_src1))
        continue;

      int32_t shift_src_vr = irop_get_vreg(shift_src1);

      IROperand other = (attempt == 0) ? tcc_ir_op_get_src1(ir, q)
                                        : tcc_ir_op_get_src2(ir, q);
      if (!irop_has_vreg(other))
        continue;
      if (irop_has_vreg(other) && irop_get_vreg(other) == shift_src_vr)
        continue;

      int safe = 1;
      for (int j = shift_idx + 1; j < i && safe; j++)
      {
        IRQuadCompact *jq = &ir->compact_instructions[j];
        TccIrOp bop = jq->op;
        if (bop == TCCIR_OP_JUMP || bop == TCCIR_OP_JUMPIF)
          safe = 0;
        if (bop == TCCIR_OP_NOP)
          continue;
        if (irop_config[bop].has_dest)
        {
          IROperand jdest = tcc_ir_op_get_dest(ir, jq);
          if (irop_has_vreg(jdest) && irop_get_vreg(jdest) == shift_src_vr)
            safe = 0;
        }
      }
      if (!safe)
        continue;

      /* For the swap path: rewrite src1 to the non-shift operand so the
       * backend sees `op dest, other, shifted`. The shift's source vreg
       * goes into src2 in both paths. */
      if (attempt == 1)
        tcc_ir_set_src1(ir, i, other);
      tcc_ir_set_src2(ir, i, shift_src1);
      ir->barrel_shifts[q->orig_index] = (uint8_t)((stype << 5) | (int)amount);
      sq->op = TCCIR_OP_NOP;
      break;
    }
  }

  tcc_free(du.def);
}

