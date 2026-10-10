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

/* Thumb-2 code generator: call lowering -- argument moves and stack
 * arguments, the function-call generator, block copies, VLA, setjmp/longjmp,
 * and __builtin_apply. */

#include "arm-thumb-gen.h"
#include "source/backend/arch/arm/thumb/thop_ldaex.h"
#include "source/backend/arch/arm/thumb/thop_ldrex.h"
#include "source/backend/arch/arm/thumb/thop_mrs.h"
#include "source/backend/arch/arm/thumb/thop_mem_exclusive.h"

static int get_struct_base_addr_mop(const MachineOperand *mop, int default_reg);
static int find_call_scratch(uint32_t extra_exclude, uint32_t arg_move_dst_mask);
static int find_call_scratch_free(uint32_t extra_exclude, uint32_t arg_move_dst_mask);

/* Is the by-value struct source ADDRESS provably 4-byte aligned?
 *
 * `struct_src_align` is the C type's alignment, and it is 1 for every packed
 * aggregate — tinycc's own 9-byte IROperand, which dominates these call sites,
 * being the obvious one.  What LDRD and LDM actually require is an aligned
 * address, so for a frame-resident source ask the offset instead of the type.
 *
 * Only the low two bits matter, and they survive the frame bias: SP is 8-byte
 * aligned and FP 4-byte aligned at steady state, and every term
 * fp_adjust_local_offset() adds (allocated_stack_size, scratch_push_sp_bias)
 * is a multiple of 4.  Testing the RAW offset therefore
 * gives the same answer as testing the adjusted one, and — unlike the adjusted
 * one, whose scratch_push_sp_bias() term is dry-run gated — it reads the same
 * in the rehearsal and the real pass, so it cannot desync the two code sizes.
 *
 * Incoming stack parameters are aligned by construction (arm_aapcs.c floors
 * every argument slot at 4 and offset_to_args is a multiple of 4), but the
 * check is written out rather than assumed. */
static bool struct_src_addr_aligned4(const ThumbArgMove *m)
{
  if (m->struct_src_align >= 4)
    return true;
  if (m->mop.needs_deref)
    return false; /* address comes from memory — nothing provable */
  switch (m->mop.kind)
  {
  case MACH_OP_SPILL:
    return (m->mop.u.spill.offset & 3) == 0;
  case MACH_OP_FRAME_ADDR:
    return (m->mop.u.frame.offset & 3) == 0;
  case MACH_OP_PARAM_STACK:
    return ((param_frame_offset(m->mop.u.param.offset)) & 3) == 0;
  default:
    return false;
  }
}

