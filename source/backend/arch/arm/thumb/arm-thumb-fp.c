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

/* Thumb-2 code generator: floating point -- soft-float helper calls, VFP and
 * RP2350 DCP arithmetic, and complex arithmetic. */

#include "arm-thumb-gen.h"

/* Get the soft float library function name for an FP operation */
static const char *get_softfp_func_name(TccIrOp op, int is_double)
{
  switch (op)
  {
  case TCCIR_OP_FADD:
    return is_double ? "__aeabi_dadd" : "__aeabi_fadd";
  case TCCIR_OP_FSUB:
    return is_double ? "__aeabi_dsub" : "__aeabi_fsub";
  case TCCIR_OP_FMUL:
    return is_double ? "__aeabi_dmul" : "__aeabi_fmul";
  case TCCIR_OP_FDIV:
    return is_double ? "__aeabi_ddiv" : "__aeabi_fdiv";
  case TCCIR_OP_FNEG:
    /* For negation, we can XOR the sign bit - handled separately */
    return NULL;
  default:
    return NULL;
  }
}

/* fp_mop_load_arg: Load a MachineOperand value into a fixed argument register
 * (R0, R1, etc.) for a soft-float ABI call.  Unlike mach_ensure_in_reg, this
 * writes to a caller-specified register without scratch allocation bookkeeping.
 * Used by tcc_gen_machine_fp_mop to set up R0/R1 before BL __aeabi_f*. */
static void fp_mop_load_arg(int target_reg, const MachineOperand *op)
{
  switch (op->kind)
  {
  case MACH_OP_NONE:
    return;
  case MACH_OP_VFP_REG:
    ot_check(th_vmov_gp_sp((uint16_t)target_reg, (uint16_t)op->u.reg.r0, 1)); /* target = sN */
    return;
  case MACH_OP_REG:
    if (!op->needs_deref)
    {
      if (op->u.reg.r0 != target_reg)
        ot_check_mov_reg((uint32_t)target_reg, (uint32_t)op->u.reg.r0, flags_safe(),
                         THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE, false);
    }
    else
    {
      load_from_base(target_reg, PREG_REG_NONE, op->btype, (int)op->is_unsigned, 0, 0, (uint32_t)op->u.reg.r0);
    }
    return;
  case MACH_OP_SPILL:
    tcc_machine_load_spill_slot(target_reg, op->u.spill.offset);
    if (op->needs_deref)
      load_from_base(target_reg, PREG_REG_NONE, op->btype, (int)op->is_unsigned, 0, 0, (uint32_t)target_reg);
    return;
  case MACH_OP_PARAM_STACK:
  {
    const int adjusted = param_frame_offset(op->u.param.offset);
    const int base_reg = tcc_state->need_frame_pointer ? R_FP : R_SP;
    const int sign = (adjusted < 0);
    const int abs_off = sign ? -adjusted : adjusted;
    load_from_base(target_reg, PREG_REG_NONE, op->btype, (int)op->is_unsigned, abs_off, sign, (uint32_t)base_reg);
    return;
  }
  case MACH_OP_IMM:
    tcc_machine_load_constant(target_reg, PREG_REG_NONE, op->u.imm.val, 0, NULL);
    return;
  case MACH_OP_SYMBOL:
  {
    Sym *sym = op->u.sym.sym ? validate_sym_for_reloc(op->u.sym.sym) : NULL;
    if (!op->needs_deref)
    {
      tcc_machine_load_constant(target_reg, PREG_REG_NONE, op->u.sym.addend, 0, sym);
    }
    else
    {
      /* Load symbol address into target_reg, then dereference through it. */
      tcc_machine_load_constant(target_reg, PREG_REG_NONE, 0, 0, sym);
      const int32_t addend = op->u.sym.addend;
      load_from_base(target_reg, PREG_REG_NONE, op->btype, (int)op->is_unsigned,
                     addend < 0 ? (int)(-addend) : (int)addend, addend < 0 ? 1 : 0, (uint32_t)target_reg);
    }
    return;
  }
  case MACH_OP_FRAME_ADDR:
    tcc_machine_addr_of_stack_slot(target_reg, op->u.frame.offset, 0);
    if (op->needs_deref)
      load_from_base(target_reg, PREG_REG_NONE, op->btype, (int)op->is_unsigned, 0, 0, (uint32_t)target_reg);
    return;
  case MACH_OP_CHAIN_REL:
  {
    /* Captured variable: load from parent frame via static chain. */
    ScratchRegAlloc chain_scratch = {0};
    int chain_used = 0;
    int base = resolve_chain_base(tcc_state->ir, op->u.chain.chain_index, (1u << (uint32_t)target_reg), &chain_scratch,
                                  &chain_used);
    int32_t off = op->u.chain.offset;
    int sign = (off < 0);
    int abs_off = sign ? (int)(-off) : (int)off;
    load_from_base(target_reg, PREG_REG_NONE, op->btype, (int)op->is_unsigned, abs_off, sign, (uint32_t)base);
    if (chain_used)
      restore_scratch_reg(&chain_scratch);
    return;
  }
  default:
    tcc_error("compiler_error: fp_mop_load_arg: unhandled kind %d", (int)op->kind);
  }
}

/* Load a 64-bit (double-precision) MachineOperand into two consecutive argument
 * registers (lo_reg = low 32 bits, hi_reg = high 32 bits).
 * Handles REG pair, SPILL pair, PARAM_STACK, and deref'd REG. */
static void fp_mop_load_double_arg(int lo_reg, int hi_reg, const MachineOperand *op)
{
  if (op->needs_deref && op->kind == MACH_OP_REG)
  {
    /* Pointer in register: resolve [r0] and [r0+4] into physical regs first. */
    MachineCodegenContext mctx;
    memset(&mctx, 0, sizeof(mctx));
    uint32_t excl = (1u << (uint32_t)lo_reg) | (1u << (uint32_t)hi_reg);
    MachineOperand resolved = mach_resolve_deref_64(&mctx, op, &excl);
    mach_release_all(&mctx);
    MachineOperand lo_op = mach_make_lo_half(&resolved);
    MachineOperand hi_op = mach_make_hi_half(&resolved);
    fp_mop_load_arg(lo_reg, &lo_op);
    fp_mop_load_arg(hi_reg, &hi_op);
    return;
  }
  if (op->kind == MACH_OP_PARAM_STACK)
  {
    /* Stack parameter: low word at op->offset, high word at op->offset + 4. */
    fp_mop_load_arg(lo_reg, op);
    MachineOperand hi_op = *op;
    hi_op.u.param.offset += 4;
    fp_mop_load_arg(hi_reg, &hi_op);
    return;
  }
  /* REG (non-deref) or SPILL: split with lo/hi helpers. */
  {
    MachineOperand lo_op = mach_make_lo_half(op);
    MachineOperand hi_op = mach_make_hi_half(op);
    fp_mop_load_arg(lo_reg, &lo_op);
    fp_mop_load_arg(hi_reg, &hi_op);
  }
}

/* Issue a BL to a soft-float library function. Under text+data separation a
 * helper in a shared FP runtime is reached through a stub that swaps in its
 * own R9, so R9 (with R12, keeping SP 8-aligned) is saved around the call; a
 * helper linked into this module (static FP runtime, libtcc1) leaves R9 alone
 * and needs neither. */
static void fp_mop_do_bl(const char *func_name)
{
  Sym *sym = external_global_sym(tok_alloc_const(func_name), &func_old_type);
  MachineOperand func_mop = {0};
  func_mop.kind = MACH_OP_SYMBOL;
  func_mop.u.sym.sym = sym;
  func_mop.u.sym.addend = 0;
  const int save_r9 = text_and_data_separation && !thumb_callee_in_this_module(&func_mop);
  if (save_r9)
    ot_check(th_push((uint16_t)((1 << R9) | (1 << R12))));
  gcall_or_jump_mop(0, func_mop);
  if (save_r9)
    ot_check(th_pop((uint16_t)((1 << R9) | (1 << R12))));
}

