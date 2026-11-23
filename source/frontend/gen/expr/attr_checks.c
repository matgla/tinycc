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

/* attr_checks.c -- the checks GCC attributes ask for at each use.
 *
 * - -Wdeprecated-declarations: a `deprecated` function, variable, typedef,
 *   tag or enumerator named by the program (not by its own declarations).
 * - -Wunused-result: the result of a `warn_unused_result` / `[[nodiscard]]`
 *   function dropped by an expression statement, a comma or a for clause.
 *   A cast to void keeps it, for both (gcc does not let (void) silence
 *   warn_unused_result; clang does, and so does tcc).
 * - -Wformat, -Wformat-security, -Wformat-n: a `format(printf|scanf, n, m)`
 *   call's literal format against the arguments, as they are parsed.
 *
 * decl/attribute.c parses the attributes and keeps them in the declared
 * Sym's SymLocalFacts (SymAttr.attr_facts).  The checks are frontend
 * diagnostics; at -O1+ the inliner replays a body once per call site, so
 * each is reported once per place (attr_warn: tcc_warning_once). */

#include "gen_priv.h"

/* ------------------------------------------------------------------------- */
/* Reporting */

/* The line error1 would give a diagnostic now */
static int attr_cur_line(void)
{
  BufferedFile *f = file;
  while (f && f->filename[0] == ':' && f->prev)
    f = f->prev;
  return f ? f->line_num - ((tok_flags & TOK_FLAG_BOL) && !macro_ptr) : 0;
}

/* warning OPT at LINE: tcc_warning_once -- once per place and text (the
 * -O1+ inliner parses a body again at each call site), quiet in system
 * headers as gcc is, under the expanded function's file.  Returns whether it
 * was shown (its notes follow it). */
static int attr_warn(int opt, int line, const char *fmt, ...)
{
  va_list ap;
  va_start(ap, fmt);
  tcc_vwarning_once(opt + (int)offsetof(TCCState, warn_none), line, fmt, ap);
  va_end(ap);
  return tcc_state->diag_last_shown;
}

#define ATTR_OPT(sw) (offsetof(TCCState, sw) - offsetof(TCCState, warn_none))

/* ------------------------------------------------------------------------- */
/* -Wdeprecated-declarations */

/* Inside parse_btype the use of a deprecated typedef or tag waits for the
 * end of the specifiers: `old_t __attribute__((deprecated)) f(void);` says
 * nothing, as with gcc (attr_btype_end). */
#define DEP_PENDING_MAX 8
static struct
{
  Sym *s;
  int line;
} dep_pending[DEP_PENDING_MAX];
static int dep_pending_nb, dep_btype_depth;
/* a deprecated sprintf and kin, waiting for its call (dep_call_waits) */
static struct
{
  Sym *s;
  int line;
} dep_call;
int attr_deprecated_suppress; /* the declarators of a deprecated declaration (decl.c) */

/* Fix-it hints for the deprecated C library functions with a known
 * replacement (yasos libc marks them deprecated for yasc).  RENAME: the same
 * arguments under another name, a fix-it; else a note only. */
static const struct
{
  const char *name, *use, *rename;
} dep_fixes[] = {
    {"sprintf", "snprintf(buf, size, fmt, ...), which bounds the output", NULL},
    {"vsprintf", "vsnprintf(buf, size, fmt, ap), which bounds the output", NULL},
    {"strcpy", "strlcpy(dst, src, size), which bounds the copy", NULL},
    {"strcat", "strlcat(dst, src, size), which bounds the copy", NULL},
    {"gets", "fgets(buf, size, stdin), which bounds the line", NULL},
    {"tmpnam", "mkstemp(template), which creates the file safely", NULL},
    {"tempnam", "mkstemp(template), which creates the file safely", NULL},
    {"mktemp", "mkstemp(template), which creates the file safely", NULL},
    {"strtok", "strtok_r(str, delim, &saveptr), which keeps no hidden state", NULL},
    {"atoi", "strtol(str, &end, 10), which reports bad input", NULL},
    {"atol", "strtol(str, &end, 10), which reports bad input", NULL},
    {"asctime", "strftime(buf, size, fmt, tm), which bounds the output", NULL},
    {"ctime", "strftime(buf, size, fmt, localtime_r(...)), which bounds the output", NULL},
    {"getwd", "getcwd(buf, size), which bounds the path", NULL},
    {"bzero", "memset(p, 0, n)", NULL},
    {"bcopy", "memmove(dst, src, n) (the first two arguments swap)", NULL},
    {"index", "strchr", "strchr"},
    {"rindex", "strrchr", "strrchr"},
    {"usleep", "nanosleep(&ts, NULL)", NULL},
};

