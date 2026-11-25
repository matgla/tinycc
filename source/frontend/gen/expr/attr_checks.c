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

/* The checks of the calls being parsed (a call in another's arguments is
 * one more), here rather than in the caller's frame: an error in the middle
 * of a call leaves its records for attr_checks_recover to free.  Past the
 * depth a call is not checked (fmt_off: nothing to do). */
#define FMT_CALL_DEPTH 32
static FmtCheck fmt_calls[FMT_CALL_DEPTH], fmt_off;
static int fmt_ncalls;

static void fmt_call_free(FmtCheck *fc)
{
  for (int i = 0; i < fc->nfmts; i++)
    tcc_free(fc->fmts[i]);
  tcc_free(fc->fmts);
  tcc_free(fc->args);
  tcc_free(fc->fa_str);
  memset(fc, 0, sizeof *fc);
}

static void fmt_cond_reset(void);

/* error recovery (core/recover.c): no declaration's specifiers are open */
void attr_checks_recover(void)
{
  dep_pending_nb = dep_btype_depth = 0;
  attr_deprecated_suppress = 0;
  dep_call.s = NULL;
  fmt_cond_reset();
  while (fmt_ncalls)
    fmt_call_free(&fmt_calls[--fmt_ncalls]);
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
  /* the conditional parsed last (`c ? "%d" : _("%s")`): its arms' literals,
     for its result (res_at, res_sv) */
  struct FmtArms
  {
    unsigned char n, nonlit; /* nonlit: an arm that is no literal */
    char *s[8];
  } res;
  SValue *res_at;
  SValue res_sv;
} fmt_cap;
typedef struct FmtArms FmtArms;
#define FMT_ARMS_MAX 8
#define FMT_COND_DEPTH 16
/* the conditionals being parsed inside a format argument */
static FmtArms fmt_arms_pool[FMT_COND_DEPTH];
static int fmt_arms_depth;

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
 * any of the checks is on; attr_format_end ends one that does */
FmtCheck *attr_format_begin(Sym *fsym)
{
  const SymLocalFacts *f;
  FmtCheck *fc;
  if (!fsym || !fsym->a.attr_facts || tcc_state->reverse_funcargs)
    return &fmt_off;
  if (!((tcc_state->warn_format | tcc_state->warn_format_security | tcc_state->warn_format_n) & WARN_ON))
    return &fmt_off;
  f = sym_facts_peek(fsym);
  if (!f || fmt_ncalls == FMT_CALL_DEPTH)
    return &fmt_off;
  fc = &fmt_calls[fmt_ncalls];
  memset(fc, 0, sizeof *fc);
  if (f->fmt_arg && fmt_cap.armed && f->fmt_arg != f->fmt_idx)
    fc->fa_idx = f->fmt_arg;
  if (f->fmt_kind == ATTR_FMT_PRINTF || f->fmt_kind == ATTR_FMT_SCANF || f->fmt_kind == ATTR_FMT_STRFTIME ||
      f->fmt_kind == ATTR_FMT_STRFMON)
  {
    fc->kind = f->fmt_kind;
    fc->idx = f->fmt_idx;
    fc->first = f->fmt_first;
    fc->state = 1;
  }
  if (!(fc->kind | fc->fa_idx))
    return &fmt_off;
  fmt_ncalls++;
  return fc;
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
static const char *fmt_strftime_conv(const char *p, int line)
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
    attr_warn(ATTR_OPT(warn_format), line, "spurious trailing '%%' in format");
    return NULL;
  }
  if (both)
    attr_warn(ATTR_OPT(warn_format), line,
              "use of 'E' modifier and 'O' modifier together in strftime format");
  for (i = 0; i < sizeof strftime_convs / sizeof strftime_convs[0]; i++)
    if (strchr(strftime_convs[i][0], *p))
      allowed = strftime_convs[i][1];
  if (!allowed)
  {
    attr_warn(ATTR_OPT(warn_format), line, "unknown conversion type character '%c' in format", *p);
    return p + 1;
  }
  for (i = 0; i < (unsigned)nf; i++)
    if (!strchr(allowed, flags[i]))
      attr_warn(ATTR_OPT(warn_format), line, "'%c' flag used with '%%%c' strftime format", flags[i], *p);
  if (width && !strchr(allowed, 'w'))
    attr_warn(ATTR_OPT(warn_format), line, "field width used with '%%%c' strftime format", *p);
  if (mod && !both && !strchr(allowed, mod))
    attr_warn(ATTR_OPT(warn_format), line, "'%c' modifier used with '%%%c' strftime format", mod, *p);
  return p + 1;
}

