/*
 *  gen_predef_table.c -- stage B of the predefine pipeline.
 *
 *  Stage A is c2str (conftest.c -DC2STR): include/tccdefs.h -> tccdefs_.h,
 *  which turns indented lines into C strings but deliberately KEEPS the
 *  column-1 conditionals as host directives, so one generated header serves
 *  every target and the selection happens when tccpp.c is compiled.
 *
 *  That is why the table cannot be produced by a pre-compilation generator:
 *  before the host preprocessor runs, "which strings survive" is unknown.
 *  So this program is compiled WITH the target's -D flags, includes
 *  tccdefs_.h to obtain the already-target-resolved text, and flattens it.
 *
 *  What is left to resolve, and how:
 *    - `#ifndef __INTn_MAX__` (8 of them): sequence-dependent, and this
 *      program walks the list in order, so it knows whether the guard name
 *      was defined earlier. Resolved here; the guard disappears.
 *    - `#if __STDC_VERSION__ >= 201112L`: depends on -std=, a *runtime*
 *      option, so it cannot be resolved at build time. Entries inside it are
 *      tagged TCC_PREDEF_C11 and the consumer checks s1->cversion.
 *    - `#ifdef __leading_underscore`: depends on -fleading-underscore, also
 *      runtime. Tagged TCC_PREDEF_LEADING_US / TCC_PREDEF_NO_LEADING_US.
 *
 *  Output is tccdefs_table_.h: entries in source order (definition order is
 *  preserved so a body that names another predefine still resolves the same
 *  way), plus the name hash index the lazy path looks identifiers up in.
 *  All of it is pointer-free (strings are offsets into one blob), so on
 *  YasOS the tables stay in flash instead of being relocated into RAM.
 *
 *  The text remains authoritative: TCC_NO_PREDEF_TABLE=1 selects it and is
 *  the byte-identity A/B seam, exactly as TCC_NO_PROGRAMMATIC_DECLS is for
 *  the builtin prototypes.
 */

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* tcc.h is included for PTR_SIZE/LONG_SIZE, which the column-1 conditionals
   in tccdefs_.h test. It also redirects malloc/free/strdup to tcc's
   allocator-tracking wrappers, which this standalone generator does not link
   against -- so it uses static buffers throughout and takes them back. */
#include "tcc.h"

#undef malloc
#undef free
#undef realloc
#undef strdup
#undef qsort

static const char predef_text[] =
#include "tccdefs_.h"
    ;

#define MAX_ENTRIES 512
#define MAX_BODY 1024

typedef struct
{
  char name[128];
  char body[MAX_BODY];
  const char *flag; /* C expression for the flags field */
  int tok_off;      /* index into tcc_predef_toks[] */
  int tok_count;
  int name_off;     /* offset of name in tcc_predef_strs[] */
} Entry;

static Entry entries[MAX_ENTRIES];
static int nb_entries;

#define MAX_RAW 16
static char raw[MAX_RAW][MAX_BODY];
static int nb_raw;

/* ---- body tokenisation ------------------------------------------------
 * The consumer materialises a predefine WITHOUT running the lexer: there is
 * no safe point to re-enter it (tok_alloc_new runs mid-lex, define_find runs
 * mid-expansion, and tcc has no "string -> TokenString" helper that leaves
 * `file` alone).  So the bodies are tokenised here and replayed as data.
 *
 * The inventory is small and checked: over the 97 armv8m/YasOS predefines the
 * bodies hold 64 identifiers, 81 numbers and 48 punctuation characters, no
 * string or character constants, and the longest is 16 tokens.  Numbers keep
 * their spelling and are handed to tcc's own parse_number() at materialise
 * time -- it reads only from the string it is given, so float suffixes, hex
 * and exponents stay tcc's business rather than being re-implemented here.
 */
#define PDT_FIXED 0 /* tok holds the token id (char value or TOK_*) */
#define PDT_IDENT 1 /* text is interned with tok_alloc */
#define PDT_NUM 2   /* text is handed to parse_number */

#define MAX_TOKS 4096
typedef struct
{
  int kind;
  int tok;
  const char *tokname; /* symbolic TOK_* spelling, for readable output */
  char text[64];
  int text_off; /* offset of text in tcc_predef_strs[] */
} TokItem;

static TokItem toks[MAX_TOKS];
static int nb_toks;

/* tcc_predef_strs[]: NUL-separated, each distinct string once. */
static char blob[16384];
static int blob_len;

