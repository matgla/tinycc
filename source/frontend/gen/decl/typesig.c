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

/* typesig.c -- -Wcross-tu, the compile side: at the end of a unit, a type
 * signature for every global symbol it defines or uses.
 *
 * A record is four NUL-terminated strings: the ELF name, the type as C
 * ("int (int, char *)"), 'D' (defined here, a tentative definition too) or
 * 'U' (only used) followed by where ("a.h:3": the first declaration of a 'U',
 * the definition of a 'D'; empty when not known), and the unit's file name.
 * With -c they go in the object's non-allocated section ".tcc.typesig" (only
 * with -Wcross-tu, so objects without it stay byte-identical; `tcc -ar` keeps
 * the section, `-r` merges it); a unit compiled straight into a link hands
 * them to the linker's table (obj/tccelf_load.c, tcc_typesig_*), which
 * compares them.
 *
 * The type is normalised to what C calls compatible: no storage class, no
 * yasc qualifiers (a C header and a yasc unit name the same function), a
 * parameter's own qualifiers dropped and its array / function decayed, an
 * enum as its integer type.  `int f()` and `int a[]` stay open ("()", "[]")
 * and the linker matches them with any parameters / size.  A named struct or
 * union stays its tag ("struct s *"); an anonymous one is written out
 * ("struct {int a; char b}").
 *
 * Every complete named struct / union those types reach (through pointers,
 * parameters, members, at any depth) gets a record of its own: name
 * "struct s" (no ELF name has a space), kind 'S' and where it is defined, and
 * as type its layout: "SIZE/ALIGN", then per member "\x1f" NAME "\x1e" TYPE
 * "\x1e" OFFSET (".BIT" for a bit-field, whose TYPE ends in ":WIDTH").  The
 * link compares the layouts a name's units see (C's compatible types across
 * units: the same members, in order, of compatible types). */

#include "gen_priv.h"

static void ts_type(char *buf, int buf_size, CType *type, const char *varstr, int param);

static int ts_anon_depth;

/* an anonymous struct / union, written out: "{int a; char b:3}" */
static void ts_anon(char *buf, int buf_size, Sym *ref)
{
  if (ref->c < 0 || ts_anon_depth >= 8)
  {
    pstrcat(buf, buf_size, ref->c < 0 ? "<anonymous>" : "{...}");
    return;
  }
  ts_anon_depth++;
  pstrcat(buf, buf_size, "{");
  for (Sym *f = sym_first(ref); f; f = f->next)
  {
    char m[256], w[16];
    int v = f->v & ~SYM_FIELD;
    ts_type(m, sizeof m, &f->type, v < SYM_FIRST_ANOM ? get_tok_str(v, NULL) : NULL, 0);
    pstrcat(buf, buf_size, m);
    if (f->type.t & VT_BITFIELD)
    {
      snprintf(w, sizeof w, ":%d", BIT_SIZE(f->type.t));
      pstrcat(buf, buf_size, w);
    }
    if (f->next)
      pstrcat(buf, buf_size, "; ");
  }
  pstrcat(buf, buf_size, "}");
  ts_anon_depth--;
}

static const char *ts_int_name(int t)
{
  int bt = t & VT_BTYPE, u = (t & VT_UNSIGNED) != 0;
  switch (bt)
  {
  case VT_BOOL:
    return "_Bool";
  case VT_BYTE:
    return (t & VT_DEFSIGN) ? (u ? "unsigned char" : "signed char") : "char";
  case VT_SHORT:
    return u ? "unsigned short" : "short";
  case VT_INT:
    if (t & VT_LONG)
      return u ? "unsigned long" : "long";
    return u ? "unsigned int" : "int";
  case VT_LLONG:
    if (t & VT_LONG)
      return u ? "unsigned long" : "long";
    return u ? "unsigned long long" : "long long";
  }
  return NULL;
}

