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

/* alloc.c -- Initializer storage allocation and the initialized-object driver.
 * Split out of tccgen.c; see docs/plan_tccgen_split.md. */

#include "gen_priv.h"

/* 't' contains the type and storage info. 'c' is the offset of the
   object in section 'sec'. If 'sec' is NULL, it means stack based
   allocation. 'flags & DIF_FIRST' is true if array '{' must be read (multi
   dimension implicit array init handling). 'flags & DIF_SIZE_ONLY' is true if
   size only evaluation is wanted (only for arrays). */
void decl_initializer(init_params *p, CType *type, unsigned long c, int flags, int vreg)
{
  int len, n, no_oblock, i;
  int size1, align1;
  int need_sso_swap = 0;
  Sym *s, *f;
  Sym indexsym;
  CType *t1;

  /* generate line number info */
  if (debug_modes && !(flags & DIF_SIZE_ONLY) && !p->sec)
    tcc_debug_line(tcc_state), tcc_tcov_check_line(tcc_state, 1);

  if (!(flags & DIF_HAVE_ELEM) && tok != '{' &&
      /* In case of strings we have special handling for arrays, so
         don't consume them as initializer value (which would commit them
         to some anonymous symbol).  */
      tok != TOK_LSTR && tok != TOK_STR &&
      (!(flags & DIF_SIZE_ONLY)
       /* a struct may be initialized from a struct of same type, as in
               struct {int x,y;} a = {1,2}, b = {3,4}, c[] = {a,b};
          In that case we need to parse the element in order to check
         it for compatibility below.  Likewise, an array may be
         initialized from a compound-literal expression such as
         '(const unsigned short[]){ ... }'. */
       || (type->t & VT_ARRAY) || (type->t & VT_BTYPE) == VT_STRUCT))
  {
    int ncw_prev = nocode_wanted;
    if ((flags & DIF_SIZE_ONLY) && !p->sec)
      ++nocode_wanted;
    parse_init_elem(!p->sec ? EXPR_ANY : EXPR_CONST);
    nocode_wanted = ncw_prev;
    flags |= DIF_HAVE_ELEM;
  }

  if (type->t & VT_ARRAY)
  {
    if ((flags & DIF_HAVE_ELEM) && is_compatible_unqualified_types(type, &vtop->type))
    {
      if ((flags & DIF_SIZE_ONLY) && type->ref->c < 0 && (vtop->type.t & VT_ARRAY) && vtop->type.ref->c > 0)
        decl_design_flex(p, type->ref, vtop->type.ref->c - 1);
      goto one_elem;
    }

    no_oblock = 1;
    if (((flags & DIF_FIRST) && tok != TOK_LSTR && tok != TOK_STR) || tok == '{')
    {
      skip('{');
      no_oblock = 0;
    }

    s = type->ref;
    n = s->c;
    t1 = pointed_type(type);
    size1 = type_size(t1, &align1);

    /* only parse strings here if correct type (otherwise: handle
       them as ((w)char *) expressions */
    if ((tok == TOK_LSTR &&
#ifdef TCC_TARGET_PE
         (t1->t & VT_BTYPE) == VT_SHORT && (t1->t & VT_UNSIGNED)
#else
         (t1->t & VT_BTYPE) == VT_INT
#endif
             ) ||
        (tok == TOK_STR && (t1->t & VT_BTYPE) == VT_BYTE))
    {
      len = 0;
      cstr_reset(&initstr);
      if (size1 != (tok == TOK_STR ? 1 : sizeof(nwchar_t)))
        tcc_error("unhandled string literal merging");
      while (tok == TOK_STR || tok == TOK_LSTR)
      {
        int tok_width = (tok == TOK_STR) ? 1 : (int)sizeof(nwchar_t);
        if (initstr.size)
          initstr.size -= size1;
        if (tok == TOK_STR)
          len += tokc.str.size;
        else
          len += tokc.str.size / sizeof(nwchar_t);
        len--;
        if (tok_width == size1)
        {
          cstr_cat(&initstr, tokc.str.data, tokc.str.size);
        }
        else if (size1 == (int)sizeof(nwchar_t) && tok == TOK_STR)
        {
          /* Mixing a narrow piece into a wide initializer (C permits e.g.
           * `L"a" "b"`): widen each byte to an nwchar_t element instead of
           * byte-copying it, which would otherwise be read back at the wider
           * element stride below and over-read initstr. */
          const unsigned char *np = (const unsigned char *)tokc.str.data;
          for (int z = 0; z < tokc.str.size; z++)
            cstr_wccat(&initstr, np[z]);
        }
        else
        {
          /* A wide piece in a narrow (char) array is not representable. */
          tcc_error("unhandled string literal merging");
        }
        next();
      }
      if (tok != ')' && tok != '}' && tok != ',' && tok != ';' && tok != TOK_EOF)
      {
        /* Not a lone literal but part of a bigger expression.  */
        unget_tok(size1 == 1 ? TOK_STR : TOK_LSTR);
        tokc.str.size = initstr.size;
        tokc.str.data = initstr.data;
        goto do_init_array;
      }

      decl_design_flex(p, s, len);
      if (!(flags & DIF_SIZE_ONLY))
      {
        n = s->c; /* re-read after flex array expansion */
        int nb = n, ch;
        if (len < nb)
          nb = len;
        if (len > nb)
          tcc_warning("initializer-string for array is too long");
        /* in order to go faster for common case (char
           string in global variable, we handle it
           specifically */
        if (p->sec && size1 == 1)
        {
          init_assert(p, c + nb);
          if (!NODATA_WANTED)
            memcpy(p->sec->data + c, initstr.data, nb);
        }
        else if (tcc_state->ir && size1 == 1 && nb >= 8 && !NODATA_WANTED)
        {
          /* Bulk copy string literal from .rodata instead of byte-by-byte stores.
           * Matches GCC: memcpy(dest, .rodata, str_len) + memset(trailing, 0, rem) */
          int copy_len = (nb < n) ? nb + 1 : nb;
          addr_t rodata_off = section_add(rodata_section, copy_len, 4);
          unsigned char *rodata_ptr = rodata_section->data + rodata_off;
          memcpy(rodata_ptr, initstr.data, copy_len);

          Sym *rodata_sym = get_sym_ref(&char_type, rodata_section, rodata_off, copy_len);

          SValue args[3];

          svalue_init(&args[0]);
          args[0].type = char_pointer_type;
          args[0].r = VT_LOCAL;
          args[0].c.i = c;
          args[0].vr = -1;

          svalue_init(&args[1]);
          args[1].type = char_pointer_type;
          args[1].r = VT_CONST | VT_SYM;
          args[1].sym = rodata_sym;
          args[1].c.i = 0;
          args[1].vr = -1;

          svalue_init(&args[2]);
          args[2].type.t = VT_INT;
          args[2].type.ref = NULL;
          args[2].r = VT_CONST;
          args[2].c.i = copy_len;
          args[2].vr = -1;

          gen_ir_void_call_args(args, 3, TOK_memcpy);

          int remaining = n - copy_len;
          if (remaining > 0 && !(flags & DIF_CLEAR))
            init_putz(p, c + copy_len, remaining);
        }
        else
        {
          for (i = 0; i < n; i++)
          {
            if (i >= nb)
            {
              /* only add trailing zero if enough storage (no
                 warning in this case since it is standard) */
              if (flags & DIF_CLEAR)
                break;
              if (n - i >= 4)
              {
                init_putz(p, c + i * size1, (n - i) * size1);
                break;
              }
              ch = 0;
            }
            else if (size1 == 1)
              ch = ((unsigned char *)initstr.data)[i];
            else
              ch = ((nwchar_t *)initstr.data)[i];
            vpushi(ch);
            init_putv(p, t1, c + i * size1, vreg);
          }
        }
      }
    }
    else
    {

    do_init_array:
      indexsym.c = 0;
      f = &indexsym;

    do_init_list:
      /* zero memory once in advance */
      if (!(flags & (DIF_CLEAR | DIF_SIZE_ONLY)))
      {
        init_putz(p, c, n * size1);
        flags |= DIF_CLEAR;
      }

      len = 0;
      /* GNU extension: if the initializer is empty for a flex array,
         it's size is zero.  We won't enter the loop, so set the size
         now.  */
      decl_design_flex(p, s, len);
      while (tok != '}' || (flags & DIF_HAVE_ELEM))
      {
        len = decl_designator(p, type, c, &f, flags, len);
        flags &= ~DIF_HAVE_ELEM;
        if (type->t & VT_ARRAY)
        {
          ++indexsym.c;
          /* special test for multi dimensional arrays (may not
             be strictly correct if designators are used at the
             same time) */
          if (no_oblock && len >= n * size1)
            break;
        }
        else
        {
          if (s->type.t == VT_UNION)
            f = NULL;
          else
            f = f->next;
          if (no_oblock && f == NULL)
            break;
        }

        if (tok == '}')
          break;
        skip(',');
      }
    }
    if (!no_oblock)
      skip('}');
    /* Byte-swap storage units for big-endian scalar_storage_order structs.
       After all bitfield values have been stored with LE byte order (using
       BE bit positions), swap bytes of each storage unit to produce the
       correct big-endian memory layout. */
    if (need_sso_swap && !(flags & DIF_SIZE_ONLY) && !NODATA_WANTED)
      sso_swap_struct_init(p, type, c);
  }
  else if ((flags & DIF_HAVE_ELEM)
           /* Use i_c_parameter_t, to strip toplevel qualifiers.
              The source type might have VT_CONSTANT set, which is
              of course assignable to non-const elements.  */
           && is_compatible_unqualified_types(type, &vtop->type))
  {
    goto one_elem;
  }
  else if ((type->t & VT_BTYPE) == VT_STRUCT && (type->t & VT_VECTOR))
  {
    /* GCC vector type: initialise element-wise, reusing the VT_ARRAY path.
     * Build a temporary fake array CType with the same element type and
     * element count, then recurse so the brace-enclosed list is processed
     * element by element (including designators and DIF_CLEAR handling). */
    CType elem_type, arr_type;
    Sym arr_sym;
    int elem_align_dummy, elem_sz, n_elems;

    elem_type = type->ref->type;
    elem_sz = type_size(&elem_type, &elem_align_dummy);
    n_elems = type->ref->c / elem_sz;

    memset(&arr_sym, 0, sizeof(arr_sym));
    arr_sym.type = elem_type; /* element type (pointed-to for VT_PTR|VT_ARRAY) */
    arr_sym.c = n_elems;      /* element count */

    arr_type.t = VT_PTR | VT_ARRAY;
    arr_type.ref = &arr_sym;

    decl_initializer(p, &arr_type, c, flags, vreg);
  }
  else if ((type->t & VT_BTYPE) == VT_STRUCT)
  {
    no_oblock = 1;
    if ((flags & DIF_FIRST) || tok == '{')
    {
      skip('{');
      no_oblock = 0;
    }
    s = type->ref;
    f = s->next;
    n = s->c;
    size1 = 1;
    if (s->a.sso_be)
      need_sso_swap = 1;
    goto do_init_list;
  }
  else if (tok == '{')
  {
    if (flags & DIF_HAVE_ELEM)
      skip(';');
    next();
    decl_initializer(p, type, c, flags & ~DIF_HAVE_ELEM, vreg);
    skip('}');
  }
  else
  one_elem:
    if ((flags & DIF_SIZE_ONLY))
    {
      /* If we supported only ISO C we wouldn't have to accept calling
         this on anything than an array if DIF_SIZE_ONLY (and even then
         only on the outermost level, so no recursion would be needed),
         because initializing a flex array member isn't supported.
         But GNU C supports it, so we need to recurse even into
         subfields of structs and arrays when DIF_SIZE_ONLY is set.  */
      /* just skip expression */
      if (flags & DIF_HAVE_ELEM)
        vpop();
      else
        skip_or_save_block(NULL);
    }
    else
    {
      if (!(flags & DIF_HAVE_ELEM))
      {
        /* This should happen only when we haven't parsed
           the init element above for fear of committing a
           string constant to memory too early.  */
        if (tok != TOK_STR && tok != TOK_LSTR)
          expect("string constant");
        parse_init_elem(!p->sec ? EXPR_ANY : EXPR_CONST);
      }
      if (!p->sec && (flags & DIF_CLEAR) /* container was already zero'd */
          && (vtop->r & (VT_VALMASK | VT_LVAL | VT_SYM)) == VT_CONST && vtop->c.i == 0 &&
          btype_size(type->t & VT_BTYPE) /* not for fp constants */
      )
        vpop();
      else
      {
        int align;
        int size = type_size(type, &align);
        /* Don't try to store empty structs (size 0) */
        if (size > 0)
          init_putv(p, type, c, vreg);
        else
          vpop(); /* pop the empty struct value */
      }
    }
}

