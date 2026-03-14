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
 * machine_op_from_ir: Convert an IROperand to a MachineOperand
 * ============================================================================
 *
 * Reads the raw (unfilled) IROperand and the register-allocation interval
 * table to produce a MachineOperand directly.  Does NOT call
 * tcc_ir_fill_registers_ir — the IROperand is not mutated.
 *
 * Decision order:
 *
 *   1. Immediate constants (tag = IMM32 / F32 / I64 / F64, is_const=1)
 *      → MACH_OP_IMM: literal value in u.imm.val
 *
 *   2. Symbol references (tag = SYMREF)
 *      → MACH_OP_SYMBOL
 *
 *   3. Concrete stack slots (tag = STACKOFF, vreg < 0)
 *      → MACH_OP_CHAIN_REL (captured var via static chain)
 *      → MACH_OP_PARAM_STACK (stack-passed parameter)
 *      → MACH_OP_FRAME_ADDR  (address-of local, no is_lval)
 *      → MACH_OP_SPILL       (load from slot)
 *
 *   4. Allocated operands (valid vreg, look up interval):
 *      → MACH_OP_PARAM_STACK (stack-passed param, not register-allocated)
 *      → MACH_OP_SPILL / MACH_OP_FRAME_ADDR (spilled)
 *      → MACH_OP_REG (register-allocated)
 *
 *   5. Fallback → MACH_OP_NONE
 */
