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

/* module.c -- yasc modules: `module NAME;`, `export DECL` and `import NAME;`.
 *
 * docs/yasc_modules.md has the design.  A file that starts with
 * `module NAME;` is a module unit: its file-scope declarations are private
 * (internal linkage, as if static) unless they say `export`.  Exported names
 * keep their plain C symbol names.
 *
 * The three words are keywords only where a file-scope declaration starts,
 * in the main file of a yasc unit, and only while no typedef of that name is
 * in scope: headers (regular C) and ordinary uses such as `int module;` are
 * left alone.  Until the dialect is per file (static_analysis_todo.md P0),
 * "the module" is the main file: declarations that come from an #include
 * keep C linkage. */

#include "gen_priv.h"

#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <errno.h>
#include <ctype.h>

/* set from `export` to the end of that declaration: struct, union and enum
   bodies defined meanwhile are exported with it */
int yasc_exporting;
/* the export being parsed declared an object (its interface says extern) */
int yasc_export_object;

/* The interface: the tokens of every export, as importers will parse them */
static TokenString *yasc_iface;
/* -fmodule-header=: the same for C callers (yasc qualifiers left out, a plain
   `export inline` as its prototype), each export ending in TOK_LINEFEED */
static TokenString *yasc_cheader;
/* The export being parsed: its saved tokens, and how they end */
static TokenString *yasc_pending;
static int yasc_pending_end; /* YASC_END_* */
static int yasc_pending_body_at; /* an inline definition: where its body starts */
/* The export is a struct's method (decl/method.c): its marker's kind, tag
   and name.  The prototype every method gets ahead of the bodies
   (TOK___yasc_method_decl) is no piece of the interface. */
static int yasc_pending_method, yasc_pending_method_tag, yasc_pending_method_name;
#define YASC_END_DECL 0  /* up to and with its ';' */
#define YASC_END_HEAD 1  /* a function definition: the head, its body left in the file */
#define YASC_END_INLINE 2 /* an inline function definition, body included */
/* Every exported identifier and tag, checked again at the end of the unit:
   a struct defined after the export that uses it */
static Sym **yasc_exports;
static int nb_yasc_exports;
static char *yasc_module_src;
/* Modules imported by this unit: "L" + name while loading, "D" + name done */
static char **yasc_imported;
static int nb_yasc_imported;
/* The module unit's own imports, for its interface */
static char **yasc_module_imports;
static int nb_yasc_module_imports;
/* > 0 while an interface is being read */
static int yasc_import_depth;

static void yasc_export_finish(void);

int yasc_in_main_file(void)
{
  return tcc_state->include_stack_ptr == tcc_state->include_stack;
}

static int yasc_decl_keyword(int t)
{
  Sym *s;
  if (tok != t || !yasc_in_main_file())
    return 0;
  s = sym_find(t);
  return !(s && (s->type.t & VT_TYPEDEF));
}

/* NAME or NAME.NAME...: the spelling, allocated */
static char *yasc_module_name(void)
{
  CString cs;
  char *name;

  cstr_new(&cs);
  for (;;)
  {
    if (tok < TOK_UIDENT)
      expect("module name");
    cstr_cat(&cs, get_tok_str(tok, NULL), -1);
    next();
    if (tok != '.')
      break;
    cstr_ccat(&cs, '.');
    next();
  }
  cstr_ccat(&cs, 0);
  name = tcc_strdup(cs.data);
  cstr_free(&cs);
  return name;
}

static void yasc_module_decl(void)
{
  TCCState *s1 = tcc_state;
  char *name;

  next();
  name = yasc_module_name();
  if (s1->yasc_module)
  {
    tcc_free(name);
    tcc_error("a second module declaration: the file is already module '%s'", s1->yasc_module);
  }
  if (s1->yasc_main_decl_seen)
  {
    tcc_free(name);
    tcc_error("'module' must come before the file's first declaration");
  }
  s1->yasc_module = name;
  tcc_free(yasc_module_src);
  yasc_module_src = tcc_strdup(file->filename);
  skip(';');
}

static void yasc_import(TCCState *s1, const char *name, const char *via);

static void yasc_import_decl(void)
{
  char *name;

  next();
  name = yasc_module_name();
  if (tok != ';')
  {
    tcc_free(name);
    expect("';' after the module name");
  }
  yasc_import(tcc_state, name, NULL);
  tcc_free(name);
  skip(';');
}

/* At the start of a file-scope declaration of a yasc unit.  Returns -1 when
   `module` or `import` took the whole declaration, 1 after `export` (the
   declaration follows), 0 otherwise. */
static void yasc_export_capture(void);

int yasc_file_scope_decl(void)
{
  TCCState *s1 = tcc_state;

  /* the end of an imported interface's headers (yasc_import) */
  if (yasc_import_depth && tok == TOK___yasc_import_end)
    return -2;
  if (yasc_pending)
    yasc_export_finish();
  yasc_method_cur_kind = 0;
  if (tok == TOK___yasc_method || tok == TOK___yasc_method_decl)
    yasc_method_marker(); /* a struct's method follows (decl/method.c) */
  if (yasc_decl_keyword(TOK_module))
  {
    yasc_module_decl();
    return -1;
  }
  if (yasc_decl_keyword(TOK_import))
  {
    s1->yasc_main_decl_seen = 1;
    yasc_import_decl();
    return -1;
  }
  if (yasc_decl_keyword(TOK_export))
  {
    s1->yasc_main_decl_seen = 1;
    if (!s1->yasc_module)
      tcc_error("'export' outside a module: start the file with 'module NAME;'");
    next();
    if (tok == TOK_import)
      tcc_error("'export import' is not supported yet");
    if (tok == TOK_module || tok == TOK_export)
      tcc_error("'export %s' makes no sense", get_tok_str(tok, NULL));
    yasc_exporting = 1;
    yasc_export_object = 0;
    yasc_pending_method = yasc_method_cur_kind;
    yasc_pending_method_tag = yasc_method_cur_tag;
    yasc_pending_method_name = yasc_method_cur_name;
    yasc_export_capture();
    return 1;
  }
  if (tok != ';' && tok != TOK_EOF && yasc_in_main_file())
    s1->yasc_main_decl_seen = 1;
  return 0;
}

/* A struct, union or enum the module defined but did not export. */
static int yasc_private_tag(const Sym *s)
{
  return (s->ymod & (YASC_MODULE_OWN | YASC_EXPORT)) == YASC_MODULE_OWN;
}

static const char *yasc_tag_kind(const Sym *s)
{
  return IS_ENUM(s->type.t) ? "enum" : IS_UNION(s->type.t) ? "union" : "struct";
}

/* An importer must be able to write every type an export names.  Through a
   pointer a struct or union may stay opaque (an importer sees only its tag);
   by value its body must be exported.  An enum always needs its body. */
