/*
 *  TCC format - lays out C and yasc source (tcc -format)
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

/* What it changes and what it keeps: docs/tcc_format.md. #included by
   tcctools.c.

   One pass over raw tokens (no preprocessing, so macros, #if branches and
   comments are all seen as written).  The layout owns indentation, the
   spaces between tokens, where block braces go and one statement per line;
   it keeps the author's line breaks inside a statement (as zig fmt does,
   nothing is re-wrapped to a column limit).  What comes out is lexed again
   and must be the same tokens as what went in, or nothing is written. */

#ifndef _WIN32
#include <fnmatch.h>
#else
#define fnmatch(p, s, f) strcmp(p, s)
#define strtok_r(s, d, sv) strtok(s, d)
#endif

enum
{
  FK_ID,
  FK_NUM,
  FK_STR,
  FK_PUNCT,
  FK_LCOM,
  FK_BCOM,
  FK_PP
};

typedef struct FmtTok
{
  int start, len;
  int line, col; /* source line (0-based) and column (tabs at 8) */
  int nl;        /* newlines between it and the token before */
  int gap;       /* blank columns before it on its line (0 after a newline) */
  unsigned char kind;
  unsigned char verbatim; /* 1: body of #if 0 (skipped), 2: format off (state only) */
} FmtTok;

/* the role a token plays, decided before it is written */
enum
{
  R_NONE,
  R_UNARY,
  R_BINARY,
  R_PTR,
  R_POSTFIX,
  R_CAST_CLOSE,
  R_CTRL_CLOSE,
  R_ARG_CLOSE, /* borrows(x) owned(f) c_api(T) union(k) __attribute__((..)) */
  R_IDX_OPEN,
  R_IDX_CLOSE,
  R_TYPEBR_OPEN, /* []T [:0]T, designators [3] = */
  R_TYPEBR_CLOSE,
  R_ANGLE_OPEN, /* cast<...> */
  R_ANGLE_CLOSE,
  R_BLOCK_OPEN,
  R_BLOCK_CLOSE,
  R_INIT_OPEN,
  R_INIT_CLOSE,
  R_LIST_OPEN, /* enum / error body: { A, B } */
  R_LIST_CLOSE,
  R_LABEL_COLON,
  R_SLICE_COLON,
  R_GENERIC_COLON,
  R_BAR_OPEN, /* catch |e| */
  R_BAR_CLOSE,
  R_AT
};

/* frames: what the token stream is inside */
enum
{
  FF_BLOCK,   /* { } holding statements or members */
  FF_PENDING, /* the one-statement body of a braceless if/for/while/else/do */
  FF_PAREN,
  FF_BRACKET,
  FF_ANGLE,
  FF_INIT, /* braced initializer: the author's layout is kept */
  FF_LIST  /* enum / error body */
};

enum
{
  FB_CODE,   /* a plain block: ends a statement */
  FB_CTRL,   /* the body of if/for/while/switch/else/do or a case */
  FB_FUNC,   /* a function body */
  FB_STRUCT, /* struct/union members: the declaration goes on after } */
  FB_EXPR,   /* ({ ... }), catch |e| { ... }: the statement goes on after } */
  FB_EXTERN  /* extern "C" { ... }: not indented */
};

enum
{
  KWC_NONE,
  KWC_IF,
  KWC_FOR,
  KWC_WHILE,
  KWC_SWITCH,
  KWC_ELSE,
  KWC_DO,
  KWC_DOWHILE,
  KWC_LABEL,
  KWC_MACRO
};

/* paren flags */
#define FP_CAST_CAND 1 /* could be a cast: (T)x */
#define FP_DECL 2      /* a parameter list of a declaration */
#define FP_GENERIC 4
#define FP_ARG 8 /* borrows(..), owned(..), __attribute__((..)), union(k) */
#define FP_TYPEISH 16
#define FP_HASTYPE 32
#define FP_HASSTAR 64
#define FP_FOR 128

typedef struct FmtFrame
{
  unsigned char kind, sub, kw;
  int flags;
  int indent; /* BLOCK/PENDING: the indent of what is inside; brackets: the
                 column of a continued line (-1 until known) */
  int base;   /* where its closer goes on a line of its own */
  int ternary;
  int qcol; /* column of its last '?' (a continued ':' goes under it) */
  int acol; /* column of the operand after its last '=' or return */
  int ntok; /* paren: tokens directly inside */
  /* the enclosing statement, restored after a block */
  int s_stmt_indent, s_prefix, s_tag;
} FmtFrame;

typedef struct FmtText
{
  char *data;
  int size, cap;
} FmtText;

typedef struct FmtAlign
{
  int line;     /* output line */
  int ws_start; /* where the blank before the comment begins */
  int cstart;   /* where the comment begins */
  int code_col; /* column at ws_start (-1: a comment line continuing the one above) */
  int src_col;
} FmtAlign;

typedef struct FmtSnap
{
  FmtFrame *frames;
  int nframes;
  int at_start, stmt_indent, prefix, tag, case_lbl, pending_ctrl, expect_dowhile;
  int have_first, first_dead;
  struct FmtSnap *first; /* the state at the end of the first branch */
} FmtSnap;

typedef struct FmtStyle
{
  int indent, cont;
  int wrap_func, wrap_struct, wrap_ctrl, wrap_else;
  int short_case; /* case X: stmt; stays on one line when written so */
  int disable;
} FmtStyle;

typedef struct FmtState
{
  const char *file;
  const char *src;
  int len;
  FmtTok *toks;
  int ntoks, captoks;
  int *line_off;
  int nlines;
  unsigned char *role;
  FmtStyle style;

  FmtFrame *frames;
  int nframes, capframes;
  FmtSnap *snaps;
  int nsnaps, capsnaps;

  /* the statement */
  int at_start;    /* the next significant token begins a statement */
  int stmt_indent; /* indent of the line the statement began on */
  int stmt_tok;
  int prefix;       /* everything so far in the declaration looks like a type */
  int tag;          /* 1: struct/union seen, its body may follow; 2: enum/error */
  int tag_ids;      /* identifiers after the tag keyword */
  int case_lbl;     /* between case/default and its ':' */
  int pending_ctrl; /* the next { is the body of this KWC_ */
  int expect_dowhile;
  int need_break; /* the next significant token starts a line */
  int just_opened; /* the last significant token opened a block */
  int prev;        /* last significant token */
  int prev_any;    /* last token written (any kind) */

  /* the names this file declares as types */
  char **types;
  int ntypes, captypes;

  /* output */
  FmtText out, junk;
  FmtText *sink;
  int col, line_indent, out_line;
  FmtAlign *aligns;
  int naligns, capaligns;
  int errors;
} FmtState;

/* ------------------------------------------------------------------ */

static void ft_grow(void **p, int *cap, int need, int elem)
{
  if (need <= *cap)
    return;
  *cap = *cap ? *cap * 2 : 64;
  while (*cap < need)
    *cap *= 2;
  *p = tcc_realloc(*p, (size_t)*cap * elem);
}

static void ft_cat(FmtText *t, const char *s, int n)
{
  ft_grow((void **)&t->data, &t->cap, t->size + n + 1, 1);
  memcpy(t->data + t->size, s, n);
  t->size += n;
  t->data[t->size] = 0;
}

static void ft_putc(FmtText *t, char c)
{
  ft_cat(t, &c, 1);
}

static void ft_spaces(FmtText *t, int n)
{
  while (n-- > 0)
    ft_putc(t, ' ');
}

static int ft_is_idchar(int c)
{
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '$' ||
         c >= 0x80;
}

/* ------------------------------------------------------------------ */
/* the lexer: raw tokens, comments and directives kept */

static const char *const ft_puncts[] = {"...", "<<=", ">>=", "->", "++", "--", "<<", ">>", "<=", ">=", "==",
                                        "!=",  "&&",  "||",  "*=", "/=", "%=", "+=", "-=", "&=", "^=", "|=",
                                        "##",  "::",  NULL};

static int ft_error(FmtState *st, int line, const char *msg)
{
  fprintf(stderr, "%s:%d: error: %s\n", st->file, line + 1, msg);
  st->errors++;
  return -1;
}

static int ft_skip_quoted(const char *s, int i, int end)
{
  int q = s[i++];
  while (i < end && s[i] != q && s[i] != '\n') {
    if (s[i] == '\\' && i + 1 < end)
      i++;
    i++;
  }
  return i < end && s[i] == q ? i + 1 : i;
}

/* lex s into *ptoks (also the re-lex of the output) */
static int ft_lex(FmtState *st, const char *s, int end, FmtTok **ptoks, int *pn, int *pcap)
{
  int i = 0, line = 0, col = 0, nl = 0, gap = 0, bol = 1;
  int n = 0;
  FmtTok *toks = *ptoks;
  while (i < end) {
    FmtTok t;
    int c = (unsigned char)s[i];
    if (c == '\n') {
      i++, line++, col = 0, nl++, gap = 0, bol = 1;
      continue;
    }
    if (c == ' ' || c == '\t' || c == '\f' || c == '\v' || c == '\r') {
      int w = c == '\t' ? 8 - col % 8 : 1;
      col += w;
      gap += w;
      i++;
      continue;
    }
    if (c == '\\' && i + 1 < end && s[i + 1] == '\n')
      return ft_error(st, line, "a line splice outside a directive: not formatted");
    memset(&t, 0, sizeof t);
    t.start = i;
    t.line = line;
    t.col = col;
    t.nl = nl;
    t.gap = nl ? 0 : gap;
    if (c == '#' && bol) {
      /* a directive: to the end of its line, splices and comments included */
      t.kind = FK_PP;
      while (i < end && s[i] != '\n') {
        if (s[i] == '\\' && i + 1 < end && s[i + 1] == '\n') {
          i += 2, line++;
          continue;
        }
        if (s[i] == '\\' && i + 2 < end && s[i + 1] == '\r' && s[i + 2] == '\n') {
          i += 3, line++;
          continue;
        }
        if (s[i] == '/' && i + 1 < end && s[i + 1] == '*') {
          i += 2;
          while (i < end && !(s[i] == '*' && i + 1 < end && s[i + 1] == '/')) {
            if (s[i] == '\n')
              line++;
            i++;
          }
          if (i >= end)
            return ft_error(st, t.line, "unterminated comment");
          i += 2;
          continue;
        }
        if (s[i] == '/' && i + 1 < end && s[i + 1] == '/') {
          while (i < end && s[i] != '\n') {
            if (s[i] == '\\' && i + 1 < end && s[i + 1] == '\n')
              line++, i++;
            i++;
          }
          break;
        }
        if (s[i] == '"' || s[i] == '\'') {
          /* <...> and odd quotes (#error don't) stay within the line */
          i = ft_skip_quoted(s, i, end);
          continue;
        }
        i++;
      }
    } else if (c == '/' && i + 1 < end && s[i + 1] == '/') {
      t.kind = FK_LCOM;
      while (i < end && s[i] != '\n')
        i++;
      if (i > t.start && s[i - 1] == '\\')
        return ft_error(st, line, "a // comment continued by a backslash: not formatted");
    } else if (c == '/' && i + 1 < end && s[i + 1] == '*') {
      t.kind = FK_BCOM;
      i += 2;
      while (i < end && !(s[i] == '*' && i + 1 < end && s[i + 1] == '/')) {
        if (s[i] == '\n')
          line++;
        i++;
      }
      if (i >= end)
        return ft_error(st, t.line, "unterminated comment");
      i += 2;
    } else if (c == '"' || c == '\'') {
      t.kind = FK_STR;
      i = ft_skip_quoted(s, i, end);
    } else if ((c >= '0' && c <= '9') || (c == '.' && i + 1 < end && s[i + 1] >= '0' && s[i + 1] <= '9')) {
      t.kind = FK_NUM;
      i++;
      while (i < end) {
        int d = (unsigned char)s[i];
        if ((d == '+' || d == '-') && strchr("eEpP", s[i - 1]))
          i++;
        else if (ft_is_idchar(d) || d == '.')
          i++;
        else if (d == '\'' && i + 1 < end && ft_is_idchar((unsigned char)s[i + 1]))
          i++; /* C23 digit separator */
        else
          break;
      }
    } else if (ft_is_idchar(c)) {
      t.kind = FK_ID;
      while (i < end && ft_is_idchar((unsigned char)s[i]))
        i++;
      /* L"..", u8"..", U'..' */
      if (i < end && (s[i] == '"' || s[i] == '\'')) {
        int l = i - t.start;
        const char *w = s + t.start;
        if ((l == 1 && (*w == 'L' || *w == 'u' || *w == 'U')) || (l == 2 && w[0] == 'u' && w[1] == '8')) {
          t.kind = FK_STR;
          i = ft_skip_quoted(s, i, end);
        }
      }
    } else {
      int k, l = 1;
      t.kind = FK_PUNCT;
      for (k = 0; ft_puncts[k]; k++) {
        int pl = (int)strlen(ft_puncts[k]);
        if (i + pl <= end && !memcmp(s + i, ft_puncts[k], pl)) {
          l = pl;
          break;
        }
      }
      i += l;
    }
    t.len = i - t.start;
    ft_grow((void **)&toks, pcap, n + 1, sizeof(FmtTok));
    toks[n++] = t;
    /* where the token ends */
    {
      int k = t.start, last_nl = -1;
      for (; k < i; k++)
        if (s[k] == '\n')
          last_nl = k;
      if (last_nl >= 0) {
        col = 0;
        for (k = last_nl + 1; k < i; k++)
          col += s[k] == '\t' ? 8 - col % 8 : 1;
        if (t.kind != FK_PP && t.kind != FK_BCOM)
          return ft_error(st, t.line, "a token spans lines: not formatted");
      } else {
        for (k = t.start; k < i; k++)
          col += s[k] == '\t' ? 8 - col % 8 : 1;
      }
    }
    nl = 0, gap = 0, bol = 0;
  }
  *ptoks = toks;
  *pn = n;
  return 0;
}

