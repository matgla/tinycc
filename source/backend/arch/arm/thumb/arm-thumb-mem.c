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

/* Thumb-2 code generator: load and store lowering (plain, indexed and
 * post-increment) of MachineOperands. */

#include "arm-thumb-gen.h"

/* tcc_gen_machine_load_mop: MachineOperand-based entry point for TCCIR_OP_LOAD.
 *
 * dest can be MACH_OP_REG, MACH_OP_SPILL, or MACH_OP_PARAM_STACK.
 * For spilled destinations, a scratch register is allocated and the result
 * is written back to the spill slot after the load completes.
 * 64-bit dest is supported: for MACH_OP_REG dest.u.reg.r1 holds the hi
 * register; for spilled dests, a second scratch is allocated for hi-half.
 *
 * src encodes the memory address:
 *   MACH_OP_REG + needs_deref=true  → LDR dest, [src_reg]
 *   MACH_OP_SPILL                   → LDR dest, [FP + fp_adjust(offset)]
 *   MACH_OP_SPILL + needs_deref=true → LLOCAL: LDR ptr,[FP+off]; LDR dest,[ptr]
 *   MACH_OP_PARAM_STACK             → LDR dest, [FP + param_off + offset_to_args]
 *   MACH_OP_SYMBOL                  → LDR_literal addr; LDR dest, [addr]
 *   MACH_OP_IMM                     → tcc_machine_load_constant (constant load)
 */
