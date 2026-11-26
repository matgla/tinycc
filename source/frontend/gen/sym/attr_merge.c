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
  sa->vis_explicit |= sa1->vis_explicit;
  sa->dllexport |= sa1->dllexport;
#ifdef TCC_TARGET_PE
  sa->nodecorate |= sa1->nodecorate;
#endif
  sa->dllimport |= sa1->dllimport;
  sa->naked |= sa1->naked;
  sa->transparent_union |= sa1->transparent_union;
  sa->used |= sa1->used;
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
  if (fa1->func_leaf)
    fa->func_leaf = 1;
  if (fa1->func_sys_decl)
    fa->func_sys_decl = 1;
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
  ad->attr_unused |= ad1->attr_unused;
  if (ad1->vector_size)
    ad->vector_size = ad1->vector_size;
}

/* The function type a declaration's own declarator built (decl.c sets it
 * around external_sym): no typedef or other declaration holds it. */
Sym *patch_own_functype;

/* Whether SRC marks a parameter nonnull that DST does not. */
static int param_nonnull_missing(Sym *dst, Sym *src)
{
  Sym *d = sym_first(dst), *s = sym_first(src);
  for (; d && s; d = d->next, s = s->next)
    if (s->a.nonnull && !d->a.nonnull && (d->type.t & VT_BTYPE) == VT_PTR)
      return 1;
  return 0;
}

static void param_nonnull_or(Sym *dst, Sym *src)
{
  Sym *d = sym_first(dst), *s = sym_first(src);
  for (; d && s; d = d->next, s = s->next)
    if (s->a.nonnull && (d->type.t & VT_BTYPE) == VT_PTR)
      d->a.nonnull = 1;
}

/* nonnull on any declaration counts, as with GCC: each function type only
 * this function holds takes the other declaration's marks.  Before the
 * types are patched: a definition's type replaces the prototype's. */
static void param_nonnull_merge(Sym *old_ref, Sym *new_ref)
{
  if (!old_ref || !new_ref || old_ref == new_ref)
    return;
  if (new_ref == patch_own_functype && param_nonnull_missing(new_ref, old_ref))
    param_nonnull_or(new_ref, old_ref);
  if (old_ref->a.proto_own && param_nonnull_missing(old_ref, new_ref))
    param_nonnull_or(old_ref, new_ref);
}

/* yasc: how one pointer level (or int handle) of two declarations differs
   in what it says -- NULL when it does not, or when either says nothing:
   written in regular C (a header's prototype before the yasc one), or a
   plain pointer in yasc, an error of its own (`T *restrict p` too) */
static const char *yasc_level_diff(CType *a, CType *b)
{
  const int words = YASC_OWNED | YASC_VIEW | YASC_MAYBE | YASC_OUT | YASC_DROP | YASC_TAKEN | YASC_RESTRICT;
  int qa = type_yasc(a), qb = type_yasc(b), d;
  if (!(qa & (YASC_OWNED | YASC_VIEW)) || !(qb & (YASC_OWNED | YASC_VIEW)) || ((qa | qb) & YASC_UNCHECKED))
    return NULL;
  d = (qa ^ qb) & words;
  if (d & (YASC_OWNED | YASC_VIEW))
    return "owned / view";
  if (d & YASC_MAYBE)
    return "maybe";
  if (d & YASC_OUT)
    return "out";
  /* in (the default: a const target) or inout */
  if ((qa & YASC_VIEW) && (a->t & (VT_BTYPE | VT_ARRAY)) == VT_PTR && (b->t & (VT_BTYPE | VT_ARRAY)) == VT_PTR &&
      ((a->ref->type.t ^ b->ref->type.t) & VT_CONSTANT))
    return "in / inout (whether its target is written)";
  if (d & YASC_DROP)
    return "drop";
  if (d & YASC_TAKEN)
    return "takes(...)";
  if (d & YASC_RESTRICT)
    return "restrict";
  /* where a view points: borrows(...), the elision, static */
  if ((qa & YASC_VIEW) && type_borrows(a) != type_borrows(b))
    return "borrows(...)";
  return NULL;
}

