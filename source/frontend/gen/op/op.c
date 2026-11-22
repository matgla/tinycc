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

/* op.c -- Binary operator dispatch (gen_op).
 * Split out of tccgen.c; see docs/plan_tccgen_split.md. */

#include "gen_priv.h"

/* ---------------------------------------------------------------------
 * The checks of an operator the source wrote (gen_op_checked): constant
 * shift counts, division by the constant 0, signed overflow folding two
 * constants, and the comparisons (-Wsign-compare, -Wtautological-compare,
 * -Wtype-limits).  Read on the operands before gen_op converts them.  Not in
 * code that is never evaluated (sizeof, the dead arm of a constant ?:, &&
 * or ||), nor in #if, as gcc.  Dead statements (`if (0) x = 1 / 0;`) are
 * still checked, also as gcc.
 * --------------------------------------------------------------------- */

/* the frontend checks may speak: an evaluated operand of the source */
int fe_eval_checks_wanted(void)
{
  return !NOEVAL_WANTED && !pp_expr;
}

/* a plain integer constant (not one only the folder made) */
int fe_int_const(const SValue *sv)
{
  return (sv->r & (VT_VALMASK | VT_LVAL | VT_SYM | VT_NONCONST)) == VT_CONST &&
         is_integer_btype(sv->type.t & VT_BTYPE) && !(sv->type.t & (VT_COMPLEX | VT_VECTOR));
}

/* an integer value: its width and signedness, bit-fields and _Bool included */
int fe_int_bits(int t)
{
  if (t & VT_BITFIELD)
    return BIT_SIZE(t);
  if ((t & VT_BTYPE) == VT_BOOL)
    return 1;
  return btype_size(t & VT_BTYPE) * 8;
}

int fe_int_unsigned(int t)
{
  return (t & VT_UNSIGNED) || (t & VT_BTYPE) == VT_BOOL;
}

/* the type after the integer promotions: 32 or 64 bits wide, *uns set if unsigned */
static int fe_promoted_bits(int t, int *uns)
{
  int bits = fe_int_bits(t);
  if (bits < 32 || (bits == 32 && (t & VT_BTYPE) != VT_INT && (t & VT_BTYPE) != VT_LLONG))
  {
    *uns = 0; /* everything narrower than int fits in int */
    return 32;
  }
  *uns = fe_int_unsigned(t);
  return (t & VT_BTYPE) == VT_LLONG ? 64 : 32;
}

/* the value of an integer constant: *big set for an unsigned 64-bit value
   at or above 2^63 (then the return is its bits) */
int64_t fe_const_value(const SValue *sv, int *big)
{
  int t = sv->type.t;
  uint64_t v = value64(sv->c.i, t);
  *big = (t & VT_BTYPE) == VT_LLONG && (t & VT_UNSIGNED) && (int64_t)v < 0;
  return (int64_t)v;
}

static void fe_type_str(char *buf, int size, int t)
{
  CType ct;
  ct.t = t & ~(VT_STORAGE | VT_CONSTANT | VT_VOLATILE);
  ct.ref = NULL;
  if (!(ct.t & VT_BITFIELD))
    ct.t &= ~VT_STRUCT_MASK;
  type_to_str(buf, size, &ct, NULL);
}

static void fe_check_shift(int op)
{
  SValue *a = vtop - 1, *b = vtop;
  int uns, big, width;
  int64_t n;
  const char *dir = op == TOK_SHL ? "left" : "right";
  if (!fe_int_const(b) || !is_integer_btype(a->type.t & VT_BTYPE))
    return;
  width = fe_promoted_bits(a->type.t, &uns);
  n = fe_const_value(b, &big);
  if (n < 0 && !big)
    tcc_fe_warning_c(warn_shift_count_negative, "%s shift count is negative", dir);
  else if (big || n >= width)
    tcc_fe_warning_c(warn_shift_count_overflow, "%s shift count >= width of type", dir);
  else if (op == TOK_SHL && !uns && fe_int_const(a) && tcc_state->warn_shift_overflow)
  {
    /* -Wshift-overflow (gcc's level 1): a positive constant shifted past the
       sign bit; into it is allowed */
    int64_t v = fe_const_value(a, &big);
    int prec = 0;
    char tn[64];
    if (width == 32)
      v = (int32_t)v;
    if (v <= 0)
      return;
    while (prec < 64 && (v >> prec))
      prec++;
    if (prec + n <= width)
      return;
    fe_type_str(tn, sizeof tn, width == 64 ? VT_LLONG : VT_INT);
    tcc_fe_warning_c(warn_shift_overflow, "result of '%lld << %lld' requires %d bits to represent, but '%s' only has %d bits",
                     (long long)v, (long long)n, (int)(prec + n + 1), tn, width);
  }
}

static void fe_check_div(void)
{
  int big;
  if (fe_int_const(vtop) && fe_const_value(vtop, &big) == 0)
    tcc_fe_warning_c(warn_div_by_zero, "division by zero");
}

/* two integer constants folded in a signed type: does the result fit? */
static void fe_check_overflow(int op)
{
  SValue *a = vtop - 1, *b = vtop;
  int ua, ub, wa, wb, w, big;
  int64_t x, y, r;
  char tn[64];
  if (!fe_int_const(a) || !fe_int_const(b))
    return;
  wa = fe_promoted_bits(a->type.t, &ua);
  wb = fe_promoted_bits(b->type.t, &ub);
  w = wa > wb ? wa : wb;
  if ((wa == wb && (ua || ub)) || (wa > wb && ua) || (wb > wa && ub))
    return; /* unsigned arithmetic wraps */
  x = fe_const_value(a, &big);
  y = fe_const_value(b, &big);
  if (w == 32)
  {
    int64_t r64;
    x = (int32_t)x, y = (int32_t)y;
    switch (op)
    {
    case '+': r64 = x + y; break;
    case '-': r64 = x - y; break;
    case '*': r64 = x * y; break;
    case '/':
      if (x != INT32_MIN || y != -1)
        return;
      r64 = -(int64_t)INT32_MIN;
      break;
    default: return;
    }
    if (r64 == (int32_t)r64)
      return;
    r = (int32_t)(uint32_t)r64;
  }
  else
  {
    uint64_t ux = (uint64_t)x, uy = (uint64_t)y;
    switch (op)
    {
    case '+':
      r = (int64_t)(ux + uy);
      if (!((x < 0) == (y < 0) && (r < 0) != (x < 0)))
        return;
      break;
    case '-':
      r = (int64_t)(ux - uy);
      if (!((x < 0) != (y < 0) && (r < 0) != (x < 0)))
        return;
      break;
    case '*':
      if (x == 0 || y == 0)
        return;
      r = (int64_t)(ux * uy);
      if (!((x == -1 && y == INT64_MIN) || (y == -1 && x == INT64_MIN) || r / y != x))
        return;
      break;
    case '/':
      if (!(x == INT64_MIN && y == -1))
        return;
      r = INT64_MIN;
      break;
    default: return;
    }
  }
  fe_type_str(tn, sizeof tn, w == 64 ? VT_LLONG : VT_INT);
  tcc_fe_warning_c(warn_overflow, "integer overflow in expression of type '%s' results in '%lld'", tn, (long long)r);
}

