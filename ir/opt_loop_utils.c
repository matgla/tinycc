/*
 *  TCC IR - Loop optimization utilities (pre-SSA)
 *
 *  Copyright (c) 2025 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#define USING_GLOBALS

#include "ir.h"
#include "licm.h"
#include "opt_du.h"
#include "opt_xform.h"
#include "opt_utils.h"
#include "opt_loop_utils.h"

/* Find basic induction variables in a loop.
 * An IV is a variable that is incremented by a constant in each iteration.
 * Pattern: V = V + const (where V is a VAR type vreg)
 */
int find_induction_vars_ex(TCCIRState *ir, IRLoop *loop, InductionVar *ivs, int max_ivs, int allow_copy_through)
{
  int num_ivs = 0;

  /* Scan the ORIGINAL loop range (not extended body) for IV increments */
  for (int i = loop->start_idx; i <= loop->end_idx && num_ivs < max_ivs; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];

    if (q->op != TCCIR_OP_ADD)
      continue;

    IROperand dest = tcc_ir_op_get_dest(ir, q);
    IROperand src1 = tcc_ir_op_get_src1(ir, q);
    IROperand src2 = tcc_ir_op_get_src2(ir, q);

    int dest_vr = irop_get_vreg(dest);
    int src1_vr = irop_get_vreg(src1);

    /* Must be a VAR register */
    if (dest_vr < 0 || TCCIR_DECODE_VREG_TYPE(dest_vr) != TCCIR_VREG_TYPE_VAR)
      continue;

    /* Pattern: V = V + const  OR  V = T + const where T := V (copy-through) */
    int effective_src_vr = src1_vr;
    if (allow_copy_through && src1_vr != dest_vr && src1_vr >= 0 && irop_is_immediate(src2))
    {
      /* Check if src1 is a temp assigned from dest_vr just before */
      for (int k = i - 1; k >= loop->start_idx && k >= i - 3; k--)
      {
        IRQuadCompact *aq = &ir->compact_instructions[k];
        if (aq->op == TCCIR_OP_ASSIGN)
        {
          IROperand adest = tcc_ir_op_get_dest(ir, aq);
          IROperand asrc = tcc_ir_op_get_src1(ir, aq);
          if (irop_get_vreg(adest) == src1_vr && irop_get_vreg(asrc) == dest_vr)
          {
            effective_src_vr = dest_vr;
            break;
          }
        }
        if (aq->op != TCCIR_OP_NOP)
          break; /* stop at first non-NOP non-matching instr */
      }
    }
    if (effective_src_vr == dest_vr && irop_is_immediate(src2))
    {
      int step = (int)irop_get_imm64_ex(ir, src2);

      /* Check that this VAR is only defined ONCE in the loop range
       * (the increment itself) and once in the preheader (initialization) */
      int def_count = 0;
      for (int j = loop->start_idx; j <= loop->end_idx; j++)
      {
        IRQuadCompact *dq = &ir->compact_instructions[j];
        IROperand ddest = tcc_ir_op_get_dest(ir, dq);
        if (irop_get_vreg(ddest) == dest_vr && dq->op != TCCIR_OP_NOP)
          def_count++;
      }

      if (def_count != 1)
        continue; /* IV has multiple definitions in loop - not simple */

      /* Look for initialization in preheader */
      int init_val = 0;
      int init_idx = -1;
      for (int j = loop->preheader_idx; j >= 0 && j >= loop->preheader_idx - 5; j--)
      {
        IRQuadCompact *pq = &ir->compact_instructions[j];
        if (pq->op == TCCIR_OP_ASSIGN)
        {
          IROperand pdest = tcc_ir_op_get_dest(ir, pq);
          IROperand psrc1 = tcc_ir_op_get_src1(ir, pq);
          if (irop_get_vreg(pdest) == dest_vr && irop_is_immediate(psrc1))
          {
            init_val = (int)irop_get_imm64_ex(ir, psrc1);
            init_idx = j;
            break;
          }
        }
      }

      if (init_idx < 0)
        continue; /* No initialization found */

      ivs[num_ivs].vreg = dest_vr;
      ivs[num_ivs].init_val = init_val;
      ivs[num_ivs].step = step;
      ivs[num_ivs].def_idx = i;
      ivs[num_ivs].init_idx = init_idx;
      num_ivs++;

      LOG_IV_SR("IV_SR: Found BIV VAR%d (init=%d, step=%d) at idx=%d", TCCIR_DECODE_VREG_POSITION(dest_vr), init_val,
                step, i);
      LOG_LOOP_OPT("IV found: VAR%d init=%d step=%d def_idx=%d init_idx=%d", TCCIR_DECODE_VREG_POSITION(dest_vr),
                   init_val, step, i, init_idx);
    }
  }

  LOG_LOOP_OPT("find_induction_vars_ex: found %d IV(s) in loop [%d..%d]", num_ivs, loop->start_idx, loop->end_idx);
  return num_ivs;
}


/* Find derived induction variables in a loop.
 * A DIV is: base + (IV << shift) - used for array indexing.
 * We look for ADD instructions that use a SHL result where SHL uses an IV,
 * and (separately) MLA instructions where MUL+ADD have already been fused.
 *
 * The licm.c body detector caps body extension at +50 instructions, which
 * misses rotated loops with the body proper placed AFTER the back-edge
 * (latch) in instruction order — common when tcc's loop rotation moves the
 * latch above the body.  We compute our own extended scan range here so
 * IV/SR works regardless of body layout.  Any j > end_idx whose JMP/JUMPIF
 * targets back into the current body must itself be part of the loop —
 * extend the body upward to include it, iterating until convergence.
 */
int find_derived_ivs(TCCIRState *ir, IRLoop *loop, InductionVar *ivs, int num_ivs, DerivedIV *divs, int max_divs)
{
  int num_divs = 0;

  /* Compute an MLA-only extended scan range.  licm.c's body detector caps
   * extension at +50 instructions, missing rotated loops with the body proper
   * placed AFTER the back-edge in instruction order.  Iteratively extend the
   * end of the loop range to include any j > end_idx whose JMP/JUMPIF targets
   * back into the body — those instructions must execute as part of the loop.
   *
   * We use this extended range ONLY for MLA-fused DIV detection (a new pattern
   * that the existing pass never handled).  The existing ADD-based detection
   * keeps using loop->body_instrs to preserve baseline behavior — extending
   * its scan can trigger downstream passes (local_alu_cse → copy_prop → DCE)
   * to wrongly drop SHR/AND chains in bodies that weren't previously visible
   * to IV/SR.  Restricting body extension to the new MLA pattern avoids that
   * regression while still catching the test_ge_operator case. */
  int mla_scan_start = loop->start_idx;
  int mla_scan_end = loop->end_idx;
  {
    int extended;
    do
    {
      extended = 0;
      for (int j = mla_scan_end + 1; j < ir->next_instruction_index; j++)
      {
        IRQuadCompact *jq = &ir->compact_instructions[j];
        if (jq->op != TCCIR_OP_JUMP && jq->op != TCCIR_OP_JUMPIF)
          continue;
        IROperand jdest = tcc_ir_op_get_dest(ir, jq);
        int jtarget = (int)irop_get_imm64_ex(ir, jdest);
        if (jtarget >= mla_scan_start && jtarget <= mla_scan_end)
        {
          mla_scan_end = j;
          extended = 1;
        }
      }
    } while (extended);
  }

  if (TCC_LOG_IV_SR)
  {
    fprintf(stderr, "[IV_SR] Loop body_instrs:");
    for (int bi = 0; bi < loop->num_body_instrs; bi++)
      fprintf(stderr, " %d", loop->body_instrs[bi]);
    fprintf(stderr, " (MLA scan range: [%d..%d])\n", mla_scan_start, mla_scan_end);
  }

  /* Scan the extended body for ADD instructions (DIV computation) */
  for (int bi = 0; bi < loop->num_body_instrs && num_divs < max_divs; bi++)
  {
    int i = loop->body_instrs[bi];
    IRQuadCompact *q = &ir->compact_instructions[i];

    if (q->op != TCCIR_OP_ADD)
      continue;

    IROperand dest = tcc_ir_op_get_dest(ir, q);
    IROperand src1 = tcc_ir_op_get_src1(ir, q);
    IROperand src2 = tcc_ir_op_get_src2(ir, q);

    /* Pattern: T = base + T_mul_shl  OR  T = T_mul_shl + base */
    int shl_vr = -1, base_vr = -1;
    IROperand *base_op = NULL;
    int shl_idx = -1;
    int is_mul = 0;

    /* Check src2 for SHL/MUL result */
    int vr2 = irop_get_vreg(src2);
    if (vr2 >= 0 && TCCIR_DECODE_VREG_TYPE(vr2) == TCCIR_VREG_TYPE_TEMP)
    {
      /* Look for SHL/MUL defining this temp */
      for (int j = 0; j < loop->num_body_instrs; j++)
      {
        int sj = loop->body_instrs[j];
        if (sj >= i)
          break; /* Must be before the ADD */
        IRQuadCompact *sq = &ir->compact_instructions[sj];
        if (sq->op == TCCIR_OP_SHL || sq->op == TCCIR_OP_MUL)
        {
          IROperand sdest = tcc_ir_op_get_dest(ir, sq);
          if (irop_get_vreg(sdest) == vr2)
          {
            shl_vr = vr2;
            shl_idx = sj;
            base_op = &src1;
            base_vr = irop_get_vreg(src1);
            is_mul = (sq->op == TCCIR_OP_MUL);
            break;
          }
        }
      }
    }

    /* Check src1 for SHL/MUL result if not found */
    if (shl_vr < 0)
    {
      int vr1 = irop_get_vreg(src1);
      if (vr1 >= 0 && TCCIR_DECODE_VREG_TYPE(vr1) == TCCIR_VREG_TYPE_TEMP)
      {
        for (int j = 0; j < loop->num_body_instrs; j++)
        {
          int sj = loop->body_instrs[j];
          if (sj >= i)
            break;
          IRQuadCompact *sq = &ir->compact_instructions[sj];
          if (sq->op == TCCIR_OP_SHL || sq->op == TCCIR_OP_MUL)
          {
            IROperand sdest = tcc_ir_op_get_dest(ir, sq);
            if (irop_get_vreg(sdest) == vr1)
            {
              shl_vr = vr1;
              shl_idx = sj;
              base_op = &src2;
              base_vr = irop_get_vreg(src2);
              is_mul = (sq->op == TCCIR_OP_MUL);
              break;
            }
          }
        }
      }
    }

    if (shl_idx < 0)
      continue; /* Not a base + SHL/MUL pattern */

    /* Check that the SHL/MUL input is an IV */
    IRQuadCompact *shl_q = &ir->compact_instructions[shl_idx];
    IROperand shl_src1 = tcc_ir_op_get_src1(ir, shl_q);
    IROperand shl_src2 = tcc_ir_op_get_src2(ir, shl_q);

    int iv_vr = irop_get_vreg(shl_src1);
    if (iv_vr < 0 || !irop_is_immediate(shl_src2))
    {
      /* Check if src1 is immediate and src2 is IV (for MUL) */
      if (is_mul && irop_is_immediate(shl_src1))
      {
        iv_vr = irop_get_vreg(shl_src2);
        if (iv_vr >= 0)
        {
          IROperand tmp = shl_src1;
          shl_src1 = shl_src2;
          shl_src2 = tmp;
        }
        else
        {
          continue;
        }
      }
      else
      {
        continue;
      }
    }

    /* Find which IV this corresponds to */
    int iv_idx = -1;
    for (int k = 0; k < num_ivs; k++)
    {
      if (ivs[k].vreg == iv_vr)
      {
        iv_idx = k;
        break;
      }
    }

    /* Chase one level of copy: if iv_vr is defined by ASSIGN/STORE from
     * a BIV (e.g. V4 <-- V1 [STORE]), treat it as the BIV. */
    if (iv_idx < 0 && iv_vr >= 0)
    {
      int def = tcc_ir_find_defining_instruction(ir, iv_vr, shl_idx);
      if (def >= 0)
      {
        IRQuadCompact *dq = &ir->compact_instructions[def];
        if (dq->op == TCCIR_OP_ASSIGN || dq->op == TCCIR_OP_STORE)
        {
          int copy_src = irop_get_vreg(tcc_ir_op_get_src1(ir, dq));
          for (int k = 0; k < num_ivs; k++)
          {
            if (ivs[k].vreg == copy_src)
            {
              iv_idx = k;
              break;
            }
          }
        }
      }
    }

    if (iv_idx < 0)
      continue; /* SHL/MUL operand is not an IV */

    /* Calculate stride */
    int stride;
    if (is_mul)
    {
      int mul_const = (int)irop_get_imm64_ex(ir, shl_src2);
      stride = ivs[iv_idx].step * mul_const;
    }
    else
    {
      int shift = (int)irop_get_imm64_ex(ir, shl_src2);
      stride = ivs[iv_idx].step * (1 << shift);
    }

    /* Check that this ADD result is used (not dead code).  Multiple uses
     * are fine: the transformation replaces the ADD with ASSIGN (result =
     * strength-reduced ptr), so all existing uses transparently receive
     * the correct address value. */
    int dest_vr = irop_get_vreg(dest);
    int use_count = 0;
    for (int j = 0; j < ir->next_instruction_index; j++)
    {
      if (j == i)
        continue;
      IRQuadCompact *uq = &ir->compact_instructions[j];
      IROperand u1 = tcc_ir_op_get_src1(ir, uq);
      IROperand u2 = tcc_ir_op_get_src2(ir, uq);
      if (irop_get_vreg(u1) == dest_vr)
        use_count++;
      if (irop_get_vreg(u2) == dest_vr)
        use_count++;
      if (uq->op == TCCIR_OP_STORE || uq->op == TCCIR_OP_STORE_INDEXED || uq->op == TCCIR_OP_STORE_POSTINC)
      {
        IROperand ud = tcc_ir_op_get_dest(ir, uq);
        if (irop_get_vreg(ud) == dest_vr)
          use_count++;
      }
    }

    if (use_count < 1)
      continue; /* Dead code — skip */

    /* Check that the SHL result is only used by this ADD.
     * After CSE, other instructions might reference this SHL's result.
     * If so, we can't NOP the SHL without breaking those uses. */
    int shl_vr_uses = 0;
    for (int j = 0; j < ir->next_instruction_index; j++)
    {
      if (j == shl_idx)
        continue;
      IRQuadCompact *uq = &ir->compact_instructions[j];
      IROperand u1 = tcc_ir_op_get_src1(ir, uq);
      IROperand u2 = tcc_ir_op_get_src2(ir, uq);
      if (irop_get_vreg(u1) == shl_vr)
        shl_vr_uses++;
      if (irop_get_vreg(u2) == shl_vr)
        shl_vr_uses++;
    }

    if (shl_vr_uses != 1)
    {
      LOG_IV_SR("IV_SR: Skipping DIV at idx=%d: SHL result has %d uses (not 1)", i, shl_vr_uses);
      continue; /* SHL result used by other instructions - can't NOP it */
    }

    divs[num_divs].iv_idx = iv_idx;
    divs[num_divs].base_vreg = base_vr;
    divs[num_divs].base_op = *base_op;
    divs[num_divs].stride = stride;
    divs[num_divs].use_idx = i;
    divs[num_divs].shl_idx = shl_idx;
    divs[num_divs].share_with = -1;
    num_divs++;

    LOG_IV_SR("IV_SR: Found DIV base+%d*VAR%d at ADD idx=%d (SHL idx=%d)", stride, TCCIR_DECODE_VREG_POSITION(iv_vr), i,
              shl_idx);
  }

  /* Second pass: detect MLA-fused derived IVs.
   * Pattern: dest = src1 * src2 + accum  where
   *   src1 = IV (or copy-through to one)
   *   src2 = stride immediate
   *   accum = loop-invariant base
   * MLA is produced by Phase 3b–4b's MUL→MLA fusion BEFORE IV/SR runs, so
   * the original `MUL+ADD` shape this pass was written for is gone.  We treat
   * the MLA itself as the use site and set shl_idx = -1 (no separate SHL/MUL
   * to NOP — the multiply is fused into the MLA we replace).  Uses the
   * extended MLA scan range so rotated loops with body-after-back-edge are
   * also covered. */
  if (getenv("TCC_DBG_MLAIV")) {
    fprintf(stderr, "[MLAIV] scan loop [%d..%d]\n", mla_scan_start, mla_scan_end);
    for (int dbg = mla_scan_start; dbg <= mla_scan_end; dbg++)
      fprintf(stderr, "[MLAIV]   idx %d op=%d\n", dbg, ir->compact_instructions[dbg].op);
  }
  for (int i = mla_scan_start; i <= mla_scan_end && num_divs < max_divs; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];

    if (q->op != TCCIR_OP_MLA)
      continue;
    if (getenv("TCC_DBG_MLAIV"))
      fprintf(stderr, "[MLAIV] candidate MLA at idx %d\n", i);

    IROperand dest = tcc_ir_op_get_dest(ir, q);
    IROperand src1 = tcc_ir_op_get_src1(ir, q);
    IROperand src2 = tcc_ir_op_get_src2(ir, q);
    IROperand accum = tcc_ir_op_get_accum(ir, q);

    /* src2 must be the stride immediate */
    if (!irop_is_immediate(src2))
      continue;

    /* src1 must be an IV (with one level of copy-through) */
    int iv_vr = irop_get_vreg(src1);
    if (iv_vr < 0)
      continue;

    int iv_idx = -1;
    for (int k = 0; k < num_ivs; k++)
    {
      if (ivs[k].vreg == iv_vr)
      {
        iv_idx = k;
        break;
      }
    }
    if (iv_idx < 0)
    {
      /* Chase one level of copy: T <-- VAR ASSIGN */
      int def = tcc_ir_find_defining_instruction(ir, iv_vr, i);
      if (def >= 0)
      {
        IRQuadCompact *dq = &ir->compact_instructions[def];
        if (dq->op == TCCIR_OP_ASSIGN || dq->op == TCCIR_OP_STORE)
        {
          int copy_src = irop_get_vreg(tcc_ir_op_get_src1(ir, dq));
          for (int k = 0; k < num_ivs; k++)
          {
            if (ivs[k].vreg == copy_src)
            {
              iv_idx = k;
              break;
            }
          }
        }
      }
    }
    if (iv_idx < 0)
      continue;

    /* accum (the base) must be loop-invariant: not redefined inside the loop.
     * Use the extended MLA scan range so rotated loops with the body proper
     * outside [start_idx..end_idx] are still checked correctly. */
    int base_vr = irop_get_vreg(accum);
    if (base_vr >= 0)
    {
      int redefined = 0;
      for (int j = mla_scan_start; j <= mla_scan_end; j++)
      {
        IRQuadCompact *lq = &ir->compact_instructions[j];
        if (lq->op == TCCIR_OP_NOP || j == i)
          continue;
        if (irop_config[lq->op].has_dest)
        {
          IROperand ld = tcc_ir_op_get_dest(ir, lq);
          if (irop_get_vreg(ld) == base_vr)
          {
            redefined = 1;
            break;
          }
        }
      }
      if (redefined)
        continue;
    }

    int mul_const = (int)irop_get_imm64_ex(ir, src2);
    int stride = ivs[iv_idx].step * mul_const;

    /* Dead-code check: must have at least one use of this MLA's dest. */
    int dest_vr = irop_get_vreg(dest);
    int use_count = 0;
    for (int j = 0; j < ir->next_instruction_index; j++)
    {
      if (j == i)
        continue;
      IRQuadCompact *uq = &ir->compact_instructions[j];
      IROperand u1 = tcc_ir_op_get_src1(ir, uq);
      IROperand u2 = tcc_ir_op_get_src2(ir, uq);
      if (irop_get_vreg(u1) == dest_vr)
        use_count++;
      if (irop_get_vreg(u2) == dest_vr)
        use_count++;
      if (uq->op == TCCIR_OP_STORE || uq->op == TCCIR_OP_STORE_INDEXED || uq->op == TCCIR_OP_STORE_POSTINC)
      {
        IROperand ud = tcc_ir_op_get_dest(ir, uq);
        if (irop_get_vreg(ud) == dest_vr)
          use_count++;
      }
    }
    if (use_count < 1)
      continue;

    divs[num_divs].iv_idx = iv_idx;
    divs[num_divs].base_vreg = base_vr;
    divs[num_divs].base_op = accum;
    divs[num_divs].stride = stride;
    divs[num_divs].use_idx = i;
    divs[num_divs].shl_idx = -1; /* fused into MLA — nothing to NOP */
    divs[num_divs].share_with = -1;
    num_divs++;

    if (getenv("TCC_DBG_MLAIV"))
      fprintf(stderr, "[MLAIV] FOUND MLA-DIV at idx %d, stride=%d, iv_vr=%d, base_vr=%d\n", i, stride, iv_vr, base_vr);
    LOG_IV_SR("IV_SR: Found MLA-DIV base+%d*VAR%d at MLA idx=%d (fused)", stride, TCCIR_DECODE_VREG_POSITION(iv_vr), i);
  }

  /* Third pass: detect LOAD_INDEXED / STORE_INDEXED derived IVs.
   *
   * The earlier indexed-memory fusion pass (tcc_ir_opt_fusion_pass) folded the
   * `SHL idx, #k; ADD addr, base, idx; LOAD val, addr` triple into a single
   * `LOAD_INDEXED val, base, idx, #k` before IV/SR runs.  The original
   * ADD-based scan above can no longer see those uses, so counted array-walk
   * loops (`for (i=0;i<N;i++) sum += arr[i]`) never get a pointer IV.
   *
   * Match the folded shape directly: treat each LOAD_INDEXED/STORE_INDEXED
   * whose index operand is a BIV (or copy-through) as a DIV with stride
   * iv.step * (1<<scale), use_idx pointing at the indexed op, and shl_idx = -1
   * (no separate SHL to NOP — the shift is encoded in the scale field).
   *
   * Gating: only register the DIV when the IV's *only* remaining uses are
   * these indexed accesses, its own self-increment, and a CMP against an
   * immediate.  Otherwise the IV stays live after transformation, the
   * `ptr += stride` we insert is pure cost, and the body grows by one
   * instruction with no compensating saving — a regression.  When the IV
   * is eliminable, the later POSTINC fusion and try_eliminate_iv_counter
   * passes collapse `LOAD ptr; ptr += k` into `LOAD_POSTINC` and replace
   * `CMP i, N` with `CMP ptr, end_ptr`, dropping two instructions overall. */
  for (int bi = 0; bi < loop->num_body_instrs && num_divs < max_divs; bi++)
  {
    int i = loop->body_instrs[bi];
    IRQuadCompact *q = &ir->compact_instructions[i];

    int is_load = (q->op == TCCIR_OP_LOAD_INDEXED);
    int is_store = (q->op == TCCIR_OP_STORE_INDEXED);
    if (!is_load && !is_store)
      continue;

    IROperand base_op = is_load ? tcc_ir_op_get_src1(ir, q) : tcc_ir_op_get_dest(ir, q);
    IROperand index_op = tcc_ir_op_get_src2(ir, q);
    IROperand scale_op = tcc_ir_op_get_scale(ir, q);
    IROperand val_op = is_load ? tcc_ir_op_get_dest(ir, q) : tcc_ir_op_get_src1(ir, q);

    if (!irop_is_immediate(scale_op))
      continue;

    /* Restrict to 32-bit accesses.  POSTINC fusion (which we rely on to
     * recoup the preheader cost of the end-pointer) only handles INT32 —
     * see opt.c around the `btype != IROP_BTYPE_INT32` check.  For other
     * widths the transform adds a preheader instruction with no body
     * saving, producing a net regression. */
    if (val_op.btype != IROP_BTYPE_INT32)
      continue;

    int iv_vr = irop_get_vreg(index_op);
    if (iv_vr < 0)
      continue;

    int iv_idx = -1;
    for (int k = 0; k < num_ivs; k++)
    {
      if (ivs[k].vreg == iv_vr)
      {
        iv_idx = k;
        break;
      }
    }

    /* Copy-through chase, same shape as the MLA path above. */
    if (iv_idx < 0)
    {
      int def = tcc_ir_find_defining_instruction(ir, iv_vr, i);
      if (def >= 0)
      {
        IRQuadCompact *dq = &ir->compact_instructions[def];
        if (dq->op == TCCIR_OP_ASSIGN || dq->op == TCCIR_OP_STORE)
        {
          int copy_src = irop_get_vreg(tcc_ir_op_get_src1(ir, dq));
          for (int k = 0; k < num_ivs; k++)
          {
            if (ivs[k].vreg == copy_src)
            {
              iv_idx = k;
              break;
            }
          }
        }
      }
    }
    if (iv_idx < 0)
      continue;

    /* Base must be loop-invariant.  STORE/STORE_INDEXED/STORE_POSTINC writing
     * through the base address are USES, not definitions, even though their
     * dest slot holds the base — skip those when checking for redefinition. */
    int base_vr = irop_get_vreg(base_op);
    if (base_vr >= 0)
    {
      int redefined = 0;
      for (int j = loop->start_idx; j <= loop->end_idx; j++)
      {
        IRQuadCompact *lq = &ir->compact_instructions[j];
        if (lq->op == TCCIR_OP_NOP || j == i)
          continue;
        if (lq->op == TCCIR_OP_STORE || lq->op == TCCIR_OP_STORE_INDEXED ||
            lq->op == TCCIR_OP_STORE_POSTINC)
          continue;
        if (irop_config[lq->op].has_dest)
        {
          IROperand ld = tcc_ir_op_get_dest(ir, lq);
          if (irop_get_vreg(ld) == base_vr)
          {
            redefined = 1;
            break;
          }
        }
      }
      if (redefined)
        continue;
    }

    int scale = (int)irop_get_imm64_ex(ir, scale_op);
    if (scale < 0 || scale > 3)
      continue;
    int stride = ivs[iv_idx].step * (1 << scale);

    /* Eliminability gate: the IV must have NO uses besides
     *   - its own self-increment (ivs[iv_idx].def_idx),
     *   - the copy-through ASSIGN that precedes the self-increment (if any),
     *   - LOAD_INDEXED / STORE_INDEXED instructions with this same IV as index,
     *   - the header pre-test CMP and/or the back-edge CMP (each compared to
     *     an immediate).  Loop rotation may emit both, and try_eliminate_iv_counter
     *     rewrites both — so we accept up to two.
     * Without this guarantee, the IV stays live after we add `ptr += stride`,
     * and the body grows by one instruction with nothing to compensate. */
    int safe_to_transform = 1;
    int cmp_count = 0;
    int iv_def_idx = ivs[iv_idx].def_idx;
    for (int j = 0; j < ir->next_instruction_index && safe_to_transform; j++)
    {
      IRQuadCompact *uq = &ir->compact_instructions[j];
      if (uq->op == TCCIR_OP_NOP)
        continue;
      if (j == iv_def_idx)
        continue;

      /* Allow LOAD_INDEXED/STORE_INDEXED uses where the IV appears as the
       * index — these are the use sites we're transforming. */
      if (uq->op == TCCIR_OP_LOAD_INDEXED || uq->op == TCCIR_OP_STORE_INDEXED)
      {
        if (irop_get_vreg(tcc_ir_op_get_src2(ir, uq)) == iv_vr)
          continue;
      }

      /* Allow the copy-through pattern `T = V` placed just before the self
       * increment by SSA-out (see find_induction_vars_ex copy-through path). */
      if (uq->op == TCCIR_OP_ASSIGN && j >= iv_def_idx - 3 && j < iv_def_idx)
      {
        if (irop_get_vreg(tcc_ir_op_get_src1(ir, uq)) == iv_vr)
          continue;
      }

      /* Allow up to two CMP V_iv, #imm — try_eliminate_iv_counter rewrites
       * both a header pre-test guard and a back-edge test if present. */
      if (uq->op == TCCIR_OP_CMP)
      {
        IROperand cs1 = tcc_ir_op_get_src1(ir, uq);
        IROperand cs2 = tcc_ir_op_get_src2(ir, uq);
        if (irop_get_vreg(cs1) == iv_vr && irop_is_immediate(cs2) && cmp_count < 2)
        {
          cmp_count++;
          continue;
        }
      }

      if (irop_config[uq->op].has_src1)
      {
        IROperand s1 = tcc_ir_op_get_src1(ir, uq);
        if (irop_get_vreg(s1) == iv_vr)
          safe_to_transform = 0;
      }
      if (safe_to_transform && irop_config[uq->op].has_src2)
      {
        IROperand s2 = tcc_ir_op_get_src2(ir, uq);
        if (irop_get_vreg(s2) == iv_vr)
          safe_to_transform = 0;
      }
    }
    if (!safe_to_transform || cmp_count == 0)
    {
      LOG_IV_SR("IV_SR: Skipping INDEXED-DIV at idx=%d — IV VAR%d not eliminable (cmp_count=%d)", i,
                TCCIR_DECODE_VREG_POSITION(iv_vr), cmp_count);
      continue;
    }

    divs[num_divs].iv_idx = iv_idx;
    divs[num_divs].base_vreg = base_vr;
    divs[num_divs].base_op = base_op;
    divs[num_divs].stride = stride;
    divs[num_divs].use_idx = i;
    divs[num_divs].shl_idx = -1; /* shift is encoded in the scale field */
    divs[num_divs].share_with = -1;
    num_divs++;

    LOG_IV_SR("IV_SR: Found INDEXED-DIV base+%d*VAR%d at %s idx=%d (scale=%d, stride=%d)", stride,
              TCCIR_DECODE_VREG_POSITION(iv_vr), is_load ? "LOAD_INDEXED" : "STORE_INDEXED", i, scale, stride);
  }

  return num_divs;
}