static void yasc_check_type(CType *t, int v, int by_value)
{
  int bt = t->t & VT_BTYPE;
  Sym *s;

  if (IS_ENUM(t->t))
  {
    s = t->ref;
    if (s && yasc_private_tag(s))
      tcc_error("exported '%s' uses enum %s, which is private: export the enum", get_tok_str(v, NULL),
                get_tok_str(s->v & ~SYM_STRUCT, NULL));
    return;
  }
  if (bt == VT_PTR)
  {
    yasc_check_type(&t->ref->type, v, (t->t & VT_ARRAY) ? by_value : 0);
    return;
  }
  if (bt == VT_FUNC)
  {
    s = t->ref;
    yasc_check_type(&s->type, v, 1);
    for (sym_first(s); (s = s->next) != NULL;)
      yasc_check_type(&s->type, v, 1);
    return;
  }
  if (bt == VT_STRUCT && by_value)
  {
    s = t->ref;
    if (yasc_private_tag(s))
      tcc_error("exported '%s' uses %s %s by value, but its body is private: export the definition or use a pointer",
                get_tok_str(v, NULL), yasc_tag_kind(s), get_tok_str(s->v & ~SYM_STRUCT, NULL));
  }
}

/* An exported struct or union: its members are visible to importers too. */
void yasc_check_export_members(Sym *s)
{
  Sym *f = s;
  for (sym_first(f); (f = f->next) != NULL;)
    yasc_check_type(&f->type, s->v & ~SYM_STRUCT, 1);
}

/* A file-scope declarator of the module (main file).  Settles its linkage
   and returns whether it is exported: `export`, or an earlier declaration of
   the name was. */
int yasc_module_linkage(CType *type, int v, int exported)
{
  Sym *s = sym_find(v);

  if (type->t & VT_TYPEDEF)
  {
    /* `typedef struct s s_t;` only names the struct: an importer gets an
       opaque handle.  An array of it needs the body. */
    if (exported)
      yasc_check_type(type, v, (type->t & VT_BTYPE) != VT_STRUCT);
    return exported;
  }
  if (exported)
  {
    /* `export static inline`: no symbol, the body goes to importers */
    if ((type->t & (VT_STATIC | VT_INLINE)) == VT_STATIC)
      tcc_error("'%s' cannot be both export and static", get_tok_str(v, NULL));
    if (s && (s->ymod & YASC_MODULE_OWN) && !(s->ymod & YASC_EXPORT))
      tcc_error("'%s' was declared private before: put 'export' on its first declaration", get_tok_str(v, NULL));
    yasc_check_type(type, v, 1);
    /* a plain `export inline` is also defined here, for C callers (importers
       get its body as `static inline`) */
    if ((type->t & (VT_INLINE | VT_STATIC)) == VT_INLINE)
      type->t &= ~VT_INLINE;
    return 1;
  }
  if (s && (s->ymod & YASC_EXPORT))
    return 1; /* the definition of an exported prototype */
  if (!(type->t & VT_EXTERN))
    type->t |= VT_STATIC; /* private; `extern` still names something outside */
  return 0;
}

/* The Sym a module declarator made: owned by the module, maybe exported. */
void yasc_module_sym(Sym *s, int exported)
{
  if (!s)
    return;
  if (exported)
  {
    if ((s->type.t & VT_BTYPE) != VT_FUNC && !(s->type.t & VT_TYPEDEF))
      yasc_export_object = 1;
    yasc_export_tag(s);
  }
  else if (!(s->type.t & (VT_STATIC | VT_TYPEDEF)))
    return; /* `extern`: names something outside the module, not private */
  s->ymod |= YASC_MODULE_OWN;
}

/* An exported identifier, or a struct/union/enum whose body is exported */
void yasc_export_tag(Sym *s)
{
  if (!(s->ymod & YASC_EXPORT))
    dynarray_add(&yasc_exports, &nb_yasc_exports, s);
  s->ymod |= YASC_EXPORT;
}

/* ------------------------------------------------------------------------- */
/* Saving an export's tokens */

static int yasc_is_inline_kw(int t)
{
  return t == TOK_INLINE1 || t == TOK_INLINE2 || t == TOK_INLINE3;
}

/* `export` was read; tok is the declaration's first token.  Save its tokens
 * up to its ';' -- or, for a function definition, up to the body's '{',
 * which stays in the file (all of it for an inline one) -- and replay them,
 * so decl() parses the declaration as usual.  The saved tokens become the
 * export's piece of the interface (yasc_export_finish).
 *
 * A '{' at nesting level 0 opens a function body when it follows a ')' --
 * unless a struct, union or enum keyword is still waiting for its body:
 * `struct __attribute__((packed)) {` ends with ')' too.  That keyword stops
 * waiting at any punctuation but ')' and ':' (an enum's base type), and at a
 * '(' that does not open an attribute (`struct s f(void) {`). */
static void yasc_export_capture(void)
{
  TCCState *s1 = tcc_state;
  TokenString *saved_capture = pp_pragma_capture;
  TokenString *str = tok_str_alloc();
  int level = 0, prev = 0, tag_waits = 0, is_inline = 0, end = YASC_END_DECL, len;

  if (*s1->pack_stack_ptr)
    tcc_error("'export' under #pragma pack is not supported yet: use __attribute__((packed))");
  yasc_pending_body_at = -1;
  /* a #pragma pack inside the declaration travels with its tokens */
  pp_pragma_capture = str;
  for (;;)
  {
    const int t = tok;
    if (t == TOK_EOF)
    {
      pp_pragma_capture = saved_capture;
      tcc_error("unexpected end of file in an exported declaration");
    }
    if (level == 0)
    {
      if (t == TOK_STRUCT || t == TOK_UNION || t == TOK_ENUM)
        tag_waits = 1;
      else if (yasc_is_inline_kw(t))
        is_inline = 1;
      else if (t == '{')
      {
        if (!tag_waits && prev == ')')
        {
          end = YASC_END_HEAD;
          break;
        }
        tag_waits = 0;
      }
      else if (t == '(')
      {
        if (prev != TOK_ATTRIBUTE1 && prev != TOK_ATTRIBUTE2)
          tag_waits = 0;
      }
      else if (t < TOK_IDENT && t != ')' && t != ':')
        tag_waits = 0;
    }
    tok_str_add_tok(str);
    if (level == 0 && t == ';')
      break;
    if (t == '{' || t == '(' || t == '[')
      level++;
    else if (t == '}' || t == ')' || t == ']')
      level--;
    prev = t;
    next();
  }
  if (end == YASC_END_HEAD && is_inline)
  {
    /* importers compile an inline body themselves: keep it, up to and with
       its closing '}', without reading past it */
    end = YASC_END_INLINE;
    yasc_pending_body_at = str->len;
    for (;;)
    {
      const int t = tok;
      if (t == TOK_EOF)
      {
        pp_pragma_capture = saved_capture;
        tcc_error("unexpected end of file in an exported declaration");
      }
      tok_str_add_tok(str);
      if (t == '{')
        level++;
      else if (t == '}' && --level == 0)
        break;
      next();
    }
  }
  pp_pragma_capture = saved_capture;

  /* The replay: the saved tokens, then the body's '{' already read from the
     file, then 0, where next() goes back to the file. */
  len = str->len;
  if (end == YASC_END_HEAD)
    tok_str_add(str, '{');
  tok_str_add(str, 0);
  begin_macro(tok_str_borrow(str, str->len), 2);
  str->len = len;
  yasc_pending = str;
  yasc_pending_end = end;
  next();
}