/* ------------------------------------------------------------------ */
/* token predicates */

static int ft_is(FmtState *st, int i, const char *w)
{
  FmtTok *t;
  int l;
  if (i < 0 || i >= st->ntoks)
    return 0;
  t = &st->toks[i];
  l = (int)strlen(w);
  return t->len == l && !memcmp(st->src + t->start, w, l);
}

static int ft_is_punct(FmtState *st, int i, const char *w)
{
  return i >= 0 && i < st->ntoks && st->toks[i].kind == FK_PUNCT && ft_is(st, i, w);
}

static int ft_is_word(FmtState *st, int i)
{
  int k;
  if (i < 0 || i >= st->ntoks)
    return 0;
  k = st->toks[i].kind;
  return k == FK_ID || k == FK_NUM || k == FK_STR;
}

static int ft_is_id(FmtState *st, int i)
{
  return i >= 0 && i < st->ntoks && st->toks[i].kind == FK_ID;
}

/* keyword classes */
#define KW_TYPE 1      /* names a type */
#define KW_QUAL 2      /* qualifies a type or a pointer level */
#define KW_STORAGE 4   /* static extern inline ... */
#define KW_TAG 8       /* struct union enum */
#define KW_CTRL 16     /* if for while switch: a space before ( */
#define KW_STMT 32     /* return case goto else do ...: a space before ( */
#define KW_FUNCLIKE 64 /* sizeof(x) __attribute__((x)) */
#define KW_OPERAND 128 /* nullptr true false: a value */
#define KW_YPARAM 256  /* in out inout drop */
#define KW_YARG 512    /* owned(f) borrows(x) takes(p) c_api(T): may take parentheses */
#define KW_PTRQUAL 1024 /* goes after a '*' */
#define KW_OTHER 2048   /* any other keyword: not a value */
#define KW_YASC 4096    /* a yasc word: a keyword only where one can stand */

typedef struct FmtKw
{
  const char *name;
  int cls;
} FmtKw;

static const FmtKw ft_kws[] = {
    {"void", KW_TYPE},
    {"char", KW_TYPE},
    {"short", KW_TYPE},
    {"int", KW_TYPE},
    {"long", KW_TYPE},
    {"float", KW_TYPE},
    {"double", KW_TYPE},
    {"signed", KW_TYPE},
    {"unsigned", KW_TYPE},
    {"_Bool", KW_TYPE},
    {"bool", KW_TYPE},
    {"_Complex", KW_TYPE},
    {"__int128", KW_TYPE},
    {"auto", KW_TYPE | KW_STORAGE},
    {"const", KW_QUAL | KW_PTRQUAL},
    {"volatile", KW_QUAL | KW_PTRQUAL},
    {"restrict", KW_QUAL | KW_PTRQUAL},
    {"__restrict", KW_QUAL | KW_PTRQUAL},
    {"__restrict__", KW_QUAL | KW_PTRQUAL},
    {"_Atomic", KW_QUAL},
    {"owned", KW_QUAL | KW_PTRQUAL | KW_YARG | KW_YASC},
    {"view", KW_QUAL | KW_PTRQUAL | KW_YASC},
    {"borrows", KW_QUAL | KW_PTRQUAL | KW_YARG | KW_YASC},
    {"takes", KW_QUAL | KW_PTRQUAL | KW_YARG | KW_YASC},
    {"c_api", KW_QUAL | KW_PTRQUAL | KW_YARG | KW_YASC},
    {"unchecked", KW_QUAL | KW_PTRQUAL | KW_YASC},
    {"in", KW_YPARAM | KW_YASC},
    {"out", KW_YPARAM | KW_YASC},
    {"inout", KW_YPARAM | KW_YASC},
    {"drop", KW_YPARAM | KW_YASC},
    {"static", KW_STORAGE},
    {"extern", KW_STORAGE},
    {"inline", KW_STORAGE},
    {"__inline", KW_STORAGE},
    {"__inline__", KW_STORAGE},
    {"register", KW_STORAGE},
    {"typedef", KW_STORAGE},
    {"_Noreturn", KW_STORAGE},
    {"_Thread_local", KW_STORAGE},
    {"thread_local", KW_STORAGE},
    {"export", KW_STORAGE | KW_YASC},
    {"struct", KW_TAG},
    {"union", KW_TAG | KW_YARG},
    {"enum", KW_TAG},
    {"if", KW_CTRL},
    {"for", KW_CTRL},
    {"while", KW_CTRL},
    {"switch", KW_CTRL},
    {"return", KW_STMT},
    {"case", KW_STMT},
    {"goto", KW_STMT},
    {"else", KW_STMT},
    {"do", KW_STMT},
    {"break", KW_OTHER},
    {"continue", KW_OTHER},
    {"default", KW_OTHER},
    {"defer", KW_STMT | KW_YASC},
    {"errdefer", KW_STMT | KW_YASC},
    {"try", KW_STMT | KW_YASC},
    {"catch", KW_STMT | KW_YASC},
    {"sizeof", KW_FUNCLIKE},
    {"alignof", KW_FUNCLIKE},
    {"_Alignof", KW_FUNCLIKE},
    {"__alignof__", KW_FUNCLIKE},
    {"_Alignas", KW_FUNCLIKE | KW_QUAL},
    {"alignas", KW_FUNCLIKE | KW_QUAL},
    {"typeof", KW_FUNCLIKE | KW_TYPE},
    {"__typeof__", KW_FUNCLIKE | KW_TYPE},
    {"__typeof", KW_FUNCLIKE | KW_TYPE},
    {"__attribute__", KW_FUNCLIKE | KW_QUAL | KW_YARG},
    {"__attribute", KW_FUNCLIKE | KW_QUAL | KW_YARG},
    {"__declspec", KW_FUNCLIKE | KW_QUAL | KW_YARG},
    {"asm", KW_FUNCLIKE},
    {"__asm__", KW_FUNCLIKE},
    {"__asm", KW_FUNCLIKE},
    {"_Static_assert", KW_FUNCLIKE},
    {"static_assert", KW_FUNCLIKE},
    {"_Generic", KW_FUNCLIKE},
    {"nullptr", KW_OPERAND},
    {"true", KW_OPERAND},
    {"false", KW_OPERAND},
    {"undefined", KW_OPERAND | KW_YASC},
    {NULL, 0}};

static int ft_next_sig(FmtState *st, int i);
static int ft_prev_sig(FmtState *st, int i);
static int ft_ptr_maybe(FmtState *st, int i);
static int ft_in_ptr_group(FmtState *st, int i);

static int ft_kw_raw(FmtState *st, int i)
{
  const FmtKw *k;
  FmtTok *t;
  if (!ft_is_id(st, i))
    return 0;
  t = &st->toks[i];
  for (k = ft_kws; k->name; k++)
    if ((int)strlen(k->name) == t->len && !memcmp(k->name, st->src + t->start, t->len))
      return k->cls;
  return 0;
}

static int ft_kw(FmtState *st, int i)
{
  int c = ft_kw_raw(st, i), n, p;
  if (!(c & KW_YASC))
    return c;
  /* in regular C these are names: `out = out * 2`, `char *view;` */
  n = ft_next_sig(st, i);
  p = ft_prev_sig(st, i);
  if (c & KW_OPERAND)
    return c;
  if (c & (KW_YPARAM | KW_STORAGE))
    return ft_is_id(st, n) ? c : 0;
  if (c & KW_PTRQUAL) {
    if (ft_is_id(st, n) || ft_is_punct(st, n, "*") || ((c & KW_YARG) && ft_is_punct(st, n, "(")))
      return c;
    if ((ft_is_punct(st, p, "*") || ft_ptr_maybe(st, p)) &&
        (ft_is_punct(st, n, ")") || ft_is_punct(st, n, ",") || ft_is_punct(st, n, "[")))
      return c;
    if (ft_in_ptr_group(st, i))
      return c; /* *@(const, view) */
    if (ft_is_punct(st, p, "[") && (ft_is_punct(st, n, ":") || ft_is_punct(st, n, "]")))
      return c; /* char path[?:0], args[owned] */
    return 0;
  }
  if (c & KW_STMT) {
    /* defer f(); try g(); x catch |e| ...; but not `try = 1` */
    if (ft_is_id(st, n) || ft_is_punct(st, n, "{") || ft_is_punct(st, n, "|") || ft_is_punct(st, n, "-") ||
        ft_is_punct(st, n, "(") || ft_is_punct(st, n, "*") || ft_is_punct(st, n, "!") || ft_is_punct(st, n, "&") ||
        (n >= 0 && st->toks[n].kind == FK_NUM))
      return c;
    return 0;
  }
  return c;
}

static int ft_tok_eq(FmtState *st, int i, const char *s, int l)
{
  return st->toks[i].len == l && !memcmp(st->src + st->toks[i].start, s, l);
}

static int ft_is_type_name(FmtState *st, int i)
{
  FmtTok *t;
  int k;
  if (!ft_is_id(st, i) || ft_kw(st, i))
    return 0;
  t = &st->toks[i];
  if (t->len > 2 && st->src[t->start + t->len - 2] == '_' && st->src[t->start + t->len - 1] == 't')
    return 1;
  if (ft_tok_eq(st, i, "FILE", 4) || ft_tok_eq(st, i, "DIR", 3) || ft_tok_eq(st, i, "va_list", 7) ||
      ft_tok_eq(st, i, "jmp_buf", 7) || ft_tok_eq(st, i, "__builtin_va_list", 17))
    return 1;
  for (k = 0; k < st->ntypes; k++)
    if (ft_tok_eq(st, i, st->types[k], (int)strlen(st->types[k])))
      return 1;
  return 0;
}

static void ft_add_type(FmtState *st, int i)
{
  char *name;
  if (!ft_is_id(st, i) || ft_kw(st, i) || ft_is_type_name(st, i))
    return;
  name = tcc_malloc(st->toks[i].len + 1);
  memcpy(name, st->src + st->toks[i].start, st->toks[i].len);
  name[st->toks[i].len] = 0;
  ft_grow((void **)&st->types, &st->captypes, st->ntypes + 1, sizeof(char *));
  st->types[st->ntypes++] = name;
}

static int ft_sig_kind(int k)
{
  return k != FK_LCOM && k != FK_BCOM && k != FK_PP;
}

static int ft_prev_sig(FmtState *st, int i)
{
  for (i--; i >= 0; i--)
    if (ft_sig_kind(st->toks[i].kind) && st->toks[i].verbatim != 1)
      return i;
  return -1;
}

/* the next significant token after i (skipping comments and directives) */
static int ft_next_sig(FmtState *st, int i)
{
  for (i++; i < st->ntoks; i++)
    if (ft_sig_kind(st->toks[i].kind) && st->toks[i].verbatim != 1)
      return i;
  return -1;
}

/* yasc: a pointer's `?` (may be null; docs/yasc_cheatsheet.md) -- after a
   '*' or a '[' (neither ever starts a conditional's '?'), or against an int
   handle's owned / view (`int ?owned(close) fd`) -- not a conditional */
