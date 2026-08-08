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

/* fold_math.c -- Compile-time folding of math/complex builtins and the conservative object-size facts.
 * Split out of tccgen.c; see docs/plan_tccgen_split.md. */

#include "gen_priv.h"

Sym *find_local_scalar_sym_by_offset(int offset)
{
  Sym *s;

  for (s = local_stack; s; s = s->prev)
  {
    if ((s->r & VT_VALMASK) != VT_LOCAL)
      continue;
    if (s->v & (SYM_FIELD | SYM_STRUCT))
      continue;
    if ((s->type.t & VT_BTYPE) == VT_STRUCT || (s->type.t & (VT_ARRAY | VT_VLA)))
      continue;
    if (s->c == offset)
      return s;
  }

  return NULL;
}

static Sym *find_local_scalar_sym_for_svalue(SValue *sv)
{
  if ((sv->r & VT_VALMASK) == VT_LOCAL && sv->sym && (sv->sym->r & VT_VALMASK) == VT_LOCAL)
    return sv->sym;

  if ((sv->r & VT_VALMASK) == VT_LOCAL)
    return find_local_scalar_sym_by_offset((int)sv->c.i);

  return NULL;
}

typedef struct ObjsizeVregFact
{
  int vreg;
  unsigned long long max_value;
  unsigned long long strlen_value;
  unsigned char max_valid;
  unsigned char strlen_valid;
} ObjsizeVregFact;

static TCCIRState *objsize_fact_ir;
static ObjsizeVregFact *objsize_vreg_facts;
static int objsize_vreg_fact_count;
static int objsize_vreg_fact_capacity;

static void objsize_vreg_facts_switch_ir(TCCIRState *ir)
{
  if (objsize_fact_ir == ir)
    return;

  objsize_fact_ir = ir;
  objsize_vreg_fact_count = 0;
}

static ObjsizeVregFact *objsize_vreg_fact_find(TCCIRState *ir, int vreg)
{
  objsize_vreg_facts_switch_ir(ir);

  for (int i = 0; i < objsize_vreg_fact_count; i++)
  {
    if (objsize_vreg_facts[i].vreg == vreg)
      return &objsize_vreg_facts[i];
  }

  return NULL;
}

void objsize_vreg_fact_record(TCCIRState *ir, int vreg, int max_valid, unsigned long long max_value,
                                     int strlen_valid, unsigned long long strlen_value)
{
  ObjsizeVregFact *fact;

  if (!ir || vreg < 0)
    return;

  fact = objsize_vreg_fact_find(ir, vreg);
  if (!fact)
  {
    if (objsize_vreg_fact_count >= objsize_vreg_fact_capacity)
    {
      objsize_vreg_fact_capacity = objsize_vreg_fact_capacity ? objsize_vreg_fact_capacity * 2 : 32;
      objsize_vreg_facts = tcc_realloc(objsize_vreg_facts, objsize_vreg_fact_capacity * sizeof(*objsize_vreg_facts));
    }
    fact = &objsize_vreg_facts[objsize_vreg_fact_count++];
    fact->vreg = vreg;
  }

  fact->max_valid = max_valid;
  fact->max_value = max_valid ? max_value : 0;
  fact->strlen_valid = strlen_valid;
  fact->strlen_value = strlen_valid ? strlen_value : 0;
}

static int objsize_vreg_fact_get_max(TCCIRState *ir, int vreg, unsigned long long *out_max)
{
  ObjsizeVregFact *fact;

  if (!ir || vreg < 0)
    return 0;

  fact = objsize_vreg_fact_find(ir, vreg);
  if (!fact || !fact->max_valid)
    return 0;

  *out_max = fact->max_value;
  return 1;
}

static int objsize_vreg_fact_get_strlen(TCCIRState *ir, int vreg, unsigned long long *out_max)
{
  ObjsizeVregFact *fact;

  if (!ir || vreg < 0)
    return 0;

  fact = objsize_vreg_fact_find(ir, vreg);
  if (!fact || !fact->strlen_valid)
    return 0;

  *out_max = fact->strlen_value;
  return 1;
}

