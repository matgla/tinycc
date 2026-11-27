/*
 *  diag_out.c - machine-readable diagnostics and fix-it hints
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 *  -fdiagnostics-format=json   gcc's JSON: one array of diagnostics, each
 *                              {kind, message, option, locations, children,
 *                              fixits}; notes are children of the one above.
 *  -fdiagnostics-format=sarif  SARIF 2.1.0: one run, a result per warning or
 *                              error (ruleId = "-Wname", or "error" /
 *                              "warning" when no option names it), notes as
 *                              relatedLocations, fix-its as fixes.
 *  -fdiagnostics-format=text   the default.  json-stderr / sarif-stderr are
 *                              gcc 13's names for the same.
 *  -fdiagnostics-parseable-fixits  text: a "fix-it:" line after each fix-it,
 *                              gcc's format, columns half-open.
 *
 *  The document is written to stderr as the diagnostics come (a result is
 *  held back only while notes can still join it) and closed when the process
 *  exits, so `tcc -c a.c b.c` gives one document, as does a compile that
 *  stops at an error (exit(1)).  Output given to tcc_set_error_func() stays
 *  text.
 *
 *  tcc keeps no columns: a fix-it names a word on the diagnostic's line, and
 *  the line is read back from the file when the fix-it is given (rare, off the
 *  compile path).  A word that is not found exactly once there (a macro, the
 *  same name twice) gives no fix-it, only the note.  tcc_fixit_call() reads
 *  a call's arguments the same way (sprintf -> snprintf(buf, sizeof buf,
 *  ...)): the call's name once, its arguments up to the one wanted on that
 *  line, literals and nested brackets skipped.
 *
 *  Not compiled on its own: libtcc.c #includes it before error1().
 */

enum
{
  DIAG_FMT_TEXT,
  DIAG_FMT_JSON,
  DIAG_FMT_SARIF
};

static struct
{
  unsigned char format;           /* DIAG_FMT_*; process-wide: one document per run */
  unsigned char parseable_fixits; /* -fdiagnostics-parseable-fixits */
  unsigned char started;          /* the document's head is out */
  unsigned char exit_hooked;      /* atexit(diag_out_close) */
  unsigned char pending;          /* res holds a result notes may still join */
  int nb_results;                 /* results written (a comma before the next) */
  int nb_children, nb_fixits;
  CString res, children, fixits;  /* the held result, in the chosen format */
  CString fix_desc;               /* SARIF: the last note before a fix-it, its description */
  char file[512];                 /* the place of the last diagnostic shown: */
  int line;                       /* where its fix-its look */
  /* tcc_fixit_next(): given with the error about to abort the compile */
  const char *next_word, *next_text, *next_note;
  int next_line, next_where;
  const char *alt_word[2], *alt_text[2]; /* tcc_fixit_next_alt(), tried in turn */
  int alt_where[2], alt_nb;
  /* tcc_fixit_call(): the call found, for tcc_fixit_call_emit() */
  int call_line, call_name, call_name_len, call_arg_end;
} dout;

/* ---- the unit's diagnostics in -O0's order -----------------------------
 *
 * Each diagnostic has an anchor (tcc_state->diag_anchor): the file-scope
 * declaration it belongs to in source order.  A body parsed away from its
 * place speaks with its definition's anchor -- a function deferred to the
 * end of the unit (-O1+), a static's saved initializer, a static inline
 * (at every -O), an inline expansion.  From the first body saved for later on
 * (tcc_diag_queue_begin), what would be written is held here, and written at
 * the end of the unit (tcc_diag_queue_flush) sorted by anchor, in the order
 * written within one: the order -O0 gives, at every -O and with
 * -fsyntax-only.  The text lines, the JSON / SARIF results and their fix-its
 * are held alike; a note stays with its diagnostic.  -fmax-errors=N counts
 * in that order: the Nth error is the last written, then "compilation
 * terminated". */
enum
{
  DQ_TEXT,   /* a line of text (diag_text_out) */
  DQ_RESULT, /* a JSON / SARIF result or note (diag_out_result) */
  DQ_FIXIT,  /* a JSON / SARIF fix-it of the result before it */
};

