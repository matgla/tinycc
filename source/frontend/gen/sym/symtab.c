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

/* symtab.c -- Symbol allocator, symbol/label stacks and scope lookup.
 * Split out of tccgen.c; see docs/plan_tccgen_split.md. */

#include "gen_priv.h"

/* ------------------------------------------------------------------------- */
/* symbol allocator */
static Sym *__sym_malloc(void)
{
  Sym *sym_pool, *sym, *last_sym;
  int i;

  sym_pool = tcc_malloc(SYM_POOL_NB * sizeof(Sym));
  dynarray_add(&sym_pools, &nb_sym_pools, sym_pool);

  last_sym = sym_free_first;
  sym = sym_pool;
  for (i = 0; i < SYM_POOL_NB; i++)
  {
    sym->next = last_sym;
    last_sym = sym;
    sym++;
  }
  sym_free_first = last_sym;
  return last_sym;
}

Sym *sym_malloc(void)
{
  Sym *sym;
#ifndef SYM_DEBUG
  sym = sym_free_first;
  if (!sym)
    sym = __sym_malloc();
  sym_free_first = sym->next;
  return sym;
#else
  sym = tcc_malloc(sizeof(Sym));
  return sym;
#endif
}

static const SymLocalFacts sym_no_facts;

/* The facts map: Sym -> SymLocalFacts, open-addressed with linear probing,
 * at most half full.  Only the few locals that record facts have an entry,
 * so the common lookup is the facts_nb == 0 test. */
typedef struct FactsSlot
{
  const Sym *s; /* NULL = empty */
  SymLocalFacts *f;
} FactsSlot;

static FactsSlot *facts_map;
static int facts_nb, facts_shift = 32; /* capacity = 1 << (32 - facts_shift) */

static unsigned facts_hash(const Sym *s)
{
  /* Fibonacci hashing: the top bits of the product mix every address bit. */
  return (unsigned)((uintptr_t)s >> 2) * 2654435761u >> facts_shift;
}

static FactsSlot *facts_find(const Sym *s)
{
  unsigned i, mask;
  if (!facts_nb)
    return NULL;
  mask = (1u << (32 - facts_shift)) - 1;
  for (i = facts_hash(s);; i = (i + 1) & mask)
  {
    if (facts_map[i].s == s)
      return &facts_map[i];
    if (!facts_map[i].s)
      return NULL;
  }
}

static void facts_insert(const Sym *s, SymLocalFacts *f)
{
  unsigned i, mask = (1u << (32 - facts_shift)) - 1;
  for (i = facts_hash(s); facts_map[i].s; i = (i + 1) & mask)
    ;
  facts_map[i].s = s;
  facts_map[i].f = f;
  facts_nb++;
}

SymLocalFacts *sym_facts_peek(const Sym *s)
{
  FactsSlot *e = facts_find(s);
  return e ? e->f : NULL;
}

const SymLocalFacts *sym_facts_get(const Sym *s)
{
  FactsSlot *e = facts_find(s);
  return e ? e->f : &sym_no_facts;
}

SymLocalFacts *sym_facts(Sym *s)
{
  FactsSlot *e = facts_find(s);
  SymLocalFacts *f;
  if (e)
    return e->f;
  if (2 * (facts_nb + 1) > (1 << (32 - facts_shift)))
  {
    FactsSlot *old = facts_map;
    int i, n = facts_map ? 1 << (32 - facts_shift) : 0;
    facts_shift = facts_map ? facts_shift - 1 : 32 - 4;
    facts_map = tcc_mallocz(sizeof *facts_map << (32 - facts_shift));
    facts_nb = 0;
    for (i = 0; i < n; i++)
      if (old[i].s)
        facts_insert(old[i].s, old[i].f);
    tcc_free(old);
  }
  f = tcc_mallocz(sizeof(SymLocalFacts));
  facts_insert(s, f);
  return f;
}

void sym_free_facts(Sym *s)
{
  FactsSlot *e = facts_find(s);
  unsigned i, j, k, mask;
  if (!e)
    return;
  tcc_free(e->f->const_init_data);
  tcc_free(e->f);
  facts_nb--;
  /* Backward-shift deletion: pull later members of the probe run into the
     hole so lookups never need tombstones. */
  mask = (1u << (32 - facts_shift)) - 1;
  i = e - facts_map;
  for (j = i;;)
  {
    facts_map[i].s = NULL;
    for (;;)
    {
      j = (j + 1) & mask;
      if (!facts_map[j].s)
        return;
      k = facts_hash(facts_map[j].s);
      /* stays when its home k lies cyclically in (i, j] */
      if (i <= j ? (i < k && k <= j) : (i < k || k <= j))
        continue;
      break;
    }
    facts_map[i] = facts_map[j];
    i = j;
  }
}

/* End of TU: whatever facts are left go with the Syms. */
void sym_facts_free_all(void)
{
  int i, n = facts_map ? 1 << (32 - facts_shift) : 0;
  for (i = 0; i < n; i++)
    if (facts_map[i].s)
    {
      tcc_free(facts_map[i].f->const_init_data);
      tcc_free(facts_map[i].f);
    }
  tcc_free(facts_map);
  facts_map = NULL;
  facts_nb = 0;
  facts_shift = 32;
}

