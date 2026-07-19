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
 *
 * SHL into ADD is the one combination that can LOSE instructions, because
 * `t = x SHL #n; a = base ADD t; *a` is exactly `base[x]` -- the shape the
 * addressing-mode selector collapses into a single `ldr rd,[base,x,lsl #n]`.
 * Eating the shift here leaves a standalone address-forming ADD behind and
 * costs one instruction per access (pr46883's loop body grew 25->32, pr53645
 * 762->852).  Two gates keep only the profitable half: skip when the ADD's
 * result reaches an address, and skip scales 1..3 outright (the only ones a
 * scaled addressing mode can encode, so a consumer this pass does not model --
 * PREFETCH, a later-formed indexed access -- can still claim them).  Every
 * other consumer keeps the fusion, which is a strict win: the shift vanishes.
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

  /* Vregs that reach a memory operation's address: an is_lval operand (a real
   * pointer -- is_local/is_llocal reads a stack variable's value, not an
   * address), or any operand of an already-formed indexed/post-inc/LEA access,
   * whose base is src1 for loads but dest for stores.  Marking those ops'
   * value operand too only forfeits a fusion, never breaks one.  One O(n)
   * prepass keeps the per-candidate test O(1). */
  uint8_t *deref_base = du.total > 0 ? tcc_mallocz((size_t)du.total) : NULL;
  if (deref_base)
  {
    for (int i = 0; i < n; i++)
    {
      IRQuadCompact *q = &ir->compact_instructions[i];
      int addressing = 0;
      switch (q->op)
      {
      case TCCIR_OP_LOAD_INDEXED: case TCCIR_OP_STORE_INDEXED:
      case TCCIR_OP_LOAD_POSTINC: case TCCIR_OP_STORE_POSTINC:
      case TCCIR_OP_LEA:
        addressing = 1;
        break;
      default:
        break;
      }
      for (int k = 0; k < 3; k++)
      {
        if (k == 0 ? !irop_config[q->op].has_dest
                   : k == 1 ? !irop_config[q->op].has_src1
                            : !irop_config[q->op].has_src2)
          continue;
        IROperand s = k == 0 ? tcc_ir_op_get_dest(ir, q)
                    : k == 1 ? tcc_ir_op_get_src1(ir, q)
                             : tcc_ir_op_get_src2(ir, q);
        if (!irop_has_vreg(s))
          continue;
        if (!addressing && (!s.is_lval || s.is_local || s.is_llocal))
          continue;
        int idx = ir_opt_du_idx(&du, irop_get_vreg(s));
        if (idx >= 0)
          deref_base[idx] = 1;
      }
    }
  }

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

      /* `base + (idx << 1..3)` is a scaled addressing mode; so is any ADD whose
       * result reaches a dereference.  Leave both for the addressing selector. */
      if (stype == 1 && q->op == TCCIR_OP_ADD) {
        if (amount >= 1 && amount <= 3)
          continue;
        IROperand ad = tcc_ir_op_get_dest(ir, q);
        int aidx = irop_has_vreg(ad) ? ir_opt_du_idx(&du, irop_get_vreg(ad)) : -1;
        if (!deref_base || aidx < 0 || deref_base[aidx])
          continue;
      }

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

  tcc_free(deref_base);
  tcc_free(du.def);
}

