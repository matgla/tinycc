/*
 *  ARMvX-m code generator for TCC
 *  Uses thumb instruction set
 *
 *  Based on:
 *  ARM Thumb 2 instruction functions for TCC
 *  Copyright (c) 2020 Erlend J. Sveen
 *  from:
 * https://git.erlendjs.no/erlendjs/tinycc/-/blob/arm-thumb/arm-thumb-gen.c
 *        https://git.erlendjs.no/erlendjs/tinycc/-/blob/arm-thumb/arm-thumb-instructions.c
 *
 *  And
 *
 *  ARMv4 code generator for TCC
 *
 *  Copyright (c) 2003 Daniel Glöckner
 *  Copyright (c) 2012 Thomas Preud'homme
 *
 *  Based on i386-gen.c by Fabrice Bellard
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

/* Thumb-2 code generator: 32- and 64-bit data processing, shifts,
 * multiply/divide/modulo and compare lowering of MachineOperands. */

#include "arm-thumb-gen.h"

/* tcc_gen_machine_data_processing_mop: MachineOperand-based entry point for
 * arithmetic/logic operations.  Called from ir/codegen.c when dest does not
 * use a static chain register.
 * Dispatches to thumb_emit_data_processing_mop64 / thumb_emit_shift64_mop for
 * 64-bit pair destinations, or thumb_emit_data_processing_mop32 for 32-bit.
 */
static void data_processing_mop_impl(MachineOperand src1, MachineOperand src2, MachineOperand dest, TccIrOp op,
                                     thumb_flags_behaviour flags_override, uint32_t barrel_shift);

static ThumbAndImmForm thumb_and_imm_form(uint32_t mask)
{
  /* Zero-extends: a 2-byte encoding when both registers are low, against 4
   * bytes for the AND-immediate, which has no narrow form at all. */
  if (mask == 0xFFu)
    return AND_IMM_UXTB;
  if (mask == 0xFFFFu)
    return AND_IMM_UXTH;

  /* A mask the AND-immediate already encodes is one instruction as it stands;
   * these forms only pay off when they remove a constant materialization. */
  if (th_pack_const(mask) != 0)
    return AND_IMM_NONE;

  /* A low-contiguous run of ones is exactly UBFX Rd, Rn, #0, #W.  0 and
   * 0xFFFFFFFF are degenerate (the callers fold them to a load-zero and a
   * copy) and UBFX cannot encode width 32 anyway. */
  if (mask != 0 && mask != 0xFFFFFFFFu && (mask & (mask + 1)) == 0)
    return AND_IMM_UBFX;

  /* `x & ~m` is `BIC x, m`, and a clear-mask whose complement encodes is the
   * usual shape of a bitfield read-modify-write. */
  if (~mask != 0 && th_pack_const(~mask) != 0)
    return AND_IMM_BIC;

  return AND_IMM_NONE;
}