ST_INLN void sym_free(Sym *sym)
{
  sym_free_facts(sym);
#ifndef SYM_DEBUG
  /* Poison freed symbols to detect use-after-free */
  sym->v = 0xDEADBEEF;
  sym->next = sym_free_first;
  sym_free_first = sym;
#else
  tcc_free(sym);
#endif
}

/* Short Syms: only the fields up to prev (SYM_SHORT_SIZE), for macros, their
 * parameters and the expander's nesting list, which use nothing else.  Their
 * own pools and free list; a short Sym is freed with sym_free_short only. */
static Sym *sym_short_free_first;
static void **sym_short_pools;
static int nb_sym_short_pools;

ST_FUNC Sym *sym_push2_short(Sym **ps, int v, int t, int c)
{
  Sym *s;
#ifndef SYM_DEBUG
  if (!sym_short_free_first)
  {
    char *pool = tcc_malloc(SYM_POOL_NB * SYM_SHORT_SIZE);
    int i;
    dynarray_add(&sym_short_pools, &nb_sym_short_pools, pool);
    for (i = 0; i < SYM_POOL_NB; i++)
    {
      s = (Sym *)(pool + i * SYM_SHORT_SIZE);
      s->next = sym_short_free_first;
      sym_short_free_first = s;
    }
  }
  s = sym_short_free_first;
  sym_short_free_first = s->next;
#else
  s = tcc_malloc(SYM_SHORT_SIZE);
#endif
  memset(s, 0, SYM_SHORT_SIZE);
  s->v = v;
  s->type.t = t;
  s->c = c;
  s->prev = *ps;
  *ps = s;
  return s;
}

ST_FUNC void sym_free_short(Sym *s)
{
#ifndef SYM_DEBUG
  s->v = 0xDEADBEEF;
  s->next = sym_short_free_first;
  sym_short_free_first = s;
#else
  tcc_free(s);
#endif
}

/* End of TU, after free_defines. */
ST_FUNC void sym_short_pools_free(void)
{
  dynarray_reset(&sym_short_pools, &nb_sym_short_pools);
  sym_short_free_first = NULL;
}

/* push, without hashing */
ST_FUNC Sym *sym_push2(Sym **ps, int v, int t, int c)
{
  Sym *s;

  s = sym_malloc();
  memset(s, 0, sizeof *s);
  s->v = v;
  s->type.t = t;
  s->c = c;
  /* add in stack */
  s->prev = *ps;
  *ps = s;
  return s;
}

/* find a symbol and return its associated structure. 's' is the top
   of the symbol stack */
ST_FUNC Sym *sym_find2(Sym *s, int v)
{
  while (s)
  {
    if (s->v == v)
      return s;
    s = s->prev;
  }
  return NULL;
}

/* Struct tag and label bindings by token.  A TU interns thousands of
 * identifiers but only tens of them are ever a tag or a label, so the head of
 * each such binding chain lives in a small open-addressed map rather than in
 * two pointers of every TokenSym (8 bytes of each on ARM).  Entries are never
 * removed: an unbound name keeps its entry with a NULL head. */
typedef struct TokHead
{
  int tok; /* 0 = empty slot; tokens start at TOK_IDENT */
  Sym *head;
} TokHead;

typedef struct TokHeadMap
{
  TokHead *e;
  int nb, mask;
} TokHeadMap;

static TokHeadMap tag_heads, label_heads;

static TokHead *tok_head_slot(TokHeadMap *m, int tok, int create)
{
  unsigned h;
  TokHead *e;

  if (!m->e)
  {
    if (!create)
      return NULL;
    m->mask = 15;
    m->e = tcc_mallocz((m->mask + 1) * sizeof *m->e);
  }
  for (h = (unsigned)tok * 2654435761u;; h++)
  {
    e = &m->e[h & m->mask];
    if (e->tok == tok)
      return e;
    if (!e->tok)
      break;
  }
  if (!create)
    return NULL;
  if (2 * (m->nb + 1) > m->mask + 1)
  {
    /* keep it at most half full: rehash into twice the slots */
    TokHead *old = m->e;
    int i, n = m->mask + 1;
    m->mask = 2 * n - 1;
    m->e = tcc_mallocz(2 * n * sizeof *m->e);
    m->nb = 0;
    for (i = 0; i < n; i++)
      if (old[i].tok)
        tok_head_slot(m, old[i].tok, 1)->head = old[i].head;
    tcc_free(old);
    return tok_head_slot(m, tok, 1);
  }
  m->nb++;
  e->tok = tok;
  return e;
}

static void tok_head_set(TokHeadMap *m, int tok, Sym *s)
{
  TokHead *e = tok_head_slot(m, tok, s != NULL);
  if (e)
    e->head = s;
}

ST_FUNC Sym *sym_tag_head(int v)
{
  TokHead *e = tok_head_slot(&tag_heads, v, 0);
  return e ? e->head : NULL;
}

ST_FUNC void sym_set_tag_head(int v, Sym *s)
{
  tok_head_set(&tag_heads, v, s);
}

