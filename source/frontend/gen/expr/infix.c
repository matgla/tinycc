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

/* infix.c -- Operator-precedence infix parser, assignment and constant expressions.
 * Split out of tccgen.c; see docs/plan_tccgen_split.md. */

#include "gen_priv.h"

#ifndef precedence_parser /* original top-down parser */

static void expr_prod(void)
{
  int t;

  unary();
  while ((t = tok) == '*' || t == '/' || t == '%')
  {
    next();
    unary();
    gen_op(t);
  }
}

static void expr_sum(void)
{
  int t;

  expr_prod();
  while ((t = tok) == '+' || t == '-')
  {
    next();
    expr_prod();
    gen_op(t);
  }
}

static void expr_shift(void)
{
  int t;

  expr_sum();
  while ((t = tok) == TOK_SHL || t == TOK_SAR)
  {
    next();
    expr_sum();
    gen_op(t);
  }
}

static void expr_cmp(void)
{
  int t;

  expr_shift();
  while (((t = tok) >= TOK_ULE && t <= TOK_GT) || t == TOK_ULT || t == TOK_UGE)
  {
    next();
    expr_shift();
    gen_op(t);
  }
}

static void expr_cmpeq(void)
{
  int t;

  expr_cmp();
  while ((t = tok) == TOK_EQ || t == TOK_NE)
  {
    next();
    expr_cmp();
    gen_op(t);
  }
}

static void expr_and(void)
{
  expr_cmpeq();
  while (tok == '&')
  {
    next();
    expr_cmpeq();
    gen_op('&');
  }
}

static void expr_xor(void)
{
  expr_and();
  while (tok == '^')
  {
    next();
    expr_and();
    gen_op('^');
  }
}

static void expr_or(void)
{
  expr_xor();
  while (tok == '|')
  {
    next();
    expr_xor();
    gen_op('|');
  }
}


static void expr_land(void)
{
  expr_or();
  if (tok == TOK_LAND)
    expr_landor(tok);
}

static void expr_lor(void)
{
  expr_land();
  if (tok == TOK_LOR)
    expr_landor(tok);
}

#else /* defined precedence_parser */

static int precedence(int tok)
{
  switch (tok)
  {
  case TOK_LOR:
    return 1;
  case TOK_LAND:
    return 2;
  case '|':
    return 3;
  case '^':
    return 4;
  case '&':
    return 5;
  case TOK_EQ:
  case TOK_NE:
    return 6;
  relat:
  case TOK_ULT:
  case TOK_UGE:
    return 7;
  case TOK_SHL:
  case TOK_SAR:
    return 8;
  case '+':
  case '-':
    return 9;
  case '*':
  case '/':
  case '%':
    return 10;
  default:
    if (tok >= TOK_ULE && tok <= TOK_GT)
      goto relat;
    return 0;
  }
}
static unsigned char prec[256];
void init_prec(void)
{
  int i;
  for (i = 0; i < 256; i++)
    prec[i] = precedence(i);
}
#define precedence(i) ((unsigned)i < 256 ? prec[i] : 0)

/* Out-of-line accessor for prec[], which stays file-local.  Used by
 * gen_priv.h's expr_landor_next() from expr/cond.c. */
int expr_precedence(int tok)
{
  return precedence(tok);
}


static int is_comparison_op(int t)
{
  return precedence(t) == 6 || precedence(t) == 7;
}

static const char *op_name(int t)
{
  switch (t)
  {
  case '+':
    return "+";
  case '-':
    return "-";
  case '&':
    return "&";
  case '^':
    return "^";
  case '|':
    return "|";
  case TOK_SHL:
    return "<<";
  case TOK_SAR:
    return ">>";
  case TOK_EQ:
    return "==";
  case TOK_NE:
    return "!=";
  case TOK_LAND:
    return "&&";
  case TOK_LOR:
    return "||";
  default:
    return "?";
  }
}

/* -Wparentheses (gcc's warn_about_parentheses): operator T applied to operands
 * whose own top operators, when written without parentheses, were L and R (0:
 * a unary expression, or one in parentheses) */