/* -Warray-bounds through pointer arithmetic: `a + k` (a an array of known
 * size, k a constant) kept by its stack place and contents, so `*(a + 4)`,
 * `(a + 2)[3]` and `(a + 4)->m` are checked as a[4], a[5], a[4] */
static struct
{
  SValue *at;
  SValue sv;
  int64_t k;
  int n;
  char tname[64];
} fe_aptr, fe_flex;

static int fe_sv_is(const SValue *sv, const SValue *at, const SValue *rec)
{
  return at == sv && sv->r == rec->r && sv->vr == rec->vr && sv->c.i == rec->c.i && sv->type.t == rec->type.t &&
         sv->type.ref == rec->type.ref && sv->sym == rec->sym;
}

/* vtop is a trailing array member taken as flexible (unary.c) */
ST_FUNC void fe_note_flex_member(void)
{
  fe_flex.at = vtop;
  fe_flex.sv = *vtop;
}

/* SV is `a + k` (fe_aptr): K and the type's name; the array's size, or -1 */
ST_FUNC int fe_array_ptr_bounds(SValue *sv, int64_t *k, char *tbuf, int tsize)
{
  if (!fe_aptr.at || !fe_sv_is(sv, fe_aptr.at, &fe_aptr.sv))
    return -1;
  *k = fe_aptr.k;
  pstrcpy(tbuf, tsize, fe_aptr.tname);
  return fe_aptr.n;
}

/* `*vtop`: index ALLOW_END (the operand of &) may be one past the end */
ST_FUNC void fe_check_deref_bounds(int allow_end)
{
  int64_t k;
  char tn[64];
  int n;
  if (!fe_eval_checks_wanted() || (nocode_wanted & CODE_OFF_BIT) || (n = fe_array_ptr_bounds(vtop, &k, tn, sizeof tn)) < 0)
    return;
  if (k < 0 || k > n || (k == n && !allow_end))
    tcc_fe_warning_c(warn_array_bounds, "array subscript %lld is %s array bounds of '%s'", (long long)k,
                     k < 0 ? "below" : "above", tn);
}

/* gen_op('+' / '-') of an array of known size (or of `a + k`) and a constant */
static void fe_array_arith(int op)
{
  SValue *arr = vtop - 1, *k = vtop;
  int64_t base = 0, kv;
  int n, big;
  char tn[64];
  if (op == '+' && !fe_int_const(k) && fe_int_const(arr))
    arr = vtop, k = vtop - 1;
  if (!fe_int_const(k))
    goto plain;
  kv = fe_const_value(k, &big);
  if (big)
    goto plain;
  if ((n = fe_array_ptr_bounds(arr, &base, tn, sizeof tn)) < 0)
  {
    CType ct;
    if (!(arr->type.t & VT_ARRAY) || (arr->type.t & VT_VLA) || !arr->type.ref || arr->type.ref->c < 0 ||
        arr->union_member || (fe_flex.at && fe_sv_is(arr, fe_flex.at, &fe_flex.sv)))
      goto plain;
    n = arr->type.ref->c;
    ct = arr->type;
    ct.t &= ~(VT_STORAGE | VT_CONSTANT | VT_VOLATILE);
    type_to_str(tn, sizeof tn, &ct, NULL);
    for (char *p = tn; (p = strstr(p, " [")) != NULL;) /* gcc's 'int[4]' */
      memmove(p, p + 1, strlen(p));
  }
  gen_op(op);
  fe_aptr.k = op == '+' ? base + kv : base - kv;
  fe_aptr.n = n;
  pstrcpy(fe_aptr.tname, sizeof fe_aptr.tname, tn);
  fe_aptr.at = vtop;
  fe_aptr.sv = *vtop;
  return;
plain:
  gen_op(op);
}

static int fe_narrow_nonneg(const SValue *sv);

/* comparisons: vtop[-1] OP vtop */
static int fe_known_nonneg(const SValue *sv)
{
  int big, v = sv->r & VT_VALMASK;
  if (fe_int_const(sv))
  {
    int64_t c = fe_const_value(sv, &big);
    return c >= 0 || big;
  }
  /* a comparison or a logical operator: 0 or 1 */
  if (v == VT_CMP || v == VT_JMP || v == VT_JMPI)
    return 1;
  /* an unsigned value narrower than int, promoted */
  if (fe_int_unsigned(sv->type.t) && fe_int_bits(sv->type.t) < 32)
    return 1;
  return fe_narrow_nonneg(sv); /* `x & 0xff`, `uc + 1` */
}

static void fe_check_sign_compare(int op)
{
  SValue *a = vtop - 1, *b = vtop;
  int ua, ub, wa, wb, big;
  char ta[64], tb[64];
  wa = fe_promoted_bits(a->type.t, &ua);
  wb = fe_promoted_bits(b->type.t, &ub);
  /* the comparison is unsigned and converts a signed operand */
  if (ua == ub || (wa > wb && !ua) || (wb > wa && !ub))
    return;
  SValue *sgn = ua ? b : a, *usg = ua ? a : b;
  if (fe_known_nonneg(sgn))
    return;
  /* x == 3u: the constant would fit as signed, the result is the same */
  if ((op == TOK_EQ || op == TOK_NE) && fe_int_const(usg))
  {
    int64_t v = fe_const_value(usg, &big);
    if (!big && v >= 0 && ((wa > wb ? wa : wb) == 64 || v <= INT32_MAX))
      return;
  }
  fe_type_str(ta, sizeof ta, a->type.t);
  fe_type_str(tb, sizeof tb, b->type.t);
  tcc_fe_warning_c(warn_sign_compare, "comparison of integer expressions of different signedness: '%s' and '%s'", ta,
                   tb);
}

