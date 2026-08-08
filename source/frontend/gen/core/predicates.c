/*
 *  TCC - Tiny C Compiler
 *
 *  Copyright (c) 2001-2004 Fabrice Bellard
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

/* predicates.c -- Cheap type predicates and return-register/register-class helpers.
 * Split out of tccgen.c; see docs/plan_tccgen_split.md. */

#include "gen_priv.h"

/* ------------------------------------------------------------------------- */

ST_INLN int is_float(int t)
{
  int bt = t & VT_BTYPE;
  return bt == VT_LDOUBLE || bt == VT_DOUBLE || bt == VT_FLOAT || bt == VT_QFLOAT;
}

int is_integer_btype(int bt)
{
  return bt == VT_BYTE || bt == VT_BOOL || bt == VT_SHORT || bt == VT_INT || bt == VT_LLONG;
}

int btype_size(int bt)
{
  return bt == VT_BYTE || bt == VT_BOOL ? 1
         : bt == VT_SHORT               ? 2
         : bt == VT_INT                 ? 4
         : bt == VT_LLONG               ? 8
         : bt == VT_PTR                 ? PTR_SIZE
                                        : 0;
}

/* returns function return register from type */
static int R_RET(int t)
{
  if (!is_float(t))
    return REG_IRET;
#ifdef TCC_TARGET_X86_64
  if ((t & VT_BTYPE) == VT_LDOUBLE)
    return TREG_ST0;
#elif defined TCC_TARGET_RISCV64
  if ((t & VT_BTYPE) == VT_LDOUBLE)
    return REG_IRET;
#endif
  return REG_FRET;
}

/* put function return registers to stack value */
void PUT_R_RET(SValue *sv, int t)
{
  sv->r = R_RET(t);
}

/* returns function return register class for type t */
int RC_RET(int t)
{
  return reg_classes[R_RET(t)] & ~(RC_FLOAT | RC_INT);
}

/* returns generic register class for type t */
int RC_TYPE(int t)
{
  if (!is_float(t))
    return RC_INT;
  return RC_FLOAT;
}

// /* returns 2nd register class corresponding to t and rc */
// static int RC2_TYPE(int t, int rc)
// {
//   if (!USING_TWO_WORDS(t))
//     return 0;
// #ifdef RC_IRE2
//   if (rc == RC_IRET)
//     return RC_IRE2;
// #endif
// #ifdef RC_FRE2
//   if (rc == RC_FRET)
//     return RC_FRE2;
// #endif
//   if (rc & RC_FLOAT)
//     return RC_FLOAT;
//   return RC_INT;
// }

/* we use our own 'finite' function to avoid potential problems with
   non standard math libs */
/* XXX: endianness dependent */
ST_FUNC int ieee_finite(double d)
{
  int p[4];
  memcpy(p, &d, sizeof(double));
  return ((unsigned)((p[1] | 0x800fffff) + 1)) >> 31;
}

/* compiling intel long double natively */
#if (defined __i386__ || defined __x86_64__) && (defined TCC_TARGET_I386 || defined TCC_TARGET_X86_64)
#define TCC_IS_NATIVE_387
#endif

ST_FUNC void test_lvalue(void)
{
  if (!(vtop->r & VT_LVAL))
    expect("lvalue");
}

ST_FUNC void check_vstack(void)
{
  if (vtop != vstack - 1)
    tcc_error("internal compiler error: vstack leak (%d)", (int)(vtop - vstack + 1));
}

/* vstack debugging aid */
#if 0
void pv(const char *lbl, int a, int b) {
  int i;
  for (i = a; i < a + b; ++i) {
    SValue *p = &vtop[-i];
    printf("%s vtop[-%d] : type.t:%04x  r:%04x  r2:%04x  c.i:%d\n", lbl, i,
           p->type.t, p->r, p->r2, (int)p->c.i);
  }
}
#endif

// debugging aid when stack is corrupted
#if 0
void dbg_print_vstack(const char *msg, const char *file, int line) {
  printf("print_vstack '%s' vtop: %p, elements: %d, at: %s:%d\n", msg, vtop,
         (vtop - vstack) + 1, file, line);
}
#endif
