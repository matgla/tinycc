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

/* unary.c -- Unary expressions, parenthesized/cast/compound-literal and _Generic.
 * Split out of tccgen.c; see docs/plan_tccgen_split.md. */

#include "gen_priv.h"

/* Parenthesized expression, cast, compound literal, or statement expression.
   Extracted from unary_primary() to keep its locals out of the main frame.
   Returns 1 for sizeof/alignof type-only operand (early return), 0 otherwise. */
__attribute__((noinline)) int unary_paren(void)
{
  int t, n, r;
  CType type;
  AttributeDef ad;

  type.ref = NULL;
  t = tok;
  next();
  /* cast ? */
  if (parse_btype(&type, &ad, 0))
  {
    type_decl(&type, &ad, &n, TYPE_ABSTRACT);
    skip(')');
    /* check ISOC99 compound literal */
    if (tok == '{')
    {
      /* data is allocated locally by default */
      if (global_expr)
        r = VT_CONST;
      else
        r = VT_LOCAL;
      /* all except arrays are lvalues */
      if (!(type.t & VT_ARRAY))
        r |= VT_LVAL;
      memset(&ad, 0, sizeof(AttributeDef));
      int lit_ir_start = tcc_state->ir ? tcc_state->ir->next_instruction_index : 0;
      decl_initializer_alloc(&type, &ad, r, 1, 0, 0);
      /* A fully-constant vector compound literal only ever writes its own
       * brand-new stack object, whose address nothing else can hold yet.  Mark
       * the materialising stores/copy so an in-flight vector recipe is not
       * invalidated by them (see gen_op_vector's element-major fusion). */
      if (tcc_state->ir && (type.t & VT_VECTOR) && find_sv_vec_literal_init(vtop, 1))
        vec_emit_range_record(lit_ir_start, tcc_state->ir->next_instruction_index);
    }
    else if (t == TOK_SOTYPE)
    { /* from sizeof/alignof (...) */
      vpush(&type);
      return 1; /* early return - skip postfix ops */
    }
    else if (IS_UNION(type.t))
    {
      /* GCC extension: (union_type) scalar_expr */
      unary();

      if ((vtop->type.t & VT_BTYPE) == VT_STRUCT || (vtop->type.t & (VT_ARRAY | VT_VLA)))
      {
        gen_cast(&type);
      }
      else if (nocode_wanted)
      {
        vtop->type = type;
      }
      else
      {
        int u_align;
        int u_size = type_size(&type, &u_align);
        int vr_tmp;
        int tmp_loc = get_temp_local_var(u_size, u_align, &vr_tmp);

        Sym *field = type.ref->next;
        if (field)
          gen_cast(&field->type);

        SValue dst_sv;
        memset(&dst_sv, 0, sizeof(dst_sv));
        dst_sv.type = vtop->type;
        dst_sv.r = VT_LOCAL | VT_LVAL;
        dst_sv.vr = vr_tmp;
        dst_sv.c.i = tmp_loc;

        vpushv(&dst_sv);
        vswap();
        vstore();
        vtop--;

        dst_sv.type = type;
        vpushv(&dst_sv);
      }
    }
    else
    {
      unary();
      gen_cast(&type);
    }
  }
  else if (tok == '{')
  {
    int saved_nocode_wanted = nocode_wanted;
    if (CONST_WANTED && !NOEVAL_WANTED)
      expect("constant");
    if (0 == local_scope)
      tcc_error("statement expression outside of function");
    block(STMT_EXPR);
    if (saved_nocode_wanted)
      nocode_wanted = saved_nocode_wanted;
    skip(')');
  }
  else
  {
    gexpr();
    skip(')');
  }
  return 0;
}

/* _Generic() expression parser - extracted to reduce unary_primary() frame. */
__attribute__((noinline)) void unary_generic(void)
{
  CType controlling_type;
  int has_default = 0;
  int has_match = 0;
  int learn = 0;
  TokenString *str = NULL;
  int saved_nocode_wanted = nocode_wanted;
  nocode_wanted &= ~CONST_WANTED_MASK;

  next();
  skip('(');
  expr_type(&controlling_type, expr_eq);
  convert_parameter_type(&controlling_type);

  nocode_wanted = saved_nocode_wanted;

  for (;;)
  {
    learn = 0;
    skip(',');
    if (tok == TOK_DEFAULT)
    {
      if (has_default)
        tcc_error("too many 'default'");
      has_default = 1;
      if (!has_match)
        learn = 1;
      next();
    }
    else
    {
      AttributeDef ad_tmp;
      int itmp;
      CType cur_type;

      parse_btype(&cur_type, &ad_tmp, 0);
      type_decl(&cur_type, &ad_tmp, &itmp, TYPE_ABSTRACT);
      if (compare_types(&controlling_type, &cur_type, 0))
      {
        if (has_match)
        {
          tcc_error("type match twice");
        }
        has_match = 1;
        learn = 1;
      }
    }
    skip(':');
    if (learn)
    {
      if (str)
        tok_str_free(str);
      skip_or_save_block(&str);
    }
    else
    {
      skip_or_save_block(NULL);
    }
    if (tok == ')')
      break;
  }
  if (!str)
  {
    char buf[60];
    type_to_str(buf, sizeof buf, &controlling_type, NULL);
    tcc_error("type '%s' does not match any association", buf);
  }
  begin_macro(str, 1);
  next();
  expr_eq();
  if (tok != TOK_EOF)
    expect(",");
  end_macro();
  next();
}