typedef struct DiagQueued
{
  int anchor, seq;
  unsigned char op;
  unsigned char head; /* a diagnostic starts here: 1 an error, 2 a warning */
  int kind, line, col, end;
  char *a, *b, *c;
} DiagQueued;

static struct
{
  DiagQueued *q;
  int n, cap, seq;
  unsigned char head_next; /* error1: what the next entry starts */
  unsigned char writing;   /* the flush: written, not held */
} dq;

static int dq_on(TCCState *s1)
{
  return s1->diag_queue_on && !dq.writing && !s1->fe_speculative; /* a speculation says nothing */
}

static char *dq_dup(const char *s)
{
  return s ? tcc_strdup(s) : NULL;
}

static DiagQueued *dq_add(TCCState *s1, int op)
{
  DiagQueued *e;
  if (dq.n == dq.cap)
  {
    dq.cap = dq.cap ? 2 * dq.cap : 32;
    dq.q = tcc_realloc(dq.q, dq.cap * sizeof *dq.q);
  }
  e = &dq.q[dq.n++];
  memset(e, 0, sizeof *e);
  e->anchor = s1->diag_anchor;
  e->seq = dq.seq++;
  e->op = op;
  e->head = dq.head_next;
  dq.head_next = 0;
  return e;
}

/* a body is saved to be parsed later: from here on the diagnostics wait */
ST_FUNC void tcc_diag_queue_begin(TCCState *s1)
{
  if (s1->diag_queue_on || !s1->error_set_jmp_enabled)
    return;
  s1->diag_queue_on = 1;
  s1->diag_queue_base_errors = s1->nb_errors;
}

static int dq_cmp(const void *pa, const void *pb)
{
  const DiagQueued *a = pa, *b = pb;
  if (a->anchor != b->anchor)
    return a->anchor < b->anchor ? -1 : 1;
  return a->seq < b->seq ? -1 : a->seq > b->seq;
}

static void diag_text_out(TCCState *s1, const char *line);
static void diag_out_result(int kind, const char *option, const char *file, int line, const char *msg);
static void diag_out_place(const char *file, int line);
static void fixit_emit(TCCState *s1, int line, int col, int end, const char *text);
static void diag_terminated(TCCState *s1);

/* The end of the unit (or of its compile, by an error): what was held, in
   anchor order, cut after the Nth error (-fmax-errors=N) and, after a fatal
   error that stopped the parse, after it (what -O0 never got to). */
ST_FUNC void tcc_diag_queue_flush(TCCState *s1)
{
  int errors = s1->diag_queue_base_errors, cut = 0;
  if (!s1->diag_queue_on)
    return;
  s1->diag_queue_on = 0;
  if (dq.n > 1)
    tcc_qsort(dq.q, dq.n, sizeof *dq.q, dq_cmp);
  dq.writing = 1;
  for (int i = 0; i < dq.n; i++)
  {
    DiagQueued *e = &dq.q[i];
    if (s1->diag_stop == 1 &&
        (e->anchor > s1->diag_stop_anchor || (e->anchor == s1->diag_stop_anchor && e->seq > s1->diag_stop_seq)))
      continue;
    if (e->head && s1->max_errors > 0 && errors >= s1->max_errors)
    {
      cut = 1;
      break;
    }
    if (e->head == 1)
      errors++;
    if (e->op == DQ_TEXT)
      diag_text_out(s1, e->a);
    else if (e->op == DQ_RESULT)
    {
      if (e->kind != 2)
        diag_out_place(e->b, e->line);
      diag_out_result(e->kind, e->a, e->b, e->line, e->c);
    }
    else
    {
      pstrcpy(dout.file, sizeof dout.file, e->b ? e->b : "");
      fixit_emit(s1, e->line, e->col, e->end, e->a);
    }
  }
  if (cut || (s1->max_errors > 0 && errors >= s1->max_errors && errors > s1->diag_queue_base_errors))
    diag_terminated(s1);
  dq.writing = 0;
  for (int i = 0; i < dq.n; i++)
  {
    tcc_free(dq.q[i].a);
    tcc_free(dq.q[i].b);
    tcc_free(dq.q[i].c);
  }
  dq.n = 0;
  dq.seq = 0;
  dq.head_next = 0;
}

