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





/* Trace the address held in trace_vr back from instruction j -- at most 8
 * defining instructions, never across a join or a jump -- through ADDs of an
 * immediate (accumulated onto trace_add) to a LEA/ASSIGN of a local StackLoc.  On
 * success *off is that slot's offset plus the adds and the LEA's index is
 * returned; otherwise -1. */
static int mtis_trace_stack_addr(TCCIRState *ir, int j, int32_t trace_vr, int trace_add, int *off)
{
  int trace_depth = 0;
  for (int k = j - 1; k >= 0 && trace_depth < 8; k--)
  {
    IRQuadCompact *kq = &ir->compact_instructions[k];
    if (kq->is_jump_target) break; /* a NOP can be the join */
    if (kq->op == TCCIR_OP_NOP) continue;
    if (kq->op == TCCIR_OP_JUMP || kq->op == TCCIR_OP_JUMPIF || kq->op == TCCIR_OP_IJUMP) break;
    if (!irop_config[kq->op].has_dest) continue;
    IROperand kd = tcc_ir_op_get_dest(ir, kq);
    if (irop_get_vreg(kd) != trace_vr || !irop_dest_defines_vreg(kd)) continue;
    trace_depth++;
    if (kq->op == TCCIR_OP_ADD)
    {
      IROperand as1 = tcc_ir_op_get_src1(ir, kq);
      if (tcc_ir_op_src2_is_imm(ir, kq) && irop_has_vreg(as1) && !as1.is_lval)
      {
        trace_add += (int)tcc_ir_op_src2_imm(ir, kq);
        trace_vr = irop_get_vreg(as1);
        continue;
      }
      break;
    }
    if (kq->op != TCCIR_OP_LEA && kq->op != TCCIR_OP_ASSIGN) break;
    IROperand ks = tcc_ir_op_get_src1(ir, kq);
    if (irop_get_tag(ks) != IROP_TAG_STACKOFF || !ks.is_local || ks.is_lval) break;
    *off = (int)irop_get_imm64_ex(ir, ks) + trace_add;
    return k;
  }
  return -1;
}

/* Whether an op touches memory only through its operands (no hidden reads or
 * writes: calls, asm, setjmp, ... are barriers). */
static int mtis_plain_op(TccIrOp op)
{
  switch (op)
  {
  case TCCIR_OP_ASSIGN: case TCCIR_OP_LEA: case TCCIR_OP_LOAD: case TCCIR_OP_STORE:
  case TCCIR_OP_LOAD_INDEXED: case TCCIR_OP_STORE_INDEXED: case TCCIR_OP_LOAD_POSTINC:
  case TCCIR_OP_STORE_POSTINC: case TCCIR_OP_ADD: case TCCIR_OP_SUB: case TCCIR_OP_MUL:
  case TCCIR_OP_AND: case TCCIR_OP_OR: case TCCIR_OP_XOR: case TCCIR_OP_SHL: case TCCIR_OP_SHR:
  case TCCIR_OP_SAR: case TCCIR_OP_ROR: case TCCIR_OP_CMP: case TCCIR_OP_TEST_ZERO:
  case TCCIR_OP_SETIF: case TCCIR_OP_SELECT: case TCCIR_OP_ZEXT: case TCCIR_OP_UBFX:
  case TCCIR_OP_SBFX: case TCCIR_OP_BFI: case TCCIR_OP_CLZ: case TCCIR_OP_RBIT: case TCCIR_OP_REV:
  case TCCIR_OP_REV16: case TCCIR_OP_MLA: case TCCIR_OP_UMULL: case TCCIR_OP_SMULL:
  case TCCIR_OP_UMAAL: case TCCIR_OP_PACK64: case TCCIR_OP_FUNCPARAMVAL:
  case TCCIR_OP_FUNCPARAMVOID: case TCCIR_OP_NOP:
    return 1;
  default:
    return 0;
  }
}

/* Is the address of any byte of frame object [lo, hi) computed anywhere,
 * other than as the memcpy's own PARAM0 (call id CALL_ID)? */