MachineOperand machine_op_from_ir(TCCIRState *ir, const IROperand *op)
{
  MachineOperand m = {0};
  m.kind = MACH_OP_NONE;

  if (!op || irop_is_none(*op))
    return m;

  m.btype = irop_get_btype(*op);
  m.is_unsigned = (bool)op->is_unsigned;
  m.is_64bit = (bool)irop_needs_pair(*op);
  m.is_complex = (bool)op->is_complex;
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
  /* 2. Symbol references                                                 */
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
  /* 3. Concrete stack slots (vreg < 0): locals, temp locals, and raw    */
  /*    stack-offset operands not assigned to a register.                */
  /*    fill_registers_ir returns early for these.                       */
  /* ------------------------------------------------------------------ */
  const int vreg = irop_get_vreg(*op);

  if (vreg < 0 && (op->is_local || op->is_llocal || tag == IROP_TAG_STACKOFF))
  {
    const int32_t stack_off = irop_get_stack_offset(*op);

    /* Captured variable: vreg < 0 means the variable belongs to a parent
     * frame and must be reached via the static chain (R10). */
    if (ir->has_static_chain && ir->captured_count > 0)
    {
      for (int ci = 0; ci < ir->captured_count; ci++)
      {
        if (ir->captured_offsets_list[ci] == stack_off)
        {
          m.kind = MACH_OP_CHAIN_REL;
          m.u.chain.offset = stack_off;
          m.u.chain.chain_index = ci;
          m.needs_deref = (bool)op->is_lval;
          return m;
        }
      }
    }

    if (op->is_param && op->is_local)
    {
      m.kind = MACH_OP_PARAM_STACK;
      m.u.param.offset = stack_off;
      m.needs_deref = (bool)op->is_lval;
      return m;
    }

    if (!op->is_lval)
    {
      m.kind = MACH_OP_FRAME_ADDR;
      m.u.frame.offset = stack_off;
      return m;
    }

    m.kind = MACH_OP_SPILL;
    m.u.spill.offset = stack_off;
    m.needs_deref = (bool)op->is_llocal;
    return m;
  }

  /* ------------------------------------------------------------------ */
  /* 4. Allocated operands: look up interval for register/spill info     */
  /* ------------------------------------------------------------------ */

  /* IROP_TAG_VREG with vreg=-1 ("no vreg"): value lives in a pinned physical
   * register, not tracked by the vreg system.  svalue_to_iroperand() Case 1b
   * encodes the register in u.imm32 with IROP_VREG_PHYS_VALID as a flag,
   * bypassing the pr0_reg bitfield (which tcc_ir_put() clears, but
   * svalue_to_iroperand would re-derive from sv->r & VT_VALMASK). */
  if (vreg == -1 && tag == IROP_TAG_VREG)
  {
    uint32_t phys = (uint32_t)op->u.imm32;
    if (phys & IROP_VREG_PHYS_VALID)
    {
      m.kind = MACH_OP_REG;
      m.u.reg.r0 = (int)(phys & IROP_VREG_PHYS_MASK);
      m.u.reg.r1 = -1; /* pr1 is always PREG_REG_NONE for pool-stored vreg=-1 */
      m.needs_deref = (bool)op->is_lval;
      return m;
    }
    m.kind = MACH_OP_NONE;
    return m;
  }

  if (!tcc_ir_vreg_is_valid(ir, vreg))
  {
    m.kind = MACH_OP_NONE;
    return m;
  }

  IRLiveInterval *interval = tcc_ir_vreg_live_interval(ir, vreg);
  if (!interval)
  {
    m.kind = MACH_OP_NONE;
    return m;
  }

  /* Stack-passed parameters: if not allocated to a register, treat them as
   * residing in the incoming argument area. */
  if (TCCIR_DECODE_VREG_TYPE(vreg) == TCCIR_VREG_TYPE_PARAM && interval->incoming_reg0 < 0 &&
      interval->allocation.r0 == PREG_NONE && interval->allocation.offset == 0)
  {
    m.kind = MACH_OP_PARAM_STACK;
    /* Use actual operand offset when available (tag == STACKOFF).
     * Sub-component access (e.g. __imag__ on a complex double param)
     * adjusts the stack offset via c.i += elem_size, producing a
     * different offset than the param's original_offset.  Without
     * this, __imag__ of a stack-passed complex double would read the
     * real part instead of the imaginary part. */
    if (tag == IROP_TAG_STACKOFF)
      m.u.param.offset = irop_get_stack_offset(*op);
    else
      m.u.param.offset = interval->original_offset;
    int need_lval = op->is_lval;
    if (!op->is_const && !op->is_local && !op->is_llocal && interval->is_lvalue)
      need_lval = 1;
    m.needs_deref = (bool)need_lval;
    return m;
  }

  int is_register_param = (TCCIR_DECODE_VREG_TYPE(vreg) == TCCIR_VREG_TYPE_PARAM && interval->incoming_reg0 >= 0);

  /* Compute the final stack offset, applying the delta for locals that
   * had a sub-component offset in the original operand. */
  int32_t alloc_offset;
  if (op->btype == IROP_BTYPE_STRUCT)
  {
    alloc_offset = interval->allocation.offset;
  }
  else if ((op->is_local || op->is_llocal) && !op->is_param && tag == IROP_TAG_STACKOFF)
  {
    int32_t old_stackoff = op->u.imm32;
    int32_t delta = old_stackoff - interval->original_offset;
    alloc_offset = interval->allocation.offset + delta;
  }
  else
  {
    alloc_offset = interval->allocation.offset;
  }

  bool is_spilled = (interval->allocation.r0 & PREG_SPILLED) || alloc_offset != 0;
  /* Unallocated vreg (PREG_NONE, offset = 0): the operand is effectively still
   * on the stack at alloc_offset (which may be 0 for unresolved cases).  Treat
   * the same as a spill so we produce MACH_OP_SPILL / MACH_OP_FRAME_ADDR. */
  bool is_unallocated = (interval->allocation.r0 == PREG_NONE);

  if (is_spilled || is_unallocated)
  {
    /* Determine need_lval and double-indirection (llocal). */
    int need_lval;
    if (op->is_local || op->is_llocal)
    {
      /* Local variable: preserve original is_lval (load vs address-of). */
      need_lval = op->is_lval;
    }
    else
    {
      /* Computed value (was in register): always need lval to load from spill. */
      need_lval = 1;
    }

    int use_llocal = 0;
    if (op->is_lval && !op->is_local && !op->is_llocal)
    {
      /* The original use wants to dereference the value in this vreg.
       * Since the value is spilled, we need double indirection:
       * load pointer from spill slot, then dereference it. */
      use_llocal = 1;
    }

    /* Only preserve is_param for stack-passed parameters (incoming_reg0 < 0). */
    int spilled_param = 0;
    if (op->is_param && interval->incoming_reg0 < 0)
    {
      spilled_param = 1;
    }

    /* Captured variable check for spilled vreg < 0 case. */
    if (ir->has_static_chain && ir->captured_count > 0 && vreg < 0)
    {
      for (int ci = 0; ci < ir->captured_count; ci++)
      {
        if (ir->captured_offsets_list[ci] == alloc_offset)
        {
          m.kind = MACH_OP_CHAIN_REL;
          m.u.chain.offset = alloc_offset;
          m.u.chain.chain_index = ci;
          m.needs_deref = (bool)need_lval;
          return m;
        }
      }
    }

    if (spilled_param && op->is_local)
    {
      /* Stack-passed parameter that stayed on stack. */
      m.kind = MACH_OP_PARAM_STACK;
      m.u.param.offset = alloc_offset;
      m.needs_deref = (bool)need_lval;
      return m;
    }

    if (!need_lval)
    {
      /* Address-of expression: compute FP + offset rather than load. */
      m.kind = MACH_OP_FRAME_ADDR;
      m.u.frame.offset = alloc_offset;
      return m;
    }

    m.kind = MACH_OP_SPILL;
    m.u.spill.offset = alloc_offset;
    m.needs_deref = (bool)use_llocal;
    return m;
  }

  /* ------------------------------------------------------------------ */
  /* Register-resident operand                                            */
  /* ------------------------------------------------------------------ */
  if (interval->allocation.r0 != PREG_NONE)
  {
    m.kind = MACH_OP_REG;
    m.u.reg.r0 = (int)(interval->allocation.r0 & PREG_REG_NONE);
    m.u.reg.r1 = m.is_64bit ? (int)(interval->allocation.r1 & PREG_REG_NONE) : -1;

    /* Preserve is_lval only for pointer derefs, not for locals promoted to reg. */
    int preserve_lval = 0;
    if (op->is_lval && !op->is_const && !op->is_local && !op->is_llocal && !is_register_param)
    {
      preserve_lval = 1;
    }
    m.needs_deref = (bool)preserve_lval;
    return m;
  }

  /* ------------------------------------------------------------------ */
  /* 5. Fallback — unallocated / IROP_TAG_NONE                           */
  /* ------------------------------------------------------------------ */
  m.kind = MACH_OP_NONE;
  return m;
}
