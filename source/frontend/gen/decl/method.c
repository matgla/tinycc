/*
 *  TCC - Tiny C Compiler
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 */

/* method.c -- yasc member functions (docs/yasc_methods.md).
 *
 *   struct vec
 *   {
 *     int x, y;
 *     int len2(const struct vec * view self) { return self->x * self->x + self->y * self->y; }
 *     struct vec make(int x, int y) { return (struct vec){ x, y }; }
 *   };
 *
 *   struct vec v = vec.make(3, 4);    // no self: called through the tag
 *   int n = v.len2();                 // vec__len2(&v)
 *
 * A function declared in a yasc struct body is a method.  It takes no room in
 * the struct: it is the plain file-scope function TAG__NAME, so C links with
 * it as it is.  Its body is parsed after the struct is complete -- at the next
 * file-scope declaration, where its saved tokens are replayed with the name
 * changed, prototypes of every method first, then the definitions.  Replayed
 * that way, a method is an ordinary declaration: static, inline, attributes,
 * module linkage and export, and the lifetime checks need nothing new.
 *
 * Each replayed declaration starts with a marker, `__yasc_method TAG NAME`
 * (`__yasc_method_decl` for the prototype of a method that has a body).
 * yasc_file_scope_decl reads it and registers TAG.NAME -> TAG__NAME.  The
 * marker also travels in a module interface ahead of an exported method's
 * prototype, which is how importers learn the struct's methods (the struct's
 * own piece of the interface leaves them out, yasc_methods_strip).
 *
 * The self parameter is explicit, as in Zig: the first parameter, a pointer
 * to the struct (with whatever yasc qualifiers) or the struct by value.
 * `obj.m(...)` passes &obj, `p->m(...)` passes p (or a copy of the struct for
 * a by-value self).  A method without such a first parameter is called
 * through its tag only: `vec.make(...)`. */

#include "gen_priv.h"

/* A method waiting for its replay */
typedef struct YascPendingMethod
{
  Sym *tag;
  int name;         /* the member's own name */
  int fn;           /* TAG__NAME */
  TokenString *str; /* its tokens, the name already changed to fn */
  int body_at;      /* where its body's '{' starts in str, -1 if none */
} YascPendingMethod;

static YascPendingMethod *ym_pending;
static int ym_nb_pending, ym_cap_pending;

/* TAG.NAME -> TAG__NAME, open addressing */
typedef struct YascMethodSlot
{
  Sym *tag;
  int name;
  int fn;
} YascMethodSlot;

static YascMethodSlot *ym_slots;
static int ym_nb_slots_used, ym_slots_mask; /* ym_slots_mask + 1 ym_slots, a power of 2 */

/* TAG__NAME -> NAME, indexed by TAG__NAME - TOK_IDENT (yasc_method_name_of) */
static int *ym_name_of;
static int ym_name_of_len;

/* some struct of the unit has methods */
static int ym_any;

/* > 0 while a yasc struct or union body is being parsed */
static int ym_struct_depth;

/* error recovery (core/recover.c) */
int yasc_methods_depth(int set)
{
  if (set >= 0)
    ym_struct_depth = set;
  return ym_struct_depth;
}

/* set by a marker for the declaration that follows (yasc_method_marker) */
int yasc_method_cur_kind; /* 0, TOK___yasc_method or TOK___yasc_method_decl */
int yasc_method_cur_tag;
int yasc_method_cur_name;

/* An argument unary_funcall takes before the parenthesized ones (call.c) */
int funcall_self_pending;
SValue funcall_self_arg;

static unsigned ym_slot_hash(const Sym *tag, int name)
{
  uintptr_t h = (uintptr_t)tag;
  h ^= h >> 7;
  return (unsigned)(h * 31u + (unsigned)name * 2654435761u);
}

static YascMethodSlot *ym_slot_find(const Sym *tag, int name)
{
  unsigned i;
  if (!ym_slots)
    return NULL;
  for (i = ym_slot_hash(tag, name) & ym_slots_mask;; i = (i + 1) & ym_slots_mask)
  {
    YascMethodSlot *s = &ym_slots[i];
    if (!s->tag)
      return NULL;
    if (s->tag == tag && s->name == name)
      return s;
  }
}