/* Write a soft-float call result back to dest.
 * Single-precision result is in R0; double-precision is in R0 (lo) : R1 (hi). */
static void fp_mop_writeback_result(const MachineOperand *dest, int is_double)
{
  if (is_double)
  {
    MachineOperand lo_dest = mach_make_lo_half(dest);
    MachineOperand hi_dest = mach_make_hi_half(dest);
    mach_writeback_dest(&lo_dest, R0);
    mach_writeback_dest(&hi_dest, R1);
  }
  else
    mach_writeback_dest(dest, R0);
}

/* ============================================================
 * Complex float MOP path — Phase 5k
 * ============================================================
 *
 * Complex floats are 64-bit register pairs: lo = real, hi = imaginary.
 * Complex doubles are 128-bit values (always spilled): real at offset+0, imag at offset+8.
 * These functions use the MOP infrastructure (fp_mop_load_arg, fp_mop_do_bl,
 * mach_writeback_dest) to handle any operand kind (REG, SPILL, PARAM_STACK,
 * CHAIN_REL, etc.) without requiring fill_registers_ir.
 *
 * Strategy: save all inputs to a stack frame, call __aeabi_f* / __aeabi_d*
 * library functions, write results back to dest via mach_writeback_dest.
 */

/* Split a complex MachineOperand into its real component.
 * For complex float: real is the 32-bit lo half (same as mach_make_lo_half).
 * For complex double: real is the 64-bit double at the base offset. */
MachineOperand mach_make_complex_real(const MachineOperand *op)
{
  if (op->btype == IROP_BTYPE_FLOAT64)
  {
    /* Complex double: real part is a 64-bit double at the base offset. */
    MachineOperand real = *op;
    real.is_complex = false;
    real.is_64bit = true; /* each component is 64-bit double */
    if (real.kind == MACH_OP_REG)
      ; /* keep r0:r1 pair — only valid for register-allocated complex floats */
    return real;
  }
  /* Complex float: fall back to lo half. */
  return mach_make_lo_half(op);
}

/* Split a complex MachineOperand into its imaginary component.
 * For complex float: imag is the 32-bit hi half (same as mach_make_hi_half).
 * For complex double: imag is the 64-bit double at base offset + 8. */
MachineOperand mach_make_complex_imag(const MachineOperand *op)
{
  if (op->btype == IROP_BTYPE_FLOAT64)
  {
    /* Complex double: imag part is a 64-bit double at offset + 8. */
    MachineOperand imag = *op;
    imag.is_complex = false;
    imag.is_64bit = true;
    switch (imag.kind)
    {
    case MACH_OP_SPILL:
      imag.u.spill.offset += 8;
      break;
    case MACH_OP_FRAME_ADDR:
      imag.u.frame.offset += 8;
      break;
    case MACH_OP_PARAM_STACK:
      imag.u.param.offset += 8;
      break;
    case MACH_OP_CHAIN_REL:
      imag.u.chain.offset += 8;
      break;
    case MACH_OP_SYMBOL:
      imag.u.sym.addend += 8;
      break;
    case MACH_OP_REG:
      /* Register-based complex double shouldn't happen (force-spilled),
       * but handle gracefully: imaginary part is not representable. */
      break;
    default:
      break;
    }
    return imag;
  }
  /* Complex float: fall back to hi half. */
  return mach_make_hi_half(op);
}

/* Helper: save a double from R0:R1 to SP-relative stack offset. */
static void fp_mop_save_double_to_sp(int off)
{
  ot_check_str_imm(R0, R_SP, off, 6, ENFORCE_ENCODING_NONE);
  ot_check_str_imm(R1, R_SP, off + 4, 6, ENFORCE_ENCODING_NONE);
}

/* Helper: load a double from SP-relative stack offset into (lo_reg, hi_reg). */
static void fp_mop_load_double_from_sp(int lo_reg, int hi_reg, int off)
{
  ot_check_ldr_imm(lo_reg, R_SP, off, 6, ENFORCE_ENCODING_NONE);
  ot_check_ldr_imm(hi_reg, R_SP, off + 4, 6, ENFORCE_ENCODING_NONE);
}

/* Process complex double multiplication via MachineOperands.
 * Handles all cases:
 *   scalar × complex:  a * (c+di) = ac + (ad)i
 *   complex × scalar:  (a+bi) * c = ac + (bc)i
 *   complex × complex: (a+bi) * (c+di) = (ac-bd) + (ad+bc)i
 *
 * Uses __aeabi_dmul, __aeabi_dadd, __aeabi_dsub for double-precision.
 * Double AEABI calling convention: R0:R1 = arg1, R2:R3 = arg2, result in R0:R1.
 */