/* Insert an instruction at position 'pos', shifting all later instructions.
 * Updates jump targets that reference instructions >= pos.
 * Returns the instruction index where the new instruction was inserted.
 */

int insert_instr_at(TCCIRState *ir, int pos, TccIrOp op, IROperand dest, IROperand src1, IROperand src2)
{
  int n = ir->next_instruction_index;

  /* Make room by shifting instructions */
  if (n + 1 >= ir->compact_instructions_size)
  {
    int new_size = ir->compact_instructions_size << 1;
    ir->compact_instructions = (IRQuadCompact *)tcc_realloc(ir->compact_instructions, new_size * sizeof(IRQuadCompact));
    ir->compact_instructions_size = new_size;
  }

  /* Ensure operand pool has room for 3 slots */
  if (ir->iroperand_pool_count + 3 > ir->iroperand_pool_capacity)
  {
    tcc_ir_pool_ensure(ir, 3);
    if (ir->iroperand_pool_count + 3 > ir->iroperand_pool_capacity)
    {
      if (TCC_LOG_IV_SR)
        fprintf(stderr, "[IV_SR] ERROR: iroperand_pool_capacity limit reached\n");
      return -1;
    }
  }

  /* Shift instructions from pos to end */
  for (int i = n; i > pos; i--)
  {
    ir->compact_instructions[i] = ir->compact_instructions[i - 1];
  }
  ir->next_instruction_index++;

  /* Update jump targets that point at or after pos */
  for (int i = 0; i < ir->next_instruction_index; i++)
  {
    if (i == pos)
      continue; /* Skip the new instruction */
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_JUMPIF)
    {
      IROperand jdest = tcc_ir_op_get_dest(ir, q);
      int target = (int)irop_get_imm64_ex(ir, jdest);
      if (target >= pos)
      {
        IROperand new_dest = irop_make_imm32(-1, target + 1, IROP_BTYPE_INT32);
        tcc_ir_op_set_dest(ir, q, new_dest);
      }
    }
  }

  /* Create the new instruction using operand pool */
  IRQuadCompact *new_q = &ir->compact_instructions[pos];
  new_q->op = op;
  new_q->orig_index = pos;
  new_q->is_jump_target = 0; /* shifted instructions carry their flag; new slot has none */
  new_q->line_num = 0;
  new_q->operand_base = tcc_ir_pool_add(ir, dest); /* dest at base + 0 */
  tcc_ir_pool_add(ir, src1);                       /* src1 at base + 1 */
  tcc_ir_pool_add(ir, src2);                       /* src2 at base + 2 */

  return pos;
}

/* Transform a derived IV to use pointer increment.
 * 1. Insert ptr = base + (iv_init * stride) in preheader (BEFORE the header)
 * 2. Replace the ADD (DIV) with just using ptr
 * 3. Insert ptr += stride after the IV increment
 * 4. NOP out the SHL instruction (skipped when div->shl_idx < 0, MLA case)
 *
 * If shared_ptr_vreg >= 0 the DIV reuses an already-strength-reduced pointer
 * (group of identical recurrences), so this skips the init/bump steps and
 * only rewrites use_idx in place to ASSIGN dest, shared_ptr.
 */