ST_FUNC Sym *sym_label_head(int v)
{
  TokHead *e = tok_head_slot(&label_heads, v, 0);
  return e ? e->head : NULL;
}

ST_FUNC void sym_set_label_head(int v, Sym *s)
{
  tok_head_set(&label_heads, v, s);
}

/* Pointer type nodes, shared.  The Sym of a pointer type holds nothing but
 * the type pointed to (v = SYM_FIELD, c = -1), and a TU reads a `T *` at every
 * other declaration: stdio.c made 463 file-scope pointer nodes, 31 of them
 * distinct.  mk_pointer hands out one node per (t, ref) pair instead.  Nothing
 * writes such a node once it is made: the one pointer that is written -- the
 * one a nested declarator's suffix applies to, as in `int (*f)(int)` -- is
 * made privately (type_decl).  The nodes are on no symbol stack and live
 * until the end of the TU, as every pointer node did before (sym_pop keeps a
 * Sym whose c is not 0).  A key whose ref has been freed can only be met
 * again by a type that refers to a new Sym at that address, and the node
 * then means exactly that type. */
static Sym **ptr_nodes; /* open-addressed, at most half full */
static int ptr_nodes_nb, ptr_nodes_mask;

static unsigned ptr_node_hash(int t, const Sym *ref)
{
  return ((unsigned)t ^ (unsigned)((uintptr_t)ref >> 2) * 2654435761u) * 2654435761u;
}

static Sym **ptr_node_slot(int t, const Sym *ref)
{
  unsigned h;
  Sym **e;
  for (h = ptr_node_hash(t, ref);; h++)
  {
    e = &ptr_nodes[h & ptr_nodes_mask];
    if (!*e || ((*e)->type.t == t && (*e)->type.ref == ref))
      return e;
  }
}

ST_FUNC Sym *sym_pointer_node(CType *type)
{
  Sym *s, **e = ptr_nodes ? ptr_node_slot(type->t, type->ref) : NULL;

  if (e && *e)
    return *e;
  if (2 * (ptr_nodes_nb + 1) > ptr_nodes_mask + 1)
  {
    Sym **old = ptr_nodes;
    int i, n = old ? ptr_nodes_mask + 1 : 0;
    ptr_nodes_mask = n ? 2 * n - 1 : 31;
    ptr_nodes = tcc_mallocz((ptr_nodes_mask + 1) * sizeof *ptr_nodes);
    for (i = 0; i < n; i++)
      if (old[i])
        *ptr_node_slot(old[i]->type.t, old[i]->type.ref) = old[i];
    tcc_free(old);
    e = ptr_node_slot(type->t, type->ref);
  }
  /* what sym_push(SYM_FIELD, type, 0, -1) makes, without the stack */
  s = sym_malloc();
  memset(s, 0, sizeof *s);
  s->v = SYM_FIELD;
  s->type = *type;
  s->c = -1;
  s->vreg = -1;
  *e = s;
  ptr_nodes_nb++;
  return s;
}

/* End of TU, before the Sym pools go. */
ST_FUNC void sym_pointer_nodes_free(void)
{
  int i, n = ptr_nodes ? ptr_nodes_mask + 1 : 0;
  for (i = 0; i < n; i++)
    if (ptr_nodes[i])
      sym_free(ptr_nodes[i]);
  tcc_free(ptr_nodes);
  ptr_nodes = NULL;
  ptr_nodes_nb = ptr_nodes_mask = 0;
}

static int sym_is_pointer_node(Sym *s)
{
  return s->v == SYM_FIELD && ptr_nodes && *ptr_node_slot(s->type.t, s->type.ref) == s;
}

/* Parked prototypes.  System headers declare far more functions than a TU
 * calls: stdio.c's headers declare 250, 166 of them never referenced, and
 * each costs an extern Sym, a function type Sym and a Sym per parameter --
 * a quarter of the TU's symbol table.  So a plain file-scope prototype (an
 * extern function, no asm label, its return and parameter types built from
 * basic types, tags and shared pointer nodes) is parked when it is first
 * declared: what its Syms hold goes into a ParkedProto, the Syms are freed,
 * and its name's sym_identifier holds the record, tagged in bit 0.  Every
 * reader of sym_identifier (sym_find, sym_push, global_identifier_push and
 * the few direct ones) builds the Syms back first, exactly as the
 * declaration made them, at the bottom of global_stack: below any parameter
 * list that may be open there, as if declared at the start of the TU (the
 * place does not matter otherwise: nothing walks global_stack for a function
 * that has no ELF symbol yet).  The types stay valid while parked: tags are
 * kept on global_stack to the end of the TU and pointer nodes live as long. */
#define PARK_VOLATILE 0x80000000 /* in ParkedParam.v: a.param_volatile */
#define PARK_SCOPED 0x40000000   /* in ParkedParam.v: sym_scope is 1 */

typedef struct ParkedParam
{
  int v; /* name token, 0 if unnamed; PARK_* flags */
  CType type;
} ParkedParam;

typedef struct ParkedProto
{
  int t;            /* the extern's type.t */
  struct SymAttr a; /* the extern's attributes */
  CType ret;        /* the function type Sym: return type, attributes */
  struct SymAttr fa;
  struct FuncAttr f;
  int nb_params;
  ParkedParam params[];
} ParkedProto;