static int blob_add(const char *str)
{
  int i, n = (int)strlen(str) + 1;
  for (i = 0; i < blob_len; i += (int)strlen(blob + i) + 1)
    if (!strcmp(blob + i, str))
      return i;
  if (blob_len + n > (int)sizeof(blob) || blob_len + n > 65535)
  {
    fprintf(stderr, "gen_predef_table: string blob full\n");
    exit(1);
  }
  memcpy(blob + blob_len, str, n);
  i = blob_len;
  blob_len += n;
  return i;
}

/* Mirrors tccpp.c's tok_two_chars[]. Kept as a table rather than special-casing
 * "->" (the only two-char operator the current predefines use) so a future
 * macro with "<<" or "==" is tokenised rather than silently mis-split. */
static const struct
{
  char a, b;
  int tok;
  const char *name;
} two_chars[] = {
    {'<', '=', TOK_LE, "TOK_LE"},         {'>', '=', TOK_GE, "TOK_GE"},
    {'!', '=', TOK_NE, "TOK_NE"},         {'&', '&', TOK_LAND, "TOK_LAND"},
    {'|', '|', TOK_LOR, "TOK_LOR"},       {'+', '+', TOK_INC, "TOK_INC"},
    {'-', '-', TOK_DEC, "TOK_DEC"},       {'=', '=', TOK_EQ, "TOK_EQ"},
    {'<', '<', TOK_SHL, "TOK_SHL"},       {'>', '>', TOK_SAR, "TOK_SAR"},
    {'+', '=', TOK_A_ADD, "TOK_A_ADD"},   {'-', '=', TOK_A_SUB, "TOK_A_SUB"},
    {'*', '=', TOK_A_MUL, "TOK_A_MUL"},   {'/', '=', TOK_A_DIV, "TOK_A_DIV"},
    {'%', '=', TOK_A_MOD, "TOK_A_MOD"},   {'&', '=', TOK_A_AND, "TOK_A_AND"},
    {'^', '=', TOK_A_XOR, "TOK_A_XOR"},   {'|', '=', TOK_A_OR, "TOK_A_OR"},
    {'-', '>', TOK_ARROW, "TOK_ARROW"},   {'.', '.', TOK_TWODOTS, "TOK_TWODOTS"},
    {'#', '#', TOK_TWOSHARPS, "TOK_TWOSHARPS"},
    {0, 0, 0, NULL}};

static int isidchar(int c)
{
  return isalnum((unsigned char)c) || c == '_';
}

/* Tokenise one macro body. Returns 0 on success, -1 on a construct this
   generator does not model (which fails the build rather than emitting a
   table that silently differs from what the preprocessor would have built). */
static int tokenize_body(const char *body, int *out_off, int *out_count)
{
  const char *p = body;
  *out_off = nb_toks;
  *out_count = 0;
  while (*p)
  {
    TokItem *it;
    if (isspace((unsigned char)*p))
    {
      p++;
      continue;
    }
    if (nb_toks >= MAX_TOKS)
    {
      fprintf(stderr, "gen_predef_table: token table full\n");
      return -1;
    }
    it = &toks[nb_toks];
    memset(it, 0, sizeof(*it));

    if (isalpha((unsigned char)*p) || *p == '_')
    {
      int n = 0;
      while (isidchar(*p) && n < (int)sizeof(it->text) - 1)
        it->text[n++] = *p++;
      it->text[n] = 0;
      it->kind = PDT_IDENT;
    }
    else if (isdigit((unsigned char)*p) ||
             (*p == '.' && isdigit((unsigned char)p[1])))
    {
      /* A pp-number: digits, letters, '.', and the exponent signs. Handing
         the whole spelling to parse_number keeps suffix/exponent handling in
         one place -- tcc's. */
      int n = 0;
      while ((isidchar(*p) || *p == '.' ||
              ((*p == '+' || *p == '-') && n > 0 &&
               (it->text[n - 1] == 'e' || it->text[n - 1] == 'E' ||
                it->text[n - 1] == 'p' || it->text[n - 1] == 'P'))) &&
             n < (int)sizeof(it->text) - 1)
        it->text[n++] = *p++;
      it->text[n] = 0;
      it->kind = PDT_NUM;
    }
    else if (*p == '"' || *p == '\'')
    {
      fprintf(stderr, "gen_predef_table: string/char constant in a predefine "
                      "body is not modelled: %s\n",
              body);
      return -1;
    }
    else
    {
      int i, matched = 0;
      for (i = 0; two_chars[i].a; i++)
        if (p[0] == two_chars[i].a && p[1] == two_chars[i].b)
        {
          it->kind = PDT_FIXED;
          it->tok = two_chars[i].tok;
          it->tokname = two_chars[i].name;
          p += 2;
          matched = 1;
          break;
        }
      if (!matched)
      {
        it->kind = PDT_FIXED;
        it->tok = (unsigned char)*p; /* single-char tokens are their own id */
        p++;
      }
    }
    nb_toks++;
    (*out_count)++;
  }
  return 0;
}