/* A private name an importer could not resolve: a typedef or an enumerator
 * of a private enum anywhere; in an inline body, also a private function or
 * object (not after '.' or '->': a member).  Names are looked up at file
 * scope, so a local that shadows a private global in an inline body is
 * reported too. */
static void yasc_check_piece_name(int t, int prev, int in_body)
{
  Sym *s;

  if (t < TOK_UIDENT)
    return;
  s = sym_find(t);
  if (!s)
    return;
  if (s->type.t & VT_TYPEDEF)
  {
    if ((s->ymod & (YASC_MODULE_OWN | YASC_EXPORT)) == YASC_MODULE_OWN)
      tcc_error("an exported declaration uses the private typedef '%s': export it", get_tok_str(t, NULL));
    return;
  }
  if (IS_ENUM_VAL(s->type.t))
  {
    if (s->type.ref && yasc_private_tag(s->type.ref))
      tcc_error("an exported declaration uses '%s' of a private enum: export the enum", get_tok_str(t, NULL));
    return;
  }
  if (in_body && prev != '.' && prev != TOK_ARROW &&
      (s->ymod & (YASC_MODULE_OWN | YASC_EXPORT)) == YASC_MODULE_OWN)
    tcc_error("an exported inline function uses the private '%s': export it", get_tok_str(t, NULL));
}

/* The token after P that is not a line number. */
static int yasc_peek(const int *p, const int *stop, const int **next)
{
  int t = TOK_EOF;
  CValue cv;
  while (p < stop)
  {
    tok_get(&t, &p, &cv);
    if (t != TOK_LINENUM)
      break;
    t = TOK_EOF;
  }
  if (next)
    *next = p;
  return t & ~SYM_FIELD;
}

/* A pointer's qualifier: what may stand between a '*' and borrows(...) or
   takes(...) (declarator.c type_decl). */
static int yasc_ptr_qual(int t)
{
  return (t >= TOK_owned && t <= TOK_drop) || (t >= TOK_CONST1 && t <= TOK_CONST3) ||
         (t >= TOK_VOLATILE1 && t <= TOK_VOLATILE3) || (t >= TOK_RESTRICT1 && t <= TOK_RESTRICT3) ||
         t == TOK__Atomic;
}

/* A yasc parameter's target is read-only unless it says inout / out / drop
   (`in`, the default): in the C header the `*` at P of a parameter's
   outermost pointer is `const *` when its qualifiers say view (not
   owned), and an array parameter's name at P is
   `T const a[]`. */
static int yasc_c_in(int t, const int *p, const int *stop)
{
  int n, view = 0;
  if (t == '*')
  {
    while ((n = yasc_peek(p, stop, &p)) != TOK_EOF && yasc_ptr_qual(n))
    {
      if (n == TOK_owned)
        return 0;
      view |= n == TOK_view;
    }
    /* another '*' after it: not the outermost; '(' : a function's; the
       name and '[': the array is (`char * view a[]` is `char * const a[]`) */
    if (n >= TOK_UIDENT && yasc_peek(p, stop, NULL) == '[')
      return 0;
    return view && n != '*' && n != '(';
  }
  /* `a[owned]`: the array a parameter becomes is owned, its elements free */
  return t >= TOK_UIDENT && yasc_peek(p, stop, &p) == '[' && yasc_peek(p, stop, NULL) != TOK_owned;
}

/* T, read after PREV (and PREV2) with P at the token after it: a yasc
   qualifier whose (...) follows -- `* borrows(x)`, `* takes(p)`,
   `struct T borrows(x)`, owned(fclose) -- as the parser reads them. */
static int yasc_qual_args(int t, int prev, int prev2, int ptr_quals, const int *p, const int *stop)
{
  const int *q;
  if (yasc_peek(p, stop, &q) != '(')
    return 0;
  if (t == TOK_owned)
    return yasc_peek(q, stop, &q) >= TOK_UIDENT && yasc_peek(q, stop, NULL) == ')';
  if (t == TOK_takes)
    return ptr_quals;
  if (t == TOK_borrows)
    return ptr_quals || ((prev2 == TOK_STRUCT || prev2 == TOK_UNION) && prev >= TOK_UIDENT);
  return 0;
}

/* The export just parsed: append its piece to the interface.
 * - an object: `extern` in front, initializers left out;
 * - a function definition: its head and ';';
 * - an inline definition: all of it, as `static inline`. */