static void dep_fixit(Sym *s, const char *name, int line, ObjCall *oc)
{
  if ((s->type.t & VT_BTYPE) != VT_FUNC || (s->v & SYM_STRUCT))
    return;
  /* sprintf(buf, ...) -> snprintf(buf, sizeof buf, ...) when buf's size is
     known (builtin/objsize.c) */
  if (oc && objsize_bound_fixit(oc, line))
    return;
  for (size_t i = 0; i < sizeof dep_fixes / sizeof dep_fixes[0]; i++)
    if (!strcmp(name, dep_fixes[i].name))
    {
      if (dep_fixes[i].rename)
      {
        tcc_note("%i:use '%s' instead", line, dep_fixes[i].use);
        tcc_fixit(line, name, FIXIT_REPLACE, dep_fixes[i].rename);
      }
      else
        tcc_note("%i:use %s", line, dep_fixes[i].use);
      return;
    }
}

/* error recovery (core/recover.c): no declaration's specifiers are open */
void attr_checks_recover(void)
{
  dep_pending_nb = dep_btype_depth = 0;
  attr_deprecated_suppress = 0;
  dep_call.s = NULL;
}

/* F's declaration is at LINE (0: the current line) of the file read now */
void attr_decl_place(SymLocalFacts *f, int line)
{
  const char *name = tcc_diag_file(NULL);
  if (!name)
    return;
  f->deprecated_file = name;
  f->deprecated_line = line ? line : attr_cur_line();
}

/* "note: declared here" at FILE:LINE (NAME interned), after the warning it
 * explains: error1 names the file diag_replay_file says */
static void attr_note_at(const char *name, int line, const char *msg)
{
  TCCState *s1 = tcc_state;
  const char *saved = s1->diag_replay_file;
  if (!name || !line || !s1->diag_last_shown || !tcc_diag_file(NULL))
    return;
  s1->diag_replay_file = name;
  tcc_note("%i:%s", line, msg);
  s1->diag_replay_file = saved;
}

static void dep_report(Sym *s, int line, ObjCall *oc)
{
  const SymLocalFacts *f = sym_facts_peek(s);
  const char *name = get_tok_str(s->v & ~(SYM_STRUCT | SYM_FIELD), NULL);
  int shown;
  if (f->deprecated_msg)
    shown = attr_warn(ATTR_OPT(warn_deprecated_declarations), line, "'%s' is deprecated: %s", name,
                      get_tok_str(f->deprecated_msg, NULL));
  else
    shown = attr_warn(ATTR_OPT(warn_deprecated_declarations), line, "'%s' is deprecated", name);
  if (shown)
    dep_fixit(s, name, line, oc);
  attr_note_at(f->deprecated_file, f->deprecated_line, "declared here");
}

/* A call of a deprecated sprintf, vsprintf, strcpy or strcat is reported
 * once its arguments are parsed (objsize_call_args_end): the fix-it to
 * snprintf, vsnprintf, strlcpy or strlcat needs the destination's size.  The
 * function waits here from its name to the call's start (objsize_call_begin
 * takes it); anything else in between reports it at once. */
static int dep_call_waits(Sym *s)
{
  const char *name;
  if ((s->type.t & (VT_BTYPE | VT_STATIC)) != VT_FUNC || (s->v & SYM_STRUCT) || tok != '(' ||
      (tcc_state->no_builtin_funcs & NO_BUILTIN_ALL) || tcc_state->reverse_funcargs)
    return 0;
  name = get_tok_str(s->v, NULL);
  return !strcmp(name, "sprintf") || !strcmp(name, "vsprintf") || !strcmp(name, "strcpy") || !strcmp(name, "strcat");
}

/* a waiting function that no call took: reported as any other use */
void attr_dep_call_flush(void)
{
  Sym *s = dep_call.s;
  if (!s)
    return;
  dep_call.s = NULL;
  dep_report(s, dep_call.line, NULL);
}

/* the call of FSYM begins: the line of its waiting report, else 0 */
int attr_dep_call_take(Sym *fsym)
{
  int line = dep_call.line;
  if (!dep_call.s)
    return 0;
  if (dep_call.s != fsym)
  {
    attr_dep_call_flush();
    return 0;
  }
  dep_call.s = NULL;
  return line ? line : -1;
}

/* the arguments of OC, a call of the deprecated FSYM named at LINE, are in */
void attr_dep_call_report(Sym *fsym, int line, ObjCall *oc)
{
  dep_report(fsym, line, oc);
}

/* S (SymAttr.attr_facts set) is named here: a function, a variable, an
 * enumerator, a typedef or a tag */
