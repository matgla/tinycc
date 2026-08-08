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

/* indir.c -- Indirection, typed parameter passing and builtin parameter parsing.
 * Split out of tccgen.c; see docs/plan_tccgen_split.md. */

#include "gen_priv.h"

/* indirection with full error checking and bound check */
ST_FUNC void indir(void)
{
  if ((vtop->type.t & VT_BTYPE) != VT_PTR)
  {
    if ((vtop->type.t & VT_BTYPE) == VT_FUNC)
      return;
    expect("pointer");
  }
  if (vtop->r & VT_LVAL)
  {
    SValue dest;
    svalue_init(&dest);
    /* The temp holds the pointer value (u32), not the dereferenced value.
     * Use vtop's pointer type so the ASSIGN's dest btype matches what the
     * register actually contains.  Earlier code used *pointed_type(), which
     * mis-typed pointer temps as their pointed-to type and forced the
     * codegen to emit a spurious u32→u64 zero-extension. */
    dest.type = vtop->type;
    dest.vr = tcc_ir_get_vreg_temp(tcc_state->ir);
    tcc_ir_put(tcc_state->ir, TCCIR_OP_ASSIGN, vtop, NULL, &dest);
    vtop->vr = dest.vr;
    vtop->r = 0;
  }
  vtop->type = *pointed_type(&vtop->type);
  /* After pointer dereference, the result represents the pointed-to object,
   * not the original parameter.  Clear VT_PARAM so that a subsequent
   * gaddrof() (e.g. during c->field struct member access) does NOT emit
   * a spurious LEA of the parameter's stack slot.  Without this, code like
   * c->items[idx] (where c is a register-passed pointer parameter) would
   * compute the address of c's stack slot + field_offset instead of
   * loading c's value and adding the field offset. */
  vtop->r &= ~VT_PARAM;
  /* Arrays and functions are never lvalues */
  if (!(vtop->type.t & (VT_ARRAY | VT_VLA)) && (vtop->type.t & VT_BTYPE) != VT_FUNC)
  {
    vtop->r |= VT_LVAL;
    /* if bound checking, the referenced pointer must be checked */
#ifdef CONFIG_TCC_BCHECK
    if (tcc_state->do_bounds_check)
      vtop->r |= VT_MUSTBOUND;
#endif
  }

  /* Inline-eval fold: `*&g` where g is a static with a known initializer and
   * no observed writes becomes a VT_CONST of the initializer value. Applies
   * only under nocode_wanted (speculative try_inline_const_eval) so regular
   * code generation is unaffected. */
  if (nocode_wanted && (vtop->r & (VT_VALMASK | VT_SYM | VT_LVAL)) == (VT_CONST | VT_SYM | VT_LVAL) && vtop->sym &&
      !vtop->sym->a.possibly_written && !(vtop->type.t & (VT_ARRAY | VT_VLA)))
  {
    int btype = vtop->type.t & VT_BTYPE;
    if (btype == VT_BYTE || btype == VT_SHORT || btype == VT_INT || btype == VT_LLONG || btype == VT_BOOL ||
        btype == VT_PTR)
    {
      ElfSym *esym = elfsym(vtop->sym);
      if (esym && esym->st_shndx != SHN_UNDEF && esym->st_shndx != SHN_COMMON &&
          esym->st_shndx < tcc_state->nb_sections)
      {
        Section *sec = tcc_state->sections[esym->st_shndx];
        int align;
        int sz = type_size(&vtop->type, &align);
        unsigned long off = (unsigned long)(esym->st_value + (unsigned long long)vtop->c.i);
        if (sec && sec->data && sz > 0 && off + (unsigned long)sz <= sec->data_offset)
        {
          const unsigned char *ptr = sec->data + off;
          int64_t val = 0;
          if (sz == 8)
            memcpy(&val, ptr, 8);
          else
          {
            memcpy(&val, ptr, sz);
            if (!(vtop->type.t & VT_UNSIGNED) && sz < 8)
            {
              int shift = (8 - sz) * 8;
              val = (int64_t)(val << shift) >> shift;
            }
          }
          vtop->c.i = val;
          vtop->r = VT_CONST;
          /* Preserve sym so a later & operator can restore the lvalue form. */
        }
      }
    }
  }
}