/* The right operand (vtop) looks like a truth value, as gcc's
 * APPEARS_TO_BE_BOOLEAN_EXPR_P: a comparison, `&&`, `||` or `!` (R, its
 * operator), a _Bool, or (WITH_ONE) the constant 0 or 1 */
static int rhs_looks_boolean(int r, int with_one)
{
  int v = vtop->r & VT_VALMASK;
  if (is_comparison_op(r) || r == TOK_LAND || r == TOK_LOR || r == '!')
    return 1;
  if (v == VT_CMP || v == VT_JMP || v == VT_JMPI) /* one in parentheses */
    return 1;
  if ((vtop->type.t & VT_BTYPE) == VT_BOOL)
    return 1;
  return with_one && (vtop->r & (VT_VALMASK | VT_LVAL | VT_SYM)) == VT_CONST && is_integer_btype(vtop->type.t & VT_BTYPE) &&
         (vtop->c.i == 0 || vtop->c.i == 1);
}

/* `!a == b`, `!a & b`: the left operand is a `!` not in parentheses (L is
 * '!') and the right one is not a truth value */
static void check_logical_not(int t, int r)
{
  const int line = file ? file->line_num : 0;
  if (is_comparison_op(t))
  {
    if (!fe_check_on(tcc_state->warn_logical_not_parentheses) || rhs_looks_boolean(r, 0))
      return;
    /* `!x == 0` is `!(x == 0)` */
    if ((t == TOK_EQ || t == TOK_NE) && (vtop->r & (VT_VALMASK | VT_LVAL | VT_SYM)) == VT_CONST &&
        is_integer_btype(vtop->type.t & VT_BTYPE) && vtop->c.i == 0)
      return;
    /* quiet in an inline expansion (fe_check_on), so the check-only parse
       of a body every call inlined reports it: tcc_diag_first, as the
       other -Wparentheses cases */
    if (!tcc_diag_first(tcc_state, line, DIAG_KEY(DIAG_KEY_PARENTHESES, ((unsigned)t << 1) | 1)))
      return;
    tcc_warning_c(warn_logical_not_parentheses)("%i:logical not is only applied to the left hand side of comparison",
                                                line);
    tcc_note("%i:add parentheses around left hand side expression to silence this warning", line);
  }
  else if ((t == '&' || t == '|') && fe_check_on(tcc_state->warn_parentheses) && !rhs_looks_boolean(r, 1) &&
           tcc_diag_first(tcc_state, line, DIAG_KEY(DIAG_KEY_PARENTHESES, ((unsigned)t << 1) | 1)))
    tcc_warning_c(warn_parentheses)("%i:suggest parentheses around operand of '!' or change '%c' to '%c%c' or '!' to '~'",
                                    line, t, t, t);
}

void check_parentheses(int t, int l, int r)
{
  const int line = file ? file->line_num : 0;
  char buf[96];
  int o;
  if (l == '!')
  {
    check_logical_not(t, r);
    l = 0;
  }
  if (r == '!')
    r = 0;
  if (!(l | r) || !fe_check_on(tcc_state->warn_parentheses))
    return;
  buf[0] = 0;
  switch (t)
  {
  case TOK_SHL:
  case TOK_SAR:
    o = (l == '+' || l == '-') ? l : (r == '+' || r == '-') ? r : 0;
    if (o)
      snprintf(buf, sizeof buf, "suggest parentheses around '%s' inside '%s'", op_name(o), op_name(t));
    break;
  case TOK_LOR:
    if (l == TOK_LAND || r == TOK_LAND)
      snprintf(buf, sizeof buf, "suggest parentheses around '&&' within '||'");
    break;
  case '|':
  case '^':
    if (l == '&' || l == '+' || l == '-' || r == '&' || r == '+' || r == '-' || (t == '|' && (l == '^' || r == '^')))
      snprintf(buf, sizeof buf, "suggest parentheses around arithmetic in operand of '%s'", op_name(t));
    else if (is_comparison_op(l) || is_comparison_op(r))
      snprintf(buf, sizeof buf, "suggest parentheses around comparison in operand of '%s'", op_name(t));
    break;
  case '&':
    o = (l == '+' || l == '-') ? l : (r == '+' || r == '-') ? r : 0;
    if (o)
      snprintf(buf, sizeof buf, "suggest parentheses around '%s' in operand of '&'", op_name(o));
    else if (is_comparison_op(l) || is_comparison_op(r))
      snprintf(buf, sizeof buf, "suggest parentheses around comparison in operand of '&'");
    break;
  case TOK_EQ:
  case TOK_NE:
    if (is_comparison_op(l) || is_comparison_op(r))
      snprintf(buf, sizeof buf, "suggest parentheses around comparison in operand of '%s'", op_name(t));
    break;
  default:
    /* < <= > >= */
    if (precedence(t) == 7 && (precedence(l) == 7 || precedence(r) == 7))
      snprintf(buf, sizeof buf, "comparisons like 'X<=Y<=Z' do not have their mathematical meaning");
    break;
  }
  if (buf[0] && tcc_diag_first(tcc_state, line, DIAG_KEY(DIAG_KEY_PARENTHESES, (unsigned)t << 1)))
    tcc_warning_c(warn_parentheses)("%i:%s", line, buf);
}