void attr_deprecated_use(Sym *s)
{
  const SymLocalFacts *f = sym_facts_peek(s);
  if (!f || !f->deprecated || attr_deprecated_suppress || !(tcc_state->warn_deprecated_declarations & WARN_ON))
    return;
  if (dep_btype_depth && dep_pending_nb < DEP_PENDING_MAX)
  {
    dep_pending[dep_pending_nb].s = s;
    dep_pending[dep_pending_nb].line = attr_cur_line();
    dep_pending_nb++;
    return;
  }
  attr_dep_call_flush();
  if (dep_call_waits(s))
  {
    dep_call.s = s;
    dep_call.line = attr_cur_line();
    return;
  }
  dep_report(s, attr_cur_line(), NULL);
}

int attr_btype_begin(void)
{
  dep_btype_depth++;
  return dep_pending_nb;
}

/* The specifiers parse_btype read since MARK: report the deprecated types
 * they named, unless they say deprecated themselves */
void attr_btype_end(int mark, AttributeDef *ad)
{
  int i;
  dep_btype_depth--;
  if (!ad->attr_deprecated)
    for (i = mark; i < dep_pending_nb; i++)
      dep_report(dep_pending[i].s, dep_pending[i].line, NULL);
  dep_pending_nb = mark;
}

/* The string literals parsed while a format argument is (primary.c): the
 * last one, and whether there was any */
static struct
{
  unsigned char armed, seen, wide;
  Sym *sym;
  CString str;
  /* a format_arg function's result whose argument was a literal (str) */
  char *fa_pending; /* the literal, until the call's result is pushed */
  SValue *fa_at;
  SValue fa_sv;
} fmt_cap;

/* ------------------------------------------------------------------------- */
/* -Wunused-result */

/* The value the last call left, when its function says nodiscard: an
 * expression statement (or a comma, a for clause) that drops exactly that
 * value drops the result.  Any other call clears it. */
static struct
{
  Sym *fsym; /* the function, or (by_type) the nodiscard tag of its result */
  SValue *at;
  SValue sv;
  unsigned char by_type;
} unused_res;

/* C23: [[nodiscard]] on a struct, union or enum is on every function
 * returning it */
static Sym *nodiscard_tag(CType *type)
{
  const SymLocalFacts *f;
  Sym *tag;
  if ((type->t & VT_BTYPE) != VT_STRUCT && !IS_ENUM(type->t))
    return NULL;
  tag = type->ref;
  /* warn_unused_result on a type is gcc's "attribute ignored" */
  if (!tag || !tag->a.attr_facts || !(f = sym_facts_peek(tag)) || f->unused_result != ATTR_NODISCARD)
    return NULL;
  return tag;
}

void attr_call_result(Sym *fsym)
{
  const SymLocalFacts *f;
  unused_res.fsym = NULL;
  if (fmt_cap.fa_pending)
  {
    /* `printf(_("%d"), x)`: this result is that literal for -Wformat */
    cstr_reset(&fmt_cap.str);
    cstr_cat(&fmt_cap.str, fmt_cap.fa_pending, strlen(fmt_cap.fa_pending) + 1);
    tcc_free(fmt_cap.fa_pending);
    fmt_cap.fa_pending = NULL;
    fmt_cap.fa_at = vtop;
    fmt_cap.fa_sv = *vtop;
  }
  if (!(tcc_state->warn_unused_result & WARN_ON) || (vtop->type.t & VT_BTYPE) == VT_VOID)
    return;
  unused_res.by_type = 0;
  if (!fsym || !fsym->a.attr_facts || !(f = sym_facts_peek(fsym)) || !f->unused_result)
  {
    if (!(fsym = nodiscard_tag(&vtop->type)))
      return;
    unused_res.by_type = 1;
  }
  unused_res.fsym = fsym;
  unused_res.at = vtop;
  unused_res.sv = *vtop;
}

/* vtop is about to be dropped */
void attr_check_discard(void)
{
  Sym *fs = unused_res.fsym;
  const SymLocalFacts *f;
  const char *name;
  if (!fs)
    return;
  unused_res.fsym = NULL;
  if (unused_res.at != vtop || vtop->r != unused_res.sv.r || vtop->vr != unused_res.sv.vr ||
      vtop->type.t != unused_res.sv.type.t || vtop->type.ref != unused_res.sv.type.ref ||
      vtop->c.i != unused_res.sv.c.i || vtop->sym != unused_res.sv.sym)
    return;
  f = sym_facts_peek(fs);
  if (unused_res.by_type)
  {
    char tname[256];
    type_to_str(tname, sizeof tname, &unused_res.sv.type, NULL);
    if (f->nodiscard_msg)
      attr_warn(ATTR_OPT(warn_unused_result), attr_cur_line(),
                "ignoring return value of type '%s', declared with attribute 'nodiscard': %s", tname,
                get_tok_str(f->nodiscard_msg, NULL));
    else
      attr_warn(ATTR_OPT(warn_unused_result), attr_cur_line(),
                "ignoring return value of type '%s', declared with attribute 'nodiscard'", tname);
    return;
  }
  name = get_tok_str(fs->v, NULL);
  if (f->unused_result == ATTR_NODISCARD)
  {
    if (f->nodiscard_msg)
      attr_warn(ATTR_OPT(warn_unused_result), attr_cur_line(),
                "ignoring return value of '%s', declared with attribute 'nodiscard': %s", name,
                get_tok_str(f->nodiscard_msg, NULL));
    else
      attr_warn(ATTR_OPT(warn_unused_result), attr_cur_line(),
                "ignoring return value of '%s', declared with attribute 'nodiscard'", name);
  }
  else
    attr_warn(ATTR_OPT(warn_unused_result), attr_cur_line(),
              "ignoring return value of '%s', declared with attribute 'warn_unused_result'", name);
}