static Sym *gs_bottom; /* the bottom of global_stack, once looked for */

static int park_type_ok(const CType *t)
{
  while (t->ref)
  {
    if ((t->t & (VT_BTYPE | VT_ARRAY | VT_VLA)) == VT_PTR && sym_is_pointer_node(t->ref))
      t = &t->ref->type;
    else
      return (t->ref->v & SYM_STRUCT) != 0; /* struct, union or enum tag */
  }
  return 1;
}

static int park_attr_zero(struct SymAttr a, int param)
{
  static const struct SymAttr zero;
  if (param)
    a.param_volatile = 0;
  return !memcmp(&a, &zero, sizeof a);
}

/* s is on the stack between top and bottom (exclusive) */
static int park_in(Sym *s, Sym *top, Sym *bottom)
{
  for (; top != bottom; top = top->prev)
    if (top == s)
      return 1;
  return 0;
}

static int park_is_param(Sym *s, Sym *f)
{
  for (f = f->next; f; f = f->next)
    if (f == s)
      return 1;
  return 0;
}

/* An open-addressed set of the Syms a prototype parks (its function type Sym
 * and its parameters), so that "is it on this declaration's stack segment"
 * and "is it one of ours" cost one probe per segment entry instead of a walk
 * of the segment per parameter and of the parameter list per segment entry --
 * quadratic in the parameter count (limits-fnargs.c declares 10000). */
static unsigned park_hash(Sym *p, unsigned mask)
{
  return (unsigned)(((uintptr_t)p >> 4) * 2654435761u) & mask;
}

static int park_set_has(Sym **set, unsigned mask, Sym *p)
{
  unsigned h;
  for (h = park_hash(p, mask); set[h]; h = (h + 1) & mask)
    if (set[h] == p)
      return 1;
  return 0;
}

ST_FUNC void sym_park_proto(Sym *s, Sym *stack_before)
{
  Sym *f = s->type.ref, *p, **pp, *next;
  TokenSym *ts;
  ParkedProto *rec;
  int n = 0, i, have_s = 0;
  Sym *small[64], **set = NULL;
  unsigned mask = 15, found = 0;

  /* Only what sym_unpark rebuilds exactly, made by this declaration. */
  if (local_stack || tcc_state->ir || s->v < TOK_IDENT || s->v >= tok_ident)
    return;
  ts = table_ident[s->v - TOK_IDENT];
  if (!ts || ts->sym_identifier != s || s->prev_tok || (s->type.t & VT_BTYPE) != VT_FUNC ||
      (s->type.t & (VT_STATIC | VT_INLINE | VT_TYPEDEF)) || s->r != (VT_CONST | VT_SYM) || s->c || s->sym_scope ||
      s->vreg || s->asm_label)
    return;
  if (f->v != SYM_FIELD || f->r || f->c || f->vreg != -1 || f->prev_tok || !park_type_ok(&f->type))
    return;
  for (p = f->next; p; p = p->next, n++)
    if (!(p->v & SYM_FIELD) || p->r != (VT_LOCAL | VT_LVAL) || p->c || (unsigned)p->sym_scope > 1 ||
        p->vreg != -1 || !park_attr_zero(p->a, 1) || !park_type_ok(&p->type))
      return;

  /* s, f and every parameter must lie on global_stack above stack_before.
   * A short prototype just walks the segment for each; past that, they are
   * n + 2 distinct Syms and the segment holds each Sym once, so it holds iff
   * the segment holds s and n + 1 members of the set {f, params}. */
  if (n < 8)
  {
    for (p = f; p; p = p->next)
      if (!park_in(p, global_stack, stack_before))
        return;
    if (!park_in(s, global_stack, stack_before))
      return;
  }
  else
  {
    while (mask + 1 < 2u * (unsigned)(n + 1))
      mask = mask * 2 + 1;
    set = mask < sizeof small / sizeof small[0] ? small : tcc_malloc((mask + 1) * sizeof *set);
    memset(set, 0, (mask + 1) * sizeof *set);
    for (p = f; p; p = p->next)
    {
      unsigned h = park_hash(p, mask);
      while (set[h])
        h = (h + 1) & mask;
      set[h] = p;
    }
    for (p = global_stack; p != stack_before; p = p->prev)
      if (p == s)
        have_s = 1;
      else if (park_set_has(set, mask, p))
        found++;
    if (!have_s || found != (unsigned)n + 1)
    {
      if (set != small)
        tcc_free(set);
      return;
    }
  }

  rec = tcc_malloc(sizeof *rec + n * sizeof rec->params[0]);
  rec->t = s->type.t;
  rec->a = s->a;
  rec->ret = f->type;
  rec->fa = f->a;
  rec->f = f->f;
  rec->nb_params = n;
  for (p = f->next, i = 0; p; p = p->next, i++)
  {
    rec->params[i].v = (p->v & ~SYM_FIELD) | (p->a.param_volatile ? PARK_VOLATILE : 0) | (p->sym_scope ? PARK_SCOPED : 0);
    rec->params[i].type = p->type;
  }

#ifdef TCC_PARK_VERIFY
  Sym *orig = tcc_malloc((n + 2) * sizeof(Sym));
  orig[0] = *s, orig[1] = *f;
  for (p = f->next, i = 2; p; p = p->next, i++)
    orig[i] = *p;
#endif
  /* Unlink them from global_stack, leaving whatever else is there. */
  for (pp = &global_stack; *pp != stack_before;)
  {
    p = *pp;
    if (p == s || (set ? park_set_has(set, mask, p) : p == f || park_is_param(p, f)))
    {
      *pp = p->prev;
      if (p == gs_bottom)
        gs_bottom = NULL;
    }
    else
      pp = &p->prev;
  }
  if (set && set != small)
    tcc_free(set);
  for (p = f->next; p; p = next)
  {
    next = p->next;
    sym_free(p);
  }
  sym_free(f);
  sym_free(s);
  ts->sym_identifier = (Sym *)((uintptr_t)rec | 1);
#ifdef TCC_PARK_VERIFY
  /* build it straight back and compare everything but the stack links */
  {
    Sym *u[2], *o, *q;
    u[0] = sym_unpark(ts), u[1] = u[0]->type.ref;
    for (i = 0; i < n + 2; i++)
    {
      o = &orig[i];
      q = i < 2 ? u[i] : NULL;
      if (i >= 2)
      {
        int k;
        for (q = u[1]->next, k = 2; k < i; k++)
          q = q->next;
      }
      if (q->v != o->v || q->r != o->r || memcmp(&q->a, &o->a, sizeof q->a) || q->vreg != o->vreg ||
          memcmp(&q->c, &o->c, 2 * sizeof(int)) || q->type.t != o->type.t || q->type.ref != o->type.ref ||
          (i < 2 && q->prev_tok != o->prev_tok) || (i == 0 && q->next != o->next) ||
          (i >= 2 && !q->next != !o->next))
      {
        fprintf(stderr, "PARK MISMATCH %s sym %d\n", get_tok_str(ts->tok, NULL), i);
        abort();
      }
    }
    if (u[1]->next ? !orig[1].next : !!orig[1].next)
      abort();
    tcc_free(orig);
    fprintf(stderr, "PARK OK %s\n", get_tok_str(ts->tok, NULL));
  }
#endif
}

