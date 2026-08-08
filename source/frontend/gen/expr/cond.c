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

/* cond.c -- Short-circuit && / || and the ternary conditional.
 * Split out of tccgen.c; see docs/plan_tccgen_split.md. */

#include "gen_priv.h"

/* Assuming vtop is a value used in a conditional context
   (i.e. compared with zero) return 0 if it's false, 1 if
   true and -1 if it can't be statically determined.  */
static int condition_3way(void)
{
  int c = -1;
  if ((vtop->r & (VT_VALMASK | VT_LVAL)) == VT_CONST && (!(vtop->r & VT_SYM) || !vtop->sym->a.weak))
  {
    vdup();
    gen_cast_s(VT_BOOL);
    c = vtop->c.i;
    vpop();
  }
  return c;
}

void expr_landor(int op)
{
  int t = 0, cc = 1, f = 0, i = op == TOK_LAND, c;

  /* In classic (non-IR) codegen, jump-chain sentinel is 0.
     In IR mode, jump-chain sentinel is -1 (see tcc_ir_backpatch). */
  if (tcc_state->ir != NULL)
    t = -1;

  /* Standard branch-based evaluation */
  for (;;)
  {
    c = f ? i : condition_3way();
    if (c < 0)
    {
      cc = 0;
    }
    // save_regs(1), cc = 0;
    else if (c != i)
      nocode_wanted++, f = 1;
    if (tok != op)
      break;
    if (c < 0)
    {
      // t = gvtst(i, t);
      t = tcc_ir_codegen_test_gen(tcc_state->ir, i, t);
    }
    else
      vpop();
    next();
    {
      int saved_nocode = nocode_wanted;
      expr_landor_next(op);
      nocode_wanted = saved_nocode;
    }
  }

  if (cc || f)
  {
    vpop();
    vpushi(i ^ f);
    if (tcc_state->ir == NULL)
    {
      gsym(t);
    }
    else
    {
      tcc_ir_backpatch_to_here(tcc_state->ir, t);
    }
    nocode_wanted -= f;
  }
  else if (tcc_state->ir != NULL && i == 0 && (vtop->r & (VT_VALMASK | VT_LVAL | VT_SYM)) == VT_CONST)
  {
    /* IR mode, || only: the last operand is a compile-time constant false
     * but earlier operands were runtime values (cc=0, f=0).
     *
     * gvtst_set() would create a synthetic VT_CMP(TOK_EQ) with no real
     * CMP instruction, causing the next JUMPIF to reuse stale condition
     * flags from a prior comparison — producing the wrong branch direction.
     *
     * For ||, the chain 't' holds "jump-when-true" entries.  A constant-
     * false last operand means the overall result depends solely on
     * whether a prior operand was true → encode as VT_JMP with chain t.
     *
     * (For &&, i==1, the synthetic VT_CMP(TOK_NE) from gvtst_set happens
     * to match the stale flags correctly on the fallthrough path, so the
     * original codepath is valid and must not be replaced.)
     */
    int const_val = (vtop->c.i != 0);
    vpop();
    if (const_val == 0)
    {
      /* false || … — outcome depends on chain only. */
      vseti(VT_JMP, t);
    }
    else
    {
      /* true || … — always true. */
      vpushi(1);
      tcc_ir_backpatch_to_here(tcc_state->ir, t);
    }
  }
  else
  {
    gvtst_set(i, t);
    // vset_VT_JMP();
  }
}

static int is_cond_bool(SValue *sv)
{
  /* Only return true for actual comparison results (VT_CMP).
   * Previously this also returned true for constants 0/1, but that caused
   * incorrect code generation for ternary expressions like `x == 0 ? 1 : 0`
   * because the optimization path would generate SETIF instructions that
   * depend on stale condition flags after unconditional branches. */
  if (sv->r == VT_CMP)
    return 1;
  return 0;
}


/* Ternary conditional (?:) handler - extracted from expr_cond() to reduce
   stack frame size on the recursive path. Called after '?' has been seen
   and next() consumed it. */