/* ------------------------------------------------------------------------- */
/* -Wformat */


void attr_format_capture(Sym *lit, int wide)
{
  fmt_cap.seen = 1;
  fmt_cap.sym = lit;
  fmt_cap.wide = wide;
  if (!wide)
  {
    cstr_reset(&fmt_cap.str);
    cstr_cat(&fmt_cap.str, initstr.data, initstr.size);
  }
}

int attr_format_capturing(void)
{
  return fmt_cap.armed;
}

enum
{
  LM_NONE,
  LM_HH,
  LM_H,
  LM_L,
  LM_LL,
  LM_J,
  LM_Z,
  LM_T,
  LM_BIGL
};

/* What a format wants of fsym's call, if it says format(printf|scanf) and
 * any of the checks is on */
void attr_format_begin(FmtCheck *fc, Sym *fsym)
{
  const SymLocalFacts *f;
  memset(fc, 0, sizeof *fc);
  if (!fsym || !fsym->a.attr_facts || tcc_state->reverse_funcargs)
    return;
  if (!((tcc_state->warn_format | tcc_state->warn_format_security | tcc_state->warn_format_n) & WARN_ON))
    return;
  f = sym_facts_peek(fsym);
  if (!f)
    return;
  if (f->fmt_arg && fmt_cap.armed && f->fmt_arg != f->fmt_idx)
    fc->fa_idx = f->fmt_arg;
  if (f->fmt_kind != ATTR_FMT_PRINTF && f->fmt_kind != ATTR_FMT_SCANF && f->fmt_kind != ATTR_FMT_STRFTIME &&
      f->fmt_kind != ATTR_FMT_STRFMON)
    return;
  fc->kind = f->fmt_kind;
  fc->idx = f->fmt_idx;
  fc->first = f->fmt_first;
  fc->state = 1;
}

/* Argument ARGNO (from 0) is about to be parsed */
void attr_format_before(FmtCheck *fc, int argno)
{
  if (fc->fa_idx == argno + 1)
  {
    /* is this argument a literal? */
    fc->fa_saved_seen = fmt_cap.seen;
    fc->fa_saved_sym = fmt_cap.sym;
    fmt_cap.seen = 0;
    fmt_cap.sym = NULL;
  }
  if (fc->state == 1 && argno + 1 == fc->idx)
  {
    fc->saved_armed = fmt_cap.armed;
    fc->saved_seen = fmt_cap.seen;
    fmt_cap.armed = 1;
    fmt_cap.seen = 0;
    fmt_cap.sym = NULL;
  }
}

/* The conversion character's meaning */
static int fmt_conv_class(int kind, int c)
{
  if (kind == ATTR_FMT_STRFMON)
    return c == 'i' || c == 'n' ? 'f' : 0;
  if (kind == ATTR_FMT_PRINTF)
    switch (c)
    {
    case 'd': case 'i':
      return 'd';
    case 'o': case 'u': case 'x': case 'X': case 'b': case 'B':
      return 'u';
    case 'c': case 'C':
      return 'c';
    case 's': case 'S':
      return 's';
    case 'p':
      return 'p';
    case 'n':
      return 'n';
    case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A':
      return 'f';
    case 'm':
      return 'm';
    }
  else
    switch (c)
    {
    case 'd': case 'i': case 'n':
      return 'd';
    case 'o': case 'u': case 'x': case 'X': case 'b':
      return 'u';
    case 'c': case 's': case '[': case 'S': case 'C':
      return 's';
    case 'p':
      return 'p';
    case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A':
      return 'f';
    }
  return 0;
}

/* strftime: each conversion with the flags, width and E / O modifier it
 * takes (gcc's time_char_table; 'w' is a width) */
static const char *const strftime_convs[][2] = {
    {"AZa", "^#"},      {"Bb", "O^#"},     {"cx", "E"},   {"HIMSUWdmwj", "-_0Ow"}, {"p", "#"},
    {"X", "E"},         {"y", "EO-_0w"},   {"Y", "-_0EOw"}, {"C", "-_0EOw"},      {"D", ""},
    {"eVu", "-_0Ow"},   {"FRTnrt", ""},    {"g", "O-_0w"}, {"G", "-_0Ow"},        {"h", "^#"},
    {"z", "O"},         {"kls", "-_0Ow"},  {"P", ""}};

