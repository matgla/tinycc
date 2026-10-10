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

/* Thumb-2 code generator: instruction emission (ot), branch patching, SP
 * adjustment, frame-word pairing into LDRD/STRD, and loading constants and
 * addresses into registers. */

#include "arm-thumb-gen.h"

static int unalias_ldbl(int btype);
static int is_hgen_float_aggr(CType *type);

int ot(thumb_opcode op)
{
  if (op.size == 0)
    return op.size;

  /* Detect instructions that write to R9 when it's reserved for GOT pointer.
   * Exclude push/pop/stmdb/ldmia which legitimately save/restore R9. */
  if (text_and_data_separation && !allow_r9_write)
  {
    int dest = thumb_decode_dest_reg(op);
    if (dest == R9)
    {
      tcc_error("instruction 0x%0*x (size=%d) writes to R9 (GOT pointer) at ind=0x%x ir_op=%d", op.size == 4 ? 8 : 4,
                op.opcode, op.size, (unsigned)ind, g_debug_current_op);
    }
  }

  /* Update the MOV-coalescing register-equivalence cache and the STR->LDR
   * redundant-reload cache based on what is about to be emitted.  This only
   * tracks state — no elision happens here; elision is performed at the
   * call sites via ot_check_mov_reg / ot_check_ldr_imm so that ot()'s
   * return value remains the real emitted size and downstream jump/offset
   * accounting never sees a phantom emission.
   *
   * IT blocks: instructions inside an IT/ITx/ITxy/ITxyz are conditionally
   * executed.  Their writes are therefore not guaranteed, so destination
   * registers must be invalidated rather than recorded as equivalences. */
  if (thumb_gen_state.generating_function)
  {
    if (mov_equiv_it_pending > 0)
    {
      /* Conditional instruction: pessimistically drop anything this op
       * might write, and never record new equivalences.  Treat STR/LDR
       * the same way — their effect is gated on the IT condition. */
      int mv_rd = -1, mv_rm = -1;
      if (decode_mov_reg_plain(op, &mv_rd, &mv_rm))
      {
        mov_equiv_invalidate_reg(mv_rd);
        strldr_cache_invalidate_reg(mv_rd);
        imm_cache_invalidate_reg(mv_rd);
      }
      else if (thumb_op_is_pure_flag_setter(op))
      {
        /* CMP/CMN/TST/TEQ — no GPR clobber even under predication. */
      }
      else if (thumb_op_is_plain_branch(op))
      {
        /* Writes neither a GPR nor memory; see the unconditioned path. */
      }
      else
      {
        int dest = thumb_decode_dest_reg(op);
        if (dest >= 0)
        {
          mov_equiv_invalidate_reg(dest);
          strldr_cache_invalidate_reg(dest);
          imm_cache_invalidate_reg(dest);
        }
        else
        {
          mov_equiv_reset_all();
          tcc_gen_machine_strldr_cache_reset();
          imm_cache_reset_all();
        }
      }
      mov_equiv_it_pending--;
    }
    else
    {
      int it_len = mov_equiv_it_block_length(op);
      if (it_len > 0)
      {
        /* IT itself writes no GPR; start the conditional window. */
        mov_equiv_it_pending = it_len;
      }
      else
      {
        int mv_rd = -1, mv_rm = -1;
        int sl_is_str = 0, sl_rt = 0, sl_rn = 0, sl_imm = 0, sl_width = 0;
        uint32_t sl_puw = 0;
        int sd_is_str = 0, sd_rn = 0, sd_imm = 0;
        if (decode_str_ldr_imm(op, &sl_is_str, &sl_rt, &sl_rn, &sl_imm, &sl_puw, &sl_width))
        {
          if (!sl_is_str)
          {
            /* LDR writes Rt: invalidate both caches for that register FIRST.
             * If the call-site helper ran the match it would have
             * elided without reaching ot(); so if we get here, this LDR
             * is actually emitting and genuinely clobbers Rt. */
            mov_equiv_invalidate_reg(sl_rt);
            strldr_cache_invalidate_reg(sl_rt);
            imm_cache_invalidate_reg(sl_rt);
          }
          /* Either way Rt now holds [Rn+imm], so both directions are worth
           * recording; a store additionally kills what it overwrote. */
          strldr_cache_record_access(sl_rt, sl_rn, sl_imm, sl_puw, op.size, sl_width, sl_is_str);
          if (sl_puw & 1)
          {
            /* Write-back form (post/pre-indexed): Rn now holds the UPDATED
             * address.  record_access already threw the whole strldr cache
             * away (puw != 6); the register-equivalence and known-immediate
             * caches must drop Rn too, or a later `mov rn,rx` / immediate
             * rematerialization gets elided against the pre-increment value. */
            mov_equiv_invalidate_reg(sl_rn);
            imm_cache_invalidate_reg(sl_rn);
          }
        }
        else if (decode_mov_reg_plain(op, &mv_rd, &mv_rm))
        {
          mov_equiv_record_mov(mv_rd, mv_rm);
          strldr_cache_invalidate_reg(mv_rd);
          imm_cache_invalidate_reg(mv_rd);
        }
        else if (op.size == 4 &&
                 (((op.opcode >> 16) & 0xFE40) == 0xE840))
        {
          /* LDRD/STRD (Thumb-2): encoded as 1110 100P U1W0 nnnn (STRD) or
           * 1110 100P U1W1 nnnn (LDRD).  Bit 20 (high-halfword bit 4)
           * distinguishes load (1) vs store (0).
           *
           * STRD writes no GPR — only memory.  LDRD writes both Rt and Rt2
           * (low-halfword bits [15:12] and [11:8] respectively).  Either way
           * the rest of the GPR-equivalence cache is unaffected, so don't
           * fall through to the "unknown opcode → reset everything" path
           * which destroys upstream coalescing wins. */
          if ((op.opcode >> 20) & 1)
          {
            /* LDRD: invalidate Rt and Rt2 (writeback to Rn is rare here and
             * already covered by the writeback handling — for the typical
             * STRD imm with W=0 used by the codegen we don't touch Rn). */
            int rt = (int)((op.opcode >> 12) & 0xF);
            int rt2 = (int)((op.opcode >> 8) & 0xF);
            mov_equiv_invalidate_reg(rt);
            mov_equiv_invalidate_reg(rt2);
            strldr_cache_invalidate_reg(rt);
            strldr_cache_invalidate_reg(rt2);
            imm_cache_invalidate_reg(rt);
            imm_cache_invalidate_reg(rt2);
          }
          else if (decode_strd_ldrd_imm(op, &sd_is_str, &sd_rn, &sd_imm))
          {
            /* STRD writes no GPR, but it writes EIGHT bytes of memory: with
             * the cache living across IR ops, missing that leaves a stale
             * entry for either half.  Undecodable form -> assume the worst. */
            strldr_cache_invalidate_mem(sd_rn, sd_imm, 8);
            /* Then record what it just put there, as the two 4-byte slots the
             * rest of the cache is made of.  That is what lets the LDRD which
             * reads the pair straight back -- the return-value slot round trip
             * every 64-bit helper ends with -- be skipped entirely. */
            if (sd_imm >= 0)
            {
              /* Only the U=1 form: every entry in this cache is a puw==6
               * access, and handing a subtract-offset one to the recorder
               * would make it throw the whole cache away. */
              int sd_rt = (int)((op.opcode >> 12) & 0xF);
              int sd_rt2 = (int)((op.opcode >> 8) & 0xF);
              strldr_cache_record_access(sd_rt, sd_rn, sd_imm, 6, 4, 4, 1);
              strldr_cache_record_access(sd_rt2, sd_rn, sd_imm + 4, 6, 4, 4, 1);
            }
          }
          else
          {
            tcc_gen_machine_strldr_cache_reset();
          }
        }
        else if (thumb_op_is_pure_flag_setter(op))
        {
          /* CMP/CMN/TST/TEQ write only the flags — no GPR clobber, no
           * cache invalidation needed. */
        }
        else if (thumb_op_is_plain_branch(op))
        {
          /* B / B<c> / CBZ / CBNZ write neither a GPR nor memory, so the
           * FALL-THROUGH path's register and memory state is exactly what it
           * was before -- and the taken path lands on an IR-level jump target,
           * where codegen.c resets.  Without this a conditional branch inside
           * a basic block would drop every entry, which is the whole shape
           * this cache exists for: reload four spilled halves, compare, branch,
           * and use the same four in the not-taken arm.  BL/BLX are NOT here:
           * they write LR and clobber memory, and fall into the reset below. */
        }
        else
        {
          int dest = thumb_decode_dest_reg(op);
          if (dest >= 0)
          {
            mov_equiv_invalidate_reg(dest);
            strldr_cache_invalidate_reg(dest);
            imm_cache_invalidate_reg(dest);
          }
          else
          {
            mov_equiv_reset_all();
            tcc_gen_machine_strldr_cache_reset();
            imm_cache_reset_all();
          }
        }
      }
    }
  }
  else
  {
    mov_equiv_reset_all();
    tcc_gen_machine_strldr_cache_reset();
    imm_cache_reset_all();
  }

  /* -Os outliner: an instruction of a window the real pass calls instead is
   * recorded, not written, after the caches above saw it -- the BL runs
   * exactly this code.  `ind` advances as if it had been emitted, so the
   * frame-word pair peephole can still rewind over it. */
  if (tcc_gen_machine_outline_suppress(op.opcode, op.size))
  {
    thumb_gen_state.code_size += op.size;
    ind += op.size;
    return op.size;
  }

  /* Literal-pool flush safety around IT blocks.  Call-site reservations
   * (th_literal_pool_reserve_upcoming_bytes) cover the block's CODE bytes,
   * but a conditioned arm that materializes a large constant
   * (load_full_const) grows the pool AFTER the reservation was checked, so
   * the threshold can still trip mid-block.  Track the architectural IT
   * window here and (a) never flush while an op is conditioned, (b) flush
   * BEFORE the IT opcode itself if the worst-case block — 4 code bytes plus
   * an 8-byte pool entry per conditioned instruction — could hit the
   * threshold, so the deferred flush of (a) never overshoots the LDR-literal
   * range.  Runs in both passes so dry-run and real layouts stay identical. */
  int op_in_it_block = 0;
  if (thumb_gen_state.generating_function)
  {
    if (pool_flush_it_pending > 0)
    {
      op_in_it_block = 1;
      pool_flush_it_pending--;
    }
    else
    {
      int it_len = mov_equiv_it_block_length(op);
      if (it_len > 0)
      {
        /* 12 * it_len: worst case per conditioned insn is 4 code bytes plus
         * an 8-byte pool entry. */
        if (th_pool_span_after(op.size + 12 * it_len) >= THUMB_POOL_RANGE_LIMIT - THUMB_POOL_FLUSH_SLACK)
          th_literal_pool_generate();
        pool_flush_it_pending = it_len;
      }
    }
  }

  tcc_gen_machine_outline_capture(op.opcode, op.size, ind);

  /* Dry run: don't emit actual opcodes, but still track code size and
   * handle literal pool generation to ensure code addresses match real pass. */
  if (dry_run_state.active)
  {
    if (thumb_gen_state.generating_function)
    {
      thumb_gen_state.code_size += op.size;
      /* Check if literal pool needs to be generated during dry-run.
       * We need to call th_literal_pool_generate to properly track the
       * code size including the literal pool, so that ind matches
       * between dry-run and real pass. */
      if (th_pool_span_after(op.size) >= THUMB_POOL_RANGE_LIMIT - THUMB_POOL_FLUSH_SLACK && !op_in_it_block)
      {
        th_literal_pool_generate();
      }
    }
    /* Increment ind as if we emitted the instruction, but don't write to section */
    ind += op.size;
    return op.size;
  }

  if (thumb_gen_state.generating_function)
  {
    thumb_gen_state.code_size += op.size;
    if (th_pool_span_after(op.size) >= THUMB_POOL_RANGE_LIMIT - THUMB_POOL_FLUSH_SLACK && !op_in_it_block)
    {
      th_literal_pool_generate();
    }
  }

  if (op.size == 4)
    o(op.opcode >> 16);
  o(op.opcode & 0xffff);
  return op.size;
}