static void yasc_export_finish(void)
{
  TokenString *str = yasc_pending;
  const int *p, *start, *stop;
  int level = 0, prev = 0, t, has_extern = 0, has_static = 0, c_proto;
  /* the C header leaves out a yasc qualifier's (...): armed by the
     qualifier, then the depth inside them */
  int prev2 = 0, ptr_quals = 0, c_arm = 0, c_skip = 0, c_const = 0, c_prev = 0;
  CValue cv;

  yasc_pending = NULL;
  if (yasc_pending_method == TOK___yasc_method_decl)
  {
    yasc_pending_method = 0;
    tok_str_free(str);
    return;
  }
  if (!yasc_iface)
    yasc_iface = tok_str_alloc();
  if (tcc_state->module_header_dir && !yasc_cheader)
    yasc_cheader = tok_str_alloc();
  if (yasc_pending_method)
  {
    /* importers register the method from its marker */
    tok_str_add(yasc_iface, TOK___yasc_method);
    tok_str_add(yasc_iface, yasc_pending_method_tag);
    tok_str_add(yasc_iface, yasc_pending_method_name);
    yasc_pending_method = 0;
  }
  else
  {
    /* a struct with methods: its piece leaves them out */
    TokenString *stripped = yasc_methods_strip(str);
    if (stripped)
    {
      tok_str_free(str);
      str = stripped;
    }
  }
  start = tok_str_buf(str);
  stop = start + str->len;

  for (p = start; p < stop;)
  {
    tok_get(&t, &p, &cv);
    if (t == TOK_EXTERN)
      has_extern = 1;
    else if (t == TOK_STATIC)
      has_static = 1;
  }
  if (yasc_export_object && !has_extern)
  {
    tok_str_add(yasc_iface, TOK_EXTERN);
    if (yasc_cheader)
      tok_str_add(yasc_cheader, TOK_EXTERN);
  }
  if (yasc_pending_end == YASC_END_INLINE && !has_static)
    tok_str_add(yasc_iface, TOK_STATIC);
  /* C callers of a plain `export inline` call the module's definition */
  c_proto = yasc_pending_end == YASC_END_INLINE && !has_static;

  /* the parameter lists open at each level (yasc_c_in): the '(' after a
     name or a ')' opens one; where the current parameter started, whether
     it says inout / out / drop / in, and whether it has had a '*' */
  uint32_t plist = 0;
  int pstart = 0, paccess = 0;
  for (p = start; p < stop;)
  {
    const int in_body = yasc_pending_body_at >= 0 && p - start >= yasc_pending_body_at;
    tok_get(&t, &p, &cv);
    if (t == TOK_LINENUM)
    {
      tok_str_add2(yasc_iface, t, &cv);
      continue;
    }
    t &= ~SYM_FIELD;
    yasc_check_piece_name(t, prev, in_body);
    if (yasc_pending_end == YASC_END_INLINE && (prev == TOK_STRUCT || prev == TOK_UNION) && t >= TOK_UIDENT)
    {
      /* an inline body may reach into the struct: importers need its body */
      Sym *tag = struct_find(t);
      if (tag && yasc_private_tag(tag))
        tcc_error("an exported inline function uses %s %s, whose body is private: export the definition",
                  yasc_tag_kind(tag), get_tok_str(t, NULL));
    }
    if (level == 0 && t == '=' && yasc_pending_end == YASC_END_DECL)
    {
      /* an initializer: up to the next ',' or ';' of this level */
      int lv = 0;
      while (p < stop)
      {
        const int *q = p;
        tok_get(&t, &q, &cv);
        if (lv == 0 && (t == ',' || t == ';'))
          break;
        if (t == '{' || t == '(' || t == '[')
          lv++;
        else if (t == '}' || t == ')' || t == ']')
          lv--;
        p = q;
      }
      continue;
    }
    const int at_plist = !in_body && level > 0 && level < 32 && ((plist >> level) & 1);
    const int was_pstart = pstart;
    if (t == '(' && !in_body && level + 1 < 32)
    {
      plist = (plist & ~(1u << (level + 1))) | ((prev >= TOK_UIDENT || prev == ')') ? 1u << (level + 1) : 0);
      if ((plist >> (level + 1)) & 1)
        pstart = 1, paccess = 0;
    }
    else if (t == ',' && at_plist)
      pstart = 1, paccess = 0;
    else if (at_plist && (t == TOK_out || t == TOK_inout || t == TOK_drop || (t == TOK_in && was_pstart)))
      paccess = 1;
    else
      pstart = 0;
    if (t == '{' || t == '(' || t == '[')
      level++;
    else if (t == '}' || t == ')' || t == ']')
      level--;
    tok_str_add2(yasc_iface, t, &cv);
    const int prev_c = prev;
    prev = t;
    int c_drop = 0;
    if (c_arm)
    {
      c_arm = 0;
      c_skip = c_drop = 1; /* the '(' */
    }
    else if (c_skip)
    {
      c_drop = 1;
      c_skip += t == '(' ? 1 : t == ')' ? -1 : 0;
    }
    else if (yasc_cheader && yasc_qual_args(t, prev_c, prev2, ptr_quals, p, stop))
      c_arm = c_drop = 1;
    if (!c_drop)
      ptr_quals = t == '*' || (ptr_quals && yasc_ptr_qual(t));
    prev2 = prev_c;
    if (yasc_cheader && !c_drop && !(c_proto && (in_body || yasc_is_inline_kw(t))) &&
        !(t >= TOK_owned && t <= TOK_drop) && !(t == TOK_in && was_pstart && at_plist))
    {
      if (in_body && (t == TOK_defer || t == TOK_errdefer))
        tcc_error("-fmodule-header: an exported static inline function uses '%s', which C does not have",
                  get_tok_str(t, NULL));
      if (in_body && (prev_c == '.' || prev_c == TOK_ARROW) && yasc_method_name_known(t))
        tcc_error("-fmodule-header: an exported static inline function calls the method '%s': C has no method "
                  "calls, call the function TAG__%s",
                  get_tok_str(t, NULL), get_tok_str(t, NULL));
      /* a const already there (`const char * view s`): not twice */
      if (!c_const && at_plist && !paccess && c_prev != '(' && yasc_c_in(t, p, stop))
        tok_str_add(yasc_cheader, TOK_CONST1);
      tok_str_add2(yasc_cheader, t, &cv);
      if (t >= TOK_CONST1 && t <= TOK_CONST3)
        c_const = 1;
      else if (t == '*' || t == ',' || t == ';' || t == '(' || t == ')' || t == '[' || t == ']' || t == '{' ||
               t == '}' || t == '=')
        c_const = 0;
      c_prev = t;
    }
  }
  if (yasc_pending_end == YASC_END_HEAD)
    tok_str_add(yasc_iface, ';');
  if (yasc_cheader)
  {
    if (yasc_pending_end == YASC_END_HEAD || c_proto)
      tok_str_add(yasc_cheader, ';');
    tok_str_add(yasc_cheader, TOK_LINEFEED);
  }
  tok_str_free(str);
}

/* ------------------------------------------------------------------------- */
/* The interface file */

typedef struct YBuf
{
  unsigned char *b;
  size_t n, cap;
} YBuf;

static void yb_put(YBuf *y, const void *data, size_t n)
{
  if (y->n + n > y->cap)
  {
    y->cap = (y->n + n) * 2 + 256;
    y->b = tcc_realloc(y->b, y->cap);
  }
  memcpy(y->b + y->n, data, n);
  y->n += n;
}

static void yb_u32(YBuf *y, uint32_t v)
{
  unsigned char c[4] = {v & 0xff, (v >> 8) & 0xff, (v >> 16) & 0xff, v >> 24};
  yb_put(y, c, 4);
}

static void yb_str(YBuf *y, const char *s, int len)
{
  if (len < 0)
    len = strlen(s);
  yb_u32(y, len);
  yb_put(y, s, len);
}

/* what makes an interface from another compiler or ABI unusable */
void yasc_ymi_compiler_id(char *buf, int size)
{
  TCCState *s1 = tcc_state;
  snprintf(buf, size, "tcc %s ptr%d long%d ld%d uc%d fabi%d", TCC_VERSION, PTR_SIZE, LONG_SIZE, LDOUBLE_SIZE,
           s1->char_is_unsigned, s1->float_abi);
}

/* The token stream: punctuators and value tokens as they are (values in
 * words, strings with their size), identifiers and keywords as
 * TOK_IDENT + an index into the string table, since a unit numbers its
 * identifiers in the order it meets them. */