/* One strftime conversion, P after its '%': past it, NULL at the format's end */
static const char *fmt_strftime_conv(const char *p)
{
  char flags[8];
  const char *allowed = NULL;
  int nf = 0, width = 0, mod = 0, both = 0;
  unsigned i;
  while (*p && strchr("-_0^#", *p))
  {
    if (nf < 7 && !memchr(flags, *p, nf))
      flags[nf++] = *p;
    p++;
  }
  while (*p >= '0' && *p <= '9')
    width = 1, p++;
  while (*p == 'E' || *p == 'O')
  {
    if (mod && mod != *p)
      both = 1;
    mod = *p++;
  }
  if (!*p)
  {
    attr_warn(ATTR_OPT(warn_format), attr_cur_line(), "spurious trailing '%%' in format");
    return NULL;
  }
  if (both)
    attr_warn(ATTR_OPT(warn_format), attr_cur_line(),
              "use of 'E' modifier and 'O' modifier together in strftime format");
  for (i = 0; i < sizeof strftime_convs / sizeof strftime_convs[0]; i++)
    if (strchr(strftime_convs[i][0], *p))
      allowed = strftime_convs[i][1];
  if (!allowed)
  {
    attr_warn(ATTR_OPT(warn_format), attr_cur_line(), "unknown conversion type character '%c' in format", *p);
    return p + 1;
  }
  for (i = 0; i < (unsigned)nf; i++)
    if (!strchr(allowed, flags[i]))
      attr_warn(ATTR_OPT(warn_format), attr_cur_line(), "'%c' flag used with '%%%c' strftime format", flags[i], *p);
  if (width && !strchr(allowed, 'w'))
    attr_warn(ATTR_OPT(warn_format), attr_cur_line(), "field width used with '%%%c' strftime format", *p);
  if (mod && !both && !strchr(allowed, mod))
    attr_warn(ATTR_OPT(warn_format), attr_cur_line(), "'%c' modifier used with '%%%c' strftime format", mod, *p);
  return p + 1;
}

/* Parse up to the next argument the format consumes: 1 with fc->cur set
 * (fc->cur_star: a '*' width or precision), 0 at its end */
static int fmt_next(FmtCheck *fc)
{
  const char *p, *start;
  for (;;)
  {
    if (fc->stars)
    {
      fc->stars--;
      fc->cur_star = 1;
      return 1;
    }
    if (fc->have_conv)
    {
      fc->have_conv = 0;
      fc->cur_star = 0;
      return 1;
    }
    p = fc->fmt + fc->pos;
    while (*p && *p != '%')
      p++;
    if (!*p)
    {
      fc->pos = p - fc->fmt;
      return 0;
    }
    start = p++;
    if (*p == '%')
    {
      fc->pos = p + 1 - fc->fmt;
      continue;
    }
    if (fc->kind == ATTR_FMT_STRFTIME)
    {
      /* no arguments: the conversions alone, every one of them, as gcc */
      if (!(p = fmt_strftime_conv(p)))
      {
        fc->state = 0;
        return 0;
      }
      fc->pos = p - fc->fmt;
      continue;
    }
    /* %N$: operands by number are not checked */
    {
      const char *q = p;
      while (*q >= '0' && *q <= '9')
        q++;
      if (q != p && *q == '$')
      {
        fc->state = 0;
        return 0;
      }
    }
    int suppress = 0, lm = LM_NONE;
    if (fc->kind == ATTR_FMT_STRFMON)
    {
      /* %[=f^+(!-][width][#left][.right][L](i|n) */
      while (*p && (strchr("^+(!-", *p) || *p == '='))
        if (*p++ == '=' && *p)
          p++; /* the fill character */
      while (*p >= '0' && *p <= '9')
        p++;
      if (*p == '#')
        for (p++; *p >= '0' && *p <= '9'; p++)
          ;
      if (*p == '.')
        for (p++; *p >= '0' && *p <= '9'; p++)
          ;
      if (*p == 'L')
        lm = LM_BIGL, p++;
    }
    else if (fc->kind == ATTR_FMT_PRINTF)
    {
      while (*p && strchr("-+ #0'I", *p))
        p++;
      if (*p == '*')
        fc->stars++, p++;
      else
        while (*p >= '0' && *p <= '9')
          p++;
      if (*p == '.')
      {
        p++;
        if (*p == '*')
          fc->stars++, p++;
        else
          while (*p >= '0' && *p <= '9')
            p++;
      }
    }
    else
    {
      if (*p == '*')
        suppress = 1, p++;
      while (*p >= '0' && *p <= '9')
        p++;
      if (*p == 'm') /* allocate: char ** */
        p++;
    }
    switch (fc->kind == ATTR_FMT_STRFMON ? 0 : *p)
    {
    case 'h':
      lm = p[1] == 'h' ? (p++, LM_HH) : LM_H;
      p++;
      break;
    case 'l':
      lm = p[1] == 'l' ? (p++, LM_LL) : LM_L;
      p++;
      break;
    case 'q':
      lm = LM_LL, p++;
      break;
    case 'L':
      lm = LM_BIGL, p++;
      break;
    case 'j':
      lm = LM_J, p++;
      break;
    case 'z':
    case 'Z':
      lm = LM_Z, p++;
      break;
    case 't':
      lm = LM_T, p++;
      break;
    }
    if (!*p)
    {
      attr_warn(ATTR_OPT(warn_format), attr_cur_line(), "spurious trailing '%%' in format");
      fc->state = 0;
      return 0;
    }
    int cls = fmt_conv_class(fc->kind, *p);
    if (!cls)
    {
      attr_warn(ATTR_OPT(warn_format), attr_cur_line(), "unknown conversion type character '%c' in format", *p);
      fc->state = 0;
      return 0;
    }
    if (*p == '[')
    {
      p++;
      if (*p == '^')
        p++;
      if (*p == ']')
        p++;
      while (*p && *p != ']')
        p++;
      if (!*p)
      {
        attr_warn(ATTR_OPT(warn_format), attr_cur_line(), "no closing ']' for '%%[' format");
        fc->state = 0;
        return 0;
      }
    }
    if (cls == 'n' && fc->kind == ATTR_FMT_PRINTF)
      attr_warn(ATTR_OPT(warn_format_n), attr_cur_line(), "'%%n' in format writes through its argument");
    fc->pos = p + 1 - fc->fmt;
    fc->cur_start = start - fc->fmt;
    fc->cur_len = p + 1 - start;
    fc->cur_conv = *p;
    fc->cur_cls = cls;
    fc->cur_lm = lm;
    fc->have_conv = cls != 'm' && !suppress;
  }
}