/* ... at every pointer level of a declared type */
static const char *yasc_contract_diff(CType *a, CType *b)
{
  for (int depth = 0; depth < 8; depth++)
  {
    const char *w = yasc_level_diff(a, b);
    if (w)
      return w;
    if ((a->t & (VT_BTYPE | VT_ARRAY)) != VT_PTR || (b->t & (VT_BTYPE | VT_ARRAY)) != VT_PTR || !a->ref || !b->ref)
      return NULL;
    a = &a->ref->type, b = &b->ref->type;
  }
  return NULL;
}

/* A function declared again (or defined) in yasc says what it does with its
   pointers as the declaration before it did: callers that saw only that one
   are checked against it.  C's compatibility leaves the yasc words out. */
static void yasc_redecl_check(Sym *sym, CType *type)
{
  Sym *o = sym->type.ref, *n = type->ref, *po, *pn;
  const char *w, *name = get_tok_str(sym->v, NULL);
  int i = 1;
  if (!o || !n || o == n)
    return;
  if ((w = yasc_contract_diff(&o->type, &n->type)))
    tcc_error("'%s' is declared again with another yasc contract: the result differs in %s", name, w);
  for (po = sym_first(o), pn = sym_first(n); po && pn; po = po->next, pn = pn->next, i++)
    if ((w = yasc_contract_diff(&po->type, &pn->type)))
    {
      int v = pn->v & ~SYM_FIELD;
      if (v >= TOK_UIDENT)
        tcc_error("'%s' is declared again with another yasc contract: parameter %d ('%s') differs in %s", name, i,
                  get_tok_str(v, NULL), w);
      tcc_error("'%s' is declared again with another yasc contract: parameter %d differs in %s", name, i, w);
    }
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
    /* An asm label that C now declares a function: asm already names it,
     * so its body keeps the AAPCS convention (gfunc_regpair_ret). */
    if ((type->t & VT_BTYPE) == VT_FUNC)
      sym->a.regpair_ret = REGPAIR_RET_NO;
    /* stay static if both are static */
    sym->type.t = type->t & (sym->type.t | ~VT_STATIC);
    sym->type.ref = type->ref;
    if ((type->t & VT_BTYPE) != VT_FUNC && !(type->t & VT_ARRAY))
      sym->r |= VT_LVAL;
  }

  Sym *capi_old = NULL, *capi_new = NULL, *yapi_old = NULL, *yapi_new = NULL;
  /* first: a yasc difference also makes the C types differ (in / out) */
  if ((sym->type.t & VT_BTYPE) == VT_FUNC && (type->t & VT_BTYPE) == VT_FUNC)
  {
    yasc_redecl_check(sym, type);
    capi_old = yasc_capi_of(sym->type.ref);
    capi_new = yasc_capi_of(type->ref);
    yapi_old = yasc_capi_yasc_type(sym->type.ref);
    yapi_new = yasc_capi_yasc_type(type->ref);
  }
  if (!is_compatible_types(&sym->type, type))
  {
    /* a regular-C declaration of a function whose yasc one says c_api(...):
       the C type it says (decl/c_api.c) */
    if ((capi_old || capi_new) && (type->t & VT_BTYPE) == VT_FUNC && yasc_capi_redecl(sym, type))
      return;
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

    Sym *old_ref = sym->type.ref;
    param_nonnull_merge(old_ref, type->ref);
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
      /* the prototype's type is a typedef's (shared): a nonnull this
       * declaration adds needs the type its declarator built */
      if (!old_ref->a.proto_own && type->ref == patch_own_functype && old_ref->f.func_type != FUNC_OLD &&
          type->ref->f.func_type != FUNC_OLD && param_nonnull_missing(old_ref, type->ref))
      {
        merge_funcattr(&type->ref->f, &old_ref->f);
        sym->type.ref = type->ref;
      }
    }

    /* A definition replacing its own prototype's type: the prototype's type
     * Sym takes the definition's contents and the definition's goes, so no
     * orphaned type Sym per defined function stays on global_stack (4.8K of
     * them in the Zig kernel.c compile).  Not K&R: knr_proto keeps old_ref. */
    if (!local_scope && !local_stack && sym->type.ref == type->ref && old_ref && old_ref != type->ref &&
        old_ref->a.proto_own && old_ref->f.func_type != FUNC_OLD && type->ref->f.func_type != FUNC_OLD &&
        sym_functype_adopt(old_ref, type->ref))
    {
      sym->type.ref = old_ref;
      type->ref = old_ref;
    }

    /* An old-style definition met by a prototype, in either order: the
     * symbol keeps the definition's parameters -- its body (and an inline
     * replay of it) binds their names and declared types -- and calls take
     * the prototype (knr_call_type). */
    if (!local_scope && sym->type.ref->f.func_type == FUNC_OLD && sym_first(sym->type.ref) &&
        old_ref->f.func_type != FUNC_OLD && sym->type.ref != old_ref)
    {
      merge_funcattr(&old_ref->f, &sym->type.ref->f);
      sym_facts(sym)->knr_proto = old_ref;
    }
    else if (sym->type.ref->f.func_type == FUNC_OLD && type->ref->f.func_type != FUNC_OLD && !local_scope)
    {
      if (sym_first(sym->type.ref))
      {
        merge_funcattr(&sym->type.ref->f, &type->ref->f);
        merge_funcattr(&type->ref->f, &sym->type.ref->f);
        sym_facts(sym)->knr_proto = type->ref;
      }
      else
        sym->type.ref = type->ref;
    }
    if (capi_old || capi_new || yapi_old || yapi_new)
      yasc_capi_merge(sym, capi_old, capi_new, yapi_old, yapi_new);
  }
  else
  {
    if ((sym->type.t & VT_ARRAY) && type->ref->c >= 0)
    {
      /* Set the size omitted in the extern declaration on a ref of its own:
       * the old one may be a typedef's.  Global stack, as sym outlives any scope. */
      Sym *own = sym_push2(&global_stack, SYM_FIELD, sym->type.ref->type.t, type->ref->c);
      own->type.ref = sym->type.ref->type.ref;
      sym->type.ref = own;
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
  gfunc_regpair_check_redecl(sym);
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
  s = sym_malloc(), *s = *s0; /* facts are keyed by Sym: s0 keeps them */
  s->prev = *ps, *ps = s;
  if (s->v < SYM_FIRST_ANOM)
  {
    if (SYM_IS_PARKED(table_ident[s->v - TOK_IDENT]->sym_identifier))
      sym_unpark(table_ident[s->v - TOK_IDENT]);
    ps = &table_ident[s->v - TOK_IDENT]->sym_identifier;
    s->prev_tok = *ps, *ps = s;
  }
  return s;
}

/* copy s->type.ref to stack 'ps' for VT_FUNC and VT_PTR */
void sym_copy_ref(Sym *s, Sym **ps)
{
  int bt = s->type.t & VT_BTYPE;
  /* A struct is copied only when its type is local: a local of a file-scope
   * struct type left the struct Sym itself in the chain, and the copies of its
   * members replaced the file-scope struct's own list (a spilled one with the
   * spill sentinel copied as a member). */
  if (bt == VT_FUNC || bt == VT_PTR || (bt == VT_STRUCT && s->type.ref && s->type.ref->sym_scope))
  {
    /* For VLA array types, the SYM_FIELD's next/vla_array_str union may
       contain a token stream pointer (set in post_type for TYPE_NEST),
       not a valid Sym* chain.  Don't follow next in that case. */
    int is_vla = s->type.t & VT_VLA;
    Sym **sp = &s->type.ref;
    if (bt == VT_FUNC && *sp)
      sym_first(*sp); /* copied whole below: never with a spilled list */
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

  /* a global variable a system header declares (errno, stdout): what a
     libc call may still write (-Wlifetime) */
  if ((type->t & VT_BTYPE) != VT_FUNC && v < SYM_FIRST_ANOM)
  {
    BufferedFile *f = file;
    while (f && f->filename[0] == ':' && f->prev)
      f = f->prev;
    if (f && f->sys)
    {
      TCCState *s1 = tcc_state;
      if ((v >> 3) >= s1->sys_globals_size)
      {
        int n = (v >> 3) + 256;
        s1->sys_globals = tcc_realloc(s1->sys_globals, n);
        memset(s1->sys_globals + s1->sys_globals_size, 0, n - s1->sys_globals_size);
        s1->sys_globals_size = n;
      }
      s1->sys_globals[v >> 3] |= 1 << (v & 7);
    }
  }
  if (!s)
  {
    /* push forward reference */
    s = global_identifier_push(v, type->t, 0);
    s->r |= r;
    s->a = ad->a;
    /* pure/const are NOT set on s->f: a global symbol's f shares its word
     * with sym_scope, and the lookup above takes a non-zero sym_scope for a
     * local -- so the next reference pushed a second, undefined global (a
     * static pure function called through a separate `U f`). The declaration
     * merges them into type.ref->f, which is where the readers look. */
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

/* A block-scope `extern int g;` is a Sym of its own (external_sym copies the
 * file-scope one onto local_stack) that is freed at the end of the block, yet
 * names the same object.  The optimizers decide "same global" by comparing Sym
 * pointers, so an IR symref always carries the outermost Sym of the ELF
 * symbol: the one the file-scope declaration made, which lives for the whole
 * TU.  Installed as tcc_ir_sym_canonicalizer by tccgen_init. */
Sym *canonical_global_sym(Sym *sym)
{
  if (!sym || sym->c <= 0 || sym->v < TOK_IDENT || sym->v >= SYM_FIRST_ANOM)
    return sym;
  TokenSym *ts = table_ident[sym->v - TOK_IDENT];
  if (!ts || SYM_IS_PARKED(ts->sym_identifier))
    return sym;
  Sym *best = sym;
  for (Sym *p = ts->sym_identifier; p; p = p->prev_tok)
    if (p->c == sym->c && p->sym_scope == 0 && p->type.t == sym->type.t)
      best = p;
  return best;
}

static int resolve_alias_symbol(Sym *alias_sym, int target_tok, int report_error)
{
  Sym *target_sym;
  ElfSym *esym;

  target_sym = find_global_alias_target_sym(target_tok);
  if (target_sym == alias_sym)
    tcc_error("'%s' is part of alias cycle", get_tok_str(alias_sym->v, NULL));
  /* An alias reaches an r0/r1 function as a pointer would: by its adapter. */
  target_sym = gfunc_regpair_address_sym(target_sym);
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
  /* Its callers must use the convention of the body it names.  A function
   * already r0/r1 has a body of its own: like gcc, defining it both normally
   * and as an alias is an error. */
  if (alias_sym->a.regpair_ret == REGPAIR_RET_YES)
    tcc_error("'%s' defined both normally and as 'alias' attribute", get_tok_str(alias_sym->v, NULL));
  if (alias_sym->a.regpair_ret == REGPAIR_RET_UNKNOWN)
    alias_sym->a.regpair_ret = REGPAIR_RET_NO;
  unused_alias_target(target_tok); /* named: not unused */
  if (!resolve_alias_symbol(alias_sym, target_tok, 0))
    queue_alias_symbol(alias_sym, target_tok);
}

/* An alias target is emitted whether or not anything else refers to it
 * (prune_unused_statics runs before the pending aliases are resolved). */
void mark_pending_alias_targets_used(void)
{
  for (int i = 0; i < nb_pending_aliases; i++)
  {
    Sym *target = find_global_alias_target_sym(pending_aliases[i].target_tok);
    if (target)
      target->a.used = 1;
  }
}

/* The adapters the pending aliases of r0/r1 functions resolve to: made now,
 * and kept, so gfunc_regpair_emit_adapters emits them before the aliases are
 * resolved. */
void regpair_adapt_pending_aliases(void)
{
  for (int i = 0; i < nb_pending_aliases; i++)
  {
    Sym *target = find_global_alias_target_sym(pending_aliases[i].target_tok);
    Sym *adapter = gfunc_regpair_address_sym(target);
    if (adapter && adapter != target)
      adapter->a.used = 1;
  }
}

/* Whether a not yet resolved alias names SYM.  Such a symbol must get a body
 * of its own: nothing but the alias will ever refer to it. */
int is_pending_alias_target(Sym *sym)
{
  for (int i = 0; i < nb_pending_aliases; i++)
    if (find_global_alias_target_sym(pending_aliases[i].target_tok) == sym)
      return 1;
  return 0;
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