ST_FUNC void tcc_gen_machine_load_mop(MachineOperand src, MachineOperand dest, TccIrOp op)
{
  MachineCodegenContext ctx = {0};
  (void)op;

  /* A load whose source is a VFP register is a read out of that s-register (a
   * register-promoted float), not a memory load — delegate to the VFP-aware
   * assign path, which handles every destination kind. */
  if (src.kind == MACH_OP_VFP_REG)
  {
    tcc_gen_machine_assign_mop(src, dest, TCCIR_OP_ASSIGN);
    return;
  }

  /* Determine dest register — allocates scratch if dest is SPILL/PARAM_STACK. */
  const bool dest_is_simple_reg =
      (dest.kind == MACH_OP_REG && !dest.needs_deref && dest.u.reg.r0 != (int)PREG_REG_NONE);
  const int dest_reg = mach_get_dest_reg(&ctx, &dest, 0);

  /* For 64-bit pairs: get hi-half dest register. */
  int dest_r1 = PREG_REG_NONE;
  MachineOperand dest_hi_mop = {0};
  if (dest.is_64bit)
  {
    if (dest_is_simple_reg)
    {
      dest_r1 = dest.u.reg.r1;
    }
    else
    {
      dest_hi_mop = mach_make_hi_half(&dest);
      dest_r1 = mach_get_dest_reg(&ctx, &dest_hi_mop, (1u << (uint32_t)dest_reg));
    }
  }

  const int btype = src.btype;
  const int is_unsigned = (int)src.is_unsigned;

  switch (src.kind)
  {
  case MACH_OP_REG:
    if (src.needs_deref)
    {
      /* Register-indirect: LDR dest, [src_reg].
       * 64-bit + proven >= 4-byte alignment (src.align4, from the frontend's
       * packed-access tracking): use LDRD directly.  LDRD Rt==Rn is fine
       * without writeback, so no base-preservation dance is needed.  Without
       * the proof, load_from_base emits the unaligned-safe LDR pair. */
      if (dest.is_64bit && !dest.is_complex && src.align4 && !src.underalign_hint && dest_r1 != (int)PREG_REG_NONE &&
          try_ldrd_pair(dest_reg, dest_r1, src.u.reg.r0, 0, 0))
        break;
      load_from_base(dest_reg, dest_r1, btype, is_unsigned, 0, 0, (uint32_t)src.u.reg.r0);
    }
    else
    {
      /* Direct register-to-register (treat as MOV — should be ASSIGN, not LOAD) */
      if (dest_reg != src.u.reg.r0)
        ot_check_mov_reg((uint32_t)dest_reg, (uint32_t)src.u.reg.r0, flags_safe(), THUMB_SHIFT_DEFAULT,
                         ENFORCE_ENCODING_NONE, false);
      /* Narrow sub-word parameter values: when a parameter is declared as
       * char/short but arrives in a full 32-bit register (AAPCS default
       * argument promotion), the upper bits may contain garbage.  Emit
       * UXTB/SXTB/UXTH/SXTH to truncate to the declared type width.  */
      if (btype == IROP_BTYPE_INT8)
      {
        if (is_unsigned)
          ot_check(th_uxtb((uint32_t)dest_reg, (uint32_t)dest_reg, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
        else
          ot_check(th_sxtb((uint32_t)dest_reg, (uint32_t)dest_reg, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
      }
      else if (btype == IROP_BTYPE_INT16)
      {
        if (is_unsigned)
          ot_check(th_uxth((uint32_t)dest_reg, (uint32_t)dest_reg, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
        else
          ot_check(th_sxth((uint32_t)dest_reg, (uint32_t)dest_reg, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
      }
      /* 64-bit pair: also copy the hi-half register */
      if (dest_r1 != PREG_REG_NONE && src.u.reg.r1 >= 0 && dest_r1 != src.u.reg.r1)
        ot_check_mov_reg((uint32_t)dest_r1, (uint32_t)src.u.reg.r1, flags_safe(), THUMB_SHIFT_DEFAULT,
                         ENFORCE_ENCODING_NONE, false);
    }
    break;

  case MACH_OP_SPILL:
  {
    const int adj = fp_adjust_local_offset(src.u.spill.offset, 0);
    const int sign = (adj < 0), abs_off = sign ? -adj : adj;
    const uint32_t base = (uint32_t)(tcc_state->need_frame_pointer ? R_FP : R_SP);
    if (!src.needs_deref)
    {
      /* Load value directly from spill/local slot */
      load_from_base(dest_reg, dest_r1, btype, is_unsigned, abs_off, sign, base);
    }
    else
    {
      /* LLOCAL: spill slot holds a pointer; load ptr, then dereference */
      int ptr_r = mach_alloc_scratch(&ctx, ((uint32_t)1u << (uint32_t)dest_reg) | ((uint32_t)1u << base));
      if (!load_word_from_base(ptr_r, (int)base, abs_off, sign))
      {
        ScratchRegAlloc rr = th_offset_to_reg_ex(abs_off, sign, ((uint32_t)1u << ptr_r) | ((uint32_t)1u << base));
        ot_check(th_ldr_reg((uint32_t)ptr_r, base, (uint32_t)rr.reg, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
        restore_scratch_reg(&rr);
      }
      /* 64-bit + proven alignment: LDRD through the loaded pointer. */
      if (dest.is_64bit && !dest.is_complex && src.align4 && !src.underalign_hint && dest_r1 != (int)PREG_REG_NONE &&
          try_ldrd_pair(dest_reg, dest_r1, ptr_r, 0, 0))
        break;
      load_from_base(dest_reg, dest_r1, btype, is_unsigned, 0, 0, (uint32_t)ptr_r);
    }
    break;
  }

  case MACH_OP_PARAM_STACK:
  {
    const int adj = param_frame_offset(src.u.param.offset);
    const int sign = (adj < 0), abs_off = sign ? -adj : adj;
    const uint32_t base = (uint32_t)(tcc_state->need_frame_pointer ? R_FP : R_SP);
    load_from_base(dest_reg, dest_r1, btype, is_unsigned, abs_off, sign, base);
    break;
  }

  case MACH_OP_SYMBOL:
  {
    Sym *sym = src.u.sym.sym ? validate_sym_for_reloc(src.u.sym.sym) : NULL;
    const int32_t addend = src.u.sym.addend;
    if (!src.needs_deref)
    {
      /* Load symbol address (+ addend) into dest — no dereference.
       * Load symbol address (+ addend) — no dereference. */
      tcc_machine_load_constant(dest_reg, dest_r1, (int64_t)addend, (int)dest.is_64bit, sym);
      break;
    }
    /* needs_deref: load symbol address into scratch, then dereference. */
    int addr_r = mach_alloc_scratch_for_sym(&ctx, (uint32_t)1u << (uint32_t)dest_reg, sym, 0);
    tcc_machine_load_constant(addr_r, PREG_REG_NONE, 0, 0, sym);
    /* For a 64-bit deref, try LDRD when we can prove the symbol's address
     * at `addend` is 4-byte aligned.  Otherwise fall back to the pair of
     * 32-bit loads via load_from_base. */
    const int sym_sign = (addend < 0), sym_abs = sym_sign ? (int)(-addend) : (int)addend;
    if (dest.is_64bit && dest_r1 != PREG_REG_NONE && sym_is_4_byte_aligned_for_64bit(sym, addend) &&
        try_ldrd_pair(dest_reg, dest_r1, addr_r, sym_abs, sym_sign))
    {
      break;
    }
    load_from_base(dest_reg, dest_r1, btype, is_unsigned, sym_abs, sym_sign, (uint32_t)addr_r);
    break;
  }

  case MACH_OP_IMM:
    if (src.needs_deref)
    {
      /* Read through an absolute address: `*(volatile T *)0x40000000`, the
       * MMIO register idiom.  Materialize the address, then load from it —
       * the same shape as the MACH_OP_SYMBOL deref path above, and the mirror
       * of the store side, which has always done this.  Alignment of an
       * arbitrary constant address is unknown, so no LDRD. */
      int addr_r = mach_alloc_scratch(&ctx, (uint32_t)1u << (uint32_t)dest_reg);
      tcc_machine_load_constant(addr_r, PREG_REG_NONE, src.u.imm.val, 0, NULL);
      load_from_base(dest_reg, dest_r1, btype, is_unsigned, 0, 0, (uint32_t)addr_r);
      break;
    }
    /* Not an lvalue: the immediate IS the value. */
    tcc_machine_load_constant(dest_reg, dest_r1, src.u.imm.val, (int)dest.is_64bit, NULL);
    break;

  case MACH_OP_FRAME_ADDR:
  {
    if (!src.needs_deref)
    {
      /* Load the frame-slot address itself (LEA semantics). */
      tcc_machine_addr_of_stack_slot(dest_reg, src.u.frame.offset, 0);
    }
    else
    {
      /* Frame address is a pointer to data — compute addr, then dereference. */
      int addr_r = mach_alloc_scratch(&ctx, (uint32_t)1u << (uint32_t)dest_reg);
      tcc_machine_addr_of_stack_slot(addr_r, src.u.frame.offset, 0);
      load_from_base(dest_reg, dest_r1, btype, is_unsigned, 0, 0, (uint32_t)addr_r);
    }
    break;
  }

  case MACH_OP_CHAIN_REL:
  {
    /* Captured variable: load from parent frame via static chain. */
    ScratchRegAlloc chain_scratch = {0};
    int chain_used = 0;
    int base = resolve_chain_base(tcc_state->ir, src.u.chain.chain_index, (1u << (uint32_t)dest_reg), &chain_scratch,
                                  &chain_used);
    int32_t off = src.u.chain.offset;
    int sign = (off < 0), abs_off = sign ? (int)(-off) : (int)off;
    load_from_base(dest_reg, dest_r1, btype, is_unsigned, abs_off, sign, (uint32_t)base);
    if (chain_used)
      restore_scratch_reg(&chain_scratch);
    break;
  }

  default:
    tcc_ice("load_mop: unhandled src kind %d", (int)src.kind);
  }

  /* Write back result to spill/param slot if dest was not a plain register. */
  if (!dest_is_simple_reg)
  {
    if (dest.is_64bit)
    {
      MachineOperand dest_lo_mop = mach_make_lo_half(&dest);
      mach_writeback_dest(&dest_lo_mop, dest_reg);
      mach_writeback_dest(&dest_hi_mop, dest_r1);
    }
    else
    {
      mach_writeback_dest(&dest, dest_reg);
    }
  }

  mach_release_all(&ctx);
}

/* tcc_gen_machine_store_mop: MachineOperand-based entry point for TCCIR_OP_STORE.
 *
 * dest encodes the destination address (memory location to write to).
 * src encodes the value to store.
 * Store width is determined by dest.btype.
 *
 * dest kinds handled:
 *   MACH_OP_REG + needs_deref=true  → STR src, [dest_reg]
 *   MACH_OP_REG (no deref)          → MOV dest_reg, src (reg-to-reg)
 *   MACH_OP_SPILL                   → STR src, [FP + fp_adjust(offset)]
 *   MACH_OP_PARAM_STACK             → STR src, [FP + param_off + offset_to_args]
 *   MACH_OP_SYMBOL                  → load addr, STR src, [addr + addend]
 *
 * 64-bit src: emits two 32-bit stores at [dest+0] (lo) and [dest+4] (hi)
 * for all dest kinds above, plus MACH_OP_IMM and MACH_OP_FRAME_ADDR.
 */
ST_FUNC void tcc_gen_machine_store_mop(MachineOperand dest, MachineOperand src, TccIrOp op)
{
  MachineCodegenContext ctx = {0};
  (void)op;

  /* A store whose destination is a VFP register is a write into that s-register
   * (a register-promoted float), not a memory store — delegate to the VFP-aware
   * assign path. */
  if (dest.kind == MACH_OP_VFP_REG)
  {
    tcc_gen_machine_assign_mop(src, dest, TCCIR_OP_ASSIGN);
    return;
  }

  /* 128-bit complex double store: emit four 32-bit stores for real lo/hi + imag lo/hi.
   * Complex double values are 16 bytes: real (8 bytes) at base, imag (8 bytes) at base+8.
   * The source is always spilled (force-spilled by the register allocator). */
  if (src.is_64bit && src.is_complex && src.btype == IROP_BTYPE_FLOAT64)
  {
    /* Split into four 32-bit words using complex then lo/hi splitting. */
    MachineOperand real_part = mach_make_complex_real(&src);
    MachineOperand imag_part = mach_make_complex_imag(&src);
    MachineOperand w0 = mach_make_lo_half(&real_part);
    w0.btype = IROP_BTYPE_INT32;
    MachineOperand w1 = mach_make_hi_half(&real_part);
    w1.btype = IROP_BTYPE_INT32;
    MachineOperand w2 = mach_make_lo_half(&imag_part);
    w2.btype = IROP_BTYPE_INT32;
    MachineOperand w3 = mach_make_hi_half(&imag_part);
    w3.btype = IROP_BTYPE_INT32;

    /* Load all 4 words into registers. */
    const int r0 = mach_ensure_in_reg(&ctx, &w0, 0);
    uint32_t excl = (1u << (uint32_t)r0);
    const int r1 = mach_ensure_in_reg(&ctx, &w1, excl);
    excl |= (1u << (uint32_t)r1);
    const int r2 = mach_ensure_in_reg(&ctx, &w2, excl);
    excl |= (1u << (uint32_t)r2);
    const int r3 = mach_ensure_in_reg(&ctx, &w3, excl);
    excl |= (1u << (uint32_t)r3);

    /* Store through dest: 4 × 32-bit stores at [dest+0], [dest+4], [dest+8], [dest+12]. */
    if (dest.kind == MACH_OP_REG && dest.needs_deref)
    {
      const uint32_t base = (uint32_t)dest.u.reg.r0;
      th_store32_imm_or_reg_ex(r0, base, 0, 0, excl | (1u << base));
      th_store32_imm_or_reg_ex(r1, base, 4, 0, excl | (1u << base));
      th_store32_imm_or_reg_ex(r2, base, 8, 0, excl | (1u << base));
      th_store32_imm_or_reg_ex(r3, base, 12, 0, excl | (1u << base));
    }
    else if (dest.kind == MACH_OP_SPILL)
    {
      const int adj = fp_adjust_local_offset(dest.u.spill.offset, 0);
      const uint32_t base = (uint32_t)(tcc_state->need_frame_pointer ? R_FP : R_SP);
      th_store32_imm_or_reg_ex(r0, base, adj < 0 ? -adj : adj, adj < 0 ? 1 : 0, excl | (1u << base));
      int a1 = adj + 4;
      th_store32_imm_or_reg_ex(r1, base, a1 < 0 ? -a1 : a1, a1 < 0 ? 1 : 0, excl | (1u << base));
      int a2 = adj + 8;
      th_store32_imm_or_reg_ex(r2, base, a2 < 0 ? -a2 : a2, a2 < 0 ? 1 : 0, excl | (1u << base));
      int a3 = adj + 12;
      th_store32_imm_or_reg_ex(r3, base, a3 < 0 ? -a3 : a3, a3 < 0 ? 1 : 0, excl | (1u << base));
    }
    else
    {
      tcc_ice("store_mop: unhandled dest kind %d for complex double store", (int)dest.kind);
    }
    mach_release_all(&ctx);
    return;
  }

  /* 64-bit store: emit two 32-bit stores for lo and hi halves */
  /* A store is as wide as its destination.  A 64-bit integer into a word or
   * narrower is a truncation: only the low half goes anywhere.  Taking the
   * pair path below instead wrote the high half too -- into memory past the
   * word, or, for a plain register destination, into its absent second
   * register: r1 == -1 encodes as pc (`mov pc, r7`, Zig's InternPool.Key.eql
   * truncating aggregateTypeLen's u64 once var_to_param_forward had put the
   * u64 in a register pair). */
  if (src.is_64bit && !src.is_complex && src.btype == IROP_BTYPE_INT64 && !dest.is_64bit &&
      (dest.btype == IROP_BTYPE_INT32 || dest.btype == IROP_BTYPE_INT16 || dest.btype == IROP_BTYPE_INT8))
  {
    src = mach_make_lo_half(&src);
    src.btype = IROP_BTYPE_INT32;
  }
  if (src.is_64bit)
  {
    MachineOperand src_lo = mach_make_lo_half(&src);
    src_lo.btype = IROP_BTYPE_INT32;
    MachineOperand src_hi = mach_make_hi_half(&src);
    src_hi.btype = IROP_BTYPE_INT32;

    uint32_t dest_excl = 0;
    if (dest.kind == MACH_OP_REG && dest.needs_deref &&
        dest.u.reg.r0 >= 0 && dest.u.reg.r0 < 16)
      dest_excl = (1u << (uint32_t)dest.u.reg.r0);

    const int lo_reg = mach_ensure_in_reg(&ctx, &src_lo, dest_excl);
    uint32_t excl = dest_excl | (thumb_is_hw_reg(lo_reg) ? (1u << (uint32_t)lo_reg) : 0u);
    const int hi_reg = mach_ensure_in_reg(&ctx, &src_hi, excl);
    excl |= thumb_is_hw_reg(hi_reg) ? (1u << (uint32_t)hi_reg) : 0u;

    switch (dest.kind)
    {
    case MACH_OP_REG:
      if (dest.needs_deref)
      {
        /* 64-bit pointer-store through a register-held address.  STRD needs
         * a 4-byte-aligned address on ARMv7-M/v8-M (faults otherwise,
         * regardless of UNALIGN_TRP); use it only when the frontend proved
         * alignment (dest.align4 — no packed member in the access chain).
         * Otherwise the pointer may target packed-struct memory that is only
         * 1- or 2-byte aligned, so two plain STRs stay the safe fallback. */
        const uint32_t base = (uint32_t)dest.u.reg.r0;
        if (dest.align4 && !dest.underalign_hint && try_strd_pair(lo_reg, hi_reg, (int)base, 0, 0))
          break;
        th_store32_imm_or_reg_ex(lo_reg, base, 0, 0, excl | (1u << base));
        th_store32_imm_or_reg_ex(hi_reg, base, 4, 0, excl | (1u << base));
      }
      else
      {
        /* Reg-pair dst: emit hi first unless lo_reg == dest.r1 (safe-ordering) */
        const int dreg_lo = dest.u.reg.r0;
        const int dreg_hi = dest.u.reg.r1;
        if (lo_reg == dreg_hi)
        {
          if (dreg_lo != lo_reg && dreg_lo != (int)PREG_REG_NONE)
            ot_check_mov_reg((uint32_t)dreg_lo, (uint32_t)lo_reg, flags_safe(), THUMB_SHIFT_DEFAULT,
                             ENFORCE_ENCODING_NONE, false);
          if (dreg_hi != hi_reg && dreg_hi != (int)PREG_REG_NONE)
            ot_check_mov_reg((uint32_t)dreg_hi, (uint32_t)hi_reg, flags_safe(), THUMB_SHIFT_DEFAULT,
                             ENFORCE_ENCODING_NONE, false);
        }
        else
        {
          if (dreg_hi != hi_reg && dreg_hi != (int)PREG_REG_NONE)
            ot_check_mov_reg((uint32_t)dreg_hi, (uint32_t)hi_reg, flags_safe(), THUMB_SHIFT_DEFAULT,
                             ENFORCE_ENCODING_NONE, false);
          if (dreg_lo != lo_reg && dreg_lo != (int)PREG_REG_NONE)
            ot_check_mov_reg((uint32_t)dreg_lo, (uint32_t)lo_reg, flags_safe(), THUMB_SHIFT_DEFAULT,
                             ENFORCE_ENCODING_NONE, false);
        }
      }
      break;

    case MACH_OP_SPILL:
    {
      const int adj = fp_adjust_local_offset(dest.u.spill.offset, 0);
      const uint32_t base = (uint32_t)(tcc_state->need_frame_pointer ? R_FP : R_SP);
      if (dest.needs_deref)
      {
        /* LLOCAL: spill slot holds a pointer; load ptr, then store through it */
        int ptr_r = mach_alloc_scratch(&ctx, excl | (1u << base));
        if (!load_word_from_base(ptr_r, (int)base, adj < 0 ? -adj : adj, adj < 0 ? 1 : 0))
        {
          ScratchRegAlloc rr =
              th_offset_to_reg_ex(adj < 0 ? -adj : adj, adj < 0 ? 1 : 0, (uint32_t)(1u << ptr_r) | (1u << base));
          ot_check(th_ldr_reg((uint32_t)ptr_r, base, (uint32_t)rr.reg, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
          restore_scratch_reg(&rr);
        }
        /* Pointer-through store from an LLOCAL spill slot: STRD only when
         * the frontend proved >= 4-byte alignment of the target (dest.align4);
         * an arbitrary pointer may reference unaligned packed-struct memory. */
        if (dest.align4 && !dest.underalign_hint && try_strd_pair(lo_reg, hi_reg, ptr_r, 0, 0))
          break;
        th_store32_imm_or_reg_ex(lo_reg, (uint32_t)ptr_r, 0, 0, excl | (1u << (uint32_t)ptr_r));
        th_store32_imm_or_reg_ex(hi_reg, (uint32_t)ptr_r, 4, 0, excl | (1u << (uint32_t)ptr_r));
      }
      else
      {
        const int adj_hi = adj + 4;
        const int sign = (adj < 0), abs_off = sign ? -adj : adj;
        if (!try_strd_pair(lo_reg, hi_reg, (int)base, abs_off, sign))
        {
          th_store32_imm_or_reg_ex(lo_reg, base, abs_off, sign, excl | (1u << base));
          th_store32_imm_or_reg_ex(hi_reg, base, adj_hi < 0 ? -adj_hi : adj_hi, adj_hi < 0 ? 1 : 0,
                                   excl | (1u << base));
        }
      }
      break;
    }

    case MACH_OP_PARAM_STACK:
    {
      const int adj = param_frame_offset(dest.u.param.offset);
      const int adj_hi = adj + 4;
      const uint32_t base = (uint32_t)(tcc_state->need_frame_pointer ? R_FP : R_SP);
      const int sign = (adj < 0), abs_off = sign ? -adj : adj;
      if (!try_strd_pair(lo_reg, hi_reg, (int)base, abs_off, sign))
      {
        th_store32_imm_or_reg_ex(lo_reg, base, abs_off, sign, excl | (1u << base));
        th_store32_imm_or_reg_ex(hi_reg, base, adj_hi < 0 ? -adj_hi : adj_hi, adj_hi < 0 ? 1 : 0, excl | (1u << base));
      }
      break;
    }

    case MACH_OP_SYMBOL:
    {
      /* Global symbol store.  STRD needs 4-byte alignment; allow it only when
       * the symbol's declared type guarantees natural alignment >= 4 (regular
       * scalar globals) or the symbol was explicitly aligned.  Packed structs
       * and struct-typed globals stay on the STR-pair path. */
      Sym *sym = dest.u.sym.sym ? validate_sym_for_reloc(dest.u.sym.sym) : NULL;
      int addr_r = mach_alloc_scratch_for_sym(&ctx, excl, sym, 0);
      tcc_machine_load_constant(addr_r, PREG_REG_NONE, 0, 0, sym);
      const int32_t addend = dest.u.sym.addend;
      const int32_t addend_hi = addend + 4;
      const int sign = (addend < 0), abs_off = sign ? (int)(-addend) : (int)addend;
      if (sym_is_4_byte_aligned_for_64bit(sym, addend) && try_strd_pair(lo_reg, hi_reg, addr_r, abs_off, sign))
      {
        break;
      }
      th_store32_imm_or_reg_ex(lo_reg, (uint32_t)addr_r, abs_off, sign, excl | (1u << addr_r));
      th_store32_imm_or_reg_ex(hi_reg, (uint32_t)addr_r, addend_hi < 0 ? (int)(-addend_hi) : (int)addend_hi,
                               addend_hi < 0 ? 1 : 0, excl | (1u << addr_r));
      break;
    }

    case MACH_OP_IMM:
    {
      /* Store to a constant address — alignment unknown, skip STRD. */
      int addr_r = mach_alloc_scratch(&ctx, excl);
      tcc_machine_load_constant(addr_r, PREG_REG_NONE, dest.u.imm.val, 0, NULL);
      th_store32_imm_or_reg_ex(lo_reg, (uint32_t)addr_r, 0, 0, excl | (1u << addr_r));
      th_store32_imm_or_reg_ex(hi_reg, (uint32_t)addr_r, 4, 0, excl | (1u << addr_r));
      break;
    }

    case MACH_OP_FRAME_ADDR:
    {
      int addr_r = mach_alloc_scratch(&ctx, excl);
      tcc_machine_addr_of_stack_slot(addr_r, dest.u.frame.offset, 0 /* not param */);
      if (!try_strd_pair(lo_reg, hi_reg, addr_r, 0, 0))
      {
        th_store32_imm_or_reg_ex(lo_reg, (uint32_t)addr_r, 0, 0, excl | (1u << addr_r));
        th_store32_imm_or_reg_ex(hi_reg, (uint32_t)addr_r, 4, 0, excl | (1u << addr_r));
      }
      break;
    }

    case MACH_OP_CHAIN_REL:
    {
      /* 64-bit captured variable: store lo+hi words to parent frame. */
      ScratchRegAlloc chain_scratch = {0};
      int chain_used = 0;
      int base = resolve_chain_base(tcc_state->ir, dest.u.chain.chain_index, excl, &chain_scratch, &chain_used);
      int32_t off = dest.u.chain.offset;
      int sign = (off < 0), abs_off = sign ? (int)(-off) : (int)off;
      int32_t off_hi = off + 4;
      int sign_hi = (off_hi < 0), abs_off_hi = sign_hi ? (int)(-off_hi) : (int)off_hi;
      if (!try_strd_pair(lo_reg, hi_reg, base, abs_off, sign))
      {
        th_store32_imm_or_reg_ex(lo_reg, (uint32_t)base, abs_off, sign, excl | (1u << (uint32_t)base));
        th_store32_imm_or_reg_ex(hi_reg, (uint32_t)base, abs_off_hi, sign_hi, excl | (1u << (uint32_t)base));
      }
      if (chain_used)
        restore_scratch_reg(&chain_scratch);
      break;
    }

    default:
      tcc_ice("store_mop: unhandled dest kind %d for 64-bit src", (int)dest.kind);
    }
    mach_release_all(&ctx);
    return;
  }

  const int btype = dest.btype; /* Store width from destination type */

  /* Fast path: plain-register dest (no deref) — load src directly into dest,
   * skipping the intermediate scratch + MOV that the generic path emits.
   * Covers IMM, SYMBOL, SPILL, FRAME_ADDR, PARAM_STACK, CHAIN_REL, and
   * REG-with-or-without-deref src kinds. */
  if (dest.kind == MACH_OP_REG && !dest.needs_deref && dest.u.reg.r0 != (int)PREG_REG_NONE)
  {
    tcc_gen_mach_load_to_reg(dest.u.reg.r0, &src);
    mach_release_all(&ctx);
    return;
  }

  /* Get source value register — may allocate a scratch if spilled/const.
   * When storing through a register-held pointer, protect the base register
   * before materializing immediates or spilled values. */
  uint32_t src_excl = 0;
  if (dest.kind == MACH_OP_REG && dest.needs_deref &&
      dest.u.reg.r0 >= 0 && dest.u.reg.r0 < 16)
    src_excl |= (1u << (uint32_t)dest.u.reg.r0);
  const int src_reg = mach_ensure_in_reg(&ctx, &src, src_excl);

  switch (dest.kind)
  {
  case MACH_OP_REG:
    if (dest.needs_deref)
    {
      /* Store through pointer: STR src, [dest_reg] */
      const uint32_t base = (uint32_t)dest.u.reg.r0;
      if (btype == IROP_BTYPE_INT8)
        th_store8_imm_or_reg(src_reg, base, 0, 0);
      else if (btype == IROP_BTYPE_INT16)
        th_store16_imm_or_reg(src_reg, base, 0, 0);
      else
        th_store32_imm_or_reg_ex(src_reg, base, 0, 0, (uint32_t)1u << (uint32_t)src_reg);
    }
    else
    {
      /* Register-to-register store (MOV) */
      const int dreg = dest.u.reg.r0;
      if (dreg != src_reg && dreg != (int)PREG_REG_NONE)
        ot_check_mov_reg((uint32_t)dreg, (uint32_t)src_reg, flags_safe(), THUMB_SHIFT_DEFAULT,
                         ENFORCE_ENCODING_NONE, false);
    }
    break;

  case MACH_OP_SPILL:
  {
    const int adj = fp_adjust_local_offset(dest.u.spill.offset, 0);
    const int sign = (adj < 0), abs_off = sign ? -adj : adj;
    const uint32_t base = (uint32_t)(tcc_state->need_frame_pointer ? R_FP : R_SP);
    if (dest.needs_deref)
    {
      /* LLOCAL: spill slot holds a pointer; load ptr, then store through it */
      int ptr_r = mach_alloc_scratch(&ctx, (uint32_t)1u << (uint32_t)src_reg | (1u << base));
      if (!load_word_from_base(ptr_r, (int)base, abs_off, sign))
      {
        ScratchRegAlloc rr =
            th_offset_to_reg_ex(abs_off, sign, (uint32_t)(1u << ptr_r) | (1u << base) | (1u << (uint32_t)src_reg));
        ot_check(th_ldr_reg((uint32_t)ptr_r, base, (uint32_t)rr.reg, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
        restore_scratch_reg(&rr);
      }
      if (btype == IROP_BTYPE_INT8)
        th_store8_imm_or_reg(src_reg, (uint32_t)ptr_r, 0, 0);
      else if (btype == IROP_BTYPE_INT16)
        th_store16_imm_or_reg(src_reg, (uint32_t)ptr_r, 0, 0);
      else
        th_store32_imm_or_reg_ex(src_reg, (uint32_t)ptr_r, 0, 0,
                                 (uint32_t)1u << (uint32_t)src_reg | (1u << (uint32_t)ptr_r));
    }
    else if ((btype == IROP_BTYPE_INT8 || btype == IROP_BTYPE_INT16) && dest.vreg >= 0 &&
             (TCCIR_DECODE_VREG_TYPE(dest.vreg) == TCCIR_VREG_TYPE_TEMP || mach_var_owns_spill_slot(dest.vreg)))
    {
      /* A temporary's spill slot holds a register image, and every reload of
       * it is a word load (mach_load_slot reads narrow only for a narrow
       * operand, and a use such as `T ADD #1` is not one).  A byte store left
       * three stale bytes for that load to pick up: pr82524's
       * `foo(y->c.b, w)` spilled both u8 operands and multiplied garbage.
       * Store the value extended to a word, the truncation the narrow store
       * gave included.  The same for a VAR in a spill slot of its own
       * (mach_var_owns_spill_slot); an address-taken VAR's slot is its
       * narrow home, whose neighbours a word store would clobber -- that
       * stays narrow. */
      const int ext = mach_alloc_scratch(&ctx, (uint32_t)1u << (uint32_t)src_reg);
      if (btype == IROP_BTYPE_INT8)
        ot_check(dest.is_unsigned
                     ? th_uxtb((uint32_t)ext, (uint32_t)src_reg, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE)
                     : th_sxtb((uint32_t)ext, (uint32_t)src_reg, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
      else
        ot_check(dest.is_unsigned
                     ? th_uxth((uint32_t)ext, (uint32_t)src_reg, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE)
                     : th_sxth((uint32_t)ext, (uint32_t)src_reg, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
      th_store32_imm_or_reg_ex(ext, base, abs_off, sign, (uint32_t)1u << (uint32_t)ext);
    }
    else
    {
      if (btype == IROP_BTYPE_INT8)
        th_store8_imm_or_reg(src_reg, base, abs_off, sign);
      else if (btype == IROP_BTYPE_INT16)
        th_store16_imm_or_reg(src_reg, base, abs_off, sign);
      else
        th_store32_imm_or_reg_ex(src_reg, base, abs_off, sign, (uint32_t)1u << (uint32_t)src_reg);
    }
    break;
  }

  case MACH_OP_PARAM_STACK:
  {
    const int adj = param_frame_offset(dest.u.param.offset);
    const int sign = (adj < 0), abs_off = sign ? -adj : adj;
    const uint32_t base = (uint32_t)(tcc_state->need_frame_pointer ? R_FP : R_SP);
    if (btype == IROP_BTYPE_INT8)
      th_store8_imm_or_reg(src_reg, base, abs_off, sign);
    else if (btype == IROP_BTYPE_INT16)
      th_store16_imm_or_reg(src_reg, base, abs_off, sign);
    else
      th_store32_imm_or_reg_ex(src_reg, base, abs_off, sign, (uint32_t)1u << (uint32_t)src_reg);
    break;
  }

  case MACH_OP_SYMBOL:
  {
    Sym *sym = dest.u.sym.sym ? validate_sym_for_reloc(dest.u.sym.sym) : NULL;
    int addr_r = mach_alloc_scratch_for_sym(&ctx, (uint32_t)1u << (uint32_t)src_reg, sym, 0);
    tcc_machine_load_constant(addr_r, PREG_REG_NONE, 0, 0, sym);
    const int32_t addend = dest.u.sym.addend;
    const int abs_off = addend < 0 ? (int)(-addend) : (int)addend;
    const int sign = addend < 0 ? 1 : 0;
    if (btype == IROP_BTYPE_INT8)
      th_store8_imm_or_reg(src_reg, (uint32_t)addr_r, abs_off, sign);
    else if (btype == IROP_BTYPE_INT16)
      th_store16_imm_or_reg(src_reg, (uint32_t)addr_r, abs_off, sign);
    else
      th_store32_imm_or_reg_ex(src_reg, (uint32_t)addr_r, abs_off, sign, (uint32_t)1u << (uint32_t)src_reg);
    break;
  }

  case MACH_OP_IMM:
  {
    /* Store to an absolute address — e.g. *(volatile uint32_t*)0xABCD = val */
    int addr_r = mach_alloc_scratch(&ctx, (uint32_t)1u << (uint32_t)src_reg);
    tcc_machine_load_constant(addr_r, PREG_REG_NONE, dest.u.imm.val, 0, NULL);
    if (btype == IROP_BTYPE_INT8)
      th_store8_imm_or_reg(src_reg, (uint32_t)addr_r, 0, 0);
    else if (btype == IROP_BTYPE_INT16)
      th_store16_imm_or_reg(src_reg, (uint32_t)addr_r, 0, 0);
    else
      th_store32_imm_or_reg_ex(src_reg, (uint32_t)addr_r, 0, 0, (uint32_t)1u << (uint32_t)src_reg);
    break;
  }

  case MACH_OP_FRAME_ADDR:
  {
    /* Store to a frame-relative address; equivalent to MACH_OP_SPILL but via addr computation */
    int addr_r = mach_alloc_scratch(&ctx, (uint32_t)1u << (uint32_t)src_reg);
    tcc_machine_addr_of_stack_slot(addr_r, dest.u.frame.offset, 0 /* not param */);
    if (btype == IROP_BTYPE_INT8)
      th_store8_imm_or_reg(src_reg, (uint32_t)addr_r, 0, 0);
    else if (btype == IROP_BTYPE_INT16)
      th_store16_imm_or_reg(src_reg, (uint32_t)addr_r, 0, 0);
    else
      th_store32_imm_or_reg_ex(src_reg, (uint32_t)addr_r, 0, 0, (uint32_t)1u << (uint32_t)src_reg);
    break;
  }

  case MACH_OP_CHAIN_REL:
  {
    /* Captured variable: store to parent frame via static chain. */
    ScratchRegAlloc chain_scratch = {0};
    int chain_used = 0;
    int base = resolve_chain_base(tcc_state->ir, dest.u.chain.chain_index, (1u << (uint32_t)src_reg), &chain_scratch,
                                  &chain_used);
    int32_t off = dest.u.chain.offset;
    int sign = (off < 0), abs_off = sign ? (int)(-off) : (int)off;
    if (btype == IROP_BTYPE_INT8)
      th_store8_imm_or_reg(src_reg, (uint32_t)base, abs_off, sign);
    else if (btype == IROP_BTYPE_INT16)
      th_store16_imm_or_reg(src_reg, (uint32_t)base, abs_off, sign);
    else
      th_store32_imm_or_reg_ex(src_reg, (uint32_t)base, abs_off, sign,
                               (1u << (uint32_t)src_reg) | (1u << (uint32_t)base));
    if (chain_used)
      restore_scratch_reg(&chain_scratch);
    break;
  }

  default:
    tcc_ice("store_mop: unhandled dest kind %d", (int)dest.kind);
  }

  mach_release_all(&ctx);
}

/* Indexed load: dest = *(base + (index << scale))
 * Generates: LDR dest, [base, index, LSL #scale]
 */
ST_FUNC void tcc_gen_machine_load_indexed_mop(MachineOperand dest, MachineOperand base, MachineOperand index,
                                              MachineOperand scale, TccIrOp op)
{
  MachineCodegenContext ctx = {0};
  (void)op;

  int shift_amount = (scale.kind == MACH_OP_IMM) ? (int)scale.u.imm.val : 2;
  if (shift_amount < 0 || shift_amount > 31)
    shift_amount = 2;

  /* Fast path: base is &local + constant index — fold into SP/FP-relative load.
   * Mirrors the store_indexed FRAME_ADDR fast path. */
  if (!dest.is_64bit && shift_amount == 0 && index.kind == MACH_OP_IMM &&
      base.kind == MACH_OP_FRAME_ADDR && !base.needs_deref)
  {
    int combined = base.u.frame.offset + (int)index.u.imm.val;
    int adjusted = fp_adjust_local_offset(combined, 0);
    int sign = (adjusted < 0);
    int abs_off = sign ? -adjusted : adjusted;
    if (abs_off <= 4095)
    {
      const int dest_reg = mach_get_dest_reg(&ctx, &dest, 0);
      const int base_reg = tcc_state->need_frame_pointer ? R_FP : R_SP;
      load_from_base(dest_reg, PREG_REG_NONE, dest.btype, (int)dest.is_unsigned, abs_off, sign, (uint32_t)base_reg);
      mach_writeback_dest(&dest, dest_reg);
      mach_release_all(&ctx);
      return;
    }
  }

  /* Fast path: constant-displacement load (scale == 0 and index is an immediate).
   * Generated by the displacement-fusion pass when folding `ADD base,#imm; LOAD *`
   * into a single `LDR dest,[base,#imm]`, matching GCC's addressing-mode output. */
  if (!dest.is_64bit && shift_amount == 0 && index.kind == MACH_OP_IMM)
  {
    int imm = (int)index.u.imm.val;
    int sign = (imm < 0);
    int abs_off = sign ? -imm : imm;
    if (abs_off <= 4095)
    {
      const int dest_reg = mach_get_dest_reg(&ctx, &dest, 0);
      uint32_t excl = (1u << (uint32_t)dest_reg);
      int base_reg = mach_ensure_in_reg(&ctx, &base, excl);
      load_from_base(dest_reg, PREG_REG_NONE, dest.btype, (int)dest.is_unsigned, abs_off, sign, (uint32_t)base_reg);
      mach_writeback_dest(&dest, dest_reg);
      mach_release_all(&ctx);
      return;
    }
  }

  /* scale 0 → no shift: use THUMB_SHIFT_NONE so the 16-bit T1 register-offset
   * encoding (all-low regs) can be selected instead of the wide T32 form. */
  thumb_shift shift = (shift_amount == 0)
                          ? (thumb_shift){.type = THUMB_SHIFT_NONE, .value = 0, .mode = THUMB_SHIFT_IMMEDIATE}
                          : (thumb_shift){.type = THUMB_SHIFT_LSL, .value = (uint32_t)shift_amount, .mode = THUMB_SHIFT_IMMEDIATE};

  /* Fast path: 64-bit constant-displacement load using LDRD [base, #imm].
   * LDRD supports word-aligned offsets in range [-1020, 1020].
   * base.underalign_hint (packed-derived address): skip — LDRD faults on
   * unaligned addresses; the generic path below emits an LDR pair instead. */
  if (dest.is_64bit && shift_amount == 0 && index.kind == MACH_OP_IMM && base.align4 && !base.underalign_hint)
  {
    int imm = (int)index.u.imm.val;
    int sign = (imm < 0);
    int abs_off = sign ? -imm : imm;
    if (abs_off <= 1020 && (abs_off & 3) == 0)
    {
      const bool dest_is_reg = (dest.kind == MACH_OP_REG && !dest.needs_deref);
      int dest_lo, dest_hi;
      MachineOperand dest_hi_mop = {0};
      uint32_t excl = 0;

      if (dest_is_reg)
      {
        dest_lo = dest.u.reg.r0;
        if (!thumb_is_hw_reg(dest.u.reg.r1))
          tcc_error("load_indexed_mop: 64-bit dest has invalid r1=%d (r0=%d) — "
                    "register allocator must produce a valid pair",
                    dest.u.reg.r1, dest.u.reg.r0);
        dest_hi = dest.u.reg.r1;
        excl = (1u << (uint32_t)dest_lo) | (1u << (uint32_t)dest_hi);
      }
      else
      {
        dest_lo = mach_get_dest_reg(&ctx, &dest, 0);
        excl = (1u << (uint32_t)dest_lo);
        dest_hi_mop = mach_make_hi_half(&dest);
        dest_hi = mach_get_dest_reg(&ctx, &dest_hi_mop, excl);
        excl |= (1u << (uint32_t)dest_hi);
      }

      int base_reg = mach_ensure_in_reg(&ctx, &base, excl);
      uint32_t puw = sign ? 4u : 6u;
      ot_check(th_ldrd_imm((uint32_t)dest_lo, (uint32_t)dest_hi, (uint32_t)base_reg, abs_off, puw));
      if (!dest_is_reg)
      {
        MachineOperand dest_lo_mop = mach_make_lo_half(&dest);
        mach_writeback_dest(&dest_lo_mop, dest_lo);
        mach_writeback_dest(&dest_hi_mop, dest_hi);
      }
      mach_release_all(&ctx);
      return;
    }
  }

  /* 64-bit indexed load: compute EA = base + index<<shift into scratch, then LDRD. */
  if (dest.is_64bit)
  {
    const bool dest_is_reg = (dest.kind == MACH_OP_REG && !dest.needs_deref);
    int dest_lo, dest_hi;
    MachineOperand dest_hi_mop = {0};
    uint32_t excl = 0;

    if (dest_is_reg)
    {
      dest_lo = dest.u.reg.r0;
      if (!thumb_is_hw_reg(dest.u.reg.r1))
        tcc_error("load_indexed_mop: 64-bit dest has invalid r1=%d (r0=%d) — "
                  "register allocator must produce a valid pair",
                  dest.u.reg.r1, dest.u.reg.r0);
      dest_hi = dest.u.reg.r1;
      excl = (1u << (uint32_t)dest_lo) | (1u << (uint32_t)dest_hi);
    }
    else
    {
      dest_lo = mach_get_dest_reg(&ctx, &dest, 0);
      excl = (1u << (uint32_t)dest_lo);
      dest_hi_mop = mach_make_hi_half(&dest);
      dest_hi = mach_get_dest_reg(&ctx, &dest_hi_mop, excl);
      excl |= (1u << (uint32_t)dest_hi);
    }

    if (index.kind == MACH_OP_REG && !index.needs_deref)
      excl |= (1u << (uint32_t)index.u.reg.r0);
    int base_reg = mach_ensure_in_reg(&ctx, &base, excl);
    excl |= (1u << (uint32_t)base_reg);
    int index_reg = mach_ensure_in_reg(&ctx, &index, excl);
    excl |= (1u << (uint32_t)index_reg);
    int ea_r = mach_alloc_scratch(&ctx, excl);
    ot_check(th_add_reg((uint32_t)ea_r, (uint32_t)base_reg, (uint32_t)index_reg, flags_safe(), shift,
                        ENFORCE_ENCODING_NONE));
    if (!base.align4 || base.underalign_hint)
    {
      /* Packed-derived address: LDRD faults on unaligned addresses, plain LDR
       * tolerates them (UNALIGN_TRP=0).  load_from_base with a general base
       * register emits the safe LDR pair. */
      load_from_base(dest_lo, dest_hi, IROP_BTYPE_INT64, 0, 0, 0, (uint32_t)ea_r);
    }
    else
      ot_check(th_ldrd_imm((uint32_t)dest_lo, (uint32_t)dest_hi, (uint32_t)ea_r, 0, 5));
    if (!dest_is_reg)
    {
      MachineOperand dest_lo_mop = mach_make_lo_half(&dest);
      mach_writeback_dest(&dest_lo_mop, dest_lo);
      mach_writeback_dest(&dest_hi_mop, dest_hi);
    }
    mach_release_all(&ctx);
    return;
  }

  /* Use mach_get_dest_reg so MACH_OP_SPILL / MACH_OP_PARAM_STACK dests get a
   * scratch + writeback (previously `dest.u.reg.r0` was read unconditionally,
   * aliasing with spill.offset and emitting an invalid encoding). */
  const int dest_reg = mach_get_dest_reg(&ctx, &dest, 0);
  const int btype = dest.btype;
  const int is_unsigned = (int)dest.is_unsigned;

  uint32_t excl = (1u << (uint32_t)dest_reg);
  if (index.kind == MACH_OP_REG && !index.needs_deref)
    excl |= (1u << (uint32_t)index.u.reg.r0);
  int base_reg = mach_ensure_in_reg(&ctx, &base, excl);
  excl |= (1u << (uint32_t)base_reg);
  int index_reg = mach_ensure_in_reg(&ctx, &index, excl);

  if (btype == IROP_BTYPE_INT8)
  {
    if (is_unsigned)
      ot_check(th_ldrb_reg(dest_reg, base_reg, index_reg, shift, ENFORCE_ENCODING_NONE));
    else
      ot_check(th_ldrsb_reg(dest_reg, base_reg, index_reg, shift, ENFORCE_ENCODING_NONE));
  }
  else if (btype == IROP_BTYPE_INT16)
  {
    if (is_unsigned)
      ot_check(th_ldrh_reg(dest_reg, base_reg, index_reg, shift, ENFORCE_ENCODING_NONE));
    else
      ot_check(th_ldrsh_reg(dest_reg, base_reg, index_reg, shift, ENFORCE_ENCODING_NONE));
  }
  else
  {
    ot_check(th_ldr_reg(dest_reg, base_reg, index_reg, shift, ENFORCE_ENCODING_NONE));
  }
  mach_writeback_dest(&dest, dest_reg);
  mach_release_all(&ctx);
}

/* Indexed store: *(base + (index << scale)) = value
 * Generates: STR value, [base, index, LSL #scale]
 */
ST_FUNC void tcc_gen_machine_store_indexed_mop(MachineOperand base, MachineOperand index, MachineOperand scale,
                                               MachineOperand value, TccIrOp op)
{
  MachineCodegenContext ctx = {0};
  (void)op;

  int shift_amount = (scale.kind == MACH_OP_IMM) ? (int)scale.u.imm.val : 2;
  if (shift_amount < 0 || shift_amount > 31)
    shift_amount = 2;

  /* Fast path: base is &local + constant index — fold into SP/FP-relative store.
   * Avoids emitting a separate `ADD base, sp, #frame_off` LEA before the STR,
   * cutting one instruction per access in dense local-array initialization. */
  if (!value.is_64bit && shift_amount == 0 && index.kind == MACH_OP_IMM &&
      base.kind == MACH_OP_FRAME_ADDR && !base.needs_deref)
  {
    int combined = base.u.frame.offset + (int)index.u.imm.val;
    int adjusted = fp_adjust_local_offset(combined, 0);
    int sign = (adjusted < 0);
    int abs_off = sign ? -adjusted : adjusted;
    if (abs_off <= 4095)
    {
      const int btype = value.btype;
      const int base_reg = tcc_state->need_frame_pointer ? R_FP : R_SP;
      int value_reg = mach_ensure_in_reg(&ctx, &value, 0);
      if (btype == IROP_BTYPE_INT8)
        th_store8_imm_or_reg(value_reg, (uint32_t)base_reg, abs_off, sign);
      else if (btype == IROP_BTYPE_INT16)
        th_store16_imm_or_reg(value_reg, (uint32_t)base_reg, abs_off, sign);
      else
        th_store32_imm_or_reg_ex(value_reg, (uint32_t)base_reg, abs_off, sign,
                                 (1u << (uint32_t)value_reg) | (1u << (uint32_t)base_reg));
      mach_release_all(&ctx);
      return;
    }
  }

  /* Fast path: constant-displacement store (scale == 0 and index is an immediate).
   * Mirrors the load_indexed fast path; emits `STR value,[base,#imm]`. */
  if (!value.is_64bit && shift_amount == 0 && index.kind == MACH_OP_IMM)
  {
    int imm = (int)index.u.imm.val;
    int sign = (imm < 0);
    int abs_off = sign ? -imm : imm;
    if (abs_off <= 4095)
    {
      const int btype = value.btype;
      int value_reg = mach_ensure_in_reg(&ctx, &value, mach_reg_excl(&base));
      uint32_t excl = (1u << (uint32_t)value_reg);
      int base_reg = mach_ensure_in_reg(&ctx, &base, excl);
      if (btype == IROP_BTYPE_INT8)
        th_store8_imm_or_reg(value_reg, (uint32_t)base_reg, abs_off, sign);
      else if (btype == IROP_BTYPE_INT16)
        th_store16_imm_or_reg(value_reg, (uint32_t)base_reg, abs_off, sign);
      else
        th_store32_imm_or_reg_ex(value_reg, (uint32_t)base_reg, abs_off, sign,
                                 (1u << (uint32_t)value_reg) | (1u << (uint32_t)base_reg));
      mach_release_all(&ctx);
      return;
    }
  }

  /* scale 0 → no shift: use THUMB_SHIFT_NONE so the 16-bit T1 register-offset
   * encoding (all-low regs) can be selected instead of the wide T32 form. */
  thumb_shift shift = (shift_amount == 0)
                          ? (thumb_shift){.type = THUMB_SHIFT_NONE, .value = 0, .mode = THUMB_SHIFT_IMMEDIATE}
                          : (thumb_shift){.type = THUMB_SHIFT_LSL, .value = (uint32_t)shift_amount, .mode = THUMB_SHIFT_IMMEDIATE};

  /* Fast path: 64-bit constant-displacement store using STRD [base, #imm].
   * base.underalign_hint (packed-derived address): skip — STRD faults on
   * unaligned addresses; the generic path below emits an STR pair instead. */
  if (value.is_64bit && shift_amount == 0 && index.kind == MACH_OP_IMM && base.align4 && !base.underalign_hint)
  {
    int imm = (int)index.u.imm.val;
    int sign = (imm < 0);
    int abs_off = sign ? -imm : imm;
    if (abs_off <= 1020 && (abs_off & 3) == 0)
    {
      MachineOperand val_lo = mach_make_lo_half(&value);
      val_lo.btype = IROP_BTYPE_INT32;
      MachineOperand val_hi = mach_make_hi_half(&value);
      val_hi.btype = IROP_BTYPE_INT32;
      const uint32_t base_excl = mach_reg_excl(&base);
      const int lo_reg = mach_ensure_in_reg(&ctx, &val_lo, base_excl);
      uint32_t excl = (1u << (uint32_t)lo_reg);
      const int hi_reg = mach_ensure_in_reg(&ctx, &val_hi, excl | base_excl);
      excl |= (1u << (uint32_t)hi_reg);
      int base_reg = mach_ensure_in_reg(&ctx, &base, excl);
      uint32_t puw = sign ? 4u : 6u;
      ot_check(th_strd_imm((uint32_t)lo_reg, (uint32_t)hi_reg, (uint32_t)base_reg, abs_off, puw));
      mach_release_all(&ctx);
      return;
    }
  }

  /* 64-bit indexed store: compute EA = base + index<<shift into scratch, then STRD. */
  if (value.is_64bit)
  {
    MachineOperand val_lo = mach_make_lo_half(&value);
    val_lo.btype = IROP_BTYPE_INT32;
    MachineOperand val_hi = mach_make_hi_half(&value);
    val_hi.btype = IROP_BTYPE_INT32;
    uint32_t excl = 0;
    if (base.kind == MACH_OP_REG && !base.needs_deref)
      excl |= (1u << (uint32_t)base.u.reg.r0);
    if (index.kind == MACH_OP_REG && !index.needs_deref)
      excl |= (1u << (uint32_t)index.u.reg.r0);
    const int lo_reg = mach_ensure_in_reg(&ctx, &val_lo, excl);
    excl |= (1u << (uint32_t)lo_reg);
    const int hi_reg = mach_ensure_in_reg(&ctx, &val_hi, excl);
    excl |= (1u << (uint32_t)hi_reg);
    int base_reg = mach_ensure_in_reg(&ctx, &base, excl);
    excl |= (1u << (uint32_t)base_reg);
    int index_reg = mach_ensure_in_reg(&ctx, &index, excl);
    excl |= (1u << (uint32_t)index_reg);
    int ea_r = mach_alloc_scratch(&ctx, excl);
    ot_check(th_add_reg((uint32_t)ea_r, (uint32_t)base_reg, (uint32_t)index_reg, flags_safe(), shift,
                        ENFORCE_ENCODING_NONE));
    if (!base.align4 || base.underalign_hint)
    {
      /* Packed-derived address: STRD faults on unaligned addresses, plain STR
       * tolerates them (UNALIGN_TRP=0).  Store the halves separately. */
      excl |= (1u << (uint32_t)ea_r);
      th_store32_imm_or_reg_ex(lo_reg, (uint32_t)ea_r, 0, 0, excl);
      th_store32_imm_or_reg_ex(hi_reg, (uint32_t)ea_r, 4, 0, excl);
    }
    else
      ot_check(th_strd_imm((uint32_t)lo_reg, (uint32_t)hi_reg, (uint32_t)ea_r, 0, 6));
    mach_release_all(&ctx);
    return;
  }

  const int btype = value.btype;

  uint32_t excl = 0;
  if (base.kind == MACH_OP_REG && !base.needs_deref)
    excl |= (1u << (uint32_t)base.u.reg.r0);
  if (index.kind == MACH_OP_REG && !index.needs_deref)
    excl |= (1u << (uint32_t)index.u.reg.r0);
  int value_reg = mach_ensure_in_reg(&ctx, &value, excl);
  excl |= (1u << (uint32_t)value_reg);
  int base_reg = mach_ensure_in_reg(&ctx, &base, excl);
  excl |= (1u << (uint32_t)base_reg);
  int index_reg = mach_ensure_in_reg(&ctx, &index, excl);

  if (btype == IROP_BTYPE_INT8)
    ot_check(th_strb_reg(value_reg, base_reg, index_reg, shift, ENFORCE_ENCODING_NONE));
  else if (btype == IROP_BTYPE_INT16)
    ot_check(th_strh_reg(value_reg, base_reg, index_reg, shift, ENFORCE_ENCODING_NONE));
  else
    ot_check(th_str_reg(value_reg, base_reg, index_reg, shift, ENFORCE_ENCODING_NONE));

  mach_release_all(&ctx);
}

/* Post-increment load: dest = *ptr; ptr += offset
 * Generates: LDR dest, [ptr], #offset  (puw=3: post-index, add, writeback)
 */
ST_FUNC void tcc_gen_machine_load_postinc_mop(MachineOperand dest, MachineOperand ptr, MachineOperand offset,
                                              TccIrOp op)
{
  MachineCodegenContext ctx = {0};
  (void)op;

  int offset_imm = (offset.kind == MACH_OP_IMM) ? (int)offset.u.imm.val : 4;
  if (offset_imm < 0 || offset_imm > 255)
  {
    mach_release_all(&ctx);
    tcc_ice("post-increment offset %d out of range (0-255)", offset_imm);
    return;
  }
  const uint32_t puw = 3; /* post-index (p=0), add (u=1), writeback (w=1) */

  /* 64-bit post-increment load: LDRD dest_lo, dest_hi, [ptr], #offset */
  if (dest.is_64bit)
  {
    const int dest_lo = mach_get_dest_reg(&ctx, &dest, 0);
    MachineOperand dest_hi_mop = mach_make_hi_half(&dest);
    const int dest_hi = mach_get_dest_reg(&ctx, &dest_hi_mop, (1u << (uint32_t)dest_lo));
    uint32_t excl = (1u << (uint32_t)dest_lo) | (1u << (uint32_t)dest_hi);
    int ptr_reg = mach_ensure_in_reg(&ctx, &ptr, excl);
    if (ptr.align4 && !ptr.underalign_hint && !(offset_imm & 3))
      ot_check(th_ldrd_imm((uint32_t)dest_lo, (uint32_t)dest_hi, (uint32_t)ptr_reg, offset_imm, puw));
    else
    {
      load_from_base(dest_lo, dest_hi, IROP_BTYPE_INT64, 0, 0, 0, (uint32_t)ptr_reg);
      ot_check(th_add_imm(ptr_reg, ptr_reg, offset_imm, flags_safe(), ENFORCE_ENCODING_NONE));
    }
    mach_writeback_dest(&dest_hi_mop, dest_hi);
    mach_writeback_dest(&dest, dest_lo);
    mach_release_all(&ctx);
    return;
  }

  const int dest_reg = mach_get_dest_reg(&ctx, &dest, 0);
  const int btype = dest.btype;
  const int is_unsigned = (int)dest.is_unsigned;

  uint32_t excl = (1u << (uint32_t)dest_reg);
  int ptr_reg = mach_ensure_in_reg(&ctx, &ptr, excl);

  if (btype == IROP_BTYPE_INT8)
  {
    if (is_unsigned)
      ot_check(th_ldrb_imm(dest_reg, ptr_reg, offset_imm, puw, ENFORCE_ENCODING_NONE));
    else
      ot_check(th_ldrsb_imm(dest_reg, ptr_reg, offset_imm, puw, ENFORCE_ENCODING_NONE));
  }
  else if (btype == IROP_BTYPE_INT16)
  {
    if (is_unsigned)
      ot_check(th_ldrh_imm(dest_reg, ptr_reg, offset_imm, puw, ENFORCE_ENCODING_NONE));
    else
      ot_check(th_ldrsh_imm(dest_reg, ptr_reg, offset_imm, puw, ENFORCE_ENCODING_NONE));
  }
  else
  {
    ot_check_ldr_imm(dest_reg, ptr_reg, offset_imm, puw, ENFORCE_ENCODING_NONE);
  }
  mach_writeback_dest(&dest, dest_reg);
  mach_release_all(&ctx);
}

/* Post-increment store: *ptr = value; ptr += offset
 * Generates: STR value, [ptr], #offset  (puw=3: post-index, add, writeback)
 */
ST_FUNC void tcc_gen_machine_store_postinc_mop(MachineOperand ptr, MachineOperand value, MachineOperand offset,
                                               TccIrOp op)
{
  MachineCodegenContext ctx = {0};
  (void)op;

  int offset_imm = (offset.kind == MACH_OP_IMM) ? (int)offset.u.imm.val : 4;
  if (offset_imm < 0 || offset_imm > 255)
  {
    mach_release_all(&ctx);
    tcc_ice("post-increment offset %d out of range (0-255)", offset_imm);
    return;
  }
  const uint32_t puw = 3; /* post-index (p=0), add (u=1), writeback (w=1) */

  /* 64-bit post-increment store: STRD lo, hi, [ptr], #offset */
  if (value.is_64bit)
  {
    MachineOperand val_lo = mach_make_lo_half(&value);
    val_lo.btype = IROP_BTYPE_INT32;
    MachineOperand val_hi = mach_make_hi_half(&value);
    val_hi.btype = IROP_BTYPE_INT32;
    const uint32_t ptr_excl = mach_reg_excl(&ptr);
    const int lo_reg = mach_ensure_in_reg(&ctx, &val_lo, ptr_excl);
    uint32_t excl = (1u << (uint32_t)lo_reg);
    const int hi_reg = mach_ensure_in_reg(&ctx, &val_hi, excl | ptr_excl);
    excl |= (1u << (uint32_t)hi_reg);
    int ptr_reg = mach_ensure_in_reg(&ctx, &ptr, excl);
    if (ptr.align4 && !ptr.underalign_hint && !(offset_imm & 3))
      ot_check(th_strd_imm((uint32_t)lo_reg, (uint32_t)hi_reg, (uint32_t)ptr_reg, offset_imm, puw));
    else
    {
      excl |= (1u << (uint32_t)ptr_reg);
      th_store32_imm_or_reg_ex(lo_reg, (uint32_t)ptr_reg, 0, 0, excl);
      th_store32_imm_or_reg_ex(hi_reg, (uint32_t)ptr_reg, 4, 0, excl);
      ot_check(th_add_imm(ptr_reg, ptr_reg, offset_imm, flags_safe(), ENFORCE_ENCODING_NONE));
    }
    mach_release_all(&ctx);
    return;
  }

  const int btype = value.btype;

  int value_reg = mach_ensure_in_reg(&ctx, &value, mach_reg_excl(&ptr));
  uint32_t excl = (1u << (uint32_t)value_reg);
  int ptr_reg = mach_ensure_in_reg(&ctx, &ptr, excl);

  if (btype == IROP_BTYPE_INT8)
    ot_check(th_strb_imm(value_reg, ptr_reg, offset_imm, puw, ENFORCE_ENCODING_NONE));
  else if (btype == IROP_BTYPE_INT16)
    ot_check(th_strh_imm(value_reg, ptr_reg, offset_imm, puw, ENFORCE_ENCODING_NONE));
  else
    ot_check_str_imm(value_reg, ptr_reg, offset_imm, puw, ENFORCE_ENCODING_NONE);

  mach_release_all(&ctx);
}