/* SRC's bytes onto CS (cstr_cat takes a length of 0 as "with the NUL") */
static void diag_cat(CString *cs, const CString *src)
{
  if (src->size > 0)
    cstr_cat(cs, src->data, src->size);
}

static void diag_json_str(CString *cs, const char *s)
{
  cstr_ccat(cs, '"');
  for (; *s; s++)
  {
    unsigned char c = *s;
    if (c == '"' || c == '\\')
      cstr_ccat(cs, '\\'), cstr_ccat(cs, c);
    else if (c == '\n')
      cstr_cat(cs, "\\n", 2);
    else if (c == '\t')
      cstr_cat(cs, "\\t", 2);
    else if (c < 0x20 || c == 0x7f)
      cstr_printf(cs, "\\u%04x", c);
    else
      cstr_ccat(cs, c);
  }
  cstr_ccat(cs, '"');
}

/* a file name as a SARIF uri (a relative reference): unreserved characters
   and '/' as they are, the rest %XX */
static void diag_uri(CString *cs, const char *s)
{
  static const char hex[] = "0123456789ABCDEF";
  cstr_ccat(cs, '"');
  for (; *s; s++)
  {
    unsigned char c = *s;
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '.' ||
        c == '_' || c == '~' || c == '/')
      cstr_ccat(cs, c);
    else
      cstr_ccat(cs, '%'), cstr_ccat(cs, hex[c >> 4]), cstr_ccat(cs, hex[c & 15]);
  }
  cstr_ccat(cs, '"');
}

static void diag_sarif_loc(CString *cs, const char *file, int line, int col, int end)
{
  cstr_printf(cs, "\"physicalLocation\":{\"artifactLocation\":{\"uri\":");
  diag_uri(cs, file);
  cstr_ccat(cs, '}');
  if (line > 0)
  {
    cstr_printf(cs, ",\"region\":{\"startLine\":%d", line);
    if (col > 0)
      cstr_printf(cs, ",\"startColumn\":%d,\"endColumn\":%d", col, end);
    cstr_ccat(cs, '}');
  }
  cstr_ccat(cs, '}');
}

static void diag_json_loc(CString *cs, const char *key, const char *file, int line, int col)
{
  cstr_printf(cs, "\"%s\":{\"file\":", key);
  diag_json_str(cs, file);
  cstr_printf(cs, ",\"line\":%d", line);
  if (col > 0)
    cstr_printf(cs, ",\"column\":%d", col);
  cstr_ccat(cs, '}');
}

static void diag_out_head(void)
{
  if (dout.started)
    return;
  dout.started = 1;
  if (dout.format == DIAG_FMT_SARIF)
    fputs("{\"version\":\"2.1.0\",\"$schema\":\"https://json.schemastore.org/sarif-2.1.0.json\","
          "\"runs\":[{\"tool\":{\"driver\":{\"name\":\"tcc\",\"version\":\"" TCC_VERSION "\"}},"
          "\"columnKind\":\"unicodeCodePoints\",\"results\":[",
          stderr);
  else
    fputc('[', stderr);
}

/* the held result goes out: nothing can join it any more */
static void diag_out_flush(void)
{
  CString *cs = &dout.res;
  if (!dout.pending)
    return;
  dout.pending = 0;
  diag_out_head();
  if (dout.format == DIAG_FMT_SARIF)
  {
    if (dout.nb_children)
    {
      cstr_cat(cs, ",\"relatedLocations\":[", -1);
      diag_cat(cs, &dout.children);
      cstr_ccat(cs, ']');
    }
    if (dout.nb_fixits)
    {
      cstr_cat(cs, ",\"fixes\":[{\"description\":{\"text\":", -1);
      cstr_ccat(&dout.fix_desc, 0);
      diag_json_str(cs, dout.fix_desc.size > 1 ? (char *)dout.fix_desc.data : "fix-it");
      cstr_cat(cs, "},\"artifactChanges\":[{\"artifactLocation\":{\"uri\":", -1);
      diag_uri(cs, dout.file);
      cstr_cat(cs, "},\"replacements\":[", -1);
      diag_cat(cs, &dout.fixits);
      cstr_cat(cs, "]}]}]", -1);
    }
  }
  else
  {
    cstr_cat(cs, ",\"children\":[", -1);
    diag_cat(cs, &dout.children);
    cstr_cat(cs, "],\"fixits\":[", -1);
    diag_cat(cs, &dout.fixits);
    cstr_ccat(cs, ']');
  }
  cstr_ccat(cs, '}');
  if (dout.nb_results++)
    fputc(',', stderr);
  fputc('\n', stderr);
  fwrite(cs->data, 1, cs->size, stderr);
  fflush(stderr);
}

