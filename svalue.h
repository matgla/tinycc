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
  struct
  {
    uint8_t pr0_reg : 5;     /* Physical register number (0-15 for ARM, 31=PREG_REG_NONE) */
    uint8_t pr0_spilled : 1; /* Spilled to stack flag */
  };

  struct
  {
    uint8_t pr1_reg : 5;     /* Physical register number (0-15 for ARM, 31=PREG_REG_NONE) */
    uint8_t pr1_spilled : 1; /* Spilled to stack flag */
  };

  unsigned short r; /* register + flags */
  int vr;           /* virtual register for IR */
  CType type;       /* type */
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