ST_FUNC HOT void unary(void)
{
  Sym *s;

  /* generate line number info */
  if (debug_modes)
    tcc_debug_line(tcc_state), tcc_tcov_check_line(tcc_state, 1);

  /* Handle simple prefix operators directly to avoid entering
     unary_primary()'s large stack frame on the recursive path. */
  switch (tok)
  {
  case '*':
    next();
    unary();
    indir();
    goto postfix;
  case '!':
    next();
    unary();
    gen_test_zero(TOK_EQ);
    goto postfix;
  case TOK_INC:
  case TOK_DEC:
  {
    int t = tok;
    next();
    unary();
    inc(0, t);
  }
    goto postfix;
  case '-':
    next();
    unary();
    if (is_float(vtop->type.t))
    {
      gen_opif(TOK_NEG);
    }
    else
    {
      vpushi(0);
      vswap();
      gen_op('-');
    }
    goto postfix;
  case '~':
    next();
    unary();
    if (vtop->type.t & VT_COMPLEX)
    {
      gen_complex_conjugate();
    }
    else
    {
      vpushi(-1);
      gen_op('^');
    }
    goto postfix;
  case '+':
    next();
    unary();
    if ((vtop->type.t & VT_BTYPE) == VT_PTR)
      tcc_error("pointer not accepted for unary plus");
    if (!is_float(vtop->type.t))
    {
      vpushi(0);
      gen_op('+');
    }
    goto postfix;
  case '&':
    next();
    unary();
    if ((vtop->type.t & VT_BTYPE) != VT_FUNC && !(vtop->type.t & (VT_ARRAY | VT_VLA)))
    {
      if (!(vtop->r & VT_LVAL) && (vtop->r & VT_VALMASK) == VT_CONST && vtop->sym != NULL)
      {
        vtop->r = VT_LVAL | VT_CONST | VT_SYM;
        vtop->c.i = 0;
        vtop->type = vtop->sym->type;
        vtop->vr = -1;
      }
      test_lvalue();
    }
    if (vtop->sym && ((vtop->r & VT_SYM) || (vtop->r & VT_LOCAL) || (vtop->r & VT_PARAM)))
    {
      vtop->sym->a.addrtaken = 1;
      tcc_ir_set_addrtaken(tcc_state->ir, vtop->sym->vreg);
      if (vtop->sym->a.nested_func)
        setup_nested_func_trampoline(vtop->sym);
    }
    {
      int is_vla_struct_local = struct_has_vla_member(&vtop->type) && (vtop->r & VT_VALMASK) == VT_LOCAL;
      mk_pointer(&vtop->type);
      if (!is_vla_struct_local)
      {
        gaddrof();
      }
    }
    goto postfix;
  case TOK_SOTYPE:
  case '(':
    if (unary_paren())
      return;
    goto postfix;
  default:
    break;
  }

  if (unary_primary())
    return;

postfix:
  /* post operations */
  while (1)
  {
    if (tok == TOK_INC || tok == TOK_DEC)
    {
      inc(1, tok);
      next();
    }
    else if (tok == '.' || tok == TOK_ARROW)
    {
      int qualifiers, cumofs;
      /* field */
      if (tok == TOK_ARROW)
        indir();
      qualifiers = vtop->type.t & (VT_CONSTANT | VT_VOLATILE);
      test_lvalue();
      /* expect pointer on structure */
      next();
      CType struct_type = vtop->type; /* save before find_field/type changes */
      s = find_field(&vtop->type, tok, &cumofs);
      /* 64-bit deref alignment tracking: the member access is possibly
       * under-aligned (< 4) when the base lvalue already was (chained packed
       * access), when the struct type itself has alignment < 4 (packed /
       * #pragma pack), or when the member offset is not word-aligned.  The
       * mark is consumed by svalue_to_iroperand to keep 64-bit accesses off
       * the LDRD/STRD path, which faults on unaligned addresses. */
      int member_underaligned;
      {
        int salign;
        type_size(&struct_type, &salign);
        member_underaligned = vtop->underaligned || salign < 4 || (cumofs & 3) != 0;
      }
      /* add field offset to pointer */
      if (struct_has_vla_member(&vtop->type) && (vtop->r & VT_VALMASK) == VT_LOCAL)
      {
        /* VLA struct stored via pointer indirection: load the data pointer
           from the pointer slot instead of computing its address.
           Works whether VT_LVAL is already set (normal variable reference)
           or not (e.g. from declaration context). */
        vtop->type = char_pointer_type;
        vtop->r |= VT_LVAL;
      }
      else
      {
        gaddrof();
        vtop->type = char_pointer_type; /* change type to 'char *' */
      }
      /* Check if any VLA field precedes the target field.  If so, the
         compile-time cumofs does not account for VLA field sizes and we
         must compute the full offset dynamically at runtime. */
      {
        int has_preceding_vla = 0;
        if ((struct_type.t & VT_BTYPE) == VT_STRUCT && struct_has_vla_member(&struct_type) &&
            struct_type.ref->type.t != VT_UNION)
        {
          Sym *f;
          for (f = struct_type.ref->next; f && f != s; f = f->next)
          {
            if (f->type.t & VT_VLA)
            {
              has_preceding_vla = 1;
              break;
            }
          }
        }
        if (has_preceding_vla)
        {
          /* Walk all fields in order, computing the cumulative byte
             offset at runtime.  For each field we align to its
             required alignment, then (unless it is the target) add
             its runtime or compile-time size. */
          Sym *f;
          vpushi(0); /* running integer offset */
          for (f = struct_type.ref->next; f; f = f->next)
          {
            int f_align, f_size;
            if (f->type.t & VT_VLA)
            {
              f_size = 0; /* determined at runtime */
              type_size(&f->type.ref->type, &f_align);
            }
            else
            {
              f_size = type_size(&f->type, &f_align);
              if (f_size < 0)
                f_size = 0;
            }
            /* honour explicit alignment attribute on the field */
            if (f->a.aligned)
            {
              int ea = 1 << (f->a.aligned - 1);
              if (ea > f_align)
                f_align = ea;
            }
            /* runtime: offset = (offset + align-1) & -align */
            if (f_align > 1)
            {
              vpushi(f_align - 1);
              gen_op('+');
              vpushi(~(f_align - 1));
              gen_op('&');
            }
            if (f == s)
              break; /* aligned to target field — done */
            /* add this field's size to the running offset */
            if (f->type.t & VT_VLA)
            {
              vset(&int_type, VT_LOCAL | VT_LVAL, f->type.ref->c);
            }
            else
            {
              vpushi(f_size);
            }
            gen_op('+');
          }
          gen_op('+'); /* pointer + computed_offset */
        }
        else
        {
          vpushi(cumofs);
          gen_op('+');
        }
      }
      /* change type to field type, and set to lvalue */
      vtop->type = s->type;
      vtop->type.t |= qualifiers;
      /* Set even for array-typed members (no VT_LVAL): the mark rides the
       * SValue into pointer decay and gen_op('+') propagates it, so
       * packed->arr[i] still reaches the deref marked. */
      vtop->underaligned = member_underaligned;
      /* an array (or VLA) is never an lvalue */
      if (!(vtop->type.t & (VT_ARRAY | VT_VLA)))
      {
        vtop->r |= VT_LVAL;
#ifdef CONFIG_TCC_BCHECK
        /* if bound checking, the referenced pointer must be checked */
        if (tcc_state->do_bounds_check)
          vtop->r |= VT_MUSTBOUND;
#endif
      }
      next();
    }
    else if (tok == '[')
    {
      next();
      gexpr();
      if (is_vector_type(&vtop[-1].type))
      {
        /* GCC vector subscript: vec[i] -> element of the vector. */
        gen_vec_subscript();
      }
      else
      {
        /* A subscript `base[idx]` on a VT_SYM global (array or pointer) is
         * treated as a potential write path: gen_op('+') below will often
         * materialize the base into a register, stripping VT_SYM from the
         * resulting lvalue, so vstore's sym-based poisoning can't catch a
         * later store through this lvalue.  Poison here if the pointee is
         * non-const.  Cost: blocks `*&g` scalar folds for syms that are
         * also subscripted — but scalar syms aren't subscripted. */
        if (!nocode_wanted && (vtop[-1].r & (VT_VALMASK | VT_SYM)) == (VT_CONST | VT_SYM) && vtop[-1].sym)
        {
          CType *pointed = NULL;
          if (vtop[-1].type.t & VT_ARRAY)
            pointed = &vtop[-1].type; /* array element type */
          else if ((vtop[-1].type.t & VT_BTYPE) == VT_PTR)
            pointed = pointed_type(&vtop[-1].type);
          if (pointed && !(pointed->t & VT_CONSTANT))
            vtop[-1].sym->a.possibly_written = 1;
        }
        gen_op('+');
        indir();
      }
      skip(']');
    }
    else if (tok == '(')
    {
      unary_funcall();
    }
    else
    {
      break;
    }
  }
}