static void thumb_emit_arg_move(const ThumbArgMove *m)
{
  if (m->kind == THUMB_ARG_MOVE_REG)
  {
    if (m->src_reg == m->dst_reg)
      return;
    ot_check_mov_reg(m->dst_reg, m->src_reg, flags_safe(), THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE,
                     false);
    return;
  }

  if (m->kind == THUMB_ARG_MOVE_LOCAL_ADDR)
  {
    /* Compute address of local variable: dst = fp + offset */
    tcc_machine_addr_of_stack_slot(m->dst_reg, m->local_offset, m->local_is_param);
    return;
  }

  if (m->kind == THUMB_ARG_MOVE_STRUCT)
  {
    /* Load struct words into consecutive registers.
     * The mop contains the struct operand; get its base address. */
    int word_count = m->struct_word_count;
    int base_dst = m->dst_reg;

    /* LDRD fast path for the common 2-word (8-byte) aggregate case sourced
     * directly from a stack-backed location.  Mirrors the LDRD path used by
     * THUMB_ARG_MOVE_MOP for 64-bit scalars — Thumb-2 LDRD requires natural
     * 4-byte alignment, which spill slots and the caller param stack both
     * provide.  Skips the scratch + per-word loads that would otherwise
     * emit `add.w ip, sp, #N; ldr lo, [ip]; ldr hi, [ip, #4]` (3 insts).
     *
     * LDRD writes Rt before Rt2, so Rt2 (dst+1) must not equal the base
     * register, otherwise the 2nd half reads from a clobbered base. */
    if (word_count == 2 && !m->mop.needs_deref &&
        (m->mop.kind == MACH_OP_SPILL || m->mop.kind == MACH_OP_PARAM_STACK))
    {
      int raw_off =
          (m->mop.kind == MACH_OP_SPILL) ? m->mop.u.spill.offset : param_frame_offset(m->mop.u.param.offset);
      int adjusted = (m->mop.kind == MACH_OP_SPILL) ? fp_adjust_local_offset(raw_off, 0) : raw_off;
      int ldrd_base = tcc_state->need_frame_pointer ? R_FP : R_SP;
      int ldrd_sign = (adjusted < 0);
      int ldrd_abs_off = ldrd_sign ? -adjusted : adjusted;
      int dst_hi = base_dst + 1;
      if (dst_hi != ldrd_base && base_dst != ldrd_base &&
          try_ldrd_pair(base_dst, dst_hi, ldrd_base, ldrd_abs_off, ldrd_sign))
      {
        return;
      }
    }

    /* A struct in the frame loads word by word straight off sp/fp, without
     * first building its address in a scratch register
     * (`add.w ip, sp, #N; ldr r1, [ip]`).  The sources and offsets are the
     * ones get_struct_base_addr_mop would address: only a spill slot's
     * needs_deref means the slot holds a pointer.  Taken when every word encodes
     * and the loads are no larger than the address + LDM form (8 bytes). */
    if (base_dst >= 0 && ((m->mop.kind == MACH_OP_SPILL && !m->mop.needs_deref) ||
                          m->mop.kind == MACH_OP_PARAM_STACK || m->mop.kind == MACH_OP_FRAME_ADDR))
    {
      int off;
      if (m->mop.kind == MACH_OP_PARAM_STACK)
        off = param_frame_offset(m->mop.u.param.offset);
      else
        off = fp_adjust_local_offset(m->mop.kind == MACH_OP_SPILL ? m->mop.u.spill.offset : m->mop.u.frame.offset, 0);
      int base = tcc_state->need_frame_pointer ? R_FP : R_SP;
      int bytes = 0;
      for (int w = 0; w < word_count && bytes >= 0; ++w)
      {
        int o = off + 4 * w;
        int size = base_dst + w == base ? 0
                                         : th_ldr_imm(base_dst + w, base, o < 0 ? -o : o, o < 0 ? 4 : 6,
                                                      ENFORCE_ENCODING_NONE)
                                               .size;
        bytes = size ? bytes + size : -1;
      }
      /* Three or more low registers: build the address in the last of them
       * and load the run with a 16-bit LDM, which does no writeback when its
       * base is in the list -- `add r3, sp, #N; ldm r3, {r0-r3}`, 4 bytes where
       * four LDRs take 8. */
      const int last = base_dst + word_count - 1;
      if (word_count >= 3 && last <= R7 && !(base >= base_dst && base <= last) && (off & 3) == 0 &&
          struct_src_addr_aligned4(m))
      {
        thumb_opcode addr = off < 0 ? th_sub_imm(last, base, -off, flags_safe(), ENFORCE_ENCODING_NONE)
                                    : th_add_imm(last, base, off, flags_safe(), ENFORCE_ENCODING_NONE);
        if (addr.size && (bytes < 0 || addr.size + 2 < bytes))
        {
          ot_check(addr);
          const uint32_t regs = ((1u << word_count) - 1u) << base_dst;
          ot_check((thumb_opcode){.size = 2, .opcode = 0xC800u | ((uint32_t)last << 8) | regs});
          return;
        }
      }
      if (bytes > 0 && (word_count <= 2 || bytes <= 8))
      {
        for (int w = 0; w < word_count; ++w)
        {
          int o = off + 4 * w;
          load_word_from_base(base_dst + w, base, o < 0 ? -o : o, o < 0);
        }
        return;
      }
    }

    /* Get the struct base address into a scratch register */
    ScratchRegAlloc struct_scratch = get_scratch_reg_with_save(0);
    int base_addr_reg = get_struct_base_addr_mop(&m->mop, struct_scratch.reg);

    /* Load each word from the struct into consecutive target registers.
     * Adjacent word pairs use LDRD when the source ADDRESS is 4-byte aligned
     * (LDRD faults otherwise) and neither destination register aliases the base
     * (LDRD writes Rt then Rt2; an alias would read a clobbered base on the
     * fallback path / be unsafe). */
    bool src_aligned = struct_src_addr_aligned4(m);

    /* A single LDMIA.W replaces the whole per-word load sequence.  It only pays
     * from three words up: at two, the LDRD in the loop below is already one
     * 4-byte instruction.
     *
     * A base inside the destination range would be clobbered mid-transfer by
     * the loaded word, so keep it out.  PC and SP must stay out of the list
     * too: PC would turn the load into a branch. */
    int last_dst = base_dst + word_count - 1;
    if (word_count >= 3 && src_aligned && base_addr_reg != R_SP && base_dst >= 0 && last_dst <= R_IP &&
        !(base_addr_reg >= base_dst && base_addr_reg <= last_dst))
    {
      uint32_t regset = 0;
      for (int k = 0; k < word_count; ++k)
        regset |= 1u << (unsigned)(base_dst + k);
      ot_check(th_ldm((uint32_t)base_addr_reg, regset, 0 /* no writeback */, ENFORCE_ENCODING_NONE));
      restore_scratch_reg(&struct_scratch);
      return;
    }

    int w = 0;
    for (; w + 1 < word_count; )
    {
      int dst = base_dst + w;
      int dst_hi = base_dst + w + 1;
      int offset = w * 4;
      if (src_aligned && dst != base_addr_reg && dst_hi != base_addr_reg &&
          tcc_gen_machine_try_ldrd_base(dst, dst_hi, base_addr_reg, offset))
      {
        w += 2;
        continue;
      }
      /* Single-word load of this word; the next iteration handles w+1. */
      if (!load_word_from_base(dst, base_addr_reg, offset, 0))
      {
        ScratchRegAlloc off_scratch = get_scratch_reg_with_save((1u << base_addr_reg) | (1u << dst));
        load_immediate(off_scratch.reg, offset, NULL, false);
        ot_check(th_ldr_reg(dst, base_addr_reg, off_scratch.reg, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
        restore_scratch_reg(&off_scratch);
      }
      w += 1;
    }
    /* Trailing odd word. */
    for (; w < word_count; ++w)
    {
      int dst = base_dst + w;
      int offset = w * 4;
      if (!load_word_from_base(dst, base_addr_reg, offset, 0))
      {
        ScratchRegAlloc off_scratch = get_scratch_reg_with_save((1u << base_addr_reg) | (1u << dst));
        load_immediate(off_scratch.reg, offset, NULL, false);
        ot_check(th_ldr_reg(dst, base_addr_reg, off_scratch.reg, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
        restore_scratch_reg(&off_scratch);
      }
    }
    restore_scratch_reg(&struct_scratch);
    return;
  }

  if (m->kind == THUMB_ARG_MOVE_MOP)
  {
    /* Generic MachineOperand → register load.
     * Handles all MOP kinds (REG+deref, SPILL, PARAM_STACK, CHAIN_REL,
     * SYMBOL+deref, etc.) via mach_ensure_in_reg. */
    MachineCodegenContext mctx = {0};
    if (m->mop.is_64bit && m->dst_reg_hi != 0 && m->dst_reg_hi != PREG_REG_NONE)
    {
      if (m->mop.needs_deref && !mach_op_64_names_memory(&m->mop))
      {
        /* The operand holds a pointer (in reg, spill, etc.).  Load the
         * pointer into a register, then fetch lo/hi from [ptr+0]/[ptr+4].
         * mach_make_hi_half cannot handle this because it adjusts the
         * storage location (e.g. spill offset) instead of the deref offset.
         *
         * PARAM_STACK and CHAIN_REL are excluded (mach_op_64_names_memory):
         * mach_ensure_in_reg loads those directly from the caller's argument
         * area / the parent frame, so the mach_make_lo/hi_half path handles
         * them correctly. */
        int base;
        if (m->mop.kind == MACH_OP_REG)
        {
          base = m->mop.u.reg.r0;
        }
        else
        {
          MachineOperand addr = m->mop;
          addr.needs_deref = false;
          addr.is_64bit = false;
          addr.btype = IROP_BTYPE_INT32;
          uint32_t excl = (1u << m->dst_reg) | (1u << m->dst_reg_hi);
          base = mach_ensure_in_reg(&mctx, &addr, excl);
        }
        /* Proven-aligned access (mop.align4): LDRD straight into the pair —
         * Rt == Rn is fine without writeback, so no base preservation needed.
         * Otherwise the 64-bit load_from_base path, which preserves the base
         * when base == dst_reg (the lo-load would clobber it before the
         * hi-load can use it) and stays unaligned-safe. */
        if (!(m->mop.align4 && !m->mop.underalign_hint && try_ldrd_pair(m->dst_reg, m->dst_reg_hi, base, 0, 0)))
          load_from_base(m->dst_reg, m->dst_reg_hi, IROP_BTYPE_INT64, 0, 0, 0, (uint32_t)base);
      }
      else
      {
        /* Fast path: when the source is a stack-backed 64-bit value (spill
         * slot or caller's param stack area) and the destination is a valid
         * AAPCS register pair, emit a single LDRD straight into dst_reg /
         * dst_reg_hi, skipping the scratch + MOV sequence that the generic
         * lo/hi lowering below would produce.
         *
         * Stack spill slots and the caller-argument frame are guaranteed
         * 8-byte aligned (AAPCS stack_align = 8, spill slots obey type
         * alignment), so LDRD's 4-byte alignment requirement is satisfied. */
        int ldrd_base = -1;
        int ldrd_abs_off = 0;
        int ldrd_sign = 0;
        int ldrd_ok = 0;
        if (!m->mop.needs_deref && (m->mop.kind == MACH_OP_SPILL || m->mop.kind == MACH_OP_PARAM_STACK))
        {
          int raw_off = (m->mop.kind == MACH_OP_SPILL) ? m->mop.u.spill.offset : param_frame_offset(m->mop.u.param.offset);
          int adjusted = (m->mop.kind == MACH_OP_SPILL) ? fp_adjust_local_offset(raw_off, 0) : raw_off;
          ldrd_base = tcc_state->need_frame_pointer ? R_FP : R_SP;
          ldrd_sign = (adjusted < 0);
          ldrd_abs_off = ldrd_sign ? -adjusted : adjusted;
          ldrd_ok = 1;
        }
        if (ldrd_ok && try_ldrd_pair(m->dst_reg, m->dst_reg_hi, ldrd_base, ldrd_abs_off, ldrd_sign))
        {
          /* LDRD emitted. */
        }
        else
        {
          /* 64-bit: load lo and hi halves separately. */
          MachineOperand lo = mach_make_lo_half(&m->mop);
          MachineOperand hi = mach_make_hi_half(&m->mop);
          uint32_t excl = (1u << m->dst_reg) | (1u << m->dst_reg_hi);
          int r_lo = mach_ensure_in_reg(&mctx, &lo, excl);
          int r_hi = mach_ensure_in_reg(&mctx, &hi, excl | (1u << (uint32_t)r_lo));
          if (r_lo != m->dst_reg)
            ot_check_mov_reg(m->dst_reg, r_lo, flags_safe(), THUMB_SHIFT_DEFAULT,
                             ENFORCE_ENCODING_NONE, false);
          if (r_hi != m->dst_reg_hi)
            ot_check_mov_reg(m->dst_reg_hi, r_hi, flags_safe(), THUMB_SHIFT_DEFAULT,
                             ENFORCE_ENCODING_NONE, false);
        }
      }
    }
    else
    {
      /* 32-bit: prefer loading directly into dst_reg when the operand kind
       * permits it, bypassing the scratch + MOV sequence that
       * mach_ensure_in_reg would emit.  Kinds that need an extra
       * pointer-chain scratch beyond dst_reg (CHAIN_REL) fall through to
       * the generic path. */
      const MachineOperand *mop = &m->mop;
      const int dst = m->dst_reg;
      int handled = 0;

      switch (mop->kind)
      {
      case MACH_OP_NONE:
        tcc_machine_load_constant(dst, PREG_REG_NONE, 0, 0, NULL);
        handled = 1;
        break;

      case MACH_OP_REG:
        if (mop->needs_deref)
        {
          /* LDR dst, [r0]; legal even when r0 == dst (loaded value
           * just replaces the base). */
          load_from_base(dst, PREG_REG_NONE, mop->btype, (int)mop->is_unsigned, 0, 0,
                         (uint32_t)mop->u.reg.r0);
        }
        else if (mop->u.reg.r0 != dst)
        {
          ot_check_mov_reg(dst, mop->u.reg.r0, flags_safe(), THUMB_SHIFT_DEFAULT,
                           ENFORCE_ENCODING_NONE, false);
        }
        handled = 1;
        break;

      case MACH_OP_SPILL:
        if (!mop->needs_deref)
        {
          mach_load_slot(dst, mop);
        }
        else
        {
          /* LLOCAL: load pointer into dst, then dereference into dst. */
          tcc_machine_load_spill_slot(dst, mop->u.spill.offset);
          load_from_base(dst, PREG_REG_NONE, mop->btype, (int)mop->is_unsigned, 0, 0,
                         (uint32_t)dst);
        }
        handled = 1;
        break;

      case MACH_OP_PARAM_STACK:
      {
        const int adjusted = param_frame_offset(mop->u.param.offset);
        const int base_reg = tcc_state->need_frame_pointer ? R_FP : R_SP;
        const int sign = (adjusted < 0);
        const int abs_off = sign ? -adjusted : adjusted;
        load_from_base(dst, PREG_REG_NONE, mop->btype, (int)mop->is_unsigned, abs_off, sign,
                       (uint32_t)base_reg);
        handled = 1;
        break;
      }

      case MACH_OP_IMM:
        tcc_machine_load_constant(dst, PREG_REG_NONE, mop->u.imm.val, 0, NULL);
        /* `*(volatile T *)0x40000000`: the register's value, not its address. */
        if (mop->needs_deref)
          load_from_base(dst, PREG_REG_NONE, mop->btype, (int)mop->is_unsigned, 0, 0, (uint32_t)dst);
        handled = 1;
        break;

      case MACH_OP_FRAME_ADDR:
        if (!mop->needs_deref)
        {
          tcc_machine_addr_of_stack_slot(dst, mop->u.frame.offset, 0);
        }
        else
        {
          tcc_machine_addr_of_stack_slot(dst, mop->u.frame.offset, 0);
          load_from_base(dst, PREG_REG_NONE, mop->btype, (int)mop->is_unsigned, 0, 0,
                         (uint32_t)dst);
        }
        handled = 1;
        break;

      case MACH_OP_SYMBOL:
      {
        Sym *raw_sym = mop->u.sym.sym;
        Sym *sym = raw_sym ? validate_sym_for_reloc(raw_sym) : NULL;
        if (!mop->needs_deref)
        {
          tcc_machine_load_constant(dst, PREG_REG_NONE, mop->u.sym.addend, 0, sym);
        }
        else
        {
          tcc_machine_load_constant(dst, PREG_REG_NONE, 0, 0, sym);
          const int32_t addend = mop->u.sym.addend;
          const int sign = (addend < 0);
          const int abs_off = sign ? (int)(-addend) : (int)addend;
          load_from_base(dst, PREG_REG_NONE, mop->btype, (int)mop->is_unsigned, abs_off, sign,
                         (uint32_t)dst);
        }
        handled = 1;
        break;
      }

      default:
        /* CHAIN_REL etc.: fall through to generic scratch + MOV. */
        break;
      }

      if (!handled)
      {
        uint32_t excl = (1u << dst);
        int r = mach_ensure_in_reg(&mctx, mop, excl);
        if (r != dst)
          ot_check_mov_reg(dst, r, flags_safe(), THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE,
                           false);
      }
    }
    mach_release_all(&mctx);
    return;
  }

  if (m->kind == THUMB_ARG_MOVE_IMM64)
  {
    /* Load 64-bit immediate into register pair */
    uint32_t lo = (uint32_t)(m->imm64 & 0xFFFFFFFF);
    uint32_t hi = (uint32_t)(m->imm64 >> 32);
    load_immediate(m->dst_reg, lo, NULL, false);
    load_immediate(m->dst_reg_hi, hi, NULL, false);
    return;
  }

  /* THUMB_ARG_MOVE_IMM */
  load_immediate(m->dst_reg, m->imm, m->sym, false);
}

/* Compute the full set of destination registers written by an arg move.
 * Multi-register moves (IMM64, 64-bit MOP, STRUCT) write more than dst_reg.
 * The parallel move scheduler must check ALL written registers against
 * pending source registers to avoid clobbering. */
static uint32_t arg_move_write_set(const ThumbArgMove *m)
{
  uint32_t set = (1u << m->dst_reg);
  switch (m->kind)
  {
  case THUMB_ARG_MOVE_IMM64:
    set |= (1u << m->dst_reg_hi);
    break;
  case THUMB_ARG_MOVE_MOP:
    if (m->dst_reg_hi > 0 && m->dst_reg_hi < 16)
      set |= (1u << m->dst_reg_hi);
    break;
  case THUMB_ARG_MOVE_STRUCT:
    for (int w = 1; w < m->struct_word_count; w++)
      set |= (1u << (m->dst_reg + w));
    break;
  default:
    break;
  }
  return set;
}

/* Schedule register argument setup as a parallel assignment.
 * This avoids clobbering a source register needed for another argument.
 * Example: r0 <- r6, r1 <- r0 must be emitted as:
 *   mov r1, r0
 *   mov r0, r6
 */
static void thumb_emit_parallel_arg_moves(ThumbArgMove *moves, int move_count)
{
  if (move_count <= 0)
    return;

  uint8_t done[16];
  memset(done, 0, sizeof(done));

  ScratchRegAlloc tmp_alloc = (ScratchRegAlloc){0};
  int have_tmp = 0;

  for (int remaining = move_count; remaining > 0;)
  {
    uint32_t src_set = 0;
    for (int i = 0; i < move_count; ++i)
    {
      if (done[i])
        continue;
      if (moves[i].kind == THUMB_ARG_MOVE_REG)
        src_set |= (1u << moves[i].src_reg);
    }

    int chosen = -1;
    for (int i = 0; i < move_count; ++i)
    {
      if (done[i])
        continue;
      /* Check ALL destination registers of this move against pending sources.
       * Multi-reg writes (IMM64, 64-bit MOP, STRUCT) must not clobber any
       * register that a pending REG move still needs to read. */
      if ((src_set & arg_move_write_set(&moves[i])) == 0)
      {
        chosen = i;
        break;
      }
    }

    if (chosen < 0)
    {
      /* Cycle among register moves. Break it with a scratch temp. */
      int cyc = -1;
      for (int i = 0; i < move_count; ++i)
      {
        if (!done[i] && moves[i].kind == THUMB_ARG_MOVE_REG)
        {
          cyc = i;
          break;
        }
      }
      if (cyc < 0)
        tcc_ice("arg move cycle without reg sources");

      if (!have_tmp)
      {
        /* Exclude all regs involved in the parallel move. */
        uint32_t exclude = 0;
        for (int i = 0; i < move_count; ++i)
        {
          if (done[i])
            continue;
          exclude |= arg_move_write_set(&moves[i]);
          if (moves[i].kind == THUMB_ARG_MOVE_REG)
            exclude |= (1u << moves[i].src_reg);
        }
        /* Also exclude SP/PC. */
        exclude |= (1u << ARM_SP) | (1u << ARM_PC);
        tmp_alloc = get_scratch_reg_with_save(exclude);
        have_tmp = 1;
      }

      thumb_require_materialized_reg("thumb_emit_parallel_arg_moves", "tmp", tmp_alloc.reg);
      ot_check_mov_reg(tmp_alloc.reg, moves[cyc].src_reg, flags_safe(), THUMB_SHIFT_DEFAULT,
                       ENFORCE_ENCODING_NONE, false);
      moves[cyc].src_reg = tmp_alloc.reg;
      continue;
    }

    thumb_emit_arg_move(&moves[chosen]);
    done[chosen] = 1;
    --remaining;
  }

  if (have_tmp && tmp_alloc.saved)
    ot_check(th_pop(1u << tmp_alloc.reg));
}

/* ========================================================================
 * Helper functions for call argument handling
 * ======================================================================== */

/* Store a word to stack with large offset fallback.  An offset out of the
 * immediate's reach goes through a scratch register, which must not be one
 * in `keep`: the other half of a pair still to be stored, a struct base. */
static void store_word_to_stack_keep(int src_reg, int stack_offset, uint32_t keep)
{
  if (!store_word_to_base(src_reg, ARM_SP, stack_offset, 0))
  {
    ScratchRegAlloc sc = get_scratch_reg_with_save((1u << src_reg) | keep);
    /* A scratch saved by PUSH has moved SP down a word under the store. */
    load_immediate(sc.reg, stack_offset + (sc.saved == 1 ? 4 : 0), NULL, false);
    ot_check(th_str_reg(src_reg, ARM_SP, sc.reg, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
    restore_scratch_reg(&sc);
  }
}

static void store_word_to_stack(int src_reg, int stack_offset)
{
  store_word_to_stack_keep(src_reg, stack_offset, 0);
}

/* Store a word to stack, keeping the struct base register intact */
static void store_word_to_stack_safe(int src_reg, int stack_offset, int base_addr_reg)
{
  store_word_to_stack_keep(src_reg, stack_offset, 1u << base_addr_reg);
}

/* Get struct base address into a register (MOP path).
 * For struct arguments, we want the ADDRESS of the struct, not a word from it.
 * The MOP from machine_op_from_ir encodes the "value-level" view, so we
 * convert / strip one level of indirection to obtain the address instead. */
static int get_struct_base_addr_mop(const MachineOperand *mop, int default_reg)
{
  switch (mop->kind)
  {
  case MACH_OP_REG:
    /* Register holds the struct address (for both needs_deref=true and false,
     * the register value IS the address we want for struct copying). */
    return mop->u.reg.r0;

  case MACH_OP_FRAME_ADDR:
    /* Address-of local struct: compute FP + offset. */
    tcc_machine_addr_of_stack_slot(default_reg, mop->u.frame.offset, 0);
    return default_reg;

  case MACH_OP_SPILL:
    if (mop->needs_deref)
    {
      /* llocal: spill slot holds pointer to struct. Load just the pointer. */
      tcc_machine_load_spill_slot(default_reg, mop->u.spill.offset);
    }
    else
    {
      /* Local struct on stack: compute address FP + offset. */
      tcc_machine_addr_of_stack_slot(default_reg, mop->u.spill.offset, 0);
    }
    return default_reg;

  case MACH_OP_PARAM_STACK:
    /* Struct in caller's argument area: compute address with param adjustment. */
    tcc_machine_addr_of_stack_slot(default_reg, mop->u.param.offset, 1 /* is_param */);
    return default_reg;

  case MACH_OP_SYMBOL:
  {
    Sym *sym = mop->u.sym.sym ? validate_sym_for_reloc(mop->u.sym.sym) : NULL;
    load_immediate(default_reg, (uint32_t)mop->u.sym.addend, sym, false);
    return default_reg;
  }

  default:
  {
    /* CHAIN_REL, etc: generic path with needs_deref stripped. */
    MachineOperand addr_mop = *mop;
    addr_mop.needs_deref = false;
    MachineCodegenContext mctx = {0};
    int r = mach_ensure_in_reg(&mctx, &addr_mop, 0);
    mach_release_all(&mctx);
    return r;
  }
  }
}

/* Build register move for a struct argument (MOP path) */
static int build_reg_move_struct(ThumbArgMove *moves, int move_count, const MachineOperand *mop,
                                 const TCCAbiArgLoc *loc, int base_reg, ThumbGenCallSite *call_site,
                                 int src_align)
{
  int words = loc->reg_count;
  if (words > 0 && words <= 4)
  {
    moves[move_count++] = (ThumbArgMove){
        .kind = THUMB_ARG_MOVE_STRUCT,
        .dst_reg = base_reg,
        .mop = *mop,
        .struct_word_count = words,
        .struct_src_align = src_align,
    };
  }
  for (int w = 0; w < words && w < loc->reg_count; w++)
    call_site->registers_map |= (1 << (base_reg + w));
  return move_count;
}

/* Build register move for a 64-bit argument (MOP path) */
static int build_reg_move_64bit(ThumbArgMove *moves, int move_count, const MachineOperand *mop, const IROperand *arg,
                                int base_reg, ThumbGenCallSite *call_site, TCCIRState *ir)
{
  if (mop->kind == MACH_OP_REG && !mop->needs_deref && thumb_is_hw_reg(mop->u.reg.r0) && thumb_is_hw_reg(mop->u.reg.r1))
  {
    /* Both halves in registers — emit up to two REG moves. */
    if (mop->u.reg.r0 != base_reg)
      moves[move_count++] = (ThumbArgMove){.kind = THUMB_ARG_MOVE_REG, .dst_reg = base_reg, .src_reg = mop->u.reg.r0};
    if (mop->u.reg.r1 != (base_reg + 1))
      moves[move_count++] =
          (ThumbArgMove){.kind = THUMB_ARG_MOVE_REG, .dst_reg = base_reg + 1, .src_reg = mop->u.reg.r1};
  }
  else if (mop->kind == MACH_OP_IMM && !mop->needs_deref)
  {
    const uint64_t imm64 = (uint64_t)mop->u.imm.val;
    moves[move_count++] =
        (ThumbArgMove){.kind = THUMB_ARG_MOVE_IMM64, .dst_reg = base_reg, .dst_reg_hi = base_reg + 1, .imm64 = imm64};
  }
  else
  {
    /* Generic: load MOP value into register pair at emit time.
     * Covers SPILL, PARAM_STACK, CHAIN_REL, REG+needs_deref, SYMBOL, etc. */
    MachineOperand m = *mop;
    m.is_64bit = true;
    moves[move_count++] =
        (ThumbArgMove){.kind = THUMB_ARG_MOVE_MOP, .dst_reg = base_reg, .dst_reg_hi = base_reg + 1, .mop = m};
  }

  call_site->registers_map |= (1 << base_reg) | (1 << (base_reg + 1));
  return move_count;
}

/* Build register move for a 32-bit argument (MOP path) */
static int build_reg_move_32bit(ThumbArgMove *moves, int move_count, const MachineOperand *mop, const IROperand *arg,
                                int base_reg, ThumbGenCallSite *call_site, TCCIRState *ir)
{
  switch (mop->kind)
  {
  case MACH_OP_REG:
    if (mop->needs_deref)
    {
      /* Register-indirect: needs dereference at emit time. */
      moves[move_count++] = (ThumbArgMove){.kind = THUMB_ARG_MOVE_MOP, .dst_reg = base_reg, .mop = *mop};
    }
    else if (mop->u.reg.r0 != base_reg)
    {
      moves[move_count++] = (ThumbArgMove){.kind = THUMB_ARG_MOVE_REG, .dst_reg = base_reg, .src_reg = mop->u.reg.r0};
    }
    break;

  case MACH_OP_IMM:
    /* An absolute-address lvalue -- an MMIO register passed straight to a call,
     * pico-sdk's SDIO_ERRMSG(..., SDIO_PIO->sm[0].addr, SDIO_PIO->ctrl) -- is a
     * load through the address at emit time, not the address itself. */
    if (mop->needs_deref)
      moves[move_count++] = (ThumbArgMove){.kind = THUMB_ARG_MOVE_MOP, .dst_reg = base_reg, .mop = *mop};
    else
      moves[move_count++] =
          (ThumbArgMove){.kind = THUMB_ARG_MOVE_IMM, .dst_reg = base_reg, .imm = (uint32_t)mop->u.imm.val, .sym = NULL};
    break;

  case MACH_OP_SYMBOL:
    if (mop->needs_deref)
    {
      /* Load value from global symbol — emit at emit time. */
      moves[move_count++] = (ThumbArgMove){.kind = THUMB_ARG_MOVE_MOP, .dst_reg = base_reg, .mop = *mop};
    }
    else
    {
      /* Load symbol address (with addend). */
      moves[move_count++] = (ThumbArgMove){
          .kind = THUMB_ARG_MOVE_IMM, .dst_reg = base_reg, .imm = (uint32_t)mop->u.sym.addend, .sym = mop->u.sym.sym};
    }
    break;

  case MACH_OP_FRAME_ADDR:
    moves[move_count++] = (ThumbArgMove){.kind = THUMB_ARG_MOVE_LOCAL_ADDR,
                                         .dst_reg = base_reg,
                                         .local_offset = mop->u.frame.offset,
                                         .local_is_param = 0};
    break;

  case MACH_OP_PARAM_STACK:
    /* `&P` of a parameter in the caller's argument area -- the sret pointer of
     * `s = f(s)` when `s` is a by-value struct parameter split across r0-r3
     * and the stack.  The generic MOP load below always reads the slot, so it
     * passed the struct's first word as the pointer.  A value read (is_lval,
     * or a PARAM vreg) still loads. */
    if (arg && arg->is_local && !arg->is_lval && !arg->is_llocal && !mop->needs_deref)
    {
      moves[move_count++] = (ThumbArgMove){.kind = THUMB_ARG_MOVE_LOCAL_ADDR,
                                           .dst_reg = base_reg,
                                           .local_offset = mop->u.param.offset,
                                           .local_is_param = 1};
      break;
    }
    moves[move_count++] = (ThumbArgMove){.kind = THUMB_ARG_MOVE_MOP, .dst_reg = base_reg, .mop = *mop};
    break;

  default:
    /* SPILL, PARAM_STACK, CHAIN_REL, etc.: generic MOP load at emit time. */
    moves[move_count++] = (ThumbArgMove){.kind = THUMB_ARG_MOVE_MOP, .dst_reg = base_reg, .mop = *mop};
    break;
  }

  call_site->registers_map |= (1 << base_reg);
  return move_count;
}

/* Place a struct argument on stack (MOP path) */
/* Load one struct word at [base_addr_reg + off] into `reg`, falling back to a
 * register-offset load when `off` exceeds the LDR immediate range. */
static void load_struct_word_into(int reg, int base_addr_reg, int off)
{
  if (!load_word_from_base(reg, base_addr_reg, off, 0))
  {
    load_immediate(reg, off, NULL, false);
    ot_check(th_ldr_reg(reg, base_addr_reg, reg, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
  }
}

/* Word copies at least this many words long call libtcc1's __tcc_wcopy_N
 * (lib/arm_copystub.S) instead of expanding inline; 0 keeps them all inline. */
TCC_DBG_ENV_INT(copy_stub_min_words, "TCC_COPY_STUB_MIN", 3)
#define COPY_STUB_MAX_WORDS 32

/* A word-aligned copy of `size` bytes from [r1] to [r0] as a call to the
 * libtcc1 routine of its size: 4 bytes of code where the inline LDM/STM pairs
 * took 8 per 16 bytes copied (Zig's C passes, returns and copies 40-200 byte
 * structs by value everywhere).  The routine clobbers exactly what the inline
 * copy did -- r0-r3, IP, LR -- and neither R9 nor the stack.  Only when the
 * libtcc1.a this module links has it: -nodefaultlibs links none. */
static int thumb_copy_stub_call(int size)
{
  const int words = size / 4, min = copy_stub_min_words();
  if (min <= 0 || (size & 3) || words < (min > 3 ? min : 3) || words > COPY_STUB_MAX_WORDS)
    return 0;
  char name[32];
  snprintf(name, sizeof name, "__tcc_wcopy_%d", words);
  if (!tcc_yaff_libtcc1_has(tcc_state, name))
    return 0;
  MachineOperand target = {0};
  target.kind = MACH_OP_SYMBOL;
  target.u.sym.sym = external_helper_sym(tok_alloc_const(name));
  /* A struct argument of a tail call is copied before its B.W: this BL
   * returns. */
  const int saved_tail = tail_call_pending;
  tail_call_pending = 0;
  gcall_or_jump_mop(0, target);
  tail_call_pending = saved_tail;
  return 1;
}

/* Copy `size` bytes from [r1] to [r0], both word-aligned, clobbering r0-r3,
 * IP and LR (what a memcpy call would).  A call to __tcc_wcopy_N when there
 * is one for the size (thumb_copy_stub_call); else up to
 * STACK_ARG_UNROLL_MAX_WORDS, `ldmia r1!, {r2,r3,ip,lr}` / `stmia r0!` per 16
 * bytes; above, a loop moving 12 bytes a turn with LR counting.  A 1-3 byte
 * tail moves by bytes. */
static void thumb_emit_word_copy_r0_r1(int size)
{
  const uint32_t set4 = (1u << R2) | (1u << R3) | (1u << R12) | (1u << ARM_LR);
  const uint32_t set3 = (1u << R2) | (1u << R3) | (1u << R12);
  if (thumb_copy_stub_call(size))
    return;
  int left = size / 4;
  if (left <= STACK_ARG_UNROLL_MAX_WORDS)
  {
    for (; left >= 4; left -= 4)
    {
      ot_check(th_ldm(R1, set4, 1, ENFORCE_ENCODING_NONE));
      ot_check(th_stm(R0, set4, 1, ENFORCE_ENCODING_NONE));
    }
  }
  else
  {
    load_immediate(ARM_LR, left / 3, NULL, false);
    th_literal_pool_reserve_upcoming_bytes(14);
    const int top = ind;
    ot_check(th_ldm(R1, set3, 1, ENFORCE_ENCODING_NONE));
    ot_check(th_stm(R0, set3, 1, ENFORCE_ENCODING_NONE));
    ot_check(th_sub_imm(ARM_LR, ARM_LR, 1, FLAGS_BEHAVIOUR_SET, ENFORCE_ENCODING_NONE));
    const int off = top - (ind + 4);
    if (off < -256)
      tcc_ice("word copy loop out of branch range");
    ot_check(th_b_t1(1 /* NE */, (uint32_t)(off >> 1)));
    left %= 3;
  }
  if (left == 3)
  {
    ot_check(th_ldm(R1, set3, 0, ENFORCE_ENCODING_NONE));
    ot_check(th_stm(R0, set3, 0, ENFORCE_ENCODING_NONE));
  }
  else if (left == 2)
  {
    ot_check(th_ldrd_imm(R2, R3, R1, 0, 6));
    ot_check(th_strd_imm(R2, R3, R0, 0, 6));
  }
  else if (left == 1)
  {
    ot_check_ldr_imm(R2, R1, 0, 6, ENFORCE_ENCODING_NONE);
    ot_check_str_imm(R2, R0, 0, 6, ENFORCE_ENCODING_NONE);
  }
  for (int b = 0; b < (size & 3); b++)
  {
    ot_check(th_ldrb_imm(R2, R1, left * 4 + b, 6, ENFORCE_ENCODING_NONE));
    ot_check(th_strb_imm(R2, R0, left * 4 + b, 6, ENFORCE_ENCODING_NONE));
  }
}

static void place_stack_arg_struct(const MachineOperand *mop, const TCCAbiArgLoc *loc, int stack_offset,
                                   int src_align, uint32_t arg_move_dst_mask, uint32_t live_mask)
{
  int words_in_regs = (loc->kind == TCC_ABI_LOC_REG_STACK) ? loc->reg_count : 0;
  int struct_src_offset = words_in_regs * 4;
  int struct_size = (loc->kind == TCC_ABI_LOC_REG_STACK) ? loc->stack_size : loc->size;
  int words = (struct_size + 3) / 4;

  /* A large struct (AAPCS32 passes any size by value) is copied in the
   * argument registers, IP and LR rather than a word per load/store pair.
   * They may hold values the rest of the call setup still needs, so they are
   * saved around the copy; the 24 bytes pushed enter every SP-relative address
   * computed meanwhile. */
  if (words >= STACK_ARG_MEMCPY_MIN_WORDS)
  {
    /* Only the registers that hold something the rest of the call setup still
     * reads are saved (live_mask, call_live_regs); the others -- the common
     * case, the argument registers are loaded after the stack arguments -- are
     * free, and the push/pop pair around the copy (6 registers, 24 bytes of
     * SP bias) vanishes.  The memcpy call clobbers all six, so it keeps them. */
    const uint16_t all6 = (uint16_t)((1u << R0) | (1u << R1) | (1u << R2) | (1u << R3) | (1u << R12) | (1u << ARM_LR));
    const uint16_t saved = (src_align >= 4) ? (uint16_t)(all6 & live_mask) : all6;
    const int nsaved = __builtin_popcount(saved);
    if (saved)
      ot_check(th_push(saved));
    helper_call_sp_bias += nsaved * 4;
    int src = get_struct_base_addr_mop(mop, R1);
    /* `add r1, src, #off` and `add r0, sp, #off` in one instruction each, where
     * the offset encodes (TCC_DISABLE_PASS=codegen:copy_addr_fuse: the old
     * mov + adds / movs + add pairs). */
    const int fuse = !tcc_ir_opt_pass_disabled("codegen:copy_addr_fuse");
    thumb_opcode fused;
    if (fuse && src != R1 && struct_src_offset &&
        (fused = th_add_imm(R1, (uint32_t)src, (uint32_t)struct_src_offset, flags_safe(), ENFORCE_ENCODING_NONE)).size)
      ot_check(fused);
    else
    {
      if (src != R1)
        ot_check_mov_reg(R1, (uint32_t)src, flags_safe(), THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE, false);
      if (struct_src_offset)
        ot_check(th_add_imm(R1, R1, (uint32_t)struct_src_offset, flags_safe(), ENFORCE_ENCODING_NONE));
    }
    const int dst_off = stack_offset + scratch_push_sp_bias() - call_args_sp_bias;
    if (fuse && dst_off >= 0 &&
        (fused = th_add_imm(R0, ARM_SP, (uint32_t)dst_off, flags_safe(), ENFORCE_ENCODING_NONE)).size)
      ot_check(fused);
    else
    {
      load_immediate(R0, dst_off, NULL, false);
      ot_check(th_add_reg(R0, R0, ARM_SP, flags_safe(), THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
    }
    if (src_align >= 4)
    {
      /* A word-aligned source copies inline: the saved registers are all free
       * now.  A memcpy call cost ~75 instructions for the 48-256 byte structs
       * the Zig C backend passes by value. */
      thumb_emit_word_copy_r0_r1(words * 4);
      if (saved)
        ot_check(th_pop(saved));
      helper_call_sp_bias -= nsaved * 4;
      return;
    }
    load_immediate(R2, words * 4, NULL, false);
    Sym *memcpy_sym = external_global_sym(tok_alloc_const("memcpy"), &func_old_type);
    MachineOperand func_mop = {0};
    func_mop.kind = MACH_OP_SYMBOL;
    func_mop.u.sym.sym = memcpy_sym;
    if (text_and_data_separation)
      ot_check(th_push((uint16_t)((1 << R9) | (1 << R12))));
    gcall_or_jump_mop(0, func_mop);
    if (text_and_data_separation)
      ot_check(th_pop((uint16_t)((1 << R9) | (1 << R12))));
    ot_check(th_pop(saved));
    helper_call_sp_bias -= nsaved * 4;
    return;
  }

  /* A frame-resident source needs no base register: LDR/LDRD carry the frame
   * offset in their own immediate field, so the `add rX, sp, #off` that
   * get_struct_base_addr_mop emits is pure overhead — one instruction per
   * marshaled by-value struct argument.  Only take the direct form while every
   * source word stays inside the offset range both LDR and LDRD encode
   * (word-aligned, 0..1020); outside it the per-word fallback would cost more
   * than the single ADD.  Nothing in the loop below pushes a scratch across a
   * source load, so the SP bias folded in here stays valid throughout. */
  int frame_raw = 0;
  bool src_is_frame = false;
  if (mop->kind == MACH_OP_SPILL && !mop->needs_deref)
  {
    frame_raw = mop->u.spill.offset;
    src_is_frame = true;
  }
  else if (mop->kind == MACH_OP_FRAME_ADDR)
  {
    frame_raw = mop->u.frame.offset;
    src_is_frame = true;
  }
  else if (mop->kind == MACH_OP_PARAM_STACK)
  {
    /* A by-value parameter passed on: in the incoming argument area. */
    src_is_frame = true;
  }

  int base_bias = 0;
  int base_addr_reg;
  ScratchRegAlloc struct_sc = {0};
  bool have_struct_sc = false;

  if (src_is_frame)
  {
    const int adj = mop->kind == MACH_OP_PARAM_STACK ? param_frame_offset(mop->u.param.offset)
                                                     : fp_adjust_local_offset(frame_raw, 0);
    const int first = adj + struct_src_offset;
    const int last = first + (words > 0 ? (words - 1) * 4 : 0);
    if ((adj & 3) == 0 && first >= 0 && last <= 1020)
    {
      base_addr_reg = tcc_state->need_frame_pointer ? R_FP : R_SP;
      base_bias = adj;
    }
    else
      src_is_frame = false;
  }
  if (!src_is_frame)
  {
    struct_sc = get_scratch_reg_with_save(0);
    have_struct_sc = true;
    base_addr_reg = get_struct_base_addr_mop(mop, struct_sc.reg);
  }

  /* The data registers.  The first is LR, free at a call's argument setup
   * since the call clobbers it -- unless the struct base is in LR: with nothing
   * else free, get_scratch_reg_with_save hands out LR itself.  Loading the
   * first pair into LR then overwrote the base, and every later word was read
   * through the loaded value (Zig's AstGen.containerDecl passing a 20-byte
   * ResultInfo split across r2-r3 and the stack).  The second data register,
   * for paired LDRD/STRD, comes from find_call_scratch too.  It never pushes
   * (SP-relative store offsets must stay valid), and its R_IP last resort is
   * refused while IP holds a live value -- a later stack argument, say. */
  int data1 = ARM_LR;
  if (base_addr_reg == ARM_LR)
    data1 = find_call_scratch_free(1u << ARM_LR, 0);
  if (data1 < 0)
  {
    /* No register but the base itself: rebuild the address into LR for every
     * word and load through it.  Only a scratch-built base can be rebuilt; a
     * struct whose address lives in LR as a register operand cannot. */
    if (mop->kind == MACH_OP_REG)
      tcc_error("internal error: struct argument addressed through lr with no free register");
    for (int k = 0; k < words; ++k)
    {
      int b = get_struct_base_addr_mop(mop, ARM_LR);
      load_struct_word_into(ARM_LR, b, struct_src_offset + k * 4);
      store_word_to_stack_keep(ARM_LR, stack_offset + k * 4, 0);
    }
    if (have_struct_sc)
      restore_scratch_reg(&struct_sc);
    return;
  }
  int data2 = find_call_scratch_free((1u << ARM_LR) | (1u << (uint32_t)data1) | (1u << (uint32_t)base_addr_reg), 0);
  bool can_pair = (words >= 2 && data2 != data1 && data2 != base_addr_reg && data2 >= 0 &&
                   data2 <= R_LR && data2 != R_SP);
  bool src_aligned = (src_align >= 4);

  int w = 0;

  /* Runs of three or four words from a frame source move by LDM/STM:
   * `add rA, sp, #src; ldm rA, {..}` then `stmia.w sp, {..}` for the bottom of
   * the outgoing area, else `add rB, sp, #dst; stmia rB!, {..}` -- 8 or 10
   * bytes where LDRD/STRD pairs take 12-16 -- as long as it comes out shorter
   * with the registers free here.  The LDM's base is one of its own
   * destinations (no writeback then).  Registers load and store in ascending
   * order, so any set keeps the words in place. */
  if (src_is_frame && src_aligned && !((base_bias + struct_src_offset) & 3))
  {
    while (words - w >= 3)
    {
      const int src_off = base_bias + struct_src_offset + w * 4, dst_off = stack_offset + w * 4;
      const int direct_sp = dst_off == 0;
      uint32_t excl = (1u << (uint32_t)base_addr_reg) | (1u << R_SP) | (1u << R_PC);
      int regs[6], got = 0;
      int want = (words - w >= 4 ? 4 : 3) + !direct_sp;
      /* LR is free at a call's argument setup: the call clobbers it. */
      regs[got++] = ARM_LR;
      excl |= 1u << ARM_LR;
      while (got < want)
      {
        int r = find_call_scratch_free(excl, arg_move_dst_mask);
        if (r < 0 || r == R_SP || r == R_PC || (excl & (1u << (uint32_t)r)) ||
            (r == R_FP && tcc_state->need_frame_pointer))
          break;
        regs[got++] = r;
        excl |= 1u << (uint32_t)r;
      }
      int rb = -1;
      if (!direct_sp)
      {
        /* The STM base: the lowest register, left out of the list. */
        int bi = 0;
        for (int i = 1; i < got; i++)
          if (regs[i] < regs[bi])
            bi = i;
        if (got < 4)
          break;
        rb = regs[bi];
        regs[bi] = regs[--got];
      }
      const int k = got < 4 ? got : 4;
      if (k < 3)
        break;
      uint32_t list = 0;
      int ra = -1, all_low = 1;
      for (int i = 0; i < k; i++)
      {
        list |= 1u << (uint32_t)regs[i];
        all_low &= regs[i] <= R7;
        if (regs[i] <= R7 && regs[i] > ra)
          ra = regs[i];
      }
      if (ra < 0)
        ra = regs[0];
      thumb_opcode addr_a = src_off < 0
                                ? th_sub_imm(ra, base_addr_reg, -src_off, flags_safe(), ENFORCE_ENCODING_NONE)
                                : th_add_imm(ra, base_addr_reg, src_off, flags_safe(), ENFORCE_ENCODING_NONE);
      thumb_opcode addr_b = {0};
      if (!direct_sp)
        addr_b = th_add_imm(rb, ARM_SP, dst_off, flags_safe(), ENFORCE_ENCODING_NONE);
      const int ldm_size = all_low && ra <= R7 ? 2 : 4;
      const int stm_size = !direct_sp && all_low && rb <= R7 ? 2 : 4;
      if (!addr_a.size || (!direct_sp && !addr_b.size) ||
          addr_a.size + ldm_size + addr_b.size + stm_size >= 4 * k)
        break;
      ot_check(addr_a);
      if (ldm_size == 2)
        ot_check((thumb_opcode){.size = 2, .opcode = 0xC800u | ((uint32_t)ra << 8) | list});
      else
        ot_check((thumb_opcode){.size = 4, .opcode = ((0xE890u | (uint32_t)ra) << 16) | list});
      if (direct_sp)
        ot_check((thumb_opcode){.size = 4, .opcode = ((0xE880u | (uint32_t)R_SP) << 16) | list});
      else
      {
        ot_check(addr_b);
        if (stm_size == 2)
          ot_check((thumb_opcode){.size = 2, .opcode = 0xC000u | ((uint32_t)rb << 8) | list});
        else
          ot_check((thumb_opcode){.size = 4, .opcode = ((0xE8A0u | (uint32_t)rb) << 16) | list});
      }
      w += k;
    }
  }

  if (can_pair)
  {
    for (; w + 1 < words; w += 2)
    {
      int src_off = base_bias + struct_src_offset + w * 4;
      int dst_off = stack_offset + w * 4;

      if (!(src_aligned && tcc_gen_machine_try_ldrd_base(data1, data2, base_addr_reg, src_off)))
      {
        load_struct_word_into(data1, base_addr_reg, src_off);
        load_struct_word_into(data2, base_addr_reg, src_off + 4);
      }
      if (!tcc_gen_machine_try_strd_base(data1, data2, ARM_SP, dst_off))
      {
        store_word_to_stack_keep(data1, dst_off, (1u << base_addr_reg) | (1u << data2));
        store_word_to_stack_safe(data2, dst_off + 4, base_addr_reg);
      }
    }
  }

  /* Trailing odd word, or every word when pairing was unavailable. */
  for (; w < words; ++w)
  {
    int src_off = base_bias + struct_src_offset + w * 4;
    int dst_off = stack_offset + w * 4;
    load_struct_word_into(data1, base_addr_reg, src_off);
    store_word_to_stack_safe(data1, dst_off, base_addr_reg);
  }
  if (have_struct_sc)
    restore_scratch_reg(&struct_sc);
}

/* Find a free scratch register via liveness (no push/pop).
 * Returns the register number, or R_IP as last resort.
 * Must not push/pop since SP-relative offsets for stack args would shift.
 *
 * Unlike tcc_ls_find_free_scratch_reg (which refuses callee-saved regs),
 * this also considers callee-saved registers already pushed in the prologue.
 * Those are safe to clobber because the epilogue will restore them.
 *
 * arg_move_dst_mask: registers that will be explicitly written by register
 * arg moves AFTER stack arg placement.  These are safe to clobber even if
 * currently live, because the subsequent moves will overwrite them.
 * Pass 0 when not in a pre-move stack arg placement context. */
static int find_call_scratch(uint32_t extra_exclude, uint32_t arg_move_dst_mask)
{
  TCCIRState *ir = tcc_state->ir;
  uint32_t exclude = scratch_global_exclude | extra_exclude;
  if (ir)
  {
    /* Standard path: try caller-saved regs via liveness */
    int reg = tcc_ls_find_free_scratch_reg(&ir->ls, ir->codegen_instruction_idx, exclude, ir->leaffunc);
    if (reg != PREG_NONE && reg >= 0 && reg < 16 && reg != R_SP && reg != R_PC)
      return reg;

    /* Extended path: try callee-saved regs that are already pushed in prologue
     * AND not live at this instruction (so we won't clobber active values). */
    if (ir->ls.live_regs_by_instruction && ir->codegen_instruction_idx >= 0 &&
        ir->codegen_instruction_idx < ir->ls.live_regs_by_instruction_size)
    {
      uint32_t live = ir->ls.live_regs_by_instruction[ir->codegen_instruction_idx];
      uint32_t callee_pushed = pushed_registers & 0x0FF0u; /* R4-R11 that were pushed */
      /* R7 is the frame base: liveness never models it (it is not an
       * interval), so it ALWAYS looks dead here — handing it out clobbered
       * the FP mid-marshal and every later FP-relative address computed
       * from garbage (lac_slot_key: `ldr r7,[ip,#8]` for a struct stack
       * arg, then `sub ip, r7, #12` for the next arg).  R9 is the GOT
       * base under text_and_data_separation.  Mirrors the reserved set in
       * scratch_pushed_dead_reg. */
      uint32_t reserved = (1u << R_FP) | scratch_exclude_baseline();
      if (tcc_state->text_and_data_separation)
        reserved |= (1u << 9);
      uint32_t candidates = callee_pushed & ~live & ~exclude & ~reserved;
      if (candidates)
      {
        /* Prefer low registers (R4-R7) for 16-bit encoding */
        int r = (int)__builtin_ctz(candidates);
        return r;
      }
    }

    /* Pre-move path: registers that are destinations of explicit (non-identity)
     * register arg moves can be used as scratch — the moves will overwrite them.
     * Prefer low registers for 16-bit encoding. */
    if (arg_move_dst_mask)
    {
      uint32_t candidates = arg_move_dst_mask & ~exclude;
      if (candidates)
      {
        int r = (int)__builtin_ctz(candidates);
        if (r >= 0 && r < 16 && r != R_SP && r != R_PC)
          return r;
      }
    }
  }
  return R_IP;
}

/* find_call_scratch without its last resort: -1 instead of R_IP while IP is
 * taken -- live in the allocation (an argument still to be stored, say), or
 * handed out as a scratch already.  find_call_scratch returns R_IP then too,
 * which a caller about to load into it cannot use. */
static int find_call_scratch_free(uint32_t extra_exclude, uint32_t arg_move_dst_mask)
{
  const int r = find_call_scratch(extra_exclude, arg_move_dst_mask);
  TCCIRState *ir = tcc_state->ir;
  if (r != R_IP || !ir || (arg_move_dst_mask & (1u << R_IP)))
    return r;
  const uint32_t exclude = scratch_global_exclude | extra_exclude;
  if ((exclude & (1u << R_IP)) ||
      tcc_ls_find_free_scratch_reg(&ir->ls, ir->codegen_instruction_idx, ~(1u << R_IP), ir->leaffunc) != R_IP)
    return -1;
  return r;
}

/* Place a 64-bit argument on stack (MOP path) */
static void place_stack_arg_64bit(const MachineOperand *mop, int stack_offset, TCCIRState *ir,
                                  uint32_t arg_move_dst_mask)
{
  int lo_offset = stack_offset;
  int hi_offset = stack_offset + 4;

  if (mop->kind == MACH_OP_REG && !mop->needs_deref && thumb_is_hw_reg(mop->u.reg.r0) && thumb_is_hw_reg(mop->u.reg.r1))
  {
    /* If either register is R0-R3, the value was already stored by
     * presave_stack_args_from_arg_regs before the register shuffle. */
    if (mop->u.reg.r0 <= ARM_R3 || mop->u.reg.r1 <= ARM_R3)
      return;
    /* The two halves land on adjacent, word-aligned SP slots — exactly STRD's
     * shape.  (STRD to the outgoing argument area is always safe: SP-relative,
     * word-multiple offset, SP 8-aligned.) */
    if (!tcc_gen_machine_try_strd_base(mop->u.reg.r0, mop->u.reg.r1, ARM_SP, lo_offset))
    {
      store_word_to_stack_keep(mop->u.reg.r0, lo_offset, 1u << mop->u.reg.r1);
      store_word_to_stack(mop->u.reg.r1, hi_offset);
    }
  }
  else if (mop->kind == MACH_OP_IMM && !mop->needs_deref)
  {
    uint64_t imm64 = (uint64_t)mop->u.imm.val;
    uint32_t lo_val = (uint32_t)imm64;
    uint32_t hi_val = (uint32_t)(imm64 >> 32);
    int scr = find_call_scratch(0, arg_move_dst_mask);
    load_immediate(scr, lo_val, NULL, false);

    /* Pair the two halves into one STRD.  When both halves are equal (every
     * `0.0`/`0LL` variadic argument) the single scratch is stored twice —
     * try_strd_pair permits Rt == Rt2 (only LDRD forbids it).  Otherwise take a
     * second scratch for the high half; find_call_scratch falls back to R_IP
     * without honouring the exclude mask, so a collision means no pair. */
    int hi_scr = scr;
    if (hi_val != lo_val)
    {
      hi_scr = find_call_scratch(1u << (uint32_t)scr, arg_move_dst_mask);
      if (hi_scr == scr)
      {
        /* No second scratch: fall back to the sequential two-store form. */
        store_word_to_stack(scr, lo_offset);
        load_immediate(scr, hi_val, NULL, false);
        store_word_to_stack(scr, hi_offset);
        return;
      }
      load_immediate(hi_scr, hi_val, NULL, false);
    }

    if (!tcc_gen_machine_try_strd_base(scr, hi_scr, ARM_SP, lo_offset))
    {
      store_word_to_stack_keep(scr, lo_offset, 1u << hi_scr);
      store_word_to_stack(hi_scr, hi_offset);
    }
  }
  else if (mop->needs_deref && !mach_op_64_names_memory(mop))
  {
    /* The operand holds a pointer (in reg, spill, etc.), not the 64-bit
     * value itself.  Load the pointer into a register, then fetch the
     * lo/hi halves from [ptr+0] and [ptr+4].  Splitting via
     * mach_make_hi_half would incorrectly adjust the storage location
     * (e.g. spill offset) instead of the dereference offset.
     *
     * PARAM_STACK and CHAIN_REL are excluded (mach_op_64_names_memory):
     * mach_ensure_in_reg loads those directly from the caller's argument
     * area / the parent frame (ignoring needs_deref), so the else path with
     * mach_make_lo/hi_half handles them correctly.
     *
     * The base register must NOT be the scratch because both halves are
     * loaded into the scratch.  If base == scratch the first load would
     * clobber the pointer before the second load can use it. */
    int scr = find_call_scratch(0, arg_move_dst_mask);
    int base;
    MachineCodegenContext mctx = {0};
    bool need_release = false;
    if (mop->kind == MACH_OP_REG && mop->u.reg.r0 != scr)
    {
      base = mop->u.reg.r0;
    }
    else
    {
      MachineOperand addr = *mop;
      addr.needs_deref = false;
      addr.is_64bit = false;
      addr.btype = IROP_BTYPE_INT32;
      base = mach_ensure_in_reg(&mctx, &addr, (1u << scr));
      need_release = true;
    }
    load_from_base(scr, PREG_REG_NONE, IROP_BTYPE_INT32, 0, 0, 0, (uint32_t)base);
    store_word_to_stack_keep(scr, lo_offset, 1u << base);
    load_from_base(scr, PREG_REG_NONE, IROP_BTYPE_INT32, 0, 4, 0, (uint32_t)base);
    store_word_to_stack(scr, hi_offset);
    if (need_release)
      mach_release_all(&mctx);
  }
  else
  {
    /* Load each 32-bit half individually.  Override btype to INT32 so that
     * mach_ensure_in_reg → load_from_base does a single-word LDR instead
     * of a 64-bit pair load (which would allocate an extra scratch via push,
     * shift SP, and corrupt the SP-relative store offsets below). */
    MachineOperand lo = mach_make_lo_half(mop);
    MachineOperand hi = mach_make_hi_half(mop);
    lo.btype = IROP_BTYPE_INT32;
    hi.btype = IROP_BTYPE_INT32;
    MachineCodegenContext mctx = {0};
    int r_lo = mach_ensure_in_reg(&mctx, &lo, 0);
    store_word_to_stack_safe(r_lo, lo_offset, r_lo);
    mach_release_all(&mctx);
    mctx = (MachineCodegenContext){0};
    int r_hi = mach_ensure_in_reg(&mctx, &hi, 0);
    store_word_to_stack_safe(r_hi, hi_offset, r_hi);
    mach_release_all(&mctx);
  }
}

/* Place a 32-bit argument on stack (MOP path) */
static void place_stack_arg_32bit(const MachineOperand *mop, int stack_offset, CallGenContext *ctx)
{
  switch (mop->kind)
  {
  case MACH_OP_REG:
    if (!mop->needs_deref)
    {
      /* Skip R0-R3 sources — handled in pre-shuffle save. */
      if (mop->u.reg.r0 <= ARM_R3)
        return;
      store_word_to_stack(mop->u.reg.r0, stack_offset);
    }
    else
    {
      /* Register-indirect: load through the register, then store to stack.
       * Must use btype-aware load so that byte/short values are properly
       * zero/sign-extended (LDRB/LDRH) instead of always doing a word LDR. */
      int scr = find_call_scratch(1u << mop->u.reg.r0, ctx->arg_move_dst_mask);
      load_from_base(scr, PREG_REG_NONE, mop->btype, mop->is_unsigned, 0, 0, mop->u.reg.r0);
      store_word_to_stack(scr, stack_offset);
    }
    break;

  case MACH_OP_IMM:
  {
    int scr = find_call_scratch(0, ctx->arg_move_dst_mask);
    load_immediate(scr, (uint32_t)mop->u.imm.val, NULL, false);
    if (mop->needs_deref) /* an absolute-address lvalue: its value */
      load_from_base(scr, PREG_REG_NONE, mop->btype, mop->is_unsigned, 0, 0, scr);
    store_word_to_stack(scr, stack_offset);
    break;
  }

  case MACH_OP_SYMBOL:
  {
    int scr = find_call_scratch(0, ctx->arg_move_dst_mask);
    Sym *sym = mop->u.sym.sym ? validate_sym_for_reloc(mop->u.sym.sym) : NULL;
    if (mop->needs_deref)
    {
      /* Load value from global symbol address. */
      load_immediate(scr, 0, sym, false);
      int32_t addend = mop->u.sym.addend;
      int sign = (addend < 0);
      int abs_off = sign ? -addend : addend;
      load_from_base(scr, PREG_REG_NONE, mop->btype, mop->is_unsigned, abs_off, sign, scr);
    }
    else
    {
      load_immediate(scr, (uint32_t)mop->u.sym.addend, sym, false);
    }
    store_word_to_stack(scr, stack_offset);
    break;
  }

  default:
  {
    /* SPILL, PARAM_STACK, FRAME_ADDR, CHAIN_REL: generic MOP load. */
    MachineCodegenContext mctx = {0};
    int r = mach_ensure_in_reg(&mctx, mop, 0);
    store_word_to_stack(r, stack_offset);
    mach_release_all(&mctx);
    break;
  }
  }
}

/* Build all register argument moves */
static int build_register_arg_moves(CallGenContext *ctx, ThumbArgMove *reg_moves)
{
  int move_count = 0;

  for (int i = 0; i < ctx->argc; ++i)
  {
    const TCCAbiArgLoc *loc = &ctx->layout->locs[i];
    const IROperand *arg = &ctx->args[i];
    const MachineOperand *mop = &ctx->mops[i];
    const int bt = irop_get_btype(*arg);
    const int is_64bit = mop->is_64bit;

    if (loc->kind != TCC_ABI_LOC_REG && loc->kind != TCC_ABI_LOC_REG_STACK)
      continue;

    int base_reg = ARM_R0 + loc->reg_base;

    if (bt == IROP_BTYPE_STRUCT || arg->is_complex)
    {
      /* Complex values already in a register pair hold the actual value,
       * not a pointer to it.  Route through individual register moves
       * instead of the struct-copy path (which dereferences as an address). */
      if (arg->is_complex && mop->kind == MACH_OP_REG && !mop->needs_deref && mop->is_64bit)
      {
        int words = loc->reg_count;
        if (words >= 1 && mop->u.reg.r0 != base_reg)
          reg_moves[move_count++] =
              (ThumbArgMove){.kind = THUMB_ARG_MOVE_REG, .dst_reg = base_reg, .src_reg = mop->u.reg.r0};
        if (words >= 2 && mop->u.reg.r1 != (base_reg + 1))
          reg_moves[move_count++] =
              (ThumbArgMove){.kind = THUMB_ARG_MOVE_REG, .dst_reg = base_reg + 1, .src_reg = mop->u.reg.r1};
        for (int w = 0; w < words; w++)
          ctx->call_site->registers_map |= (1 << (base_reg + w));
      }
      else if (arg->is_complex && mop->kind == MACH_OP_IMM)
      {
        /* Complex immediate: split 64-bit packed value (real_lo | imag_hi)
         * into individual 32-bit register moves. */
        const uint64_t imm64 = (uint64_t)mop->u.imm.val;
        int words = loc->reg_count;
        if (words >= 1)
          reg_moves[move_count++] =
              (ThumbArgMove){.kind = THUMB_ARG_MOVE_IMM, .dst_reg = base_reg, .imm = (uint32_t)imm64, .sym = NULL};
        if (words >= 2)
          reg_moves[move_count++] = (ThumbArgMove){
              .kind = THUMB_ARG_MOVE_IMM, .dst_reg = base_reg + 1, .imm = (uint32_t)(imm64 >> 32), .sym = NULL};
        for (int w = 0; w < words; w++)
          ctx->call_site->registers_map |= (1 << (base_reg + w));
      }
      else
      {
        int src_align = 0;
        irop_type_size_align(*arg, &src_align);
        move_count = build_reg_move_struct(reg_moves, move_count, mop, loc, base_reg, ctx->call_site, src_align);
      }
    }
    else if (is_64bit)
    {
      if (loc->reg_count < 2)
        tcc_ice("64-bit register argument has insufficient registers");
      move_count = build_reg_move_64bit(reg_moves, move_count, mop, arg, base_reg, ctx->call_site, tcc_state->ir);
    }
    else
    {
      move_count = build_reg_move_32bit(reg_moves, move_count, mop, arg, base_reg, ctx->call_site, tcc_state->ir);
    }
  }

  return move_count;
}

/* An HFA struct or _Complex float/double argument: its words are loaded into
 * s<reg_base>.. (see emit_vfp_composite_arg) rather than moved as a scalar. */
static int vfp_arg_is_composite(const CallGenContext *ctx, int i)
{
  return ctx->layout->locs[i].kind == TCC_ABI_LOC_VFP_REG &&
         (ctx->mops[i].btype == IROP_BTYPE_STRUCT || ctx->mops[i].is_complex);
}

/* Core registers the composite VFP arguments still read: an aggregate's
 * address, or a _Complex float's value pair.  Their loads run after the stack
 * arguments are placed, so stack placement must not take them as scratch. */
static uint32_t vfp_composite_src_regs(const CallGenContext *ctx)
{
  uint32_t regs = 0;
  for (int i = 0; i < ctx->argc; ++i)
  {
    const MachineOperand *m = &ctx->mops[i];
    if (!vfp_arg_is_composite(ctx, i) || m->kind != MACH_OP_REG)
      continue;
    if (m->u.reg.r0 >= 0 && m->u.reg.r0 < 16)
      regs |= 1u << m->u.reg.r0;
    if (m->is_complex && !m->needs_deref && m->is_64bit && m->u.reg.r1 >= 0 && m->u.reg.r1 < 16)
      regs |= 1u << m->u.reg.r1;
  }
  return regs;
}

/* Load composite VFP argument `i` into s<reg_base>..s<reg_base+reg_count-1>:
 * word w of its memory image goes to s<reg_base+w>, which puts each double in
 * its d-register low word first. */
static void emit_vfp_composite_arg(CallGenContext *ctx, int i)
{
  const TCCAbiArgLoc *loc = &ctx->layout->locs[i];
  const MachineOperand *m = &ctx->mops[i];
  const int sbase = loc->reg_base, words = loc->reg_count;

  /* A _Complex float held as a value: in a core register pair, or packed in
   * a 64-bit immediate (real low, imaginary high). */
  if (m->is_complex && !m->needs_deref && words == 2 && (m->kind == MACH_OP_REG && m->is_64bit))
  {
    ot_check(th_vmov_gp_sp((uint16_t)m->u.reg.r0, (uint16_t)sbase, 0));
    ot_check(th_vmov_gp_sp((uint16_t)m->u.reg.r1, (uint16_t)(sbase + 1), 0));
    return;
  }
  if (m->is_complex && words == 2 && m->kind == MACH_OP_IMM)
  {
    const uint64_t imm64 = (uint64_t)m->u.imm.val;
    const int r = find_call_scratch(0, ctx->arg_move_dst_mask);
    for (int w = 0; w < 2; ++w)
    {
      load_immediate(r, (uint32_t)(imm64 >> (32 * w)), NULL, false);
      ot_check(th_vmov_gp_sp((uint16_t)r, (uint16_t)(sbase + w), 0));
    }
    return;
  }

  /* In the frame: VLDR straight off sp/fp at the offsets get_struct_base_addr_mop
   * would address (as the core-register struct path does), when every word is
   * aligned and in VLDR's +-1020 range. */
  if ((m->kind == MACH_OP_SPILL && !m->needs_deref) || m->kind == MACH_OP_PARAM_STACK ||
      m->kind == MACH_OP_FRAME_ADDR)
  {
    const int off = m->kind == MACH_OP_PARAM_STACK
                        ? param_frame_offset(m->u.param.offset)
                        : fp_adjust_local_offset(m->kind == MACH_OP_SPILL ? m->u.spill.offset : m->u.frame.offset, 0);
    const int fbase = tcc_state->need_frame_pointer ? R_FP : R_SP;
    if (!(off & 3) && off >= -1020 && off + 4 * (words - 1) <= 1020)
    {
      for (int w = 0; w < words; ++w)
        ot_check(th_vldr((uint32_t)(sbase + w), (uint32_t)fbase, off + 4 * w, 0));
      return;
    }
  }

  /* In memory: VLDR straight from its address, which must be word aligned
   * (VLDR faults on any other); a packed one goes word by word through a core
   * register. */
  int align = 0;
  irop_type_size_align(ctx->args[i], &align);
  const int scratch = find_call_scratch(0, ctx->arg_move_dst_mask);
  const int base = get_struct_base_addr_mop(m, scratch);
  if (align >= 4)
  {
    for (int w = 0; w < words; ++w)
      ot_check(th_vldr((uint32_t)(sbase + w), (uint32_t)base, 4 * w, 0));
    return;
  }
  const int r = find_call_scratch(1u << base, ctx->arg_move_dst_mask);
  for (int w = 0; w < words; ++w)
  {
    load_struct_word_into(r, base, 4 * w);
    ot_check(th_vmov_gp_sp((uint16_t)r, (uint16_t)(sbase + w), 0));
  }
}

/* Pack one double argument (GPR pair / memory) into its d-register. */
static void emit_vfp_double_arg(CallGenContext *ctx, int i, uint32_t gpr_excl_all)
{
  const TCCAbiArgLoc *loc = &ctx->layout->locs[i];
  MachineCodegenContext mctx = {0};
      /* A double read through a pointer (`f(va_arg(ap, double))` passes
       * T***DEREF***) holds an address, not a pair: load both words first,
       * as the soft-float path does, keeping clear of the GPR arguments. */
      uint32_t excl = gpr_excl_all;
      MachineOperand m = mach_resolve_deref_64(&mctx, &ctx->mops[i], &excl);
      MachineOperand lo = mach_make_lo_half(&m);
      MachineOperand hi = mach_make_hi_half(&m);
      lo.btype = IROP_BTYPE_INT32;
      hi.btype = IROP_BTYPE_INT32;
      int rlo = mach_ensure_in_reg(&mctx, &lo, gpr_excl_all);
      int rhi = mach_ensure_in_reg(&mctx, &hi, gpr_excl_all | (1u << (uint32_t)rlo));
      ot_check(th_vmov_2gp_dp((uint16_t)rlo, (uint16_t)rhi, (uint16_t)(loc->reg_base / 2), 0));
      mach_release_all(&mctx);
}

/* Place hard-float arguments into their VFP argument registers (s0..s15), in
 * two phases around the GPR argument moves.
 *
 * Phase 0 runs before them, while every source register is still intact: the
 * single-precision arguments that are VFP-resident form a permutation among
 * the s-registers, resolved as a parallel move with VFP_SCRATCH0 (s14)
 * breaking any cycle; then the composite arguments (HFA, _Complex) are loaded
 * -- after the permutation, which may still read the s-registers they fill,
 * and before the GPR moves, which may overwrite the address they load from.
 *
 * Phase 1 runs after them, so r0-r3 already hold their final values: the
 * single-precision arguments materialized through a GPR scratch (imm/memory)
 * that excludes the GPR argument registers, then the doubles, packed from GPR
 * pairs. */
static void emit_vfp_arg_moves(CallGenContext *ctx, int phase)
{
  int dst[16], src[16];
  int dbl_arg[16];
  int ndbl = 0;
  const MachineOperand *mop[16];
  int n = 0;
  const uint32_t gpr_excl_all = (uint32_t)(ctx->call_site->registers_map & 0xffffu);

  for (int i = 0; i < ctx->argc && n < 16; ++i)
  {
    const TCCAbiArgLoc *loc = &ctx->layout->locs[i];
    if (loc->kind != TCC_ABI_LOC_VFP_REG || vfp_arg_is_composite(ctx, i))
      continue;

    /* Double: the value lives in a GPR pair (doubles are not VFP-resident —
     * there is no double-precision arithmetic on this FPU), so pack the two
     * halves straight into the d-register, last: the pack sources are
     * GPRs/memory, and float destinations never overlap a double's pair. */
    if (loc->reg_count == 2)
    {
      if (phase == 1 && ndbl < 16)
        dbl_arg[ndbl++] = i;
      continue;
    }
    const MachineOperand *m = &ctx->mops[i];
    const int vfp_src = (m->kind == MACH_OP_VFP_REG) ? m->u.reg.r0 : -1; /* -1 = materialize */
    if ((vfp_src >= 0) != (phase == 0))
      continue;
    dst[n] = loc->reg_base;
    src[n] = vfp_src;
    mop[n] = m;
    n++;
  }
  const uint32_t gpr_excl = gpr_excl_all;
  int remaining = n;
  while (remaining > 0)
  {
    int progressed = 0;
    for (int i = 0; i < n; ++i)
    {
      if (dst[i] < 0)
        continue;
      /* Safe to emit when this destination is not still needed as a source. */
      int needed = 0;
      for (int j = 0; j < n; ++j)
        if (dst[j] >= 0 && j != i && src[j] == dst[i])
        {
          needed = 1;
          break;
        }
      if (needed)
        continue;
      if (src[i] >= 0)
      {
        if (src[i] != dst[i])
          ot_check(th_vmov_register((uint16_t)dst[i], (uint16_t)src[i], 0)); /* vmov.f32 sd, ss */
      }
      else
      {
        MachineCodegenContext mctx = {0};
        int gpr = mach_ensure_in_reg(&mctx, mop[i], gpr_excl);
        ot_check(th_vmov_gp_sp((uint16_t)gpr, (uint16_t)dst[i], 0)); /* sd = gpr */
        mach_release_all(&mctx);
      }
      dst[i] = -1;
      remaining--;
      progressed = 1;
    }
    if (!progressed)
    {
      /* Only cycles of VFP-resident moves remain: park one source in s14. */
      for (int i = 0; i < n; ++i)
        if (dst[i] >= 0 && src[i] >= 0)
        {
          ot_check(th_vmov_register(VFP_SCRATCH0, (uint16_t)src[i], 0));
          src[i] = VFP_SCRATCH0;
          break;
        }
    }
  }
  if (phase == 0)
  {
    for (int i = 0; i < ctx->argc; ++i)
      if (vfp_arg_is_composite(ctx, i))
        emit_vfp_composite_arg(ctx, i);
    return;
  }
  for (int k = 0; k < ndbl; ++k)
    emit_vfp_double_arg(ctx, dbl_arg[k], gpr_excl_all);
}

/* The argument register that receives the indirect call target held in REG
 * when the target is also passed as a 32-bit register argument (`v(n, v)`),
 * or -1.  Once the argument moves are done that register holds the target, so
 * the call branches through it and needs no holding register.  REG itself is
 * preferred: an identity argument is never written. */
static int call_target_arg_reg(const CallGenContext *ctx, int reg)
{
  int found = -1;
  for (int i = 0; i < ctx->argc; ++i)
  {
    const TCCAbiArgLoc *loc = &ctx->layout->locs[i];
    const MachineOperand *mop = &ctx->mops[i];
    if (loc->kind != TCC_ABI_LOC_REG || loc->reg_count != 1)
      continue;
    if (mop->kind != MACH_OP_REG || mop->needs_deref || mop->is_64bit || mop->is_complex ||
        mop->btype == IROP_BTYPE_STRUCT || mop->u.reg.r0 != reg)
      continue;
    if (ARM_R0 + loc->reg_base == reg)
      return reg;
    if (found < 0)
      found = ARM_R0 + loc->reg_base;
  }
  return found;
}

/* Pre-save stack arguments that source from R0-R3 before register shuffle */
static void presave_stack_args_from_arg_regs(CallGenContext *ctx)
{
  for (int i = 0; i < ctx->argc; ++i)
  {
    const TCCAbiArgLoc *loc = &ctx->layout->locs[i];
    const MachineOperand *mop = &ctx->mops[i];
    const int bt = mop->btype;

    if (loc->kind == TCC_ABI_LOC_REG || loc->kind == TCC_ABI_LOC_VFP_REG)
      continue;
    if (bt == IROP_BTYPE_STRUCT || mop->is_complex)
      continue;
    if (mop->kind != MACH_OP_REG || mop->needs_deref)
      continue;

    if (mop->is_64bit)
    {
      /* Pre-save 64-bit register pair if either register is in R0-R3.
       * The register arg shuffle will overwrite R0-R3, so both halves
       * must be stored to the stack before that happens. */
      int r0 = mop->u.reg.r0;
      int r1 = mop->u.reg.r1;
      if ((thumb_is_hw_reg(r0) && r0 <= ARM_R3) || (thumb_is_hw_reg(r1) && r1 <= ARM_R3))
      {
        int stack_offset = loc->stack_off;
        if (thumb_is_hw_reg(r0))
          store_word_to_stack(r0, stack_offset);
        if (thumb_is_hw_reg(r1))
          store_word_to_stack(r1, stack_offset + 4);
      }
    }
    else
    {
      /* Only pre-save if operand is in R0-R3 (arg registers that get overwritten). */
      if (mop->u.reg.r0 <= ARM_R3)
      {
        store_word_to_stack(mop->u.reg.r0, loc->stack_off);
      }
    }
  }
}

/* True for a plain 32-bit immediate argument destined for a stack slot. */
static int is_simple_imm_stack_arg(const TCCAbiArgLoc *loc, const MachineOperand *mop)
{
  return loc->kind != TCC_ABI_LOC_REG && loc->kind != TCC_ABI_LOC_VFP_REG && mop->kind == MACH_OP_IMM &&
         !mop->needs_deref && !mop->is_64bit && mop->btype != IROP_BTYPE_STRUCT && !mop->is_complex;
}

/* Order by 4 KB window, then value, then offset.  Grouping equal values within a
 * window lets each distinct value be materialized once per window instead of once
 * per argument; the window ordering bounds base-register re-materialization. */
static int stack_imm_arg_cmp(const void *a, const void *b)
{
  const StackImmArg *x = (const StackImmArg *)a;
  const StackImmArg *y = (const StackImmArg *)b;
  int wx = x->off & ~0xFFF, wy = y->off & ~0xFFF;
  if (wx != wy)
    return wx < wy ? -1 : 1;
  if (x->val != y->val)
    return x->val < y->val ? -1 : 1;
  if (x->off != y->off)
    return x->off < y->off ? -1 : 1;
  return 0;
}

/* The core registers in R0-R3, IP, LR that still hold a value the call setup
 * reads after stack argument ARG_INDEX is placed: every register a pending
 * register move, a later stack argument or the call target names, and every
 * register already protected (identity arguments, move sources, the pre-saved
 * target).  Everything else is dead at the call -- the call clobbers it, so
 * nothing lives in it across the call -- and a block copy may use it freely.
 * TCC_DISABLE_PASS=codegen:struct_arg_live_save restores the full save. */
static uint32_t call_live_regs(const CallGenContext *ctx, int arg_index, const TCCAbiArgLoc *loc)
{
  const uint32_t all6 = (1u << R0) | (1u << R1) | (1u << R2) | (1u << R3) | (1u << R12) | (1u << ARM_LR);
  if (tcc_ir_opt_pass_disabled("codegen:struct_arg_live_save"))
    return all6;
  uint32_t live = scratch_global_exclude | ctx->call_target_regs;
  for (int i = 0; i < ctx->argc; ++i)
  {
    const MachineOperand *m = &ctx->mops[i];
    const TCCAbiArgLoc *li = &ctx->layout->locs[i];
    /* A stack-only argument placed before this one (or this one) is read. */
    if (li->kind != TCC_ABI_LOC_REG && li->kind != TCC_ABI_LOC_REG_STACK && li->kind != TCC_ABI_LOC_VFP_REG &&
        i <= arg_index)
      continue;
    if (m->kind != MACH_OP_REG)
      continue;
    /* A scalar stack argument held in R0-R3 was stored by
     * presave_stack_args_from_arg_regs before any placement: dead by now. */
    if (li->kind != TCC_ABI_LOC_REG && li->kind != TCC_ABI_LOC_REG_STACK && li->kind != TCC_ABI_LOC_VFP_REG &&
        !m->needs_deref && m->btype != IROP_BTYPE_STRUCT && !m->is_complex &&
        ((m->u.reg.r0 >= 0 && m->u.reg.r0 <= ARM_R3) ||
         (m->is_64bit && m->u.reg.r1 >= 0 && m->u.reg.r1 <= ARM_R3)))
      continue;
    if (m->u.reg.r0 >= 0 && m->u.reg.r0 < 16)
      live |= 1u << m->u.reg.r0;
    if (m->u.reg.r1 >= 0 && m->u.reg.r1 < 16)
      live |= 1u << m->u.reg.r1;
  }
  return live & all6;
}

/* Emit a single non-simple-immediate stack argument (struct/complex/64-bit, or a
 * non-immediate 32-bit source).  Extracted from place_stack_arguments so both the
 * inline and the grouped emission paths share identical handling. */
static void place_one_stack_arg(CallGenContext *ctx, const TCCAbiArgLoc *loc, const MachineOperand *mop,
                                int stack_offset, int arg_index)
{
  if (mop->btype == IROP_BTYPE_STRUCT || mop->is_complex)
  {
    /* Complex values in a register pair: store the stack portion directly
     * from registers instead of treating the pair as a memory pointer. */
    if (mop->is_complex && mop->kind == MACH_OP_REG && !mop->needs_deref && mop->is_64bit)
    {
      int words_in_regs = (loc->kind == TCC_ABI_LOC_REG_STACK) ? loc->reg_count : 0;
      int stack_bytes = (loc->kind == TCC_ABI_LOC_REG_STACK) ? loc->stack_size : loc->size;
      int stack_words = (stack_bytes + 3) / 4;
      int pair_regs[2] = {mop->u.reg.r0, mop->u.reg.r1};
      for (int w = 0; w < stack_words; w++)
      {
        int reg_idx = words_in_regs + w;
        if (reg_idx < 2)
          store_word_to_stack(pair_regs[reg_idx], stack_offset + w * 4);
      }
    }
    else if (mop->is_complex && mop->kind == MACH_OP_IMM)
    {
      /* Complex immediate on stack: split 64-bit packed value into words. */
      const uint64_t imm64 = (uint64_t)mop->u.imm.val;
      int words_in_regs = (loc->kind == TCC_ABI_LOC_REG_STACK) ? loc->reg_count : 0;
      int stack_bytes = (loc->kind == TCC_ABI_LOC_REG_STACK) ? loc->stack_size : loc->size;
      int stack_words = (stack_bytes + 3) / 4;
      int scr = find_call_scratch(0, ctx->arg_move_dst_mask);
      for (int w = 0; w < stack_words; w++)
      {
        int word_idx = words_in_regs + w;
        uint32_t word_val = (uint32_t)(imm64 >> (word_idx * 32));
        load_immediate(scr, word_val, NULL, false);
        store_word_to_stack(scr, stack_offset + w * 4);
      }
    }
    else
    {
      /* Struct's natural alignment gates source-side LDRD (see
       * place_stack_arg_struct).  Default conservatively to 1 (no LDRD) when
       * the originating IR operand is unavailable. */
      int src_align = 1;
      if (ctx->args && arg_index >= 0 && arg_index < ctx->argc)
      {
        int a = 0;
        irop_type_size_align(ctx->args[arg_index], &a);
        if (a > 0)
          src_align = a;
      }
      place_stack_arg_struct(mop, loc, stack_offset, src_align, ctx->arg_move_dst_mask,
                             call_live_regs(ctx, arg_index, loc));
    }
  }
  else if (mop->is_64bit)
    place_stack_arg_64bit(mop, stack_offset, tcc_state->ir, ctx->arg_move_dst_mask);
  else
    place_stack_arg_32bit(mop, stack_offset, ctx);
}

/* Inline (original-order) emission of every stack argument.  Used for the common
 * case where stack args stay within the immediate-offset store range. */
static void place_stack_arguments_inline(CallGenContext *ctx)
{
  int cached_imm_reg = -1;
  uint32_t cached_imm_val = 0;

  for (int i = 0; i < ctx->argc; ++i)
  {
    const TCCAbiArgLoc *loc = &ctx->layout->locs[i];
    const MachineOperand *mop = &ctx->mops[i];

    if (loc->kind == TCC_ABI_LOC_REG || loc->kind == TCC_ABI_LOC_VFP_REG)
      continue;

    int stack_offset = loc->stack_off;

    if (is_simple_imm_stack_arg(loc, mop))
    {
      uint32_t val = (uint32_t)mop->u.imm.val;
      int scr = find_call_scratch(0, ctx->arg_move_dst_mask);
      if (cached_imm_reg != scr || cached_imm_val != val)
      {
        load_immediate(scr, val, NULL, false);
        cached_imm_reg = scr;
        cached_imm_val = val;
      }
      store_word_to_stack(scr, stack_offset);
      continue;
    }

    cached_imm_reg = -1;
    place_one_stack_arg(ctx, loc, mop, stack_offset, i);
  }
}

/* Place all stack arguments.
 *
 * For the common case the inline path is byte-identical to before.  When simple
 * 32-bit immediate stack args spill beyond the immediate-offset store range
 * (offset > 4092) — exactly where the naive path emits movw+indexed (3 instr/arg)
 * — a windowed/grouped path is used instead:
 *   - a base register holds sp+window so each store is a single str.w [rb,#disp]
 *     (re-materialized only when crossing a 4 KB window, ~once / 1024 stores);
 *   - the immediate stores are reordered by (window, value) so each distinct
 *     value is loaded once per window rather than once per argument.
 * Reordering pure-immediate stores to distinct, non-aliasing stack slots leaves
 * the pre-call stack image unchanged, so it is observationally identical. */
/* TCC_NO_STACK_ARG_GROUP restores the ungrouped inline path, the control arm
 * of the windowed stack-argument A/B. */
TCC_DBG_ENV_FLAG(th_no_stack_arg_group, "TCC_NO_STACK_ARG_GROUP")

static void place_stack_arguments(CallGenContext *ctx)
{
  int max_imm_off = -1;
  int imm_count = 0;
  for (int i = 0; i < ctx->argc; ++i)
  {
    const TCCAbiArgLoc *loc = &ctx->layout->locs[i];
    const MachineOperand *mop = &ctx->mops[i];
    if (is_simple_imm_stack_arg(loc, mop))
    {
      imm_count++;
      if (loc->stack_off > max_imm_off)
        max_imm_off = loc->stack_off;
    }
  }

  if (!(max_imm_off > 4092 && imm_count >= 2) || th_no_stack_arg_group())
  {
    place_stack_arguments_inline(ctx);
    return;
  }

  /* --- Windowed/grouped path --- */

  /* Pass 1: emit every non-simple-immediate stack arg first, in original order. */
  for (int i = 0; i < ctx->argc; ++i)
  {
    const TCCAbiArgLoc *loc = &ctx->layout->locs[i];
    const MachineOperand *mop = &ctx->mops[i];
    if (loc->kind == TCC_ABI_LOC_REG || loc->kind == TCC_ABI_LOC_VFP_REG || is_simple_imm_stack_arg(loc, mop))
      continue;
    place_one_stack_arg(ctx, loc, mop, loc->stack_off, i);
  }

  /* Reserve two stable scratch registers: rv (holds the value) and rb (base
   * address).  Both are free across the whole argument-setup region — the call's
   * register args are moved in afterwards, and find_call_scratch only returns
   * registers that are dead here or are arg-move destinations (overwritten
   * later).  Prefer the lower-numbered register for rv so value materialization
   * can use the 16-bit MOVS encoding. */
  int s0 = find_call_scratch(0, ctx->arg_move_dst_mask);
  int s1 = find_call_scratch(1u << s0, ctx->arg_move_dst_mask);
  if (s1 < s0)
  {
    int t = s0;
    s0 = s1;
    s1 = t;
  }
  int rv = s0, rb = s1;
  int regs_ok = (rv != rb && rv >= 0 && rv < 16 && rb >= 0 && rb < 16 && rv != ARM_SP && rv != ARM_PC &&
                 rb != ARM_SP && rb != ARM_PC);

  StackImmArg *items = regs_ok ? tcc_malloc(sizeof(StackImmArg) * imm_count) : NULL;
  if (!items)
  {
    /* Out of stable registers (or alloc failure): emit the immediate args inline. */
    int cached_imm_reg = -1;
    uint32_t cached_imm_val = 0;
    for (int i = 0; i < ctx->argc; ++i)
    {
      const TCCAbiArgLoc *loc = &ctx->layout->locs[i];
      const MachineOperand *mop = &ctx->mops[i];
      if (!is_simple_imm_stack_arg(loc, mop))
        continue;
      uint32_t val = (uint32_t)mop->u.imm.val;
      int scr = find_call_scratch(0, ctx->arg_move_dst_mask);
      if (cached_imm_reg != scr || cached_imm_val != val)
      {
        load_immediate(scr, val, NULL, false);
        cached_imm_reg = scr;
        cached_imm_val = val;
      }
      store_word_to_stack(scr, loc->stack_off);
    }
    return;
  }

  int n = 0;
  for (int i = 0; i < ctx->argc; ++i)
  {
    const TCCAbiArgLoc *loc = &ctx->layout->locs[i];
    const MachineOperand *mop = &ctx->mops[i];
    if (!is_simple_imm_stack_arg(loc, mop))
      continue;
    items[n].off = loc->stack_off;
    items[n].val = (uint32_t)mop->u.imm.val;
    n++;
  }
  tcc_qsort(items, n, sizeof(StackImmArg), stack_imm_arg_cmp);

  uint32_t saved_excl = scratch_global_exclude;
  scratch_global_exclude |= (1u << rv) | (1u << rb);

  int cur_window = -1; /* base offset of the window currently in rb */
  int have_val = 0;
  uint32_t cur_val = 0;
  for (int k = 0; k < n; ++k)
  {
    int off = items[k].off;
    uint32_t val = items[k].val;
    int window = off & ~0xFFF;
    int disp = off & 0xFFF;
    int base_reg;

    if (window == 0)
    {
      base_reg = ARM_SP; /* sp+0 — store directly off sp, no base register needed */
    }
    else
    {
      if (window != cur_window)
      {
        thumb_opcode op = th_add_imm(rb, ARM_SP, (uint32_t)window, flags_safe(), ENFORCE_ENCODING_NONE);
        if (is_valid_opcode(op))
          ot(op);
        else
        {
          load_full_const(rb, PREG_NONE, (uint32_t)window, 0);
          ot_check(th_add_reg(rb, ARM_SP, rb, flags_safe(), THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
        }
      }
      base_reg = rb;
    }
    cur_window = window;

    if (!have_val || cur_val != val)
    {
      load_immediate(rv, val, NULL, false);
      have_val = 1;
      cur_val = val;
    }

    if (!store_word_to_base(rv, base_reg, disp, 0))
    {
      /* disp <= 4092 always encodes via str.w; keep a correct fallback regardless. */
      ScratchRegAlloc sc = get_scratch_reg_with_save((1u << rv) | (1u << base_reg));
      load_immediate(sc.reg, (uint32_t)off, NULL, false);
      ot_check(th_str_reg(rv, ARM_SP, sc.reg, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
      restore_scratch_reg(&sc);
    }
  }

  scratch_global_exclude = saved_excl;
  tcc_free(items);
}

/* Handle return value after call (MOP path).
 * The 'dest_mop' describes where the return value must be written.
 * mach_writeback_dest() handles all destination kinds:
 *   MACH_OP_REG   — emit MOV dest.r0, ARM_R0 when needed
 *   MACH_OP_SPILL — emit STR R0 to the spill slot
 *   MACH_OP_PARAM_STACK — emit STR R0 to the param stack slot
 *   MACH_OP_NONE  — no-op (void return or drop_value)
 * 64-bit pairs (int64, double, complex float) are split into lo/hi halves
 * via mach_make_lo_half / mach_make_hi_half (R0 → lo, R1 → hi). */
/* True when a call targets a soft-float __aeabi_* runtime helper (see
 * thumb_callee_sym_is_aeabi). */
static int mach_callee_is_aeabi(const MachineOperand *func_mop)
{
  return func_mop->kind == MACH_OP_SYMBOL && thumb_callee_sym_is_aeabi(func_mop->u.sym.sym);
}

/* True when the callee is a function this translation unit has already
 * emitted, so the call cannot leave the module and R9 survives it.
 *
 * Only an already-defined symbol qualifies: the test reads the ELF symbol,
 * and a forward reference (callee defined later in the TU, or extern) is
 * still SHN_UNDEF here, so it conservatively keeps the reload.  That also
 * makes the predicate stable across the dry run and the real pass, which it
 * must be or literal-pool windows desync -- the dry run skips
 * put_extern_sym, but that only ever creates UNDEF entries, so a symbol
 * reads defined in both passes or neither.
 *
 * STT_FUNC is required so a same-named data symbol cannot qualify, and the
 * target section must be executable: an SHN_ABS or data-section symbol is
 * not a function body we compiled under this R9 contract. */
/* -fmodule-local-calls: a global function with no definition in this
 * translation unit binds inside the module unless a shared library in the
 * library paths exports it (tcc_yaff_import_set_has).  Per-TU builds of
 * toybox, GNU make and tcc itself made every call to a function in another of
 * their own files reload R9 -- 20,792 of the native tcc's 22,035 reloads.  The
 * assumption is recorded on the BL (R_ARM_YASOS_LOCAL_CALL) and the linker
 * rejects the link if the call binds to the PLT after all. */
static int thumb_callee_assumed_local(Sym *sym)
{
  /* An asm symbol is a helper the compiler calls on its own (__aeabi_memmove,
   * __tcc_strcmp); libtcc1.a defines most of them, so they bind locally. */
  if (!tcc_state->module_local_calls || ((sym->type.t & VT_BTYPE) != VT_FUNC && !IS_ASM_SYM(sym)))
    return 0;
  /* A weak callee may bind to nothing at all; dllimport is the per-declaration
   * opt-out for a function a library outside the library paths provides. */
  if (sym->a.weak || sym->a.dllimport)
    return 0;
  /* The link name, not the spelling: __builtin_printf binds as `printf`. */
  return tcc_yaff_import_set_has(tcc_state, get_tok_str(sym->asm_label ? sym->asm_label : sym->v, NULL)) == 0;
}

/* Only an undefined callee has anything to verify at link time. */
int thumb_callee_needs_local_call_marker(Sym *sym)
{
  ElfSym *esym;
  if (!text_and_data_separation || !sym || (sym->v & SYM_FIELD))
    return 0;
  if ((sym->type.t & VT_BTYPE) == VT_FUNC && (sym->type.t & VT_STATIC))
    return 0;
  esym = elfsym(sym);
  if (esym && esym->st_shndx != SHN_UNDEF)
    return 0;
  return thumb_callee_assumed_local(sym);
}

/* libc's memcpy/memset/memcmp are PLT calls, which also cost the caller an R9
 * reload, and byte loops besides; libtcc1 has local versions with the same
 * contract (its __aeabi_memcpy returns dest, see arm_mem.S). A direct call to
 * one of them binds to the libtcc1 copy instead: gcall_or_jump_mop emits the
 * helper, and thumb_sym_in_this_module counts the call as module-local, so it
 * keeps R9. Not under -nostdlib (libc itself, which defines them) nor when this
 * TU defines the function; only when the libtcc1.a in the library paths has
 * the helper. Returns the helper symbol, or NULL to call `sym` as written. */
Sym *thumb_local_libc_helper(Sym *sym)
{
  static const char *const map[][2] = {
      {"memcpy", "__aeabi_memcpy"},
      {"memset", "__tcc_memset"},
      {"memcmp", "__tcc_memcmp"},
  };
  ElfSym *esym;
  const char *name;

  if (!text_and_data_separation || !tcc_state->module_local_calls || tcc_state->nostdlib || !sym ||
      (sym->v & SYM_FIELD) || (sym->type.t & VT_BTYPE) != VT_FUNC || (sym->type.t & VT_STATIC) ||
      sym->a.weak || sym->a.dllimport)
    return NULL;
  esym = elfsym(sym);
  if (esym && esym->st_shndx != SHN_UNDEF)
    return NULL;
  name = get_tok_str(sym->asm_label ? sym->asm_label : sym->v, NULL);
  for (unsigned i = 0; i < sizeof(map) / sizeof(map[0]); i++)
    if (!strcmp(name, map[i][0]))
      return tcc_yaff_libtcc1_has(tcc_state, map[i][1]) ? external_helper_sym(tok_alloc_const(map[i][1])) : NULL;
  return NULL;
}

static int thumb_sym_in_this_module(Sym *sym)
{
  ElfSym *esym;
  Section *sec;

  if (!text_and_data_separation || !sym)
    return 0;
  if (sym->v & SYM_FIELD)
    return 0;
  if (thumb_local_libc_helper(sym))
    return 1;
  /* A static function is this translation unit's own, so this module's,
   * whether or not its definition has been seen yet: calls to functions
   * defined further down would otherwise reload R9 for nothing -- in the Zig C
   * backend's output, where every function is static, 83,487 reloads. */
  if ((sym->type.t & VT_BTYPE) == VT_FUNC && (sym->type.t & VT_STATIC))
    return 1;
  esym = elfsym(sym);
  if (!esym || esym->st_shndx == SHN_UNDEF)
    return thumb_callee_assumed_local(sym);
  if (esym->st_shndx == SHN_ABS || esym->st_shndx >= tcc_state->nb_sections)
    return 0;
  if (ELFW(ST_TYPE)(esym->st_info) != STT_FUNC)
    return 0;
  sec = tcc_state->sections[esym->st_shndx];
  return sec && (sec->sh_flags & SHF_EXECINSTR) != 0;
}
int thumb_callee_in_this_module(const MachineOperand *func_mop)
{
  if (func_mop->kind != MACH_OP_SYMBOL)
    return 0;
  return thumb_sym_in_this_module(func_mop->u.sym.sym);
}

/* A direct call that never returns needs no R9 reload after it: nothing after
 * it runs. (A longjmp out of the callee comes back through setjmp's own call
 * site, which reloads.) */
int thumb_callee_noreturn(const MachineOperand *func_mop)
{
  Sym *sym;
  if (func_mop->kind != MACH_OP_SYMBOL || !(sym = func_mop->u.sym.sym) || (sym->v & SYM_FIELD))
    return 0;
  return (sym->type.t & VT_BTYPE) == VT_FUNC && sym->type.ref && sym->type.ref->f.func_noreturn;
}

ST_FUNC int tcc_gen_machine_callee_in_this_module(Sym *sym)
{
  return thumb_sym_in_this_module(sym);
}

static void handle_return_value_mop(const MachineOperand *dest_mop, int drop_value, int soft_float_return)
{
  if (drop_value)
    return;

  /* Hard-float single-precision return arrives in s0 (AAPCS VFP) — unless the
   * callee is a soft __aeabi_* helper, which returns it in R0. */
  if (tcc_state && tcc_state->float_abi == ARM_HARD_FLOAT && !soft_float_return && !dest_mop->is_64bit &&
      dest_mop->btype == IROP_BTYPE_FLOAT32)
  {
    if (dest_mop->kind == MACH_OP_VFP_REG)
    {
      if (dest_mop->u.reg.r0 != 0)
        ot_check(th_vmov_register((uint16_t)dest_mop->u.reg.r0, 0, 0)); /* s_dest = s0 */
    }
    else
    {
      MachineCodegenContext ctx = {0};
      int r = mach_get_dest_reg(&ctx, dest_mop, 0);
      ot_check(th_vmov_gp_sp((uint16_t)r, 0, 1)); /* r = s0 */
      mach_writeback_dest(dest_mop, r);
      mach_release_all(&ctx);
    }
    return;
  }

  /* Hard-float double return arrives in d0 (AAPCS VFP) — unless the callee is a
   * soft __aeabi_* helper, whose result comes back in the R0:R1 pair. */
  if (tcc_state && tcc_state->float_abi == ARM_HARD_FLOAT && !soft_float_return && dest_mop->is_64bit &&
      dest_mop->btype == IROP_BTYPE_FLOAT64 && !dest_mop->is_complex)
  {
    MachineCodegenContext ctx = {0};
    MachineOperand lo = mach_make_lo_half(dest_mop);
    MachineOperand hi = mach_make_hi_half(dest_mop);
    lo.btype = IROP_BTYPE_INT32;
    hi.btype = IROP_BTYPE_INT32;
    int rlo = mach_get_dest_reg(&ctx, &lo, 0);
    int rhi = mach_get_dest_reg(&ctx, &hi, (1u << (uint32_t)rlo));
    ot_check(th_vmov_2gp_dp((uint16_t)rlo, (uint16_t)rhi, 0, 1)); /* rlo,rhi = d0 */
    mach_writeback_dest(&lo, rlo);
    mach_writeback_dest(&hi, rhi);
    mach_release_all(&ctx);
    return;
  }

  if (dest_mop->is_64bit)
  {
    /* 64-bit return value: R0 = low word, R1 = high word (AAPCS). */
    MachineOperand lo = mach_make_lo_half(dest_mop);
    lo.btype = IROP_BTYPE_INT32;
    MachineOperand hi = mach_make_hi_half(dest_mop);
    hi.btype = IROP_BTYPE_INT32;
    mach_writeback_dest(&lo, ARM_R0);
    mach_writeback_dest(&hi, ARM_R1);
    return;
  }
  mach_writeback_dest(dest_mop, ARM_R0);
}

/* ======================================================================== */

/* tcc_gen_machine_func_call_mop — MOP-path function call code generator.
 *
 * The function target and return-value destination are passed as MachineOperands.
 * The call_id_op is always an immediate IROperand (no fill needed).
 *
 * Phase 5g: func_mop replaces the old filled IROperand func_target.
 * gcall_or_jump_mop() replaces gcall_or_jump_ir().
 */
/* Largest copy an aligned __aeabi_memmove4/8 or __aeabi_memcpy4/8 call is
 * unrolled inline for; larger ones become a copy loop, and 0 keeps every
 * call.  Each such call is a struct assignment (vstore.c): the Zig C backend
 * copies 20-128 byte aggregates millions of times, and every call ran ~45
 * instructions in the helper for what four registers of LDM/STM do in a few. */
TCC_DBG_ENV_INT(inline_copy_max, "TCC_INLINE_COPY_MAX", 128)

/* True when a pointer argument is provably word aligned: the address of a
 * frame slot at a word offset (the frame base is 8-aligned), or of a symbol
 * defined here in a section aligned to 4 at a word offset -- a string
 * literal's or an initialiser template's .rodata copy. */
static int thumb_copy_arg_word_aligned(TCCIRState *ir, IROperand op)
{
  if (op.is_lval || op.is_llocal)
    return 0;
  const int tag = irop_get_tag(op);
  if (tag == IROP_TAG_STACKOFF)
    return op.is_local && irop_get_vreg(op) < 0 && !(irop_get_stack_offset(op) & 3);
  if (tag != IROP_TAG_SYMREF || !ir)
    return 0;
  IRPoolSymref *sr = irop_get_symref_ex(ir, op);
  if (!sr || !sr->sym)
    return 0;
  ElfSym *esym = elfsym(sr->sym);
  if (!esym || esym->st_shndx == SHN_UNDEF || esym->st_shndx >= SHN_LORESERVE ||
      esym->st_shndx >= (unsigned)tcc_state->nb_sections || ELFW(ST_BIND)(esym->st_info) != STB_LOCAL)
    return 0;
  Section *sec = tcc_state->sections[esym->st_shndx];
  return sec && sec->sh_addralign >= 4 && !((esym->st_value + (addr_t)sr->addend) & 3);
}

/* Two frame slots a memmove could be shifting within one buffer: the forward
 * word copy is only right when they coincide or are disjoint. */
static int thumb_copy_args_overlap(const IROperand *args, int n)
{
  if (irop_get_tag(args[0]) != IROP_TAG_STACKOFF || irop_get_tag(args[1]) != IROP_TAG_STACKOFF)
    return 0;
  const int d = irop_get_stack_offset(args[0]) - irop_get_stack_offset(args[1]);
  return d && d > -n && d < n;
}

/* A call to an aligned block copy with a constant size, emitted as the copy
 * itself: dst in R0, src in R1 (the argument moves are done), 16 bytes per
 * `ldmia r1!, {r2,r3,r12,lr}` / `stmia r0!` pair, the rest in one smaller
 * pair or a single word.  Every register it touches is one the call would
 * have clobbered -- the IR still sees a call, so the allocator keeps nothing
 * live in R0-R3/R12, and the function saves LR.  Both pointers are word
 * aligned (the helper's contract, or thumb_copy_arg_word_aligned's proof for
 * the plain __aeabi_memcpy/memmove), which LDM/STM need.  Source and
 * destination are the same object or disjoint objects (C struct assignment
 * allows no partial overlap), and each chunk is loaded before it is stored, so
 * the memmove variants are safe copied forwards. */
/* How thumb_inline_aligned_copy_call will emit this call: 0 as a call, 1 by
 * thumb_emit_word_copy_r0_r1, 2 as unrolled 16-byte LDM/STM pairs.  Decided
 * before the argument moves, so the size never goes to R2 for an expansion
 * that does not read it (`movs r2, #72` right before the `ldmia r1!,
 * {r2, r3, ip, lr}` overwriting it: 7k dead moves in zig.c). */
static int thumb_inline_copy_mode(TCCIRState *ir, MachineOperand func_mop, const IROperand *args, int argc)
{
  if (func_mop.kind != MACH_OP_SYMBOL || !func_mop.u.sym.sym || func_mop.u.sym.addend || argc != 3)
    return 0;
  const char *nm = get_tok_str(func_mop.u.sym.sym->v, NULL);
  IROperand n_op = args[2];
  if (irop_get_tag(n_op) != IROP_TAG_IMM32 || n_op.is_lval)
    return 0;
  int n = irop_get_imm32(n_op);
  /* -Os: a copy longer than the largest copy stub stays the call (a MOVS and
   * a BL) -- inline it is a 14-byte LDM/STM loop plus its count and tail. */
  if (TCC_OPT(tcc_state, optimize_size) && n > COPY_STUB_MAX_WORDS * 4)
    return 0;
  /* A volatile side keeps its accesses visible in THIS function -- the
   * volatile_trace contract: exact widths, counts and order.  The __tcc_wcopy_N
   * stubs and thumb_emit_word_copy_r0_r1's own loop hide the accesses inside
   * libtcc1 / one shared routine, so a volatile copy expands unrolled here
   * instead (mode 3; vstore leaves the copy's argument operands unmarked for
   * exactly this query).  The unrolled body moves whole words only, so a
   * plain-variant copy with a byte tail keeps its call. */
  const int copy_volatile = tcc_ir_access_is_volatile(ir, args[0]) || tcc_ir_access_is_volatile(ir, args[1]);
  /* An unaligned-contract copy whose pointers are word aligned after all: a
   * string literal copied into a char array field of a word-aligned local
   * (Zig's `undefined` fill, 48 bytes in every Wyhash.init). */
  if ((!strcmp(nm, "__aeabi_memcpy") || !strcmp(nm, "__aeabi_memmove")) && n >= 8 && inline_copy_max() &&
      thumb_copy_arg_word_aligned(ir, args[0]) && thumb_copy_arg_word_aligned(ir, args[1]) &&
      !thumb_copy_args_overlap(args, n))
    return (copy_volatile && !(n & 3)) ? 3 : 1;
  if (strcmp(nm, "__aeabi_memmove4") && strcmp(nm, "__aeabi_memmove8") && strcmp(nm, "__aeabi_memcpy4") &&
      strcmp(nm, "__aeabi_memcpy8"))
    return 0;
  if (n <= 0 || (n & 3) || !inline_copy_max())
    return 0;
  if (copy_volatile)
    return 3;
  return n > inline_copy_max() ? 1 : 2;
}

/* A call to an aligned block copy with a constant size, emitted as the copy
 * itself: dst in R0, src in R1 (the argument moves are done), 16 bytes per
 * `ldmia r1!, {r2,r3,r12,lr}` / `stmia r0!` pair, the rest in one smaller
 * pair or a single word.  Every register it touches is one the call would
 * have clobbered -- the IR still sees a call, so the allocator keeps nothing
 * live in R0-R3/R12, and the function saves LR.  Both pointers are word
 * aligned (the helper's contract, or thumb_copy_arg_word_aligned's proof for
 * the plain __aeabi_memcpy/memmove), which LDM/STM need.  Source and
 * destination are the same object or disjoint objects (C struct assignment
 * allows no partial overlap), and each chunk is loaded before it is stored, so
 * the memmove variants are safe copied forwards.  The size is not read: it
 * is not moved to R2 (thumb_inline_copy_mode). */
static int thumb_inline_aligned_copy_call(TCCIRState *ir, MachineOperand func_mop, const IROperand *args, int argc)
{
  const int mode = thumb_inline_copy_mode(ir, func_mop, args, argc);
  if (!mode)
    return 0;
  int n = irop_get_imm32(args[2]);
  if (mode == 1)
  {
    thumb_emit_word_copy_r0_r1(n);
    return 1;
  }
  if (mode == 3)
  {
    /* Volatile side: every access stays visible in this function.  Outside
     * the copy-stub window thumb_emit_word_copy_r0_r1 already is the
     * unrolled pairs or its own visible 12-bytes-per-turn loop; inside the
     * window (where it would call __tcc_wcopy_N) expand unrolled here. */
    if (n < 3 * 4 || n > COPY_STUB_MAX_WORDS * 4)
    {
      thumb_emit_word_copy_r0_r1(n);
      return 1;
    }
  }
  else if (thumb_copy_stub_call(n))
    return 1;
  const uint32_t quad = (1u << R2) | (1u << R3) | (1u << R_IP) | (1u << R_LR);
  for (; n >= 16; n -= 16)
  {
    ot_check(th_ldm(R1, quad, 1, ENFORCE_ENCODING_NONE));
    ot_check(th_stm(R0, quad, 1, ENFORCE_ENCODING_NONE));
  }
  if (n == 4)
  {
    ot_check(th_ldr_imm(R2, R1, 0, 6, ENFORCE_ENCODING_NONE));
    ot_check(th_str_imm(R2, R0, 0, 6, ENFORCE_ENCODING_NONE));
  }
  else if (n)
  {
    uint32_t regs = n == 8 ? (1u << R2) | (1u << R3) : (1u << R2) | (1u << R3) | (1u << R_IP);
    ot_check(th_ldm(R1, regs, 1, ENFORCE_ENCODING_NONE));
    ot_check(th_stm(R0, regs, 1, ENFORCE_ENCODING_NONE));
  }
  return 1;
}

/* An -minline-atomics read-modify-write (inline_atomic_rmw): a call to
 * __tcc_ax_<op><size>_<flags> emitted as the LDREX/STREX loop itself, on the
 * registers its operands already occupy -- pointer, then the operand (for
 * "cas" the expected value, then the desired one) -- with the old value,
 * zero-extended like the runtime helpers', in the result's register.  The loop
 * takes its other registers from the scratch allocator, so the register
 * allocator treats the call as writing nothing but its result
 * (tcc_ir_call_clobbers_nothing).  Flags: 1 acquire, 2 release, 4 a DMB after
 * (seq_cst, for the plain `ldr; dmb` of a seq_cst load to come).  A core with
 * LDAEX/STLEX orders through them, as LLVM does; one without (ARMv7-M) puts
 * DMBs around the loop instead. */
static int thumb_is_machine_call(MachineOperand func_mop, const char *prefix)
{
  return func_mop.kind == MACH_OP_SYMBOL && func_mop.u.sym.sym && !func_mop.u.sym.addend &&
         !strncmp(get_tok_str(func_mop.u.sym.sym->v, NULL), prefix, 9);
}

static void thumb_atomic_load_ex(int size, int acq, uint32_t rt, uint32_t rn)
{
  if (size == 1)
    ot_check(acq ? th_ldaexb(rt, rn) : th_ldrexb(rt, rn));
  else if (size == 2)
    ot_check(acq ? th_ldaexh(rt, rn) : th_ldrexh(rt, rn));
  else
    ot_check(acq ? th_ldaex(rt, rn) : th_ldrex(rt, rn, 0));
}

static void thumb_atomic_store_ex(int size, int rel, uint32_t rd, uint32_t rt, uint32_t rn)
{
  if (size == 1)
    ot_check(rel ? th_stlexb(rd, rt, rn) : th_strexb(rd, rt, rn));
  else if (size == 2)
    ot_check(rel ? th_stlexh(rd, rt, rn) : th_strexh(rd, rt, rn));
  else
    ot_check(rel ? th_stlex(rd, rt, rn) : th_strex(rd, rt, rn, 0));
}

static void thumb_inline_atomic_call(TCCIRState *ir, int call_idx, int call_id, int argc, MachineOperand func_mop,
                                     MachineOperand dest_mop, int drop_value)
{
  enum
  {
    AX_CAS,
    AX_XCHG,
    AX_ADD,
    AX_SUB,
    AX_AND,
    AX_OR,
    AX_XOR,
    AX_NAND,
    AX_LDA,
    AX_STL
  };
  static const char *const ops[] = {"cas", "xchg", "add", "sub", "and", "or", "xor", "nand", "lda", "stl"};
  const char *const name = get_tok_str(func_mop.u.sym.sym->v, NULL);
  const char *p = name + 9;
  int op = -1;
  for (int i = 0; i < (int)(sizeof(ops) / sizeof(ops[0])); i++)
  {
    const size_t len = strlen(ops[i]);
    if (!strncmp(p, ops[i], len) && p[len] >= '1' && p[len] <= '4')
    {
      op = i;
      p += len;
      break;
    }
  }
  const int size = op >= 0 ? *p - '0' : 0;
  const int flags = op >= 0 && p[1] == '_' ? atoi(p + 2) : -1;
  if ((size != 1 && size != 2 && size != 4) || flags < 0 || flags > 7 ||
      argc != (op == AX_CAS ? 3 : op == AX_LDA ? 1 : 2))
    tcc_ice("unknown inline atomic '%s'", name);

  /* The operands, where they are. */
  TCCAbiCallLayout layout;
  memset(&layout, 0, sizeof(layout));
  small_sequence(ThumbIROperandSequence) args_owner = {0};
  small_sequence(ThumbMachineOperandSequence) mops_owner = {0};
  if (thumb_build_call_layout_from_ir(ir, call_idx, call_id, argc, &layout, &args_owner, &mops_owner) != argc)
    tcc_ice("inline atomic '%s': bad operands", name);
  const MachineOperand *mops = ThumbMachineOperandSequence_data(&mops_owner);
  MachineCodegenContext mctx = {0};
  uint32_t used = 0;
  const int ptr = mach_ensure_in_reg(&mctx, &mops[0], used);
  used |= 1u << ptr;
  const int v8 = arm_target_dependent.feat.ldaex;
  if (op == AX_LDA)
  {
    /* An acquire load (inline_atomic_access): LDA, or LDR; DMB. */
    if (layout.locs)
      tcc_free(layout.locs);
    int rt = !drop_value && dest_mop.kind != MACH_OP_NONE ? mach_get_dest_reg(&mctx, &dest_mop, used)
                                                          : mach_alloc_scratch(&mctx, used);
    if (v8)
      ot_check(size == 1 ? th_ldab(rt, ptr) : size == 2 ? th_ldah(rt, ptr) : th_lda(rt, ptr));
    else
    {
      if (size == 1)
        ot_check(th_ldrb_imm(rt, ptr, 0, 6, ENFORCE_ENCODING_NONE));
      else if (size == 2)
        ot_check(th_ldrh_imm(rt, ptr, 0, 6, ENFORCE_ENCODING_NONE));
      else
        ot_check_ldr_imm(rt, ptr, 0, 6, ENFORCE_ENCODING_NONE);
      ot_check(th_dmb(0xf));
    }
    if (!drop_value && dest_mop.kind != MACH_OP_NONE)
      mach_writeback_dest(&dest_mop, rt);
    mach_release_all(&mctx);
    return;
  }
  /* A compare-exchange against a constant that CMP's immediate holds (the
   * spin lock's free == 0): no register for it. */
  int cas_imm = -1;
  if (op == AX_CAS && mops[1].kind == MACH_OP_IMM)
  {
    const uint32_t v = (uint32_t)mops[1].u.imm.val & (size == 1 ? 0xffu : size == 2 ? 0xffffu : 0xffffffffu);
    if (v <= 255)
      cas_imm = (int)v;
  }
  int val = cas_imm >= 0 ? -1 : mach_ensure_in_reg(&mctx, &mops[1], used);
  if (val >= 0)
    used |= 1u << val;
  if (op == AX_STL)
  {
    /* A release store (inline_atomic_access): STL, or DMB; STR. */
    if (layout.locs)
      tcc_free(layout.locs);
    if (v8)
      ot_check(size == 1 ? th_stlb(val, ptr) : size == 2 ? th_stlh(val, ptr) : th_stl(val, ptr));
    else
    {
      ot_check(th_dmb(0xf));
      if (size == 1)
        ot_check(th_strb_imm(val, ptr, 0, 6, ENFORCE_ENCODING_NONE));
      else if (size == 2)
        ot_check(th_strh_imm(val, ptr, 0, 6, ENFORCE_ENCODING_NONE));
      else
        ot_check_str_imm(val, ptr, 0, 6, ENFORCE_ENCODING_NONE);
    }
    mach_release_all(&mctx);
    return;
  }
  int desired = -1;
  if (op == AX_CAS)
  {
    desired = mach_ensure_in_reg(&mctx, &mops[2], used);
    used |= 1u << desired;
  }
  if (layout.locs)
    tcc_free(layout.locs);

  /* The old value: the result's own register when no operand is in it. */
  int rout = -1, old;
  if (!drop_value && dest_mop.kind != MACH_OP_NONE)
    rout = mach_get_dest_reg(&mctx, &dest_mop, used);
  if (rout >= 0 && !(used >> rout & 1))
    old = rout;
  else
    old = mach_alloc_scratch(&mctx, used);
  used |= 1u << old;
  const int status = mach_alloc_scratch(&mctx, used);
  used |= 1u << status;
  int value = val;
  const thumb_shift no_shift = {THUMB_SHIFT_NONE, 0, THUMB_SHIFT_IMMEDIATE};
  if (op == AX_CAS && size < 4 && cas_imm < 0)
  {
    /* The expected value compares with what LDREXB/H zero-extends. */
    value = mach_alloc_scratch(&mctx, used);
    used |= 1u << value;
    if (size == 1)
      ot_check(th_uxtb(value, val, no_shift, ENFORCE_ENCODING_NONE));
    else
      ot_check(th_uxth(value, val, no_shift, ENFORCE_ENCODING_NONE));
  }
  else if (op != AX_CAS && op != AX_XCHG)
  {
    value = mach_alloc_scratch(&mctx, used); /* the new value */
    used |= 1u << value;
  }

  const int acq = v8 && (flags & 1), rel = v8 && (flags & 2);
  if (!v8 && (flags & 2))
    ot_check(th_dmb(0xf));
  /* The loop in one piece: no literal pool inside, and no IT block split. */
  th_literal_pool_reserve_upcoming_bytes(32);
  const int top = ind;
  thumb_atomic_load_ex(size, acq, old, ptr);
  if (op == AX_CAS)
  {
    /*  top:  ldrex   old, [ptr]
     *        cmp     old, expected
     *        itt     eq
     *        strexeq status, desired, [ptr]
     *        cmpeq   status, #1           (the store failed: try again)
     *        beq     top
     *        clrex
     * Strong, so also a valid weak compare-exchange; success is old ==
     * expected, which the frontend tests. */
    if (cas_imm >= 0)
      ot_check(th_cmp_imm(old, (uint32_t)cas_imm, FLAGS_BEHAVIOUR_SET, ENFORCE_ENCODING_NONE));
    else
      ot_check(th_cmp_reg(0, old, value, FLAGS_BEHAVIOUR_SET, no_shift, ENFORCE_ENCODING_NONE));
    ot_check(th_it(0 /* EQ */, 0x4 /* TT */));
    thumb_atomic_store_ex(size, rel, status, desired, ptr);
    ot_check(th_cmp_imm(status, 1, FLAGS_BEHAVIOUR_SET, ENFORCE_ENCODING_NONE));
    const int off = top - (ind + 4);
    if (off < -256)
      tcc_ice("inline atomic loop out of branch range");
    ot_check(th_b_t1(0 /* EQ */, (uint32_t)(off >> 1)));
    /* A mismatch leaves the reservation open.  Close it, as the runtime and
     * LLVM do: a spin lock waits next with WFE, and an open reservation keeps
     * the WFE from sleeping -- on QEMU each contended kernel lock_irqsave
     * spun at full speed instead (+4.5% instructions per launch).  After a
     * successful store it is already closed. */
    ot_check(th_clrex());
  }
  else
  {
    /*  top:  ldrex   old, [ptr]
     *        <op>    new, old, val        (xchg: stores val itself)
     *        strex   status, new, [ptr]
     *        cmp     status, #0
     *        bne     top */
    switch (op)
    {
    case AX_ADD:
      ot_check(th_add_reg(value, old, val, FLAGS_BEHAVIOUR_NOT_IMPORTANT, no_shift, ENFORCE_ENCODING_NONE));
      break;
    case AX_SUB:
      ot_check(th_sub_reg(value, old, val, FLAGS_BEHAVIOUR_NOT_IMPORTANT, no_shift, ENFORCE_ENCODING_NONE));
      break;
    case AX_AND:
    case AX_NAND:
      ot_check(th_and_reg(value, old, val, FLAGS_BEHAVIOUR_NOT_IMPORTANT, no_shift, ENFORCE_ENCODING_NONE));
      if (op == AX_NAND)
        ot_check(th_mvn_reg(value, value, value, FLAGS_BEHAVIOUR_NOT_IMPORTANT, no_shift, ENFORCE_ENCODING_NONE));
      break;
    case AX_OR:
      ot_check(th_orr_reg(value, old, val, FLAGS_BEHAVIOUR_NOT_IMPORTANT, no_shift, ENFORCE_ENCODING_NONE));
      break;
    case AX_XOR:
      ot_check(th_eor_reg(value, old, val, FLAGS_BEHAVIOUR_NOT_IMPORTANT, no_shift, ENFORCE_ENCODING_NONE));
      break;
    }
    thumb_atomic_store_ex(size, rel, status, value, ptr);
    ot_check(th_cmp_imm(status, 0, FLAGS_BEHAVIOUR_SET, ENFORCE_ENCODING_NONE));
    const int off = top - (ind + 4);
    if (off < -256)
      tcc_ice("inline atomic loop out of branch range");
    ot_check(th_b_t1(1 /* NE */, (uint32_t)(off >> 1)));
  }
  if (flags & 4 || (!v8 && (flags & 1)))
    ot_check(th_dmb(0xf));
  if (rout >= 0)
  {
    if (old != rout)
      ot_check_mov_reg(rout, old, flags_safe(), no_shift, ENFORCE_ENCODING_NONE, false);
    mach_writeback_dest(&dest_mop, rout);
  }
  mach_release_all(&mctx);
}

/* An asm statement of system instructions (asm_machine_call_name): a call to
 * __tcc_mc_<insn>_<insn>... emitted as those instructions where the operands
 * already are -- an msr reads its operand's register, an mrs writes the
 * result's -- with no call around them: the register allocator treats the
 * call as writing nothing but its result (tcc_ir_call_clobbers_nothing). */
static void thumb_machine_insn_call(TCCIRState *ir, int call_idx, int call_id, int argc, MachineOperand func_mop,
                                    MachineOperand dest_mop, int drop_value)
{
  const char *const name = get_tok_str(func_mop.u.sym.sym->v, NULL);
  MachineCodegenContext mctx = {0};
  int rin = -1, rout = -1;
  if (argc > 0)
  {
    TCCAbiCallLayout layout;
    memset(&layout, 0, sizeof(layout));
    small_sequence(ThumbIROperandSequence) args_owner = {0};
    small_sequence(ThumbMachineOperandSequence) mops_owner = {0};
    if (argc != 1 || thumb_build_call_layout_from_ir(ir, call_idx, call_id, argc, &layout, &args_owner, &mops_owner) != 1)
      tcc_ice("machine call '%s': bad operand", name);
    rin = mach_ensure_in_reg(&mctx, &ThumbMachineOperandSequence_data(&mops_owner)[0], 0);
    if (layout.locs)
      tcc_free(layout.locs);
  }
  if (!drop_value && dest_mop.kind != MACH_OP_NONE)
    rout = mach_get_dest_reg(&mctx, &dest_mop, rin >= 0 ? 1u << rin : 0);
  const char *p = name + 8;
  while (*p == '_')
  {
    char code[16];
    int n = 0;
    for (p++; *p && *p != '_'; p++)
      if (n < (int)sizeof(code) - 1)
        code[n++] = *p;
    code[n] = 0;
    if (!strcmp(code, "wfe"))
      ot_check(th_wfe(ENFORCE_ENCODING_NONE));
    else if (!strcmp(code, "wfi"))
      ot_check(th_wfi(ENFORCE_ENCODING_NONE));
    else if (!strcmp(code, "sev"))
      ot_check(th_sev(ENFORCE_ENCODING_NONE));
    else if (!strcmp(code, "nop"))
      ot_check(th_nop(ENFORCE_ENCODING_NONE));
    else if (!strcmp(code, "yield"))
      ot_check(th_yield(ENFORCE_ENCODING_NONE));
    else if (!strcmp(code, "isb"))
      ot_check(th_isb(0xf));
    else if (!strcmp(code, "dsb"))
      ot_check(th_dsb(0xf));
    else if (!strcmp(code, "dmb"))
      ot_check(th_dmb(0xf));
    else if (!strncmp(code, "cpsi", 4) && (code[4] == 'd' || code[4] == 'e') && code[5])
      ot_check(th_cps(code[4] == 'd', strchr(code + 5, 'i') != NULL, strchr(code + 5, 'f') != NULL));
    else if (!strncmp(code, "mrs", 3) && n == 5)
    {
      /* Reading a special register has no effect of its own. */
      if (rout >= 0)
        ot_check(th_mrs((uint32_t)rout, (uint32_t)strtoul(code + 3, NULL, 16)));
    }
    else if (!strncmp(code, "msr", 3) && n == 5 && rin >= 0)
      ot_check(th_msr((uint32_t)strtoul(code + 3, NULL, 16), (uint32_t)rin, 2));
    else
      tcc_ice("unknown machine instruction '%s' in '%s'", code, name);
  }
  if (*p)
    tcc_ice("malformed machine call '%s'", name);
  if (rout >= 0)
    mach_writeback_dest(&dest_mop, rout);
  mach_release_all(&mctx);
}

/* A call to __tcc_vfp_ret_ld / __tcc_vfp_ret_st (gen_vfp_ret_transfer): a
 * result returned in s0..s(n-1) moved from or to the buffer whose address is
 * argument 0.  Returns n (argument 1), or 0 for any other call; *load says
 * which direction. */
static int thumb_vfp_ret_transfer(MachineOperand func_mop, const IROperand *args, int argc, int *load)
{
  if (func_mop.kind != MACH_OP_SYMBOL || !func_mop.u.sym.sym || argc != 2)
    return 0;
  const char *nm = get_tok_str(func_mop.u.sym.sym->v, NULL);
  if (strcmp(nm, "__tcc_vfp_ret_ld") && strcmp(nm, "__tcc_vfp_ret_st"))
    return 0;
  const int n = irop_is_immediate(args[1]) ? (int)irop_get_imm32(args[1]) : 0;
  if (n < 1 || n > 8)
    tcc_ice("%s: bad register count", nm);
  *load = nm[14] == 'l';
  return n;
}

/* The registers a store reads were left by the call before it: no
 * floating-point instruction may sit between the two. */
static void thumb_vfp_ret_store_check(TCCIRState *ir, int call_idx)
{
  for (int j = call_idx - 1; j >= 0; j--)
  {
    const IRQuadCompact *q = &ir->compact_instructions[j];
    if (q->op == TCCIR_OP_FUNCCALLVAL || q->op == TCCIR_OP_FUNCCALLVOID)
      return;
    if (q->op == TCCIR_OP_NOP)
      continue;
    IROperand ops[3] = {IROP_NONE, IROP_NONE, IROP_NONE};
    if (irop_config[q->op].has_dest)
      ops[0] = tcc_ir_op_get_dest(ir, q);
    if (irop_config[q->op].has_src1)
      ops[1] = tcc_ir_op_get_src1(ir, q);
    if (irop_config[q->op].has_src2)
      ops[2] = tcc_ir_op_get_src2(ir, q);
    for (int k = 0; k < 3; k++)
      if (!irop_is_none(ops[k]) && !ops[k].is_lval &&
          (ops[k].btype == IROP_BTYPE_FLOAT32 || ops[k].btype == IROP_BTYPE_FLOAT64))
        tcc_ice("VFP result store separated from its call by instruction %d", j);
  }
}

ST_FUNC void tcc_gen_machine_func_call_mop(MachineOperand func_mop, IROperand call_id_op, MachineOperand dest_mop,
                                           int drop_value, TCCIRState *ir, int call_idx)
{
  /* === Validation === */
  if (irop_is_none(call_id_op) || !ir)
    tcc_ice("func_call_op requires call_id+ir");

  const int call_id = TCCIR_DECODE_CALL_ID(call_id_op.u.imm32);
  const int argc_hint = TCCIR_DECODE_CALL_ARGC(call_id_op.u.imm32);

  /* The memory barrier of an inline atomic (parse_atomic): a call everywhere
   * before this point, so every pass treats it as touching all memory, and
   * one DMB here.  On M-profile every DMB option behaves as SY. */
  if (func_mop.kind == MACH_OP_SYMBOL && func_mop.u.sym.sym &&
      !strcmp(get_tok_str(func_mop.u.sym.sym->v, NULL), "__tcc_dmb"))
  {
    ot_check(th_dmb(0xf));
    return;
  }

  if (thumb_is_machine_call(func_mop, "__tcc_mc_"))
  {
    thumb_machine_insn_call(ir, call_idx, call_id, argc_hint, func_mop, dest_mop, drop_value);
    return;
  }
  if (thumb_is_machine_call(func_mop, "__tcc_ax_"))
  {
    thumb_inline_atomic_call(ir, call_idx, call_id, argc_hint, func_mop, dest_mop, drop_value);
    return;
  }

  ThumbGenCallSite *call_site = thumb_get_call_site_for_id(call_id);
  if (!call_site)
    tcc_ice("no call site found for call_id=%d", call_id);

  /* === Build ABI layout === */
  TCCAbiCallLayout layout;
  memset(&layout, 0, sizeof(layout));

  thumb_call_layout_abi_flags(&layout, func_mop.kind == MACH_OP_SYMBOL ? func_mop.u.sym.sym : NULL);

  small_sequence(ThumbIROperandSequence) args_owner = {0};
  small_sequence(ThumbMachineOperandSequence) mops_owner = {0};
  const int argc = thumb_build_call_layout_from_ir(ir, call_idx, call_id, argc_hint, &layout, &args_owner, &mops_owner);
  if (argc < 0)
    tcc_ice("failed to build call layout for call_id=%d", call_id);

  int stack_size = (argc > 0) ? (int)layout.stack_size : 0;

  /* === Setup call context === */
  CallGenContext ctx = {
      .call_site = call_site,
      .layout = &layout,
      .args = ThumbIROperandSequence_data(&args_owner),
      .mops = ThumbMachineOperandSequence_data(&mops_owner),
      .argc = argc,
      .stack_size = stack_size,
  };

  int vfp_load = 0;
  const int vfp_xfer = thumb_vfp_ret_transfer(func_mop, ctx.args, ctx.argc, &vfp_load);
  if (vfp_xfer && !vfp_load)
    thumb_vfp_ret_store_check(ir, call_idx);

  /* Set tail_call_pending if this is a tail-call-only function. */
  if (ir->tail_call_only && !vfp_xfer)
    tail_call_pending = 1;

  /* === Preserve nested call registers (R0-R3, R9) via STR to frame ===
   * Instead of PUSH/POP (which moves SP), store to the pre-reserved
   * nested-call save area in the frame.  SP stays fixed. */
  int arg_regs_in_use = call_site->registers_map & 0x0F;
  /* ...plus the values ra:caller_save keeps in caller-saved registers across
   * this call: stored here, before argument setup, and reloaded below after
   * the return value has been moved out. */
  int arg_regs_save_mask = tail_call_pending ? 0 : (arg_regs_in_use | (int)tcc_ir_caller_save_mask_at(ir, call_idx));

  /* On yasos with no-pic-data-is-text-relative, R9 holds the GOT base and is
   * caller-saved.  Save it alongside the nested-call argument registers so it
   * is restored after the callee returns. */
  if (!tail_call_pending && text_and_data_separation)
    arg_regs_save_mask |= (1 << ARM_R9);

  /* ...but a callee in THIS module shares our GOT base and hands it back
   * intact, so its reload is dead.  R9 is not allocatable under
   * text_and_data_separation, tail calls are disabled outright in this mode
   * (ir/codegen.c), and every call site below reloads R9, so a function here
   * either never touches R9 or returns with its own module's base in it --
   * which for a same-module callee is the value we already hold.  Only the
   * cross-module path breaks that: an import goes through a loader thunk that
   * swaps in the callee's base and tail-jumps, so nothing restores ours.
   *
   * Note this suppresses the RELOAD only, never the mask bit: the mask also
   * assigns save-area slots, and clearing R9 from it would slide R0-R3 down
   * onto slot 0 and overwrite the prologue-stored GOT base for every other
   * call site in the function. */
  int restore_r9 = 1;
  if (arg_regs_save_mask & (1 << ARM_R9))
    restore_r9 = !thumb_callee_in_this_module(&func_mop) && !thumb_callee_noreturn(&func_mop);

  /* Save nested-call registers to pre-reserved frame area via STR.
   * The nested save area is at [SP + ir->call_outgoing_size].
   *
   * In functions with VLA/alloca the runtime SP has moved below the static
   * frame, so [SP + off] would land inside the dynamically allocated memory
   * (the callee then overwrites the saved R9/GOT base with user data).
   * Address the slots FP-relative instead, at the same offsets: the frame
   * pointer is the static SP. */
  int nested_save_sp_offset = ir ? ir->call_outgoing_size : 0;
  int nested_save_count = 0;
  if (arg_regs_save_mask)
  {
    /* R9 takes slot 0 so that its frame offset is the same at every call site
     * in the function.  That is what lets the store be hoisted: the GOT base
     * is function-invariant, so the single store in the prologue dominates
     * every reload below and the per-call-site store is redundant.  It used to
     * be emitted here, and because R9 was slotted after whichever of R0-R3
     * happened to be live the offset moved from call to call, which is what
     * hid the redundancy.  The argument registers, whose live set genuinely
     * does vary per call site, are packed above it.
     *
     * Emitting the store here again would be harmless but wasteful: it was
     * 45,329 instructions (165 KiB) across the compiler's own build. */
    if (arg_regs_save_mask & (1 << ARM_R9))
      nested_save_count = 1;
    for (int r = 0; r < 16; r++)
    {
      if (r == ARM_R9)
        continue;
      if (arg_regs_save_mask & (1 << r))
      {
        if (tcc_state->func_dynamic_sp)
          tcc_gen_machine_store_to_stack_ex(
              r, nested_save_sp_offset + nested_save_count * 4,
              arg_regs_save_mask);
        else
          store_word_to_stack(r, nested_save_sp_offset + nested_save_count * 4);
        nested_save_count++;
      }
    }
  }
  if (nested_save_count > dry_run_state.max_nested_saves)
    dry_run_state.max_nested_saves = nested_save_count;

  /* Stack args go to the pre-reserved outgoing area at [SP+0] -- unless this
   * call passes more than the area holds (ir->call_dyn_extra): then SP drops
   * by the difference for the call alone, the arguments land at the new SP,
   * and every frame-relative access meanwhile adds the drop
   * (call_args_sp_bias).  The nested-call saves above are already done
   * against the steady SP and are restored after the window closes. */
  stack_size = (stack_size + 7) & ~7; /* 8-byte align */
  const int dyn_extra =
      ir->call_dyn_extra && call_id >= 0 && call_id < ir->call_dyn_extra_size ? ir->call_dyn_extra[call_id] : 0;
  const int dyn_push_mark = scratch_push_count;
  if (dyn_extra)
  {
    gadd_sp_ex(-dyn_extra, -1);
    call_args_sp_bias += dyn_extra;
  }

  /* === Save scratch exclusion state === */
  uint32_t saved_scratch_exclude = scratch_global_exclude;

  /* === Indirect call target that is also a register argument ===
   *
   * `v(n, v)`: once the argument moves are done, the target's argument
   * register holds it, so the call branches through that register and the
   * target needs no holding register while the arguments are placed.  The
   * register allocator steers such a target into its argument register; when
   * that hint misses, the move scheduler reads it before overwriting it. */
  int target_arg_reg = -1;
  if (func_mop.kind == MACH_OP_REG && !func_mop.needs_deref && func_mop.u.reg.r0 >= 0 &&
      (func_mop.u.reg.r0 <= 3 || tail_call_pending))
    target_arg_reg = call_target_arg_reg(&ctx, func_mop.u.reg.r0);

  /* === Pre-save indirect call target if it resides in an argument register ===
   *
   * When a function pointer is allocated to R0-R3 by the register allocator,
   * the argument placement phase will overwrite those registers.  Pre-move the
   * pointer to a safe register before argument setup.
   *
   * Phase 5g: operates on MachineOperand func_mop instead of filled IROperand.
   */
  {
    const int is_direct = (func_mop.kind == MACH_OP_SYMBOL || func_mop.kind == MACH_OP_IMM);
    if (!is_direct && func_mop.kind == MACH_OP_REG && !func_mop.needs_deref && func_mop.u.reg.r0 >= 0 &&
        func_mop.u.reg.r0 <= 3 && target_arg_reg < 0)
    {
      /* Find a free register outside R0-R3, R12 (stack-arg scratch), SP, PC. */
      uint32_t exclude = scratch_global_exclude | 0x0Fu | (1u << R_IP) | (1u << R_SP) | (1u << R_PC);
      int safe_reg = PREG_NONE;
      if (ir)
        safe_reg = tcc_ls_find_free_scratch_reg(&ir->ls, ir->codegen_instruction_idx, exclude, ir->leaffunc);

      if (safe_reg == PREG_NONE || safe_reg < 0 || safe_reg >= 16 || safe_reg == R_SP || safe_reg == R_PC)
        tcc_ice("func_call_mop: cannot find safe register "
                "to pre-save indirect call target (R%d)",
                func_mop.u.reg.r0);

      /* Move function pointer from arg reg to safe reg. */
      thumb_shift no_shift = {THUMB_SHIFT_NONE, 0, THUMB_SHIFT_IMMEDIATE};
      ot_check_mov_reg(safe_reg, func_mop.u.reg.r0, flags_safe(), no_shift, ENFORCE_ENCODING_NONE,
                       false);

      /* Rewrite func_mop to point to the safe register. */
      func_mop.kind = MACH_OP_REG;
      func_mop.u.reg.r0 = safe_reg;
      func_mop.u.reg.r1 = -1;
      func_mop.needs_deref = false;

      /* Protect the safe register from scratch allocation during arg setup. */
      scratch_global_exclude |= (1u << safe_reg);
    }
  }

  if (func_mop.kind == MACH_OP_REG)
  {
    if (func_mop.u.reg.r0 >= 0 && func_mop.u.reg.r0 < 16)
      ctx.call_target_regs |= 1u << func_mop.u.reg.r0;
    if (func_mop.u.reg.r1 >= 0 && func_mop.u.reg.r1 < 16)
      ctx.call_target_regs |= 1u << func_mop.u.reg.r1;
  }

  /* === Build register argument moves === */
  ThumbArgMove reg_moves[8];
  int reg_move_count = build_register_arg_moves(&ctx, reg_moves);
  /* An inline-expanded copy never reads its size: no move into R2. */
  if (!tail_call_pending && thumb_inline_copy_mode(ir, func_mop, ctx.args, ctx.argc))
  {
    int w = 0;
    for (int i = 0; i < reg_move_count; i++)
      if (!(reg_moves[i].dst_reg == R2 && reg_moves[i].kind != THUMB_ARG_MOVE_IMM64 &&
            reg_moves[i].kind != THUMB_ARG_MOVE_STRUCT))
        reg_moves[w++] = reg_moves[i];
    reg_move_count = w;
  }
  /* A register transfer reads only its buffer address: no move into R1. */
  if (vfp_xfer)
  {
    int w = 0;
    for (int i = 0; i < reg_move_count; i++)
      if (!(reg_moves[i].dst_reg == R1 && reg_moves[i].kind != THUMB_ARG_MOVE_IMM64 &&
            reg_moves[i].kind != THUMB_ARG_MOVE_STRUCT))
        reg_moves[w++] = reg_moves[i];
    reg_move_count = w;
  }

  /* === Compute arg_move_dst_mask and identity-move protection ===
   *
   * Stack arguments are placed BEFORE register argument moves so that
   * R0-R3 (non-identity move destinations) can serve as scratch registers
   * for stack arg stores, saving 2 bytes per store (16-bit vs 32-bit encoding).
   *
   * arg_move_dst_mask: registers written by explicit (non-identity) reg moves.
   *   These will be overwritten by the moves, so they're safe as scratch.
   * identity_mask: registers where the reg allocator already placed the correct
   *   value (no move entry created).  These MUST be protected from clobbering. */
  {
    uint32_t arg_move_dst_mask = 0;
    /* Registers the pending moves still have to READ.  A register can be both:
     * `mov r1, r0` reads r0 while another move writes r0 (`add r0, sp, #52`),
     * which put r0 in arg_move_dst_mask and so offered it as scratch -- and
     * the stack-arg placement that took it destroyed the value the first move
     * was about to copy.  The parallel-move scheduler below already respects
     * this (see the src_set it builds); placement, which runs first, did not. */
    uint32_t arg_move_src_mask = 0;
    for (int i = 0; i < reg_move_count; i++)
    {
      arg_move_dst_mask |= arg_move_write_set(&reg_moves[i]);
      if (reg_moves[i].kind == THUMB_ARG_MOVE_REG)
        arg_move_src_mask |= (1u << reg_moves[i].src_reg);
    }

    /* Compute all register-arg destination registers from the ABI layout. */
    uint32_t all_reg_arg_dst = 0;
    for (int i = 0; i < ctx.argc; i++)
    {
      const TCCAbiArgLoc *loc = &ctx.layout->locs[i];
      if (loc->kind == TCC_ABI_LOC_REG || loc->kind == TCC_ABI_LOC_REG_STACK)
      {
        int base = ARM_R0 + loc->reg_base;
        for (int w = 0; w < loc->reg_count; w++)
          all_reg_arg_dst |= (1u << (base + w));
      }
    }

    /* Protect identity-move registers (value already in place, no move entry). */
    uint32_t identity_mask = all_reg_arg_dst & ~arg_move_dst_mask;
    scratch_global_exclude |= identity_mask;

    /* ...and every register a pending move reads, on every scratch path, not
     * just the arg_move_dst_mask one: the value sitting there may have been
     * materialised by the argument lowering itself and so have no live
     * interval for the liveness query to find. */
    scratch_global_exclude |= arg_move_src_mask;

    /* The composite VFP arguments load after the stack ones are placed. */
    const uint32_t vfp_src_mask = vfp_composite_src_regs(&ctx);
    scratch_global_exclude |= vfp_src_mask;

    ctx.arg_move_dst_mask = arg_move_dst_mask & ~arg_move_src_mask & ~vfp_src_mask;
  }

  /* Pre-save stack args sourcing from R0-R3 before register shuffle */
  presave_stack_args_from_arg_regs(&ctx);

  /* === Place stack arguments FIRST ===
   * R0-R3 that are non-identity move destinations can be used as scratch
   * via arg_move_dst_mask in find_call_scratch, yielding 16-bit STR
   * encodings instead of 32-bit STR.W with R12. */
  place_stack_arguments(&ctx);
  emit_vfp_arg_moves(&ctx, 0);

  /* === Now block all R0-R3 and emit register argument moves === */
  scratch_global_exclude |= 0x0F;
  thumb_emit_parallel_arg_moves(reg_moves, reg_move_count);
  emit_vfp_arg_moves(&ctx, 1);
  if (target_arg_reg >= 0)
    func_mop.u.reg.r0 = target_arg_reg;

  /* === Tail call: tear down frame before branching === */
  if (tail_call_pending)
  {
    /* For indirect calls, the target may be in a callee-saved register that
     * will be popped.  Move it to R_IP (R12) before frame teardown. */
    if (func_mop.kind == MACH_OP_REG && !func_mop.needs_deref &&
        func_mop.u.reg.r0 >= R4 && func_mop.u.reg.r0 <= R11)
    {
      ot_check_mov_reg(R_IP, func_mop.u.reg.r0, flags_safe(),
                       THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE, false);
      func_mop.u.reg.r0 = R_IP;
    }
    if (epilogue_stack_dealloc > 0)
      gadd_sp_ex(epilogue_stack_dealloc, R_IP);
    /* Only pop true callee-saved registers (R4-R11), and LR when the prologue
     * saved it (__builtin_return_address, or LR taken as a scratch): the
     * callee returns straight to our caller, so LR must hold our return
     * address again and SP must be back where our caller left it.  R0-R3 may
     * be pushed for alignment but now hold call arguments — popping them
     * would clobber the prepared args.  Skip non-callee slots FIRST (they sit
     * at lower addresses after push), then pop callee-saved from correct
     * position. */
    uint32_t callee_pop = pushed_registers & (0x0FF0u | (1u << R_LR));
    uint32_t non_callee = pushed_registers & ~callee_pop & ~(1u << R_LR) & ~(1u << R_PC);
    int non_callee_bytes = __builtin_popcount(non_callee) * 4;
    if (non_callee_bytes > 0)
      gadd_sp_ex(non_callee_bytes, R_IP);
    if (callee_pop)
      ot_check(th_pop(callee_pop));
  }

  /* === Emit call === */
  if (vfp_xfer)
    ot_check(th_vldmstm(vfp_load, 0, R0, 0, (1u << vfp_xfer) - 1u, 0));
  else if (tail_call_pending || !thumb_inline_aligned_copy_call(ir, func_mop, ctx.args, ctx.argc))
    gcall_or_jump_mop(0, func_mop);
  if (dyn_extra)
  {
    /* A scratch PUSHed inside the window would be popped after SP moved back
     * -- from the wrong address.  The window is only opened where the scratch
     * save slots stay in reach (ir/codegen.c, DYN_ARGS_MAX), so this is a
     * compiler bug, not a limit. */
    for (int k = dyn_push_mark; k < scratch_push_count; k++)
      if (scratch_push_type[k] == 1 && !dry_run_state.active)
        tcc_ice("scratch PUSH inside a call's argument window");
    call_args_sp_bias -= dyn_extra;
    gadd_sp_ex(dyn_extra, -1);
  }
  /* Restore scratch register exclusion */
  scratch_global_exclude = saved_scratch_exclude;

  if (tail_call_pending)
  {
    tail_call_pending = 0;
    goto call_cleanup;
  }

  handle_return_value_mop(&dest_mop, drop_value, mach_callee_is_aeabi(&func_mop));

  /* === Cleanup: restore nested-call saved registers via LDR === */
  if (arg_regs_save_mask)
  {
    /* Match the FP-relative addressing used by the save side in functions
     * with VLA/alloca (runtime SP has moved; see the save block above). */
    const int restore_base = tcc_state->func_dynamic_sp ? R_FP : ARM_SP;
    /* Mirror the slot assignment of the save block: R9 is at slot 0 (stored
     * once in the prologue), the argument registers start above it. */
    int restore_idx = (arg_regs_save_mask & (1 << ARM_R9)) ? 1 : 0;
    for (int r = 0; r < 16; r++)
    {
      if (r == ARM_R9 && !restore_r9)
        continue; /* same-module callee: our GOT base is still live in R9 */
      if (arg_regs_save_mask & (1 << r))
      {
        int slot = (r == ARM_R9) ? 0 : restore_idx;
        int off = nested_save_sp_offset + slot * 4;
        int sign = (off < 0);
        int abs_off = sign ? -off : off;
        /* R9 restore in text_and_data_separation mode needs the write guard
         * temporarily lifted — the safety check blocks all R9 writes, but
         * we are legitimately restoring it after a call. */
        if (r == ARM_R9 && text_and_data_separation)
          allow_r9_write = 1;
        if (!load_word_from_base(r, restore_base, abs_off, sign))
        {
          ScratchRegAlloc osc = get_scratch_reg_with_save((1u << r));
          load_immediate(osc.reg, off, NULL, false);
          ot_check(th_ldr_reg(r, restore_base, osc.reg, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
          restore_scratch_reg(&osc);
        }
        if (r == ARM_R9 && text_and_data_separation)
          allow_r9_write = 0;
        if (r != ARM_R9)
          restore_idx++;
      }
    }
  }

  call_site->registers_map &= ~0x0F; /* Clear R0-R3 */

call_cleanup:
  if (layout.locs)
    tcc_free(layout.locs);
}

/* Check if a backward branch to target_ir can use a narrow encoding.
 * For backward branches, the target code address is already known in
 * ir_to_code_mapping (it was emitted earlier in this pass).
 * current_ir_idx is the IR index of the branch instruction itself.
 * Returns 1 if narrow encoding fits, 0 otherwise. */
static int can_narrow_backward_branch(int32_t target_ir, int is_conditional, int current_ir_idx)
{
  TCCIRState *ir = tcc_state->ir;
  if (!ir || !ir->ir_to_code_mapping)
    return 0;
  if (target_ir < 0 || target_ir >= ir->ir_to_code_mapping_size)
    return 0;

  /* Forward branches have uninitialized ir_to_code_mapping[target_ir] (still 0).
   * Only narrow genuinely backward branches where target was already emitted. */
  if (target_ir >= current_ir_idx)
    return 0;

  int target_addr = (int)ir->ir_to_code_mapping[target_ir];
  /* ind is the current code address where the branch will be emitted.
   * offset = target - (source + 4) for Thumb pipeline. */
  int offset = target_addr - ind - 4;

  /* Only backward branches (negative offset) are safe to narrow here */
  if (offset >= 0)
    return 0;

  /* If emitting the narrow branch would first flush a pending literal pool,
   * the branch source moves forward after this range check.  A borderline
   * T1/T2 branch can become out of range by the time backpatching runs, and
   * th_patch_call() cannot widen an already-emitted 16-bit branch in place. */
  if (th_literal_pool_would_flush_for(2))
    return 0;

  return is_conditional ? branch_fits_t1(offset) : branch_fits_t2(offset);
}

/* Check if a FORWARD branch to target_ir can use a narrow encoding.
 *
 * The target address is not known yet in the real pass — it is backpatched
 * later — so the decision comes from the rehearsal pass, which laid the
 * function out with the same registers and frame the real pass uses and
 * differs from it only by emitting every branch wide.  The real pass is
 * therefore never larger, so the real distance between two IR instructions is
 * <= their rehearsal distance, and "fits in range in the rehearsal" implies
 * "fits in range for real" -- except for word-alignment pads, which follow the
 * parity the narrowed branches change; rehearsal_max_growth_between adds
 * them back.
 *
 * The one way that breaks is a literal pool flush landing between the branch
 * and its target in the real pass but not in the rehearsal — it inserts up to
 * ~1 KB.  Since the real pass emits fewer bytes, code_size grows more slowly
 * and a flush can only move LATER, so one sitting before the branch in the
 * rehearsal could drift into the range.  Rather than pin flush points, this
 * refuses to narrow unless no flush is possible at all: the range adds at most
 * dry_dist bytes of code and (entries in range * 4) bytes of pool, and if that
 * upper bound stays below the flush threshold the monotonicity argument holds
 * unconditionally. */
/* Upper bound on how many bytes the real pass can ADD, relative to the
 * rehearsal, to the distance from from_ir to to_ir's address.  The rehearsal
 * emits every branch wide, so without alignment the real pass is never
 * larger -- but a word-alignment pad (IRQuadCompact.align_target) depends on
 * the parity of everything before it, and narrowing a branch flips it: a
 * target the rehearsal found aligned needs the pad for real.  Each aligned
 * instruction in (from_ir, to_ir] can add one pad; to_ir's own pad sits before
 * its recorded address, so it counts.  Found as a CBZ over two such targets
 * that measured 126 bytes in the rehearsal and landed at 128 (Zig's
 * InternPool.getOrPutTrailingString). */
static int rehearsal_max_growth_between(TCCIRState *ir, int from_ir, int to_ir)
{
  if (!align_pad_max)
    return 0;
  int pads = 0;
  for (int k = from_ir + 1; k <= to_ir && k < ir->next_instruction_index; k++)
    if (ir->compact_instructions[k].align_target)
      pads++;
  return pads * align_pad_max;
}

/* Upper bound on how many bytes the real pass can shed, relative to the
 * rehearsal, strictly between two IR instructions.  The rehearsal emits every
 * branch wide and never fuses CBZ, so the only shrink sources are:
 *   - a wide branch narrowed to its 16-bit form            (4 -> 2, saves 2)
 *   - a CMP+B.W pair fused into a single CBZ/CBNZ          (6 -> 2, saves 4)
 *   - a word-alignment pad the rehearsal needed and the real pass does not
 * Only a JUMPIF right after its CMP or TEST_ZERO can be fused, which is the
 * shape the codegen.c peepholes look for (NOPs between are skipped there too);
 * every other branch, a return's jump to the epilogue included, can at most
 * be narrowed.  Charging 4 bytes to every branch refused CBZs whose target
 * sat just past a `return` (Zig's `if (err) return err;`, ~19k of them in
 * the Zig compiler built as C).  The pads come from
 * rehearsal_max_growth_between. */
static int rehearsal_max_shrink_between(TCCIRState *ir, int from_ir, int to_ir)
{
  int shrink = 0;
  int prev_op = TCCIR_OP_NOP;
  for (int k = from_ir; k >= 0 && prev_op == TCCIR_OP_NOP; k--)
    prev_op = ir->compact_instructions[k].op;
  for (int k = from_ir + 1; k < to_ir; k++)
  {
    TccIrOp op = (TccIrOp)ir->compact_instructions[k].op;
    if (op == TCCIR_OP_JUMPIF)
      shrink += (prev_op == TCCIR_OP_CMP || prev_op == TCCIR_OP_TEST_ZERO) ? 4 : 2;
    else if (op == TCCIR_OP_JUMP || op == TCCIR_OP_RETURNVALUE || op == TCCIR_OP_RETURNVOID)
      shrink += 2;
    if (op != TCCIR_OP_NOP)
      prev_op = op;
  }
  /* ... a window the -Os outliner calls instead, and a pad the rehearsal
   * needed can be gone (parity flipped back). */
  return shrink + tcc_gen_machine_outline_shrink_between(from_ir, to_ir) +
         rehearsal_max_growth_between(ir, from_ir, to_ir);
}

/* Does the range between two IR instructions (exclusive of from_ir, inclusive
 * of to_ir) hold an inline asm statement?  The dry passes never assemble the
 * body, so the rehearsal lays it out as zero bytes and no distance measured
 * across it bounds the real one: a CBZ over 144 bytes of asm was fused at a
 * modelled offset of a few bytes.  Narrowing checks refuse such ranges. */
static int rehearsal_range_has_asm(TCCIRState *ir, int from_ir, int to_ir)
{
  if (ir->inline_asm_count == 0)
    return 0;
  for (int k = from_ir + 1; k <= to_ir && k < ir->next_instruction_index; k++)
    if (ir->compact_instructions[k].op == TCCIR_OP_INLINE_ASM)
      return 1;
  return 0;
}

/* Can `CMP rN,#0; B<eq|ne> target` at current_ir_idx be fused into a single
 * 16-bit CBZ/CBNZ?  CBZ is forward-only with a 0..126 byte range and cannot be
 * widened in place once committed (th_patch_call errors), so the check has to
 * be sound in BOTH directions, unlike plain narrowing which only needs an
 * upper bound:
 *
 *   upper — the real offset is at most the rehearsal-derived one, because the
 *           real pass only ever shrinks;
 *   lower — the real offset is at least that minus the maximum shrink the
 *           range can undergo, and it must not go below 0.
 *
 * The fusion itself removes 6 bytes (CMP + wide branch) and puts back 2 at the
 * branch site, which is why the base offset is dry_dist - 8 rather than the
 * dry_dist - 4 that plain narrowing uses. */
ST_FUNC int tcc_gen_machine_cbz_forward_ok(int32_t target_ir, int current_ir_idx)
{
  TCCIRState *ir = tcc_state->ir;
  if (!ir || !ir->codegen_cbz_dry_mapping || !ir->codegen_dry_pool_entries)
    return 0;
  if (current_ir_idx < 0 || target_ir <= current_ir_idx)
    return 0;
  if (target_ir >= ir->ir_to_code_mapping_size || target_ir >= ir->next_instruction_index)
    return 0;

  const uint32_t *dry = ir->codegen_cbz_dry_mapping;
  int dry_dist = (int)dry[target_ir] - (int)dry[current_ir_idx];
  if (dry_dist < 0)
    return 0;

  /* The JUMPIF this fusion consumes is part of the 8 already; count what can
   * shrink after it. */
  int jump_ir = current_ir_idx + 1;
  while (jump_ir < target_ir && ir->compact_instructions[jump_ir].op == TCCIR_OP_NOP)
    jump_ir++;
  if (jump_ir >= target_ir || ir->compact_instructions[jump_ir].op != TCCIR_OP_JUMPIF)
    return 0;
  int max_offset = dry_dist - 8 + rehearsal_max_growth_between(ir, current_ir_idx, target_ir);
  int min_offset = dry_dist - 8 - rehearsal_max_shrink_between(ir, jump_ir, target_ir);
  if (min_offset < 0 || max_offset > 126)
    return 0;
  if (rehearsal_range_has_asm(ir, current_ir_idx, target_ir))
    return 0;

  /* A pool flush anywhere in the range would push the branch out of its 126-byte
   * reach, and unlike a wide branch it cannot be repaired.  Same bound as
   * can_narrow_forward_branch. */
  if (th_literal_pool_would_flush_for(2))
    return 0;
  int entries_in_range =
      (int)ir->codegen_dry_pool_entries[target_ir] - (int)ir->codegen_dry_pool_entries[current_ir_idx];
  if (entries_in_range < 0)
    return 0;
  /* The emitter flushes when th_pool_span_after() reaches the range limit,
   * evaluated continuously while the range's instructions emit.  This
   * one-shot estimate bounds the span at any point in the range: the current
   * span plus the range's code bytes plus the pool bytes its entries can
   * add.  The real pass can still drift past it — per-emit upcoming bytes,
   * dry-vs-real sizing differences.  Fuzz seed float:8061 passed a hairline
   * check and then took a 188-byte in-range flush (COMPILE_FAIL at
   * th_patch_call, offset 204).  Require an extra 64-byte cushion so a
   * hairline pass can't commit an unrepairable CBZ. */
  int pressure = th_pool_span_after(8) + dry_dist + entries_in_range * 4;
  if (pressure >= THUMB_POOL_RANGE_LIMIT - THUMB_POOL_FLUSH_SLACK - 64)
    return 0;

  return 1;
}

/* The most a range of code can grow by literal-pool flushes landing inside
 * it: every entry pending now and every one the range adds (at most 8 bytes
 * each) dumped, and each dump preceded by its B.W over the pool and an
 * alignment halfword.  A dump needs a pending entry, so there are at most
 * one more of them than entries added.  Plus the 64-byte cushion
 * tcc_gen_machine_cbz_forward_ok keeps for dry-vs-real drift.  A 16-bit
 * branch whose offset still fits after all of that is safe wherever the
 * flushes fall -- the "no flush can happen in the range" rule refused almost
 * every branch longer than a pool window (~950 bytes), 7,900 in zig.c. */
static int pool_flush_growth_bound(int entries_in_range)
{
  return thumb_gen_state.pool_bytes + entries_in_range * (8 + 6) + 6 + 64;
}

/* Return jumps pass target_ir == -1: they go to the epilogue, which has no IR
 * index, so can_narrow_forward_branch cannot size them and they were always
 * emitted wide.  The rehearsal recorded where the body ends — which is exactly
 * where the epilogue begins — so the same monotonicity argument applies: the
 * real pass is never larger, so a distance that fits in the rehearsal fits for
 * real.  Same pool-flush bound as the forward case, using the running entry
 * total at the end of the function. */
static int can_narrow_epilogue_branch(int32_t target_ir, int current_ir_idx)
{
  TCCIRState *ir = tcc_state->ir;
  /* A jump to the end label (target == next_instruction_index) lands on the
   * epilogue too. */
  if (target_ir != -1 && !(ir && target_ir == ir->next_instruction_index))
    return 0;
  if (!ir || !ir->codegen_cbz_dry_mapping || !ir->codegen_dry_pool_entries)
    return 0;
  if (current_ir_idx < 0 || current_ir_idx >= ir->next_instruction_index)
    return 0;
  if (ir->codegen_rehearsal_end == 0)
    return 0;

  int dry_dist = (int)ir->codegen_rehearsal_end - (int)ir->codegen_cbz_dry_mapping[current_ir_idx];
  if (dry_dist < 0)
    return 0;
  const int est_offset =
      dry_dist - 4 + rehearsal_max_growth_between(ir, current_ir_idx, ir->next_instruction_index - 1);
  if (!branch_fits_t2(est_offset))
    return 0;
  if (rehearsal_range_has_asm(ir, current_ir_idx, ir->next_instruction_index - 1))
    return 0;

  if (th_literal_pool_would_flush_for(2))
    return 0;

  /* Bound the pool pressure over the whole remaining body: entries added from
   * here to the last instruction is the most that can accumulate before the
   * epilogue. */
  int last = ir->next_instruction_index - 1;
  int entries_in_range = (int)ir->codegen_dry_pool_entries[last] -
                         (int)ir->codegen_dry_pool_entries[current_ir_idx];
  if (entries_in_range < 0)
    return 0;
  int pressure = th_pool_span_after(8) + dry_dist + entries_in_range * 4;
  if (pressure >= THUMB_POOL_RANGE_LIMIT - THUMB_POOL_FLUSH_SLACK &&
      !branch_fits_t2(est_offset + pool_flush_growth_bound(entries_in_range)))
    return 0;

  return 1;
}

static int can_narrow_forward_branch(int32_t target_ir, int is_conditional, int current_ir_idx)
{
  TCCIRState *ir = tcc_state->ir;
  if (!ir || !ir->codegen_cbz_dry_mapping || !ir->codegen_dry_pool_entries)
    return 0;
  if (current_ir_idx < 0 || target_ir <= current_ir_idx)
    return 0;
  if (target_ir >= ir->ir_to_code_mapping_size || target_ir >= ir->next_instruction_index)
    return 0;

  const uint32_t *dry = ir->codegen_cbz_dry_mapping;
  int dry_dist = (int)dry[target_ir] - (int)dry[current_ir_idx];
  if (dry_dist < 0)
    return 0;

  /* dry[current_ir_idx] is the START of the IR instruction; the branch may sit
   * a few bytes into it, which only makes the true offset smaller.  Dropping
   * the 4-byte Thumb pipeline bias keeps the estimate conservative. */
  int est_offset = dry_dist - 4 + rehearsal_max_growth_between(ir, current_ir_idx, target_ir);
  if (!(is_conditional ? branch_fits_t1(est_offset) : branch_fits_t2(est_offset)))
    return 0;
  if (rehearsal_range_has_asm(ir, current_ir_idx, target_ir))
    return 0;

  /* A flush scheduled at this very point would move the branch itself. */
  if (th_literal_pool_would_flush_for(2))
    return 0;

  int entries_in_range =
      (int)ir->codegen_dry_pool_entries[target_ir] - (int)ir->codegen_dry_pool_entries[current_ir_idx];
  if (entries_in_range < 0)
    return 0;
  /* Same bound as tcc_gen_machine_cbz_forward_ok: no flush can trigger in
   * the range while this stays under the flush threshold.  Past it, the
   * branch still narrows if it fits with every flush the range can take. */
  int pressure = th_pool_span_after(8) + dry_dist + entries_in_range * 4;
  if (pressure >= THUMB_POOL_RANGE_LIMIT - THUMB_POOL_FLUSH_SLACK &&
      !(is_conditional ? branch_fits_t1 : branch_fits_t2)(est_offset + pool_flush_growth_bound(entries_in_range)))
    return 0;

  return 1;
}

ST_FUNC int tcc_gen_machine_jump_mop(TccIrOp op, int32_t target_ir, int ir_idx)
{

  if (dry_run_state.active)
  {
    /* Emit 32-bit placeholder for code size tracking */
    ot_check(th_b_t4(0));
    return 4;
  }

  /* Real pass: try narrow encoding, backward first then forward */
  if (can_narrow_backward_branch(target_ir, 0, ir_idx) || can_narrow_forward_branch(target_ir, 0, ir_idx) ||
      can_narrow_epilogue_branch(target_ir, ir_idx))
  {
    ot_check(th_b_t2(0)); /* 16-bit unconditional */
    return 2;
  }
  else
  {
    ot_check(th_b_t4(0)); /* 32-bit unconditional */
    return 4;
  }
}

ST_FUNC int tcc_gen_machine_conditional_jump_mop(int32_t condition, TccIrOp op, int32_t target_ir, int ir_idx)
{
  int cond = mapcc(condition);

  if (dry_run_state.active)
  {
    /* Emit 32-bit placeholder for code size tracking */
    ot_check(th_b_t3(cond, 0));
    return 4;
  }

  /* Real pass: try narrow encoding, backward first then forward */
  if (can_narrow_backward_branch(target_ir, 1, ir_idx) || can_narrow_forward_branch(target_ir, 1, ir_idx))
  {
    ot_check(th_b_t1(cond, 0)); /* 16-bit conditional */
    return 2;
  }
  else
  {
    ot_check(th_b_t3(cond, 0)); /* 32-bit conditional */
    return 4;
  }
}

/* Return the maximum bytes a pending literal pool dump could insert.
 * Used for CBZ/CBNZ distance safety checks. */
ST_FUNC int tcc_gen_machine_pending_pool_size(void)
{
  int count = dry_run_state.active ? dry_run_literal_pool_count : thumb_gen_state.literal_pool_count;
  return count * 4 + (count > 0 ? 2 : 0); /* entries + possible alignment padding */
}

/* Emit CBZ/CBNZ: combined compare-zero + branch in a single 16-bit instruction.
 * rn must be r0-r7, target must be forward within 126 bytes.
 * Returns the instruction size (always 2). */
ST_FUNC int tcc_gen_machine_cbz_jump_mop(int rn, int nonzero, int32_t target_ir, int ir_idx)
{
  ot_check(th_cbz((uint16_t)rn, 0, (uint32_t)nonzero));
  return 2;
}

/* reg = this frame's top: the frame pointer plus the frame size.  That is
 * the static chain a nested function receives: it addresses its parent's
 * captured variables at their IR frame offsets from it (MACH_OP_CHAIN_REL,
 * negative, counted down from the top), and the parent's own saved chain at
 * -4 (resolve_chain_base) -- the frame pointer itself is the frame's bottom. */
static void emit_frame_top(int reg)
{
  if (allocated_stack_size == 0)
  {
    ot_check_mov_reg(reg, R_FP, flags_safe(), THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE, false);
    return;
  }
  if (!ot(th_add_imm(reg, R_FP, allocated_stack_size, flags_safe(), ENFORCE_ENCODING_NONE)))
  {
    load_full_const(reg, PREG_NONE, LFC_SPLIT(allocated_stack_size));
    ot_check(th_add_reg(reg, reg, R_FP, flags_safe(), THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
  }
}

/* Offset of this frame's top above the frame pointer: DWARF's frame base for
 * a frame-pointer function, where variables sit at their IR frame offsets. */
ST_FUNC int tcc_gen_machine_frame_top_offset(void)
{
  return allocated_stack_size;
}

/* Set static chain register: R10 = this frame's top (emit_frame_top). */
ST_FUNC void tcc_gen_machine_set_chain(void)
{
  emit_frame_top(architecture_config.static_chain_reg);
}

/* Reload static chain register from the chain save slot (frame offset -4).
 * Called after function calls in nested functions with has_static_chain,
 * because trampoline calls can clobber R10. */
ST_FUNC void tcc_gen_machine_restore_chain(void)
{
  int chain_reg = architecture_config.static_chain_reg;
  const int off = fp_adjust_local_offset(-4, 0);
  const int sign = off < 0;
  const int abs_off = sign ? -off : off;
  if (!load_word_from_base(chain_reg, R_FP, abs_off, sign))
  {
    ScratchRegAlloc rr_alloc = th_offset_to_reg_ex(abs_off, sign, (1u << chain_reg) | (1u << R_FP));
    int rr = rr_alloc.reg;
    ot_check(th_ldr_reg(chain_reg, R_FP, rr, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
    restore_scratch_reg(&rr_alloc);
  }
}

/* Store this frame's top into chain slot in .data for nested function trampoline.
 * src1 carries the chain slot symbol via SYMREF so we can emit a relocation. */
ST_FUNC void tcc_gen_machine_init_chain_slot(IROperand src1)
{
  /* Extract the chain slot Sym* from the IROperand */
  Sym *chain_sym = irop_get_sym(src1);
  if (!chain_sym)
    tcc_error("internal error: INIT_CHAIN_SLOT without chain slot symbol");

  /* Get a scratch register to hold the chain slot address */
  ScratchRegAlloc scratch = get_scratch_reg_with_save(0);

  /* Load chain slot address into scratch register via literal pool. */
  _lfc_sym = chain_sym;
  load_full_const(scratch.reg, PREG_NONE, 0, 0);

  /* STR top, [scratch, #0] — store this frame's top (the static chain the
   * trampoline hands the nested function, emit_frame_top) into the slot */
  ScratchRegAlloc top = get_scratch_reg_with_save(1u << scratch.reg);
  emit_frame_top(top.reg);
  ot_check_str_imm(top.reg, scratch.reg, 0, 6, ENFORCE_ENCODING_NONE);

  /* Restore scratch registers */
  restore_scratch_reg(&top);
  restore_scratch_reg(&scratch);
}

/* Called at end of each IR instruction to clean up scratch register state.
 * - Restores any pushed scratch registers (POP in reverse push order)
 * - Resets global exclusion mask for next instruction */
ST_FUNC void tcc_gen_machine_end_instruction(void)
{
  restore_all_pushed_scratch_regs();
}

/* Bytes of outgoing-argument area that have to stay below the live stack top.
 *
 * Stack arguments are written at [SP, SP+call_outgoing_size) right before the
 * BL, because that is where AAPCS says the callee looks for them, so SP cannot
 * also be the block alloca just handed back -- the next call with a stack
 * argument writes straight over it.  GNU make's pattern_search does exactly
 * that: `int_file = alloca (sizeof (struct file))` followed by a five-argument
 * recursive call, and the fifth argument landed on int_file->name, so the
 * callee dereferenced a NULL name.
 *
 * SP is therefore kept this many bytes below the *logical* stack top:
 * VLA_ALLOC carves its block off that top and then drops SP past a fresh
 * outgoing area, and VLA_SP_SAVE / VLA_SP_RESTORE convert between the two.
 * The conversion is symmetric, so the SAVE/RESTORE pairs that bracket a VLA
 * scope still round-trip, and the SAVE that captures an alloca's result now
 * yields the block rather than the argument area beneath it.
 *
 * The nested-call R9/argument save slots need no equivalent: in a dynamic-SP
 * function they are addressed FP-relative (the frame pointer is the static
 * SP; see the call-site save block). */
static int vla_outgoing_reserve(void)
{
  TCCIRState *ir = tcc_state->ir;
  int k = (ir && ir->call_outgoing_size > 0) ? ir->call_outgoing_size : 0;
  return (k + 7) & ~7;
}

/* Whether the value in register `reg` is last used by the current
 * instruction, so the instruction may overwrite it.  Without liveness (no IR)
 * nothing is known to outlive it. */
static int reg_value_dies_here(int reg)
{
  TCCIRState *ir = tcc_state->ir;
  if (!ir)
    return 1;
  int holder = tcc_ls_find_int_reg_holder(&ir->ls, reg, ir->codegen_instruction_idx);
  return holder >= 0 && (int)ir->ls.intervals[holder].end <= ir->codegen_instruction_idx;
}

/* tcc_gen_machine_vla_mop: MachineOperand-based entry point for VLA operations.
 *
 *   VLA_ALLOC:      src1=size(bytes), src2=alignment(IMM bytes), dest unused
 *   VLA_SP_SAVE:    dest=save slot, src1/src2 unused
 *   VLA_SP_RESTORE: src1=save slot, dest/src2 unused
 *
 * Gate: !ir->has_static_chain (VLA ops are always 32-bit pointer/int sized).
 */
ST_FUNC void tcc_gen_machine_vla_mop(MachineOperand dest, MachineOperand src1, MachineOperand src2, TccIrOp op)
{
  MachineCodegenContext ctx = {0};
  switch (op)
  {
  case TCCIR_OP_VLA_ALLOC:
  {
    /* src1=size (may be register or spilled); src2=alignment (IMM or NONE). */
    int align = (src2.kind == MACH_OP_IMM) ? (int)src2.u.imm.val : 8;
    if (align < 8)
      align = 8;
    if (align & (align - 1))
      tcc_error("alignment is not a power of 2: %i", align);

    /* The new top is built in a working register: the size's own register
     * when this is its last use (a VLA's byte count), else a scratch.  A size
     * that stays live -- alloca(n) with n used afterwards -- had its register
     * overwritten by the new SP.  The scratch must not be one saved on the
     * stack: its restore would read from the moved SP.  The dry run records
     * such a save and the fixup frees a register for the real run. */
    int size_reg = mach_ensure_in_reg(&ctx, &src1, 0);
    int r = size_reg;
    if (src1.kind == MACH_OP_REG && !src1.needs_deref && !reg_value_dies_here(size_reg))
    {
      r = mach_alloc_scratch(&ctx, 1u << (uint32_t)size_reg);
      if (ctx.scratches[ctx.n_scratch - 1].would_save && !dry_run_state.active)
        tcc_ice("alloca/VLA size stays live and no register is free "
                  "to compute the new stack top");
    }
    if (r == R_SP)
      tcc_ice("VLA alloc picked SP as temp");

    const int vla_reserve = vla_outgoing_reserve();

    /* r = SP - size */
    ot_check(th_sub_reg(r, R_SP, size_reg, flags_safe(), THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));

    /* ... and back up to the logical top, so the block is carved above the
     * outgoing-argument area rather than on top of it. */
    if (vla_reserve)
    {
      thumb_opcode add_res = th_add_imm(r, r, (uint32_t)vla_reserve, flags_safe(), ENFORCE_ENCODING_NONE);
      if (is_valid_opcode(add_res))
      {
        ot(add_res);
      }
      else
      {
        int res_reg = mach_alloc_scratch(&ctx, 1u << (uint32_t)r);
        if (!ot(th_generic_mov_imm(res_reg, vla_reserve)))
          load_full_const(res_reg, PREG_NONE, LFC_SPLIT(vla_reserve));
        ot_check(th_add_reg(r, r, res_reg, flags_safe(), THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
      }
    }

    if (align > 1)
    {
      /* Align down: r &= ~(align-1).  Try immediate BIC first. */
      if (!ot(th_bic_imm(r, r, (uint32_t)(align - 1), flags_safe(), ENFORCE_ENCODING_NONE)))
      {
        /* Fallback: materialize mask in a scratch register. */
        int mask_reg = mach_alloc_scratch(&ctx, 1u << (uint32_t)r);
        if (!ot(th_generic_mov_imm(mask_reg, align - 1)))
          load_full_const(mask_reg, PREG_NONE, LFC_SPLIT(align - 1));
        ot_check(th_bic_reg(r, r, mask_reg, flags_safe(), THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
      }
    }

    ot_check_mov_reg(R_SP, r, flags_safe(), THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE, false);
    /* r is the new logical top; SP sits a fresh outgoing area below it. */
    gadd_sp(-vla_reserve);
    break;
  }
  case TCCIR_OP_VLA_SP_SAVE:
  {
    /* Fast path: when dest is a register-allocated vreg, copy SP directly into
     * its register — saves the scratch-mov + writeback-mov pair that the
     * generic path would emit.  Triggered by the alloca-load-fwd IR pass
     * which rewrites a `VLA_SP_SAVE slot; LOAD vreg <- slot` pair into a
     * single `VLA_SP_SAVE vreg`. */
    const int save_reserve = vla_outgoing_reserve();
    if (dest.kind == MACH_OP_REG && !dest.needs_deref &&
        dest.u.reg.r0 != (int)PREG_REG_NONE)
    {
      ot_check_mov_reg((uint32_t)dest.u.reg.r0, R_SP, flags_safe(),
                       THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE, false);
      if (save_reserve)
        ot_check(th_add_imm((uint32_t)dest.u.reg.r0, (uint32_t)dest.u.reg.r0, (uint32_t)save_reserve,
                            flags_safe(), ENFORCE_ENCODING_NONE));
      break;
    }
    /* Save the logical stack top to the destination save slot via a scratch. */
    ScratchRegAlloc sp_scratch = get_scratch_reg_with_save(0);
    ot_check_mov_reg(sp_scratch.reg, R_SP, flags_safe(), THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE,
                     false);
    if (save_reserve)
      ot_check(th_add_imm((uint32_t)sp_scratch.reg, (uint32_t)sp_scratch.reg, (uint32_t)save_reserve,
                          flags_safe(), ENFORCE_ENCODING_NONE));
    mach_writeback_dest(&dest, sp_scratch.reg);
    restore_scratch_reg(&sp_scratch);
    break;
  }
  case TCCIR_OP_VLA_SP_RESTORE:
  {
    /* Load the saved logical top from src1, then put SP back below it.  The
     * SUB goes through SP rather than the loaded register: src1 may be a live
     * vreg (alloca_load_fwd hands one over) that this must not clobber. */
    int saved_sp = mach_ensure_in_reg(&ctx, &src1, 0);
    ot_check_mov_reg(R_SP, saved_sp, flags_safe(), THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE, false);
    gadd_sp(-vla_outgoing_reserve());
    break;
  }
  default:
    tcc_ice("tcc_gen_machine_vla_mop unsupported op %d", op);
  }
  mach_release_all(&ctx);
}

/* Block copy from const data section to stack using LDM/STM.
 * dest = STACKOFF (destination stack offset, is_local=1)
 * src  = SYMREF (anonymous symbol in rodata)
 * size = number of bytes to copy (must be multiple of 4)
 *
 * Generated code for 20 bytes (5 words):
 *   LDR   r_src, [PC, #lit_pool]    ; load rodata address
 *   ADD   r_dst, FP/SP, #stack_off  ; compute stack dest
 *   LDMIA r_src!, {r0, r1, r2, r3}  ; load 4 words from rodata
 *   STMIA r_dst!, {r0, r1, r2, r3}  ; store 4 words to stack
 *   LDR   r0, [r_src]               ; load remaining word
 *   STR   r0, [r_dst]               ; store remaining word
 */
/* tcc_gen_machine_select_mop: Conditional select using ITE block.
 * Emits: ITE <cond>; MOV dest, then_val; MOV dest, else_val
 *
 * For simple register/immediate operands, this is 3 instructions (ITE + 2 MOVs)
 * instead of 5+ (B.cond + MOV + B + MOV + ...) with branching.
 */
/* Check if a MachineOperand can be materialized in exactly one instruction.
 * Returns 1 for: IMM (any value), REG (no deref), SYMBOL (no deref), SPILL (no deref).
 * Returns 0 for: multi-instruction sequences (deref, chain_rel, etc). */
static int select_can_inline(const MachineOperand *op)
{
  switch (op->kind)
  {
  case MACH_OP_IMM:
    return !op->needs_deref; /* MOV/MOVW/MVN or literal pool LDR; a deref loads too */
  case MACH_OP_REG:
    return !op->needs_deref; /* MOV reg is 1 instr; deref needs LDR too */
  case MACH_OP_SYMBOL:
    /* A symbol address is a single literal-pool LDR only in the plain,
     * non-PIC, non-separated layout.  Under PIC/PIE or text+data separation it
     * expands to a multi-instruction GOT/GOTOFF sequence (ldr GOT-slot; add r9;
     * ldr; ...).  Emitting that "inline" inside an IT block predicates only the
     * FIRST instruction and lets the remaining ones run unconditionally, which
     * clobbers the select result with the else-operand's address.  Force
     * pre-materialization into a scratch register in those modes. */
    return !op->needs_deref && !pic && !text_and_data_separation;
  case MACH_OP_SPILL:
    return !op->needs_deref; /* LDR from stack is 1 instr; deref (VT_LLOCAL) needs 2 */
  case MACH_OP_FRAME_ADDR:
    return 1; /* ADD reg, FP, #off is 1 instr */
  default:
    return 0;
  }
}

/* Emit a single-instruction materialization of 'op' into 'reg'.
 * Caller must ensure select_can_inline(op) returned 1. */
/* Every caller emits this as an instruction of an IT block, where the 16-bit
 * MOV #imm8 (MOVS outside one) sets no flags: a low register takes it, not the
 * flag-preserving 4-byte MOV.W -- `ite eq; moveq r2, #1; movne r2, #0`. */
static void select_emit_inline(MachineCodegenContext *ctx, const MachineOperand *op, int reg)
{
  switch (op->kind)
  {
  case MACH_OP_IMM:
  {
    if (reg < 8 && op->u.imm.val >= 0 && op->u.imm.val <= 255)
    {
      ot_check(th_mov_imm((uint32_t)reg, (uint32_t)op->u.imm.val, FLAGS_BEHAVIOUR_NOT_IMPORTANT,
                          ENFORCE_ENCODING_16BIT));
      break;
    }
    thumb_opcode imm_op = th_generic_mov_imm((uint32_t)reg, (int)op->u.imm.val);
    if (imm_op.size != 0)
      ot(imm_op);
    else
      load_full_const(reg, PREG_NONE, LFC_SPLIT(op->u.imm.val));
    break;
  }
  case MACH_OP_REG:
    ot_check_mov_reg(reg, op->u.reg.r0, flags_safe(), THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE,
                     true);
    break;
  case MACH_OP_SYMBOL:
  {
    Sym *raw_sym = op->u.sym.sym;
    Sym *sym = raw_sym ? validate_sym_for_reloc(raw_sym) : NULL;
    tcc_machine_load_constant(reg, PREG_REG_NONE, op->u.sym.addend, 0, sym);
    break;
  }
  case MACH_OP_SPILL:
    mach_load_slot(reg, op);
    break;
  case MACH_OP_FRAME_ADDR:
    tcc_machine_addr_of_stack_slot(reg, op->u.frame.offset, 0);
    break;
  default:
    tcc_ice("select_emit_inline: unhandled kind %d", (int)op->kind);
    break;
  }
}

ST_FUNC void tcc_gen_machine_select_mop(MachineOperand then_val, MachineOperand else_val, MachineOperand dest,
                                        int cond_code)
{
  MachineCodegenContext mctx = {0};

  int cond = mapcc(cond_code);

  /* Get destination register */
  int dest_reg = mach_get_dest_reg(&mctx, &dest, 0);
  uint32_t excl = (1u << (uint32_t)dest_reg);

  /* Determine if each operand can be materialized in exactly one instruction.
   * If so, we can emit it directly inside the ITE block into dest_reg,
   * saving scratch registers and pre-materialization instructions.
   *
   * Emitting inside the IT block is preferred because:
   * - It avoids flag clobber (MOVS before ITE would destroy CMP flags)
   * - It saves scratch registers (no pre-materialization needed)
   * - It produces smaller code */
  int then_inline = select_can_inline(&then_val);
  int else_inline = select_can_inline(&else_val);

  int then_reg = -1, else_reg = -1;

  /* Pre-materialize operands that need multi-instruction sequences.
   * These are loaded into scratch registers BEFORE the ITE block. */
  if (!then_inline)
  {
    then_reg = mach_ensure_in_reg(&mctx, &then_val, excl);
    excl |= (1u << (uint32_t)then_reg);
  }
  if (!else_inline)
  {
    else_reg = mach_ensure_in_reg(&mctx, &else_val, excl);
    excl |= (1u << (uint32_t)else_reg);
  }

  /* Identity-then shortcut: if the then-value is already in dest_reg, the
   * predicated mov would be `movXX dest, dest` — a real instruction inside an
   * IT block (the usual elision in ot_check_mov_reg is suppressed by in_it).
   * Emit `IT <inv_cond>` + the else mov instead.  Saves one instruction. */
  int then_is_identity = 0;
  if (then_inline && then_val.kind == MACH_OP_REG && !then_val.needs_deref &&
      (int)then_val.u.reg.r0 == dest_reg)
    then_is_identity = 1;
  else if (!then_inline && then_reg == dest_reg)
    then_is_identity = 1;

  if (then_is_identity)
  {
    int inv_cond = cond ^ 1;
    th_literal_pool_reserve_upcoming_bytes(8); /* IT(2) + instr(2-4) */
    ot_check(th_it((uint16_t)inv_cond, 0x8u)); /* IT <inv_cond>, single insn */
    if (else_inline)
      select_emit_inline(&mctx, &else_val, dest_reg);
    else
      ot_check_mov_reg(dest_reg, else_reg, flags_safe(), THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE, true);
    mach_writeback_dest(&dest, dest_reg);
    mach_release_all(&mctx);
    return;
  }

  /* Identity-else shortcut: mirror of the above.  If the else-value is already
   * in dest_reg, emit `IT <cond>` + the then mov only, dropping the
   * `movXX dest, dest` for the else arm.  Common for `cond ? f(x) : x` shapes
   * (e.g. abs `x<0?-x:x`) where the else arm is the unmodified input. */
  int else_is_identity = 0;
  if (else_inline && else_val.kind == MACH_OP_REG && !else_val.needs_deref &&
      (int)else_val.u.reg.r0 == dest_reg)
    else_is_identity = 1;
  else if (!else_inline && else_reg == dest_reg)
    else_is_identity = 1;

  if (else_is_identity)
  {
    th_literal_pool_reserve_upcoming_bytes(8); /* IT(2) + instr(2-4) */
    ot_check(th_it((uint16_t)cond, 0x8u)); /* IT <cond>, single insn */
    if (then_inline)
      select_emit_inline(&mctx, &then_val, dest_reg);
    else
      ot_check_mov_reg(dest_reg, then_reg, flags_safe(), THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE, true);
    mach_writeback_dest(&dest, dest_reg);
    mach_release_all(&mctx);
    return;
  }

  /* ITE mask: the second instruction uses the opposite condition.
   * mask encoding: bit3 = E_flag for 2nd instr, bit2 = end marker.
   * E_flag = opposite of cond[0], so: mask = ((cond[0]^1) << 3) | (1 << 2) */
  uint32_t ite_mask = (uint32_t)(((cond & 1) ^ 1) << 3) | 0x4u;

  /* Reserve literal pool space to prevent pool dumps inside the IT block */
  th_literal_pool_reserve_upcoming_bytes(10); /* ITE(2) + instr(2-4) + instr(2-4) */

  ot_check(th_it((uint16_t)cond, (uint16_t)ite_mask));

  /* Emit the Then instruction inside IT block */
  if (then_inline)
    select_emit_inline(&mctx, &then_val, dest_reg);
  else
    ot_check_mov_reg(dest_reg, then_reg, flags_safe(), THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE,
                     true);

  /* Emit the Else instruction inside IT block */
  if (else_inline)
    select_emit_inline(&mctx, &else_val, dest_reg);
  else
    ot_check_mov_reg(dest_reg, else_reg, flags_safe(), THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE,
                     true);

  mach_writeback_dest(&dest, dest_reg);
  mach_release_all(&mctx);
}

/* Predicated-compute fusion: when a single-use ALU op feeds an else-identity
 * SELECT (else == SELECT dest) whose condition flags are still live from a
 * preceding CMP, the op can be emitted predicated inside an IT block directly
 * into the SELECT dest — `cmp; it <cond>; rsb.w dest, src, #0` — instead of
 * `rsb Tc,src,#0; it <cond>; mov dest,Tc`.  gcc-parity abs `x<0?-x:x`.
 *
 * Only a reverse-subtract negate (`dest = 0 - src2`) is folded: it lowers to a
 * single 32-bit rsb.w, which inside the IT is conditional and, emitted
 * flags-safe, leaves NZCV untouched.  can_predicate answers before emission so
 * the caller keeps the two-pass scratch accounting consistent. */
ST_FUNC int tcc_gen_machine_can_predicate_alu(MachineOperand src1, MachineOperand src2,
                                              MachineOperand dest, TccIrOp op)
{
  if (op != TCCIR_OP_SUB)
    return 0;
  if (!(src1.kind == MACH_OP_IMM && src1.u.imm.val == 0))
    return 0;
  if (dest.is_64bit || src2.is_64bit || dest.needs_deref)
    return 0;
  return 1;
}

ST_FUNC void tcc_gen_machine_predicated_alu_mop(MachineOperand src1, MachineOperand src2,
                                                MachineOperand dest, TccIrOp op, int cond_code)
{
  (void)src1;
  (void)op;
  MachineCodegenContext mctx = {0};
  int cond = mapcc(cond_code);
  int dest_reg = mach_get_dest_reg(&mctx, &dest, 0);
  uint32_t excl = (1u << (uint32_t)dest_reg);
  int src_reg = mach_ensure_in_reg(&mctx, &src2, excl);

  th_literal_pool_reserve_upcoming_bytes(8); /* IT(2) + rsb.w(4) */
  ot_check(th_it((uint16_t)cond, 0x8u));      /* IT <cond>, single insn */
  ot_check(th_rsb_imm((uint32_t)dest_reg, (uint32_t)src_reg, 0, flags_safe(), ENFORCE_ENCODING_NONE));

  mach_writeback_dest(&dest, dest_reg);
  mach_release_all(&mctx);
}

ST_FUNC void tcc_gen_machine_block_copy_mop(TCCIRState *ir, IROperand dest, IROperand src, int size)
{
  if (size <= 0)
    tcc_ice("block_copy size must be positive, got %d", size);

  /* Get the source symbol from the SYMREF operand */
  IRPoolSymref *symref = irop_get_symref_ex(ir, src);
  if (!symref || !symref->sym)
    tcc_ice("block_copy source is not a valid symbol reference");
  Sym *sym = validate_sym_for_reloc(symref->sym);

  /* Get the destination stack offset */
  int frame_offset = (int)irop_get_imm64_ex(ir, dest);

  /* Large copies run in r0-r3/IP/LR, which the allocator keeps free across
   * them as across a memcpy call (ra_build_call_prefix), instead of in saved
   * scratch registers.  Compute dest address into r0 BEFORE pushing lr, since
   * the address is sp-relative and pushing changes sp.  The copy clobbers lr,
   * so it is saved/restored for leaf functions whose prologue didn't. */
  if (size >= TCCIR_BLOCK_COPY_MEMCPY_MIN_BYTES)
  {
    tcc_machine_addr_of_stack_slot(R0, frame_offset, 0 /* not param */);
    tcc_machine_load_constant(R1, PREG_REG_NONE, symref->addend, 0, sym);
    int need_lr_save = ir->leaffunc;
    if (need_lr_save)
      ot_check(th_push(1u << ARM_LR));
    if (!(frame_offset & 3) && !(symref->addend & 3))
      thumb_emit_word_copy_r0_r1(size);
    else
    {
      /* LDM/STM fault on an unaligned base: a byte buffer's slot, say. */
      tcc_machine_load_constant(R2, PREG_REG_NONE, size, 0, NULL);
      Sym *memcpy_sym = external_global_sym(tok_alloc_const("memcpy"), &func_old_type);
      MachineOperand func_mop = {0};
      func_mop.kind = MACH_OP_SYMBOL;
      func_mop.u.sym.sym = memcpy_sym;
      if (text_and_data_separation)
        ot_check(th_push((uint16_t)((1 << R9) | (1 << R12))));
      gcall_or_jump_mop(0, func_mop);
      if (text_and_data_separation)
        ot_check(th_pop((uint16_t)((1 << R9) | (1 << R12))));
    }
    if (need_lr_save)
      ot_check(th_pop(1u << ARM_LR));
    return;
  }

  int nwords = size / 4;

  /* Allocate pointer registers first and compute addresses BEFORE allocating
   * data registers.  Data register saves may use PUSH which modifies SP,
   * so all SP-relative address computation must happen before that. */
  ScratchRegAlloc src_scratch = get_scratch_reg_with_save(0);
  int r_src = src_scratch.reg;
  ScratchRegAlloc dst_scratch = get_scratch_reg_with_save(1u << (uint32_t)r_src);
  int r_dst = dst_scratch.reg;

  /* Load source address (rodata symbol) into r_src */
  tcc_machine_load_constant(r_src, PREG_REG_NONE, symref->addend, 0, sym);

  /* Compute destination stack address into r_dst BEFORE any data reg saves
   * that might change SP via PUSH */
  tcc_machine_addr_of_stack_slot(r_dst, frame_offset, 0 /* not param */);

  /* Now allocate data registers for LDM/STM.  Even if these saves use PUSH
   * and modify SP, we've already captured the destination address in r_dst. */
  int max_data = nwords < 4 ? nwords : 4;
  if (max_data < 1)
    max_data = 1;

  ScratchRegAlloc data_scratches[4];
  int data_regs[4];
  int ndata = 0;
  uint32_t exclude = (1u << (uint32_t)r_src) | (1u << (uint32_t)r_dst);
  for (int k = 0; k < max_data; k++)
  {
    data_scratches[k] = get_scratch_reg_with_save(exclude);
    data_regs[k] = data_scratches[k].reg;
    exclude |= (1u << (uint32_t)data_regs[k]);
    ndata++;
  }

  int remaining_words = nwords;

  /* Process in chunks of ndata words using LDM/STM with writeback */
  while (remaining_words >= ndata && ndata >= 2)
  {
    uint32_t regset = 0;
    for (int j = 0; j < ndata; j++)
      regset |= (1u << (uint32_t)data_regs[j]);

    ot_check(th_ldm(r_src, regset, 1 /* writeback */, ENFORCE_ENCODING_NONE));
    ot_check(th_stm(r_dst, regset, 1 /* writeback */, ENFORCE_ENCODING_NONE));
    remaining_words -= ndata;
  }

  /* Handle remaining words individually */
  int dr = data_regs[0]; /* first data register */
  int single_words = remaining_words;
  while (remaining_words > 0)
  {
    ot_check_ldr_imm(dr, r_src, 0, 6, ENFORCE_ENCODING_NONE);
    ot_check_str_imm(dr, r_dst, 0, 6, ENFORCE_ENCODING_NONE);
    if (remaining_words > 1)
    {
      if (!ot(th_add_imm(r_src, r_src, 4, flags_safe(), ENFORCE_ENCODING_NONE)))
        tcc_ice("block_copy cannot advance source pointer");
      if (!ot(th_add_imm(r_dst, r_dst, 4, flags_safe(), ENFORCE_ENCODING_NONE)))
        tcc_ice("block_copy cannot advance dest pointer");
    }
    remaining_words--;
  }

  /* Copy the 1..3 trailing bytes of a size that is not a whole number of words.
   * Writing them as bytes matters: a strcpy fold must not touch dst past the
   * terminating NUL, so the copy may not be rounded up to a word.  The LDM/STM
   * chunks advanced the pointers by writeback; the single-word loop above stops
   * ON its last word rather than past it, so the tail sits 4 bytes further on
   * whenever that loop ran at all. */
  int tail = size & 3;
  if (tail)
  {
    int tail_off = single_words > 0 ? 4 : 0;
    for (int b = 0; b < tail; b++)
    {
      ot_check(th_ldrb_imm(dr, r_src, tail_off + b, 6, ENFORCE_ENCODING_NONE));
      ot_check(th_strb_imm(dr, r_dst, tail_off + b, 6, ENFORCE_ENCODING_NONE));
    }
  }

  /* Restore all scratch registers in reverse order: data regs first, then ptrs */
  for (int k = ndata - 1; k >= 0; k--)
    restore_scratch_reg(&data_scratches[k]);
  restore_scratch_reg(&dst_scratch);
  restore_scratch_reg(&src_scratch);
}

/* Size of copying `nwords` words base+src -> base+dst in chunks through data
 * registers regs[0..k) (the LDM's base the highest low one) with STM base rb;
 * emits the code when `emit`.  -1 when some chunk would hold a lone word or an
 * address does not encode. */
static int spill_block_copy_plan(int base, int src, int dst, int nwords, const int *regs, int k, int rb, int emit)
{
  int all_low = rb <= R7;
  for (int i = 0; i < k; i++)
    all_low &= regs[i] <= R7;
  /* Words spread evenly over the chunks, so none is left with a lone word. */
  const int nchunks = (nwords + k - 1) / k;
  int size = 0, w = 0;
  for (int chunk = 0; chunk < nchunks; chunk++)
  {
    const int n = (nwords - w) / (nchunks - chunk);
    if (n < 2)
      return -1;
    uint32_t l = 0;
    int la = -1;
    for (int i = 0; i < n; i++)
    {
      l |= 1u << (uint32_t)regs[i];
      if (regs[i] <= R7 && regs[i] > la)
        la = regs[i];
    }
    if (la < 0)
      la = regs[0];
    const int so = src + 4 * w;
    thumb_opcode a = so < 0 ? th_sub_imm(la, base, -so, flags_safe(), ENFORCE_ENCODING_NONE)
                            : th_add_imm(la, base, so, flags_safe(), ENFORCE_ENCODING_NONE);
    thumb_opcode b = {0};
    if (chunk == 0)
      b = dst < 0 ? th_sub_imm(rb, base, -dst, flags_safe(), ENFORCE_ENCODING_NONE)
                  : th_add_imm(rb, base, dst, flags_safe(), ENFORCE_ENCODING_NONE);
    if (!a.size || (chunk == 0 && !b.size))
      return -1;
    const thumb_opcode ldm = all_low ? (thumb_opcode){.size = 2, .opcode = 0xC800u | ((uint32_t)la << 8) | l}
                                     : (thumb_opcode){.size = 4, .opcode = ((0xE890u | (uint32_t)la) << 16) | l};
    const thumb_opcode stm = all_low ? (thumb_opcode){.size = 2, .opcode = 0xC000u | ((uint32_t)rb << 8) | l}
                                     : (thumb_opcode){.size = 4, .opcode = ((0xE8A0u | (uint32_t)rb) << 16) | l};
    size += a.size + ldm.size + b.size + stm.size;
    if (emit)
    {
      ot_check(a);
      ot_check(ldm);
      if (chunk == 0)
        ot_check(b);
      ot_check(stm);
    }
    w += n;
  }
  return size;
}

/* Copy `nwords` words between two frame slots with registers that are free at
 * this instruction (plus `also_free`), never pushing: chunks of
 *   add rA, sp, #src; ldm rA, {..}  (rA one of the loaded registers)
 *   add rB, sp, #dst; stmia rB!, {..}   (rB outside the list; first chunk)
 * Registers load and store in ascending order, so any set keeps the words in
 * place.  Of the all-low (16-bit) and the widest plan, the shorter is taken;
 * returns 0, having emitted nothing, when neither beats one LDR/STR pair per
 * word. */
ST_FUNC int tcc_gen_machine_spill_block_copy_free(int32_t src_spill_off, int32_t dst_spill_off, int nwords,
                                                  uint32_t also_free)
{
  TCCIRState *ir = tcc_state->ir;
  if (!ir || nwords < 3)
    return 0;
  const int base = tcc_state->need_frame_pointer ? R_FP : R_SP;
  const int src = fp_adjust_local_offset(src_spill_off, 0), dst = fp_adjust_local_offset(dst_spill_off, 0);
  if ((src & 3) || (dst & 3))
    return 0;
  uint32_t excl = (1u << (uint32_t)base) | (1u << R_SP) | (1u << R_PC) | scratch_global_exclude;
  uint32_t freeset = also_free & ~excl & 0x100Fu; /* r0-r3, ip */
  for (;;)
  {
    int r = tcc_ls_find_free_scratch_reg(&ir->ls, ir->codegen_instruction_idx, excl | freeset, ir->leaffunc);
    if (r < 0 || r == PREG_NONE || r >= 16 || ((excl | freeset) & (1u << (uint32_t)r)))
      break;
    freeset |= 1u << (uint32_t)r;
  }
  if (__builtin_popcount(freeset) < 3)
    return 0;
  /* The STM base: the lowest free register; data registers low first. */
  const int rb = __builtin_ctz(freeset);
  int all[6], nall = 0, low[6], nlow = 0;
  for (int r = 0; r < 16 && nall < 5; r++)
    if (r != rb && (freeset & (1u << (uint32_t)r)))
    {
      all[nall++] = r;
      if (r <= R7)
        low[nlow++] = r;
    }

  int plain = 0;
  for (int w = 0; w < nwords; w++)
  {
    const int so = src + 4 * w, d = dst + 4 * w;
    thumb_opcode l = th_ldr_imm(R0, base, so < 0 ? -so : so, so < 0 ? 4 : 6, ENFORCE_ENCODING_NONE);
    thumb_opcode st = th_str_imm(R0, base, d < 0 ? -d : d, d < 0 ? 4 : 6, ENFORCE_ENCODING_NONE);
    if (!l.size || !st.size)
      return 0;
    plain += l.size + st.size;
  }
  const int kl = nlow < 4 ? nlow : 4, ka = nall < 4 ? nall : 4;
  const int cost_low = kl >= 2 ? spill_block_copy_plan(base, src, dst, nwords, low, kl, rb, 0) : -1;
  const int cost_all = ka >= 2 ? spill_block_copy_plan(base, src, dst, nwords, all, ka, rb, 0) : -1;
  if (cost_low > 0 && cost_low < plain && (cost_all < 0 || cost_low <= cost_all))
    spill_block_copy_plan(base, src, dst, nwords, low, kl, rb, 1);
  else if (cost_all > 0 && cost_all < plain)
    spill_block_copy_plan(base, src, dst, nwords, all, ka, rb, 1);
  else
    return 0;
  return 1;
}

/* Size of one word access `reg <- [base, #off]` (or the store), as the encoder
 * would lay it out; 0 when the offset does not encode. */
static int block_copy_word_size(int reg, int base, int32_t off, int is_store)
{
  const int neg = off < 0;
  const uint32_t a = neg ? (uint32_t)-off : (uint32_t)off;
  const thumb_opcode o = is_store ? th_str_imm(reg, base, a, neg ? 4 : 6, ENFORCE_ENCODING_NONE)
                                  : th_ldr_imm(reg, base, a, neg ? 4 : 6, ENFORCE_ENCODING_NONE);
  return o.size;
}

/* Copy `nwords` words from src_base+src_off to dst_base+dst_off as one LDM and
 * one STM through `regs`, which the caller owns: each holds one word of the
 * copy and dies at its store.  Which register takes which word does not matter
 * -- LDM and STM both run in ascending register order, so the same list on both
 * sides keeps the words in place.  Unlike the frame-slot copier above, either
 * side may be a general base register (a pointer deref), and the whole copy is
 * one LDM followed by one STM, so it holds for overlapping ranges exactly as
 * the loads-then-stores it replaces does.  Returns 0, having emitted nothing,
 * when an address does not encode, no register is free to hold a destination
 * address that needs computing, or the pair is no smaller than the individual
 * accesses. */
ST_FUNC int tcc_gen_machine_reg_block_copy(int src_base, int32_t src_off, int dst_base, int32_t dst_off,
                                           const int *regs, int nwords, int end_idx)
{
  TCCIRState *ir = tcc_state->ir;
  if (!ir || nwords < 3 || nwords > 8)
    return 0;

  const int frame = tcc_state->need_frame_pointer ? R_FP : R_SP;
  if (src_base == MACH_BLOCK_COPY_FRAME)
  {
    src_base = frame;
    src_off = fp_adjust_local_offset(src_off, 0);
  }
  if (dst_base == MACH_BLOCK_COPY_FRAME)
  {
    dst_base = frame;
    dst_off = fp_adjust_local_offset(dst_off, 0);
  }
  if (src_base < 0 || src_base >= 16 || dst_base < 0 || dst_base >= 16)
    return 0;
  if ((src_off & 3) || (dst_off & 3))
    return 0;

  uint32_t list = 0;
  int all_low = 1, top_low = -1;
  for (int k = 0; k < nwords; k++)
  {
    const int r = regs[k];
    if (r < 0 || r >= 16 || r == R_SP || r == R_PC || (list & (1u << (uint32_t)r)))
      return 0;
    list |= 1u << (uint32_t)r;
    if (r > R7)
      all_low = 0;
    else if (r > top_low)
      top_low = r;
  }
  /* The LDM would overwrite the destination address before the STM reads it. */
  if (list & (1u << (uint32_t)dst_base))
    return 0;

  /* The LDM's base.  With an offset it goes into one of the loaded registers,
   * which the LDM then overwrites -- that is what makes it free.  At offset 0
   * the source register itself is the base, and since it is then not
   * necessarily in the list the wide (no-writeback) LDM has to carry it: the
   * narrow one writes back whenever its base is outside the list. */
  int ra = src_base;
  thumb_opcode add_src = {0};
  if (src_off != 0)
  {
    ra = top_low >= 0 ? top_low : regs[0];
    add_src = src_off < 0 ? th_sub_imm(ra, src_base, (uint32_t)-src_off, flags_safe(), ENFORCE_ENCODING_NONE)
                          : th_add_imm(ra, src_base, (uint32_t)src_off, flags_safe(), ENFORCE_ENCODING_NONE);
    if (!add_src.size)
      return 0;
  }
  const int ldm_narrow = all_low && ra <= R7 && (list & (1u << (uint32_t)ra));
  const thumb_opcode ldm = ldm_narrow ? (thumb_opcode){.size = 2, .opcode = 0xC800u | ((uint32_t)ra << 8) | list}
                                      : (thumb_opcode){.size = 4, .opcode = ((0xE890u | (uint32_t)ra) << 16) | list};

  /* The STM's base.  A computed destination address is ours to destroy, so it
   * may take the narrow (writeback) form; the destination register itself must
   * survive, so it takes the wide one with the writeback bit clear. */
  int rb = dst_base;
  thumb_opcode add_dst = {0};
  if (dst_off != 0)
  {
    /* Free at the run's LAST instruction, not its first: the destination
     * address is computed after the LDM has read everything it needs, so a
     * register the copy itself was using -- the source pointer above all -- is
     * as good as one that was free all along.  Nothing inside the run reads any
     * other register, so no value can end between the two points. */
    const uint32_t excl = list | (1u << (uint32_t)R_SP) | (1u << R_PC) | (1u << (uint32_t)dst_base) |
                          scratch_global_exclude;
    rb = tcc_ls_find_free_scratch_reg(&ir->ls, end_idx, excl, ir->leaffunc);
    if (rb < 0 || rb == PREG_NONE || rb >= 16 || (excl & (1u << (uint32_t)rb)))
      return 0;
    add_dst = dst_off < 0 ? th_sub_imm(rb, dst_base, (uint32_t)-dst_off, flags_safe(), ENFORCE_ENCODING_NONE)
                          : th_add_imm(rb, dst_base, (uint32_t)dst_off, flags_safe(), ENFORCE_ENCODING_NONE);
    if (!add_dst.size)
      return 0;
  }
  const int stm_narrow = all_low && rb <= R7 && dst_off != 0;
  const thumb_opcode stm = stm_narrow ? (thumb_opcode){.size = 2, .opcode = 0xC000u | ((uint32_t)rb << 8) | list}
                                      : (thumb_opcode){.size = 4, .opcode = ((0xE880u | (uint32_t)rb) << 16) | list};

  /* Against the loads and stores themselves, paired into LDRD/STRD where the
   * peepholes downstream would pair them. */
  int plain = 0;
  for (int k = 0; k < nwords; k++)
  {
    const int l = block_copy_word_size(regs[k], src_base, src_off + 4 * k, 0);
    const int s = block_copy_word_size(regs[k], dst_base, dst_off + 4 * k, 1);
    if (!l || !s)
      return 0;
    plain += l + s;
  }
  for (int k = 0; k + 1 < nwords; k += 2)
  {
    const int l = block_copy_word_size(regs[k], src_base, src_off + 4 * k, 0) +
                  block_copy_word_size(regs[k + 1], src_base, src_off + 4 * k + 4, 0);
    const int s = block_copy_word_size(regs[k], dst_base, dst_off + 4 * k, 1) +
                  block_copy_word_size(regs[k + 1], dst_base, dst_off + 4 * k + 4, 1);
    plain -= (l > 4 ? l - 4 : 0) + (s > 4 ? s - 4 : 0);
  }
  if (add_src.size + ldm.size + add_dst.size + stm.size >= plain)
    return 0;

  if (add_src.size)
    ot_check(add_src);
  ot_check(ldm);
  if (add_dst.size)
    ot_check(add_dst);
  ot_check(stm);
  return 1;
}

ST_FUNC void tcc_gen_machine_spill_block_copy(int32_t src_spill_off, int32_t dst_spill_off, int nwords)
{
  ScratchRegAlloc src_scratch = get_scratch_reg_with_save(0);
  int r_src = src_scratch.reg;
  ScratchRegAlloc dst_scratch = get_scratch_reg_with_save(1u << (uint32_t)r_src);
  int r_dst = dst_scratch.reg;

  tcc_machine_addr_of_stack_slot(r_src, src_spill_off, 0);
  tcc_machine_addr_of_stack_slot(r_dst, dst_spill_off, 0);

  int max_data = nwords < 4 ? nwords : 4;
  if (max_data < 1)
    max_data = 1;

  ScratchRegAlloc data_scratches[4];
  int data_regs[4];
  int ndata = 0;
  uint32_t exclude = (1u << (uint32_t)r_src) | (1u << (uint32_t)r_dst);
  for (int k = 0; k < max_data; k++)
  {
    data_scratches[k] = get_scratch_reg_with_save(exclude);
    data_regs[k] = data_scratches[k].reg;
    exclude |= (1u << (uint32_t)data_regs[k]);
    ndata++;
  }

  int remaining = nwords;

  while (remaining >= ndata && ndata >= 2)
  {
    uint32_t regset = 0;
    for (int j = 0; j < ndata; j++)
      regset |= (1u << (uint32_t)data_regs[j]);
    ot_check(th_ldm(r_src, regset, 1 /* writeback */, ENFORCE_ENCODING_NONE));
    ot_check(th_stm(r_dst, regset, 1 /* writeback */, ENFORCE_ENCODING_NONE));
    remaining -= ndata;
  }

  int dr = data_regs[0];
  while (remaining > 0)
  {
    ot_check_ldr_imm(dr, r_src, 0, 6, ENFORCE_ENCODING_NONE);
    ot_check_str_imm(dr, r_dst, 0, 6, ENFORCE_ENCODING_NONE);
    if (remaining > 1)
    {
      if (!ot(th_add_imm(r_src, r_src, 4, flags_safe(), ENFORCE_ENCODING_NONE)))
        tcc_ice("spill_block_copy cannot advance source pointer");
      if (!ot(th_add_imm(r_dst, r_dst, 4, flags_safe(), ENFORCE_ENCODING_NONE)))
        tcc_ice("spill_block_copy cannot advance dest pointer");
    }
    remaining--;
  }

  for (int k = ndata - 1; k >= 0; k--)
    restore_scratch_reg(&data_scratches[k]);
  restore_scratch_reg(&dst_scratch);
  restore_scratch_reg(&src_scratch);
}

ST_FUNC void tcc_gen_machine_trap_mop(void)
{
  /* Emit UDF #0xfe - Undefined instruction for trap */
  ot_check(th_udf(0xfe, ENFORCE_ENCODING_NONE));
}

ST_FUNC void tcc_gen_machine_prefetch_mop(MachineOperand addr, int rw)
{
  /* Emit PLD (Preload Data) or PLDW (Preload Data with intent to Write)
   * based on the rw hint.
   *
   * PLD/PLDW are hints to the memory system that data may be needed soon.
   * They don't wait for the data and don't fault if the address is invalid.
   *
   * We support several addressing modes:
   * - Register indirect: [Rn] -> use th_pld_imm with offset 0
   * - Register + immediate offset: [Rn, #imm]
   * - Literal (PC-relative): label
   */
  (void)rw; /* PLD/PLDW distinction may not be supported on all ARM variants */

  switch (addr.kind)
  {
  case MACH_OP_REG:
  {
    /* Register indirect: PLD [Rn] */
    int reg = addr.u.reg.r0;
    ot_check(th_pld_imm((uint32_t)reg, 0, 0));
    break;
  }
  case MACH_OP_SPILL:
  {
    /* Spill slot: compute address (FP + offset) then PLD */
    int32_t offset = addr.u.spill.offset;
    if (offset != 0)
    {
      ScratchRegAlloc scr = get_scratch_reg_with_save(0);
      load_full_const(scr.reg, PREG_NONE, LFC_SPLIT(offset));
      ot_check(th_add_reg(scr.reg, R_FP, scr.reg, flags_safe(), THUMB_SHIFT_DEFAULT,
                          ENFORCE_ENCODING_NONE));
      ot_check(th_pld_imm(scr.reg, 0, 0));
      restore_scratch_reg(&scr);
    }
    else
    {
      ot_check(th_pld_imm(R_FP, 0, 0));
    }
    break;
  }
  case MACH_OP_IMM:
  {
    /* For immediate addresses, load into a register first */
    ScratchRegAlloc scr = get_scratch_reg_with_save(0);
    load_full_const(scr.reg, PREG_NONE, LFC_SPLIT(addr.u.imm.val));
    ot_check(th_pld_imm(scr.reg, 0, 0));
    restore_scratch_reg(&scr);
    break;
  }
  case MACH_OP_SYMBOL:
  {
    /* For symbol addresses, load into a register first */
    ScratchRegAlloc scr = get_scratch_reg_with_save(0);
    _lfc_sym = addr.u.sym.sym;
    load_full_const(scr.reg, PREG_NONE, LFC_SPLIT(addr.u.sym.addend));
    ot_check(th_pld_imm(scr.reg, 0, 0));
    restore_scratch_reg(&scr);
    break;
  }
  case MACH_OP_FRAME_ADDR:
  {
    /* Frame address: frame base + the frame offset, adjusted like any local
     * (this used to add the raw IR offset to r7, frame pointer or not). */
    int32_t offset = fp_adjust_local_offset(addr.u.frame.offset, 0);
    const int base = tcc_state->need_frame_pointer ? R_FP : R_SP;
    if (offset != 0)
    {
      ScratchRegAlloc scr = get_scratch_reg_with_save(0);
      load_full_const(scr.reg, PREG_NONE, LFC_SPLIT(offset));
      ot_check(th_add_reg(scr.reg, base, scr.reg, flags_safe(), THUMB_SHIFT_DEFAULT,
                          ENFORCE_ENCODING_NONE));
      ot_check(th_pld_imm(scr.reg, 0, 0));
      restore_scratch_reg(&scr);
    }
    else
    {
      ot_check(th_pld_imm(base, 0, 0));
    }
    break;
  }
  default:
    tcc_error("unsupported operand type for __builtin_prefetch");
  }
}

/* __builtin_setjmp implementation for ARM Thumb-2.
 *
 * GCC's documented ABI gives __builtin_setjmp a 5-word buffer; callers
 * (e.g. gcc.c-torture pr84521) really do pass `void *buf[5]`, so nothing
 * larger may be written through the buffer pointer.  The callee-saved
 * register file (r4-r11) still must be restored on longjmp — the register
 * allocator keeps VARs and the R9 GOT base in r4-r11 across the setjmp —
 * so those 8 words live in a hidden, compiler-allocated save area in the
 * setjmp-containing function's frame (src2/area), which stays valid for
 * as long as a longjmp to this buffer is legal.
 *
 * Jump buffer layout (4 words used, fits the standard 5-word buffer):
 *   buf[0]  = frame pointer (R7/FP)
 *   buf[1]  = resume address (Thumb-bit set)
 *   buf[2]  = stack pointer (SP)
 *   buf[3]  = address of the hidden r4-r11 save area (32 bytes)
 *
 * Returns 0 on initial call, 1 when returning via longjmp.
 */
ST_FUNC void tcc_gen_machine_setjmp_mop(MachineOperand buf, MachineOperand area, MachineOperand dest)
{
  MachineCodegenContext ctx = {0};
  int buf_reg;

  if (buf.kind == MACH_OP_NONE)
  {
    buf_reg = mach_alloc_scratch(&ctx, 0);
    ot_check(th_mov_imm(buf_reg, 0, flags_safe(), ENFORCE_ENCODING_NONE));
  }
  else
  {
    /* Exclude r4-r11 as scratch candidates: a saved-scratch there would
     * hold the buffer pointer when the area stores below run, corrupting
     * the saved register file (same class as the MLA scratch-pop bug). */
    buf_reg = mach_ensure_in_reg(&ctx, &buf, 0x0FF0);
  }

  /* ---- save callee-saved r4-r11 into the hidden frame area ----
   * The area address is computed in IP (caller-saved) so the r4-r11
   * values stored are the untouched setjmp-time ones; a scratch from
   * mach_alloc_scratch could pick a callee-saved register. */
  if (area.kind == MACH_OP_FRAME_ADDR)
  {
    tcc_machine_addr_of_stack_slot(R_IP, area.u.frame.offset, 0 /* not param */);
  }
  else
  {
    tcc_ice("setjmp save area must be a frame slot (kind %d)", (int)area.kind);
  }
  ot_check_str_imm(4, R_IP, 0, 6, ENFORCE_ENCODING_NONE);     /* r4  -> area[0] */
  ot_check_str_imm(5, R_IP, 4, 6, ENFORCE_ENCODING_NONE);     /* r5  -> area[1] */
  ot_check_str_imm(6, R_IP, 8, 6, ENFORCE_ENCODING_NONE);     /* r6  -> area[2] */
  ot_check_str_imm(R_FP, R_IP, 12, 6, ENFORCE_ENCODING_NONE); /* r7  -> area[3] */
  ot_check_str_imm(8, R_IP, 16, 6, ENFORCE_ENCODING_NONE);    /* r8  -> area[4] */
  ot_check_str_imm(9, R_IP, 20, 6, ENFORCE_ENCODING_NONE);    /* r9  -> area[5] */
  ot_check_str_imm(10, R_IP, 24, 6, ENFORCE_ENCODING_NONE);   /* r10 -> area[6] */
  ot_check_str_imm(11, R_IP, 28, 6, ENFORCE_ENCODING_NONE);   /* r11 -> area[7] */
  ot_check_str_imm(R_IP, buf_reg, 12, 6, ENFORCE_ENCODING_NONE); /* &area -> buf[3] */

  /* ---- save frame pointer ---- */
  ot_check_str_imm(R_FP, buf_reg, 0, 6, ENFORCE_ENCODING_NONE); /* r7  -> buf[0]  */

  /* ---- save SP ---- */
  ot_check_mov_reg(R_IP, R_SP, flags_safe(), THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE, false);
  ot_check_str_imm(R_IP, buf_reg, 8, 6, ENFORCE_ENCODING_NONE); /* SP -> buf[2] */

  /* ---- save resume address (ADR IP, resume_label) ----
   * The ADR immediate is the fixed distance to the resume label, so nothing
   * may be emitted between ADR and label except the five instructions below:
   * pick the destination register first (it can emit a scratch save) and
   * flush the literal pool up front if it would otherwise fall in the window. */
  int dest_reg = mach_get_dest_reg(&ctx, &dest, 0);
  th_literal_pool_reserve_upcoming_bytes(24);
  int adr_addr = ind;
  int adr_pc = adr_addr + 4;
  int adr_base = adr_pc & ~3;
  int resume_label_addr = adr_addr + 20; /* 4(ORR)+4(STR)+4(MOV)+4(B) after ADR */
  int adr_imm = resume_label_addr - adr_base;
  ot_check(th_adr_imm(R_IP, adr_imm, ENFORCE_ENCODING_32BIT));

  ot_check(th_orr_imm(R_IP, R_IP, 1, flags_safe(), ENFORCE_ENCODING_NONE)); /* Thumb bit */
  ot_check_str_imm(R_IP, buf_reg, 4, 6, ENFORCE_ENCODING_NONE);                              /* -> buf[1] */

  /* ---- normal path: return 0 ---- */
  ot_check(th_mov_imm(dest_reg, 0, flags_safe(), ENFORCE_ENCODING_32BIT)); /* dest = 0 */
  ot_check(th_b_t4(4));                                                                     /* B.W +4 (skip resume) */
  codegen_internal_merge_point(); /* longjmp lands below */

  /* ---- resume_label: longjmp lands here ---- */
  if (ind != resume_label_addr)
    tcc_ice("__builtin_setjmp resume label moved (pool flush inside ADR window)");
  ot_check(th_mov_imm(dest_reg, 1, flags_safe(), ENFORCE_ENCODING_32BIT)); /* dest = 1 */
  /* ---- end_label ---- */

  mach_writeback_dest(&dest, dest_reg);
  mach_release_all(&ctx);
}

/* Non-local goto setjmp: saves ALL callee-saved registers (r4-r11), SP,
 * and resume address in a 40-byte buffer. Used for __label__ + nested
 * function goto support.
 *
 * Buffer layout (10 words = 40 bytes):
 *   buf[0-7]  = r4-r11
 *   buf[8]    = SP
 *   buf[9]    = resume address (Thumb-bit set)
 */
ST_FUNC void tcc_gen_machine_nl_setjmp_mop(MachineOperand buf, MachineOperand dest)
{
  MachineCodegenContext ctx = {0};
  int buf_reg;

  if (buf.kind == MACH_OP_NONE)
  {
    buf_reg = mach_alloc_scratch(&ctx, 0);
    ot_check(th_mov_imm(buf_reg, 0, flags_safe(), ENFORCE_ENCODING_NONE));
  }
  else
  {
    buf_reg = mach_ensure_in_reg(&ctx, &buf, 0);
  }

  /* ---- save callee-saved registers r4-r11 ---- */
  ot_check_str_imm(4, buf_reg, 0, 6, ENFORCE_ENCODING_NONE);     /* r4  -> buf[0]  */
  ot_check_str_imm(5, buf_reg, 4, 6, ENFORCE_ENCODING_NONE);     /* r5  -> buf[1]  */
  ot_check_str_imm(6, buf_reg, 8, 6, ENFORCE_ENCODING_NONE);     /* r6  -> buf[2]  */
  ot_check_str_imm(R_FP, buf_reg, 12, 6, ENFORCE_ENCODING_NONE); /* r7  -> buf[3]  */
  ot_check_str_imm(8, buf_reg, 16, 6, ENFORCE_ENCODING_NONE);    /* r8  -> buf[4]  */
  ot_check_str_imm(9, buf_reg, 20, 6, ENFORCE_ENCODING_NONE);    /* r9  -> buf[5]  */
  ot_check_str_imm(10, buf_reg, 24, 6, ENFORCE_ENCODING_NONE);   /* r10 -> buf[6]  */
  ot_check_str_imm(11, buf_reg, 28, 6, ENFORCE_ENCODING_NONE);   /* r11 -> buf[7]  */

  /* ---- save SP ---- */
  ot_check_mov_reg(R_IP, R_SP, flags_safe(), THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE, false);
  ot_check_str_imm(R_IP, buf_reg, 32, 6, ENFORCE_ENCODING_NONE); /* SP -> buf[8] */

  /* ---- save resume address (ADR IP, resume_label) ----
   * The ADR immediate is the fixed distance to the resume label, so nothing
   * may be emitted between ADR and label except the five instructions below:
   * pick the destination register first (it can emit a scratch save) and
   * flush the literal pool up front if it would otherwise fall in the window. */
  int dest_reg = mach_get_dest_reg(&ctx, &dest, 0);
  th_literal_pool_reserve_upcoming_bytes(24);
  int adr_addr = ind;
  int adr_pc = adr_addr + 4;
  int adr_base = adr_pc & ~3;
  int resume_label_addr = adr_addr + 20; /* 4(ORR)+4(STR)+4(MOV)+4(B) after ADR */
  int adr_imm = resume_label_addr - adr_base;
  ot_check(th_adr_imm(R_IP, adr_imm, ENFORCE_ENCODING_32BIT));

  ot_check(th_orr_imm(R_IP, R_IP, 1, flags_safe(), ENFORCE_ENCODING_NONE)); /* Thumb bit */
  ot_check_str_imm(R_IP, buf_reg, 36, 6, ENFORCE_ENCODING_NONE);                             /* -> buf[9] */

  /* ---- normal path: return 0 ---- */
  ot_check(th_mov_imm(dest_reg, 0, flags_safe(), ENFORCE_ENCODING_32BIT)); /* dest = 0 */
  ot_check(th_b_t4(4));                                                                     /* B.W +4 (skip resume) */
  codegen_internal_merge_point(); /* longjmp lands below */

  /* ---- resume_label: longjmp lands here ---- */
  if (ind != resume_label_addr)
    tcc_ice("__builtin_setjmp resume label moved (pool flush inside ADR window)");
  ot_check(th_mov_imm(dest_reg, 1, flags_safe(), ENFORCE_ENCODING_32BIT)); /* dest = 1 */
  /* ---- end_label ---- */

  mach_writeback_dest(&dest, dest_reg);
  mach_release_all(&ctx);
}

/* __builtin_longjmp implementation for ARM Thumb-2.
 *
 * Restores the callee-saved register file (r4-r11, from the hidden save
 * area whose address setjmp left in buf[3]) and SP, then jumps to the
 * resume address.  This function does not return, so every caller-saved
 * register is fair game as a temporary.
 *
 * Buffer layout (must match tcc_gen_machine_setjmp_mop):
 *   buf[0] = FP, buf[1] = resume_addr, buf[2] = SP, buf[3] = &save_area
 */
ST_FUNC void tcc_gen_machine_longjmp_mop(MachineOperand buf)
{
  MachineCodegenContext ctx = {0};
  int buf_reg;

  if (buf.kind == MACH_OP_NONE)
  {
    tcc_error("__builtin_longjmp: invalid buffer operand");
    return;
  }

  buf_reg = mach_ensure_in_reg(&ctx, &buf, 0);

  /* Copy buf pointer to IP so it survives the register restores */
  ot_check_mov_reg(R_IP, buf_reg, flags_safe(), THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE, false);

  /* Read resume address, saved SP and save-area pointer into caller-saved
   * regs before clobbering anything callee-saved. */
  ot_check_ldr_imm(0, R_IP, 4, 6, ENFORCE_ENCODING_NONE);  /* r0 = resume addr */
  ot_check_ldr_imm(1, R_IP, 8, 6, ENFORCE_ENCODING_NONE);  /* r1 = saved SP    */
  ot_check_ldr_imm(2, R_IP, 12, 6, ENFORCE_ENCODING_NONE); /* r2 = &save_area  */

  /* Restore callee-saved r4-r11 (r7/FP comes from the area too; the copy
   * in buf[0] is identical). */
  ot_check_ldr_imm(4, 2, 0, 6, ENFORCE_ENCODING_NONE);     /* r4  */
  ot_check_ldr_imm(5, 2, 4, 6, ENFORCE_ENCODING_NONE);     /* r5  */
  ot_check_ldr_imm(6, 2, 8, 6, ENFORCE_ENCODING_NONE);     /* r6  */
  ot_check_ldr_imm(R_FP, 2, 12, 6, ENFORCE_ENCODING_NONE); /* r7  */
  ot_check_ldr_imm(8, 2, 16, 6, ENFORCE_ENCODING_NONE);    /* r8  */
  allow_r9_write = 1; /* restoring the setjmp-time GOT base is the point */
  ot_check_ldr_imm(9, 2, 20, 6, ENFORCE_ENCODING_NONE);    /* r9  */
  allow_r9_write = 0;
  ot_check_ldr_imm(10, 2, 24, 6, ENFORCE_ENCODING_NONE);   /* r10 */
  ot_check_ldr_imm(11, 2, 28, 6, ENFORCE_ENCODING_NONE);   /* r11 */

  /* Restore SP */
  ot_check_mov_reg(R_SP, 1, flags_safe(), THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE, false);

  /* Jump to resume address (Thumb bit already set by setjmp code) */
  ot_check(th_bx_reg(0));

  mach_release_all(&ctx);
}

/* Non-local goto longjmp: restores ALL callee-saved registers (r4-r11), SP,
 * then jumps to the resume address. Used for __label__ + nested function goto.
 *
 * Buffer layout (must match nl_setjmp):
 *   buf[0-7] = r4-r11, buf[8] = SP, buf[9] = resume_addr
 */
ST_FUNC void tcc_gen_machine_nl_longjmp_mop(MachineOperand buf)
{
  MachineCodegenContext ctx = {0};
  int buf_reg;

  if (buf.kind == MACH_OP_NONE)
  {
    tcc_error("nl_longjmp: invalid buffer operand");
    return;
  }

  if (buf.kind == MACH_OP_CHAIN_REL)
  {
    /* For chain-relative buffers (non-local goto from nested function),
     * we need the ADDRESS of the buffer in the parent frame, not the value.
     * mach_ensure_in_reg would load the value; use LEA logic instead. */
    ScratchRegAlloc chain_scratch = {0};
    int chain_used = 0;
    uint32_t excl = 0;
    int base = resolve_chain_base(tcc_state->ir, buf.u.chain.chain_index, excl, &chain_scratch, &chain_used);
    buf_reg = mach_alloc_scratch(&ctx, excl | (1u << (uint32_t)base));
    int32_t off = buf.u.chain.offset;
    int sign = (off < 0);
    int abs_off = sign ? (int)(-off) : (int)off;
    if (abs_off == 0)
    {
      if (buf_reg != base)
        ot_check_mov_reg((uint32_t)buf_reg, (uint32_t)base, flags_safe(), THUMB_SHIFT_DEFAULT,
                         ENFORCE_ENCODING_NONE, false);
    }
    else
    {
      thumb_opcode ins = sign
                             ? th_sub_imm(buf_reg, base, abs_off, flags_safe(), ENFORCE_ENCODING_NONE)
                             : th_add_imm(buf_reg, base, abs_off, flags_safe(), ENFORCE_ENCODING_NONE);
      if (ins.size != 0)
      {
        ot_check(ins);
      }
      else
      {
        ScratchRegAlloc off_sc = get_scratch_reg_with_save(excl | (1u << (uint32_t)buf_reg) | (1u << (uint32_t)base));
        load_full_const(off_sc.reg, PREG_NONE, LFC_SPLIT(abs_off));
        ot_check(sign ? th_sub_reg(buf_reg, base, off_sc.reg, flags_safe(), THUMB_SHIFT_DEFAULT,
                                   ENFORCE_ENCODING_NONE)
                      : th_add_reg(buf_reg, base, off_sc.reg, flags_safe(), THUMB_SHIFT_DEFAULT,
                                   ENFORCE_ENCODING_NONE));
        restore_scratch_reg(&off_sc);
      }
    }
    if (chain_used)
      restore_scratch_reg(&chain_scratch);
  }
  else
  {
    buf_reg = mach_ensure_in_reg(&ctx, &buf, 0);
  }

  /* Copy buf pointer to IP so it survives register restores */
  ot_check_mov_reg(R_IP, buf_reg, flags_safe(), THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE, false);

  /* Load resume address and saved SP into caller-saved regs first
   * (before we clobber r4+ with the restore) */
  ot_check_ldr_imm(0, R_IP, 36, 6, ENFORCE_ENCODING_NONE); /* r0 = resume addr */
  ot_check_ldr_imm(1, R_IP, 32, 6, ENFORCE_ENCODING_NONE); /* r1 = saved SP    */

  /* Restore callee-saved registers r4-r11 */
  ot_check_ldr_imm(4, R_IP, 0, 6, ENFORCE_ENCODING_NONE);     /* r4  = buf[0] */
  ot_check_ldr_imm(5, R_IP, 4, 6, ENFORCE_ENCODING_NONE);     /* r5  = buf[1] */
  ot_check_ldr_imm(6, R_IP, 8, 6, ENFORCE_ENCODING_NONE);     /* r6  = buf[2] */
  ot_check_ldr_imm(R_FP, R_IP, 12, 6, ENFORCE_ENCODING_NONE); /* r7  = buf[3] (FP) */
  ot_check_ldr_imm(8, R_IP, 16, 6, ENFORCE_ENCODING_NONE);    /* r8  = buf[4] */
  allow_r9_write = 1;
  ot_check_ldr_imm(9, R_IP, 20, 6, ENFORCE_ENCODING_NONE); /* r9  = buf[5] */
  allow_r9_write = 0;
  ot_check_ldr_imm(10, R_IP, 24, 6, ENFORCE_ENCODING_NONE); /* r10 = buf[6] */
  ot_check_ldr_imm(11, R_IP, 28, 6, ENFORCE_ENCODING_NONE); /* r11 = buf[7] */

  /* Restore SP */
  ot_check_mov_reg(R_SP, 1, flags_safe(), THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE, false);

  /* Jump to resume address (Thumb bit already set by setjmp code) */
  ot_check(th_bx_reg(0));

  mach_release_all(&ctx);
}

/* ============================================================================
 * __builtin_apply_args / __builtin_apply implementation for ARM Thumb-2
 * ============================================================================
 *
 * __builtin_apply_args() returns a pointer to a saved argument block:
 *   [0]  pointer to incoming stack arguments (above saved register area)
 *   [4]  saved r0
 *   [8]  saved r1
 *   [12] saved r2
 *   [16] saved r3
 *
 * The prologue stores r0-r3 and the stack args pointer when
 * func_save_apply_args is set.  This handler just computes the address.
 *
 * __builtin_apply(fn, args, size) restores r0-r3 from the args block,
 * calls fn via BLX, and returns the result in dest (r0).
 */

ST_FUNC void tcc_gen_machine_builtin_apply_args_mop(MachineOperand dest)
{
  MachineCodegenContext ctx = {0};

  /* The apply_args block lives at tcc_state->apply_args_offset relative to FP.
   * Compute FP + adjusted_offset into the dest register. */
  int dest_reg = mach_get_dest_reg(&ctx, &dest, 0);
  int offset = tcc_state->apply_args_offset;
  tcc_machine_addr_of_stack_slot(dest_reg, offset, 0 /* not param */);

  mach_writeback_dest(&dest, dest_reg);
  mach_release_all(&ctx);
}

/* __builtin_return_address(0).  force_lr_save makes every prologue save LR,
 * and LR is the highest register of its register push, so it sits in the word
 * just below the argument base: param offset 0 is the pushed r0 of a function
 * whose r0-r3 are saved first (split struct), else the first stack argument.
 * A variadic function's offset_to_args also spans its r0-r3 save.  The same
 * holds for the two-phase {r7, lr} frame record.  Reading the slot off SP
 * needs no frame pointer, which used to be forced here: a Thumb r7 frame
 * addresses locals at -255..0, so every other access of a large function
 * cost movw + rsb + ldr (zig's allocator helpers read @returnAddress()). */
ST_FUNC void tcc_gen_machine_return_address_mop(MachineOperand dest)
{
  MachineCodegenContext ctx = {0};
  int rd = mach_get_dest_reg(&ctx, &dest, 0);
  const int adjusted = param_frame_offset(-4 - (func_var ? vararg_push_size : 0));
  const int base_reg = tcc_state->need_frame_pointer ? R_FP : R_SP;
  const int sign = adjusted < 0;
  load_from_base(rd, PREG_REG_NONE, IROP_BTYPE_INT32, 0, sign ? -adjusted : adjusted, sign, (uint32_t)base_reg);
  mach_writeback_dest(&dest, rd);
  mach_release_all(&ctx);
}

ST_FUNC void tcc_gen_machine_builtin_apply_mop(MachineOperand fn, MachineOperand args, MachineOperand dest)
{
  MachineCodegenContext ctx = {0};

  /* Registers destroyed by the restore-and-call sequence below: r0-r3 are
   * reloaded with the saved argument values, and ip(r12)+lr are clobbered by
   * the BLX.  The args-block base pointer (used by all four restore loads) and
   * the callee address must therefore live OUTSIDE this set until used. */
  const uint32_t clobbered =
      (1u << R0) | (1u << R1) | (1u << R2) | (1u << R3) | (1u << (uint32_t)R_IP);

  /* Step 1: Materialize the args block pointer, then guarantee it is in a
   * register the restore loads won't overwrite.  mach_ensure_in_reg returns an
   * already-allocated operand register verbatim (ignoring the exclusion mask),
   * so when the value already lives in r0-r3 / ip we must relocate it to a
   * safe scratch — otherwise the very first load (r0 <- [base+4]) destroys the
   * base pointer and the remaining loads read from garbage addresses. */
  int args_reg = mach_ensure_in_reg(&ctx, &args, clobbered);
  if (clobbered & (1u << (uint32_t)args_reg))
  {
    int safe = mach_alloc_scratch(&ctx, clobbered);
    ot_check_mov_reg(safe, args_reg, flags_safe(), THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE, false);
    args_reg = safe;
  }

  /* Step 2: Load the function pointer into R12 (IP), which survives the
   * register loads below because IP is not one of r0-r3. */
  int fn_reg = mach_ensure_in_reg(&ctx, &fn, (1u << (uint32_t)args_reg));
  if (fn_reg != R_IP)
  {
    ot_check_mov_reg(R_IP, fn_reg, flags_safe(), THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE, false);
  }

  /* Step 3: Restore r0-r3 from the args block.
   * Layout: [+0]=stack_args_ptr, [+4]=r0, [+8]=r1, [+12]=r2, [+16]=r3. */
  ot_check_ldr_imm(R0, args_reg, 4, 6, ENFORCE_ENCODING_NONE);
  ot_check_ldr_imm(R1, args_reg, 8, 6, ENFORCE_ENCODING_NONE);
  ot_check_ldr_imm(R2, args_reg, 12, 6, ENFORCE_ENCODING_NONE);
  ot_check_ldr_imm(R3, args_reg, 16, 6, ENFORCE_ENCODING_NONE);

  /* Step 4: Call the function via BLX R12.
   * This clobbers LR and r0-r3 (caller-saved). */
  ot_check(th_blx_reg(R_IP));

  /* Step 5: Move return value (r0) to dest register. */
  int dest_reg = mach_get_dest_reg(&ctx, &dest, 0);
  if (dest_reg != R0)
  {
    ot_check_mov_reg(dest_reg, R0, flags_safe(), THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE, false);
  }

  mach_writeback_dest(&dest, dest_reg);
  mach_release_all(&ctx);
}

ST_FUNC void tcc_gen_machine_backpatch_jump(int address, int offset)
{
  th_patch_call(address, offset);
}

static int tcc_get_type_size(CType *type)
{
  switch (type->t & VT_BTYPE)
  {
  case VT_BYTE:
    return 1;
  case VT_SHORT:
    return 2;
  case VT_INT:
  case VT_LONG:
    return 4;
  case VT_LLONG:
    return 8;
  case VT_FLOAT:
    return 4;
  case VT_DOUBLE:
    return 8;
  case VT_LDOUBLE:
    return 8; // treat long double as double for ARM EABI softcalls
  default:
    return 0;
  }
}

ST_FUNC const char *tcc_get_abi_softcall_name(SValue *src1, SValue *src2, SValue *dest, TccIrOp op)
{
  const int src1_64bit = tcc_is_64bit_operand(src1);
  const int src2_64bit = src2 ? tcc_is_64bit_operand(src2) : 0;
  const int dest_64bit = dest ? tcc_is_64bit_type(dest->type.t) : 0;
  const int src1_size = tcc_get_type_size(&src1->type);
  const int dest_size = dest ? tcc_get_type_size(&dest->type) : 0;

  if (src1_64bit || src2_64bit || dest_64bit)
  {
    switch (op)
    {
    case TCCIR_OP_FADD:
      return "__aeabi_dadd";
    case TCCIR_OP_FSUB:
      return "__aeabi_dsub";
    case TCCIR_OP_FMUL:
      return "__aeabi_dmul";
    case TCCIR_OP_FDIV:
      return "__aeabi_ddiv";
    case TCCIR_OP_FNEG:
      return "__aeabi_dneg";
    default:
      break;
    }
  }
  else
  {
    switch (op)
    {
    case TCCIR_OP_FADD:
      return "__aeabi_fadd";
    case TCCIR_OP_FSUB:
      return "__aeabi_fsub";
    case TCCIR_OP_FMUL:
      return "__aeabi_fmul";
    case TCCIR_OP_FDIV:
      return "__aeabi_fdiv";
    case TCCIR_OP_FNEG:
      return "__aeabi_fneg";
    default:
      break;
    }
  }

  switch (op)
  {
  case TCCIR_OP_CVT_FTOF:
  {
    if (src1_size == 4 && dest_size == 8)
    {
      return "__aeabi_f2d";
    }
    else if (src1_size == 8 && dest_size == 4)
    {
      return "__aeabi_d2f";
    }
    /* Same size conversion is a no-op, no function needed */
    return NULL;
  }
  break;
  case TCCIR_OP_CVT_FTOI:
  {
    /* Float/double to integer conversion.
     * Map based on destination width, not just VT_BTYPE (since VT_LONG is 32-bit on ARM).
     * Use the standard ARM EABI helpers:
     *  - 32-bit: __aeabi_{f,d}2iz / __aeabi_{f,d}2uiz
     *  - 64-bit: __aeabi_{f,d}2lz / __aeabi_{f,d}2ulz
     */
    const int is_float = (src1_size == 4);
    const int is_unsigned = (dest && (dest->type.t & VT_UNSIGNED)) ? 1 : 0;

    if (dest_size == 8)
    {
      return is_unsigned ? (is_float ? "__aeabi_f2ulz" : "__aeabi_d2ulz")
                         : (is_float ? "__aeabi_f2lz" : "__aeabi_d2lz");
    }

    return is_unsigned ? (is_float ? "__aeabi_f2uiz" : "__aeabi_d2uiz") : (is_float ? "__aeabi_f2iz" : "__aeabi_d2iz");
  }
  break;
  case TCCIR_OP_FCMP:
  {
    /* Get comparison operation from src2.c.i (stored during IR generation) */
    int cmp_op = src2->c.i;
    int is_float = (src1_size == 4);

    switch (cmp_op)
    {
    case TOK_EQ:
      return is_float ? "__aeabi_fcmpeq" : "__aeabi_dcmpeq";
    case TOK_NE:
      /* NE uses cmpeq and inverts the result */
      return is_float ? "__aeabi_fcmpeq" : "__aeabi_dcmpeq";
    case TOK_LT:
    case TOK_ULT:
      return is_float ? "__aeabi_fcmplt" : "__aeabi_dcmplt";
    case TOK_LE:
    case TOK_ULE:
      return is_float ? "__aeabi_fcmple" : "__aeabi_dcmple";
    case TOK_GT:
    case TOK_UGT:
      return is_float ? "__aeabi_fcmpgt" : "__aeabi_dcmpgt";
    case TOK_GE:
    case TOK_UGE:
      return is_float ? "__aeabi_fcmpge" : "__aeabi_dcmpge";
    default:
      /* Fallback to cfcmple/cdcmple which sets flags */
      return is_float ? "__aeabi_cfcmple" : "__aeabi_cdcmple";
    }
  }
  break;
  case TCCIR_OP_CVT_ITOF:
  {
    /* Integer to float/double conversion.
     * Need to distinguish 32-bit int vs 64-bit long long sources:
     *  - 32-bit: __aeabi_{ui,i}2{d,f}
     *  - 64-bit: __aeabi_{ul,l}2{d,f}
     */
    int is_unsigned = (src1->type.t & VT_UNSIGNED) ? 1 : 0;
    if (src1_size == 8)
    {
      /* 64-bit integer source (long long / unsigned long long) */
      if (is_unsigned)
        return dest_64bit ? "__aeabi_ul2d" : "__aeabi_ul2f";
      return dest_64bit ? "__aeabi_l2d" : "__aeabi_l2f";
    }
    /* 32-bit integer source (int / unsigned int) */
    if (is_unsigned)
      return dest_64bit ? "__aeabi_ui2d" : "__aeabi_ui2f";
    return dest_64bit ? "__aeabi_i2d" : "__aeabi_i2f";
  }
  break;
  default:
    break;
  }

  return NULL;
}

/* tcc_gen_machine_func_parameter_mop: MachineOperand-based entry point for
 * FUNCPARAMVAL / FUNCPARAMVOID.  src2_enc must be MACH_OP_IMM holding the
 * packed call_id / param_idx value (same encoding as irop_get_imm64_ex).
 * src1 is the value being passed (unused here — handled by the call-site ABI).
 */
ST_FUNC void tcc_gen_machine_func_parameter_mop(MachineOperand src1, MachineOperand src2_enc, TccIrOp op)
{
  (void)src1;

  const uint32_t encoded = (uint32_t)src2_enc.u.imm.val;
  int call_id = TCCIR_DECODE_CALL_ID(encoded);
  int param_index = TCCIR_DECODE_PARAM_IDX(encoded);

  /* Find or create call site for this call_id */
  ThumbGenCallSite *call_site = thumb_get_or_create_call_site(call_id);
  if (call_site == NULL)
  {
    tcc_ice("failed to allocate call site for call_id=%d", call_id);
    return;
  }

  /* FUNCPARAMVOID is a marker for a 0-argument call.
   * Ensure the call site exists, but do not create a fake argument entry. */
  if (op == TCCIR_OP_FUNCPARAMVOID)
    return;

  /* During dry-run, don't modify the argument list - it causes memory leaks
   * when we restore the call sites after dry-run. The argument list is not
   * needed for scratch register tracking anyway. */
  if (dry_run_state.active)
    return;

  /* Expand argument list if needed.  Grow the allocation geometrically: params
   * arrive one index at a time, and growing by one int per param made a
   * 10000-argument call (limits-fnargs) do 10000 reallocs that each copy the
   * whole list -- ~200 MB of copying on a device without in-place realloc. */
  if (param_index >= call_site->function_argument_count)
  {
    int new_count = param_index + 1;
    if (new_count > call_site->function_argument_capacity)
    {
      int new_cap = call_site->function_argument_capacity ? call_site->function_argument_capacity * 2 : 8;
      if (new_cap < new_count)
        new_cap = new_count;
      call_site->function_argument_list = (int *)tcc_realloc(call_site->function_argument_list, new_cap * sizeof(int));
      call_site->function_argument_capacity = new_cap;
    }
    /* Initialize new slots */
    for (int i = call_site->function_argument_count; i < new_count; i++)
    {
      call_site->function_argument_list[i] = -1;
    }
    call_site->function_argument_count = new_count;
  }

  /* Store parameter information - for now just mark as present */
  call_site->function_argument_list[param_index] = 1; /* Mark parameter as present */
}
/* Emit a nested-function trampoline into the current text section.
 * chain_slot_sym: TCC symbol for the chain slot in .data
 * func_sym:       TCC symbol for the nested function in .text
 *
 * The trampoline loads the parent frame pointer from the chain slot
 * into R10 (the static-chain register) and tail-calls the nested function.
 *
 * Two variants:
 *  - GOT-indirect (text_and_data_separation): uses R9-relative GOT loads,
 *    relocations are R_ARM_GOT32 (linker-resolved, no absolute addresses
 *    in the code section).
 *  - Direct: inline literal pool with R_ARM_ABS32 relocations.
 */
ST_FUNC addr_t gen_nested_func_trampoline(Sym *chain_slot_sym, Sym *func_sym)
{
  Section *text_sec = cur_text_section;
  int use_got = tcc_state->text_and_data_separation;

  section_prealloc(text_sec, use_got ? 36 : 24);

  /* Align ind to 4-byte boundary */
  while (ind & 3)
    text_sec->data[ind++] = 0x00;

  addr_t tramp_start = ind;

  if (use_got)
  {
    /* GOT-indirect trampoline (32 bytes):
     *   +0:  LDR  r12, [pc, #20]  ; GOT offset of chain_slot (from +24)
     *   +4:  LDR  r10, [r9, r12]  ; chain_slot address via GOT
     *   +8:  LDR  r10, [r10, #0]  ; *chain_slot = parent FP
     *   +12: LDR  r12, [pc, #12]  ; GOT offset of function (from +28)
     *   +16: LDR  r12, [r9, r12]  ; function address via GOT
     *   +20: BX   r12             ; tail-call
     *   +22: NOP
     *   +24: .word 0              ; R_ARM_GOT32 chain_slot
     *   +28: .word 0              ; R_ARM_GOT32 function
     */

    /* +0: LDR R12, [PC, #20] - F8DF C014 */
    text_sec->data[ind++] = 0xDF;
    text_sec->data[ind++] = 0xF8;
    text_sec->data[ind++] = 0x14;
    text_sec->data[ind++] = 0xC0;

    /* +4: LDR R10, [R9, R12] - F859 A00C */
    text_sec->data[ind++] = 0x59;
    text_sec->data[ind++] = 0xF8;
    text_sec->data[ind++] = 0x0C;
    text_sec->data[ind++] = 0xA0;

    /* +8: LDR R10, [R10, #0] - F8DA A000 */
    text_sec->data[ind++] = 0xDA;
    text_sec->data[ind++] = 0xF8;
    text_sec->data[ind++] = 0x00;
    text_sec->data[ind++] = 0xA0;

    /* +12: LDR R12, [PC, #12] - F8DF C00C */
    text_sec->data[ind++] = 0xDF;
    text_sec->data[ind++] = 0xF8;
    text_sec->data[ind++] = 0x0C;
    text_sec->data[ind++] = 0xC0;

    /* +16: LDR R12, [R9, R12] - F859 C00C */
    text_sec->data[ind++] = 0x59;
    text_sec->data[ind++] = 0xF8;
    text_sec->data[ind++] = 0x0C;
    text_sec->data[ind++] = 0xC0;

    /* +20: BX R12 - 4760 */
    text_sec->data[ind++] = 0x60;
    text_sec->data[ind++] = 0x47;

    /* +22: NOP - BF00 */
    text_sec->data[ind++] = 0x00;
    text_sec->data[ind++] = 0xBF;

    /* +24: chain slot GOT offset */
    greloc(text_sec, chain_slot_sym, ind, R_ARM_GOT32);
    text_sec->data[ind++] = 0x00;
    text_sec->data[ind++] = 0x00;
    text_sec->data[ind++] = 0x00;
    text_sec->data[ind++] = 0x00;

    /* +28: function GOT offset */
    greloc(text_sec, func_sym, ind, R_ARM_GOT32);
    text_sec->data[ind++] = 0x00;
    text_sec->data[ind++] = 0x00;
    text_sec->data[ind++] = 0x00;
    text_sec->data[ind++] = 0x00;
  }
  else
  {
    /* Direct trampoline (20 bytes):
     *   +0:  LDR  r10, [pc, #8]   ; chain_slot address (from +12)
     *   +4:  LDR  r10, [r10, #0]  ; *chain_slot = parent FP
     *   +8:  LDR  pc, [pc, #4]    ; function address (from +16), tail call
     *   +12: .word chain_slot     ; R_ARM_ABS32
     *   +16: .word function        ; R_ARM_ABS32
     */

    /* LDR R10, [PC, #8] - F8DF A008 */
    text_sec->data[ind++] = 0xDF;
    text_sec->data[ind++] = 0xF8;
    text_sec->data[ind++] = 0x08;
    text_sec->data[ind++] = 0xA0;

    /* LDR R10, [R10, #0] - F8DA A000 */
    text_sec->data[ind++] = 0xDA;
    text_sec->data[ind++] = 0xF8;
    text_sec->data[ind++] = 0x00;
    text_sec->data[ind++] = 0xA0;

    /* LDR PC, [PC, #4] - F8DF F004 */
    text_sec->data[ind++] = 0xDF;
    text_sec->data[ind++] = 0xF8;
    text_sec->data[ind++] = 0x04;
    text_sec->data[ind++] = 0xF0;

    /* chain slot address */
    greloc(text_sec, chain_slot_sym, ind, R_ARM_ABS32);
    text_sec->data[ind++] = 0x00;
    text_sec->data[ind++] = 0x00;
    text_sec->data[ind++] = 0x00;
    text_sec->data[ind++] = 0x00;

    /* function address */
    greloc(text_sec, func_sym, ind, R_ARM_ABS32);
    text_sec->data[ind++] = 0x00;
    text_sec->data[ind++] = 0x00;
    text_sec->data[ind++] = 0x00;
    text_sec->data[ind++] = 0x00;
  }

  text_sec->data_offset = ind;
  return tramp_start + 1; /* +1 for Thumb interworking bit */
}