int svalue_get_conservative_max_u64(SValue *sv, unsigned long long *out_max)
{
  int kind = sv->r & (VT_VALMASK | VT_LVAL | VT_SYM);

  if (kind == VT_CONST)
  {
    unsigned long long value = (unsigned long long)sv->c.i;

    if (!(sv->type.t & VT_UNSIGNED) && (sv->type.t & VT_BTYPE) != VT_PTR && (int64_t)sv->c.i < 0)
      return 0;
    *out_max = value;
    return 1;
  }

  if ((sv->r & VT_VALMASK) == VT_LOCAL)
  {
    Sym *sym = find_local_scalar_sym_for_svalue(sv);

    if (sym && sym->objsize_max_valid)
    {
      *out_max = sym->objsize_max_value;
      return 1;
    }
  }

  if (sv->vr >= 0 && objsize_vreg_fact_get_max(tcc_state ? tcc_state->ir : NULL, sv->vr, out_max))
    return 1;

  return 0;
}

int svalue_get_conservative_string_bytes_u64(SValue *sv, unsigned long long *out_max)
{
  int len;

  if (try_get_constant_string(sv, &len))
  {
    *out_max = (unsigned long long)len + 1;
    return 1;
  }

  if ((sv->r & VT_VALMASK) == VT_LOCAL)
  {
    Sym *sym = find_local_scalar_sym_for_svalue(sv);

    if (sym && sym->objsize_strlen_valid)
    {
      *out_max = sym->objsize_strlen_value;
      return 1;
    }
  }

  if (sv->vr >= 0 && objsize_vreg_fact_get_strlen(tcc_state ? tcc_state->ir : NULL, sv->vr, out_max))
    return 1;

  return 0;
}

int chk_get_conservative_sprintf_bytes(int tok, int fmt_idx, SValue *all_args, int total_args,
                                              unsigned long long *out_bytes)
{
  int fmt_len = 0;
  const char *fmt;

  if (total_args <= fmt_idx)
    return 0;

  fmt = try_get_constant_string(&all_args[fmt_idx], &fmt_len);
  if (!fmt)
    return 0;

  if (strchr(fmt, '%') == NULL)
  {
    *out_bytes = (unsigned long long)fmt_len + 1;
    return 1;
  }

  if (tok == TOK_builtin___sprintf_chk && strcmp(fmt, "%s") == 0 && total_args == fmt_idx + 2)
  {
    return svalue_get_conservative_string_bytes_u64(&all_args[fmt_idx + 1], out_bytes);
  }

  return 0;
}

void update_local_scalar_max_bound(SValue *dst, SValue *src)
{
  Sym *sym;
  unsigned long long max_value;
  unsigned long long max_strlen;

  if ((dst->r & VT_VALMASK) != VT_LOCAL)
    return;
  sym = find_local_scalar_sym_for_svalue(dst);
  if (!sym)
    return;

  if (!svalue_get_conservative_max_u64(src, &max_value))
  {
    sym->objsize_max_valid = 0;
    sym->objsize_max_value = 0;
  }

  if (svalue_get_conservative_max_u64(src, &max_value))
  {
    if (!sym->objsize_max_valid || max_value > sym->objsize_max_value)
      sym->objsize_max_value = max_value;
    sym->objsize_max_valid = 1;
  }

  if (svalue_get_conservative_string_bytes_u64(src, &max_strlen))
  {
    if (!sym->objsize_strlen_valid || max_strlen > sym->objsize_strlen_value)
      sym->objsize_strlen_value = max_strlen;
    sym->objsize_strlen_valid = 1;
  }
  else
  {
    sym->objsize_strlen_valid = 0;
    sym->objsize_strlen_value = 0;
  }
}
/* ============================================================================
 * Constant Folding for Math Builtins
 * ============================================================================
 * This allows compile-time evaluation of math functions when all arguments
 * are constant values, similar to GCC's constant folding for builtins.
 */

typedef enum
{
  FOLD_TYPE_FLOAT,
  FOLD_TYPE_DOUBLE,
  FOLD_TYPE_LONG_DOUBLE
} FoldType;

typedef struct
{
  const char *name;  /* Function name (e.g., "sin") */
  int num_args;      /* Number of arguments (1 or 2) */
  FoldType arg_type; /* Type of arguments */
  FoldType ret_type; /* Type of return value */
  union
  {
    double (*f1_d)(double);         /* Single-argument double function */
    double (*f2_d)(double, double); /* Two-argument double function */
    float (*f1_f)(float);           /* Single-argument float function */
    float (*f2_f)(float, float);    /* Two-argument float function */
  } func;
} FoldableMathFunc;