/* the end of the process: the last result, and the document closed */
static void diag_out_close(void)
{
  if (dout.format == DIAG_FMT_TEXT)
    return;
  diag_out_flush();
  diag_out_head();
  fputs(dout.format == DIAG_FMT_SARIF ? "\n]}]}\n" : "\n]\n", stderr);
  fflush(stderr);
}

/* tcc_delete: the unit's memory goes, and with it the held result */
static void diag_out_state_end(void)
{
  diag_out_flush();
  cstr_free(&dout.res);
  cstr_free(&dout.children);
  cstr_free(&dout.fixits);
  cstr_free(&dout.fix_desc);
  cstr_new(&dout.res);
  cstr_new(&dout.children);
  cstr_new(&dout.fixits);
  cstr_new(&dout.fix_desc);
}

/* -fdiagnostics-format=, -fdiagnostics-parseable-fixits.  0 when OPT was one. */
static int set_diag_format(const char *opt)
{
  if (!strcmp(opt, "diagnostics-parseable-fixits"))
  {
    dout.parseable_fixits = 1;
    return 0;
  }
  if (!strcmp(opt, "no-diagnostics-parseable-fixits"))
  {
    dout.parseable_fixits = 0;
    return 0;
  }
  if (strncmp(opt, "diagnostics-format=", 19))
    return -1;
  opt += 19;
  if (!strcmp(opt, "text"))
  {
    if (dout.format != DIAG_FMT_TEXT && dout.started)
      return 0; /* a document is open: it stays what it is */
    dout.format = DIAG_FMT_TEXT;
    return 0;
  }
  if (!strcmp(opt, "json") || !strcmp(opt, "json-stderr"))
    dout.format = dout.started ? dout.format : DIAG_FMT_JSON;
  else if (!strcmp(opt, "sarif") || !strcmp(opt, "sarif-stderr"))
    dout.format = dout.started ? dout.format : DIAG_FMT_SARIF;
  else
    return -1;
  if (!dout.exit_hooked)
  {
    dout.exit_hooked = 1;
    atexit(diag_out_close);
  }
  return 0;
}

static int diag_out_on(TCCState *s1)
{
  return dout.format != DIAG_FMT_TEXT && !s1->error_func;
}

/* A diagnostic shown: remember its place for the fix-its */
static void diag_out_place(const char *file, int line)
{
  pstrcpy(dout.file, sizeof dout.file, file ? file : "");
  dout.line = file ? line : 0;
}

