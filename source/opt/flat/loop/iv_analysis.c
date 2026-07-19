/*
 *  TCC IR - Induction variable analysis
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
#include "opt.h"
#include "opt_du.h"
#include "opt_xform.h"
#include "opt_utils.h"
#include "opt_loop_utils.h"

/* Find basic induction variables: V = V + const (V is a VAR vreg) */
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

      /* VAR must be defined exactly once in loop (the increment) */
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


/* Find derived induction variables: base + (IV << shift), used for indexing */
int find_derived_ivs(TCCIRState *ir, IRLoop *loop, InductionVar *ivs, int num_ivs, DerivedIV *divs, int max_divs)
{
  int num_divs = 0;

  /* MLA-only extended scan: include j>end_idx whose JMP/JUMPIF targets back into body (rotated loops); MLA-only avoids regressing the ADD-based scan */
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

    /* Chase one level of copy: iv_vr defined by ASSIGN/STORE from a BIV */
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

    /* ADD result must be used (dead-code check); multiple uses are fine */
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

    /* SHL result must be used only by this ADD, else we can't NOP it */
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

  /* Second pass: MLA-fused DIV — dest = IV*stride + invariant base */
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

    /* accum (base) must be loop-invariant: not redefined in loop */
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

  /* Third pass: LOAD_INDEXED/STORE_INDEXED DIV — index is a BIV, stride = iv.step*(1<<scale) */
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

    /* Restrict to INT32: POSTINC fusion only handles INT32, else net regression */
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

    /* Copy-through chase */
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

    /* Base must be loop-invariant; STORE* dest slot is a use, not a def — skip */
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

    /* Eliminability gate: IV usable only by self-increment, copy-through, indexed uses, and up to two CMP-vs-imm; else net regression */
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

      /* Allow indexed uses where IV is the index — these are the transform sites */
      if (uq->op == TCCIR_OP_LOAD_INDEXED || uq->op == TCCIR_OP_STORE_INDEXED)
      {
        if (irop_get_vreg(tcc_ir_op_get_src2(ir, uq)) == iv_vr)
          continue;
      }

      /* Allow copy-through `T = V` just before the self-increment */
      if (uq->op == TCCIR_OP_ASSIGN && j >= iv_def_idx - 3 && j < iv_def_idx)
      {
        if (irop_get_vreg(tcc_ir_op_get_src1(ir, uq)) == iv_vr)
          continue;
      }

      /* Allow up to two CMP V_iv,#imm (header pre-test + back-edge test) */
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