// TODO: this is armv7-m code
int decbranch(int pos)
{
  int xa = *(uint16_t *)(cur_text_section->data + pos);
  int xb = *(uint16_t *)(cur_text_section->data + pos + 2);

  TRACE("  decbranch ins at pos 0x%.8x, target inst 0x%x 0x%x", pos, xa, xb);

  if ((xa & 0xf000) == 0xd000)
  {
    // Branch encoding t1
    xa &= 0x00ff;
    if (xa & 0x0080)
      xa -= 0x100;
    xa = (xa * 2) + pos + 4;
  }
  else if ((xa & 0xf800) == 0xe000)
  {
    // Branch encoding t2
    xa &= 0x7ff;
    if (xa & 0x400)
      xa -= 0x800;
    xa = (xa * 2) + pos + 4;
  }
  else if ((xa & 0xf800) == 0xf000 && (xb & 0xd000) == 0x8000)
  {
    // Branch encoding t3
    uint32_t s = (xa >> 10) & 1;
    uint32_t imm6 = (xa & 0x3f);
    uint32_t j1 = (xb >> 13) & 1;
    uint32_t j2 = (xb >> 11) & 1;
    uint32_t imm11 = xb & 0x7ff;

    //      10 9876543210 9876543210 9876543210
    // IMM:             s 21bbbbbbaa aaaaaaaaa0
    // IMM:               s21bbbbbba aaaaaaaaaa
    uint32_t ret = (j2 << 19) | (j1 << 18) | (imm6 << 12) | (imm11 << 1);
    if (s)
      ret |= 0xfff00000;

    xa = ret + pos + 4;
  }
  else if ((xa & 0xf800) == 0xf000 && (xb & 0xd000) == 0x9000)
  {
    // Branch encoding t4
    uint32_t s = (xa >> 10) & 1;
    uint32_t imm10 = (xa & 0x3ff);
    uint32_t j1 = (xb >> 13) & 1;
    uint32_t j2 = (xb >> 11) & 1;
    uint32_t imm11 = xb & 0x7ff;

    uint32_t i1 = ~(j1 ^ s) & 1;
    uint32_t i2 = ~(j2 ^ s) & 1;

    //      10 9876543210 9876543210 9876543210
    // IMM:         s21bb bbbbbbbbaa aaaaaaaaa0
    uint32_t ret = (i2 << 23) | (i1 << 22) | (imm10 << 12) | (imm11 << 1);
    if (s)
      ret |= 0xff000000;

    xa = ret + pos + 4;
  }
  else if ((xa & 0xf500) == 0xb100)
  {
    /* CBZ/CBNZ encoding: offset = (i:imm5) * 2, forward only */
    uint32_t i_bit = (xa >> 9) & 1;
    uint32_t imm5 = (xa >> 3) & 0x1f;
    uint32_t imm6 = (i_bit << 5) | imm5;
    xa = (int)(imm6 * 2) + pos + 4;
  }
  else
  {
    tcc_error("internal error: decbranch unknown encoding pos 0x%x, inst: 0x%x\n", pos, xa);
    return 0;
  }

  return xa;
}
thumb_opcode th_generic_mov_imm(uint32_t r, int imm)
{
  if (imm < 0)
  {
    /* ~imm == -imm - 1 without overflowing for INT_MIN */
    return th_mvn_imm(r, 0, ~imm, flags_safe(), ENFORCE_ENCODING_NONE);
  }
  return th_mov_imm(r, imm, flags_safe(), ENFORCE_ENCODING_NONE);
}

/* Pure query for the IR-level constant hoist: true when a 32-bit constant has
 * no single-instruction materialization (all MOV/MOVW/MVN encodings fail) and
 * tcc_machine_load_constant would fall through to a literal-pool load.  Probes
 * with r0 so the 16-bit MOVS form is reachable — encodability is the same for
 * every register from there (MOV.W covers imm8 for high registers). */
ST_FUNC int tcc_gen_machine_const_needs_pool(int32_t value)
{
  return th_generic_mov_imm(0, value).size == 0;
}
ScratchRegAlloc th_offset_to_reg_ex(int off, int sign, uint32_t exclude_regs)
{
  /* Find a free scratch register (must not clobber excluded regs).
   * Returns ScratchRegAlloc struct so caller can manage cleanup.
   * Caller MUST call restore_scratch_reg() when done with the register. */
  ScratchRegAlloc alloc = get_scratch_reg_with_save(exclude_regs);
  int rr = alloc.reg;

  /* If mov is not possible then load from data */
  if (!ot(th_generic_mov_imm(rr, off)))
  {
    load_full_const(rr, PREG_NONE, LFC_SPLIT(sign ? -off : off));
    return alloc;
  }

  if (sign)
    ot_check(th_rsb_imm(rr, rr, 0, flags_safe(), ENFORCE_ENCODING_NONE));
  return alloc;
}

int th_patch_call(int t, int a)
{
  uint16_t *x = (uint16_t *)(cur_text_section->data + t);
  int lt = t;

  TRACE("'th_patch_call' t: %.8x, a: %.8x\n", t, a);

  t = decbranch(t);
  TRACE("t: %.8x\n", t);
  if (a == lt + 2)
    *x = 0xbf00;
  else if ((*x & 0xf000) == 0xd000)
  {
    *x &= 0xff00;
    *x |= th_encbranch_8(lt, a);
  }
  else if ((*x & 0xf800) == 0xe000)
  {
    *x &= 0xf800;
    *x |= th_encbranch_11(lt, a);
  }
  else if ((x[0] & 0xf800) == 0xf000 && (x[1] & 0xd000) == 0x8000)
  {
    uint32_t enc = 0;
    x[0] &= 0xfbc0;
    x[1] &= 0xd000;
    enc = th_encbranch_b_t3(th_encbranch_20(lt, a));
    x[0] |= enc >> 16;
    x[1] |= enc;
  }
  else if ((x[0] & 0xf800) == 0xf000 && (x[1] & 0xd000) == 0x9000)
  {
    uint32_t enc = 0;
    x[0] &= 0xf800;
    x[1] &= 0xd000;
    enc = th_packimm_10_11_0(th_encbranch_20(lt, a) << 1);
    x[0] |= enc >> 16;
    x[1] |= enc;
  }
  else if ((*x & 0xf500) == 0xb100)
  {
    /* CBZ/CBNZ: 16-bit, forward-only, range 0-126 bytes.
     * CBZ base = 0xb100, CBNZ base = 0xb900; both match (x & 0xf500) == 0xb100
     * since bit 11 (0x0800) is not in the mask.
     * Encoding: op | (i << 9) | (imm5 << 3) | Rn
     * where offset = (i:imm5) * 2 */
    int offset = a - (lt + 4); /* PC-relative, Thumb PC = insn + 4 */
    if (offset < 0 || offset > 126 || (offset & 1))
      tcc_ice("CBZ/CBNZ target out of range: offset=%d in %s", offset, funcname ? funcname : "?");
    uint32_t imm6 = (uint32_t)offset >> 1;
    uint32_t i_bit = (imm6 >> 5) & 1;
    uint32_t imm5 = imm6 & 0x1f;
    *x &= 0xfd07; /* Keep base opcode, NZ bit, and Rn */
    *x |= (uint16_t)((i_bit << 9) | (imm5 << 3));
  }
  else
    tcc_ice("unhandled branch type in th_patch_call for: t: "
              "0x%x, a: 0x%x, x: 0x%x 0x%x\n",
              t, a, x[0], x[1]);

  return t;
}

/* Add a value to SP.  When the immediate doesn't fit the ADD/SUB SP encoding,
 * a scratch register is needed.  scratch_reg selects which one:
 *   >= 0  : use that specific physical register (caller guarantees it's free)
 *   < 0   : default to R_IP (safe in prologue/epilogue where R0-R3 hold args)
 */