static void ts_type(char *buf, int buf_size, CType *type, const char *varstr, int param)
{
  int t = type->t, bt = t & VT_BTYPE;
  char buf1[256];
  const char *tstr;
  Sym *s, *sa;

  buf[0] = 0;
  if (param)
  {
    /* a parameter: its own qualifiers do not count, arrays and functions decay */
    if (bt == VT_PTR && (t & (VT_ARRAY | VT_VLA)))
    {
      CType p = type->ref->type;
      mk_pointer(&p);
      ts_type(buf, buf_size, &p, varstr, 0);
      return;
    }
    if (bt == VT_FUNC)
    {
      CType p = *type;
      mk_pointer(&p);
      ts_type(buf, buf_size, &p, varstr, 0);
      return;
    }
    t &= ~(VT_CONSTANT | VT_VOLATILE);
  }
  if (bt != VT_PTR)
  {
    if (t & VT_VOLATILE)
      pstrcat(buf, buf_size, "volatile ");
    if (t & VT_CONSTANT)
      pstrcat(buf, buf_size, "const ");
  }
  buf_size -= strlen(buf);
  buf += strlen(buf);
  if (t & VT_COMPLEX)
    tstr = bt == VT_FLOAT ? "float _Complex" : bt == VT_LDOUBLE || (t & VT_LONG) ? "long double _Complex"
                                                                                 : "double _Complex";
  else
    switch (bt)
    {
    case VT_VOID:
      tstr = "void";
      break;
    case VT_FLOAT:
      tstr = "float";
      break;
    case VT_DOUBLE:
      tstr = (t & VT_LONG) ? "long double" : "double";
      break;
    case VT_LDOUBLE:
      tstr = "long double";
      break;
    case VT_STRUCT:
    {
      int v = type->ref->v & ~SYM_STRUCT;
      pstrcat(buf, buf_size, IS_UNION(t) ? "union " : "struct ");
      if (v < SYM_FIRST_ANOM)
        pstrcat(buf, buf_size, get_tok_str(v, NULL));
      else
        ts_anon(buf, buf_size, type->ref); /* its members are its name */
      tstr = NULL;
      break;
    }
    case VT_FUNC:
      s = type->ref;
      buf1[0] = 0;
      if (varstr && '*' == *varstr)
      {
        pstrcat(buf1, sizeof buf1, "(");
        pstrcat(buf1, sizeof buf1, varstr);
        pstrcat(buf1, sizeof buf1, ")");
      }
      pstrcat(buf1, sizeof buf1, "(");
      if (s->f.func_type != FUNC_OLD)
      {
        sa = sym_first(s);
        if (!sa && s->f.func_type == FUNC_NEW)
          pstrcat(buf1, sizeof buf1, "void");
        for (; sa; sa = sa->next)
        {
          char buf2[256];
          ts_type(buf2, sizeof buf2, &sa->type, NULL, 1);
          pstrcat(buf1, sizeof buf1, buf2);
          if (sa->next)
            pstrcat(buf1, sizeof buf1, ", ");
        }
        if (s->f.func_type == FUNC_ELLIPSIS)
          pstrcat(buf1, sizeof buf1, sym_first(s) ? ", ..." : "...");
      }
      pstrcat(buf1, sizeof buf1, ")");
      ts_type(buf, buf_size, &s->type, buf1, 0);
      return;
    case VT_PTR:
      s = type->ref;
      if (t & (VT_ARRAY | VT_VLA))
      {
        char dim[16] = "";
        if (s->c >= 0 && !(t & VT_VLA))
          snprintf(dim, sizeof dim, "%d", s->c);
        if (varstr && '*' == *varstr)
          snprintf(buf1, sizeof buf1, "(%s)[%s]", varstr, dim);
        else
          snprintf(buf1, sizeof buf1, "%s[%s]", varstr ? varstr : "", dim);
        ts_type(buf, buf_size, &s->type, buf1, 0);
        return;
      }
      pstrcpy(buf1, sizeof buf1, "*");
      if (t & VT_CONSTANT)
        pstrcat(buf1, sizeof buf1, " const");
      if (t & VT_VOLATILE)
        pstrcat(buf1, sizeof buf1, " volatile");
      if (varstr && *varstr)
      {
        if (buf1[1])
          pstrcat(buf1, sizeof buf1, " ");
        pstrcat(buf1, sizeof buf1, varstr);
      }
      ts_type(buf, buf_size, &s->type, buf1, 0);
      return;
    default:
      tstr = ts_int_name(t); /* an enum is its integer type */
      if (!tstr)
        tstr = "?";
      break;
    }
  if (tstr)
    pstrcat(buf, buf_size, tstr);
  if (varstr && *varstr)
  {
    pstrcat(buf, buf_size, " ");
    pstrcat(buf, buf_size, varstr);
  }
}