ST_FUNC Sym *sym_unpark(TokenSym *ts)
{
  ParkedProto *rec = (ParkedProto *)((uintptr_t)ts->sym_identifier & ~(uintptr_t)1);
  Sym *s, *f, *p, *bottom = NULL, *below = NULL, **plast;
  int i;

  /* what post_type and external_sym made, bottom up */
  f = sym_malloc();
  memset(f, 0, sizeof *f);
  plast = &f->next;
  for (i = 0; i < rec->nb_params; i++)
  {
    ParkedParam *pr = &rec->params[i];
    p = sym_malloc();
    memset(p, 0, sizeof *p);
    p->v = (pr->v & ~(PARK_VOLATILE | PARK_SCOPED)) | SYM_FIELD;
    p->type = pr->type;
    p->r = VT_LOCAL | VT_LVAL;
    p->vreg = -1;
    p->a.param_volatile = (pr->v & PARK_VOLATILE) != 0;
    p->sym_scope = (pr->v & PARK_SCOPED) != 0;
    p->prev = below;
    if (!bottom)
      bottom = p;
    below = p;
    *plast = p;
    plast = &p->next;
  }
  f->v = SYM_FIELD;
  f->type = rec->ret;
  f->vreg = -1;
  f->a = rec->fa;
  f->f = rec->f;
  f->prev = below;
  if (!bottom)
    bottom = f;

  s = sym_malloc();
  memset(s, 0, sizeof *s);
  s->v = ts->tok;
  s->type.t = rec->t;
  s->type.ref = f;
  s->r = VT_CONST | VT_SYM;
  s->a = rec->a;
  s->prev = f;
  tcc_free(rec);

  /* under everything on global_stack */
  if (!global_stack)
    global_stack = s;
  else
  {
    if (!gs_bottom)
      gs_bottom = global_stack;
    while (gs_bottom->prev)
      gs_bottom = gs_bottom->prev;
    gs_bottom->prev = s;
  }
  bottom->prev = NULL;
  gs_bottom = bottom;
  ts->sym_identifier = s;
  return s;
}

/* End of TU: drop the records of the prototypes nothing named. */
ST_FUNC void sym_parked_free_all(void)
{
  int i;
  for (i = 0; i < tok_ident - TOK_IDENT; i++)
    if (table_ident[i] && SYM_IS_PARKED(table_ident[i]->sym_identifier))
    {
      tcc_free((void *)((uintptr_t)table_ident[i]->sym_identifier & ~(uintptr_t)1));
      table_ident[i]->sym_identifier = NULL;
    }
  gs_bottom = NULL;
}

/* End of TU: the Syms the heads point to are about to go. */
ST_FUNC void sym_heads_free(void)
{
  tcc_free(tag_heads.e);
  tcc_free(label_heads.e);
  memset(&tag_heads, 0, sizeof tag_heads);
  memset(&label_heads, 0, sizeof label_heads);
}