static int mtis_object_addr_taken(TCCIRState *ir, int lo, int hi, int call_id)
{
  int n = ir->next_instruction_index;
  for (int j = 0; j < n; j++)
  {
    IRQuadCompact *sq = &ir->compact_instructions[j];
    if (sq->op == TCCIR_OP_NOP)
      continue;
    if ((sq->op == TCCIR_OP_FUNCPARAMVAL || sq->op == TCCIR_OP_FUNCPARAMVOID))
    {
      uint32_t enc = (uint32_t)tcc_ir_op_src2_imm(ir, sq);
      if (TCCIR_DECODE_CALL_ID(enc) == call_id && TCCIR_DECODE_PARAM_IDX(enc) == 0)
        continue;
    }
    for (int si = 0; si < 3; si++)
    {
      int has = si == 0 ? irop_config[sq->op].has_dest : si == 1 ? irop_config[sq->op].has_src1
                                                                  : irop_config[sq->op].has_src2;
      if (!has)
        continue;
      IROperand o = tcc_ir_op_get_slot(ir, sq, si);
      if (irop_get_tag(o) != IROP_TAG_STACKOFF || !o.is_local || o.is_lval)
        continue;
      int off = (int)irop_get_imm64_ex(ir, o);
      if (off >= lo && off < hi)
        return 1;
    }
  }
  return 0;
}

/* Could instruction SQ read or write the memcpy destination?  For a stack
 * destination, [obj_lo, obj_hi) is its frame object and ADDR_TAKEN says
 * whether a pointer may reach it; for a pointer destination (dst_is_stackoff
 * 0) any memory access not provably elsewhere counts. */