static void thumb_process_complex_mul_double_mop(MachineOperand src1, MachineOperand src2, MachineOperand dest)
{
  int s1_complex = src1.is_complex;
  int s2_complex = src2.is_complex;

  MachineOperand d_real = mach_make_complex_real(&dest);
  MachineOperand d_imag = mach_make_complex_imag(&dest);

  if (!s1_complex && s2_complex)
  {
    /* scalar double × complex double: a * (c+di) = ac + (ad)i */
    MachineOperand s2_real = mach_make_complex_real(&src2);
    MachineOperand s2_imag = mach_make_complex_imag(&src2);

    /* Allocate 8 bytes to save the scalar 'a'. */
    ot_check(th_sub_imm(R_SP, R_SP, 8, flags_safe(), ENFORCE_ENCODING_NONE));

    /* Load scalar 'a' into R0:R1 and save to stack. */
    fp_mop_load_double_arg(R0, R1, &src1);
    fp_mop_save_double_to_sp(0);

    /* Compute a * c: load 'c' into R2:R3. R0:R1 already = 'a'. */
    fp_mop_load_double_arg(R2, R3, &s2_real);
    fp_mop_do_bl("__aeabi_dmul");
    /* R0:R1 = a*c → write to dest real. */
    fp_mop_writeback_result(&d_real, 1);

    /* Compute a * d: reload 'a' from stack, load 'd' into R2:R3. */
    fp_mop_load_double_from_sp(R0, R1, 0);
    fp_mop_load_double_arg(R2, R3, &s2_imag);
    fp_mop_do_bl("__aeabi_dmul");
    /* R0:R1 = a*d → write to dest imag. */
    fp_mop_writeback_result(&d_imag, 1);

    ot_check(th_add_imm(R_SP, R_SP, 8, flags_safe(), ENFORCE_ENCODING_NONE));
  }
  else if (s1_complex && !s2_complex)
  {
    /* complex double × scalar double: (a+bi) * c = ac + (bc)i */
    MachineOperand s1_real = mach_make_complex_real(&src1);
    MachineOperand s1_imag = mach_make_complex_imag(&src1);

    /* Allocate 8 bytes to save the scalar 'c'. */
    ot_check(th_sub_imm(R_SP, R_SP, 8, flags_safe(), ENFORCE_ENCODING_NONE));

    /* Load scalar 'c' into R0:R1 and save to stack. */
    fp_mop_load_double_arg(R0, R1, &src2);
    fp_mop_save_double_to_sp(0);

    /* Compute a * c. */
    fp_mop_load_double_arg(R0, R1, &s1_real);
    fp_mop_load_double_arg(R2, R3, &src2);
    fp_mop_do_bl("__aeabi_dmul");
    fp_mop_writeback_result(&d_real, 1);

    /* Compute b * c: reload 'c', load 'b'. */
    fp_mop_load_double_arg(R0, R1, &s1_imag);
    fp_mop_load_double_from_sp(R2, R3, 0);
    fp_mop_do_bl("__aeabi_dmul");
    fp_mop_writeback_result(&d_imag, 1);

    ot_check(th_add_imm(R_SP, R_SP, 8, flags_safe(), ENFORCE_ENCODING_NONE));
  }
  else
  {
    /* complex × complex: (a+bi)*(c+di) = (ac-bd) + (ad+bc)i
     *
     * Stack layout (48 bytes):
     *   [sp+40] = d  (imag of src2, 8 bytes)
     *   [sp+32] = c  (real of src2, 8 bytes)
     *   [sp+24] = b  (imag of src1, 8 bytes)
     *   [sp+16] = a  (real of src1, 8 bytes)
     *   [sp+8]  = scratch1 (8 bytes)
     *   [sp+0]  = scratch0 (8 bytes)
     */
    MachineOperand s1_real = mach_make_complex_real(&src1);
    MachineOperand s1_imag = mach_make_complex_imag(&src1);
    MachineOperand s2_real = mach_make_complex_real(&src2);
    MachineOperand s2_imag = mach_make_complex_imag(&src2);

    const int off_scratch0 = 0, off_scratch1 = 8;
    const int off_a = 16, off_b = 24, off_c = 32, off_d = 40;

    ot_check(th_sub_imm(R_SP, R_SP, 48, flags_safe(), ENFORCE_ENCODING_NONE));

    /* Save all 4 components to stack. */
    fp_mop_load_double_arg(R0, R1, &s1_real);
    fp_mop_save_double_to_sp(off_a);
    fp_mop_load_double_arg(R0, R1, &s1_imag);
    fp_mop_save_double_to_sp(off_b);
    fp_mop_load_double_arg(R0, R1, &s2_real);
    fp_mop_save_double_to_sp(off_c);
    fp_mop_load_double_arg(R0, R1, &s2_imag);
    fp_mop_save_double_to_sp(off_d);

    /* Step 1: ac → scratch0. */
    fp_mop_load_double_from_sp(R0, R1, off_a);
    fp_mop_load_double_from_sp(R2, R3, off_c);
    fp_mop_do_bl("__aeabi_dmul");
    fp_mop_save_double_to_sp(off_scratch0);

    /* Step 2: bd → scratch1. */
    fp_mop_load_double_from_sp(R0, R1, off_b);
    fp_mop_load_double_from_sp(R2, R3, off_d);
    fp_mop_do_bl("__aeabi_dmul");
    fp_mop_save_double_to_sp(off_scratch1);

    /* Step 3: real = ac - bd → scratch0. */
    fp_mop_load_double_from_sp(R0, R1, off_scratch0);
    fp_mop_load_double_from_sp(R2, R3, off_scratch1);
    fp_mop_do_bl("__aeabi_dsub");
    fp_mop_save_double_to_sp(off_scratch0);

    /* Step 4: ad → scratch1. */
    fp_mop_load_double_from_sp(R0, R1, off_a);
    fp_mop_load_double_from_sp(R2, R3, off_d);
    fp_mop_do_bl("__aeabi_dmul");
    fp_mop_save_double_to_sp(off_scratch1);

    /* Step 5: bc → off_a (reuse slot). */
    fp_mop_load_double_from_sp(R0, R1, off_b);
    fp_mop_load_double_from_sp(R2, R3, off_c);
    fp_mop_do_bl("__aeabi_dmul");
    fp_mop_save_double_to_sp(off_a);

    /* Step 6: imag = ad + bc → scratch1. */
    fp_mop_load_double_from_sp(R0, R1, off_scratch1);
    fp_mop_load_double_from_sp(R2, R3, off_a);
    fp_mop_do_bl("__aeabi_dadd");
    fp_mop_save_double_to_sp(off_scratch1);

    /* Write results back to dest. */
    fp_mop_load_double_from_sp(R0, R1, off_scratch0);
    fp_mop_writeback_result(&d_real, 1);
    fp_mop_load_double_from_sp(R0, R1, off_scratch1);
    fp_mop_writeback_result(&d_imag, 1);

    ot_check(th_add_imm(R_SP, R_SP, 48, flags_safe(), ENFORCE_ENCODING_NONE));
  }
}

/* complex_pair_writeback: Write a (real, imag) pair from two physical registers
 * into a split MachineOperand pair without clobbering.
 * Handles the case where d_lo's target register overlaps hi_reg (or vice versa)
 * by saving the clobbered value to R2 or R3 first. */
static void complex_pair_writeback(MachineOperand *d_lo, int lo_reg, MachineOperand *d_hi, int hi_reg)
{
  int lo_clobbers_hi = (d_lo->kind == MACH_OP_REG && d_lo->u.reg.r0 == hi_reg);
  int hi_clobbers_lo = (d_hi->kind == MACH_OP_REG && d_hi->u.reg.r0 == lo_reg);

  if (lo_clobbers_hi && hi_clobbers_lo)
  {
    /* Total swap: save hi to temp, then write both */
    int tmp = (lo_reg != R2 && hi_reg != R2) ? R2 : R3;
    ot_check_mov_reg((uint32_t)tmp, (uint32_t)hi_reg, flags_safe(), THUMB_SHIFT_DEFAULT,
                     ENFORCE_ENCODING_NONE, false);
    mach_writeback_dest(d_lo, lo_reg);
    mach_writeback_dest(d_hi, tmp);
  }
  else if (lo_clobbers_hi)
  {
    /* Lo writeback would clobber hi value; save hi first */
    int tmp = (lo_reg != R2 && hi_reg != R2) ? R2 : R3;
    ot_check_mov_reg((uint32_t)tmp, (uint32_t)hi_reg, flags_safe(), THUMB_SHIFT_DEFAULT,
                     ENFORCE_ENCODING_NONE, false);
    mach_writeback_dest(d_lo, lo_reg);
    mach_writeback_dest(d_hi, tmp);
  }
  else if (hi_clobbers_lo)
  {
    /* Hi writeback would clobber lo value; write lo first */
    mach_writeback_dest(d_lo, lo_reg);
    mach_writeback_dest(d_hi, hi_reg);
  }
  else
  {
    mach_writeback_dest(d_lo, lo_reg);
    mach_writeback_dest(d_hi, hi_reg);
  }
}

/* thumb_emit_dcp_addsub_mop: inline double add/subtract on RP2350's DCP.
 *
 * The sequence is the one in lib/fp/arm/rp2350/dcp_aeabi.S, itself transcribed
 * from pico-sdk's dcp_canned.inc.S -- keep the two in step:
 *
 *   WXUP a ; WYUP b ; ADD0 ; ADD1|SUB1 ; NRDD ; RDDA|RDDS
 *
 * Six instructions, no scratch registers, and -- unlike the library form --
 * on whichever registers the allocator already chose.  `mcrr`/`mrrc` move a
 * GPR *pair* but place no consecutiveness constraint on it (that is LDRD's
 * rule, not theirs), so any two distinct registers work and nothing has to be
 * shuffled into r0-r3 first.
 *
 * Overwriting a source register with the result is safe: WXUP/WYUP have
 * already copied both operands into the coprocessor by the time RDDA/RDDS
 * writes back.  The destination is still excluded from source allocation
 * because the shared 64-bit idiom does that, not because it is required.
 */