/* ------------------------------------------------------------------------ */
/* Where each global was first declared and first defined, and each named
 * struct / union tag defined, by token (a file-scope name is one symbol);
 * also the structs already given a record this unit.  Only with -Wcross-tu
 * (decl.c and struct.c call tcc_typesig_note); emptied at the unit's end. */

typedef struct TsLoc
{
  int v; /* 0: a free slot */
  int decl_line, def_line;
  const char *decl_file, *def_file;
  unsigned char seen; /* a struct: its record is written */
} TsLoc;

static TsLoc *ts_locs;
static int ts_nlocs, ts_cap; /* ts_cap a power of 2 */

static TsLoc *ts_loc(int v, int add)
{
  unsigned h;
  if (!ts_cap)
  {
    if (!add)
      return NULL;
    ts_cap = 64;
    ts_locs = tcc_mallocz(ts_cap * sizeof *ts_locs);
  }
  for (h = (unsigned)v * 2654435761u;; h++)
  {
    TsLoc *l = &ts_locs[h & (ts_cap - 1)];
    if (l->v == v)
      return l;
    if (!l->v)
    {
      if (!add)
        return NULL;
      if (2 * (ts_nlocs + 1) > ts_cap)
      {
        TsLoc *old = ts_locs;
        int n = ts_cap;
        ts_cap *= 2;
        ts_locs = tcc_mallocz(ts_cap * sizeof *ts_locs);
        for (int i = 0; i < n; i++)
          if (old[i].v)
          {
            unsigned g = (unsigned)old[i].v * 2654435761u;
            while (ts_locs[g & (ts_cap - 1)].v)
              g++;
            ts_locs[g & (ts_cap - 1)] = old[i];
          }
        tcc_free(old);
        return ts_loc(v, add);
      }
      ts_nlocs++;
      l->v = v;
      return l;
    }
  }
}

void tcc_typesig_reset(void)
{
  tcc_free(ts_locs);
  ts_locs = NULL;
  ts_nlocs = ts_cap = 0;
}

/* V (a global's token, or a struct tag's with SYM_STRUCT) declared (or, with
   DEFINED, defined) at LINE of the file being read */
void tcc_typesig_note(int v, int line, int defined)
{
  BufferedFile *f = file;
  const char *fn;
  TsLoc *l;
  while (f && f->filename[0] == ':' && f->prev)
    f = f->prev;
  if (!f || (v & ~SYM_STRUCT) >= SYM_FIRST_ANOM)
    return;
  fn = tcc_filename_intern(f->filename);
  l = ts_loc(v, 1);
  if (defined && !l->def_file)
    l->def_file = fn, l->def_line = line;
  else if (!defined && !l->decl_file)
    l->decl_file = fn, l->decl_line = line;
}

/* "D" / "U" / "S" and where: the definition of a 'D' or 'S', the first
   declaration of a 'U' (else the other one) */
static void ts_kind(char *buf, int size, int kind, int v)
{
  TsLoc *l = ts_loc(v, 0);
  const char *f = NULL;
  int line = 0;
  if (l)
  {
    int def = kind != 'U';
    f = def && l->def_file ? l->def_file : l->decl_file ? l->decl_file : l->def_file;
    line = f == l->def_file && (def || !l->decl_file) ? l->def_line : l->decl_line;
  }
  if (f)
    snprintf(buf, size, "%c%s:%d", kind, f, line);
  else
    snprintf(buf, size, "%c", kind);
}

static Section *ts_sec;

static void ts_record(TCCState *s1, const char *name, const char *sig, const char *kind, const char *unit)
{
  if (s1->output_type == TCC_OUTPUT_OBJ)
  {
    size_t n1 = strlen(name) + 1, n2 = strlen(sig) + 1, n3 = strlen(kind) + 1, n4 = strlen(unit) + 1;
    char *p;
    if (!ts_sec) /* -c: one unit, one section */
    {
      ts_sec = new_section(s1, ".tcc.typesig", SHT_PROGBITS, 0);
      ts_sec->sh_addralign = 1; /* `-r` concatenates them: no padding */
    }
    p = section_ptr_add(ts_sec, n1 + n2 + n3 + n4);
    memcpy(p, name, n1);
    memcpy(p + n1, sig, n2);
    memcpy(p + n1 + n2, kind, n3);
    memcpy(p + n1 + n2 + n3, unit, n4);
  }
  else
    tcc_typesig_add(s1, name, sig, kind[0], kind + 1, unit);
}