/* the same lvalue on both sides, read without side effects */
static int fe_self_compare_simple(void)
{
  SValue *a = vtop - 1, *b = vtop;
  int bt = a->type.t & VT_BTYPE;
  if (!(a->r & VT_LVAL) || a->r != b->r || a->vr != b->vr || a->c.i != b->c.i || a->type.t != b->type.t ||
      a->type.ref != b->type.ref || ((a->r & VT_SYM) && a->sym != b->sym))
    return 0;
  /* x != x is the NaN test; a volatile read is not the same twice */
  if (is_float(bt) || (a->type.t & (VT_VOLATILE | VT_COMPLEX | VT_VECTOR)) || a->volatile_access ||
      b->volatile_access || bt == VT_STRUCT)
    return 0;
  /* only a value at a known place: a variable, a member of one */
  if ((a->r & VT_VALMASK) != VT_LOCAL && !(a->r & VT_PARAM) && (a->r & (VT_VALMASK | VT_SYM)) != (VT_CONST | VT_SYM))
    return 0;
  return 1;
}

static void fe_check_self_compare(int op)
{
  if (!fe_self_compare_simple())
    return;
  tcc_fe_warning_c(warn_tautological_compare, "self-comparison always evaluates to %s",
                   op == TOK_EQ || op == TOK_LE || op == TOK_GE ? "true" : "false");
}

/* vtop[-1] OP vtop, two operands that read the same tokens with no side
   effect (expr/infix.c): the same value, as gcc's operand_equal_p -- not a
   float (x != x is the NaN test), a volatile read, two constants (a macro
   testing a feature) or two truth values (gcc says nothing of them) */
ST_FUNC void fe_check_same_operands(int op)
{
  SValue *a = vtop - 1, *b = vtop;
  int bt = a->type.t & VT_BTYPE, va = a->r & VT_VALMASK, vb = b->r & VT_VALMASK;
  if (!fe_eval_checks_wanted() || is_float(bt) || is_float(b->type.t & VT_BTYPE) ||
      ((a->type.t | b->type.t) & (VT_VOLATILE | VT_COMPLEX | VT_VECTOR)) || a->volatile_access ||
      b->volatile_access || bt == VT_STRUCT || (fe_int_const(a) && fe_int_const(b)) || va == VT_CMP ||
      va == VT_JMP || va == VT_JMPI || vb == VT_CMP || vb == VT_JMP || vb == VT_JMPI)
    return;
  if (tcc_state->warn_tautological_compare && fe_self_compare_simple())
    return; /* fe_check_self_compare says it */
  tcc_fe_warning_c(warn_tautological_compare, "self-comparison always evaluates to %s",
                   op == TOK_EQ || op == TOK_LE || op == TOK_GE ? "true" : "false");
}

/* -Wtype-limits: an unsigned value against 0, a char or short against a
   constant out of its range */
static void fe_check_type_limits(int op)
{
  SValue *a = vtop - 1, *b = vtop, *var, *k;
  int big, uv, uk, wv, wk, bits;
  int64_t c, lo, hi;
  if (fe_int_const(b) && !fe_int_const(a))
    var = a, k = b;
  else if (fe_int_const(a) && !fe_int_const(b))
  {
    /* K op x as x op' K */
    var = b, k = a;
    switch (op)
    {
    case TOK_LT: op = TOK_GT; break;
    case TOK_GT: op = TOK_LT; break;
    case TOK_LE: op = TOK_GE; break;
    case TOK_GE: op = TOK_LE; break;
    }
  }
  else
    return;
  if (!is_integer_btype(var->type.t & VT_BTYPE) || (var->type.t & VT_BTYPE) == VT_BOOL)
    return;
  c = fe_const_value(k, &big);
  wv = fe_promoted_bits(var->type.t, &uv);
  wk = fe_promoted_bits(k->type.t, &uk);
  bits = fe_int_bits(var->type.t);
  if (uv && !big && c == 0 && (op == TOK_LT || op == TOK_GE))
  {
    tcc_fe_warning_c(warn_type_limits, "comparison of unsigned expression in '%s 0' is always %s",
                     op == TOK_LT ? "<" : ">=", op == TOK_LT ? "false" : "true");
    return;
  }
  /* a narrower type promoted, compared signed: its range against K */
  if (bits >= 32 || (var->type.t & VT_BITFIELD) || big)
    return;
  if (uk && wk >= wv) /* the comparison is unsigned */
  {
    if (!fe_int_unsigned(var->type.t))
      return;
    c = wk == 32 ? (int64_t)(uint32_t)c : c;
  }
  if (fe_int_unsigned(var->type.t))
    lo = 0, hi = ((int64_t)1 << bits) - 1;
  else
    lo = -((int64_t)1 << (bits - 1)), hi = ((int64_t)1 << (bits - 1)) - 1;
  int always = -1; /* 1 true, 0 false */
  switch (op)
  {
  case TOK_LT: always = hi < c ? 1 : lo >= c ? 0 : -1; break;
  case TOK_LE: always = hi <= c ? 1 : lo > c ? 0 : -1; break;
  case TOK_GT: always = lo > c ? 1 : hi <= c ? 0 : -1; break;
  case TOK_GE: always = lo >= c ? 1 : hi < c ? 0 : -1; break;
  case TOK_EQ: always = c < lo || c > hi ? 0 : -1; break;
  case TOK_NE: always = c < lo || c > hi ? 1 : -1; break;
  }
  if (always >= 0)
    tcc_fe_warning_c(warn_type_limits, "comparison is always %s due to limited range of data type",
                     always ? "true" : "false");
}

/* -Wtautological-compare's bitwise forms: the values `x & C` / `x | C`
 * computed last (gen_op_checked), by their place and contents on the stack */
static struct
{
  SValue *at;
  SValue sv;
  int op;
  uint64_t c;
} fe_bitop[2];
static int fe_bitop_next;

/* a function starts: its temporaries are new */
static void fe_narrow_reset(void);

ST_FUNC void fe_bitop_reset(void)
{
  memset(fe_bitop, 0, sizeof fe_bitop);
  fe_aptr.at = fe_flex.at = NULL;
  fe_narrow_reset();
}

static int fe_bitop_find(SValue *sv)
{
  for (int i = 0; i < 2; i++)
    if (fe_bitop[i].at == sv && sv->r == fe_bitop[i].sv.r && sv->vr == fe_bitop[i].sv.vr &&
        sv->c.i == fe_bitop[i].sv.c.i && sv->type.t == fe_bitop[i].sv.type.t && sv->sym == fe_bitop[i].sv.sym)
      return i;
  return -1;
}