/* RELRO support (-share-rodata): a const object can only acquire a relocation
   (and therefore must stay in the per-process writable data segment rather than
   the shared, read-only .rodata) if its initializer stores an address — which
   requires a pointer somewhere in its type. Returns 1 if 'type' contains a
   pointer, recursing through array element types and struct/union members.
   Sound: every relocation into a const object originates from a pointer-typed
   sub-object, so a type with no pointer can never be relocated.
   Written with explicit branches (no folded ternaries) — this code runs under
   the armv8m self-host, which has historically miscompiled compact forms. */
static int type_contains_pointer(CType *type)
{
  CType *tp = type;
  int bt;
  /* Strip array dimensions: an array is VT_BTYPE==VT_PTR with VT_ARRAY set;
     its element type is reached through ref->type. */
  while ((tp->t & VT_ARRAY) && (tp->t & VT_BTYPE) == VT_PTR)
  {
    tp = &tp->ref->type;
  }
  bt = tp->t & VT_BTYPE;
  if (bt == VT_PTR)
  {
    /* A real pointer (arrays were stripped above). */
    return 1;
  }
  if (bt == VT_STRUCT)
  {
    Sym *f;
    for (f = tp->ref->next; f; f = f->next)
    {
      if (type_contains_pointer(&f->type))
      {
        return 1;
      }
    }
    return 0;
  }
  return 0;
}