static void ym_slot_insert(Sym *tag, int name, int fn)
{
  unsigned i;
  YascMethodSlot *s = ym_slot_find(tag, name);

  if (s)
  {
    if (s->fn != fn)
      tcc_error("struct %s: method '%s' declared again with another symbol", get_tok_str(tag->v & ~SYM_STRUCT, NULL),
                get_tok_str(name, NULL));
    return;
  }
  if (!ym_slots || (ym_nb_slots_used + 1) * 4 > (ym_slots_mask + 1) * 3)
  {
    YascMethodSlot *old = ym_slots;
    int old_n = ym_slots ? ym_slots_mask + 1 : 0, j;
    int n = old_n ? old_n * 2 : 64;
    ym_slots = tcc_mallocz(n * sizeof *ym_slots);
    ym_slots_mask = n - 1;
    for (j = 0; j < old_n; j++)
      if (old[j].tag)
      {
        for (i = ym_slot_hash(old[j].tag, old[j].name) & ym_slots_mask; ym_slots[i].tag; i = (i + 1) & ym_slots_mask)
          ;
        ym_slots[i] = old[j];
      }
    tcc_free(old);
  }
  for (i = ym_slot_hash(tag, name) & ym_slots_mask; ym_slots[i].tag; i = (i + 1) & ym_slots_mask)
    ;
  if (fn - TOK_IDENT >= ym_name_of_len)
  {
    int n = (fn - TOK_IDENT + 1) * 2;
    ym_name_of = tcc_realloc(ym_name_of, n * sizeof *ym_name_of);
    memset(ym_name_of + ym_name_of_len, 0, (n - ym_name_of_len) * sizeof *ym_name_of);
    ym_name_of_len = n;
  }
  ym_name_of[fn - TOK_IDENT] = name;
  ym_slots[i].tag = tag;
  ym_slots[i].name = name;
  ym_slots[i].fn = fn;
  ym_nb_slots_used++;
}

void yasc_methods_reset(void)
{
  int i;
  for (i = 0; i < ym_nb_pending; i++)
    tok_str_free(ym_pending[i].str);
  tcc_free(ym_pending);
  ym_pending = NULL;
  ym_nb_pending = ym_cap_pending = 0;
  tcc_free(ym_slots);
  ym_slots = NULL;
  tcc_free(ym_name_of);
  ym_name_of = NULL;
  ym_name_of_len = 0;
  ym_nb_slots_used = ym_slots_mask = 0;
  ym_struct_depth = 0;
  ym_any = 0;
  yasc_method_cur_kind = yasc_method_cur_tag = yasc_method_cur_name = 0;
  funcall_self_pending = 0;
}

static int ym_tag_tok(const Sym *tag)
{
  return tag->v & ~SYM_STRUCT;
}

static int ym_method_symbol(int tag, int name)
{
  char buf[512];
  snprintf(buf, sizeof buf, "%s__%s", get_tok_str(tag, NULL), get_tok_str(name, NULL));
  return tok_alloc(buf, strlen(buf))->tok;
}

/* ------------------------------------------------------------------------- */
/* Telling a member declaration's end, and whether it declares a method.
 *
 * Fed one token at a time (TOK_LINENUM left out).  A member ends at its ';'
 * at nesting level 0, or with the '}' that closes a function body: a '{' at
 * level 0 after a ')' -- unless a struct, union or enum keyword still waits
 * for its body, as in yasc_export_capture.  The '}' that closes the struct
 * itself ends a last member that has no ';' and is not part of it.
 *
 * The member declares a function when, at level 0 and before any ':' or '=',
 * a '(' follows an identifier that is not a typedef name nor a tag: `int
 * len(void)`, but not `int (*fp)(void)`, `T (x)` or `struct s (*f)(void)`. */

typedef struct MemberScan
{
  int level, prev, prev2, tag_waits, in_body, no_name, name, commas;
} MemberScan;

#define MS_MORE 0
#define MS_END 1   /* the member ends with this token */
#define MS_CLOSE 2 /* this token closes the struct: not the member's */

static int ms_typedef_name(int t)
{
  Sym *s = sym_find(t);
  return s && (s->type.t & VT_TYPEDEF);
}