int transform_derived_iv(TCCIRState *ir, IRLoop *loop, InductionVar *iv, DerivedIV *div, int *out_ptr_vreg,
                                int *out_idx_shift, int *out_postnop_origpos, int shared_ptr_vreg)
{
  if (out_ptr_vreg)
    *out_ptr_vreg = -1;
  if (out_idx_shift)
    *out_idx_shift = 0;
  if (out_postnop_origpos)
    *out_postnop_origpos = -1;

  /* Shared-pointer fast path: rewrite the use site to ASSIGN of the existing
   * primary's strength-reduced pointer.  No insertions — just rewrites.
   * Returns 1 to signal success without triggering the caller's index-shift
   * bookkeeping (no instructions inserted). */
  if (shared_ptr_vreg >= 0)
  {
    if (div->use_idx < 0 || div->use_idx >= ir->next_instruction_index)
      return 0;
    IRQuadCompact *use_q = &ir->compact_instructions[div->use_idx];
    IROperand ptr_op = irop_make_vreg(shared_ptr_vreg, IROP_BTYPE_INT32);
    IROperand null_op = {0};

    /* INDEXED-DIV use site: rewrite LOAD_INDEXED→LOAD or STORE_INDEXED→STORE
     * pointing at the shared primary's pointer.  The trailing index/scale
     * slots are left orphaned in the pool (harmless — plain LOAD/STORE never
     * reads them). */
    if (use_q->op == TCCIR_OP_LOAD_INDEXED)
    {
      IROperand ptr_lval = ptr_op;
      ptr_lval.is_lval = 1;
      use_q->op = TCCIR_OP_LOAD;
      tcc_ir_op_set_src1(ir, use_q, ptr_lval);
      LOG_IV_SR("IV_SR: shared INDEXED-DIV at idx=%d rewritten to LOAD <- TMP%d", div->use_idx,
                TCCIR_DECODE_VREG_POSITION(shared_ptr_vreg));
      if (out_ptr_vreg)
        *out_ptr_vreg = shared_ptr_vreg;
      return 1;
    }
    if (use_q->op == TCCIR_OP_STORE_INDEXED)
    {
      IROperand ptr_lval = ptr_op;
      ptr_lval.is_lval = 1;
      use_q->op = TCCIR_OP_STORE;
      tcc_ir_op_set_dest(ir, use_q, ptr_lval);
      LOG_IV_SR("IV_SR: shared INDEXED-DIV at idx=%d rewritten to STORE -> TMP%d", div->use_idx,
                TCCIR_DECODE_VREG_POSITION(shared_ptr_vreg));
      if (out_ptr_vreg)
        *out_ptr_vreg = shared_ptr_vreg;
      return 1;
    }

    use_q->op = TCCIR_OP_ASSIGN;
    tcc_ir_op_set_src1(ir, use_q, ptr_op);
    tcc_ir_op_set_src2(ir, use_q, null_op);
    /* If this DIV had a separate SHL/MUL feeding into it (shl_idx >= 0),
     * NOP it — its result is now dead because the consuming ADD just became
     * an ASSIGN.  Leaving a dead SHL/MUL in place would let later passes
     * (e.g. local_alu_cse) treat its output as a live equivalent expression
     * and CSE other matching ADDs into stale values, miscompiling the loop.
     * For MLA-fused DIVs (shl_idx == -1) there is no separate instruction. */
    if (div->shl_idx >= 0 && div->shl_idx < ir->next_instruction_index)
    {
      IRQuadCompact *shl_q = &ir->compact_instructions[div->shl_idx];
      shl_q->op = TCCIR_OP_NOP;
    }
    /* Note: MLA's accum operand at +3 is now orphaned in the pool, harmless. */
    if (out_ptr_vreg)
      *out_ptr_vreg = shared_ptr_vreg;
    LOG_IV_SR("IV_SR: shared-DIV at idx=%d rewritten to ASSIGN <- TMP%d (NOPed shl_idx=%d)", div->use_idx,
              TCCIR_DECODE_VREG_POSITION(shared_ptr_vreg), div->shl_idx);
    return 1;
  }

  /* Allocate a new temp vreg for the pointer */
  int ptr_vreg = tcc_ir_vreg_alloc_temp(ir);
  if (ptr_vreg < 0)
    return 0;

  LOG_IV_SR("IV_SR: Transforming DIV at idx=%d, new ptr vreg=TMP%d, iv_init=%d, stride=%d", div->use_idx,
            TCCIR_DECODE_VREG_POSITION(ptr_vreg), iv->init_val, div->stride);

  /* Step 1: Insert ptr = base + (iv_init * stride) BEFORE the loop header
   * This ensures the init is executed once before entering the loop.
   * Important: We insert at preheader_idx + 1 to place it AFTER the preheader
   * instruction but BEFORE the header instruction.
   *
   * If iv_init == 0, we just do ptr = base
   * Otherwise, ptr = base + (iv_init * stride) requires two instructions:
   *   ptr = base
   *   ptr = ptr + offset
   */
  if (loop->preheader_idx < 0)
    return 0;
  int insert_pos = loop->preheader_idx + 1;

  /* Bail out if inserting would split a CMP→JUMPIF pair.  The preheader
   * might be the CMP itself — inserting after it places instructions
   * between CMP and JUMPIF, and the ADD would clobber condition flags.
   * Only bail when init_offset != 0 (which inserts an ADD that clobbers flags);
   * a single ASSIGN does not clobber condition flags on ARM. */
  {
    int element_size_check = div->stride / iv->step;
    int init_offset_check = iv->init_val * element_size_check;
    if (init_offset_check != 0 && insert_pos > 0 && ir->compact_instructions[insert_pos - 1].op == TCCIR_OP_CMP &&
        insert_pos < ir->next_instruction_index && ir->compact_instructions[insert_pos].op == TCCIR_OP_JUMPIF)
    {
      LOG_IV_SR("IV_SR: Skipping DIV transform — would split CMP→JUMPIF at %d→%d", insert_pos - 1, insert_pos);
      return 0;
    }
  }

  LOG_IV_SR("IV_SR: transform_derived_iv: header_idx=%d, preheader_idx=%d, start_idx=%d, end_idx=%d, insert_pos=%d",
            loop->header_idx, loop->preheader_idx, loop->start_idx, loop->end_idx, insert_pos);

  /* Safety check: verify that base_op (if it's a vreg) is defined before
   * insert_pos.  This can fail when LICM hoists a stack-address for an inner
   * loop, placing the definition of the base vreg AFTER the outer loop's
   * header.  Inserting the derived-IV init before that definition would
   * create a use-before-def. */
  {
    int32_t base_vr = irop_get_vreg(div->base_op);
    if (base_vr >= 0)
    {
      int def_found_before = 0;
      for (int i = 0; i < insert_pos; i++)
      {
        IRQuadCompact *q = &ir->compact_instructions[i];
        if (irop_config[q->op].has_dest)
        {
          IROperand qd = tcc_ir_op_get_dest(ir, q);
          if (irop_get_vreg(qd) == base_vr)
          {
            def_found_before = 1;
            break;
          }
        }
      }
      if (!def_found_before)
      {
        LOG_IV_SR("IV_SR: Skipping DIV transform — base vreg not defined before insert_pos %d", insert_pos);
        return 0;
      }

      /* Also verify base is loop-invariant: not defined inside the loop body.
       * If the base changes each iteration, the strength-reduced pointer
       * would diverge from the original address computation. */
      for (int i = loop->start_idx; i <= loop->end_idx; i++)
      {
        IRQuadCompact *lq = &ir->compact_instructions[i];
        if (lq->op == TCCIR_OP_NOP)
          continue;
        if (irop_config[lq->op].has_dest)
        {
          IROperand ld = tcc_ir_op_get_dest(ir, lq);
          if (irop_get_vreg(ld) == base_vr)
          {
            LOG_IV_SR("IV_SR: Skipping DIV transform — base vreg redefined inside loop at idx %d", i);
            return 0;
          }
        }
      }
    }
  }

  IROperand ptr_op = irop_make_vreg(ptr_vreg, IROP_BTYPE_INT32);
  IROperand null_op = {0};

  int idx_shift = 0;

  /* Calculate initial offset = iv_init * element_size
   * where element_size = stride / step = (1 << shift).
   * Note: init_offset = init_val * stride is WRONG when step != 1,
   * because stride = step * element_size, not just element_size. */
  int element_size = div->stride / iv->step;
  int init_offset = iv->init_val * element_size;

  if (init_offset == 0)
  {
    /* Simple case: ptr = base */
    int inserted = insert_instr_at(ir, insert_pos, TCCIR_OP_ASSIGN, ptr_op, div->base_op, null_op);
    LOG_IV_SR("IV_SR: init insert at pos=%d, result=%d (base_vr=%d)", insert_pos, inserted,
              irop_get_vreg(div->base_op));
    if (inserted < 0)
      return 0;
    idx_shift = 1;
  }
  else
  {
    /* Need: ptr = base + init_offset
     * Insert: ptr = base
     *         ptr = ptr + init_offset */
    int inserted = insert_instr_at(ir, insert_pos, TCCIR_OP_ASSIGN, ptr_op, div->base_op, null_op);
    LOG_IV_SR("IV_SR: init insert at pos=%d, result=%d", insert_pos, inserted);
    if (inserted < 0)
      return 0;
    idx_shift = 1;

    IROperand offset_op = irop_make_imm32(-1, init_offset, IROP_BTYPE_INT32);
    inserted = insert_instr_at(ir, insert_pos + 1, TCCIR_OP_ADD, ptr_op, ptr_op, offset_op);
    if (inserted < 0)
      return 1; /* Partial - at least did the assignment */
    idx_shift = 2;
  }

  /* After insertion, all indices >= insert_pos have shifted */

  /* Update our tracked indices */
  int new_use_idx = div->use_idx + idx_shift;
  int new_shl_idx = (div->shl_idx >= 0) ? div->shl_idx + idx_shift : -1;
  int new_iv_def_idx = iv->def_idx;
  if (iv->def_idx >= insert_pos)
    new_iv_def_idx += idx_shift;

  /* Step 2: Replace the use-site instruction with one that consumes the
   * strength-reduced pointer.
   *
   *   ADD / MLA       →  ASSIGN dest, ptr         (existing behavior — address
   *                                                temp now equals ptr)
   *   LOAD_INDEXED    →  LOAD   dest, *ptr        (new INDEXED-DIV path)
   *   STORE_INDEXED   →  STORE  *ptr, value       (new INDEXED-DIV path)
   *
   * For INDEXED forms the orig dest/src1 slot already holds the value temp;
   * we just need to update the address slot and the opcode.  The trailing
   * index/scale operands in the pool become orphaned — harmless. */
  IRQuadCompact *add_q = &ir->compact_instructions[new_use_idx];
  int rewrote_to_load_or_store = 0;
  if (add_q->op == TCCIR_OP_LOAD_INDEXED)
  {
    IROperand ptr_lval = ptr_op;
    ptr_lval.is_lval = 1;
    add_q->op = TCCIR_OP_LOAD;
    tcc_ir_op_set_src1(ir, add_q, ptr_lval);
    rewrote_to_load_or_store = 1;
    /* dest (loaded value temp) is preserved at slot 0. */
  }
  else if (add_q->op == TCCIR_OP_STORE_INDEXED)
  {
    IROperand ptr_lval = ptr_op;
    ptr_lval.is_lval = 1;
    add_q->op = TCCIR_OP_STORE;
    tcc_ir_op_set_dest(ir, add_q, ptr_lval);
    rewrote_to_load_or_store = 1;
    /* src1 (value to store) is preserved at slot 1. */
  }
  else
  {
    add_q->op = TCCIR_OP_ASSIGN;
    tcc_ir_op_set_src1(ir, add_q, ptr_op);
    tcc_ir_op_set_src2(ir, add_q, null_op);
    /* dest stays the same - it's the address temp that was being used.
     * For an MLA being rewritten, the accum operand at +3 is now orphaned. */
  }

  /* Step 2b (INDEXED-DIV only): synthesize a NOP slot immediately after the
   * rewritten LOAD/STORE so the later tcc_ir_opt_loop_postinc_fusion pass has
   * room to materialize the writeback-ASSIGN it needs.  Without this slot the
   * fusion bails out (`assign_nop = -1`) and the loop body is left with a
   * separate `ptr += stride` ADD that POSTINC would have absorbed.
   *
   * Communicates the NOP position back to the caller via out_postnop_origpos
   * (in pre-call original-index space, i.e., div->use_idx).  The caller folds
   * this third shift point into its APPLY_SHIFT bookkeeping for remaining DIVs,
   * IVs, and loop metadata. */
  if (rewrote_to_load_or_store)
  {
    int nop_pos = new_use_idx + 1;
    int inserted_nop = insert_instr_at(ir, nop_pos, TCCIR_OP_NOP, null_op, null_op, null_op);
    if (inserted_nop >= 0)
    {
      /* Update local indices: anything strictly after new_use_idx shifts by 1. */
      if (new_iv_def_idx > new_use_idx)
        new_iv_def_idx++;
      if (out_postnop_origpos)
        *out_postnop_origpos = div->use_idx;
    }
  }

  /* Step 3: NOP out the SHL/MUL instruction (skipped for fused MLA where
   * the multiply has no separate IR instruction). */
  if (new_shl_idx >= 0)
  {
    IRQuadCompact *shl_q = &ir->compact_instructions[new_shl_idx];
    shl_q->op = TCCIR_OP_NOP;
  }

  /* Step 4: Insert ptr += stride AFTER the IV increment.
   * In most loops the IV increment is at the latch (unconditional),
   * so the stride executes exactly once per iteration.
   *
   * Special case: in post-increment patterns like arglist[numargs++],
   * the IV increment sits in the body BEFORE the derived pointer use.
   * Placing the stride right after the increment would advance the
   * pointer before the store through it.  We must push the stride past
   * ALL uses of the derived address (not just the ASSIGN), because copy
   * propagation or register coalescing can merge the pointer with the
   * address temp, causing a store to see the post-increment value. */
  int stride_insert_pos = new_iv_def_idx + 1;
  if (new_use_idx > new_iv_def_idx)
  {
    int safe_to_push = 1;
    for (int si = new_iv_def_idx + 1; si <= new_use_idx; si++)
    {
      IRQuadCompact *sq = &ir->compact_instructions[si];
      if (sq->op == TCCIR_OP_JUMP || sq->op == TCCIR_OP_JUMPIF || sq->op == TCCIR_OP_FUNCCALLVAL ||
          sq->op == TCCIR_OP_FUNCCALLVOID)
      {
        safe_to_push = 0;
        break;
      }
    }
    if (safe_to_push)
    {
      /* Find the last use of the address temp (dest of the ASSIGN at new_use_idx)
       * within the same straight-line block.  The stride must go after the last
       * dereference through this pointer. */
      IROperand use_dest = tcc_ir_op_get_dest(ir, &ir->compact_instructions[new_use_idx]);
      int32_t use_dest_vr = irop_get_vreg(use_dest);
      int last_use = new_use_idx;
      if (use_dest_vr >= 0)
      {
        int loop_end = loop->end_idx + idx_shift;
        int saw_param_use = 0;
        for (int si = new_use_idx + 1; si <= loop_end; si++)
        {
          IRQuadCompact *sq = &ir->compact_instructions[si];
          if (sq->op == TCCIR_OP_NOP)
            continue;
          if (sq->op == TCCIR_OP_JUMP || sq->op == TCCIR_OP_JUMPIF)
            break;
          if (sq->op == TCCIR_OP_FUNCCALLVAL || sq->op == TCCIR_OP_FUNCCALLVOID)
          {
            /* If any preceding FUNCPARAMVAL passes our address, the actual
             * register read happens at the CALL (params are materialized
             * lazily at the call site).  Push the stride past the CALL. */
            if (saw_param_use)
              last_use = si;
            break;
          }
          int uses_it = 0;
          int defines_it = 0;
          if (irop_config[sq->op].has_dest)
          {
            IROperand d = tcc_ir_op_get_dest(ir, sq);
            if (irop_get_vreg(d) == use_dest_vr)
            {
              if (sq->op == TCCIR_OP_STORE || sq->op == TCCIR_OP_STORE_INDEXED ||
                  sq->op == TCCIR_OP_STORE_POSTINC)
                uses_it = 1;
              else
                defines_it = 1;
            }
          }
          if (irop_config[sq->op].has_src1)
          {
            IROperand s1 = tcc_ir_op_get_src1(ir, sq);
            if (irop_get_vreg(s1) == use_dest_vr)
              uses_it = 1;
          }
          if (irop_config[sq->op].has_src2)
          {
            IROperand s2 = tcc_ir_op_get_src2(ir, sq);
            if (irop_get_vreg(s2) == use_dest_vr)
              uses_it = 1;
          }
          if (defines_it && !uses_it)
            break;
          if (uses_it)
          {
            last_use = si;
            if (sq->op == TCCIR_OP_FUNCPARAMVAL)
              saw_param_use = 1;
          }
        }
      }
      stride_insert_pos = last_use + 1;
    }
  }
  IROperand stride_op = irop_make_imm32(-1, div->stride, IROP_BTYPE_INT32);

  int stride_inserted = insert_instr_at(ir, stride_insert_pos, TCCIR_OP_ADD, ptr_op, ptr_op, stride_op);
  LOG_IV_SR("IV_SR: stride insert at pos=%d, result=%d, new_iv_def=%d", stride_insert_pos, stride_inserted,
            new_iv_def_idx);
  if (stride_inserted < 0)
    return 2; /* Partial success - at least did the pointer init and use replacement */

  if (out_ptr_vreg)
    *out_ptr_vreg = ptr_vreg;
  if (out_idx_shift)
    *out_idx_shift = idx_shift;

  return 3; /* Full success: init + replace + stride */
}

/* Convert signed comparison condition to unsigned equivalent.
 * Pointer comparisons must use unsigned conditions since addresses
 * are unsigned quantities. Returns the unsigned token, or -1 if
 * the condition is not a simple signed relational. */

int signed_to_unsigned_cond(int cond_token)
{
  switch (cond_token)
  {
  case 0x9c: /* TOK_LT  → TOK_ULT */
    return 0x92;
  case 0x9d: /* TOK_GE  → TOK_UGE */
    return 0x93;
  case 0x9e: /* TOK_LE  → TOK_ULE */
    return 0x96;
  case 0x9f: /* TOK_GT  → TOK_UGT */
    return 0x97;
  case 0x94: /* TOK_EQ  - same for signed/unsigned */
    return 0x94;
  case 0x95: /* TOK_NE  - same for signed/unsigned */
    return 0x95;
  /* Already unsigned conditions — pass through */
  case 0x92: /* TOK_ULT */
    return 0x92;
  case 0x93: /* TOK_UGE */
    return 0x93;
  case 0x96: /* TOK_ULE */
    return 0x96;
  case 0x97: /* TOK_UGT */
    return 0x97;
  default:
    return -1;
  }
}

/* Try to eliminate the original IV counter after strength reduction has created
 * a derived pointer.  If the IV's only remaining uses are its own increment
 * and the loop exit CMP, we can replace the CMP with a pointer comparison
 * against a precomputed end address, making the IV completely dead.
 *
 * Before:  CMP i, #5; JUMPIF >=S exit   (signed comparison of index)
 *          i = i + 1
 *          ptr = ptr + 4
 *
 * After:   CMP ptr, end_ptr; JUMPIF >=U exit   (unsigned pointer comparison)
 *          ptr = ptr + 4
 *          (i is dead, eliminated by DCE)
 *
 * Parameters:
 *   ir       - IR state
 *   loop     - Loop structure (indices already shifted by transform_derived_iv)
 *   iv       - The basic induction variable (indices already shifted)
 *   div      - The derived IV info
 *   ptr_vreg - The vreg allocated for the pointer by transform_derived_iv
 *   idx_shift - Number of instructions inserted at the header by transform_derived_iv
 *
 * Returns 1 if elimination succeeded, 0 otherwise.
 */