/* KIND: 0 warning, 1 error, 2 note.  FILE NULL: no place (the linker). */
static void diag_out_result(int kind, const char *option, const char *file, int line, const char *msg)
{
  static const char *const kinds[] = {"warning", "error", "note"};
  CString *cs;
  if (dq_on(tcc_state))
  {
    DiagQueued *e = dq_add(tcc_state, DQ_RESULT);
    e->kind = kind;
    e->line = line;
    e->a = dq_dup(option);
    e->b = dq_dup(file);
    e->c = dq_dup(msg);
    return;
  }
  if (kind == 2)
  {
    if (!dout.pending)
      return;
    cs = &dout.children;
    if (dout.nb_children++)
      cstr_ccat(cs, ',');
    if (dout.format == DIAG_FMT_SARIF)
    {
      cstr_printf(cs, "{\"id\":%d,", dout.nb_children - 1);
      if (file)
      {
        diag_sarif_loc(cs, file, line, 0, 0);
        cstr_ccat(cs, ',');
      }
      cstr_cat(cs, "\"message\":{\"text\":", -1);
      diag_json_str(cs, msg);
      cstr_cat(cs, "}}", -1);
      if (!dout.nb_fixits) /* the note a fix-it came with; later ones say other things */
      {
        cstr_reset(&dout.fix_desc);
        cstr_cat(&dout.fix_desc, msg, -1);
      }
    }
    else
    {
      cstr_cat(cs, "{\"kind\":\"note\",\"message\":", -1);
      diag_json_str(cs, msg);
      cstr_cat(cs, ",\"locations\":[", -1);
      if (file)
      {
        cstr_ccat(cs, '{');
        diag_json_loc(cs, "caret", file, line, 0);
        cstr_ccat(cs, '}');
      }
      cstr_cat(cs, "]}", -1);
    }
    return;
  }
  diag_out_flush();
  dout.pending = 1;
  dout.nb_children = dout.nb_fixits = 0;
  cstr_reset(&dout.res);
  cstr_reset(&dout.children);
  cstr_reset(&dout.fixits);
  cstr_reset(&dout.fix_desc);
  cstr_cat(&dout.fix_desc, msg, -1); /* a note after it says it better */
  cs = &dout.res;
  if (dout.format == DIAG_FMT_SARIF)
  {
    cstr_cat(cs, "{\"ruleId\":", -1);
    if (option)
    {
      cstr_cat(cs, "\"-W", -1);
      cstr_cat(cs, option, -1);
      cstr_ccat(cs, '"');
    }
    else
      diag_json_str(cs, kinds[kind]);
    cstr_printf(cs, ",\"level\":\"%s\",\"message\":{\"text\":", kinds[kind]);
    diag_json_str(cs, msg);
    cstr_cat(cs, "},\"locations\":[", -1);
    if (file)
    {
      cstr_ccat(cs, '{');
      diag_sarif_loc(cs, file, line, 0, 0);
      cstr_ccat(cs, '}');
    }
    cstr_ccat(cs, ']');
  }
  else
  {
    cstr_printf(cs, "{\"kind\":\"%s\",\"message\":", kinds[kind]);
    diag_json_str(cs, msg);
    if (option)
    {
      cstr_cat(cs, ",\"option\":\"-W", -1);
      cstr_cat(cs, option, -1);
      cstr_ccat(cs, '"');
    }
    cstr_cat(cs, ",\"locations\":[", -1);
    if (file)
    {
      cstr_ccat(cs, '{');
      diag_json_loc(cs, "caret", file, line, 0);
      cstr_ccat(cs, '}');
    }
    cstr_ccat(cs, ']');
  }
}

/* LINE of FNAME into TEXT (NUL-terminated): 0 when the file is not there or
   is shorter */
static int fixit_line(const char *fname, int line, CString *text)
{
  FILE *fp;
  int c, n = 1;
  if (!fname[0] || line <= 0 || !(fp = fopen(fname, "rb")))
    return 0;
  while (n < line && (c = getc(fp)) != EOF)
    if (c == '\n')
      n++;
  while ((c = getc(fp)) != EOF && c != '\n')
    cstr_ccat(text, c);
  fclose(fp);
  cstr_ccat(text, 0);
  return n == line;
}

/* P at a literal or a comment of the line: past it (at its last character),
   NULL for a comment to the line's end or an unterminated one; P itself
   when it is neither */
static char *fixit_skip_lit(char *p)
{
  if (*p == '"' || *p == '\'')
  {
    char quote = *p;
    for (p++; *p && *p != quote; p++)
      if (*p == '\\' && p[1])
        p++;
    return *p ? p : NULL;
  }
  if (p[0] == '/' && p[1] == '/')
    return NULL;
  if (p[0] == '/' && p[1] == '*')
  {
    char *e = strstr(p + 2, "*/");
    return e ? e + 1 : NULL;
  }
  return p;
}