static int ft_ptr_maybe(FmtState *st, int i)
{
  int p, n;
  if (i < 0 || !ft_is_punct(st, i, "?"))
    return 0;
  p = ft_prev_sig(st, i);
  if (p >= 0 && (ft_is_punct(st, p, "*") || ft_is_punct(st, p, "[")))
    return 1;
  n = ft_next_sig(st, i);
  /* glued, or after a type or a parameter word (`drop ? owned` as clang-format writes it) */
  return n >= 0 && ((st->toks[n].gap == 0 && st->toks[n].nl == 0) || (ft_kw_raw(st, p) & (KW_TYPE | KW_YPARAM))) &&
         ft_is_id(st, n) &&
         ((st->toks[n].len == 5 && !memcmp(st->src + st->toks[n].start, "owned", 5)) ||
          (st->toks[n].len == 4 && !memcmp(st->src + st->toks[n].start, "view", 4)));
}

/* the '@' of a pointer's `*@(a, b)` / `*?@(a, b)` */
static int ft_ptr_group(FmtState *st, int i)
{
  int p, n;
  if (i < 0 || !ft_is_punct(st, i, "@"))
    return 0;
  p = ft_prev_sig(st, i);
  n = ft_next_sig(st, i);
  return n >= 0 && ft_is_punct(st, n, "(") && p >= 0 && (ft_is_punct(st, p, "*") || ft_ptr_maybe(st, p));
}

/* token I is in a pointer's @( ) at its own level */
static int ft_in_ptr_group(FmtState *st, int i)
{
  int k, depth = 0;
  for (k = ft_prev_sig(st, i); k >= 0; k = ft_prev_sig(st, k)) {
    if (ft_is_punct(st, k, ")"))
      depth++;
    else if (ft_is_punct(st, k, "(") && depth-- == 0)
      return ft_ptr_group(st, ft_prev_sig(st, k));
    else if (depth == 0 && !ft_is_id(st, k) && !ft_is_punct(st, k, ","))
      return 0;
  }
  return 0;
}

/* typedef names: the last identifier before the ';' of a typedef */
static void ft_collect_typedefs(FmtState *st)
{
  int i, depth, last;
  for (i = 0; i < st->ntoks; i++) {
    if (!ft_is(st, i, "typedef") || st->toks[i].kind != FK_ID)
      continue;
    depth = 0, last = -1;
    for (i++; i < st->ntoks; i++) {
      FmtTok *t = &st->toks[i];
      if (!ft_sig_kind(t->kind))
        continue;
      if (t->kind == FK_PUNCT && (ft_is(st, i, "{") || ft_is(st, i, "(") || ft_is(st, i, "[")))
        depth++;
      else if (t->kind == FK_PUNCT && (ft_is(st, i, "}") || ft_is(st, i, ")") || ft_is(st, i, "]")))
        depth--;
      else if (depth == 0 && ft_is_punct(st, i, ";"))
        break;
      else if (t->kind == FK_ID && !ft_kw(st, i) && (depth == 0 || (depth == 1 && ft_is_punct(st, i - 1, "*"))))
        last = i;
      if (depth < 0)
        break;
    }
    if (last >= 0)
      ft_add_type(st, last);
  }
}

/* comments that turn formatting off and on; #if 0 bodies */
static int ft_comment_says(FmtState *st, int i, const char *what)
{
  FmtTok *t = &st->toks[i];
  const char *names[] = {"tcc-format ", "clang-format "};
  int k, l, wl = (int)strlen(what);
  for (k = 0; k < 2; k++) {
    int nl = (int)strlen(names[k]);
    for (l = t->start; l + nl + wl <= t->start + t->len; l++)
      if (!memcmp(st->src + l, names[k], nl) && !memcmp(st->src + l + nl, what, wl) &&
          (l + nl + wl == t->start + t->len || !ft_is_idchar((unsigned char)st->src[l + nl + wl])))
        return 1;
  }
  return 0;
}

static int ft_tok_end_line(FmtState *st, int i)
{
  FmtTok *t = &st->toks[i];
  int k, line = t->line;
  for (k = t->start; k < t->start + t->len; k++)
    line += st->src[k] == '\n';
  return line;
}

/* the directive's name: "if", "endif", ... (into buf) */
static void ft_pp_name(FmtState *st, int i, char *buf, int size)
{
  FmtTok *t = &st->toks[i];
  const char *p = st->src + t->start + 1, *e = st->src + t->start + t->len;
  int n = 0;
  while (p < e && (*p == ' ' || *p == '\t'))
    p++;
  while (p < e && ft_is_idchar((unsigned char)*p) && n < size - 1)
    buf[n++] = *p++;
  buf[n] = 0;
  if (!strcmp(buf, "if")) {
    while (p < e && (*p == ' ' || *p == '\t'))
      p++;
    if (p < e && *p == '0' && (p + 1 == e || !ft_is_idchar((unsigned char)p[1])))
      strcpy(buf, "if0");
  }
}

static void ft_mark_verbatim(FmtState *st)
{
  int i, k;
  char name[16];
  for (i = 0; i < st->ntoks; i++) {
    FmtTok *t = &st->toks[i];
    if (t->verbatim)
      continue;
    if (t->kind == FK_PP) {
      int depth = 0;
      ft_pp_name(st, i, name, sizeof name);
      if (strcmp(name, "if0"))
        continue;
      for (k = i + 1; k < st->ntoks; k++) {
        if (st->toks[k].kind == FK_PP) {
          ft_pp_name(st, k, name, sizeof name);
          if (!strncmp(name, "if", 2))
            depth++;
          else if (!strcmp(name, "endif") && depth-- == 0)
            break;
          else if (depth == 0 && (!strcmp(name, "else") || !strcmp(name, "elif") || !strcmp(name, "elifdef") ||
                                  !strcmp(name, "elifndef")))
            break;
        }
        st->toks[k].verbatim = 1;
      }
      i = k - 1;
    } else if ((t->kind == FK_LCOM || t->kind == FK_BCOM) && ft_comment_says(st, i, "off")) {
      int from = ft_tok_end_line(st, i);
      for (k = i + 1; k < st->ntoks; k++) {
        if ((st->toks[k].kind == FK_LCOM || st->toks[k].kind == FK_BCOM) && ft_comment_says(st, k, "on"))
          break;
        if (st->toks[k].line > from && !st->toks[k].verbatim)
          st->toks[k].verbatim = 2;
      }
      i = k - 1;
    }
  }
}

/* ------------------------------------------------------------------ */
/* output */

static int ft_at_bol(FmtState *st)
{
  return st->sink->size == 0 || st->sink->data[st->sink->size - 1] == '\n';
}

static void ft_trim(FmtState *st)
{
  FmtText *o = st->sink;
  while (o->size > 0 && (o->data[o->size - 1] == ' ' || o->data[o->size - 1] == '\t'))
    o->size--;
  if (o->data)
    o->data[o->size] = 0;
}

/* start a new output line (blank: one empty line before it) */
static void ft_newline(FmtState *st, int blank)
{
  FmtText *o = st->sink;
  if (!ft_at_bol(st)) {
    ft_trim(st);
    ft_putc(o, '\n');
    st->out_line++;
  }
  if (blank && o->size > 0 && !(o->size >= 2 && o->data[o->size - 2] == '\n')) {
    ft_putc(o, '\n');
    st->out_line++;
  }
  st->col = 0;
  st->line_indent = 0;
}

static void ft_indent(FmtState *st, int n)
{
  if (n < 0)
    n = 0;
  ft_spaces(st->sink, n);
  st->col = n;
  st->line_indent = n;
}

/* write token text; a block comment's later lines move with its first */
static void ft_write_tok(FmtState *st, int i)
{
  FmtTok *t = &st->toks[i];
  const char *s = st->src + t->start;
  int k, n = t->len;
  if (t->kind == FK_BCOM) {
    int delta = st->col - t->col, ls = 0;
    for (k = 0; k < n; k++) {
      if (s[k] != '\n')
        continue;
      ft_cat(st->sink, s + ls, k - ls);
      ft_trim(st);
      ft_putc(st->sink, '\n');
      st->out_line++;
      ls = k + 1;
      /* the next line's indent, shifted */
      {
        int c = 0, j = ls;
        while (j < n && (s[j] == ' ' || s[j] == '\t')) {
          c += s[j] == '\t' ? 8 - c % 8 : 1;
          j++;
        }
        if (j < n && s[j] != '\n') {
          int want = c + delta;
          if (want < 0)
            want = 0;
          if (c == 0 && delta > 0)
            want = 0; /* text at column 0 stays there */
          ft_spaces(st->sink, want);
        }
        ls = j;
      }
    }
    ft_cat(st->sink, s + ls, n - ls);
    st->col = 0;
    {
      int j = st->sink->size;
      while (j > 0 && st->sink->data[j - 1] != '\n')
        j--;
      st->col = st->sink->size - j;
    }
    return;
  }
  if (t->kind == FK_PP) {
    /* trailing blanks of each line go */
    int ls = 0;
    for (k = 0; k <= n; k++) {
      if (k < n && s[k] != '\n')
        continue;
      ft_cat(st->sink, s + ls, k - ls);
      if (k < n) {
        ft_trim(st);
        ft_putc(st->sink, '\n');
        st->out_line++;
      }
      ls = k + 1;
    }
    ft_trim(st);
    st->col = 1;
    return;
  }
  ft_cat(st->sink, s, n);
  st->col += n;
}

static void ft_copy_lines(FmtState *st, int from_line, int to_line)
{
  int a = st->line_off[from_line];
  int b = to_line + 1 < st->nlines ? st->line_off[to_line + 1] : st->len;
  ft_cat(&st->out, st->src + a, b - a);
  if (b > a && st->src[b - 1] != '\n')
    ft_putc(&st->out, '\n');
  {
    int k;
    for (k = a; k < b; k++)
      st->out_line += st->src[k] == '\n';
  }
  st->col = 0;
  st->line_indent = 0;
}

/* ------------------------------------------------------------------ */
/* frames */

static FmtFrame *ft_top(FmtState *st)
{
  return st->nframes ? &st->frames[st->nframes - 1] : NULL;
}

static FmtFrame *ft_push(FmtState *st, int kind)
{
  FmtFrame *f;
  ft_grow((void **)&st->frames, &st->capframes, st->nframes + 1, sizeof(FmtFrame));
  f = &st->frames[st->nframes++];
  memset(f, 0, sizeof *f);
  f->kind = kind;
  f->indent = -1;
  f->qcol = f->acol = -1;
  return f;
}

static int ft_is_bracket(FmtFrame *f)
{
  return f && (f->kind == FF_PAREN || f->kind == FF_BRACKET || f->kind == FF_ANGLE || f->kind == FF_INIT ||
               f->kind == FF_LIST);
}

/* the innermost block (or 0 at file scope) */
static FmtFrame *ft_block(FmtState *st)
{
  int k;
  for (k = st->nframes - 1; k >= 0; k--)
    if (st->frames[k].kind == FF_BLOCK)
      return &st->frames[k];
  return NULL;
}

/* the indent statements get here */
static int ft_stmt_level(FmtState *st)
{
  int k;
  for (k = st->nframes - 1; k >= 0; k--)
    if (st->frames[k].kind == FF_BLOCK || st->frames[k].kind == FF_PENDING)
      return st->frames[k].indent;
  return 0;
}

static int ft_in_code(FmtState *st)
{
  FmtFrame *b = ft_block(st);
  return b && b->sub != FB_STRUCT && b->sub != FB_EXTERN;
}

/* at statement level: no bracket open since the statement began */
static int ft_stmt_scope(FmtState *st)
{
  FmtFrame *f = ft_top(st);
  return !f || f->kind == FF_BLOCK || f->kind == FF_PENDING;
}

static int ft_file_scope(FmtState *st)
{
  FmtFrame *b = ft_block(st);
  return !b || b->sub == FB_EXTERN;
}

/* ------------------------------------------------------------------ */
/* classification */

static int ft_ends_operand(FmtState *st, int p)
{
  FmtTok *t;
  int r, k;
  if (p < 0)
    return 0;
  t = &st->toks[p];
  r = st->role[p];
  if (t->kind == FK_NUM || t->kind == FK_STR)
    return 1;
  if (t->kind == FK_ID) {
    k = ft_kw(st, p);
    return !k || (k & KW_OPERAND);
  }
  if (ft_is(st, p, ")"))
    return r != R_CAST_CLOSE && r != R_CTRL_CLOSE && r != R_ARG_CLOSE;
  if (ft_is(st, p, "]"))
    return r == R_IDX_CLOSE;
  if (r == R_POSTFIX || r == R_INIT_CLOSE)
    return 1; /* (int[]){1, 2}[1] */
  return 0;
}