/* Rebuild a body's text from its tokens and compare with the original,
 * ignoring whitespace.  Tokenisation that silently drops or splits something
 * (a pp-number's exponent, an operator this generator does not know) would
 * otherwise produce a table that differs from what the preprocessor builds,
 * and nothing downstream would notice.  Fails the build instead. */
static int roundtrip_ok(const Entry *e)
{
  char out[MAX_BODY];
  int n = 0, i;
  for (i = 0; i < e->tok_count; i++)
  {
    const TokItem *it = &toks[e->tok_off + i];
    const char *sp = NULL;
    char buf[3];
    if (it->kind != PDT_FIXED)
      sp = it->text;
    else
    {
      int j;
      for (j = 0; two_chars[j].a; j++)
        if (two_chars[j].tok == it->tok)
        {
          buf[0] = two_chars[j].a;
          buf[1] = two_chars[j].b;
          buf[2] = 0;
          sp = buf;
          break;
        }
      if (!sp)
      {
        buf[0] = (char)it->tok;
        buf[1] = 0;
        sp = buf;
      }
    }
    while (*sp && n < (int)sizeof(out) - 1)
      out[n++] = *sp++;
  }
  out[n] = 0;
  {
    const char *a = e->body, *b = out;
    for (;;)
    {
      while (*a == ' ' || *a == '\t')
        a++;
      while (*b == ' ' || *b == '\t')
        b++;
      if (*a != *b)
        return 0;
      if (!*a)
        return 1;
      a++, b++;
    }
  }
}

static int defined_earlier(const char *name)
{
  int i;
  for (i = 0; i < nb_entries; i++)
    if (!strcmp(entries[i].name, name))
      return 1;
  return 0;
}

static char *skip_spc(char *p)
{
  while (*p == ' ' || *p == '\t')
    p++;
  return p;
}

/* Emit a C string literal for `s`. */
static void put_cstr(FILE *f, const char *s)
{
  fputc('"', f);
  for (; *s; s++)
  {
    if (*s == '"' || *s == '\\')
      fprintf(f, "\\%c", *s);
    else if (*s == '\n')
      fprintf(f, "\\n");
    else
      fputc(*s, f);
  }
  fputc('"', f);
}