/* Feeds T; *IS_NAME is set when T is the method's name (seen at the '(') */
static int ms_feed(MemberScan *m, int t, int *name_now)
{
  *name_now = 0;
  if (m->in_body)
  {
    if (t == '{')
      m->level++;
    else if (t == '}' && --m->level == 0)
      return MS_END;
    return MS_MORE;
  }
  if (m->level == 0)
  {
    if (t == '}')
      return MS_CLOSE;
    if (t == TOK_STRUCT || t == TOK_UNION || t == TOK_ENUM)
      m->tag_waits = 1;
    else if (t == '{')
    {
      if (!m->tag_waits && m->prev == ')')
      {
        m->in_body = 1;
        m->level = 1;
        return MS_MORE;
      }
      m->tag_waits = 0;
    }
    else if (t == '(')
    {
      if (m->prev != TOK_ATTRIBUTE1 && m->prev != TOK_ATTRIBUTE2)
        m->tag_waits = 0;
      /* `T * borrows(buf) key`: the qualifier, unless a pointer kind came
         first (`T * view borrows(...)` is a method of that name);
         `int owned(close) fd`, `FILE * owned(fclose) f`: always the
         qualifier (owned is a keyword, never a method's name); `char * view
         c_api(char *) name`: c_api(T) after a '*' is the C type, always */
      int qualifier = (m->prev == TOK_borrows && m->prev2 != TOK_view && m->prev2 != TOK_owned) ||
                      (m->prev == TOK_takes && m->prev2 != TOK_view) || m->prev == TOK_owned ||
                      m->prev == TOK_c_api;
      if (!m->no_name && !m->name && !qualifier && m->prev >= TOK_UIDENT && m->prev2 != TOK_STRUCT &&
          m->prev2 != TOK_UNION && m->prev2 != TOK_ENUM && !ms_typedef_name(m->prev))
      {
        m->name = m->prev;
        *name_now = 1;
      }
    }
    else if (t == ':' || t == '=' || t == '[')
    {
      m->no_name = 1;
      if (t != ':')
        m->tag_waits = 0;
    }
    else if (t == ',')
    {
      m->commas++;
      m->tag_waits = 0;
    }
    else if (t < TOK_IDENT && t != ')')
      m->tag_waits = 0;
    if (t == ';')
      return MS_END;
  }
  if (t == '{' || t == '(' || t == '[')
    m->level++;
  else if (t == '}' || t == ')' || t == ']')
    m->level--;
  m->prev2 = m->prev;
  m->prev = t;
  return MS_MORE;
}

/* ------------------------------------------------------------------------- */
/* In a yasc struct body */

void yasc_struct_body_begin(void)
{
  ym_struct_depth++;
}

void yasc_struct_body_end(void)
{
  ym_struct_depth--;
}

/* tok starts a member of TAG's body.  Save the member's tokens; a method is
 * kept for its replay (1 is returned, the member is consumed), anything else
 * is replayed now for struct_decl to parse as usual (0). */
int yasc_method_member(Sym *tag)
{
  TokenString *saved_capture = pp_pragma_capture;
  TokenString *str = tok_str_alloc();
  MemberScan m;
  int r, name_now, name_at = -1, body_at = -1, closed = 0;

  memset(&m, 0, sizeof m);
  pp_pragma_capture = str;
  for (;;)
  {
    const int t = tok;
    if (t == TOK_EOF)
    {
      pp_pragma_capture = saved_capture;
      tcc_error("unexpected end of file in a struct body");
    }
    r = ms_feed(&m, t, &name_now);
    if (r == MS_CLOSE)
    {
      closed = 1;
      break;
    }
    if (name_now)
    {
      /* the name was the token before this '(': change it to TAG__NAME
         now, while the buffer is surely in memory */
      int *buf = (int *)tok_str_buf(str);
      name_at = str->len - 1;
      if (buf[name_at] != m.name)
        tcc_error("internal error: method name not where expected");
    }
    if (m.in_body && body_at < 0 && t == '{')
      body_at = str->len;
    tok_str_add_tok(str);
    if (r == MS_END)
      break;
    next();
  }
  pp_pragma_capture = saved_capture;

  if (!m.name)
  {
    /* a field: the saved tokens, then the token that ended them if it is
       not theirs, then 0, where next() goes back to the file */
    if (closed)
      tok_str_add2(str, tok, &tokc);
    tok_str_add(str, 0);
    begin_macro(str, 1);
    next();
    return 0;
  }

  if (ym_tag_tok(tag) >= SYM_FIRST_ANOM)
    tcc_error("method '%s' in an unnamed struct: give the struct a tag", get_tok_str(m.name, NULL));
  if (local_stack)
    tcc_error("method '%s': a struct with methods must be defined at file scope", get_tok_str(m.name, NULL));
  if (ym_struct_depth > 1)
    tcc_error("method '%s': a struct with methods cannot be defined inside another struct", get_tok_str(m.name, NULL));
  if (m.commas && body_at < 0)
    tcc_error("method '%s': declare one method per declaration", get_tok_str(m.name, NULL));
  {
    int fn = ym_method_symbol(ym_tag_tok(tag), m.name), i;
    YascPendingMethod *pm;
    int *buf = (int *)tok_str_buf(str);
    for (i = 0; i < ym_nb_pending; i++)
      if (ym_pending[i].tag == tag && ym_pending[i].name == m.name)
        tcc_error("struct %s: method '%s' declared twice", get_tok_str(ym_tag_tok(tag), NULL),
                  get_tok_str(m.name, NULL));
    buf[name_at] = fn;
    if (ym_nb_pending == ym_cap_pending)
    {
      ym_cap_pending = ym_cap_pending ? ym_cap_pending * 2 : 8;
      ym_pending = tcc_realloc(ym_pending, ym_cap_pending * sizeof *ym_pending);
    }
    pm = &ym_pending[ym_nb_pending++];
    pm->tag = tag;
    pm->name = m.name;
    pm->fn = fn;
    pm->str = str;
    pm->body_at = body_at;
  }
  tag->ymod |= YASC_HAS_METHODS;
  ym_any = 1;
  if (!closed)
    next();
  return 1;
}

