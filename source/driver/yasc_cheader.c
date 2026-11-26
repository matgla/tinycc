/*
 *  TCC yasc C headers - a regular C header written from a yasc one (tcc -yasc-c-header)
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

/* What it writes and why: docs/yasc_c_headers.md. #included by tcctools.c. */

#include "memory/vector.h"

enum
{
  YH_WS,
  YH_COMMENT,
  YH_PP,
  YH_ID,
  YH_NUM,
  YH_STR,
  YH_PUNCT
};

typedef struct YhTok
{
  int kind;
  int start, end;
  int del;
  const char *repl;
  char ins[160];
} YhTok;

TCC_VECTOR_DEFINE(YhToks, YhTok);

typedef struct YhDropper
{
  char name[64];
  int arg;
} YhDropper;

TCC_VECTOR_DEFINE(YhDroppers, YhDropper);

enum
{
  YH_FRAME_PLAIN,
  YH_FRAME_YASC,
  YH_FRAME_NOT_YASC
};

TCC_VECTOR_DEFINE(YhFrames, int);
TCC_VECTOR_DEFINE(YhText, char);

static void yh_cat(YhText *t, const char *s, int n)
{
  size_t at = t->size;
  if (n < 0)
    n = (int)strlen(s);
  if (n > 0 && YhText_resize(t, at + n) == 0)
    memcpy(t->data + at, s, n);
}

typedef struct YhState
{
  const char *file;
  const char *src;
  int len;
  YhToks toks;
  YhDroppers droppers;
  YhFrames frames;
  YhText out;
  int c_region;
  int need_attrs;
  int errors;
} YhState;

static void yh_error(YhState *st, int off, const char *msg, const char *what)
{
  int line = 1, i;
  for (i = 0; i < off && i < st->len; i++)
    line += st->src[i] == '\n';
  fprintf(stderr, "%s:%d: error: %s%s%s\n", st->file, line, msg, what ? ": " : "", what ? what : "");
  st->errors++;
}

