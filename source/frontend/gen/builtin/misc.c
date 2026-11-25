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

/* Push this function's return address, __builtin_return_address(0), as a
 * void *.  The prologue must then save LR even in a leaf; the value is read
 * back from that slot (TCCIR_OP_RETURN_ADDRESS), with no frame pointer. */
ST_FUNC void vpush_return_address(void)
{
  CType type;
  SValue dest;
  type.t = VT_VOID;
  type.ref = NULL;
  mk_pointer(&type);
  tcc_state->force_lr_save = 1;
  svalue_init(&dest);
  dest.type = type;
  dest.r = 0;
  dest.vr = tcc_ir_get_vreg_temp(tcc_state->ir);
  tcc_ir_put(tcc_state->ir, TCCIR_OP_RETURN_ADDRESS, NULL, NULL, &dest);
  vpush(&type);
  vtop->vr = dest.vr;
  vtop->r = 0;
  vtop->c.i = 0;
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
 * Pushes the result onto the vstack with the given return type; returns the
 * call's id. */
int gen_ir_call_args(SValue *args, int argc, int func_tok, CType *ret_ctype)
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
  return new_call_id;
}

/* ------------------------------------------------------------------------ */
/* @min(a, b) and @max(a, b): short builtins, in C and in yasc alike.
 *
 * Unlike a MIN() macro each operand is evaluated once, left to right, and
 * unlike `a < b ? a : b` a signed operand that may be negative does not
 * silently turn into a huge unsigned value: mixing one with an unsigned
 * operand is an error (a non-negative constant is fine, so
 * @min(buflen, PATH_MAX) with a size_t buflen is accepted).  The result has
 * the usual arithmetic type of the two.  Two constants fold to a constant.
 *
 * Integers: the compare, branch and assign `?:` emits, which if-conversion
 * turns into cmp/it/mov.  Floating point: TCCIR_OP_FMIN/FMAX with fmin's NaN
 * rule (the other operand wins): one vminnm.f32/vmaxnm.f32 on an FPv5 FPU
 * (RP2350 user code), a call to fminf/fmin otherwise and for doubles.
 *
 * A regular compiler has no '@' token and no #define can make one: code that
 * gcc also builds spells it through a macro that becomes @min under tcc. */

static int at_operand_maybe_negative(SValue *sv)
{
  int t = sv->type.t;
  int bt = t & VT_BTYPE;
  if (bt == VT_BOOL || (t & VT_UNSIGNED))
    return 0;
  if ((sv->r & (VT_VALMASK | VT_LVAL | VT_SYM)) == VT_CONST)
  {
    int64_t v = (bt == VT_LLONG) ? (int64_t)sv->c.i : (int64_t)(int32_t)sv->c.i;
    return v < 0;
  }
  return 1;
}

/* the operand on vtop, evaluated now: a constant stays a constant, anything
 * else lands in a temp of its own, so the other operand cannot change it */
static void at_operand(const char *name)
{
  int bt = vtop->type.t & VT_BTYPE;
  if ((vtop->type.t & VT_ARRAY) || !(is_integer_btype(bt) || bt == VT_BOOL || is_float(vtop->type.t)))
  {
    char buf[256];
    type_to_str(buf, sizeof buf, &vtop->type, NULL);
    tcc_error("@%s wants numbers, not '%s'", name, buf);
  }
  if ((vtop->r & (VT_VALMASK | VT_LVAL | VT_SYM)) == VT_CONST || nocode_wanted)
    return;
  gv(RC_TYPE(vtop->type.t));
  if (vtop->vr >= 0 && TCCIR_DECODE_VREG_TYPE(vtop->vr) != TCCIR_VREG_TYPE_TEMP)
  {
    SValue src = *vtop, dest;
    svalue_init(&dest);
    dest.type = vtop->type;
    dest.vr = tcc_ir_get_vreg_temp(tcc_state->ir);
    tcc_ir_put(tcc_state->ir, TCCIR_OP_ASSIGN, &src, NULL, &dest);
    vtop->vr = dest.vr;
  }
}

/* cast vtop[-1] (the first operand) to TYPE */
static void at_cast_first(CType *type)
{
  vswap();
  gen_cast(type);
  vswap();
}

/* RV = the value SV holds */
static void at_copy_to(SValue *sv, int rv, int rc)
{
  SValue dest;
  vpushv(sv);
  gv(rc);
  svalue_init(&dest);
  dest.vr = rv;
  dest.type = vtop->type;
  tcc_ir_put(tcc_state->ir, TCCIR_OP_ASSIGN, vtop, NULL, &dest);
  vpop();
}

