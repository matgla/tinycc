/*
 *  TCC IR - Machine Operand Representation
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

#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Forward declarations — full types available after tcc.h / tccir_operand.h */
struct TCCIRState;
struct IROperand;
struct Sym;

/* ============================================================================
 * MachineOperand: Unambiguous machine-level operand representation
 * ============================================================================
 *
 * Produced by machine_op_from_ir() from a FILLED IROperand (already processed
 * by tcc_ir_fill_registers_ir()). Replaces the combination of bit-flag tests
 * that the backend must currently perform to determine materialization steps.
 *
 * Each kind maps to a single, self-contained action for the backend:
 *
 *   MACH_OP_REG        — value is in physical register(s); use u.reg.r0/r1
 *   MACH_OP_SPILL      — value is in a stack spill slot; load from u.spill.offset
 *   MACH_OP_IMM        — literal immediate constant in u.imm.val
 *   MACH_OP_FRAME_ADDR — compute address FP + u.frame.offset (LEA of local)
 *   MACH_OP_SYMBOL     — global/extern symbol reference in u.sym.sym + addend
 *   MACH_OP_PARAM_STACK — stack-passed parameter at u.param.offset in caller frame
 *
 * needs_deref=true means the representated entity is an address: the caller
 * must emit a load through it to obtain the actual value (replaces VT_LVAL).
 * For MACH_OP_SPILL with needs_deref: the spill slot holds a pointer, and
 * after loading that pointer a further dereference is required (VT_LLOCAL).
 */

typedef enum
{
  MACH_OP_NONE = 0,    /* Uninitialized / no allocation (error/sentinel) */
  MACH_OP_REG,         /* Value in physical register(s) */
  MACH_OP_SPILL,       /* Value in spill slot on stack, needs load */
  MACH_OP_IMM,         /* Immediate constant */
  MACH_OP_FRAME_ADDR,  /* Address = FP + offset (address-of local variable) */
  MACH_OP_SYMBOL,      /* Symbol reference (global/extern/function) */
  MACH_OP_PARAM_STACK, /* Stack-passed parameter in caller's argument frame */
  MACH_OP_CHAIN_REL,   /* Captured variable: chain_index + FP-relative offset in parent */
} MachineOperandKind;

typedef struct MachineOperand
{
  MachineOperandKind kind; /* How to materialize this operand */
  int btype;               /* IROP_BTYPE_* — compressed base type */
  int vreg;                /* Original vreg (for debug / liveness queries) */
  bool needs_deref;        /* Emit a load through this address (VT_LVAL) */
  bool is_64bit;           /* Two-register value (INT64 or FLOAT64) */
  bool is_unsigned;        /* Unsigned type (VT_UNSIGNED) */
  bool is_complex;         /* Complex type (VT_COMPLEX) */
  bool align4;             /* 64-bit deref only: the accessed address is proven
                            * >= 4-byte aligned, so LDRD/STRD may be used through
                            * a general base register (IROperand.align4_ok). */
  bool underalign_hint;    /* Base of an indexed access whose chain crossed a
                            * packed member: address may be < 4-byte aligned, so
                            * the 64-bit indexed lowering must avoid LDRD/STRD
                            * (IROperand.underalign_hint). */
  union
  {
    struct
    {
      int r0; /* Primary physical register */
      int r1; /* Second register for 64-bit pair (-1 if not 64-bit) */
    } reg;    /* MACH_OP_REG */
    struct
    {
      int32_t offset; /* FP-relative byte offset of the spill slot */
    } spill;          /* MACH_OP_SPILL */
    struct
    {
      int64_t val; /* Integer/float bits of the constant */
    } imm;         /* MACH_OP_IMM */
    struct
    {
      int32_t offset; /* FP-relative byte offset for LEA */
    } frame;          /* MACH_OP_FRAME_ADDR */
    struct
    {
      struct Sym *sym; /* Target symbol */
      int addend;      /* Constant addend (e.g. struct field offset) */
    } sym;             /* MACH_OP_SYMBOL */
    struct
    {
      int32_t offset; /* Byte offset from start of the caller argument area */
    } param;          /* MACH_OP_PARAM_STACK */
    struct
    {
      int32_t offset;      /* Parent-frame byte offset of the captured variable */
      int32_t chain_index; /* Index into ir->captured_offsets_list */
    } chain;               /* MACH_OP_CHAIN_REL */
  } u;
} MachineOperand;

/* ============================================================================
 * machine_op_from_ir: Convert an IROperand to a MachineOperand
 * ============================================================================
 *
 * Reads the raw (unfilled) IROperand and the register-allocation interval
 * table to produce a MachineOperand directly.  Does NOT call
 * tcc_ir_fill_registers_ir — the IROperand is not mutated.
 *
 * Callers may pass the same operand to multiple calls without worrying about
 * fill ordering or double-fill issues.
 */
MachineOperand machine_op_from_ir(struct TCCIRState *ir, const struct IROperand *op);
