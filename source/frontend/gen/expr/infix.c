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


void expr_infix(int p)
{
  int t = tok, p2;
  while ((p2 = precedence(t)) >= p)
  {
    if (t == TOK_LOR || t == TOK_LAND)
    {
      expr_landor(t);
    }
    else
    {
      next();
      unary();
      if (precedence(tok) > p2)
        expr_infix(p2 + 1);
      gen_op(t);
    }
    t = tok;
  }
}
#endif
void expr_cond(void)
{
  expr_lor();
  if (tok == '?')
  {
    next();
    expr_cond_ternary();
  }
}

void expr_eq(void)
{
  int t;

  expr_cond();
  if ((t = tok) == '=' || TOK_ASSIGN(t))
  {
    test_lvalue();
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
          !(vtop->type.t & VT_VOLATILE))
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
      gen_op(TOK_ASSIGN_OP(t));
    }
    vstore();
  }
}

ST_FUNC void gexpr(void)
{
  expr_eq();
  if (tok == ',')
  {
    do
    {
      gv_discarded_volatile();
      vpop();
      next();
      expr_eq();
      tcc_ir_codegen_drop_return(tcc_state->ir);
    } while (tok == ',');

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