/* structure lookup */
ST_INLN Sym *struct_find(int v)
{
  return sym_tag_head(v);
}

/* find an identifier */
ST_INLN Sym *sym_find(int v)
{
  TokenSym *ts;
  /* A builtin prototype whose name was interned exists from here on. */
  TCC_PREDEF_PROTOS_FLUSH();
  v -= TOK_IDENT;
  if ((unsigned)v >= (unsigned)(tok_ident - TOK_IDENT))
    return NULL;
  ts = table_ident[v]; /* NULL = lazy builtin, never declared as an identifier */
  if (!ts)
    /* ...unless it is a builtin prototype looked up by fixed id */
    return tcc_predef_protos_armed ? tccgen_predef_proto_find_unseen(v + TOK_IDENT) : NULL;
  if (SYM_IS_PARKED(ts->sym_identifier))
    return sym_unpark(ts);
  return ts->sym_identifier;
}

int sym_scope(Sym *s)
{
  int scope;
  if (IS_ENUM_VAL(s->type.t))
    scope = s->type.ref->sym_scope;
  else
    scope = s->sym_scope;
  return scope;
}

int token_stream_references_local_object(const int *p)
{
  while (1)
  {
    int t;
    int bt;
    CValue cv;
    Sym *s;

    tok_get(&t, &p, &cv);
    if (t == TOK_EOF || t == 0)
      break;
    if (t < TOK_IDENT)
      continue;

    s = sym_find(t);
    if (!s || !sym_scope(s))
      continue;

    bt = s->type.t & VT_BTYPE;
    if ((s->type.t & VT_TYPEDEF) || IS_ENUM_VAL(s->type.t) || bt == VT_FUNC)
      continue;

    return 1;
  }

  return 0;
}

/* push a given symbol on the symbol stack */
ST_FUNC Sym *sym_push(int v, CType *type, int r, int c)
{
  Sym *s, **ps;
  TokenSym *ts;
  int vreg = -1;
  /* register local variable at IR code generator, get Vreg number */
  int valmask = r & VT_VALMASK;

  if (r & VT_PARAM)
  {
    /* Create PARAM vreg for ALL parameters, including stack-passed ones */
    vreg = tcc_ir_get_vreg_param(tcc_state->ir);
    if (vreg >= 0)
    {
      IRLiveInterval *iv = tcc_ir_vreg_live_interval(tcc_state->ir, vreg);
      if (iv)
      {
        iv->is_volatile = (type->t & VT_VOLATILE) != 0;
        iv->is_struct = (type->t & VT_BTYPE) == VT_STRUCT;
      }
    }
    /* For stack-passed params (VT_LOCAL), c is the stack offset;
     * for register params, c is the parameter index */
    tcc_ir_assign_physical_register(tcc_state->ir, vreg, c, -1, -1);
    /* Store original parameter offset for prolog code generation */
    tcc_ir_set_original_offset(tcc_state->ir, vreg, c);
    /* Mark float/double parameters */
    if (is_float(type->t))
    {
      int is_double = (type->t & VT_BTYPE) == VT_DOUBLE || (type->t & VT_BTYPE) == VT_LDOUBLE;
      tcc_ir_set_float_type(tcc_state->ir, vreg, 1, is_double);
    }
    /* Mark complex parameters - needs register pairs */
    if (type->t & VT_COMPLEX)
      tcc_ir_vreg_type_set_complex(tcc_state->ir, vreg);
    /* Mark long long parameters */
    if ((type->t & VT_BTYPE) == VT_LLONG)
    {
      tcc_ir_set_llong_type(tcc_state->ir, vreg);
    }
  }
  else
  {
    if (((valmask == VT_LOCAL) || (valmask == VT_LLOCAL)) && (r & VT_LVAL) && ((type->t & VT_BTYPE) != VT_STRUCT) &&
        !(type->t & (VT_ARRAY | VT_VLA | VT_COMPLEX)))
    {
      vreg = tcc_ir_get_vreg_var(tcc_state->ir);
      /* Set the variable's stack offset so LEA operations can find it */
      if (vreg >= 0)
      {
        IRLiveInterval *iv = tcc_ir_vreg_live_interval(tcc_state->ir, vreg);
        tcc_ir_assign_physical_register(tcc_state->ir, vreg, c, -1, -1);
        tcc_ir_set_original_offset(tcc_state->ir, vreg, c);
        if (iv)
          iv->is_volatile = (type->t & VT_VOLATILE) != 0;
      }
      /* Mark float/double variables */
      if (is_float(type->t))
      {
        int is_double = (type->t & VT_BTYPE) == VT_DOUBLE || (type->t & VT_BTYPE) == VT_LDOUBLE;
        tcc_ir_set_float_type(tcc_state->ir, vreg, 1, is_double);
      }
      /* Mark complex variables - needs register pairs */
      if (type->t & VT_COMPLEX)
      {
        tcc_ir_vreg_type_set_complex(tcc_state->ir, vreg);
      }
      /* Mark long long variables */
      if ((type->t & VT_BTYPE) == VT_LLONG)
      {
        tcc_ir_set_llong_type(tcc_state->ir, vreg);
      }
    }
  }
  // }
  // r &= ~VT_PARAM;

  if (local_stack)
    ps = &local_stack;
  else
    ps = &global_stack;
  s = sym_push2(ps, v, type->t, c);
  s->type.ref = type->ref;
  s->r = r;
  s->vreg = vreg;
  /* don't record fields or anonymous symbols */
  /* XXX: simplify */
  if (!(v & SYM_FIELD) && (v & ~SYM_STRUCT) < SYM_FIRST_ANOM)
  {
    /* record symbol in token array (materialize a lazy builtin slot if the
       symbol's name is a builtin token referenced by fixed id) */
    ts = tok_ensure(v & ~SYM_STRUCT);
    if (v & SYM_STRUCT)
      ps = &tok_head_slot(&tag_heads, v & ~SYM_STRUCT, 1)->head;
    else
    {
      /* A builtin prototype of this name goes underneath, as if declared
         at the start of the translation unit. */
      TCC_PREDEF_PROTOS_FLUSH();
      if (SYM_IS_PARKED(ts->sym_identifier))
        sym_unpark(ts); /* it goes underneath too */
      ps = &ts->sym_identifier;
    }
    s->prev_tok = *ps;
    *ps = s;
    s->sym_scope = local_scope;
    if (s->prev_tok && sym_scope(s->prev_tok) == s->sym_scope)
      tcc_error("redeclaration of '%s'", get_tok_str(v & ~SYM_STRUCT, NULL));
  }
  return s;
}