/* a pointer qualifier here: the word after a '*' that is not the declarator's name */
static int ft_ptrqual_here(FmtState *st, int i)
{
  int n;
  if (!(ft_kw(st, i) & KW_PTRQUAL))
    return 0;
  n = ft_next_sig(st, i);
  if (n < 0)
    return 0;
  if (ft_is_id(st, n) || ft_is_punct(st, n, "*"))
    return 1;
  if (ft_is_punct(st, n, "(") && (ft_kw(st, i) & KW_YARG))
    return 1;
  return ft_is_punct(st, n, "[") || ft_is_punct(st, n, ")") || ft_is_punct(st, n, ",");
}

/* the '<' after cast/unsafe_cast opens a type list closed by a '>' before '(' */
static int ft_angle_cast(FmtState *st, int i)
{
  int depth = 0, k;
  if (!ft_is(st, st->prev, "cast") && !ft_is(st, st->prev, "unsafe_cast"))
    return 0;
  for (k = i + 1; k < st->ntoks; k++) {
    if (!ft_sig_kind(st->toks[k].kind))
      continue;
    if (ft_is_punct(st, k, "(") || ft_is_punct(st, k, "["))
      depth++;
    else if (ft_is_punct(st, k, ")") || ft_is_punct(st, k, "]"))
      depth--;
    else if (ft_is_punct(st, k, ";") || ft_is_punct(st, k, "{") || ft_is_punct(st, k, "}"))
      return 0;
    else if (depth == 0 && ft_is_punct(st, k, ">"))
      return ft_is_punct(st, ft_next_sig(st, k), "(");
    if (depth < 0)
      return 0;
  }
  return 0;
}

static int ft_typeish_tok(FmtState *st, int i)
{
  int k = ft_kw(st, i);
  if (k & (KW_TYPE | KW_QUAL | KW_STORAGE | KW_TAG | KW_YPARAM))
    return 1;
  if (ft_is_id(st, i) && !k)
    return 1;
  return 0;
}

static int ft_classify(FmtState *st, int i)
{
  int p = st->prev, n = ft_next_sig(st, i);
  FmtTok *t = &st->toks[i];
  FmtFrame *f = ft_top(st);
  if (t->kind == FK_ID)
    return R_NONE;
  if (t->kind != FK_PUNCT)
    return R_NONE;
  if (ft_is(st, i, "(") || ft_is(st, i, ",") || ft_is(st, i, ";"))
    return R_NONE;
  if (ft_is(st, i, ")")) {
    if (f && f->kind == FF_PAREN) {
      if (f->flags & FP_ARG)
        return R_ARG_CLOSE;
      if (f->kw && f->kw != KWC_DOWHILE)
        return R_CTRL_CLOSE;
      if ((f->flags & FP_CAST_CAND) && (f->flags & FP_TYPEISH) && f->ntok > 0) {
        if (f->flags & (FP_HASTYPE | FP_HASSTAR))
          return R_CAST_CLOSE;
        /* (name) then a value: only a cast reads like that */
        if (n >= 0 && (st->toks[n].kind == FK_ID || st->toks[n].kind == FK_NUM || st->toks[n].kind == FK_STR) &&
            !(ft_kw(st, n) & (KW_STMT | KW_OTHER)))
          return R_CAST_CLOSE;
      }
    }
    return R_NONE;
  }
  if (ft_is(st, i, "["))
    return ft_ends_operand(st, p) ? R_IDX_OPEN : R_TYPEBR_OPEN;
  if (ft_is(st, i, "]")) {
    if (f && f->kind == FF_BRACKET)
      return f->sub ? R_TYPEBR_CLOSE : R_IDX_CLOSE;
    return R_IDX_CLOSE;
  }
  if (ft_is(st, i, "<") && ft_angle_cast(st, i))
    return R_ANGLE_OPEN;
  if (ft_is(st, i, ">") && f && f->kind == FF_ANGLE)
    return R_ANGLE_CLOSE;
  if (ft_is(st, i, "{") || ft_is(st, i, "}"))
    return R_NONE; /* decided in ft_brace */
  if (ft_is(st, i, "@"))
    return R_AT;
  if (ft_is(st, i, "|")) {
    if (ft_is(st, p, "catch"))
      return R_BAR_OPEN;
    if (p >= 1 && st->role[p - 1] == R_BAR_OPEN)
      return R_BAR_CLOSE;
    {
      /* catch |e| with a comment between: find the open bar */
      int k;
      for (k = p - 1; k >= 0 && !ft_sig_kind(st->toks[k].kind); k--)
        ;
      if (k >= 0 && st->role[k] == R_BAR_OPEN)
        return R_BAR_CLOSE;
    }
    return R_BINARY;
  }
  if (ft_is(st, i, "++") || ft_is(st, i, "--"))
    return ft_ends_operand(st, p) ? R_POSTFIX : R_UNARY;
  if (ft_is(st, i, "!") || ft_is(st, i, "~"))
    return R_UNARY;
  if (ft_is(st, i, "-") || ft_is(st, i, "+") || ft_is(st, i, "&"))
    return ft_ends_operand(st, p) ? R_BINARY : R_UNARY;
  if (ft_is(st, i, "*")) {
    int pk = ft_kw(st, p);
    if (p >= 0 && (st->role[p] == R_PTR || st->role[p] == R_ARG_CLOSE))
      return R_PTR;
    if (pk & (KW_TYPE | KW_QUAL) && !(pk & KW_FUNCLIKE && !(pk & KW_TYPE)))
      return R_PTR;
    if (ft_is_punct(st, p, "*") && st->role[p] == R_UNARY)
      return R_UNARY;
    if (ft_is_id(st, p) && !pk) {
      int q = n;
      if (ft_is_type_name(st, p) || (ft_kw(st, ft_prev_sig(st, p)) & KW_TAG))
        return R_PTR;
      /* (T **), (T * const *) */
      while (q >= 0 && (ft_is_punct(st, q, "*") || (ft_kw(st, q) & KW_PTRQUAL)))
        q = ft_next_sig(st, q);
      if (q >= 0 && ft_is_punct(st, q, ")") && f && f->kind == FF_PAREN && (f->flags & FP_TYPEISH) &&
          (f->flags & (FP_CAST_CAND | FP_DECL)))
        return R_PTR;
      if (n >= 0 && (ft_is_punct(st, n, ")") || (ft_is_punct(st, n, ",") && f && (f->flags & FP_DECL))))
        return R_PTR;
      if (st->prefix && n >= 0 &&
          (ft_is_id(st, n) || ft_is_punct(st, n, "*") || ft_is_punct(st, n, "(") || ft_is_punct(st, n, "["))) {
        if (!(ft_kw(st, n) & (KW_OPERAND | KW_STMT | KW_OTHER | KW_FUNCLIKE)))
          return R_PTR;
      }
      return R_BINARY;
    }
    if (ft_is_punct(st, p, "*") && st->role[p] == R_PTR)
      return R_PTR;
    return ft_ends_operand(st, p) ? R_BINARY : R_UNARY;
  }
  if (ft_is(st, i, ":")) {
    if (st->case_lbl && (!f || f->ternary == 0) && ft_stmt_scope(st))
      return R_LABEL_COLON;
    if (ft_stmt_scope(st) && p == st->stmt_tok && ft_is_id(st, p) && !ft_kw(st, p) && ft_in_code(st))
      return R_LABEL_COLON;
    if (f && f->ternary > 0)
      return R_BINARY;
    if (f && f->kind == FF_BRACKET)
      return R_SLICE_COLON;
    if (f && f->kind == FF_PAREN && (f->flags & FP_GENERIC))
      return R_GENERIC_COLON;
    return R_BINARY;
  }
  if (ft_is(st, i, "::"))
    return f && f->kind == FF_BRACKET ? R_SLICE_COLON : R_BINARY;
  if (ft_is(st, i, ".") || ft_is(st, i, "->") || ft_is(st, i, "..."))
    return R_NONE;
  if (ft_is(st, i, "#") || ft_is(st, i, "##"))
    return R_NONE;
  return R_BINARY;
}

/* what a '{' opens; sets *sub and *kw for a block */
static int ft_brace_kind(FmtState *st, int i, int *sub, int *kw)
{
  int p = st->prev, pr = p >= 0 ? st->role[p] : R_NONE;
  FmtFrame *f = ft_top(st);
  (void)i;
  *sub = FB_CODE;
  *kw = KWC_NONE;
  if (st->pending_ctrl) {
    *sub = FB_CTRL;
    *kw = st->pending_ctrl;
    return FF_BLOCK;
  }
  if (p < 0)
    return FF_INIT;
  if (st->toks[p].kind == FK_STR && ft_is(st, p, "\"C\"")) {
    int q;
    for (q = p - 1; q >= 0 && !ft_sig_kind(st->toks[q].kind); q--)
      ;
    if (ft_is(st, q, "extern")) {
      *sub = FB_EXTERN;
      return FF_BLOCK;
    }
  }
  if (st->tag && (ft_is_id(st, p) || pr == R_ARG_CLOSE)) {
    if (st->tag == 2)
      return FF_LIST;
    *sub = FB_STRUCT;
    return FF_BLOCK;
  }
  if (pr == R_CAST_CLOSE)
    return FF_INIT;
  if (ft_is_punct(st, p, "(")) {
    *sub = FB_EXPR;
    return FF_BLOCK;
  }
  if (pr == R_BAR_CLOSE || ft_is(st, p, "catch")) {
    *sub = FB_EXPR;
    return FF_BLOCK;
  }
  if (ft_is_punct(st, p, ")")) {
    if (ft_is_bracket(f))
      return FF_INIT;
    if (ft_file_scope(st) || (ft_block(st) && ft_block(st)->sub == FB_STRUCT)) {
      *sub = FB_FUNC;
      return FF_BLOCK;
    }
    *sub = FB_CTRL;
    *kw = KWC_MACRO;
    return FF_BLOCK;
  }
  if (ft_is(st, p, "unsafe") || ft_is(st, p, "defer") || ft_is(st, p, "errdefer")) {
    *sub = FB_CODE;
    return FF_BLOCK;
  }
  if (ft_is_bracket(f))
    return FF_INIT;
  if (st->at_start && (!f || f->kind == FF_BLOCK || f->kind == FF_PENDING) && ft_in_code(st)) {
    *sub = FB_CODE;
    return FF_BLOCK;
  }
  return FF_INIT;
}

/* ------------------------------------------------------------------ */
/* spacing between two significant tokens on one line */