/* `(x & 1) == 2`, `(x | 4) != 1`: a bit the constant needs is never (or
   always) there, as gcc */
static void fe_check_bitwise_compare(int op)
{
  SValue *a = vtop - 1, *b = vtop, *k;
  int i, big, uns;
  uint64_t c2, c1, mask;
  if ((i = fe_bitop_find(a)) >= 0 && fe_int_const(b))
    k = b;
  else if ((i = fe_bitop_find(b)) >= 0 && fe_int_const(a))
    k = a;
  else
    return;
  c2 = (uint64_t)fe_const_value(k, &big);
  c1 = fe_bitop[i].c;
  mask = fe_promoted_bits(fe_bitop[i].sv.type.t, &uns) == 64 ? ~(uint64_t)0 : 0xffffffffu;
  if (fe_bitop[i].op == '&' ? !(c2 & ~c1 & mask) : !(c1 & ~c2 & mask))
    return;
  tcc_fe_warning_c(warn_tautological_compare, "bitwise comparison always evaluates to %s",
                   op == TOK_EQ ? "false" : "true");
}

static void fe_check_compare(int op)
{
  SValue *a = vtop - 1, *b = vtop;
  int bta = a->type.t & VT_BTYPE, btb = b->type.t & VT_BTYPE;
  if (tcc_state->warn_tautological_compare)
  {
    fe_check_self_compare(op);
    if (op == TOK_EQ || op == TOK_NE)
      fe_check_bitwise_compare(op);
  }
  if (!is_integer_btype(bta) || !is_integer_btype(btb) || ((a->type.t | b->type.t) & (VT_COMPLEX | VT_VECTOR)))
    return;
  if (tcc_state->warn_sign_compare)
    fe_check_sign_compare(op);
  if (tcc_state->warn_type_limits)
    fe_check_type_limits(op);
}

/* unary minus of the most negative value: before it becomes 0 - x */
ST_FUNC void fe_check_neg(void)
{
  int uns, big, w;
  int64_t v;
  char tn[64];
  if (!tcc_state->warn_overflow || !fe_eval_checks_wanted() || !fe_int_const(vtop))
    return;
  w = fe_promoted_bits(vtop->type.t, &uns);
  v = fe_const_value(vtop, &big);
  if (uns || v != (w == 64 ? INT64_MIN : (int64_t)INT32_MIN))
    return;
  fe_type_str(tn, sizeof tn, w == 64 ? VT_LLONG : VT_INT);
  tcc_fe_warning_c(warn_overflow, "integer overflow in expression '%lld' of type '%s' results in '%lld'", (long long)v,
                   tn, (long long)v);
}

/* gen_op for an operator the source wrote: its checks first */
/* -Wconversion / -Wsign-conversion: the value ranges gcc's conversion_warning
 * sees in the expression, for the results of the operators the source
 * wrote, kept by their stack place and contents (fe_narrow_fits reads them):
 *  - `+ - * | ^ &` whose operands are all narrower than int (or constants,
 *    comparisons, such results): the result fits the widest operand type, as
 *    gcc's shortening has it -- `uc = uc + 1`, `s = sc * sc` are quiet;
 *  - a comparison: 0 or 1;
 *  - `x & C`: a constant that fits the target, signed and unsigned (or an
 *    unsigned one that fits) bounds the result, whatever x is. */
typedef struct FeNarrow
{
  SValue *at;
  SValue sv;
  unsigned char narrow, ubits, sbits, has_k, has_and, and_uns;
  int64_t kmin, kmax;
  uint64_t and_c;
} FeNarrow;
#define FE_NARROW_N 4
static FeNarrow fe_narrow[FE_NARROW_N];
static int fe_narrow_next;

/* the record of the value SV, which sits at AT on the stack */
static FeNarrow *fe_narrow_find_at(const SValue *sv, const SValue *at)
{
  for (int i = 0; i < FE_NARROW_N; i++)
  {
    FeNarrow *n = &fe_narrow[i];
    if (n->at == at && sv->r == n->sv.r && sv->vr == n->sv.vr && sv->c.i == n->sv.c.i && sv->type.t == n->sv.type.t &&
        sv->sym == n->sv.sym)
      return n;
  }
  return NULL;
}

static FeNarrow *fe_narrow_find(const SValue *sv)
{
  return fe_narrow_find_at(sv, sv);
}

/* SV (at AT) as an operand: what it adds to N's classes; 0 when it is a full int */
static int fe_narrow_operand_at(FeNarrow *n, const SValue *sv, const SValue *at)
{
  int t = sv->type.t, v = sv->r & VT_VALMASK, big;
  const FeNarrow *r;
  if (!is_integer_btype(t & VT_BTYPE) || (t & (VT_COMPLEX | VT_VECTOR)))
    return 0;
  if (fe_int_const(sv))
  {
    int64_t k = fe_const_value(sv, &big);
    if (big)
      return 0;
    if (!n->has_k || k < n->kmin)
      n->kmin = k;
    if (!n->has_k || k > n->kmax)
      n->kmax = k;
    n->has_k = 1;
    return 1;
  }
  if (v == VT_CMP || v == VT_JMP || v == VT_JMPI)
  {
    if (n->ubits < 1)
      n->ubits = 1;
    return 1;
  }
  if ((r = fe_narrow_find_at(sv, at)) && r->narrow)
  {
    if (r->ubits > n->ubits)
      n->ubits = r->ubits;
    if (r->sbits > n->sbits)
      n->sbits = r->sbits;
    if (r->has_k)
    {
      if (!n->has_k || r->kmin < n->kmin)
        n->kmin = r->kmin;
      if (!n->has_k || r->kmax > n->kmax)
        n->kmax = r->kmax;
      n->has_k = 1;
    }
    return 1;
  }
  {
    int bits = fe_int_bits(t);
    if (bits >= 32 && !(t & VT_BITFIELD))
      return 0;
    if (fe_int_unsigned(t))
    {
      if (bits > n->ubits)
        n->ubits = bits;
    }
    else if (bits > n->sbits)
      n->sbits = bits;
  }
  return 1;
}

static int fe_narrow_operand(FeNarrow *n, const SValue *sv)
{
  return fe_narrow_operand_at(n, sv, sv);
}

