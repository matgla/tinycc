/*
 *  TCC IR - Redundant bitfield insert/extract elimination (flat, pre-SSA)
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#define USING_GLOBALS

#include "tcc.h"
#include "tccir.h"
#include "tccir_operand.h"
#include "ir.h"
#include "opt.h"
#include "opt_utils.h"
#include "opt_engine.h"
#include "log.h"

#ifndef LOG_BITFIELD
#ifdef TCC_LOG_BITFIELD
#define LOG_BITFIELD(...) fprintf(stderr, "[BITFIELD] " __VA_ARGS__), fprintf(stderr, "\n")
#else
#define LOG_BITFIELD(...) ((void)0)
#endif
#endif

static int bf_const_u32(TCCIRState *ir, IROperand op, uint32_t *out)
{
  if (irop_is_immediate(op) && !op.is_sym)
  {
    *out = (uint32_t)irop_get_imm64_ex(ir, op);
    return 1;
  }
  return 0;
}

static int bf_is_const(TCCIRState *ir, IROperand op)
{
  uint32_t tmp;
  return bf_const_u32(ir, op, &tmp);
}

static int32_t bf_temp_vreg(IROperand op)
{
  if (op.is_lval)
    return -1;
  int32_t vr = irop_get_vreg(op);
  if (vr < 0 || TCCIR_DECODE_VREG_TYPE(vr) != TCCIR_VREG_TYPE_TEMP)
    return -1;
  return vr;
}

static int bf_possible_bits(TCCIRState *ir, IROperand op, int before_idx, uint32_t *out)
{
  if (bf_const_u32(ir, op, out))
    return 1;
  int32_t vr = bf_temp_vreg(op);
  if (vr < 0)
    return 0;
  /* Multi-def TEMPs under-report bits (nearest-def only); only trust single-def bounds. */
  if (!tcc_ir_vreg_has_single_def(ir, vr))
    return 0;
  int d = tcc_ir_find_defining_instruction(ir, vr, before_idx);
  if (d < 0)
    return 0;
  IRQuadCompact *dq = &ir->compact_instructions[d];
  IROperand a1 = tcc_ir_op_get_src1(ir, dq);
  IROperand a2 = tcc_ir_op_get_src2(ir, dq);

  if (dq->op == TCCIR_OP_ASSIGN)
    return bf_possible_bits(ir, a1, d, out);

  if (irop_get_btype(tcc_ir_op_get_dest(ir, dq)) != IROP_BTYPE_INT32)
    return 0;

  switch (dq->op)
  {
  case TCCIR_OP_AND:
    if (bf_const_u32(ir, a2, out))
      return 1;
    if (bf_const_u32(ir, a1, out))
      return 1;
    return 0;
  case TCCIR_OP_SHL:
  case TCCIR_OP_SHR:
  {
    uint32_t sv;
    if (bf_const_u32(ir, a2, &sv))
    {
      int s = (int)sv;
      if (s >= 0 && s < 32)
      {
        *out = (dq->op == TCCIR_OP_SHL) ? (0xffffffffu << s) : (0xffffffffu >> s);
        return 1;
      }
    }
    return 0;
  }
  case TCCIR_OP_UBFX:
  {
    /* src2 packs lsb (bits 0-4) and width (bits 5-9); width 0 encodes 8. */
    uint32_t pv;
    if (bf_const_u32(ir, a2, &pv))
    {
      int param = (int)pv;
      int width = (param >> 5) & 0x1F;
      if (width == 0)
        width = 8;
      *out = (width >= 32) ? 0xffffffffu : (((uint32_t)1u << width) - 1u);
      return 1;
    }
    return 0;
  }
  default:
    return 0;
  }
}

static int bf_value_fits_unsigned(TCCIRState *ir, IROperand op, int bits, int before_idx)
{
  if (bits <= 0)
    return 0;
  if (bits >= 32)
    return 1;
  uint32_t pb;
  if (!bf_possible_bits(ir, op, before_idx, &pb))
    return 0;
  return (pb >> bits) == 0;
}

static int bf_make_copy_src(IROperand v, int dest_btype, IROperand *src)
{
  if (irop_is_immediate(v) && !v.is_sym)
  {
    *src = v;
    return 1;
  }
  if (v.is_lval)
    return 0;
  int32_t v_vr = irop_get_vreg(v);
  if (v_vr < 0)
    return 0;
  *src = irop_make_vreg(v_vr, dest_btype);
  return 1;
}