/* Table of foldable math functions */
static const FoldableMathFunc foldable_math_funcs[] = {
    /* Double-precision functions */
    {"sin", 1, FOLD_TYPE_DOUBLE, FOLD_TYPE_DOUBLE, {.f1_d = sin}},
    {"cos", 1, FOLD_TYPE_DOUBLE, FOLD_TYPE_DOUBLE, {.f1_d = cos}},
    {"tan", 1, FOLD_TYPE_DOUBLE, FOLD_TYPE_DOUBLE, {.f1_d = tan}},
    {"asin", 1, FOLD_TYPE_DOUBLE, FOLD_TYPE_DOUBLE, {.f1_d = asin}},
    {"acos", 1, FOLD_TYPE_DOUBLE, FOLD_TYPE_DOUBLE, {.f1_d = acos}},
    {"atan", 1, FOLD_TYPE_DOUBLE, FOLD_TYPE_DOUBLE, {.f1_d = atan}},
    {"atan2", 2, FOLD_TYPE_DOUBLE, FOLD_TYPE_DOUBLE, {.f2_d = atan2}},
    {"sinh", 1, FOLD_TYPE_DOUBLE, FOLD_TYPE_DOUBLE, {.f1_d = sinh}},
    {"cosh", 1, FOLD_TYPE_DOUBLE, FOLD_TYPE_DOUBLE, {.f1_d = cosh}},
    {"tanh", 1, FOLD_TYPE_DOUBLE, FOLD_TYPE_DOUBLE, {.f1_d = tanh}},
    {"exp", 1, FOLD_TYPE_DOUBLE, FOLD_TYPE_DOUBLE, {.f1_d = exp}},
    {"log", 1, FOLD_TYPE_DOUBLE, FOLD_TYPE_DOUBLE, {.f1_d = log}},
    {"log10", 1, FOLD_TYPE_DOUBLE, FOLD_TYPE_DOUBLE, {.f1_d = log10}},
    {"pow", 2, FOLD_TYPE_DOUBLE, FOLD_TYPE_DOUBLE, {.f2_d = pow}},
    {"sqrt", 1, FOLD_TYPE_DOUBLE, FOLD_TYPE_DOUBLE, {.f1_d = sqrt}},
    {"cbrt", 1, FOLD_TYPE_DOUBLE, FOLD_TYPE_DOUBLE, {.f1_d = cbrt}},
    {"ceil", 1, FOLD_TYPE_DOUBLE, FOLD_TYPE_DOUBLE, {.f1_d = ceil}},
    {"floor", 1, FOLD_TYPE_DOUBLE, FOLD_TYPE_DOUBLE, {.f1_d = floor}},
    {"round", 1, FOLD_TYPE_DOUBLE, FOLD_TYPE_DOUBLE, {.f1_d = round}},
    {"trunc", 1, FOLD_TYPE_DOUBLE, FOLD_TYPE_DOUBLE, {.f1_d = trunc}},
    {"fabs", 1, FOLD_TYPE_DOUBLE, FOLD_TYPE_DOUBLE, {.f1_d = fabs}},
    {"fmod", 2, FOLD_TYPE_DOUBLE, FOLD_TYPE_DOUBLE, {.f2_d = fmod}},
    {"remainder", 2, FOLD_TYPE_DOUBLE, FOLD_TYPE_DOUBLE, {.f2_d = remainder}},
    {"copysign", 2, FOLD_TYPE_DOUBLE, FOLD_TYPE_DOUBLE, {.f2_d = copysign}},

    /* Single-precision functions */
    {"sinf", 1, FOLD_TYPE_FLOAT, FOLD_TYPE_FLOAT, {.f1_f = sinf}},
    {"cosf", 1, FOLD_TYPE_FLOAT, FOLD_TYPE_FLOAT, {.f1_f = cosf}},
    {"tanf", 1, FOLD_TYPE_FLOAT, FOLD_TYPE_FLOAT, {.f1_f = tanf}},
    {"asinf", 1, FOLD_TYPE_FLOAT, FOLD_TYPE_FLOAT, {.f1_f = asinf}},
    {"acosf", 1, FOLD_TYPE_FLOAT, FOLD_TYPE_FLOAT, {.f1_f = acosf}},
    {"atanf", 1, FOLD_TYPE_FLOAT, FOLD_TYPE_FLOAT, {.f1_f = atanf}},
    {"atan2f", 2, FOLD_TYPE_FLOAT, FOLD_TYPE_FLOAT, {.f2_f = atan2f}},
    {"sinhf", 1, FOLD_TYPE_FLOAT, FOLD_TYPE_FLOAT, {.f1_f = sinhf}},
    {"coshf", 1, FOLD_TYPE_FLOAT, FOLD_TYPE_FLOAT, {.f1_f = coshf}},
    {"tanhf", 1, FOLD_TYPE_FLOAT, FOLD_TYPE_FLOAT, {.f1_f = tanhf}},
    {"expf", 1, FOLD_TYPE_FLOAT, FOLD_TYPE_FLOAT, {.f1_f = expf}},
    {"logf", 1, FOLD_TYPE_FLOAT, FOLD_TYPE_FLOAT, {.f1_f = logf}},
    {"log10f", 1, FOLD_TYPE_FLOAT, FOLD_TYPE_FLOAT, {.f1_f = log10f}},
    {"powf", 2, FOLD_TYPE_FLOAT, FOLD_TYPE_FLOAT, {.f2_f = powf}},
    {"sqrtf", 1, FOLD_TYPE_FLOAT, FOLD_TYPE_FLOAT, {.f1_f = sqrtf}},
    {"cbrtf", 1, FOLD_TYPE_FLOAT, FOLD_TYPE_FLOAT, {.f1_f = cbrtf}},
    {"ceilf", 1, FOLD_TYPE_FLOAT, FOLD_TYPE_FLOAT, {.f1_f = ceilf}},
    {"floorf", 1, FOLD_TYPE_FLOAT, FOLD_TYPE_FLOAT, {.f1_f = floorf}},
    {"roundf", 1, FOLD_TYPE_FLOAT, FOLD_TYPE_FLOAT, {.f1_f = roundf}},
    {"truncf", 1, FOLD_TYPE_FLOAT, FOLD_TYPE_FLOAT, {.f1_f = truncf}},
    {"fabsf", 1, FOLD_TYPE_FLOAT, FOLD_TYPE_FLOAT, {.f1_f = fabsf}},
    {"fmodf", 2, FOLD_TYPE_FLOAT, FOLD_TYPE_FLOAT, {.f2_f = fmodf}},
    {"remainderf", 2, FOLD_TYPE_FLOAT, FOLD_TYPE_FLOAT, {.f2_f = remainderf}},
    {"copysignf", 2, FOLD_TYPE_FLOAT, FOLD_TYPE_FLOAT, {.f2_f = copysignf}},
};

