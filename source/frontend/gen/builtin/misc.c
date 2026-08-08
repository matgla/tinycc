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

/* misc.c -- Builtin classification, bit operations and IR call-argument emission.
 * Split out of tccgen.c; see docs/plan_tccgen_split.md. */

#include "gen_priv.h"

/* GCC __builtin_classify_type return values (C mode) */
#define GCC_TYPE_CLASS_VOID 0
#define GCC_TYPE_CLASS_INTEGER 1
#define GCC_TYPE_CLASS_POINTER 5
#define GCC_TYPE_CLASS_REAL 8
#define GCC_TYPE_CLASS_COMPLEX 9
#define GCC_TYPE_CLASS_FUNCTION 10
#define GCC_TYPE_CLASS_STRUCT 12
#define GCC_TYPE_CLASS_UNION 13
#define GCC_TYPE_CLASS_ARRAY 14
#define GCC_TYPE_CLASS_VECTOR 18

int gcc_classify_type(CType *type)
{
  int bt = type->t & VT_BTYPE;
  int t = type->t;

  switch (bt)
  {
  case VT_VOID:
    return GCC_TYPE_CLASS_VOID;

  case VT_BYTE:
  case VT_SHORT:
  case VT_INT:
  case VT_LLONG:
  case VT_BOOL:
    return GCC_TYPE_CLASS_INTEGER;

  case VT_PTR:
    if (t & VT_ARRAY)
      return GCC_TYPE_CLASS_ARRAY;
    return GCC_TYPE_CLASS_POINTER;

  case VT_FUNC:
    return GCC_TYPE_CLASS_FUNCTION;

  case VT_STRUCT:
    if (IS_UNION(t))
      return GCC_TYPE_CLASS_UNION;
    return GCC_TYPE_CLASS_STRUCT;

  case VT_FLOAT:
  case VT_DOUBLE:
  case VT_LDOUBLE:
    if (t & VT_COMPLEX)
      return GCC_TYPE_CLASS_COMPLEX;
    return GCC_TYPE_CLASS_REAL;

  default:
    return GCC_TYPE_CLASS_INTEGER; /* fallback */
  }
}

/* Emit a single-operand bit-manipulation IR op (TCCIR_OP_CLZ / RBIT / REV /
 * REV16) consuming the 32-bit value on top of the vstack and replacing it with
 * the (unsigned int) result.  Only valid when tcc_machine_has_bit_ops(). */
void gen_bitop1(TccIrOp op)
{
  SValue dest;
  svalue_init(&dest);
  dest.type.t = VT_INT | VT_UNSIGNED;
  dest.type.ref = NULL;
  dest.r = 0;
  dest.vr = tcc_ir_get_vreg_temp(tcc_state->ir);
  tcc_ir_put(tcc_state->ir, op, vtop, NULL, &dest);
  vtop->type.t = VT_INT | VT_UNSIGNED;
  vtop->type.ref = NULL;
  vtop->vr = dest.vr;
  vtop->r = 0;
  vtop->c.i = 0; /* Clear c.i to avoid corrupting later operations */
}

/* Emit an IR function call to a library helper for a builtin.
 * Arguments are already on the vstack (1 or 2 args).
 * func_tok: TOK_xxx or tok_alloc_const("name") for the target function
 * argc: number of arguments (1 or 2), already on vstack
 * ret_type: VT_INT, VT_FLOAT, VT_DOUBLE, etc.
 * Pops argc args from vstack, pushes the result. */
void gen_builtin_libcall(int func_tok, int argc, int ret_type)
{
  const int new_call_id = tcc_state->ir->next_call_id++;
  SValue param_num;
  svalue_init(&param_num);
  param_num.vr = -1;
  param_num.r = VT_CONST;

  for (int i = 0; i < argc; i++)
  {
    param_num.c.i = TCCIR_ENCODE_PARAM(new_call_id, i);
    tcc_ir_put(tcc_state->ir, TCCIR_OP_FUNCPARAMVAL, &vtop[i - (argc - 1)], &param_num, NULL);
  }

  vpush_helper_func(func_tok);

  SValue call_id_sv = tcc_ir_svalue_call_id_argc(new_call_id, argc);
  SValue dest;
  svalue_init(&dest);
  dest.type.t = ret_type;
  dest.r = 0;
  dest.vr = tcc_ir_get_vreg_temp(tcc_state->ir);
  tcc_ir_put(tcc_state->ir, TCCIR_OP_FUNCCALLVAL, &vtop[0], &call_id_sv, &dest);

  vtop -= (argc + 1); /* pop func + args */
  vpushi(0);
  vtop->type.t = ret_type;
  vtop->vr = dest.vr;
  vtop->r = TREG_R0;
}

/* Emit an IR function call with arguments from an SValue array (not from vstack).
 * args[0..argc-1] are the arguments.
 * Pushes the result onto the vstack with the given return type. */
void gen_ir_call_args(SValue *args, int argc, int func_tok, CType *ret_ctype)
{
  const int new_call_id = tcc_state->ir->next_call_id++;
  SValue param_num;
  svalue_init(&param_num);
  param_num.vr = -1;
  param_num.r = VT_CONST;

  for (int i = 0; i < argc; i++)
  {
    param_num.c.i = TCCIR_ENCODE_PARAM(new_call_id, i);
    tcc_ir_put(tcc_state->ir, TCCIR_OP_FUNCPARAMVAL, &args[i], &param_num, NULL);
  }

  vpush_helper_func(func_tok);

  SValue call_id_sv = tcc_ir_svalue_call_id_argc(new_call_id, argc);
  SValue dest;
  svalue_init(&dest);
  dest.type = *ret_ctype;
  dest.r = 0;
  dest.vr = tcc_ir_get_vreg_temp(tcc_state->ir);
  tcc_ir_put(tcc_state->ir, TCCIR_OP_FUNCCALLVAL, &vtop[0], &call_id_sv, &dest);

  --vtop; /* pop function */
  vpushi(0);
  vtop->type = dest.type;
  vtop->vr = dest.vr;
  vtop->r = TREG_R0;
}