/* the operator OP applies to vtop[-1], vtop: what its result will be */
static int fe_narrow_begin(int op, FeNarrow *n)
{
  memset(n, 0, sizeof *n);
  if (TOK_ISCOND(op))
  {
    n->narrow = 1, n->ubits = 1;
    return 1;
  }
  if (op != '+' && op != '-' && op != '*' && op != '&' && op != '|' && op != '^')
    return 1; /* a result nothing is known of */
  if (op == '&')
    for (int i = 0; i < 2; i++)
    {
      int big;
      SValue *k = vtop - i;
      if (fe_int_const(k) && !fe_int_const(vtop - 1 + i))
      {
        n->has_and = 1;
        n->and_c = (uint64_t)fe_const_value(k, &big);
        n->and_uns = fe_int_unsigned(k->type.t);
        if (!n->and_uns && (int64_t)n->and_c < 0)
          n->has_and = 0;
      }
    }
  n->narrow = fe_narrow_operand(n, vtop - 1) && fe_narrow_operand(n, vtop);
  return 1;
}

static void fe_narrow_end(FeNarrow *n)
{
  FeNarrow *r = fe_narrow_find(vtop);
  if (!r)
  {
    r = &fe_narrow[fe_narrow_next];
    fe_narrow_next = (fe_narrow_next + 1) % FE_NARROW_N;
  }
  *r = *n;
  r->at = vtop;
  r->sv = *vtop;
}

/* vtop, a comparison's VT_CMP / VT_JMP, was just put in a register: 0 or 1 */
ST_FUNC void fe_narrow_note_bool(void)
{
  FeNarrow n;
  if (!(tcc_state->warn_conversion || tcc_state->warn_sign_conversion))
    return;
  memset(&n, 0, sizeof n);
  n.narrow = 1, n.ubits = 1;
  fe_narrow_end(&n);
}

/* A call returned a value of the narrow type RET (the register holds it
   promoted): what fits that type */
ST_FUNC void fe_narrow_note_type(int ret)
{
  FeNarrow n;
  int bits;
  if (!(tcc_state->warn_conversion || tcc_state->warn_sign_conversion) || !is_integer_btype(ret & VT_BTYPE) ||
      (ret & (VT_COMPLEX | VT_VECTOR)) || (bits = fe_int_bits(ret)) >= 32 || !is_integer_btype(vtop->type.t & VT_BTYPE))
    return;
  memset(&n, 0, sizeof n);
  n.narrow = 1;
  if (fe_int_unsigned(ret))
    n.ubits = bits;
  else
    n.sbits = bits;
  fe_narrow_end(&n);
}

/* An operator the parser applies itself (unary minus, `?:`): its operands'
   classes before (fe_narrow_pend_begin: vtop, and ARM -- the other `?:` arm,
   saved from where vtop is now -- or NULL), its result's after (_end) */
static FeNarrow fe_narrow_pend;
static unsigned char fe_narrow_pending;

ST_FUNC void fe_narrow_pend_begin(const SValue *arm)
{
  fe_narrow_pending = 0;
  if (!(tcc_state->warn_conversion || tcc_state->warn_sign_conversion) || !fe_eval_checks_wanted())
    return;
  memset(&fe_narrow_pend, 0, sizeof fe_narrow_pend);
  fe_narrow_pend.narrow = fe_narrow_operand(&fe_narrow_pend, vtop) && (!arm || fe_narrow_operand_at(&fe_narrow_pend, arm, vtop));
  fe_narrow_pending = 1;
}

ST_FUNC void fe_narrow_pend_end(void)
{
  if (fe_narrow_pending)
    fe_narrow_end(&fe_narrow_pend);
  fe_narrow_pending = 0;
}

/* SV is never negative: `x & C` with C >= 0, or a narrow result of
   unsigned operands (and nonnegative constants) only */
static int fe_narrow_nonneg(const SValue *sv)
{
  const FeNarrow *r = fe_narrow_find(sv);
  if (!r)
    return 0;
  if (r->has_and)
    return 1;
  return r->narrow && !r->sbits && (!r->has_k || r->kmin >= 0) && r->ubits < 31;
}

/* the value SV converted to the integer type DTT: does its known range fit? */
ST_FUNC int fe_narrow_fits(const SValue *sv, int dtt)
{
  const FeNarrow *r = fe_narrow_find(sv);
  int n = fe_int_bits(dtt), du = fe_int_unsigned(dtt);
  int64_t lo, hi;
  if (!r || n >= 64)
    return 0;
  lo = du ? 0 : -((int64_t)1 << (n - 1));
  hi = du ? ((int64_t)1 << n) - 1 : ((int64_t)1 << (n - 1)) - 1;
  if (r->has_and)
  {
    /* gcc: a constant that fits both the signed and the unsigned type of
       the target, or an unsigned one that fits the target */
    if (r->and_c < ((uint64_t)1 << (n - 1)) || (r->and_uns && r->and_c <= (uint64_t)hi))
      return 1;
  }
  if (!r->narrow)
    return 0;
  if (r->ubits && (du ? r->ubits > n : r->ubits >= n))
    return 0;
  if (r->sbits && (du || r->sbits > n))
    return 0;
  if (r->has_k && (r->kmin < lo || r->kmax > hi))
    return 0;
  if (r->has_k && (r->ubits | r->sbits))
  {
    /* gcc shortens only when the constants fit the narrow operands' type:
       `b1 * 1000` is an int */
    int64_t clo = r->sbits ? -((int64_t)1 << (r->sbits - 1)) : 0;
    int64_t chi = r->ubits ? ((int64_t)1 << r->ubits) - 1 : 0;
    if (r->sbits && ((int64_t)1 << (r->sbits - 1)) - 1 > chi)
      chi = ((int64_t)1 << (r->sbits - 1)) - 1;
    if (r->kmin < clo || r->kmax > chi)
      return 0;
  }
  return 1;
}

static void fe_narrow_reset(void)
{
  memset(fe_narrow, 0, sizeof fe_narrow);
}

static void gen_op_checked_1(int op);

ST_FUNC void gen_op_checked(int op)
{
  FeNarrow nr;
  const int narrow = (tcc_state->warn_conversion || tcc_state->warn_sign_conversion) && fe_eval_checks_wanted() &&
                     fe_narrow_begin(op, &nr);
  gen_op_checked_1(op);
  if (narrow)
    fe_narrow_end(&nr);
}