/* -Wtautological-compare: where an operand's tokens start (fe_tokfp_*, the
 * hash before its first token) and the side effects parsed before it */
typedef struct
{
  uint64_t h;
  unsigned n;
  int effects;
} OperandStart;

static inline void operand_start(OperandStart *o)
{
  o->h = fe_tokfp_prev;
  o->n = fe_tokfp_n - 1;
  o->effects = uv_effects;
}

/* the hash of the tokens from S to E (both operand_start states) */
static uint64_t tokfp_span(const OperandStart *s, const OperandStart *e)
{
  uint64_t k = 0x100000001B3ull, pw = 1;
  for (unsigned m = e->n - s->n; m; m >>= 1, k *= k)
    if (m & 1)
      pw *= k;
  return e->h - s->h * pw;
}

/* A token span from here (before the current token) to where fe_span_end
 * is called (before the token current then): for "the same expression" */
void fe_span_begin(FeSpan *s)
{
  s->h = fe_tokfp_prev;
  s->n = fe_tokfp_n - 1;
}

void fe_span_end(FeSpan *s)
{
  OperandStart b, e;
  b.h = s->h, b.n = s->n;
  operand_start(&e);
  s->h = tokfp_span(&b, &e);
  s->n = e.n - b.n;
}

/* `( X )` of the span X: '(' K^(n+1) + X K + ')' (tokfp_add's values) */
static uint64_t fe_span_paren(const FeSpan *x)
{
  uint64_t k = 0x100000001B3ull, pw = 1, kk = k;
  for (unsigned m = x->n + 1; m; m >>= 1, kk *= kk)
    if (m & 1)
      pw *= kk;
  return ((uint64_t)'(' + 1) * pw + x->h * k + ((uint64_t)')' + 1);
}

/* A and B read the same tokens, or one of them in parentheses */
int fe_span_same(const FeSpan *a, const FeSpan *b)
{
  if (!a->n || !b->n)
    return 0;
  if (a->n == b->n)
    return a->h == b->h;
  if (a->n == b->n + 2)
    return a->h == fe_span_paren(b);
  if (b->n == a->n + 2)
    return b->h == fe_span_paren(a);
  return 0;
}

/* `a OP b` with OP a comparison: did a and b read the same tokens, with no
 * side effect from a's start on?  (`p->a == p->a`, `(x+1) == (x+1)`) */
static void check_same_operands(int op, const OperandStart *ls, const OperandStart *le, const OperandStart *rs)
{
  OperandStart re;
  operand_start(&re);
  if (re.effects != ls->effects || le->n - ls->n != re.n - rs->n || le->n == ls->n ||
      tokfp_span(ls, le) != tokfp_span(rs, &re))
    return;
  fe_check_same_operands(op);
}

