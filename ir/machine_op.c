/*
 *  TCC IR - Machine Operand Representation Implementation
 *
 *  Copyright (c) 2025 Mateusz Stadnik
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

#define USING_GLOBALS
#include "ir.h"
#include <stdbool.h>

/* ============================================================================
 * machine_op_from_ir: Convert a filled IROperand to a MachineOperand
 * ============================================================================
 *
 * The input operand *op must have already been processed by
 * tcc_ir_fill_registers_ir() so that pr0_reg / pr0_spilled / tag / flags all
 * reflect the final register-allocation decision.
 *
 * Decision order reflects what fill_registers_ir can produce:
 *
 *   1. Immediate constants (tag = IMM32 / F32 / I64 / F64, is_const=1)
 *      → MACH_OP_IMM: literal value in u.imm.val
 *
 *   2. Symbol references (tag = SYMREF)
 *      → MACH_OP_SYMBOL: kept only when fill_registers_ir did NOT rewrite
 *        the tag (i.e. the symbol's vreg was not allocated to a register).
 *        If the result was allocated, tag becomes IROP_TAG_VREG and falls
 *        through to the register case below.
 *
 *   3. Stack operands (tag = STACKOFF, set by fill_registers_ir for spills,
 *      locals, and stack-passed parameters):
 *        is_param=1 + is_local=1  → MACH_OP_PARAM_STACK
 *        !is_lval                  → MACH_OP_FRAME_ADDR  (LEA of local)
 *        is_lval=1 + !is_llocal   → MACH_OP_SPILL       (load from slot)
 *        is_lval=1 + is_llocal=1  → MACH_OP_SPILL with needs_deref=true
 *                                   (slot holds ptr, need extra dereference)
 *
 *   4. Register-resident operands (tag = VREG, pr0_reg != PREG_REG_NONE)
 *      → MACH_OP_REG: value is in r0 (and r1 for 64-bit pairs)
 *        needs_deref=true when is_lval: register holds an address that the
 *        backend must load through to get the actual value.
 *
 *   5. Fallback → MACH_OP_NONE (unresolved vreg, IROP_TAG_NONE, etc.)
 */
MachineOperand machine_op_from_ir(TCCIRState *ir, const IROperand *op)
{
  MachineOperand m = {0};
  m.kind = MACH_OP_NONE;

  if (!op)
    return m;

  m.btype = irop_get_btype(*op);
  m.is_unsigned = (bool)op->is_unsigned;
  m.is_64bit = (bool)irop_is_64bit(*op);
  m.vreg = (int)irop_get_vreg(*op);

  const int tag = irop_get_tag(*op);

  /* ------------------------------------------------------------------ */
  /* 1. Immediate constants                                               */
  /* ------------------------------------------------------------------ */
  if (tag == IROP_TAG_IMM32)
  {
    m.kind = MACH_OP_IMM;
    m.u.imm.val = (int64_t)irop_get_imm32(*op);
    return m;
  }
  if (tag == IROP_TAG_F32)
  {
    /* Store raw IEEE-754 bits; the backend decides how to encode them. */
    m.kind = MACH_OP_IMM;
    m.u.imm.val = (int64_t)(uint64_t)op->u.f32_bits;
    return m;
  }
  if (tag == IROP_TAG_I64 || tag == IROP_TAG_F64)
  {
    m.kind = MACH_OP_IMM;
    m.u.imm.val = irop_get_imm64_ex(ir, *op);
    return m;
  }

  /* ------------------------------------------------------------------ */
  /* 2. Symbol references (only when tag was NOT rewritten by            */
  /*    fill_registers_ir to VREG/STACKOFF)                              */
  /* ------------------------------------------------------------------ */
  if (tag == IROP_TAG_SYMREF)
  {
    m.kind = MACH_OP_SYMBOL;
    IRPoolSymref *symref = irop_get_symref_ex(ir, *op);
    if (symref)
    {
      m.u.sym.sym = symref->sym;
      m.u.sym.addend = symref->addend;
    }
    m.needs_deref = (bool)op->is_lval;
    return m;
  }

  /* ------------------------------------------------------------------ */
  /* 3. Stack operands (fill_registers_ir sets tag=STACKOFF for spills,  */
  /*    locals, and stack-passed parameters)                             */
  /* ------------------------------------------------------------------ */
  if (tag == IROP_TAG_STACKOFF)
  {
    /* Use irop_get_stack_offset() instead of op->u.imm32 directly:
     * for STRUCT types the offset is stored in u.s.aux_data (16-bit signed),
     * not in u.imm32 (which encodes both ctype_idx and the offset). */
    const int32_t stack_off = irop_get_stack_offset(*op);

    if (op->is_param && op->is_local)
    {
      /* Stack-passed parameter sitting in the caller's argument frame.
       * offset is relative to the frame base (positive = above saved LR). */
      m.kind = MACH_OP_PARAM_STACK;
      m.u.param.offset = stack_off;
      m.needs_deref = (bool)op->is_lval;
      return m;
    }

    if (!op->is_lval)
    {
      /* is_local=1 without is_lval: this operand represents the *address*
       * of a local variable (i.e. the result of a LEA / address-of).
       * The backend should compute FP + offset rather than load from it. */
      m.kind = MACH_OP_FRAME_ADDR;
      m.u.frame.offset = stack_off;
      return m;
    }

    /* Spill slot: the value must be loaded from the stack at run time.
     *
     * is_llocal=1 (double indirection): the spill slot holds a pointer.
     * After loading that pointer into a register, a second load is needed
     * to reach the actual value.  needs_deref signals this to the caller. */
    m.kind = MACH_OP_SPILL;
    m.u.spill.offset = stack_off;
    m.needs_deref = (bool)op->is_llocal;
    return m;
  }

  /* ------------------------------------------------------------------ */
  /* 4. Register-resident operands                                        */
  /* ------------------------------------------------------------------ */
  if (op->pr0_reg != PREG_REG_NONE)
  {
    m.kind = MACH_OP_REG;

    m.u.reg.r0 = (int)op->pr0_reg;
    /* r1 is only valid for 64-bit register pairs; -1 means unused. */
    m.u.reg.r1 = m.is_64bit ? (int)op->pr1_reg : -1;
    /* is_lval in a register context means the register holds an address
     * (pointer) and the backend must emit a load through it. */
    m.needs_deref = (bool)op->is_lval;
    return m;
  }

  /* ------------------------------------------------------------------ */
  /* 5. Fallback — unallocated / IROP_TAG_NONE                           */
  /* ------------------------------------------------------------------ */
  m.kind = MACH_OP_NONE;
  return m;
}