static int mtis_may_touch_dst(TCCIRState *ir, IRQuadCompact *sq, int dst_is_stackoff, int obj_lo, int obj_hi,
                              int addr_taken, int call_id)
{
  /* An indirect access reaches a stack destination only if its address is taken. */
  int indirect_hits = dst_is_stackoff ? addr_taken : 1;
  if (!mtis_plain_op(sq->op))
    return indirect_hits;
  int base_slot = -1; /* operand slot used as an address base by an indexed/postinc access */
  if (sq->op == TCCIR_OP_STORE_INDEXED || sq->op == TCCIR_OP_STORE_POSTINC)
    base_slot = 0;
  else if (sq->op == TCCIR_OP_LOAD_INDEXED || sq->op == TCCIR_OP_LOAD_POSTINC)
    base_slot = 1;
  for (int si = 0; si < 3; si++)
  {
    int has = si == 0 ? irop_config[sq->op].has_dest : si == 1 ? irop_config[sq->op].has_src1
                                                                : irop_config[sq->op].has_src2;
    if (!has)
      continue;
    IROperand o = tcc_ir_op_get_slot(ir, sq, si);
    int tag = irop_get_tag(o);
    int32_t vr = irop_get_vreg(o);
    if (tag == IROP_TAG_STACKOFF && o.is_local)
    {
      if (o.is_llocal)
      {
        if (indirect_hits)
          return 1; /* through the pointer held in the slot */
        continue;
      }
      /* A VAR/PARAM read through its home is a register value unless its
         address is taken, in which case it lives in the frame. */
      if (vr >= 0)
      {
        IRLiveInterval *iv = tcc_ir_vreg_live_interval(ir, vr);
        if (iv && !iv->addrtaken)
          continue;
      }
      if (!o.is_lval && si != base_slot)
        continue; /* only computes an address */
      int off = (int)irop_get_imm64_ex(ir, o);
      if (dst_is_stackoff)
      {
        if (off >= obj_lo && off < obj_hi)
          return 1;
      }
      else
      {
        /* A pointer destination can only be in a frame object whose address is taken. */
        int lo, hi;
        if (!tcc_ir_frame_object_at(ir, off, &lo, &hi) || mtis_object_addr_taken(ir, lo, hi, call_id))
          return 1;
      }
      continue;
    }
    if (si == base_slot && vr >= 0)
    {
      if (indirect_hits)
        return 1;
      continue;
    }
    if (tag == IROP_TAG_VREG && o.is_lval)
    {
      if (indirect_hits)
        return 1;
      continue;
    }
    if (o.is_sym && o.is_lval && !dst_is_stackoff)
      return 1; /* a global the pointer may point to */
  }
  return 0;
}

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
    Sym *callee = tcc_ir_op_src1_sym(ir, q);
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
      int32_t ret_vr = tcc_ir_op_dest_vreg(ir, q);
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
            if (tcc_ir_op_src1_has_vreg(ir, sq) && tcc_ir_op_src1_vreg(ir, sq) == ret_vr)
            {
              has_reader = 1;
              break;
            }
          }
          if (irop_config[sq->op].has_src2)
          {
            if (tcc_ir_op_src2_has_vreg(ir, sq) && tcc_ir_op_src2_vreg(ir, sq) == ret_vr)
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
        if (lq->is_jump_target) /* a NOP can be the join */
          break;
        if (lq->op == TCCIR_OP_NOP)
          continue;
        if (lq->op == TCCIR_OP_JUMP || lq->op == TCCIR_OP_JUMPIF || lq->op == TCCIR_OP_IJUMP)
          break;
        if (!irop_config[lq->op].has_dest)
          continue;
        /* A VAR written as itself is a STACKOFF lvalue: still a definition
         * (irop_dest_defines_vreg), not one to search past. */
        IROperand ld = tcc_ir_op_get_dest(ir, lq);
        if (irop_get_vreg(ld) != src_vr || !irop_dest_defines_vreg(ld))
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
    /* When the destination slot belongs to a NAMED local, the relocated
     * stores must carry that name: an anonymous store into a named var's
     * storage is invisible to every name-keyed analysis (the anonymous
     * StackLoc DCE sees no anonymous reader, var liveness sees no named
     * writer), and NOPing the dst LEA below also un-address-takes the var.
     * The mirror image of the named-VAR source-store guard further down. */
    IROperand dst_name_op = p_dst;
    int dst_named = 0;
    if (irop_get_tag(p_dst) == IROP_TAG_STACKOFF && p_dst.is_local && !p_dst.is_lval)
    {
      dst_is_stackoff = 1;
      dst_base = (int)irop_get_imm64_ex(ir, p_dst);
      if (irop_get_vreg(p_dst) >= 0)
      {
        dst_named = 1;
        dst_name_op = p_dst;
      }
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
          if (lq->is_jump_target) /* a NOP can be the join */
            break;
          if (lq->op == TCCIR_OP_NOP)
            continue;
          if (lq->op == TCCIR_OP_JUMP || lq->op == TCCIR_OP_JUMPIF || lq->op == TCCIR_OP_IJUMP)
            break;
          if (!irop_config[lq->op].has_dest)
            continue;
          IROperand ld = tcc_ir_op_get_dest(ir, lq);
          if (irop_get_vreg(ld) != trace_vr || !irop_dest_defines_vreg(ld))
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
              if (irop_get_vreg(ls) >= 0)
              {
                dst_named = 1;
                dst_name_op = ls;
              }
              if (dst_base + total_size > tmp_base && dst_base < tmp_base + total_size) {
                dst_is_stackoff = 0;
                dst_named = 0;
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
            if (tcc_ir_op_src2_is_imm(ir, lq) && irop_has_vreg(as1) && !as1.is_lval)
            {
              trace_add += (int)tcc_ir_op_src2_imm(ir, lq);
              trace_vr = irop_get_vreg(as1);
              continue;
            }
            break;
          }
          if (lq->op == TCCIR_OP_STORE && !ld.is_lval)
          {
            if (tcc_ir_op_src1_has_vreg(ir, lq) && !tcc_ir_op_src1_is_lval(ir, lq))
            {
              trace_vr = tcc_ir_op_src1_vreg(ir, lq);
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
      if (sq->is_jump_target) /* before the skips: a NOP can be the join */
        break;
      if (sq->op == TCCIR_OP_NOP)
        continue;
      if (sq->op == TCCIR_OP_FUNCPARAMVAL || sq->op == TCCIR_OP_FUNCPARAMVOID)
        continue; /* memmove's own params */
      if (sq->op == TCCIR_OP_JUMP || sq->op == TCCIR_OP_JUMPIF || sq->op == TCCIR_OP_IJUMP)
        break;

      if (sq->op != TCCIR_OP_STORE && sq->op != TCCIR_OP_STORE_INDEXED)
      {
        /* Only one preceding zeroing memset is tracked; later ones bail. */
        if (memset_idx < 0 &&
            (sq->op == TCCIR_OP_FUNCCALLVOID || sq->op == TCCIR_OP_FUNCCALLVAL))
        {
          Sym *ms_callee = tcc_ir_op_src1_sym(ir, sq);
          const char *ms_name = ms_callee ? get_tok_str(ms_callee->v, NULL) : NULL;
          int is_memset_like = ms_name && ir_opt_name_in(ms_name, "memset\0__aeabi_memset\0");
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
                int ms_call_id = TCCIR_DECODE_CALL_ID((uint32_t)tcc_ir_op_src2_imm(ir, sq));
                int p0_idx = -1;
                for (int k = j - 1; k >= 0; --k)
                {
                  IRQuadCompact *pq = &ir->compact_instructions[k];
                  if (pq->op == TCCIR_OP_NOP)
                    continue;
                  if (pq->op != TCCIR_OP_FUNCPARAMVAL && pq->op != TCCIR_OP_FUNCPARAMVOID)
                    continue;
                  uint32_t enc = (uint32_t)tcc_ir_op_src2_imm(ir, pq);
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
          if (trace_vr >= 0)
          {
            int lea_k = mtis_trace_stack_addr(ir, j, trace_vr, trace_add, &st_off);
            if (lea_k >= 0)
            {
              st_off_found = 1;
              st_store_lea = lea_k;
            }
            if (st_off_found)
              st_size = ir_opt_store_btype_size_bytes(irop_get_btype(st_dest));
          }
        }
      }
      else /* TCCIR_OP_STORE_INDEXED */
      {
        st_src = tcc_ir_op_get_src1(ir, sq);
        if (irop_get_tag(st_dest) == IROP_TAG_VREG && irop_has_vreg(st_dest) &&
            tcc_ir_op_src2_tag(ir, sq) == IROP_TAG_IMM32)
        {
          IROperand scale_op = ir->iroperand_pool[sq->operand_base + 3];
          int scale_val = (int)irop_get_imm64_ex(ir, scale_op);
          if (scale_val == 0)
          {
            int32_t trace_vr = irop_get_vreg(st_dest);
            int idx_val = (int)tcc_ir_op_src2_imm(ir, sq);
            int trace_add = idx_val;
            if (trace_vr >= 0)
            {
              int lea_k = mtis_trace_stack_addr(ir, j, trace_vr, trace_add, &st_off);
              if (lea_k >= 0)
              {
                st_off_found = 1;
                st_store_lea = lea_k;
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

    /* The memset rewrite only shifts PARAM0's offset, leaving an anonymous
     * address of the named var's slot behind — the same invisibility this
     * pass must avoid.  Rare combination; just refuse it. */
    if (dst_named && memset_idx >= 0)
      continue;
    /* The memset is only moved onto a stack destination (below); through a
     * pointer it would stay on the dead temporary, and the bytes only it
     * zeroes -- a trailing `0` member of a compound literal, the padding --
     * would never reach dst.  frame_dfe.c's `d->acc[n] = (DfeAcc){..., 0}`
     * left `dead` as heap garbage, and the device tcc dropped copies. */
    if (!dst_is_stackoff && memset_idx >= 0)
      continue;

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
        if (TCCIR_DECODE_CALL_ID((uint32_t)tcc_ir_op_src2_imm(ir, sq)) ==
            TCCIR_DECODE_CALL_ID((uint32_t)tcc_ir_op_src2_imm(ir, q)))
          continue;
      }

      /* The memset's PARAM0 is shifted to dst during the rewrite. */
      if (memset_idx >= 0)
      {
        if (j == memset_idx)
          continue;
        if (sq->op == TCCIR_OP_FUNCPARAMVAL || sq->op == TCCIR_OP_FUNCPARAMVOID)
        {
          if (TCCIR_DECODE_CALL_ID((uint32_t)tcc_ir_op_src2_imm(ir, sq)) ==
              TCCIR_DECODE_CALL_ID((uint32_t)tcc_ir_op_src2_imm(ir, &ir->compact_instructions[memset_idx])))
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
        if (irop_get_vreg(d) == dst_vr && irop_dest_defines_vreg(d))
        {
          dst_def_idx = j;
          break;
        }
      }
      if (dst_def_idx < 0 || dst_def_idx >= earliest_store_idx)
        continue;
    }

    /* Relocated stores write dst EARLIER than the memcpy did: nothing between
       the first of them and the call may read or write dst -- not a direct
       access to its frame object, not a load or store through a pointer that
       may reach it, not a call. */
    {
      int win_lo = i;
      for (int s = 0; s < nstores; s++)
        if (store_indices[s] < win_lo)
          win_lo = store_indices[s];
      if (memset_idx >= 0 && memset_idx < win_lo)
        win_lo = memset_idx;
      int call_id = TCCIR_DECODE_CALL_ID((uint32_t)tcc_ir_op_src2_imm(ir, q));
      int ms_call_id = memset_idx >= 0 ? TCCIR_DECODE_CALL_ID((uint32_t)tcc_ir_op_src2_imm(ir, &ir->compact_instructions[memset_idx]))
                                       : -1;
      int obj_lo = 0, obj_hi = 0, addr_taken = 1;
      if (dst_is_stackoff)
      {
        if (tcc_ir_frame_object_at(ir, dst_base, &obj_lo, &obj_hi))
          addr_taken = mtis_object_addr_taken(ir, obj_lo, obj_hi, call_id);
        else
        {
          obj_lo = dst_base;
          obj_hi = dst_base + total_size;
        }
        if (obj_lo > dst_base)
          obj_lo = dst_base;
        if (obj_hi < dst_base + total_size)
          obj_hi = dst_base + total_size;
      }
      int dst_safe = 1;
      for (int j = win_lo + 1; j < i && dst_safe; j++)
      {
        IRQuadCompact *sq = &ir->compact_instructions[j];
        if (sq->op == TCCIR_OP_NOP || j == lea_idx || j == memset_idx)
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
        /* Params of the memcpy and of the memset that moves with it. */
        if (sq->op == TCCIR_OP_FUNCPARAMVAL || sq->op == TCCIR_OP_FUNCPARAMVOID)
        {
          int pid = TCCIR_DECODE_CALL_ID((uint32_t)tcc_ir_op_src2_imm(ir, sq));
          if (pid == call_id || pid == ms_call_id)
            continue;
        }
        if (mtis_may_touch_dst(ir, sq, dst_is_stackoff, obj_lo, obj_hi, addr_taken, call_id))
          dst_safe = 0;
      }
      if (!dst_safe)
        continue;
    }

    /* The LEA producing p_src must have no user other than the memmove's PARAM1. */
    if (lea_idx >= 0)
    {
      int32_t lea_vr = tcc_ir_op_dest_vreg(ir, &ir->compact_instructions[lea_idx]);
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
          if (TCCIR_DECODE_CALL_ID((uint32_t)tcc_ir_op_src2_imm(ir, sq)) ==
                  TCCIR_DECODE_CALL_ID((uint32_t)tcc_ir_op_src2_imm(ir, q)) &&
              TCCIR_DECODE_PARAM_IDX((uint32_t)tcc_ir_op_src2_imm(ir, sq)) == 1)
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
        if (dst_named)
        {
          /* The store must write the named var AS that var: keep its vreg
           * identity so name-keyed liveness/DCE see the definition, with the
           * per-store width and interior offset (the same shape a struct
           * member write through the var produces). */
          int store_btype = (sq->op == TCCIR_OP_STORE_INDEXED)
                            ? irop_get_btype(st_src)
                            : irop_get_btype(st_dest_old);
          IROperand new_dest = dst_name_op;
          new_dest.is_lval = 1;
          new_dest.btype = store_btype;
          new_dest.u.imm32 = dst_base + offset;
          tcc_ir_pool_ensure(ir, 2);
          int new_pool = ir->iroperand_pool_count;
          tcc_ir_pool_add(ir, new_dest);
          tcc_ir_pool_add(ir, st_src);
          sq->op = TCCIR_OP_STORE;
          sq->operand_base = new_pool;
        }
        else if (irop_get_tag(st_dest_old) == IROP_TAG_STACKOFF && st_dest_old.is_local &&
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
          new_dest.aux |= IROP_AUX_NONVOLATILE; /* a memmove's store: no volatile access */
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