int try_eliminate_iv_counter(TCCIRState *ir, IRLoop *loop, InductionVar *iv, DerivedIV *div, int ptr_vreg,
                                    int idx_shift)
{
  int n = ir->next_instruction_index;
  int iv_vr = iv->vreg;

  /* Adjusted loop indices (transform_derived_iv inserted instructions at header) */
  int adj_header = loop->header_idx + idx_shift;
  int adj_end = loop->end_idx + idx_shift;
  int adj_iv_def = iv->def_idx;
  if (iv->def_idx >= loop->header_idx)
    adj_iv_def += idx_shift;

  /* Step 1a: Find the CMP + JUMPIF pre-test guard that tests the IV.
   * After loop rotation and IV strength reduction insertions, the pre-test
   * guard CMP is typically in the preheader area (just before the header).
   * Scan from the preheader up through a few instructions past the header. */
  int hdr_cmp_idx = -1, hdr_jmpif_idx = -1;
  int limit_val = 0, hdr_cond_token = 0;

  {
    int scan_start = loop->preheader_idx;
    if (scan_start < 0)
      scan_start = adj_header > 4 ? adj_header - 4 : 0;
    int scan_end = adj_header + 4;
    if (scan_end >= n - 1)
      scan_end = n - 2;

    for (int i = scan_start; i <= scan_end; i++)
    {
      IRQuadCompact *cq = &ir->compact_instructions[i];
      if (cq->op != TCCIR_OP_CMP)
        continue;

      IROperand cmp_src1 = tcc_ir_op_get_src1(ir, cq);
      IROperand cmp_src2 = tcc_ir_op_get_src2(ir, cq);

      if (irop_get_vreg(cmp_src1) != iv_vr || !irop_is_immediate(cmp_src2))
        continue;

      /* Look forward for the JUMPIF, skipping NOPs and ASSIGNs (which don't clobber flags) */
      int jq_idx = i + 1;
      while (jq_idx < n && (ir->compact_instructions[jq_idx].op == TCCIR_OP_NOP ||
                            ir->compact_instructions[jq_idx].op == TCCIR_OP_ASSIGN))
        jq_idx++;

      if (jq_idx >= n)
        continue;
      IRQuadCompact *jq = &ir->compact_instructions[jq_idx];
      if (jq->op != TCCIR_OP_JUMPIF)
        continue;

      IROperand cond_op = tcc_ir_op_get_src1(ir, jq);
      hdr_cond_token = (int)irop_get_imm64_ex(ir, cond_op);

      /* Exit target must be outside the loop */
      IROperand jmp_dest = tcc_ir_op_get_dest(ir, jq);
      int exit_target = (int)irop_get_imm64_ex(ir, jmp_dest);
      if (exit_target <= adj_end + 1) /* +1 for the stride ADD we inserted */
        continue;

      limit_val = (int)irop_get_imm64_ex(ir, cmp_src2);
      hdr_cmp_idx = i;
      hdr_jmpif_idx = jq_idx;
      break;
    }
  }

  /* Step 1b: Find the CMP + JUMPIF near the back-edge (post-test / latch test).
   * This is typically just before the back-edge JUMP at adj_end. Scan backward
   * from adj_end looking for a CMP of the IV against the same limit. */
  int be_cmp_idx = -1, be_jmpif_idx = -1;
  int be_cond_token = 0;

  for (int i = adj_end; i >= adj_end - 5 && i >= 0; i--)
  {
    IRQuadCompact *cq = &ir->compact_instructions[i];
    if (cq->op != TCCIR_OP_CMP)
      continue;

    IROperand cmp_src1 = tcc_ir_op_get_src1(ir, cq);
    IROperand cmp_src2 = tcc_ir_op_get_src2(ir, cq);

    if (irop_get_vreg(cmp_src1) != iv_vr || !irop_is_immediate(cmp_src2))
      continue;

    /* Look forward for the JUMPIF */
    int jq_idx = i + 1;
    while (jq_idx < n && (ir->compact_instructions[jq_idx].op == TCCIR_OP_NOP ||
                          ir->compact_instructions[jq_idx].op == TCCIR_OP_ASSIGN))
      jq_idx++;

    if (jq_idx >= n)
      continue;
    IRQuadCompact *jq = &ir->compact_instructions[jq_idx];
    if (jq->op != TCCIR_OP_JUMPIF)
      continue;

    IROperand cond_op = tcc_ir_op_get_src1(ir, jq);
    be_cond_token = (int)irop_get_imm64_ex(ir, cond_op);

    /* Back-edge target must be inside the loop (jumps back) */
    IROperand jmp_dest = tcc_ir_op_get_dest(ir, jq);
    int back_target = (int)irop_get_imm64_ex(ir, jmp_dest);
    if (back_target > adj_end)
      continue; /* Not a back-edge */

    int be_limit = (int)irop_get_imm64_ex(ir, cmp_src2);

    /* Use limit_val from header if found, otherwise from back-edge */
    if (hdr_cmp_idx < 0)
      limit_val = be_limit;

    be_cmp_idx = i;
    be_jmpif_idx = jq_idx;
    break;
  }

  /* We need at least one CMP to proceed */
  if (hdr_cmp_idx < 0 && be_cmp_idx < 0)
  {
    LOG_IV_SR("IV_SR_ELIM: No CMP+JUMPIF found for IV VAR%d at header %d or back-edge %d",
              TCCIR_DECODE_VREG_POSITION(iv_vr), adj_header, adj_end);
    return 0;
  }

  if (TCC_LOG_IV_SR)
    fprintf(stderr, "[IV_SR_ELIM] hdr_cmp_idx=%d, be_cmp_idx=%d, adj_iv_def=%d, adj_iv_init=%d\n", hdr_cmp_idx,
            be_cmp_idx, adj_iv_def, iv->init_idx);

  /* Step 2: Check that the IV has no other uses besides:
   *   - The header CMP instruction (pre-test, if present)
   *   - The back-edge CMP instruction (post-test, if present)
   *   - Its own increment (adj_iv_def)
   *   - A copy-through temp (ASSIGN T=V just before the ADD)
   * If the IV is used elsewhere (e.g., as a function argument), we can't eliminate it. */
  int other_uses = 0;
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;
    if (i == hdr_cmp_idx || i == be_cmp_idx)
      continue; /* The CMPs we'll replace/remove */
    if (i == adj_iv_def)
      continue; /* The IV increment */

    /* Allow the copy-through pattern: T = V just before V = T + 1 */
    if (q->op == TCCIR_OP_ASSIGN && i >= adj_iv_def - 2 && i < adj_iv_def)
    {
      IROperand asrc = tcc_ir_op_get_src1(ir, q);
      if (irop_get_vreg(asrc) == iv_vr)
        continue; /* This is the copy-through temp */
    }

    /* Check src1 and src2 for uses of the IV */
    if (irop_config[q->op].has_src1)
    {
      IROperand s1 = tcc_ir_op_get_src1(ir, q);
      if (irop_get_vreg(s1) == iv_vr)
      {
        other_uses++;
        if (TCC_LOG_IV_SR)
          fprintf(stderr, "[IV_SR_ELIM] other_uses++ at idx=%d (src1) op=%d\n", i, q->op);
      }
    }
    if (irop_config[q->op].has_src2)
    {
      IROperand s2 = tcc_ir_op_get_src2(ir, q);
      if (irop_get_vreg(s2) == iv_vr)
      {
        other_uses++;
        if (TCC_LOG_IV_SR)
          fprintf(stderr, "[IV_SR_ELIM] other_uses++ at idx=%d (src2) op=%d\n", i, q->op);
      }
    }
  }

  if (other_uses > 0)
  {
    LOG_IV_SR("IV_SR_ELIM: IV VAR%d has %d other uses, cannot eliminate", TCCIR_DECODE_VREG_POSITION(iv_vr),
              other_uses);
    return 0;
  }

  /* Step 3: Compute end_ptr = base + limit * element_size
   * element_size = stride / step (e.g., stride=4, step=1 → element_size=4)
   * end_value = limit * element_size */
  int element_size = div->stride / iv->step;
  int end_offset = limit_val * element_size;

  /* Allocate a vreg for end_ptr */
  int end_vreg = tcc_ir_vreg_alloc_temp(ir);
  if (end_vreg < 0)
    return 0;

  IROperand end_op = irop_make_vreg(end_vreg, IROP_BTYPE_INT32);
  IROperand ptr_op = irop_make_vreg(ptr_vreg, IROP_BTYPE_INT32);
  IROperand null_op = {0};

  /* Insert end_ptr computation in preheader (before the loop header).
   * We insert at adj_header (which is already shifted).
   *
   * Use the strength-reduced ptr_vreg as the source rather than div->base_op
   * whenever ptr's value at this point equals base.  That holds when
   * iv.init_val == 0 — transform_derived_iv inserted `ptr = base` (a single
   * ASSIGN) and nothing has mutated ptr yet.  Using ptr_op saves an extra
   * materialization of the base address: ptr is already register-resident,
   * whereas base_op (a SYMREF/STACKOFF/etc) would require a second LDR [PC]
   * or LEA to bring into a register.
   *
   * When end_offset != 0, also emit a single non-destructive ADD (dest != src1)
   *   end_ptr = src + end_offset
   * rather than ASSIGN end_ptr = src; end_ptr += off.  On Thumb-2 the former
   * is one wide `add.w rd, rn, #imm` instruction; the latter is two. */
  IROperand end_src = (iv->init_val == 0) ? ptr_op : div->base_op;
  int insert_pos = adj_header;
  int end_shift = 0;
  int inserted;

  if (end_offset != 0)
  {
    IROperand offset_op = irop_make_imm32(-1, end_offset, IROP_BTYPE_INT32);
    inserted = insert_instr_at(ir, insert_pos, TCCIR_OP_ADD, end_op, end_src, offset_op);
    if (inserted < 0)
      return 0;
    end_shift = 1;
  }
  else
  {
    inserted = insert_instr_at(ir, insert_pos, TCCIR_OP_ASSIGN, end_op, end_src, null_op);
    if (inserted < 0)
      return 0;
    end_shift = 1;
  }

  /* Update indices after insertion — only shift those at or after insert_pos */
  if (hdr_cmp_idx >= 0 && hdr_cmp_idx >= insert_pos)
  {
    hdr_cmp_idx += end_shift;
    hdr_jmpif_idx += end_shift;
  }
  if (be_cmp_idx >= 0 && be_cmp_idx >= insert_pos)
  {
    be_cmp_idx += end_shift;
    be_jmpif_idx += end_shift;
  }

  /* Step 4: Replace the back-edge CMP with pointer comparison (ptr vs end_ptr).
   * This is the primary loop continuation test. */
  if (be_cmp_idx >= 0)
  {
    int be_unsigned_cond = signed_to_unsigned_cond(be_cond_token);
    if (be_unsigned_cond < 0)
    {
      LOG_IV_SR("IV_SR_ELIM: Unsupported back-edge condition token 0x%x", be_cond_token);
      return 0;
    }

    IRQuadCompact *be_cmp_q = &ir->compact_instructions[be_cmp_idx];
    tcc_ir_op_set_src1(ir, be_cmp_q, ptr_op);
    tcc_ir_op_set_src2(ir, be_cmp_q, end_op);

    IRQuadCompact *be_jmp_q = &ir->compact_instructions[be_jmpif_idx];
    IROperand be_new_cond = irop_make_imm32(-1, be_unsigned_cond, IROP_BTYPE_INT32);
    tcc_ir_op_set_src1(ir, be_jmp_q, be_new_cond);
  }

  /* Step 5: Handle the header pre-test guard.
   * If the initial value satisfies the exit condition (e.g., init=0 < limit=5),
   * the pre-test is always false (loop always executes), so we can NOP it out.
   * Otherwise, replace with pointer comparison. */
  if (hdr_cmp_idx >= 0)
  {
    /* If there's a back-edge CMP, the pre-test is just a guard and can potentially
     * be NOP'd away. If the pre-test is the ONLY loop exit test (no back-edge CMP),
     * we must keep it and replace with pointer comparison — NOP'ing it would make
     * the loop infinite. */
    if (be_cmp_idx >= 0)
    {
      /* Back-edge test exists. Check if the pre-test can be constant-folded away.
       * The header tests: if (iv_init <cond> limit) goto exit
       * If this is always false for the initial value, the guard is redundant. */
      int guard_always_false = evaluate_compare_condition((int64_t)iv->init_val, (int64_t)limit_val, hdr_cond_token);

      if (!guard_always_false)
      {
        /* Guard is never taken — NOP out both CMP and JUMPIF */
        ir->compact_instructions[hdr_cmp_idx].op = TCCIR_OP_NOP;
        ir->compact_instructions[hdr_jmpif_idx].op = TCCIR_OP_NOP;
      }
      else
      {
        /* Guard might be taken — replace with pointer comparison */
        int hdr_unsigned_cond = signed_to_unsigned_cond(hdr_cond_token);
        if (hdr_unsigned_cond >= 0)
        {
          IRQuadCompact *hdr_cmp_q = &ir->compact_instructions[hdr_cmp_idx];
          tcc_ir_op_set_src1(ir, hdr_cmp_q, ptr_op);
          tcc_ir_op_set_src2(ir, hdr_cmp_q, end_op);

          IRQuadCompact *hdr_jmp_q = &ir->compact_instructions[hdr_jmpif_idx];
          IROperand hdr_new_cond = irop_make_imm32(-1, hdr_unsigned_cond, IROP_BTYPE_INT32);
          tcc_ir_op_set_src1(ir, hdr_jmp_q, hdr_new_cond);
        }
      }
    }
    else
    {
      /* Pre-test is the only loop exit — must replace with pointer comparison */
      int hdr_unsigned_cond = signed_to_unsigned_cond(hdr_cond_token);
      if (hdr_unsigned_cond >= 0)
      {
        IRQuadCompact *hdr_cmp_q = &ir->compact_instructions[hdr_cmp_idx];
        tcc_ir_op_set_src1(ir, hdr_cmp_q, ptr_op);
        tcc_ir_op_set_src2(ir, hdr_cmp_q, end_op);

        IRQuadCompact *hdr_jmp_q = &ir->compact_instructions[hdr_jmpif_idx];
        IROperand hdr_new_cond = irop_make_imm32(-1, hdr_unsigned_cond, IROP_BTYPE_INT32);
        tcc_ir_op_set_src1(ir, hdr_jmp_q, hdr_new_cond);
      }
    }
  }

  /* Step 6: NOP out the now-dead IV initialization and increment.
   * DCE cannot eliminate self-referential cycles (V = V + 1 uses itself),
   * so we must explicitly remove them.
   *
   * Index adjustments:
   * - iv->init_idx is in the preheader (before header_idx), not shifted by any insertions
   * - adj_iv_def was already adjusted for idx_shift; needs end_shift added for our insertions */
  {
    /* NOP the IV initialization (in preheader, not shifted) */
    int adj_iv_init = iv->init_idx;
    if (adj_iv_init >= 0 && adj_iv_init < ir->next_instruction_index)
    {
      IRQuadCompact *init_q = &ir->compact_instructions[adj_iv_init];
      IROperand init_dest = tcc_ir_op_get_dest(ir, init_q);
      if (irop_get_vreg(init_dest) == iv_vr)
        init_q->op = TCCIR_OP_NOP;
    }

    /* NOP the IV increment (in loop body, shifted by both idx_shift and end_shift) */
    int adj_iv_inc = adj_iv_def + end_shift;
    if (adj_iv_inc >= 0 && adj_iv_inc < ir->next_instruction_index)
    {
      IRQuadCompact *inc_q = &ir->compact_instructions[adj_iv_inc];
      IROperand inc_dest = tcc_ir_op_get_dest(ir, inc_q);
      if (irop_get_vreg(inc_dest) == iv_vr)
        inc_q->op = TCCIR_OP_NOP;

      /* Also NOP the copy-through temp (T1 = V1) that precedes V1 = T1 + 1 */
      for (int k = adj_iv_inc - 1; k >= adj_iv_inc - 3 && k >= 0; k--)
      {
        IRQuadCompact *cq = &ir->compact_instructions[k];
        if (cq->op == TCCIR_OP_NOP)
          continue;
        if (cq->op == TCCIR_OP_ASSIGN)
        {
          IROperand csrc = tcc_ir_op_get_src1(ir, cq);
          if (irop_get_vreg(csrc) == iv_vr)
          {
            cq->op = TCCIR_OP_NOP;
            break;
          }
        }
        break; /* Stop at first non-NOP, non-matching */
      }
    }
  }

  LOG_IV_SR("IV_SR_ELIM: Eliminated IV VAR%d, replaced CMP with ptr(TMP%d) vs end(TMP%d), "
            "end_offset=%d, hdr_cmp=%d, be_cmp=%d",
            TCCIR_DECODE_VREG_POSITION(iv_vr), TCCIR_DECODE_VREG_POSITION(ptr_vreg),
            TCCIR_DECODE_VREG_POSITION(end_vreg), end_offset, hdr_cmp_idx, be_cmp_idx);

  return 1;
}

/* Main entry point: Induction Variable Strength Reduction
 * Returns number of transformations applied
 */
/* Core IV strength reduction using pre-detected loops */