/* WORD in TEXT, exactly once outside comments and literals (as a whole
   identifier when it is one): its 1-based column, else 0 */
static int fixit_find_in(char *text, const char *word)
{
  /* a word that begins (ends) with an identifier character is not found
     inside a longer identifier: `*s` is not the start of `*sep` */
  int col = 0, found = 0, wlen = strlen(word), ident = isid(word[0]) || isnum(word[0]);
  int ident_end = wlen && (isid(word[wlen - 1]) || isnum(word[wlen - 1]));
  char *p, *q;
  if (!wlen)
    return 0;
  for (p = text; *p; p++)
  {
    if ((q = fixit_skip_lit(p)) != p)
    {
      if (!q)
        break;
      p = q;
      continue;
    }
    if (strncmp(p, word, wlen))
      continue;
    if (ident && p > text && (isid(p[-1]) || isnum(p[-1])))
      continue;
    if (ident_end && (isid(p[wlen]) || isnum(p[wlen])))
      continue;
    if (found++)
      return 0;
    col = p - text + 1;
  }
  return col;
}

/* WORD on LINE of FNAME, exactly once: its column, else 0.  FIRST: not
   after a ',' (spaces between) */
static int fixit_find(const char *fname, int line, const char *word, int first)
{
  CString text;
  int col = 0;
  cstr_new(&text);
  if (fixit_line(fname, line, &text))
    col = fixit_find_in(text.data, word);
  if (col && first)
  {
    int i = col - 2;
    while (i >= 0 && (text.data[i] == ' ' || text.data[i] == '\t'))
      i--;
    if (i >= 0 && text.data[i] == ',')
      col = 0;
  }
  cstr_free(&text);
  return col;
}

static void diag_text_out(TCCState *s1, const char *line);

/* the fix-it: columns COL..END (half-open) of LINE replaced by TEXT */
static void fixit_emit(TCCState *s1, int line, int col, int end, const char *text)
{
  if (diag_out_on(s1) && dq_on(s1))
  {
    DiagQueued *e = dq_add(s1, DQ_FIXIT);
    e->line = line;
    e->col = col;
    e->end = end;
    e->a = dq_dup(text);
    e->b = dq_dup(dout.file);
    return;
  }
  if (diag_out_on(s1))
  {
    CString *cs = &dout.fixits;
    if (!dout.pending)
      return;
    if (dout.nb_fixits++)
      cstr_ccat(cs, ',');
    if (dout.format == DIAG_FMT_SARIF)
    {
      cstr_printf(cs, "{\"deletedRegion\":{\"startLine\":%d,\"startColumn\":%d,\"endColumn\":%d},", line, col, end);
      cstr_cat(cs, "\"insertedContent\":{\"text\":", -1);
      diag_json_str(cs, text);
      cstr_cat(cs, "}}", -1);
    }
    else
    {
      cstr_ccat(cs, '{');
      diag_json_loc(cs, "start", dout.file, line, col);
      cstr_ccat(cs, ',');
      diag_json_loc(cs, "next", dout.file, line, end);
      cstr_cat(cs, ",\"string\":", -1);
      diag_json_str(cs, text);
      cstr_ccat(cs, '}');
    }
  }
  else if (dout.parseable_fixits)
  {
    CString cs;
    cstr_new(&cs);
    cstr_cat(&cs, "fix-it:", -1);
    diag_json_str(&cs, dout.file);
    cstr_printf(&cs, ":{%d:%d-%d:%d}:", line, col, line, end);
    diag_json_str(&cs, text);
    cstr_ccat(&cs, 0);
    diag_text_out(s1, cs.data);
    cstr_free(&cs);
  }
}

/* tcc_fixit(): see tcc.h */
PUB_FUNC int tcc_fixit(int line, const char *word, int where, const char *text)
{
  TCCState *s1 = tcc_state;
  int col, end;
  if (!s1->diag_last_shown || !dout.file[0])
    return 0;
  if (!line)
    line = dout.line;
  if (!(col = fixit_find(dout.file, line, word, where == FIXIT_REPLACE_FIRST)))
    return 0;
  end = col;
  if (where == FIXIT_REPLACE || where == FIXIT_REPLACE_FIRST)
    end = col + strlen(word);
  else if (where == FIXIT_AFTER)
    col = end = col + strlen(word);
  fixit_emit(s1, line, col, end, text);
  return 1;
}