/* pass a parameter to a function and do type checking and casting */
void gfunc_param_typed(Sym *func, Sym *arg)
{
  int func_type;
  CType type;

  /* If &g is being bound to a non-const pointer param, the callee may write
   * through it — poison g so inline-eval won't fold `*&g` to its initializer. */
  if (!nocode_wanted && arg && vtop->sym && (vtop->r & (VT_VALMASK | VT_SYM | VT_LVAL)) == (VT_CONST | VT_SYM) &&
      (arg->type.t & VT_BTYPE) == VT_PTR)
  {
    CType *pointed = pointed_type(&arg->type);
    if (pointed && !(pointed->t & VT_CONSTANT))
      vtop->sym->a.possibly_written = 1;
  }

  func_type = func->f.func_type;
  if (func_type == FUNC_OLD || (func_type == FUNC_ELLIPSIS && arg == NULL))
  {
    /* Handle struct/union arguments for unprototyped/variadic calls. */
    if ((vtop->type.t & VT_BTYPE) == VT_STRUCT)
    {
      int align, size = type_size(&vtop->type, &align);

      /* VLA structs have runtime-determined size (type_size returns 0).
       * Pass by invisible reference: the VLA struct's stack slot already
       * contains a pointer to the VLA-allocated data.  Load that pointer
       * and pass it directly as a pointer argument. */
      if (struct_has_vla_member(&vtop->type))
      {
        if (nocode_wanted)
          return;
        /* vtop is VT_LOCAL pointing to the pointer slot.
         * Setting VT_LVAL makes the backend load the pointer value
         * stored in that slot, giving us the VLA data address. */
        vtop->type.t = VT_PTR;
        vtop->r |= VT_LVAL;
        return;
      }

      if (size > 16)
      {
        if (nocode_wanted)
          return;

        if (!(vtop->r & VT_LVAL))
        {
          tcc_error("cannot pass large struct by value");
        }

        /* Allocate a stack slot for the struct copy.
         *
         * For a non-variadic argument the slot is converted to a pointer by
         * the gaddrof() below, which drops the VR_TEMP_LOCAL marker from the
         * vstack — so get_temp_local_var() could reuse the same slot for a
         * sibling struct argument in the same call, aliasing the two copies
         * (GCC PR 67226).  Those keep a fresh, never-reused slot.
         *
         * A variadic anonymous argument instead stays a struct lvalue (no
         * gaddrof — see the FUNC_ELLIPSIS return below), so it is safe to draw
         * its copy from the call-scoped arg-struct temp pool: that pool keeps
         * concurrently-live copies in distinct slots while reclaiming slots
         * from completed statements, collapsing the one-copy-per-call-site
         * stack growth seen when marshaling many large by-value variadic
         * structs. */
        int tmp_loc;
        if (func_type == FUNC_ELLIPSIS)
        {
          tmp_loc = get_arg_struct_temp(size, align);
        }
        else
        {
          loc = (loc - size) & -align;
          tmp_loc = loc;
        }

        /* Store the source struct into the temporary destination.
         * vstore() will emit a memmove() for struct types. */
        {
          SValue dst;
          memset(&dst, 0, sizeof(dst));
          dst.type = vtop->type;
          dst.r = VT_LOCAL | VT_LVAL;
          dst.vr = -1;
          dst.c.i = tmp_loc;
          vpushv(&dst);
          vswap();
          vstore();
        }

        if (func_type == FUNC_ELLIPSIS)
        {
          /* Variadic anonymous argument: keep as struct lvalue so the
           * backend decomposes it into words for register/stack placement.
           * va_arg reads the raw data from the va area, not a pointer. */
          return;
        }

        /* Unprototyped (FUNC_OLD) call: the callee may have been compiled
         * with a prototype and expect invisible reference (pointer) for
         * structs > 16 bytes.  Convert the temp copy to a pointer arg. */
        mk_pointer(&vtop->type);
        gaddrof();
        return;
      }
    }

    /* default casting : only need to convert float to double */
    /* Complex types are NOT promoted (treated like composites per AAPCS) */
    if ((vtop->type.t & VT_BTYPE) == VT_FLOAT && !(vtop->type.t & VT_COMPLEX))
    {
      gen_cast_s(VT_DOUBLE);
    }
    else if (vtop->type.t & VT_BITFIELD)
    {
      type.t = vtop->type.t & (VT_BTYPE | VT_UNSIGNED);
      type.ref = vtop->type.ref;
      gen_cast(&type);
    }
    else if (vtop->r & (VT_MUSTCAST | (VT_MUSTCAST << 1)))
    {
      force_charshort_cast();
    }
  }
  else if (arg == NULL)
  {
    tcc_error("too many arguments to function");
  }
  else
  {
    type = arg->type;
    type.t &= ~VT_CONSTANT; /* need to do that to avoid false warning */
    if (arg->a.transparent_union && type.ref)
      type.ref->a.transparent_union = 1;

    if (is_transparent_union_type(&type))
    {
      CType *member_type = find_assignable_transparent_union_member(&type);
      if (member_type)
      {
        gen_assign_cast(member_type);
        return;
      }
    }

    /* ARM EABI AAPCS: Composite types (struct/union) larger than 4 words (16 bytes)
     * must be passed by invisible reference - the caller passes a pointer.
     * Check if this is a large struct that should be passed by reference. */
    if ((type.t & VT_BTYPE) == VT_STRUCT)
    {
      int align, size = type_size(&type, &align);
      if (size > 16)
      {
        /* Pass by invisible reference: caller must allocate a temporary copy
         * and pass a pointer to that copy (AAPCS). Passing the original object's
         * address would break C's by-value semantics.
         */
        if (nocode_wanted)
          return;

        if (!(vtop->r & VT_LVAL))
        {
          /* For now we require an lvalue source; most struct expressions in TCC
           * are materialized as lvalues already.
           */
          tcc_error("cannot pass large struct by value");
        }

        /* Always allocate a fresh stack slot for the struct copy.
         * Do NOT use get_temp_local_var() here: after gaddrof() converts
         * the lvalue to a pointer, the VR_TEMP_LOCAL marker is lost from
         * vstack, causing get_temp_local_var() to reuse the same slot for
         * a subsequent struct argument in the same call.  This would make
         * both struct copies alias the same memory.  (See GCC PR 67226.) */
        loc = (loc - size) & -align;
        int tmp_loc = loc;

        /* Store the source struct into the temporary destination.
         * vstore() will emit a memmove() for struct types.
         */
        {
          SValue dst;
          memset(&dst, 0, sizeof(dst));
          dst.type = type;
          dst.r = VT_LOCAL | VT_LVAL;
          dst.vr = -1;
          dst.c.i = tmp_loc;
          vpushv(&dst);
          vswap();
          vstore();
        }

        /* Save const_init_data before gaddrof invalidates it — the
         * inline expansion needs it for compile-time vector folding. */
        aapcs_last_const_init = find_sv_const_init(vtop, size);
        aapcs_last_const_init_size = aapcs_last_const_init ? size : 0;

        /* Convert the temp lvalue to a pointer argument. */
        mk_pointer(&vtop->type);
        gaddrof();
        return;
      }
    }

    gen_assign_cast(&type);
  }
}

