/*
 *  TCC - Tiny C Compiler
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

#include <stdint.h>

/* Forward declarations */
typedef struct CType CType;
typedef union CValue CValue;
typedef struct Sym Sym;

/* SValue: Semantic value representing variables, constants, and intermediate results.
 * Used throughout code generation and intermediate representation.
 */
typedef struct SValue
{
  uint8_t pr0_reg : 5;     /* Physical register number (0-15 for ARM, 31=PREG_REG_NONE) */
  uint8_t pr0_spilled : 1; /* Spilled to stack flag */
  uint8_t underaligned : 1; /* Access through this value may be < 4-byte aligned
                             * (packed-struct member chain).  Set at member access
                             * and propagated through pointer arithmetic; read by
                             * svalue_to_iroperand to decide whether a 64-bit
                             * deref may use LDRD/STRD (which fault on unaligned
                             * addresses on ARMv7-M/v8-M).  A stale 1 only costs
                             * the optimization; a lost 1 would fault, so setters
                             * must never be skipped. */
  uint8_t pr1_reg : 5;     /* Physical register number (0-15 for ARM, 31=PREG_REG_NONE) */
  uint8_t pr1_spilled : 1; /* Spilled to stack flag */

  /* Value location and flags - union for bitfield or legacy access */
  union
  {
    short r; /* legacy: full 16-bit register + flags */
    struct
    {
      unsigned short location : 8;    /* VT_CONST, VT_LOCAL, VT_LLOCAL, VT_CMP, VT_JMP, VT_JMPI (bits 0-7) */
      unsigned short is_lval : 1;     /* VT_LVAL: var is an lvalue (bit 8) */
      unsigned short has_sym : 1;     /* VT_SYM: symbol value is added (bit 9) */
      unsigned short mustcast : 2;    /* VT_MUSTCAST: value must be casted (bits 10-11) */
      unsigned short nonconst : 1;    /* VT_NONCONST: not a C standard integer constant (bit 12) */
      unsigned short reserved_13 : 1; /* unused (bit 13) */
      unsigned short mustbound : 1;   /* VT_MUSTBOUND: bound checking required (bit 14) */
      unsigned short bounded : 1;     /* VT_BOUNDED: value is bounded (bit 15) */
    };
  };
  int vr;     /* virtual register for IR */
  CType type; /* type */
  union
  {
    struct
    {
      int jtrue, jfalse;
    }; /* forward jmps */
    CValue c; /* constant, if VT_CONST */
  };
  union
  {
    struct
    {
      unsigned short cmp_op, cmp_r;
    }; /* VT_CMP operation */
    struct Sym *sym; /* symbol, if (VT_SYM | VT_CONST), or if */
  }; /* result of unary() for an identifier. */
} SValue;

/* Initialize an SValue to a clean state with pr0/pr1 set to PREG_NONE */
void svalue_init(SValue *sv);

/* Create a const i64 SValue */
SValue svalue_const_i64(int64_t v);

/* Create an SValue for IR call ID */
SValue svalue_call_id(int call_id);

/* Create an SValue for IR call ID with argc */
SValue svalue_call_id_argc(int call_id, int argc);