static void yasc_ymi_tokens(YBuf *strings, uint32_t *nb_strings, YBuf *words, uint32_t *nb_words)
{
  const int *p = tok_str_buf(yasc_iface), *stop = p + yasc_iface->len;
  int nb_ids = tok_ident - TOK_IDENT, *index = tcc_malloc(nb_ids * sizeof(int));
  uint32_t count = 0;
  int t;
  CValue cv;

  memset(index, -1, nb_ids * sizeof(int));
  *nb_strings = 0;
  while (p < stop)
  {
    tok_get(&t, &p, &cv);
    t &= ~SYM_FIELD;
    if (t >= TOK_IDENT)
    {
      int *slot = &index[t - TOK_IDENT];
      if (*slot < 0)
      {
        *slot = (*nb_strings)++;
        yb_str(strings, get_tok_str(t, NULL), -1);
      }
      yb_u32(words, TOK_IDENT + *slot), count++;
      continue;
    }
    yb_u32(words, t), count++;
    switch (t)
    {
#if LONG_SIZE == 4
    case TOK_CLONG:
    case TOK_CULONG:
#endif
    case TOK_CINT:
    case TOK_CUINT:
    case TOK_CCHAR:
    case TOK_LCHAR:
    case TOK_CINT_I:
    case TOK_LINENUM:
    case TOK_PACK_REPLAY:
      yb_u32(words, (uint32_t)cv.i), count++;
      break;
    case TOK_CFLOAT:
    case TOK_CFLOAT_I:
      yb_u32(words, (uint32_t)cv.tab[0]), count++;
      break;
#if LONG_SIZE == 8
    case TOK_CLONG:
    case TOK_CULONG:
#endif
    case TOK_CLLONG:
    case TOK_CULLONG:
      yb_u32(words, (uint32_t)cv.i), yb_u32(words, (uint32_t)(cv.i >> 32)), count += 2;
      break;
    case TOK_CDOUBLE:
    case TOK_CDOUBLE_I:
    case TOK_CLDOUBLE:
    case TOK_CLDOUBLE_I:
    {
      /* the target's long double is a double */
      double d = (t == TOK_CLDOUBLE || t == TOK_CLDOUBLE_I) ? (double)cv.ld : cv.d;
      uint64_t u;
      memcpy(&u, &d, 8);
      yb_u32(words, (uint32_t)u), yb_u32(words, (uint32_t)(u >> 32)), count += 2;
      break;
    }
    case TOK_STR:
    case TOK_LSTR:
    case TOK_PPNUM:
    case TOK_PPSTR:
    {
      int nw = (cv.str.size + 3) / 4;
      uint32_t zero = 0;
      yb_u32(words, cv.str.size), count++;
      yb_put(words, cv.str.data, cv.str.size);
      yb_put(words, &zero, nw * 4 - cv.str.size);
      count += nw;
      break;
    }
    default:
      break;
    }
  }
  tcc_free(index);
  *nb_words = count;
}

/* DIR/a/b.EXT for module a.b; DIR is the one given, else the first
   -fmodule-dir=, else the object's directory */
static char *yasc_ymi_path(TCCState *s1, const char *dir, const char *ext)
{
  CString cs;
  const char *m;
  char *path;

  cstr_new(&cs);
  if (dir)
    cstr_cat(&cs, dir, -1);
  else if (s1->nb_module_dirs)
    cstr_cat(&cs, s1->module_dirs[0], -1);
  else if (s1->outfile && tcc_basename(s1->outfile) != s1->outfile)
    cstr_cat(&cs, s1->outfile, tcc_basename(s1->outfile) - s1->outfile - 1);
  else
    cstr_ccat(&cs, '.');
  cstr_ccat(&cs, '/');
  for (m = s1->yasc_module; *m; m++)
    cstr_ccat(&cs, *m == '.' ? '/' : *m);
  cstr_cat(&cs, ext, -1);
  cstr_ccat(&cs, 0);
  path = tcc_strdup(cs.data);
  cstr_free(&cs);
  return path;
}

static void yasc_mkdirs(char *path)
{
  char *p;
  for (p = path + 1; *p; p++)
  {
    if (*p != '/')
      continue;
    *p = 0;
#ifdef _WIN32
    mkdir(path);
#else
    mkdir(path, 0777);
#endif
    *p = '/';
  }
}

/* Is the file at PATH exactly these bytes? */
static int yasc_same_file(const char *path, const YBuf *y)
{
  FILE *f = fopen(path, "rb");
  int same = 0;
  if (f)
  {
    unsigned char *old = tcc_malloc(y->n + 1);
    same = fread(old, 1, y->n + 1, f) == y->n && !memcmp(old, y->b, y->n);
    tcc_free(old);
    fclose(f);
  }
  return same;
}

/* Write OUT to PATH through a temporary and a rename, and only when the bytes
   differ: an unchanged file keeps its time, so make rebuilds nothing. */
static void yasc_write_if_changed(const char *path, const YBuf *out, const char *what)
{
  char *dir = tcc_strdup(path), *tmp;
  FILE *f;

  if (yasc_same_file(path, out))
  {
    tcc_free(dir);
    return;
  }
  yasc_mkdirs(dir);
  tcc_free(dir);
  tmp = tcc_malloc(strlen(path) + 32);
  sprintf(tmp, "%s.%d.tmp", path, (int)getpid());
  f = fopen(tmp, "wb");
  if (!f || fwrite(out->b, 1, out->n, f) != out->n || fclose(f) != 0 || rename(tmp, path) != 0)
  {
    if (f)
      remove(tmp);
    tcc_error_noabort("cannot write the %s '%s'", what, path);
  }
  tcc_free(tmp);
}

static void yasc_ymi_write(TCCState *s1)
{
  YBuf out = {0}, strings = {0}, words = {0};
  uint32_t nb_strings, nb_words;
  char id[128], *path;
  int i;

  yasc_ymi_compiler_id(id, sizeof id);
  if (!yasc_iface)
    yasc_iface = tok_str_alloc();
  yasc_ymi_tokens(&strings, &nb_strings, &words, &nb_words);

  yb_put(&out, "YMI1", 4);
  yb_u32(&out, 1); /* format version */
  yb_str(&out, id, -1);
  yb_str(&out, s1->yasc_module, -1);
  yb_str(&out, yasc_module_src ? yasc_module_src : "", -1);
  yb_u32(&out, s1->nb_yasc_includes);
  for (i = 0; i < s1->nb_yasc_includes; i++)
    yb_str(&out, s1->yasc_includes[i], -1);
  yb_u32(&out, nb_yasc_module_imports);
  for (i = 0; i < nb_yasc_module_imports; i++)
    yb_str(&out, yasc_module_imports[i], -1);
  yb_u32(&out, nb_strings);
  yb_put(&out, strings.b, strings.n);
  yb_u32(&out, nb_words);
  yb_put(&out, words.b, words.n);
  tcc_free(strings.b);
  tcc_free(words.b);

  path = yasc_ymi_path(s1, NULL, ".ymi");
  /* an edited function body leaves the bytes alone: no importer rebuilds */
  yasc_write_if_changed(path, &out, "module interface");
  tcc_free(path);
  tcc_free(out.b);
}

/* ------------------------------------------------------------------------- */
/* -fmodule-header=DIR: DIR/a/b.h, the interface for C callers */

