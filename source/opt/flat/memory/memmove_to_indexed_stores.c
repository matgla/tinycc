/*
 *  TCC IR - memmove of a stack temp to indexed stores (flat, pre-SSA)
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
#include "opt_alias.h"
#include "opt_utils.h"




/* Fold memmove/memcpy from a stack temp fully covered by preceding local STOREs. */
int tcc_ir_opt_memmove_to_indexed_stores(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  int changes = 0;

  if (n == 0)
    return 0;

  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_FUNCCALLVOID && q->op != TCCIR_OP_FUNCCALLVAL)
      continue;

    /* Callee must be memmove/memcpy family (returns first arg). */
    Sym *callee = irop_get_sym_ex(ir, tcc_ir_op_get_src1(ir, q));
    if (!callee)
      continue;
    const char *name = get_tok_str(callee->v, NULL);
    if (!name)
      continue;
    int is_memmove_like = ir_opt_is_memcpy_or_memmove_name(name);
    if (!is_memmove_like)
      continue;

    /* FUNCCALLVAL returns the dst pointer: foldable only if nothing reads it. */
    if (q->op == TCCIR_OP_FUNCCALLVAL)
    {
      IROperand call_dest = tcc_ir_op_get_dest(ir, q);
      int32_t ret_vr = irop_get_vreg(call_dest);
      int has_reader = 0;
      if (ret_vr >= 0)
      {
        for (int j = 0; j < n && !has_reader; j++)
        {
          if (j == i)
            continue;
          IRQuadCompact *sq = &ir->compact_instructions[j];
          if (sq->op == TCCIR_OP_NOP)
            continue;
          if (irop_config[sq->op].has_src1)
          {
            IROperand s = tcc_ir_op_get_src1(ir, sq);
            if (irop_has_vreg(s) && irop_get_vreg(s) == ret_vr)
            {
              has_reader = 1;
              break;
            }
          }
          if (irop_config[sq->op].has_src2)
          {
            IROperand s = tcc_ir_op_get_src2(ir, sq);
            if (irop_has_vreg(s) && irop_get_vreg(s) == ret_vr)
            {
              has_reader = 1;
              break;
            }
          }
        }
      }
      if (has_reader)
        continue;
    }

    IROperand p_dst, p_src, p_size;
    if (!ir_opt_get_call_param_operand(ir, i, 0, &p_dst))
      continue;
    if (!ir_opt_get_call_param_operand(ir, i, 1, &p_src))
      continue;
    if (!ir_opt_get_call_param_operand(ir, i, 2, &p_size))
      continue;

    /* Size must be a small positive constant. */
    if (irop_get_tag(p_size) != IROP_TAG_IMM32)
      continue;
    int total_size = (int)irop_get_imm64_ex(ir, p_size);
    if (total_size <= 0 || total_size > 64)
      continue;

    /* Source is a local stack address: direct STACKOFF, or a vreg set by an earlier LEA. */
    int tmp_base;
    int lea_idx = -1; /* instruction index of the LEA that produced p_src, if any */
    if (irop_get_tag(p_src) == IROP_TAG_STACKOFF && p_src.is_local && !p_src.is_lval)
    {
      tmp_base = (int)irop_get_imm64_ex(ir, p_src);
    }
    else if (irop_get_tag(p_src) == IROP_TAG_VREG && irop_has_vreg(p_src) && !p_src.is_lval)
    {
      int32_t src_vr = irop_get_vreg(p_src);
      if (src_vr < 0)
        continue;
      /* Scan backwards for the most recent `<vr> <- Addr[StackLoc[X]]`. */
      int found_lea = 0;
      tmp_base = 0;
      for (int j = i - 1; j >= 0; j--)
      {
        IRQuadCompact *lq = &ir->compact_instructions[j];
        if (lq->op == TCCIR_OP_NOP)
          continue;
        if (lq->is_jump_target)
          break;
        if (lq->op == TCCIR_OP_JUMP || lq->op == TCCIR_OP_JUMPIF || lq->op == TCCIR_OP_IJUMP)
          break;
        if (!irop_config[lq->op].has_dest)
          continue;
        IROperand ld = tcc_ir_op_get_dest(ir, lq);
        if (!irop_has_vreg(ld) || irop_get_vreg(ld) != src_vr || ld.is_lval)
          continue;
        /* The op writing our vreg must be a LEA/ASSIGN from Addr[StackLoc[X]]. */
        if (lq->op != TCCIR_OP_LEA && lq->op != TCCIR_OP_ASSIGN)
          break;
        IROperand ls = tcc_ir_op_get_src1(ir, lq);
        if (irop_get_tag(ls) != IROP_TAG_STACKOFF || !ls.is_local || ls.is_lval)
          break;
        tmp_base = (int)irop_get_imm64_ex(ir, ls);
        lea_idx = j;
        found_lea = 1;
        break;
      }
      if (!found_lea)
        continue;
    }
    else
    {
      continue;
    }

    /* Destination is either a vreg pointer (STORE_INDEXED) or a stack offset (plain STORE). */
    int dst_is_stackoff = 0;
    int dst_base = 0;
    int32_t dst_vr = -1;
    if (irop_get_tag(p_dst) == IROP_TAG_STACKOFF && p_dst.is_local && !p_dst.is_lval)
    {
      dst_is_stackoff = 1;
      dst_base = (int)irop_get_imm64_ex(ir, p_dst);
      /* Forbid overlap with src range; the rewrite assumes non-overlap. */
      if (dst_base + total_size > tmp_base && dst_base < tmp_base + total_size)
        continue;
    }
    else if (irop_get_tag(p_dst) == IROP_TAG_VREG && !p_dst.is_lval && !p_dst.is_local && !p_dst.is_const)
    {
      if (!irop_has_vreg(p_dst))
        continue;
      dst_vr = irop_get_vreg(p_dst);
      if (dst_vr < 0)
        continue;

      /* Trace dst_vr back to a stack offset: stores may precede its LEA, so STORE_INDEXED on it would read an undefined vreg. */
      {
        int32_t trace_vr = dst_vr;
        int trace_add = 0;
        int trace_depth = 0;
        for (int j = i - 1; j >= 0 && trace_depth < 8; j--)
        {
          IRQuadCompact *lq = &ir->compact_instructions[j];
          if (lq->op == TCCIR_OP_NOP)
            continue;
          if (lq->is_jump_target)
            break;
          if (lq->op == TCCIR_OP_JUMP || lq->op == TCCIR_OP_JUMPIF || lq->op == TCCIR_OP_IJUMP)
            break;
          if (!irop_config[lq->op].has_dest)
            continue;
          IROperand ld = tcc_ir_op_get_dest(ir, lq);
          if (!irop_has_vreg(ld) || irop_get_vreg(ld) != trace_vr || ld.is_lval)
            continue;
          trace_depth++;
          if (lq->op == TCCIR_OP_LEA || lq->op == TCCIR_OP_ASSIGN)
          {
            IROperand ls = tcc_ir_op_get_src1(ir, lq);
            if (irop_get_tag(ls) == IROP_TAG_STACKOFF && ls.is_local && !ls.is_lval)
            {
              dst_is_stackoff = 1;
              dst_base = (int)irop_get_imm64_ex(ir, ls) + trace_add;
              dst_vr = -1;
              if (dst_base + total_size > tmp_base && dst_base < tmp_base + total_size) {
                dst_is_stackoff = 0;
                dst_vr = irop_get_vreg(p_dst);
              }
              break;
            }
            if (irop_has_vreg(ls) && !ls.is_lval)
            {
              trace_vr = irop_get_vreg(ls);
              continue;
            }
            break;
          }
          if (lq->op == TCCIR_OP_ADD)
          {
            IROperand as1 = tcc_ir_op_get_src1(ir, lq);
            IROperand as2 = tcc_ir_op_get_src2(ir, lq);
            if (irop_is_immediate(as2) && irop_has_vreg(as1) && !as1.is_lval)
            {
              trace_add += (int)irop_get_imm64_ex(ir, as2);
              trace_vr = irop_get_vreg(as1);
              continue;
            }
            break;
          }
          if (lq->op == TCCIR_OP_STORE && !ld.is_lval)
          {
            IROperand ls = tcc_ir_op_get_src1(ir, lq);
            if (irop_has_vreg(ls) && !ls.is_lval)
            {
              trace_vr = irop_get_vreg(ls);
              continue;
            }
            break;
          }
          if (lq->op == TCCIR_OP_LOAD)
          {
            IROperand ls = tcc_ir_op_get_src1(ir, lq);
            if (irop_has_vreg(ls) && ls.is_lval)
            {
              trace_vr = irop_get_vreg(ls);
              continue;
            }
            break;
          }
          break;
        }
      }
    }
    else
    {
      continue;
    }

    /* Backwards scan for STOREs (plus one zeroing memset) covering the temp range; coverage is a byte bitmap, so total_size is capped at 64. */
    int store_indices[16];
    int store_offsets[16];
    int store_lea_indices[16]; /* LEA that produced base vreg for indirect stores (-1 if direct) */
    int nstores = 0;
    int aborted = 0;
    uint64_t covered_mask = 0;
    int memset_idx = -1;
    int memset_off = 0;
    int memset_len = 0;
    int memset_dst_param_idx = -1; /* IR index of the memset's PARAM0 */

    for (int j = i - 1; j >= 0 && nstores < 16; j--)
    {
      IRQuadCompact *sq = &ir->compact_instructions[j];
      if (sq->op == TCCIR_OP_NOP)
        continue;
      if (sq->op == TCCIR_OP_FUNCPARAMVAL || sq->op == TCCIR_OP_FUNCPARAMVOID)
        continue; /* memmove's own params */
      if (sq->is_jump_target)
        break;
      if (sq->op == TCCIR_OP_JUMP || sq->op == TCCIR_OP_JUMPIF || sq->op == TCCIR_OP_IJUMP)
        break;

      if (sq->op != TCCIR_OP_STORE && sq->op != TCCIR_OP_STORE_INDEXED)
      {
        /* Only one preceding zeroing memset is tracked; later ones bail. */
        if (memset_idx < 0 &&
            (sq->op == TCCIR_OP_FUNCCALLVOID || sq->op == TCCIR_OP_FUNCCALLVAL))
        {
          Sym *ms_callee = irop_get_sym_ex(ir, tcc_ir_op_get_src1(ir, sq));
          const char *ms_name = ms_callee ? get_tok_str(ms_callee->v, NULL) : NULL;
          int is_memset_like = ms_name &&
                               (strcmp(ms_name, "memset") == 0 ||
                                strcmp(ms_name, "__aeabi_memset") == 0);
          if (is_memset_like)
          {
            IROperand ms_p0, ms_p1, ms_p2;
            int ok = ir_opt_get_call_param_operand(ir, j, 0, &ms_p0) &&
                     ir_opt_get_call_param_operand(ir, j, 1, &ms_p1) &&
                     ir_opt_get_call_param_operand(ir, j, 2, &ms_p2);
            /* __aeabi_memset(dst, len, val); memset(dst, val, len). */
            int is_aeabi = (strcmp(ms_name, "__aeabi_memset") == 0);
            IROperand ms_val = is_aeabi ? ms_p2 : ms_p1;
            IROperand ms_len = is_aeabi ? ms_p1 : ms_p2;
            if (ok &&
                irop_get_tag(ms_p0) == IROP_TAG_STACKOFF && ms_p0.is_local && !ms_p0.is_lval &&
                irop_get_tag(ms_val) == IROP_TAG_IMM32 && irop_get_imm64_ex(ir, ms_val) == 0 &&
                irop_get_tag(ms_len) == IROP_TAG_IMM32)
            {
              int ms_off = (int)irop_get_imm64_ex(ir, ms_p0);
              int ms_n = (int)irop_get_imm64_ex(ir, ms_len);
              if (ms_n > 0 &&
                  ms_off >= tmp_base &&
                  ms_off + ms_n <= tmp_base + total_size)
              {
                /* Find the PARAM0 instruction index for later rewrite. */
                int ms_call_id = TCCIR_DECODE_CALL_ID((uint32_t)irop_get_imm64_ex(
                    ir, tcc_ir_op_get_src2(ir, sq)));
                int p0_idx = -1;
                for (int k = j - 1; k >= 0; --k)
                {
                  IRQuadCompact *pq = &ir->compact_instructions[k];
                  if (pq->op == TCCIR_OP_NOP)
                    continue;
                  if (pq->op != TCCIR_OP_FUNCPARAMVAL && pq->op != TCCIR_OP_FUNCPARAMVOID)
                    continue;
                  IROperand penc = tcc_ir_op_get_src2(ir, pq);
                  uint32_t enc = (uint32_t)irop_get_imm64_ex(ir, penc);
                  if (TCCIR_DECODE_CALL_ID(enc) != ms_call_id)
                    continue;
                  if (TCCIR_DECODE_PARAM_IDX(enc) == 0)
                  {
                    p0_idx = k;
                    break;
                  }
                }
                if (p0_idx >= 0)
                {
                  memset_idx = j;
                  memset_off = ms_off;
                  memset_len = ms_n;
                  memset_dst_param_idx = p0_idx;
                  /* Mark bytes as covered by the memset. */
                  int bit0 = ms_off - tmp_base;
                  for (int b = 0; b < ms_n; b++)
                    covered_mask |= ((uint64_t)1) << (bit0 + b);
                  continue;
                }
              }
            }
          }
        }

        /* Other ops are fine (the global scan below enforces aliasing); bail early only on a write into the temp range. */
        if (irop_config[sq->op].has_dest)
        {
          IROperand d = tcc_ir_op_get_dest(ir, sq);
          if (irop_get_tag(d) == IROP_TAG_STACKOFF && d.is_local && d.is_lval)
          {
            int doff = (int)irop_get_imm64_ex(ir, d);
            if (doff >= tmp_base && doff < tmp_base + total_size)
            {
              aborted = 1;
              break;
            }
          }
        }
        continue;
      }

      /* Resolve the store's byte offset: direct STACKOFF dest, a vreg tracing to a stack LEA, or STORE_INDEXED with immediate offset and scale 0. */
      int st_off = 0;
      int st_off_found = 0;
      int st_size = -1;
      int st_store_lea = -1;
      IROperand st_src;
      IROperand st_dest = tcc_ir_op_get_dest(ir, sq);

      if (sq->op == TCCIR_OP_STORE)
      {
        st_src = tcc_ir_op_get_src1(ir, sq);
        if (irop_get_tag(st_dest) == IROP_TAG_STACKOFF && st_dest.is_local && st_dest.is_lval)
        {
          /* A named VAR/PARAM vreg on the STACKOFF dest keys the backend store, so it cannot be relocated by offset alone. */
          int32_t dvr = irop_get_vreg(st_dest);
          if (dvr >= 0)
          {
            int vt = TCCIR_DECODE_VREG_TYPE(dvr);
            if (vt == TCCIR_VREG_TYPE_VAR || vt == TCCIR_VREG_TYPE_PARAM)
            {
              aborted = 1;
              break;
            }
          }
          st_off = (int)irop_get_imm64_ex(ir, st_dest);
          st_off_found = 1;
          st_size = ir_opt_store_btype_size_bytes(irop_get_btype(st_dest));
        }
        else if (irop_get_tag(st_dest) == IROP_TAG_VREG && st_dest.is_lval)
        {
          int32_t trace_vr = irop_get_vreg(st_dest);
          int trace_add = 0;
          int trace_depth = 0;
          if (trace_vr >= 0)
          {
            for (int k = j - 1; k >= 0 && trace_depth < 8; k--)
            {
              IRQuadCompact *kq = &ir->compact_instructions[k];
              if (kq->op == TCCIR_OP_NOP) continue;
              if (kq->is_jump_target) break;
              if (kq->op == TCCIR_OP_JUMP || kq->op == TCCIR_OP_JUMPIF || kq->op == TCCIR_OP_IJUMP) break;
              if (!irop_config[kq->op].has_dest) continue;
              IROperand kd = tcc_ir_op_get_dest(ir, kq);
              if (!irop_has_vreg(kd) || irop_get_vreg(kd) != trace_vr || kd.is_lval) continue;
              trace_depth++;
              if (kq->op == TCCIR_OP_ADD)
              {
                IROperand as1 = tcc_ir_op_get_src1(ir, kq);
                IROperand as2 = tcc_ir_op_get_src2(ir, kq);
                if (irop_is_immediate(as2) && irop_has_vreg(as1) && !as1.is_lval)
                {
                  trace_add += (int)irop_get_imm64_ex(ir, as2);
                  trace_vr = irop_get_vreg(as1);
                  continue;
                }
                break;
              }
              if (kq->op != TCCIR_OP_LEA && kq->op != TCCIR_OP_ASSIGN) break;
              IROperand ks = tcc_ir_op_get_src1(ir, kq);
              if (irop_get_tag(ks) != IROP_TAG_STACKOFF || !ks.is_local || ks.is_lval) break;
              st_off = (int)irop_get_imm64_ex(ir, ks) + trace_add;
              st_off_found = 1;
              st_store_lea = k;
              break;
            }
            if (st_off_found)
              st_size = ir_opt_store_btype_size_bytes(irop_get_btype(st_dest));
          }
        }
      }
      else /* TCCIR_OP_STORE_INDEXED */
      {
        st_src = tcc_ir_op_get_src1(ir, sq);
        IROperand st_idx = tcc_ir_op_get_src2(ir, sq);
        if (irop_get_tag(st_dest) == IROP_TAG_VREG && irop_has_vreg(st_dest) &&
            irop_get_tag(st_idx) == IROP_TAG_IMM32)
        {
          IROperand scale_op = ir->iroperand_pool[sq->operand_base + 3];
          int scale_val = (int)irop_get_imm64_ex(ir, scale_op);
          if (scale_val == 0)
          {
            int32_t trace_vr = irop_get_vreg(st_dest);
            int idx_val = (int)irop_get_imm64_ex(ir, st_idx);
            int trace_add = idx_val;
            int trace_depth = 0;
            if (trace_vr >= 0)
            {
              for (int k = j - 1; k >= 0 && trace_depth < 8; k--)
              {
                IRQuadCompact *kq = &ir->compact_instructions[k];
                if (kq->op == TCCIR_OP_NOP) continue;
                if (kq->is_jump_target) break;
                if (kq->op == TCCIR_OP_JUMP || kq->op == TCCIR_OP_JUMPIF || kq->op == TCCIR_OP_IJUMP) break;
                if (!irop_config[kq->op].has_dest) continue;
                IROperand kd = tcc_ir_op_get_dest(ir, kq);
                if (!irop_has_vreg(kd) || irop_get_vreg(kd) != trace_vr || kd.is_lval) continue;
                trace_depth++;
                if (kq->op == TCCIR_OP_ADD)
                {
                  IROperand as1 = tcc_ir_op_get_src1(ir, kq);
                  IROperand as2 = tcc_ir_op_get_src2(ir, kq);
                  if (irop_is_immediate(as2) && irop_has_vreg(as1) && !as1.is_lval)
                  {
                    trace_add += (int)irop_get_imm64_ex(ir, as2);
                    trace_vr = irop_get_vreg(as1);
                    continue;
                  }
                  break;
                }
                if (kq->op != TCCIR_OP_LEA && kq->op != TCCIR_OP_ASSIGN) break;
                IROperand ks = tcc_ir_op_get_src1(ir, kq);
                if (irop_get_tag(ks) != IROP_TAG_STACKOFF || !ks.is_local || ks.is_lval) break;
                st_off = (int)irop_get_imm64_ex(ir, ks) + trace_add;
                st_off_found = 1;
                st_store_lea = k;
                break;
              }
            }
          }
          if (st_off_found)
            st_size = ir_opt_store_btype_size_bytes(irop_get_btype(st_src));
        }
      }

      if (!st_off_found)
        continue;
      if (st_off < tmp_base || st_off >= tmp_base + total_size)
        continue;

      if (st_size <= 0)
      {
        aborted = 1;
        break;
      }
      if (st_off + st_size > tmp_base + total_size)
      {
        aborted = 1;
        break;
      }

      /* The stored value must be a vreg or immediate to be reusable in a STORE_INDEXED. */
      int src_tag = irop_get_tag(st_src);
      if (src_tag != IROP_TAG_VREG && src_tag != IROP_TAG_IMM32 &&
          src_tag != IROP_TAG_I64 && src_tag != IROP_TAG_F32 && src_tag != IROP_TAG_F64)
      {
        aborted = 1;
        break;
      }

      store_indices[nstores] = j;
      store_offsets[nstores] = st_off - tmp_base;
      store_lea_indices[nstores] = st_store_lea;
      nstores++;
      /* Mark the bytes covered by this store in the bitmap. */
      int bit0 = st_off - tmp_base;
      for (int b = 0; b < st_size; b++)
        covered_mask |= ((uint64_t)1) << (bit0 + b);

      /* Stop when full coverage is reached (explicit stores + any memset). */
      uint64_t want_mask = (total_size >= 64) ? ~(uint64_t)0
                                              : (((uint64_t)1 << total_size) - 1);
      if (covered_mask == want_mask)
        break;
    }

    {
      uint64_t want_mask = (total_size >= 64) ? ~(uint64_t)0
                                              : (((uint64_t)1 << total_size) - 1);
      if (aborted || nstores == 0 || covered_mask != want_mask)
        continue;
    }

    /* Stores into the src range before this index are dead: the contributing stores fully cover it. */
    int earliest_contrib_idx = i;
    for (int s = 0; s < nstores; s++)
    {
      if (store_indices[s] < earliest_contrib_idx)
        earliest_contrib_idx = store_indices[s];
    }

    /* Bail if the stack temp is read or address-taken anywhere else. */
    int safe = 1;
    int dead_pre_stores[16];
    int n_dead_pre_stores = 0;
    for (int j = 0; j < n; j++)
    {
      if (j == i)
        continue;
      IRQuadCompact *sq = &ir->compact_instructions[j];
      if (sq->op == TCCIR_OP_NOP)
        continue;
      /* The contributing stores and their LEAs become dead after the rewrite. */
      int is_store_of_ours = 0;
      for (int s = 0; s < nstores; s++)
      {
        if (store_indices[s] == j || store_lea_indices[s] == j)
        {
          is_store_of_ours = 1;
          break;
        }
      }
      if (is_store_of_ours)
        continue;
      /* The LEA producing p_src is NOPed alongside the call. */
      if (j == lea_idx)
        continue;
      /* Skip params of the memmove call. */
      if (sq->op == TCCIR_OP_FUNCPARAMVAL || sq->op == TCCIR_OP_FUNCPARAMVOID)
      {
        IROperand pop_enc = tcc_ir_op_get_src2(ir, sq);
        IROperand call_src2 = tcc_ir_op_get_src2(ir, q);
        if (TCCIR_DECODE_CALL_ID((uint32_t)irop_get_imm64_ex(ir, pop_enc)) ==
            TCCIR_DECODE_CALL_ID((uint32_t)irop_get_imm64_ex(ir, call_src2)))
          continue;
      }

      /* The memset's PARAM0 is shifted to dst during the rewrite. */
      if (memset_idx >= 0)
      {
        if (j == memset_idx)
          continue;
        if (sq->op == TCCIR_OP_FUNCPARAMVAL || sq->op == TCCIR_OP_FUNCPARAMVOID)
        {
          IROperand pop_enc = tcc_ir_op_get_src2(ir, sq);
          IROperand ms_src2 = tcc_ir_op_get_src2(ir, &ir->compact_instructions[memset_idx]);
          if (TCCIR_DECODE_CALL_ID((uint32_t)irop_get_imm64_ex(ir, pop_enc)) ==
              TCCIR_DECODE_CALL_ID((uint32_t)irop_get_imm64_ex(ir, ms_src2)))
            continue;
        }
      }

      /* A pre-contributing STORE entirely inside the src range is dead; record it for NOPing. */
      if (sq->op == TCCIR_OP_STORE && j < earliest_contrib_idx)
      {
        IROperand sd = tcc_ir_op_get_dest(ir, sq);
        if (irop_get_tag(sd) == IROP_TAG_STACKOFF && sd.is_local && sd.is_lval)
        {
          int sd_off = (int)irop_get_imm64_ex(ir, sd);
          int sd_size = ir_opt_store_btype_size_bytes(irop_get_btype(sd));
          if (sd_size > 0 && sd_off >= tmp_base && sd_off + sd_size <= tmp_base + total_size)
          {
            if (n_dead_pre_stores < (int)(sizeof(dead_pre_stores) / sizeof(dead_pre_stores[0])))
              dead_pre_stores[n_dead_pre_stores++] = j;
            continue;
          }
        }
      }

      /* Check operands for tmp_base address use or stack offset use. */
      for (int si = 0; si < 3; si++)
      {
        IROperand op;
        if (si == 0 && irop_config[sq->op].has_dest)
          op = tcc_ir_op_get_dest(ir, sq);
        else if (si == 1 && irop_config[sq->op].has_src1)
          op = tcc_ir_op_get_src1(ir, sq);
        else if (si == 2 && irop_config[sq->op].has_src2)
          op = tcc_ir_op_get_src2(ir, sq);
        else
          continue;
        if (irop_get_tag(op) != IROP_TAG_STACKOFF)
          continue;
        if (!op.is_local)
          continue;
        int off = (int)irop_get_imm64_ex(ir, op);
        if (off < tmp_base || off >= tmp_base + total_size)
          continue;
        /* The stack temp is referenced elsewhere — bail. */
        safe = 0;
        break;
      }
      if (!safe)
        break;
    }
    if (!safe)
      continue;

    /* dst_vr must be defined before every store we relocate onto it. */
    if (!dst_is_stackoff)
    {
      int earliest_store_idx = i;
      for (int s = 0; s < nstores; s++)
      {
        if (store_indices[s] < earliest_store_idx)
          earliest_store_idx = store_indices[s];
      }
      int dst_def_idx = -1;
      for (int j = i - 1; j >= 0; j--)
      {
        IRQuadCompact *sq = &ir->compact_instructions[j];
        if (sq->op == TCCIR_OP_NOP)
          continue;
        if (!irop_config[sq->op].has_dest)
          continue;
        IROperand d = tcc_ir_op_get_dest(ir, sq);
        if (irop_has_vreg(d) && irop_get_vreg(d) == dst_vr && !d.is_lval)
        {
          dst_def_idx = j;
          break;
        }
      }
      if (dst_def_idx < 0 || dst_def_idx >= earliest_store_idx)
        continue;
    }

    /* Relocated stores write dst earlier than the memcpy did, so nothing may access dst's range before the call. */
    if (dst_is_stackoff)
    {
      int dst_safe = 1;
      for (int j = 0; j < i && dst_safe; j++)
      {
        if (j == i)
          continue;
        IRQuadCompact *sq = &ir->compact_instructions[j];
        if (sq->op == TCCIR_OP_NOP)
          continue;
        /* Contributing stores and their LEAs are about to be relocated to dst. */
        int is_store_of_ours = 0;
        for (int s = 0; s < nstores; s++)
        {
          if (store_indices[s] == j || store_lea_indices[s] == j)
          {
            is_store_of_ours = 1;
            break;
          }
        }
        if (is_store_of_ours)
          continue;
        /* Skip params of the memcpy call. */
        if (sq->op == TCCIR_OP_FUNCPARAMVAL || sq->op == TCCIR_OP_FUNCPARAMVOID)
        {
          IROperand pop_enc = tcc_ir_op_get_src2(ir, sq);
          IROperand call_src2 = tcc_ir_op_get_src2(ir, q);
          if (TCCIR_DECODE_CALL_ID((uint32_t)irop_get_imm64_ex(ir, pop_enc)) ==
              TCCIR_DECODE_CALL_ID((uint32_t)irop_get_imm64_ex(ir, call_src2)))
            continue;
        }
        /* A LEA only computes an address, so it neither reads nor writes dst's range. */
        if (sq->op == TCCIR_OP_LEA)
          continue;
        for (int si = 0; si < 3; si++)
        {
          IROperand op;
          if (si == 0 && irop_config[sq->op].has_dest)
            op = tcc_ir_op_get_dest(ir, sq);
          else if (si == 1 && irop_config[sq->op].has_src1)
            op = tcc_ir_op_get_src1(ir, sq);
          else if (si == 2 && irop_config[sq->op].has_src2)
            op = tcc_ir_op_get_src2(ir, sq);
          else
            continue;
          if (irop_get_tag(op) != IROP_TAG_STACKOFF)
            continue;
          if (!op.is_local)
            continue;
          int off = (int)irop_get_imm64_ex(ir, op);
          if (off < dst_base || off >= dst_base + total_size)
            continue;
          /* dst range referenced elsewhere — bail. */
          dst_safe = 0;
          break;
        }
      }
      if (!dst_safe)
        continue;
    }

    /* The LEA producing p_src must have no user other than the memmove's PARAM1. */
    if (lea_idx >= 0)
    {
      int32_t lea_vr = irop_get_vreg(tcc_ir_op_get_dest(ir, &ir->compact_instructions[lea_idx]));
      int lea_other_uses = 0;
      for (int j = 0; j < n && !lea_other_uses; j++)
      {
        if (j == lea_idx)
          continue;
        IRQuadCompact *sq = &ir->compact_instructions[j];
        if (sq->op == TCCIR_OP_NOP)
          continue;
        /* The memmove PARAM1 is the expected use. */
        if (sq->op == TCCIR_OP_FUNCPARAMVAL || sq->op == TCCIR_OP_FUNCPARAMVOID)
        {
          IROperand pop_enc = tcc_ir_op_get_src2(ir, sq);
          IROperand call_src2 = tcc_ir_op_get_src2(ir, q);
          if (TCCIR_DECODE_CALL_ID((uint32_t)irop_get_imm64_ex(ir, pop_enc)) ==
                  TCCIR_DECODE_CALL_ID((uint32_t)irop_get_imm64_ex(ir, call_src2)) &&
              TCCIR_DECODE_PARAM_IDX((uint32_t)irop_get_imm64_ex(ir, pop_enc)) == 1)
            continue;
        }
        for (int si = 0; si < 3; si++)
        {
          IROperand op2;
          if (si == 0 && irop_config[sq->op].has_dest)
            op2 = tcc_ir_op_get_dest(ir, sq);
          else if (si == 1 && irop_config[sq->op].has_src1)
            op2 = tcc_ir_op_get_src1(ir, sq);
          else if (si == 2 && irop_config[sq->op].has_src2)
            op2 = tcc_ir_op_get_src2(ir, sq);
          else
            continue;
          if (irop_has_vreg(op2) && irop_get_vreg(op2) == lea_vr)
          {
            lea_other_uses = 1;
            break;
          }
        }
      }
      if (lea_other_uses)
        continue;
    }

    /* Retarget each contributing store: STORE_INDEXED on a dst pointer, plain STORE on a dst stack offset. */
    for (int s = 0; s < nstores; s++)
    {
      int sidx = store_indices[s];
      IRQuadCompact *sq = &ir->compact_instructions[sidx];
      IROperand st_src = tcc_ir_op_get_src1(ir, sq);
      IROperand st_dest_old = tcc_ir_op_get_dest(ir, sq);
      int offset = store_offsets[s];

      if (dst_is_stackoff)
      {
        if (irop_get_tag(st_dest_old) == IROP_TAG_STACKOFF && st_dest_old.is_local &&
            sq->op == TCCIR_OP_STORE)
        {
          IROperand new_dest = st_dest_old;
          new_dest.u.imm32 = dst_base + offset;
          tcc_ir_set_dest(ir, sidx, new_dest);
        }
        else
        {
          int store_btype = (sq->op == TCCIR_OP_STORE_INDEXED)
                            ? irop_get_btype(st_src)
                            : irop_get_btype(st_dest_old);
          IROperand new_dest = irop_make_stackoff(-1, dst_base + offset,
                                                  /*is_lval*/ 1, /*is_llocal*/ 0,
                                                  /*is_param*/ 0, store_btype);
          tcc_ir_pool_ensure(ir, 2);
          int new_pool = ir->iroperand_pool_count;
          tcc_ir_pool_add(ir, new_dest);
          tcc_ir_pool_add(ir, st_src);
          sq->op = TCCIR_OP_STORE;
          sq->operand_base = new_pool;
        }
        continue;
      }

      /* STORE_INDEXED operand block: base pointer, value, immediate byte index, scale 0. */
      tcc_ir_pool_ensure(ir, 4);
      int new_base = ir->iroperand_pool_count;

      IROperand base = p_dst;
      base.is_lval = 0;
      /* The store width comes from src1's btype; the index field carries the byte offset. */
      tcc_ir_pool_add(ir, base);
      tcc_ir_pool_add(ir, st_src);
      IROperand index_op = irop_make_imm32(-1, offset, IROP_BTYPE_INT32);
      tcc_ir_pool_add(ir, index_op);
      IROperand scale_op = irop_make_imm32(-1, 0, IROP_BTYPE_INT32);
      tcc_ir_pool_add(ir, scale_op);

      sq->op = TCCIR_OP_STORE_INDEXED;
      sq->operand_base = new_base;
      (void)st_dest_old;
    }

    /* NOP the memmove call and all its FUNCPARAMVAL/FUNCPARAMVOID params. */
    ir_opt_nop_call_params(ir, i);
    q->op = TCCIR_OP_NOP;

    /* NOP the LEA of the temp address; its only user is gone. */
    if (lea_idx >= 0)
      ir->compact_instructions[lea_idx].op = TCCIR_OP_NOP;

    /* NOP the dead pre-contributing stores into the src range. */
    for (int s = 0; s < n_dead_pre_stores; s++)
      ir->compact_instructions[dead_pre_stores[s]].op = TCCIR_OP_NOP;

    /* Shift the recognized memset's PARAM0 from src to the matching dst offset. */
    if (dst_is_stackoff && memset_idx >= 0 && memset_dst_param_idx >= 0)
    {
      IRQuadCompact *pq = &ir->compact_instructions[memset_dst_param_idx];
      IROperand p0_val = tcc_ir_op_get_src1(ir, pq);
      p0_val.u.imm32 = dst_base + (memset_off - tmp_base);
      tcc_ir_set_src1(ir, memset_dst_param_idx, p0_val);
      /* memset_len bytes still get written; it's just to a different slot. */
      (void)memset_len;
    }

    changes++;
  }

  return changes;
}
