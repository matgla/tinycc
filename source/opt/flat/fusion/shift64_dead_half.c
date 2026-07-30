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


/* tcc_ir_opt_shift64_dead_half: flag the halves of a 64-bit shift result that
 * are provably dead, so codegen can skip materializing them.  Both rules target
 * the 64-bit bitfield-extract idiom for a field spanning a storage-unit word
 * boundary:
 *
 *   T1 = V  SHL #a      ; i64, T1 has a single use: the SHR below
 *   T2 = T1 SHR #b      ; i64, b >= 32  -> reads ONLY T1's high word
 *   T3 = T2             ; i32 dest      -> reads ONLY T2's low word
 *
 * bit0 (skip_lo): a 64-bit SHR/SAR by >= 32 reads only its source's HIGH word,
 * so a SHL whose sole consumer is such a shift has a provably dead LOW word.
 * Skipping its `lsl dst_lo, src_lo, #a` removes one instruction per extract.
 *
 * bit1 (skip_hi): that same SHR/SAR fills its own HIGH word with a constant
 * (0 for SHR, the sign for SAR).  When its sole consumer reads only the low
 * word, the fill is never read and the `mov dst_hi, #0` / `asr dst_hi, #31`
 * is pure waste.  See s64_mark_dead_hi.
 *
 * Pure annotation: no IR mutation.  Both rules are gated on single-use, so they
 * stay valid across RA spills (which store/reload the dead word as never-read
 * garbage). */

/* A use of `vreg` at instruction `u` reads only the LOW word of the pair when
 * the operand naming it is not itself pair-typed.  Codegen derives
 * MachineOperand.is_64bit straight from the operand's own btype
 * (machine_op.c:89 -> irop_needs_pair), so a non-pair operand only ever
 * materializes the low register — the high one is never referenced.
 * Returns 0 unless `u` names `vreg` and every slot naming it is low-only. */
static int s64_use_reads_low_only(TCCIRState *ir, IRQuadCompact *u, int32_t vreg)
{
  IROperand s1 = tcc_ir_op_get_src1(ir, u);
  IROperand s2 = tcc_ir_op_get_src2(ir, u);
  int m1 = (irop_get_vreg(s1) == vreg);
  int m2 = (irop_get_vreg(s2) == vreg);
  if (!m1 && !m2)
    return 0;

  /* A truncating ASSIGN keeps its SOURCE operand pair-typed while narrowing —
   * the width that decides what is read is the DEST's.  tcc_gen_machine_assign_mop
   * dispatches on `dest.is_64bit` and its `src.is_64bit && !dest.is_64bit` arm
   * copies the low half only, never naming the high register.  This is the shape
   * the 64-bit bitfield extract ends in, so without this case the rule below
   * would reject every real candidate. */
  if (u->op == TCCIR_OP_ASSIGN && m1 && !m2 &&
      !irop_needs_pair(tcc_ir_op_get_dest(ir, u)))
    return 1;

  if (m1 && irop_needs_pair(s1))
    return 0;
  if (m2 && irop_needs_pair(s2))
    return 0;
  return 1;
}

/* Mark bit1 (skip_hi) on a 64-bit SHR/SAR by >= 32 whose result's HIGH word is
 * provably dead, so codegen skips the `mov dst_hi, #0` / `asr dst_hi, #31` that
 * materializes it.
 *
 * Such a shift's high word is a constant fill (0 for SHR, the sign for SAR).
 * When the value's SINGLE use reads only the low word — i.e. the operand naming
 * it is not pair-typed, which is precisely how the bitfield-extract idiom
 * consumes it (`T2(i64) = T1 SHR #41; T3(i32) = T2`) — nothing ever reads the
 * fill, so emitting it is pure waste.
 *
 * Only right shifts by >= 32 are marked: those are exactly the emitter paths
 * that honour skip_hi (thumb_emit_shift64_mop's sh==32 / sh<64 / sh>=64 arms).
 * Gated on single-use so the analysis stays valid across RA spills, which may
 * store/reload the dead high word as never-read garbage — same reasoning as the
 * skip_lo rule above.  Pure annotation: no IR mutation. */
static int s64_mark_dead_hi(TCCIRState *ir, int n)
{
  int changes = 0;

  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_SHR && q->op != TCCIR_OP_SAR)
      continue;
    IROperand s2 = tcc_ir_op_get_src2(ir, q);
    if (!irop_is_immediate(s2) || irop_get_imm64_ex(ir, s2) < 32)
      continue;
    IROperand dest = tcc_ir_op_get_dest(ir, q);
    if (!irop_needs_pair(dest))
      continue;
    /* Register destination only.  For a memory/lval dest the emitter's
     * `store_hi && !skip_hi` writeback guard would leave the high 4 bytes of the
     * destination object stale, and this analysis only proves that no VREG use
     * reads the high half — it says nothing about reads of that memory. */
    if (dest.is_lval)
      continue;
    int32_t dv = irop_get_vreg(dest);
    if (TCCIR_DECODE_VREG_TYPE(dv) != TCCIR_VREG_TYPE_TEMP)
      continue;
    if (q->orig_index < 0 || q->orig_index > ir->max_orig_index)
      continue;
    /* Single use keeps the consumer unambiguous (and rejects the MLA-accumulator
     * blind spot, which tcc_ir_vreg_has_single_use reports as multi-use). */
    if (!tcc_ir_vreg_has_single_use(ir, dv, i))
      continue;

    int low_only = 0;
    for (int j = 0; j < n; j++)
    {
      if (j == i)
        continue;
      IRQuadCompact *u = &ir->compact_instructions[j];
      if (u->op == TCCIR_OP_NOP)
        continue;
      if (irop_get_vreg(tcc_ir_op_get_src1(ir, u)) != dv &&
          irop_get_vreg(tcc_ir_op_get_src2(ir, u)) != dv)
        continue;
      low_only = s64_use_reads_low_only(ir, u, dv);
      break;
    }
    if (!low_only)
      continue;

    if (!ir->shift64_dead_half)
    {
      ir->shift64_dead_half = tcc_mallocz(ir->max_orig_index + 1);
      ir->shift64_dead_half_len = ir->max_orig_index + 1;
    }
    ir->shift64_dead_half[q->orig_index] |= 2; /* skip_hi */
    changes++;
  }

  return changes;
}

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

  changes += s64_mark_dead_hi(ir, n);

  tcc_free(def_idx);
  return changes;
}