static int bf_real_def(TCCIRState *ir, int32_t vr, int before_idx)
{
  for (int guard = 0; guard < 64; guard++)
  {
    if (vr < 0 || TCCIR_DECODE_VREG_TYPE(vr) != TCCIR_VREG_TYPE_TEMP)
      return -1;
    int d = tcc_ir_find_defining_instruction(ir, vr, before_idx);
    if (d < 0)
      return -1;
    IRQuadCompact *dq = &ir->compact_instructions[d];
    if (dq->op != TCCIR_OP_ASSIGN)
      return d;
    IROperand a1 = tcc_ir_op_get_src1(ir, dq);
    if (a1.is_lval)
      return -1;
    vr = irop_get_vreg(a1);
    before_idx = d;
  }
  return -1;
}

int tcc_ir_opt_bitfield_insert_extract(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  if (n < 3)
    return 0;

  int changes = 0;

  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_SHR && q->op != TCCIR_OP_AND)
      continue;

    IROperand dest = tcc_ir_op_get_dest(ir, q);
    if (dest.is_lval || irop_get_btype(dest) != IROP_BTYPE_INT32)
      continue;
    int32_t dest_vr = irop_get_vreg(dest);
    if (dest_vr < 0 || TCCIR_DECODE_VREG_TYPE(dest_vr) != TCCIR_VREG_TYPE_TEMP)
      continue;

    IROperand sop = tcc_ir_op_get_src1(ir, q);
    int32_t s_vr = bf_temp_vreg(sop);
    if (s_vr < 0)
      continue;
    int def_or = bf_real_def(ir, s_vr, i);
    if (def_or < 0)
      continue;
    IRQuadCompact *orq = &ir->compact_instructions[def_or];

    int outer_shl = 0;
    if (q->op == TCCIR_OP_SHR && orq->op == TCCIR_OP_SHL)
    {
      IROperand sa = tcc_ir_op_get_src2(ir, orq);
      uint32_t sav;
      if (bf_const_u32(ir, sa, &sav))
      {
        int av = (int)sav;
        IROperand inner = tcc_ir_op_get_src1(ir, orq);
        int32_t iv = inner.is_lval ? -1 : irop_get_vreg(inner);
        if (av >= 1 && av <= 31 && iv >= 0 && TCCIR_DECODE_VREG_TYPE(iv) == TCCIR_VREG_TYPE_TEMP)
        {
          int di = bf_real_def(ir, iv, def_or);
          if (di >= 0 && ir->compact_instructions[di].op == TCCIR_OP_OR)
          {
            outer_shl = av;
            def_or = di;
            orq = &ir->compact_instructions[di];
          }
        }
      }
    }
    if (orq->op != TCCIR_OP_OR)
      continue;

    IROperand or_a = tcc_ir_op_get_src1(ir, orq);
    IROperand or_b = tcc_ir_op_get_src2(ir, orq);

    if (q->op == TCCIR_OP_SHR)
    {
      IROperand shn = tcc_ir_op_get_src2(ir, q);
      if (!bf_is_const(ir, shn))
        continue;
      int b = (int)irop_get_imm64_ex(ir, shn);
      if (b < 1 || b > 31)
        continue;
      if (outer_shl > b)
        continue;
      int s_eff = b - outer_shl;
      int w = 32 - b;
      if (w < 1 || w > 31)
        continue;
      uint32_t field_window = ((1u << w) - 1) << s_eff;

      for (int trial = 0; trial < 2; trial++)
      {
        IROperand high = trial ? or_b : or_a;
        IROperand low = trial ? or_a : or_b;

        uint32_t low_bits;
        if (!bf_possible_bits(ir, low, def_or, &low_bits))
          continue;
        if ((low_bits & field_window) != 0)
          continue;

        IROperand v;
        if (s_eff == 0)
        {
          v = high;
        }
        else
        {
          int32_t h_vr = bf_temp_vreg(high);
          if (h_vr < 0)
            continue;
          int def_shl = bf_real_def(ir, h_vr, def_or);
          if (def_shl < 0)
            continue;
          IRQuadCompact *shlq = &ir->compact_instructions[def_shl];
          if (shlq->op != TCCIR_OP_SHL)
            continue;
          IROperand shl_n = tcc_ir_op_get_src2(ir, shlq);
          if (!bf_is_const(ir, shl_n))
            continue;
          if ((int)irop_get_imm64_ex(ir, shl_n) != s_eff)
            continue;
          v = tcc_ir_op_get_src1(ir, shlq);
        }

        if (!bf_value_fits_unsigned(ir, v, w, def_or))
          continue;

        IROperand new_src;
        if (!bf_make_copy_src(v, IROP_BTYPE_INT32, &new_src))
          continue;

        q->op = TCCIR_OP_ASSIGN;
        tcc_ir_set_src1(ir, i, new_src);
        tcc_ir_set_src2(ir, i, IROP_NONE);
        LOG_BITFIELD("@%d: ((V SHL %d) | low) extract folded to field value (a=%d b=%d w=%d)", i, s_eff, outer_shl,
                     b, w);
        changes++;
        break;
      }
    }
    else
    {
      IROperand mop = tcc_ir_op_get_src2(ir, q);
      if (!bf_is_const(ir, mop))
        continue;
      uint32_t m = (uint32_t)irop_get_imm64_ex(ir, mop);
      if (m == 0 || m == 0xffffffffu)
        continue;

      for (int trial = 0; trial < 2; trial++)
      {
        IROperand lowpart = trial ? or_b : or_a;
        IROperand other = trial ? or_a : or_b;

        uint32_t low_bits, other_bits;
        if (!bf_possible_bits(ir, lowpart, def_or, &low_bits))
          continue;
        if ((low_bits & ~m) != 0)
          continue;
        if (!bf_possible_bits(ir, other, def_or, &other_bits))
          continue;
        if ((other_bits & m) != 0)
          continue;

        IROperand new_src;
        if (!bf_make_copy_src(lowpart, IROP_BTYPE_INT32, &new_src))
          continue;

        q->op = TCCIR_OP_ASSIGN;
        tcc_ir_set_src1(ir, i, new_src);
        tcc_ir_set_src2(ir, i, IROP_NONE);
        LOG_BITFIELD("@%d: (OR@%d) & %#x folded to low field value", i, def_or, m);
        changes++;
        break;
      }
    }
  }

  return changes;
}