/* TAG's body is complete: no field may share a method's name */
void yasc_methods_struct_done(Sym *tag)
{
  int i, cumofs;
  CType t;

  if (!(tag->ymod & YASC_HAS_METHODS))
    return;
  t.t = tag->type.t;
  t.ref = tag;
  for (i = 0; i < ym_nb_pending; i++)
    if (ym_pending[i].tag == tag && find_field(&t, ym_pending[i].name | SYM_FIELD, &cumofs))
      tcc_error("struct %s: '%s' is both a field and a method", get_tok_str(ym_tag_tok(tag), NULL),
                get_tok_str(ym_pending[i].name, NULL));
}

/* ------------------------------------------------------------------------- */
/* The replay, at the start of the next file-scope declaration */

static void ym_add_marker(TokenString *out, int kind, YascPendingMethod *pm)
{
  tok_str_add(out, kind);
  tok_str_add(out, ym_tag_tok(pm->tag));
  tok_str_add(out, pm->name);
  if (pm->tag->ymod & YASC_EXPORT)
    tok_str_add(out, TOK_export);
}

static void ym_add_words(TokenString *out, TokenString *str, int len)
{
  const int *p = tok_str_buf(str), *stop = p + len;
  int t;
  CValue cv;
  while (p < stop)
  {
    tok_get(&t, &p, &cv);
    tok_str_add2(out, t, &cv);
  }
}

int yasc_methods_pending(void)
{
  return ym_nb_pending;
}

void yasc_methods_flush(void)
{
  TCCState *s1 = tcc_state;
  TokenString *out = tok_str_alloc();
  CValue cv;
  int i;

  cv.i = (TCC_PCH_REPLAY_YASC << 16) | 1;
  tok_str_add2(out, TOK_PACK_REPLAY, &cv);
  /* every prototype first: a body may call any method of the struct */
  for (i = 0; i < ym_nb_pending; i++)
  {
    YascPendingMethod *pm = &ym_pending[i];
    ym_add_marker(out, pm->body_at >= 0 ? TOK___yasc_method_decl : TOK___yasc_method, pm);
    ym_add_words(out, pm->str, pm->body_at >= 0 ? pm->body_at : pm->str->len);
    if (pm->body_at >= 0)
      tok_str_add(out, ';');
  }
  for (i = 0; i < ym_nb_pending; i++)
  {
    YascPendingMethod *pm = &ym_pending[i];
    if (pm->body_at < 0)
      continue;
    ym_add_marker(out, TOK___yasc_method, pm);
    ym_add_words(out, pm->str, pm->str->len);
  }
  for (i = 0; i < ym_nb_pending; i++)
    tok_str_free(ym_pending[i].str);
  ym_nb_pending = 0;

  /* back to the dialect and the line of the token already read */
  cv.i = (TCC_PCH_REPLAY_YASC << 16) | (s1->yasc ? 1 : 0);
  tok_str_add2(out, TOK_PACK_REPLAY, &cv);
  cv.i = file->line_num;
  tok_str_add2(out, TOK_LINENUM, &cv);
  if (tok != TOK_EOF)
    tok_str_add2(out, tok, &tokc);
  tok_str_add(out, 0);
  begin_macro(out, 1);
  next();
}