static void thumb_emit_dcp_addsub_mop(MachineOperand src1, MachineOperand src2, MachineOperand dest, int is_sub)
{
  MachineCodegenContext mctx = {0};
  uint32_t excl = 0;

  /* Destination pair first, so deref resolution never allocates a scratch that
   * overlaps it and then restores over the result (same reason as the integer
   * 64-bit path). */
  int rd_lo, rd_hi;
  bool store_lo = false, store_hi = false;
  if (dest.kind == MACH_OP_REG && !dest.needs_deref && dest.u.reg.r0 != (int)PREG_REG_NONE && dest.u.reg.r1 >= 0)
  {
    rd_lo = dest.u.reg.r0;
    rd_hi = dest.u.reg.r1;
    excl |= (1u << (uint32_t)rd_lo) | (1u << (uint32_t)rd_hi);
  }
  else
  {
    rd_lo = mach_alloc_scratch(&mctx, excl);
    excl |= (1u << (uint32_t)rd_lo);
    rd_hi = mach_alloc_scratch(&mctx, excl);
    excl |= (1u << (uint32_t)rd_hi);
    store_lo = store_hi = (dest.kind != MACH_OP_NONE);
  }

  /* Pre-exclude both sources' registers so resolving one deref cannot steal
   * the physical registers of the other. */
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

  MachineOperand a = mach_resolve_deref_64(&mctx, &src1, &excl);
  MachineOperand b = mach_resolve_deref_64(&mctx, &src2, &excl);

  int a_lo, a_hi, b_lo, b_hi;
  mach_ensure_pair_in_regs(&mctx, &a, &excl, &a_lo, &a_hi);
  mach_ensure_pair_in_regs(&mctx, &b, &excl, &b_lo, &b_hi);

  ot_check(th_mcrr(4, 1, (uint32_t)a_lo, (uint32_t)a_hi, 0, 0)); /* WXUP a */
  ot_check(th_mcrr(4, 1, (uint32_t)b_lo, (uint32_t)b_hi, 1, 0)); /* WYUP b */
  ot_check(th_cdp(4, 0, 0, 0, 1, 0, 0));                         /* ADD0 */
  ot_check(th_cdp(4, 1, 0, 0, 1, is_sub ? 1 : 0, 0));            /* SUB1 : ADD1 */
  ot_check(th_cdp(4, 8, 0, 0, 0, 1, 0));                         /* NRDD */
  ot_check(th_mrrc(4, is_sub ? 3 : 1, (uint32_t)rd_lo, (uint32_t)rd_hi, 0, 0)); /* RDDS : RDDA */

  if (store_lo)
  {
    MachineOperand dst_lo = mach_make_lo_half(&dest);
    dst_lo.btype = IROP_BTYPE_INT32;
    mach_writeback_dest(&dst_lo, rd_lo);
  }
  if (store_hi)
  {
    MachineOperand dst_hi = mach_make_hi_half(&dest);
    dst_hi.btype = IROP_BTYPE_INT32;
    mach_writeback_dest(&dst_hi, rd_hi);
  }
  mach_release_all(&mctx);
}

/* thumb_emit_vfp_arith_mop: inline single-precision arithmetic on the FPU.
 *
 *   vmov s0, rn ; vmov s1, rm ; v<op>.f32 s0, s0, s1 ; vmov rd, s0
 *
 * Four instructions against a BL plus the library's own five.  s0/s1 are used
 * as fixed scratch rather than allocated: this is the softfp ABI, so no float
 * value ever lives in a VFP register between operations and s0-s15 are free at
 * every point.  That is also what keeps this independent of Phase 5 proper --
 * making floats *live* in s0-s15 needs a register class and th_vldr/th_vstr
 * (which do not exist yet) for spill and reload.
 *
 * The values move through GPRs on both sides, so nothing here depends on the
 * float ABI and fast-mode objects stay link-compatible with portable ones.
 */
/* Get a float operand into a single-precision register: use it directly when it
 * already lives in one (MACH_OP_VFP_REG), else materialize it through a GPR into
 * the caller-provided scratch s-register. */
static int vfp_operand_to_sreg(MachineCodegenContext *ctx, const MachineOperand *op, int scratch_s)
{
  if (op->kind == MACH_OP_VFP_REG)
    return op->u.reg.r0;
  int gpr = mach_ensure_in_reg(ctx, op, 0);
  ot_check(th_vmov_gp_sp((uint16_t)gpr, (uint16_t)scratch_s, 0)); /* scratch_s = gpr */
  return scratch_s;
}

static void thumb_emit_vfp_arith_mop(MachineOperand src1, MachineOperand src2, MachineOperand dest, TccIrOp op)
{
  MachineCodegenContext ctx = {0};

  /* Hard-float: operate on real single-precision registers.  VFP-resident
   * operands/dest are used directly (v<op>.f32 sd, sn, sm); GPR/imm/spill
   * endpoints are materialized through the reserved s14/s15 scratch. */
  if (tcc_state && tcc_state->float_abi == ARM_HARD_FLOAT)
  {
    int sn = vfp_operand_to_sreg(&ctx, &src1, VFP_SCRATCH0);
    int sm = vfp_operand_to_sreg(&ctx, &src2, VFP_SCRATCH1);
    int sd = (dest.kind == MACH_OP_VFP_REG) ? dest.u.reg.r0 : VFP_SCRATCH0;
    switch (op)
    {
    case TCCIR_OP_FADD: ot_check(th_vadd_f((uint16_t)sd, (uint16_t)sn, (uint16_t)sm, 0)); break;
    case TCCIR_OP_FSUB: ot_check(th_vsub_f((uint16_t)sd, (uint16_t)sn, (uint16_t)sm, 0)); break;
    case TCCIR_OP_FMUL: ot_check(th_vmul_f((uint16_t)sd, (uint16_t)sn, (uint16_t)sm, 0)); break;
    case TCCIR_OP_FDIV: ot_check(th_vdiv_f((uint16_t)sd, (uint16_t)sn, (uint16_t)sm, 0)); break;
    default: tcc_error("compiler_error: thumb_emit_vfp_arith_mop: unhandled op %d", (int)op); break;
    }
    if (dest.kind != MACH_OP_VFP_REG)
    {
      int rd = mach_get_dest_reg(&ctx, &dest, 0);
      ot_check(th_vmov_gp_sp((uint16_t)rd, (uint16_t)sd, 1)); /* rd = sd */
      mach_writeback_dest(&dest, rd);
    }
    mach_release_all(&ctx);
    return;
  }

  /* Softfp: no float ever lives in a VFP register, so s0/s1 are free fixed
   * scratch.  Values move through GPRs on both sides — kept byte-identical. */
  int rd = mach_get_dest_reg(&ctx, &dest, 0);
  uint32_t excl = (1u << (uint32_t)rd);
  int rn = mach_ensure_in_reg(&ctx, &src1, excl);
  if (thumb_is_hw_reg(rn))
    excl |= (1u << (uint32_t)rn);
  int rm = mach_ensure_in_reg(&ctx, &src2, excl);

  ot_check(th_vmov_gp_sp((uint16_t)rn, 0, 0)); /* s0 = rn */
  ot_check(th_vmov_gp_sp((uint16_t)rm, 1, 0)); /* s1 = rm */
  switch (op)
  {
  case TCCIR_OP_FADD:
    ot_check(th_vadd_f(0, 0, 1, 0));
    break;
  case TCCIR_OP_FSUB:
    ot_check(th_vsub_f(0, 0, 1, 0));
    break;
  case TCCIR_OP_FMUL:
    ot_check(th_vmul_f(0, 0, 1, 0));
    break;
  case TCCIR_OP_FDIV:
    ot_check(th_vdiv_f(0, 0, 1, 0));
    break;
  default:
    tcc_error("compiler_error: thumb_emit_vfp_arith_mop: unhandled op %d", (int)op);
    break;
  }
  ot_check(th_vmov_gp_sp((uint16_t)rd, 0, 1)); /* rd = s0 */

  mach_writeback_dest(&dest, rd);
  mach_release_all(&ctx);
}

/* thumb_emit_dcp_cmp_mop: inline double compare on RP2350's DCP.
 *
 *   WXUP a ; WYUP b ; ADD0 ; RCMP apsr_nzcv
 *
 * Four instructions and no result register: RCMP with Rt == PC writes the
 * relation directly into the flags, so the compare feeds an ordinary
 * conditional branch instead of a call plus flag decode.
 *
 * The flag encoding is the AEABI's -- C set means "ordered and >=", Z set
 * means equal, V set means unordered -- and is read with *unsigned*
 * conditions.  ir/gen/float.c already mirrored the operands and recorded
 * TOK_UGT / TOK_UGE accordingly, so this only has to compare in operand
 * order.
 *
 * The RCMP is emitted LAST, after mach_release_all(): releasing can restore
 * saved scratch registers, and the flags must be the final thing written
 * before the branch consumes them.  Splitting ADD0 from RCMP is safe because
 * the DCP holds its state across unrelated core instructions -- that is the
 * same property __rp2350_dcp_save/_restore rely on.
 */