int iv_strength_reduction_core(TCCIRState *ir, IRLoops *loops)
{
  int total_changes = 0;

  LOG_IV_SR("IV_SR: Found %d loop(s)", loops->num_loops);

  /* Process each loop, but only process loops with valid preheaders */
  for (int li = 0; li < loops->num_loops; li++)
  {
    IRLoop *loop = &loops->loops[li];

    if (loop->preheader_idx < 0)
      continue;

    /* Skip if inserting at preheader+1 would land inside a CHILD loop's
     * body range (a smaller loop contained within ours).  Inserting inside
     * a parent loop is fine — that's the expected case for inner loops. */
    {
      int insert_pos = loop->preheader_idx + 1;
      int loop_size = loop->end_idx - loop->start_idx;
      int skip = 0;
      for (int other = 0; other < loops->num_loops; other++)
      {
        if (other == li)
          continue;
        IRLoop *oloop = &loops->loops[other];
        int oloop_size = oloop->end_idx - oloop->start_idx;
        if (insert_pos > oloop->start_idx && insert_pos <= oloop->end_idx && oloop_size < loop_size)
        {
          skip = 1;
          break;
        }
      }
      if (skip)
        continue;
    }

    InductionVar ivs[MAX_IV];
    DerivedIV divs[MAX_DIV];

    int num_ivs = find_induction_vars_ex(ir, loop, ivs, MAX_IV, 1);
    if (num_ivs == 0)
      continue;

    int num_divs = find_derived_ivs(ir, loop, ivs, num_ivs, divs, MAX_DIV);
    if (num_divs == 0)
      continue;

    LOG_IV_SR("IV_SR: Found %d DIV(s) in loop %d", num_divs, li);

    /* Deduplicate DIVs that compute identical (iv, stride, base) recurrences.
     * Without this, N identical MLAs (e.g. arr[i].a, arr[i].b, arr[i].c each
     * computing the same &arr[i]) would each get its own strength-reduced
     * pointer, requiring N pointer bumps in the latch — strictly worse than
     * the original.  Mark each duplicate's share_with field with the index of
     * the earliest equivalent DIV, so the transform can rewrite them to
     * ASSIGN dest, primary_ptr instead of allocating fresh pointers. */
    for (int dj = 1; dj < num_divs; dj++)
    {
      for (int dk = 0; dk < dj; dk++)
      {
        if (divs[dk].share_with >= 0)
          continue; /* only chain to primaries */
        if (divs[dj].iv_idx != divs[dk].iv_idx || divs[dj].stride != divs[dk].stride)
          continue;
        /* Compare base operands: same vreg, or same immediate value, or same stack offset. */
        IROperand a = divs[dj].base_op;
        IROperand b = divs[dk].base_op;
        int tag_a = irop_get_tag(a);
        int tag_b = irop_get_tag(b);
        if (tag_a != tag_b)
          continue;
        int eq = 0;
        if (tag_a == IROP_TAG_IMM32 || tag_a == IROP_TAG_STACKOFF)
          eq = (a.u.imm32 == b.u.imm32 && a.is_lval == b.is_lval);
        else if (tag_a == IROP_TAG_VREG)
        {
          int32_t va = irop_get_vreg(a);
          int32_t vb = irop_get_vreg(b);
          if (va >= 0 && va == vb && a.is_lval == b.is_lval)
            eq = 1;
        }
        if (eq)
        {
          divs[dj].share_with = dk;
          LOG_IV_SR("IV_SR: DIV %d shares pointer with DIV %d (iv_idx=%d stride=%d)", dj, dk, divs[dj].iv_idx,
                    divs[dj].stride);
          break;
        }
      }
    }

    /* Transform each derived IV, deferring IV elimination to pick the
     * cheapest end-pointer across all transformed DIVs. */
    int div_ptr_vregs[MAX_DIV];
    int div_changes[MAX_DIV];
    for (int di = 0; di < num_divs; di++)
    {
      div_ptr_vregs[di] = -1;
      div_changes[di] = 0;
    }

    for (int di = 0; di < num_divs; di++)
    {
      int sr_ptr_vreg = -1, sr_idx_shift = 0;
      int sr_postnop_origpos = -1;
      int shared = -1;
      if (divs[di].share_with >= 0)
      {
        /* Use primary's already-allocated ptr (must have been processed first
         * given the dedup invariant share_with < di and we iterate in order). */
        shared = div_ptr_vregs[divs[di].share_with];
        if (shared < 0)
        {
          LOG_IV_SR("IV_SR: skipping shared DIV %d — primary %d not transformed", di, divs[di].share_with);
          continue;
        }
      }
      int changes =
          transform_derived_iv(ir, loop, &ivs[divs[di].iv_idx], &divs[di], &sr_ptr_vreg, &sr_idx_shift,
                               &sr_postnop_origpos, shared);
      total_changes += changes;
      div_ptr_vregs[di] = sr_ptr_vreg;
      div_changes[di] = changes;

      /* After transformation, indices have shifted.  insert_instr_at
       * shifts all instructions >= pos by +1 per insertion.  Apply the
       * same position-aware shift to remaining loops' metadata.
       *
       * Insertion points (before any shifting):
       *   init_pos   = preheader_idx + 1 (sr_idx_shift instructions)
       *   postnop    = div->use_idx + 1 (1 instruction, INDEXED-DIV only)
       *   stride_pos = iv->def_idx + sr_idx_shift + 1 (1 instruction)
       */
      if (changes > 0)
      {
        /* Compute shift for each original-space index.  Insertions:
         *   sr_idx_shift instructions at init_pos = preheader_idx + 1
         *   1 instruction at use_idx + 1 (only when sr_postnop_origpos >= 0)
         *   1 instruction at def_idx + 1 (only when changes >= 3)
         * Since init_pos < use_idx + 1 < def_idx + 1 always holds for a
         * single loop body, the total shift for original index X:
         *   X < init_pos                  → 0
         *   init_pos <= X <= use_idx      → sr_idx_shift
         *   use_idx < X <= def_idx        → sr_idx_shift + 1 (postnop only)
         *   X > def_idx (and changes>=3)  → sr_idx_shift + (1|0) + 1
         */
        int init_pos = loop->preheader_idx + 1;
        int orig_def_idx = ivs[divs[di].iv_idx].def_idx;
        int has_stride = (changes >= 3);
        int has_postnop = (sr_postnop_origpos >= 0);
        int orig_use_for_nop = sr_postnop_origpos; /* div->use_idx in pre-call space */

#define APPLY_SHIFT(idx)                                                                                               \
  do                                                                                                                   \
  {                                                                                                                    \
    int _orig = (idx);                                                                                                 \
    if (_orig >= init_pos)                                                                                             \
    {                                                                                                                  \
      (idx) = _orig + sr_idx_shift;                                                                                    \
      if (has_postnop && _orig > orig_use_for_nop)                                                                     \
        (idx)++;                                                                                                       \
      if (has_stride && _orig > orig_def_idx)                                                                          \
        (idx)++;                                                                                                       \
    }                                                                                                                  \
  } while (0)

        /* Shift indices of remaining DIVs and all IVs so we can
         * continue processing more DIVs in this loop. */
        for (int dj = di + 1; dj < num_divs; dj++)
        {
          APPLY_SHIFT(divs[dj].use_idx);
          APPLY_SHIFT(divs[dj].shl_idx);
        }
        for (int ij = 0; ij < num_ivs; ij++)
        {
          APPLY_SHIFT(ivs[ij].def_idx);
          APPLY_SHIFT(ivs[ij].init_idx);
        }

        /* Shift current loop metadata */
        APPLY_SHIFT(loop->header_idx);
        APPLY_SHIFT(loop->start_idx);
        APPLY_SHIFT(loop->end_idx);
        if (loop->preheader_idx >= 0)
          APPLY_SHIFT(loop->preheader_idx);
        for (int bi = 0; bi < loop->num_body_instrs; bi++)
          APPLY_SHIFT(loop->body_instrs[bi]);

#undef APPLY_SHIFT

        /* After insertions, later loops' metadata is stale.
         * Break out — the caller re-invokes with fresh loop detection.
         * Already-transformed DIVs won't re-match (SHL/MUL are NOP'd). */
        if (di == num_divs - 1)
          goto try_elim;
      }
    }
    continue;

  try_elim:
    /* All DIVs in this loop are transformed.  Now try IV elimination
     * with each candidate, preferring the one whose end-pointer is
     * cheapest to materialize.
     *
     * Heuristic: prefer stack-based base (SP-relative end = single ADD)
     * over immediate base.  Among same-kind bases, prefer smaller
     * absolute end_offset (fewer bits to encode). */
    {
      int best_di = -1;
      int best_cost = 0x7fffffff;

      for (int di = 0; di < num_divs; di++)
      {
        if (div_changes[di] != 3 || div_ptr_vregs[di] < 0)
          continue;

        int element_size = divs[di].stride / ivs[divs[di].iv_idx].step;
        int abs_end_offset = ivs[divs[di].iv_idx].init_val * element_size;
        if (abs_end_offset < 0)
          abs_end_offset = -abs_end_offset;

        int cost;
        if (irop_get_tag(divs[di].base_op) == IROP_TAG_STACKOFF)
          cost = abs_end_offset;
        else
          cost = abs_end_offset + 0x10000;

        if (cost < best_cost)
        {
          best_cost = cost;
          best_di = di;
        }
      }

      if (best_di >= 0)
      {
        /* idx_shift=0 because APPLY_SHIFT already updated all loop/IV indices
         * to current (post-all-transforms) positions. */
        int elim =
            try_eliminate_iv_counter(ir, loop, &ivs[divs[best_di].iv_idx], &divs[best_di], div_ptr_vregs[best_di], 0);
        total_changes += elim;
      }
    }
    goto done;
  }

done:
  LOG_IV_SR("=== IV STRENGTH REDUCTION END: %d changes ===", total_changes);

  return total_changes;
}


int find_loop_exit_condition(TCCIRState *ir, IRLoop *loop, int iv_vreg, int *out_cmp_idx, int *out_jmpif_idx,
                                    int *out_limit, int *out_cond, int *out_exit_target)
{
  /* Define scan ranges: header region and tail region */
  int ranges[2][2] = {
      {loop->header_idx, loop->header_idx + 3}, /* top-tested */
      {loop->end_idx - 3, loop->end_idx}        /* bottom-tested (rotated) */
  };

  for (int r = 0; r < 2; r++)
  {
    int scan_start = ranges[r][0];
    int scan_end = ranges[r][1];
    if (scan_start < loop->start_idx)
      scan_start = loop->start_idx;
    if (scan_end > loop->end_idx)
      scan_end = loop->end_idx;

    LOG_LOOP_OPT("find_loop_exit_condition: iv_vreg=VAR%d %s scan [%d..%d]", TCCIR_DECODE_VREG_POSITION(iv_vreg),
                 r == 0 ? "header" : "tail", scan_start, scan_end);

    for (int i = scan_start; i <= scan_end && i < ir->next_instruction_index - 1; i++)
    {
      IRQuadCompact *cq = &ir->compact_instructions[i];
      if (cq->op != TCCIR_OP_CMP)
      {
        LOG_LOOP_OPT("[%d] op=%d (not CMP), skipping", i, cq->op);
        continue;
      }

      IROperand cmp_src1 = tcc_ir_op_get_src1(ir, cq);
      IROperand cmp_src2 = tcc_ir_op_get_src2(ir, cq);

      /* Check: CMP Viv, #limit */
      int32_t vr1 = irop_get_vreg(cmp_src1);
      if (vr1 != iv_vreg || !irop_is_immediate(cmp_src2))
      {
        LOG_LOOP_OPT("[%d] CMP but vr1=%d (want %d) imm=%d, skipping", i,
                     vr1 >= 0 ? TCCIR_DECODE_VREG_POSITION(vr1) : -1, TCCIR_DECODE_VREG_POSITION(iv_vreg),
                     irop_is_immediate(cmp_src2));
        continue;
      }

      int limit = (int)irop_get_imm64_ex(ir, cmp_src2);

      /* Next instruction must be JUMPIF */
      IRQuadCompact *jq = &ir->compact_instructions[i + 1];
      if (jq->op != TCCIR_OP_JUMPIF)
      {
        LOG_LOOP_OPT("[%d] CMP ok but [%d] is op=%d (not JUMPIF)", i, i + 1, jq->op);
        continue;
      }

      IROperand cond_op = tcc_ir_op_get_src1(ir, jq);
      int cond = (int)irop_get_imm64_ex(ir, cond_op);

      IROperand jmp_dest = tcc_ir_op_get_dest(ir, jq);
      int jmp_target = (int)irop_get_imm64_ex(ir, jmp_dest);

      /* Top-tested: exit target is outside the loop */
      if (jmp_target > loop->end_idx)
      {
        LOG_LOOP_OPT("[%d] FOUND top-tested exit: CMP VAR%d,#%d JUMPIF@%d exit=%d cond=%d", i,
                     TCCIR_DECODE_VREG_POSITION(iv_vreg), limit, i + 1, jmp_target, cond);
        *out_cmp_idx = i;
        *out_jmpif_idx = i + 1;
        *out_limit = limit;
        *out_cond = cond;
        *out_exit_target = jmp_target;
        return 1;
      }

      /* Bottom-tested (rotated): JUMPIF is the back-edge, exit is fall-through.
       * The back-edge target is inside the loop, and the condition is inverted
       * (loop continues when condition holds, exits on fall-through).
       * We need to invert the condition so callers see it as an exit condition. */
      if (jmp_target >= loop->start_idx && jmp_target <= loop->end_idx)
      {
        int exit_target = i + 2; /* fall-through past the JUMPIF */
        /* Invert the condition: the JUMPIF continues the loop, so the exit
         * condition is the opposite. */
        int inv_cond;
        switch (cond)
        {
        case TOK_GE:
          inv_cond = TOK_LT;
          break;
        case TOK_GT:
          inv_cond = TOK_LE;
          break;
        case TOK_LT:
          inv_cond = TOK_GE;
          break;
        case TOK_LE:
          inv_cond = TOK_GT;
          break;
        case TOK_EQ:
          inv_cond = TOK_NE;
          break;
        case TOK_NE:
          inv_cond = TOK_EQ;
          break;
        default:
          inv_cond = -1;
          break;
        }
        if (inv_cond < 0)
        {
          LOG_LOOP_OPT("[%d] bottom-tested but can't invert cond=%d", i, cond);
          continue;
        }
        LOG_LOOP_OPT("[%d] FOUND bottom-tested exit: CMP VAR%d,#%d JUMPIF@%d back=%d exit=%d cond=%d->%d", i,
                     TCCIR_DECODE_VREG_POSITION(iv_vreg), limit, i + 1, jmp_target, exit_target, cond, inv_cond);
        *out_cmp_idx = i;
        *out_jmpif_idx = i + 1;
        *out_limit = limit;
        *out_cond = inv_cond;
        *out_exit_target = exit_target;
        return 1;
      }

      LOG_LOOP_OPT("[%d] CMP+JUMPIF found but target=%d doesn't match any pattern", i, jmp_target);
    }
  }
  LOG_LOOP_OPT("-> exit condition NOT FOUND");
  return 0;
}

/* Compute the trip count for a loop given IV init, limit, step, and condition.
 * Returns trip count >= 0, or -1 if it cannot be computed.
 * Uses int64_t internally to avoid signed overflow when init_val and limit
 * are far apart (e.g. 0x60000000 and 0xA0000000 in 32-bit signed). */
int compute_trip_count(int init_val, int limit, int step, int cond_token)
{
  if (step <= 0)
    return -1;

  int64_t range = (int64_t)limit - (int64_t)init_val;

  LOG_LOOP_OPT("compute_trip_count: init=%d limit=%d step=%d cond=%d range=%lld", init_val, limit, step, cond_token,
               (long long)range);

  switch (cond_token)
  {
  case TOK_GE: /* exit if iv >= limit → loop while iv < limit */
    if (range <= 0)
      return 0;
    return (int)((range + step - 1) / step);

  case TOK_GT: /* exit if iv > limit → loop while iv <= limit */
    if (range < 0)
      return 0;
    return (int)(range / step + 1);

  case TOK_NE: /* exit if iv != limit → loop until iv == limit */
    if (range < 0)
      return -1;
    if (range == 0)
      return 0;
    if (range % step != 0)
      return -1; /* would loop forever */
    return (int)(range / step);

  default:
    return -1;
  }
}

/* Collect the body instructions to clone (excluding loop control flow and IV update).
 * Returns count of body instructions, fills body_indices[]. */
int collect_body_instructions(TCCIRState *ir, IRLoop *loop, int iv_vreg, int cmp_idx, int jmpif_idx,
                                     int iv_def_idx, int *body_indices, int max_body)
{
  int count = 0;
  /* Scan only [start_idx..end_idx].  The forward-jump extension in the loop
   * detector can pull in post-loop instructions (e.g. the exit target), which
   * must NOT be treated as body.  The merge pass already ensures end_idx
   * covers all body instructions from overlapping loops. */
  for (int i = loop->start_idx; i <= loop->end_idx && count < max_body; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];

    /* Skip NOP */
    if (q->op == TCCIR_OP_NOP)
      continue;

    /* Skip CMP and JUMPIF (exit condition) */
    if (i == cmp_idx || i == jmpif_idx)
      continue;

    /* Skip all unconditional jumps (loop structure) */
    if (q->op == TCCIR_OP_JUMP)
      continue;

    /* Skip IV increment */
    if (i == iv_def_idx)
      continue;

    /* Skip the ASSIGN that saves old IV for post-increment pattern:
     * T = Viv  (where T is only used by the IV ADD on the next line) */
    if (q->op == TCCIR_OP_ASSIGN && i == iv_def_idx - 1)
    {
      IROperand src1 = tcc_ir_op_get_src1(ir, q);
      if (irop_get_vreg(src1) == iv_vreg)
        continue;
    }

    /* Reject bodies with internal branches (too complex for v1) */
    if (q->op == TCCIR_OP_JUMPIF)
    {
      LOG_LOOP_OPT("collect_body: REJECTED at [%d] internal JUMPIF", i);
      return -1;
    }

    /* Reject bodies with calls (side effects) */
    if (q->op == TCCIR_OP_FUNCCALLVAL || q->op == TCCIR_OP_FUNCCALLVOID || q->op == TCCIR_OP_FUNCPARAMVAL ||
        q->op == TCCIR_OP_FUNCPARAMVOID)
    {
      LOG_LOOP_OPT("collect_body: REJECTED at [%d] function call op=%d", i, q->op);
      return -1;
    }

    /* Reject inline asm */
    if (q->op == TCCIR_OP_INLINE_ASM)
    {
      LOG_LOOP_OPT("collect_body: REJECTED at [%d] inline asm", i);
      return -1;
    }

    /* Reject ops that store a 4th operand at pool[base+3]. write_instr_at_nop
     * only copies dest/src1/src2 (3 slots), so an unrolled copy of these ops
     * loses the 4th slot (scale, accumulator, condition, post-inc offset) and
     * produces wrong code.  These ops are added by later fusion passes, so the
     * body sometimes contains them after loop-rotation+fusion. */
    if (q->op == TCCIR_OP_LOAD_INDEXED || q->op == TCCIR_OP_STORE_INDEXED ||
        q->op == TCCIR_OP_LOAD_POSTINC || q->op == TCCIR_OP_STORE_POSTINC ||
        q->op == TCCIR_OP_MLA || q->op == TCCIR_OP_SELECT)
    {
      LOG_LOOP_OPT("collect_body: REJECTED at [%d] 4-operand op=%d", i, q->op);
      return -1;
    }

    LOG_LOOP_OPT("collect_body: body[%d] = instr [%d] op=%d", count, i, q->op);
    body_indices[count++] = i;
  }

  LOG_LOOP_OPT("collect_body: collected %d body instruction(s)", count);
  return count;
}

/* Write an instruction into a NOP slot at position pos.
 * The slot MUST already be NOP. */