static int ft_space(FmtState *st, int p, int t)
{
  /* yasc: a pointer's words go against its '*' -- `*?owned`, `*view`,
     `*@(const, owned)`, `*const`, `a[?4]`, `int ?owned(close) fd` */
  if (ft_ptr_maybe(st, t))
    return !(ft_is_punct(st, p, "*") || ft_is_punct(st, p, "["));
  if (ft_ptr_maybe(st, p) || (ft_ptr_group(st, p) && ft_is_punct(st, t, "(")))
    return 0;
  if (ft_is_punct(st, p, "*") && (ft_ptr_group(st, t) || (ft_kw(st, t) & (KW_PTRQUAL | KW_YARG))))
    return 0;
  int pr = st->role[p], tr = st->role[t];
  int pk, tk;
  FmtFrame *f = ft_top(st);
  if (ft_is_punct(st, p, "(") || pr == R_IDX_OPEN || pr == R_TYPEBR_OPEN || pr == R_ANGLE_OPEN || pr == R_INIT_OPEN)
    return 0;
  if (ft_is_punct(st, t, ")") || ft_is_punct(st, t, "]") || tr == R_ANGLE_CLOSE || tr == R_INIT_CLOSE)
    return 0;
  if (pr == R_LIST_OPEN)
    return tr != R_LIST_CLOSE;
  if (tr == R_LIST_CLOSE)
    return 1;
  if (ft_is_punct(st, t, ",") || ft_is_punct(st, t, ";"))
    return 0;
  if (ft_is_punct(st, p, ","))
    return 1;
  if (ft_is_punct(st, p, ";"))
    return 1;
  if (pr == R_BLOCK_OPEN && tr == R_BLOCK_CLOSE)
    return 0;
  if (tr == R_LABEL_COLON || tr == R_GENERIC_COLON)
    return 0;
  if (pr == R_LABEL_COLON || pr == R_GENERIC_COLON)
    return 1;
  if (tr == R_SLICE_COLON)
    return (ft_kw(st, p) & (KW_QUAL | KW_STORAGE)) ? 1 : 0;
  if (pr == R_SLICE_COLON)
    return 0;
  if (ft_is_punct(st, t, ".") || ft_is_punct(st, t, "->"))
    return 0;
  if (ft_is_punct(st, p, ".") || ft_is_punct(st, p, "->"))
    return 0;
  if (ft_is_punct(st, t, "...") || ft_is_punct(st, p, "..."))
    return 1;
  if (pr == R_UNARY && ft_is_punct(st, p, "*") && ft_ptrqual_here(st, t))
    return 1; /* int (* view cmp)(...) */
  if (pr == R_UNARY || pr == R_AT)
    return 0;
  if (tr == R_POSTFIX)
    return 0;
  if (tr == R_BAR_OPEN || pr == R_BAR_CLOSE)
    return 1;
  if (pr == R_BAR_OPEN || tr == R_BAR_CLOSE)
    return 0;
  if (tr == R_ANGLE_OPEN || pr == R_ANGLE_CLOSE)
    return 0;
  if (ft_is_punct(st, p, "?") && ft_is_punct(st, t, ":"))
    return 0;
  pk = ft_kw(st, p);
  tk = ft_kw(st, t);
  if (ft_is_punct(st, t, "(")) {
    if (pr == R_CAST_CLOSE)
      return 0;
    if (ft_is_word(st, p)) {
      if (f && f->kind == FF_ANGLE)
        return 0; /* cast<int(T)> */
      if (pk & (KW_CTRL | KW_STMT))
        return 1;
      if ((pk & KW_TYPE) && !(pk & KW_FUNCLIKE))
        return 1; /* void (*f)(void) */
      return 0;
    }
    if (ft_is_punct(st, p, ")") || ft_is_punct(st, p, "]"))
      return pr == R_CTRL_CLOSE;
    return pr == R_BINARY || pr == R_PTR || ft_is_punct(st, p, "}") || ft_is_punct(st, p, "?");
  }
  if (tr == R_IDX_OPEN)
    return 0;
  if (pr == R_CAST_CLOSE)
    return 0;
  if (pr == R_TYPEBR_CLOSE && (ft_is_word(st, t) || ft_is_punct(st, t, "[")))
    return 0;
  if (tr == R_PTR)
    return pr == R_PTR ? 0 : 1;
  if (pr == R_PTR) {
    if (tr == R_PTR)
      return 0;
    return ft_ptrqual_here(st, t) ? 1 : 0;
  }
  if (tr == R_BINARY || pr == R_BINARY)
    return 1;
  if (tr == R_UNARY || tr == R_AT) {
    if (ft_is_word(st, p))
      return (pk && !(pk & KW_OPERAND)) ? 1 : st->toks[t].gap > 0;
    return 1;
  }
  if (ft_is_word(st, p) && ft_is_word(st, t))
    return 1;
  if (ft_is_punct(st, t, "{"))
    return 1;
  if (ft_is_punct(st, p, ")") && ft_is_word(st, t))
    return 1;
  if (ft_is_punct(st, p, "]") && ft_is_word(st, t))
    return 1;
  if (ft_is_punct(st, p, "}"))
    return 1;
  if (tr == R_TYPEBR_OPEN) {
    /* (int[]){1, 2}, but static []const char s */
    if (ft_is_word(st, p))
      return !((pk & KW_TYPE) || (ft_is_id(st, p) && !pk));
    return ft_is_punct(st, p, ")");
  }
  (void)tk;
  return st->toks[t].gap > 0;
}

/* would two tokens written side by side lex as one? */
static int ft_glue(FmtState *st, int a, int b)
{
  FmtTok *x = &st->toks[a], *y = &st->toks[b];
  char buf[8];
  int k;
  if (ft_is_word(st, a) && ft_is_word(st, b))
    return 1;
  if (x->kind == FK_NUM && (y->kind == FK_PUNCT && (ft_is(st, b, ".") || ft_is(st, b, "+") || ft_is(st, b, "-"))))
    return 1;
  if (x->kind == FK_PUNCT && y->kind == FK_NUM && st->src[y->start] == '.')
    return 1;
  if (x->kind != FK_PUNCT || y->kind != FK_PUNCT)
    return 0;
  if (x->len + 1 > 4)
    return 0;
  memcpy(buf, st->src + x->start, x->len);
  buf[x->len] = st->src[y->start];
  buf[x->len + 1] = 0;
  for (k = 0; ft_puncts[k]; k++)
    if (!strncmp(ft_puncts[k], buf, x->len + 1))
      return 1;
  if ((buf[x->len - 1] == '/' && (buf[x->len] == '/' || buf[x->len] == '*')))
    return 1;
  return 0;
}

/* ------------------------------------------------------------------ */
/* where a line starting with token t goes */

static int ft_line_indent(FmtState *st, int t)
{
  FmtFrame *f = ft_top(st);
  int iw = st->style.indent;
  if (f && ft_is_bracket(f)) {
    int closes = (f->kind == FF_PAREN && ft_is_punct(st, t, ")")) ||
                 (f->kind == FF_BRACKET && ft_is_punct(st, t, "]")) ||
                 ((f->kind == FF_INIT || f->kind == FF_LIST) && ft_is_punct(st, t, "}")) ||
                 (f->kind == FF_ANGLE && ft_is_punct(st, t, ">"));
    if (closes)
      return f->base;
    if (ft_is_punct(st, t, ":") && st->role[t] == R_BINARY && f->ternary > 0 && f->qcol >= 0)
      return f->qcol;
    if (ft_is_punct(st, t, "?") && f->acol >= 0)
      return f->acol;
    if (f->indent < 0)
      f->indent = f->base + (f->kind == FF_LIST ? iw : st->style.cont);
    return f->indent;
  }
  if (f && f->kind == FF_BLOCK && ft_is_punct(st, t, "}"))
    return f->base;
  if (!st->at_start && f && ft_is_punct(st, t, ":") && st->role[t] == R_BINARY && f->ternary > 0 && f->qcol >= 0)
    return f->qcol;
  if (!st->at_start && f && ft_is_punct(st, t, "?") && f->acol >= 0)
    return f->acol;
  if (st->at_start) {
    int lvl = ft_stmt_level(st);
    if (ft_in_code(st)) {
      int n = ft_next_sig(st, t);
      if (ft_is(st, t, "case") || (ft_is(st, t, "default") && ft_is_punct(st, n, ":")) ||
          (ft_is_id(st, t) && !ft_kw(st, t) && ft_is_punct(st, n, ":")))
        return lvl - iw > 0 ? lvl - iw : 0;
    }
    return lvl;
  }
  if (st->pending_ctrl && ft_is_punct(st, t, "{"))
    return st->stmt_indent;
  if (ft_is_punct(st, t, "{") && st->prev >= 0 &&
      (ft_is_punct(st, st->prev, ")") || ft_is_id(st, st->prev)) && ft_stmt_scope(st))
    return st->stmt_indent; /* a function's or struct's own-line brace */
  if (ft_file_scope(st) && ft_stmt_scope(st) && st->prev >= 0 &&
      (ft_is_word(st, st->prev) || ft_is_punct(st, st->prev, "*") || ft_is_punct(st, st->prev, ")")) &&
      !ft_is_punct(st, t, "="))
    return st->stmt_indent; /* static int\nname(void) */
  return st->stmt_indent + st->style.cont;
}

/* ------------------------------------------------------------------ */
/* the statement ends: pop what waited for one statement */

static void ft_stmt_end(FmtState *st, int ended_kw)
{
  int n = ft_next_sig(st, st->prev);
  FmtFrame *f;
  int k, found = -1;
  if (ft_is(st, n, "else")) {
    if (ended_kw != KWC_IF) {
      for (k = st->nframes - 1; k >= 0 && st->frames[k].kind == FF_PENDING; k--)
        if (st->frames[k].kw == KWC_IF) {
          found = k;
          break;
        }
      if (found >= 0)
        st->nframes = found;
    }
  } else if (ft_is(st, n, "while") && (ended_kw == KWC_DO || (ft_top(st) && ft_top(st)->kind == FF_PENDING))) {
    if (ended_kw == KWC_DO)
      st->expect_dowhile = 1;
    else {
      for (k = st->nframes - 1; k >= 0 && st->frames[k].kind == FF_PENDING; k--)
        if (st->frames[k].kw == KWC_DO) {
          found = k;
          break;
        }
      if (found >= 0) {
        st->nframes = found;
        st->expect_dowhile = 1;
      } else {
        while ((f = ft_top(st)) && f->kind == FF_PENDING)
          st->nframes--;
      }
    }
  } else {
    while ((f = ft_top(st)) && f->kind == FF_PENDING)
      st->nframes--;
  }
  st->at_start = 1;
  st->stmt_tok = -1;
  st->prefix = 1;
  st->tag = 0;
  st->tag_ids = 0;
  st->case_lbl = 0;
  if ((f = ft_top(st)))
    f->ternary = 0, f->qcol = f->acol = -1;
  st->need_break = 1;
  if (ended_kw && (ft_is(st, n, "else") || (st->expect_dowhile && ft_is(st, n, "while"))))
    st->need_break = 0; /* } else, } while */
}

/* ------------------------------------------------------------------ */
/* #if branches: each starts from the state at #if; after #endif the first wins */

static void ft_snap_take(FmtState *st, FmtSnap *s)
{
  s->frames = tcc_malloc(sizeof(FmtFrame) * (st->nframes + 1));
  memcpy(s->frames, st->frames, sizeof(FmtFrame) * st->nframes);
  s->nframes = st->nframes;
  s->at_start = st->at_start;
  s->stmt_indent = st->stmt_indent;
  s->prefix = st->prefix;
  s->tag = st->tag;
  s->case_lbl = st->case_lbl;
  s->pending_ctrl = st->pending_ctrl;
  s->expect_dowhile = st->expect_dowhile;
}

static void ft_snap_put(FmtState *st, FmtSnap *s)
{
  ft_grow((void **)&st->frames, &st->capframes, s->nframes + 1, sizeof(FmtFrame));
  memcpy(st->frames, s->frames, sizeof(FmtFrame) * s->nframes);
  st->nframes = s->nframes;
  st->at_start = s->at_start;
  st->stmt_indent = s->stmt_indent;
  st->prefix = s->prefix;
  st->tag = s->tag;
  st->case_lbl = s->case_lbl;
  st->pending_ctrl = s->pending_ctrl;
  st->expect_dowhile = s->expect_dowhile;
}

static void ft_snap_free(FmtSnap *s)
{
  tcc_free(s->frames);
  if (s->first) {
    tcc_free(s->first->frames);
    tcc_free(s->first);
  }
  memset(s, 0, sizeof *s);
}

static void ft_pp_state(FmtState *st, int i)
{
  char name[16];
  FmtSnap *s;
  ft_pp_name(st, i, name, sizeof name);
  if (!strncmp(name, "if", 2)) {
    ft_grow((void **)&st->snaps, &st->capsnaps, st->nsnaps + 1, sizeof(FmtSnap));
    s = &st->snaps[st->nsnaps++];
    memset(s, 0, sizeof *s);
    ft_snap_take(st, s);
    s->first_dead = !strcmp(name, "if0");
  } else if (!strncmp(name, "el", 2) && st->nsnaps) {
    s = &st->snaps[st->nsnaps - 1];
    if (!s->have_first && !s->first_dead) {
      s->first = tcc_mallocz(sizeof(FmtSnap));
      ft_snap_take(st, s->first);
      s->have_first = 1;
    }
    if (s->first_dead && !s->have_first)
      s->first_dead = 2; /* the live branch is this one: keep its end */
    ft_snap_put(st, s);
  } else if (!strcmp(name, "endif") && st->nsnaps) {
    s = &st->snaps[st->nsnaps - 1];
    if (s->have_first)
      ft_snap_put(st, s->first);
    ft_snap_free(s);
    st->nsnaps--;
  }
}

/* ------------------------------------------------------------------ */
/* one significant token */