#define NUM_FOLDABLE_MATH_FUNCS (sizeof(foldable_math_funcs) / sizeof(foldable_math_funcs[0]))

/* When parsing the body of an inline-expanded callee, references to a
 * parameter are loaded as VT_LOCAL | VT_LVAL from the slot where the
 * caller's argument was stored. If that argument was a compile-time
 * constant at the call site, the inliner recorded it in
 * inline_const_args[]; this helper rewrites sv back to the original
 * constant SValue so the builtin/math fold paths can see it.
 *
 * Why: covers cases like foo(inff(), nanf("")) where foo's body contains
 * __builtin_fabsf(x)/__builtin_isinf(x) — without substitution the
 * builtin sees the post-store local load and falls back to a runtime
 * libcall instead of folding.
 *
 * How to apply: call before any "is this VT_CONST?" check in a builtin
 * handler that wants to fold a constant FP/integer argument. Safe to
 * call unconditionally — no-op outside inline expansion. */
void inline_subst_const_arg(SValue *sv)
{
  if (!tcc_state->in_inline_expansion)
    return;
  if ((sv->r & (VT_VALMASK | VT_LVAL)) != (VT_LOCAL | VT_LVAL))
    return;
  for (int i = 0; i < tcc_state->inline_const_arg_count; i++)
  {
    if (tcc_state->inline_const_args[i].vreg == sv->vr &&
        tcc_state->inline_const_args[i].stack_offset == sv->c.i)
    {
      *sv = tcc_state->inline_const_args[i].value;
      return;
    }
  }
}

/* Check if a value is a compile-time constant suitable for folding */
int is_const_for_folding(SValue *sv)
{
  /* See an inlined parameter that was bound to a constant arg through. */
  inline_subst_const_arg(sv);

  /* Must be VT_CONST without VT_SYM (symbolic constants can't be folded) */
  if ((sv->r & (VT_VALMASK | VT_LVAL | VT_SYM)) != VT_CONST)
    return 0;

  /* Must be a floating point type or integer */
  int bt = sv->type.t & VT_BTYPE;
  if (bt != VT_FLOAT && bt != VT_DOUBLE && bt != VT_LDOUBLE && bt != VT_INT && bt != VT_SHORT && bt != VT_BYTE &&
      bt != VT_LLONG)
    return 0;

  return 1;
}