static void thumb_emit_dcp_cmp_mop(MachineOperand src1, MachineOperand src2)
{
  MachineCodegenContext mctx = {0};
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

  MachineOperand a = mach_resolve_deref_64(&mctx, &src1, &excl);
  MachineOperand b = mach_resolve_deref_64(&mctx, &src2, &excl);

  int a_lo, a_hi, b_lo, b_hi;
  mach_ensure_pair_in_regs(&mctx, &a, &excl, &a_lo, &a_hi);
  mach_ensure_pair_in_regs(&mctx, &b, &excl, &b_lo, &b_hi);

  ot_check(th_mcrr(4, 1, (uint32_t)a_lo, (uint32_t)a_hi, 0, 0)); /* WXUP a */
  ot_check(th_mcrr(4, 1, (uint32_t)b_lo, (uint32_t)b_hi, 1, 0)); /* WYUP b */
  ot_check(th_cdp(4, 0, 0, 0, 1, 0, 0));                         /* ADD0 */

  mach_release_all(&mctx);

  ot_check(th_mrc(4, 0, R_PC, 0, 0, 1, 0)); /* RCMP apsr_nzcv */
}

/* Process complex double addition/subtraction via MachineOperands.
 * (a+bi) + (c+di) = (a+c) + (b+d)i
 * (a+bi) - (c+di) = (a-c) + (b-d)i
 * Uses __aeabi_dadd/__aeabi_dsub for double-precision.
 * Double AEABI calling convention: R0:R1 = arg1, R2:R3 = arg2, result in R0:R1.
 */
static void thumb_process_complex_op_double_mop(MachineOperand src1, MachineOperand src2, MachineOperand dest,
                                                TccIrOp op)
{
  const int is_add = (op == TCCIR_OP_ADD);
  const char *func_name = is_add ? "__aeabi_dadd" : "__aeabi_dsub";

  MachineOperand s1_real = mach_make_complex_real(&src1);
  MachineOperand s1_imag = mach_make_complex_imag(&src1);
  MachineOperand s2_real = mach_make_complex_real(&src2);
  MachineOperand s2_imag = mach_make_complex_imag(&src2);
  MachineOperand d_real = mach_make_complex_real(&dest);
  MachineOperand d_imag = mach_make_complex_imag(&dest);

  /* Stack layout (32 bytes):
   *   [sp+24] = s2_imag (8 bytes)
   *   [sp+16] = s2_real (8 bytes)
   *   [sp+8]  = s1_imag (8 bytes)
   *   [sp+0]  = s1_real (8 bytes)
   */
  ot_check(th_sub_imm(R_SP, R_SP, 32, flags_safe(), ENFORCE_ENCODING_NONE));

  /* Save all 4 components to stack. */
  fp_mop_load_double_arg(R0, R1, &s1_real);
  fp_mop_save_double_to_sp(0);
  fp_mop_load_double_arg(R0, R1, &s1_imag);
  fp_mop_save_double_to_sp(8);
  fp_mop_load_double_arg(R0, R1, &s2_real);
  fp_mop_save_double_to_sp(16);
  fp_mop_load_double_arg(R0, R1, &s2_imag);
  fp_mop_save_double_to_sp(24);

  /* Compute real part: func(a.real, b.real) */
  fp_mop_load_double_from_sp(R0, R1, 0);
  fp_mop_load_double_from_sp(R2, R3, 16);
  fp_mop_do_bl(func_name);
  /* Save real result to stack slot 0 */
  fp_mop_save_double_to_sp(0);

  /* Compute imag part: func(a.imag, b.imag) */
  fp_mop_load_double_from_sp(R0, R1, 8);
  fp_mop_load_double_from_sp(R2, R3, 24);
  fp_mop_do_bl(func_name);
  /* R0:R1 = imag result. Load real result from stack. */
  fp_mop_save_double_to_sp(8); /* save imag to slot 8 */

  /* Write results back to dest. */
  fp_mop_load_double_from_sp(R0, R1, 0);
  fp_mop_writeback_result(&d_real, 1);
  fp_mop_load_double_from_sp(R0, R1, 8);
  fp_mop_writeback_result(&d_imag, 1);

  ot_check(th_add_imm(R_SP, R_SP, 32, flags_safe(), ENFORCE_ENCODING_NONE));
}

/* Process complex addition/subtraction via MachineOperands.
 * (a+bi) + (c+di) = (a+c) + (b+d)i
 * (a+bi) - (c+di) = (a-c) + (b-d)i
 */
static void thumb_process_complex_op_mop(MachineOperand src1, MachineOperand src2, MachineOperand dest, TccIrOp op)
{
  const int is_add = (op == TCCIR_OP_ADD);

  /* Complex float: each component is a 32-bit float. */
  const char *func_name = is_add ? "__aeabi_fadd" : "__aeabi_fsub";

  /* Split into real/imag components. */
  MachineOperand s1_real = mach_make_lo_half(&src1);
  MachineOperand s1_imag = mach_make_hi_half(&src1);
  MachineOperand s2_real = mach_make_lo_half(&src2);
  MachineOperand s2_imag = mach_make_hi_half(&src2);

  /* Stack-based: save all 4 inputs, do calls, write results back.
   * Stack layout (16 bytes):
   *   [sp+12] = s2_imag
   *   [sp+8]  = s2_real
   *   [sp+4]  = s1_imag
   *   [sp+0]  = s1_real
   */
  ot_check(th_sub_imm(R_SP, R_SP, 16, flags_safe(), ENFORCE_ENCODING_NONE));

  /* Load and save each component to stack. */
  fp_mop_load_arg(R0, &s1_real);
  ot_check_str_imm(R0, R_SP, 0, 6, ENFORCE_ENCODING_NONE);
  fp_mop_load_arg(R0, &s1_imag);
  ot_check_str_imm(R0, R_SP, 4, 6, ENFORCE_ENCODING_NONE);
  fp_mop_load_arg(R0, &s2_real);
  ot_check_str_imm(R0, R_SP, 8, 6, ENFORCE_ENCODING_NONE);
  fp_mop_load_arg(R0, &s2_imag);
  ot_check_str_imm(R0, R_SP, 12, 6, ENFORCE_ENCODING_NONE);

  /* Compute real part: func(a.real, b.real) */
  ot_check_ldr_imm(R0, R_SP, 0, 6, ENFORCE_ENCODING_NONE);
  ot_check_ldr_imm(R1, R_SP, 8, 6, ENFORCE_ENCODING_NONE);
  fp_mop_do_bl(func_name);
  /* Save real result to stack slot 0 */
  ot_check_str_imm(R0, R_SP, 0, 6, ENFORCE_ENCODING_NONE);

  /* Compute imag part: func(a.imag, b.imag) */
  ot_check_ldr_imm(R0, R_SP, 4, 6, ENFORCE_ENCODING_NONE);
  ot_check_ldr_imm(R1, R_SP, 12, 6, ENFORCE_ENCODING_NONE);
  fp_mop_do_bl(func_name);
  /* R0 = imag result */

  /* Load real result from stack, deallocate, write back. */
  MachineOperand d_real = mach_make_lo_half(&dest);
  MachineOperand d_imag = mach_make_hi_half(&dest);

  /* R0 = imag result.  Load real result from stack into R1. */
  ot_check_ldr_imm(R1, R_SP, 0, 6, ENFORCE_ENCODING_NONE);
  ot_check(th_add_imm(R_SP, R_SP, 16, flags_safe(), ENFORCE_ENCODING_NONE));

  /* Write back: R1 = real part, R0 = imag part.
   * Use safe writeback to avoid clobbering when dest overlaps R0/R1. */
  complex_pair_writeback(&d_real, R1, &d_imag, R0);
}

/* Process complex multiplication via MachineOperands.
 * (a+bi) * (c+di) = (ac-bd) + (ad+bc)i
 *
 * Stack layout (24 bytes):
 *   [sp+20] = d  (imag of src2)
 *   [sp+16] = c  (real of src2)
 *   [sp+12] = b  (imag of src1)
 *   [sp+8]  = a  (real of src1)
 *   [sp+4]  = scratch1
 *   [sp+0]  = scratch0
 */