static void ft_open_frame_for_paren(FmtState *st, int i)
{
  int p = st->prev, pk = ft_kw(st, p);
  FmtFrame *f;
  int flags = FP_TYPEISH, kw = KWC_NONE;
  int saved_prefix = st->prefix;
  (void)i;
  if (pk & KW_CTRL) {
    if (ft_is(st, p, "if"))
      kw = KWC_IF;
    else if (ft_is(st, p, "for"))
      kw = KWC_FOR, flags |= FP_FOR;
    else if (ft_is(st, p, "switch"))
      kw = KWC_SWITCH;
    else
      kw = st->expect_dowhile ? KWC_DOWHILE : KWC_WHILE;
  } else if ((pk & KW_YARG) || ft_ptr_group(st, p)) {
    flags |= FP_ARG;
  } else if (ft_is(st, p, "when") || ft_is(st, p, "_Alignas") || ft_is(st, p, "alignas")) {
    flags |= FP_ARG;
  } else if (ft_is(st, p, "_Generic")) {
    flags |= FP_GENERIC;
  } else if (!ft_ends_operand(st, p) && !(pk & (KW_FUNCLIKE | KW_CTRL))) {
    if ((!ft_is_punct(st, p, ")") || st->role[p] == R_CAST_CLOSE) && !ft_is_punct(st, p, "]"))
      flags |= FP_CAST_CAND;
  } else if (ft_is_id(st, p) && !pk && st->prefix && ft_stmt_scope(st) &&
             (ft_file_scope(st) || (ft_block(st) && ft_block(st)->sub == FB_STRUCT))) {
    flags |= FP_DECL;
  }
  if (pk & KW_TAG)
    flags |= FP_ARG; /* union(k) */
  f = ft_push(st, FF_PAREN);
  f->flags = flags;
  f->kw = kw;
  f->base = st->line_indent;
  f->s_prefix = saved_prefix;
  st->prefix = (flags & (FP_DECL | FP_FOR)) ? 1 : 0;
  if (flags & FP_ARG)
    st->prefix = saved_prefix;
  if (kw == KWC_FOR) {
    st->stmt_tok = -1;
  }
}

static void ft_note_paren_content(FmtState *st, int i)
{
  /* a token directly inside the innermost paren: is it still a type? */
  FmtFrame *f = ft_top(st);
  int k;
  if (!f || f->kind != FF_PAREN)
    return;
  f->ntok++;
  if (!(f->flags & FP_TYPEISH))
    return;
  k = ft_kw(st, i);
  if (ft_is_punct(st, i, "*")) {
    f->flags |= FP_HASSTAR;
    return;
  }
  if (k & KW_TYPE) {
    f->flags |= FP_HASTYPE;
    return;
  }
  if (k & KW_TAG) {
    f->flags |= FP_HASTYPE; /* (struct tm){} is a compound literal */
    return;
  }
  if (k & KW_QUAL)
    return;
  if (ft_is_id(st, i) && !k) {
    if (ft_is_punct(st, st->prev, "*") && !ft_is_punct(st, ft_next_sig(st, i), "("))
      f->flags &= ~FP_TYPEISH; /* (*name), (a * b): a declarator or a product */
    else if (ft_is_type_name(st, i))
      f->flags |= FP_HASTYPE;
    return;
  }
  if (ft_is_punct(st, i, "[") || ft_is_punct(st, i, "]") || ft_is_punct(st, i, "(") || ft_is_punct(st, i, ")"))
    return; /* (void (*)(int)): the inner group decides when it closes */
  if (ft_ptr_maybe(st, i) || ft_ptr_group(st, i))
    return; /* (const char *?[]), (char *@(const, view)) */
  f->flags &= ~FP_TYPEISH;
}

static void ft_sig(FmtState *st, int i)
{
  FmtTok *t = &st->toks[i];
  int p = st->prev;
  int role, brk, indent = 0, sp;
  int bkind = -1, bsub = 0, bkw = 0;
  FmtFrame *f;
  int n = ft_next_sig(st, i);
  int iw = st->style.indent;
  int prev_is_sig = st->prev_any == p && p >= 0 && st->toks[p].kind != FK_PP;

  /* 1. what it is */
  role = ft_classify(st, i);
  if (ft_is_punct(st, i, "{")) {
    bkind = ft_brace_kind(st, i, &bsub, &bkw);
    role = bkind == FF_BLOCK ? R_BLOCK_OPEN : bkind == FF_LIST ? R_LIST_OPEN : R_INIT_OPEN;
  } else if (ft_is_punct(st, i, "}")) {
    f = ft_top(st);
    while (f && f->kind == FF_PENDING)
      f = st->nframes > 1 ? &st->frames[st->nframes - 2] : NULL, st->nframes--;
    f = ft_top(st);
    role = f && f->kind == FF_INIT ? R_INIT_CLOSE : f && f->kind == FF_LIST ? R_LIST_CLOSE : R_BLOCK_CLOSE;
  }
  st->role[i] = role;

  /* the paren content bookkeeping happens before the token's own frame */
  if (!ft_is_punct(st, i, ")"))
    ft_note_paren_content(st, i);

  /* 2. line break */
  brk = t->nl >= 2 ? 2 : t->nl ? 1 : 0;
  if (st->need_break && !brk)
    brk = 1;
  if (st->just_opened && brk == 2)
    brk = 1;
  if (role == R_BLOCK_CLOSE) {
    if (p >= 0 && st->role[p] == R_BLOCK_OPEN && t->nl == 0 && prev_is_sig)
      brk = 0; /* {} */
    else {
      if (brk == 2)
        brk = 1;
      if (!brk)
        brk = 1;
    }
  }
  if (role == R_BLOCK_OPEN) {
    int wrap = bsub == FB_FUNC     ? st->style.wrap_func
               : bsub == FB_STRUCT ? st->style.wrap_struct
               : bsub == FB_CTRL   ? st->style.wrap_ctrl
                                   : 0;
    if (bsub == FB_CTRL && (bkw == KWC_LABEL || bkw == KWC_MACRO))
      wrap = st->style.wrap_ctrl;
    if (bsub == FB_EXPR || bsub == FB_EXTERN || (bsub == FB_CODE && st->at_start))
      wrap = -1; /* as written */
    if (wrap == 1 && p >= 0)
      brk = 1;
    else if (wrap == 0 && brk && prev_is_sig)
      brk = 0;
  }
  if (ft_is(st, i, "else") && t->kind == FK_ID && p >= 0 && ft_is_punct(st, p, "}") && prev_is_sig) {
    brk = st->style.wrap_else ? 1 : 0;
  }
  if (ft_is(st, i, "while") && st->expect_dowhile && ft_is_punct(st, p, "}") && prev_is_sig)
    brk = 0;
  if (ft_is(st, i, "if") && ft_is(st, p, "else") && prev_is_sig)
    brk = 0;
  if (p < 0 && st->out.size == 0 && brk == 2)
    brk = 1;

  /* 3. write it */
  if (brk) {
    indent = ft_line_indent(st, i);
    ft_newline(st, brk == 2);
    ft_indent(st, indent);
  } else if (p >= 0) {
    if (st->prev_any != p)
      sp = t->gap > 0; /* after a comment: as written */
    else
      sp = ft_space(st, p, i);
    if (!sp && ft_glue(st, st->prev_any, i))
      sp = 1;
    if (sp)
      ft_putc(st->sink, ' '), st->col++;
  }
  /* a bracket's continued lines line up after the opener */
  f = ft_top(st);
  if (f && ft_is_bracket(f) && f->indent < 0 && !brk)
    f->indent = st->col;
  if (st->at_start) {
    st->stmt_indent = brk ? indent : st->line_indent;
    st->stmt_tok = i;
    if (role == R_BLOCK_OPEN && bsub == FB_CTRL)
      st->stmt_indent = brk ? indent : st->stmt_indent;
  }
  {
    FmtFrame *g = ft_top(st);
    if (g && ft_is_punct(st, i, "?") && !ft_ptr_maybe(st, i))
      g->qcol = st->col;
    if (g && p >= 0 && !ft_is_punct(st, i, "?") &&
        ((st->role[p] == R_BINARY && st->toks[p].kind == FK_PUNCT &&
          st->src[st->toks[p].start + st->toks[p].len - 1] == '=' && !ft_is(st, p, "==") && !ft_is(st, p, "!=") &&
          !ft_is(st, p, "<=") && !ft_is(st, p, ">=")) ||
         ft_is(st, p, "return")))
      g->acol = st->col;
  }
  ft_write_tok(st, i);
  st->need_break = 0;
  st->just_opened = 0;

  /* 4. the state after it */
  if (st->at_start) {
    st->at_start = 0;
    if (ft_is(st, i, "case") || (ft_is(st, i, "default") && ft_is_punct(st, n, ":")))
      st->case_lbl = 1;
  }
  if (t->kind == FK_ID) {
    int k = ft_kw(st, i);
    if (k & KW_TAG) {
      if (!ft_is_punct(st, n, "("))
        st->tag = ft_is(st, i, "enum") ? 2 : 1, st->tag_ids = 0;
      else if (ft_is(st, i, "union"))
        st->tag = 1, st->tag_ids = 0;
    } else if (ft_is(st, i, "error") && ft_is_id(st, n) && st->stmt_tok == i) {
      int n2 = ft_next_sig(st, n);
      if (ft_is_punct(st, n2, "{"))
        st->tag = 2, st->tag_ids = 0;
    } else if (st->tag && !(k & (KW_QUAL | KW_YARG))) {
      if (++st->tag_ids > 1)
        st->tag = 0;
    }
    if (!ft_typeish_tok(st, i) && ft_stmt_scope(st))
      st->prefix = 0;
    if (ft_is_id(st, i) && !k && st->prefix && ft_stmt_scope(st) && ft_is_punct(st, n, "*") && i == st->stmt_tok &&
        ft_in_code(st)) {
      int n2 = ft_next_sig(st, n);
      if (ft_is_id(st, n2))
        ft_add_type(st, i); /* T *x; at the start of a statement */
    }
    if (ft_is(st, i, "else")) {
      if (ft_is(st, n, "if")) {
      } else if (ft_is_punct(st, n, "{"))
        st->pending_ctrl = KWC_ELSE;
      else {
        f = ft_push(st, FF_PENDING);
        f->kw = KWC_ELSE;
        f->indent = st->stmt_indent + iw;
        st->at_start = 1;
        st->need_break = 1;
      }
    } else if (ft_is(st, i, "do") && k) {
      if (ft_is_punct(st, n, "{"))
        st->pending_ctrl = KWC_DO;
      else {
        f = ft_push(st, FF_PENDING);
        f->kw = KWC_DO;
        f->indent = st->stmt_indent + iw;
        st->at_start = 1;
        st->need_break = 1;
      }
    }
  } else if (t->kind == FK_NUM || t->kind == FK_STR) {
    if (ft_stmt_scope(st))
      st->prefix = 0;
  } else if (ft_is(st, i, "(")) {
    ft_open_frame_for_paren(st, i);
  } else if (ft_is(st, i, ")")) {
    f = ft_top(st);
    if (f && f->kind == FF_PAREN) {
      int kw = f->kw, flags = f->flags, sp_ = f->s_prefix;
      st->nframes--;
      ft_note_paren_content(st, i);
      /* a qualifier's list -- borrows(a, b), *@(const, view) -- says nothing of it */
      if ((f = ft_top(st)) && f->kind == FF_PAREN && !(flags & (FP_TYPEISH | FP_ARG)))
        f->flags &= ~FP_TYPEISH;
      st->prefix = (flags & FP_ARG) ? sp_ : 0;
      if (flags & FP_DECL)
        st->prefix = 0;
      if (kw == KWC_DOWHILE) {
        st->expect_dowhile = 0;
      } else if (kw) {
        if (ft_is_punct(st, n, "{"))
          st->pending_ctrl = kw;
        else {
          f = ft_push(st, FF_PENDING);
          f->kw = kw;
          f->indent = st->stmt_indent + iw;
          st->at_start = 1;
          st->need_break = 1;
        }
      }
    }
  } else if (ft_is(st, i, "[")) {
    f = ft_push(st, FF_BRACKET);
    f->sub = role == R_TYPEBR_OPEN;
    f->base = st->line_indent;
    f->s_prefix = st->prefix;
  } else if (ft_is(st, i, "]")) {
    f = ft_top(st);
    if (f && f->kind == FF_BRACKET) {
      st->prefix = f->s_prefix;
      st->nframes--;
    }
  } else if (role == R_ANGLE_OPEN) {
    f = ft_push(st, FF_ANGLE);
    f->base = st->line_indent;
  } else if (role == R_ANGLE_CLOSE) {
    st->nframes--;
  } else if (role == R_BLOCK_OPEN) {
    FmtFrame save;
    memset(&save, 0, sizeof save);
    save.s_stmt_indent = st->stmt_indent;
    save.s_prefix = st->prefix;
    save.s_tag = st->tag;
    f = ft_push(st, FF_BLOCK);
    f->sub = bsub;
    f->kw = bkw;
    f->base = (bsub == FB_EXPR) ? st->stmt_indent : st->stmt_indent;
    if (brk && bsub != FB_EXPR)
      f->base = st->line_indent;
    f->indent = f->base + (bsub == FB_EXTERN ? 0 : iw);
    f->s_stmt_indent = save.s_stmt_indent;
    f->s_prefix = save.s_prefix;
    f->s_tag = save.s_tag;
    st->pending_ctrl = 0;
    st->at_start = 1;
    st->stmt_tok = -1;
    st->prefix = 1;
    st->tag = 0;
    st->case_lbl = 0;
    st->just_opened = bsub != FB_EXTERN;
    if (!(ft_is_punct(st, n, "}") && st->toks[n].nl == 0))
      st->need_break = 1;
  } else if (role == R_INIT_OPEN || role == R_LIST_OPEN) {
    f = ft_push(st, bkind == FF_LIST ? FF_LIST : FF_INIT);
    f->base = st->line_indent;
    st->tag = 0;
  } else if (role == R_BLOCK_CLOSE) {
    f = ft_top(st);
    if (f && f->kind == FF_BLOCK) {
      int sub = f->sub, kw = f->kw;
      st->stmt_indent = f->s_stmt_indent;
      st->nframes--;
      if (sub == FB_STRUCT || sub == FB_EXPR) {
        st->at_start = 0;
        st->prefix = 1;
        st->tag = 0;
      } else {
        st->prev = i;
        ft_stmt_end(st, kw == KWC_IF || kw == KWC_DO ? kw : (kw ? KWC_MACRO : KWC_NONE));
      }
    }
  } else if (role == R_INIT_CLOSE || role == R_LIST_CLOSE) {
    st->nframes--;
  } else if (ft_is(st, i, ";")) {
    if (ft_stmt_scope(st)) {
      st->prev = i;
      ft_stmt_end(st, KWC_NONE);
    } else if ((f = ft_top(st)) && f->kind == FF_PAREN && (f->flags & FP_FOR)) {
      st->prefix = 0;
    }
  } else if (ft_is(st, i, ",")) {
    f = ft_top(st);
    if (f && f->kind == FF_PAREN && (f->flags & FP_DECL))
      st->prefix = 1;
  } else if (role == R_LABEL_COLON) {
    int was_case = st->case_lbl;
    st->case_lbl = 0;
    st->at_start = 1;
    st->stmt_tok = -1;
    if (was_case && ft_is_punct(st, n, "{"))
      st->pending_ctrl = KWC_LABEL; /* case 1: { */
    else if (was_case && st->style.short_case && n >= 0 && st->toks[n].nl == 0 && !ft_is(st, n, "case") &&
             !ft_is(st, n, "default"))
      st->pending_ctrl = 0; /* case 1: return x; as written */
    else
      st->need_break = 1, st->pending_ctrl = 0;
  } else if (ft_is(st, i, "?") && !ft_ptr_maybe(st, i)) {
    f = ft_top(st);
    if (f)
      f->ternary++;
  } else if (role == R_BINARY && ft_is(st, i, ":")) {
    f = ft_top(st);
    if (f && f->ternary > 0)
      f->ternary--;
  }
  if (t->kind == FK_PUNCT && role != R_PTR && role != R_ARG_CLOSE && role != R_TYPEBR_CLOSE &&
      !ft_is(st, i, "(") && !ft_is(st, i, "[") && !ft_is(st, i, ",") && !ft_is(st, i, ";") &&
      role != R_BLOCK_OPEN && role != R_BLOCK_CLOSE && role != R_TYPEBR_OPEN && ft_stmt_scope(st) &&
      !(ft_is(st, i, "!") && st->stmt_tok == i))
    st->prefix = 0;
  if (ft_is(st, i, "*") && role == R_PTR)
    st->tag = 0;
  if (ft_is(st, i, "=") || ft_is(st, i, ";"))
    st->tag = 0;
  st->prev = i;
  st->prev_any = i;
}