/* push a global identifier */
ST_FUNC Sym *global_identifier_push(int v, int t, int c)
{
  Sym *s, **ps;
  s = sym_push2(&global_stack, v, t, c);
  s->r = VT_CONST | VT_SYM;
  /* don't record anonymous symbol */
  if (v < SYM_FIRST_ANOM)
  {
    TokenSym *ts = tok_ensure(v);
    TCC_PREDEF_PROTOS_FLUSH(); /* see sym_push */
    if (SYM_IS_PARKED(ts->sym_identifier))
      sym_unpark(ts);
    ps = &ts->sym_identifier;
    /* modify the top most local identifier, so that sym_identifier will
       point to 's' when popped; happens when called from inline asm */
    while (*ps != NULL && (*ps)->sym_scope)
      ps = &(*ps)->prev_tok;
    s->prev_tok = *ps;
    *ps = s;
  }
  return s;
}

/* pop symbols until top reaches 'b'.  If KEEP is non-zero don't really
   pop them yet from the list, but do remove them from the token array.  */
ST_FUNC void sym_pop(Sym **ptop, Sym *b, int keep)
{
  Sym *s, *ss;
  int v;

  s = *ptop;
  while (s != b)
  {
    ss = s->prev;
    v = s->v;
    /* remove symbol in token array */
    /* XXX: simplify */
    if (!(v & SYM_FIELD) && (v & ~SYM_STRUCT) < SYM_FIRST_ANOM)
    {
      if (v & SYM_STRUCT)
        sym_set_tag_head(v & ~SYM_STRUCT, s->prev_tok);
      else
        table_ident[v - TOK_IDENT]->sym_identifier = s->prev_tok;
    }
    /* Don't free symbols that have been exported to ELF (sym->c != 0)
       as they may still be referenced by IR instructions */
    /* In IR mode the backend may still need Sym pointers (notably for
     * VT_SYM address materialization and relocations). Block-scope extern
     * declarations create temporary Sym copies that can be referenced by IR
     * after the scope ends; freeing them here can lead to missing relocations
     * and loads/stores from address 0 at runtime.
     */
    if (!keep && s->c == 0 && !(tcc_state->ir && (s->r & VT_SYM)))
      sym_free(s);
    else if (!keep)
      /* The Sym outlives its scope (a local's c is its frame offset); its
       * facts do not, and nothing frees them later. */
      sym_free_facts(s);
    s = ss;
  }
  if (!keep)
    *ptop = b;
}

/* label lookup */
ST_FUNC Sym *label_find(int v)
{
  return sym_label_head(v);
}

ST_FUNC Sym *label_push(Sym **ptop, int v, int flags)
{
  Sym *s, **ps;
  s = sym_push2(ptop, v, VT_STATIC, 0);
  s->r = flags;
  s->jnext = -1; /* Initialize to -1 so we know if there's an actual forward goto */
  ps = &tok_head_slot(&label_heads, v, 1)->head;
  if (ptop == &global_label_stack)
  {
    /* modify the top most local identifier, so that
       sym_identifier will point to 's' when popped */
    while (*ps != NULL)
      ps = &(*ps)->prev_tok;
  }
  s->prev_tok = *ps;
  *ps = s;
  return s;
}

/* pop labels until element last is reached. Look if any labels are
   undefined. Define symbols if '&&label' was used. */