/* a token as C source text */
static void yasc_c_token(CString *cs, int t, CValue *cv)
{
  char num[64];
  const char *suffix = "";

  switch (t)
  {
  case TOK_CFLOAT:
    snprintf(num, sizeof num, "%.9g", (double)cv->f);
    suffix = "f";
    break;
  case TOK_CDOUBLE:
    snprintf(num, sizeof num, "%.17g", cv->d);
    break;
  case TOK_CLDOUBLE:
    snprintf(num, sizeof num, "%.17g", (double)cv->ld);
    suffix = "L";
    break;
  case TOK_CFLOAT_I:
  case TOK_CDOUBLE_I:
  case TOK_CLDOUBLE_I:
  case TOK_CINT_I:
    tcc_error("-fmodule-header: an imaginary constant in an export");
  default:
    cstr_cat(cs, get_tok_str(t, cv), -1);
    switch (t)
    {
    case TOK_CUINT:
      cstr_cat(cs, "u", -1);
      break;
    case TOK_CLONG:
      cstr_cat(cs, "l", -1);
      break;
    case TOK_CULONG:
      cstr_cat(cs, "ul", -1);
      break;
    case TOK_CLLONG:
      cstr_cat(cs, "ll", -1);
      break;
    case TOK_CULLONG:
      cstr_cat(cs, "ull", -1);
      break;
    }
    return;
  }
  /* a floating constant needs its point: 1e+30 has one, 2 does not */
  if (!strpbrk(num, ".eEn"))
    strcat(num, ".0");
  cstr_cat(cs, num, -1);
  cstr_cat(cs, suffix, -1);
}

/* The tokens as C: an export per line, a body's statements one per line */
static void yasc_c_text(CString *cs, TokenString *str)
{
  const int *p = tok_str_buf(str), *stop = p + str->len;
  int t, prev = TOK_LINEFEED, braces = 0, parens = 0, line_start = 1, i;
  CValue cv;

  while (p < stop)
  {
    tok_get(&t, &p, &cv);
    if (t == TOK_LINEFEED)
    {
      cstr_ccat(cs, '\n');
      line_start = 1;
      prev = t;
      continue;
    }
    if (t == '}' && braces > 0)
    {
      braces--;
      if (!line_start)
        cstr_ccat(cs, '\n'), line_start = 1;
    }
    if (line_start)
    {
      for (i = 0; i < braces; i++)
        cstr_cat(cs, "  ", -1);
    }
    else if (prev != '(' && prev != '[' && prev != '.' && prev != TOK_ARROW && t != ',' && t != ';' && t != ')' &&
             t != ']' && t != '.' && t != TOK_ARROW && !((t == '(' || t == '[') && prev >= TOK_IDENT))
      cstr_ccat(cs, ' ');
    line_start = 0;
    yasc_c_token(cs, t, &cv);
    if (t == '(')
      parens++;
    else if (t == ')')
      parens--;
    /* a new line after a body's '{' and after each of its statements */
    if ((t == '{' && ++braces) || (t == ';' && braces > 0 && parens == 0))
    {
      cstr_ccat(cs, '\n');
      line_start = 1;
    }
    prev = t;
  }
}

static void yasc_header_write(TCCState *s1)
{
  CString cs;
  YBuf out = {0};
  char *path, *guard;
  const char *m;
  int i;

  if (!yasc_cheader)
    yasc_cheader = tok_str_alloc();
  path = yasc_ymi_path(s1, s1->module_header_dir, ".h");
  cstr_new(&cs);
  cstr_printf(&cs, "/* C header of yasc module %s (%s), written by tcc -fmodule-header.\n"
                   "   Do not edit: it is written again when the module's interface changes. */\n",
              s1->yasc_module, yasc_module_src ? tcc_basename(yasc_module_src) : "?");
  guard = tcc_malloc(strlen(s1->yasc_module) + 1);
  for (i = 0, m = s1->yasc_module; *m; m++)
    guard[i++] = *m == '.' ? '_' : toupper((unsigned char)*m);
  guard[i] = 0;
  cstr_printf(&cs, "#ifndef YASC_MODULE_%s_H\n#define YASC_MODULE_%s_H\n", guard, guard);
  tcc_free(guard);
  for (i = 0; i < s1->nb_yasc_includes; i++)
  {
    const char *h = s1->yasc_includes[i];
    cstr_printf(&cs, h[0] == '<' ? "#include %s\n" : "#include \"%s\"\n", h);
  }
  /* the headers of the modules it imports, from the same -fmodule-header= */
  for (i = 0; i < nb_yasc_module_imports; i++)
  {
    cstr_cat(&cs, "#include <", -1);
    for (m = yasc_module_imports[i]; *m; m++)
      cstr_ccat(&cs, *m == '.' ? '/' : *m);
    cstr_cat(&cs, ".h>\n", -1);
  }
  cstr_cat(&cs, "\n", -1);
  yasc_c_text(&cs, yasc_cheader);
  cstr_cat(&cs, "\n#endif\n", -1);
  yb_put(&out, cs.data, cs.size);
  cstr_free(&cs);
  yasc_write_if_changed(path, &out, "module header");
  tcc_free(path);
  tcc_free(out.b);
}

/* ------------------------------------------------------------------------- */

/* The unit was parsed: check every export again (a struct defined after the
   export that uses it) and write the interface. */
void yasc_module_finish(TCCState *s1)
{
  int i;

  if (yasc_pending)
    yasc_export_finish();
  if (!s1->yasc_module)
    return;
  for (i = 0; i < nb_yasc_exports; i++)
  {
    Sym *s = yasc_exports[i];
    if (s->v & SYM_STRUCT)
    {
      if (!IS_ENUM(s->type.t))
        yasc_check_export_members(s);
    }
    else
      yasc_check_type(&s->type, s->v, !(s->type.t & VT_TYPEDEF) || (s->type.t & VT_BTYPE) != VT_STRUCT);
  }
  if (!s1->nb_errors)
    yasc_ymi_write(s1);
  if (!s1->nb_errors && s1->module_header_dir)
    yasc_header_write(s1);
  yasc_module_reset();
}

/* A new unit, or the end of one */
void yasc_module_reset(void)
{
  if (yasc_pending)
    tok_str_free(yasc_pending);
  if (yasc_iface)
    tok_str_free(yasc_iface);
  if (yasc_cheader)
    tok_str_free(yasc_cheader);
  yasc_pending = yasc_iface = yasc_cheader = NULL;
  /* the Syms belong to the symbol table: free only the array */
  tcc_free(yasc_exports);
  yasc_exports = NULL;
  nb_yasc_exports = 0;
  tcc_free(yasc_module_src);
  yasc_module_src = NULL;
  yasc_exporting = 0;
  dynarray_reset(&yasc_imported, &nb_yasc_imported);
  dynarray_reset(&yasc_module_imports, &nb_yasc_module_imports);
  yasc_import_depth = 0;
  yasc_pending_method = 0;
}

/* ------------------------------------------------------------------------- */
/* Reading interfaces: `import NAME;` */