static void thumb_emit_and_imm_special(ThumbAndImmForm form, int rd, int rn, uint32_t mask,
                                       thumb_flags_behaviour flags)
{
  switch (form)
  {
  case AND_IMM_UXTB:
    ot_check(th_uxtb((uint32_t)rd, (uint32_t)rn, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
    return;
  case AND_IMM_UXTH:
    ot_check(th_uxth((uint32_t)rd, (uint32_t)rn, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
    return;
  case AND_IMM_UBFX:
  {
    int width = 0;
    while ((mask >> width) & 1u)
      width++;
    thumb_opcode ubfx_op;
    ubfx_op.size = 4;
    /* 11110 0 11 1100 Rn | 0 imm3 Rd imm2 0 widthm1, with lsb 0 (imm3=imm2=0). */
    ubfx_op.opcode = 0xF3C00000u | ((uint32_t)rn << 16) | ((uint32_t)rd << 8) | (uint32_t)(width - 1);
    ot(ubfx_op);
    return;
  }
  case AND_IMM_BIC:
    ot_check(th_bic_imm((uint32_t)rd, (uint32_t)rn, ~mask, flags, ENFORCE_ENCODING_NONE));
    return;
  case AND_IMM_NONE:
  default:
    tcc_error("thumb_emit_and_imm_special: no form for mask 0x%x", (unsigned)mask);
    return;
  }
}

/* ============================================================
 * thumb_try_orrs_zero64
 * ============================================================
 * A 64-bit value is zero exactly when the bitwise OR of its halves is, so
 * `ORRS Rt, Rlo, Rhi` sets Z for the whole comparison in one instruction where
 * the general form needs three (`CMP hi,#0` / `IT EQ` / `CMPEQ lo,#0`).  Only
 * Z is meaningful afterwards, which is all an equality comparison consumes --
 * the three-instruction form is no better in that respect, since which of its
 * two CMPs ran last decides N/C/V.
 *
 * ORRS needs a destination the CMP form does not, so this fires only when the
 * allocator has a register genuinely free at this point: buying one with a
 * push/pop would cost more than the two instructions it saves.  Rt may alias
 * either source -- ORRS reads both operands before writing.
 *
 * `soft_common.h`'s classifiers are built out of this test (`double_mant(bits)
 * != 0` and friends), 60 sites across the soft-float library.
 *
 * Returns 1 if it emitted the comparison, 0 if the caller must fall back.
 */
static int thumb_try_orrs_zero64(int rn_lo, int rn_hi)
{
  TCCIRState *ir = tcc_state->ir;
  if (!ir)
    return 0;
  if (!thumb_is_hw_reg(rn_lo) || !thumb_is_hw_reg(rn_hi))
    return 0;

  const uint32_t excl = scratch_global_exclude;
  int rt = tcc_ls_find_free_scratch_reg(&ir->ls, ir->codegen_instruction_idx, excl, ir->leaffunc);
  if (rt == (int)PREG_NONE || !thumb_is_hw_reg(rt))
    return 0;

  thumb_opcode orrs = th_orr_reg((uint32_t)rt, (uint32_t)rn_lo, (uint32_t)rn_hi, FLAGS_BEHAVIOUR_SET,
                                 THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE);
  if (orrs.size == 0)
    return 0;
  ot_check(orrs);
  return 1;
}

static void thumb_emit_op_imm_fallback(int rd, int rn, uint32_t imm, thumb_flags_behaviour flags,
                                       ThumbDataProcessingHandler handler)
{
  thumb_opcode sub_low = thumb_call_imm_handler(handler.imm_handler, rd, rn, imm, flags, ENFORCE_ENCODING_NONE);
  if (sub_low.size == 0)
  {
    uint32_t exclude = 0;
    if (rd >= 0 && rd <= 15)
      exclude |= (1u << rd);
    if (rn >= 0 && rn <= 15)
      exclude |= (1u << rn);
    ScratchRegAlloc scratch = get_scratch_reg_with_save(exclude);
    tcc_machine_load_constant(scratch.reg, PREG_NONE, (int32_t)imm, 0, NULL);
    ot_check(thumb_call_reg_handler(handler.reg_handler, rd, rn, scratch.reg, flags, THUMB_SHIFT_DEFAULT,
                                    ENFORCE_ENCODING_NONE));
    restore_scratch_reg(&scratch);
  }
  else
  {
    ot_check(sub_low);
  }
}

static thumb_opcode thumb_mul_regonly(uint32_t rd, uint32_t rn, uint32_t rm)
{
  return th_mul(rd, rn, rm, flags_safe(), ENFORCE_ENCODING_NONE);
}

static thumb_opcode thumb_sdiv_regonly(uint32_t rd, uint32_t rn, uint32_t rm)
{
  return th_sdiv((uint16_t)rd, (uint16_t)rn, (uint16_t)rm);
}

static thumb_opcode thumb_udiv_regonly(uint32_t rd, uint32_t rn, uint32_t rm)
{
  return th_udiv((uint16_t)rd, (uint16_t)rn, (uint16_t)rm);
}

/* A 64-bit operand whose needs_deref came from is_lval NAMES memory: the value
 * sits at that address, it is not a pointer to follow.  Two kinds are of that
 * sort — PARAM_STACK (the caller's argument area) and CHAIN_REL (a variable
 * captured from a parent frame, reached through the static chain).  Splitting
 * either into halves steps the OFFSET by 4, which is right; following it as a
 * pointer reads the low word and dereferences that.  MACH_OP_SPILL is not in
 * the set: its needs_deref comes from is_llocal, which really is a pointer. */
bool mach_op_64_names_memory(const MachineOperand *op)
{
  return op->kind == MACH_OP_PARAM_STACK || op->kind == MACH_OP_CHAIN_REL;
}

/* ============================================================
 * mach_resolve_deref_64
 * ============================================================
 * When a 64-bit source has needs_deref=true, the operand holds a POINTER
 * to a 64-bit value — not the value itself.  Splitting such an operand
 * via mach_make_lo_half / mach_make_hi_half is WRONG because
 * mach_make_hi_half would increment the register number (e.g. R0 → R1)
 * instead of the memory offset.
 *
 * This helper resolves the deref by loading both 32-bit halves from
 * [base+0] and [base+4] into scratch registers, returning a clean
 * MACH_OP_REG pair operand with needs_deref=false.  The caller can
 * then safely call mach_make_lo_half / mach_make_hi_half on the result.
 *
 * Returns *op unchanged if needs_deref is false.
 */
MachineOperand mach_resolve_deref_64(MachineCodegenContext *mctx, const MachineOperand *op, uint32_t *excl)
{
  if (!op->needs_deref)
    return *op;

  /* The value is directly at [fp+offset] / [chain+offset]: clear needs_deref
   * and let the normal mach_make_lo_half / mach_make_hi_half path handle it. */
  if (mach_op_64_names_memory(op))
  {
    MachineOperand result = *op;
    result.needs_deref = false;
    return result;
  }

  /* Strip deref to get the raw address into a register. */
  MachineOperand addr = *op;
  addr.needs_deref = false;
  addr.is_64bit = false;
  addr.btype = IROP_BTYPE_INT32;
  int base_reg = mach_ensure_in_reg(mctx, &addr, *excl);
  if (thumb_is_hw_reg(base_reg))
    *excl |= (1u << (uint32_t)base_reg);

  /* Allocate two scratch registers for the loaded halves. */
  int lo_reg = mach_alloc_scratch(mctx, *excl);
  *excl |= (1u << (uint32_t)lo_reg);
  int hi_reg = mach_alloc_scratch(mctx, *excl);
  *excl |= (1u << (uint32_t)hi_reg);

  /* Load [base+0] → lo, [base+4] → hi.  Proven-aligned access (op->align4,
   * from the frontend's packed-access tracking): one LDRD; otherwise the
   * unaligned-safe pair of 32-bit loads. */
  if (!(op->align4 && try_ldrd_pair(lo_reg, hi_reg, base_reg, 0, 0)))
  {
    load_from_base(lo_reg, PREG_REG_NONE, IROP_BTYPE_INT32, 0, 0, 0, (uint32_t)base_reg);
    load_from_base(hi_reg, PREG_REG_NONE, IROP_BTYPE_INT32, 0, 4, 0, (uint32_t)base_reg);
  }

  /* Build a clean register-pair operand. */
  MachineOperand result = {0};
  result.kind = MACH_OP_REG;
  result.is_64bit = true;
  result.needs_deref = false;
  result.btype = op->btype;
  result.u.reg.r0 = lo_reg;
  result.u.reg.r1 = hi_reg;
  return result;
}

/* ============================================================
 * mach_make_lo_half / mach_make_hi_half
 * ============================================================
 * Split a 64-bit MachineOperand into its 32-bit low and high halves.
 * The resulting operands have is_64bit=false and represent the individual
 * 32-bit words, suitable for mach_ensure_in_reg / mach_writeback_dest.
 *
 * Only call mach_make_hi_half on a 64-bit operand (is_64bit=true or
 * MACH_OP_SPILL); the result for 32-bit REG is the next register (r0+1).
 */
MachineOperand mach_make_lo_half(const MachineOperand *op)
{
  MachineOperand lo = *op;
  lo.is_64bit = false;
  if (lo.kind == MACH_OP_REG)
    lo.u.reg.r1 = -1;
  /* SPILL: keep the same offset — low word is at the base offset.   */
  /* IMM:   u.imm.val bits [31:0] are the low word (callers truncate). */
  /* CHAIN_REL: keep offset/chain_index — low word is at base offset. */
  return lo;
}
MachineOperand mach_make_hi_half(const MachineOperand *op)
{
  MachineOperand hi = *op;
  hi.is_64bit = false;
  switch (hi.kind)
  {
  case MACH_OP_REG:
    /* r1 holds the high register for 64-bit pairs.  If r1 is not a valid
     * hardware register the allocator failed to produce a proper pair —
     * error out instead of silently using r0+1 which can clobber reserved
     * registers (e.g. R9 = GOT base). */
    if (!thumb_is_hw_reg(op->u.reg.r1))
      tcc_error("mach_make_hi_half: 64-bit REG operand has invalid r1=%d (r0=%d) — "
                "register allocator must produce a valid pair",
                op->u.reg.r1, op->u.reg.r0);
    hi.u.reg.r0 = op->u.reg.r1;
    hi.u.reg.r1 = -1;
    break;
  case MACH_OP_SPILL:
    hi.u.spill.offset += 4;
    break;
  case MACH_OP_IMM:
    hi.u.imm.val = (int64_t)(int32_t)(uint32_t)((uint64_t)op->u.imm.val >> 32);
    break;
  case MACH_OP_PARAM_STACK:
    hi.u.param.offset += 4;
    break;
  case MACH_OP_CHAIN_REL:
    hi.u.chain.offset += 4; /* high word is 4 bytes above low word */
    break;
  case MACH_OP_SYMBOL:
    hi.u.sym.addend += 4; /* high word at symbol + addend + 4 */
    break;
  case MACH_OP_FRAME_ADDR:
    hi.u.frame.offset += 4; /* high word at FP + offset + 4 */
    break;
  default:
    break;
  }
  return hi;
}

/* Materialize both 32-bit halves of a 64-bit operand into registers.
 *
 * When the source is a plain, word-aligned stack slot the two halves sit at
 * adjacent addresses — exactly LDRD's shape, so one instruction replaces the
 * two LDRs the per-half `mach_ensure_in_reg` calls would emit.  A `long long`
 * comparison or arithmetic op reading a local is otherwise the one 64-bit
 * memory shape that never pairs (STOREs already fuse to STRD, and derefs pair
 * via mach_resolve_deref_64), which is why `if (x.a != y.a)` on 64-bit fields
 * costs four loads instead of two.
 *
 * Every other operand kind, and any slot LDRD cannot reach, falls back to the
 * original per-half path, so behaviour there is unchanged.  `*excl` picks up
 * both chosen registers, mirroring what the callers did by hand. */
void mach_ensure_pair_in_regs(MachineCodegenContext *ctx, const MachineOperand *op64,
                                     uint32_t *excl, int *out_lo, int *out_hi)
{
  MachineOperand lo = mach_make_lo_half(op64);
  MachineOperand hi = mach_make_hi_half(op64);
  lo.btype = IROP_BTYPE_INT32;
  hi.btype = IROP_BTYPE_INT32;

  if (op64->is_64bit && lo.kind == MACH_OP_SPILL && !lo.needs_deref && (lo.u.spill.offset & 3) == 0)
  {
    int rlo = mach_alloc_scratch(ctx, *excl);
    int rhi = mach_alloc_scratch(ctx, *excl | (1u << (uint32_t)rlo));
    if (rlo != rhi)
    {
      /* LDRD, or — when its offset/register constraints do not hold — the two
       * single-word loads into the scratches we already own. */
      if (!tcc_gen_machine_try_ldrd_spill(rlo, lo.u.spill.offset, rhi, hi.u.spill.offset))
      {
        tcc_machine_load_spill_slot(rlo, lo.u.spill.offset);
        tcc_machine_load_spill_slot(rhi, hi.u.spill.offset);
      }
      *excl |= (1u << (uint32_t)rlo) | (1u << (uint32_t)rhi);
      *out_lo = rlo;
      *out_hi = rhi;
      return;
    }
    tcc_machine_load_spill_slot(rlo, lo.u.spill.offset);
    *excl |= (1u << (uint32_t)rlo);
    *out_lo = rlo;
    *out_hi = mach_ensure_in_reg(ctx, &hi, *excl);
    if (thumb_is_hw_reg(*out_hi))
      *excl |= (1u << (uint32_t)*out_hi);
    return;
  }

  int rlo = mach_ensure_in_reg(ctx, &lo, *excl);
  if (thumb_is_hw_reg(rlo))
    *excl |= (1u << (uint32_t)rlo);
  int rhi = mach_ensure_in_reg(ctx, &hi, *excl);
  if (thumb_is_hw_reg(rhi))
    *excl |= (1u << (uint32_t)rhi);
  *out_lo = rlo;
  *out_hi = rhi;
}

/* ============================================================
 * thumb_emit_data_processing_mop64
 * ============================================================
 * 64-bit ADD / SUB / AND / OR / XOR via MachineOperand register pairs.
 * Handles REG (r0:r1), SPILL (offset, offset+4) and IMM operands.
 *
 * uses_carry=true  → low word uses FLAGS_BEHAVIOUR_SET, high word uses the
 *                    carry handler (ADDS + ADC for ADD, SUBS + SBC for SUB).
 * uses_carry=false → both halves use the same handler independently (AND/OR/XOR).
 *
 * If src1 is not 64-bit (e.g. int promoted to long long), its high half is
 * zero-extended.  Similarly for src2.
 *
 * `zh` carries source/opt/flat/fusion/zero_half64.c's verdict in its low six
 * bits: which operand halves are provably the constant zero, and which halves
 * of the result no consumer reads.
 */
static void thumb_emit_data_processing_mop64(const MachineOperand *src1, const MachineOperand *src2,
                                             const MachineOperand *dest, TccIrOp op, ThumbDataProcessingHandler regular,
                                             ThumbDataProcessingHandler carry_h, bool uses_carry, uint32_t zh)
{
  (void)op;
  MachineCodegenContext mctx = {0};
  uint32_t excl = 0;

  /* 0. Determine destination register pair FIRST so that deref resolution
   *    never allocates scratch registers that overlap with the dest pair.
   *    Without this, mach_release_all would restore saved scratch regs
   *    and clobber the result sitting in rd_lo / rd_hi. */
  int rd_lo, rd_hi;
  bool store_lo = false, store_hi = false;
  if (dest->kind == MACH_OP_REG && !dest->needs_deref && dest->u.reg.r0 != (int)PREG_REG_NONE && dest->u.reg.r1 >= 0)
  {
    rd_lo = dest->u.reg.r0;
    rd_hi = dest->u.reg.r1;
    excl |= (1u << (uint32_t)rd_lo) | (1u << (uint32_t)rd_hi);
  }
  else
  {
    rd_lo = mach_alloc_scratch(&mctx, excl);
    excl |= (1u << (uint32_t)rd_lo);
    rd_hi = mach_alloc_scratch(&mctx, excl);
    excl |= (1u << (uint32_t)rd_hi);
    store_lo = store_hi = (dest->kind != MACH_OP_NONE);
  }

  /* 0b. Pre-exclude register operands so that deref resolution of one
   *     source never steals the physical registers of another source.
   *     This must include needs_deref registers: they hold live pointers
   *     that will be consumed during their own deref resolution and must
   *     not be repurposed as scratch during the other source's deref. */
  if (src1->kind == MACH_OP_REG)
  {
    if (src1->u.reg.r0 != (int)PREG_REG_NONE)
      excl |= (1u << (uint32_t)src1->u.reg.r0);
    if (!src1->needs_deref && src1->is_64bit && src1->u.reg.r1 >= 0)
      excl |= (1u << (uint32_t)src1->u.reg.r1);
  }
  if (src2->kind == MACH_OP_REG)
  {
    if (src2->u.reg.r0 != (int)PREG_REG_NONE)
      excl |= (1u << (uint32_t)src2->u.reg.r0);
    if (!src2->needs_deref && src2->is_64bit && src2->u.reg.r1 >= 0)
      excl |= (1u << (uint32_t)src2->u.reg.r1);
  }

  /* 1. Resolve deref'd source pointers before splitting into halves. */
  MachineOperand r_src1 = mach_resolve_deref_64(&mctx, src1, &excl);
  src1 = &r_src1;
  MachineOperand r_src2 = mach_resolve_deref_64(&mctx, src2, &excl);
  src2 = &r_src2;

  /* 1b. Plan the HIGH half before loading anything.
   *
   * A source that is not 64-bit is zero-extended, and against a zero half a
   * bitwise op has a constant answer: OR/XOR copy the other operand, AND give
   * zero.  Deciding that here rather than after the loads means the zero is
   * never materialised AND the scratch register that would have held it is
   * never allocated -- on a saturated function that scratch costs a push/pop
   * of a callee-saved register too.
   *
   * `(uint64_t)x | ((uint64_t)y << 32)` and the make_double/make_float packing
   * idiom soft float is built from are exactly this shape: before this, each
   * such OR paid `movs rt,#0` plus an `orr rd,rt,rm` that could only ever
   * return rm. */
  enum
  {
    HALF_NORMAL = 0, /* emit the handler over both registers of this half */
    HALF_ZERO,       /* result half is the constant 0 */
    HALF_COPY_S1,    /* result half is src1's */
    HALF_COPY_S2,    /* result half is src2's */
    HALF_CARRY_IMM0, /* carry op, src2 half is zero: `adc/sbc rd, rn, #0` */
    HALF_DEAD        /* nothing reads the result half; emit nothing at all */
  };
  /* A non-64-bit source is zero-extended by construction; the annotation adds
   * the halves a 64-bit value is only *known* to hold zero in. */
  const bool s1_lo_zero = (zh & ZH64_S1_LO) != 0;
  const bool s1_hi_zero = !src1->is_64bit || (zh & ZH64_S1_HI) != 0;
  const bool s2_lo_zero = (zh & ZH64_S2_LO) != 0;
  const bool s2_hi_zero = (src2->kind != MACH_OP_IMM && !src2->is_64bit) || (zh & ZH64_S2_HI) != 0;
  int lo_plan = HALF_NORMAL, hi_plan = HALF_NORMAL;
  /* Carry ops need the real words (the carry still has to propagate), and
   * CMP's halves must set flags, so neither can take a shortcut. */
  if (!uses_carry && op != TCCIR_OP_CMP && src2->kind != MACH_OP_IMM)
  {
    for (int half = 0; half < 2; half++)
    {
      const bool z1 = half ? s1_hi_zero : s1_lo_zero;
      const bool z2 = half ? s2_hi_zero : s2_lo_zero;
      int plan = HALF_NORMAL;
      if (!z1 && !z2)
        plan = HALF_NORMAL;
      else if (op == TCCIR_OP_AND)
        plan = HALF_ZERO;
      else if (op == TCCIR_OP_OR || op == TCCIR_OP_XOR)
        plan = (z1 && z2) ? HALF_ZERO : (z1 ? HALF_COPY_S2 : HALF_COPY_S1);
      if (half)
        hi_plan = plan;
      else
        lo_plan = plan;
    }
  }
  /* A carry op cannot skip a half -- the carry still has to propagate -- but a
   * zero high word on src2 turns the high half into `adc rd, rn, #0` /
   * `sbc rd, rn, #0`, which reads no second register and needs no `mov rt, #0`
   * to feed it.  This is most of what sfp_div_sig64 spends its zeros on:
   * Knuth-D adds and subtracts 32-bit quantities from 64-bit ones throughout,
   * and each one was materialising a zero purely to be added to a high word.
   * src1 is deliberately not covered: SUB is not commutative, and mirroring
   * this rule in zero_half64's producer analysis is only sound while the
   * emitter's choice is unambiguous. */
  if (uses_carry && op != TCCIR_OP_CMP && src2->kind != MACH_OP_IMM && s2_hi_zero)
    hi_plan = HALF_CARRY_IMM0;

  /* A 64-bit compare against a constant with a zero low half, read only by
   * strict order tests: the low words cannot decide it, so drop that half and
   * compare the high words with a real flag-setting CMP instead of the SBCS
   * that would have folded in a borrow nothing can generate.  Two instructions
   * and a scratch register become one instruction and none.  Proof and the
   * conditions that make it exact: source/opt/flat/fusion/cmp_hi_only.c. */
  const bool cmp_hi_only = (op == TCCIR_OP_CMP) && (zh & ZH64_CMP_HI) != 0 &&
                           src2->kind == MACH_OP_IMM && src1->is_64bit;
  if (cmp_hi_only)
    lo_plan = HALF_DEAD;

  /* A half no consumer reads outranks every plan above: skip it entirely.
   * Only ever set for a register destination -- a memory one still owes its
   * full eight bytes whatever the vreg uses do. */
  if ((zh & ZH64_D_LO) && !uses_carry && op != TCCIR_OP_CMP)
    lo_plan = HALF_DEAD;
  if ((zh & ZH64_D_HI) && !uses_carry && op != TCCIR_OP_CMP)
    hi_plan = HALF_DEAD;
  const bool need_rn_lo = (lo_plan == HALF_NORMAL || lo_plan == HALF_COPY_S1 || lo_plan == HALF_CARRY_IMM0);
  const bool need_rn_hi = (hi_plan == HALF_NORMAL || hi_plan == HALF_COPY_S1 || hi_plan == HALF_CARRY_IMM0);
  const bool need_rm_lo = (lo_plan == HALF_NORMAL || lo_plan == HALF_COPY_S1 || lo_plan == HALF_COPY_S2 ||
                           lo_plan == HALF_CARRY_IMM0);
  const bool need_rm_hi = (hi_plan == HALF_NORMAL || hi_plan == HALF_COPY_S2);

  /* 2. Load src1 low and high halves into registers. */
  int rn_lo = (int)PREG_REG_NONE;
  if (need_rn_lo)
  {
    MachineOperand s1_lo = mach_make_lo_half(src1);
    rn_lo = mach_ensure_in_reg(&mctx, &s1_lo, excl);
    if (thumb_is_hw_reg(rn_lo))
      excl |= (1u << (uint32_t)rn_lo);
  }
  int rn_hi = (int)PREG_REG_NONE;
  if (!need_rn_hi)
  {
    /* left unloaded on purpose -- nothing below reads it */
  }
  else if (src1->is_64bit)
  {
    MachineOperand s1_hi = mach_make_hi_half(src1);
    rn_hi = mach_ensure_in_reg(&mctx, &s1_hi, excl);
  }
  else
  {
    rn_hi = mach_alloc_scratch(&mctx, excl);
    ot_check(th_mov_imm((uint32_t)rn_hi, 0, flags_safe(), ENFORCE_ENCODING_NONE));
  }
  if (need_rn_hi && thumb_is_hw_reg(rn_hi))
    excl |= (1u << (uint32_t)rn_hi);

  /* 3. Load src2 and emit the 64-bit operation. */
  const thumb_flags_behaviour lo_flags = uses_carry ? FLAGS_BEHAVIOUR_SET : flags_safe();
  /* For CMP, the high-word SBCS must set flags (the following SETIF reads them). */
  const thumb_flags_behaviour hi_flags = (op == TCCIR_OP_CMP) ? FLAGS_BEHAVIOUR_SET : flags_safe();
  if (src2->kind == MACH_OP_IMM)
  {
    const uint32_t imm_lo = (uint32_t)((uint64_t)src2->u.imm.val & 0xffffffffu);
    const uint32_t imm_hi = (uint32_t)((uint64_t)src2->u.imm.val >> 32);
    /* Per-half peephole: when the immediate half makes the op a constant
     * answer (OR/XOR with 0 → copy src; AND with 0 → load 0; AND with -1 →
     * copy src), skip the data-processing op.  Cuts dead `orr r, r, #0` and
     * `and r, r, #0` halves left behind by 64-bit ops on 32-bit values. */
    const bool is_or = (op == TCCIR_OP_OR);
    const bool is_xor = (op == TCCIR_OP_XOR);
    const bool is_and = (op == TCCIR_OP_AND);
    const bool can_simplify_lo = lo_flags == flags_safe();
    const bool can_simplify_hi = hi_flags == flags_safe();
    for (int half = 0; half < 2; half++)
    {
      const uint32_t imm = (half == 0) ? imm_lo : imm_hi;
      const int rd = (half == 0) ? rd_lo : rd_hi;
      const int rn = (half == 0) ? rn_lo : rn_hi;
      const thumb_flags_behaviour fb = (half == 0) ? lo_flags : hi_flags;
      const bool can_simplify = (half == 0) ? can_simplify_lo : can_simplify_hi;
      const ThumbDataProcessingHandler *h = (half == 0 || cmp_hi_only) ? &regular : &carry_h;

      if ((half == 0 ? lo_plan : hi_plan) == HALF_DEAD)
        continue;
      if (can_simplify && (is_or || is_xor) && imm == 0)
      {
        if (rd != rn)
          ot_check_mov_reg((uint32_t)rd, (uint32_t)rn, flags_safe(),
                           THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE, false);
      }
      else if (can_simplify && is_and && imm == 0)
      {
        ot_check(th_mov_imm((uint32_t)rd, 0, flags_safe(), ENFORCE_ENCODING_NONE));
      }
      else if (can_simplify && is_and && imm == 0xFFFFFFFFu)
      {
        if (rd != rn)
          ot_check_mov_reg((uint32_t)rd, (uint32_t)rn, flags_safe(),
                           THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE, false);
      }
      /* A 64-bit AND against a bitfield mask splits into two half-masks, and
       * the interesting half is never a modified immediate: DOUBLE_MANT_MASK's
       * high half is 0x000FFFFF, which the fallback below materializes from the
       * literal pool at every one of the dozen sites a soft-float classifier
       * has.  UBFX does the same job in one instruction with no constant. */
      else if (can_simplify && is_and && thumb_and_imm_form(imm) != AND_IMM_NONE)
      {
        thumb_emit_and_imm_special(thumb_and_imm_form(imm), rd, rn, imm, fb);
      }
      else
      {
        thumb_emit_op_imm_fallback(rd, rn, imm, fb, *h);
      }
    }
  }
  else
  {
    int rm_lo = (int)PREG_REG_NONE;
    if (need_rm_lo)
    {
      MachineOperand s2_lo = mach_make_lo_half(src2);
      rm_lo = mach_ensure_in_reg(&mctx, &s2_lo, excl);
      if (thumb_is_hw_reg(rm_lo))
        excl |= (1u << (uint32_t)rm_lo);
    }
    int rm_hi = (int)PREG_REG_NONE;
    if (!need_rm_hi)
    {
      /* left unloaded on purpose -- the high-half plan does not read it */
    }
    else if (src2->is_64bit)
    {
      MachineOperand s2_hi = mach_make_hi_half(src2);
      rm_hi = mach_ensure_in_reg(&mctx, &s2_hi, excl);
    }
    else
    {
      rm_hi = mach_alloc_scratch(&mctx, excl);
      ot_check(th_mov_imm((uint32_t)rm_hi, 0, flags_safe(), ENFORCE_ENCODING_NONE));
    }
    for (int half = 0; half < 2; half++)
    {
      const int plan = half ? hi_plan : lo_plan;
      const int rd = half ? rd_hi : rd_lo;
      const int rn = half ? rn_hi : rn_lo;
      const int rm = half ? rm_hi : rm_lo;
      const thumb_flags_behaviour fb = half ? hi_flags : lo_flags;
      const ThumbDataProcessingHandler *h = half ? &carry_h : &regular;
      switch (plan)
      {
      case HALF_DEAD:
        break;
      case HALF_ZERO:
        ot_check(th_mov_imm((uint32_t)rd, 0, flags_safe(), ENFORCE_ENCODING_NONE));
        break;
      case HALF_COPY_S1:
        if (rd != rn)
          ot_check_mov_reg((uint32_t)rd, (uint32_t)rn, flags_safe(), THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE,
                           false);
        break;
      case HALF_COPY_S2:
        if (rd != rm)
          ot_check_mov_reg((uint32_t)rd, (uint32_t)rm, flags_safe(), THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE,
                           false);
        break;
      case HALF_CARRY_IMM0:
        thumb_emit_op_imm_fallback(rd, rn, 0, fb, *h);
        break;
      default:
        ot_check(thumb_call_reg_handler(h->reg_handler, (uint32_t)rd, (uint32_t)rn, (uint32_t)rm, fb,
                                        THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
        break;
      }
    }
  }

  /* 4. Write results back to spill/param slots if dest was not pre-allocated.
   *    A half nothing reads is not written back either -- the slot keeps
   *    whatever it held, which is exactly what "dead" asserts. */
  if (store_lo && lo_plan != HALF_DEAD)
  {
    MachineOperand dst_lo = mach_make_lo_half(dest);
    dst_lo.btype = IROP_BTYPE_INT32;
    mach_writeback_dest(&dst_lo, rd_lo);
  }
  if (store_hi && hi_plan != HALF_DEAD)
  {
    MachineOperand dst_hi = mach_make_hi_half(dest);
    dst_hi.btype = IROP_BTYPE_INT32;
    mach_writeback_dest(&dst_hi, rd_hi);
  }
  mach_release_all(&mctx);
}

/* ============================================================
 * thumb_emit_shift64_reg_mop
 * ============================================================
 * 64-bit SHL / SHR / SAR whose count is a value rather than a literal.
 *
 * Before this every such shift went to __aeabi_llsl / __aeabi_llsr /
 * __aeabi_lasr.  Those helpers are compiled from C like any other library
 * routine, so each one costs a call, a frame and ~30 executed instructions; a
 * dynamic profile of __aeabi_dadd put two of them -- the alignment shift and
 * the `(1ULL << n) - 1` mask -- at 62 of its 267 instructions per call.
 * Inline the same work is 8 instructions and touches no memory.
 *
 * The sequences lean on ARM's register-shift rule: LSL/LSR/ASR by a register
 * read only the bottom BYTE of the count, and a count of 32 or more yields 0
 * (LSL/LSR) or a full sign fill (ASR).  That makes the `32 - n` and `n - 32`
 * cross terms self-disabling over the half of the range where they must not
 * contribute, so the logical shifts need neither a branch nor a conditional:
 *
 *   x << n :  hi = (hi << n) | (lo >> (32-n)) | (lo << (n-32));  lo = lo << n
 *   x >> n :  lo = (lo >> n) | (hi << (32-n)) | (hi >> (n-32));  hi = hi >> n
 *
 * SAR cannot borrow that trick for its cross term -- `hi ASR (n-32)` is a sign
 * fill when n < 32, which would flood the low half with ones where it must
 * contribute nothing -- so it selects between the two halves of the range with
 * a mask taken from the sign of `n - 32`.  A mask and not an IT/MOVGE pair
 * because this runs wherever the allocator placed the shift, including with a
 * pending CMP's flags still live.
 *
 * Counts of 64 and above are undefined in C; the sequences give 0 (or a sign
 * fill for SAR), which is what GCC's inline expansion produces as well.
 */
static void thumb_emit_shift64_reg_mop(const MachineOperand *src1, const MachineOperand *src2,
                                       const MachineOperand *dest, TccIrOp op, bool skip_lo, bool skip_hi)
{
  const bool is_left = (op == TCCIR_OP_SHL);
  const bool arith_right = (op == TCCIR_OP_SAR);

  MachineCodegenContext mctx = {0};
  uint32_t excl = 0;

  /* Destination pair first, exactly as the immediate path does, so that deref
   * resolution never hands out a scratch that overlaps it. */
  int dst_lo, dst_hi;
  bool store_lo = false, store_hi = false;
  if (dest->kind == MACH_OP_REG && !dest->needs_deref && dest->u.reg.r0 != (int)PREG_REG_NONE && dest->u.reg.r1 >= 0)
  {
    dst_lo = dest->u.reg.r0;
    dst_hi = dest->u.reg.r1;
    excl |= (1u << (uint32_t)dst_lo) | (1u << (uint32_t)dst_hi);
  }
  else
  {
    dst_lo = mach_alloc_scratch(&mctx, excl);
    excl |= (1u << (uint32_t)dst_lo);
    dst_hi = mach_alloc_scratch(&mctx, excl);
    excl |= (1u << (uint32_t)dst_hi);
    store_lo = store_hi = (dest->kind != MACH_OP_NONE);
  }

  if (src1->kind == MACH_OP_REG)
  {
    if (src1->u.reg.r0 != (int)PREG_REG_NONE)
      excl |= (1u << (uint32_t)src1->u.reg.r0);
    if (!src1->needs_deref && src1->is_64bit && src1->u.reg.r1 >= 0)
      excl |= (1u << (uint32_t)src1->u.reg.r1);
  }
  if (src2->kind == MACH_OP_REG)
  {
    if (src2->u.reg.r0 != (int)PREG_REG_NONE)
      excl |= (1u << (uint32_t)src2->u.reg.r0);
    if (!src2->needs_deref && src2->is_64bit && src2->u.reg.r1 >= 0)
      excl |= (1u << (uint32_t)src2->u.reg.r1);
  }

  MachineOperand r_src1 = mach_resolve_deref_64(&mctx, src1, &excl);
  src1 = &r_src1;

  MachineOperand s1_lo = mach_make_lo_half(src1);
  int src_lo = mach_ensure_in_reg(&mctx, &s1_lo, excl);
  if (thumb_is_hw_reg(src_lo))
    excl |= (1u << (uint32_t)src_lo);

  /* A high word that is a known zero deletes every term that reads it -- and
   * the register that would have held it.  `(1ULL << n) - 1`, the mask soft
   * float builds every alignment sticky-bit from, is exactly this shape: the
   * generic sequence would materialize a zero just to shift and OR it in. */
  const int hi_is_zero =
      !arith_right && (!src1->is_64bit ||
                       (src1->kind == MACH_OP_IMM && ((uint64_t)src1->u.imm.val >> 32) == 0));

  int src_hi = (int)PREG_REG_NONE;
  if (!hi_is_zero)
  {
    if (src1->is_64bit)
    {
      MachineOperand s1_hi = mach_make_hi_half(src1);
      src_hi = mach_ensure_in_reg(&mctx, &s1_hi, excl);
      if (thumb_is_hw_reg(src_hi))
        excl |= (1u << (uint32_t)src_hi);
    }
    else
    {
      src_hi = mach_alloc_scratch(&mctx, excl);
      excl |= (1u << (uint32_t)src_hi);
      ot_check(th_asr_imm((uint32_t)src_hi, (uint32_t)src_lo, 31, flags_safe(), ENFORCE_ENCODING_NONE));
    }
  }

  /* Only the low word of a 64-bit count can matter — the hardware reads one
   * byte of it. */
  MachineOperand cnt_op = src2->is_64bit ? mach_make_lo_half(src2) : *src2;
  int cnt = mach_ensure_in_reg(&mctx, &cnt_op, excl);
  if (thumb_is_hw_reg(cnt))
    excl |= (1u << (uint32_t)cnt);

  /* The half that collects the cross terms.  Writing it straight into the
   * destination saves a MOV, but only where that register is neither a source
   * half still to be read nor the count. */
  /* A right shift of a zero-high-word value needs no accumulator and no
   * temporaries; allocating them anyway can cost a scratch push/pop. */
  const bool need_acc = (!is_left && hi_is_zero) ? false : (is_left ? !skip_hi : !skip_lo);
  const int acc_pref = is_left ? dst_hi : dst_lo;
  const int acc_conflict = is_left ? src_lo : src_hi;
  int acc = -1;
  if (need_acc)
  {
    acc = (acc_pref != acc_conflict && acc_pref != cnt) ? acc_pref : -1;
    if (acc < 0)
    {
      acc = mach_alloc_scratch(&mctx, excl);
      excl |= (1u << (uint32_t)acc);
    }
  }

  int t0 = -1, t1 = -1;
  if (need_acc)
  {
    t0 = mach_alloc_scratch(&mctx, excl);
    excl |= (1u << (uint32_t)t0);
    if (arith_right)
    {
      t1 = mach_alloc_scratch(&mctx, excl);
      excl |= (1u << (uint32_t)t1);
    }
  }

  if (is_left)
  {
    if (need_acc)
    {
      ot_check(th_rsb_imm((uint32_t)t0, (uint32_t)cnt, 32, flags_safe(), ENFORCE_ENCODING_NONE));
      if (hi_is_zero)
      {
        /* (0 << n) | (lo >> (32-n)) is just the cross term. */
        ot_check(th_lsr_reg((uint32_t)acc, (uint32_t)src_lo, (uint32_t)t0, flags_safe(), THUMB_SHIFT_DEFAULT,
                            ENFORCE_ENCODING_NONE));
      }
      else
      {
        ot_check(th_lsl_reg((uint32_t)acc, (uint32_t)src_hi, (uint32_t)cnt, flags_safe(), THUMB_SHIFT_DEFAULT,
                            ENFORCE_ENCODING_NONE));
        ot_check(th_lsr_reg((uint32_t)t0, (uint32_t)src_lo, (uint32_t)t0, flags_safe(), THUMB_SHIFT_DEFAULT,
                            ENFORCE_ENCODING_NONE));
        ot_check(th_orr_reg((uint32_t)acc, (uint32_t)acc, (uint32_t)t0, flags_safe(), THUMB_SHIFT_DEFAULT,
                            ENFORCE_ENCODING_NONE));
      }
      ot_check(th_sub_imm((uint32_t)t0, (uint32_t)cnt, 32, flags_safe(), ENFORCE_ENCODING_NONE));
      ot_check(th_lsl_reg((uint32_t)t0, (uint32_t)src_lo, (uint32_t)t0, flags_safe(), THUMB_SHIFT_DEFAULT,
                          ENFORCE_ENCODING_NONE));
      ot_check(th_orr_reg((uint32_t)acc, (uint32_t)acc, (uint32_t)t0, flags_safe(), THUMB_SHIFT_DEFAULT,
                          ENFORCE_ENCODING_NONE));
    }
    /* dst_lo last: it is the final read of src_lo and of the count. */
    if (!skip_lo)
      ot_check(th_lsl_reg((uint32_t)dst_lo, (uint32_t)src_lo, (uint32_t)cnt, flags_safe(), THUMB_SHIFT_DEFAULT,
                          ENFORCE_ENCODING_NONE));
    if (need_acc && acc != dst_hi)
      ot_check_mov_reg((uint32_t)dst_hi, (uint32_t)acc, flags_safe(), THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE,
                       false);
  }
  else if (hi_is_zero)
  {
    /* Every cross term reads the zero high word, so the whole shift is one
     * instruction and a zero fill. */
    if (!skip_lo)
      ot_check(th_lsr_reg((uint32_t)dst_lo, (uint32_t)src_lo, (uint32_t)cnt, flags_safe(), THUMB_SHIFT_DEFAULT,
                          ENFORCE_ENCODING_NONE));
    if (!skip_hi)
      ot_check(th_mov_imm((uint32_t)dst_hi, 0, flags_safe(), ENFORCE_ENCODING_NONE));
  }
  else
  {
    if (need_acc)
    {
      ot_check(th_rsb_imm((uint32_t)t0, (uint32_t)cnt, 32, flags_safe(), ENFORCE_ENCODING_NONE));
      ot_check(th_lsr_reg((uint32_t)acc, (uint32_t)src_lo, (uint32_t)cnt, flags_safe(), THUMB_SHIFT_DEFAULT,
                          ENFORCE_ENCODING_NONE));
      ot_check(th_lsl_reg((uint32_t)t0, (uint32_t)src_hi, (uint32_t)t0, flags_safe(), THUMB_SHIFT_DEFAULT,
                          ENFORCE_ENCODING_NONE));
      ot_check(th_orr_reg((uint32_t)acc, (uint32_t)acc, (uint32_t)t0, flags_safe(), THUMB_SHIFT_DEFAULT,
                          ENFORCE_ENCODING_NONE));
      ot_check(th_sub_imm((uint32_t)t0, (uint32_t)cnt, 32, flags_safe(), ENFORCE_ENCODING_NONE));
      if (arith_right)
      {
        /* `hi ASR (n-32)` is only the answer for n >= 32; below that it is a
         * sign fill that must contribute nothing.  `(n-32) ASR 31` is all-ones
         * exactly on that range, so it selects between the two candidates
         * without touching the flags. */
        ot_check(th_asr_reg((uint32_t)t1, (uint32_t)src_hi, (uint32_t)t0, flags_safe(), THUMB_SHIFT_DEFAULT,
                            ENFORCE_ENCODING_NONE));
        ot_check(th_asr_imm((uint32_t)t0, (uint32_t)t0, 31, flags_safe(), ENFORCE_ENCODING_NONE));
        ot_check(th_and_reg((uint32_t)acc, (uint32_t)acc, (uint32_t)t0, flags_safe(), THUMB_SHIFT_DEFAULT,
                            ENFORCE_ENCODING_NONE));
        ot_check(th_bic_reg((uint32_t)t1, (uint32_t)t1, (uint32_t)t0, flags_safe(), THUMB_SHIFT_DEFAULT,
                            ENFORCE_ENCODING_NONE));
        ot_check(th_orr_reg((uint32_t)acc, (uint32_t)acc, (uint32_t)t1, flags_safe(), THUMB_SHIFT_DEFAULT,
                            ENFORCE_ENCODING_NONE));
      }
      else
      {
        ot_check(th_lsr_reg((uint32_t)t0, (uint32_t)src_hi, (uint32_t)t0, flags_safe(), THUMB_SHIFT_DEFAULT,
                            ENFORCE_ENCODING_NONE));
        ot_check(th_orr_reg((uint32_t)acc, (uint32_t)acc, (uint32_t)t0, flags_safe(), THUMB_SHIFT_DEFAULT,
                            ENFORCE_ENCODING_NONE));
      }
    }
    /* dst_hi last: it is the final read of src_hi and of the count. */
    if (!skip_hi)
    {
      if (arith_right)
        ot_check(th_asr_reg((uint32_t)dst_hi, (uint32_t)src_hi, (uint32_t)cnt, flags_safe(), THUMB_SHIFT_DEFAULT,
                            ENFORCE_ENCODING_NONE));
      else
        ot_check(th_lsr_reg((uint32_t)dst_hi, (uint32_t)src_hi, (uint32_t)cnt, flags_safe(), THUMB_SHIFT_DEFAULT,
                            ENFORCE_ENCODING_NONE));
    }
    if (need_acc && acc != dst_lo)
      ot_check_mov_reg((uint32_t)dst_lo, (uint32_t)acc, flags_safe(), THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE,
                       false);
  }

  if (store_lo && !skip_lo)
  {
    MachineOperand dst_lo_op = mach_make_lo_half(dest);
    dst_lo_op.btype = IROP_BTYPE_INT32;
    mach_writeback_dest(&dst_lo_op, dst_lo);
  }
  if (store_hi && !skip_hi)
  {
    MachineOperand dst_hi_op = mach_make_hi_half(dest);
    dst_hi_op.btype = IROP_BTYPE_INT32;
    mach_writeback_dest(&dst_hi_op, dst_hi);
  }
  mach_release_all(&mctx);
}

/* ============================================================
 * thumb_emit_shift64_mop
 * ============================================================
 * 64-bit SHL / SHR / SAR via MachineOperand register pairs.
 * Shift amount (src2) must be a 32-bit immediate (MACH_OP_IMM).
 * Logic mirrors thumb_emit_shift64_imm but operates on register numbers
 * extracted from MachineOperand rather than IROperand fields.
 */
static void thumb_emit_shift64_mop(const MachineOperand *src1, const MachineOperand *src2, const MachineOperand *dest,
                                   TccIrOp op, bool skip_lo, bool skip_hi)
{
  if (src2->kind != MACH_OP_IMM)
  {
    thumb_emit_shift64_reg_mop(src1, src2, dest, op, skip_lo, skip_hi);
    return;
  }
  const uint32_t sh = (uint32_t)(uint64_t)src2->u.imm.val;
  const bool is_left = (op == TCCIR_OP_SHL);
  const bool arith_right = (op == TCCIR_OP_SAR);

  thumb_imm_handler_t dst_lo_shift, dst_hi_shift, cross_shift;
  if (is_left)
  {
    dst_lo_shift = th_lsl_imm;
    dst_hi_shift = th_lsl_imm;
    cross_shift = th_lsr_imm;
  }
  else if (arith_right)
  {
    dst_lo_shift = th_lsr_imm;
    dst_hi_shift = th_asr_imm;
    cross_shift = th_lsl_imm;
  }
  else
  {
    dst_lo_shift = th_lsr_imm;
    dst_hi_shift = th_lsr_imm;
    cross_shift = th_lsl_imm;
  }

  MachineCodegenContext mctx = {0};
  uint32_t excl = 0;

  /* Determine destination register pair FIRST so that deref resolution
   * never allocates scratch registers that overlap with the dest pair. */
  int dst_lo, dst_hi;
  bool store_lo = false, store_hi = false;
  if (dest->kind == MACH_OP_REG && !dest->needs_deref && dest->u.reg.r0 != (int)PREG_REG_NONE && dest->u.reg.r1 >= 0)
  {
    dst_lo = dest->u.reg.r0;
    dst_hi = dest->u.reg.r1;
    excl |= (1u << (uint32_t)dst_lo) | (1u << (uint32_t)dst_hi);
  }
  else
  {
    dst_lo = mach_alloc_scratch(&mctx, excl);
    excl |= (1u << (uint32_t)dst_lo);
    dst_hi = mach_alloc_scratch(&mctx, excl);
    excl |= (1u << (uint32_t)dst_hi);
    store_lo = store_hi = (dest->kind != MACH_OP_NONE);
  }

  /* Pre-exclude register operands so that deref resolution does not
   * steal the physical registers already holding src1 values.
   * Include needs_deref registers: they hold live pointers needed
   * during their own deref resolution. */
  if (src1->kind == MACH_OP_REG)
  {
    if (src1->u.reg.r0 != (int)PREG_REG_NONE)
      excl |= (1u << (uint32_t)src1->u.reg.r0);
    if (!src1->needs_deref && src1->is_64bit && src1->u.reg.r1 >= 0)
      excl |= (1u << (uint32_t)src1->u.reg.r1);
  }

  /* Resolve deref'd source pointer before splitting into halves. */
  MachineOperand r_src1 = mach_resolve_deref_64(&mctx, src1, &excl);
  src1 = &r_src1;

  /* Load src1 low half. */
  MachineOperand s1_lo = mach_make_lo_half(src1);
  int src_lo = mach_ensure_in_reg(&mctx, &s1_lo, excl);
  if (thumb_is_hw_reg(src_lo))
    excl |= (1u << (uint32_t)src_lo);

  /* Skip src1 high-half materialization when the shift will not read it.
   * SHL with sh >= 32 only uses src_lo (everything shifts up out of view).
   * SHR/SAR with sh >= 64 produces a 0/sign-fill that the emit tail
   * generates directly without referencing src_hi. */
  int hi_needed = 1;
  if (is_left && sh >= 32)
    hi_needed = 0;
  else if (!is_left && sh >= 64 && !arith_right)
    hi_needed = 0;
  /* SAR by >= 64 is the exception: its tail fills BOTH halves from the sign of
   * src_hi, so it reads the very half the count would suggest is irrelevant.
   * Declaring it unneeded left src_hi as PREG_REG_NONE and the tail encoded a
   * shift off a register that was never loaded. */

  /* Load src1 high half or compute by extension.  `hi_needed` gates the
   * ALREADY-64-bit case too: materializing a half nothing below reads is a
   * whole instruction, and `(uint64_t)sign << 63` in make_double is exactly
   * that -- the widening puts a zero in a register purely to shift it out of
   * existence. */
  int src_hi = (int)PREG_REG_NONE;
  if (src1->is_64bit)
  {
    if (hi_needed)
    {
      MachineOperand s1_hi = mach_make_hi_half(src1);
      src_hi = mach_ensure_in_reg(&mctx, &s1_hi, excl);
      if (thumb_is_hw_reg(src_hi))
        excl |= (1u << (uint32_t)src_hi);
    }
  }
  else if (hi_needed)
  {
    src_hi = mach_alloc_scratch(&mctx, excl);
    excl |= (1u << (uint32_t)src_hi);
    if (arith_right)
      ot_check(
          th_asr_imm((uint32_t)src_hi, (uint32_t)src_lo, 31, flags_safe(), ENFORCE_ENCODING_NONE));
    else
      ot_check(th_mov_imm((uint32_t)src_hi, 0, flags_safe(), ENFORCE_ENCODING_NONE));
  }

  /* `x <<= 1` is ADDS/ADC: two instructions where the generic sh<32 path below
   * needs four (cross-shift into a scratch, a shift per half, then an OR).
   * This is the step every restoring-division and normalization loop is built
   * from -- a dynamic profile of ddiv puts 280,000 iterations through it for
   * the benchmark's double kernels, against 5,000 through everything else.
   *
   * Two guards.  ADDS/ADC write the flags, so it must not run while a pending
   * CMP's flags are still live.  And ADC has to read src_hi AFTER ADDS has
   * written dst_lo, so dst_lo aliasing src_hi rules the pair out; every other
   * aliasing case is safe, since each instruction reads its sources before
   * writing its destination. */
  if (is_left && sh == 1 && !skip_lo && !skip_hi &&
      thumb_is_hw_reg(src_lo) && thumb_is_hw_reg(src_hi) && dst_lo != src_hi &&
      !(tcc_state->ir && tcc_state->ir->codegen_flags_live))
  {
    ot_check(th_add_reg((uint32_t)dst_lo, (uint32_t)src_lo, (uint32_t)src_lo, FLAGS_BEHAVIOUR_SET,
                        THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
    ot_check(th_adc_reg((uint32_t)dst_hi, (uint32_t)src_hi, (uint32_t)src_hi, flags_safe(),
                        THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
  }
  /* Emit the shift — logic identical to thumb_emit_shift64_imm core. */
  else if (sh == 0)
  {
    ot_check_mov_reg((uint32_t)dst_lo, (uint32_t)src_lo, flags_safe(), THUMB_SHIFT_DEFAULT,
                     ENFORCE_ENCODING_NONE, false);
    ot_check_mov_reg((uint32_t)dst_hi, (uint32_t)src_hi, flags_safe(), THUMB_SHIFT_DEFAULT,
                     ENFORCE_ENCODING_NONE, false);
  }
  else if (sh < 32)
  {
    /* The cross term is an ORR away from being free: T2 ORR takes a shifted
     * REGISTER operand, so `orr rd, rd, src, LSR #(32-sh)` does in one
     * instruction what a separate shift into a scratch plus a plain ORR did in
     * two -- and it needs no scratch at all, which on a saturated function is
     * a push/pop as well.  Every 64-bit shift by a literal in soft float is
     * this shape (`mant <<= 3`, `mant >> 3`), and __aeabi_dadd runs two per
     * call.
     *
     * Only the aliasing case the fallback below reorders for is excluded: the
     * destination half written FIRST must not be the source half the ORR still
     * has to read. */
    const uint32_t cross_amount = (uint32_t)(32 - sh);
    const thumb_shift cross_fold = {is_left ? THUMB_SHIFT_LSR : THUMB_SHIFT_LSL, cross_amount,
                                    THUMB_SHIFT_IMMEDIATE};
    if (is_left && dst_hi != src_lo)
    {
      ot_check(thumb_call_imm_handler(dst_hi_shift, (uint32_t)dst_hi, (uint32_t)src_hi, sh, flags_safe(),
                                      ENFORCE_ENCODING_NONE));
      ot_check(th_orr_reg((uint32_t)dst_hi, (uint32_t)dst_hi, (uint32_t)src_lo, flags_safe(), cross_fold,
                          ENFORCE_ENCODING_32BIT));
      if (!skip_lo)
        ot_check(thumb_call_imm_handler(dst_lo_shift, (uint32_t)dst_lo, (uint32_t)src_lo, sh, flags_safe(),
                                        ENFORCE_ENCODING_NONE));
      goto shift64_imm_done;
    }
    if (!is_left && dst_lo != src_hi)
    {
      ot_check(th_lsr_imm((uint32_t)dst_lo, (uint32_t)src_lo, sh, flags_safe(), ENFORCE_ENCODING_NONE));
      ot_check(th_orr_reg((uint32_t)dst_lo, (uint32_t)dst_lo, (uint32_t)src_hi, flags_safe(), cross_fold,
                          ENFORCE_ENCODING_32BIT));
      ot_check(thumb_call_imm_handler(dst_hi_shift, (uint32_t)dst_hi, (uint32_t)src_hi, sh, flags_safe(),
                                      ENFORCE_ENCODING_NONE));
      goto shift64_imm_done;
    }

    const int regs[] = {dst_lo, dst_hi, src_lo, src_hi};
    ScratchRegAlloc tmp = get_scratch_reg_with_save(thumb_exclude_mask_for_regs(4, regs) | excl);
    if (is_left)
    {
      /* Compute the cross-shift into tmp BEFORE any destination is written,
       * because dst_lo/dst_hi may alias src_lo/src_hi. */
      ot_check(thumb_call_imm_handler(cross_shift, (uint32_t)tmp.reg, (uint32_t)src_lo, 32 - sh, flags_safe(),
                           ENFORCE_ENCODING_NONE));
      if (dst_hi == src_lo)
      {
        /* dst_hi aliases src_lo — compute dst_lo first (needs src_lo). */
        if (!skip_lo)
          ot_check(
              thumb_call_imm_handler(dst_lo_shift, (uint32_t)dst_lo, (uint32_t)src_lo, sh, flags_safe(), ENFORCE_ENCODING_NONE));
        ot_check(
            thumb_call_imm_handler(dst_hi_shift, (uint32_t)dst_hi, (uint32_t)src_hi, sh, flags_safe(), ENFORCE_ENCODING_NONE));
      }
      else
      {
        /* Default order: dst_hi first to avoid clobbering src_hi via dst_lo. */
        ot_check(
            thumb_call_imm_handler(dst_hi_shift, (uint32_t)dst_hi, (uint32_t)src_hi, sh, flags_safe(), ENFORCE_ENCODING_NONE));
        if (!skip_lo)
          ot_check(
              thumb_call_imm_handler(dst_lo_shift, (uint32_t)dst_lo, (uint32_t)src_lo, sh, flags_safe(), ENFORCE_ENCODING_NONE));
      }
      ot_check(th_orr_reg((uint32_t)dst_hi, (uint32_t)dst_hi, (uint32_t)tmp.reg, flags_safe(),
                          THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
    }
    else
    {
      /* Compute the cross-shift into tmp BEFORE any destination is written,
       * because dst_lo/dst_hi may alias src_lo/src_hi. */
      ot_check(thumb_call_imm_handler(cross_shift, (uint32_t)tmp.reg, (uint32_t)src_hi, 32 - sh, flags_safe(),
                           ENFORCE_ENCODING_NONE));
      if (dst_lo == src_hi)
      {
        /* dst_lo aliases src_hi — compute dst_hi first (needs src_hi). */
        ot_check(
            thumb_call_imm_handler(dst_hi_shift, (uint32_t)dst_hi, (uint32_t)src_hi, sh, flags_safe(), ENFORCE_ENCODING_NONE));
        ot_check(
            th_lsr_imm((uint32_t)dst_lo, (uint32_t)src_lo, sh, flags_safe(), ENFORCE_ENCODING_NONE));
      }
      else
      {
        /* Default order: dst_lo first to avoid clobbering src_lo via dst_hi. */
        ot_check(
            th_lsr_imm((uint32_t)dst_lo, (uint32_t)src_lo, sh, flags_safe(), ENFORCE_ENCODING_NONE));
        ot_check(
            thumb_call_imm_handler(dst_hi_shift, (uint32_t)dst_hi, (uint32_t)src_hi, sh, flags_safe(), ENFORCE_ENCODING_NONE));
      }
      ot_check(th_orr_reg((uint32_t)dst_lo, (uint32_t)dst_lo, (uint32_t)tmp.reg, flags_safe(),
                          THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
    }
    restore_scratch_reg(&tmp);
  }
  else if (sh == 32)
  {
    if (is_left)
    {
      /* Emit MOV dst_hi first: dst_lo may alias src_lo. */
      ot_check_mov_reg((uint32_t)dst_hi, (uint32_t)src_lo, flags_safe(), THUMB_SHIFT_DEFAULT,
                       ENFORCE_ENCODING_NONE, false);
      if (!skip_lo)
        ot_check(th_mov_imm((uint32_t)dst_lo, 0, flags_safe(), ENFORCE_ENCODING_NONE));
    }
    else
    {
      /* Emit MOV dst_lo first: dst_hi may alias src_hi. */
      ot_check_mov_reg((uint32_t)dst_lo, (uint32_t)src_hi, flags_safe(), THUMB_SHIFT_DEFAULT,
                       ENFORCE_ENCODING_NONE, false);
      if (!skip_hi)
      {
        if (arith_right)
          ot_check(
              th_asr_imm((uint32_t)dst_hi, (uint32_t)src_hi, 31, flags_safe(), ENFORCE_ENCODING_NONE));
        else
          ot_check(th_mov_imm((uint32_t)dst_hi, 0, flags_safe(), ENFORCE_ENCODING_NONE));
      }
    }
  }
  else if (sh < 64)
  {
    if (is_left)
    {
      /* Emit shift into dst_hi first: dst_lo may alias src_lo. */
      ot_check(thumb_call_imm_handler(dst_hi_shift, (uint32_t)dst_hi, (uint32_t)src_lo, sh - 32, flags_safe(),
                            ENFORCE_ENCODING_NONE));
      if (!skip_lo)
        ot_check(th_mov_imm((uint32_t)dst_lo, 0, flags_safe(), ENFORCE_ENCODING_NONE));
    }
    else
    {
      if (arith_right && dst_lo == src_hi)
      {
        /* dst_lo aliases src_hi — compute dst_hi (sign extension) first
         * while src_hi is still intact, then shift into dst_lo. */
        if (!skip_hi)
          ot_check(
              th_asr_imm((uint32_t)dst_hi, (uint32_t)src_hi, 31, flags_safe(), ENFORCE_ENCODING_NONE));
        ot_check(thumb_call_imm_handler(dst_hi_shift, (uint32_t)dst_lo, (uint32_t)src_hi, sh - 32, flags_safe(),
                              ENFORCE_ENCODING_NONE));
      }
      else
      {
        ot_check(thumb_call_imm_handler(dst_hi_shift, (uint32_t)dst_lo, (uint32_t)src_hi, sh - 32, flags_safe(),
                              ENFORCE_ENCODING_NONE));
        if (!skip_hi)
        {
          if (arith_right)
            ot_check(
                th_asr_imm((uint32_t)dst_hi, (uint32_t)src_hi, 31, flags_safe(), ENFORCE_ENCODING_NONE));
          else
            ot_check(th_mov_imm((uint32_t)dst_hi, 0, flags_safe(), ENFORCE_ENCODING_NONE));
        }
      }
    }
  }
  else /* sh >= 64 */
  {
    if (is_left)
    {
      if (!skip_lo)
        ot_check(th_mov_imm((uint32_t)dst_lo, 0, flags_safe(), ENFORCE_ENCODING_NONE));
      if (!skip_hi)
        ot_check(th_mov_imm((uint32_t)dst_hi, 0, flags_safe(), ENFORCE_ENCODING_NONE));
    }
    else if (arith_right)
    {
      /* Both halves are the sign of src_hi; dst_lo copies dst_hi, so leave
       * this degenerate path intact rather than risk the inter-half dep. */
      ot_check(
          th_asr_imm((uint32_t)dst_hi, (uint32_t)src_hi, 31, flags_safe(), ENFORCE_ENCODING_NONE));
      ot_check_mov_reg((uint32_t)dst_lo, (uint32_t)dst_hi, flags_safe(), THUMB_SHIFT_DEFAULT,
                       ENFORCE_ENCODING_NONE, false);
    }
    else
    {
      if (!skip_lo)
        ot_check(th_mov_imm((uint32_t)dst_lo, 0, flags_safe(), ENFORCE_ENCODING_NONE));
      if (!skip_hi)
        ot_check(th_mov_imm((uint32_t)dst_hi, 0, flags_safe(), ENFORCE_ENCODING_NONE));
    }
  }

shift64_imm_done:
  /* Write back.  A dead half was never materialized, so skip its store. */
  if (store_lo && !skip_lo)
  {
    MachineOperand dst_lo_op = mach_make_lo_half(dest);
    dst_lo_op.btype = IROP_BTYPE_INT32;
    mach_writeback_dest(&dst_lo_op, dst_lo);
  }
  if (store_hi && !skip_hi)
  {
    MachineOperand dst_hi_op = mach_make_hi_half(dest);
    dst_hi_op.btype = IROP_BTYPE_INT32;
    mach_writeback_dest(&dst_hi_op, dst_hi);
  }
  mach_release_all(&mctx);
}

/* ============================================================
 * MachineOperand-based data processing (_mop path)
 * ============================================================
 * thumb_emit_data_processing_mop32: simplified version of
 * thumb_emit_data_processing_op32 using MachineOperand instead of IROperand.
 * Handles 32-bit non-complex arithmetic/logic ops via the mach_* helpers,
 * eliminating the two-layer materialization present in the old path.
 */
/* TCC_NO_CMP_ADDS=1 keeps CMP Rn, #-k as CMP.W (thumb_emit_data_processing_mop32). */
TCC_DBG_ENV_FLAG(cmp_adds_off, "TCC_NO_CMP_ADDS")
static int cmp_adds_enabled(void)
{
  return !cmp_adds_off();
}

static void thumb_emit_data_processing_mop32(const MachineOperand *src1, const MachineOperand *src2,
                                             const MachineOperand *dest, TccIrOp op, ThumbDataProcessingHandler handler,
                                             thumb_flags_behaviour flags, uint32_t barrel_shift)
{
  const bool dest_sets_flags = (op == TCCIR_OP_CMP);
  MachineCodegenContext mctx = {0};

  /* Barrel-shifted immediate src2: the ALU immediate form has no shift field,
   * so if src2 carries a barrel-shift annotation but has been lowered to an
   * immediate (a rematerialized constant substituted for the shift's source
   * register — see ra_mark_rematerializable), fold the shift into the constant
   * and clear the annotation.  Without this the shift is silently dropped: the
   * immediate path below emits `<op> Rd, Rn, #imm` and the `if (!imm_emitted)`
   * shift block never runs (fuzz seed longlong:6393 — `y ^ (u5 LSR #31)` with
   * u5 remat'd to #1 wrongly emitted `eor r,r,#1` instead of `eor r,r,#0`).
   * The fusion pass keeps amount in 0..31 (never the ARM "0 means 32" LSR/ASR/
   * ROR case), so these C shifts are well-defined. */
  MachineOperand src2_folded;
  if (barrel_shift != 0 && src2->kind == MACH_OP_IMM && !src2->needs_deref && !src2->is_64bit)
  {
    uint32_t stype = (barrel_shift >> 5) & 7;
    uint32_t samt = barrel_shift & 31;
    uint32_t v = (uint32_t)src2->u.imm.val;
    uint32_t r;
    switch (stype)
    {
    case 1: r = v << samt; break;                                     /* LSL */
    case 2: r = v >> samt; break;                                     /* LSR */
    case 3: r = (uint32_t)((int32_t)v >> samt); break;                /* ASR */
    case 4: r = samt ? ((v >> samt) | (v << (32 - samt))) : v; break; /* ROR */
    default: r = v; break;
    }
    src2_folded = *src2;
    src2_folded.u.imm.val = (int64_t)(int32_t)r;
    src2 = &src2_folded;
    barrel_shift = 0;
  }

  /* RSB fast path: SUB with immediate src1 → RSB Rd, src2, #imm.
   * Avoids materializing the immediate into a register.
   * Only attempt when the immediate is encodable as a Thumb-2 modified
   * constant (th_pack_const returns non-zero, or imm==0). */
  if (op == TCCIR_OP_SUB && !dest_sets_flags && barrel_shift == 0 &&
      src1->kind == MACH_OP_IMM && !src1->needs_deref && !src1->is_64bit)
  {
    uint32_t imm = (uint32_t)src1->u.imm.val;
    if (imm == 0 || th_pack_const(imm) != 0)
    {
      int dest_reg = mach_get_dest_reg(&mctx, dest, 0);
      uint32_t excl = thumb_is_hw_reg(dest_reg) ? (1u << (uint32_t)dest_reg) : 0;
      int src2_reg = mach_ensure_in_reg(&mctx, src2, excl);
      ot_check(th_rsb_imm((uint32_t)dest_reg, (uint32_t)src2_reg, imm, flags, ENFORCE_ENCODING_NONE));
      if (dest->kind != MACH_OP_NONE)
      {
        const bool needs_wb = dest->kind == MACH_OP_SPILL || dest->kind == MACH_OP_PARAM_STACK ||
                              (dest->kind == MACH_OP_REG && (dest->needs_deref || dest->u.reg.r0 == (int)PREG_REG_NONE));
        if (needs_wb)
          mach_writeback_dest(dest, dest_reg);
      }
      mach_release_all(&mctx);
      return;
    }
  }

  /* Shift-by-0 identity: on ARM, LSR/ASR with immediate field 0 means
   * shift-by-32 (yielding 0 / sign-extend), NOT shift-by-0.  Fold x >> 0
   * to a plain MOV Rd, Rm so the semantics are correct regardless of
   * whether the optimizer managed to simplify the IR. */
  if (!dest_sets_flags && barrel_shift == 0 &&
      (op == TCCIR_OP_SHR || op == TCCIR_OP_SAR || op == TCCIR_OP_ROR) &&
      src2->kind == MACH_OP_IMM && !src2->needs_deref && !src2->is_64bit &&
      (uint32_t)src2->u.imm.val == 0)
  {
    int dest_reg = mach_get_dest_reg(&mctx, dest, 0);
    uint32_t excl = thumb_is_hw_reg(dest_reg) ? (1u << (uint32_t)dest_reg) : 0;
    int src1_reg = mach_ensure_in_reg(&mctx, src1, excl);
    ot_check_mov_reg((uint32_t)dest_reg, (uint32_t)src1_reg, flags,
                     THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE, false);
    if (dest->kind != MACH_OP_NONE)
    {
      const bool needs_wb = dest->kind == MACH_OP_SPILL || dest->kind == MACH_OP_PARAM_STACK ||
                            (dest->kind == MACH_OP_REG && (dest->needs_deref || dest->u.reg.r0 == (int)PREG_REG_NONE));
      if (needs_wb)
        mach_writeback_dest(dest, dest_reg);
    }
    mach_release_all(&mctx);
    return;
  }

  /* Mask fast paths: AND with an immediate the AND-immediate encoding cannot
   * hold, lowered as UXTB/UXTH/UBFX/BIC instead of materializing a constant.
   * See thumb_try_and_imm_special; the 64-bit half-lowering shares it. */
  if (op == TCCIR_OP_AND && !dest_sets_flags && barrel_shift == 0 &&
      src2->kind == MACH_OP_IMM && !src2->needs_deref && !src2->is_64bit &&
      flags != FLAGS_BEHAVIOUR_SET)
  {
    const uint32_t mask = (uint32_t)src2->u.imm.val;
    const ThumbAndImmForm form = thumb_and_imm_form(mask);
    /* Classify before resolving: on AND_IMM_NONE nothing has been emitted and
     * the general path below resolves both operands itself. */
    if (form != AND_IMM_NONE)
    {
      int dest_reg = mach_get_dest_reg(&mctx, dest, 0);
      uint32_t excl = thumb_is_hw_reg(dest_reg) ? (1u << (uint32_t)dest_reg) : 0;
      int src1_reg = mach_ensure_in_reg(&mctx, src1, excl);
      thumb_emit_and_imm_special(form, dest_reg, src1_reg, mask, flags);
      if (dest->kind != MACH_OP_NONE)
      {
        const bool needs_wb = dest->kind == MACH_OP_SPILL || dest->kind == MACH_OP_PARAM_STACK ||
                              (dest->kind == MACH_OP_REG && (dest->needs_deref || dest->u.reg.r0 == (int)PREG_REG_NONE));
        if (needs_wb)
          mach_writeback_dest(dest, dest_reg);
      }
      mach_release_all(&mctx);
      return;
    }
  }

  /* 1. Determine dest register (allocate scratch for spills/param/no-reg).
   * CMP and other flag-setting ops don't write a result register, so we
   * use R0 as a dummy (Rd field is architecturally ignored). */
  int dest_reg;
  if (dest_sets_flags)
    dest_reg = R0;
  else
    dest_reg = mach_get_dest_reg(&mctx, dest, 0);

  uint32_t excl = thumb_is_hw_reg(dest_reg) ? (1u << (uint32_t)dest_reg) : 0;

  /* Exclude src2's register from scratch allocation for src1.
   * Without this, materializing an immediate for src1 could pick src2's
   * register, clobbering it before src2 is read.  This applies whether
   * src2 is a plain register or a dereferenced one (the address register
   * must survive until the load). */
  if (src2->kind == MACH_OP_REG && thumb_is_hw_reg(src2->u.reg.r0))
    excl |= (1u << (uint32_t)src2->u.reg.r0);

  /* 2. Ensure src1 is in a register; add it to the exclusion mask. */
  int src1_reg = mach_ensure_in_reg(&mctx, src1, excl);
  if (thumb_is_hw_reg(src1_reg))
    excl |= (1u << (uint32_t)src1_reg);

  /* CMP Rn, #-k (k = 1..7, Rn low) as ADDS Rt, Rn, #k into a low register
   * free here: Rn - (-k) and Rn + k are one sum with one carry out and one
   * overflow, so N, Z, C and V are the same, in 16 bits where the compare
   * needs CMP.W (or CMN.W).  Zig compares against maxInt(u32), its `none`
   * index, 1,800 times in zig.c.  The free register comes from liveness alone
   * (not the prolog's pushed set), so the rehearsal decides the same way;
   * never inside an IT block, where the 16-bit ADDS does not set flags. */
  if (dest_sets_flags && src2->kind == MACH_OP_IMM && !src2->needs_deref && !src2->is_64bit &&
      thumb_is_hw_reg(src1_reg) && src1_reg < 8 && pool_flush_it_pending == 0 && tcc_state->ir && cmp_adds_enabled())
  {
    const int32_t v = (int32_t)src2->u.imm.val;
    if (v >= -7 && v <= -1)
    {
      TCCIRState *ir = tcc_state->ir;
      int rt = tcc_ls_find_free_scratch_reg(&ir->ls, ir->codegen_instruction_idx,
                                            excl | scratch_global_exclude | 0xFF00u | (1u << (uint32_t)src1_reg),
                                            ir->leaffunc);
      if (rt != PREG_NONE && rt >= 0 && rt < 8)
      {
        ot_check(th_add_imm((uint32_t)rt, (uint32_t)src1_reg, (uint32_t)-v, FLAGS_BEHAVIOUR_SET,
                            ENFORCE_ENCODING_16BIT));
        mach_release_all(&mctx);
        return;
      }
    }
  }

  /* 3. Try immediate form for src2; fall back to register if needed. */
  bool imm_emitted = false;
  int src2_reg =
      mach_ensure_imm_or_reg(&mctx, src2, excl, handler.imm_handler, dest_reg, src1_reg, flags, &imm_emitted);
  if (!imm_emitted)
  {
    /* Decode barrel shift annotation (0=none, else type<<5|amount). */
    thumb_shift sh = THUMB_SHIFT_DEFAULT;
    if (barrel_shift != 0)
    {
      static const thumb_shift_type bs_map[] = {
        [1] = THUMB_SHIFT_LSL, [2] = THUMB_SHIFT_LSR,
        [3] = THUMB_SHIFT_ASR, [4] = THUMB_SHIFT_ROR,
      };
      uint32_t stype = (barrel_shift >> 5) & 7;
      uint32_t samt = barrel_shift & 31;
      sh.type = bs_map[stype];
      sh.value = samt;
      sh.mode = THUMB_SHIFT_IMMEDIATE;
    }
    thumb_enforce_encoding enc = (barrel_shift != 0) ? ENFORCE_ENCODING_32BIT : ENFORCE_ENCODING_NONE;
    ot_check(thumb_call_reg_handler(handler.reg_handler, (uint32_t)dest_reg, (uint32_t)src1_reg, (uint32_t)src2_reg,
                                    flags, sh, enc));
  }

  /* 4. Write result back to spill slot / stack param / pointer-dest. */
  if (!dest_sets_flags && dest && dest->kind != MACH_OP_NONE)
  {
    const bool needs_wb = dest->kind == MACH_OP_SPILL || dest->kind == MACH_OP_PARAM_STACK ||
                          (dest->kind == MACH_OP_REG && (dest->needs_deref || dest->u.reg.r0 == (int)PREG_REG_NONE));
    if (needs_wb)
      mach_writeback_dest(dest, dest_reg);
  }

  /* 5. Release all scratches in LIFO order. */
  mach_release_all(&mctx);
}

void tcc_gen_machine_data_processing_mop(MachineOperand src1, MachineOperand src2, MachineOperand dest, TccIrOp op,
                                         uint32_t barrel_shift)
{
  data_processing_mop_impl(src1, src2, dest, op, flags_safe(), barrel_shift);
}

void tcc_gen_machine_data_processing_mop_flags(MachineOperand src1, MachineOperand src2, MachineOperand dest,
                                               TccIrOp op, uint32_t barrel_shift)
{
  data_processing_mop_impl(src1, src2, dest, op, FLAGS_BEHAVIOUR_SET, barrel_shift);
}

static void data_processing_mop_impl(MachineOperand src1, MachineOperand src2, MachineOperand dest, TccIrOp op,
                                     thumb_flags_behaviour flags_override, uint32_t barrel_shift)
{
  ThumbDataProcessingHandler handler;
  ThumbDataProcessingHandler carry_handler; /* used for hi word of 64-bit ops */
  bool uses_carry = false;
  /* CMP always sets flags — it has no non-flag-setting variant.
   * Ignore FLAGS_BEHAVIOUR_BLOCK for CMP; it must always use SET. */
  thumb_flags_behaviour flags = (op == TCCIR_OP_CMP) ? FLAGS_BEHAVIOUR_SET : flags_override;

  switch (op)
  {
  case TCCIR_OP_ADD:
    handler.imm_handler = th_add_imm;
    handler.reg_handler = th_add_reg;
    carry_handler.imm_handler = th_adc_imm;
    carry_handler.reg_handler = th_adc_reg;
    uses_carry = true;
    break;
  case TCCIR_OP_SUB:
    handler.imm_handler = th_sub_imm;
    handler.reg_handler = th_sub_reg;
    carry_handler.imm_handler = th_sbc_imm;
    carry_handler.reg_handler = th_sbc_reg;
    uses_carry = true;
    break;
  case TCCIR_OP_CMP:
    handler.imm_handler = th_cmp_imm_handler;
    handler.reg_handler = th_cmp_reg;
    carry_handler.imm_handler = th_sbc_imm;
    carry_handler.reg_handler = th_sbc_reg;
    uses_carry = true;
    break;
  case TCCIR_OP_SHL:
    handler.imm_handler = th_lsl_imm;
    handler.reg_handler = th_lsl_reg;
    carry_handler = handler;
    break;
  case TCCIR_OP_SHR:
    handler.imm_handler = th_lsr_imm;
    handler.reg_handler = th_lsr_reg;
    carry_handler = handler;
    break;
  case TCCIR_OP_SAR:
    handler.imm_handler = th_asr_imm;
    handler.reg_handler = th_asr_reg;
    carry_handler = handler;
    break;
  case TCCIR_OP_ROR:
    handler.imm_handler = th_ror_imm;
    handler.reg_handler = th_ror_reg;
    carry_handler = handler;
    break;
  case TCCIR_OP_OR:
    handler.imm_handler = th_orr_imm;
    handler.reg_handler = th_orr_reg;
    carry_handler = handler;
    break;
  case TCCIR_OP_AND:
    handler.imm_handler = th_and_imm;
    handler.reg_handler = th_and_reg;
    carry_handler = handler;
    break;
  case TCCIR_OP_XOR:
    handler.imm_handler = th_eor_imm;
    handler.reg_handler = th_eor_reg;
    carry_handler = handler;
    break;
  case TCCIR_OP_ADC_GEN:
    flags = FLAGS_BEHAVIOUR_SET;
    /* fall through */
  case TCCIR_OP_ADC_USE:
    handler.imm_handler = th_adc_imm;
    handler.reg_handler = th_adc_reg;
    carry_handler = handler;
    break;
  default:
    tcc_ice("tcc_gen_machine_data_processing_mop: unhandled op %d", (int)op);
    return;
  }

  /* Dispatch 64-bit pair destinations to the mop64 path.
   * CMP has no dest (MACH_OP_NONE), so also check src1 for 64-bit. */
  if (dest.is_64bit || (op == TCCIR_OP_CMP && src1.is_64bit))
  {
    if (op == TCCIR_OP_SHL || op == TCCIR_OP_SHR || op == TCCIR_OP_SAR)
    {
      /* Two independent reasons a half need not be written: shift64_dead_half's
       * bitfield-extract rule (bits 16-17), and zero_half64 finding the half a
       * provable zero that every consumer now ignores (bit 22/23). */
      const uint32_t zh = (barrel_shift >> 18) & 0x7Fu;
      bool skip_lo = ((barrel_shift >> 16) & 1) || (zh & ZH64_D_LO) != 0;
      bool skip_hi = ((barrel_shift >> 17) & 1) || (zh & ZH64_D_HI) != 0;
      thumb_emit_shift64_mop(&src1, &src2, &dest, op, skip_lo, skip_hi);
    }
    else
      thumb_emit_data_processing_mop64(&src1, &src2, &dest, op, handler, carry_handler, uses_carry,
                                       (barrel_shift >> 18) & 0x7Fu);
    return;
  }

  thumb_emit_data_processing_mop32(&src1, &src2, &dest, op, handler, flags, barrel_shift & 0xFFFFu);
}

/* tcc_gen_machine_ubfx_mop: emit UBFX Rd, Rn, #lsb, #width.
 * src2 encodes lsb (bits 0-4) and width (bits 5-9). */
void tcc_gen_machine_ubfx_mop(MachineOperand src1, MachineOperand src2, MachineOperand dest)
{
  MachineCodegenContext ctx = {0};
  int rd = mach_get_dest_reg(&ctx, &dest, 0);
  uint32_t excl = (1u << (uint32_t)rd);
  int param = (src2.kind == MACH_OP_IMM) ? (int)src2.u.imm.val : 0;
  /* The field may live in the HIGH word of a 64-bit source: `(v >> 52) & 0x7FF`
   * is one UBFX on that word, with no shift in front of it. */
  if ((param & UBFX_HI_HALF) && src1.is_64bit && !src1.needs_deref)
    src1 = mach_make_hi_half(&src1);
  int rn = mach_ensure_in_reg(&ctx, &src1, excl);
  int lsb = param & 0x1F;
  int width = (param >> 5) & 0x1F;
  if (width == 0)
    width = 8;
  /* A low byte or halfword is UXTB / UXTH, which has a 16-bit form for low
   * registers; UBFX is always 32 bits. */
  if (lsb == 0 && (width == 8 || width == 16))
  {
    if (width == 8)
      ot_check(th_uxtb((uint32_t)rd, (uint32_t)rn, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
    else
      ot_check(th_uxth((uint32_t)rd, (uint32_t)rn, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
    mach_writeback_dest(&dest, rd);
    mach_release_all(&ctx);
    return;
  }
  int widthm1 = width - 1;
  int imm3 = (lsb >> 2) & 0x7;
  int imm2 = lsb & 0x3;
  /* Thumb-2 UBFX encoding: 11110 0 11 1100 Rn | 0 imm3 Rd imm2 0 widthm1 */
  thumb_opcode op;
  op.size = 4;
  op.opcode = 0xF3C00000 | ((uint32_t)rn << 16) | ((uint32_t)imm3 << 12) | ((uint32_t)rd << 8) | ((uint32_t)imm2 << 6) | (uint32_t)widthm1;
  ot(op);
  mach_writeback_dest(&dest, rd);
  mach_release_all(&ctx);
}

/* tcc_machine_has_bit_ops: does the active core encode clz/rbit/rev/rev16?
 * All four live in the Thumb-2 main extension (rev/rev16 also have a T16 form,
 * but the front end needs one answer for the whole family, so require T32 +
 * clz_rbit). */
ST_FUNC int tcc_machine_has_bit_ops(void)
{
  return arm_target_dependent.feat.t32 && arm_target_dependent.feat.clz_rbit;
}

/* tcc_gen_machine_bitop1_mop: emit a single-operand bit manipulation,
 * dest = <op>(src1), for TCCIR_OP_CLZ / RBIT / REV / REV16. */
ST_FUNC void tcc_gen_machine_bitop1_mop(MachineOperand src1, MachineOperand dest, TccIrOp op)
{
  MachineCodegenContext ctx = {0};
  int rd = mach_get_dest_reg(&ctx, &dest, 0);
  int rm = mach_ensure_in_reg(&ctx, &src1, (1u << (uint32_t)rd));
  switch (op)
  {
  case TCCIR_OP_CLZ:
    ot_check(th_clz((uint32_t)rd, (uint32_t)rm));
    break;
  case TCCIR_OP_RBIT:
    ot_check(th_rbit((uint32_t)rd, (uint32_t)rm));
    break;
  case TCCIR_OP_REV:
    ot_check(th_rev((uint32_t)rd, (uint32_t)rm, ENFORCE_ENCODING_NONE));
    break;
  case TCCIR_OP_REV16:
    ot_check(th_rev16((uint32_t)rd, (uint32_t)rm, ENFORCE_ENCODING_NONE));
    break;
  default:
    tcc_ice("tcc_gen_machine_bitop1_mop: unhandled op %d", (int)op);
    break;
  }
  mach_writeback_dest(&dest, rd);
  mach_release_all(&ctx);
}

/* tcc_gen_machine_sbfx_mop: emit SBFX Rd, Rn, #lsb, #width.
 * src2 encodes lsb (bits 0-4) and width (bits 5-9).  Same field layout as UBFX;
 * only the base opcode differs (0xF3400000 vs 0xF3C00000). */
void tcc_gen_machine_sbfx_mop(MachineOperand src1, MachineOperand src2, MachineOperand dest)
{
  MachineCodegenContext ctx = {0};
  int rd = mach_get_dest_reg(&ctx, &dest, 0);
  uint32_t excl = (1u << (uint32_t)rd);
  int rn = mach_ensure_in_reg(&ctx, &src1, excl);
  int param = (src2.kind == MACH_OP_IMM) ? (int)src2.u.imm.val : 0;
  int lsb = param & 0x1F;
  int width = (param >> 5) & 0x1F;
  if (width == 0)
    width = 8;
  /* SXTB / SXTH: 16 bits for low registers, like UXTB / UXTH above. */
  if (lsb == 0 && (width == 8 || width == 16))
  {
    if (width == 8)
      ot_check(th_sxtb((uint32_t)rd, (uint32_t)rn, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
    else
      ot_check(th_sxth((uint32_t)rd, (uint32_t)rn, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
    mach_writeback_dest(&dest, rd);
    mach_release_all(&ctx);
    return;
  }
  int widthm1 = width - 1;
  int imm3 = (lsb >> 2) & 0x7;
  int imm2 = lsb & 0x3;
  /* Thumb-2 SBFX encoding: 11110 0 11 0100 Rn | 0 imm3 Rd imm2 0 widthm1 */
  thumb_opcode op;
  op.size = 4;
  op.opcode = 0xF3400000 | ((uint32_t)rn << 16) | ((uint32_t)imm3 << 12) | ((uint32_t)rd << 8) | ((uint32_t)imm2 << 6) | (uint32_t)widthm1;
  ot(op);
  mach_writeback_dest(&dest, rd);
  mach_release_all(&ctx);
}

/* tcc_gen_machine_bfi_mop: emit BFI Rd, Rn, #lsb, #width.
 * src1 = host word (moved into Rd, the BFI base, if not already there),
 * src2 = value supplying the field bits (only its low `width` bits are used),
 * dest = result.  params packs lsb (bits 0-7) and width (bits 8-15). */
void tcc_gen_machine_bfi_mop(MachineOperand src1, MachineOperand src2, MachineOperand dest, uint32_t params)
{
  MachineCodegenContext ctx = {0};
  int rd = mach_get_dest_reg(&ctx, &dest, 0);
  int rn = mach_ensure_in_reg(&ctx, &src2, 0);                            /* value (Rn) */
  int rword = mach_ensure_in_reg(&ctx, &src1, (1u << (uint32_t)rn));      /* host word */
  /* Establish Rd = host word.  If the value happens to live in Rd (RA coalesced
   * the result onto src2), preserve it in a scratch before clobbering Rd. */
  if (rd != rword)
  {
    if (rd == rn)
    {
      int tmp = mach_alloc_scratch(&ctx, (1u << (uint32_t)rd) | (1u << (uint32_t)rword));
      ot_check_mov_reg((uint32_t)tmp, (uint32_t)rd, flags_safe(), THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE, false);
      rn = tmp;
    }
    ot_check_mov_reg((uint32_t)rd, (uint32_t)rword, flags_safe(), THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE, false);
  }
  int lsb = (int)(params & 0xFF);
  int width = (int)((params >> 8) & 0xFF);
  if (width < 1)
    width = 1;
  int msb = lsb + width - 1;
  if (msb > 31)
    msb = 31;
  int imm3 = (lsb >> 2) & 0x7;
  int imm2 = lsb & 0x3;
  /* Thumb-2 BFI: 11110 0 11 0110 Rn | 0 imm3 Rd imm2 0 msb */
  thumb_opcode op;
  op.size = 4;
  op.opcode = 0xF3600000 | ((uint32_t)rn << 16) | ((uint32_t)imm3 << 12) | ((uint32_t)rd << 8) | ((uint32_t)imm2 << 6) | (uint32_t)msb;
  ot(op);
  mach_writeback_dest(&dest, rd);
  mach_release_all(&ctx);
}

/* ============================================================
 * MachineOperand-based mul/div/mod/test-zero (_mop path)
 * ============================================================
 * Internal helpers and public entry point for 32-bit register-only ops:
 *   MUL, DIV, UDIV  — simple rd = rn OP rm
 *   IMOD, UMOD      — dest = src1 - (src1/src2)*src2
 *   TEST_ZERO       — CMP src, #0 (flags only, no dest)
 * MLA (accumulator) and UMULL (64-bit pair) remain on the old IR path.
 */

/* Emit rd = emitter(src1, src2) for register-only 3-operand ops. */
static void mach_regonly_binop_mop(MachineCodegenContext *ctx, const MachineOperand *src1, const MachineOperand *src2,
                                   const MachineOperand *dest, thumb_regonly3_handler_t emitter)
{
  /* 1. Get dest register (scratch if spill/param). */
  int dest_reg = mach_get_dest_reg(ctx, dest, 0);
  uint32_t excl = thumb_is_hw_reg(dest_reg) ? (1u << (uint32_t)dest_reg) : 0;

  /* Pre-exclude src2's physical register so that loading src1 (which may
   * need a scratch for deref) does not clobber src2's value.  This must apply
   * even when src2 is a DEREF operand: its register holds the pointer, and the
   * step-3 load reads through that same register, so it has to survive src1's
   * materialization.  Omitting the deref case let src1's scratch land on the
   * pointer register at high pressure -> `u6 * arr10[3]` dereferenced u6 as a
   * pointer -> bus fault (fuzz switch 219754). Matches the data-processing
   * guard in thumb_emit_data_processing_mop32. */
  if (src2->kind == MACH_OP_REG && thumb_is_hw_reg(src2->u.reg.r0))
    excl |= (1u << (uint32_t)src2->u.reg.r0);

  /* 2. Ensure src1 in a register; extend exclusion mask. */
  int src1_reg = mach_ensure_in_reg(ctx, src1, excl);
  if (thumb_is_hw_reg(src1_reg))
    excl |= (1u << (uint32_t)src1_reg);

  /* 3. Ensure src2 in a register. */
  int src2_reg = mach_ensure_in_reg(ctx, src2, excl);

  /* 4. Emit instruction. */
  ot_check(emitter((uint32_t)dest_reg, (uint32_t)src1_reg, (uint32_t)src2_reg));

  /* 5. Write result back to spill slot / stack param if needed. */
  mach_writeback_dest(dest, dest_reg);
}

/* Emit dest = src1 - (src1/src2)*src2 for IMOD/UMOD. */
static void mach_mod_mop(MachineCodegenContext *ctx, const MachineOperand *src1, const MachineOperand *src2,
                         const MachineOperand *dest, thumb_regonly3_handler_t div_emitter)
{
  /* 1. Get dest register. */
  int dest_reg = mach_get_dest_reg(ctx, dest, 0);
  uint32_t excl = thumb_is_hw_reg(dest_reg) ? (1u << (uint32_t)dest_reg) : 0;

  /* Pre-exclude src2's physical register so that materializing src1 (which may
   * need a scratch when it is an immediate or a deref) does not clobber src2's
   * value before the divide reads it — same guard as mach_regonly_binop_mop.
   * Without it an immediate dividend's scratch load could land on the divisor's
   * register (random-C O1 wrong-code, seed 151: `K % (lr|1)` divisor clobbered).
   * Applies to the deref case too (pointer register must survive src1's
   * materialization, same as mach_regonly_binop_mop / fuzz switch 219754). */
  if (src2->kind == MACH_OP_REG && thumb_is_hw_reg(src2->u.reg.r0))
    excl |= (1u << (uint32_t)src2->u.reg.r0);

  /* 2. Ensure src1 in a register. */
  int src1_reg = mach_ensure_in_reg(ctx, src1, excl);
  if (thumb_is_hw_reg(src1_reg))
    excl |= (1u << (uint32_t)src1_reg);

  /* 3. Ensure src2 in a register. */
  int src2_reg = mach_ensure_in_reg(ctx, src2, excl);
  if (thumb_is_hw_reg(src2_reg))
    excl |= (1u << (uint32_t)src2_reg);

  /* 4. Scratch register for quotient. */
  int quotient_reg = mach_alloc_scratch(ctx, excl);

  /* 5. quotient = src1 / src2 */
  ot_check(div_emitter((uint32_t)quotient_reg, (uint32_t)src1_reg, (uint32_t)src2_reg));

  /* 6. quotient = quotient * src2 */
  ot_check(thumb_mul_regonly((uint32_t)quotient_reg, (uint32_t)quotient_reg, (uint32_t)src2_reg));

  /* 7. dest = src1 - quotient */
  ot_check(th_sub_reg(dest_reg, src1_reg, quotient_reg, flags_safe(), THUMB_SHIFT_DEFAULT,
                      ENFORCE_ENCODING_NONE));

  /* 8. Write result back. */
  mach_writeback_dest(dest, dest_reg);
}

/* thumb_emit_mul64_mop
 * ============================================================
 * Emit a 64-bit multiply (lower 64 bits of the result) using MachineOperands.
 *
 * For a 64-bit result (dest->is_64bit):
 *   UMULL r_c_lo, r_c_hi, r_a_lo, r_b_lo  // a_lo * b_lo → 64-bit unsigned
 *   MLA   r_c_hi, r_a_hi, r_b_lo, r_c_hi  // cross product (when src1 is 64-bit)
 *   MLA   r_c_hi, r_a_lo, r_b_hi, r_c_hi  // cross product (when src2 is 64-bit)
 *
 * For a 32-bit result with 64-bit source(s):
 *   MUL   r_c, r_a_lo, r_b_lo             // upper bits don't contribute
 *
 * The lower 64 bits of the signed / unsigned 128-bit product are identical
 * (i.e. UMULL is correct for both signed and unsigned long long mul).
 * The caller must call mach_release_all() after this function returns.
 */
static void thumb_emit_mul64_mop(MachineCodegenContext *ctx, const MachineOperand *src1, const MachineOperand *src2,
                                 const MachineOperand *dest)
{
  uint32_t excl = 0;

  /* Resolve deref'd 64-bit sources before splitting into halves. */
  MachineOperand r_s1 = mach_resolve_deref_64(ctx, src1, &excl);
  MachineOperand r_s2 = mach_resolve_deref_64(ctx, src2, &excl);

  /* Load lo halves (always needed). */
  MachineOperand a_lo_op = r_s1.is_64bit ? mach_make_lo_half(&r_s1) : r_s1;
  a_lo_op.btype = IROP_BTYPE_INT32;
  a_lo_op.is_64bit = false;
  MachineOperand b_lo_op = r_s2.is_64bit ? mach_make_lo_half(&r_s2) : r_s2;
  b_lo_op.btype = IROP_BTYPE_INT32;
  b_lo_op.is_64bit = false;

  int r_a_lo = mach_ensure_in_reg(ctx, &a_lo_op, excl);
  if (thumb_is_hw_reg(r_a_lo))
    excl |= (1u << (uint32_t)r_a_lo);
  int r_b_lo = mach_ensure_in_reg(ctx, &b_lo_op, excl);
  if (thumb_is_hw_reg(r_b_lo))
    excl |= (1u << (uint32_t)r_b_lo);

  if (dest->is_64bit)
  {
    /* Load hi halves for cross-product MLA terms. */
    int r_a_hi = PREG_REG_NONE, r_b_hi = PREG_REG_NONE;
    if (r_s1.is_64bit)
    {
      MachineOperand a_hi_op = mach_make_hi_half(&r_s1);
      a_hi_op.btype = IROP_BTYPE_INT32;
      r_a_hi = mach_ensure_in_reg(ctx, &a_hi_op, excl);
      if (thumb_is_hw_reg(r_a_hi))
        excl |= (1u << (uint32_t)r_a_hi);
    }
    if (r_s2.is_64bit)
    {
      MachineOperand b_hi_op = mach_make_hi_half(&r_s2);
      b_hi_op.btype = IROP_BTYPE_INT32;
      r_b_hi = mach_ensure_in_reg(ctx, &b_hi_op, excl);
      if (thumb_is_hw_reg(r_b_hi))
        excl |= (1u << (uint32_t)r_b_hi);
    }

    /* Allocate 64-bit destination pair — must not overlap sources for UMULL. */
    MachineOperand dst_lo_op = mach_make_lo_half(dest);
    dst_lo_op.btype = IROP_BTYPE_INT32;
    MachineOperand dst_hi_op = mach_make_hi_half(dest);
    dst_hi_op.btype = IROP_BTYPE_INT32;

    int r_c_lo = mach_get_dest_reg(ctx, &dst_lo_op, excl);
    if (thumb_is_hw_reg(r_c_lo))
      excl |= (1u << (uint32_t)r_c_lo);
    int r_c_hi = mach_get_dest_reg(ctx, &dst_hi_op, excl);

    /* UMULL: r_c_lo:r_c_hi = r_a_lo * r_b_lo (unsigned 64-bit product) */
    ot_check(th_umull((uint32_t)r_c_lo, (uint32_t)r_c_hi, (uint32_t)r_a_lo, (uint32_t)r_b_lo));

    /* Add cross products to high half. */
    if (thumb_is_hw_reg(r_a_hi))
      ot_check(th_mla((uint32_t)r_c_hi, (uint32_t)r_a_hi, (uint32_t)r_b_lo, (uint32_t)r_c_hi));
    if (thumb_is_hw_reg(r_b_hi))
      ot_check(th_mla((uint32_t)r_c_hi, (uint32_t)r_a_lo, (uint32_t)r_b_hi, (uint32_t)r_c_hi));

    mach_writeback_dest(&dst_lo_op, r_c_lo);
    mach_writeback_dest(&dst_hi_op, r_c_hi);
  }
  else
  {
    /* 32-bit result with 64-bit source(s): only the low bits matter. */
    MachineOperand dest32 = *dest;
    dest32.is_64bit = false;
    int r_c = mach_get_dest_reg(ctx, &dest32, excl);
    ot_check(thumb_mul_regonly((uint32_t)r_c, (uint32_t)r_a_lo, (uint32_t)r_b_lo));
    mach_writeback_dest(&dest32, r_c);
  }
}

/* tcc_gen_machine_muldiv_mop: MachineOperand-based entry point for multiply,
 * divide, modulo, and test-zero operations.  Called from ir/codegen.c when
 * use_mop_muldiv is true for:
 *   MUL                         — 32-bit or 64-bit multiply
 *   DIV, UDIV, IMOD, UMOD       — 32-bit divide/modulo
 *   TEST_ZERO                   — 32-bit or 64-bit compare against zero (flags only)
 * MLA (accumulator; 4-operand) uses tcc_gen_machine_mla_mop.
 * UMULL (64-bit output from 32-bit inputs) uses tcc_gen_machine_umull_mop.
 */
/* Decompose multiply-by-constant into shift+add sequences.
 * Returns 1 if handled, 0 to fall back to hardware MUL. */
static int thumb_try_mul_by_const_mop(MachineCodegenContext *ctx, MachineOperand *src1, MachineOperand *src2,
                                      MachineOperand *dest)
{
  /* Identify which operand is the immediate and which is the variable. */
  const MachineOperand *imm_op, *var_op;
  if (src2->kind == MACH_OP_IMM)
  {
    imm_op = src2;
    var_op = src1;
  }
  else if (src1->kind == MACH_OP_IMM)
  {
    imm_op = src1;
    var_op = src2;
  }
  else
    return 0;

  int64_t c = imm_op->u.imm.val;
  if (c <= 0)
    return 0;

  /* Determine the decomposition pattern.
   * We handle: powers of 2, (2^n ± 1), and products thereof.
   *
   * Pattern             Insns  Example
   * ─────────────────── ───── ───────
   * 2^n                 1     LSL Rd, Rn, #n
   * 2^n + 1             1     ADD Rd, Rn, Rn LSL #n
   * 2^n - 1             1     SUB Rd, Rn LSL #n, Rn  (RSB-like via SUB)
   * (2^a + 1) * 2^b     2     ADD Rd, Rn, Rn LSL #a; LSL Rd, Rd, #b
   * (2^a - 1) * 2^b     2     SUB Rd, Rn LSL #a, Rn; LSL Rd, Rd, #b
   * (2^a + 1)(2^b + 1)  2     ADD Rd, Rn, Rn LSL #a; ADD Rd, Rd, Rn LSL #(a+b)
   *                            — only some cases, handled via table
   */

  int shift1 = 0, shift2 = 0;
  enum
  {
    MUL_NONE,
    MUL_POWER_OF_2,          /* c = 2^n : LSL #n */
    MUL_TWO_N_PLUS_1,        /* c = 2^n+1 : ADD Rd, Rn, Rn LSL #n */
    MUL_TWO_N_MINUS_1,       /* c = 2^n-1 : SUB Rd, Rn LSL #n, Rn */
    MUL_TWO_N_PLUS_1_SHIFT,  /* c = (2^a+1)*2^b : ADD; LSL */
    MUL_TWO_N_MINUS_1_SHIFT, /* c = (2^a-1)*2^b : SUB; LSL */
  } pattern = MUL_NONE;

  /* Check for power of 2 */
  if (c > 0 && (c & (c - 1)) == 0)
  {
    int n = 0;
    int64_t v = c;
    while (v > 1)
    {
      n++;
      v >>= 1;
    }
    if (n >= 1 && n <= 31)
    {
      shift1 = n;
      pattern = MUL_POWER_OF_2;
    }
  }

  /* Check for 2^n + 1 (3, 5, 9, 17, ...) */
  if (pattern == MUL_NONE && c >= 3)
  {
    int64_t v = c - 1;
    if (v > 0 && (v & (v - 1)) == 0)
    {
      int n = 0;
      while (v > 1)
      {
        n++;
        v >>= 1;
      }
      if (n >= 1 && n <= 31)
      {
        shift1 = n;
        pattern = MUL_TWO_N_PLUS_1;
      }
    }
  }

  /* Check for 2^n - 1 (7, 15, 31, ...) */
  if (pattern == MUL_NONE && c >= 7)
  {
    int64_t v = c + 1;
    if (v > 0 && (v & (v - 1)) == 0)
    {
      int n = 0;
      while (v > 1)
      {
        n++;
        v >>= 1;
      }
      if (n >= 2 && n <= 31)
      {
        shift1 = n;
        pattern = MUL_TWO_N_MINUS_1;
      }
    }
  }

  /* Check for (2^a + 1) * 2^b (6, 10, 12, 20, 24, 40, 48, ...) */
  if (pattern == MUL_NONE && c >= 6)
  {
    int64_t v = c;
    int b = 0;
    while ((v & 1) == 0)
    {
      b++;
      v >>= 1;
    }
    if (b >= 1 && b <= 31)
    {
      int64_t inner = v - 1;
      if (inner > 0 && (inner & (inner - 1)) == 0)
      {
        int a = 0;
        while (inner > 1)
        {
          a++;
          inner >>= 1;
        }
        if (a >= 1 && a <= 31)
        {
          shift1 = a;
          shift2 = b;
          pattern = MUL_TWO_N_PLUS_1_SHIFT;
        }
      }
    }
  }

  /* Check for (2^a - 1) * 2^b (14, 28, 30, 56, 60, 62, ...) */
  if (pattern == MUL_NONE && c >= 14)
  {
    int64_t v = c;
    int b = 0;
    while ((v & 1) == 0)
    {
      b++;
      v >>= 1;
    }
    if (b >= 1 && b <= 31)
    {
      int64_t inner = v + 1;
      if (inner > 0 && (inner & (inner - 1)) == 0)
      {
        int a = 0;
        while (inner > 1)
        {
          a++;
          inner >>= 1;
        }
        if (a >= 2 && a <= 31)
        {
          shift1 = a;
          shift2 = b;
          pattern = MUL_TWO_N_MINUS_1_SHIFT;
        }
      }
    }
  }

  if (pattern == MUL_NONE)
    return 0;

  /* Emit the decomposed sequence. */
  int dest_reg = mach_get_dest_reg(ctx, dest, 0);
  uint32_t excl = thumb_is_hw_reg(dest_reg) ? (1u << (uint32_t)dest_reg) : 0;
  int var_reg = mach_ensure_in_reg(ctx, var_op, excl);
  thumb_flags_behaviour fl = flags_safe();
  thumb_shift sh;

  switch (pattern)
  {
  case MUL_POWER_OF_2:
    ot_check(th_lsl_imm((uint32_t)dest_reg, (uint32_t)var_reg, (uint32_t)shift1, fl, ENFORCE_ENCODING_NONE));
    break;

  case MUL_TWO_N_PLUS_1:
    sh = (thumb_shift){THUMB_SHIFT_LSL, (uint16_t)shift1, THUMB_SHIFT_IMMEDIATE};
    ot_check(th_add_reg((uint32_t)dest_reg, (uint32_t)var_reg, (uint32_t)var_reg, fl, sh, ENFORCE_ENCODING_NONE));
    break;

  case MUL_TWO_N_MINUS_1:
  {
    /* Thumb-2 SUB Rd, Rn, Rm LSL #n = Rn - (Rm << n).
     * We need (var << n) - var, which is the reverse. No RSB with shift
     * exists in Thumb-2, so we do: LSL tmp, var, #n; SUB Rd, tmp, var.
     * The LSL destination must differ from var_reg, otherwise it destroys
     * var before the SUB reads it (mach_ensure_in_reg returns an already-
     * resident var in dest_reg's register, ignoring the exclusion mask, so
     * dest_reg == var_reg is reachable).  Shift straight into dest when they
     * differ; otherwise borrow a scratch. */
    int tmp = (dest_reg == var_reg) ? mach_alloc_scratch(ctx, (1u << (uint32_t)var_reg)) : dest_reg;
    ot_check(th_lsl_imm((uint32_t)tmp, (uint32_t)var_reg, (uint32_t)shift1, fl, ENFORCE_ENCODING_NONE));
    sh = (thumb_shift){THUMB_SHIFT_NONE, 0, THUMB_SHIFT_IMMEDIATE};
    ot_check(th_sub_reg((uint32_t)dest_reg, (uint32_t)tmp, (uint32_t)var_reg, fl, sh, ENFORCE_ENCODING_NONE));
    break;
  }

  case MUL_TWO_N_PLUS_1_SHIFT:
    sh = (thumb_shift){THUMB_SHIFT_LSL, (uint16_t)shift1, THUMB_SHIFT_IMMEDIATE};
    ot_check(th_add_reg((uint32_t)dest_reg, (uint32_t)var_reg, (uint32_t)var_reg, fl, sh, ENFORCE_ENCODING_NONE));
    ot_check(th_lsl_imm((uint32_t)dest_reg, (uint32_t)dest_reg, (uint32_t)shift2, fl, ENFORCE_ENCODING_NONE));
    break;

  case MUL_TWO_N_MINUS_1_SHIFT:
  {
    /* (2^a - 1) * 2^b: LSL tmp, var, #a; SUB tmp, tmp, var; LSL Rd, tmp, #b.
     * As in MUL_TWO_N_MINUS_1, the first LSL must not target var_reg, or it
     * destroys var before the SUB reads it. */
    int tmp = (dest_reg == var_reg) ? mach_alloc_scratch(ctx, (1u << (uint32_t)var_reg)) : dest_reg;
    ot_check(th_lsl_imm((uint32_t)tmp, (uint32_t)var_reg, (uint32_t)shift1, fl, ENFORCE_ENCODING_NONE));
    sh = (thumb_shift){THUMB_SHIFT_NONE, 0, THUMB_SHIFT_IMMEDIATE};
    ot_check(th_sub_reg((uint32_t)tmp, (uint32_t)tmp, (uint32_t)var_reg, fl, sh, ENFORCE_ENCODING_NONE));
    ot_check(th_lsl_imm((uint32_t)dest_reg, (uint32_t)tmp, (uint32_t)shift2, fl, ENFORCE_ENCODING_NONE));
    break;
  }

  default:
    return 0;
  }

  mach_writeback_dest(dest, dest_reg);
  mach_release_all(ctx);
  return 1;
}

/* Fused MUL-by-const + ADD peephole.
 * Transforms:  tmp = var * C;  dest = base + tmp
 * Into a shorter sequence using ARM shifted-add (ADD Rd, Rn, Rm LSL #imm):
 *   C = 2^n:              ADD dest, base, var LSL #n           (1 insn vs 2)
 *   C = (2^a+1)*2^b:      ADD t, var, var LSL #a;
 *                          ADD dest, base, t LSL #b             (2 insn vs 3)
 *   C = (2^a-1)*2^b:      LSL t, var, #a; SUB t, t, var;
 *                          ADD dest, base, t LSL #b             (3 insn vs 4)
 * Returns 1 if fused, 0 to fall back to separate MUL + ADD. */
ST_FUNC int tcc_gen_machine_mul_const_add_fused_mop(MachineOperand mul_var, int64_t mul_const,
                                                    MachineOperand mul_dest, MachineOperand add_base,
                                                    MachineOperand add_dest)
{
  if (mul_const <= 0)
    return 0;

  int shift1 = 0, shift2 = 0;
  enum
  {
    FUSE_NONE,
    FUSE_POW2,
    FUSE_TWO_N_PLUS_1_SHIFT,
    FUSE_TWO_N_MINUS_1_SHIFT,
  } pattern = FUSE_NONE;

  /* Power of 2: C = 2^n */
  if (mul_const > 1 && (mul_const & (mul_const - 1)) == 0)
  {
    int n = 0;
    int64_t v = mul_const;
    while (v > 1) { n++; v >>= 1; }
    if (n >= 1 && n <= 31)
    {
      shift1 = n;
      pattern = FUSE_POW2;
    }
  }

  /* (2^a + 1) * 2^b: e.g. 12 = 3*4 = (2^1+1)*2^2 */
  if (pattern == FUSE_NONE && mul_const >= 6)
  {
    int64_t v = mul_const;
    int b = 0;
    while ((v & 1) == 0) { b++; v >>= 1; }
    if (b >= 1 && b <= 31)
    {
      int64_t inner = v - 1;
      if (inner > 0 && (inner & (inner - 1)) == 0)
      {
        int a = 0;
        while (inner > 1) { a++; inner >>= 1; }
        if (a >= 1 && a <= 31)
        {
          shift1 = a;
          shift2 = b;
          pattern = FUSE_TWO_N_PLUS_1_SHIFT;
        }
      }
    }
  }

  /* (2^a - 1) * 2^b: e.g. 28 = 7*4 = (2^3-1)*2^2 */
  if (pattern == FUSE_NONE && mul_const >= 14)
  {
    int64_t v = mul_const;
    int b = 0;
    while ((v & 1) == 0) { b++; v >>= 1; }
    if (b >= 1 && b <= 31)
    {
      int64_t inner = v + 1;
      if (inner > 0 && (inner & (inner - 1)) == 0)
      {
        int a = 0;
        while (inner > 1) { a++; inner >>= 1; }
        if (a >= 2 && a <= 31)
        {
          shift1 = a;
          shift2 = b;
          pattern = FUSE_TWO_N_MINUS_1_SHIFT;
        }
      }
    }
  }

  if (pattern == FUSE_NONE)
    return 0;

  MachineCodegenContext ctx = {0};
  thumb_flags_behaviour fl = flags_safe();
  thumb_shift sh;

  /* Allocate registers: dest first (may hint to the ADD dest's phys reg),
   * then base and var, using exclusion masks to prevent conflicts. */
  int dest_reg = mach_get_dest_reg(&ctx, &add_dest, 0);
  uint32_t excl = thumb_is_hw_reg(dest_reg) ? (1u << (uint32_t)dest_reg) : 0;
  int base_reg = mach_ensure_in_reg(&ctx, &add_base, excl);
  if (thumb_is_hw_reg(base_reg))
    excl |= (1u << (uint32_t)base_reg);
  int var_reg = mach_ensure_in_reg(&ctx, &mul_var, excl);

  switch (pattern)
  {
  case FUSE_POW2:
    /* ADD dest, base, var LSL #n */
    sh = (thumb_shift){THUMB_SHIFT_LSL, (uint16_t)shift1, THUMB_SHIFT_IMMEDIATE};
    ot_check(th_add_reg((uint32_t)dest_reg, (uint32_t)base_reg, (uint32_t)var_reg, fl, sh, ENFORCE_ENCODING_NONE));
    break;

  case FUSE_TWO_N_PLUS_1_SHIFT:
  {
    /* Step 1: ADD tmp, var, var LSL #a */
    if (thumb_is_hw_reg(var_reg))
      excl |= (1u << (uint32_t)var_reg);
    int tmp_reg = mach_get_dest_reg(&ctx, &mul_dest, excl);
    sh = (thumb_shift){THUMB_SHIFT_LSL, (uint16_t)shift1, THUMB_SHIFT_IMMEDIATE};
    ot_check(th_add_reg((uint32_t)tmp_reg, (uint32_t)var_reg, (uint32_t)var_reg, fl, sh, ENFORCE_ENCODING_NONE));
    /* Step 2: ADD dest, base, tmp LSL #b */
    sh = (thumb_shift){THUMB_SHIFT_LSL, (uint16_t)shift2, THUMB_SHIFT_IMMEDIATE};
    ot_check(th_add_reg((uint32_t)dest_reg, (uint32_t)base_reg, (uint32_t)tmp_reg, fl, sh, ENFORCE_ENCODING_NONE));
    break;
  }

  case FUSE_TWO_N_MINUS_1_SHIFT:
  {
    /* Step 1: LSL tmp, var, #a */
    if (thumb_is_hw_reg(var_reg))
      excl |= (1u << (uint32_t)var_reg);
    int tmp_reg = mach_get_dest_reg(&ctx, &mul_dest, excl);
    ot_check(th_lsl_imm((uint32_t)tmp_reg, (uint32_t)var_reg, (uint32_t)shift1, fl, ENFORCE_ENCODING_NONE));
    /* Step 2: SUB tmp, tmp, var */
    sh = (thumb_shift){THUMB_SHIFT_NONE, 0, THUMB_SHIFT_IMMEDIATE};
    ot_check(th_sub_reg((uint32_t)tmp_reg, (uint32_t)tmp_reg, (uint32_t)var_reg, fl, sh, ENFORCE_ENCODING_NONE));
    /* Step 3: ADD dest, base, tmp LSL #b */
    sh = (thumb_shift){THUMB_SHIFT_LSL, (uint16_t)shift2, THUMB_SHIFT_IMMEDIATE};
    ot_check(th_add_reg((uint32_t)dest_reg, (uint32_t)base_reg, (uint32_t)tmp_reg, fl, sh, ENFORCE_ENCODING_NONE));
    break;
  }

  default:
    mach_release_all(&ctx);
    return 0;
  }

  mach_writeback_dest(&add_dest, dest_reg);
  mach_release_all(&ctx);
  return 1;
}

ST_FUNC void tcc_gen_machine_muldiv_mop(MachineOperand src1, MachineOperand src2, MachineOperand dest, TccIrOp op)
{
  MachineCodegenContext ctx = {0};
  switch (op)
  {
  case TCCIR_OP_MUL:
    if (src1.is_64bit || src2.is_64bit || dest.is_64bit)
      thumb_emit_mul64_mop(&ctx, &src1, &src2, &dest);
    else if (!thumb_try_mul_by_const_mop(&ctx, &src1, &src2, &dest))
      mach_regonly_binop_mop(&ctx, &src1, &src2, &dest, thumb_mul_regonly);
    break;
  case TCCIR_OP_DIV:
    mach_regonly_binop_mop(&ctx, &src1, &src2, &dest, thumb_sdiv_regonly);
    break;
  case TCCIR_OP_UDIV:
    mach_regonly_binop_mop(&ctx, &src1, &src2, &dest, thumb_udiv_regonly);
    break;
  case TCCIR_OP_IMOD:
    mach_mod_mop(&ctx, &src1, &src2, &dest, thumb_sdiv_regonly);
    break;
  case TCCIR_OP_UMOD:
    mach_mod_mop(&ctx, &src1, &src2, &dest, thumb_udiv_regonly);
    break;
  case TCCIR_OP_TEST_ZERO:
  {
    if (src1.is_64bit)
    {
      /* 64-bit: Z set iff (lo == 0 && hi == 0).
       * Use CMP lo,#0; IT EQ; CMPEQ hi,#0 to avoid clobbering source registers. */
      uint32_t excl = 0;
      MachineOperand resolved = mach_resolve_deref_64(&ctx, &src1, &excl);
      MachineOperand lo = mach_make_lo_half(&resolved);
      lo.btype = IROP_BTYPE_INT32;
      MachineOperand hi = mach_make_hi_half(&resolved);
      hi.btype = IROP_BTYPE_INT32;
      int r_lo = mach_ensure_in_reg(&ctx, &lo, excl);
      if (thumb_is_hw_reg(r_lo))
        excl |= (1u << (uint32_t)r_lo);
      int r_hi = mach_ensure_in_reg(&ctx, &hi, excl);
      if (!thumb_try_orrs_zero64(r_lo, r_hi))
      {
        ot_check(th_cmp_imm(r_lo, 0, FLAGS_BEHAVIOUR_SET, ENFORCE_ENCODING_NONE));
        th_literal_pool_reserve_upcoming_bytes(6);
        ot_check(th_it(mapcc(TOK_EQ), 0x8)); /* IT EQ (single instruction) */
        ot_check(th_cmp_imm(r_hi, 0, FLAGS_BEHAVIOUR_SET, ENFORCE_ENCODING_NONE));
      }
    }
    else
    {
      /* 32-bit: CMP src, #0 — no destination, only flags. */
      int src_reg = mach_ensure_in_reg(&ctx, &src1, 0);
      ot_check(th_cmp_imm(src_reg, 0, FLAGS_BEHAVIOUR_SET, ENFORCE_ENCODING_NONE));
    }
    break;
  }
  default:
    tcc_ice("tcc_gen_machine_muldiv_mop: unhandled op %d", (int)op);
    break;
  }
  mach_release_all(&ctx);
}

/* tcc_gen_machine_cmp_eq64_mop: 64-bit equality comparison.
 * Emits CMP hi1,hi2; IT EQ; CMPEQ lo1,lo2 which correctly sets
 * the Z flag for full 64-bit equality (used by SETIF/JUMPIF EQ/NE). */
ST_FUNC void tcc_gen_machine_cmp_eq64_mop(MachineOperand src1, MachineOperand src2)
{
  MachineCodegenContext ctx = {0};
  uint32_t excl = 0;

  if (src1.kind == MACH_OP_REG)
  {
    if (src1.u.reg.r0 != (int)PREG_REG_NONE)
      excl |= (1u << (uint32_t)src1.u.reg.r0);
    if (!src1.needs_deref && src1.is_64bit && src1.u.reg.r1 >= 0)
      excl |= (1u << (uint32_t)src1.u.reg.r1);
  }
  if (src2.kind == MACH_OP_REG)
  {
    if (src2.u.reg.r0 != (int)PREG_REG_NONE)
      excl |= (1u << (uint32_t)src2.u.reg.r0);
    if (!src2.needs_deref && src2.is_64bit && src2.u.reg.r1 >= 0)
      excl |= (1u << (uint32_t)src2.u.reg.r1);
  }

  MachineOperand r_src1 = mach_resolve_deref_64(&ctx, &src1, &excl);
  MachineOperand r_src2 = mach_resolve_deref_64(&ctx, &src2, &excl);

  int rn_lo, rn_hi;
  mach_ensure_pair_in_regs(&ctx, &r_src1, &excl, &rn_lo, &rn_hi);

  /* Immediate-CMP fast path: if src2 is a u64 immediate, try the cmp-imm
   * form (`cmp.w Rn, #imm`) for each half — avoids loading the constant
   * into a scratch reg.  Probe encodability before allocating scratches:
   * `mach_ensure_in_reg` on a MACH_OP_IMM would unconditionally emit a
   * `movs Rscratch, #imm`, which is exactly the instruction we're trying
   * to avoid here. */
  thumb_opcode hi_imm_op = {0};
  thumb_opcode lo_imm_op = {0};
  if (r_src2.kind == MACH_OP_IMM)
  {
    const uint64_t imm = (uint64_t)r_src2.u.imm.val;
    const uint32_t imm_lo = (uint32_t)(imm & 0xffffffffu);
    const uint32_t imm_hi = (uint32_t)(imm >> 32);
    hi_imm_op = th_cmp_imm((uint32_t)rn_hi, imm_hi, FLAGS_BEHAVIOUR_SET, ENFORCE_ENCODING_NONE);
    lo_imm_op = th_cmp_imm((uint32_t)rn_lo, imm_lo, FLAGS_BEHAVIOUR_SET, ENFORCE_ENCODING_NONE);
  }

  /* Use cmp-imm for whichever halves fit; only allocate scratch
   * registers for halves that need them. */
  int hi_uses_imm = (r_src2.kind == MACH_OP_IMM && hi_imm_op.size);
  int lo_uses_imm = (r_src2.kind == MACH_OP_IMM && lo_imm_op.size);

  /* Comparing against literal zero is `(lo | hi) == 0`, one ORRS. */
  if (r_src2.kind == MACH_OP_IMM && (uint64_t)r_src2.u.imm.val == 0 &&
      thumb_try_orrs_zero64(rn_lo, rn_hi))
  {
    mach_release_all(&ctx);
    return;
  }

  int rm_lo = 0, rm_hi = 0;
  if (!lo_uses_imm && !hi_uses_imm)
  {
    /* Both halves are needed in registers — pair them (the immediate fast path
     * above is the only case that wants one half loaded and not the other). */
    mach_ensure_pair_in_regs(&ctx, &r_src2, &excl, &rm_lo, &rm_hi);
  }
  else
  {
    if (!lo_uses_imm)
    {
      MachineOperand s2_lo = mach_make_lo_half(&r_src2);
      s2_lo.btype = IROP_BTYPE_INT32;
      rm_lo = mach_ensure_in_reg(&ctx, &s2_lo, excl);
      if (thumb_is_hw_reg(rm_lo))
        excl |= (1u << (uint32_t)rm_lo);
    }
    if (!hi_uses_imm)
    {
      MachineOperand s2_hi = mach_make_hi_half(&r_src2);
      s2_hi.btype = IROP_BTYPE_INT32;
      rm_hi = mach_ensure_in_reg(&ctx, &s2_hi, excl);
    }
  }

  if (hi_uses_imm)
    ot_check(hi_imm_op);
  else
    ot_check(th_cmp_reg(0, (uint32_t)rn_hi, (uint32_t)rm_hi, FLAGS_BEHAVIOUR_SET, THUMB_SHIFT_DEFAULT,
                         ENFORCE_ENCODING_NONE));
  th_literal_pool_reserve_upcoming_bytes(6);
  ot_check(th_it(mapcc(TOK_EQ), 0x8));
  if (lo_uses_imm)
    ot_check(lo_imm_op);
  else
    ot_check(th_cmp_reg(0, (uint32_t)rn_lo, (uint32_t)rm_lo, FLAGS_BEHAVIOUR_SET, THUMB_SHIFT_DEFAULT,
                         ENFORCE_ENCODING_NONE));

  mach_release_all(&ctx);
}

/* tcc_gen_machine_subs_eq_select_01: emit
 *   SUBS dest, src1, #K
 *   IT NE
 *   MOVNE dest, #1
 * for the CMP src1,#K + SELECT(#1,#0,NE) / SELECT(#0,#1,EQ) peephole.
 * Returns 1 if emitted, 0 if the SUBS immediate didn't encode (caller falls back). */
ST_FUNC int tcc_gen_machine_subs_eq_select_01(MachineOperand src1, MachineOperand src2, MachineOperand dest)
{
  if (src2.kind != MACH_OP_IMM)
    return 0;
  if (src1.kind != MACH_OP_REG || src1.needs_deref || src1.u.reg.r0 < 0)
    return 0;
  if (dest.kind != MACH_OP_REG || dest.needs_deref || dest.u.reg.r0 < 0)
    return 0;

  uint32_t src_reg = (uint32_t)src1.u.reg.r0;
  uint32_t dst_reg = (uint32_t)dest.u.reg.r0;
  uint32_t Ku = (uint32_t)src2.u.imm.val;

  thumb_opcode subs = th_sub_imm(dst_reg, src_reg, Ku, FLAGS_BEHAVIOUR_SET, ENFORCE_ENCODING_NONE);
  if (subs.size == 0)
    return 0;

  /* Reserve so a literal-pool flush can't split the IT/MOV pair. */
  th_literal_pool_reserve_upcoming_bytes(10);
  ot_check(subs);
  ot_check(th_it(mapcc(TOK_NE), 0x8u));
  thumb_opcode movne = th_generic_mov_imm(dst_reg, 1);
  if (movne.size != 0) {
    ot_check(movne);
  } else {
    /* mov #1 always encodes on ARM Thumb-2, but be safe. */
    load_full_const((int)dst_reg, PREG_NONE, 1u, 0u);
  }
  return 1;
}

/* tcc_gen_machine_mla_mop: MachineOperand-based entry point for MLA.
 * dest = src1 * src2 + accum  (all operands are 32-bit)
 *
 * All four operands are loaded into hardware registers via mach_ensure_in_reg
 * before emitting a single MLA instruction.  No fallback path is needed
 * because mach_ensure_in_reg always returns a valid register.
 *
 * Note: th_mla(rd, rn, rm, ra) → rd = rn * rm + ra
 */
ST_FUNC void tcc_gen_machine_mla_mop(MachineOperand src1, MachineOperand src2, MachineOperand dest,
                                     MachineOperand accum)
{
  MachineCodegenContext ctx = {0};

  /* Pre-exclude registers directly referenced by REG operands so that scratch
   * allocations for other operands (e.g. immediates) cannot clobber them.
   * A dereferenced operand's r0 is its POINTER register — it must survive
   * until that operand's load is emitted, so it is excluded exactly like a
   * plain value register (ptr fuzz seed 59549: src2's spill reload picked the
   * deref-accumulator's pointer register as scratch, and the accumulator then
   * dereferenced the just-loaded multiplicand value → wild-address fault).
   * The pre-allocated DEST register must be excluded too: if a source load
   * grabs it as a saved scratch (push/pop), the restoring pop after the MLA
   * overwrites the just-computed result. */
  uint32_t live_regs = 0;
  if (src1.kind == MACH_OP_REG && src1.u.reg.r0 >= 0 && src1.u.reg.r0 < 16)
    live_regs |= (1u << (uint32_t)src1.u.reg.r0);
  if (src2.kind == MACH_OP_REG && src2.u.reg.r0 >= 0 && src2.u.reg.r0 < 16)
    live_regs |= (1u << (uint32_t)src2.u.reg.r0);
  if (accum.kind == MACH_OP_REG && accum.u.reg.r0 >= 0 && accum.u.reg.r0 < 16)
    live_regs |= (1u << (uint32_t)accum.u.reg.r0);
  if (dest.kind == MACH_OP_REG &&
      dest.u.reg.r0 != (int)PREG_REG_NONE && dest.u.reg.r0 >= 0 && dest.u.reg.r0 < 16)
    live_regs |= (1u << (uint32_t)dest.u.reg.r0);

  int src1_reg = mach_ensure_in_reg(&ctx, &src1, live_regs);
  uint32_t excl = live_regs;
  if (thumb_is_hw_reg(src1_reg))
    excl |= (1u << (uint32_t)src1_reg);

  int src2_reg = mach_ensure_in_reg(&ctx, &src2, excl);
  if (thumb_is_hw_reg(src2_reg))
    excl |= (1u << (uint32_t)src2_reg);

  int accum_reg = mach_ensure_in_reg(&ctx, &accum, excl);
  if (thumb_is_hw_reg(accum_reg))
    excl |= (1u << (uint32_t)accum_reg);

  int dest_reg = mach_get_dest_reg(&ctx, &dest, excl);

  /* th_mla(rd, rn, rm, ra): rd = rn * rm + ra */
  ot_check(th_mla((uint32_t)dest_reg, (uint32_t)src1_reg, (uint32_t)src2_reg, (uint32_t)accum_reg));

  mach_writeback_dest(&dest, dest_reg);
  mach_release_all(&ctx);
}

/* tcc_gen_machine_umull_mop: MachineOperand-based entry point for UMULL.
 * {dest_hi:dest_lo} = (uint32_t)src1 * (uint32_t)src2  (64-bit unsigned result)
 *
 * src1 and src2 are 32-bit inputs (is_64bit is cleared before loading).
 * dest must be a 64-bit pair; it is split via mach_make_lo/hi_half.
 * Each half is allocated independently via mach_get_dest_reg, with the
 * exclusion mask preventing rdlo==rdhi and preventing overlap with rn/rm.
 *
 * Note: th_umull(rdlo, rdhi, rn, rm) → {rdhi:rdlo} = rn * rm (unsigned)
 */
ST_FUNC void tcc_gen_machine_umull_mop(MachineOperand src1, MachineOperand src2, MachineOperand dest)
{
  MachineCodegenContext ctx = {0};

  /* UMULL takes 32-bit inputs — drop any 64-bit flag the src may carry. */
  MachineOperand s1 = src1;
  s1.is_64bit = false;
  MachineOperand s2 = src2;
  s2.is_64bit = false;

  /* Pre-exclude the pre-allocated dest pair: a saved-scratch (push/pop) on a
   * dest register would have its restoring pop clobber the result. */
  uint32_t dest_excl = 0;
  if (dest.kind == MACH_OP_REG && !dest.needs_deref)
  {
    if (dest.u.reg.r0 != (int)PREG_REG_NONE)
      dest_excl |= (1u << (uint32_t)dest.u.reg.r0);
    if (dest.is_64bit && dest.u.reg.r1 >= 0 && dest.u.reg.r1 != (int)PREG_REG_NONE)
      dest_excl |= (1u << (uint32_t)dest.u.reg.r1);
  }

  int rn = mach_ensure_in_reg(&ctx, &s1, dest_excl);
  uint32_t excl = dest_excl | (thumb_is_hw_reg(rn) ? (1u << (uint32_t)rn) : 0u);

  int rm = mach_ensure_in_reg(&ctx, &s2, excl);
  if (thumb_is_hw_reg(rm))
    excl |= (1u << (uint32_t)rm);

  /* Split 64-bit destination into lo (bits [31:0]) and hi (bits [63:32]). */
  MachineOperand dst_lo = mach_make_lo_half(&dest);
  MachineOperand dst_hi = mach_make_hi_half(&dest);
  dst_lo.btype = IROP_BTYPE_INT32;
  dst_hi.btype = IROP_BTYPE_INT32;

  int rd_lo = mach_get_dest_reg(&ctx, &dst_lo, excl);
  if (thumb_is_hw_reg(rd_lo))
    excl |= (1u << (uint32_t)rd_lo);
  int rd_hi = mach_get_dest_reg(&ctx, &dst_hi, excl);

  /* th_umull(rdlo, rdhi, rn, rm): {rdhi:rdlo} = rn * rm */
  ot_check(th_umull((uint32_t)rd_lo, (uint32_t)rd_hi, (uint32_t)rn, (uint32_t)rm));

  mach_writeback_dest(&dst_lo, rd_lo);
  mach_writeback_dest(&dst_hi, rd_hi);
  mach_release_all(&ctx);
}

/* tcc_gen_machine_smull_mop: MachineOperand-based entry point for SMULL.
 * {dest_hi:dest_lo} = (int32_t)src1 * (int32_t)src2  (64-bit signed result).
 * Mirrors umull_mop but emits th_smull. */
ST_FUNC void tcc_gen_machine_smull_mop(MachineOperand src1, MachineOperand src2, MachineOperand dest)
{
  MachineCodegenContext ctx = {0};

  MachineOperand s1 = src1;
  s1.is_64bit = false;
  MachineOperand s2 = src2;
  s2.is_64bit = false;

  /* Pre-exclude the pre-allocated dest pair (see umull_mop). */
  uint32_t dest_excl = 0;
  if (dest.kind == MACH_OP_REG && !dest.needs_deref)
  {
    if (dest.u.reg.r0 != (int)PREG_REG_NONE)
      dest_excl |= (1u << (uint32_t)dest.u.reg.r0);
    if (dest.is_64bit && dest.u.reg.r1 >= 0 && dest.u.reg.r1 != (int)PREG_REG_NONE)
      dest_excl |= (1u << (uint32_t)dest.u.reg.r1);
  }

  int rn = mach_ensure_in_reg(&ctx, &s1, dest_excl);
  uint32_t excl = dest_excl | (thumb_is_hw_reg(rn) ? (1u << (uint32_t)rn) : 0u);

  int rm = mach_ensure_in_reg(&ctx, &s2, excl);
  if (thumb_is_hw_reg(rm))
    excl |= (1u << (uint32_t)rm);

  MachineOperand dst_lo = mach_make_lo_half(&dest);
  MachineOperand dst_hi = mach_make_hi_half(&dest);
  dst_lo.btype = IROP_BTYPE_INT32;
  dst_hi.btype = IROP_BTYPE_INT32;

  int rd_lo = mach_get_dest_reg(&ctx, &dst_lo, excl);
  if (thumb_is_hw_reg(rd_lo))
    excl |= (1u << (uint32_t)rd_lo);
  int rd_hi = mach_get_dest_reg(&ctx, &dst_hi, excl);

  /* th_smull(rdlo, rdhi, rn, rm): {rdhi:rdlo} = (signed)rn * (signed)rm */
  ot_check(th_smull((uint32_t)rd_lo, (uint32_t)rd_hi, (uint32_t)rn, (uint32_t)rm));

  mach_writeback_dest(&dst_lo, rd_lo);
  mach_writeback_dest(&dst_hi, rd_hi);
  mach_release_all(&ctx);
}

/* tcc_gen_machine_mlal_accum_mop: emit SMLAL/UMLAL for
 *   dest = accum + (int32/uint32)src1 * (int32/uint32)src2
 *
 * This narrow helper is used by codegen peepholes after register allocation.
 * It only handles the cheap in-place accumulate form, where the ADD destination
 * already holds the accumulator pair.  Other forms fall back to SMULL/UMULL
 * plus the normal 64-bit ADD so we do not risk clobbering multiply sources. */
ST_FUNC int tcc_gen_machine_mlal_accum_mop(MachineOperand src1, MachineOperand src2, MachineOperand accum,
                                           MachineOperand dest, int is_signed)
{
  if (!dest.is_64bit || !accum.is_64bit)
    return 0;
  if (dest.kind != MACH_OP_REG || accum.kind != MACH_OP_REG)
    return 0;
  if (dest.needs_deref || accum.needs_deref)
    return 0;
  if (dest.u.reg.r0 != accum.u.reg.r0 || dest.u.reg.r1 != accum.u.reg.r1)
    return 0;

  int rd_lo = dest.u.reg.r0;
  int rd_hi = dest.u.reg.r1;
  /* Inline the hw-reg range checks rather than calling thumb_is_hw_reg(): the
   * self-host cross drops the argument move into the inlined helper here and
   * tests a stale register (the dest pointer) instead of rd_lo/rd_hi, so the
   * native compiler wrongly bails out of every in-place 64-bit MLA with
   * "unable to lower 64-bit MLA".  Direct comparisons on rd_lo/rd_hi (as the
   * adjacent rd_lo == rd_hi check already does) compile correctly. */
  if (rd_lo < 0 || rd_lo > 15 || rd_hi < 0 || rd_hi > 15 || rd_lo == rd_hi)
    return 0;

  MachineCodegenContext ctx = {0};
  MachineOperand s1 = src1;
  s1.is_64bit = false;
  MachineOperand s2 = src2;
  s2.is_64bit = false;

  uint32_t excl = (1u << (uint32_t)rd_lo) | (1u << (uint32_t)rd_hi);
  /* Pre-exclude both sources' registers (a deref operand's r0 is its pointer
   * register) so ensuring one source cannot grab the other's register as a
   * spill-reload scratch — same clobber class as tcc_gen_machine_mla_mop. */
  if (s1.kind == MACH_OP_REG && s1.u.reg.r0 >= 0 && s1.u.reg.r0 < 16)
    excl |= (1u << (uint32_t)s1.u.reg.r0);
  if (s2.kind == MACH_OP_REG && s2.u.reg.r0 >= 0 && s2.u.reg.r0 < 16)
    excl |= (1u << (uint32_t)s2.u.reg.r0);
  int rn = mach_ensure_in_reg(&ctx, &s1, excl);
  if (thumb_is_hw_reg(rn))
    excl |= (1u << (uint32_t)rn);

  int rm = mach_ensure_in_reg(&ctx, &s2, excl);
  if (thumb_is_hw_reg(rm))
    excl |= (1u << (uint32_t)rm);

  if (is_signed)
    ot_check(th_smlal((uint32_t)rd_lo, (uint32_t)rd_hi, (uint32_t)rn, (uint32_t)rm));
  else
    ot_check(th_umlal((uint32_t)rd_lo, (uint32_t)rd_hi, (uint32_t)rn, (uint32_t)rm));

  MachineOperand dst_lo = mach_make_lo_half(&dest);
  MachineOperand dst_hi = mach_make_hi_half(&dest);
  dst_lo.btype = IROP_BTYPE_INT32;
  dst_hi.btype = IROP_BTYPE_INT32;
  mach_writeback_dest(&dst_lo, rd_lo);
  mach_writeback_dest(&dst_hi, rd_hi);
  mach_release_all(&ctx);
  return 1;
}

/* The plain register a 32-bit operand lives in, or -1 (spilled, immediate,
 * register-indirect, ...). */
static int mach_plain_reg(const MachineOperand *op)
{
  if (op->kind == MACH_OP_REG && !op->needs_deref && op->u.reg.r0 >= 0 && op->u.reg.r0 < 16)
    return op->u.reg.r0;
  return -1;
}

/* Put a 32-bit operand's value in register `target`: a move, or a direct load
 * for a spill slot or an immediate. */
static void mach_move_into(MachineCodegenContext *ctx, int target, const MachineOperand *op, uint32_t excl)
{
  int r = mach_plain_reg(op);
  if (r == target)
    return;
  if (r < 0 && op->kind == MACH_OP_SPILL && !op->needs_deref)
  {
    mach_load_slot(target, op);
    return;
  }
  if (r < 0 && op->kind == MACH_OP_IMM && !op->needs_deref)
  {
    tcc_machine_load_constant(target, PREG_REG_NONE, op->u.imm.val, 0, NULL);
    return;
  }
  if (r < 0)
    r = mach_ensure_in_reg(ctx, op, excl | (1u << (uint32_t)target));
  ot_check_mov_reg((uint32_t)target, (uint32_t)r, flags_safe(), THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE, false);
}

/* tcc_machine_has_umaal: UMAAL is in the DSP extension (ARMv7E-M, ARMv8-M
 * Mainline with DSP), which v8-M Baseline and plain v7-M lack. */
ST_FUNC int tcc_machine_has_umaal(void)
{
  return arm_target_dependent.feat.t32 && arm_target_dependent.feat.dsp;
}

/* tcc_gen_machine_umaal_mop: TCCIR_OP_UMAAL,
 *   dest = (uint32)src1 * (uint32)src2 + lo32(accum) + hi32(accum)
 * UMAAL accumulates in place ({RdHi:RdLo} = Rn * Rm + RdLo + RdHi), so the
 * accumulator's words go to the destination pair first -- no move at all when
 * the allocator put them there.  The pair the moves overwrite must not hold a
 * multiplicand; a destination that overlaps one is replaced by a scratch pair
 * and written back after. */
ST_FUNC void tcc_gen_machine_umaal_mop(MachineOperand src1, MachineOperand src2, MachineOperand accum,
                                       MachineOperand dest)
{
  MachineCodegenContext ctx = {0};
  MachineOperand s1 = src1;
  s1.is_64bit = false;
  MachineOperand s2 = src2;
  s2.is_64bit = false;
  MachineOperand acc_lo = mach_make_lo_half(&accum);
  MachineOperand acc_hi = mach_make_hi_half(&accum);
  acc_lo.btype = IROP_BTYPE_INT32;
  acc_hi.btype = IROP_BTYPE_INT32;
  MachineOperand dst_lo = mach_make_lo_half(&dest);
  MachineOperand dst_hi = mach_make_hi_half(&dest);
  dst_lo.btype = IROP_BTYPE_INT32;
  dst_hi.btype = IROP_BTYPE_INT32;

  const int al = mach_plain_reg(&acc_lo), ah = mach_plain_reg(&acc_hi);
  const int dl = mach_plain_reg(&dst_lo), dh = mach_plain_reg(&dst_hi);
  uint32_t excl = 0;
  if (al >= 0) excl |= 1u << (uint32_t)al;
  if (ah >= 0) excl |= 1u << (uint32_t)ah;
  if (dl >= 0) excl |= 1u << (uint32_t)dl;
  if (dh >= 0) excl |= 1u << (uint32_t)dh;
  if (mach_plain_reg(&s1) >= 0) excl |= 1u << (uint32_t)mach_plain_reg(&s1);
  if (mach_plain_reg(&s2) >= 0) excl |= 1u << (uint32_t)mach_plain_reg(&s2);

  int rn = mach_ensure_in_reg(&ctx, &s1, excl);
  if (thumb_is_hw_reg(rn))
    excl |= 1u << (uint32_t)rn;
  int rm = mach_ensure_in_reg(&ctx, &s2, excl);
  if (thumb_is_hw_reg(rm))
    excl |= 1u << (uint32_t)rm;

  int rd_lo = dl, rd_hi = dh;
  if (rd_lo < 0 || rd_lo == rn || rd_lo == rm || rd_lo == rd_hi)
  {
    rd_lo = mach_alloc_scratch(&ctx, excl);
    excl |= 1u << (uint32_t)rd_lo;
  }
  if (rd_hi < 0 || rd_hi == rn || rd_hi == rm || rd_hi == rd_lo)
  {
    rd_hi = mach_alloc_scratch(&ctx, excl);
    excl |= 1u << (uint32_t)rd_hi;
  }

  /* The two addends are summed symmetrically, so either may go in either
   * register: keep whichever is already in place, which also means neither
   * move can overwrite the other addend before it is read. */
  if (al == rd_hi || ah == rd_lo)
  {
    mach_move_into(&ctx, rd_lo, &acc_hi, excl);
    mach_move_into(&ctx, rd_hi, &acc_lo, excl);
  }
  else
  {
    mach_move_into(&ctx, rd_lo, &acc_lo, excl);
    mach_move_into(&ctx, rd_hi, &acc_hi, excl);
  }

  ot_check(th_umaal((uint32_t)rd_lo, (uint32_t)rd_hi, (uint32_t)rn, (uint32_t)rm));

  mach_writeback_dest(&dst_lo, rd_lo);
  mach_writeback_dest(&dst_hi, rd_hi);
  mach_release_all(&ctx);
}

/* tcc_gen_machine_pack64_mop: lower TCCIR_OP_PACK64 by emitting two
 * 32-bit assigns into the dest's halves.  src_lo and src_hi are u32
 * operands; dest is a u64 register pair / spill / param slot.
 *
 * The two sub-assigns delegate to tcc_gen_machine_assign_mop, so they
 * benefit from its existing handling of every dest kind (REG/SPILL/...).
 * Often regalloc has already aligned the registers (e.g. dest.r0 = src_lo
 * register), in which case the sub-assigns degrade to a no-op MOV that
 * the encoder can skip. */
ST_FUNC void tcc_gen_machine_pack64_mop(MachineOperand src_lo, MachineOperand src_hi, MachineOperand dest)
{
  if (!dest.is_64bit)
  {
    tcc_ice("tcc_gen_machine_pack64_mop: dest not 64-bit");
    return;
  }
  MachineOperand dst_lo = mach_make_lo_half(&dest);
  MachineOperand dst_hi = mach_make_hi_half(&dest);
  dst_lo.btype = IROP_BTYPE_INT32;
  dst_hi.btype = IROP_BTYPE_INT32;

  /* Detect register-swap aliasing: dst_lo == src_hi AND dst_hi == src_lo.
   * Neither write order can preserve both source values; we must stage one
   * side through a scratch register. */
  int swap_alias = 0;
  if (src_lo.kind == MACH_OP_REG && !src_lo.needs_deref &&
      src_hi.kind == MACH_OP_REG && !src_hi.needs_deref &&
      dst_lo.kind == MACH_OP_REG && !dst_lo.needs_deref &&
      dst_hi.kind == MACH_OP_REG && !dst_hi.needs_deref &&
      src_hi.u.reg.r0 == dst_lo.u.reg.r0 && src_lo.u.reg.r0 == dst_hi.u.reg.r0 &&
      src_lo.u.reg.r0 != src_hi.u.reg.r0)
    swap_alias = 1;

  if (swap_alias)
  {
    /* Save src_lo to a scratch before overwriting it via dst_hi. */
    uint32_t excl = (1u << (uint32_t)dst_lo.u.reg.r0) | (1u << (uint32_t)dst_hi.u.reg.r0);
    ScratchRegAlloc scratch = get_scratch_reg_with_save(excl);
    ot_check_mov_reg((uint32_t)scratch.reg, (uint32_t)src_lo.u.reg.r0, flags_safe(),
                     THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE, false);
    /* Now dst_hi = src_hi (still live), then dst_lo = scratch (=old src_lo). */
    tcc_gen_machine_assign_mop(src_hi, dst_hi, TCCIR_OP_ASSIGN);
    MachineOperand scratch_op = src_lo;
    scratch_op.u.reg.r0 = scratch.reg;
    tcc_gen_machine_assign_mop(scratch_op, dst_lo, TCCIR_OP_ASSIGN);
    restore_scratch_reg(&scratch);
  }
  else if (src_hi.kind == MACH_OP_REG && !src_hi.needs_deref &&
           dst_lo.kind == MACH_OP_REG && !dst_lo.needs_deref &&
           src_hi.u.reg.r0 == dst_lo.u.reg.r0)
  {
    /* dst_lo == src_hi register: write hi first to free src_hi's slot. */
    tcc_gen_machine_assign_mop(src_hi, dst_hi, TCCIR_OP_ASSIGN);
    tcc_gen_machine_assign_mop(src_lo, dst_lo, TCCIR_OP_ASSIGN);
  }
  else
  {
    tcc_gen_machine_assign_mop(src_lo, dst_lo, TCCIR_OP_ASSIGN);
    tcc_gen_machine_assign_mop(src_hi, dst_hi, TCCIR_OP_ASSIGN);
  }
}

/* tcc_gen_machine_assign_mop: MachineOperand-based entry point for simple
 * 32-bit value assignment.  Called from ir/codegen.c instead of
 * tcc_gen_machine_assign_op when:
 *   - Neither dest nor src requires a 64-bit or complex register pair, AND
 *   - The function does not use a static chain.
 *
 * Handles all destination kinds: MACH_OP_REG (direct), MACH_OP_SPILL
 * (via mach_get_dest_reg + mach_writeback_dest → tcc_machine_store_spill_slot),
 * and MACH_OP_PARAM_STACK (via mach_writeback_dest → tcc_machine_store_param_slot).
 *
 * Strategy: load src directly into dest_reg; use mach_ensure_in_reg only
 * as a fallback for unhandled source kinds.
 */
ST_FUNC void tcc_gen_machine_assign_mop(MachineOperand src, MachineOperand dest, TccIrOp op)
{
  tcc_gen_machine_assign_mop_ex(src, dest, op, 0);
}

ST_FUNC void tcc_gen_machine_assign_mop_ex(MachineOperand src, MachineOperand dest, TccIrOp op, uint32_t zh)
{
  (void)op;
  /* Only the top-level 64-bit destination consults `zh`; the per-half recursive
   * calls below have already split the pair and pass 0. */
  const bool dst_lo_dead = (zh & ZH64_D_LO) != 0;
  const bool dst_hi_dead = (zh & ZH64_D_HI) != 0;

  /* 64-bit pair assignment: handle each 32-bit half independently.
   * mach_make_lo/hi_half splits MACH_OP_REG (r0:r1), MACH_OP_SPILL (offset,
   * offset+4) and MACH_OP_IMM into separate 32-bit MachineOperands.
   * We then recursively assign each half (is_64bit=false prevents recursion).
   *
   * Special care: when src has needs_deref=true, the operand is a POINTER
   * to a 64-bit value. The address is in one register (or spill slot);
   * splitting registers via mach_make_hi_half would create a bogus base
   * address. Instead, load both halves from [base+0] and [base+4].
   *
   * Exception: PARAM_STACK and CHAIN_REL with needs_deref mean the 64-bit
   * value IS directly at [fp+offset] / [chain+offset], not a pointer to
   * follow (mach_op_64_names_memory). Clear needs_deref so it falls through
   * to the normal lo/hi split path. */
  if (src.needs_deref && src.is_64bit && mach_op_64_names_memory(&src))
    src.needs_deref = false;

  if (dest.is_64bit)
  {
    MachineOperand dst_lo = mach_make_lo_half(&dest);
    MachineOperand dst_hi = mach_make_hi_half(&dest);
    dst_lo.btype = IROP_BTYPE_INT32;
    dst_hi.btype = IROP_BTYPE_INT32;

    if (src.needs_deref && src.is_64bit)
    {
      /* Source is a 64-bit lvalue: a pointer to a 64-bit value (e.g. R0
       * holding address of an unsigned long long). Load both 32-bit halves
       * from [base+0] and [base+4] using the same base address register. */
      MachineCodegenContext mctx = {0};

      /* Strip deref to get the raw address into a register. */
      MachineOperand addr = src;
      addr.needs_deref = false;
      addr.is_64bit = false;
      addr.btype = IROP_BTYPE_INT32;
      int base_reg = mach_ensure_in_reg(&mctx, &addr, 0);
      uint32_t excl = (1u << (uint32_t)base_reg);

      /* Determine destination registers for lo and hi halves. */
      int lo_reg = mach_get_dest_reg(&mctx, &dst_lo, excl);
      if (thumb_is_hw_reg(lo_reg))
        excl |= (1u << (uint32_t)lo_reg);
      int hi_reg = mach_get_dest_reg(&mctx, &dst_hi, excl);

      /* Load [base+0] → lo, [base+4] → hi (32-bit loads). */
      load_from_base(lo_reg, PREG_REG_NONE, IROP_BTYPE_INT32, 0, 0, 0, (uint32_t)base_reg);
      load_from_base(hi_reg, PREG_REG_NONE, IROP_BTYPE_INT32, 0, 4, 0, (uint32_t)base_reg);

      mach_writeback_dest(&dst_lo, lo_reg);
      mach_writeback_dest(&dst_hi, hi_reg);
      mach_release_all(&mctx);
      return;
    }

    if (src.is_64bit)
    {
      /* Whole 64-bit local → register pair: one LDRD instead of the two LDRs
       * the per-half recursion below would emit. */
      /* LDRD moves both halves at once, so it is only the right choice while
       * both are wanted. */
      if (!dst_lo_dead && !dst_hi_dead && src.kind == MACH_OP_SPILL && !src.needs_deref &&
          (src.u.spill.offset & 3) == 0 && dest.kind == MACH_OP_REG && !dest.needs_deref &&
          thumb_is_hw_reg(dest.u.reg.r0) && thumb_is_hw_reg(dest.u.reg.r1) &&
          dest.u.reg.r0 != dest.u.reg.r1 &&
          tcc_gen_machine_try_ldrd_spill(dest.u.reg.r0, src.u.spill.offset, dest.u.reg.r1,
                                         src.u.spill.offset + 4))
        return;

      MachineOperand src_lo = mach_make_lo_half(&src);
      MachineOperand src_hi = mach_make_hi_half(&src);
      src_lo.btype = IROP_BTYPE_INT32;
      src_hi.btype = IROP_BTYPE_INT32;
      if (!dst_lo_dead)
        tcc_gen_machine_assign_mop(src_lo, dst_lo, op);
      if (!dst_hi_dead)
        tcc_gen_machine_assign_mop(src_hi, dst_hi, op);
    }
    else
    {
      /* 32-bit source into 64-bit dest: assign lo half, zero the high half.
       * That zero is the single commonest instruction in soft float -- every
       * `(uint64_t)x` before a shift-and-OR pays one -- so skipping it when no
       * consumer reads the half is the point of the whole annotation. */
      MachineOperand zero = {0};
      zero.kind = MACH_OP_IMM;
      zero.u.imm.val = 0;
      zero.btype = IROP_BTYPE_INT32;
      if (!dst_lo_dead)
        tcc_gen_machine_assign_mop(src, dst_lo, op);
      if (!dst_hi_dead)
        tcc_gen_machine_assign_mop(zero, dst_hi, op);
    }
    return;
  }

  if (src.is_64bit && !dest.is_64bit)
  {
    /* Truncation: extract and assign only the low half of the 64-bit source. */
    MachineOperand src_lo = mach_make_lo_half(&src);
    src_lo.btype = IROP_BTYPE_INT32;
    tcc_gen_machine_assign_mop(src_lo, dest, op);
    return;
  }

  /* Single-precision float in a VFP register (hard-float): vmov between s-regs,
   * or bridge through a GPR for the GPR/imm/spill/memory endpoint. */
  if (src.kind == MACH_OP_VFP_REG || dest.kind == MACH_OP_VFP_REG)
  {
    if (src.kind == MACH_OP_VFP_REG && dest.kind == MACH_OP_VFP_REG)
    {
      if (src.u.reg.r0 != dest.u.reg.r0)
        ot_check(th_vmov_register((uint16_t)dest.u.reg.r0, (uint16_t)src.u.reg.r0, 0));
      return;
    }
    MachineCodegenContext vctx = {0};
    if (dest.kind == MACH_OP_VFP_REG)
    {
      int gpr = mach_ensure_in_reg(&vctx, &src, 0); /* GPR/imm/spill/... -> GPR */
      ot_check(th_vmov_gp_sp((uint16_t)gpr, (uint16_t)dest.u.reg.r0, 0)); /* sN = gpr */
    }
    else
    {
      int gpr = mach_ensure_in_reg(&vctx, &src, 0); /* VFP src bridges to a GPR */
      mach_writeback_dest(&dest, gpr);
    }
    mach_release_all(&vctx);
    return;
  }

  /* A copy between two values the allocator spilled to the SAME slot, read
   * and written at the same width, leaves memory as it was.  The store half
   * was already dropped by the encoder (strldr_cache_str_is_redundant), which
   * left the load behind as a dead `ldr rX,[sp,#N]`: a phi copy whose two
   * sides share a slot.  Knob: TCC_DISABLE_PASS=codegen:slot_self_copy. */
  if (!slot_self_copy_off && src.kind == MACH_OP_SPILL && dest.kind == MACH_OP_SPILL && !src.needs_deref &&
      !dest.needs_deref && src.u.spill.offset == dest.u.spill.offset &&
      mach_slot_load_width(&src) == mach_slot_store_width(&dest))
    return;

  MachineCodegenContext mctx = {0};

  /* --- Fast path: source is already in a register (no dereference) ---
   * Write it directly to the destination via mach_writeback_dest without
   * allocating any scratch.  This covers REG→REG (MOV or NOP) and
   * REG→SPILL/PARAM_STACK (direct store from src register). */
  if (src.kind == MACH_OP_REG && !src.needs_deref)
  {
    mach_writeback_dest(&dest, src.u.reg.r0);
    return;
  }

  /* --- Determine destination register ---
   * For REG destinations, reuse the pre-allocated register (0 scratch).
   * For SPILL/PARAM_STACK/REG(deref) destinations, allocate a scratch. */
  int dest_reg;
  bool need_writeback;
  if (dest.kind == MACH_OP_REG && !dest.needs_deref && dest.u.reg.r0 != (int)PREG_REG_NONE)
  {
    dest_reg = dest.u.reg.r0;
    need_writeback = false;
  }
  else
  {
    dest_reg = mach_get_dest_reg(&mctx, &dest, 0);
    need_writeback = true;
  }

  /* --- Load source value directly into dest_reg --- */
  switch (src.kind)
  {
  case MACH_OP_REG:
    /* Only the needs_deref case reaches here (non-deref handled above).
     * Load from [src_reg] directly into dest_reg. */
    load_from_base(dest_reg, PREG_REG_NONE, src.btype, (int)src.is_unsigned, 0, 0, (uint32_t)src.u.reg.r0);
    break;

  case MACH_OP_IMM:
    tcc_machine_load_constant(dest_reg, PREG_REG_NONE, src.u.imm.val, 0, NULL);
    /* `T <-- #0x40000000***DEREF***`: `switch (REG)` assigns the register's
     * value to the controlling temp, not its address. */
    if (src.needs_deref)
      load_from_base(dest_reg, PREG_REG_NONE, src.btype, (int)src.is_unsigned, 0, 0, (uint32_t)dest_reg);
    break;

  case MACH_OP_SPILL:
    mach_load_slot(dest_reg, &src);
    if (src.needs_deref)
    {
      /* Double indirection: dest_reg now holds a pointer; dereference it. */
      load_from_base(dest_reg, PREG_REG_NONE, src.btype, (int)src.is_unsigned, 0, 0, (uint32_t)dest_reg);
    }
    break;

  case MACH_OP_SYMBOL:
  {
    Sym *sym = src.u.sym.sym ? validate_sym_for_reloc(src.u.sym.sym) : NULL;
    if (!src.needs_deref)
    {
      tcc_machine_load_constant(dest_reg, PREG_REG_NONE, src.u.sym.addend, 0, sym);
    }
    else
    {
      const int32_t addend = src.u.sym.addend;
      const int abs_off = addend < 0 ? (int)(-addend) : (int)addend;
      const int sign = addend < 0 ? 1 : 0;
      /* Prefer a base register OTHER than dest_reg: `ldr rD,[rD,#off]`
       * overwrites the address with the loaded value, so imm_cache loses it
       * and the store that follows a read-modify-write has to re-load the
       * literal.  A separate base keeps the address cached (and an imm_cache
       * hit removes this materialization entirely).  Only when no register is
       * free do we fall back to routing through dest_reg - taking a
       * push/pop there would cost more than the reload it saves. */
      ScratchRegAlloc base;
      if (try_scratch_reg_for_sym_addr(sym, 0, (1u << (uint32_t)dest_reg), &base))
      {
        tcc_machine_load_constant(base.reg, PREG_REG_NONE, 0, 0, sym);
        load_from_base(dest_reg, PREG_REG_NONE, src.btype, (int)src.is_unsigned, abs_off, sign,
                       (uint32_t)base.reg);
        restore_scratch_reg(&base);
      }
      else
      {
        tcc_machine_load_constant(dest_reg, PREG_REG_NONE, 0, 0, sym);
        load_from_base(dest_reg, PREG_REG_NONE, src.btype, (int)src.is_unsigned, abs_off, sign,
                       (uint32_t)dest_reg);
      }
    }
    break;
  }

  case MACH_OP_FRAME_ADDR:
    tcc_machine_addr_of_stack_slot(dest_reg, src.u.frame.offset, 0);
    break;

  case MACH_OP_PARAM_STACK:
  {
    const int adjusted = param_frame_offset(src.u.param.offset);
    const int base_reg = tcc_state->need_frame_pointer ? R_FP : R_SP;
    const int sign = (adjusted < 0);
    const int abs_off = sign ? -adjusted : adjusted;
    load_from_base(dest_reg, PREG_REG_NONE, src.btype, (int)src.is_unsigned, abs_off, sign, (uint32_t)base_reg);
    break;
  }

  default:
  {
    /* Fallback: generic mach_ensure_in_reg + MOV. */
    uint32_t excl = thumb_is_hw_reg(dest_reg) ? (1u << (uint32_t)dest_reg) : 0;
    int src_reg = mach_ensure_in_reg(&mctx, &src, excl);
    if (src_reg != dest_reg)
      ot_check_mov_reg((uint32_t)dest_reg, (uint32_t)src_reg, flags_safe(), THUMB_SHIFT_DEFAULT,
                       ENFORCE_ENCODING_NONE, false);
    break;
  }
  }

  if (need_writeback)
    mach_writeback_dest(&dest, dest_reg);

  mach_release_all(&mctx);
}

/* tcc_gen_machine_setif_mop: MachineOperand-based entry point for SETIF.
 * src must be MACH_OP_IMM carrying the raw condition code in u.imm.val.
 *
 * 32-bit dest:
 *   ITE <cond>
 *   MOV dest, #1   (T: cond met)
 *   MOV dest, #0   (E: cond not met)
 *
 * 64-bit dest pair (e.g. long long result = (x > y)):
 *   The boolean result 0 or 1 fits in 32 bits, so hi word is always 0.
 *   ITE <cond>
 *   MOV dest_lo, #1
 *   MOV dest_lo, #0
 *   MOV dest_hi, #0   (unconditional, outside IT block — hi is always 0)
 *
 * Inner MOVs use NOT_IMPORTANT for flags: SETIF is the consumer of the CMP
 * flags; once the ITE captures the condition, no subsequent code in this
 * lowering depends on CMP's flag state, so the 16-bit T1 encoding (which
 * implicitly sets flags) is safe.  This shrinks each conditional MOV from
 * 4 bytes (mov.w) to 2 bytes (movs).
 */
ST_FUNC void tcc_gen_machine_setif_mop(MachineOperand src, MachineOperand dest, TccIrOp op)
{
  (void)op;
  MachineCodegenContext mctx = {0};

  const int cond = mapcc((int)src.u.imm.val);
  /* ITE mask: 2nd instruction has opposite condition.
   * mask[3] = 1 if it should be the 'else' bit (opposite of cond[0]).
   * For the T-then-E pattern, mask = ((!cond[0]) << 3) | 0x4. */
  const uint16_t ite_mask = (uint16_t)(((cond ^ 1) & 1) << 3) | 0x4u;

  if (dest.is_64bit)
  {
    /* Split 64-bit destination into two independent 32-bit halves. */
    MachineOperand dst_lo = mach_make_lo_half(&dest);
    MachineOperand dst_hi = mach_make_hi_half(&dest);
    dst_lo.btype = IROP_BTYPE_INT32;
    dst_hi.btype = IROP_BTYPE_INT32;

    int lo_reg = mach_get_dest_reg(&mctx, &dst_lo, 0);
    uint32_t excl = thumb_is_hw_reg(lo_reg) ? (1u << (uint32_t)lo_reg) : 0u;
    int hi_reg = mach_get_dest_reg(&mctx, &dst_hi, excl);

    /* Emit ITE sequence for lo word.  Reserve the WHOLE atomic ITE+movs block so
     * a literal-pool flush never lands between the IT and its conditioned movs.
     * A high register (R8-R12) dest forces the 4-byte mov.w (T2) encoding, so the
     * worst case is ITE(2) + 3*mov.w(4) = 14 bytes — NOT 6 (which only covers the
     * 2-byte movs of a low-reg dest).  Under-reserving split the ITE and ran the
     * fall-through into the literal pool (seed 89 O1 HardFault). */
    th_literal_pool_reserve_upcoming_bytes(14);
    ot_check(th_it(cond, ite_mask)); /* ITE <cond> — two conditioned instructions */
    ot_check(th_mov_imm(lo_reg, 1, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE));
    ot_check(th_mov_imm(lo_reg, 0, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE));
    /* Hi word is always 0 — boolean result never exceeds 1 (i.e. fits in 32-bit lo). */
    ot_check(th_mov_imm(hi_reg, 0, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE));

    mach_writeback_dest(&dst_lo, lo_reg);
    mach_writeback_dest(&dst_hi, hi_reg);
  }
  else
  {
    int dest_reg = mach_get_dest_reg(&mctx, &dest, 0);

    /* Reserve the whole ITE+2-movs block: a high-register dest (R8-R12) uses the
     * 4-byte mov.w (T2) encoding, so the worst case is ITE(2) + 2*mov.w(4) = 10
     * bytes, not 6.  Under-reserving let a literal-pool flush split the ITE and
     * run the fall-through into the pool (seed 89 O1 HardFault). */
    th_literal_pool_reserve_upcoming_bytes(10);
    ot_check(th_it(cond, ite_mask)); /* ITE <cond> — two conditioned instructions */
    ot_check(th_mov_imm(dest_reg, 1, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE));
    ot_check(th_mov_imm(dest_reg, 0, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE));

    mach_writeback_dest(&dest, dest_reg);
  }

  mach_release_all(&mctx);
}

/* tcc_gen_machine_bool_mop: MachineOperand-based entry point for
 * BOOL_OR / BOOL_AND.  Called from ir/codegen.c for simple 32-bit
 * non-complex boolean operations.
 *
 * BOOL_OR:   ORRS dest, src1, src2   (sets Z flag)
 *            MOV  dest, #0           (flag-preserving)
 *            IT   NE
 *            MOV  dest, #1
 *
 * BOOL_AND:  CMP  src1, #0
 *            IT   NE
 *            CMP  src2, #0           (only if src1 != 0)
 *            MOV  dest, #0           (flag-preserving)
 *            IT   NE
 *            MOV  dest, #1
 */
/* Flags for a MOV #imm8 that is the conditioned instruction of an IT block:
 * there the 16-bit encoding sets no flags, so a low register need not pay for
 * the flag-preserving MOV.W. */
static thumb_flags_behaviour it_mov_flags(int reg)
{
  return reg < 8 ? FLAGS_BEHAVIOUR_NOT_IMPORTANT : flags_safe();
}

ST_FUNC void tcc_gen_machine_bool_mop(MachineOperand src1, MachineOperand src2, MachineOperand dest, TccIrOp op)
{
  MachineCodegenContext mctx = {0};

  /* 64-bit operands: reduce each to a 32-bit "is non-zero" value by OR-ing
   * its low and high halves, then apply the standard 32-bit BOOL logic. */
  if (src1.is_64bit || src2.is_64bit)
  {
    uint32_t excl = 0;
    int r1, r2;

    if (src1.is_64bit)
    {
      MachineOperand lo1 = mach_make_lo_half(&src1);
      lo1.btype = IROP_BTYPE_INT32;
      MachineOperand hi1 = mach_make_hi_half(&src1);
      hi1.btype = IROP_BTYPE_INT32;
      r1 = mach_ensure_in_reg(&mctx, &lo1, excl);
      excl |= thumb_is_hw_reg(r1) ? (1u << (uint32_t)r1) : 0;
      int hi1_reg = mach_ensure_in_reg(&mctx, &hi1, excl);
      excl |= thumb_is_hw_reg(hi1_reg) ? (1u << (uint32_t)hi1_reg) : 0;
      /* r1 = lo1 | hi1 — is src1 non-zero? */
      ot_check(th_orr_reg(r1, r1, hi1_reg, flags_safe(), THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
    }
    else
    {
      r1 = mach_ensure_in_reg(&mctx, &src1, excl);
      excl |= thumb_is_hw_reg(r1) ? (1u << (uint32_t)r1) : 0;
    }

    if (src2.is_64bit)
    {
      MachineOperand lo2 = mach_make_lo_half(&src2);
      lo2.btype = IROP_BTYPE_INT32;
      MachineOperand hi2 = mach_make_hi_half(&src2);
      hi2.btype = IROP_BTYPE_INT32;
      r2 = mach_ensure_in_reg(&mctx, &lo2, excl);
      excl |= thumb_is_hw_reg(r2) ? (1u << (uint32_t)r2) : 0;
      int hi2_reg = mach_ensure_in_reg(&mctx, &hi2, excl);
      excl |= thumb_is_hw_reg(hi2_reg) ? (1u << (uint32_t)hi2_reg) : 0;
      /* r2 = lo2 | hi2 — is src2 non-zero? */
      ot_check(th_orr_reg(r2, r2, hi2_reg, flags_safe(), THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
    }
    else
    {
      r2 = mach_ensure_in_reg(&mctx, &src2, excl);
      excl |= thumb_is_hw_reg(r2) ? (1u << (uint32_t)r2) : 0;
    }

    int dest_reg = mach_get_dest_reg(&mctx, &dest, excl);

    if (op == TCCIR_OP_BOOL_OR)
    {
      ot_check(th_orr_reg(dest_reg, r1, r2, FLAGS_BEHAVIOUR_SET, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
      ot_check(th_mov_imm(dest_reg, 0, FLAGS_BEHAVIOUR_BLOCK, ENFORCE_ENCODING_NONE));
      th_literal_pool_reserve_upcoming_bytes(6);
      ot_check(th_it(0x1, 0x8)); /* IT NE */
      ot_check(th_mov_imm(dest_reg, 1, it_mov_flags(dest_reg), ENFORCE_ENCODING_NONE));
    }
    else /* TCCIR_OP_BOOL_AND */
    {
      ot_check(th_cmp_imm(r1, 0, FLAGS_BEHAVIOUR_SET, ENFORCE_ENCODING_NONE));
      th_literal_pool_reserve_upcoming_bytes(6);
      ot_check(th_it(0x1, 0x8));                                                  /* IT NE */
      ot_check(th_cmp_imm(r2, 0, FLAGS_BEHAVIOUR_SET, ENFORCE_ENCODING_NONE)); /* CMPne r2, #0 */
      ot_check(th_mov_imm(dest_reg, 0, FLAGS_BEHAVIOUR_BLOCK, ENFORCE_ENCODING_NONE));
      th_literal_pool_reserve_upcoming_bytes(6);
      ot_check(th_it(0x1, 0x8)); /* IT NE */
      ot_check(th_mov_imm(dest_reg, 1, it_mov_flags(dest_reg), ENFORCE_ENCODING_NONE));
    }

    mach_writeback_dest(&dest, dest_reg);
    mach_release_all(&mctx);
    return;
  }

  int dest_reg = mach_get_dest_reg(&mctx, &dest, 0);
  uint32_t excl = thumb_is_hw_reg(dest_reg) ? (1u << (uint32_t)dest_reg) : 0;

  int src1_reg = mach_ensure_in_reg(&mctx, &src1, excl);
  if (thumb_is_hw_reg(src1_reg))
    excl |= (1u << (uint32_t)src1_reg);

  int src2_reg = mach_ensure_in_reg(&mctx, &src2, excl);

  if (op == TCCIR_OP_BOOL_OR)
  {
    ot_check(th_orr_reg(dest_reg, src1_reg, src2_reg, FLAGS_BEHAVIOUR_SET, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
    ot_check(th_mov_imm(dest_reg, 0, FLAGS_BEHAVIOUR_BLOCK, ENFORCE_ENCODING_NONE));
    th_literal_pool_reserve_upcoming_bytes(6);
    ot_check(th_it(0x1, 0x8)); /* IT NE */
    ot_check(th_mov_imm(dest_reg, 1, it_mov_flags(dest_reg), ENFORCE_ENCODING_NONE));
  }
  else /* TCCIR_OP_BOOL_AND */
  {
    ot_check(th_cmp_imm(src1_reg, 0, FLAGS_BEHAVIOUR_SET, ENFORCE_ENCODING_NONE));
    th_literal_pool_reserve_upcoming_bytes(6);
    ot_check(th_it(0x1, 0x8));                                                        /* IT NE */
    ot_check(th_cmp_imm(src2_reg, 0, FLAGS_BEHAVIOUR_SET, ENFORCE_ENCODING_NONE)); /* CMPne src2, #0 */
    ot_check(th_mov_imm(dest_reg, 0, FLAGS_BEHAVIOUR_BLOCK, ENFORCE_ENCODING_NONE));
    th_literal_pool_reserve_upcoming_bytes(6);
    ot_check(th_it(0x1, 0x8)); /* IT NE */
    ot_check(th_mov_imm(dest_reg, 1, it_mov_flags(dest_reg), ENFORCE_ENCODING_NONE));
  }

  mach_writeback_dest(&dest, dest_reg);
  mach_release_all(&mctx);
}