/* One format checked against the arguments kept (attr_format_end) */
typedef struct FmtRun
{
  const char *fmt;
  const FmtArgRec *args;
  int kind, first, nargs, pos, line, end_line, fmt_line;
  /* the conversion (or '*') checked now */
  int cur_start, cur_len, cur_star, cur_conv, cur_cls, cur_lm;
  /* `%N$` operands: -1 not known yet, 1 every one numbered, 0 none */
  int dollar, dollar_max, seq;
  unsigned char *used; /* by number: the arguments a `$` names */
  int used_cap;
  unsigned char stop, extra, dollar_extra;
  int extra_line;
} FmtRun;

static void fmt_expected(FmtRun *r, char *buf, int size);
static int fmt_type_ok(FmtRun *r, CType *type);

/* An operand number `N$` at *PP (printf and scanf): N, past it; 0 when
 * there is none (an error when NEEDED); -1 when the check stops (said) */
static int fmt_dollar(FmtRun *r, const char **pp, int needed)
{
  const char *p = *pp;
  int n = 0, over = 0;
  if (r->kind != ATTR_FMT_PRINTF && r->kind != ATTR_FMT_SCANF)
    return 0;
  for (; *p >= '0' && *p <= '9'; p++)
    if (n >= 100000)
      over = 1;
    else
      n = n * 10 + (*p - '0');
  if (p == *pp || *p != '$')
  {
    if (!needed)
      return 0;
    attr_warn(ATTR_OPT(warn_format), r->fmt_line, "missing $ operand number in format");
    return -1;
  }
  *pp = p + 1;
  if (!r->dollar)
  {
    attr_warn(ATTR_OPT(warn_format), r->fmt_line, "'$' operand number used after format without operand number");
    return -1;
  }
  if (over || !n || (r->first && n > r->nargs))
  {
    attr_warn(ATTR_OPT(warn_format), r->fmt_line, "operand number out of range in format");
    return -1;
  }
  r->dollar = 1;
  /* used, whether or not the conversion takes it (gcc) */
  if (n > r->used_cap)
  {
    int cap = n < 16 ? 16 : 2 * n;
    r->used = tcc_realloc(r->used, cap);
    memset(r->used + r->used_cap, 0, cap - r->used_cap);
    r->used_cap = cap;
  }
  r->used[n - 1] = 1;
  if (n > r->dollar_max)
    r->dollar_max = n;
  return n;
}

/* The conversion read last (r->cur_*) takes format argument K (from 1) */
static void fmt_use(FmtRun *r, int k, int star)
{
  char want[64], have[256];
  const FmtArgRec *a;
  r->cur_star = star;
  if (!r->first)
    return; /* a va_list */
  if (k > r->nargs)
  {
    fmt_expected(r, want, sizeof want);
    if (star)
      attr_warn(ATTR_OPT(warn_format), r->end_line, "field width or precision in '%.*s' expects a matching 'int' argument",
                r->cur_len, r->fmt + r->cur_start);
    else
      attr_warn(ATTR_OPT(warn_format), r->end_line, "format '%.*s' expects a matching '%s' argument", r->cur_len,
                r->fmt + r->cur_start, want);
    return;
  }
  a = &r->args[k - 1];
  if (fmt_type_ok(r, (CType *)&a->type))
    return;
  fmt_expected(r, want, sizeof want);
  {
    CType shown = a->type; /* an array argument is its first element's address */
    shown.t &= ~(VT_ARRAY | VT_VLA | VT_STORAGE);
    type_to_str(have, sizeof have, &shown, NULL);
  }
  if (star)
    attr_warn(ATTR_OPT(warn_format), a->line,
              "field width or precision in '%.*s' expects argument of type 'int', but argument %d has type '%s'",
              r->cur_len, r->fmt + r->cur_start, r->first + k - 1, have);
  else
    attr_warn(ATTR_OPT(warn_format), a->line,
              "format '%.*s' expects argument of type '%s', but argument %d has type '%s'", r->cur_len,
              r->fmt + r->cur_start, want, r->first + k - 1, have);
}