int tcc_ir_opt_bitfield_insert_extract_ex(IROptCtx *ctx)
{
  return tcc_ir_opt_bitfield_insert_extract(ctx->ir);
}

/* Must run before tcc_ir_barrel_shift_fusion (which would fold the SHL into the OR). */
static int bf_thumb_modified_imm_ok(uint32_t imm)
{
  if ((imm & 0xffffff00u) == 0)
    return 1;
  if (!(imm & 0xff00ff00u) && (imm >> 16) == (imm & 0xffu))
    return 1;
  if (!(imm & 0x00ff00ffu) && ((imm >> 16) & 0xff00u) == (imm & 0xff00u))
    return 1;
  if ((imm & 0xffffu) == ((imm >> 16) & 0xffffu) && ((imm >> 8) & 0xffu) == (imm & 0xffu))
    return 1;
  for (uint32_t j = 0; j <= 23; j++)
  {
    uint32_t mask = 0xFF000000u >> j;
    uint32_t one = 0x80000000u >> j;
    if ((imm & one) == one && (imm & ~mask) == 0)
      return 1;
  }
  return 0;
}

int tcc_ir_opt_bitfield_insert_to_bfi(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  int changes = 0;

  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *orq = &ir->compact_instructions[i];
    if (orq->op != TCCIR_OP_OR || orq->is_jump_target)
      continue;
    IROperand dest = tcc_ir_op_get_dest(ir, orq);
    if (dest.is_lval || irop_get_btype(dest) != IROP_BTYPE_INT32)
      continue;

    IROperand o1 = tcc_ir_op_get_src1(ir, orq);
    IROperand o2 = tcc_ir_op_get_src2(ir, orq);

    for (int swap = 0; swap < 2 && orq->op == TCCIR_OP_OR; swap++)
    {
      IROperand and_side = swap ? o2 : o1;
      IROperand val_side = swap ? o1 : o2;

      if (and_side.is_lval || !irop_has_vreg(and_side))
        continue;
      int32_t and_vr = irop_get_vreg(and_side);
      if (and_vr < 0 || TCCIR_DECODE_VREG_TYPE(and_vr) != TCCIR_VREG_TYPE_TEMP)
        continue;
      int and_idx = tcc_ir_find_defining_instruction(ir, and_vr, i);
      if (and_idx < 0 || ir->compact_instructions[and_idx].op != TCCIR_OP_AND)
        continue;
      IRQuadCompact *andq = &ir->compact_instructions[and_idx];
      IROperand mop = tcc_ir_op_get_src2(ir, andq);
      if (!bf_is_const(ir, mop))
        continue;
      uint32_t clearmask = (uint32_t)irop_get_imm64_ex(ir, mop);
      uint32_t fieldmask = ~clearmask;
      if (fieldmask == 0)
        continue;
      int lsb_field = __builtin_ctz(fieldmask);
      int width = __builtin_popcount(fieldmask);
      if (lsb_field + width > 32 ||
          ((((uint64_t)1u << width) - 1) << lsb_field) != fieldmask)
        continue;

      /* Non-increasing gate: skip Thumb-2-encodable clearmasks (BFI's mov could break even/regress). */
      if (bf_thumb_modified_imm_ok(clearmask))
        continue;

      IROperand word_op = tcc_ir_op_get_src1(ir, andq);
      if (word_op.is_lval || !irop_has_vreg(word_op))
        continue;
      int32_t word_vr = irop_get_vreg(word_op);

      /* Non-increasing gate: skip only when the word is provably field-clear (clear is a DCE'd no-op → BFI regresses). */
      {
        uint32_t word_pb;
        if (bf_possible_bits(ir, word_op, and_idx, &word_pb) && (word_pb & fieldmask) == 0)
          continue;
      }

      if (val_side.is_lval || !irop_has_vreg(val_side))
        continue;
      int32_t val_vr = irop_get_vreg(val_side);
      int lsb;
      IROperand value_op;
      int shl_idx = -1;
      int32_t shl_res_vr = -1;
      int vdef = (val_vr >= 0 && TCCIR_DECODE_VREG_TYPE(val_vr) == TCCIR_VREG_TYPE_TEMP)
                     ? tcc_ir_find_defining_instruction(ir, val_vr, i)
                     : -1;
      if (vdef >= 0 && ir->compact_instructions[vdef].op == TCCIR_OP_SHL)
      {
        IRQuadCompact *shlq = &ir->compact_instructions[vdef];
        IROperand sn = tcc_ir_op_get_src2(ir, shlq);
        if (!bf_is_const(ir, sn))
          continue;
        lsb = (int)irop_get_imm64_ex(ir, sn);
        IROperand vsrc = tcc_ir_op_get_src1(ir, shlq);
        if (vsrc.is_lval || !irop_has_vreg(vsrc))
          continue;
        value_op = vsrc;
        shl_idx = vdef;
        shl_res_vr = val_vr;
      }
      else
      {
        lsb = 0;
        value_op = val_side;
      }
      if (lsb != lsb_field || lsb < 0 || lsb > 31 || width < 1)
        continue;

      /* BFI needs the value vreg distinct from the word base (self-insert is degenerate). */
      int32_t value_vr = irop_get_vreg(value_op);
      if (value_op.is_lval || value_vr < 0 || value_vr == word_vr)
        continue;

      /* Equivalence needs V < 2^width (BFI inserts only the low width bits). */
      if (!bf_value_fits_unsigned(ir, value_op, width, i))
        continue;

      /* Single-use gates: NOPing the AND / SHL must drop exactly those insns. */
      if (!tcc_ir_vreg_has_single_use(ir, and_vr, and_idx))
        continue;
      if (shl_idx >= 0 && !tcc_ir_vreg_has_single_use(ir, shl_res_vr, shl_idx))
        continue;

      /* W and V must be unmodified (and no CFG edge) up to the OR; check W in (and_idx,i), V in (shl_idx,i). */
      int lo = and_idx;
      if (shl_idx >= 0 && shl_idx < lo)
        lo = shl_idx;
      int safe = 1;
      for (int j = lo + 1; j < i && safe; j++)
      {
        IRQuadCompact *jq = &ir->compact_instructions[j];
        if (jq->op == TCCIR_OP_NOP)
          continue;
        if (jq->op == TCCIR_OP_JUMP || jq->op == TCCIR_OP_JUMPIF || jq->op == TCCIR_OP_IJUMP ||
            jq->op == TCCIR_OP_SWITCH_TABLE || jq->is_jump_target)
        {
          safe = 0;
          break;
        }
        if (irop_config[jq->op].has_dest)
        {
          IROperand jd = tcc_ir_op_get_dest(ir, jq);
          if (irop_has_vreg(jd))
          {
            int32_t jdv = irop_get_vreg(jd);
            if ((j > and_idx && jdv == word_vr) || (shl_idx >= 0 && j > shl_idx && jdv == value_vr))
            {
              safe = 0;
              break;
            }
          }
        }
      }
      if (!safe)
        continue;

      if (!ir->bfi_params) {
        ir->bfi_params = tcc_mallocz((size_t)(ir->max_orig_index + 1) * sizeof(uint16_t));
        ir->bfi_params_len = ir->max_orig_index + 1;
      }

      orq->op = TCCIR_OP_BFI;
      tcc_ir_set_src1(ir, i, word_op);
      tcc_ir_set_src2(ir, i, value_op);
      ir->bfi_params[orq->orig_index] = (uint16_t)((lsb & 0xFF) | ((width & 0xFF) << 8));
      andq->op = TCCIR_OP_NOP;
      if (shl_idx >= 0)
        ir->compact_instructions[shl_idx].op = TCCIR_OP_NOP;
      changes++;
      LOG_BITFIELD("INSERT->BFI @%d: lsb=%d width=%d (AND@%d SHL@%d)", i, lsb, width, and_idx, shl_idx);
    }
  }

  return changes;
}