void write_instr_at_nop(TCCIRState *ir, int pos, TccIrOp op, IROperand dest, IROperand src1, IROperand src2)
{
  IRQuadCompact *q = &ir->compact_instructions[pos];
  q->op = op;
  q->is_jump_target = 0;
  /* Only write operand slots that the instruction uses, so the pool
   * layout matches what codegen expects (it reads at base + has_dest
   * for src1, etc.).  Writing an unused dest slot would misalign. */
  int base = ir->iroperand_pool_count;
  if (irop_config[op].has_dest)
    tcc_ir_pool_add(ir, dest);
  if (irop_config[op].has_src1)
    tcc_ir_pool_add(ir, src1);
  if (irop_config[op].has_src2)
    tcc_ir_pool_add(ir, src2);
  q->operand_base = base;
}

/* Write a SELECT into a NOP slot.  SELECT needs four pool entries:
 * dest, then-value, else-value, cond-token at pool[base+3]. */
void write_select_at_nop(TCCIRState *ir, int pos, IROperand dest, IROperand then_val,
                                IROperand else_val, int cond_tok)
{
  IRQuadCompact *q = &ir->compact_instructions[pos];
  q->op = TCCIR_OP_SELECT;
  q->is_jump_target = 0;
  IROperand cond_op = irop_make_imm32(-1, cond_tok, IROP_BTYPE_INT32);
  int base = tcc_ir_pool_add(ir, dest);
  tcc_ir_pool_add(ir, then_val);
  tcc_ir_pool_add(ir, else_val);
  tcc_ir_pool_add(ir, cond_op);
  q->operand_base = base;
}

/* Find the loop exit condition with a possibly-symbolic limit.
 * Mirrors find_loop_exit_condition but returns the limit operand (vreg or
 * immediate) instead of only an integer.  Used by the symbolic-limit
 * eliminator.  Returns 1 if found, 0 otherwise. */
int find_loop_exit_condition_op(TCCIRState *ir, IRLoop *loop, int iv_vreg, int *out_cmp_idx,
                                       int *out_jmpif_idx, IROperand *out_limit_op, int *out_cond,
                                       int *out_exit_target)
{
  int ranges[2][2] = {
      {loop->header_idx, loop->header_idx + 3},
      {loop->end_idx - 3, loop->end_idx},
  };
  for (int r = 0; r < 2; r++)
  {
    int scan_start = ranges[r][0];
    int scan_end = ranges[r][1];
    if (scan_start < loop->start_idx)
      scan_start = loop->start_idx;
    if (scan_end > loop->end_idx)
      scan_end = loop->end_idx;
    for (int i = scan_start; i <= scan_end && i < ir->next_instruction_index - 1; i++)
    {
      IRQuadCompact *cq = &ir->compact_instructions[i];
      if (cq->op != TCCIR_OP_CMP)
        continue;
      IROperand cmp_src1 = tcc_ir_op_get_src1(ir, cq);
      IROperand cmp_src2 = tcc_ir_op_get_src2(ir, cq);
      int32_t vr1 = irop_get_vreg(cmp_src1);
      if (vr1 != iv_vreg)
        continue;
      /* Accept either immediate OR a plain vreg (no DEREF/sym/complex). */
      int src2_is_imm = irop_is_immediate(cmp_src2);
      int src2_is_simple_vreg =
          (!cmp_src2.is_lval && !cmp_src2.is_sym && !cmp_src2.is_complex && irop_get_vreg(cmp_src2) >= 0);
      if (!src2_is_imm && !src2_is_simple_vreg)
        continue;
      IRQuadCompact *jq = &ir->compact_instructions[i + 1];
      if (jq->op != TCCIR_OP_JUMPIF)
        continue;
      IROperand cond_op = tcc_ir_op_get_src1(ir, jq);
      int cond = (int)irop_get_imm64_ex(ir, cond_op);
      IROperand jmp_dest = tcc_ir_op_get_dest(ir, jq);
      int jmp_target = (int)irop_get_imm64_ex(ir, jmp_dest);
      if (jmp_target > loop->end_idx)
      {
        *out_cmp_idx = i;
        *out_jmpif_idx = i + 1;
        *out_limit_op = cmp_src2;
        *out_cond = cond;
        *out_exit_target = jmp_target;
        return 1;
      }
      if (jmp_target >= loop->start_idx && jmp_target <= loop->end_idx)
      {
        int exit_target = i + 2;
        int inv_cond;
        switch (cond)
        {
        case TOK_GE: inv_cond = TOK_LT; break;
        case TOK_GT: inv_cond = TOK_LE; break;
        case TOK_LT: inv_cond = TOK_GE; break;
        case TOK_LE: inv_cond = TOK_GT; break;
        case TOK_EQ: inv_cond = TOK_NE; break;
        case TOK_NE: inv_cond = TOK_EQ; break;
        default: inv_cond = -1; break;
        }
        if (inv_cond < 0)
          continue;
        *out_cmp_idx = i;
        *out_jmpif_idx = i + 1;
        *out_limit_op = cmp_src2;
        *out_cond = inv_cond;
        *out_exit_target = exit_target;
        return 1;
      }
    }
  }
  return 0;
}

int try_eliminate_loop_symbolic(TCCIRState *ir, IRLoop *loop)
{
  LOG_LOOP_OPT("try_eliminate_loop_symbolic: header=%d start=%d end=%d preheader=%d", loop->header_idx,
               loop->start_idx, loop->end_idx, loop->preheader_idx);

  InductionVar ivs[MAX_IV];
  int num_ivs = find_induction_vars_ex(ir, loop, ivs, MAX_IV, 1);
  if (num_ivs < 1)
    return 0;

  /* Find a counter IV (step=1) whose exit condition has a symbolic limit. */
  int cmp_idx = -1, jmpif_idx = -1, cond = -1, exit_target = -1;
  IROperand limit_op = {0};
  InductionVar *counter_iv = NULL;
  for (int k = 0; k < num_ivs; k++)
  {
    int ci, ji, c, et;
    IROperand lop;
    if (find_loop_exit_condition_op(ir, loop, ivs[k].vreg, &ci, &ji, &lop, &c, &et))
    {
      if (irop_is_immediate(lop))
        continue; /* constant limit — try_eliminate_loop handles this. */
      if (ivs[k].step != 1 || ivs[k].init_val != 0)
        continue; /* keep v1 restricted to the common case. */
      counter_iv = &ivs[k];
      cmp_idx = ci; jmpif_idx = ji; limit_op = lop; cond = c; exit_target = et;
      break;
    }
  }
  if (!counter_iv)
  {
    LOG_LOOP_OPT("try_eliminate_loop_symbolic: no symbolic-limit counter IV");
    return 0;
  }

  /* Condition must be one we know how to invert into a step direction. */
  if (cond != TOK_GE && cond != TOK_LT && cond != TOK_GT && cond != TOK_LE)
    return 0;

  /* Verify the loop body contains ONLY IV updates / copy-throughs / NOP /
   * JUMP / the exit CMP+JUMPIF.  Same body shape try_eliminate_loop demands. */
  for (int i = loop->start_idx; i <= loop->end_idx; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP || q->op == TCCIR_OP_JUMP)
      continue;
    if (i == cmp_idx || i == jmpif_idx)
      continue;
    int is_iv_def = 0;
    for (int k = 0; k < num_ivs; k++)
    {
      if (i == ivs[k].def_idx) { is_iv_def = 1; break; }
    }
    if (is_iv_def)
      continue;
    if (q->op == TCCIR_OP_ASSIGN)
    {
      int is_iv_copy = 0;
      for (int k = 0; k < num_ivs; k++)
      {
        if (i == ivs[k].def_idx - 1)
        {
          IROperand src1 = tcc_ir_op_get_src1(ir, q);
          if (irop_get_vreg(src1) == ivs[k].vreg) { is_iv_copy = 1; break; }
        }
      }
      if (is_iv_copy)
        continue;
    }
    LOG_LOOP_OPT("try_eliminate_loop_symbolic: BLOCKED by instr [%d] op=%d", i, q->op);
    return 0;
  }

  /* Compute how many slots we need: one MUL per used-after accumulator, plus
   * an optional ADD when init_acc != 0, plus an ASSIGN for the counter final
   * value if it's read after the loop.  Bail out if we won't fit in the
   * loop region (avoids needing IR growth here). */
  int slots_avail = loop->end_idx - loop->start_idx + 1;
  int slots_needed = 0;
  int per_iv_writes[MAX_IV] = {0};
  for (int k = 0; k < num_ivs; k++)
  {
    int used_after = 0;
    for (int j = exit_target; j < ir->next_instruction_index && !used_after; j++)
    {
      IRQuadCompact *q = &ir->compact_instructions[j];
      if (q->op == TCCIR_OP_NOP)
        continue;
      if (irop_config[q->op].has_src1 && irop_get_vreg(tcc_ir_op_get_src1(ir, q)) == ivs[k].vreg)
        used_after = 1;
      if (irop_config[q->op].has_src2 && irop_get_vreg(tcc_ir_op_get_src2(ir, q)) == ivs[k].vreg)
        used_after = 1;
    }
    if (!used_after)
      continue;
    if (&ivs[k] == counter_iv)
    {
      per_iv_writes[k] = 1; /* ASSIGN V_iv = limit */
      slots_needed += 1;
    }
    else
    {
      /* Accumulator: MUL + optional ADD */
      per_iv_writes[k] = (ivs[k].init_val == 0) ? 1 : 2;
      slots_needed += per_iv_writes[k];
    }
  }
  if (slots_needed > slots_avail)
  {
    LOG_LOOP_OPT("try_eliminate_loop_symbolic: needs %d slots, only %d avail", slots_needed, slots_avail);
    return 0;
  }

  LOG_IR_GEN("[LOOP-ELIM-SYM] Eliminating loop header=%d num_ivs=%d slots_needed=%d", loop->header_idx, num_ivs,
             slots_needed);

  /* Look for the pre-loop entry guard: CMP iv,limit / JUMPIF (cond) exit_target
   * in the preheader region [counter_init_idx+1 .. start_idx-1].  When found,
   * and when there's exactly one used-after accumulator with init_acc=0 and no
   * used-after counter, we can rewrite the whole thing into a single ITE-style
   * SELECT: emit MUL into the counter init slot, replace the guard CMP with
   * CMP limit,#0 (saves the materialise-zero+reg-cmp pair), and replace the
   * guard JUMPIF with SELECT V_acc, T_mul, #0, cond=GT.  Codegen lowers SELECT
   * to an ITE block. */
  int guard_cmp = -1, guard_jmpif = -1;
  for (int g = counter_iv->init_idx + 1; g < loop->start_idx; g++)
  {
    IRQuadCompact *gq = &ir->compact_instructions[g];
    if (gq->op == TCCIR_OP_CMP)
    {
      IROperand gsrc1 = tcc_ir_op_get_src1(ir, gq);
      if (irop_get_vreg(gsrc1) == counter_iv->vreg && g + 1 < loop->start_idx)
      {
        IRQuadCompact *gjq = &ir->compact_instructions[g + 1];
        if (gjq->op == TCCIR_OP_JUMPIF)
        {
          guard_cmp = g;
          guard_jmpif = g + 1;
          break;
        }
      }
    }
  }

  /* Count used-after accumulators (non-counter) and detect the simple case. */
  int num_acc_used = 0;
  int single_acc_idx = -1;
  int counter_used_after = 0;
  for (int k = 0; k < num_ivs; k++)
  {
    if (per_iv_writes[k] == 0)
      continue;
    if (&ivs[k] == counter_iv)
      counter_used_after = 1;
    else
    {
      num_acc_used++;
      single_acc_idx = k;
    }
  }

  int use_select_path =
      (guard_cmp >= 0 && guard_jmpif >= 0 && num_acc_used == 1 && !counter_used_after &&
       ivs[single_acc_idx].init_val == 0);

  /* NOP the loop body in both paths. */
  for (int i = loop->start_idx; i <= loop->end_idx; i++)
    ir->compact_instructions[i].op = TCCIR_OP_NOP;

  if (use_select_path)
  {
    InductionVar *acc_iv = &ivs[single_acc_idx];
    IROperand step_imm = irop_make_imm32(-1, acc_iv->step, IROP_BTYPE_INT32);
    IROperand zero_imm = irop_make_imm32(-1, 0, IROP_BTYPE_INT32);
    IROperand acc_dest = irop_make_vreg(acc_iv->vreg, IROP_BTYPE_INT32);

    /* Layout: CMP → MUL → SELECT.
     *   slot=counter_init_idx → CMP limit,#0
     *   slot=guard_cmp        → MUL V_acc = limit * step_acc
     *   slot=guard_jmpif      → SELECT V_acc = (cond=GT) ? V_acc : 0
     *
     * Putting CMP before MUL frees the limit register at MUL time so the
     * regalloc can place V_acc in the same physical register as limit —
     * no end-of-function move from V_acc to the return register is needed.
     *
     * The flag-liveness tracker in codegen (codegen_flags_live) is set by
     * the CMP and consumed by the SELECT; intermediate MUL ops are emitted
     * with FLAGS_BEHAVIOUR_BLOCK so they preserve the CMP-set flags.
     *
     * SELECT's identity-then path (then == dest vreg) collapses the usual
     * ITE+two-movs into IT+single-mov. */
    ir->compact_instructions[counter_iv->init_idx].op = TCCIR_OP_NOP;
    write_instr_at_nop(ir, counter_iv->init_idx, TCCIR_OP_CMP, (IROperand){0}, limit_op, zero_imm);

    {
      IRQuadCompact *gq = &ir->compact_instructions[guard_cmp];
      gq->op = TCCIR_OP_MUL;
      /* MUL needs dest + src1 + src2.  CMP only set src1/src2, so re-add a
       * dest slot at the start of fresh pool entries. */
      int new_base = tcc_ir_pool_add(ir, acc_dest);
      tcc_ir_pool_add(ir, limit_op);
      tcc_ir_pool_add(ir, step_imm);
      gq->operand_base = new_base;
    }

    if (acc_iv->init_idx >= 0 && acc_iv->init_idx != counter_iv->init_idx &&
        acc_iv->init_idx != guard_cmp)
      ir->compact_instructions[acc_iv->init_idx].op = TCCIR_OP_NOP;

    ir->compact_instructions[guard_jmpif].op = TCCIR_OP_NOP;
    write_select_at_nop(ir, guard_jmpif, acc_dest, acc_dest, zero_imm, TOK_GT);

    return 1;
  }

  /* Fallback: ASSIGN-based closed form (kept for cases the SELECT path
   * doesn't cover: init_acc != 0, multiple accumulators, counter used
   * after, or no pre-loop guard present). */
  int write_pos = loop->start_idx;
  for (int k = 0; k < num_ivs; k++)
  {
    if (per_iv_writes[k] == 0)
      continue;
    InductionVar *iv = &ivs[k];
    IROperand acc_dest = irop_make_vreg(iv->vreg, IROP_BTYPE_INT32);
    if (iv == counter_iv)
    {
      write_instr_at_nop(ir, write_pos++, TCCIR_OP_ASSIGN, acc_dest, limit_op, (IROperand){0});
    }
    else
    {
      IROperand step_imm = irop_make_imm32(-1, iv->step, IROP_BTYPE_INT32);
      if (iv->init_val == 0)
      {
        write_instr_at_nop(ir, write_pos++, TCCIR_OP_MUL, acc_dest, limit_op, step_imm);
      }
      else
      {
        write_instr_at_nop(ir, write_pos++, TCCIR_OP_MUL, acc_dest, limit_op, step_imm);
        IROperand init_imm = irop_make_imm32(-1, iv->init_val, IROP_BTYPE_INT32);
        IROperand acc_src = irop_make_vreg(iv->vreg, IROP_BTYPE_INT32);
        write_instr_at_nop(ir, write_pos++, TCCIR_OP_ADD, acc_dest, acc_src, init_imm);
      }
    }
  }

  return 1;
}

/* Try to eliminate a loop entirely by computing final IV values.
 * This handles loops whose body consists only of induction variable
 * updates (no side effects, no calls, no branches).
 * Example: for (i=0; i<10; i++) count++ → count = 10
 * Returns 1 if eliminated, 0 otherwise. */