__attribute__((noinline)) void expr_cond_ternary(void)
{
  int tt, u, r1, r2, rc, t1, t2, islv, c, g;
  SValue sv;
  CType type;
  unsigned long long false_max = 0, false_strlen = 0, true_max = 0, true_strlen = 0;
  int false_max_valid = 0, false_strlen_valid = 0, true_max_valid = 0, true_strlen_valid = 0;

  c = condition_3way();
  g = (tok == ':' && gnu_ext);
  tt = -1; /* -1 = no chain */
  if (!g)
  {
    if (c < 0)
    {
      tt = tcc_ir_codegen_test_gen(tcc_state->ir, 1, -1);
    }
    else
    {
      vpop();
    }
  }
  else if (c < 0)
  {
    /* needed to avoid having different registers saved in
       each branch */
    gv_dup();
    tt = tcc_ir_codegen_test_gen(tcc_state->ir, 0, -1);
  }

  if (c == 0)
    nocode_wanted++;
  if (!g)
    gexpr();

  if ((vtop->type.t & VT_BTYPE) == VT_FUNC)
    mk_pointer(&vtop->type);
  sv = *vtop; /* save value to handle it later */
  vtop--;     /* no vpop so that FP stack is not flushed */
  print_vstack("expr_cond");

  if (g)
  {
    u = tt;
  }
  else if (c < 0)
  {
    u = gjmp(-1); /* -1 = no chain */
    tcc_ir_backpatch_to_here(tcc_state->ir, tt);
  }
  else
    u = -1; /* -1 = no chain */

  if (c == 0)
    nocode_wanted--;
  if (c == 1)
    nocode_wanted++;
  skip(':');
  expr_cond();

  if ((vtop->type.t & VT_BTYPE) == VT_FUNC)
    mk_pointer(&vtop->type);

  /* cast operands to correct type according to ISOC rules */
  if (!combine_types(&type, &sv, vtop, '?'))
    type_incompatibility_error(&sv.type, &vtop->type, "type mismatch in conditional expression (have '%s' and '%s')");

  if (c < 0 && is_cond_bool(vtop) && is_cond_bool(&sv))
  {
    /* optimize "if (f ? a > b : c || d) ..." for example, where normally
       "a < b" and "c || d" would be forced to "(int)0/1" first, whereas
       this code jumps directly to the if's then/else branches. */
    t1 = tcc_ir_codegen_test_gen(tcc_state->ir, 0, -1);
    t2 = gjmp(-1); /* -1 = no chain */
    tcc_ir_backpatch_to_here(tcc_state->ir, u);
    vpushv(&sv);
    /* combine jump targets of 2nd op with VT_CMP of 1st op */
    gvtst_set(0, t1);
    gvtst_set(1, t2);
    gen_cast(&type);
    //  tcc_warning("two conditions expr_cond");
    return;
  }

  /* keep structs lvalue by transforming `(expr ? a : b)` to `*(expr ? &a :
    &b)` so that `(expr ? a : b).mem` does not error with "lvalue expected".
    If the condition is statically false (c == 0), the expression reduces to
    the selected operand and is already a proper lvalue, so skip this
    transformation (otherwise we'd call indir() on a non-pointer). */
  islv = (c != 0) && (vtop->r & VT_LVAL) && (sv.r & VT_LVAL) && VT_STRUCT == (type.t & VT_BTYPE);

  if (c != 0)
  {
    /* Arrays must decay to pointers BEFORE gen_cast overwrites the type.
       gen_cast converts array type to pointer type but doesn't compute the
       address. If we don't decay here, the VT_ARRAY flag is lost and later
       gv() won't recognize it needs to call gaddrof().

       Note: Local arrays are stored without VT_LVAL in the symbol table
       (they decay to pointers immediately). So we check for VT_ARRAY
       regardless of VT_LVAL for locals. */
    int is_local_array = ((vtop->r & VT_VALMASK) == VT_LOCAL) && (vtop->type.t & VT_ARRAY);
    int is_lval_array = (vtop->r & VT_LVAL) && (vtop->type.t & VT_ARRAY);
    if (is_lval_array || is_local_array)
    {
      /* For local arrays without VT_LVAL, temporarily set it for gaddrof */
      if (is_local_array && !(vtop->r & VT_LVAL))
        vtop->r |= VT_LVAL;
      gaddrof();
      vtop->type.t &= ~VT_ARRAY;
    }
    gen_cast(&type);
    if (islv)
    {
      mk_pointer(&vtop->type);
      gaddrof();
    }
    else if (VT_STRUCT == (vtop->type.t & VT_BTYPE))
      gaddrof();
  }
  else
  {
    /* Even if the condition is a compile-time constant, the conditional
       operator's result type is determined from both operands.
       Do not reduce `0 ? a : b` to just `b`'s type; this breaks sizeof/_Generic.
       Cast the selected (false) operand to the combined result type.
       Keep struct lvalues untouched (no &/ * transformation) in this case. */
    /* Arrays must decay here too */
    if ((vtop->r & VT_LVAL) && (vtop->type.t & VT_ARRAY))
    {
      gaddrof();
      vtop->type.t &= ~VT_ARRAY;
    }
    gen_cast(&type);
  }

  rc = RC_TYPE(type.t);

  tt = r2 = 0;
  int false_vreg = 0; /* Save false branch vreg for IR mode */
  if (c < 0)
  {
    false_max_valid = svalue_get_conservative_max_u64(vtop, &false_max);
    false_strlen_valid = svalue_get_conservative_string_bytes_u64(vtop, &false_strlen);
    r2 = gv(rc);
    false_vreg = vtop->vr; /* Save the false branch's vreg */
    tt = gjmp(-1);         /* -1 = no chain */
  }
  tcc_ir_backpatch_to_here(tcc_state->ir, u);
  if (c == 1)
    nocode_wanted--;

  /* this is horrible, but we must also convert first
     operand */
  if (c != 0)
  {
    *vtop = sv;
    /* Arrays must decay to pointers BEFORE gen_cast overwrites the type.
       Same logic as for the false branch - handle local arrays without VT_LVAL. */
    int is_local_array = ((vtop->r & VT_VALMASK) == VT_LOCAL) && (vtop->type.t & VT_ARRAY);
    int is_lval_array = (vtop->r & VT_LVAL) && (vtop->type.t & VT_ARRAY);
    if (is_lval_array || is_local_array)
    {
      /* For local arrays without VT_LVAL, temporarily set it for gaddrof */
      if (is_local_array && !(vtop->r & VT_LVAL))
        vtop->r |= VT_LVAL;
      gaddrof();
      vtop->type.t &= ~VT_ARRAY;
    }
    gen_cast(&type);
    if (islv)
    {
      mk_pointer(&vtop->type);
      gaddrof();
    }
    else if (VT_STRUCT == (vtop->type.t & VT_BTYPE))
      gaddrof();
  }

  if (c < 0)
  {
    true_max_valid = svalue_get_conservative_max_u64(vtop, &true_max);
    true_strlen_valid = svalue_get_conservative_string_bytes_u64(vtop, &true_strlen);
    r1 = gv(rc);
    /* For IR mode: after both branches are materialized, we need to ensure
     * they converge to the same vreg at the merge point.
     * Generate ASSIGN from true_vreg to false_vreg (which is used at merge). */
    int true_vreg = vtop->vr;
    int true_vreg_valid =
        (true_vreg != -1) && (TCCIR_DECODE_VREG_TYPE(true_vreg) >= 1) && (TCCIR_DECODE_VREG_TYPE(true_vreg) <= 3);
    int false_vreg_valid =
        (false_vreg != -1) && (TCCIR_DECODE_VREG_TYPE(false_vreg) >= 1) && (TCCIR_DECODE_VREG_TYPE(false_vreg) <= 3);
    if (tcc_state->ir && true_vreg_valid && false_vreg_valid && true_vreg != false_vreg)
    {
      /* Copy true branch result to false branch's vreg so both paths use same vreg */
      SValue src, dest;
      svalue_init(&src);
      svalue_init(&dest);
      src.vr = true_vreg;
      src.type = vtop->type;
      dest.vr = false_vreg;
      dest.type = vtop->type;
      tcc_ir_put(tcc_state->ir, TCCIR_OP_ASSIGN, &src, NULL, &dest);
      vtop->vr = false_vreg;
    }
    if (!tcc_state->ir)
    {
      move_reg(r2, r1, islv ? VT_PTR : type.t);
      vtop->r = r2;
    }

    objsize_vreg_fact_record(tcc_state ? tcc_state->ir : NULL, vtop->vr, true_max_valid && false_max_valid,
                             true_max > false_max ? true_max : false_max, true_strlen_valid && false_strlen_valid,
                             true_strlen > false_strlen ? true_strlen : false_strlen);

    tcc_ir_backpatch_to_here(tcc_state->ir, tt);
  }

  if (islv)
  {
    indir();
    /* C11 6.5.15: ?: with struct operands yields an rvalue (temporary copy),
       not an lvalue into the original.  Copy to a stack temporary so that
       stores through the result don't modify the originals. */
    int sz, al, tvr, tloc;
    CType st = vtop->type;
    sz = type_size(&st, &al);
    if (sz > 0)
    {
      tloc = get_temp_local_var(sz, al > 8 ? 8 : al, &tvr);
      SValue dst;
      svalue_init(&dst);
      dst.type = st;
      dst.r = VT_LOCAL | VT_LVAL;
      dst.vr = tvr;
      dst.c.i = tloc;
      vpushv(&dst);
      vswap();
      vstore();
      vpop();
      vpushv(&dst);
    }
  }
}