/* One directive from r->pos on, its arguments checked: 0 at the format's
 * end or when the check stops (r->stop) */
static int fmt_directive(FmtRun *r)
{
  const char *p = r->fmt + r->pos, *start;
  int main_n, nstar = 0, star_n[2], suppress = 0, lm = LM_NONE, cls, i;
  while (*p && *p != '%')
    p++;
  if (!*p)
  {
    r->pos = p - r->fmt;
    return 0;
  }
  start = p++;
  if (*p == '%')
  {
    r->pos = p + 1 - r->fmt;
    return 1;
  }
  /* said at the line of the argument it reaches (the call's end past them) */
  r->line = r->seq < r->nargs ? r->args[r->seq].line : r->end_line;
  if (r->kind == ATTR_FMT_STRFTIME)
  {
    /* no arguments: the conversions alone, every one of them, as gcc */
    if (!(p = fmt_strftime_conv(p, r->line)))
      goto stop;
    r->pos = p - r->fmt;
    return 1;
  }
  if ((main_n = fmt_dollar(r, &p, 0)) < 0)
    goto stop;
  if (r->kind == ATTR_FMT_STRFMON)
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
  else if (r->kind == ATTR_FMT_PRINTF)
  {
    while (*p && strchr("-+ #0'I", *p))
      p++;
    /* the width, then the precision: a number, '*' or '*N$' */
    for (i = 0; i < 2; i++)
    {
      if (i && *p++ != '.')
      {
        p--;
        break;
      }
      if (*p == '*')
      {
        int n;
        p++;
        if ((n = fmt_dollar(r, &p, r->dollar == 1)) < 0)
          goto stop;
        if (!n)
          r->dollar = 0;
        star_n[nstar++] = n;
      }
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
  switch (r->kind == ATTR_FMT_STRFMON ? 0 : *p)
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
    attr_warn(ATTR_OPT(warn_format), r->line, "spurious trailing '%%' in format");
    goto stop;
  }
  cls = fmt_conv_class(r->kind, *p);
  if (!cls)
  {
    attr_warn(ATTR_OPT(warn_format), r->line, "unknown conversion type character '%c' in format", *p);
    goto stop;
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
      attr_warn(ATTR_OPT(warn_format), r->line, "no closing ']' for '%%[' format");
      goto stop;
    }
  }
  if (cls == 'n' && r->kind == ATTR_FMT_PRINTF)
    attr_warn(ATTR_OPT(warn_format_n), r->line, "'%%n' in format writes through its argument");
  r->pos = p + 1 - r->fmt;
  r->cur_start = start - r->fmt;
  r->cur_len = p + 1 - start;
  r->cur_conv = *p;
  r->cur_cls = cls;
  r->cur_lm = lm;
  for (i = 0; i < nstar; i++)
    fmt_use(r, star_n[i] ? star_n[i] : ++r->seq, 1);
  if (cls == 'm' || suppress)
  {
    /* takes no argument */
    if (main_n)
      attr_warn(ATTR_OPT(warn_format), r->fmt_line,
                suppress ? "operand number specified with suppressed assignment"
                         : "operand number specified for format taking no argument");
    return 1;
  }
  if (!main_n)
  {
    if (r->dollar == 1)
    {
      attr_warn(ATTR_OPT(warn_format), r->fmt_line, "missing $ operand number in format");
      goto stop;
    }
    r->dollar = 0;
    main_n = ++r->seq;
  }
  fmt_use(r, main_n, 0);
  return 1;
stop:
  r->stop = 1;
  return 0;
}

/* The whole format R holds: r->extra / r->dollar_extra when it leaves
 * arguments over (said by the caller, which knows the other arms) */