/* Returns the operator at the top of what it parsed (0: it applied none), for
 * -Wparentheses: an operand's own operator, when it is not in parentheses.
 * L is the left operand's ('!': a `!` expression, 0: another unary one), LS
 * where it started (NULL: not known). */
static int expr_infix_l(int p, int l, const OperandStart *ls)
{
  int t = tok, p2;
  while ((p2 = precedence(t)) >= p)
  {
    if (t == TOK_LOR || t == TOK_LAND)
    {
      if (t == TOK_LOR && l == TOK_LAND)
        check_parentheses(TOK_LOR, l, 0);
      expr_landor(t);
      /* -Wunused-value, as gcc: `c && (x = 1)` and `c || (f(), 0)` are
         statements; `c && f()` computes a value nothing uses */
      uv_last = uv_last == UV_ASSIGN || uv_last == UV_COMMA ? UV_EFFECT : UV_VALUE;
    }
    else
    {
      int r;
      OperandStart le = {0, 0, 0}, rs = {0, 0, 0};
      if (ls && fe_tokfp_on)
        operand_start(&le); /* the operator: the left operand ends before it */
      next();
      if (fe_tokfp_on)
        operand_start(&rs);
      r = tok == '!' ? '!' : 0; /* a unary expression starting with `!` is one */
      unary();
      if (precedence(tok) > p2)
        r = expr_infix_l(p2 + 1, r, fe_tokfp_on ? &rs : NULL);
      if (l | r)
        check_parentheses(t, l, r);
      if (ls && fe_tokfp_on && TOK_ISCOND(t) && tcc_state->warn_tautological_compare)
        check_same_operands(t, ls, &le, &rs);
      gen_op_checked(t);
      uv_last = UV_VALUE;
    }
    l = t;
    t = tok;
  }
  return l;
}

int expr_infix(int p)
{
  return expr_infix_l(p, 0, NULL);
}

/* unary(), then the operators after it from precedence P on */
int expr_unary_infix(int p)
{
  int l = tok == '!' ? '!' : 0;
  OperandStart ls = {0, 0, 0};
  if (fe_tokfp_on)
    operand_start(&ls);
  unary();
  return expr_infix_l(p, l, fe_tokfp_on ? &ls : NULL);
}
#endif
void expr_cond(void)
{
  expr_lor();
  if (tok == '?')
  {
    /* -Wunused-value: gcc says nothing of a conditional with effects */
    const int effects = uv_effects;
    if (objsize_ab_watch)
      objsize_branch(); /* -Warray-bounds in loops (builtin/objsize.c) */
    next();
    expr_cond_ternary();
    uv_last = uv_effects != effects ? UV_EFFECT : UV_VALUE;
  }
}

/* A destination claimed as a call's struct-return buffer (NRVO) is written
 * by the call: sound only when that call's result IS the value assigned,
 * `x = f(...)` and nothing more.  With anything around the call --
 * `x = (f(), x)`, `x = *g(f(), &x)`, `x = ({ f(); x; })`, `S x = { f().b }`,
 * and after it `x = f().fp(x)`, `x = f().arr[x.i]`, `z = cf() + z`,
 * `x = f().k ? y : x` -- the call would overwrite x while x's old value is
 * still to be read.  So the hint is only given when the right-hand side is
 * the name of a function (or a function pointer), its parenthesized
 * arguments, and then a token that ends the assignment.  The tokens are read
 * ahead and replayed (tokens replayed from a string are not macro-expanded
 * again).  The first call parsed is then the outermost one, and every call
 * drops the hint before it parses its arguments (gfunc_call).  Whether the
 * callee can reach the destination some other way is decided on the IR
 * (tcc_ir_sret_dealias). */