/* The integer type a length modifier names: type.t bits (VT_BTYPE | VT_LONG) */
static int fmt_lm_int(int lm)
{
  switch (lm)
  {
  case LM_HH:
    return VT_BYTE;
  case LM_H:
    return VT_SHORT;
  case LM_L:
    return LONG_SIZE == 8 ? VT_LLONG | VT_LONG : VT_INT | VT_LONG;
  case LM_LL:
  case LM_J:
  case LM_BIGL:
    return VT_LLONG;
  case LM_Z:
    return VT_SIZE_T & (VT_BTYPE | VT_LONG);
  case LM_T:
    return VT_PTRDIFF_T & (VT_BTYPE | VT_LONG);
  }
  return VT_INT;
}

static const char *fmt_int_name(int lm, int is_unsigned)
{
  static const char *const s[] = {"int", "signed char", "short int", "long int", "long long int",
                                  "intmax_t", "signed size_t", "ptrdiff_t", "long long int"};
  static const char *const u[] = {"unsigned int", "unsigned char", "short unsigned int", "long unsigned int",
                                  "long long unsigned int", "uintmax_t", "size_t", "unsigned ptrdiff_t",
                                  "long long unsigned int"};
  return is_unsigned ? u[lm] : s[lm];
}

/* What the current conversion wants, for the message */
static void fmt_expected(FmtCheck *fc, char *buf, int size)
{
  int lm = fc->cur_lm, cls = fc->cur_cls;
  const char *star = fc->kind == ATTR_FMT_SCANF ? " *" : "";
  if (fc->cur_star)
  {
    pstrcpy(buf, size, "int");
    return;
  }
  switch (cls)
  {
  case 'd':
  case 'u':
    if (fc->kind == ATTR_FMT_PRINTF && (lm == LM_HH || lm == LM_H))
      lm = LM_NONE; /* promoted */
    if (fc->kind == ATTR_FMT_PRINTF && fc->cur_conv == 'n')
      star = " *";
    snprintf(buf, size, "%s%s", fmt_int_name(lm, cls == 'u'), star);
    return;
  case 'c':
    pstrcpy(buf, size, lm == LM_L || fc->cur_conv == 'C' ? "wint_t" : "int");
    return;
  case 's':
    pstrcpy(buf, size, lm == LM_L || fc->cur_conv == 'S' ? "wchar_t *" : "char *");
    return;
  case 'p':
    pstrcpy(buf, size, fc->kind == ATTR_FMT_SCANF ? "void **" : "void *");
    return;
  case 'n':
    snprintf(buf, size, "%s *", fmt_int_name(lm, 0));
    return;
  case 'f':
    if (fc->kind == ATTR_FMT_SCANF)
      pstrcpy(buf, size, lm == LM_BIGL ? "long double *" : lm == LM_L ? "double *" : "float *");
    else
      pstrcpy(buf, size, lm == LM_BIGL ? "long double" : "double");
    return;
  }
  pstrcpy(buf, size, "?");
}

