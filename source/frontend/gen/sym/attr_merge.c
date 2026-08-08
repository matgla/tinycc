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

/* attr_merge.c -- Symbol attribute merging, type/storage patching and alias resolution.
 * Split out of tccgen.c; see docs/plan_tccgen_split.md. */

#include "gen_priv.h"

/* Merge symbol attributes.  */
void merge_symattr(struct SymAttr *sa, struct SymAttr *sa1)
{
  if (sa1->aligned && !sa->aligned)
    sa->aligned = sa1->aligned;
  sa->packed |= sa1->packed;
  sa->weak |= sa1->weak;
  sa->nodebug |= sa1->nodebug;
  if (sa1->visibility != STV_DEFAULT)
  {
    int vis = sa->visibility;
    if (vis == STV_DEFAULT || vis > sa1->visibility)
      vis = sa1->visibility;
    sa->visibility = vis;
  }
  sa->dllexport |= sa1->dllexport;
  sa->nodecorate |= sa1->nodecorate;
  sa->dllimport |= sa1->dllimport;
  sa->naked |= sa1->naked;
  sa->transparent_union |= sa1->transparent_union;
}

/* Merge function attributes.  */
void merge_funcattr(struct FuncAttr *fa, struct FuncAttr *fa1)
{
  if (fa1->func_call && !fa->func_call)
    fa->func_call = fa1->func_call;
  if (fa1->func_type && !fa->func_type)
    fa->func_type = fa1->func_type;
  if (fa1->func_args && !fa->func_args)
    fa->func_args = fa1->func_args;
  if (fa1->func_alwinl)
    fa->func_alwinl = 1;
  if (fa1->func_noreturn)
    fa->func_noreturn = 1;
  if (fa1->func_ctor)
    fa->func_ctor = 1;
  if (fa1->func_dtor)
    fa->func_dtor = 1;
  if (fa1->func_pure)
    fa->func_pure = 1;
  if (fa1->func_const)
    fa->func_const = 1;
  if (fa1->func_noinline)
    fa->func_noinline = 1;
  if (fa1->func_no_instrument)
    fa->func_no_instrument = 1;
  /* func_rewritten_extern_inline is parser provenance for one specific
     definition and should not be inherited by a later replacement
     definition. */
}

/* Merge attributes.  */
void merge_attr(AttributeDef *ad, AttributeDef *ad1)
{
  merge_symattr(&ad->a, &ad1->a);
  merge_funcattr(&ad->f, &ad1->f);

  if (ad1->section)
    ad->section = ad1->section;
  if (ad1->alias_target)
    ad->alias_target = ad1->alias_target;
  if (ad1->asm_label)
    ad->asm_label = ad1->asm_label;
  if (ad1->attr_mode)
    ad->attr_mode = ad1->attr_mode;
  if (ad1->vector_size)
    ad->vector_size = ad1->vector_size;
}