static void thumb_process_complex_mul_mop(MachineOperand src1, MachineOperand src2, MachineOperand dest)
{
  MachineOperand s1_real = mach_make_lo_half(&src1);
  MachineOperand s1_imag = mach_make_hi_half(&src1);
  MachineOperand s2_real = mach_make_lo_half(&src2);
  MachineOperand s2_imag = mach_make_hi_half(&src2);

  /* Allocate 24 bytes on stack */
  ot_check(th_sub_imm(R_SP, R_SP, 24, flags_safe(), ENFORCE_ENCODING_NONE));

  /* Save inputs to stack */
  fp_mop_load_arg(R0, &s1_real);
  ot_check_str_imm(R0, R_SP, 8, 6, ENFORCE_ENCODING_NONE); /* a */
  fp_mop_load_arg(R0, &s1_imag);
  ot_check_str_imm(R0, R_SP, 12, 6, ENFORCE_ENCODING_NONE); /* b */
  fp_mop_load_arg(R0, &s2_real);
  ot_check_str_imm(R0, R_SP, 16, 6, ENFORCE_ENCODING_NONE); /* c */
  fp_mop_load_arg(R0, &s2_imag);
  ot_check_str_imm(R0, R_SP, 20, 6, ENFORCE_ENCODING_NONE); /* d */

  const int off_scratch0 = 0;
  const int off_scratch1 = 4;
  const int off_a = 8;
  const int off_b = 12;
  const int off_c = 16;
  const int off_d = 20;

  /* Step 1: ac = a * c → scratch0 */
  ot_check_ldr_imm(R0, R_SP, off_a, 6, ENFORCE_ENCODING_NONE);
  ot_check_ldr_imm(R1, R_SP, off_c, 6, ENFORCE_ENCODING_NONE);
  fp_mop_do_bl("__aeabi_fmul");
  ot_check_str_imm(R0, R_SP, off_scratch0, 6, ENFORCE_ENCODING_NONE);

  /* Step 2: bd = b * d → scratch1 */
  ot_check_ldr_imm(R0, R_SP, off_b, 6, ENFORCE_ENCODING_NONE);
  ot_check_ldr_imm(R1, R_SP, off_d, 6, ENFORCE_ENCODING_NONE);
  fp_mop_do_bl("__aeabi_fmul");
  ot_check_str_imm(R0, R_SP, off_scratch1, 6, ENFORCE_ENCODING_NONE);

  /* Step 3: real = ac - bd → scratch0 */
  ot_check_ldr_imm(R0, R_SP, off_scratch0, 6, ENFORCE_ENCODING_NONE);
  ot_check_ldr_imm(R1, R_SP, off_scratch1, 6, ENFORCE_ENCODING_NONE);
  fp_mop_do_bl("__aeabi_fsub");
  ot_check_str_imm(R0, R_SP, off_scratch0, 6, ENFORCE_ENCODING_NONE);

  /* Step 4: ad = a * d → scratch1 */
  ot_check_ldr_imm(R0, R_SP, off_a, 6, ENFORCE_ENCODING_NONE);
  ot_check_ldr_imm(R1, R_SP, off_d, 6, ENFORCE_ENCODING_NONE);
  fp_mop_do_bl("__aeabi_fmul");
  ot_check_str_imm(R0, R_SP, off_scratch1, 6, ENFORCE_ENCODING_NONE);

  /* Step 5: bc = b * c → off_a (no longer needed) */
  ot_check_ldr_imm(R0, R_SP, off_b, 6, ENFORCE_ENCODING_NONE);
  ot_check_ldr_imm(R1, R_SP, off_c, 6, ENFORCE_ENCODING_NONE);
  fp_mop_do_bl("__aeabi_fmul");
  ot_check_str_imm(R0, R_SP, off_a, 6, ENFORCE_ENCODING_NONE);

  /* Step 6: imag = ad + bc → scratch1 */
  ot_check_ldr_imm(R0, R_SP, off_scratch1, 6, ENFORCE_ENCODING_NONE);
  ot_check_ldr_imm(R1, R_SP, off_a, 6, ENFORCE_ENCODING_NONE);
  fp_mop_do_bl("__aeabi_fadd");
  ot_check_str_imm(R0, R_SP, off_scratch1, 6, ENFORCE_ENCODING_NONE);

  /* Load results and write back */
  MachineOperand d_real = mach_make_lo_half(&dest);
  MachineOperand d_imag = mach_make_hi_half(&dest);
  ot_check_ldr_imm(R0, R_SP, off_scratch0, 6, ENFORCE_ENCODING_NONE); /* real */
  ot_check_ldr_imm(R1, R_SP, off_scratch1, 6, ENFORCE_ENCODING_NONE); /* imag */
  ot_check(th_add_imm(R_SP, R_SP, 24, flags_safe(), ENFORCE_ENCODING_NONE));

  complex_pair_writeback(&d_real, R0, &d_imag, R1);
}

/* Process complex float division via MachineOperands.
 * Calls __divsc3 from libgcc for numerically robust division.
 *
 * __divsc3 calling convention (soft-float AAPCS, hidden return pointer):
 *   R0       = hidden return pointer (8-byte buffer for result)
 *   R1       = a_real (first float arg)
 *   R2       = a_imag (second float arg)
 *   R3       = b_real (third float arg)
 *   [sp+0]   = b_imag (fourth float arg, on stack)
 *   Result written to [R0+0..3] = real, [R0+4..7] = imag
 *
 * Stack layout (16 bytes):
 *   [sp+0]   = b_imag for __divsc3 stack arg  (4 bytes)
 *   [sp+4]   = padding                        (4 bytes)
 *   [sp+8]   = result buffer: real part        (4 bytes)
 *   [sp+12]  = result buffer: imag part        (4 bytes)
 */
static void thumb_process_complex_div_mop(MachineOperand src1, MachineOperand src2, MachineOperand dest)
{
  MachineOperand s1_real = mach_make_lo_half(&src1);
  MachineOperand s1_imag = mach_make_hi_half(&src1);
  MachineOperand s2_real = mach_make_lo_half(&src2);
  MachineOperand s2_imag = mach_make_hi_half(&src2);

  /* In PIC mode, save {r9, r12} BEFORE allocating the call frame so that
   * SP-relative offsets within the 16-byte area remain correct when
   * __divsc3 reads its stack arg at [sp+0]. */
  if (text_and_data_separation)
    ot_check(th_push((uint16_t)((1 << R9) | (1 << R12))));

  /* Allocate 16 bytes on stack. */
  ot_check(th_sub_imm(R_SP, R_SP, 16, flags_safe(), ENFORCE_ENCODING_NONE));

  /* Save all inputs to stack first to avoid register clobbering. */
  fp_mop_load_arg(R0, &s1_real);
  ot_check_str_imm(R0, R_SP, 0, 6, ENFORCE_ENCODING_NONE); /* a_real */
  fp_mop_load_arg(R0, &s1_imag);
  ot_check_str_imm(R0, R_SP, 4, 6, ENFORCE_ENCODING_NONE); /* a_imag */
  fp_mop_load_arg(R0, &s2_real);
  ot_check_str_imm(R0, R_SP, 8, 6, ENFORCE_ENCODING_NONE); /* b_real */
  fp_mop_load_arg(R0, &s2_imag);
  ot_check_str_imm(R0, R_SP, 12, 6, ENFORCE_ENCODING_NONE); /* b_imag */

  /* Rearrange stack for __divsc3 call:
   * Need [sp+0] = b_imag, [sp+8..15] = result buffer.
   * Currently [sp+0]=a_real, [sp+4]=a_imag, [sp+8]=b_real, [sp+12]=b_imag.
   * Load R1-R3 from stack, then rearrange. */
  ot_check_ldr_imm(R1, R_SP, 0, 6, ENFORCE_ENCODING_NONE);  /* R1 = a_real */
  ot_check_ldr_imm(R2, R_SP, 4, 6, ENFORCE_ENCODING_NONE);  /* R2 = a_imag */
  ot_check_ldr_imm(R3, R_SP, 8, 6, ENFORCE_ENCODING_NONE);  /* R3 = b_real */
  ot_check_ldr_imm(R0, R_SP, 12, 6, ENFORCE_ENCODING_NONE); /* R0 = b_imag */
  ot_check_str_imm(R0, R_SP, 0, 6, ENFORCE_ENCODING_NONE);  /* [sp+0] = b_imag (stack arg) */

  /* R0 = pointer to result buffer at [sp+8]. */
  ot_check(th_add_imm(R0, R_SP, 8, flags_safe(), ENFORCE_ENCODING_NONE));

  /* Call __divsc3 (directly, not via fp_mop_do_bl which would add another
   * push/pop of {r9, r12} and corrupt the stack arg layout). */
  {
    Sym *sym = external_global_sym(tok_alloc_const("__divsc3"), &func_old_type);
    MachineOperand func_mop = {0};
    func_mop.kind = MACH_OP_SYMBOL;
    func_mop.u.sym.sym = sym;
    func_mop.u.sym.addend = 0;
    gcall_or_jump_mop(0, func_mop);
  }

  /* Read result from buffer and write back to dest. */
  MachineOperand d_real = mach_make_lo_half(&dest);
  MachineOperand d_imag = mach_make_hi_half(&dest);
  ot_check_ldr_imm(R0, R_SP, 8, 6, ENFORCE_ENCODING_NONE);  /* real */
  ot_check_ldr_imm(R1, R_SP, 12, 6, ENFORCE_ENCODING_NONE); /* imag */
  ot_check(th_add_imm(R_SP, R_SP, 16, flags_safe(), ENFORCE_ENCODING_NONE));

  if (text_and_data_separation)
    ot_check(th_pop((uint16_t)((1 << R9) | (1 << R12))));

  complex_pair_writeback(&d_real, R0, &d_imag, R1);
}