int nrvo_rhs_is_direct_call(void)
{
  if (tok < TOK_UIDENT)
    return 0;
  Sym *s = sym_find(tok);
  if (!s || (s->type.t & VT_TYPEDEF))
    return 0;
  const int bt = s->type.t & VT_BTYPE;
  if (bt != VT_FUNC && !(bt == VT_PTR && !(s->type.t & VT_ARRAY) && s->type.ref &&
                         (s->type.ref->type.t & VT_BTYPE) == VT_FUNC))
    return 0;
  const int ident = tok;
  /* #pragma pack met while reading ahead is replayed in place, as for a
   * saved function body (skip_or_save_block) */
  TokenString *saved_capture = pp_pragma_capture;
  TokenString *str = tok_str_alloc();
  pp_pragma_capture = str;
  next();
  int call = 0;
  if (tok == '(')
  {
    int level = 0;
    for (;;)
    {
      const int t = tok;
      if (t == TOK_EOF)
        break;
      tok_str_add_tok(str);
      next();
      if (t == '(' || t == '[' || t == '{')
        level++;
      else if ((t == ')' || t == ']' || t == '}') && --level == 0)
        break;
    }
    /* tok: what follows the call's ')' */
    call = level == 0 && (tok == ';' || tok == ',' || tok == ')' || tok == ']' || tok == '}' || tok == ':');
  }
  if (tok != TOK_EOF) /* (a string's TOK_EOF stays where it is) */
    tok_str_add_tok(str);
  pp_pragma_capture = saved_capture;
  tok_str_add(str, 0);
  begin_macro(str, 1);
  tok = ident;
  return call;
}

/* -Wparentheses: the expr_eq() that returned last was an assignment `a = b`
 * (in parentheses it is a primary expression, and the expr_eq() around it,
 * finishing later, says 0) */
int expr_eq_assigned;

void expr_eq(void)
{
  int t, assigned = 0;

  expr_cond();
  while (tok == TOK_catch && tcc_state->yasc)
  {
    yasc_catch();
    uv_effects++, uv_last = UV_EFFECT;
  }
  if ((t = tok) == '=' || TOK_ASSIGN(t))
  {
    Sym *lhs;
    test_lvalue();
    lhs = t == '=' ? unused_lhs_assign() : unused_lhs(); /* -Wunused-but-set-variable */
    next();
    if (t == '=')
    {
      /* NRVO for plain assignment `dest = sret_call(...)`: hint that the
       * first composite-returning call in the RHS may write its result
       * directly into the destination, eliminating the temp buffer plus the
       * temp->dst copy (a memmove for big structs).  Mirror the
       * local-declaration NRVO path (see decl initializer handling).  Two
       * destination shapes qualify, both of a non-volatile struct/complex
       * type:
       *   - a plain stack-local lvalue (`v = f()`): targeted by stack offset.
       *   - a register-deref lvalue (`v.field = f()`, where this fork's
       *     gaddrof materialized the field address into a vreg via LEA):
       *     targeted by that address vreg.
       * The call-site claim (gfunc_call) re-checks an exact size/align match
       * before reusing the destination. */
      int saved_nrvo_active = tcc_state->nrvo_target_active;
      int saved_nrvo_loc = tcc_state->nrvo_target_loc;
      int saved_nrvo_vreg = tcc_state->nrvo_target_vreg;
      int saved_nrvo_size = tcc_state->nrvo_target_size;
      int saved_nrvo_align = tcc_state->nrvo_target_align;
      int saved_nrvo_ptr_vreg = tcc_state->nrvo_target_ptr_vreg;
      int lhs_bt = vtop->type.t & VT_BTYPE;
      int lhs_is_local =
          (vtop->r & (VT_VALMASK | VT_LVAL | VT_SYM)) == (VT_LOCAL | VT_LVAL);
      int lhs_is_reg_deref =
          (vtop->r & VT_LVAL) && (vtop->r & VT_VALMASK) < VT_CONST &&
          vtop->vr >= 0;
      if (tcc_state->ir && !nocode_wanted &&
          (lhs_bt == VT_STRUCT || (vtop->type.t & VT_COMPLEX)) &&
          !(vtop->type.t & VT_VECTOR) &&
          (lhs_is_local || lhs_is_reg_deref) &&
          !(vtop->type.t & VT_VOLATILE) && nrvo_rhs_is_direct_call())
      {
        int nrvo_align;
        int nrvo_size = type_size(&vtop->type, &nrvo_align);
        tcc_state->nrvo_target_active = 1;
        tcc_state->nrvo_target_size = nrvo_size;
        tcc_state->nrvo_target_align = nrvo_align;
        if (lhs_is_local)
        {
          tcc_state->nrvo_target_loc = vtop->c.i;
          tcc_state->nrvo_target_vreg = vtop->vr;
          tcc_state->nrvo_target_ptr_vreg = -1;
        }
        else
        {
          tcc_state->nrvo_target_ptr_vreg = vtop->vr;
        }
      }
      /* a local pointer variable: what the value points into (objsize.c) */
      if ((vtop->type.t & (VT_BTYPE | VT_ARRAY)) == VT_PTR && lhs_is_local && !NOEVAL_WANTED)
      {
        ObjRec os_saved;
        objsize_rhs_begin(&os_saved);
        expr_eq();
        objsize_rhs_end(&os_saved);
      }
      else
        expr_eq();
      tcc_state->nrvo_target_active = saved_nrvo_active;
      tcc_state->nrvo_target_loc = saved_nrvo_loc;
      tcc_state->nrvo_target_vreg = saved_nrvo_vreg;
      tcc_state->nrvo_target_size = saved_nrvo_size;
      tcc_state->nrvo_target_align = saved_nrvo_align;
      tcc_state->nrvo_target_ptr_vreg = saved_nrvo_ptr_vreg;
    }
    else
    {
      vdup();
      expr_eq();
      int aop = TOK_ASSIGN_OP(t);
      if (tcc_state->warn_conversion || tcc_state->warn_sign_conversion)
        fe_compound_rhs = *vtop, fe_compound_pending = aop == TOK_SHL || aop == TOK_SAR ? 2 : 1;
      gen_op_checked(aop);
    }
    vstore();
    fe_compound_pending = 0;
    assigned = t == '=';
    unused_assigned_to(lhs);
  }
  expr_eq_assigned = assigned;
}