/* parse an initializer for type 't' if 'has_init' is non zero, and
   allocate space in local or global data space ('r' is either
   VT_LOCAL or VT_CONST). If 'v' is non zero, then an associated
   variable 'v' of scope 'scope' is declared before initializers
   are parsed. If 'v' is zero, then a reference to the new object
   is put in the value stack. If 'has_init' is 2, a special parsing
   is done to handle string constants. */
void decl_initializer_alloc(CType *type, AttributeDef *ad, int r, int has_init, int v, int global)
{
  int size, align, addr;
  TokenString *init_str = NULL;
  int vreg = -1;
  Section *sec;
  Sym *flexible_array;
  Sym *sym;
  int saved_nocode_wanted = nocode_wanted;
#ifdef CONFIG_TCC_BCHECK
  int bcheck = tcc_state->do_bounds_check && !NODATA_WANTED;
#endif
  init_params p = {0};

  /* Always allocate static or global variables */
  if (v && (r & VT_VALMASK) == VT_CONST)
    nocode_wanted |= DATA_ONLY_WANTED;

  flexible_array = NULL;
  size = type_size(type, &align);

  /* exactly one flexible array may be initialized, either the
     toplevel array or the last member of the toplevel struct */

  if (size < 0)
  {
    // error out except for top-level incomplete arrays
    // (arrays of incomplete types are handled in array parsing)
    if (!(type->t & VT_ARRAY))
      tcc_error("initialization of incomplete type");

    /* If the base type itself was an array type of unspecified size
       (like in 'typedef int arr[]; arr x = {1};') then we will
       overwrite the unknown size by the real one for this decl.
       We need to unshare the ref symbol holding that size. */
    type->ref = sym_push(SYM_FIELD, &type->ref->type, 0, type->ref->c);
    p.flex_array_ref = type->ref;
  }
  else if (has_init && (type->t & VT_BTYPE) == VT_STRUCT)
  {
    Sym *field = type->ref->next;
    if (field)
    {
      while (field->next)
        field = field->next;
      if (field->type.t & VT_ARRAY && field->type.ref->c < 0)
      {
        flexible_array = field;
        p.flex_array_ref = field->type.ref;
        size = -1;
      }
    }
    /* For unions: if any member is a struct with a flexible array
       member, set flex_array_ref so the FAM can be initialized.
       The union already provides backing storage, so no dry-run
       size computation is needed. */
    if (!flexible_array && IS_UNION(type->ref->type.t))
    {
      Sym *member;
      for (member = type->ref->next; member; member = member->next)
      {
        if ((member->type.t & VT_BTYPE) == VT_STRUCT)
        {
          Sym *mf = member->type.ref->next;
          if (mf)
          {
            while (mf->next)
              mf = mf->next;
            if (mf->type.t & VT_ARRAY && mf->type.ref->c < 0)
            {
              p.flex_array_ref = mf->type.ref;
              break;
            }
          }
        }
      }
    }
  }

  if (size < 0)
  {
    /* If unknown size, do a dry-run 1st pass */
    if (!has_init)
      tcc_error("unknown type size");
    if (has_init == 2)
    {
      /* only get strings */
      init_str = tok_str_alloc();
      while (tok == TOK_STR || tok == TOK_LSTR)
      {
        tok_str_add_tok(init_str);
        next();
      }
      tok_str_add(init_str, TOK_EOF);
    }
    else
      skip_or_save_block(&init_str);
    unget_tok(0);

    /* compute size */
    begin_macro(init_str, 1);
    next();
    decl_initializer(&p, type, 0, DIF_FIRST | DIF_SIZE_ONLY, vreg);
    /* prepare second initializer parsing */
    macro_ptr = tok_str_buf(init_str);
    next();

    /* if still unknown size, error */
    size = type_size(type, &align);
    if (size < 0)
      tcc_error("unknown type size");

    /* If there's a flex member and it was used in the initializer
       adjust size.  */
    if (flexible_array && flexible_array->type.ref->c > 0)
      size += flexible_array->type.ref->c * pointed_size(&flexible_array->type);
  }

  /* take into account specified alignment if bigger */
  if (ad->a.aligned)
  {
    int speca = 1 << (ad->a.aligned - 1);
    if (speca > align)
      align = speca;
  }
  else if (ad->a.packed)
  {
    align = 1;
  }

  if (!v && NODATA_WANTED)
  {
    size = 0, align = 1;
  }

  if ((r & VT_VALMASK) == VT_LOCAL)
  {
    sec = NULL;
#ifdef CONFIG_TCC_BCHECK
    if (bcheck && v)
    {
      /* add padding between stack variables for bound checking */
      loc -= align;
    }
#endif
    if (type->t & VT_VLA)
    {
      /* VLA types need a pointer-sized slot for the alloca'd data pointer.
       * The VLA byte-size was already stored in a separate slot allocated
       * during type parsing (post_type); do not reuse that slot. */
      loc = (loc - PTR_SIZE) & -PTR_SIZE;
    }
    else if (!((r & VT_LVAL) && ((type->t & VT_BTYPE) != VT_STRUCT) && !(type->t & VT_COMPLEX)))
    {
      // allocate stack for variables that are not register allocation
      // candidates.  Complex types need explicit stack allocation since
      // they are too large for single vregs and use offset-based access
      // via __real__/__imag__.
      // VLA structs allocate a pointer slot instead of
      // the full struct — the actual data is VLA_ALLOC'd later.
      if (struct_has_vla_member(type))
        loc = (loc - PTR_SIZE) & -PTR_SIZE;
      else
      {
        /* Floor the SLOT alignment of word-or-larger aggregates at 4, whatever
         * the type says.  `align` is the C type's alignment, which a packed
         * aggregate drives to 1 (see the ad->a.packed branch above), so a
         * `& -1` leaves the local wherever the running `loc` happened to land
         * — odd offsets included.  The backend then cannot use LDRD/LDM to
         * marshal it by value (both fault on unaligned addresses on Cortex-M
         * regardless of UNALIGN_TRP) and falls back to one LDR per word.
         *
         * Packing controls the layout of fields WITHIN the object; it does not
         * require the object itself to be misaligned, so raising the slot's
         * alignment is standards-neutral.  It costs at most 3 bytes of frame
         * per such local and also spares every plain word access the unaligned
         * penalty. */
        int slot_align = align;
        if (size >= 4 && slot_align < 4 && (type->t & VT_BTYPE) == VT_STRUCT)
          slot_align = 4;
        loc = (loc - size) & -slot_align;
      }
    }
    addr = loc;
    p.local_offset = addr + size;
#ifdef CONFIG_TCC_BCHECK
    if (bcheck && v)
    {
      /* add padding between stack variables for bound checking */
      loc -= align;
    }
#endif
    if (v)
    {
      /* local variable */
#ifdef CONFIG_TCC_ASM
      if (ad->asm_label)
      {
        int reg = asm_parse_regvar(ad->asm_label);
        if (reg >= 0)
          r = (r & ~VT_VALMASK) | reg;
      }
#endif
      sym = sym_push(v, type, r, addr);
      vreg = sym->vreg;
      if (ad->cleanup_func)
      {
        Sym *cls = sym_push2(&all_cleanups, SYM_FIELD | ++cur_scope->cl.n, 0, 0);
        cls->prev_tok = sym;
        cls->cleanup_func = ad->cleanup_func;
        cls->next = cur_scope->cl.s;
        cur_scope->cl.s = cls;
      }

      sym->a = ad->a;

      /* For small local arrays/vectors with an initializer, allocate a
       * buffer where init_putv will capture constant scalar values.
       * Lets later passes (e.g. __builtin_shuffle) treat the variable
       * as having compile-time-known contents when it is read-only. */
      if (has_init && size > 0 && size <= 256 && ((type->t & VT_ARRAY) || (type->t & VT_VECTOR)) &&
          !(type->t & VT_VLA))
      {
        sym->const_init_data = tcc_mallocz(size);
        sym->const_init_size = size;
        sym->const_init_valid = 1;
        sym->const_init_in_progress = 1;
        p.const_init_sym = sym;
        p.const_init_base = addr;
      }
    }
    else
    {
      /* push local reference */
      vset(type, r, addr);
      /* Anonymous compound literals (v==0): set up const_init_data tracking
       * via a dummy Sym so vector constant folding can read the data. */
      if (has_init && size > 0 && size <= 256 && ((type->t & VT_ARRAY) || (type->t & VT_VECTOR)) &&
          !(type->t & VT_VLA))
      {
        Sym *anon = sym_push2(&local_stack, SYM_FIRST_ANOM, type->t, addr);
        anon->type.ref = type->ref;
        anon->const_init_data = tcc_mallocz(size);
        anon->const_init_size = size;
        anon->const_init_valid = 1;
        anon->const_init_in_progress = 1;
        p.const_init_sym = anon;
        p.const_init_base = addr;
      }
    }
  }
  else
  {
    sym = NULL;
    if (v && global)
    {
      /* see if the symbol was already defined */
      sym = sym_find(v);
      if (sym)
      {
        if (p.flex_array_ref && (sym->type.t & type->t & VT_ARRAY) && sym->type.ref->c > type->ref->c)
        {
          /* flex array was already declared with explicit size
                  extern int arr[10];
                  int arr[] = { 1,2,3 }; */
          type->ref->c = sym->type.ref->c;
          size = type_size(type, &align);
        }
        patch_storage(sym, ad, type);
        /* we accept several definitions of the same global variable. */
        if (!has_init && sym->c && elfsym(sym)->st_shndx != SHN_UNDEF)
          goto no_alloc;
      }
    }

    /* allocate symbol in corresponding section */
    sec = ad->section;
    if (!sec)
    {
      CType *tp = type;
      while ((tp->t & (VT_BTYPE | VT_ARRAY)) == (VT_PTR | VT_ARRAY))
        tp = &tp->ref->type;
      if (tp->t & VT_CONSTANT)
      {
        /* RELRO: with -share-rodata, a const object whose type contains a
           pointer can hold a relocation, so it must live in the writable
           per-process data segment (GOTOFF-addressed, like .data) — leaving
           .rodata pure-const and shareable read-only across processes. */
        if (tcc_state->share_rodata && type_contains_pointer(type))
        {
          sec = data_section;
        }
        else
        {
          sec = rodata_section;
        }
      }
      else if (has_init)
      {
        sec = data_section;
        /*if (tcc_state->g_debug & 4)
            tcc_warning("rw data: %s", get_tok_str(v, 0));*/
      }
      else if (tcc_state->nocommon)
        sec = bss_section;
    }

    if (sec)
    {
      addr = section_add(sec, size, align);
#ifdef CONFIG_TCC_BCHECK
      /* add padding if bound check */
      if (bcheck)
        section_add(sec, 1, 1);
#endif
    }
    else
    {
      addr = align; /* SHN_COMMON is special, symbol value is align */
      sec = common_section;
    }

    if (v)
    {
      if (!sym)
      {
        sym = sym_push(v, type, r | VT_SYM, 0);
        vreg = sym->vreg;
        patch_storage(sym, ad, NULL);
      }
      /* update symbol definition */
      put_extern_sym(sym, sec, addr, size);
    }
    else
    {
      /* push global reference */
      vpush_ref(type, sec, addr, size);
      sym = vtop->sym;
      vtop->r |= r;
    }

#ifdef CONFIG_TCC_BCHECK
    /* handles bounds now because the symbol must be defined
       before for the relocation */
    if (bcheck)
    {
      addr_t *bounds_ptr;

      greloca(bounds_section, sym, bounds_section->data_offset, R_DATA_PTR, 0);
      /* then add global bound info */
      bounds_ptr = section_ptr_add(bounds_section, 2 * sizeof(addr_t));
      bounds_ptr[0] = 0; /* relocated */
      bounds_ptr[1] = size;
    }
#endif
  }

  if (type->t & VT_VLA)
  {
    int a;

    if (NODATA_WANTED)
      goto no_alloc;

    if (tcc_state->ir)
      tcc_state->force_frame_pointer = 1;

    /* save before-VLA stack pointer if needed */
    if (cur_scope->vla.num == 0)
    {
      if (cur_scope->prev && cur_scope->prev->vla.num)
      {
        cur_scope->vla.locorig = cur_scope->prev->vla.loc;
      }
      else
      {
        /* No outer VLA active: lazily allocate a slot and save the current SP
         * as the "before VLA" restore point for VLAs introduced in this scope. */
        loc -= PTR_SIZE;
        if (tcc_state->ir)
        {
          tcc_ir_gen_vla_sp_save(tcc_state->ir, loc);
        }
        else
        {
          gen_vla_sp_save(loc);
        }
        cur_scope->vla.locorig = loc;
      }
    }

    vpush_type_size(type, &a);
    if (tcc_state->ir)
    {
      /* vtop holds the runtime allocation size (bytes). Emit an IR op that
       * adjusts SP and aligns it. */
      SValue size_sv = *vtop;
      tcc_ir_gen_vla_alloc(tcc_state->ir, &size_sv, a);
      vpop();
    }
    else
    {
      gen_vla_alloc(type, a);
    }
#if defined TCC_TARGET_PE && defined TCC_TARGET_X86_64
    /* on _WIN64, because of the function args scratch area, the
       result of alloca differs from RSP and is returned in RAX.  */
    gen_vla_result(addr), addr = (loc -= PTR_SIZE);
#endif

    if (tcc_state->ir)
    {
      tcc_ir_gen_vla_sp_save(tcc_state->ir, addr);
    }
    else
    {
      gen_vla_sp_save(addr);
    }
    cur_scope->vla.loc = addr;
    cur_scope->vla.num++;
  }
  else if ((r & VT_VALMASK) == VT_LOCAL && struct_has_vla_member(type) && !NODATA_WANTED)
  {
    /* The struct contains VLA member(s) stored inline.  Allocate the
       entire struct (fixed + VLA portions) dynamically via VLA_ALLOC.
       The struct is accessed indirectly through a pointer stored at addr. */
    int a;

    if (tcc_state->ir)
      tcc_state->force_frame_pointer = 1;

    /* save before-VLA stack pointer if needed */
    if (cur_scope->vla.num == 0)
    {
      if (cur_scope->prev && cur_scope->prev->vla.num)
      {
        cur_scope->vla.locorig = cur_scope->prev->vla.loc;
      }
      else
      {
        loc -= PTR_SIZE;
        if (tcc_state->ir)
        {
          tcc_ir_gen_vla_sp_save(tcc_state->ir, loc);
        }
        else
        {
          gen_vla_sp_save(loc);
        }
        cur_scope->vla.locorig = loc;
      }
    }

    /* Compute total runtime struct size: fixed_component + sum of VLA sizes */
    vpush_type_size(type, &a);

    if (tcc_state->ir)
    {
      SValue size_sv = *vtop;
      tcc_ir_gen_vla_alloc(tcc_state->ir, &size_sv, a);
      vpop();
    }
    else
    {
      gen_vla_alloc(type, a);
    }

    /* Save the allocated address (current SP after VLA_ALLOC) to the
       struct's addr slot, which was already reserved by loc -= PTR_SIZE
       at declaration time (addr already points to a PTR_SIZE slot). */
    if (tcc_state->ir)
    {
      tcc_ir_gen_vla_sp_save(tcc_state->ir, addr);
    }
    else
    {
      gen_vla_sp_save(addr);
    }
    cur_scope->vla.loc = addr;
    cur_scope->vla.num++;
  }
  else if (has_init)
  {
    p.sec = sec;

    /* NRVO: for a local _Complex initializer, hint that the first
     * sret-returning call inside the initializer expression may write
     * directly to this variable's slot instead of a fresh temp.
     *
     * Limited to VT_COMPLEX for now: VT_STRUCT locals are sometimes
     * placed at offsets that don't match the addr computed here (they
     * may be spilled later by the register allocator), so the NRVO
     * pointer would point at the wrong slot. */
    int saved_nrvo_active = tcc_state->nrvo_target_active;
    int saved_nrvo_loc = tcc_state->nrvo_target_loc;
    int saved_nrvo_vreg = tcc_state->nrvo_target_vreg;
    int saved_nrvo_size = tcc_state->nrvo_target_size;
    int saved_nrvo_align = tcc_state->nrvo_target_align;
    int saved_nrvo_ptr_vreg = tcc_state->nrvo_target_ptr_vreg;
    if (!sec && tcc_state->ir &&
        ((type->t & VT_BTYPE) == VT_STRUCT || (type->t & VT_COMPLEX)))
    {
      int nrvo_size, nrvo_align;
      nrvo_size = type_size(type, &nrvo_align);
      tcc_state->nrvo_target_active = 1;
      tcc_state->nrvo_target_loc = addr;
      tcc_state->nrvo_target_vreg = vreg;
      tcc_state->nrvo_target_size = nrvo_size;
      tcc_state->nrvo_target_align = nrvo_align;
      tcc_state->nrvo_target_ptr_vreg = -1;
    }

    /* Large constant local array: lower as a single memcpy from a .rodata
     * template (matching GCC) instead of memset + a store per non-zero
     * element.  Probe the initializer into a throwaway template; only if it
     * is entirely load-time-constant do we emit the memcpy and skip the
     * per-element path.  Otherwise rewind and fall through to normal init. */
    int templated = 0;
    if (!sec && tcc_state->ir && has_init && !NODATA_WANTED && (type->t & VT_ARRAY) && !(type->t & VT_VLA) &&
        !(type->t & VT_COMPLEX) && size > 256)
    {
      /* Both paths leave the live token at the start of the initializer:
       * the unknown-size path has reset the macro to its start, and the
       * known-size path is reading the live stream.  Only divert brace
       * initializers (strings are already bulk-copied elsewhere). */
      if (tok == '{')
      {
        TokenString *saved = init_str;
        if (!saved)
        {
          skip_or_save_block(&saved);
          unget_tok(0);
          begin_macro(saved, 1);
          next();
          init_str = saved; /* so the no_alloc cleanup pops the macro */
        }
        /* Probe the initializer into a host-side template buffer under
         * nocode_wanted (so runtime initializer expressions emit no code and
         * the fall-back reparse stays clean).  const_probe_failed is set if any
         * element is not a plain load-time-constant integer/pointer. */
        unsigned char *tmpl_buf = tcc_mallocz(size);
        init_params pp = {0};
        pp.const_probe = 1;
        pp.const_probe_data = tmpl_buf;
        pp.const_probe_base = addr;
        pp.const_probe_size = size;
        pp.flex_array_ref = p.flex_array_ref;
        nocode_wanted++;
        decl_initializer(&pp, type, addr, DIF_FIRST, -1);
        nocode_wanted--;
        /* Density gate: the per-element path costs ~memset + a store per
         * non-zero element, while the template costs a fixed memcpy plus the
         * full initializer in .rodata.  Only template when enough of the array
         * is non-zero — otherwise a sparse initializer (e.g. `int t[1025] =
         * { 1024 }`) would bloat code and defeat dead-store elimination. */
        int nz = 0;
        if (!pp.const_probe_failed)
        {
          for (int bi = 0; bi < size; bi++)
            if (tmpl_buf[bi])
              nz++;
        }
        if (!pp.const_probe_failed && nz >= 16 && nz * 4 >= size)
        {
          /* Commit the template to .rodata and emit memcpy(&local, &tmpl, size). */
          int tmpl_off = section_add(rodata_section, size, align);
          if (!NODATA_WANTED)
            memcpy(rodata_section->data + tmpl_off, tmpl_buf, size);
          Sym *tmpl_sym = get_sym_ref(&char_type, rodata_section, tmpl_off, size);
          SValue cargs[3];

          svalue_init(&cargs[0]);
          cargs[0].type = char_pointer_type;
          cargs[0].r = VT_LOCAL;
          cargs[0].c.i = addr;
          cargs[0].vr = -1;

          svalue_init(&cargs[1]);
          cargs[1].type = char_pointer_type;
          cargs[1].r = VT_CONST | VT_SYM;
          cargs[1].sym = tmpl_sym;
          cargs[1].c.i = 0;
          cargs[1].vr = -1;

          svalue_init(&cargs[2]);
          cargs[2].type.t = VT_INT;
          cargs[2].type.ref = NULL;
          cargs[2].r = VT_CONST;
          cargs[2].c.i = size;
          cargs[2].vr = -1;

          gen_ir_void_call_args(cargs, 3, TOK_memcpy);
          templated = 1;
        }
        else
        {
          /* Not all-constant: rewind the macro for a normal per-element pass. */
          macro_ptr = tok_str_buf(saved);
          next();
        }
        tcc_free(tmpl_buf);
      }
    }

    if (!templated)
      decl_initializer(&p, type, addr, DIF_FIRST, vreg);

    if (p.const_init_sym)
      p.const_init_sym->const_init_in_progress = 0;

    tcc_state->nrvo_target_ptr_vreg = saved_nrvo_ptr_vreg;
    tcc_state->nrvo_target_active = saved_nrvo_active;
    tcc_state->nrvo_target_loc = saved_nrvo_loc;
    tcc_state->nrvo_target_vreg = saved_nrvo_vreg;
    tcc_state->nrvo_target_size = saved_nrvo_size;
    tcc_state->nrvo_target_align = saved_nrvo_align;

    /* patch flexible array member size back to -1, */
    /* for possible subsequent similar declarations */
    if (flexible_array)
      flexible_array->type.ref->c = -1;
  }

no_alloc:
  /* restore parse state if needed */
  if (init_str)
  {
    end_macro_to(init_str);
    next();
  }

  nocode_wanted = saved_nocode_wanted;
}