/* yasc: WORD at TEXT[S..S+LEN) leaves its pointer's words.  In a group,
   `*@(const, view)` (or `*?@(...)`), the group is written again without it:
   one word left stands against the '*' (`*const`), more stay in @( ).  Sets
   the half-open 1-based columns *COL..*END and the text REPL (REPLSZ bytes)
   that replaces them; 0 when WORD is not in a group. */
static int fixit_group_drop(const char *text, int s, int len, int *col, int *end, char *repl, int replsz)
{
  int open = -1, close = -1, depth = 0, i, n = 0, kept = 0;
  const int tlen = strlen(text);
  char items[8][64];
  for (i = s - 1; i >= 0; i--)
  {
    if (text[i] == ')')
      depth++;
    else if (text[i] == '(')
    {
      if (depth-- == 0)
      {
        open = i;
        break;
      }
    }
  }
  if (open < 2 || text[open - 1] != '@' || !(text[open - 2] == '*' || text[open - 2] == '?'))
    return 0;
  for (i = open + 1, depth = 0; i < tlen; i++)
  {
    if (text[i] == '(')
      depth++;
    else if (text[i] == ')' && depth-- == 0)
    {
      close = i;
      break;
    }
  }
  if (close < 0 || s + len > close)
    return 0;
  /* the items, at the group's own level */
  for (i = open + 1; i <= close;)
  {
    int b = i, e, d = 0;
    while (i < close && (text[i] != ',' || d))
    {
      d += text[i] == '(' ? 1 : text[i] == ')' ? -1 : 0;
      i++;
    }
    e = i++;
    while (b < e && (text[b] == ' ' || text[b] == '\t'))
      b++;
    while (e > b && (text[e - 1] == ' ' || text[e - 1] == '\t'))
      e--;
    if (b == s && e == s + len)
      continue; /* the word that goes */
    if (n == 8 || e - b >= 64)
      return 0;
    memcpy(items[n], text + b, e - b);
    items[n++][e - b] = 0;
  }
  repl[0] = 0;
  if (n > 1)
    pstrcat(repl, replsz, "@(");
  for (i = 0; i < n; i++)
  {
    if (kept++)
      pstrcat(repl, replsz, ", ");
    pstrcat(repl, replsz, items[i]);
  }
  if (n > 1)
    pstrcat(repl, replsz, ")");
  *col = open; /* the '@', 1-based */
  *end = close + 2;
  return 1;
}

/* tcc_fixit_col(): see tcc.h */
PUB_FUNC int tcc_fixit_col(int line, int col, const char *word, const char *text)
{
  TCCState *s1 = tcc_state;
  CString src;
  int end = 0, len = strlen(word);
  if (!s1->diag_last_shown || !dout.file[0] || col <= 0)
    return 0;
  if (!line)
    line = dout.line;
  cstr_new(&src);
  if (fixit_line(dout.file, line, &src) && col - 1 + len <= src.size - 1 && !memcmp(src.data + col - 1, word, len))
  {
    char repl[256];
    if (!*text && fixit_group_drop(src.data, col - 1, len, &col, &end, repl, sizeof repl))
    {
      cstr_free(&src);
      fixit_emit(s1, line, col, end, repl);
      return 1;
    }
    end = col + len;
    while (src.data[end - 1] == ' ' || src.data[end - 1] == '\t')
      end++;
  }
  cstr_free(&src);
  if (!end)
    return 0;
  fixit_emit(s1, line, col, end, text);
  return 1;
}

/* tcc_fixit_drop(): see tcc.h */
PUB_FUNC int tcc_fixit_drop(int line, const char *word)
{
  TCCState *s1 = tcc_state;
  CString src;
  int col = 0;
  if (!s1->diag_last_shown || !dout.file[0])
    return 0;
  if (!line)
    line = dout.line;
  cstr_new(&src);
  if (fixit_line(dout.file, line, &src))
    col = fixit_find_in(src.data, word);
  cstr_free(&src);
  return col ? tcc_fixit_col(line, col, word, "") : 0;
}