/* Merge some type attributes.  */
static void patch_type(Sym *sym, CType *type)
{
  int old_rewritten_extern_inline = 0;
  int new_rewritten_extern_inline = 0;

  if ((sym->type.t & VT_BTYPE) == VT_FUNC && sym->type.ref)
    old_rewritten_extern_inline = sym->type.ref->f.func_rewritten_extern_inline;
  if ((type->t & VT_BTYPE) == VT_FUNC && type->ref)
    new_rewritten_extern_inline = type->ref->f.func_rewritten_extern_inline;

  if (!(type->t & VT_EXTERN) || IS_ENUM_VAL(sym->type.t))
  {
    if (!(sym->type.t & VT_EXTERN))
    {
      /* A rewritten 'extern inline' definition behaves like an inline-only
         body and may be replaced once by a later real definition (plain,
         inline, or static inline).  Another rewritten extern-inline is still
         a duplicate definition and must be rejected. */
      if (old_rewritten_extern_inline && !new_rewritten_extern_inline)
        sym->type.t &= ~(VT_STATIC | VT_INLINE);
      else
        tcc_error("redefinition of '%s'", get_tok_str(sym->v, NULL));
    }
    sym->type.t &= ~VT_EXTERN;
  }

  if (IS_ASM_SYM(sym))
  {
    /* stay static if both are static */
    sym->type.t = type->t & (sym->type.t | ~VT_STATIC);
    sym->type.ref = type->ref;
    if ((type->t & VT_BTYPE) != VT_FUNC && !(type->t & VT_ARRAY))
      sym->r |= VT_LVAL;
  }

  if (!is_compatible_types(&sym->type, type))
  {
    tcc_error("incompatible types for redefinition of '%s'", get_tok_str(sym->v, NULL));
  }
  else if ((sym->type.t & VT_BTYPE) == VT_FUNC)
  {
    int static_proto = sym->type.t & VT_STATIC;
    /* warn if static follows non-static function declaration */
    if ((type->t & VT_STATIC) &&
        !static_proto
        /* XXX this test for inline shouldn't be here.  Until we
           implement gnu-inline mode again it silences a warning for
           mingw caused by our workarounds.  */
        && !((type->t | sym->type.t) & VT_INLINE))
      tcc_warning("static storage ignored for redefinition of '%s'", get_tok_str(sym->v, NULL));

    /* set 'inline' if both agree or if one has static */
    if ((type->t | sym->type.t) & VT_INLINE)
    {
      if (!((type->t ^ sym->type.t) & VT_INLINE) || ((type->t | sym->type.t) & VT_STATIC))
        static_proto |= VT_INLINE;
    }

    if (0 == (type->t & VT_EXTERN))
    {
      struct FuncAttr f = sym->type.ref->f;
      /* put complete type, use static from prototype */
      sym->type.t = (type->t & ~(VT_STATIC | VT_INLINE)) | static_proto;
      sym->type.ref = type->ref;
      merge_funcattr(&sym->type.ref->f, &f);
      sym->type.ref->f.func_rewritten_extern_inline = new_rewritten_extern_inline;
    }
    else
    {
      sym->type.t &= ~VT_INLINE | static_proto;
    }

    if (sym->type.ref->f.func_type == FUNC_OLD && type->ref->f.func_type != FUNC_OLD
        && !local_scope)
    {
      sym->type.ref = type->ref;
    }
  }
  else
  {
    if ((sym->type.t & VT_ARRAY) && type->ref->c >= 0)
    {
      /* set array size if it was omitted in extern declaration */
      sym->type.ref->c = type->ref->c;
    }
    if ((type->t ^ sym->type.t) & VT_STATIC)
      tcc_warning("storage mismatch for redefinition of '%s'", get_tok_str(sym->v, NULL));
  }
}

/* Merge some storage attributes.  */
void patch_storage(Sym *sym, AttributeDef *ad, CType *type)
{
  if (type)
    patch_type(sym, type);

#ifdef TCC_TARGET_PE
  if (sym->a.dllimport != ad->a.dllimport)
    tcc_error("incompatible dll linkage for redefinition of '%s'", get_tok_str(sym->v, NULL));
#endif
  merge_symattr(&sym->a, &ad->a);
  /* Note: func_pure/func_const attributes are handled in external_sym
   * and in the function type symbol (type.ref->f), not in sym->f.
   * We don't merge ad->f into sym->f here to avoid corrupting function
   * type information (func_type, func_args). */
  if (ad->asm_label)
    sym->asm_label = ad->asm_label;
  update_storage(sym);
}

/* copy sym to other stack */
static Sym *sym_copy(Sym *s0, Sym **ps)
{
  Sym *s;
  s = sym_malloc(), *s = *s0;
  s->prev = *ps, *ps = s;
  if (s->v < SYM_FIRST_ANOM)
  {
    ps = &table_ident[s->v - TOK_IDENT]->sym_identifier;
    s->prev_tok = *ps, *ps = s;
  }
  return s;
}

/* copy s->type.ref to stack 'ps' for VT_FUNC and VT_PTR */
void sym_copy_ref(Sym *s, Sym **ps)
{
  int bt = s->type.t & VT_BTYPE;
  if (bt == VT_FUNC || bt == VT_PTR || (bt == VT_STRUCT && s->sym_scope))
  {
    /* For VLA array types, the SYM_FIELD's next/vla_array_str union may
       contain a token stream pointer (set in post_type for TYPE_NEST),
       not a valid Sym* chain.  Don't follow next in that case. */
    int is_vla = s->type.t & VT_VLA;
    Sym **sp = &s->type.ref;
    for (s = *sp, *sp = NULL; s; s = s->next)
    {
      /* For struct types without local scope, don't copy - preserve type identity.
       * This fixes nested function struct return type mismatches where the struct
       * type would be copied, creating different ref pointers for the same type. */
      if ((s->type.t & VT_BTYPE) == VT_STRUCT && !s->sym_scope)
      {
        /* Keep the original global struct type, don't copy */
        *sp = s;
        sp = &s->next;
      }
      else
      {
        Sym *s2 = sym_copy(s, ps);
        sp = &(*sp = s2)->next;
        sym_copy_ref(s2, ps);
      }
      if (is_vla)
      {
        /* Stop after the first field — s2->next is in a union with
           vla_array_str and may hold a token stream pointer, not a
           valid Sym*. Do NOT clear *sp because it points to the
           next/vla_array_str union and we must preserve vla_array_str. */
        break;
      }
    }
  }
}