static __attribute__((noinline)) void at_minmax(int is_max)
{
  const char *name = is_max ? "max" : "min";
  CType type;
  SValue a, b;

  skip('(');
  expr_eq();
  at_operand(name);
  skip(',');
  expr_eq();
  at_operand(name);
  skip(')');

  if (!combine_types(&type, &vtop[-1], vtop, '?'))
    type_incompatibility_error(&vtop[-1].type, &vtop->type, "@min/@max of '%s' and '%s'");
  type.t &= ~(VT_CONSTANT | VT_VOLATILE | VT_DEFSIGN);
  if (is_integer_btype(type.t & VT_BTYPE) && (type.t & VT_UNSIGNED))
  {
    for (int i = -1; i <= 0; i++)
      if (at_operand_maybe_negative(&vtop[i]))
      {
        char sb[256], ub[256];
        type_to_str(sb, sizeof sb, &vtop[i].type, NULL);
        type_to_str(ub, sizeof ub, &vtop[i ? 0 : -1].type, NULL);
        tcc_error("@%s of signed '%s' and unsigned '%s': a negative value would compare as a huge one; cast "
                  "one of them",
                  name, sb, ub);
      }
  }

  at_cast_first(&type);
  gen_cast(&type);
  a = vtop[-1];
  b = vtop[0];

  /* two constants: fold */
  if ((a.r & (VT_VALMASK | VT_LVAL | VT_SYM)) == VT_CONST && (b.r & (VT_VALMASK | VT_LVAL | VT_SYM)) == VT_CONST)
  {
    int bt = type.t & VT_BTYPE, pick_a;
    if (is_float(type.t))
    {
      double x = bt == VT_FLOAT ? a.c.f : bt == VT_DOUBLE ? a.c.d : (double)a.c.ld;
      double y = bt == VT_FLOAT ? b.c.f : bt == VT_DOUBLE ? b.c.d : (double)b.c.ld;
      pick_a = y != y ? 1 : x != x ? 0 : is_max ? x >= y : x <= y;
    }
    else if (type.t & VT_UNSIGNED)
    {
      uint64_t x = a.c.i, y = b.c.i;
      if (bt != VT_LLONG)
        x = (uint32_t)x, y = (uint32_t)y;
      pick_a = is_max ? x >= y : x <= y;
    }
    else
    {
      int64_t x = a.c.i, y = b.c.i;
      if (bt != VT_LLONG)
        x = (int32_t)x, y = (int32_t)y;
      pick_a = is_max ? x >= y : x <= y;
    }
    vtop -= 2;
    vpushv(pick_a ? &a : &b);
    return;
  }

  if (nocode_wanted)
  {
    /* sizeof, typeof, a dead arm: the type is all that is wanted.  Not a
     * constant, though: a static initializer is parsed with nocode_wanted
     * too, and `static int x = @min(g, 3);` must say "not constant" rather
     * than store 3. */
    vtop -= 2;
    vpushv(&b);
    vtop->r = 0;
    return;
  }

  if (is_float(type.t))
  {
    /* TCCIR_OP_FMIN/FMAX: vminnm.f32 inline on an FPv5 FPU, else put as a
     * call to fminf/fmin (ir/gen/softfloat.c) */
    int is_double = (type.t & VT_BTYPE) != VT_FLOAT;
    SValue dest;
    svalue_init(&dest);
    dest.vr = tcc_ir_get_vreg_temp(tcc_state->ir);
    dest.r = 0;
    dest.type = type;
    tcc_ir_set_float_type(tcc_state->ir, dest.vr, 1, is_double);
    tcc_ir_put(tcc_state->ir, is_max ? TCCIR_OP_FMAX : TCCIR_OP_FMIN, &vtop[-1], &vtop[0], &dest);
    vtop -= 2;
    vpushv(&dest);
    return;
  }

  /* the shape `a < b ? a : b` takes in expr_cond_ternary -- a copy into the
   * result in each arm -- so the same if-conversion makes it cmp/it/mov
   * (with the false arm empty it stays a branch):
   *     cmp a, b; jump-if-false F; jmp T
   *   F: r = b; jmp J
   *   T: r = a
   *   J: */
  int rc = RC_TYPE(type.t), tt, u;
  int rv = tcc_ir_get_vreg_temp(tcc_state->ir);
  vpushv(&a);
  vpushv(&b);
  gen_op(is_max ? TOK_GT : TOK_LT); /* '<' is TOK_SHL */
  tt = tcc_ir_codegen_test_gen(tcc_state->ir, 1, -1);
  u = gjmp(-1);
  tcc_ir_backpatch_to_here(tcc_state->ir, tt);
  vtop -= 2; /* the operands: values in temps or constants, nothing to free */
  at_copy_to(&b, rv, rc);
  tt = gjmp(-1);
  tcc_ir_backpatch_to_here(tcc_state->ir, u);
  at_copy_to(&a, rv, rc);
  tcc_ir_backpatch_to_here(tcc_state->ir, tt);
  vpushv(&b);
  vtop->r = 0;
  vtop->vr = rv;
}

/* '@' NAME ( ... ): tok is '@' */
__attribute__((noinline)) void unary_at_builtin(void)
{
  next();
  const char *name = tok >= TOK_IDENT ? get_tok_str(tok, NULL) : "";
  if (!strcmp(name, "min") || !strcmp(name, "max"))
  {
    int is_max = name[1] == 'a';
    next();
    at_minmax(is_max);
    return;
  }
  tcc_error("unknown builtin '@%s' (there are @min and @max)", get_tok_str(tok, &tokc));
}