/* Extract double value from SValue */
double get_const_double(SValue *sv)
{
  int bt = sv->type.t & VT_BTYPE;
  switch (bt)
  {
  case VT_FLOAT:
    return (double)sv->c.f;
  case VT_DOUBLE:
    return sv->c.d;
  case VT_LDOUBLE:
    return (double)sv->c.ld;
  case VT_INT:
    return (double)(int)sv->c.i;
  case VT_SHORT:
    return (double)(short)sv->c.i;
  case VT_BYTE:
    return (double)(char)sv->c.i;
  case VT_LLONG:
    return (double)(long long)sv->c.i;
  default:
    return 0.0;
  }
}

/* Extract float value from SValue */
float get_const_float(SValue *sv)
{
  int bt = sv->type.t & VT_BTYPE;
  switch (bt)
  {
  case VT_FLOAT:
    return sv->c.f;
  case VT_DOUBLE:
    return (float)sv->c.d;
  case VT_LDOUBLE:
    return (float)sv->c.ld;
  case VT_INT:
    return (float)(int)sv->c.i;
  case VT_SHORT:
    return (float)(short)sv->c.i;
  case VT_BYTE:
    return (float)(char)sv->c.i;
  case VT_LLONG:
    return (float)(long long)sv->c.i;
  default:
    return 0.0f;
  }
}

const char *get_value_type(int r)
{
  return NULL;
}

/* Try to constant-fold a math function call.
 * Returns 1 if folding was successful, 0 otherwise.
 * On success, the function result is pushed onto the value stack.
 */
int try_fold_math_call(const char *func_name, SValue *args, int nb_args)
{
  const FoldableMathFunc *fmf = NULL;
  int i;

  /* Look up the function in our table */
  for (i = 0; i < NUM_FOLDABLE_MATH_FUNCS; i++)
  {
    if (strcmp(foldable_math_funcs[i].name, func_name) == 0)
    {
      fmf = &foldable_math_funcs[i];
      break;
    }
  }

  if (!fmf)
    return 0;

  /* Check argument count */
  if (nb_args != fmf->num_args)
    return 0;

  /* Check if all arguments are constants */
  for (i = 0; i < nb_args; i++)
  {
    if (!is_const_for_folding(&args[i]))
      return 0;
  }

  /* Evaluate the function at compile time */
  CValue result;
  memset(&result, 0, sizeof(result));

  if (fmf->arg_type == FOLD_TYPE_DOUBLE)
  {
    if (fmf->num_args == 1)
    {
      double arg = get_const_double(&args[0]);
      double res = fmf->func.f1_d(arg);

      if (fmf->ret_type == FOLD_TYPE_DOUBLE)
        result.d = res;
      else if (fmf->ret_type == FOLD_TYPE_FLOAT)
        result.f = (float)res;
    }
    else
    {
      double arg1 = get_const_double(&args[0]);
      double arg2 = get_const_double(&args[1]);
      double res = fmf->func.f2_d(arg1, arg2);

      if (fmf->ret_type == FOLD_TYPE_DOUBLE)
        result.d = res;
      else if (fmf->ret_type == FOLD_TYPE_FLOAT)
        result.f = (float)res;
    }
  }
  else
  { /* FOLD_TYPE_FLOAT */
    if (fmf->num_args == 1)
    {
      float arg = get_const_float(&args[0]);
      float res = fmf->func.f1_f(arg);
      result.f = res;
    }
    else
    {
      float arg1 = get_const_float(&args[0]);
      float arg2 = get_const_float(&args[1]);
      float res = fmf->func.f2_f(arg1, arg2);
      result.f = res;
    }
  }

  /* Push the result onto the value stack */
  CType result_type;
  result_type.ref = NULL;

  if (fmf->ret_type == FOLD_TYPE_DOUBLE)
    result_type.t = VT_DOUBLE;
  else if (fmf->ret_type == FOLD_TYPE_FLOAT)
    result_type.t = VT_FLOAT;
  else
    result_type.t = VT_LDOUBLE;

  /* For C standard compliance: only fold finite results */
  double res_d = (fmf->ret_type == FOLD_TYPE_DOUBLE) ? result.d : (double)result.f;
  if (!ieee_finite(res_d))
    return 0;

  vsetc(&result_type, VT_CONST, &result);

  return 1;
}

/* Constant-fold complex-number library calls: conj{f,,l}, creal{f,,l}, cimag{f,,l}.
 * Returns 1 if folded (result pushed onto vstack), 0 otherwise. */