ST_FUNC void label_pop(Sym **ptop, Sym *slast, int keep)
{
  Sym *s, *s1;
  for (s = *ptop; s != slast; s = s1)
  {
    s1 = s->prev;
    int addr_taken =
        (s->c == -3 || s->c > 0 || s->a.addrtaken); /* Remember if address was taken before modifying s->c */
    if (s->r == LABEL_DECLARED)
    {
      tcc_warning_c(warn_all)("label '%s' declared but not used", get_tok_str(s->v, NULL));
    }
    else if (s->r == LABEL_FORWARD)
    {
      tcc_error("label '%s' used but not defined", get_tok_str(s->v, NULL));
    }
    else
    {
      if (s->c)
      {
        /* In IR mode, label_pop for local labels runs at scope exit BEFORE
           codegen, so orig_ir_to_code_mapping is NULL.  Defer resolution of
           addr-taken labels by moving them to global_label_stack, which is
           popped AFTER codegen when the mapping is available. */
        if (addr_taken && tcc_state->ir && !tcc_state->ir->orig_ir_to_code_mapping && ptop != &global_label_stack &&
            !tcc_state->check_only) /* a checked-only body has no codegen to wait for */
        {
          /* Unlink from table_ident now (function scope is ending) */
          if (s->r != LABEL_GONE)
            sym_set_label_head(s->v, s->prev_tok);
          s->r = LABEL_GONE;
          /* Create ELF symbol NOW with placeholder value (0) so that
             relocations emitted during codegen reference a valid symbol.
             Use put_extern_sym2 directly to bypass nocode_wanted check.
             After codegen the global label_pop will UPDATE this symbol
             with the correct code offset via orig_ir_to_code_mapping. */
          if (s->c == -3)
            s->c = 0; /* Reset marker so put_extern_sym2 creates new symbol */
          put_extern_sym2(s, cur_text_section->sh_num, 0, 1, 1);
          /* Push onto global_label_stack for deferred value update.
             s->c is now a valid ELF symbol index (> 0). */
          s->prev = global_label_stack;
          global_label_stack = s;
          continue;
        }

        /* Define corresponding symbol for &&label.
           In IR mode, the label position is recorded as an IR instruction index
           (s->jind) BEFORE DCE/IR compaction, so we must translate it using the
           original-index mapping.
           Also set Thumb bit (+1) so computed goto uses correct state.

           Note: s->c can be:
           - -3: LABEL_ADDR_TAKEN marker, need to reset to 0 for put_extern_sym to create symbol
           - > 0: valid ELF symbol index, put_extern_sym will UPDATE the existing symbol */
        if (s->c == -3)
          s->c = 0; /* Reset marker so put_extern_sym creates new symbol */

        TCCIRState *lir = tcc_state->ir;
        int label_at = -1; /* current index of the labelled position, its own instruction gone */
        if (lir && lir->ir_to_code_mapping && s->jind >= 0)
        {
          int survived = 0;
          for (int k = 0; k < lir->next_instruction_index && !survived; k++)
            survived = lir->compact_instructions[k].orig_index == s->jind;
          if (!survived)
            label_at = tcc_ir_label_insn(lir, s->jind);
          if (label_at >= lir->ir_to_code_mapping_size)
            label_at = -1;
        }
        if (label_at >= 0)
        {
          /* The instruction the label stood on was deleted.  The next original
           * index need not be the next instruction any more (an unrolled loop's
           * copies carry fresh ones, so it skipped the whole body): use where
           * the label itself was carried through the renumberings. */
          put_extern_sym(s, cur_text_section, lir->ir_to_code_mapping[label_at] + 1, 1);
        }
        else if (tcc_state->ir && tcc_state->ir->orig_ir_to_code_mapping && s->jind >= 0 &&
            s->jind < tcc_state->ir->orig_ir_to_code_mapping_size)
        {
          uint32_t off = tcc_state->ir->orig_ir_to_code_mapping[s->jind];
          /* If the instruction at jind was deleted by DSE/optimization, find the next
             valid mapping. The sentinel value 0xFFFFFFFF indicates no instruction. */
          if (off == 0xFFFFFFFF)
          {
            for (int idx = s->jind + 1; idx < tcc_state->ir->orig_ir_to_code_mapping_size; idx++)
            {
              if (tcc_state->ir->orig_ir_to_code_mapping[idx] != 0xFFFFFFFF)
              {
                off = tcc_state->ir->orig_ir_to_code_mapping[idx];
                break;
              }
            }
          }
          put_extern_sym(s, cur_text_section, off + 1, 1);
        }
        else if (tcc_state->ir && tcc_state->ir->ir_to_code_mapping && s->jind >= 0 &&
                 s->jind < tcc_state->ir->ir_to_code_mapping_size)
        {
          /* Backward-compatible fallback for older IR mapping */
          uint32_t off = tcc_state->ir->ir_to_code_mapping[s->jind];
          put_extern_sym(s, cur_text_section, off + 1, 1);
        }
        else
        {
          /* Fallback for non-IR codegen */
          put_extern_sym(s, cur_text_section, s->jnext, 1);
        }
      }
    }
    /* remove label */
    if (s->r != LABEL_GONE)
      sym_set_label_head(s->v, s->prev_tok);
    /* Don't free local label symbols whose address was taken (&&label) until
       after IR codegen, as the IR instructions still reference them. The symbol
       will be freed later with global labels after code generation. */
    if (!keep && !addr_taken)
      sym_free(s);
    else
      s->r = LABEL_GONE;
  }
  if (!keep)
    *ptop = slast;
}