/* Process complex double division via MachineOperands.
 * Calls __divdc3 from libgcc for numerically robust division.
 *
 * __divdc3 calling convention (soft-float AAPCS, hidden return pointer):
 *   R0       = hidden return pointer (16-byte buffer for result)
 *   R2:R3    = a_re (first double, even-aligned)
 *   [sp+0]   = a_im (second double, on stack)
 *   [sp+8]   = b_re (third double, on stack)
 *   [sp+16]  = b_im (fourth double, on stack)
 *   Result written to [R0+0..7] = real, [R0+8..15] = imag
 *
 * Stack layout (40 bytes, 8-byte aligned):
 *   [sp+0]   = a_im for __divdc3 stack arg  (8 bytes)
 *   [sp+8]   = b_re for __divdc3 stack arg  (8 bytes)
 *   [sp+16]  = b_im for __divdc3 stack arg  (8 bytes)
 *   [sp+24]  = result buffer: real part      (8 bytes)
 *   [sp+32]  = result buffer: imag part      (8 bytes)
 */
static void thumb_process_complex_div_double_mop(MachineOperand src1, MachineOperand src2, MachineOperand dest)
{
  MachineOperand s1_real = mach_make_complex_real(&src1);
  MachineOperand s1_imag = mach_make_complex_imag(&src1);
  MachineOperand s2_real = mach_make_complex_real(&src2);
  MachineOperand s2_imag = mach_make_complex_imag(&src2);
  MachineOperand d_real = mach_make_complex_real(&dest);
  MachineOperand d_imag = mach_make_complex_imag(&dest);

  /* In PIC mode, save {r9, r12} BEFORE allocating the call frame so that
   * SP-relative offsets within the 40-byte area remain correct when
   * __divdc3 reads its stack args at [sp+0..23]. */
  if (text_and_data_separation)
    ot_check(th_push((uint16_t)((1 << R9) | (1 << R12))));

  /* Allocate 40 bytes (8-byte aligned). */
  ot_check(th_sub_imm(R_SP, R_SP, 40, flags_safe(), ENFORCE_ENCODING_NONE));

  /* Set up __divdc3 stack args (must be at lowest sp offsets). */
  /* [sp+16] = b_im (src2 imag). */
  fp_mop_load_double_arg(R0, R1, &s2_imag);
  fp_mop_save_double_to_sp(16);
  /* [sp+8] = b_re (src2 real). */
  fp_mop_load_double_arg(R0, R1, &s2_real);
  fp_mop_save_double_to_sp(8);
  /* [sp+0] = a_im (src1 imag). */
  fp_mop_load_double_arg(R0, R1, &s1_imag);
  fp_mop_save_double_to_sp(0);

  /* R2:R3 = a_re (src1 real) — first double arg in even register pair. */
  fp_mop_load_double_arg(R2, R3, &s1_real);

  /* R0 = pointer to result buffer at [sp+24]. */
  ot_check(th_add_imm(R0, R_SP, 24, flags_safe(), ENFORCE_ENCODING_NONE));

  /* Call __divdc3 (directly, not via fp_mop_do_bl which would add another
   * push/pop of {r9, r12} and corrupt the stack arg layout). */
  {
    Sym *sym = external_global_sym(tok_alloc_const("__divdc3"), &func_old_type);
    MachineOperand func_mop = {0};
    func_mop.kind = MACH_OP_SYMBOL;
    func_mop.u.sym.sym = sym;
    func_mop.u.sym.addend = 0;
    gcall_or_jump_mop(0, func_mop);
  }

  /* Read result from buffer and write back to dest. */
  fp_mop_load_double_from_sp(R0, R1, 24);
  fp_mop_writeback_result(&d_real, 1);
  fp_mop_load_double_from_sp(R0, R1, 32);
  fp_mop_writeback_result(&d_imag, 1);

  ot_check(th_add_imm(R_SP, R_SP, 40, flags_safe(), ENFORCE_ENCODING_NONE));

  if (text_and_data_separation)
    ot_check(th_pop((uint16_t)((1 << R9) | (1 << R12))));
}

/* tcc_gen_machine_fp_mop: MachineOperand-based entry point for floating-point
 * operations via soft-float EABI library calls.
 * Handles single-precision, double-precision, and complex float operations.
 *
 * Soft-float EABI calling convention (single-precision):
 *   binary arithmetic:  src1 → R0, src2 → R1, result ← R0
 *   comparison:         src1 → R0, src2 → R1, result ← CPSR flags
 *   negation:           src1 → R0, XOR sign bit, result ← R0
 *   conversion:         src1 → R0, result ← R0
 *   CVT_FTOF identity:  float32→float32, src1 → R0, dest ← R0
 */
