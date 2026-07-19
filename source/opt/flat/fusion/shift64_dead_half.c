/*
 *  TCC IR - 64-bit Register Pair Optimization
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
#include "opt_utils.h"


/* tcc_ir_opt_shift64_dead_half: flag a 64-bit SHL whose result's low word is
 * dead so codegen can skip materializing it.  Targets the 64-bit
 * bitfield-extract idiom for a sub-32-bit field spanning a storage-unit word
 * boundary:
 *
 *   T1 = V  SHL #a      ; i64, T1 has a single use: the SHR below
 *   T2 = T1 SHR #b      ; i64, b >= 32  -> reads ONLY T1's high word
 *
 * A 64-bit SHR/SAR by >= 32 reads only its source's HIGH word, so a SHL whose
 * sole consumer is such a shift has a provably dead LOW word.  Skipping its
 * `lsl dst_lo, src_lo, #a` removes one instruction per spanning-field extract.
 *
 * Writes ir->shift64_dead_half[orig_index] = bit0:skip_lo (bit1:skip_hi is
 * honoured by codegen but not currently emitted — left for a future, equally
 * safe extension).  Pure annotation: no IR mutation.  Anchored from the SHR
 * consumer and gated on single-use of T1, so it stays valid across RA spills
 * (which store/reload the dead low word as never-read garbage). */
int tcc_ir_opt_shift64_dead_half(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  if (n < 2)
    return 0;

  if (ir->shift64_dead_half)
  {
    tcc_free(ir->shift64_dead_half);
    ir->shift64_dead_half = NULL;
    ir->shift64_dead_half_len = 0;
  }

  /* Build last-def index for TEMPs (single-def in practice; the SHL we match
   * is single-use so its def is unambiguous). */
  int max_tmp_pos = 0;
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP || !irop_config[q->op].has_dest)
      continue;
    int32_t vr = irop_get_vreg(tcc_ir_op_get_dest(ir, q));
    if (TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_TEMP)
    {
      int p = TCCIR_DECODE_VREG_POSITION(vr);
      if (p > max_tmp_pos)
        max_tmp_pos = p;
    }
  }
  if (max_tmp_pos == 0)
    return 0;

  int stride = max_tmp_pos + 1;
  int *def_idx = tcc_malloc(stride * sizeof(int));
  for (int i = 0; i < stride; i++)
    def_idx[i] = -1;
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP || !irop_config[q->op].has_dest)
      continue;
    int32_t vr = irop_get_vreg(tcc_ir_op_get_dest(ir, q));
    if (TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_TEMP)
    {
      int p = TCCIR_DECODE_VREG_POSITION(vr);
      if (p <= max_tmp_pos)
        def_idx[p] = i;
    }
  }

  int changes = 0;
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    /* Consumer: a 64-bit SHR/SAR by >= 32, reading a 64-bit TEMP src1. */
    if (q->op != TCCIR_OP_SHR && q->op != TCCIR_OP_SAR)
      continue;
    IROperand s2 = tcc_ir_op_get_src2(ir, q);
    if (!irop_is_immediate(s2) || irop_get_imm64_ex(ir, s2) < 32)
      continue;
    IROperand s1 = tcc_ir_op_get_src1(ir, q);
    if (irop_get_btype(s1) != IROP_BTYPE_INT64)
      continue;
    int32_t s1_vr = irop_get_vreg(s1);
    if (TCCIR_DECODE_VREG_TYPE(s1_vr) != TCCIR_VREG_TYPE_TEMP)
      continue;
    int s1_pos = TCCIR_DECODE_VREG_POSITION(s1_vr);
    if (s1_pos > max_tmp_pos || def_idx[s1_pos] < 0)
      continue;

    /* Producer must be a 64-bit SHL feeding only this shift. */
    int dpos = def_idx[s1_pos];
    IRQuadCompact *def = &ir->compact_instructions[dpos];
    if (def->op != TCCIR_OP_SHL)
      continue;
    if (irop_get_btype(tcc_ir_op_get_dest(ir, def)) != IROP_BTYPE_INT64)
      continue;
    if (!tcc_ir_vreg_has_single_use(ir, s1_vr, dpos))
      continue;

    if (!ir->shift64_dead_half) {
      ir->shift64_dead_half = tcc_mallocz(ir->max_orig_index + 1);
      ir->shift64_dead_half_len = ir->max_orig_index + 1;
    }
    ir->shift64_dead_half[def->orig_index] |= 1; /* skip_lo */
    changes++;
  }

  tcc_free(def_idx);
  return changes;
}