static int yh_is_space(int c)
{
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

static int yh_is_idchar(int c)
{
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '$';
}

static int yh_skip_quoted(const char *s, int i, int end)
{
  int q = s[i++];
  while (i < end && s[i] != q && s[i] != '\n') {
    if (s[i] == '\\' && i + 1 < end)
      i++;
    i++;
  }
  return i < end && s[i] == q ? i + 1 : i;
}

static void yh_tokenize(YhState *st, int from, int end, int allow_pp, YhToks *toks)
{
  const char *s = st->src;
  int i = from, bol = 1;
  while (i < end) {
    YhTok t;
    int c = (unsigned char)s[i];
    memset(&t, 0, sizeof t);
    t.start = i;
    if (yh_is_space(c)) {
      while (i < end && yh_is_space((unsigned char)s[i])) {
        if (s[i] == '\n')
          bol = 1;
        i++;
      }
      t.kind = YH_WS;
    } else if (c == '/' && i + 1 < end && s[i + 1] == '*') {
      i += 2;
      while (i + 1 < end && !(s[i] == '*' && s[i + 1] == '/'))
        i++;
      i = i + 2 < end ? i + 2 : end;
      t.kind = YH_COMMENT;
    } else if (c == '/' && i + 1 < end && s[i + 1] == '/') {
      while (i < end && s[i] != '\n')
        i++;
      t.kind = YH_COMMENT;
    } else if (c == '#' && bol && allow_pp) {
      while (i < end) {
        if (s[i] == '\\' && i + 1 < end && s[i + 1] == '\n')
          i += 2;
        else if (s[i] == '/' && i + 1 < end && s[i + 1] == '*') {
          i += 2;
          while (i + 1 < end && !(s[i] == '*' && s[i + 1] == '/'))
            i++;
          i += 2;
        } else if (s[i] == '"' || s[i] == '\'')
          i = yh_skip_quoted(s, i, end);
        else if (s[i] == '\n') {
          i++;
          break;
        } else
          i++;
      }
      if (i > end)
        i = end;
      t.kind = YH_PP;
      bol = 1;
    } else if (yh_is_idchar(c) && !(c >= '0' && c <= '9')) {
      while (i < end && yh_is_idchar((unsigned char)s[i]))
        i++;
      t.kind = YH_ID;
    } else if ((c >= '0' && c <= '9') || (c == '.' && i + 1 < end && s[i + 1] >= '0' && s[i + 1] <= '9')) {
      while (i < end && (yh_is_idchar((unsigned char)s[i]) || s[i] == '.' ||
                         ((s[i] == '+' || s[i] == '-') && strchr("eEpP", s[i - 1]))))
        i++;
      t.kind = YH_NUM;
    } else if (c == '"' || c == '\'') {
      i = yh_skip_quoted(s, i, end);
      t.kind = YH_STR;
    } else {
      i++;
      t.kind = YH_PUNCT;
    }
    t.end = i;
    if (t.kind != YH_WS && t.kind != YH_COMMENT && t.kind != YH_PP)
      bol = 0;
    YhToks_push_back(toks, t);
  }
}

static int yh_is(YhState *st, YhTok *t, const char *word)
{
  int n = (int)strlen(word);
  return (t->kind == YH_ID || t->kind == YH_PUNCT) && t->end - t->start == n && !memcmp(st->src + t->start, word, n);
}

static int yh_sig(YhTok *t)
{
  return t->kind != YH_WS && t->kind != YH_COMMENT;
}

static int yh_next(YhToks *v, int i, int end)
{
  while (i < end && !yh_sig(&v->data[i]))
    i++;
  return i;
}

static int yh_prev(YhToks *v, int i, int start)
{
  for (i--; i >= start; i--)
    if (yh_sig(&v->data[i]))
      return i;
  return -1;
}

static int yh_close(YhState *st, YhToks *v, int i, int end)
{
  int depth = 0;
  for (; i < end; i++) {
    YhTok *t = &v->data[i];
    if (yh_is(st, t, "(") || yh_is(st, t, "[") || yh_is(st, t, "{"))
      depth++;
    else if (yh_is(st, t, ")") || yh_is(st, t, "]") || yh_is(st, t, "}")) {
      if (--depth == 0)
        return i;
    }
  }
  return end;
}

static int yh_word_with_group(YhState *st, YhTok *t)
{
  return yh_is(st, t, "owned") || yh_is(st, t, "borrows") || yh_is(st, t, "takes");
}

static int yh_word_alone(YhState *st, YhTok *t)
{
  return yh_is(st, t, "view") || yh_is(st, t, "maybe") || yh_is(st, t, "drop") || yh_is(st, t, "out") ||
         yh_is(st, t, "inout");
}

/* Leaves out the yasc words of tokens [a, b); body: a function's, where yasc statements have no C form. */
static void yh_strip(YhState *st, YhToks *v, int a, int b, int body)
{
  static const char *const statements[] = {"defer", "errdefer", "try",       "catch",      "unsafe",
                                           "unreachable", "undefined", "error_from", NULL};
  int i, k;
  for (i = a; i < b; i++) {
    YhTok *t = &v->data[i];
    int p, n;
    if (!yh_sig(t) || t->del)
      continue;
    if (yh_word_with_group(st, t)) {
      t->del = 1;
      n = yh_next(v, i + 1, b);
      if (n < b && yh_is(st, &v->data[n], "(")) {
        int c = yh_close(st, v, n, b);
        for (k = i + 1; k <= c && k < b; k++)
          v->data[k].del = 1;
        i = c;
      }
    } else if (yh_word_alone(st, t)) {
      t->del = 1;
    } else if (yh_is(st, t, "in")) {
      p = yh_prev(v, i, a);
      n = yh_next(v, i + 1, b);
      if (p >= 0 && (yh_is(st, &v->data[p], "(") || yh_is(st, &v->data[p], ",")) && n < b && v->data[n].kind == YH_ID)
        t->del = 1;
    } else if (yh_is(st, t, "nullptr")) {
      t->repl = "((void *)0)";
    } else if (yh_is(st, t, "[")) {
      int c = yh_close(st, v, i, b);
      p = yh_prev(v, i, a);
      if (p >= 0 && (v->data[p].kind == YH_ID || yh_is(st, &v->data[p], ")") || yh_is(st, &v->data[p], "]"))) {
        for (k = i + 1; k < c; k++)
          if (yh_is(st, &v->data[k], ":")) {
            v->data[k].del = 1;
            n = yh_next(v, k + 1, c);
            if (n < c)
              v->data[n].del = 1;
          }
      } else
        yh_error(st, t->start, "a yasc slice has no C form: declare it under #ifdef __YASC__", NULL);
    } else if (body && t->kind == YH_ID) {
      for (k = 0; statements[k]; k++)
        if (yh_is(st, t, statements[k]))
          yh_error(st, t->start, "a yasc statement in an inline function has no C form: define it under #ifdef __YASC__",
                   statements[k]);
    }
  }
}

typedef struct YhLevel
{
  int pointer;
  int view, owned, maybe, drop, borrows;
  char dealloc[64];
} YhLevel;

/* The qualifiers of the declarator's own level in [a, b): after its last '*' (a parameter's brackets: implicit view). */
static void yh_level(YhState *st, YhToks *v, int a, int b, int param, YhLevel *lv)
{
  int i, star = -1, depth = 0, group = -1, bracket = -1;
  memset(lv, 0, sizeof *lv);
  for (i = a; i < b; i++) {
    YhTok *t = &v->data[i];
    if (!yh_sig(t))
      continue;
    if (yh_is(st, t, "(") || yh_is(st, t, "[")) {
      if (depth == 0 && yh_is(st, t, "(") && group < 0 && star < 0) {
        int n = yh_next(v, i + 1, b);
        if (n < b && yh_is(st, &v->data[n], "*"))
          group = i;
      }
      if (depth == 0 && yh_is(st, t, "[") && bracket < 0)
        bracket = i;
      depth++;
    } else if (yh_is(st, t, ")") || yh_is(st, t, "]")) {
      depth--;
    } else if (depth == 0 && yh_is(st, t, "*")) {
      star = i;
    }
  }
  if (bracket >= 0 && !param)
    return;
  if (bracket >= 0) {
    int c = yh_close(st, v, bracket, b);
    lv->pointer = 1;
    lv->view = 1;
    for (i = bracket + 1; i < c; i++)
      if (yh_is(st, &v->data[i], "maybe"))
        lv->maybe = 1;
    return;
  }
  if (star < 0 && group >= 0) {
    int c = yh_close(st, v, group, b);
    yh_level(st, v, group + 1, c, param, lv);
    return;
  }
  lv->pointer = star >= 0;
  for (i = star >= 0 ? star + 1 : a; i < b; i++) {
    YhTok *t = &v->data[i];
    if (!yh_sig(t))
      continue;
    if (yh_is(st, t, "view"))
      lv->view = 1;
    else if (yh_is(st, t, "maybe"))
      lv->maybe = 1;
    else if (yh_is(st, t, "drop"))
      lv->drop = 1;
    else if (yh_is(st, t, "borrows"))
      lv->borrows = 1;
    else if (yh_is(st, t, "owned")) {
      int n = yh_next(v, i + 1, b);
      lv->owned = 1;
      if (n < b && yh_is(st, &v->data[n], "(")) {
        int d = yh_next(v, n + 1, b);
        if (d < b && v->data[d].kind == YH_ID && v->data[d].end - v->data[d].start < (int)sizeof lv->dealloc) {
          memcpy(lv->dealloc, st->src + v->data[d].start, v->data[d].end - v->data[d].start);
          lv->dealloc[v->data[d].end - v->data[d].start] = 0;
        }
      }
    }
  }
}

static int yh_type_word(YhState *st, YhTok *t)
{
  static const char *const words[] = {"void",     "char",   "short",  "int",      "long",   "float",    "double",
                                      "signed",   "unsigned", "const", "volatile", "struct", "union",    "enum",
                                      "extern",   "static", "inline", "register", "_Bool",  "_Noreturn", "typedef",
                                      "restrict", "__restrict", NULL};
  int k;
  for (k = 0; words[k]; k++)
    if (yh_is(st, t, words[k]))
      return 1;
  return 0;
}

static int yh_attr_word(YhState *st, YhTok *t)
{
  const char *s = st->src + t->start;
  int n = t->end - t->start;
  return (n >= 5 && !memcmp(s, "__yl_", 5)) || (n >= 13 && !memcmp(s, "__attribute__", 13)) ||
         (n >= 5 && !memcmp(s, "__asm", 5)) || yh_is(st, t, "asm") || yh_is(st, t, "_Alignas") ||
         yh_is(st, t, "__declspec");
}

static int yh_has_word(YhState *st, YhToks *v, int a, int b, const char *word)
{
  int i;
  for (i = a; i < b; i++)
    if (yh_is(st, &v->data[i], word))
      return 1;
  return 0;
}

static void yh_append(char *buf, size_t size, const char *text)
{
  size_t n = strlen(buf);
  if (n + strlen(text) < size)
    strcpy(buf + n, text);
}

/* A prototype: its name at name, its parameters in (lp, rp); the attributes go before the ';' at semi. */
static void yh_prototype(YhState *st, YhToks *v, int a, int name, int lp, int rp, int semi)
{
  YhLevel res;
  char attrs[160] = "", num[96], cstr[64] = "";
  int i, start = lp + 1, depth = 0, index = 0, drop_arg = 0;
  yh_level(st, v, a, name, 0, &res);
  for (i = lp + 1; i <= rp; i++) {
    YhTok *t = &v->data[i];
    if (yh_is(st, t, "(") || yh_is(st, t, "["))
      depth++;
    else if ((yh_is(st, t, ")") || yh_is(st, t, "]")) && i != rp)
      depth--;
    if (i == rp || (depth == 0 && yh_is(st, t, ","))) {
      int f = yh_next(v, start, i);
      if (f < i && !(yh_is(st, &v->data[f], "void") && yh_next(v, f + 1, i) == i) && !yh_is(st, &v->data[f], ".")) {
        index++;
        if (yh_has_word(st, v, start, i, ":") && !yh_has_word(st, v, rp, semi, "__yl_cstr")) {
          snprintf(num, sizeof num, " __yl_cstr(%d)", index);
          yh_append(cstr, sizeof cstr, num);
        }
        if (yh_has_word(st, v, start, i, "drop") && !drop_arg)
          drop_arg = index;
      }
      start = i + 1;
    }
  }
  if (drop_arg && v->data[name].end - v->data[name].start < 64) {
    YhDropper d;
    memcpy(d.name, st->src + v->data[name].start, v->data[name].end - v->data[name].start);
    d.name[v->data[name].end - v->data[name].start] = 0;
    d.arg = drop_arg;
    YhDroppers_push_back(&st->droppers, d);
  }
  if (res.pointer && res.owned && !yh_has_word(st, v, rp, semi, "__yl_dealloc")) {
    const char *dealloc = res.dealloc[0] ? res.dealloc : "free";
    int arg = 1;
    size_t k;
    for (k = 0; k < st->droppers.size; k++)
      if (!strcmp(st->droppers.data[k].name, dealloc))
        arg = st->droppers.data[k].arg;
    snprintf(num, sizeof num, " __yl_dealloc(%.40s, %d)", dealloc, arg);
    yh_append(attrs, sizeof attrs, num);
  }
  yh_append(attrs, sizeof attrs, cstr);
  if (attrs[0]) {
    yh_append(v->data[semi].ins, sizeof v->data[semi].ins, attrs);
    st->need_attrs = 1;
  }
}

/* A struct's members [a, b): a `view` member pointer becomes __yl_view (or __yl_view_maybe). */
static void yh_members(YhState *st, YhToks *v, int a, int b)
{
  int i, start = a, depth = 0;
  for (i = a; i < b; i++) {
    YhTok *t = &v->data[i];
    if (yh_is(st, t, "(") || yh_is(st, t, "[") || yh_is(st, t, "{"))
      depth++;
    else if (yh_is(st, t, ")") || yh_is(st, t, "]") || yh_is(st, t, "}"))
      depth--;
    else if (depth == 0 && (yh_is(st, t, ",") || yh_is(st, t, ";"))) {
      YhLevel lv;
      int first = yh_next(v, start, i);
      if (first < i && !yh_has_word(st, v, first, i, "{")) {
        yh_level(st, v, start, i, 0, &lv);
        if (lv.pointer && lv.view) {
          yh_append(t->ins, sizeof t->ins, lv.maybe ? " __yl_view_maybe" : " __yl_view");
          st->need_attrs = 1;
        }
      }
      start = i + 1;
    }
  }
}

/* Declaration [a, b) of the main token list. */
static void yh_declaration(YhState *st, int a, int b)
{
  YhToks *v = &st->toks;
  int i, first = yh_next(v, a, b), depth = 0, brace = -1, name = -1, lp = -1;
  if (first >= b)
    return;
  for (i = first; i < b; i++) {
    YhTok *t = &v->data[i];
    if (!yh_sig(t))
      continue;
    if (t->kind == YH_PP) {
      yh_error(st, t->start, "a preprocessor line inside a yasc declaration: split the declaration", NULL);
      return;
    }
    if (depth == 0 && yh_is(st, t, "{") && brace < 0)
      brace = i;
    if (depth == 0 && name < 0 && brace < 0 && t->kind == YH_ID && !yh_type_word(st, t) && !yh_attr_word(st, t) &&
        !yh_word_with_group(st, t)) {
      int n = yh_next(v, i + 1, b);
      if (n < b && yh_is(st, &v->data[n], "(")) {
        int g = yh_next(v, n + 1, b);
        if (g < b && !yh_is(st, &v->data[g], "*") && !yh_is(st, &v->data[g], "^")) {
          name = i;
          lp = n;
        }
      }
    }
    if (yh_is(st, t, "(") || yh_is(st, t, "[") || yh_is(st, t, "{"))
      depth++;
    else if (yh_is(st, t, ")") || yh_is(st, t, "]") || yh_is(st, t, "}"))
      depth--;
  }
  if (yh_is(st, &v->data[first], "typedef")) {
    yh_strip(st, v, a, b, 0);
    return;
  }
  if (brace >= 0) {
    int p = yh_prev(v, brace, a);
    int body = p >= 0 && yh_is(st, &v->data[p], ")");
    if (!body) {
      int c = yh_close(st, v, brace, b);
      yh_members(st, v, brace + 1, c);
    }
    yh_strip(st, v, a, b, body);
    return;
  }
  if (name >= 0) {
    int rp = yh_close(st, v, lp, b);
    int semi = yh_prev(v, b, a);
    int after = yh_next(v, rp + 1, b);
    if (semi >= 0 && yh_is(st, &v->data[semi], ";") && !(after < b && yh_is(st, &v->data[after], "(")))
      yh_prototype(st, v, a, name, lp, rp, semi);
  }
  yh_strip(st, v, a, b, 0);
}

/* Where declaration a ends: after its ';', or after a function body's '}'. */
static int yh_declaration_end(YhState *st, int a)
{
  YhToks *v = &st->toks;
  int i, depth = 0, body_depth = -1;
  for (i = a; i < (int)v->size; i++) {
    YhTok *t = &v->data[i];
    if (!yh_sig(t) || t->kind == YH_PP)
      continue;
    if (yh_is(st, t, "{")) {
      int p = yh_prev(v, i, a);
      if (depth == 0 && p >= 0 && yh_is(st, &v->data[p], ")"))
        body_depth = 0;
      depth++;
    } else if (yh_is(st, t, "(") || yh_is(st, t, "["))
      depth++;
    else if (yh_is(st, t, ")") || yh_is(st, t, "]") || yh_is(st, t, "}")) {
      depth--;
      if (depth == body_depth && yh_is(st, t, "}"))
        return i + 1;
    } else if (depth == 0 && yh_is(st, t, ";"))
      return i + 1;
  }
  yh_error(st, v->data[a].start, "a declaration without its ';'", NULL);
  return (int)v->size;
}

static void yh_emit_range(YhState *st, YhToks *v, int a, int b)
{
  int i, last = -1;
  for (i = a; i < b; i++) {
    YhTok *t = &v->data[i];
    const char *text = t->repl ? t->repl : st->src + t->start;
    int n = t->repl ? (int)strlen(t->repl) : t->end - t->start;
    if (t->ins[0])
      yh_cat(&st->out, t->ins, -1);
    if (t->del)
      continue;
    if (t->kind == YH_WS) {
      int nx = i + 1 < b ? i + 1 : -1;
      if (i > a && v->data[i - 1].del)
        continue;
      if (last >= 0 && yh_is(st, &v->data[last], "*") && nx >= 0 && v->data[nx].del)
        continue;
    } else {
      if (st->out.size > 0 && yh_is_idchar((unsigned char)st->out.data[st->out.size - 1]) &&
          yh_is_idchar((unsigned char)text[0]))
        yh_cat(&st->out, " ", 1);
      last = i;
    }
    yh_cat(&st->out, text, n);
  }
}

/* The line without blanks, continuations and comments: what the special directives are matched on. */
static void yh_squeeze(YhState *st, YhTok *t, char *buf, int size)
{
  const char *s = st->src;
  int i = t->start, n = 0;
  while (i < t->end && n < size - 1) {
    if (s[i] == '/' && i + 1 < t->end && s[i + 1] == '*') {
      i += 2;
      while (i + 1 < t->end && !(s[i] == '*' && s[i + 1] == '/'))
        i++;
      i += 2;
    } else if (s[i] == '/' && i + 1 < t->end && s[i + 1] == '/')
      break;
    else if (s[i] == '\\' || yh_is_space((unsigned char)s[i]))
      i++;
    else
      buf[n++] = s[i++];
  }
  buf[n] = 0;
}

static int yh_starts(const char *s, const char *prefix)
{
  return !strncmp(s, prefix, strlen(prefix));
}

static void yh_define(YhState *st, YhTok *t)
{
  YhToks sub;
  YhToks_init(&sub);
  yh_tokenize(st, t->start, t->end, 0, &sub);
  yh_strip(st, &sub, 0, (int)sub.size, 0);
  yh_emit_range(st, &sub, 0, (int)sub.size);
  YhToks_cleanup(&sub);
}

static int yh_run(YhState *st, const char *include_as)
{
  YhToks *v = &st->toks;
  int i, skip = 0, skip_depth = 0, skip_frame = 0, lead;
  const char *p;
  char line[256];
  YhText body;
  yh_tokenize(st, 0, st->len, 1, v);
  for (i = 0; i < (int)v->size;) {
    YhTok *t = &v->data[i];
    if (t->kind == YH_PP) {
      yh_squeeze(st, t, line, sizeof line);
      if (skip) {
        if (yh_starts(line, "#if"))
          skip_depth++;
        else if (skip_depth == 0 && yh_starts(line, "#elif"))
          yh_error(st, t->start, "#elif after a __YASC__ condition: write #else", NULL);
        else if (skip_depth == 0 && yh_starts(line, "#else") && skip_frame == YH_FRAME_YASC)
          skip = 0, st->frames.data[st->frames.size - 1] = YH_FRAME_NOT_YASC;
        else if (yh_starts(line, "#endif")) {
          if (skip_depth == 0) {
            skip = 0;
            st->frames.size--;
          } else
            skip_depth--;
        }
        i++;
        continue;
      }
      if (!strcmp(line, "#pragmayasc")) {
      } else if (!strcmp(line, "#pragmatccc_begin")) {
        st->c_region = 1;
      } else if (!strcmp(line, "#pragmatccc_end")) {
        st->c_region = 0;
      } else if (!strcmp(line, "#ifdef__YASC__") || !strcmp(line, "#ifdefined(__YASC__)") ||
                 !strcmp(line, "#ifdefined__YASC__")) {
        YhFrames_push_back(&st->frames, YH_FRAME_YASC);
        skip = 1, skip_depth = 0, skip_frame = YH_FRAME_YASC;
      } else if (!strcmp(line, "#ifndef__YASC__") || !strcmp(line, "#if!defined(__YASC__)") ||
                 !strcmp(line, "#if!defined__YASC__")) {
        YhFrames_push_back(&st->frames, YH_FRAME_NOT_YASC);
      } else if (yh_starts(line, "#if")) {
        YhFrames_push_back(&st->frames, YH_FRAME_PLAIN);
        yh_emit_range(st, v, i, i + 1);
      } else if (yh_starts(line, "#elif") || yh_starts(line, "#else") || yh_starts(line, "#endif")) {
        int top = st->frames.size ? st->frames.data[st->frames.size - 1] : YH_FRAME_PLAIN;
        if (top == YH_FRAME_PLAIN)
          yh_emit_range(st, v, i, i + 1);
        else if (yh_starts(line, "#elif"))
          yh_error(st, t->start, "#elif after a __YASC__ condition: write #else", NULL);
        else if (yh_starts(line, "#else"))
          skip = 1, skip_depth = 0, skip_frame = YH_FRAME_NOT_YASC;
        if (yh_starts(line, "#endif") && st->frames.size)
          st->frames.size--;
      } else if (yh_starts(line, "#define") && !st->c_region) {
        yh_define(st, t);
      } else {
        yh_emit_range(st, v, i, i + 1);
      }
      i++;
      continue;
    }
    if (skip) {
      i++;
      continue;
    }
    if (st->c_region || !yh_sig(t) || yh_is(st, t, "}")) {
      yh_emit_range(st, v, i, i + 1);
      i++;
      continue;
    }
    if (yh_is(st, t, "extern")) {
      int q = yh_next(v, i + 1, (int)v->size), b = q < (int)v->size ? yh_next(v, q + 1, (int)v->size) : q;
      if (q < (int)v->size && v->data[q].kind == YH_STR && b < (int)v->size && yh_is(st, &v->data[b], "{")) {
        yh_emit_range(st, v, i, b + 1);
        i = b + 1;
        continue;
      }
    }
    {
      int end = yh_declaration_end(st, i);
      yh_declaration(st, i, end);
      yh_emit_range(st, v, i, end);
      i = end;
    }
  }
  if (st->c_region)
    yh_error(st, st->len, "#pragma tcc c_begin without its c_end", NULL);
  if (skip || st->frames.size)
    yh_error(st, st->len, "a conditional without its #endif", NULL);
  YhText_init(&body);
  for (i = 0; i < (int)v->size && !yh_sig(&v->data[i]); i++)
    ;
  lead = i < (int)v->size ? v->data[i].start : st->len;
  yh_cat(&body, st->out.data, lead);
  yh_cat(&body, "/* Written by `tcc -yasc-c-header` from ", -1);
  yh_cat(&body, include_as, -1);
  yh_cat(&body, ": edit that file, not this one.\n   A yasc unit reads the yasc header itself. */\n#ifdef __YASC__\n#include \"",
         -1);
  for (p = include_as + 5; *p; p++)
    if (*p == '/')
      yh_cat(&body, "../", -1);
  yh_cat(&body, include_as, -1);
  yh_cat(&body, "\"\n#else\n", -1);
  if (st->need_attrs && !strstr(st->src, "sys/_attrs.h"))
    yh_cat(&body, "#include <sys/_attrs.h>\n", -1);
  yh_cat(&body, st->out.data + lead, (int)st->out.size - lead);
  if (body.size && body.data[body.size - 1] != '\n')
    yh_cat(&body, "\n", 1);
  yh_cat(&body, "#endif /* __YASC__ */\n", -1);
  YhText_cleanup(&st->out);
  st->out = body;
  return st->errors ? 1 : 0;
}

static int yh_usage(int ret)
{
  fprintf(stderr, "usage: tcc -yasc-c-header IN.h [-o OUT.h]\n"
                  "  IN is under a directory named yasc; OUT gets the C declarations (written when they change)\n");
  return ret;
}

static char *yh_read_file(const char *path, int *len)
{
  FILE *f = fopen(path, "rb");
  char *buf;
  long n;
  if (!f)
    return NULL;
  fseek(f, 0, SEEK_END);
  n = ftell(f);
  fseek(f, 0, SEEK_SET);
  buf = tcc_malloc(n + 1);
  if (n > 0 && fread(buf, 1, n, f) != (size_t)n) {
    tcc_free(buf);
    fclose(f);
    return NULL;
  }
  buf[n] = 0;
  fclose(f);
  *len = (int)n;
  return buf;
}

ST_FUNC int tcc_tool_yasc_c_header(TCCState *s1, int argc, char **argv)
{
  const char *in = NULL, *outname = NULL, *include_as = NULL, *p;
  YhState st;
  char *old;
  int i, ret, old_len = 0;
  (void)s1;
  for (i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "-o") && i + 1 < argc)
      outname = argv[++i];
    else if (argv[i][0] == '-')
      return yh_usage(1);
    else if (!in)
      in = argv[i];
    else
      return yh_usage(1);
  }
  if (!in)
    return yh_usage(1);
  for (p = in; *p; p++)
    if ((p == in || p[-1] == '/') && !strncmp(p, "yasc/", 5))
      include_as = p;
  if (!include_as) {
    fprintf(stderr, "%s: error: not under a directory named yasc (the C header includes it as <yasc/...>)\n", in);
    return 1;
  }
  memset(&st, 0, sizeof st);
  st.file = in;
  st.src = yh_read_file(in, &st.len);
  if (!st.src) {
    fprintf(stderr, "%s: error: cannot read it\n", in);
    return 1;
  }
  YhToks_init(&st.toks);
  YhDroppers_init(&st.droppers);
  YhFrames_init(&st.frames);
  YhText_init(&st.out);
  ret = yh_run(&st, include_as);
  if (ret == 0) {
    old = outname ? yh_read_file(outname, &old_len) : NULL;
    if (!outname)
      fwrite(st.out.data, 1, st.out.size, stdout);
    else if (!old || (size_t)old_len != st.out.size || memcmp(old, st.out.data, old_len)) {
      FILE *f = fopen(outname, "wb");
      if (!f || fwrite(st.out.data, 1, st.out.size, f) != (size_t)st.out.size) {
        fprintf(stderr, "%s: error: cannot write it\n", outname);
        ret = 1;
      }
      if (f)
        fclose(f);
    }
    tcc_free(old);
  }
  YhText_cleanup(&st.out);
  YhToks_cleanup(&st.toks);
  YhDroppers_cleanup(&st.droppers);
  YhFrames_cleanup(&st.frames);
  tcc_free((char *)st.src);
  return ret;
}