static void gen_op_checked_1(int op)
{
  TCCState *s = tcc_state;
  if ((op == '+' || op == '-') && s->warn_array_bounds && fe_eval_checks_wanted() && !(nocode_wanted & CODE_OFF_BIT) &&
      (((vtop[-1].type.t | vtop->type.t) & VT_ARRAY) || fe_aptr.at))
  {
    if (s->warn_overflow)
      fe_check_overflow(op);
    fe_array_arith(op);
    return;
  }
  if ((op == '&' || op == '|') && s->warn_tautological_compare && fe_eval_checks_wanted() &&
      fe_int_const(vtop) != fe_int_const(vtop - 1) && is_integer_btype(vtop[-1].type.t & VT_BTYPE) &&
      is_integer_btype(vtop->type.t & VT_BTYPE))
  {
    /* `x & C`: kept for a comparison of its value (fe_check_bitwise_compare) */
    int big, i = fe_bitop_next;
    fe_bitop_next ^= 1;
    fe_bitop[i].op = op;
    fe_bitop[i].c = (uint64_t)fe_const_value(fe_int_const(vtop) ? vtop : vtop - 1, &big);
    gen_op(op);
    fe_bitop[i].at = vtop;
    fe_bitop[i].sv = *vtop;
    return;
  }
  if (fe_eval_checks_wanted())
  {
    if (op == TOK_SHL || op == TOK_SAR)
    {
      if (s->warn_shift_count_overflow || s->warn_shift_count_negative || s->warn_shift_overflow)
        fe_check_shift(op);
    }
    else if (op == '/' || op == '%')
    {
      if (s->warn_div_by_zero)
        fe_check_div();
      if (s->warn_overflow)
        fe_check_overflow(op);
    }
    else if (op == '+' || op == '-' || op == '*')
    {
      if (s->warn_overflow)
        fe_check_overflow(op);
    }
    else if (TOK_ISCOND(op))
    {
      if (s->warn_tautological_compare || s->warn_sign_compare || s->warn_type_limits)
        fe_check_compare(op);
    }
  }
  if (s->sanitize && ubsan_gen_op(op))
    return;
  gen_op(op);
}

/* generic gen_op: handles types problems */
static HOT void gen_op_impl(int op);

/* Wrapper: propagate the `underaligned` access mark through pointer
 * arithmetic.  `packed->arr + i` / `packed->arr[i]` must reach the final
 * dereference still marked, or the backend would emit LDRD/STRD on an
 * unaligned address (UsageFault on ARMv7-M/v8-M regardless of UNALIGN_TRP).
 * Only '+' and '-' preserve address provenance; other operators produce
 * non-address values.  Over-marking is harmless (falls back to the
 * LDR/STR pair); under-marking faults, so this errs on the OR of both
 * operands. */
ST_FUNC HOT void gen_op(int op)
{
  int under = 0, sso = 0;
  if (yasc_pp_live && (op == TOK_EQ || op == TOK_NE))
    yasc_pp_compare(); /* a plain pointer parameter tested for null (plain_param.c) */
  if ((MAYBE_ERRUNION(&vtop->type) && type_is_errunion(&vtop->type)) ||
      (MAYBE_ERRUNION(&vtop[-1].type) && type_is_errunion(&vtop[-1].type)))
    yasc_errunion_misuse();
  /* A byte-reversed member operand (big-endian scalar_storage_order) is
   * loaded and swapped first, never handed to the IR as a memory operand.
   * What still carries the mark afterwards is an array member's address:
   * keep it on the pointer arithmetic so `s.arr[i]` is reversed too. */
  if ((vtop->r & VT_LVAL) && vtop->sso_reversed)
    gv(RC_TYPE(vtop->type.t));
  if ((vtop[-1].r & VT_LVAL) && vtop[-1].sso_reversed)
  {
    vswap();
    gv(RC_TYPE(vtop->type.t));
    vswap();
  }
  if (op == '+' || op == '-')
  {
    under = vtop[0].underaligned | vtop[-1].underaligned;
    sso = vtop[0].sso_reversed | vtop[-1].sso_reversed;
  }
  if (objsize_ctx | objsize_ab_watch)
  {
    /* __builtin_object_size: pointer +/- an integer moves in the object;
       -Warray-bounds in loops: the condition, the step, a[i + k] */
    ObjOpHook oh;
    const int on = objsize_ctx && objsize_op_begin(&oh, op);
    if (objsize_ab_watch)
      objsize_ab_op(op, 0);
    gen_op_impl(op);
    if (on)
      objsize_op_end(&oh);
    if (objsize_ab_watch)
      objsize_ab_op(op, 1);
  }
  else
    gen_op_impl(op);
  vtop->packed_addr = 0; /* &packed->m + 1 is not the member's address */
  if (under)
    vtop->underaligned = 1;
  vtop->sso_reversed = sso && (vtop->type.t & VT_BTYPE) == VT_PTR;
}

/* -Wpointer-arith: arithmetic on a pointer to void or to a function, which
   GNU C does as if they had the size 1 */
static void pointer_arith_check(int op, CType *ptr)
{
  int bt = pointed_type(ptr)->t & VT_BTYPE;
  if (bt == VT_VOID)
    tcc_warning_once(warn_pointer_arith, "pointer of type 'void *' used in %s",
                     op == '-' ? "subtraction" : "arithmetic");
  else if (bt == VT_FUNC)
    tcc_warning_once(warn_pointer_arith, "pointer to a function used in %s", op == '-' ? "subtraction" : "arithmetic");
}