int try_eliminate_loop(TCCIRState *ir, IRLoop *loop)
{
  LOG_LOOP_OPT("try_eliminate_loop: header=%d start=%d end=%d preheader=%d", loop->header_idx, loop->start_idx,
               loop->end_idx, loop->preheader_idx);
  InductionVar ivs[MAX_IV];
  int num_ivs = find_induction_vars_ex(ir, loop, ivs, MAX_IV, 1 /* allow copy-through */);
  if (num_ivs < 1)
  {
    LOG_LOOP_OPT("try_eliminate_loop: no IVs found, giving up");
    return 0;
  }

  /* Find the primary IV — the one referenced in the loop exit condition. */
  int cmp_idx, jmpif_idx, limit, cond, exit_target;
  InductionVar *primary_iv = NULL;
  for (int k = 0; k < num_ivs; k++)
  {
    if (find_loop_exit_condition(ir, loop, ivs[k].vreg, &cmp_idx, &jmpif_idx, &limit, &cond, &exit_target))
    {
      primary_iv = &ivs[k];
      LOG_LOOP_OPT("try_eliminate_loop: primary IV=VAR%d (init=%d, step=%d)",
                   TCCIR_DECODE_VREG_POSITION(primary_iv->vreg), primary_iv->init_val, primary_iv->step);
      break;
    }
  }
  if (!primary_iv)
  {
    LOG_LOOP_OPT("try_eliminate_loop: no primary IV (exit condition not found for any IV)");
    return 0;
  }

  int trip_count = compute_trip_count(primary_iv->init_val, limit, primary_iv->step, cond);
  if (trip_count <= 0)
  {
    LOG_LOOP_OPT("try_eliminate_loop: trip_count=%d (invalid), giving up", trip_count);
    return 0;
  }
  LOG_LOOP_OPT("try_eliminate_loop: trip_count=%d limit=%d", trip_count, limit);

  /* Verify the loop body contains ONLY IV updates.
   * After removing: NOPs, JUMPs, CMP+JUMPIF, and all IV defs and their
   * copy-through temps, nothing should remain. */
  for (int i = loop->start_idx; i <= loop->end_idx; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];

    if (q->op == TCCIR_OP_NOP || q->op == TCCIR_OP_JUMP)
      continue;
    if (i == cmp_idx || i == jmpif_idx)
      continue;

    /* Check if this instruction is an IV definition */
    int is_iv_def = 0;
    for (int k = 0; k < num_ivs; k++)
    {
      if (i == ivs[k].def_idx)
      {
        is_iv_def = 1;
        break;
      }
    }
    if (is_iv_def)
      continue;

    /* Check if this is a copy-through temp for any IV:
     * ASSIGN where dest is a temp and src is an IV vreg,
     * immediately before that IV's def instruction */
    if (q->op == TCCIR_OP_ASSIGN)
    {
      int is_iv_copy = 0;
      for (int k = 0; k < num_ivs; k++)
      {
        if (i == ivs[k].def_idx - 1)
        {
          IROperand src1 = tcc_ir_op_get_src1(ir, q);
          if (irop_get_vreg(src1) == ivs[k].vreg)
          {
            is_iv_copy = 1;
            break;
          }
        }
      }
      if (is_iv_copy)
        continue;
    }

    /* Any other instruction — loop has side effects, can't eliminate */
    LOG_LOOP_OPT("try_eliminate_loop: BLOCKED by instr [%d] op=%d (not IV/NOP/JUMP/CMP)", i, q->op);
    return 0;
  }

  /* Also verify no backward jumps escape the loop */
  for (int i = loop->start_idx; i <= loop->end_idx; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_JUMP)
    {
      IROperand jd = tcc_ir_op_get_dest(ir, q);
      int target = (int)irop_get_imm64_ex(ir, jd);
      if (target < loop->start_idx)
        return 0;
    }
  }

  LOG_IR_GEN("[LOOP-ELIM] Eliminating loop header=%d trip_count=%d num_ivs=%d", loop->header_idx, trip_count, num_ivs);

  /* NOP the entire loop */
  for (int i = loop->start_idx; i <= loop->end_idx; i++)
    ir->compact_instructions[i].op = TCCIR_OP_NOP;

  /* Write final value assignments for all IVs used after the loop */
  int write_pos = loop->start_idx;
  for (int k = 0; k < num_ivs; k++)
  {
    int iv_final = ivs[k].init_val + trip_count * ivs[k].step;

    /* Check if this IV is used after the loop */
    int used_after = 0;
    for (int j = exit_target; j < ir->next_instruction_index; j++)
    {
      IRQuadCompact *q = &ir->compact_instructions[j];
      if (q->op == TCCIR_OP_NOP)
        continue;
      if (irop_config[q->op].has_src1 && irop_get_vreg(tcc_ir_op_get_src1(ir, q)) == ivs[k].vreg)
      {
        used_after = 1;
        break;
      }
      if (irop_config[q->op].has_src2 && irop_get_vreg(tcc_ir_op_get_src2(ir, q)) == ivs[k].vreg)
      {
        used_after = 1;
        break;
      }
    }

    if (used_after && write_pos <= loop->end_idx)
    {
      IROperand dest = irop_make_vreg(ivs[k].vreg, IROP_BTYPE_INT32);
      IROperand val = irop_make_imm32(-1, iv_final, IROP_BTYPE_INT32);
      write_instr_at_nop(ir, write_pos++, TCCIR_OP_ASSIGN, dest, val, (IROperand){0});
    }

    /* NOP the initialization too */
    if (ivs[k].init_idx >= 0)
    {
      ir->compact_instructions[ivs[k].init_idx].op = TCCIR_OP_NOP;

      /* For bottom-tested (rotated) loops, also NOP the pre-loop guard that
       * tests this IV.  Since trip_count > 0, the guard is dead code, and
       * leaving it with a NOP'd IV init would read an undefined register. */
      for (int g = ivs[k].init_idx + 1; g < loop->start_idx; g++)
      {
        IRQuadCompact *gq = &ir->compact_instructions[g];
        if (gq->op == TCCIR_OP_CMP)
        {
          IROperand gsrc1 = tcc_ir_op_get_src1(ir, gq);
          if (irop_get_vreg(gsrc1) == ivs[k].vreg && g + 1 < loop->start_idx)
          {
            IRQuadCompact *gjq = &ir->compact_instructions[g + 1];
            if (gjq->op == TCCIR_OP_JUMPIF)
            {
              LOG_LOOP_OPT("NOP'ing pre-loop guard CMP@%d + JUMPIF@%d", g, g + 1);
              gq->op = TCCIR_OP_NOP;
              gjq->op = TCCIR_OP_NOP;
            }
          }
        }
      }
    }
  }

  return 1;
}

/* Try to unroll a single loop. Returns 1 if unrolled, 0 otherwise.
 * `loops` and `loop_idx` let us patch sibling loop records when we have to
 * grow the IR (insert NOP slots).  Without that, sibling loops keep stale
 * start_idx/end_idx values and a later try_unroll_loop call mangles unrelated
 * instructions.  Pass NULL/0 when no sibling tracking is needed. */
int try_unroll_loop_ex(TCCIRState *ir, IRLoop *loop, IRLoops *loops, int loop_idx)
{
  LOG_LOOP_OPT("try_unroll_loop: header=%d start=%d end=%d preheader=%d", loop->header_idx, loop->start_idx,
               loop->end_idx, loop->preheader_idx);
  InductionVar ivs[MAX_IV];
  int num_ivs = find_induction_vars_ex(ir, loop, ivs, MAX_IV, 1 /* allow copy-through */);
  if (num_ivs < 1)
  {
    LOG_LOOP_OPT("try_unroll_loop: no IVs found, giving up");
    return 0;
  }

  /* Find the primary IV — the one referenced in the loop exit condition.
   * Accumulators (e.g. sum += const) also match the IV pattern but are not
   * used in the exit CMP; they are handled as regular body instructions. */
  int cmp_idx, jmpif_idx, limit, cond, exit_target;
  InductionVar *iv = NULL;
  for (int k = 0; k < num_ivs; k++)
  {
    if (find_loop_exit_condition(ir, loop, ivs[k].vreg, &cmp_idx, &jmpif_idx, &limit, &cond, &exit_target))
    {
      iv = &ivs[k];
      break;
    }
  }
  if (!iv)
  {
    LOG_LOOP_OPT("try_unroll_loop: no primary IV (exit condition not found)");
    return 0;
  }

  int trip_count = compute_trip_count(iv->init_val, limit, iv->step, cond);
  if (trip_count <= 0 || trip_count > UNROLL_MAX_TRIP_COUNT)
  {
    LOG_LOOP_OPT("try_unroll_loop: trip_count=%d (invalid or > %d), giving up", trip_count, UNROLL_MAX_TRIP_COUNT);
    return 0;
  }

  int body_indices[UNROLL_MAX_BODY_INSNS];
  int body_count = collect_body_instructions(ir, loop, iv->vreg, cmp_idx, jmpif_idx, iv->def_idx, body_indices,
                                             UNROLL_MAX_BODY_INSNS);
  if (body_count <= 0 || body_count > UNROLL_MAX_BODY_INSNS)
  {
    LOG_LOOP_OPT("try_unroll_loop: body_count=%d (invalid or > %d), giving up", body_count, UNROLL_MAX_BODY_INSNS);
    return 0;
  }

  int total_insns = trip_count * body_count;
  if (total_insns > UNROLL_MAX_TOTAL_INSNS)
  {
    LOG_LOOP_OPT("try_unroll_loop: total_insns=%d > %d, giving up", total_insns, UNROLL_MAX_TOTAL_INSNS);
    return 0;
  }

  /* Save original opcodes and operands for body instructions before NOP'ing.
   * The write loop needs original data, but NOP slots may be overwritten by
   * earlier iterations (a body instruction's slot can be reused for unrolled
   * output, destroying the operand_base). */
  int body_ops[UNROLL_MAX_BODY_INSNS];
  IROperand body_dests[UNROLL_MAX_BODY_INSNS];
  IROperand body_src1s[UNROLL_MAX_BODY_INSNS];
  IROperand body_src2s[UNROLL_MAX_BODY_INSNS];
  for (int b = 0; b < body_count; b++)
  {
    IRQuadCompact *bq = &ir->compact_instructions[body_indices[b]];
    int op = bq->op;
    body_ops[b] = op;
    body_dests[b] = (IROperand){0};
    body_src1s[b] = (IROperand){0};
    body_src2s[b] = (IROperand){0};
    if (irop_config[op].has_dest)
      body_dests[b] = ir->iroperand_pool[bq->operand_base];
    if (irop_config[op].has_src1)
      body_src1s[b] = ir->iroperand_pool[bq->operand_base + irop_config[op].has_dest];
    if (irop_config[op].has_src2)
      body_src2s[b] = ir->iroperand_pool[bq->operand_base + irop_config[op].has_dest + irop_config[op].has_src1];
  }

  /* Use only the [start_idx..end_idx] range for NOP/write region.
   * Do NOT use the extended body_instrs — the forward-jump extension
   * can include post-loop instructions that must not be touched. */
  int loop_end = loop->end_idx;

  /* The unrolled body needs trip_count*body_count slots plus 1 optional slot
   * for the IV final value (if used after the loop).  When the original loop
   * region is too small, insert NOPs immediately after loop_end and extend
   * loop_end to cover them.  insert_instr_at shifts subsequent instructions
   * and patches all jump targets that point at or past the insertion site.
   * Indices inside [start_idx..loop_end] (body_indices, cmp_idx, jmpif_idx,
   * iv->def_idx, iv->init_idx) are unchanged; exit_target sits after the loop
   * and must be shifted manually. */
  int avail_slots = loop_end - loop->start_idx + 1;
  int needed_slots = total_insns + 1; /* +1 reserved for IV final assignment */
  /* Only grow the IR (and ripple-update sibling loop records) when this is
   * the sole loop being processed.  In multi-loop functions the cross-loop
   * book-keeping is fragile — even with body_instrs/start/end fix-up some
   * regalloc-visible state (e.g. live ranges that span the inserted NOP
   * region) ends up stale and corrupts unrelated loops.  This keeps the
   * single-loop win from test_mla_fusion / inline test cases without
   * breaking multi-loop ones like 110_iv_strength_reduction. */
  if (needed_slots > avail_slots && (!loops || loops->num_loops != 1))
    return 0;
  if (needed_slots > avail_slots)
  {
    int extra = needed_slots - avail_slots;
    int insert_pos = loop_end + 1;
    int orig_end = loop->end_idx;
    IROperand none_op = (IROperand){0};
    for (int k = 0; k < extra; k++)
    {
      if (insert_instr_at(ir, insert_pos, TCCIR_OP_NOP, none_op, none_op, none_op) < 0)
        return 0;
    }
    loop_end += extra;
    if (exit_target > orig_end)
      exit_target += extra;
    /* Keep this loop's record consistent so any later analysis sees the
     * extended range. */
    loop->end_idx = loop_end;
    /* Patch sibling loop records: insertions at orig_end+1 shifted every
     * later position by +extra.  Without this, a later try_unroll_loop call
     * would NOP unrelated instructions and corrupt the program. */
    if (loops)
    {
      for (int li = 0; li < loops->num_loops; li++)
      {
        if (li == loop_idx)
          continue;
        IRLoop *other = &loops->loops[li];
        if (other->start_idx < 0)
          continue;
        if (other->start_idx > orig_end)
          other->start_idx += extra;
        if (other->end_idx > orig_end)
          other->end_idx += extra;
        if (other->header_idx > orig_end)
          other->header_idx += extra;
        if (other->preheader_idx > orig_end)
          other->preheader_idx += extra;
        for (int b = 0; b < other->num_body_instrs; b++)
        {
          if (other->body_instrs[b] > orig_end)
            other->body_instrs[b] += extra;
        }
      }
    }
  }

  for (int i = loop->start_idx; i <= loop_end; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_JUMP && i != loop->end_idx)
    {
      IROperand jd = tcc_ir_op_get_dest(ir, q);
      int target = (int)irop_get_imm64_ex(ir, jd);
      if (target < i && target < loop->start_idx)
        return 0; /* Backward jump escaping the loop — nested or malformed */
    }
  }

  LOG_IR_GEN("[UNROLL] Unrolling loop header=%d trip_count=%d body_count=%d", loop->header_idx, trip_count, body_count);

  /* NOP out the entire loop region */
  for (int i = loop->start_idx; i <= loop_end; i++)
    ir->compact_instructions[i].op = TCCIR_OP_NOP;

  /* NOP out IV initialization in preheader */
  if (iv->init_idx >= 0)
    ir->compact_instructions[iv->init_idx].op = TCCIR_OP_NOP;

  /* For bottom-tested (rotated) loops, there is a guard CMP+JUMPIF before the
   * loop that tests the IV and jumps past the loop if it shouldn't execute.
   * Since trip_count > 0 (we are unrolling), the guard is dead code.
   * We must NOP it because we NOP'd the IV init above, leaving the guard's
   * IV operand undefined. */
  if (iv->init_idx >= 0)
  {
    for (int g = iv->init_idx + 1; g < loop->start_idx; g++)
    {
      IRQuadCompact *gq = &ir->compact_instructions[g];
      if (gq->op == TCCIR_OP_CMP)
      {
        IROperand gsrc1 = tcc_ir_op_get_src1(ir, gq);
        if (irop_get_vreg(gsrc1) == iv->vreg && g + 1 < loop->start_idx)
        {
          IRQuadCompact *gjq = &ir->compact_instructions[g + 1];
          if (gjq->op == TCCIR_OP_JUMPIF)
          {
            LOG_LOOP_OPT("NOP'ing pre-loop guard CMP@%d + JUMPIF@%d", g, g + 1);
            gq->op = TCCIR_OP_NOP;
            gjq->op = TCCIR_OP_NOP;
          }
        }
      }
    }
  }

  /* Also NOP the "JMP to body" that may precede the loop header
   * (instruction at start_idx - 1 if it's a jump into the loop body) */

  /* Write unrolled copies into the NOP'd slots */
  int write_pos = loop->start_idx;

  for (int k = 0; k < trip_count; k++)
  {
    for (int b = 0; b < body_count; b++)
    {
      int saved_op = body_ops[b];
      IROperand dest = body_dests[b];
      IROperand src1 = body_src1s[b];
      IROperand src2 = body_src2s[b];

      /* Substitute IV references in src operands with constant value for this iteration */
      int iv_val = iv->init_val + k * iv->step;
      IROperand iv_const = irop_make_imm32(-1, iv_val, IROP_BTYPE_INT32);

      if (irop_get_vreg(src1) == iv->vreg)
        src1 = iv_const;
      if (irop_get_vreg(src2) == iv->vreg)
        src2 = iv_const;

      /* Find next NOP slot to write into */
      while (write_pos <= loop_end && ir->compact_instructions[write_pos].op != TCCIR_OP_NOP)
        write_pos++;

      if (write_pos > loop_end)
        return 0; /* Should not happen — avail_slots check above prevents this */

      write_instr_at_nop(ir, write_pos, saved_op, dest, src1, src2);
      write_pos++;
    }
  }

  /* If the IV is used after the loop, set its final value.
   * Check if iv vreg is referenced anywhere after the loop. */
  {
    int iv_used_after = 0;
    for (int i = exit_target; i < ir->next_instruction_index; i++)
    {
      IRQuadCompact *q = &ir->compact_instructions[i];
      if (q->op == TCCIR_OP_NOP)
        continue;
      if (irop_config[q->op].has_src1)
      {
        IROperand s1 = tcc_ir_op_get_src1(ir, q);
        if (irop_get_vreg(s1) == iv->vreg)
        {
          iv_used_after = 1;
          break;
        }
      }
      if (irop_config[q->op].has_src2)
      {
        IROperand s2 = tcc_ir_op_get_src2(ir, q);
        if (irop_get_vreg(s2) == iv->vreg)
        {
          iv_used_after = 1;
          break;
        }
      }
    }

    if (iv_used_after)
    {
      /* Write final IV value into a NOP slot before exit_target */
      int iv_final = iv->init_val + trip_count * iv->step;
      IROperand iv_dest = irop_make_vreg(iv->vreg, IROP_BTYPE_INT32);
      IROperand iv_val_op = irop_make_imm32(-1, iv_final, IROP_BTYPE_INT32);

      /* Find a NOP slot */
      for (int i = write_pos; i <= loop_end; i++)
      {
        if (ir->compact_instructions[i].op == TCCIR_OP_NOP)
        {
          write_instr_at_nop(ir, i, TCCIR_OP_ASSIGN, iv_dest, iv_val_op, (IROperand){0});
          break;
        }
      }
    }
  }

  return 1;
}