ST_FUNC void gexpr(void)
{
  /* -Wunused-value: each operand from its start */
  int effects = uv_effects, line = file ? file->line_num : 0;
  uv_last = 0;
  expr_eq();
  if (tok == ',')
  {
    do
    {
      unused_discard(effects, line, 1);
      vpop_discarded();
      next();
      effects = uv_effects, line = file ? file->line_num : 0;
      uv_last = 0;
      expr_eq();
    } while (tok == ',');
    unused_comma_end(effects, uv_last);

    /* convert array & function to pointer.  It also strips the qualifiers, so
     * record a volatile access first — otherwise the comma expression's value
     * arrives at its consumer (or at the statement-level discard) looking like
     * an ordinary read, and `(void)(x, *REG)` loses the read. */
    if (vtop->type.t & VT_VOLATILE)
      vtop->volatile_access = 1;
    convert_parameter_type(&vtop->type);

    /* make builtin_constant_p((1,2)) return 0 (like on gcc) */
    if ((vtop->r & VT_VALMASK) == VT_CONST && nocode_wanted && !CONST_WANTED)
      gv(RC_TYPE(vtop->type.t));
  }
}

/* parse a constant expression and return value in vtop.  */
void expr_const1(void)
{
  nocode_wanted += CONST_WANTED_BIT;
  expr_cond();
  nocode_wanted -= CONST_WANTED_BIT;
}

/* parse an integer constant and return its value. */
ST_FUNC int64_t expr_const64(void)
{
  int64_t c;
  expr_const1();
  if ((vtop->r & (VT_VALMASK | VT_LVAL | VT_SYM | VT_NONCONST)) != VT_CONST)
    expect("constant expression");
  c = vtop->c.i;
  vpop();
  return c;
}

/* parse an integer constant and return its value.
   Complain if it doesn't fit 32bit (signed or unsigned).  */
ST_FUNC int expr_const(void)
{
  int c;
  int64_t wc = expr_const64();
  c = wc;
  if (c != wc && (unsigned)c != wc)
    tcc_error("constant exceeds 32 bit");
  return c;
}