int main(int argc, char **argv)
{
  const char *out = argc > 1 ? argv[1] : "tccdefs_table_.h";
  if (argc > 1 && !strcmp(argv[1], "--dump"))
  {
    fputs(predef_text, stdout);
    return 0;
  }
  static char text[sizeof(predef_text)];
  char *line, *save = NULL;
  const char *flag = "0";
  /* Guard state. Only the three shapes documented above occur; anything else
     is a hard error rather than a silent mis-generation. */
  int skipping = 0, depth = 0;
  int i;
  FILE *f;

  memcpy(text, predef_text, sizeof(predef_text));

  for (line = strtok_r(text, "\n", &save); line; line = strtok_r(NULL, "\n", &save))
  {
    char *p = skip_spc(line);

    if (*p == '#')
    {
      char *d = skip_spc(p + 1);
      if (!strncmp(d, "ifndef", 6))
      {
        char name[128], *q = skip_spc(d + 6), *n = name;
        while (*q && (isalnum((unsigned char)*q) || *q == '_'))
          *n++ = *q++;
        *n = 0;
        skipping = defined_earlier(name);
        depth++;
        continue;
      }
      if (!strncmp(d, "ifdef", 5))
      {
        if (!strstr(d, "__leading_underscore"))
        {
          fprintf(stderr, "gen_predef_table: unhandled #ifdef: %s\n", d);
          return 1;
        }
        flag = "TCC_PREDEF_LEADING_US";
        depth++;
        continue;
      }
      if (!strncmp(d, "if", 2))
      {
        if (!strstr(d, "__STDC_VERSION__"))
        {
          fprintf(stderr, "gen_predef_table: unhandled #if: %s\n", d);
          return 1;
        }
        flag = "TCC_PREDEF_C11";
        depth++;
        continue;
      }
      if (!strncmp(d, "else", 4))
      {
        if (!strcmp(flag, "TCC_PREDEF_LEADING_US"))
          flag = "TCC_PREDEF_NO_LEADING_US";
        else
          skipping = !skipping;
        continue;
      }
      if (!strncmp(d, "endif", 5))
      {
        skipping = 0;
        flag = "0";
        depth--;
        continue;
      }
      if (!strncmp(d, "define", 6))
      {
        char *q, *nm;
        if (skipping)
          continue;
        if (nb_entries >= MAX_ENTRIES)
        {
          fprintf(stderr, "gen_predef_table: too many entries\n");
          return 1;
        }
        q = skip_spc(d + 6);
        nm = entries[nb_entries].name;
        /* Function-like macros keep their parameter list as part of the
           name field, exactly as the text form spells it, so the consumer
           hands define_push the same spelling the preprocessor would. */
        while (*q && (isalnum((unsigned char)*q) || *q == '_'))
          *nm++ = *q++;
        *nm = 0;
        if (*q == '(')
        {
          char *b = entries[nb_entries].body;
          int n = 0;
          while (*q && *q != ')' && n < MAX_BODY - 2)
            b[n++] = *q++;
          if (*q == ')')
            b[n++] = *q++;
          b[n] = 0;
          /* params were captured into body[]; move them onto the name */
          strcat(entries[nb_entries].name, entries[nb_entries].body);
          entries[nb_entries].body[0] = 0;
        }
        q = skip_spc(q);
        strncpy(entries[nb_entries].body, q, MAX_BODY - 1);
        entries[nb_entries].body[MAX_BODY - 1] = 0;
        entries[nb_entries].flag = flag;
        nb_entries++;
        continue;
      }
      fprintf(stderr, "gen_predef_table: unhandled directive: %s\n", d);
      return 1;
    }

    if (!*p || skipping)
      continue;

    /* A non-directive line is either the continuation of a backslash-spliced
       macro body, or raw C the parser still has to see. Keying on the
       trailing backslash is what separates them: getting this wrong silently
       glued `typedef char*__builtin_va_list;` onto the body of the macro
       above it, which is exactly the kind of mis-generation the table must
       not be able to produce. */
    if (nb_entries)
    {
      Entry *e = &entries[nb_entries - 1];
      size_t have = strlen(e->body);
      if (have && e->body[have - 1] == '\\')
      {
        e->body[have - 1] = 0;
        snprintf(e->body + have - 1, MAX_BODY - have, " %s", p);
        continue;
      }
    }

    /* Raw declaration (for the armv8m/YasOS target: the one
       `typedef char *__builtin_va_list;`). Kept verbatim and separate, so the
       consumer knows precisely what still needs the declaration parser and
       what can go straight to define_push. */
    if (nb_raw < MAX_RAW)
      snprintf(raw[nb_raw++], MAX_BODY, "%s", p);
    else
    {
      fprintf(stderr, "gen_predef_table: too many raw lines\n");
      return 1;
    }
  }

  if (depth != 0)
  {
    fprintf(stderr, "gen_predef_table: unbalanced conditionals (%d)\n", depth);
    return 1;
  }

  /* Tokenise every body now that the entry list is final. */
  for (i = 0; i < nb_entries; i++)
    if (tokenize_body(entries[i].body, &entries[i].tok_off, &entries[i].tok_count) < 0)
    {
      fprintf(stderr, "gen_predef_table: while tokenising %s\n", entries[i].name);
      return 1;
    }
  for (i = 0; i < nb_entries; i++)
    if (!roundtrip_ok(&entries[i]))
    {
      fprintf(stderr, "gen_predef_table: tokens do not round-trip for %s (body \"%s\")\n",
              entries[i].name, entries[i].body);
      return 1;
    }


  f = fopen(out, "w");
  if (!f)
  {
    perror(out);
    return 1;
  }
  fprintf(f, "/* generated by gen_predef_table.c -- do not edit.\n"
             "   Source of truth: include/tccdefs.h (via tccdefs_.h).\n"
             "   %d predefined macros for this target. */\n\n",
          nb_entries);
  /* Every string the consumer reads (macro names incl. parameter lists, token
     spellings) goes into one blob and the tables hold 16-bit offsets into it:
     pointer-free, so they need no load-time relocation and stay in flash on
     YasOS instead of costing every compiler process ~3 KB of .data. */
  for (i = 0; i < nb_entries; i++)
    entries[i].name_off = blob_add(entries[i].name);
  for (i = 0; i < nb_toks; i++)
    if (toks[i].kind != PDT_FIXED)
      toks[i].text_off = blob_add(toks[i].text);
  fprintf(f, "static const char tcc_predef_strs[] =");
  for (i = 0; i < blob_len;)
  {
    fprintf(f, "\n    ");
    put_cstr(f, blob + i);
    fprintf(f, " \"\\0\"");
    i += (int)strlen(blob + i) + 1;
  }
  fprintf(f, ";\n\n");
  fprintf(f, "/* Pre-tokenised macro bodies: the consumer replays these with\n"
             "   tok_alloc/parse_number/tok_str_add, so materialising a predefine\n"
             "   never re-enters the lexer. */\n");
  fprintf(f, "static const TCCPredefTok tcc_predef_toks[] = {\n");
  for (i = 0; i < nb_toks; i++)
  {
    const TokItem *it = &toks[i];
    if (it->kind == PDT_FIXED)
    {
      if (it->tokname)
        fprintf(f, "    { PDT_FIXED, %s, 0 },\n", it->tokname);
      else if (it->tok >= 32 && it->tok < 127 && it->tok != '\'' && it->tok != '\\')
        fprintf(f, "    { PDT_FIXED, '%c', 0 },\n", it->tok);
      else
        fprintf(f, "    { PDT_FIXED, %d, 0 },\n", it->tok);
    }
    else
      fprintf(f, "    { %s, 0, %d }, /* %s */\n", it->kind == PDT_IDENT ? "PDT_IDENT" : "PDT_NUM", it->text_off,
              it->text);
  }
  fprintf(f, "};\n\n");
  fprintf(f, "static const TCCPredefMacro tcc_predef_macros[] = {\n");
  for (i = 0; i < nb_entries; i++)
  {
    int len = 0;
    while (entries[i].name[len] && entries[i].name[len] != '(')
      len++;
    if (len > 255)
    {
      fprintf(stderr, "gen_predef_table: macro name too long: %s\n", entries[i].name);
      return 1;
    }
    fprintf(f, "    { %d, %d, %s, %d, %d }, /* %s */\n", entries[i].name_off, len, entries[i].flag,
            entries[i].tok_off, entries[i].tok_count, entries[i].name);
  }
  fprintf(f, "};\n\n");
  /* Name index for the lazy path (an identifier interned for the first time
     looks itself up here): bare-name hash, chains newest first, index+1 with
     0 = end.  Built here instead of into .bss at startup. */
  {
    static unsigned short head[PREDEF_HASH_SIZE], next[MAX_ENTRIES];
    if (nb_entries >= PREDEF_HASH_SIZE)
    {
      fprintf(stderr, "gen_predef_table: %d entries, PREDEF_HASH_SIZE is %d\n", nb_entries, PREDEF_HASH_SIZE);
      return 1;
    }
    for (i = 0; i < nb_entries; i++)
    {
      unsigned h = TOK_HASH_INIT;
      int k;
      for (k = 0; entries[i].name[k] && entries[i].name[k] != '('; k++)
        h = TOK_HASH_FUNC(h, ((const unsigned char *)entries[i].name)[k]);
      h &= PREDEF_HASH_SIZE - 1;
      next[i] = head[h];
      head[h] = (unsigned short)(i + 1);
    }
    fprintf(f, "static const unsigned short predef_hash_head[PREDEF_HASH_SIZE] = {");
    for (i = 0; i < PREDEF_HASH_SIZE; i++)
      fprintf(f, "%s%d,", i % 16 ? "" : "\n   ", head[i]);
    fprintf(f, "\n};\n");
    fprintf(f, "static const unsigned short predef_hash_next[] = {");
    for (i = 0; i < nb_entries; i++)
      fprintf(f, "%s%d,", i % 16 ? "" : "\n   ", next[i]);
    fprintf(f, "\n};\n\n");
  }
  fprintf(f, "/* Raw declarations that are not #defines and still need the\n"
             "   parser -- one typedef on this target.  NUL-separated, an empty\n"
             "   string ends the list (no pointer table to relocate). */\n");
  fprintf(f, "static const char tcc_predef_raw[] =");
  for (i = 0; i < nb_raw; i++)
  {
    fprintf(f, "\n    ");
    put_cstr(f, raw[i]);
    fprintf(f, " \"\\0\"");
  }
  fprintf(f, " \"\";\n");
  fclose(f);
  fprintf(stderr, "gen_predef_table: wrote %d entries (%d body tokens) + %d raw lines to %s (target text %zu B)\n",
          nb_entries, nb_toks, nb_raw, out, sizeof(predef_text) - 1);
  return 0;
}