/* tok is a marker (yasc_file_scope_decl): register the method it names */
void yasc_method_marker(void)
{
  Sym *tag;
  int kind = tok, t, name;

  next();
  t = tok;
  next();
  name = tok;
  next();
  tag = struct_find(t);
  if (!tag || (tag->type.t & VT_BTYPE) != VT_STRUCT)
    tcc_error("method '%s' of an unknown struct '%s'", get_tok_str(name, NULL), get_tok_str(t, NULL));
  tag->ymod |= YASC_HAS_METHODS;
  ym_any = 1;
  ym_slot_insert(tag, name, ym_method_symbol(t, name));
  yasc_method_cur_kind = kind;
  yasc_method_cur_tag = t;
  yasc_method_cur_name = name;
}

/* ------------------------------------------------------------------------- */
/* Finding and calling */

/* The file-scope function TAG.NAME stands for, or NULL if TAG has no such
   method */
Sym *yasc_method_find(Sym *tag, int name)
{
  YascMethodSlot *slot;
  Sym *s, *g = NULL;

  if (!(tag->ymod & YASC_HAS_METHODS))
    return NULL;
  slot = ym_slot_find(tag, name);
  if (!slot)
    return NULL;
  /* the outermost Sym of the name is the file-scope one */
  for (s = sym_find(slot->fn); s; s = s->prev_tok)
    g = s;
  if (!g || (g->type.t & VT_BTYPE) != VT_FUNC)
    tcc_error("internal error: method %s.%s has no function", get_tok_str(ym_tag_tok(tag), NULL),
              get_tok_str(name, NULL));
  return g;
}

/* NAME if FN (a token) is the function of a method TAG.NAME, else 0.  The
 * passes that read saved bodies by name before they are compiled (the unused
 * static pruning, called-once inlining: inline/emit.c) see a method call as
 * `.NAME` or `->NAME`; they take every method of that name as mentioned. */
int yasc_method_name_of(int fn)
{
  fn -= TOK_IDENT;
  return fn >= 0 && fn < ym_name_of_len ? ym_name_of[fn] : 0;
}

/* Whether NAME is the name of some struct's method */
int yasc_method_name_known(int name)
{
  int i;
  for (i = 0; ym_slots && i <= ym_slots_mask; i++)
    if (ym_slots[i].tag && ym_slots[i].name == name)
      return 1;
  return 0;
}

/* How FN takes the object: 1 through a pointer, 2 by value, 0 not at all */
static int ym_self_kind(Sym *fn, Sym *tag)
{
  Sym *p = sym_first(fn->type.ref);
  if (!p)
    return 0;
  if ((p->type.t & VT_BTYPE) == VT_PTR)
  {
    CType *pt = pointed_type(&p->type);
    return (pt->t & VT_BTYPE) == VT_STRUCT && pt->ref == tag ? 1 : 0;
  }
  return (p->type.t & VT_BTYPE) == VT_STRUCT && p->type.ref == tag ? 2 : 0;
}

/* `obj.NAME` or `p->NAME` (ARROW): vtop is the object, an lvalue struct of
 * TAG, or the pointer to it; tok is NAME.  Returns 0 when NAME is no method
 * of TAG (nothing read).  Otherwise parses the call and leaves its result on
 * vtop.  A pointer self gets `p` itself, so its yasc qualifiers (owned,
 * maybe) are what the checks see, or &obj; a by-value self a copy. */