static int is_long_double(int t)
{
  return (t & VT_BTYPE) == VT_LDOUBLE || (t & (VT_BTYPE | VT_LONG)) == (VT_DOUBLE | VT_LONG);
}

/* An integer of the rank fmt_lm_int names; signedness is not checked (gcc
 * leaves it to -Wformat-signedness) */
static int fmt_int_match(int t, int want)
{
  if (t & (VT_VECTOR | VT_COMPLEX))
    return 0;
  return (t & (VT_BTYPE | VT_LONG)) == want;
}

/* Does an argument of type TYPE suit the current conversion? */
static int fmt_type_ok(FmtCheck *fc, CType *type)
{
  int t = type->t, bt = t & VT_BTYPE, cls = fc->cur_cls, lm = fc->cur_lm;
  int is_ptr = bt == VT_PTR || bt == VT_FUNC;
  CType *pt = bt == VT_PTR ? pointed_type(type) : NULL;

  if (fc->kind == ATTR_FMT_STRFMON)
  {
    if (t & (VT_VECTOR | VT_COMPLEX))
      return 0;
    if (bt == VT_FLOAT)
      t = VT_DOUBLE; /* promoted */
    return lm == LM_BIGL ? is_long_double(t) : (t & (VT_BTYPE | VT_LONG)) == VT_DOUBLE;
  }
  if (fc->kind == ATTR_FMT_PRINTF && !(t & (VT_VECTOR | VT_COMPLEX)))
  {
    /* default argument promotions */
    if (bt == VT_BYTE || bt == VT_SHORT || bt == VT_BOOL)
      t = VT_INT, bt = VT_INT;
    else if (bt == VT_FLOAT)
      t = VT_DOUBLE, bt = VT_DOUBLE;
  }
  if (fc->cur_star)
    return fmt_int_match(t, VT_INT);
  if (fc->kind == ATTR_FMT_PRINTF)
    switch (cls)
    {
    case 'd':
    case 'u':
      if (fc->cur_conv == 'n')
        goto int_pointer;
      return fmt_int_match(t, lm == LM_HH || lm == LM_H ? VT_INT : fmt_lm_int(lm));
    case 'c':
      return fmt_int_match(t, VT_INT);
    case 's':
      if (lm == LM_L || fc->cur_conv == 'S')
        return is_ptr;
      return pt && (pt->t & VT_BTYPE) == VT_BYTE;
    case 'p':
      return is_ptr;
    case 'f':
      if (t & (VT_VECTOR | VT_COMPLEX))
        return 0;
      return lm == LM_BIGL ? is_long_double(t) : (t & (VT_BTYPE | VT_LONG)) == VT_DOUBLE;
    }
  else
    switch (cls)
    {
    case 'd':
    case 'u':
    int_pointer:
      if (!pt)
        return 0;
      if (lm == LM_HH)
        return (pt->t & VT_BTYPE) == VT_BYTE;
      return fmt_int_match(pt->t, fmt_lm_int(lm));
    case 's':
      if (lm == LM_L || fc->cur_conv == 'S' || fc->cur_conv == 'C')
        return is_ptr;
      return pt && ((pt->t & VT_BTYPE) == VT_BYTE || (pt->t & VT_BTYPE) == VT_PTR);
    case 'p':
      return pt && (pt->t & VT_BTYPE) == VT_PTR;
    case 'f':
      if (!pt || (pt->t & (VT_VECTOR | VT_COMPLEX)))
        return 0;
      if (lm == LM_BIGL)
        return is_long_double(pt->t);
      return (pt->t & (VT_BTYPE | VT_LONG)) == (lm == LM_L ? VT_DOUBLE : VT_FLOAT);
    }
  return 1;
}

/* Argument ARGNO (from 0) was parsed: vtop, before its conversion to the
 * parameter */
/* vtop is the literal fmt_cap holds: the string itself, or the result of a
 * format_arg call (gettext) given one */
static int fmt_vtop_literal(void)
{
  if (fmt_cap.fa_at == vtop && vtop->r == fmt_cap.fa_sv.r && vtop->vr == fmt_cap.fa_sv.vr &&
      vtop->c.i == fmt_cap.fa_sv.c.i && vtop->sym == fmt_cap.fa_sv.sym && vtop->type.t == fmt_cap.fa_sv.type.t)
    return 1;
  return fmt_cap.sym && vtop->sym == fmt_cap.sym && (vtop->r & (VT_VALMASK | VT_SYM)) == (VT_CONST | VT_SYM) &&
         vtop->c.i == 0 && !fmt_cap.wide;
}