int try_rotate_loop(TCCIRState *ir, IRLoop *loop)
{
  int hi = loop->header_idx;
  int n = ir->next_instruction_index;

  /* --- Step 1: Validate header pattern --- */

  /* Need at least 3 instructions: CMP, JUMPIF, JUMP */
  if (hi + 2 > loop->end_idx)
    return 0;

  IRQuadCompact *cmp_q = &ir->compact_instructions[hi];
  IRQuadCompact *jif_q = &ir->compact_instructions[hi + 1];
  IRQuadCompact *jmp_q = &ir->compact_instructions[hi + 2];

  if (cmp_q->op != TCCIR_OP_CMP)
    return 0;
  if (jif_q->op != TCCIR_OP_JUMPIF)
    return 0;
  if (jmp_q->op != TCCIR_OP_JUMP)
    return 0;

  /* Get exit target and condition */
  IROperand exit_dest = tcc_ir_op_get_dest(ir, jif_q);
  int exit_target = (int)irop_get_imm64_ex(ir, exit_dest);
  IROperand cond_op = tcc_ir_op_get_src1(ir, jif_q);
  int cond = (int)irop_get_imm64_ex(ir, cond_op);

  /* Get body-entry target */
  IROperand body_entry_dest = tcc_ir_op_get_dest(ir, jmp_q);
  int body_start = (int)irop_get_imm64_ex(ir, body_entry_dest);

  /* --- Step 2: Find the back-edge JUMP targeting the header --- */
  /* Don't use loop->end_idx directly — when the loop detector merges
   * the body→latch jump as part of the loop, end_idx covers the body
   * too.  Instead, scan from hi+3 for the first JUMP targeting hi. */
  int backedge_idx = -1;
  for (int i = hi + 3; i <= loop->end_idx; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_JUMP)
    {
      IROperand jd = tcc_ir_op_get_dest(ir, q);
      int jt = (int)irop_get_imm64_ex(ir, jd);
      if (jt == hi)
      {
        backedge_idx = i;
        break;
      }
    }
  }
  if (backedge_idx < 0)
  {
    LOG_LOOP_OPT("Rotation: reject — no back-edge JUMP to header %d", hi);
    return 0;
  }
  LOG_LOOP_OPT("Rotation: backedge at %d, exit_target=%d, body_start=%d", backedge_idx, exit_target, body_start);

  /* Exit must jump outside the loop's core region [hi, backedge_idx].
   * The extended end_idx may include exit targets due to aggressive
   * loop extension, so use the actual backedge position instead. */
  if (exit_target > hi && exit_target <= backedge_idx)
  {
    LOG_LOOP_OPT("Rotation: reject — exit_target %d inside [%d,%d]", exit_target, hi, backedge_idx);
    return 0;
  }

  /* Body must be after the back-edge (standard TCC layout) */
  if (body_start <= backedge_idx || body_start >= n)
  {
    LOG_LOOP_OPT("Rotation: reject — body_start %d not after backedge %d (n=%d)", body_start, backedge_idx, n);
    return 0;
  }

  /* --- Step 3: Identify latch region [latch_start .. latch_end] --- */
  int latch_start = hi + 3;
  int latch_end = backedge_idx - 1; /* exclude back-edge JUMP */
  int latch_count = latch_end - latch_start + 1;
  if (latch_count < 0)
    latch_count = 0;

  /* Latch must be small (IV save + increment, typically 2 instrs) */
  if (latch_count > 8)
    return 0;

  /* --- Step 4: Identify body region and body→latch jump --- */
  /* Scan from body_start forward for a JUMP targeting anywhere in the
   * latch region [latch_start, end_idx].  After jump threading + DCE,
   * the first latch instruction may have become NOP, and the body→latch
   * JUMP may have been threaded to a later instruction in the latch. */
  int body_end_jmp = -1;
  int body_latch_target = -1;
  int body_end_is_implicit = 0;
  for (int i = body_start; i < n && i < body_start + 100; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_JUMP)
    {
      IROperand jd = tcc_ir_op_get_dest(ir, q);
      int jt = (int)irop_get_imm64_ex(ir, jd);
      if (jt >= latch_start && jt <= backedge_idx)
      {
        body_end_jmp = i;
        body_latch_target = jt;
        break;
      }
    }
  }

  /* Nested loops: the inner loop's conditional exit JUMPIF may target the
   * outer latch directly (after jump threading eliminates the explicit JUMP).
   * Only use this path when the body contains a backward jump (inner loop).
   * Simple loops with eliminated break JMPs rely on fallthrough to exit,
   * which rotation would break. */
  if (body_end_jmp < 0)
  {
    int has_inner_loop = 0;
    for (int i = body_start; i < n && i < body_start + 100; i++)
    {
      IRQuadCompact *q = &ir->compact_instructions[i];
      if (q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_JUMPIF)
      {
        IROperand jd = tcc_ir_op_get_dest(ir, q);
        int jt = (int)irop_get_imm64_ex(ir, jd);
        if (jt < i && jt >= body_start)
        {
          has_inner_loop = 1;
          break;
        }
      }
    }
    if (has_inner_loop)
    {
      for (int i = body_start; i < n && i < body_start + 100; i++)
      {
        IRQuadCompact *q = &ir->compact_instructions[i];
        if (q->op == TCCIR_OP_JUMPIF)
        {
          IROperand jd = tcc_ir_op_get_dest(ir, q);
          int jt = (int)irop_get_imm64_ex(ir, jd);
          if (jt >= latch_start && jt <= backedge_idx)
          {
            body_latch_target = jt;
            body_end_is_implicit = 1;
            body_end_jmp = exit_target - 1;
            break;
          }
        }
      }
    }
  }
  if (body_end_jmp < 0)
  {
    LOG_LOOP_OPT("Rotation: reject — no body→latch JUMP from body_start=%d targeting [%d,%d]", body_start, latch_start,
                 backedge_idx);
    return 0;
  }
  LOG_LOOP_OPT("Rotation: body_end_jmp=%d, latch=[%d,%d], latch_count=%d", body_end_jmp, latch_start, latch_end,
               latch_count);
  int body_end = body_end_is_implicit ? body_end_jmp : body_end_jmp - 1;
  int body_count = body_end - body_start + 1;
  if (body_count < 0)
    body_count = 0;
  if (body_count > 128)
    return 0;

  /* --- Step 4a2: Reject if body has a fall-through exit --- */
  /* When body_end_is_implicit, the body may end with trailing NOPs (from
   * eliminated fall-through jumps) after a JUMPIF.  In the original layout,
   * the fall-through from that JUMPIF goes to the exit target (e.g., a goto
   * label).  After rotation, the latch is placed right after the body, so the
   * fall-through would go to the latch instead — a miscompilation.
   * Reject if the last non-NOP body instruction is a JUMPIF whose fall-through
   * reaches the exit target. */
  if (body_end_is_implicit)
  {
    int last_real = body_end;
    while (last_real >= body_start && ir->compact_instructions[last_real].op == TCCIR_OP_NOP)
      last_real--;
    if (last_real >= body_start && ir->compact_instructions[last_real].op == TCCIR_OP_JUMPIF)
    {
      int ft = last_real + 1;
      while (ft < n && ir->compact_instructions[ft].op == TCCIR_OP_NOP)
        ft++;
      if (ft >= exit_target)
      {
        LOG_LOOP_OPT("Rotation: reject — body JUMPIF at %d falls through to exit_target %d", last_real, exit_target);
        return 0;
      }
    }
  }

  /* --- Step 4b: Check for external entries into the loop --- */
  /* Skip instructions inside the header/latch region [start_idx, end_idx]
   * and the body region [body_start, body_end_jmp] — those are normal
   * loop control flow, not external entries. */
  {
    int ext_entry = 0;
    for (int j = 0; j < n && !ext_entry; j++)
    {
      if (j >= loop->start_idx && j <= loop->end_idx)
        continue;
      if (j >= body_start && j <= body_end_jmp)
        continue;
      IRQuadCompact *jq = &ir->compact_instructions[j];
      if (jq->op == TCCIR_OP_JUMP || jq->op == TCCIR_OP_JUMPIF)
      {
        IROperand jdest = tcc_ir_op_get_dest(ir, jq);
        int jtarget = (int)irop_get_imm64_ex(ir, jdest);
        /* External jump into the latch or body (not the header) */
        if (jtarget > loop->start_idx && jtarget <= loop->end_idx)
          ext_entry = 1;
        if (jtarget >= body_start && jtarget <= body_end_jmp)
          ext_entry = 1;
      }
    }
    if (ext_entry)
    {
      LOG_LOOP_OPT("Rotation: reject — external entry into loop body/latch");
      return 0;
    }
  }

  /* --- Step 5: Validate body contents --- */
  /* Only allow body branches when there's a nested inner loop (backward
   * jump within the body).  Simple loops with conditional bodies (if/break)
   * should not be rotated here — later passes like IV strength reduction
   * may not handle the rotated form correctly. */
  int body_has_branches = 0;
  {
    int has_inner_loop = 0;
    for (int i = body_start; i <= body_end; i++)
    {
      IRQuadCompact *q = &ir->compact_instructions[i];
      if (q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_JUMPIF)
      {
        IROperand jd = tcc_ir_op_get_dest(ir, q);
        int jt = (int)irop_get_imm64_ex(ir, jd);
        if (jt < i && jt >= body_start)
        {
          has_inner_loop = 1;
          break;
        }
      }
    }

    int region_start_5 = hi + 2;
    for (int i = body_start; i <= body_end; i++)
    {
      IRQuadCompact *q = &ir->compact_instructions[i];
      if (q->op == TCCIR_OP_NOP)
        continue;
      if (q->op == TCCIR_OP_IJUMP)
        return 0;
      if (q->op == TCCIR_OP_JUMPIF || q->op == TCCIR_OP_JUMP)
      {
        if (!has_inner_loop)
          return 0;
        IROperand jd = tcc_ir_op_get_dest(ir, q);
        int jt = (int)irop_get_imm64_ex(ir, jd);
        /* Internal body branches (inner loops etc.) - will be remapped */
        if (jt >= body_start && jt <= body_end_jmp)
        {
          body_has_branches = 1;
          continue;
        }
        /* Branch to latch region - will be remapped */
        if (jt >= latch_start && jt <= backedge_idx)
        {
          body_has_branches = 1;
          continue;
        }
        /* Branch outside modified region - no remap needed */
        if (jt < region_start_5 || jt > body_end_jmp)
        {
          body_has_branches = 1;
          continue;
        }
        return 0;
      }
    }
  }

  /* Validate latch contents - no branches except the back-edge we already found.
   * Use body_latch_target as effective latch start (may skip leading NOPs). */
  int eff_latch_start = body_latch_target;
  int eff_latch_count = latch_end - eff_latch_start + 1;
  if (eff_latch_count < 0)
    eff_latch_count = 0;
  for (int i = eff_latch_start; i <= latch_end; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_JUMPIF || q->op == TCCIR_OP_JUMP)
    {
      LOG_LOOP_OPT("Rotation: reject — latch has branch at i=%d (op=%d)", i, q->op);
      return 0;
    }
  }

  /* --- Step 6: Check size - rotated code must fit --- */
  /* Available slots: [hi+2 .. body_end_jmp] */
  int region_start = hi + 2;     /* first slot to overwrite (was body-entry JUMP) */
  int region_end = body_end_jmp; /* last slot to overwrite (was body→latch JUMP) */
  int avail_slots = region_end - region_start + 1;

  /* Check if an exit jump is needed after the bottom test.
   * The fall-through from the bottom JUMPIF goes to region_end+1.
   * If that doesn't reach exit_target (accounting for NOPs), we need
   * an explicit JUMP to exit_target.  This happens in nested loops
   * where the inner loop's exit target (outer latch) is above the
   * loop body in instruction order, not right after it. */
  int need_exit_jump = 0;
  {
    int ft = region_end + 1;
    while (ft < n && ir->compact_instructions[ft].op == TCCIR_OP_NOP)
      ft++;
    if (ft != exit_target)
      need_exit_jump = 1;
  }

  /* Need: body_count + eff_latch_count + 2 (tail CMP + JUMPIF) + optional exit JUMP */
  int needed = body_count + eff_latch_count + 2 + need_exit_jump;
  if (needed > avail_slots)
  {
    LOG_LOOP_OPT("Rotation: reject — needed %d > avail %d", needed, avail_slots);
    return 0;
  }

  /* Invert condition for back-edge */
  int inv_cond = invert_condition(cond);
  if (inv_cond < 0)
  {
    LOG_LOOP_OPT("Rotation: reject — cannot invert cond 0x%x", cond);
    return 0;
  }
  LOG_LOOP_OPT("Rotation: all checks passed, rotating!");

  /* --- Step 7: Save instructions before overwriting --- */
  /* Save CMP operands for the tail test */
  IROperand cmp_src1 = tcc_ir_op_get_src1(ir, cmp_q);
  IROperand cmp_src2 = tcc_ir_op_get_src2(ir, cmp_q);

  /* Save body instructions */
  int body_ops[128], latch_ops[8];
  IROperand body_dests[128], body_src1s[128], body_src2s[128];
  IROperand body_extras[128]; /* MLA accumulator (operand_base+3) */
  int body_has_extra[128];
  IROperand latch_dests[8], latch_src1s[8], latch_src2s[8];
  uint32_t body_lines[128], latch_lines[8];

  for (int b = 0; b < body_count; b++)
  {
    IRQuadCompact *bq = &ir->compact_instructions[body_start + b];
    int op = bq->op;
    body_ops[b] = op;
    body_lines[b] = bq->line_num;
    body_dests[b] = (IROperand){0};
    body_src1s[b] = (IROperand){0};
    body_src2s[b] = (IROperand){0};
    body_extras[b] = (IROperand){0};
    body_has_extra[b] = (op == TCCIR_OP_MLA || op == TCCIR_OP_LOAD_INDEXED || op == TCCIR_OP_STORE_INDEXED);
    if (irop_config[op].has_dest)
      body_dests[b] = ir->iroperand_pool[bq->operand_base];
    if (irop_config[op].has_src1)
      body_src1s[b] = ir->iroperand_pool[bq->operand_base + irop_config[op].has_dest];
    if (irop_config[op].has_src2)
      body_src2s[b] = ir->iroperand_pool[bq->operand_base + irop_config[op].has_dest + irop_config[op].has_src1];
    if (body_has_extra[b])
      body_extras[b] = ir->iroperand_pool[bq->operand_base + 3];
  }

  /* Save latch instructions (from effective latch start, skipping leading NOPs) */
  for (int l = 0; l < eff_latch_count; l++)
  {
    IRQuadCompact *lq = &ir->compact_instructions[eff_latch_start + l];
    int op = lq->op;
    latch_ops[l] = op;
    latch_lines[l] = lq->line_num;
    latch_dests[l] = (IROperand){0};
    latch_src1s[l] = (IROperand){0};
    latch_src2s[l] = (IROperand){0};
    if (irop_config[op].has_dest)
      latch_dests[l] = ir->iroperand_pool[lq->operand_base];
    if (irop_config[op].has_src1)
      latch_src1s[l] = ir->iroperand_pool[lq->operand_base + irop_config[op].has_dest];
    if (irop_config[op].has_src2)
      latch_src2s[l] = ir->iroperand_pool[lq->operand_base + irop_config[op].has_dest + irop_config[op].has_src1];
  }

  /* --- Step 8: NOP the region [hi+2 .. body_end_jmp] --- */
  for (int i = region_start; i <= region_end; i++)
  {
    ir->compact_instructions[i].op = TCCIR_OP_NOP;
    ir->compact_instructions[i].is_jump_target = 0;
  }

  /* --- Step 9: Write rotated code --- */
  int wp = region_start;

  /* Write body instructions */
  int body_target = wp; /* back-edge will target this */
  for (int b = 0; b < body_count; b++)
  {
    write_instr_at_nop(ir, wp, body_ops[b], body_dests[b], body_src1s[b], body_src2s[b]);
    if (body_has_extra[b])
      tcc_ir_pool_add(ir, body_extras[b]); /* MLA accumulator at operand_base+3 */
    ir->compact_instructions[wp].line_num = body_lines[b];
    wp++;
  }

  /* Remap branch targets within the relocated body */
  if (body_has_branches)
  {
    int body_offset = region_start - body_start;
    int latch_new_start = region_start + body_count;
    int latch_offset = latch_new_start - eff_latch_start;
    for (int i = body_target; i < body_target + body_count; i++)
    {
      IRQuadCompact *q = &ir->compact_instructions[i];
      if (q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_JUMPIF)
      {
        IROperand *dest = &ir->iroperand_pool[q->operand_base];
        int old_target = dest->u.imm32;
        int new_target = -1;
        if (old_target >= body_start && old_target <= body_end_jmp)
        {
          new_target = old_target + body_offset;
        }
        else if (old_target >= eff_latch_start && old_target <= latch_end)
        {
          new_target = old_target + latch_offset;
        }
        if (new_target >= 0)
        {
          dest->u.imm32 = new_target;
          if (new_target < n)
            ir->compact_instructions[new_target].is_jump_target = 1;
        }
      }
    }
  }

  /* Write latch instructions (IV save + increment, no back-edge JUMP) */
  for (int l = 0; l < eff_latch_count; l++)
  {
    write_instr_at_nop(ir, wp, latch_ops[l], latch_dests[l], latch_src1s[l], latch_src2s[l]);
    ir->compact_instructions[wp].line_num = latch_lines[l];
    wp++;
  }

  /* Write tail CMP (duplicate of header CMP) */
  write_instr_at_nop(ir, wp, TCCIR_OP_CMP, (IROperand){0}, cmp_src1, cmp_src2);
  wp++;

  /* Write tail JUMPIF with inverted condition, targeting body_target */
  {
    IROperand jmp_dest = irop_make_imm32(-1, body_target, IROP_BTYPE_INT32);
    IROperand inv_cond_op = irop_make_imm32(-1, inv_cond, IROP_BTYPE_INT32);
    write_instr_at_nop(ir, wp, TCCIR_OP_JUMPIF, jmp_dest, inv_cond_op, (IROperand){0});
    wp++;
  }

  /* Write exit JUMP when fall-through doesn't reach exit_target */
  if (need_exit_jump)
  {
    IROperand exit_dest = irop_make_imm32(-1, exit_target, IROP_BTYPE_INT32);
    write_instr_at_nop(ir, wp, TCCIR_OP_JUMP, exit_dest, (IROperand){0}, (IROperand){0});
    wp++;
  }

  /* --- Step 10: Fix is_jump_target flags --- */
  /* The first body instruction is the back-edge target */
  ir->compact_instructions[body_target].is_jump_target = 1;

  /* The old header CMP no longer has a back-edge targeting it, but may still
   * be targeted by outer code (e.g. goto).  Conservatively leave it. */

  /* Clear is_jump_target on the exit_target instruction only if it was set
   * by the old body-entry jump — it's still targeted by the guard JUMPIF,
   * so leave it alone. */

  LOG_IR_GEN("[LOOP-ROTATE] Rotated loop header=%d body=[%d..%d] latch=[%d..%d] → bottom-tested at %d", hi, body_start,
             body_end, latch_start, latch_end, body_target);

  return 1;
}

int loop_size_cmp(const void *a, const void *b)
{
  const IRLoop *la = (const IRLoop *)a;
  const IRLoop *lb = (const IRLoop *)b;
  int sa = la->end_idx - la->start_idx;
  int sb = lb->end_idx - lb->start_idx;
  return sa - sb;
}