/* ------------------------------------------------------------------ */
/* comments and directives */

static void ft_add_align(FmtState *st, int ws_start, int code_col, int src_col)
{
  FmtAlign *a;
  ft_grow((void **)&st->aligns, &st->capaligns, st->naligns + 1, sizeof(FmtAlign));
  a = &st->aligns[st->naligns++];
  a->line = st->out_line;
  a->ws_start = ws_start;
  a->cstart = st->sink->size;
  a->code_col = code_col;
  a->src_col = src_col;
}

static void ft_comment(FmtState *st, int i)
{
  FmtTok *t = &st->toks[i];
  int ends_line = i + 1 >= st->ntoks || st->toks[i + 1].nl > 0;
  int single = t->kind == FK_LCOM || !memchr(st->src + t->start, '\n', t->len);
  if (t->nl == 0 && st->prev_any >= 0) {
    /* trailing */
    int ws = st->sink->size, code_col = st->col;
    int sp = t->kind == FK_LCOM ? 1 : (t->gap > 0);
    if (sp)
      ft_putc(st->sink, ' '), st->col++;
    if (ends_line && single && st->sink == &st->out)
      ft_add_align(st, ws, code_col, t->col);
    ft_write_tok(st, i);
  } else {
    int n = ft_next_sig(st, i), indent;
    FmtFrame *f = ft_top(st);
    int blank = t->nl >= 2 && !st->just_opened;
    FmtAlign *last = st->naligns ? &st->aligns[st->naligns - 1] : NULL;
    if (st->prev_any < 0 && st->out.size == 0)
      blank = 0;
    if (t->kind == FK_LCOM && t->nl == 1 && last && last->line == st->out_line && last->src_col == t->col &&
        st->prev_any >= 0 && st->toks[st->prev_any].kind == FK_LCOM && st->sink == &st->out) {
      /* a comment continuing the trailing one above */
      ft_newline(st, 0);
      ft_add_align(st, st->sink->size, -1, t->col);
      ft_write_tok(st, i);
      st->prev_any = i;
      return;
    }
    if (n >= 0 && ft_is_punct(st, n, "}") && f && f->kind == FF_BLOCK)
      indent = f->indent;
    else if (n >= 0 && ft_is_punct(st, n, "}") && f && f->kind == FF_PENDING)
      indent = ft_stmt_level(st);
    else if (n >= 0)
      indent = ft_line_indent(st, n);
    else
      indent = 0;
    if (st->prev_any < 0 && st->out.size == 0)
      ;
    else
      ft_newline(st, blank);
    ft_indent(st, indent);
    ft_write_tok(st, i);
    st->just_opened = 0;
  }
  st->prev_any = i;
}

static void ft_pp(FmtState *st, int i)
{
  FmtTok *t = &st->toks[i];
  if (st->out.size || st->prev_any >= 0)
    ft_newline(st, t->nl >= 2 && !st->just_opened);
  ft_write_tok(st, i);
  st->prev_any = i;
  st->just_opened = 0;
  ft_pp_state(st, i);
}

/* ------------------------------------------------------------------ */
/* trailing comments on neighbouring lines line up */

static void ft_align_comments(FmtState *st)
{
  FmtText o;
  int k = 0, from = 0;
  if (!st->naligns)
    return;
  memset(&o, 0, sizeof o);
  while (k < st->naligns) {
    int e = k + 1, target = 0, j;
    while (e < st->naligns && st->aligns[e].line == st->aligns[e - 1].line + 1)
      e++;
    for (j = k; j < e; j++)
      if (st->aligns[j].code_col >= 0 && st->aligns[j].code_col + 1 > target)
        target = st->aligns[j].code_col + 1;
    for (j = k; j < e; j++) {
      FmtAlign *a = &st->aligns[j];
      int col = a->code_col >= 0 ? a->code_col : 0;
      if (a->code_col >= 0 && a->cstart == a->ws_start)
        continue; /* written with no space: as it was */
      if (a->code_col < 0 && target == 0)
        continue;
      ft_cat(&o, st->out.data + from, a->ws_start - from);
      ft_spaces(&o, target - col);
      from = a->cstart;
    }
    k = e;
  }
  ft_cat(&o, st->out.data + from, st->out.size - from);
  tcc_free(st->out.data);
  st->out = o;
}

/* ------------------------------------------------------------------ */
/* the same tokens in and out */

static int ft_same_text(const char *a, int al, const char *b, int bl, int mode)
{
  /* mode 1: ignore all blanks; mode 2: ignore blanks at line ends */
  int i = 0, j = 0;
  if (mode == 0)
    return al == bl && !memcmp(a, b, al);
  for (;;) {
    if (mode == 1) {
      while (i < al && (a[i] == ' ' || a[i] == '\t' || a[i] == '\n' || a[i] == '\r'))
        i++;
      while (j < bl && (b[j] == ' ' || b[j] == '\t' || b[j] == '\n' || b[j] == '\r'))
        j++;
    } else {
      int ii = i, jj = j;
      while (ii < al && (a[ii] == ' ' || a[ii] == '\t' || a[ii] == '\r'))
        ii++;
      while (jj < bl && (b[jj] == ' ' || b[jj] == '\t' || b[jj] == '\r'))
        jj++;
      if ((ii == al || a[ii] == '\n') && (jj == bl || b[jj] == '\n'))
        i = ii, j = jj;
    }
    if (i == al || j == bl)
      return i == al && j == bl;
    if (a[i] != b[j])
      return 0;
    i++, j++;
  }
}

static int ft_verify(FmtState *st)
{
  FmtTok *out = NULL;
  int n = 0, cap = 0, k, ret = 0;
  FmtState chk;
  memset(&chk, 0, sizeof chk);
  chk.file = st->file;
  if (ft_lex(&chk, st->out.data, st->out.size, &out, &n, &cap) < 0)
    ret = -1;
  else if (n != st->ntoks)
    ret = -1;
  else
    for (k = 0; k < n && !ret; k++) {
      FmtTok *a = &st->toks[k], *b = &out[k];
      int mode = a->kind == FK_LCOM || a->kind == FK_BCOM ? 1 : a->kind == FK_PP ? 2 : 0;
      if (a->kind != b->kind ||
          !ft_same_text(st->src + a->start, a->len, st->out.data + b->start, b->len, mode)) {
        fprintf(stderr, "%s:%d: error: tcc -format would change this token: file left as it is (a bug in tcc -format)\n",
                st->file, a->line + 1);
        ret = -1;
      }
    }
  if (ret && n != st->ntoks)
    fprintf(stderr, "%s: error: tcc -format would change the tokens: file left as it is (a bug in tcc -format)\n",
            st->file);
  tcc_free(out);
  return ret;
}

/* ------------------------------------------------------------------ */
/* .tcc-format: the style, found next to the file or above it;
   .tcc-format-ignore: files left as they are */

static void ft_style_preset(FmtStyle *s, const char *v)
{
  s->wrap_func = s->wrap_struct = s->wrap_ctrl = s->wrap_else = 0;
  if (!strcmp(v, "linux"))
    s->wrap_func = 1;
  else if (!strcmp(v, "mozilla"))
    s->wrap_func = s->wrap_struct = 1;
  else if (!strcmp(v, "allman"))
    s->wrap_func = s->wrap_struct = s->wrap_ctrl = s->wrap_else = 1;
}

static int ft_bool(const char *v)
{
  return !strcmp(v, "true") || !strcmp(v, "yes") || !strcmp(v, "1");
}

/* an absolute path with no ".", ".." or "//" in it */
static void ft_abs_path(const char *in, char *out, int size)
{
  char tmp[2048];
  char *seg[256];
  int nseg = 0, k;
  char *p, *save;
  if (in[0] == '/')
    snprintf(tmp, sizeof tmp, "%s", in);
  else {
    char cwd[1024];
    if (!getcwd(cwd, sizeof cwd))
      strcpy(cwd, "/");
    snprintf(tmp, sizeof tmp, "%s/%s", cwd, in);
  }
  for (p = strtok_r(tmp, "/", &save); p; p = strtok_r(NULL, "/", &save)) {
    if (!strcmp(p, "."))
      continue;
    if (!strcmp(p, "..")) {
      if (nseg)
        nseg--;
      continue;
    }
    if (nseg < 256)
      seg[nseg++] = p;
  }
  out[0] = 0;
  for (k = 0; k < nseg; k++) {
    int l = (int)strlen(out);
    snprintf(out + l, size - l, "/%s", seg[k]);
  }
  if (!out[0])
    strcpy(out, "/");
}