static HOT void gen_op_impl(int op)
{
  int t1, t2, bt1, bt2, t;
  CType type1, combtype;
  int op_class = op;
  int bf_trunc_size = 0;

  if (op == TOK_SHR || op == TOK_SAR || op == TOK_SHL)
    op_class = SHIFT_OP;
  else if (TOK_ISCOND(op)) /* == != > ... */
    op_class = CMP_OP;

redo:
  t1 = vtop[-1].type.t;
  t2 = vtop[0].type.t;
  bt1 = t1 & VT_BTYPE;
  bt2 = t2 & VT_BTYPE;

  /* Complex integer == and != : decompose into per-component comparisons
   * before the usual arithmetic conversions.  We do this early because the
   * runtime cast from a narrow complex type (_Complex char/short) to a wider
   * one (_Complex int) is not implemented – it would naïvely sign-extend
   * just the low byte, losing the packed imaginary part. */
  if ((op == TOK_EQ || op == TOK_NE) && ((t1 | t2) & VT_COMPLEX) && !is_float(bt1) && !is_float(bt2))
  {
    /* Promote a non-complex operand to complex (imag = 0) so the
     * decomposition helper always sees VT_COMPLEX on both sides. */
    if (!(t1 & VT_COMPLEX))
    {
      /* lhs is real: rewrite as _Complex with base type = bt1 */
      vtop[-1].type.t |= VT_COMPLEX;
      /* For constants: mask to only the real part so the imaginary
       * (high) bits are zero.  A sign-extended c.i (e.g. -1 stored
       * as 0xFFFFFFFFFFFFFFFF) would otherwise look like imag = -1. */
      if ((vtop[-1].r & (VT_VALMASK | VT_LVAL | VT_SYM)) == VT_CONST && btype_size(bt1) < 8)
        vtop[-1].c.i &= (1ULL << (btype_size(bt1) * 8)) - 1;
    }
    if (!(t2 & VT_COMPLEX))
    {
      vtop[0].type.t |= VT_COMPLEX;
      /* Same masking for rhs constants. */
      if ((vtop[0].r & (VT_VALMASK | VT_LVAL | VT_SYM)) == VT_CONST && btype_size(bt2) < 8)
        vtop[0].c.i &= (1ULL << (btype_size(bt2) * 8)) - 1;
    }
    gen_complex_int_cmp(op);
    return;
  }

  /* Complex float/double == and != : decompose into per-component comparisons.
   * The FCMP backend only compares the real (lo) half, so we split the
   * comparison into two scalar float/double comparisons here. */
  if ((op == TOK_EQ || op == TOK_NE) && ((t1 | t2) & VT_COMPLEX) && (is_float(bt1) || is_float(bt2)))
  {
    if (!(t1 & VT_COMPLEX))
      vtop[-1].type.t |= VT_COMPLEX;
    if (!(t2 & VT_COMPLEX))
      vtop[0].type.t |= VT_COMPLEX;

    gen_complex_float_cmp(op);
    return;
  }

  /* Complex float/double +/- : decompose into per-component scalar operations.
   * Complex double (128 bits) does not fit in a register pair (64 bits max),
   * so we decompose at the front-end level. Complex float also uses this
   * path for consistency. Skip when both are constant (gen_opif folds).
   * Mixed scalar+complex takes this path too (the scalar's imaginary part
   * is 0) — letting it fall through to the generic conversions reduces the
   * complex operand to its real half and silently drops the result's
   * imaginary part (`x + 1.0i` lost the `i`). */
  if ((op == '+' || op == '-') && ((t1 | t2) & VT_COMPLEX) && (is_float(bt1) || is_float(bt2)))
  {
    gen_complex_float_arith(op); /* folds a const+const pair itself */
    return;
  }

  /* Complex float/double * : decompose at the frontend.  Handles mixed
   * scalar+complex (scalar's imag treated as 0) via the optimal 2-mul path,
   * and complex×complex via the standard 4-mul/add/sub formula.  Routing
   * through gen_op() emits plain scalar ops, so downstream optimizer passes
   * don't have to know that the underlying memory has a complex layout.
   *
   * Division is intentionally left to the backend's __divdc3 / __divsc3
   * libgcc helpers — the naïve decomposition (c² + d²) loses precision and
   * over/underflows on extreme values that IEEE-compliant libgcc handles. */
  if (op == '*' && ((t1 | t2) & VT_COMPLEX) && (is_float(bt1) || is_float(bt2)))
  {
    int l_c = (vtop[-1].r & (VT_VALMASK | VT_LVAL | VT_SYM)) == VT_CONST;
    int r_c = (vtop[0].r & (VT_VALMASK | VT_LVAL | VT_SYM)) == VT_CONST;
    if (!(l_c && r_c))
    {
      gen_complex_float_mul(op);
      return;
    }
  }

  /* Mixed scalar/complex float division: promote the scalar operand to the
   * complex type, then fall through — the both-complex path reaches the
   * backend's IEEE-correct __divdc3/__divsc3 helpers.  Without this the
   * generic conversions reduce the complex operand to its real half. */
  if (op == '/' && (((t1 ^ t2) & VT_COMPLEX) != 0) && (is_float(bt1) || is_float(bt2)))
  {
    int scalar_below = (t2 & VT_COMPLEX) != 0; /* scalar is vtop[-1] */
    CType cplx_type = scalar_below ? vtop[0].type : vtop[-1].type;
    CType scalar_ct;
    scalar_ct.t = cplx_type.t & ~VT_COMPLEX;
    scalar_ct.ref = NULL;
    if (scalar_below)
      vswap();
    gen_cast(&scalar_ct); /* align base type (no-op when equal) */
    gen_cast(&cplx_type); /* scalar → (scalar, 0) complex temp */
    if (scalar_below)
      vswap();
    t1 = vtop[-1].type.t;
    t2 = vtop[0].type.t;
    bt1 = t1 & VT_BTYPE;
    bt2 = t2 & VT_BTYPE;
  }

  /* Complex integer +, -, *, / : decompose into component-wise scalar operations.
   * Complex integers don't fit in a single 32-bit register, so non-constant
   * operations must be decomposed into real/imag scalar operations.
   * Skip when both are constant (gen_opic constant-folds those). */
  if ((op == '+' || op == '-' || op == '*' || op == '/') && ((t1 | t2) & VT_COMPLEX) && !is_float(bt1) &&
      !is_float(bt2))
  {
    int l_c = (vtop[-1].r & (VT_VALMASK | VT_LVAL | VT_SYM)) == VT_CONST;
    int r_c = (vtop[0].r & (VT_VALMASK | VT_LVAL | VT_SYM)) == VT_CONST;
    if (!(l_c && r_c))
    {
      /* Don't set VT_COMPLEX on non-complex operands here;
       * gen_complex_int_arith needs to know which were originally complex
       * to correctly extract imaginary parts (0 for real operands). */
      gen_complex_int_arith(op);
      return;
    }
  }

  /* C11 6.7.2.1p10: a bit-field has an integer type of the specified width.
     For unsigned long long bit-fields wider than int but narrower than 64 bits,
     arithmetic must wrap at the bit-field width, not at 64 bits.  But the result
     type is the *usual arithmetic conversion* of the operands, so the bit-field
     width only governs when it is the widest type involved:
       - a plain long long / unsigned long long operand (or a bit-field declared
         64 bits wide) widens the result to 64 bits and disables truncation;
       - a shift's type is that of its left operand only, so a bit-field used
         as the shift count never truncates;
       - two bit-fields of different widths take the larger width.
     Narrow (<= int) and signed bit-fields never truncate here.  Track the
     effective width so we can truncate after the operation. */
  bf_trunc_size = 0;
  {
    /* Does an operand that contributes to the result type have effective width
       64?  For a shift only the left operand (vtop[-1]) does. */
    int wide64 = 0;
    if (bt1 == VT_LLONG && (!(t1 & VT_BITFIELD) || BIT_SIZE(t1) >= 64))
      wide64 = 1;
    if (op_class != SHIFT_OP && bt2 == VT_LLONG &&
        (!(t2 & VT_BITFIELD) || BIT_SIZE(t2) >= 64))
      wide64 = 1;

    if (!wide64)
    {
      if ((t1 & VT_BITFIELD) && (t1 & VT_UNSIGNED) && bt1 == VT_LLONG)
      {
        int bs = BIT_SIZE(t1);
        if (bs > 32 && bs < 64)
          bf_trunc_size = bs;
      }
      /* A shift's right operand is only a shift count: it never governs the
         result type, so it is not considered here. */
      if (op_class != SHIFT_OP && (t2 & VT_BITFIELD) && (t2 & VT_UNSIGNED) &&
          bt2 == VT_LLONG)
      {
        int bs = BIT_SIZE(t2);
        if (bs > 32 && bs < 64 && bs > bf_trunc_size)
          bf_trunc_size = bs;
      }
    }
  }

  /* GCC vector extension: dispatch to element-wise scalar lowering */
  if ((t1 & VT_VECTOR) || (t2 & VT_VECTOR))
  {
    gen_op_vector(op);
    return;
  }

  if (bt1 == VT_FUNC || bt2 == VT_FUNC)
  {
    if (bt2 == VT_FUNC)
    {
      mk_pointer(&vtop->type);
      gaddrof();
    }
    if (bt1 == VT_FUNC)
    {
      vswap();
      mk_pointer(&vtop->type);
      gaddrof();
      vswap();
    }
    goto redo;
  }
  else if (!combine_types(&combtype, vtop - 1, vtop, op_class))
  {
  op_err:
    tcc_error("invalid operand types for binary operation");
  }
  else if (bt1 == VT_PTR || bt2 == VT_PTR)
  {
    /* at least one operand is a pointer */
    /* relational op: must be both pointers */
    int align;
    if (op_class == CMP_OP)
      goto std_op;
    if (tcc_warn_wanted(warn_pointer_arith))
      pointer_arith_check(op, bt1 == VT_PTR ? &vtop[-1].type : &vtop->type);
    /* if both pointers, then it must be the '-' op */
    if (bt1 == VT_PTR && bt2 == VT_PTR)
    {
      if (op != '-')
        goto op_err;
      vpush_type_size(pointed_type(&vtop[-1].type), &align);
      vtop->type.t &= ~VT_UNSIGNED;
      vrott(3);
      gen_opic(op);
      vtop->type.t = VT_PTRDIFF_T;
      vswap();
      gen_op(TOK_PDIV);
    }
    else
    {
      /* exactly one pointer : must be '+' or '-'. */
      if (op != '-' && op != '+')
        goto op_err;
      /* Put pointer as first operand */
      if (bt2 == VT_PTR)
      {
        vswap();
        t = t1, t1 = t2, t2 = t;
        bt2 = bt1;
      }
#if PTR_SIZE == 4
      if (bt2 == VT_LLONG)
        /* XXX: truncate here because gen_opl can't handle ptr + long long */
        gen_cast_s(VT_INT);
#endif
      type1 = vtop[-1].type;
      vpush_type_size(pointed_type(&vtop[-1].type), &align);
      gen_op('*');
#ifdef CONFIG_TCC_BCHECK
      if (tcc_state->do_bounds_check && !CONST_WANTED)
      {
        /* if bounded pointers, we generate a special code to
           test bounds */
        if (op == '-')
        {
          vpushi(0);
          vswap();
          gen_op('-');
        }
        gen_bounded_ptr_add();
      }
      else
#endif
      {
        gen_opic(op);
      }
      type1.t &= ~(VT_ARRAY | VT_VLA);
      /* yasc: p + n points into what p owns but owns nothing itself */
      type_yasc_strip(&type1);
      /* put again type if gen_opic() swaped operands */
      vtop->type = type1;
    }
  }
  else
  {
    /* floats can only be used for a few operations */
    if (is_float(combtype.t) && op != '+' && op != '-' && op != '*' && op != '/' && op_class != CMP_OP)
    {
      goto op_err;
    }
  std_op:
    t = t2 = combtype.t;
    /* special case for shifts and long long: we keep the shift as
       an integer */
    if (op_class == SHIFT_OP)
      t2 = VT_INT;
    /* XXX: currently, some unsigned operations are explicit, so
       we modify them here */
    if (t & VT_UNSIGNED)
    {
      if (op == TOK_SAR)
        op = TOK_SHR;
      else if (op == '/')
        op = TOK_UDIV;
      else if (op == '%')
        op = TOK_UMOD;
      else if (op == TOK_LT)
        op = TOK_ULT;
      else if (op == TOK_GT)
        op = TOK_UGT;
      else if (op == TOK_LE)
        op = TOK_ULE;
      else if (op == TOK_GE)
        op = TOK_UGE;
    }
    vswap();
    gen_cast_s(t);
    vswap();
    gen_cast_s(t2);
    if (is_float(t))
      gen_opif(op);
    else
      gen_opic(op);
    /* Truncate result for wide unsigned bit-field arithmetic (C11 6.7.2.1p10).
       Bit-fields wider than int but narrower than their base type have their
       own effective integer type; arithmetic must wrap at the bit-field width,
       not the full long long width. */
    if (bf_trunc_size > 0 && op_class != CMP_OP)
    {
      vpush64(VT_LLONG | VT_UNSIGNED, (1ULL << bf_trunc_size) - 1);
      gen_opic('&');
    }
    if (op_class == CMP_OP)
    {
      /* relational op: the result is an int */
      vtop->type.t = VT_INT;
    }
    else if (op == TOK_UMULL || op == TOK_SMULL)
    {
      /* UMULL/SMULL produce 64-bit result from 32-bit inputs - preserve the type set by tcc_ir_gen_opi */
    }
    else
    {
      vtop->type.t = t;
    }
  }
  // Make sure that we have converted to an rvalue:
  // if (vtop->r & VT_LVAL)
  //   gv(is_float(vtop->type.t & VT_BTYPE) ? RC_FLOAT : RC_INT);
}