int yasc_method_postfix(Sym *tag, int arrow)
{
  Sym *fn;
  int name = tok, kind;

  fn = yasc_method_find(tag, name);
  if (!fn)
    return 0;
  next();
  if (tok != '(')
    tcc_error("method '%s' of struct %s is not called: write %s.%s for the function itself", get_tok_str(name, NULL),
              get_tok_str(ym_tag_tok(tag), NULL), get_tok_str(ym_tag_tok(tag), NULL), get_tok_str(name, NULL));
  kind = ym_self_kind(fn, tag);
  if (!kind)
    tcc_error("method '%s' of struct %s has no self parameter: call it as %s.%s(...)", get_tok_str(name, NULL),
              get_tok_str(ym_tag_tok(tag), NULL), get_tok_str(ym_tag_tok(tag), NULL), get_tok_str(name, NULL));
  if (kind == 1 && !arrow)
    unary_take_address();
  else if (kind == 2 && arrow)
    indir();
  funcall_self_arg = *vtop;
  vpop();
  vset(&fn->type, fn->r, fn->c);
  vtop->sym = fn;
  vtop->vr = fn->vreg;
  if (fn->r & VT_SYM)
    vtop->c.i = 0;
  funcall_self_pending = 1;
  unary_funcall();
  return 1;
}

/* An identifier T, not a variable, followed by '.' (tok): `TAG.NAME` for a
 * struct tag or a typedef of a struct with methods.  Returns the method's
 * function with tok past NAME, or NULL (nothing read). */
Sym *yasc_method_static(int t, Sym *s)
{
  Sym *tag = NULL, *fn;
  int name;

  if (!ym_slots)
    return NULL;
  if (!s)
    tag = struct_find(t);
  else if ((s->type.t & VT_TYPEDEF) && (s->type.t & VT_BTYPE) == VT_STRUCT)
    tag = s->type.ref;
  if (!tag || !(tag->ymod & YASC_HAS_METHODS))
    return NULL;
  next();
  name = tok;
  if (name < TOK_UIDENT)
    expect("method name");
  fn = yasc_method_find(tag, name);
  if (!fn)
    tcc_error("struct %s has no method '%s'", get_tok_str(ym_tag_tok(tag), NULL), get_tok_str(name, NULL));
  next();
  return fn;
}

/* ------------------------------------------------------------------------- */
/* Module interfaces: a struct's piece without its methods */

/* STR is an export's piece.  A struct body in it that has methods: return a
   copy without them (their own exported pieces carry them), else NULL. */
TokenString *yasc_methods_strip(TokenString *str)
{
  const int *start = tok_str_buf(str), *p = start, *stop = start + str->len;
  TokenString *out = NULL;
  int level = 0, prev = 0, tag = 0, tag_waits = 0, t;
  CValue cv;

  if (!ym_any)
    return NULL;
  while (p < stop)
  {
    const int *at = p;
    tok_get(&t, &p, &cv);
    if (out)
      tok_str_add2(out, t, &cv);
    if (t == TOK_LINENUM)
      continue;
    if (level == 0)
    {
      if (t == TOK_STRUCT || t == TOK_UNION)
        tag_waits = 1, tag = 0;
      else if (tag_waits && t >= TOK_UIDENT && !tag)
        tag = t;
      else if (t == '{' && tag_waits)
      {
        Sym *ts = tag ? struct_find(tag) : NULL;
        tag_waits = 0;
        if (ts && (ts->ymod & YASC_HAS_METHODS))
        {
          /* the members, one at a time */
          if (!out)
          {
            out = tok_str_alloc();
            p = start;
            while (p <= at)
            {
              tok_get(&t, &p, &cv);
              tok_str_add2(out, t, &cv);
            }
          }
          for (;;)
          {
            MemberScan m;
            const int *mstart = p;
            int r = MS_MORE, name_now;
            memset(&m, 0, sizeof m);
            while (p < stop)
            {
              const int *q = p;
              tok_get(&t, &q, &cv);
              if (t == TOK_LINENUM)
              {
                p = q;
                continue;
              }
              r = ms_feed(&m, t, &name_now);
              if (r == MS_CLOSE)
                break;
              p = q;
              if (r == MS_END)
                break;
            }
            if (!m.name)
            {
              const int *q = mstart;
              while (q < p)
              {
                tok_get(&t, &q, &cv);
                tok_str_add2(out, t, &cv);
              }
            }
            if (r == MS_CLOSE || p >= stop)
              break;
          }
          /* p is at the struct's closing '}': the loop copies on */
          continue;
        }
      }
      else if (t != TOK_ATTRIBUTE1 && t != TOK_ATTRIBUTE2 && t != '(' && t != ')' && t < TOK_IDENT)
        tag_waits = 0;
    }
    if (t == '{' || t == '(' || t == '[')
      level++;
    else if (t == '}' || t == ')' || t == ']')
      level--;
    prev = t;
  }
  (void)prev;
  return out;
}