/* read "Key: value" lines; returns 0 when the file is not there */
static int ft_read_style(FmtStyle *s, const char *path)
{
  char line[256];
  FILE *f = fopen(path, "r");
  int ln = 0;
  if (!f)
    return 0;
  while (fgets(line, sizeof line, f)) {
    char *k = line, *v, *e;
    ln++;
    while (*k == ' ' || *k == '\t')
      k++;
    if (*k == '#' || *k == '\n' || *k == '\r' || *k == 0 || !strncmp(k, "---", 3))
      continue;
    v = strchr(k, ':');
    if (!v) {
      fprintf(stderr, "%s:%d: warning: not 'Key: value'\n", path, ln);
      continue;
    }
    *v++ = 0;
    if ((e = strchr(v, '#')))
      *e = 0;
    for (e = v + strlen(v); e > v && strchr(" \t\r\n", e[-1]); e--)
      ;
    *e = 0;
    while (*v == ' ' || *v == '\t')
      v++;
    for (e = k + strlen(k); e > k && strchr(" \t", e[-1]); e--)
      ;
    *e = 0;
    if (!strcmp(k, "IndentWidth"))
      s->indent = atoi(v);
    else if (!strcmp(k, "ContinuationIndentWidth"))
      s->cont = atoi(v);
    else if (!strcmp(k, "BraceStyle")) {
      if (strcmp(v, "attach") && strcmp(v, "linux") && strcmp(v, "mozilla") && strcmp(v, "allman"))
        fprintf(stderr, "%s:%d: warning: BraceStyle is attach, linux, mozilla or allman\n", path, ln);
      ft_style_preset(s, v);
    } else if (!strcmp(k, "AllowShortCaseLabelsOnASingleLine"))
      s->short_case = ft_bool(v);
    else if (!strcmp(k, "DisableFormat"))
      s->disable = ft_bool(v);
    else
      fprintf(stderr, "%s:%d: warning: unknown key '%s'\n", path, ln, k);
  }
  fclose(f);
  if (s->indent < 0 || s->indent > 16)
    s->indent = 2;
  if (s->cont < 0 || s->cont > 16)
    s->cont = 4;
  return 1;
}

/* does a .tcc-format-ignore in dir name rel (the file's path below dir)? */
static int ft_ignored_by(const char *dir, const char *rel)
{
  char path[2100], line[512];
  const char *base = strrchr(rel, '/');
  FILE *f;
  int hit = 0;
  base = base ? base + 1 : rel;
  snprintf(path, sizeof path, "%s/.tcc-format-ignore", strcmp(dir, "/") ? dir : "");
  f = fopen(path, "r");
  if (!f)
    return 0;
  while (!hit && fgets(line, sizeof line, f)) {
    char *e, *pat = line;
    for (e = line + strlen(line); e > line && strchr(" \t\r\n", e[-1]); e--)
      ;
    *e = 0;
    while (*pat == ' ' || *pat == '\t')
      pat++;
    if (*pat == '#' || !*pat)
      continue;
    if (*pat == '/')
      pat++;
    if (strchr(pat, '/'))
      hit = !fnmatch(pat, rel, 0);
    else
      hit = !fnmatch(pat, base, 0) || !fnmatch(pat, rel, 0);
  }
  fclose(f);
  return hit;
}

/* the style for a file: defaults, then the nearest .tcc-format; 1 if ignored */
static int ft_load_style(FmtStyle *s, const char *for_file)
{
  char abs[2048], dir[2048], path[2100];
  int have_style = 0;
  char *slash;
  memset(s, 0, sizeof *s);
  s->indent = 2;
  s->cont = 4;
  ft_style_preset(s, "attach");
  ft_abs_path(for_file ? for_file : "stdin.c", abs, sizeof abs);
  strcpy(dir, abs);
  for (;;) {
    slash = strrchr(dir, '/');
    if (!slash)
      break;
    if (slash == dir) {
      if (dir[1] == 0)
        break;
      dir[1] = 0;
    } else
      *slash = 0;
    /* dir is now a directory above the file */
    if (for_file && ft_ignored_by(dir, abs + strlen(dir) + (dir[1] != 0)))
      return 1;
    if (!have_style) {
      snprintf(path, sizeof path, "%s/.tcc-format", strcmp(dir, "/") ? dir : "");
      have_style = ft_read_style(s, path);
    }
    if (!strcmp(dir, "/"))
      break;
  }
  return 0;
}

/* ------------------------------------------------------------------ */

/* format src; the result in st->out. 0, or -1 with the file left alone */
static int ft_format(FmtState *st)
{
  int i, k, crlf = 0;
  char *norm = NULL;
  /* CRLF in, CRLF out */
  if (memchr(st->src, '\r', st->len)) {
    int j = 0;
    norm = tcc_malloc(st->len + 1);
    for (k = 0; k < st->len; k++) {
      if (st->src[k] == '\r' && k + 1 < st->len && st->src[k + 1] == '\n') {
        crlf = 1;
        continue;
      }
      norm[j++] = st->src[k];
    }
    norm[j] = 0;
    st->src = norm;
    st->len = j;
  }
  if (ft_lex(st, st->src, st->len, &st->toks, &st->ntoks, &st->captoks) < 0) {
    tcc_free(norm);
    return -1;
  }
  /* line starts */
  st->nlines = 1;
  for (k = 0; k < st->len; k++)
    st->nlines += st->src[k] == '\n';
  st->line_off = tcc_malloc(sizeof(int) * (st->nlines + 1));
  st->line_off[0] = 0;
  for (k = 0, i = 1; k < st->len; k++)
    if (st->src[k] == '\n')
      st->line_off[i++] = k + 1;
  st->role = tcc_mallocz(st->ntoks + 1);
  ft_collect_typedefs(st);
  ft_mark_verbatim(st);
  st->sink = &st->out;
  st->prev = st->prev_any = -1;
  st->at_start = 1;
  st->prefix = 1;
  st->stmt_tok = -1;
  for (i = 0; i < st->ntoks; i++) {
    FmtTok *t = &st->toks[i];
    if (t->verbatim == 1 || (t->verbatim == 2 && (i == 0 || st->toks[i - 1].verbatim != 2))) {
      int e = i, v = t->verbatim, last_line;
      while (e + 1 < st->ntoks && st->toks[e + 1].verbatim == v)
        e++;
      last_line = ft_tok_end_line(st, e);
      st->sink = &st->out;
      if (st->out.size || st->prev_any >= 0)
        ft_newline(st, t->nl >= 2);
      ft_copy_lines(st, t->line, last_line);
      if (v == 1) {
        i = e;
        continue;
      }
      st->sink = &st->junk;
    }
    if (t->verbatim != 2 && st->sink == &st->junk) {
      st->sink = &st->out;
      st->col = 0;
      st->junk.size = 0;
    }
    if (t->kind == FK_PP)
      ft_pp(st, i);
    else if (t->kind == FK_LCOM || t->kind == FK_BCOM)
      ft_comment(st, i);
    else
      ft_sig(st, i);
    if (t->verbatim == 2)
      st->prev_any = i, st->toks[i].verbatim = 2;
  }
  st->sink = &st->out;
  if (st->out.size) {
    ft_trim(st);
    if (!ft_at_bol(st))
      ft_putc(&st->out, '\n');
  }
  ft_align_comments(st);
  i = ft_verify(st);
  if (!i && crlf) {
    FmtText o;
    memset(&o, 0, sizeof o);
    for (k = 0; k < st->out.size; k++) {
      if (st->out.data[k] == '\n')
        ft_putc(&o, '\r');
      ft_putc(&o, st->out.data[k]);
    }
    tcc_free(st->out.data);
    st->out = o;
  }
  for (k = 0; k < st->nsnaps; k++)
    ft_snap_free(&st->snaps[k]);
  tcc_free(norm);
  st->src = NULL;
  return i;
}

static void ft_state_free(FmtState *st)
{
  int k;
  for (k = 0; k < st->ntypes; k++)
    tcc_free(st->types[k]);
  tcc_free(st->types);
  tcc_free(st->toks);
  tcc_free(st->line_off);
  tcc_free(st->role);
  tcc_free(st->frames);
  tcc_free(st->snaps);
  tcc_free(st->out.data);
  tcc_free(st->junk.data);
  tcc_free(st->aligns);
}

static char *ft_read(FILE *f, int *len)
{
  FmtText t;
  char buf[4096];
  size_t n;
  memset(&t, 0, sizeof t);
  while ((n = fread(buf, 1, sizeof buf, f)) > 0)
    ft_cat(&t, buf, (int)n);
  if (!t.data)
    ft_cat(&t, "", 0);
  *len = t.size;
  return t.data;
}

static int ft_usage(int code)
{
  fprintf(code ? stderr : stdout,
          "usage: tcc -format [-i] [-n] [-assume-filename=PATH] [FILE...]\n"
          "  lays out C and yasc source; prints it, or with -i rewrites the files\n"
          "  -i                  rewrite the files (only those that change)\n"
          "  -n                  change nothing: name the files that would change, exit 1 if any\n"
          "  -assume-filename=P  the path stdin stands for (finds its .tcc-format)\n"
          "  no FILE, or '-': stdin to stdout\n"
          "  the style: .tcc-format next to the file or above it (docs/tcc_format.md)\n");
  return code;
}

/* format one file (or stdin with path NULL); 0 same, 1 changed, -1 failed */
static int ft_one(const char *path, const char *assume, int inplace, int check)
{
  FmtState st;
  FILE *f;
  char *src;
  int len, ret, changed;
  const char *name = path ? path : (assume ? assume : "<stdin>");
  memset(&st, 0, sizeof st);
  if (ft_load_style(&st.style, path ? path : assume))
    st.style.disable = 1;
  if (path) {
    f = fopen(path, "rb");
    if (!f) {
      fprintf(stderr, "%s: error: cannot read it\n", path);
      return -1;
    }
    src = ft_read(f, &len);
    fclose(f);
  } else
    src = ft_read(stdin, &len);
  if (st.style.disable) {
    if (!inplace && !check)
      fwrite(src, 1, len, stdout);
    tcc_free(src);
    return 0;
  }
  st.file = name;
  st.src = src;
  st.len = len;
  ret = ft_format(&st);
  if (ret < 0) {
    /* stdout still gets the text, unchanged, so an editor keeps it */
    if (!inplace && !check)
      fwrite(src, 1, len, stdout);
    tcc_free(src);
    ft_state_free(&st);
    return -1;
  }
  changed = st.out.size != len || memcmp(st.out.data, src, len);
  if (check) {
    if (changed)
      printf("%s\n", name);
  } else if (inplace && path) {
    if (changed) {
      f = fopen(path, "wb");
      if (!f || fwrite(st.out.data, 1, st.out.size, f) != (size_t)st.out.size) {
        fprintf(stderr, "%s: error: cannot write it\n", path);
        changed = -1;
      }
      if (f && fclose(f))
        changed = -1;
    }
  } else
    fwrite(st.out.data, 1, st.out.size, stdout);
  tcc_free(src);
  ft_state_free(&st);
  return changed;
}

ST_FUNC int tcc_tool_format(TCCState *s1, int argc, char **argv)
{
  const char *assume = NULL;
  int i, inplace = 0, check = 0, nfiles = 0, failed = 0, changed = 0, r;
  (void)s1;
  for (i = 1; i < argc; i++) {
    const char *a = argv[i];
    if (!strcmp(a, "-i"))
      inplace = 1;
    else if (!strcmp(a, "-n") || !strcmp(a, "--dry-run") || !strcmp(a, "-check"))
      check = 1;
    else if (!strncmp(a, "-assume-filename=", 17))
      assume = a + 17;
    else if (!strncmp(a, "--assume-filename=", 18))
      assume = a + 18;
    else if (!strcmp(a, "-h") || !strcmp(a, "--help") || !strcmp(a, "-help"))
      return ft_usage(0);
    else if (a[0] == '-' && a[1])
      return ft_usage(1);
    else
      nfiles++;
  }
  if (!nfiles) {
    r = ft_one(NULL, assume, 0, check);
    return r < 0 ? 1 : (check && r > 0);
  }
  for (i = 1; i < argc; i++) {
    const char *a = argv[i];
    if (a[0] == '-' && a[1])
      continue;
    r = ft_one(strcmp(a, "-") ? a : NULL, assume, inplace, check);
    if (r < 0)
      failed = 1;
    else if (r > 0)
      changed = 1;
  }
  return failed ? 1 : (check && changed);
}