int try_fold_complex_call(const char *func_name, SValue *args, int nb_args)
{
  if (nb_args != 1)
    return 0;

  /* Determine operation and type variant */
  enum
  {
    CFOLD_CONJ,
    CFOLD_CREAL,
    CFOLD_CIMAG
  } op;
  int bt; /* base type: VT_FLOAT, VT_DOUBLE, or VT_LDOUBLE */

  if (strcmp(func_name, "conjf") == 0 || strcmp(func_name, "__builtin_conjf") == 0)
  {
    op = CFOLD_CONJ;
    bt = VT_FLOAT;
  }
  else if (strcmp(func_name, "conj") == 0 || strcmp(func_name, "__builtin_conj") == 0)
  {
    op = CFOLD_CONJ;
    bt = VT_DOUBLE;
  }
  else if (strcmp(func_name, "conjl") == 0 || strcmp(func_name, "__builtin_conjl") == 0)
  {
    op = CFOLD_CONJ;
    bt = VT_LDOUBLE;
  }
  else if (strcmp(func_name, "crealf") == 0 || strcmp(func_name, "__builtin_crealf") == 0)
  {
    op = CFOLD_CREAL;
    bt = VT_FLOAT;
  }
  else if (strcmp(func_name, "creal") == 0 || strcmp(func_name, "__builtin_creal") == 0)
  {
    op = CFOLD_CREAL;
    bt = VT_DOUBLE;
  }
  else if (strcmp(func_name, "creall") == 0 || strcmp(func_name, "__builtin_creall") == 0)
  {
    op = CFOLD_CREAL;
    bt = VT_LDOUBLE;
  }
  else if (strcmp(func_name, "cimagf") == 0 || strcmp(func_name, "__builtin_cimagf") == 0)
  {
    op = CFOLD_CIMAG;
    bt = VT_FLOAT;
  }
  else if (strcmp(func_name, "cimag") == 0 || strcmp(func_name, "__builtin_cimag") == 0)
  {
    op = CFOLD_CIMAG;
    bt = VT_DOUBLE;
  }
  else if (strcmp(func_name, "cimagl") == 0 || strcmp(func_name, "__builtin_cimagl") == 0)
  {
    op = CFOLD_CIMAG;
    bt = VT_LDOUBLE;
  }
  else
    return 0;

  /* Argument must be a constant */
  SValue *arg = &args[0];
  if ((arg->r & (VT_VALMASK | VT_LVAL | VT_SYM)) != VT_CONST)
    return 0;

  CValue result;
  memset(&result, 0, sizeof(result));
  CType result_type;
  result_type.ref = NULL;

  if (bt == VT_FLOAT)
  {
    union
    {
      float f;
      uint32_t u;
    } r, im;
    r.u = (uint32_t)(arg->c.i & 0xFFFFFFFF);
    im.u = (uint32_t)(arg->c.i >> 32);

    if (op == CFOLD_CONJ)
    {
      im.f = -im.f;
      result.i = (uint64_t)r.u | ((uint64_t)im.u << 32);
      result_type.t = VT_FLOAT | VT_COMPLEX;
    }
    else if (op == CFOLD_CREAL)
    {
      result.f = r.f;
      result_type.t = VT_FLOAT;
    }
    else
    {
      result.f = im.f;
      result_type.t = VT_FLOAT;
    }
  }
  else
  {
    /* double / long double (both 8-byte on target) */
    double src_real, src_imag;
    memcpy(&src_real, &arg->c, 8);
    memcpy(&src_imag, (char *)&arg->c + 8, 8);

    if (op == CFOLD_CONJ)
    {
      src_imag = -src_imag;
      memcpy(&result, &src_real, 8);
      memcpy((char *)&result + 8, &src_imag, 8);
      result_type.t = bt | VT_COMPLEX;
    }
    else if (op == CFOLD_CREAL)
    {
      result.d = src_real;
      result_type.t = bt;
    }
    else
    {
      result.d = src_imag;
      result_type.t = bt;
    }
  }

#ifdef TCC_USING_DOUBLE_FOR_LDOUBLE
  /* On ARM target, long double == double. Normalize so that the folded
   * result type matches what the parser produces for 1.0L literals
   * (VT_DOUBLE rather than VT_LDOUBLE). */
  if ((result_type.t & VT_BTYPE) == VT_LDOUBLE)
    result_type.t = (result_type.t & ~VT_BTYPE) | VT_DOUBLE;
#endif

  vsetc(&result_type, VT_CONST, &result);
  return 1;
}