/* define a new external reference to a symbol 'v' */
Sym *external_sym(int v, CType *type, int r, AttributeDef *ad)
{
  Sym *s;

  /* look for global symbol */
  s = sym_find(v);
  while (s && s->sym_scope)
    s = s->prev_tok;

  if (!s)
  {
    /* push forward reference */
    s = global_identifier_push(v, type->t, 0);
    s->r |= r;
    s->a = ad->a;
    /* Merge function attributes (pure, const, etc.) without overwriting
     * func_type and func_args which are set from type.ref->f */
    if (ad->f.func_pure)
      s->f.func_pure = 1;
    if (ad->f.func_const)
      s->f.func_const = 1;
    s->asm_label = ad->asm_label;
    s->type.ref = type->ref;
    /* copy type to the global stack */
    if (local_stack)
      sym_copy_ref(s, &global_stack);
  }
  else
  {
    patch_storage(s, ad, type);
  }
  /* push variables on local_stack if any */
  if (local_stack && (s->type.t & VT_BTYPE) != VT_FUNC)
    s = sym_copy(s, &local_stack);
  return s;
}

static Sym *find_global_alias_target_sym(int target_tok)
{
  Sym *s;

  s = sym_find(target_tok);
  while (s && s->sym_scope)
    s = s->prev_tok;
  if (s)
    return s;

  for (s = global_stack; s; s = s->prev)
  {
    if (!s->sym_scope && s->asm_label == target_tok)
      return s;
  }

  return NULL;
}

static int resolve_alias_symbol(Sym *alias_sym, int target_tok, int report_error)
{
  Sym *target_sym;
  ElfSym *esym;

  target_sym = find_global_alias_target_sym(target_tok);
  if (target_sym == alias_sym)
    tcc_error("'%s' is part of alias cycle", get_tok_str(alias_sym->v, NULL));
  if (!target_sym || target_sym->c <= 0)
    goto not_found;

  esym = elfsym(target_sym);
  if (!esym || esym->st_shndx == SHN_UNDEF)
    goto not_found;

  put_extern_sym2(alias_sym, esym->st_shndx, esym->st_value, esym->st_size, 1);
  return 1;

not_found:
  if (report_error)
  {
    tcc_error("'%s' aliased to undefined symbol '%s'", get_tok_str(alias_sym->v, NULL), get_tok_str(target_tok, NULL));
  }
  return 0;
}

static void queue_alias_symbol(Sym *alias_sym, int target_tok)
{
  pending_aliases = tcc_realloc(pending_aliases, (nb_pending_aliases + 1) * sizeof(*pending_aliases));
  pending_aliases[nb_pending_aliases].alias_sym = alias_sym;
  pending_aliases[nb_pending_aliases].target_tok = target_tok;
  ++nb_pending_aliases;
}

void apply_alias_attribute(Sym *alias_sym, int target_tok)
{
  if (!resolve_alias_symbol(alias_sym, target_tok, 0))
    queue_alias_symbol(alias_sym, target_tok);
}

void resolve_pending_aliases(void)
{
  int i, write_idx, progress;

  do
  {
    progress = 0;
    write_idx = 0;
    for (i = 0; i < nb_pending_aliases; ++i)
    {
      if (resolve_alias_symbol(pending_aliases[i].alias_sym, pending_aliases[i].target_tok, 0))
      {
        progress = 1;
      }
      else
      {
        pending_aliases[write_idx++] = pending_aliases[i];
      }
    }
    nb_pending_aliases = write_idx;
  } while (progress && nb_pending_aliases > 0);

  for (i = 0; i < nb_pending_aliases; ++i)
    resolve_alias_symbol(pending_aliases[i].alias_sym, pending_aliases[i].target_tok, 1);

  tcc_free(pending_aliases);
  pending_aliases = NULL;
  nb_pending_aliases = 0;
}