/* tcc_fixit_call(): see tcc.h */
PUB_FUNC int tcc_fixit_call(int line, const char *name, int arg, char *buf, int bufsz)
{
  CString text;
  char *p, *q, *end = NULL;
  int depth, k, ok = 0;
  dout.call_line = 0;
  if (!dout.file[0] || bufsz <= 0)
    return 0;
  if (!line)
    line = dout.line;
  cstr_new(&text);
  if (!fixit_line(dout.file, line, &text) || !(dout.call_name = fixit_find_in(text.data, name)))
    goto out;
  p = (char *)text.data + dout.call_name - 1 + strlen(name);
  while (*p == ' ' || *p == '\t')
    p++;
  if (*p++ != '(')
    goto out;
  for (k = 0; k <= arg; k++)
  {
    char *a;
    while (*p == ' ' || *p == '\t')
      p++;
    a = p, end = NULL, depth = 0;
    for (;; p++)
    {
      if (!*p)
        goto out; /* the argument goes on past the line */
      if ((q = fixit_skip_lit(p)) != p)
      {
        if (!q)
          goto out;
        if (*p != '/')
          end = q + 1; /* a literal is part of the argument, a comment not */
        p = q;
        continue;
      }
      if (depth == 0 && (*p == ',' || *p == ')'))
        break;
      if (*p == '(' || *p == '[' || *p == '{')
        depth++;
      else if (*p == ')' || *p == ']' || *p == '}')
        depth--;
      if (*p != ' ' && *p != '\t')
        end = p + 1;
    }
    if (!end || (*p == ')' && k < arg))
      goto out; /* an empty argument, or too few */
    if (!k)
    {
      if (end - a >= bufsz)
        goto out;
      memcpy(buf, a, end - a);
      buf[end - a] = 0;
    }
    p++;
  }
  dout.call_line = line;
  dout.call_name_len = strlen(name);
  dout.call_arg_end = end - (char *)text.data + 1;
  ok = 1;
out:
  cstr_free(&text);
  return ok;
}

/* tcc_fixit_call_emit(): see tcc.h */
PUB_FUNC void tcc_fixit_call_emit(const char *rename, const char *append)
{
  TCCState *s1 = tcc_state;
  const int line = dout.call_line;
  if (!line || !s1->diag_last_shown)
    return;
  dout.call_line = 0;
  if (rename)
    fixit_emit(s1, line, dout.call_name, dout.call_name + dout.call_name_len, rename);
  if (append)
    fixit_emit(s1, line, dout.call_arg_end, dout.call_arg_end, append);
}

/* tcc_fixit_next(): see tcc.h */
PUB_FUNC void tcc_fixit_next(int line, const char *word, int where, const char *text, const char *note)
{
  dout.next_line = line;
  dout.next_word = word;
  dout.next_where = where;
  dout.next_text = text;
  dout.next_note = note;
  dout.alt_nb = 0;
}

/* tcc_fixit_next_alt(): see tcc.h */
PUB_FUNC void tcc_fixit_next_alt(const char *word, int where, const char *text)
{
  if (!word || dout.alt_nb == 2)
    return;
  dout.alt_word[dout.alt_nb] = word;
  dout.alt_where[dout.alt_nb] = where;
  dout.alt_text[dout.alt_nb++] = text;
}

/* error1, after a diagnostic: the note and fix-it tcc_fixit_next() left */
static void diag_out_next(void)
{
  const char *note = dout.next_note;
  if (!note)
    return;
  dout.next_note = NULL;
  _tcc_note("%i:%s", dout.next_line ? dout.next_line : dout.line, note);
  if (dout.next_word && !tcc_fixit(dout.next_line, dout.next_word, dout.next_where, dout.next_text))
    for (int i = 0; i < dout.alt_nb; i++)
      if (tcc_fixit(dout.next_line, dout.alt_word[i], dout.alt_where[i], dout.alt_text[i]))
        break;
  dout.alt_nb = 0;
}