void attr_format_arg(FmtCheck *fc, int argno)
{
  char want[64], have[256];
  if (fc->fa_idx == argno + 1)
  {
    if (fmt_vtop_literal())
      fc->fa_str = tcc_strdup(fmt_cap.str.data ? fmt_cap.str.data : "");
    fmt_cap.fa_at = NULL;
    fmt_cap.seen |= fc->fa_saved_seen;
    fmt_cap.sym = fc->fa_saved_sym;
  }
  if (fc->state == 1 && argno + 1 == fc->idx)
  {
    int literal = fmt_vtop_literal();
    int seen = fmt_cap.seen;
    fmt_cap.armed = fc->saved_armed;
    fmt_cap.seen = fc->saved_seen | seen;
    fc->state = 0;
    fmt_cap.fa_at = NULL;
    if (literal)
    {
      fc->fmt = tcc_strdup(fmt_cap.str.data ? fmt_cap.str.data : "");
      fc->state = fc->first ? 2 : 3; /* 3: a va_list, the format alone is checked */
    }
    else if (!seen)
      fc->nonliteral = 1;
    return;
  }
  if (fc->state != 2 || argno + 1 < fc->first)
    return;
  if (!fmt_next(fc))
  {
    if (fc->state == 2 && !fc->extra_reported)
    {
      fc->extra_reported = 1;
      attr_warn(ATTR_OPT(warn_format), attr_cur_line(), "too many arguments for format");
    }
    return;
  }
  if (fmt_type_ok(fc, &vtop->type))
    return;
  fmt_expected(fc, want, sizeof want);
  {
    CType shown = vtop->type; /* an array argument is its first element's address */
    shown.t &= ~(VT_ARRAY | VT_VLA | VT_STORAGE);
    type_to_str(have, sizeof have, &shown, NULL);
  }
  if (fc->cur_star)
    attr_warn(ATTR_OPT(warn_format), attr_cur_line(),
              "field width or precision in '%.*s' expects argument of type 'int', but argument %d has type '%s'",
              fc->cur_len, fc->fmt + fc->cur_start, argno + 1, have);
  else
    attr_warn(ATTR_OPT(warn_format), attr_cur_line(),
              "format '%.*s' expects argument of type '%s', but argument %d has type '%s'", fc->cur_len,
              fc->fmt + fc->cur_start, want, argno + 1, have);
}

/* The call's arguments are all parsed: NB_ARGS of them */
void attr_format_end(FmtCheck *fc, int nb_args)
{
  char want[64];
  if (fc->fa_idx)
  {
    /* the result, once pushed, stands for the literal (attr_call_result) */
    tcc_free(fmt_cap.fa_pending);
    fmt_cap.fa_pending = fc->fa_str;
    fc->fa_str = NULL;
  }
  if (fc->state == 1)
  {
    /* never reached the format (too few arguments): the call is an error */
    fmt_cap.armed = fc->saved_armed;
    fmt_cap.seen |= fc->saved_seen;
  }
  if (fc->nonliteral && fc->first && nb_args <= fc->idx)
    attr_warn(ATTR_OPT(warn_format_security), attr_cur_line(),
              "format not a string literal and no format arguments");
  if (fc->state == 3)
  {
    /* a va_list: what the format says alone (%n, unknown conversions) */
    while (fc->state == 3 && fmt_next(fc))
      ;
  }
  else if (fc->state == 2)
  {
    while (fc->state == 2 && fmt_next(fc))
    {
      fmt_expected(fc, want, sizeof want);
      if (fc->cur_star)
        attr_warn(ATTR_OPT(warn_format), attr_cur_line(),
                  "field width or precision in '%.*s' expects a matching 'int' argument", fc->cur_len,
                  fc->fmt + fc->cur_start);
      else
        attr_warn(ATTR_OPT(warn_format), attr_cur_line(), "format '%.*s' expects a matching '%s' argument",
                  fc->cur_len, fc->fmt + fc->cur_start, want);
    }
  }
  tcc_free(fc->fmt);
  fc->fmt = NULL;
  fc->state = 0;
}

/* ------------------------------------------------------------------------- */

/* A unit starts: nothing seen, nothing pending */
void attr_checks_reset(void)
{
  dep_pending_nb = dep_btype_depth = 0;
  attr_deprecated_suppress = 0;
  dep_call.s = NULL;
  fe_conv_quiet = 0;
  unused_res.fsym = NULL;
  fmt_cap.armed = fmt_cap.seen = 0;
  fmt_cap.sym = NULL;
  tcc_free(fmt_cap.fa_pending);
  fmt_cap.fa_pending = NULL;
  fmt_cap.fa_at = NULL;
  cstr_free(&fmt_cap.str);
  memset(&fmt_cap.str, 0, sizeof fmt_cap.str);
}