/* a named struct's layout record (see the top), once per unit */
static void ts_layout(TCCState *s1, Sym *ref, const char *unit)
{
  CString cs;
  char name[256], kind[300], m[512];
  int v = ref->v & ~SYM_STRUCT;
  cstr_new(&cs);
  snprintf(m, sizeof m, "%d/%d", ref->c, ref->r);
  cstr_cat(&cs, m, -1);
  for (Sym *f = sym_first(ref); f; f = f->next)
  {
    int fv = f->v & ~SYM_FIELD, t = f->type.t;
    cstr_ccat(&cs, '\x1f');
    if (fv < SYM_FIRST_ANOM)
      cstr_cat(&cs, get_tok_str(fv, NULL), -1);
    cstr_ccat(&cs, '\x1e');
    ts_type(m, sizeof m, &f->type, NULL, 0);
    cstr_cat(&cs, m, -1);
    if (t & VT_BITFIELD)
      snprintf(m, sizeof m, ":%d\x1e%d.%d", BIT_SIZE(t), f->c, BIT_POS(t));
    else
      snprintf(m, sizeof m, "\x1e%d", f->c);
    cstr_cat(&cs, m, -1);
  }
  cstr_ccat(&cs, 0);
  snprintf(name, sizeof name, "%s %s", IS_UNION(ref->type.t) ? "union" : "struct", get_tok_str(v, NULL));
  ts_kind(kind, sizeof kind, 'S', ref->v);
  ts_record(s1, name, cs.data, kind, unit);
  cstr_free(&cs);
}

/* the named structs TYPE reaches, each given its record once */
static void ts_structs(TCCState *s1, CType *type, const char *unit, int depth)
{
  int bt = type->t & VT_BTYPE;
  Sym *ref = type->ref;
  if (depth > 32)
    return;
  if (bt == VT_PTR)
    ts_structs(s1, &ref->type, unit, depth + 1);
  else if (bt == VT_FUNC)
  {
    ts_structs(s1, &ref->type, unit, depth + 1);
    for (Sym *a = sym_first(ref); a; a = a->next)
      ts_structs(s1, &a->type, unit, depth + 1);
  }
  else if (bt == VT_STRUCT && !IS_ENUM(type->t) && ref->c >= 0)
  {
    TsLoc *l = ts_loc(ref->v, 1); /* also marks an anonymous one */
    if (l->seen)
      return;
    l->seen = 1;
    if ((ref->v & ~SYM_STRUCT) < SYM_FIRST_ANOM)
      ts_layout(s1, ref, unit);
    for (Sym *f = sym_first(ref); f; f = f->next)
      ts_structs(s1, &f->type, unit, depth + 1);
  }
}

/* the end of a unit: its global symbols' records */
void tcc_typesig_unit(TCCState *s1, const char *unit)
{
  char sig[512], kind[300];
  ts_sec = NULL;
  if (s1->output_type == TCC_OUTPUT_PREPROCESS || s1->syntax_only)
  {
    tcc_typesig_reset();
    return;
  }
  for (Sym *s = global_stack; s; s = s->prev)
  {
    ElfSym *esym;
    const char *name;
    int bt = s->type.t & VT_BTYPE;
    if ((s->v & (SYM_STRUCT | SYM_FIELD)) || s->v >= SYM_FIRST_ANOM || (s->type.t & (VT_TYPEDEF | VT_STATIC)) ||
        (s->r & (VT_VALMASK | VT_SYM)) != (VT_CONST | VT_SYM) || !s->c || bt == VT_VOID)
      continue;
    esym = elfsym(s);
    if (!esym || ELFW(ST_BIND)(esym->st_info) == STB_LOCAL)
      continue;
    name = (char *)symtab_section->link->data + esym->st_name;
    if (!name[0])
      continue;
    ts_type(sig, sizeof sig, &s->type, NULL, 0);
    ts_kind(kind, sizeof kind, esym->st_shndx != SHN_UNDEF ? 'D' : 'U', s->v);
    ts_record(s1, name, sig, kind, unit);
    ts_structs(s1, &s->type, unit, 0);
  }
  ts_sec = NULL;
  tcc_typesig_reset();
}