typedef struct YRead
{
  const unsigned char *p, *end;
  const char *path;
} YRead;

static void yr_bad(YRead *r)
{
  tcc_error("'%s' is damaged or not a module interface", r->path);
}

static uint32_t yr_u32(YRead *r)
{
  uint32_t v;
  if (r->end - r->p < 4)
    yr_bad(r);
  v = r->p[0] | r->p[1] << 8 | r->p[2] << 16 | (uint32_t)r->p[3] << 24;
  r->p += 4;
  return v;
}

static const char *yr_bytes(YRead *r, uint32_t n)
{
  const char *s = (const char *)r->p;
  if ((uint32_t)(r->end - r->p) < n)
    yr_bad(r);
  r->p += n;
  return s;
}

/* a str as a new NUL-terminated string */
static char *yr_strdup(YRead *r)
{
  uint32_t n = yr_u32(r);
  const char *s = yr_bytes(r, n);
  char *d = tcc_malloc(n + 1);
  memcpy(d, s, n);
  d[n] = 0;
  return d;
}

static int yasc_import_state(const char *name)
{
  int i;
  for (i = 0; i < nb_yasc_imported; i++)
    if (!strcmp(yasc_imported[i] + 1, name))
      return yasc_imported[i][0];
  return 0;
}

static void yasc_import_mark(const char *name, char state)
{
  int i;
  char *e;
  for (i = 0; i < nb_yasc_imported; i++)
    if (!strcmp(yasc_imported[i] + 1, name))
    {
      yasc_imported[i][0] = state;
      return;
    }
  e = tcc_malloc(strlen(name) + 2);
  e[0] = state;
  strcpy(e + 1, name);
  dynarray_add(&yasc_imported, &nb_yasc_imported, e);
}

/* Look for a/b.ymi in each -fmodule-dir=, the object's directory, then the
   -I paths. */
static char *yasc_ymi_find(TCCState *s1, const char *name)
{
  CString rel, cand;
  const char *m;
  char *found = NULL;
  int i, n = s1->nb_module_dirs + 1 + s1->nb_include_paths;

  cstr_new(&rel);
  for (m = name; *m; m++)
    cstr_ccat(&rel, *m == '.' ? '/' : *m);
  cstr_cat(&rel, ".ymi", -1);
  cstr_ccat(&rel, 0);
  cstr_new(&cand);
  for (i = 0; i < n && !found; i++)
  {
    cstr_reset(&cand);
    if (i < s1->nb_module_dirs)
      cstr_cat(&cand, s1->module_dirs[i], -1);
    else if (i == s1->nb_module_dirs)
    {
      if (s1->outfile && tcc_basename(s1->outfile) != s1->outfile)
        cstr_cat(&cand, s1->outfile, tcc_basename(s1->outfile) - s1->outfile - 1);
      else
        cstr_ccat(&cand, '.');
    }
    else
      cstr_cat(&cand, s1->include_paths[i - s1->nb_module_dirs - 1], -1);
    cstr_ccat(&cand, '/');
    cstr_cat(&cand, rel.data, -1);
    cstr_ccat(&cand, 0);
    if (access(cand.data, R_OK) == 0)
      found = tcc_strdup(cand.data);
  }
  cstr_free(&cand);
  cstr_free(&rel);
  return found;
}

static unsigned char *yasc_read_file(const char *path, size_t *size)
{
  FILE *f = fopen(path, "rb");
  unsigned char *b;
  long n;
  if (!f)
    return NULL;
  if (fseek(f, 0, SEEK_END) != 0 || (n = ftell(f)) < 0 || fseek(f, 0, SEEK_SET) != 0)
  {
    fclose(f);
    return NULL;
  }
  b = tcc_malloc(n ? n : 1);
  if (fread(b, 1, n, f) != (size_t)n)
  {
    tcc_free(b);
    b = NULL;
  }
  fclose(f);
  *size = n;
  return b;
}

/* The token words: back to a token string of this unit, ending in TOK_EOF */
static TokenString *yr_tokens(YRead *r, int *ids, uint32_t nb_ids)
{
  TokenString *str = tok_str_alloc();
  uint32_t nb_words = yr_u32(r), w;
  const unsigned char *words_end;
  CValue cv;

  if ((uint32_t)(r->end - r->p) / 4 < nb_words)
    yr_bad(r);
  words_end = r->p + 4 * (size_t)nb_words;
  while (r->p < words_end)
  {
    w = yr_u32(r);
    if (w >= TOK_IDENT)
    {
      if (w - TOK_IDENT >= nb_ids)
        yr_bad(r);
      tok_str_add(str, ids[w - TOK_IDENT]);
      continue;
    }
    switch (w)
    {
#if LONG_SIZE == 4
    case TOK_CLONG:
    case TOK_CULONG:
#endif
    case TOK_CINT:
    case TOK_CUINT:
    case TOK_CCHAR:
    case TOK_LCHAR:
    case TOK_CINT_I:
    case TOK_LINENUM:
    case TOK_PACK_REPLAY:
    {
      uint32_t v = yr_u32(r);
      cv.i = (w == TOK_CUINT || w == TOK_CULONG) ? (uint64_t)v : (uint64_t)(int64_t)(int32_t)v;
      break;
    }
    case TOK_CFLOAT:
    case TOK_CFLOAT_I:
      cv.tab[0] = yr_u32(r);
      break;
#if LONG_SIZE == 8
    case TOK_CLONG:
    case TOK_CULONG:
#endif
    case TOK_CLLONG:
    case TOK_CULLONG:
    {
      uint64_t lo = yr_u32(r);
      cv.i = lo | (uint64_t)yr_u32(r) << 32;
      break;
    }
    case TOK_CDOUBLE:
    case TOK_CDOUBLE_I:
    case TOK_CLDOUBLE:
    case TOK_CLDOUBLE_I:
    {
      uint64_t u = yr_u32(r);
      double d;
      u |= (uint64_t)yr_u32(r) << 32;
      memcpy(&d, &u, 8);
      if (w == TOK_CLDOUBLE || w == TOK_CLDOUBLE_I)
        cv.ld = d;
      else
        cv.d = d;
      break;
    }
    case TOK_STR:
    case TOK_LSTR:
    case TOK_PPNUM:
    case TOK_PPSTR:
      cv.str.size = yr_u32(r);
      cv.str.data = (char *)yr_bytes(r, (cv.str.size + 3) & ~3u);
      break;
    default:
      if (TOK_HAS_VALUE(w) || w == 0 || w == TOK_EOF)
        yr_bad(r);
      tok_str_add(str, w);
      continue;
    }
    tok_str_add2(str, w, &cv);
  }
  if (r->p != words_end)
    yr_bad(r);
  tok_str_add(str, TOK_EOF);
  return str;
}