static void fmt_run(FmtRun *r)
{
  int i, gap = 0;
  while (fmt_directive(r))
    ;
  if (r->stop)
    return;
  if (r->dollar == 1)
  {
    /* gcc's finish_dollar_format_checking; scanf may skip a pointer */
    for (i = 0; i < r->dollar_max; i++)
      if (!r->used[i])
      {
        if (r->kind == ATTR_FMT_SCANF && r->first && i < r->nargs &&
            (r->args[i].type.t & VT_BTYPE) == VT_PTR)
          gap = 1;
        else
          attr_warn(ATTR_OPT(warn_format), r->fmt_line,
                    "format argument %d unused before used argument %d in '$'-style format", i + 1,
                    r->dollar_max);
      }
    if (gap || (r->first && r->dollar_max < r->nargs))
      r->dollar_extra = 1;
  }
  else if (r->first && r->seq < r->nargs)
  {
    r->extra = 1;
    r->extra_line = r->args[r->seq].line;
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
static void fmt_expected(FmtRun *fc, char *buf, int size)
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
static int fmt_type_ok(FmtRun *fc, CType *type)
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

/* vtop is the value SV was, at AT */
static int fmt_vtop_is(const SValue *at, const SValue *sv)
{
  return at == vtop && vtop->r == sv->r && vtop->vr == sv->vr && vtop->c.i == sv->c.i && vtop->sym == sv->sym &&
         vtop->type.t == sv->type.t;
}

/* vtop is the literal fmt_cap holds: the string itself, or the result of a
 * format_arg call (gettext) given one */
static int fmt_vtop_literal(void)
{
  if (fmt_cap.fa_at && fmt_vtop_is(fmt_cap.fa_at, &fmt_cap.fa_sv))
    return 1;
  return fmt_cap.sym && vtop->sym == fmt_cap.sym && (vtop->r & (VT_VALMASK | VT_SYM)) == (VT_CONST | VT_SYM) &&
         vtop->c.i == 0 && !fmt_cap.wide;
}

/* vtop is the result of the conditional parsed last */
static int fmt_vtop_cond(void)
{
  return fmt_cap.res_at && fmt_vtop_is(fmt_cap.res_at, &fmt_cap.res_sv);
}

static void fmt_arms_free(FmtArms *a)
{
  for (int i = 0; i < a->n; i++)
    tcc_free(a->s[i]);
  a->n = a->nonlit = 0;
}

/* no conditional open (error recovery, a new unit) */
static void fmt_cond_reset(void)
{
  while (fmt_arms_depth)
    fmt_arms_free(&fmt_arms_pool[--fmt_arms_depth]);
  fmt_cap.res_at = NULL;
}

/* A conditional begins inside a format argument (expr/cond.c): its arms
 * are each a format, as gcc checks them -- a literal, a format_arg call
 * given one, or another conditional of them */
void *attr_format_cond_begin(void)
{
  FmtArms *a;
  if (fmt_arms_depth >= FMT_COND_DEPTH)
    return NULL;
  a = &fmt_arms_pool[fmt_arms_depth++];
  a->n = a->nonlit = 0;
  return a;
}

/* vtop is an arm of ARMS; not LIVE: the arm a constant condition drops
 * (folded away, as gcc) */
void attr_format_cond_arm(void *arms, int live)
{
  FmtArms *a = arms;
  if (!a || !live)
    return;
  if (fmt_vtop_cond())
  {
    for (int i = 0; i < fmt_cap.res.n; i++)
      if (a->n < FMT_ARMS_MAX)
        a->s[a->n++] = fmt_cap.res.s[i];
      else
        tcc_free(fmt_cap.res.s[i]), a->nonlit = 1;
    a->nonlit |= fmt_cap.res.nonlit;
    fmt_cap.res.n = fmt_cap.res.nonlit = 0;
    fmt_cap.res_at = NULL;
  }
  else if (fmt_vtop_literal() && a->n < FMT_ARMS_MAX)
    a->s[a->n++] = tcc_strdup(fmt_cap.str.data ? fmt_cap.str.data : "");
  else
    a->nonlit = 1;
  fmt_cap.fa_at = NULL;
}

/* The conditional ARMS is parsed: vtop is its result */
void attr_format_cond_end(void *arms)
{
  FmtArms *a = arms;
  if (!a)
    return;
  fmt_arms_depth--;
  fmt_arms_free(&fmt_cap.res);
  fmt_cap.res = *a;
  a->n = 0;
  fmt_cap.res_at = vtop;
  fmt_cap.res_sv = *vtop;
}

/* Argument ARGNO (from 0) was parsed: vtop, before its conversion to the
 * parameter */
void attr_format_arg(FmtCheck *fc, int argno)
{
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
    int seen = fmt_cap.seen;
    fmt_cap.armed = fc->saved_armed;
    fmt_cap.seen = fc->saved_seen | seen;
    fc->state = 0;
    fc->fmt_line = attr_cur_line();
    if (fmt_vtop_cond())
    {
      /* each arm a format; one that is not a literal is not checked */
      if (fmt_cap.res.n)
      {
        fc->fmts = tcc_malloc(fmt_cap.res.n * sizeof *fc->fmts);
        memcpy(fc->fmts, fmt_cap.res.s, fmt_cap.res.n * sizeof *fc->fmts);
      }
      fc->nfmts = fmt_cap.res.n;
      fc->nonliteral = fmt_cap.res.nonlit;
      fmt_cap.res.n = fmt_cap.res.nonlit = 0;
    }
    else if (fmt_vtop_literal())
    {
      fc->fmts = tcc_malloc(sizeof *fc->fmts);
      fc->fmts[0] = tcc_strdup(fmt_cap.str.data ? fmt_cap.str.data : "");
      fc->nfmts = 1;
    }
    else if (!seen)
      fc->nonliteral = 1;
    fmt_cap.fa_at = fmt_cap.res_at = NULL;
    if (fc->nfmts)
      fc->state = fc->first ? 2 : 3; /* 3: a va_list, the format alone is checked */
    return;
  }
  if (fc->state != 2 || argno + 1 < fc->first)
    return;
  /* kept for the end of the call: a `%N$` can name any of them */
  if (!(fc->nargs & 7))
    fc->args = tcc_realloc(fc->args, (fc->nargs + 8) * sizeof *fc->args);
  fc->args[fc->nargs].type = vtop->type;
  fc->args[fc->nargs].line = attr_cur_line();
  fc->nargs++;
}

/* The call's arguments are all parsed: NB_ARGS of them */
void attr_format_end(FmtCheck *fc, int nb_args)
{
  int i, other = 0, extra = 0, dollar_extra = 0, extra_line = 0, end_line;
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
  end_line = attr_cur_line();
  for (i = 0; i < fc->nfmts; i++)
  {
    FmtRun r;
    memset(&r, 0, sizeof r);
    r.fmt = fc->fmts[i];
    r.args = fc->args;
    r.nargs = fc->state == 2 ? fc->nargs : 0;
    r.kind = fc->kind;
    r.first = fc->state == 2 ? fc->first : 0;
    r.end_line = end_line;
    r.fmt_line = fc->fmt_line;
    r.dollar = -1;
    fmt_run(&r);
    tcc_free(r.used);
    tcc_free(fc->fmts[i]);
    if (r.extra && !extra_line)
      extra_line = r.extra_line;
    extra += r.extra;
    dollar_extra += r.dollar_extra;
    other += !r.extra && !r.dollar_extra;
  }
  /* arguments left over by every arm (gcc: none of them non-literal) */
  if (!other && !fc->nonliteral)
  {
    if (extra)
      attr_warn(ATTR_OPT(warn_format), extra_line, "too many arguments for format");
    if (dollar_extra)
      attr_warn(ATTR_OPT(warn_format), fc->fmt_line, "unused arguments in '$'-style format");
  }
  fc->nfmts = 0; /* each freed above */
  fmt_call_free(fc);
  if (fmt_ncalls && fc == &fmt_calls[fmt_ncalls - 1])
    fmt_ncalls--;
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
  fmt_cond_reset();
  while (fmt_ncalls)
    fmt_call_free(&fmt_calls[--fmt_ncalls]);
  fmt_arms_free(&fmt_cap.res);
  cstr_free(&fmt_cap.str);
  memset(&fmt_cap.str, 0, sizeof fmt_cap.str);
}