ST_FUNC void tcc_gen_machine_fp_mop(MachineOperand src1, MachineOperand src2, MachineOperand dest, TccIrOp op,
                                    int is_complex)
{
  /* Phase 5k: handle complex float operations via MOP path. */
  if (is_complex)
  {
    /* Detect double-precision complex: any operand has FLOAT64 btype. */
    const int complex_is_double =
        (src1.btype == IROP_BTYPE_FLOAT64 || src2.btype == IROP_BTYPE_FLOAT64 || dest.btype == IROP_BTYPE_FLOAT64);
    if (op == TCCIR_OP_FADD || op == TCCIR_OP_FSUB)
    {
      if (complex_is_double)
        return thumb_process_complex_op_double_mop(src1, src2, dest, op == TCCIR_OP_FADD ? TCCIR_OP_ADD : TCCIR_OP_SUB);
      return thumb_process_complex_op_mop(src1, src2, dest, op == TCCIR_OP_FADD ? TCCIR_OP_ADD : TCCIR_OP_SUB);
    }
    else if (op == TCCIR_OP_FMUL)
    {
      if (complex_is_double)
        return thumb_process_complex_mul_double_mop(src1, src2, dest);
      return thumb_process_complex_mul_mop(src1, src2, dest);
    }
    else if (op == TCCIR_OP_FDIV)
    {
      if (complex_is_double)
        return thumb_process_complex_div_double_mop(src1, src2, dest);
      return thumb_process_complex_div_mop(src1, src2, dest);
    }
    /* Other ops (FNEG, FCMP, CVT_*) on complex types: fall through to
     * scalar path — they operate componentwise on the lo (real) half only,
     * same as regular scalars.  TODO: extend if needed. */
  }

  /* is_double: true when the primary operand is a 64-bit float (double).
   * Note: complex float has is_64bit=true (register pair), but its btype
   * is FLOAT32, so it is NOT double.  Only FLOAT64 btype is true double. */
  const int is_double = (src1.btype == IROP_BTYPE_FLOAT64) || (dest.btype == IROP_BTYPE_FLOAT64);
  const char *func_name = NULL;

  /* --- Inline double lowering, where the target has it ---
   *
   * Reaching here with a has_d* bit set means ir_put_soft_call_fpu_if_needed()
   * deliberately did NOT rewrite this operation into an __aeabi_ call, so the
   * backend owes an inline sequence.  Falling through to the call path would
   * still produce working code, but ir_op_is_implicit_call_ra() keys its
   * clobber model on the same bits -- so the two must agree or the allocator
   * stops modelling r0-r3 as dead across an operation that still calls.
   * Enable a has_d* bit and its emitter in the same commit. */
  {
    const FloatingPointConfig *fpu = architecture_config.fpu;
    if (is_double && fpu && fpu->double_impl == FP_DOUBLE_IMPL_DCP)
    {
      if (op == TCCIR_OP_FADD && fpu->has_dadd)
        return thumb_emit_dcp_addsub_mop(src1, src2, dest, 0);
      if (op == TCCIR_OP_FSUB && fpu->has_dsub)
        return thumb_emit_dcp_addsub_mop(src1, src2, dest, 1);
      if (op == TCCIR_OP_FCMP && fpu->has_dcmp)
        return thumb_emit_dcp_cmp_mop(src1, src2);
    }

    /* Single precision is plain VFP wherever it is inline at all -- there is
     * no second single-precision implementation on ARM to discriminate
     * between, so has_f* alone selects the sequence.
     *
     * -mfloat-abi=soft is the one mode that forbids it: soft means "emit no
     * FPU instructions at all", as distinct from softfp, which is FPU
     * instructions with GPR argument passing (the default here, and what
     * -mfpu=rp2350 keeps).  has_f* describes the silicon, not the ABI, so the
     * ABI has to be checked separately or `-mfloat-abi=soft` starts emitting
     * vadd.f32. */
    if (!is_double && fpu && tcc_state && tcc_state->float_abi != ARM_SOFT_FLOAT)
    {
      if ((op == TCCIR_OP_FADD && fpu->has_fadd) || (op == TCCIR_OP_FSUB && fpu->has_fsub) ||
          (op == TCCIR_OP_FMUL && fpu->has_fmul) || (op == TCCIR_OP_FDIV && fpu->has_fdiv))
        return thumb_emit_vfp_arith_mop(src1, src2, dest, op);
    }
  }

  /* --- FNEG: XOR sign bit, no BL needed --- */
  if (op == TCCIR_OP_FNEG)
  {
    ScratchRegAlloc scr;
    if (is_double)
    {
      /* f64: load pair into R0:R1, flip sign bit of hi word (R1) only */
      fp_mop_load_double_arg(R0, R1, &src1);
      scr = get_scratch_reg_with_save((1u << R0) | (1u << R1));
      load_full_const(scr.reg, PREG_NONE, 0x80000000, 0);
      ot_check(th_eor_reg(R1, R1, scr.reg, flags_safe(), THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
      restore_scratch_reg(&scr);
      fp_mop_writeback_result(&dest, 1);
    }
    else
    {
      /* f32: R0 ^= 0x80000000 */
      fp_mop_load_arg(R0, &src1);
      scr = get_scratch_reg_with_save(1u << R0);
      load_full_const(scr.reg, PREG_NONE, 0x80000000, 0);
      ot_check(th_eor_reg(R0, R0, scr.reg, flags_safe(), THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
      restore_scratch_reg(&scr);
      mach_writeback_dest(&dest, R0);
    }
    return;
  }

  /* --- CVT_FTOF: identity or f32<->f64 conversion via BL --- */
  if (op == TCCIR_OP_CVT_FTOF)
  {
    const int src_double = src1.is_64bit;
    const int dst_double = dest.is_64bit;
    if (!src_double && !dst_double)
    {
      /* f32 -> f32 identity: direct copy without going through R0 */
      tcc_gen_machine_assign_mop(src1, dest, TCCIR_OP_ASSIGN);
      return;
    }
    if (src_double && dst_double)
    {
      /* f64 -> f64 identity: direct copy without going through R0:R1.
       * Using R0:R1 as intermediaries would clobber live values in those
       * registers (e.g. function parameters in soft-float ABI). */
      tcc_gen_machine_assign_mop(src1, dest, TCCIR_OP_ASSIGN);
      return;
    }
    /* f32 -> f64: __aeabi_f2d;  f64 -> f32: __aeabi_d2f */
    {
      const char *cvt_func = src_double ? "__aeabi_d2f" : "__aeabi_f2d";
      if (src_double)
        fp_mop_load_double_arg(R0, R1, &src1);
      else
        fp_mop_load_arg(R0, &src1);
      fp_mop_do_bl(cvt_func);
      fp_mop_writeback_result(&dest, dst_double);
    }
    return;
  }

  /* --- Load operands into argument registers --- */
  if (op == TCCIR_OP_FCMP || op == TCCIR_OP_FADD || op == TCCIR_OP_FSUB || op == TCCIR_OP_FMUL || op == TCCIR_OP_FDIV)
  {
    /* Binary: src1 first arg, src2 second arg */
    if (is_double)
    {
      fp_mop_load_double_arg(R0, R1, &src1);
      fp_mop_load_double_arg(R2, R3, &src2);
    }
    else
    {
      fp_mop_load_arg(R0, &src1);
      fp_mop_load_arg(R1, &src2);
    }
  }
  else
  {
    /* Unary conversion (CVT_ITOF, CVT_FTOI): load src1 into R0 or R0:R1 */
    if (src1.is_64bit)
      fp_mop_load_double_arg(R0, R1, &src1);
    else
      fp_mop_load_arg(R0, &src1);
  }

  /* --- Determine soft-float function name --- */
  if (op == TCCIR_OP_FCMP)
  {
    func_name = is_double ? "__aeabi_cdcmple" : "__aeabi_cfcmple";
  }
  else if (op == TCCIR_OP_CVT_ITOF)
  {
    const int src64 = src1.is_64bit;
    const int dst64 = dest.is_64bit;
    if (src64 && dst64)
      func_name = src1.is_unsigned ? "__aeabi_ul2d" : "__aeabi_l2d";
    else if (src64)
      func_name = src1.is_unsigned ? "__aeabi_ul2f" : "__aeabi_l2f";
    else if (dst64)
      func_name = src1.is_unsigned ? "__aeabi_ui2d" : "__aeabi_i2d";
    else
      func_name = src1.is_unsigned ? "__aeabi_ui2f" : "__aeabi_i2f";
  }
  else if (op == TCCIR_OP_CVT_FTOI)
  {
    const int src64 = src1.is_64bit;
    const int dst64 = dest.is_64bit;
    if (src64 && dst64)
      func_name = dest.is_unsigned ? "__aeabi_d2ulz" : "__aeabi_d2lz";
    else if (src64)
      func_name = dest.is_unsigned ? "__aeabi_d2uiz" : "__aeabi_d2iz";
    else if (dst64)
      func_name = dest.is_unsigned ? "__aeabi_f2ulz" : "__aeabi_f2lz";
    else
      func_name = dest.is_unsigned ? "__aeabi_f2uiz" : "__aeabi_f2iz";
  }
  else
  {
    /* FADD, FSUB, FMUL, FDIV */
    func_name = get_softfp_func_name(op, is_double);
  }

  if (!func_name)
    tcc_error("compiler_error: tcc_gen_machine_fp_mop: no func_name for op %d", (int)op);

  fp_mop_do_bl(func_name);

  /* Write result back (FCMP sets CPSR flags only -- no register result) */
  if (op != TCCIR_OP_FCMP)
    fp_mop_writeback_result(&dest, dest.is_64bit);
}