/* parse an expression and return its type without any side effect. */
void expr_type(CType *type, void (*expr_fn)(void))
{
  nocode_wanted++;
  expr_fn();
  *type = vtop->type;
  vpop();
  nocode_wanted--;
}

/* parse an expression of the form '(type)' or '(expr)' and return its
   type */
void parse_expr_type(CType *type)
{
  int n;
  AttributeDef ad;

  skip('(');
  if (parse_btype(type, &ad, 0))
  {
    type_decl(type, &ad, &n, TYPE_ABSTRACT);
  }
  else
  {
    expr_type(type, gexpr);
  }
  skip(')');
}

void parse_type(CType *type)
{
  AttributeDef ad;
  int n;

  if (!parse_btype(type, &ad, 0))
  {
    expect("type");
  }
  type_decl(type, &ad, &n, TYPE_ABSTRACT);
}

void parse_builtin_params(int nc, const char *args)
{
  char c, sep = '(';
  CType type;
  if (nc)
    nocode_wanted++;
  next();
  if (*args == 0)
    skip(sep);
  while ((c = *args++))
  {
    skip(sep);
    sep = ',';
    if (c == 't')
    {
      parse_type(&type);
      vpush(&type);
      continue;
    }
    expr_eq();
    type.ref = NULL;
    type.t = 0;
    switch (c)
    {
    case 'e':
      /* Apply array-to-pointer and function-to-function-pointer decay */
      convert_parameter_type(&vtop->type);
      continue;
    case 'V':
      type.t = VT_CONSTANT;
    case 'v':
      type.t |= VT_VOID;
      mk_pointer(&type);
      break;
    case 'S':
      type.t = VT_CONSTANT;
    case 's':
      type.t |= char_type.t;
      mk_pointer(&type);
      break;
    case 'i':
      type.t = VT_INT;
      break;
    case 'l':
      type.t = VT_SIZE_T;
      break;
    default:
      break;
    }
    gen_assign_cast(&type);
  }
  skip(')');
  if (nc)
    nocode_wanted--;
}