void gadd_sp_ex(int val, int scratch_reg)
{
  if (val == 0)
    return;

  if (scratch_reg < 0)
    scratch_reg = R_IP;

  if (val > 0)
  {
    thumb_opcode add_imm = th_add_imm(R_SP, R_SP, (uint32_t)val, flags_safe(), ENFORCE_ENCODING_NONE);
    if (is_valid_opcode(add_imm))
    {
      ot(add_imm);
      return;
    }

    load_full_const(scratch_reg, PREG_NONE, (uint32_t)val, 0);
    ot_check(
        th_add_reg(R_SP, R_SP, scratch_reg, flags_safe(), THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
    return;
  }

  /* val < 0 */
  const uint32_t sub = (uint32_t)(-val);
  thumb_opcode sub_imm = th_sub_imm(R_SP, R_SP, sub, flags_safe(), ENFORCE_ENCODING_NONE);
  if (is_valid_opcode(sub_imm))
  {
    ot(sub_imm);
    return;
  }

  load_full_const(scratch_reg, PREG_NONE, (uint32_t)sub, 0);
  ot_check(th_sub_reg(R_SP, R_SP, scratch_reg, flags_safe(), THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
}
void gadd_sp(int val)
{
  gadd_sp_ex(val, -1);
}

void ggoto(void)
{
  TRACE("'ggoto'");
  {
    SValue target = *vtop;
    tcc_ir_put(tcc_state->ir, TCCIR_OP_IJUMP, &target, NULL, NULL);
  }
  vtop--;
  print_vstack("ggoto");
}

ST_FUNC void tcc_gen_machine_indirect_jump_mop(MachineOperand src, TccIrOp op)
{
  (void)op;
  MachineCodegenContext ctx = {0};
  int target = mach_ensure_in_reg(&ctx, &src, 1u << R_IP);
  if (ctx.n_scratch > 0)
  {
    /* Loading the target took scratch registers, possibly pushed to free them:
     * their restore must come BEFORE the branch (after it, it never runs and
     * the stack stays unbalanced -- -O0 computed goto returned through a
     * stray word).  Branch through IP, which no scratch restore touches. */
    if (target != R_IP)
      ot_check_mov_reg(R_IP, (uint32_t)target, flags_safe(), THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE, false);
    mach_release_all(&ctx);
    target = R_IP;
  }
  ot_check(th_bx_reg((uint16_t)target));
}

/* Returns the number of bytes emitted by tcc_gen_machine_switch_table_mop for
 * a table with the given number of entries.  Used by the dry-run pass in
 * codegen.c so that branch-offset analysis is accurate without the backend
 * having to emit any real instructions. */
ST_FUNC int tcc_gen_machine_switch_table_dry_run_size(int num_entries)
{
  /* Layout: LSL.W(4) + ADD(2) + LDR.W(4) + ADD(2) + BX(2) = 14 bytes preamble
   * + 4 bytes per table entry (32-bit signed PC-relative offsets). */
  return 14 + num_entries * 4;
}

/* Force any pending literal pool to be flushed before a region of
 * `upcoming_bytes` is emitted, if leaving the pool pending that long would
 * push its load out of range.  Public wrapper so codegen.c can reserve
 * space symmetrically in both the dry-run and real-run passes.
 *
 * The SWITCH_TABLE dispatch needs this: its preamble (LSL/ADD/LDR/ADD/BX)
 * must be emitted atomically — a literal-pool flush in the middle relocates
 * the terminal `ADD Rt, PC; BX Rt` past the pool (bridged by a B.W), which
 * invalidates the `ref_point == table_start` assumption that the switch-
 * table offset backpatch in codegen.c relies on, producing a wild jump.
 * Flushing the pool up front (in both passes, so dry-run size estimates and
 * real-run addresses stay consistent) keeps the preamble + table contiguous. */
ST_FUNC void tcc_gen_machine_reserve_pool_bytes(int upcoming_bytes)
{
  th_literal_pool_reserve_upcoming_bytes(upcoming_bytes);
}

/* MOP variant: accepts a MachineOperand for the index register. */
ST_FUNC void tcc_gen_machine_switch_table_mop(MachineOperand src, TCCIRSwitchTable *table, TCCIRState *ir, int ir_idx)
{
  (void)ir_idx;

  TRACE("'tcc_gen_machine_switch_table_mop' table_id=%d entries=%d\n", table - ir->switch_tables, table->num_entries);

  MachineCodegenContext ctx = {0};
  /* The index value must be in a register at this point. */
  int index_reg = mach_ensure_in_reg(&ctx, &src, 0);
  if (!thumb_is_hw_reg(index_reg))
    tcc_error("internal error: SWITCH_TABLE index not in a hardware register (mop)");

  /* Use R_IP as scratch to avoid clobbering index_reg, which may still be
     live at the switch targets (SSA can place the loop counter directly here). */
  int rt = R_IP;

  ot_check(th_lsl_imm(rt, index_reg, 2, flags_safe(), ENFORCE_ENCODING_32BIT));
  ot_check(th_add_reg(rt, rt, R_PC, flags_safe(), THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
  ot_check_ldr_imm(rt, rt, 6, 6, ENFORCE_ENCODING_32BIT);
  ot_check(th_add_reg(rt, rt, R_PC, flags_safe(), THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
  ot_check(th_bx_reg(rt));

  int table_start = ind;
  /* The offset table is data in .text — mark it for disassemblers. */
  map_sym_d();
  for (int i = 0; i < table->num_entries; i++)
  {
    g(0);
    g(0);
    g(0);
    g(0);
  }
  map_sym_t();
  table->table_code_addr = table_start;
  mach_release_all(&ctx);
}

/* SWITCH_LOAD: data-table dispatch that loads values[index] into dest.
 *
 * Layout (uniform 14-byte preamble):
 *
 *   LSL.W rt, index, #2          (4 bytes)
 *   ADD   rt, rt, pc             (2 bytes)         ; PC=preamble_start+8
 *   LDR.W ip,  [rt, #6]          (4 bytes)         ; load table[index] -> ip
 *   B.W   skip                   (4 bytes)         ; jump past the table
 *   <table data>                 (4*N bytes)
 *   skip:
 *   [optional STR/MOV ip -> dest]                  ; only if dest is spilled,
 *                                                  ;   emitted by the IR-level
 *                                                  ;   ASSIGN that follows.
 *
 * The fixed loaded register is R_IP (same as SWITCH_TABLE's scratch); the
 * IR-level optimization wraps SWITCH_LOAD with an ASSIGN that places IP into
 * the real dest, so we don't need a separate spill path here.
 *
 * SYMREF entries emit R_ARM_ABS32 relocations at their table slots; the
 * linker fills in the absolute symbol address.
 */
/* SWITCH_LOAD dispatch size: literal-pool LDR (4 bytes, T2 encoding for
 * R_IP) + indexed shifted LDR.W (4 bytes).  The table itself lives in
 * .rodata and contributes no .text bytes. */
ST_FUNC int tcc_gen_machine_switch_load_dry_run_size(int num_entries)
{
  (void)num_entries;
  return 8;
}

ST_FUNC void tcc_gen_machine_switch_load_mop(MachineOperand src, MachineOperand dest, TCCIRSwitchValueTable *vtab,
                                             TCCIRState *ir, int ir_idx)
{
  (void)ir_idx;
  (void)ir;

  TRACE("'tcc_gen_machine_switch_load_mop' vt_id=%d entries=%d\n", (int)(vtab - ir->switch_value_tables),
        vtab->num_entries);

  if (!vtab->rodata_sym)
    tcc_error("internal error: SWITCH_LOAD table has no rodata symbol (switch_to_data should have allocated it)");

  MachineCodegenContext ctx = {0};
  /* Keep the index out of R_IP, which we clobber with the table base below. */
  int index_reg = mach_ensure_in_reg(&ctx, &src, (1u << (uint32_t)R_IP));
  if (!thumb_is_hw_reg(index_reg))
    tcc_error("internal error: SWITCH_LOAD index not in a hardware register");

  /* Resolve the destination register.  The switch_to_data optimization tries to
   * keep the SWITCH_LOAD dest in a hardware register, but under high register
   * pressure the allocator can spill it (or it may be an lvalue store).  Rather
   * than bail out, allocate a scratch via mach_get_dest_reg() and store it back
   * with mach_writeback_dest() afterwards.  Exclude index_reg and R_IP — both
   * are read by the indexed load below. */
  uint32_t dest_excl = (1u << (uint32_t)index_reg) | (1u << (uint32_t)R_IP);
  int dest_reg = mach_get_dest_reg(&ctx, &dest, dest_excl);

  /* Load the table's base address from the literal pool into IP. */
  _lfc_sym = vtab->rodata_sym;
  load_full_const(R_IP, PREG_NONE, 0, 0);

  /* dest = table[index] via LDR.W dest, [ip, index, LSL #2]. */
  thumb_shift shift = {THUMB_SHIFT_LSL, 2, THUMB_SHIFT_IMMEDIATE};
  ot_check(th_ldr_reg((uint32_t)dest_reg, (uint32_t)R_IP, (uint32_t)index_reg, shift, ENFORCE_ENCODING_32BIT));

  /* If the dest was a spill slot or lvalue, write the loaded value back. */
  mach_writeback_dest(&dest, dest_reg);

  mach_release_all(&ctx);
}

void gsym_addr(int t, int a)
{
  TRACE("'gsym_addr' %.8x branch target: %.8x\n", t, a);

  while (t > 0) /* -1 or 0 means end of chain / no chain */
    t = th_patch_call(t, a);
}

ST_FUNC void gen_vla_alloc(CType *type, int align)
{
  /* vtop holds the allocation size in bytes. Adjust SP down by that runtime
   * size and align it to at least 8 bytes.
   *
   * This follows the classic TCC scheme:
   *   r = sp - size
   *   r = r & ~(align-1)
   *   sp = r
   *
   * The size expression is consumed from the value stack.
   */
  (void)type;

  int r = gv(RC_INT);

  /* r = SP - r */
  ot_check(th_sub_reg(r, R_SP, r, flags_safe(), THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));

  if (align < 8)
    align = 8;
  if (align & (align - 1))
    tcc_error("alignment is not a power of 2: %i", align);

  if (align > 1)
  {
    /* Try immediate BIC first; if it doesn't encode, fall back to register mask. */
    if (!ot(th_bic_imm(r, r, (uint32_t)(align - 1), flags_safe(), ENFORCE_ENCODING_NONE)))
    {
      ScratchRegAlloc mask_alloc = get_scratch_reg_with_save(1u << r);
      int mask_reg = mask_alloc.reg;
      if (!ot(th_generic_mov_imm(mask_reg, align - 1)))
      {
        load_full_const(mask_reg, PREG_NONE, LFC_SPLIT(align - 1));
      }
      ot_check(th_bic_reg(r, r, mask_reg, flags_safe(), THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
      if (mask_alloc.saved)
      {
        ot_check(th_pop(1u << mask_reg));
      }
    }
  }

  /* SP = r */
  ot_check_mov_reg(R_SP, r, flags_safe(), THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE, false);

  vpop();
}

ST_FUNC void gen_vla_sp_save(int addr)
{
  if (nocode_wanted)
    return;

  /* Store SP to the local stack slot at frame offset `addr`. */
  int off = fp_adjust_local_offset(addr, 0 /* not param */);
  int sign = (off < 0) ? 1 : 0;
  int abs_off = sign ? -off : off;
  const int base_reg = tcc_state->need_frame_pointer ? R_FP : R_SP;

  ScratchRegAlloc vla_sc = get_scratch_reg_with_save(0);
  ot_check_mov_reg(vla_sc.reg, R_SP, flags_safe(), THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE, false);
  th_store32_imm_or_reg_ex(vla_sc.reg, base_reg, abs_off, sign, 0);
  restore_scratch_reg(&vla_sc);
}

ST_FUNC void gen_vla_sp_restore(int addr)
{
  if (nocode_wanted)
    return;

  /* Load SP from the local stack slot at frame offset `addr`. */
  int off = fp_adjust_local_offset(addr, 0 /* not param */);
  int sign = (off < 0) ? 1 : 0;
  int abs_off = sign ? -off : off;
  const int base_reg = tcc_state->need_frame_pointer ? R_FP : R_SP;

  ScratchRegAlloc vla_sc = get_scratch_reg_with_save(0);
  load_from_base(vla_sc.reg, PREG_REG_NONE, IROP_BTYPE_INT32, 0, abs_off, sign, base_reg);
  ot_check_mov_reg(R_SP, vla_sc.reg, flags_safe(), THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE, false);
  restore_scratch_reg(&vla_sc);
}

int load_ushort_from_base(int ir, int base, int fc, int sign)
{
  const thumb_opcode ins = th_ldrh_imm(ir, base, fc, sign ? 4 : 6, ENFORCE_ENCODING_NONE);
  TRACE("Load ushort sign: %d, r %d, base: %d, fc: %d\n", sign, ir, base, fc, sign);
  return ot(ins);
}

int load_byte_from_base(int ir, int base, int fc, int sign)
{
  const thumb_opcode ins = th_ldrsb_imm(ir, base, fc, sign ? 4 : 6, ENFORCE_ENCODING_NONE);
  TRACE("Load byte sign: %d, r %d, base: %d, fc: %d\n", sign, ir, base, fc, sign);
  return ot(ins);
}

int load_ubyte_from_base(int ir, int base, int fc, int sign)
{
  const thumb_opcode ins = th_ldrb_imm(ir, base, fc, sign ? 4 : 6, ENFORCE_ENCODING_NONE);
  TRACE("Load ubyte sign: %d, r %d, base: %d, fc: %d\n", sign, ir, base, fc, sign);
  return ot(ins);
}

/* Knob for frame_word_pair_rewind: TCC_DISABLE_PASS=codegen:frame_word_pair.
 * Off in a function with a volatile access, like the other memory caches. */
static int frame_word_pair_off;

ST_FUNC void tcc_gen_machine_frame_word_pair_set_enabled(int enabled)
{
  frame_word_pair_off = !enabled;
  frame_word_last.kind = 0;
}

/* Two word accesses to neighbouring frame words, one right after the other --
 * `ldr r7,[sp,#12]; ldr.w r8,[sp,#16]`, 60k pairs in the Zig compiler built as
 * C -- are one LDRD/STRD.  The word helpers are asked for one word at a time,
 * so the pair is formed after the fact: when this access is the word next to
 * the previous one, which was a single immediate-offset LDR/STR of the same
 * kind and the last thing emitted, rewind over it and emit both as one
 * instruction.  Every pass takes the same decision from the same `ind`
 * arithmetic, so the dry runs lay out what the real pass emits.
 *
 * Only frame bases (frame_word_base): LDRD/STRD fault on an address that is
 * not word aligned, which only a frame slot at a multiple-of-4 offset
 * guarantees.  Nothing else
 * can have captured the rewound bytes: the word helpers record no relocation,
 * literal or branch there, and every label resets the pending access (see
 * tcc_gen_machine_strldr_cache_reset).  LDRD may not load one register twice;
 * STRD may store it.  Returns 1 when the pair was emitted. */
static int frame_word_pair_rewind(int is_load, int reg, int base, int off)
{
  if (frame_word_pair_off || !thumb_gen_state.generating_function || nocode_wanted)
    return 0;
  if (frame_word_last.kind != (is_load ? 2 : 1) || frame_word_last.end != ind || frame_word_last.base != base)
    return 0;
  if (mov_equiv_it_pending != 0 || pool_flush_it_pending != 0)
    return 0;
  const int prev_reg = frame_word_last.reg, prev_off = frame_word_last.off;
  if (prev_off + 4 != off && off + 4 != prev_off)
    return 0;
  if (is_load && prev_reg == reg)
    return 0;
  const int lo_reg = prev_off < off ? prev_reg : reg;
  const int hi_reg = prev_off < off ? reg : prev_reg;
  const int lo_off = prev_off < off ? prev_off : off;
  if (lo_reg < 0 || lo_reg >= R_SP || hi_reg < 0 || hi_reg >= R_SP || lo_reg == base || hi_reg == base)
    return 0;
  const int neg = (lo_off < 0);
  const int abs_off = neg ? -lo_off : lo_off;
  if ((abs_off & 3) != 0 || abs_off > 1020)
    return 0;
  const uint32_t puw = neg ? 4u : 6u;
  const thumb_opcode pair = is_load ? th_ldrd_imm((uint32_t)lo_reg, (uint32_t)hi_reg, (uint32_t)base, abs_off, puw)
                                    : th_strd_imm((uint32_t)lo_reg, (uint32_t)hi_reg, (uint32_t)base, abs_off, puw);
  if (pair.size != 4)
    return 0;
  thumb_gen_state.code_size -= ind - frame_word_last.start;
  ind = frame_word_last.start;
  frame_word_last.kind = 0;
  ot_check(pair);
  return 1;
}

/* A base register whose word-aligned offsets are word-aligned addresses: SP,
 * or R7 while it is the frame pointer.  Without a frame pointer R7 is an
 * ordinary register holding any pointer -- a packed field's address, say --
 * and an LDRD through it took an UNALIGNED usage fault in the native tcc. */
static int frame_word_base(int base)
{
  return base == R_SP || (base == R_FP && tcc_state->need_frame_pointer);
}

/* Remember a word access to the frame that was emitted as exactly `ins`. */
static void frame_word_note(int is_load, int start, thumb_opcode ins, int reg, int base, int fc, int sign)
{
  frame_word_last.kind = 0;
  if (!frame_word_base(base) || ins.size == 0 || ind - start != ins.size)
    return;
  frame_word_last.kind = is_load ? 2 : 1;
  frame_word_last.start = start;
  frame_word_last.end = ind;
  frame_word_last.reg = reg;
  frame_word_last.base = base;
  frame_word_last.off = sign ? -fc : fc;
}

int load_word_from_base(int ir, int base, int fc, int sign)
{
  const thumb_opcode ins = th_ldr_imm(ir, base, fc, sign ? 4 : 6, ENFORCE_ENCODING_NONE);
  TRACE("Load word sign: %d, r %d, base: %d, fc: %d\n", sign, ir, base, fc, sign);
  /* This is the path every spill reload takes, so it -- not just the
   * ot_check_ldr_imm call sites -- is where the memory cache has to be
   * consulted.  Returning the encoded size without emitting keeps the
   * caller's "did it encode?" contract intact; only `ind` stands still. */
  if (ins.size != 0 && thumb_gen_state.generating_function && !sign &&
      strldr_cache_try_match_ldr(ir, base, fc, 6, ins.size, 4))
    return ins.size;
  /* Satisfying the load from ANOTHER register holding the slot (`mov rt,rh`)
   * was built and MEASURED on the rig, and it is a small LOSS -- rijndael and
   * double_mul improve, double_add/cmp/mixed/div all give more back, total
   * -0.09% against -0.12% for this same-register form alone.  A `mov` and a
   * cached-line `ldr` from the frame cost about the same on M33, so all it
   * really does is move code.  Do not re-propose without a measurement. */
  if (ins.size != 0 && frame_word_base(base) && frame_word_pair_rewind(1, ir, base, sign ? -fc : fc))
    return ins.size;
  const int start = ind;
  const int size = ot(ins);
  frame_word_note(1, start, ins, ir, base, fc, sign);
  return size;
}

int store_word_to_base(int ir, int base, int fc, int sign)
{
  const thumb_opcode ins = th_str_imm(ir, base, fc, sign ? 4 : 6, ENFORCE_ENCODING_NONE);
  TRACE("Store word sign: %d, r %d, base: %d, fc: %d\n", sign, ir, base, fc, sign);
  if (!sign && strldr_cache_str_is_redundant(ir, base, fc, 6, ins.size))
    return ins.size;
  if (ins.size != 0 && frame_word_base(base) && frame_word_pair_rewind(0, ir, base, sign ? -fc : fc))
    return ins.size;
  const int start = ind;
  const int size = ot(ins);
  frame_word_note(0, start, ins, ir, base, fc, sign);
  return size;
}

/* Returns 1 if a 64-bit access at (sym + addend) is guaranteed 4-byte aligned
 * (so LDRD/STRD is safe).  Conservative: only allows natural alignment for
 * non-struct, non-packed symbols, plus any explicit alignment >= 4. */
int sym_is_4_byte_aligned_for_64bit(Sym *sym, int32_t addend)
{
  if (!sym)
    return 0;
  if ((addend & 3) != 0)
    return 0;
  if (sym->a.packed)
    return 0;
  if (sym->a.aligned >= 3) /* explicit alignment 2^(n-1) >= 4 */
    return 1;
  if (sym->a.aligned > 0) /* explicit 1 or 2 byte alignment — not safe */
    return 0;
  /* sym->a.aligned == 0: rely on the declared type's natural alignment.
   * Structs/unions may be packed-wrapped; reject conservatively.  Native
   * scalars (long long, double, pointer) have natural alignment >= 4. */
  int btype = sym->type.t & VT_BTYPE;
  if (btype == VT_STRUCT)
    return 0;
  return 1;
}

/* Try to emit STRD Rt, Rt2, [base, #±abs_off] for a 64-bit paired store.
 * Constraints (Thumb-2 STRD imm T1):
 *   - Rt != Rt2
 *   - Rt, Rt2 in r0..r12 or r14 (not SP, not PC)
 *   - abs_off 4-byte aligned and <= 1020
 * Returns 1 on success, 0 if the caller must fall back to two 32-bit stores. */
int try_strd_pair(int lo_reg, int hi_reg, int base, int abs_off, int sign)
{
  if ((unsigned)base > 15)
    return 0;
  if ((abs_off & 3) != 0 || abs_off > 1020)
    return 0;
  if (lo_reg < 0 || lo_reg > R_LR || lo_reg == R_SP)
    return 0;
  if (hi_reg < 0 || hi_reg > R_LR || hi_reg == R_SP)
    return 0;
  const uint32_t puw = sign ? 4u : 6u;
  ot_check(th_strd_imm((uint32_t)lo_reg, (uint32_t)hi_reg, (uint32_t)base, abs_off, puw));
  return 1;
}

/* Mirror of try_strd_pair for LDRD.  Same register and offset constraints;
 * the caller is responsible for guaranteeing 4-byte alignment of the target
 * address (stack, or a symbol that passes sym_is_4_byte_aligned_for_64bit). */
int try_ldrd_pair(int lo_reg, int hi_reg, int base, int abs_off, int sign)
{
  if ((abs_off & 3) != 0 || abs_off > 1020)
    return 0;
  if (lo_reg < 0 || lo_reg > R_LR || lo_reg == R_SP)
    return 0;
  if (hi_reg < 0 || hi_reg > R_LR || hi_reg == R_SP)
    return 0;
  if (lo_reg == hi_reg)
    return 0;
  const uint32_t puw = sign ? 4u : 6u;
  if (strldr_cache_ldrd_is_redundant(lo_reg, hi_reg, base, abs_off, puw))
    return 1;
  ot_check(th_ldrd_imm((uint32_t)lo_reg, (uint32_t)hi_reg, (uint32_t)base, abs_off, puw));
  return 1;
}

/* Emit a single STR to a spill slot. Used by the codegen STRD pairing logic
 * to flush a pending store when pairing wasn't possible. */
ST_FUNC void tcc_gen_machine_store_spill(int src_reg, int32_t spill_offset)
{
  const int base_reg = tcc_state->need_frame_pointer ? R_FP : R_SP;
  int adj = fp_adjust_local_offset(spill_offset, 0);
  int sign = (adj < 0);
  int abs_off = sign ? -adj : adj;
  ot_check_str_imm((uint32_t)src_reg, (uint32_t)base_reg,
                   abs_off, sign ? 4u : 6u, ENFORCE_ENCODING_NONE);
}

/* Try to emit STRD for two 32-bit values to adjacent spill slots.
 * off1 must be the lower offset (off1 + 4 == off2).
 * Returns 1 on success, 0 if STRD constraints not met. */
ST_FUNC int tcc_gen_machine_try_strd_spill(int reg1, int32_t off1, int reg2, int32_t off2)
{
  if (off1 + 4 != off2)
    return 0;
  const int base_reg = tcc_state->need_frame_pointer ? R_FP : R_SP;
  int adj = fp_adjust_local_offset(off1, 0);
  int sign = (adj < 0);
  int abs_off = sign ? -adj : adj;
  return try_strd_pair(reg1, reg2, base_reg, abs_off, sign);
}

/* Try to emit LDRD for two 32-bit values from adjacent spill slots.
 * off1 must be the lower offset (off1 + 4 == off2).
 * Returns 1 on success, 0 if LDRD constraints not met. */
ST_FUNC int tcc_gen_machine_try_ldrd_spill(int reg1, int32_t off1, int reg2, int32_t off2)
{
  if (off1 + 4 != off2)
    return 0;
  const int base_reg = tcc_state->need_frame_pointer ? R_FP : R_SP;
  int adj = fp_adjust_local_offset(off1, 0);
  int sign = (adj < 0);
  int abs_off = sign ? -adj : adj;
  return try_ldrd_pair(reg1, reg2, base_reg, abs_off, sign);
}

/* Try to emit LDRD/STRD for two 32-bit values from adjacent offsets off a
 * generic base register (not FP/SP).  Used by the LOAD_INDEXED/STORE_INDEXED
 * pairing peephole.  `off` is the lower offset (caller has verified
 * off + 4 fits within the same access range).  Returns 1 on success. */
ST_FUNC int tcc_gen_machine_try_ldrd_base(int reg1, int reg2, int base_reg, int32_t off)
{
  int sign = (off < 0);
  int abs_off = sign ? -off : off;
  return try_ldrd_pair(reg1, reg2, base_reg, abs_off, sign);
}

ST_FUNC int tcc_gen_machine_try_strd_base(int reg1, int reg2, int base_reg, int32_t off)
{
  int sign = (off < 0);
  int abs_off = sign ? -off : off;
  return try_strd_pair(reg1, reg2, base_reg, abs_off, sign);
}

ST_FUNC int tcc_gen_machine_try_strd_imm_spill(int64_t val1, int64_t val2,
                                               int32_t off1, int32_t off2)
{
  if (off1 + 4 != off2)
    return 0;
  const int base_reg = tcc_state->need_frame_pointer ? R_FP : R_SP;
  int adj = fp_adjust_local_offset(off1, 0);
  int sign = (adj < 0);
  int abs_off = sign ? -adj : adj;
  if ((abs_off & 3) != 0 || abs_off > 1020)
    return 0;

  MachineCodegenContext ctx = {0};
  /* Materializing the immediates may PUSH the scratch register(s) when FP is
   * omitted and no scratch-save area is reserved, lowering SP by 4 per push.
   * The STRD destination is SP-relative, so an uncompensated offset would write
   * the pair 4*pushes bytes below the intended slot — the array/struct
   * initializer then lands at the wrong offset and later reads return stale
   * data (fuzz seed 12057).  Snapshot the push stack so we can measure the SP
   * shift after acquiring the registers and fold it into the offset. */
  int spc_before = scratch_push_count;
  MachineOperand op1 = {.kind = MACH_OP_IMM, .u.imm.val = val1};
  int r1 = mach_ensure_in_reg(&ctx, &op1, 0);
  int r2;
  if (val1 == val2) {
    r2 = r1;
  } else {
    MachineOperand op2 = {.kind = MACH_OP_IMM, .u.imm.val = val2};
    r2 = mach_ensure_in_reg(&ctx, &op2, (1u << (uint32_t)r1));
  }
  if (r1 == R_SP || r2 == R_SP) {
    mach_release_all(&ctx);
    return 0;
  }
  /* Account for any real SP-lowering pushes (type 1) done above.  Saves routed
   * to a reserved scratch area (type 2) keep SP stable and need no adjustment.
   * The shift only affects an SP-relative base; an FP base is unperturbed. */
  if (base_reg == R_SP) {
    int sp_shift = 0;
    for (int s = spc_before; s < scratch_push_count && s < 128; s++)
      if (scratch_push_type[s] == 1)
        sp_shift += 4;
    if (sp_shift) {
      /* Only the positive (above-SP) local case is safe to compensate by simple
       * addition; a negative (below-SP) offset combined with the shift is rare
       * and not worth special-casing — fall back to per-element stores. */
      if (sign || abs_off + sp_shift > 1020) {
        mach_release_all(&ctx);
        return 0;
      }
      abs_off += sp_shift;
    }
  }
  const uint32_t puw = sign ? 4u : 6u;
  ot_check(th_strd_imm((uint32_t)r1, (uint32_t)r2, (uint32_t)base_reg, abs_off, puw));
  mach_release_all(&ctx);
  return 1;
}

ST_FUNC int tcc_gen_machine_try_strd_imm_base(int64_t val1, int64_t val2,
                                              int base_reg, int32_t off)
{
  int sign = (off < 0);
  int abs_off = sign ? -off : off;
  if ((unsigned)base_reg > 15)
    return 0;
  if ((abs_off & 3) != 0 || abs_off > 1020)
    return 0;

  uint32_t excl = (1u << (uint32_t)base_reg);
  MachineCodegenContext ctx = {0};
  MachineOperand op1 = {.kind = MACH_OP_IMM, .u.imm.val = val1};
  int r1 = mach_ensure_in_reg(&ctx, &op1, excl);
  int r2;
  if (val1 == val2) {
    r2 = r1;
  } else {
    MachineOperand op2 = {.kind = MACH_OP_IMM, .u.imm.val = val2};
    r2 = mach_ensure_in_reg(&ctx, &op2, excl | (1u << (uint32_t)r1));
  }
  if (r1 == R_SP || r2 == R_SP) {
    mach_release_all(&ctx);
    return 0;
  }
  const uint32_t puw = sign ? 4u : 6u;
  ot_check(th_strd_imm((uint32_t)r1, (uint32_t)r2, (uint32_t)base_reg, abs_off, puw));
  mach_release_all(&ctx);
  return 1;
}

ST_FUNC int tcc_machine_can_encode_stack_offset_for_reg(int frame_offset, int dest_reg)
{
  /* Check if frame_offset can be directly encoded in ldr/str instructions
   * without requiring a scratch register. This is used to avoid wasteful
   * address materialization when the backend can handle the offset directly.
   * Tests with dest_reg since encoding availability depends on the register. */
  /* Adjust for callee-saved gap below FP (spill offsets are always locals) */
  frame_offset = fp_adjust_local_offset(frame_offset, 0);
  const int base_reg = tcc_state->need_frame_pointer ? R_FP : R_SP;
  const int sign = (frame_offset < 0);
  const int abs_offset = sign ? -frame_offset : frame_offset;

  /* Try to encode as ldr instruction with the actual destination register.
   * Some encodings (e.g., Thumb-1 T1) only work with low registers (r0-r7). */
  const thumb_opcode ins = th_ldr_imm(dest_reg, base_reg, abs_offset, sign ? 4 : 6, ENFORCE_ENCODING_NONE);
  return (ins.size != 0);
}

ST_FUNC int tcc_machine_can_encode_stack_offset_with_param_adj(int frame_offset, int is_param, int dest_reg)
{
  /* Like tcc_machine_can_encode_stack_offset_for_reg, but applies offset_to_args for VT_PARAM.
   * Stack parameters need offset_to_args adjustment (prologue push size). */
  int offset = frame_offset;
  if (is_param)
    offset = param_frame_offset(offset);
  return tcc_machine_can_encode_stack_offset_for_reg(offset, dest_reg);
}

ST_FUNC void tcc_machine_load_spill_slot(int dest_reg, int frame_offset)
{
  if (dest_reg == PREG_REG_NONE)
    tcc_ice("load_spill_slot requires a destination register");

  /* Adjust for callee-saved gap below FP (spill slots are always locals) */
  frame_offset = fp_adjust_local_offset(frame_offset, 0);

  /* Peephole: if the previous emit was a STR or LDR of the same register to/from
   * the same slot AND no other instruction has been emitted since, the value is
   * already in dest_reg — skip the redundant load. */
  TCCIRState *ir = tcc_state ? tcc_state->ir : NULL;
  if (ir && ir->spill_cache.last_emit_kind != 0 &&
      ir->spill_cache.last_emit_ind == ind &&
      ir->spill_cache.last_emit_reg == dest_reg &&
      ir->spill_cache.last_emit_offset == frame_offset)
  {
    return;
  }

  const int base_reg = tcc_state->need_frame_pointer ? R_FP : R_SP;
  const int sign = (frame_offset < 0);
  const int abs_offset = sign ? -frame_offset : frame_offset;
  if (!load_word_from_base(dest_reg, base_reg, abs_offset, sign))
  {
    ScratchRegAlloc rr_alloc = th_offset_to_reg_ex(abs_offset, sign, (1u << dest_reg) | (1u << base_reg));
    int rr = rr_alloc.reg;
    ot_check(th_ldr_reg(dest_reg, base_reg, rr, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
    restore_scratch_reg(&rr_alloc);
  }

  if (ir)
  {
    ir->spill_cache.last_emit_kind = 2; /* LDR */
    ir->spill_cache.last_emit_ind = ind;
    ir->spill_cache.last_emit_reg = (int8_t)dest_reg;
    ir->spill_cache.last_emit_offset = frame_offset;
  }
}

ST_FUNC void tcc_machine_store_spill_slot(int src_reg, int frame_offset)
{
  if (src_reg == PREG_REG_NONE)
    tcc_ice("store_spill_slot requires a source register");

  /* Adjust for callee-saved gap below FP (spill slots are always locals) */
  frame_offset = fp_adjust_local_offset(frame_offset, 0);
  const int base_reg = tcc_state->need_frame_pointer ? R_FP : R_SP;
  const int sign = (frame_offset < 0);
  const int abs_offset = sign ? -frame_offset : frame_offset;
  if (!store_word_to_base(src_reg, base_reg, abs_offset, sign))
  {
    /* Avoid clobbering the other half of a 64-bit value when storing
     * paired registers. The allocator uses adjacent register pairs for
     * 64-bit values (e.g. r0/r1, r2/r3, r4/r5). When storing one half,
     * do not use the adjacent register as the scratch offset register.
     */
    uint32_t extra_exclude = 0;
    if (src_reg >= ARM_R0 && src_reg <= ARM_R12)
    {
      int adj = (src_reg & 1) ? (src_reg - 1) : (src_reg + 1);
      if (adj >= ARM_R0 && adj <= ARM_R12 && adj != ARM_SP && adj != ARM_PC)
        extra_exclude |= (1u << adj);
    }

    ScratchRegAlloc rr_alloc =
        th_offset_to_reg_ex(abs_offset, sign, (1u << src_reg) | (1u << base_reg) | extra_exclude);
    int rr = rr_alloc.reg;
    ot_check(th_str_reg(src_reg, base_reg, rr, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
    restore_scratch_reg(&rr_alloc);
  }

  TCCIRState *ir = tcc_state ? tcc_state->ir : NULL;
  if (ir)
  {
    ir->spill_cache.last_emit_kind = 1; /* STR */
    ir->spill_cache.last_emit_ind = ind;
    ir->spill_cache.last_emit_reg = (int8_t)src_reg;
    ir->spill_cache.last_emit_offset = frame_offset;
  }
}

/* Like tcc_machine_store_spill_slot, but for stack-passed parameters.
 * Adds offset_to_args (prologue push size) to the frame offset so that
 * the store targets the correct caller-stack location above FP. */
ST_FUNC void tcc_machine_store_param_slot(int src_reg, int frame_offset)
{
  tcc_machine_store_spill_slot(src_reg, param_frame_offset(frame_offset));
}

static int unalias_ldbl(int btype)
{
#if LDOUBLE_SIZE == 8
  if (btype == VT_LDOUBLE)
    btype = VT_DOUBLE;
#endif
  return btype;
}

/* AAPCS-VFP homogeneous floating-point aggregate (HFA) classification.
 *
 * Flatten `type` (arrays and nested structs included) and count its base
 * elements: every one must be a float, or every one a double, and there must
 * be 1-4 of them.  Returns the element count (0 when not an HFA); *base is
 * set to VT_FLOAT/VT_DOUBLE.  Bit-fields, unions, complex members and
 * unsized/flexible arrays disqualify the aggregate. */
static int hfa_scan(const CType *type, int *base)
{
  const int t = type->t;
  if (t & VT_ARRAY)
  {
    if (!type->ref || type->ref->c <= 0)
      return 0;
    const int n = hfa_scan(&type->ref->type, base);
    if (!n || n * type->ref->c > 4)
      return 0;
    return n * type->ref->c;
  }
  if (t & VT_COMPLEX)
    return 0;
  const int btype = unalias_ldbl(t & VT_BTYPE);
  if (btype == VT_FLOAT || btype == VT_DOUBLE)
  {
    if (*base && *base != btype)
      return 0;
    *base = btype;
    return 1;
  }
  if (btype == VT_STRUCT && type->ref && !IS_UNION(t))
  {
    int count = 0;
    for (const Sym *f = type->ref->next; f; f = f->next)
    {
      if (f->type.t & VT_BITFIELD)
        return 0;
      const int n = hfa_scan(&f->type, base);
      if (!n)
        return 0;
      count += n;
      if (count > 4)
        return 0;
    }
    return count;
  }
  return 0;
}

static int is_hgen_float_aggr(CType *type)
{
  int base = 0;
  return (type->t & VT_BTYPE) == VT_STRUCT && hfa_scan(type, &base) > 0;
}

/* AAPCS-VFP co-processor register candidate shape of an aggregate: a struct
 * HFA, or a _Complex float/double (two elements).  Returns the element count
 * (0 when it is neither) and sets *base_size to the element size, 4 or 8. */
ST_FUNC int gfunc_hfa(CType *type, int *base_size)
{
  int base = 0, n = 0;
  if (type->t & VT_COMPLEX)
  {
    base = unalias_ldbl(type->t & VT_BTYPE);
    n = (base == VT_FLOAT || base == VT_DOUBLE) ? 2 : 0;
  }
  else if ((type->t & VT_BTYPE) == VT_STRUCT)
    n = hfa_scan(type, &base);
  *base_size = base == VT_DOUBLE ? 8 : 4;
  return n;
}

/* Hard-float (AAPCS-VFP) return of an HFA or _Complex float/double wider
 * than 8 bytes: in s0..s(n-1), n = the result's size in words (2-8).
 * Returns n, or 0 when the value is not returned that way.  The front end
 * still gives such a value a result buffer (gfunc_sret says 0), but no
 * hidden pointer reaches the callee: the caller stores s0..s(n-1) into the
 * buffer after the call and the callee loads them from its own buffer at
 * its exit (gen_vfp_ret_transfer). */
ST_FUNC int gfunc_sret_vfp_words(CType *vt, int variadic)
{
  int base;
  if (float_abi != ARM_HARD_FLOAT || variadic)
    return 0;
  const int n = gfunc_hfa(vt, &base);
  return n * base > 8 ? n * base / 4 : 0;
}

// How many registers are necessary to return struct via registers
// if not possible, then 0 means return via struct pointer
ST_FUNC int gfunc_sret(CType *vt, int variadic, CType *ret, int *ret_align, int *regsize)
{
  int align;
  const int size = type_size(vt, &align);

  TRACE("'gfunc_sret'");
  /* RETURNVALUE carries at most d0, so a VFP value wider than 8 bytes keeps
   * the result-buffer shape here; gfunc_sret_vfp_words says it is passed in
   * s0-s7 rather than through a hidden pointer. */
  if (float_abi == ARM_HARD_FLOAT && !variadic && size <= 8 && (is_float(vt->t) || is_hgen_float_aggr(vt)))
  {
    *ret_align = 8;
    *regsize = 8;
    ret->ref = NULL;
    ret->t = VT_DOUBLE;
    return ceil_div(size, 8);
  }
  else if (size > 0 && size <= 4)
  {
    *ret_align = 4;
    *regsize = 4;
    ret->ref = NULL;
    ret->t = VT_INT;
    return 1;
  }
  return 0;
}

// are those offsets to allow TREG_R0 start from other register than r0?
// not sure

void th_store32_imm_or_reg_ex(int src_reg, uint32_t base_reg, int abs_off, int sign, uint32_t extra_exclude)
{
  const thumb_opcode st = th_str_imm(src_reg, base_reg, abs_off, sign ? 4 : 6, ENFORCE_ENCODING_NONE);
  if (!sign && strldr_cache_str_is_redundant(src_reg, (int)base_reg, abs_off, 6, st.size))
    return;
  if (!ot(st))
  {
    ScratchRegAlloc rr_alloc = th_offset_to_reg_ex(abs_off, sign, (1u << src_reg) | (1u << base_reg) | extra_exclude);
    int rr = rr_alloc.reg;
    ot_check(th_str_reg(src_reg, base_reg, rr, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
    restore_scratch_reg(&rr_alloc);
  }
}
void th_store16_imm_or_reg(int src_reg, uint32_t base_reg, int abs_off, int sign)
{
  if (!ot(th_strh_imm(src_reg, base_reg, abs_off, sign ? 4 : 6, ENFORCE_ENCODING_NONE)))
  {
    ScratchRegAlloc rr_alloc = th_offset_to_reg_ex(abs_off, sign, (1u << src_reg) | (1u << base_reg));
    int rr = rr_alloc.reg;
    ot_check(th_strh_reg(src_reg, base_reg, rr, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
    restore_scratch_reg(&rr_alloc);
  }
}
void th_store8_imm_or_reg(int src_reg, uint32_t base_reg, int abs_off, int sign)
{
  if (!ot(th_strb_imm(src_reg, base_reg, abs_off, sign ? 4 : 6, ENFORCE_ENCODING_NONE)))
  {
    ScratchRegAlloc rr_alloc = th_offset_to_reg_ex(abs_off, sign, (1u << src_reg) | (1u << base_reg));
    int rr = rr_alloc.reg;
    ot_check(th_strb_reg(src_reg, base_reg, rr, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
    restore_scratch_reg(&rr_alloc);
  }
}

static ThumbLiteralPoolEntry *th_literal_pool_allocate()
{
  ThumbLiteralPoolEntry *entry;

  pool_entries_total++;

  /* During dry-run, use separate pool to avoid modifying the real pool.
   * This prevents memory corruption when restoring state after dry-run. */
  if (dry_run_state.active)
  {
    if (dry_run_literal_pool_count >= dry_run_literal_pool_size)
    {
      dry_run_literal_pool_size <<= 1;
      dry_run_literal_pool =
          tcc_realloc(dry_run_literal_pool, dry_run_literal_pool_size * sizeof(ThumbLiteralPoolEntry));
      tcc_chained_hash_reserve(&literal_pool_hash, dry_run_literal_pool_size);
    }
    entry = &dry_run_literal_pool[dry_run_literal_pool_count++];
    entry->sym = NULL;
    entry->relocation = -1;
    entry->shared_index = -1;
    /* Track the count in the main state for code size calculations */
    thumb_gen_state.literal_pool_count++;
    return entry;
  }

  if (thumb_gen_state.literal_pool_count >= thumb_gen_state.literal_pool_size)
  {
    const int new_size = thumb_gen_state.literal_pool_size << 1;
    thumb_gen_state.literal_pool = tcc_realloc(thumb_gen_state.literal_pool, new_size * sizeof(ThumbLiteralPoolEntry));
    thumb_gen_state.literal_pool_size = new_size;
    tcc_chained_hash_reserve(&literal_pool_hash, new_size);
  }
  entry = &thumb_gen_state.literal_pool[thumb_gen_state.literal_pool_count++];
  entry->sym = NULL;
  entry->relocation = -1;
  entry->shared_index = -1;
  return entry;
}

/* Find existing literal pool entry with same sym and imm, and allocate new
   entry that shares its literal value.
   Uses hash table for O(1) lookup instead of O(n) linear search. */
static ThumbLiteralPoolEntry *th_literal_pool_find_or_allocate(Sym *sym, int64_t imm)
{
  int found_index;
  uint32_t full_hash;
  TCCChainedHash *hash;
  LiteralPoolLookupCache *cache;
  ThumbLiteralPoolEntry *pool;
  int new_index;

  if (dry_run_state.active)
  {
    hash = &literal_pool_hash;
    cache = &literal_pool_last_lookup;
    pool = dry_run_literal_pool;
    new_index = dry_run_literal_pool_count;
  }
  else
  {
    hash = &literal_pool_hash;
    cache = &literal_pool_last_lookup;
    pool = thumb_gen_state.literal_pool;
    new_index = thumb_gen_state.literal_pool_count;
  }

  full_hash = literal_pool_hash_func(sym, imm);
  found_index = literal_pool_lookup_cache_find(cache, full_hash, sym, imm);
  if (found_index < 0)
  {
    found_index = literal_pool_hash_find(hash, pool, full_hash, sym, imm);
  }

  /* Allocate new entry */
  ThumbLiteralPoolEntry *entry = th_literal_pool_allocate();
  if (found_index >= 0)
  {
    /* Mark as sharing with the found entry */
    entry->shared_index = found_index;
  }
  else
  {
    literal_pool_hash_insert(hash, full_hash, new_index);
    found_index = new_index;
  }
  literal_pool_lookup_cache_insert(cache, full_hash, sym, imm, found_index);
  return entry;
}

/* PIC + text/data-separation relocation choice. Single source of truth: the
 * SB-relative fast path and the literal-pool path must agree, or the dry-run
 * and real passes emit different sizes. See docs/sb_relative_got.md. */
static int th_pic_reloc_for_sym(Sym *sym, int sym_off)
{
  int sym_in_code_section = 0;
  int sym_in_rodata = 0;
  if (sym_off > 0 && sym_off < tcc_state->nb_sections)
  {
    Section *sym_sec = tcc_state->sections[sym_off];
    if (sym_sec && (sym_sec->sh_flags & SHF_EXECINSTR))
      sym_in_code_section = 1;
    /* Exact pointer match: only the main .rodata is anchor-addressed. */
    if (sym_sec && sym_sec == rodata_section)
      sym_in_rodata = 1;
  }
  if (tcc_state->share_rodata && (sym->type.t & VT_STATIC) && sym_off != SHN_UNDEF && sym_in_rodata)
    return R_ARM_RODATA_OFF;
  /* The start of .rodata (rodata_rel.c), which is what the anchor holds:
   * the linker resolves the literal to 0 (+ the addend). */
  if (tcc_state->share_rodata && sym->v == TOK___tcc_rodata_base)
    return R_ARM_RODATA_OFF;
  /* sym_off == SHN_UNDEF: forward-declared, section unknown — GOT32 is safe.
   * So is a constant still waiting in COMMON (a tentative `static const T x;`
   * whose definition follows): the definition may place it in .rodata, which
   * r9 does not reach -- a variadic body reading it where it stands loaded
   * zeros through GOTOFF.  Anything else in COMMON ends in .data or .bss. */
  if (sym_off == SHN_COMMON && tcc_state->share_rodata)
  {
    const CType *tp = &sym->type;
    while ((tp->t & (VT_BTYPE | VT_ARRAY)) == (VT_PTR | VT_ARRAY))
      tp = &tp->ref->type;
    if (tp->t & VT_CONSTANT)
      return R_ARM_GOT32;
  }
  if ((sym->type.t & VT_STATIC) && sym_off != SHN_UNDEF && sym_off != cur_text_section->sh_num &&
      !sym_in_code_section)
    return R_ARM_GOTOFF;
  return R_ARM_GOT32;
}

/* References in this body that a hoisted .rodata anchor would serve, counted
 * from the IR so the register allocator can decide whether to leave a register
 * for one.  An over- or under-count only moves bytes: the backend still emits
 * whatever form the register situation allows.  Symbols reach codegen from
 * places with no IR operand behind them (libcall names, switch tables), so
 * this is a floor, not the exact site count. */
ST_FUNC int tcc_gen_machine_rodata_anchor_ir_sites(const TCCIRState *ir, int stop_at)
{
  if (!ir || !tcc_state->share_rodata || !text_and_data_separation)
    return 0;

  int sites = 0;
  for (int i = 0; i < ir->next_instruction_index && sites < stop_at; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    const IROperand ops[3] = {tcc_ir_op_get_dest(ir, q), tcc_ir_op_get_src1(ir, q),
                              tcc_ir_op_get_src2(ir, q)};
    for (int k = 0; k < 3; k++)
    {
      Sym *sym = irop_get_sym_ex(ir, ops[k]);
      if (!sym)
        continue;
      ElfSym *esym = elfsym(sym);
      if (th_pic_reloc_for_sym(sym, esym ? esym->st_shndx : 0) == R_ARM_RODATA_OFF)
        sites++;
    }
  }
  return sites;
}

/* Add a symbol addend to an already-materialised address in r. imm == 0 is the
 * common case and is skipped: it cost 9,733 dead instructions (19 KiB) across
 * the compiler's own build. imm is pass-invariant, so sizes still agree. */
static void th_emit_sym_addend(int r, int64_t imm)
{
  thumb_opcode ot;
  if (imm == 0)
    return;
  if ((ot = th_add_imm(r, r, imm, flags_safe(), ENFORCE_ENCODING_NONE)).size != 0)
  {
    ot_check(ot);
    return;
  }
  uint32_t exclude_regs = (1 << r);
  ScratchRegAlloc scratch_alloc = get_scratch_reg_with_save(exclude_regs);
  int scratch = scratch_alloc.reg;

  thumb_opcode ldr = th_ldr_literal(scratch, 0, 1);
  ot_check(ldr);

  ThumbLiteralPoolEntry *entry2 = th_literal_pool_allocate();
  entry2->sym = NULL;
  entry2->imm = imm;
  entry2->patch_position = ind - ldr.size;
  entry2->relocation = -1;
  entry2->data_size = 4;
  entry2->short_instruction = (ldr.size == 2);
  th_literal_pool_note_entry(entry2);
  ot_check(th_add_reg(r, r, scratch, flags_safe(), THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
  restore_scratch_reg(&scratch_alloc);
}
void load_full_const(int r, int r1, uint32_t imm_lo, uint32_t imm_hi)
{
  struct Sym *sym = _lfc_sym;
  _lfc_sym = NULL;
  int64_t imm = (int64_t)((uint64_t)imm_hi << 32 | (uint64_t)imm_lo);
  ThumbLiteralPoolEntry *entry;
  thumb_opcode load_ins;
  int patch_pos;

  /* Validate symbol - only use symbols that can be externalized */
  sym = validate_sym_for_reloc(sym);

  /* Stable cache key: the validated symbol *before* the registration block
   * below may NULL it.  Registration is skipped during dry-run, so using the
   * post-registration `sym` would make the dry and real passes disagree on
   * cache hits and desynchronise code size.  `reuse_sym` is identical in both
   * passes (validate_sym_for_reloc does not depend on dry-run state). */
  Sym *reuse_sym = sym;

  /* Symbol-address reuse: when a register already holds &sym+imm, skip the
   * redundant literal-pool load.  Uses the same per-register imm_cache that
   * is invalidated on every clobbering emit and at IR boundaries, so the
   * decision is deterministic across the dry-run and real passes.  Only the
   * single-register (non-LDRD) form participates. */
  if (reuse_sym && thumb_gen_state.generating_function && r1 == PREG_NONE && r >= 0 && r < 16)
  {
    if (imm_cache[r].valid && imm_cache[r].sym == reuse_sym && imm_cache[r].value == imm)
      return; /* r already holds &sym+imm */
    for (int rr = 0; rr < 16; rr++)
    {
      if (rr != r && imm_cache[rr].valid && imm_cache[rr].sym == reuse_sym && imm_cache[rr].value == imm)
      {
        /* Another register holds it: copy instead of reloading from the
         * literal pool (saves a memory access and a pool word). */
        ot_check_mov_reg((uint32_t)r, (uint32_t)rr, flags_safe(), THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE, false);
        imm_cache[r].value = imm;
        imm_cache[r].sym = reuse_sym;
        imm_cache[r].valid = 1;
        return;
      }
    }
  }

  /* During dry-run, skip symbol registration and literal pool allocation.
   * We just emit the instruction (ot_check handles dry-run mode) to track
   * code size and scratch register usage, without creating side effects. */
  if (!dry_run_state.active)
  {
    if (sym && sym->c == 0)
    {
      /* Symbol not yet registered - try to register it */
      put_extern_sym(sym, NULL, 0, 0);
      if (sym->c <= 0)
      {
        /* Registration failed - symbol can't be externalized */
        sym = NULL;
      }
    }
  }

  /* SB-relative GOT: a single `ldr.w Rt,[r9,#imm12]` replaces literal + add R9
   * + load and the pool word. Goes through ot_check rather than
   * ot_check_ldr_imm on purpose — the latter's redundant-reload cache is keyed
   * on the pre-relocation immediate (0), which would conflate distinct
   * symbols. See docs/sb_relative_got.md. */
  if (sym && pic && text_and_data_separation && sb_relative_got && r1 == PREG_NONE)
  {
    ElfSym *sb_esym = elfsym(sym);
    int sb_sym_off = sb_esym ? sb_esym->st_shndx : 0;
    if (th_pic_reloc_for_sym(sym, sb_sym_off) == R_ARM_GOT32)
    {
      thumb_opcode sb_ins = th_ldr_imm(r, R9, 0, 6, ENFORCE_ENCODING_32BIT);
      if (sb_ins.size != 0)
      {
        ot_check(sb_ins);
        tcc_gen_machine_outline_capture_reloc(sym, ind - sb_ins.size);
        if (!dry_run_state.active && !tcc_gen_machine_outline_suppressing())
          greloc(cur_text_section, sym, ind - sb_ins.size, R_ARM_GOT_SBREL12);
        th_emit_sym_addend(r, imm);
        return;
      }
    }
  }

  TRACE("'load_full_const' to register: %d, with imm: %d\n", r, imm);

  /* Emit the instruction first.
   * ot() may flush the current literal pool BEFORE emitting this op.
   * If patch_position is captured before ot_check(), it can end up pointing
   * at the pool skip-branch and later patching would clobber it.
   */
  if (r1 == PREG_NONE)
  {
    load_ins = th_ldr_literal(r, 0, 1);
  }
  else
  {
    load_ins = th_ldrd_imm(r, r1, R_PC, 0, 4);
  }
  ot_check(load_ins);
  patch_pos = ind - load_ins.size;

  /* Record that r now holds &sym+imm so a later reference to the same global
   * address can be elided.  Must run after ot_check(), whose emit-level
   * invalidation cleared imm_cache[r] for the LDR we just produced.  Keyed on
   * the pre-registration `reuse_sym` for dry/real-pass consistency. */
  if (reuse_sym && thumb_gen_state.generating_function && r1 == PREG_NONE && r >= 0 && r < 16)
  {
    imm_cache[r].value = imm;
    imm_cache[r].sym = reuse_sym;
    imm_cache[r].valid = 1;
  }

  /* During dry-run, we still need to create the literal pool entry to ensure
   * the literal pool behavior (threshold checks, sharing, etc.) matches the real pass.
   * We still set sym so that find_or_allocate can match entries correctly.
   * We just skip symbol registration and relocation setup. */
  entry = th_literal_pool_find_or_allocate(sym, imm);
  entry->sym = sym;
  entry->patch_position = patch_pos;
  entry->relocation = -1; /* No relocation by default */
  entry->data_size = (r1 == PREG_NONE) ? 4 : 8;
  entry->short_instruction = (r1 == PREG_NONE && load_ins.size == 2);
  th_literal_pool_note_entry(entry);

  if (!sym)
  {
    entry->imm = imm;
    return;
  }

  /* Re-derive esym after ot_check(): literal pool generation during ot_check
   * can call put_elf_sym → section_ptr_add → section_realloc, which may
   * free and reallocate the symtab section buffer, invalidating any
   * earlier ElfSym pointer. */
  ElfSym *esym = elfsym(sym);
  int sym_off = 0;
  if (esym)
  {
    sym_off = esym->st_shndx;
  }
  if (!pic)
  {
    entry->relocation = R_ARM_ABS32;
    /* The imm value is the addend (offset from symbol base).
       For arr[i], imm = i * sizeof(element).
       The linker will add the symbol's address to this addend. */
    entry->imm = imm;
  }
  else
  {
    /* For PIC without a symbol, the literal is a plain constant (e.g. -1).
     * Must still store the value so the pool emits it correctly. */
    entry->imm = imm;
    if (sym)
    {
      if (text_and_data_separation)
      {
        /* Relocation strategy for text_and_data_separation + PIC:
         *
         * R_ARM_GOTOFF computes (symbol - GOT_addr) and at runtime adds R9.
         * This only works when symbol and GOT are in the same loadable
         * segment (i.e. both in data).  With text/data separation, code
         * (.text) and data (.got) are loaded at independent addresses.
         *
         * Static symbols in *data* sections (no SHF_EXECINSTR):
         *   GOTOFF is fine — symbol and GOT are both in the data segment.
         *
         * Everything else (including static functions in other .text.*
         * sections from -ffunction-sections):
         *   Use R_ARM_GOT32 — indirect through a GOT slot.  The linker
         *   creates a GOT entry (put_got_entry → R_RELATIVE for locals),
         *   fill_local_got_entries writes sym->st_value into the slot,
         *   and the YAFF writer emits a data relocation so the dynamic
         *   loader patches the slot to the runtime code address.
         */
        entry->relocation = th_pic_reloc_for_sym(sym, sym_off);
      }
      else
      {
        if (sym->type.t & VT_STATIC)
        {
          entry->relocation = R_ARM_REL32;
        }
        else
        {
          entry->relocation = R_ARM_GOT_PREL;
        }
      }
    }
  }

  if (pic)
  {
    if (sym)
    {
      if (text_and_data_separation)
      {
        /* Dispatch on the relocation chosen above — GOTOFF adds R9, GOT32
         * adds R9 then loads through the slot. */
        if (entry->relocation == R_ARM_RODATA_OFF)
        {
          /* Shared .rodata anchor: r holds (sym - rodata_base) from the
           * R_ARM_RODATA_OFF literal; add the rodata runtime base. */
          if (DRY_RUN_MODELLING)
            dry_run_state.rodata_anchor_sites++;
          if (rodata_anchor_reg >= 0)
          {
            /* The prologue parked the base in a register reserved for the
             * whole body (tcc_gen_machine_rodata_anchor_claim). */
            if (r == rodata_anchor_reg)
              tcc_ice("rodata anchor r%d handed out as a value register",
                        rodata_anchor_reg);
            ot_check(th_add_reg(r, r, rodata_anchor_reg, flags_safe(), THUMB_SHIFT_DEFAULT,
                                ENFORCE_ENCODING_NONE));
          }
          else
          {
            /*   push {tmp}; ldr tmp, [R9, #24]; add r, r, tmp; pop {tmp}
             * Use a DETERMINISTIC fixed scratch (a low register other than r,
             * saved by push/pop), NOT get_scratch_reg_with_save: the latter's
             * callee-saved fallback is gated on !dry_run_state.active, so under
             * register pressure (e.g. ps's larger functions) it can pick a
             * different register in the dry-run vs real pass, desync instruction
             * sizes, and corrupt literal-pool offsets — yielding a near-NULL
             * rodata address. A fixed push/pop emits identically in both passes. */
            int anchor_tmp = (r == 0) ? 1 : 0;
            ot_check(th_push((uint16_t)(1u << anchor_tmp)));
            ot_check_ldr_imm(anchor_tmp, R9, YAFF_RODATA_ANCHOR_GOT_OFFSET, 6, ENFORCE_ENCODING_NONE);
            ot_check(
                th_add_reg(r, r, anchor_tmp, flags_safe(), THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
            ot_check(th_pop((uint16_t)(1u << anchor_tmp)));
          }
        }
        else if (entry->relocation == R_ARM_GOTOFF)
        {
          /* Static data symbol — GOTOFF (add R9) */
          ot_check(th_add_reg(r, r, R9, flags_safe(), THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
        }
        else
        {
          ot_check(th_add_reg(r, r, R9, flags_safe(), THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));

          ot_check_ldr_imm(r, r, 0, 6, ENFORCE_ENCODING_NONE);
          th_emit_sym_addend(r, imm);
        }
      }
      else
      {
        if (sym->type.t & VT_STATIC)
        {
          ot_check(th_add_reg(r, r, R_PC, flags_safe(), THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
          ot_check(th_sub_imm(r, r, 8, flags_safe(), ENFORCE_ENCODING_NONE));
        }
        else
        {
          thumb_opcode ot;
          ot_check(th_add_reg(r, r, R_PC, flags_safe(), THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
          ot_check_ldr_imm(r, r, 4, 6, ENFORCE_ENCODING_NONE);
          /* Same zero-addend skip as the GOT-relative path above. */
          if (imm == 0)
          {
            /* no addend: nothing to add */
          }
          else if ((ot = th_add_imm(r, r, imm, flags_safe(), ENFORCE_ENCODING_NONE)).size != 0)
          {
            ot_check(ot);
          }
          else
          {
            /* Find a free scratch register for literal pool entry */
            uint32_t exclude_regs = (1 << r); /* Exclude destination register */
            ScratchRegAlloc scratch_alloc = get_scratch_reg_with_save(exclude_regs);
            int scratch = scratch_alloc.reg;

            thumb_opcode ldr = th_ldr_literal(scratch, 0, 1);
            ot_check(ldr);

            ThumbLiteralPoolEntry *entry2 = th_literal_pool_allocate();
            entry2->sym = NULL;
            entry2->imm = imm;
            entry2->patch_position = ind - ldr.size;
            entry2->relocation = -1;
            entry2->data_size = 4;
            entry2->short_instruction = (ldr.size == 2);
            th_literal_pool_note_entry(entry2);
            ot_check(
                th_add_reg(r, r, scratch, flags_safe(), THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
            restore_scratch_reg(&scratch_alloc);
          }
        }
      }
    }
  }
}

int load_short_from_base(int ir, int base, int fc, int sign)
{
  const thumb_opcode ins = th_ldrsh_imm(ir, base, fc, sign ? 4 : 6, ENFORCE_ENCODING_NONE);
  TRACE("Load short sign: %d, r %d, base: %d, fc: %d\n", sign, ir, base, fc);
  return ot(ins);
}

ST_FUNC void tcc_machine_addr_of_stack_slot(int dest_reg, int frame_offset, int is_param)
{
  if (dest_reg == PREG_REG_NONE)
    tcc_ice("addr_of_stack_slot requires a destination register");

  /* Stack parameters live above the saved-register area.
   * When computing their address, fold in offset_to_args (prologue push size).
   * Locals/spills need callee-saved gap adjustment. */
  if (is_param)
    frame_offset = param_frame_offset(frame_offset);
  else
    frame_offset = fp_adjust_local_offset(frame_offset, 0);

  const int base_reg = tcc_state->need_frame_pointer ? R_FP : R_SP;

  if (frame_offset == 0)
  {
    if (dest_reg != base_reg)
    {
      ot_check_mov_reg(dest_reg, base_reg, flags_safe(), THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE,
                       false);
    }
    return;
  }

  /* Check FP offset cache for existing computation
   * Only use cache for callee-saved registers (r4-r11) since scratch registers
   * like ip (r12) can be overwritten at any time without invalidating the cache. */
  TCCIRState *ir = tcc_state->ir;
  int cached_reg = -1;
  int is_callee_saved = (dest_reg >= R4 && dest_reg <= R11);

  if (ir && is_callee_saved && tcc_ir_opt_fp_cache_lookup(ir, frame_offset, &cached_reg))
  {
    /* Cache hit! Verify the cached register is also callee-saved */
    if (cached_reg >= R4 && cached_reg <= R11)
    {
      if (cached_reg != dest_reg)
      {
        ot_check_mov_reg(dest_reg, cached_reg, flags_safe(), THUMB_SHIFT_DEFAULT,
                         ENFORCE_ENCODING_NONE, false);
      }
      return;
    }
    /* Cached in scratch register - don't use it */
  }

  const int neg = (frame_offset < 0);
  int abs_off = neg ? -frame_offset : frame_offset;
  thumb_opcode op = neg ? th_sub_imm(dest_reg, base_reg, abs_off, flags_safe(), ENFORCE_ENCODING_NONE)
                        : th_add_imm(dest_reg, base_reg, abs_off, flags_safe(), ENFORCE_ENCODING_NONE);

  if (op.size != 0)
  {
    ot_check(op);
    /* Record in cache for future reuse - only for callee-saved registers
     * which won't be clobbered unexpectedly */
    if (ir && is_callee_saved)
      tcc_ir_opt_fp_cache_record(ir, frame_offset, dest_reg);
    return;
  }

  ScratchRegAlloc offset_alloc = {0};
  int offset_reg = dest_reg;

  if (dest_reg == base_reg)
  {
    offset_alloc = get_scratch_reg_with_save(1u << base_reg);
    if (offset_alloc.reg == PREG_NONE)
      tcc_ice("unable to allocate scratch register for stack address");
    offset_reg = offset_alloc.reg;
  }

  load_full_const(offset_reg, PREG_NONE, LFC_SPLIT(frame_offset));
  ot_check(th_add_reg(dest_reg, base_reg, offset_reg, flags_safe(), THUMB_SHIFT_DEFAULT,
                      ENFORCE_ENCODING_NONE));

  if (dest_reg == base_reg)
  {
    restore_scratch_reg(&offset_alloc);
  }

  /* Record complex computation in cache - only for callee-saved registers */
  if (ir && is_callee_saved)
    tcc_ir_opt_fp_cache_record(ir, frame_offset, dest_reg);
}

/* A constant with no 2-byte encoding here -- MOV.W/MVN.W/MOVW, a literal-pool
 * load, or a MOVS the live flags forbid -- that another register is known to
 * hold is a 2-byte `mov rd, rs` instead.  The Zig C backend passes its
 * `undefined` 0xaaaaaaaa in two argument registers of one call, and a high
 * register receiving 0 takes `mov.w rH, #0`.  imm_cache says who holds what
 * (maintained identically in the dry and real passes).  Not inside an IT
 * block, where the copy would be conditional.
 * TCC_DISABLE_PASS=codegen:const_reg_copy turns it off. */
static int const_reg_copy_off = -1;

int load_constant_from_holding_reg(int reg, int64_t key)
{
  if (const_reg_copy_off < 0)
    const_reg_copy_off = tcc_ir_opt_pass_disabled("codegen:const_reg_copy");
  if (const_reg_copy_off || !thumb_gen_state.generating_function || mov_equiv_it_pending > 0 || reg < 0 ||
      reg >= R_SP)
    return 0;
  if (th_generic_mov_imm((uint32_t)reg, (int)(uint32_t)key).size == 2)
    return 0;
  for (int rr = 0; rr < R_SP; rr++)
  {
    if (rr == reg || !imm_cache[rr].valid || imm_cache[rr].sym != NULL ||
        (uint32_t)imm_cache[rr].value != (uint32_t)key)
      continue;
    ot_check_mov_reg((uint32_t)reg, (uint32_t)rr, flags_safe(), THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE, false);
    return 1;
  }
  return 0;
}

/* Load a constant value into a register (or register pair for 64-bit).
 * This is a simplified wrapper around load_full_const/th_generic_mov_imm
 * that doesn't require an SValue. Used by IR-level materialization.
 * If sym is non-NULL, a relocation will be generated for symbol-relative constants. */
ST_FUNC void tcc_machine_load_constant(int dest_reg, int dest_reg_high, int64_t value, int is_64bit, Sym *sym)
{
  if (dest_reg == PREG_REG_NONE)
    tcc_ice("load_constant requires a destination register");

  /* Symbol-relative constants always need the literal pool for relocations */
  if (sym)
  {
    Sym *validated_sym = validate_sym_for_reloc(sym);
    if (validated_sym)
    {
      _lfc_sym = validated_sym;
      load_full_const(dest_reg, dest_reg_high, LFC_SPLIT(value));
      return;
    }
    /* Invalid or missing sym - fall through to treat as plain constant */
  }

  if (!sym && !is_64bit && dest_reg >= 0 && dest_reg < 16 &&
      imm_cache[dest_reg].valid && imm_cache[dest_reg].sym == NULL &&
      imm_cache[dest_reg].value == value)
    return;

  if (is_64bit)
  {
    const uint32_t lo = (uint32_t)(value & 0xFFFFFFFF);
    const uint32_t hi = (uint32_t)((uint64_t)value >> 32);

    /* Try immediate encoding for both halves */
    thumb_opcode o1 = th_generic_mov_imm(dest_reg, (int)lo);
    thumb_opcode o2 = th_generic_mov_imm(dest_reg_high, (int)hi);

    if (o1.size != 0 && o2.size != 0)
    {
      /* Both can be encoded as immediates */
      ot(o1);
      ot(o2);
      return;
    }

    /* At least one half needs literal pool - use combined 64-bit load */
    load_full_const(dest_reg, dest_reg_high, LFC_SPLIT(value));
    return;
  }

  /* 32-bit constant */
  if (load_constant_from_holding_reg(dest_reg, value))
    ;
  else if (!ot(th_generic_mov_imm(dest_reg, (uint32_t)value)))
    load_full_const(dest_reg, PREG_NONE, LFC_SPLIT(value));

  if (!sym && !is_64bit && dest_reg >= 0 && dest_reg < 16)
  {
    imm_cache[dest_reg].value = value;
    imm_cache[dest_reg].sym = NULL;
    imm_cache[dest_reg].valid = 1;
  }
}

/* Load comparison result (0 or 1) based on condition flags.
 * Used by IR-level materialization for VT_CMP values. */
ST_FUNC void tcc_machine_load_cmp_result(int dest_reg, int condition_code)
{
  if (dest_reg == PREG_REG_NONE)
    tcc_ice("load_cmp_result requires a destination register");
  if (dest_reg == R_SP || dest_reg == R_PC)
    tcc_ice("load_cmp_result cannot use SP or PC");

  const uint32_t firstcond = mapcc(condition_code);
  /* IT block: if cond then mov 1, else mov 0 */
  o(0xbf00 | (firstcond << 4) | 0x4 | ((~firstcond & 1) << 3));
  ot_check(th_generic_mov_imm(dest_reg, 1));
  ot_check(th_generic_mov_imm(dest_reg, 0));
}

/* Load jump condition result (0 or 1) based on a pending jump target.
 * Used by IR-level materialization for VT_JMP/VT_JMPI values. */
ST_FUNC void tcc_machine_load_jmp_result(int dest_reg, int jmp_addr, int invert)
{
  if (dest_reg == PREG_REG_NONE)
    tcc_ice("load_jmp_result requires a destination register");

#ifdef TCC_TARGET_ARM_ARCHV6M
  if (dest_reg > 7)
    tcc_ice("implement load_jmp_result for armv6m with high register");
#endif

  /* Load the "true" branch value, then unconditionally branch over the "false" value,
   * then patch the jump target to land on the "false" value */
  ot_check(th_generic_mov_imm(dest_reg, invert ? 0 : 1));
  ot_check(th_b_t4(2));
  gsym(jmp_addr);
  codegen_internal_merge_point();
  ot_check(th_generic_mov_imm(dest_reg, invert ? 1 : 0));
}

/* Load value from memory at base+offset into register(s).
 * Uses IROP_BTYPE_* constants directly, no VT_* conversion needed.
 */
void load_from_base(int r, int r1, int irop_btype, int is_unsigned, int fc, int sign, uint32_t base)
{
  int success = 0;
  const int is_64bit =
      (irop_btype == IROP_BTYPE_INT64 || irop_btype == IROP_BTYPE_FLOAT64 || (r1 >= 0 && r1 != PREG_REG_NONE));

  TRACE("load_from_base: r=%d, r1=%d, irop_btype=%d, is_unsigned=%d, fc=%d, sign=%d, base=%d", r, r1, irop_btype,
        is_unsigned, fc, sign, base);

  if (is_64bit)
  {
    /* 64-bit value (double float or long long) - load to register pair */
    int ir_high = r1;
    ScratchRegAlloc ir_high_alloc = {0};
    if (ir_high < 0 || ir_high == PREG_REG_NONE)
    {
      /* No explicit high register — always use scratch to avoid clobbering
       * r+1 which may be allocated to another live variable.  The old r+1
       * fallback was only safe when mat.c pre-materialized into scratch
       * registers (ip:lr pair) before the handler. */
      ir_high_alloc = get_scratch_reg_with_save((1u << r) | (1u << base));
      ir_high = ir_high_alloc.reg;
    }

    /* If base overlaps with destination, preserve it */
    ScratchRegAlloc base_alloc = {0};
    uint32_t base_reg = base;
    if (base_reg == (uint32_t)r || base_reg == (uint32_t)ir_high)
    {
      uint32_t exclude = (1u << r) | (1u << ir_high);
      base_alloc = get_scratch_reg_with_save(exclude);
      base_reg = (uint32_t)base_alloc.reg;
      ot_check_mov_reg((int)base_reg, (int)base, flags_safe(), THUMB_SHIFT_DEFAULT,
                       ENFORCE_ENCODING_NONE, false);
    }

    /* Try LDRD Rt, Rt2, [Rn, #±imm] when both halves share one base.
     * T1 encoding requires: Rt != Rt2, Rt/Rt2 not SP/PC, offset 4-byte
     * aligned and |offset| <= 1020.  LDRD also requires the target address
     * to be 4-byte aligned on ARMv7-M/v8-M (faults otherwise, regardless of
     * UNALIGN_TRP).  Restrict to SP/FP-relative bases where TCC's stack
     * allocator guarantees 4-byte alignment of 64-bit slots; arbitrary
     * pointers (e.g. into a packed struct) may be unaligned. */
    const int base_is_stack = frame_word_base((int)base_reg);
    if (base_is_stack && (fc & 3) == 0 && fc <= 1020 && r >= 0 && r <= R_LR && r != R_SP && ir_high >= 0 &&
        ir_high <= R_LR && ir_high != R_SP && r != ir_high)
    {
      uint32_t puw = sign ? 4 : 6;
      if (!strldr_cache_ldrd_is_redundant(r, ir_high, (int)base_reg, fc, puw))
        ot_check(th_ldrd_imm((uint32_t)r, (uint32_t)ir_high, base_reg, fc, puw));
      if (base_alloc.saved)
        restore_scratch_reg(&base_alloc);
      if (ir_high_alloc.saved)
        restore_scratch_reg(&ir_high_alloc);
      return;
    }

    /* Load low word */
    success = load_word_from_base(r, base_reg, fc, sign);
    if (!success)
    {
      ScratchRegAlloc rr_alloc = th_offset_to_reg_ex(fc, sign, (1u << r) | (1u << base_reg) | (1u << ir_high));
      int rr = rr_alloc.reg;
      ot_check(th_ldr_reg(r, base_reg, rr, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
      restore_scratch_reg(&rr_alloc);
    }

    /* Load high word */
    int fc_high = sign ? (fc - 4) : (fc + 4);
    success = load_word_from_base(ir_high, base_reg, fc_high, sign);
    if (!success)
    {
      ScratchRegAlloc rr_alloc = th_offset_to_reg_ex(fc_high, sign, (1u << r) | (1u << base_reg) | (1u << ir_high));
      int rr = rr_alloc.reg;
      ot_check(th_ldr_reg(ir_high, base_reg, rr, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
      restore_scratch_reg(&rr_alloc);
    }

    if (base_alloc.saved)
      restore_scratch_reg(&base_alloc);
    if (ir_high_alloc.saved)
      restore_scratch_reg(&ir_high_alloc);
    return;
  }

  if (irop_btype == IROP_BTYPE_INT16)
  {
    if (!is_unsigned)
      success = load_short_from_base(r, base, fc, sign);
    else
      success = load_ushort_from_base(r, base, fc, sign);
  }
  else if (irop_btype == IROP_BTYPE_INT8)
  {
    if (!is_unsigned)
      success = load_byte_from_base(r, base, fc, sign);
    else
      success = load_ubyte_from_base(r, base, fc, sign);
  }
  else
  {
    /* IROP_BTYPE_INT32, IROP_BTYPE_FLOAT32, IROP_BTYPE_STRUCT, IROP_BTYPE_FUNC: load as word */
    success = load_word_from_base(r, base, fc, sign);
  }

  if (!success)
  {
    ScratchRegAlloc rr_alloc = th_offset_to_reg_ex(fc, sign, (1u << r) | (1u << base));
    int rr = rr_alloc.reg;
    if (irop_btype == IROP_BTYPE_INT16)
    {
      if (is_unsigned)
        ot_check(th_ldrh_reg(r, base, rr, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
      else
        ot_check(th_ldrsh_reg(r, base, rr, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
    }
    else if (irop_btype == IROP_BTYPE_INT8)
    {
      if (is_unsigned)
        ot_check(th_ldrb_reg(r, base, rr, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
      else
        ot_check(th_ldrsb_reg(r, base, rr, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
    }
    else
      ot_check(th_ldr_reg(r, base, rr, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
    restore_scratch_reg(&rr_alloc);
  }
}

ST_FUNC void gen_increment_tcov(SValue *sv)
{
  TRACE("'gen_increment_tcov'");
}

/* Dispatch a reg_handler call through a direct call instead of an indirect
 * (function pointer) call.  This works around a code-generation bug where
 * struct-by-value arguments (thumb_shift) get corrupted when passed through
 * indirect calls that also use sret return (thumb_opcode is 8 bytes).
 * By comparing the function pointer and branching to a direct call, the
 * cross-compiler generates correct struct passing code. */
thumb_opcode thumb_call_reg_handler(thumb_reg_handler_t fn, uint32_t rd, uint32_t rn, uint32_t rm,
                                           thumb_flags_behaviour flags, thumb_shift shift,
                                           thumb_enforce_encoding encoding)
{
  if (fn == th_add_reg)
    return th_add_reg(rd, rn, rm, flags, shift, encoding);
  if (fn == th_sub_reg)
    return th_sub_reg(rd, rn, rm, flags, shift, encoding);
  if (fn == th_adc_reg)
    return th_adc_reg(rd, rn, rm, flags, shift, encoding);
  if (fn == th_sbc_reg)
    return th_sbc_reg(rd, rn, rm, flags, shift, encoding);
  if (fn == th_cmp_reg)
    return th_cmp_reg(rd, rn, rm, flags, shift, encoding);
  if (fn == th_lsl_reg)
    return th_lsl_reg(rd, rn, rm, flags, shift, encoding);
  if (fn == th_lsr_reg)
    return th_lsr_reg(rd, rn, rm, flags, shift, encoding);
  if (fn == th_asr_reg)
    return th_asr_reg(rd, rn, rm, flags, shift, encoding);
  if (fn == th_orr_reg)
    return th_orr_reg(rd, rn, rm, flags, shift, encoding);
  if (fn == th_and_reg)
    return th_and_reg(rd, rn, rm, flags, shift, encoding);
  if (fn == th_eor_reg)
    return th_eor_reg(rd, rn, rm, flags, shift, encoding);
  if (fn == th_bic_reg)
    return th_bic_reg(rd, rn, rm, flags, shift, encoding);
  /* Unreachable for known handlers — fallback to direct call. */
  return fn(rd, rn, rm, flags, shift, encoding);
}
void thumb_require_materialized_reg(const char *ctx, const char *operand, int reg)
{
  const bool reg_is_hw = (reg >= 0) && (reg <= 15);
  if (reg == PREG_REG_NONE || !reg_is_hw)
  {
    tcc_ice("%s expects %s in a physical register (pr=%d)", ctx, operand, reg);
  }
}
uint32_t thumb_exclude_mask_for_regs(int count, const int *regs)
{
  uint32_t mask = 0;
  for (int i = 0; i < count; ++i)
  {
    const int reg = regs[i];
    if (reg >= 0 && reg <= 15)
      mask |= (1u << reg);
  }
  return mask;
}
bool thumb_is_hw_reg(int reg)
{
  return reg >= 0 && reg <= 15;
}