/* Load module NAME's interface into this unit: its imports first, then the
 * headers it included (a small buffer of #include lines, pushed like an
 * included file, parsed up to the __yasc_import_end it ends with), then its
 * tokens, replayed into decl() up to TOK_EOF.  Both sit above the importer's
 * file on the include stack, so nothing they declare is the importer's own.
 * Tokens replayed from a string are never macro-expanded: the importer's
 * macros cannot change the interface. */
static void yasc_import(TCCState *s1, const char *name, const char *via)
{
  char id[128], *path, *src, *mod, *got_id;
  unsigned char *buf;
  size_t size;
  YRead r;
  uint32_t i, n, nb_ids;
  int *ids, saved_tok;
  CValue saved_tokc;
  CString text;
  TokenString *body;

  switch (yasc_import_state(name))
  {
  case 'D':
    return;
  case 'L':
    tcc_error("modules import each other: '%s' imports '%s', which is still being imported", via, name);
  }
  if (s1->yasc_module && !strcmp(name, s1->yasc_module))
  {
    if (via)
      tcc_error("modules import each other: '%s' imports '%s', the module being compiled", via, name);
    tcc_error("module '%s' imports itself", name);
  }
  if (!via && s1->yasc_module)
    dynarray_add(&yasc_module_imports, &nb_yasc_module_imports, tcc_strdup(name));
  yasc_import_mark(name, 'L');

  path = yasc_ymi_find(s1, name);
  if (!path)
    tcc_error("module '%s' not found: compile it first (its interface is looked for in -fmodule-dir=, the "
              "object's directory and -I)",
              name);
  buf = yasc_read_file(path, &size);
  if (!buf)
    tcc_error("cannot read the module interface '%s'", path);
  if (s1->gen_deps)
    dynarray_add(&s1->target_deps, &s1->nb_target_deps, tcc_strdup(path));
  r.p = buf;
  r.end = buf + size;
  r.path = path;

  if (size < 8 || memcmp(buf, "YMI1", 4))
    yr_bad(&r);
  r.p += 4;
  if (yr_u32(&r) != 1)
    tcc_error("'%s' has another interface format version: rebuild module '%s'", path, name);
  yasc_ymi_compiler_id(id, sizeof id);
  got_id = yr_strdup(&r);
  if (strcmp(got_id, id))
    tcc_error("'%s' was built by another compiler or for another ABI (%s, here %s): rebuild module '%s'", path,
              got_id, id, name);
  tcc_free(got_id);
  mod = yr_strdup(&r);
  if (strcmp(mod, name))
    tcc_error("'%s' is the interface of module '%s', not '%s'", path, mod, name);
  tcc_free(mod);
  src = yr_strdup(&r);

  /* the headers, as #include lines, then the end marker */
  cstr_new(&text);
  n = yr_u32(&r);
  for (i = 0; i < n; i++)
  {
    char *h = yr_strdup(&r);
    if (h[0] == '<')
      cstr_printf(&text, "#include %s\n", h);
    else
      cstr_printf(&text, "#include \"%s\"\n", h);
    tcc_free(h);
  }
  cstr_cat(&text, "__yasc_import_end\n", -1);

  /* the modules it imports come first */
  n = yr_u32(&r);
  for (i = 0; i < n; i++)
  {
    char *dep = yr_strdup(&r);
    yasc_import(s1, dep, name);
    tcc_free(dep);
  }

  nb_ids = yr_u32(&r);
  if ((size_t)(r.end - r.p) / 4 < nb_ids)
    yr_bad(&r);
  ids = tcc_malloc((nb_ids ? nb_ids : 1) * sizeof(int));
  for (i = 0; i < nb_ids; i++)
  {
    uint32_t len = yr_u32(&r);
    const char *s = yr_bytes(&r, len);
    ids[i] = tok_alloc(s, len)->tok;
  }
  body = yr_tokens(&r, ids, nb_ids);
  tcc_free(ids);
  if (r.p != r.end)
    yr_bad(&r);
  tcc_free(buf);

  saved_tok = tok;
  saved_tokc = tokc;
  if (s1->include_stack_ptr >= s1->include_stack + INCLUDE_STACK_SIZE)
    tcc_error("imports nested too deep");
  /* "In file included from F:N" prints the line before the includer's:
     an #include has read its newline, an import has not */
  file->line_num++;
  *s1->include_stack_ptr++ = file;
  /* the interface is yasc -- and so is every body saved from it and compiled
     later (an `export inline`), which tcc_yasc_file finds by this name; the
     headers it includes are what they are */
  tcc_yasc_mark_file(s1, src[0] ? src : path);
  tcc_open_bf(s1, src[0] ? src : path, text.size);
  memcpy(file->buffer, text.data, text.size);
  cstr_free(&text);
  tcc_free(src);
  yasc_import_depth++;
  next();
  decl(VT_CONST); /* the headers, up to __yasc_import_end */
  begin_macro(body, 1);
  next();
  decl(VT_CONST); /* the interface, up to TOK_EOF */
  end_macro();
  yasc_import_depth--;
  tcc_close();
  s1->include_stack_ptr--;
  file->line_num--;
  tok = saved_tok;
  tokc = saved_tokc;
  yasc_import_mark(name, 'D');
  tcc_free(path);
}

/* ------------------------------------------------------------------------- */
/* -fmodule-deps: what a file is and what it imports, without compiling it */

/* Print `module NAME` and `import NAME` lines for the file: one for each
 * such declaration of the main file at nesting level 0, after #if and macro
 * expansion (the file is preprocessed, nothing is parsed or imported, so no
 * interface needs to exist yet).  With several input files, a `file PATH`
 * line comes first.  A build orders its compiles by these lines: a module
 * before the files that import it. */
ST_FUNC void yasc_module_scan(TCCState *s1)
{
  FILE *out = s1->ppfp ? s1->ppfp : stdout;
  const int saved_output_type = s1->output_type;
  int level = 0, at_start = 1;

  /* the input file, under the <command line> buffer of predefines */
  if (s1->nb_files > 1)
    fprintf(out, "file %s\n", file->prev ? file->prev->filename : file->filename);
  /* -fmodule-deps runs as -E, which hands #pragma lines back as tokens for
     its output: read the file as a compile would */
  s1->output_type = TCC_OUTPUT_OBJ;
  parse_flags = PARSE_FLAG_PREPROCESS;
  next();
  while (tok != TOK_EOF)
  {
    const int t = tok;
    if (at_start && level == 0 && s1->yasc && yasc_in_main_file() && (t == TOK_module || t == TOK_import))
    {
      char *name;
      next();
      name = yasc_module_name();
      if (tok == ';')
        fprintf(out, "%s %s\n", t == TOK_module ? "module" : "import", name);
      tcc_free(name);
      at_start = 0;
      continue;
    }
    at_start = 0;
    if (t == '{' || t == '(' || t == '[')
      level++;
    else if (t == '}' || t == ')' || t == ']')
    {
      if (--level == 0 && t == '}')
        at_start = 1;
    }
    else if (t == ';' && level == 0)
      at_start = 1;
    next();
  }
  s1->output_type = saved_output_type;
}
