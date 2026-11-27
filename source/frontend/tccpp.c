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

#define USING_GLOBALS
#include "tcc.h"

#include <sys/stat.h>
#ifndef _WIN32
#include <dirent.h>
#include <errno.h>
#endif

#ifdef TCC_TARGET_ARM_ARCHV8M
#include "arm-thumb-defs.h"
#endif

/* #define to 1 to enable (see parse_pp_string()) */
#define ACCEPT_LF_IN_STRINGS 0

/********************************************************/
/* global variables */

ST_DATA int tok_flags;
ST_DATA int parse_flags;

ST_DATA struct BufferedFile *file;
ST_DATA int tok;
ST_DATA CValue tokc;
/* -Wtautological-compare: a running hash of the tokens next() hands the
   parser, and its value before the current token -- two operands that read
   the same tokens are the same expression (fe_tokfp_*) */
ST_DATA unsigned char fe_tokfp_on;
ST_DATA uint64_t fe_tokfp_h, fe_tokfp_prev;
ST_DATA unsigned fe_tokfp_n;

static void tokfp_add(void)
{
  uint64_t v = (unsigned)tok + 1;
  if (TOK_HAS_VALUE(tok))
  {
    if (tok <= TOK_CULONG)
      v ^= tokc.i * 0x9E3779B97F4A7C15ull;
    else if (tok == TOK_CFLOAT)
    {
      uint32_t b;
      memcpy(&b, &tokc.f, 4);
      v ^= (uint64_t)b * 0x9E3779B97F4A7C15ull;
    }
    else if (tok == TOK_CDOUBLE || tok == TOK_CLDOUBLE)
    {
      uint64_t b;
      memcpy(&b, &tokc.d, 8);
      v ^= b * 0x9E3779B97F4A7C15ull;
    }
    else if (tok == TOK_STR || tok == TOK_LSTR)
      for (int i = 0; i < tokc.str.size; i++)
        v = (v ^ (unsigned char)tokc.str.data[i]) * 0x100000001B3ull;
  }
  fe_tokfp_prev = fe_tokfp_h;
  fe_tokfp_h = fe_tokfp_h * 0x100000001B3ull + v;
  fe_tokfp_n++;
}
ST_DATA const int *macro_ptr;
ST_DATA CString tokcstr; /* current parsed string, if any */
ST_DATA TokenString *pp_pragma_capture; /* see tcc.h */
ST_DATA int pp_pack_captures;           /* see tcc.h */

/* display benchmark infos */
ST_DATA int tok_ident;
ST_DATA TokenSym **table_ident;
ST_DATA int pp_expr;

/* ------------------------------------------------------------------------- */

static TokenSym *hash_ident[TOK_HASH_SIZE];
typedef struct TokenLookupCacheEntry
{
  TokenSym *ts;
  unsigned int hash;
  int len;
} TokenLookupCacheEntry;

#define TOK_LOOKUP_CACHE_SIZE 8
/* Initial table_ident capacity.  Must exceed NB_BUILTIN_TOKS (the reserved
   builtin id range, ~540) with headroom for a typical compile's user idents;
   the table grows on demand past this.  Was 8192 (a ~32 KB device prealloc,
   mostly wasted on tiny compiles); right-sized now that builtins are lazy. */
#define TOK_IDENT_PREALLOC 1024
static TokenLookupCacheEntry token_lookup_cache[TOK_LOOKUP_CACHE_SIZE];
static int table_ident_alloc;
static char token_buf[STRING_MAX_SIZE + 1];
static CString cstr_buf;
static TokenString tokstr_buf;
static TokenString unget_buf;
static unsigned char isidnum_table[256 - CH_EOF];
static int pp_debug_tok, pp_debug_symv;
static int pp_counter;
static void tok_print(const int *str, const char *msg, ...);
static void next_nomacro(void);
static void next_nomacro_raw(void);
static void parse_number(const char *p);
static void tcc_predef_try_materialize(TokenSym *ts, const char *str, int len);
static void parse_string(const char *p, int len);

static TokenString *macro_stack;
/* error recovery (recover.c): begin_macro numbers the replays; and > 0 while
   a macro is being substituted -- an error there leaves stack-held strings on
   the macro stack, so it cannot be recovered from */
ST_DATA unsigned tok_macro_serial;
ST_DATA int pp_subst_depth;

ST_FUNC TokenString *tok_macro_stack(void)
{
  return macro_stack;
}

/* An error the lexer cannot read past (an unterminated comment or string):
   the compile ends there, at every -O -- a body being saved as tokens says
   it where its parse reaches it (decl.c save_body_cut). */
static void lex_error_stuck(const char *fmt, int c)
{
  tcc_state->error_fatal = 1;
  tcc_error(fmt, c);
}

/* Back to a macro stack read before an error left the lexer in the middle
   of something (decl.c save_function_body); what it held above is dropped. */
ST_FUNC void tok_macro_restore(TokenString *stack, const int *ptr)
{
  macro_stack = stack;
  macro_ptr = ptr;
}

/* ---- tokens from a system header's macro ------------------------------
 *
 * gcc keeps quiet about what a macro defined in a system header expands to
 * (an assert, a FD_SET, a NULL), unless -Wsystem-headers.  A macro defined
 * in one is MACRO_SYS; next() marks its live expansion (TokenString.sys) --
 * the parse is in it while that string is on the macro stack.
 *
 * A body saved as tokens (-O1+ saves nearly every one, -O0 a nested
 * function's, an initializer) holds the expansion's tokens but not where
 * they came from, and must not: a mark in the tokens would lengthen the body,
 * which the inliner sizes by tokens, and change the objects.  So the saved
 * string's ranges of such tokens are kept beside it, here, by the string's
 * address (moved by tok_str_keep, dropped by tok_str_free, copied with it),
 * and only looked at when a warning is about to be given: a replay of the
 * string (tok_str_borrow: its src) is in one of them when the token it read
 * last is.  Bounds are word offsets, [start, end) in pairs; an odd count
 * leaves the last range open. */

typedef struct SysRanges
{
  TokenString *s;
  int n, cap;
  int *b;
} SysRanges;

static SysRanges **sys_tab; /* open addressing by the string's address */
static unsigned sys_tab_mask;
ST_DATA int tok_sys_ranges; /* entries in sys_tab */

static unsigned sys_hash(const TokenString *s)
{
  return (unsigned)(((uintptr_t)s >> 3) * 2654435761u);
}

static SysRanges **sys_slot(const TokenString *s)
{
  unsigned i = sys_hash(s) & sys_tab_mask;
  while (sys_tab[i] && sys_tab[i]->s != s)
    i = (i + 1) & sys_tab_mask;
  return &sys_tab[i];
}

static SysRanges *sys_ranges_find(const TokenString *s)
{
  return tok_sys_ranges ? *sys_slot(s) : NULL;
}

static void sys_tab_put(SysRanges *r)
{
  *sys_slot(r->s) = r;
}

static SysRanges *sys_ranges_get(TokenString *s)
{
  SysRanges *r = sys_ranges_find(s);
  if (r)
    return r;
  if (2 * (tok_sys_ranges + 1) > (int)sys_tab_mask)
  {
    SysRanges **old = sys_tab;
    unsigned n = old ? sys_tab_mask + 1 : 0, m = n ? 2 * n : 64;
    sys_tab = tcc_mallocz(m * sizeof *sys_tab);
    sys_tab_mask = m - 1;
    for (unsigned i = 0; i < n; i++)
      if (old[i])
        sys_tab_put(old[i]);
    tcc_free(old);
  }
  r = tcc_mallocz(sizeof *r);
  r->s = s;
  sys_tab_put(r);
  tok_sys_ranges++;
  return r;
}

/* S's entry goes (the slots after it in its run move up) */
static void sys_ranges_remove(SysRanges **slot)
{
  unsigned i = slot - sys_tab, j = i;
  tcc_free((*slot)->b);
  tcc_free(*slot);
  sys_tab[i] = NULL;
  tok_sys_ranges--;
  for (;;)
  {
    j = (j + 1) & sys_tab_mask;
    if (!sys_tab[j])
      break;
    SysRanges *r = sys_tab[j];
    sys_tab[j] = NULL;
    sys_tab_put(r);
  }
}

static void sys_ranges_drop(TokenString *s)
{
  SysRanges **slot = sys_slot(s);
  if (*slot)
    sys_ranges_remove(slot);
}

static void sys_ranges_rekey(TokenString *from, TokenString *to)
{
  SysRanges **slot = sys_slot(from), *r = *slot;
  if (!r || from == to)
    return;
  *slot = NULL; /* re-placed below, with its run */
  tok_sys_ranges--;
  {
    unsigned j = slot - sys_tab;
    for (;;)
    {
      j = (j + 1) & sys_tab_mask;
      if (!sys_tab[j])
        break;
      SysRanges *q = sys_tab[j];
      sys_tab[j] = NULL;
      sys_tab_put(q);
    }
  }
  r->s = to;
  sys_tab_put(r);
  tok_sys_ranges++;
}

static void sys_ranges_add(SysRanges *r, int at)
{
  if (r->n == r->cap)
  {
    r->cap = r->cap ? 2 * r->cap : 4;
    r->b = tcc_realloc(r->b, r->cap * sizeof *r->b);
  }
  r->b[r->n++] = at;
}

/* how many of the N ascending bounds B are below POS (a binary search: a
   body whose system macros take user arguments has thousands) */
static int bounds_below(const int *b, int n, int pos)
{
  int lo = 0, hi = n;
  while (lo < hi)
  {
    const int mid = (lo + hi) >> 1;
    if (b[mid] < pos)
      lo = mid + 1;
    else
      hi = mid;
  }
  return lo;
}

/* the last token read before offset POS of R's string was in a range */
static int sys_ranges_at(const SysRanges *r, int pos)
{
  return bounds_below(r->b, r->n, pos) & 1;
}

/* ---- a live expansion's tokens, by where they were spelled ------------
 *
 * gcc's rule: a token is quiet when it is spelled in a system header -- a
 * system macro's own tokens, also inside a user macro that expands it; not
 * a user's argument written inside a system macro's call.  While a macro
 * call is expanded (macro_subst_tok and what it calls), every string built
 * carries which of its tokens a system macro spelled (TokenString.xs, the
 * toggles of PPSys: [b0, b1) [b2, b3) ... are such tokens, word offsets;
 * NULL: none), and an argument read through a view (macro_view) takes them
 * from the string viewed (xv).  The finished expansion (tokstr_buf) keeps
 * them for tcc_sys_macro_here.  Nothing is done while no system macro takes
 * part: a string without xs has no such token. */
typedef struct PPSys
{
  int n, cap;
  int *b;
} PPSys;

static const int xs_all_b[1] = {0};
static const PPSys xs_all = {1, 1, (int *)xs_all_b}; /* every token */

/* was the token starting at word OFF spelled in a system macro? */
static int xs_state(const PPSys *x, int off)
{
  return x ? bounds_below(x->b, x->n, off + 1) & 1 : 0;
}

/* PPSys kept for reuse: an expansion builds and drops several */
static PPSys *xs_pool[8];
static int xs_pool_nb;

static PPSys *xs_new(void)
{
  if (xs_pool_nb)
  {
    PPSys *x = xs_pool[--xs_pool_nb];
    x->n = 0;
    return x;
  }
  return tcc_mallocz(sizeof(PPSys));
}

static void xs_free(PPSys *x)
{
  if (!x || x == &xs_all)
    return;
  if (xs_pool_nb < (int)(sizeof xs_pool / sizeof xs_pool[0]))
  {
    xs_pool[xs_pool_nb++] = x;
    return;
  }
  tcc_free(x->b);
  tcc_free(x);
}

static void xs_mark_slow(TokenString *s, int sys)
{
  PPSys *x = s->xs;
  if (!x)
    x = s->xs = xs_new();
  if (x->n && x->b[x->n - 1] == s->len)
  {
    x->n--; /* an empty range */
    return;
  }
  if (x->n == x->cap)
  {
    x->cap = x->cap ? 2 * x->cap : 4;
    x->b = tcc_realloc(x->b, x->cap * sizeof *x->b);
  }
  x->b[x->n++] = s->len;
}

/* the next token added to S is a system macro's (SYS) or not */
static inline void xs_mark(TokenString *s, int sys)
{
  const PPSys *x = s->xs;
  if ((x ? x->n & 1 : 0) != sys)
    xs_mark_slow(s, sys);
}

/* S got, from word AT on, LEN words of a string whose tokens SRC tells */
static void xs_copy(TokenString *s, int at, const PPSys *src, int len)
{
  const int end = s->len;
  s->len = at;
  xs_mark(s, xs_state(src, 0));
  for (int i = 0; src && i < src->n; i++)
    if (src->b[i] > 0 && src->b[i] < len)
    {
      s->len = at + src->b[i];
      xs_mark(s, !(s->xs && (s->xs->n & 1)));
    }
  s->len = end;
}

/* S was cut back to its length: no toggle past it */
static void xs_trunc(TokenString *s)
{
  while (s->xs && s->xs->n && s->xs->b[s->xs->n - 1] > s->len)
    s->xs->n--;
}

/* The parser tokens of BUF before word POS: where the last one starts
   (*cur) and the one before it (*prev); -1 when there is none */
static void pp_tok_starts(const int *buf, int pos, int *cur, int *prev)
{
  const int *p = buf;
  *cur = *prev = -1;
  while (p < buf + pos)
  {
    const int at = p - buf;
    int t;
    CValue cv;
    tok_get(&t, &p, &cv);
    if (t == 0 || t == ' ' || t == TOK_LINENUM || t == TOK_PACK_REPLAY || t == TOK_EOF || t == TOK_LINEFEED)
      continue;
    *prev = *cur;
    *cur = at;
  }
}

/* A system macro's expansion ended on its own token and the parse went on
   in the file (next()): until the file is read further, the token before
   the one read now was that one */
static unsigned char pp_sys_end_pending;
static BufferedFile *pp_sys_end_file;
static const unsigned char *pp_sys_end_ptr;
static int pp_sys_end_line, pp_sys_end_tok;

static void pp_sys_end_set(void)
{
  pp_sys_end_pending = 0;
  pp_sys_end_file = file;
  pp_sys_end_ptr = file ? file->buf_ptr : NULL;
  pp_sys_end_line = file ? file->line_num : 0;
  pp_sys_end_tok = tok;
}

static int pp_sys_end_here(void)
{
  return pp_sys_end_ptr && file == pp_sys_end_file && file->buf_ptr == pp_sys_end_ptr &&
         file->line_num == pp_sys_end_line;
}

/* The token read last was spelled in a system header's macro -- in a live
   expansion (by its xs), or a replayed saved string (by its ranges, which
   tok_str_sys_save wrote from the same answer).  BOTH: and so was the token
   before it (an operand's last token is what a warning given at the
   lookahead is about: `SYS_ID(v << 40)` speaks, `SYS_SHIFT(v)` does not;
   and the lookahead past the expansion -- `char c = SYS_BIG;` -- speaks as
   before, at every -O alike) */
static int pp_sys_tok(int both)
{
  const int *pos = macro_ptr;
  for (TokenString *m = macro_stack; m; pos = m->prev_ptr, m = m->prev)
  {
    int cur, before;
    if (m->alloc >= 2)
    {
      /* a replay decides by its string's ranges (the token read last: the
         toggles before its end; the one before it needs its start) */
      const SysRanges *r = m->src ? sys_ranges_find(m->src) : NULL;
      if (!r || !pos || !sys_ranges_at(r, (int)(pos - tok_str_buf(m))))
        return 0;
      if (!both)
        return 1;
      pp_tok_starts(tok_str_buf(m), (int)(pos - tok_str_buf(m)), &cur, &before);
      return before >= 0 && sys_ranges_at(r, before + 1);
    }
    if (m->sys)
    {
      if (!pos || pos == tok_str_buf(m) || !xs_state(m->xs, (int)(pos - tok_str_buf(m)) - 1))
        return 0;
      if (!both)
        return 1;
      pp_tok_starts(tok_str_buf(m), (int)(pos - tok_str_buf(m)), &cur, &before);
      if (cur < 0)
        return 0;
      if (before >= 0)
        return xs_state(m->xs, before);
      /* the token before the expansion: the last of another one's, ended
         right before this one began (next()) */
      return pp_sys_end_tok == m->save_tok && pp_sys_end_here();
    }
  }
  return 0;
}

ST_FUNC int tcc_sys_macro_here(void)
{
  return pp_sys_tok(0);
}

/* a diagnostic about the position read now: the lookahead and the token
   before it */
ST_FUNC int tcc_sys_macro_prev(void)
{
  return pp_sys_tok(1);
}

/* A saved string TO is about to get the token just read: SYS says whether
   the one before it was from a system header's macro; the new state is
   returned.  TO's ranges follow. */
ST_FUNC int tok_str_sys_save(TokenString *to, int sys)
{
  const int now = macro_stack ? tcc_sys_macro_here() : 0;
  if (now != sys)
  {
    SysRanges *r = now ? sys_ranges_get(to) : sys_ranges_find(to);
    if (r)
    {
      if (r->n && r->b[r->n - 1] == to->len)
        r->n--; /* an empty range, or two that touch: one */
      else
        sys_ranges_add(r, to->len);
    }
  }
  return now;
}

/* TO is a copy of FROM's tokens (FROM's offsets are TO's) */
ST_FUNC void tok_str_sys_copy(TokenString *from, TokenString *to)
{
  const SysRanges *r = sys_ranges_find(from);
  if (!r || !r->n)
    return;
  SysRanges *t = sys_ranges_get(to);
  t->n = 0;
  for (int i = 0; i < r->n; i++)
    sys_ranges_add(t, r->b[i]);
}

static void token_lookup_cache_clear(void)
{
  memset(token_lookup_cache, 0, sizeof(token_lookup_cache));
}

static TokenSym *token_lookup_cache_find(unsigned int hash, const char *str, int len)
{
  TokenLookupCacheEntry *entry = &token_lookup_cache[hash & (TOK_LOOKUP_CACHE_SIZE - 1)];
  TokenSym *ts = entry->ts;

  if (!ts || entry->hash != hash || entry->len != len)
    return NULL;
  if (ts->len != len || memcmp(tok_name(ts), str, len))
    return NULL;
  return ts;
}

static void token_lookup_cache_store(unsigned int hash, int len, TokenSym *ts)
{
  TokenLookupCacheEntry *entry = &token_lookup_cache[hash & (TOK_LOOKUP_CACHE_SIZE - 1)];

  entry->ts = ts;
  entry->hash = hash;
  entry->len = len;
}

static const char tcc_keywords[] =
#define DEF(id, str) str "\0"
#include "tcctok.h"
#undef DEF
    ;

/* WARNING: the content of this string encodes token numbers */
static const unsigned char tok_two_chars[] =
    /* outdated -- gr
        "<=\236>=\235!=\225&&\240||\241++\244--\242==\224<<\1>>\2+=\253"
        "-=\255*=\252/=\257%=\245&=\246^=\336|=\374->\313..\250##\266";
    */
    {'<', '=', TOK_LE,        '>', '=', TOK_GE,    '!', '=', TOK_NE,    '&', '&', TOK_LAND,  '|', '|', TOK_LOR,
     '+', '+', TOK_INC,       '-', '-', TOK_DEC,   '=', '=', TOK_EQ,    '<', '<', TOK_SHL,   '>', '>', TOK_SAR,
     '+', '=', TOK_A_ADD,     '-', '=', TOK_A_SUB, '*', '=', TOK_A_MUL, '/', '=', TOK_A_DIV, '%', '=', TOK_A_MOD,
     '&', '=', TOK_A_AND,     '^', '=', TOK_A_XOR, '|', '=', TOK_A_OR,  '-', '>', TOK_ARROW, '.', '.', TOK_TWODOTS,
     '#', '#', TOK_TWOSHARPS, 0};

ST_FUNC void skip(int c)
{
  if (tok != c)
  {
    char tmp[40];
    pstrcpy(tmp, sizeof tmp, get_tok_str(c, &tokc));
    tcc_error("'%s' expected (got \"%s\")", tmp, get_tok_str(tok, &tokc));
  }
  next();
}

ST_FUNC void expect(const char *msg)
{
  tcc_error("%s expected", msg);
}

/* ------------------------------------------------------------------------- */
/* Custom allocator for tiny objects */

/* Off: its only user was the TokenString headers, and a bump arena reuses a
 * chunk only once every header in it is freed.  Long-lived headers (saved
 * bodies, initializers) kept chunks full of dead ones, so it grew by doubling
 * chunks and gave nothing back -- 4.2 MB on Zig's 12.7 MB kernel.c, where the
 * headers live at its peak fit in a few KB.  malloc per header measured the
 * same compile time.  Define USE_TAL to bring it back. */
/* #define USE_TAL */

#ifdef CONFIG_TCC_LOW_MEM
#define TOKSYM_CHUNK_SIZE (4 * 1024)
#else
/* Equal steps: one malloc per 8 KB costs nothing measurable, and the old
 * 48 KB first arena was a third empty after a hello world. */
#define TOKSYM_CHUNK_SIZE (8 * 1024)
#endif
#define TOKSYM_BIG 256 /* a longer TokenSym gets a block of its own */

#ifndef USE_TAL
#define tal_free(al, p) tcc_free(p)
#define tal_realloc(al, p, size) tcc_realloc(p, size)
#define tal_new(a, b, c)
#define tal_delete(a)
#else
#if !defined(MEM_DEBUG)
#define tal_free(al, p) tal_free_impl(al, p)
#define tal_realloc(al, p, size) tal_realloc_impl(&al, p, size)
#define TAL_DEBUG_PARAMS
#else
#define TAL_DEBUG MEM_DEBUG
// #define TAL_INFO 1 /* collect and dump allocators stats */
#define tal_free(al, p) tal_free_impl(al, p, __FILE__, __LINE__)
#define tal_realloc(al, p, size) tal_realloc_impl(&al, p, size, __FILE__, __LINE__)
#define TAL_DEBUG_PARAMS , const char *file, int line
#define TAL_DEBUG_FILE_LEN 40
#endif

#ifdef CONFIG_TCC_LOW_MEM
/* Small first arenas, and every further one the same size (TAL_NEXT_SIZE):
 * doubling would overshoot by up to half of what is in use. */
#define TOKSTR_TAL_SIZE (2 * 1024)
#define TAL_NEXT_SIZE(size) (size)
#else
#define TOKSTR_TAL_SIZE (8 * 1024)  /* allocator for TokenString structs only (not buffers) */
#define TAL_NEXT_SIZE(size) ((size) * 2)
#endif
#define TOKSTR_TAL_LIMIT 128        /* structs are ~48-64 bytes */

typedef struct TinyAlloc
{
  unsigned limit;
  unsigned size;
  uint8_t *buffer;
  uint8_t *p;
  unsigned nb_allocs;
  struct TinyAlloc *next, *top;
#ifdef TAL_INFO
  unsigned nb_peak;
  unsigned nb_total;
  unsigned nb_missed;
  uint8_t *peak_p;
#endif
} TinyAlloc;

typedef struct tal_header_t
{
  unsigned size;
#ifdef TAL_DEBUG
  int line_num; /* negative line_num used for double free check */
  char file_name[TAL_DEBUG_FILE_LEN + 1];
#endif
} __attribute__((aligned(sizeof(void *)))) tal_header_t;

/* ------------------------------------------------------------------------- */

static TinyAlloc *tal_new(TinyAlloc **pal, unsigned limit, unsigned size)
{
  TinyAlloc *al = tcc_mallocz(sizeof(TinyAlloc));
  al->p = al->buffer = tcc_malloc(size);
  al->limit = limit;
  al->size = size;
  if (pal)
    *pal = al;
  return al;
}

static void tal_delete(TinyAlloc *al)
{
  TinyAlloc *next;

tail_call:
  if (!al)
    return;
#ifdef TAL_INFO
  fprintf(stderr,
          "limit %4d  size %7d  nb_peak %5d  nb_total %7d  nb_missed %5d  "
          "usage %5.1f%%\n",
          al->limit, al->size, al->nb_peak, al->nb_total, al->nb_missed, (al->peak_p - al->buffer) * 100.0 / al->size);
#endif
#if TAL_DEBUG && TAL_DEBUG != 3 /* do not check TAL leaks with -DMEM_DEBUG=3                                           \
                                 */
  if (al->nb_allocs > 0)
  {
    uint8_t *p;
    fprintf(stderr, "TAL_DEBUG: memory leak %d chunk(s) (limit= %d)\n", al->nb_allocs, al->limit);
    p = al->buffer;
    while (p < al->p)
    {
      tal_header_t *header = (tal_header_t *)p;
      if (header->line_num > 0)
      {
        fprintf(stderr, "%s:%d: chunk of %d bytes leaked\n", header->file_name, header->line_num, header->size);
      }
      p += header->size + sizeof(tal_header_t);
    }
#if TAL_DEBUG == 2
    exit(2);
#endif
  }
#endif
  next = al->next;
  tcc_free(al->buffer);
  tcc_free(al);
  al = next;
  goto tail_call;
}

static void tal_free_impl(TinyAlloc *al, void *p TAL_DEBUG_PARAMS)
{
  if (!p)
    return;
tail_call:
  if (al->buffer <= (uint8_t *)p && (uint8_t *)p < al->buffer + al->size)
  {
#ifdef TAL_DEBUG
    tal_header_t *header = (((tal_header_t *)p) - 1);
    if (header->line_num < 0)
    {
      fprintf(stderr, "%s:%d: TAL_DEBUG: double frees chunk from\n", file, line);
      fprintf(stderr, "%s:%d: %d bytes\n", header->file_name, (int)-header->line_num, (int)header->size);
    }
    else
      header->line_num = -header->line_num;
#endif
    al->nb_allocs--;
    if (!al->nb_allocs)
      al->p = al->buffer;
  }
  else if (al->next)
  {
    al = al->next;
    goto tail_call;
  }
  else
    tcc_free(p);
}

static void *tal_realloc_impl(TinyAlloc **pal, void *p, unsigned size TAL_DEBUG_PARAMS)
{
  tal_header_t *header;
  void *ret;
  int is_own;
  unsigned adj_size = (size + sizeof(void *) - 1) & ~(sizeof(void *) - 1);
  TinyAlloc *al = *pal;

tail_call:
  is_own = (al->buffer <= (uint8_t *)p && (uint8_t *)p < al->buffer + al->size);
  if ((!p || is_own) && size <= al->limit)
  {
    /* Align allocation pointer to ensure proper alignment */
    unsigned char *aligned_p = (unsigned char *)(((size_t)al->p + sizeof(void *) - 1) & ~(sizeof(void *) - 1));
    if (aligned_p - al->buffer + adj_size + sizeof(tal_header_t) < al->size)
    {
      al->p = aligned_p;
      header = (tal_header_t *)al->p;
      header->size = adj_size;
#ifdef TAL_DEBUG
      {
        int ofs = strlen(file) - TAL_DEBUG_FILE_LEN;
        strncpy(header->file_name, file + (ofs > 0 ? ofs : 0), TAL_DEBUG_FILE_LEN);
        header->file_name[TAL_DEBUG_FILE_LEN] = 0;
        header->line_num = line;
      }
#endif
      ret = al->p + sizeof(tal_header_t);
      al->p += adj_size + sizeof(tal_header_t);
      if (is_own)
      {
        header = (((tal_header_t *)p) - 1);
        if (p)
          memcpy(ret, p, header->size);
#ifdef TAL_DEBUG
        header->line_num = -header->line_num;
#endif
      }
      else
      {
        al->nb_allocs++;
      }
#ifdef TAL_INFO
      if (al->nb_peak < al->nb_allocs)
        al->nb_peak = al->nb_allocs;
      if (al->peak_p < al->p)
        al->peak_p = al->p;
      al->nb_total++;
#endif
      return ret;
    }
    else if (is_own)
    {
      al->nb_allocs--;
      ret = tal_realloc(*pal, 0, size);
      header = (((tal_header_t *)p) - 1);
      if (p)
        memcpy(ret, p, header->size);
#ifdef TAL_DEBUG
      header->line_num = -header->line_num;
#endif
      return ret;
    }
    if (al->next)
    {
      al = al->next;
    }
    else
    {
      TinyAlloc *bottom = al, *next = al->top ? al->top : al;

      al = tal_new(pal, next->limit, TAL_NEXT_SIZE(next->size));
      al->next = next;
      bottom->top = al;
    }
    goto tail_call;
  }
  if (is_own)
  {
    al->nb_allocs--;
    ret = tcc_malloc(size);
    header = (((tal_header_t *)p) - 1);
    if (p)
      memcpy(ret, p, header->size);
#ifdef TAL_DEBUG
    header->line_num = -header->line_num;
#endif
  }
  else if (al->next)
  {
    al = al->next;
    goto tail_call;
  }
  else
    ret = tcc_realloc(p, size);
#ifdef TAL_INFO
  al->nb_missed++;
#endif
  return ret;
}

static struct TinyAlloc *tokstr_alloc;
#endif /* USE_TAL */

/* ------------------------------------------------------------------------- */
/* TokenSym storage.  A TokenSym lives until tccpp_delete and is never freed or
 * resized on its own, so bump arenas do: no per-entry allocator header (TAL
 * kept one, 4 bytes on ARM).  The headers take one arena and the names
 * another, so that the names can leave memory once the TU is parsed
 * (tok_names_spill): the code generated after that reads few of them.  A
 * name takes len + 1 bytes rounded to 2 -- a spilled one's offset is told
 * from a pointer by its low bit.  Chunks are chained through their first
 * word and freed together. */
typedef struct TokSymChunk
{
  struct TokSymChunk *prev;
  unsigned size; /* bytes after the header */
} TokSymChunk;

typedef struct TokSymArena
{
  TokSymChunk *chunks;
  uint8_t *p, *end;
} TokSymArena;

static TokSymArena toksym_heads, toksym_names;

/* Names tok_name_tmp read back, by token: one slot per hash, so a function's
 * callees and its symbols are not read again at every reference. */
#define NAME_TMP_SLOTS 64
static struct
{
  int tok;
  int size;
  char *name;
} name_tmp[NAME_TMP_SLOTS];


static void *toksym_bump(TokSymArena *a, unsigned size)
{
  TokSymChunk *c;
  uint8_t *ret;

  if (size > (unsigned)(a->end - a->p))
  {
    if (size > TOKSYM_BIG)
    {
      /* Link it behind the current chunk, which keeps its free tail. */
      c = tcc_malloc(sizeof *c + size);
      c->size = size;
      if (a->chunks)
      {
        c->prev = a->chunks->prev;
        a->chunks->prev = c;
      }
      else
      {
        c->prev = NULL;
        a->chunks = c;
      }
      return c + 1;
    }
    /* the free tail of the chunk it retires: the names' go to the spill file
       whole (tok_names_spill), so not left uninitialised */
    if (a->p)
      memset(a->p, 0, a->end - a->p);
    c = tcc_malloc(sizeof *c + TOKSYM_CHUNK_SIZE);
    c->size = TOKSYM_CHUNK_SIZE;
    c->prev = a->chunks;
    a->chunks = c;
    a->p = (uint8_t *)(c + 1);
    a->end = a->p + TOKSYM_CHUNK_SIZE;
  }
  ret = a->p;
  a->p += size;
  return ret;
}

static void toksym_arena_free(TokSymArena *a)
{
  TokSymChunk *c, *prev;
  for (c = a->chunks; c; c = prev)
  {
    prev = c->prev;
    tcc_free(c);
  }
  a->chunks = NULL;
  a->p = a->end = NULL;
}

/* A copy of the LEN bytes at STR, NUL-terminated, in the name arena. */
static char *toksym_name_copy(const char *str, int len)
{
  char *name = toksym_bump(&toksym_names, (len + 2) & ~1u);
  memcpy(name, str, len);
  name[len] = '\0';
  if (!(len & 1))
    name[len + 1] = '\0'; /* the padding to 2: spilled with the chunk */
  return name;
}

static TokenSym *toksym_new(const char *str, int len)
{
  TokenSym *ts = toksym_bump(&toksym_heads, (sizeof(TokenSym) + sizeof(void *) - 1) & ~(sizeof(void *) - 1));
  ts->len = len;
  ts->str = toksym_name_copy(str, len);
  return ts;
}

static void toksym_free_all(void)
{
  toksym_arena_free(&toksym_heads);
  toksym_arena_free(&toksym_names);
  for (int k = 0; k < NAME_TMP_SLOTS; k++)
  {
    tcc_free(name_tmp[k].name);
    name_tmp[k].name = NULL;
    name_tmp[k].size = 0;
  }
}

static int toksym_chunk_cmp(const void *a, const void *b)
{
  uintptr_t x = (uintptr_t)*(TokSymChunk *const *)a, y = (uintptr_t)*(TokSymChunk *const *)b;
  return x < y ? -1 : x > y;
}

/* The TU is parsed: write every name to the spill file and free the name
 * chunks.  What reads a name from now on (an ELF symbol, a callee screened by
 * its name, a diagnostic) reads it back, into the arena again, for good. */
ST_FUNC void tok_names_spill(void)
{
  TokSymChunk *c, **v;
  long *off;
  int n = 0, i;

  if (!tcc_spill_active() || !toksym_names.chunks)
    return;
  for (c = toksym_names.chunks; c; c = c->prev)
    n++;
  v = tcc_malloc(n * (sizeof *v + sizeof *off));
  off = (long *)(v + n);
  for (n = 0, c = toksym_names.chunks; c; c = c->prev)
    v[n++] = c;
  tcc_qsort(v, n, sizeof *v, toksym_chunk_cmp);
  for (i = 0; i < n; i++)
  {
    unsigned used = v[i] == toksym_names.chunks ? (unsigned)(toksym_names.p - (uint8_t *)(v[i] + 1)) : v[i]->size;
    off[i] = tcc_spill_append(v[i] + 1, used);
    if (off[i] < 0 || off[i] + (long)used >= 0x7fffffffL)
    {
      tcc_free(v); /* nothing changed yet: the names stay */
      return;
    }
  }
  for (i = 0; i < tok_ident - TOK_IDENT; i++)
  {
    TokenSym *ts = table_ident[i];
    int lo = 0, hi = n - 1;
    if (!ts || ((uintptr_t)ts->str & 1))
      continue;
    while (lo < hi) /* the last chunk at or below the name */
    {
      int mid = (lo + hi + 1) / 2;
      if ((uintptr_t)v[mid] <= (uintptr_t)ts->str)
        lo = mid;
      else
        hi = mid - 1;
    }
    ts->str = (const char *)(((uintptr_t)(off[lo] + (ts->str - (const char *)(v[lo] + 1))) << 1) | 1);
  }
  tcc_free(v);
  toksym_arena_free(&toksym_names);
}

/* Token V's name for a caller that is done with it before the next call: a
 * spilled name is read into a slot it shares with other tokens, and stays in
 * the spill file. */
ST_FUNC const char *tok_name_tmp(int v)
{
  TokenSym *ts;
  if (v < TOK_IDENT || v >= tok_ident || !(ts = table_ident[v - TOK_IDENT]) || !((uintptr_t)ts->str & 1))
    return get_tok_str(v, NULL);
  int k = v & (NAME_TMP_SLOTS - 1);
  if (name_tmp[k].tok != v || !name_tmp[k].name)
  {
    if (name_tmp[k].size < ts->len + 1)
    {
      name_tmp[k].size = (ts->len + 16) & ~15;
      name_tmp[k].name = tcc_realloc(name_tmp[k].name, name_tmp[k].size);
    }
    tcc_spill_read((long)((uintptr_t)ts->str >> 1), name_tmp[k].name, ts->len + 1);
    name_tmp[k].tok = v;
  }
  return name_tmp[k].name;
}

ST_FUNC const char *tok_name_load(TokenSym *ts)
{
  char *name = toksym_bump(&toksym_names, (ts->len + 2) & ~1u);
  tcc_spill_read((long)((uintptr_t)ts->str >> 1), name, ts->len + 1);
  if (!(ts->len & 1))
    name[ts->len + 1] = '\0'; /* the padding, as toksym_name_copy */
  ts->str = name;
  return name;
}

/* String token statistics - enable for analysis
static unsigned long str_total_added = 0;
static unsigned long str_bytes_copied = 0;
*/

/* ------------------------------------------------------------------------- */
/* CString handling */
static void cstr_realloc(CString *cstr, int new_size)
{
  int size;

  size = cstr->size_allocated;
  if (size < 8)
    size = 8; /* no need to allocate a too small first string */
  while (size < new_size)
    size = size * 2;
  cstr->data = tcc_realloc(cstr->data, size);
  cstr->size_allocated = size;
}

/* add a byte */
ST_INLN void cstr_ccat(CString *cstr, int ch)
{
  int size;
  size = cstr->size + 1;
  if (size > cstr->size_allocated)
    cstr_realloc(cstr, size);
  cstr->data[size - 1] = ch;
  cstr->size = size;
}

ST_INLN char *unicode_to_utf8(char *b, uint32_t Uc)
{
  if (Uc < 0x80)
    *b++ = Uc;
  else if (Uc < 0x800)
    *b++ = 192 + Uc / 64, *b++ = 128 + Uc % 64;
  else if (Uc - 0xd800u < 0x800)
    goto error;
  else if (Uc < 0x10000)
    *b++ = 224 + Uc / 4096, *b++ = 128 + Uc / 64 % 64, *b++ = 128 + Uc % 64;
  else if (Uc < 0x110000)
    *b++ = 240 + Uc / 262144, *b++ = 128 + Uc / 4096 % 64, *b++ = 128 + Uc / 64 % 64, *b++ = 128 + Uc % 64;
  else
  error:
    tcc_error("0x%x is not a valid universal character", Uc);
  return b;
}

/* add a unicode character expanded into utf8 */
ST_INLN void cstr_u8cat(CString *cstr, int ch)
{
  char buf[4], *e;
  e = unicode_to_utf8(buf, (uint32_t)ch);
  cstr_cat(cstr, buf, e - buf);
}

/* add string of 'len', or of its len/len+1 when 'len' == -1/0 */
ST_FUNC void cstr_cat(CString *cstr, const char *str, int len)
{
  int size;
  if (len <= 0)
    len = strlen(str) + 1 + len;
  size = cstr->size + len;
  if (size > cstr->size_allocated)
    cstr_realloc(cstr, size);
  if (len > 0) /* str may be NULL for an empty append: memmove(.., NULL, 0) is UB */
    memmove(cstr->data + cstr->size, str, len);
  cstr->size = size;
}

/* add a wide char */
ST_FUNC void cstr_wccat(CString *cstr, int ch)
{
  int size;
  size = cstr->size + sizeof(nwchar_t);
  if (size > cstr->size_allocated)
    cstr_realloc(cstr, size);
  *(nwchar_t *)(cstr->data + size - sizeof(nwchar_t)) = ch;
  cstr->size = size;
}

ST_FUNC void cstr_new(CString *cstr)
{
  memset(cstr, 0, sizeof(CString));
}

/* free string and reset it to NULL */
ST_FUNC void cstr_free(CString *cstr)
{
  tcc_free(cstr->data);
}

/* reset string to empty */
ST_FUNC void cstr_reset(CString *cstr)
{
  cstr->size = 0;
}

ST_FUNC int cstr_vprintf(CString *cstr, const char *fmt, va_list ap)
{
  va_list v;
  int len, size = 80;
  for (;;)
  {
    size += cstr->size;
    if (size > cstr->size_allocated)
      cstr_realloc(cstr, size);
    size = cstr->size_allocated - cstr->size;
    va_copy(v, ap);
    len = vsnprintf(cstr->data + cstr->size, size, fmt, v);
    va_end(v);
    if (len >= 0 && len < size)
      break;
    size *= 2;
  }
  cstr->size += len;
  return len;
}

ST_FUNC int cstr_printf(CString *cstr, const char *fmt, ...)
{
  va_list ap;
  int len;
  va_start(ap, fmt);
  len = cstr_vprintf(cstr, fmt, ap);
  va_end(ap);
  return len;
}

/* XXX: unicode ? */
static void add_char(CString *cstr, int c)
{
  if (c == '\'' || c == '\"' || c == '\\')
  {
    /* XXX: could be more precise if char or string */
    cstr_ccat(cstr, '\\');
  }
  if (c >= 32 && c <= 126)
  {
    cstr_ccat(cstr, c);
  }
  else
  {
    cstr_ccat(cstr, '\\');
    if (c == '\n')
    {
      cstr_ccat(cstr, 'n');
    }
    else
    {
      cstr_ccat(cstr, '0' + ((c >> 6) & 7));
      cstr_ccat(cstr, '0' + ((c >> 3) & 7));
      cstr_ccat(cstr, '0' + (c & 7));
    }
  }
}

/* ------------------------------------------------------------------------- */
/* allocate a new token */
static TokenSym *tok_alloc_new(TokenSym **pts, const char *str, int len)
{
  TokenSym *ts, **ptable;
  int i;

  if (tok_ident >= SYM_FIRST_ANOM)
    tcc_error("memory full (symbols)");

  /* expand token table if needed */
  i = tok_ident - TOK_IDENT;
  if (i >= table_ident_alloc)
  {
    int new_alloc = table_ident_alloc ? table_ident_alloc : TOK_IDENT_PREALLOC;
    while (new_alloc <= i)
#ifdef CONFIG_TCC_LOW_MEM
      /* Equal steps: doubling 1024 -> 2048 for a hello world's ~1200 ids
         left 3 KB of the device table unused.  Every user re-reads
         table_ident, so moving it is safe. */
      new_alloc += 256;
#else
      new_alloc = (int)(tcc_grow_size(new_alloc * sizeof(TokenSym *), (i + 1) * sizeof(TokenSym *)) /
                        sizeof(TokenSym *));
#endif
    ptable = tcc_realloc(table_ident, new_alloc * sizeof(TokenSym *));
    table_ident = ptable;
    table_ident_alloc = new_alloc;
  }

  ts = toksym_new(str, len);

  table_ident[i] = ts;
  ts->tok = tok_ident++;
  ts->has_define = 0;
  ts->sym_identifier = NULL;
  ts->hash_next = NULL;
  *pts = ts;
  /* Last, and only once the entry is linked into table_ident: materialising a
     predefine re-enters tok_alloc for the identifiers in its own body, and a
     half-built entry would be found by that recursion. */
  tcc_predef_try_materialize(ts, str, len);
  TCC_PREDEF_PROTO_INTERNED(ts);
  return ts;
}


/* ------------------------------------------------------------------------- */
/* Lazy builtin-token (keyword) interning.
 *
 * Upstream tcc interns ALL ~540 builtin tokens (keywords, __builtin_*, asm
 * directives, pragma names, ...) into table_ident at startup.  On YasOS that
 * cost ~48 KB of the toksym pool plus a big chunk of the table_ident prealloc
 * for tokens a typical tiny compile never references.  Instead we reserve the
 * whole builtin id range up-front (ids are fixed by enum order == blob order)
 * but only allocate a TokenSym for a builtin the first time it is actually
 * seen (lexed) or define_push'd.  Recognition uses a small static index over
 * the tcc_keywords blob, so unreferenced builtins cost nothing but their
 * reserved (NULL) table_ident slot.  The index is generated at build time
 * (gen_kw_index.c -> tcckw_index_.h): const and pointer-free, so it sits in
 * flash instead of costing every compiler process 8 KB of .bss. */
#include "tcckw_index_.h"
_Static_assert(KW_GEN_COUNT == NB_BUILTIN_TOKS, "tcckw_index_.h is stale");

/* Allocate (or return the existing) TokenSym for builtin token id `tok` in
 * [TOK_IDENT, TOK_IDENT+NB_BUILTIN_TOKS).  Idempotent; inserts into hash_ident
 * so subsequent lexes find it via the normal dynamic-hash path. */
static TokenSym *tok_materialize_builtin(int tok)
{
  int i = tok - TOK_IDENT;
  TokenSym *ts = table_ident[i];
  const char *str;
  int len, k;
  unsigned int h;
  if (ts)
    return ts;
  str = tcc_keywords + kw_off[i];
  len = kw_len[i];
  ts = toksym_new(str, len);
  ts->tok = tok;
  ts->has_define = 0;
  ts->sym_identifier = NULL;
  h = TOK_HASH_INIT;
  for (k = 0; k < len; k++)
    h = TOK_HASH_FUNC(h, ((unsigned char *)str)[k]);
  h &= (TOK_HASH_SIZE - 1);
  ts->hash_next = hash_ident[h];
  hash_ident[h] = ts;
  table_ident[i] = ts;
  TCC_PREDEF_PROTO_INTERNED(ts);
  return ts;
}

/* On a dynamic-hash miss, check whether `str` is a builtin token and, if so,
 * materialize it at its reserved id (returns the TokenSym).  Returns NULL if
 * `str` is not a builtin (caller then allocates a fresh user ident).  Shared
 * by both the tok_alloc path and the inline identifier lexer. */
static TokenSym *kw_lookup_materialize(unsigned int full_hash, const char *str, int len)
{
  unsigned int kh = full_hash & (KW_HASH_SIZE - 1);
  int e;
  for (e = kw_hash_head[kh]; e; e = kw_hash_next[e - 1])
  {
    int ki = e - 1;
    if (kw_len[ki] == len && !memcmp(tcc_keywords + kw_off[ki], str, len))
      return tok_materialize_builtin(TOK_IDENT + ki);
  }
  return NULL;
}

/* Return table_ident[v - TOK_IDENT], materializing a lazy builtin slot first
 * if needed.  For user ids the slot is always present (interned when the name
 * was first lexed), so this is just a deref; the range guard avoids touching
 * kw_off[] for non-builtin ids.  Used by writers that may target an as-yet-
 * unseen builtin: the startup define_push of __LINE__ etc., and codegen that
 * references runtime-helper / builtin names by fixed token id (e.g. the
 * __aeabi_* helpers via external_global_sym). */
ST_FUNC TokenSym *tok_ensure(int v)
{
  TokenSym *ts = table_ident[v - TOK_IDENT];
  if (!ts && (unsigned)(v - TOK_IDENT) < (unsigned)NB_BUILTIN_TOKS)
    ts = tok_materialize_builtin(v);
  return ts;
}

/* Find an already-interned token; NULL if `str` was never interned (a lazy
 * builtin slot that has not been materialized counts as not interned).
 * Interns nothing, so it leaves token ids and the lazy slots alone. */
ST_FUNC TokenSym *tok_find(const char *str, int len)
{
  TokenSym *ts;
  unsigned int h = TOK_HASH_INIT;
  int i;
  for (i = 0; i < len; i++)
    h = TOK_HASH_FUNC(h, ((unsigned char *)str)[i]);
  for (ts = hash_ident[h & (TOK_HASH_SIZE - 1)]; ts; ts = ts->hash_next)
    if (ts->len == len && !memcmp(tok_name(ts), str, len))
      return ts;
  return NULL;
}

/* find a token and add it if not found */
ST_FUNC TokenSym *tok_alloc(const char *str, int len)
{
  TokenSym *ts, **pts;
  int i;
  unsigned int h, full_hash;

  h = TOK_HASH_INIT;

  for (i = 0; i < len; i++)
  {
    h = TOK_HASH_FUNC(h, ((unsigned char *)str)[i]);
  }

  full_hash = h;
  ts = token_lookup_cache_find(full_hash, str, len);
  if (ts)
    return ts;

  h &= (TOK_HASH_SIZE - 1);

  pts = &hash_ident[h];
  for (;;)
  {
    ts = *pts;
    if (!ts)
      break;
    if (ts->len == len && !memcmp(tok_name(ts), str, len))
    {
      token_lookup_cache_store(full_hash, len, ts);
      return ts;
    }
    pts = &(ts->hash_next);
  }

  /* NOTE: ARM assembly suffix parsing is now handled entirely in asm_opcode()
   * via thumb_parse_token_suffix(). The aliasing below is disabled because
   * it loses the original token string (e.g., "bhs" becomes "b"), making it
   * impossible to extract the condition code later.
   */
#if 0 && defined(TCC_TARGET_ARM_ARCHV8M)
  if (parse_flags & PARSE_FLAG_ASM_FILE && len >= 3)
  {
    /* Check if this looks like <instr><cond> where cond is 2 chars */
    /* Use global condition codes array from arm-thumb-defs.h */
    /* Note: len >= 3 to handle short instructions like "bhs" (b + hs) */
    for (i = 0; cond_names[i].name != NULL; i++)
    {
      if (len >= 3 && memcmp(str + len - 2, cond_names[i].name, 2) == 0)
      {
        /* Found condition code suffix - try base instruction */
        TokenSym *base_ts = tok_alloc(str, len - 2);
        if (base_ts)
        {
          /* Create a token entry for the full string with the base token's ID */
          /* Note: We're creating a separate token entry but reusing the base token's ID.
           * This allows asm_opcode() to identify the base instruction while still
           * having access to the full token string for suffix parsing. */
          TokenSym *alias_ts = tok_alloc_new(pts, str, len);
          alias_ts->tok = base_ts->tok;
          return alias_ts;
        }
        break;
      }
    }
  }
#endif

  /* Not in the dynamic hash: it may be a builtin token (keyword, __builtin_xxx,
   * asm directive, ...) whose TokenSym has not been materialized yet.  Probe
   * the static keyword index; on a hit, materialize it at its reserved id so
   * that "tok == TOK_xxx" comparisons keep working. */
  ts = kw_lookup_materialize(full_hash, str, len);
  if (ts)
  {
    token_lookup_cache_store(full_hash, len, ts);
    return ts;
  }

  ts = tok_alloc_new(pts, str, len);
  token_lookup_cache_store(full_hash, len, ts);
  return ts;
}

ST_FUNC int tok_alloc_const(const char *str)
{
  return tok_alloc(str, strlen(str))->tok;
}

/* XXX: buffer overflow */
/* XXX: float tokens */
ST_FUNC const char *get_tok_str(int v, CValue *cv)
{
  char *p;
  int i, len;

  cstr_reset(&cstr_buf);
  p = cstr_buf.data;

  switch (v)
  {
  case TOK_CINT:
  case TOK_CUINT:
  case TOK_CLONG:
  case TOK_CULONG:
  case TOK_CLLONG:
  case TOK_CULLONG:
    /* XXX: not quite exact, but only useful for testing  */
#ifdef _WIN32
    snprintf(p, cstr_buf.size_allocated, "%u", (unsigned)cv->i);
#else
    snprintf(p, cstr_buf.size_allocated, "%llu", (unsigned long long)cv->i);
#endif
    break;
  case TOK_LCHAR:
    cstr_ccat(&cstr_buf, 'L');
  case TOK_CCHAR:
    cstr_ccat(&cstr_buf, '\'');
    add_char(&cstr_buf, cv->i);
    cstr_ccat(&cstr_buf, '\'');
    cstr_ccat(&cstr_buf, '\0');
    break;
  case TOK_PPNUM:
  case TOK_PPSTR:
    return (char *)cv->str.data;
  case TOK_LSTR:
    cstr_ccat(&cstr_buf, 'L');
  case TOK_STR:
    cstr_ccat(&cstr_buf, '\"');
    if (v == TOK_STR)
    {
      len = cv->str.size - 1;
      for (i = 0; i < len; i++)
        add_char(&cstr_buf, ((unsigned char *)cv->str.data)[i]);
    }
    else
    {
      len = (cv->str.size / sizeof(nwchar_t)) - 1;
      for (i = 0; i < len; i++)
        add_char(&cstr_buf, ((nwchar_t *)cv->str.data)[i]);
    }
    cstr_ccat(&cstr_buf, '\"');
    cstr_ccat(&cstr_buf, '\0');
    break;

  case TOK_CFLOAT:
    return strcpy(p, "<float>");
  case TOK_CDOUBLE:
    return strcpy(p, "<double>");
  case TOK_CLDOUBLE:
    return strcpy(p, "<long double>");
  case TOK_CFLOAT_I:
    return strcpy(p, "<imaginary float>");
  case TOK_CDOUBLE_I:
    return strcpy(p, "<imaginary double>");
  case TOK_CLDOUBLE_I:
    return strcpy(p, "<imaginary long double>");
  case TOK_CINT_I:
    return strcpy(p, "<imaginary int>");
  case TOK_LINENUM:
    return strcpy(p, "<linenumber>");
  case TOK_PACK_REPLAY:
    return strcpy(p, "<pack-replay>");

  /* above tokens have value, the ones below don't */
  case TOK_LT:
    v = '<';
    goto addv;
  case TOK_GT:
    v = '>';
    goto addv;
  case TOK_DOTS:
    return strcpy(p, "...");
  case TOK_A_SHL:
    return strcpy(p, "<<=");
  case TOK_A_SAR:
    return strcpy(p, ">>=");
  case TOK_EOF:
    return strcpy(p, "<eof>");
  case 0: /* anonymous nameless symbols */
    return strcpy(p, "<no name>");
  default:
    v &= ~(SYM_FIELD | SYM_STRUCT);
    if (v < TOK_IDENT)
    {
      /* search in two bytes table */
      const unsigned char *q = tok_two_chars;
      while (*q)
      {
        if (q[2] == v)
        {
          *p++ = q[0];
          *p++ = q[1];
          *p = '\0';
          return cstr_buf.data;
        }
        q += 3;
      }
      if (v >= 127 || (v < 32 && !is_space(v) && v != '\n'))
      {
        snprintf(p, cstr_buf.size_allocated, "<\\x%02x>", v);
        break;
      }
    addv:
      *p++ = v;
      *p = '\0';
    }
    else if (v < tok_ident)
    {
      TokenSym *ts = table_ident[v - TOK_IDENT];
      if (ts)
        return tok_name(ts);
      /* Lazy builtin not materialized: its string lives in the blob -- but
         ONLY for the reserved builtin range.  A NULL slot outside it means a
         bogus token id, and indexing kw_off[] with it reads past the array
         and hands back whatever follows: on the RP2350 that surfaced as a
         HardFault inside tcc_ir_opt_const_aggregate_fold, whose callee-name
         strcmp walked the garbage pointer (device -O1; benign on the host,
         where the bytes after kw_off[] happen to be readable).  tok_ensure()
         guards the same array for the same reason. */
      if ((unsigned)(v - TOK_IDENT) < (unsigned)NB_BUILTIN_TOKS)
        return (char *)tcc_keywords + kw_off[v - TOK_IDENT];
      return NULL;
    }
    else if (v >= SYM_FIRST_ANOM)
    {
      /* special name for anonymous symbol */
      snprintf(p, cstr_buf.size_allocated, "L.%u", v - SYM_FIRST_ANOM);
    }
    else
    {
      /* should never happen */
      return NULL;
    }
    break;
  }
  return cstr_buf.data;
}

/* return the current character, handling end of block if necessary
   (but not stray) */
/* -Wimplicit-fallthrough: the end of the buffer before the last refill, for a
   comment read across it (ft_comment_across) */
#define FT_CARRY 256
static uint8_t ft_carry[FT_CARRY];
static int ft_carry_len;
static const uint8_t *ft_carry_end; /* where the old buffer ended */
static unsigned ft_refills;
/* a fall-through comment's blanks reached the end of the buffer: the label
   may be at the start of the next one */
static unsigned char ft_pending, ft_partial_len;
static uint8_t ft_partial[8]; /* the start of `case` / `default` it ended in */
static int ft_label_after(const uint8_t *q, const uint8_t *e, int *lines);

static int handle_eob(void)
{
  BufferedFile *bf = file;
  int len;

  /* only tries to read if really end of buffer */
  if (bf->buf_ptr >= bf->buf_end)
  {
    if (bf->fd >= 0)
    {
      if (tcc_state->warn_implicit_fallthrough | tcc_diag_snaps_on(warn_implicit_fallthrough))
      {
        int n = bf->buf_end - bf->buffer;
        if (n > FT_CARRY)
          n = FT_CARRY;
        memcpy(ft_carry, bf->buf_end - n, n);
        ft_carry_len = n;
        ft_carry_end = bf->buf_end;
        ft_refills++;
      }
      /* -fstack-usage names a definition by its column (tcc_stack_usage_name_column) */
      if (TCC_SU_WANTED(tcc_state))
      {
        const uint8_t *p = bf->buf_end;
        while (p > bf->buffer && p[-1] != '\n')
          p--;
        bf->col_base = (int)(bf->buf_end - p) + (p == bf->buffer ? bf->col_base : 0);
      }
#if defined(PARSE_DEBUG)
      len = 1;
#else
      len = IO_BUF_SIZE;
#endif
      len = read(bf->fd, bf->buffer, len);
      if (len < 0)
        len = 0;
    }
    else
    {
      len = 0;
    }
    total_bytes += len;
    bf->buf_ptr = bf->buffer;
    bf->buf_end = bf->buffer + len;
    *bf->buf_end = CH_EOB;
    if (ft_pending)
    {
      /* the label after a fall-through comment the last buffer ended in */
      int lines = 0;
      ft_pending = 0;
      if (ft_partial_len)
      {
        /* `ca` + `se ...`: the word across the two buffers */
        uint8_t w[24];
        int n = len < 16 ? len : 16;
        memcpy(w, ft_partial, ft_partial_len);
        memcpy(w + ft_partial_len, bf->buffer, n);
        w[ft_partial_len + n] = ' ';
        if (ft_label_after(w, w + ft_partial_len + n + 1, &lines) > 0)
          tcc_diag_first(tcc_state, bf->line_num, DIAG_KEY_FALLTHROUGH_COMMENT);
        ft_partial_len = 0;
      }
      else if (ft_label_after(bf->buffer, bf->buf_end, &lines) > 0)
        tcc_diag_first(tcc_state, bf->line_num + lines, DIAG_KEY_FALLTHROUGH_COMMENT);
    }
  }
  if (bf->buf_ptr < bf->buf_end)
  {
    return bf->buf_ptr[0];
  }
  bf->buf_ptr = bf->buf_end;
  return CH_EOF;
}

/* read next char from current input file and handle end of input buffer */
static int next_c(void)
{
  int ch = *++file->buf_ptr;
  /* end of buffer/file handling */
  if (ch == CH_EOB && file->buf_ptr >= file->buf_end)
    ch = handle_eob();
  return ch;
}

/* input with '\[\r]\n' handling. */
static int handle_stray_noerror(int err)
{
  int ch;
  while ((ch = next_c()) == '\\')
  {
    ch = next_c();
    if (ch == '\n')
    {
    newl:
      file->line_num++;
    }
    else
    {
      if (ch == '\r')
      {
        ch = next_c();
        if (ch == '\n')
          goto newl;
        *--file->buf_ptr = '\r';
      }
      if (err)
        tcc_error("stray '\\' in program");
      /* may take advantage of 'BufferedFile.unget[4}' */
      return *--file->buf_ptr = '\\';
    }
  }
  return ch;
}

#define ninp() handle_stray_noerror(0)

/* handle '\\' in strings, comments and skipped regions */
static int handle_bs(uint8_t **p)
{
  int c;
  file->buf_ptr = *p - 1;
  c = ninp();
  *p = file->buf_ptr;
  return c;
}

/* skip the stray and handle the \\n case. Output an error if
   incorrect char after the stray */
static int handle_stray(uint8_t **p)
{
  int c;
  file->buf_ptr = *p - 1;
  c = handle_stray_noerror(!(parse_flags & PARSE_FLAG_ACCEPT_STRAYS));
  *p = file->buf_ptr;
  return c;
}

/* handle the complicated stray case */
#define PEEKC(c, p)                                                                                                    \
  {                                                                                                                    \
    c = *++p;                                                                                                          \
    if (c == '\\')                                                                                                     \
      c = handle_stray(&p);                                                                                            \
  }

static int skip_spaces(void)
{
  int ch;
  --file->buf_ptr;
  do
  {
    ch = ninp();
  } while (isidnum_table[ch - CH_EOF] & IS_SPC);
  return ch;
}

/* single line C++ comments */
static uint8_t *parse_line_comment(uint8_t *p)
{
  int c;
  for (;;)
  {
    for (;;)
    {
      c = *++p;
    redo:
      if (c == '\n' || c == '\\')
        break;
      c = *++p;
      if (c == '\n' || c == '\\')
        break;
    }
    if (c == '\n')
      break;
    c = handle_bs(&p);
    if (c == CH_EOF)
      break;
    if (c != '\\')
      goto redo;
  }
  return p;
}

/* C comments */
static uint8_t *parse_comment(uint8_t *p)
{
  int c;
  for (;;)
  {
    uint8_t *q, *r;
    size_t len;

    q = p + 1;
    if (q < file->buf_end)
    {
      uint8_t *found = file->buf_end;

      /* Each search stops at what the one before it found: without that, a
         comment line costs a walk of the rest of the I/O buffer for the '\\'
         that is almost never there. */
      len = file->buf_end - q;
      r = memchr(q, '\n', len);
      if (r)
      {
        found = r;
        len = r - q;
      }
      r = memchr(q, '*', len);
      if (r)
      {
        found = r;
        len = r - q;
      }
      r = memchr(q, '\\', len);
      if (r)
        found = r;
      p = found;
      c = (found < file->buf_end) ? *p : '\\';
    }
    else
    {
      p = file->buf_end;
      c = '\\';
    }

    if (c == '\n')
    {
      file->line_num++;
      continue;
    }
    else if (c != '*')
    {
      c = handle_bs(&p);
      if (c == CH_EOF)
        lex_error_stuck("unexpected end of file in comment", 0);
      if (c == '\n')
      {
        file->line_num++;
      }
      else if (c == '*')
      {
        goto star;
      }
    }
    else
    {
    star:
      do
      {
        c = *++p;
      } while (c == '*');
      if (c == '\\')
        c = handle_bs(&p);
      if (c == '/')
        break;
      if (c == CH_EOF)
        lex_error_stuck("unexpected end of file in comment", 0);
      if (c == '\n')
      {
        file->line_num++;
      }
      else if (c == '*')
      {
        goto star;
      }
    }
  }
  return p + 1;
}

/* -Wimplicit-fallthrough: gcc's (=3) fall-through comments.  The whole
 * comment is `-fallthrough`, `@fallthrough@`, `lint -fallthrough`, or
 * [ELSE,? |INTENTIONAL(LY)? ]FALL(S | |-)?THR(OUGH|U) -- in capitals, in
 * lower case, or capitalised (Fall through, Falls Through, Fallthrough) --
 * with [ \t.!] around it and an optional `- reason` after. */
static int ft_lit(const uint8_t **pp, const uint8_t *e, const char *w)
{
  size_t n = strlen(w);
  if ((size_t)(e - *pp) < n || memcmp(*pp, w, n))
    return 0;
  *pp += n;
  return 1;
}

static int ft_tail(const uint8_t *q, const uint8_t *e)
{
  while (q < e && (*q == ' ' || *q == '\t' || *q == '.' || *q == '!'))
    q++;
  return q == e || *q == '-';
}

static int fallthrough_comment_text(const uint8_t *p, const uint8_t *e)
{
  /* per casing: prefixes (NULL-ended), "fall", what may follow it, "thr" */
  static const char *const prefix[3][9] = {
      {"ELSE, ", "ELSE ", "INTENTIONALLY ", "INTENTIONAL ", NULL},
      {"Else, ", "Else ", "Intentionally ", "Intentional ", NULL},
      {"else, ", "else ", "Else, ", "Else ", "intentionally ", "intentional ", "Intentionally ", "Intentional ",
       NULL}};
  static const char *const fall[3] = {"FALL", "Fall", "fall"};
  static const char *const mid[3][8] = {{"S THR", " THR", "-THR", "THR", NULL},
                                        {"s Thr", "s thr", " Thr", " thr", "-Thr", "-thr", "thr", NULL},
                                        {"s thr", " thr", "-thr", "thr", NULL}};
  static const char *const end[3][2] = {{"OUGH", "U"}, {"ough", "u"}, {"ough", "u"}};
  while (p < e && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n'))
    p++;
  while (e > p && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n'))
    e--;
  {
    const uint8_t *q = p;
    if ((ft_lit(&q, e, "-fallthrough") || ft_lit(&q, e, "@fallthrough@")) && q == e)
      return 1;
    q = p;
    if (ft_lit(&q, e, "lint -fallthrough"))
    {
      while (q < e && (*q == ' ' || *q == '\t'))
        q++;
      if (q == e)
        return 1;
    }
  }
  while (p < e && (*p == ' ' || *p == '\t' || *p == '.' || *p == '!'))
    p++;
  for (int c = 0; c < 3; c++)
    for (int i = -1; i < 0 || prefix[c][i]; i++)
    {
      const uint8_t *q = p;
      if (i >= 0 && !ft_lit(&q, e, prefix[c][i]))
        continue;
      if (!ft_lit(&q, e, fall[c]))
        continue;
      for (int m = 0; mid[c][m]; m++)
      {
        const uint8_t *r = q;
        if (!ft_lit(&r, e, mid[c][m]))
          continue;
        for (int k = 0; k < 2; k++)
        {
          const uint8_t *t = r;
          if (ft_lit(&t, e, end[c][k]) && ft_tail(t, e))
            return 1;
        }
      }
    }
  return 0;
}

/* After a fall-through comment, from Q to E: blanks and other comments (as
 * gcc), then `case` or `default`?  1 yes (*LINES newlines before it), 0 no,
 * -1 the buffer ended first */
static int ft_label_after(const uint8_t *q, const uint8_t *e, int *lines)
{
  for (; q < e; q++)
  {
    if (*q == '\n')
      ++*lines;
    else if (*q == '/' && q + 1 < e && q[1] == '*')
    {
      for (q += 2; q + 1 < e && !(q[0] == '*' && q[1] == '/'); q++)
        *lines += *q == '\n';
      if (q + 1 >= e)
        return 0;
      q++;
    }
    else if (*q == '/' && q + 1 < e && q[1] == '/')
    {
      while (q < e && *q != '\n')
        q++;
      q--;
    }
    else if (!(*q == ' ' || *q == '\t' || *q == '\r' || *q == '\f' || *q == '\v'))
      break;
  }
  if (q >= e)
    return -1;
  if (e - q < 7 && (!memcmp(q, "case", e - q < 4 ? e - q : 4) || !memcmp(q, "default", e - q)) &&
      (e - q < 4 || memcmp(q, "case", 4)))
  {
    /* the buffer ends inside the word */
    memcpy(ft_partial, q, e - q);
    ft_partial_len = e - q;
    return -1;
  }
  return (ft_lit(&q, e, "case") || ft_lit(&q, e, "default")) && !(isidnum_table[*q - CH_EOF] & (IS_ID | IS_NUM));
}

/* A recognised fall-through comment ending at Q (in the I/O buffer) that only
 * blanks and other comments separate from a `case` or `default`: mark that
 * label's line.  The parser asks for the mark (tcc_diag_marked) when the case
 * is reached by falling through, and a body the inliner replays later still
 * finds it. */
static void fallthrough_comment(const uint8_t *text, const uint8_t *text_end, const uint8_t *q)
{
  int lines = 0, r;
  if (!fallthrough_comment_text(text, text_end))
    return;
  ft_partial_len = 0;
  r = ft_label_after(q, file->buf_end, &lines);
  if (r > 0)
    tcc_diag_first(tcc_state, file->line_num + lines, DIAG_KEY_FALLTHROUGH_COMMENT);
  else if (r < 0 && file->fd >= 0)
    ft_pending = 1;
}

/* A comment from START (in the buffer before one refill) to END (in the new
   one), followed by Q: its text from the carried tail and the new buffer */
static void ft_comment_across(const uint8_t *start, const uint8_t *end, const uint8_t *q)
{
  uint8_t text[2 * FT_CARRY];
  long from = ft_carry_end - start, head = from, tail = end - file->buffer;
  if (tail < 0) /* the comment's end (`*` `/`) across the two */
    head += tail, tail = 0;
  if (head < 0 || from > ft_carry_len || head + tail > (long)sizeof text)
    return;
  memcpy(text, ft_carry + ft_carry_len - from, head);
  memcpy(text + head, file->buffer, tail);
  fallthrough_comment(text, text + head + tail, q);
}

/* parse a string without interpreting escapes */
static uint8_t *parse_pp_string(uint8_t *p, int sep, CString *str)
{
  int c;
  for (;;)
  {
    c = *++p;
  redo:
    if (c == sep)
    {
      break;
    }
    else if (c == '\\')
    {
      c = handle_bs(&p);
      if (c == CH_EOF)
      {
      unterminated_string:
        /* XXX: indicate line number of start of string */
        tok_flags &= ~TOK_FLAG_BOL;
        lex_error_stuck("missing terminating %c character", sep);
      }
      else if (c == '\\')
      {
        if (str)
          cstr_ccat(str, c);
        c = *++p;
        /* add char after '\\' unconditionally */
        if (c == '\\')
        {
          c = handle_bs(&p);
          if (c == CH_EOF)
            goto unterminated_string;
        }
        goto add_char;
      }
      else
      {
        goto redo;
      }
    }
    else if (c == '\n')
    {
    add_lf:
      if (ACCEPT_LF_IN_STRINGS)
      {
        file->line_num++;
        goto add_char;
      }
      else if (str)
      { /* not skipping */
        goto unterminated_string;
      }
      else
      {
        // tcc_warning("missing terminating %c character", sep);
        return p;
      }
    }
    else if (c == '\r')
    {
      c = *++p;
      if (c == '\\')
        c = handle_bs(&p);
      if (c == '\n')
        goto add_lf;
      if (c == CH_EOF)
        goto unterminated_string;
      if (str)
        cstr_ccat(str, '\r');
      goto redo;
    }
    else
    {
    add_char:
      if (str)
        cstr_ccat(str, c);
    }
  }
  p++;
  return p;
}

/* skip block of text until #else, #elif or #endif. skip also pairs of
   #if/#endif */
static void preprocess_skip(void)
{
  int a, start_of_line, c, in_warn_or_error;
  uint8_t *p;

  p = file->buf_ptr;
  a = 0;
redo_start:
  start_of_line = 1;
  in_warn_or_error = 0;
  for (;;)
  {
    c = *p;
    switch (c)
    {
    case ' ':
    case '\t':
    case '\f':
    case '\v':
    case '\r':
      p++;
      continue;
    case '\n':
      file->line_num++;
      p++;
      goto redo_start;
    case '\\':
      c = handle_bs(&p);
      if (c == CH_EOF)
        expect("#endif");
      if (c == '\\')
        ++p;
      continue;
    /* skip strings */
    case '\"':
    case '\'':
      if (in_warn_or_error)
        goto _default;
      tok_flags &= ~TOK_FLAG_BOL;
      p = parse_pp_string(p, c, NULL);
      break;
    /* skip comments */
    case '/':
      if (in_warn_or_error)
        goto _default;
      ++p;
      c = handle_bs(&p);
      if (c == '*')
      {
        p = parse_comment(p);
      }
      else if (c == '/')
      {
        p = parse_line_comment(p);
      }
      continue;
    case '#':
      p++;
      if (start_of_line)
      {
        file->buf_ptr = p;
        next_nomacro();
        p = file->buf_ptr;
        if (a == 0 && (tok == TOK_ELSE || tok == TOK_ELIF || tok == TOK_ENDIF))
          goto the_end;
        if (tok == TOK_IF || tok == TOK_IFDEF || tok == TOK_IFNDEF)
          a++;
        else if (tok == TOK_ENDIF)
          a--;
        else if (tok == TOK_ERROR || tok == TOK_WARNING)
          in_warn_or_error = 1;
        else if (tok == TOK_LINEFEED)
          goto redo_start;
        else if (parse_flags & PARSE_FLAG_ASM_FILE)
          p = parse_line_comment(p - 1);
      }
#if !defined(TCC_TARGET_ARM)
      else if (parse_flags & PARSE_FLAG_ASM_FILE)
        p = parse_line_comment(p - 1);
#else
      /* ARM assembly uses '#' for constants */
#endif
      break;
    _default:
    default:
      p++;
      break;
    }
    start_of_line = 0;
  }
the_end:;
  file->buf_ptr = p;
}

#if 0
/* return the number of additional 'ints' necessary to store the
   token */
static inline int tok_size(const int *p)
{
    switch(*p) {
        /* 4 bytes */
    case TOK_CINT:
    case TOK_CUINT:
    case TOK_CCHAR:
    case TOK_LCHAR:
    case TOK_CFLOAT:
    case TOK_LINENUM:
        return 1 + 1;
    case TOK_STR:
    case TOK_LSTR:
    case TOK_PPNUM:
    case TOK_PPSTR:
        return 1 + 1 + (p[1] + 3) / 4;
    case TOK_CLONG:
    case TOK_CULONG:
	return 1 + LONG_SIZE / 4;
    case TOK_CDOUBLE:
    case TOK_CLLONG:
    case TOK_CULLONG:
        return 1 + 2;
    case TOK_CLDOUBLE:
#ifdef TCC_USING_DOUBLE_FOR_LDOUBLE
        return 1 + 8 / 4;
#else
        return 1 + LDOUBLE_SIZE / 4;
#endif
    default:
        return 1 + 0;
    }
}
#endif

/* token string handling */
ST_INLN void tok_str_new(TokenString *s)
{
  s->len = s->need_spc = 0;
  s->allocated_len = 0; /* 0 means using inline buffer (small_buf) */
  s->last_line_num = 0; /* 0 means no line recorded yet */
  s->spill_pos = 0;
  s->owner = NULL;
  s->src = NULL;
  s->sys = 0;
  s->xs = NULL;
  s->xv = NULL;
}

ST_FUNC TokenString *tok_str_alloc(void)
{
  TokenString *str = tal_realloc(tokstr_alloc, 0, sizeof *str);
  tok_str_new(str);
  return str;
}

/* Note: str pointer passed here must be the heap pointer, not inline buffer */
ST_FUNC void tok_str_free_str(int *str)
{
  tcc_free(str);
}

static void tok_str_spill_forget(TokenString *s);

static void sys_ranges_drop(TokenString *s);
static void sys_ranges_rekey(TokenString *from, TokenString *to);

ST_FUNC void tok_str_free(TokenString *str)
{
  if (tok_sys_ranges)
    sys_ranges_drop(str);
  tok_str_spill_forget(str);
  if (str->allocated_len > 0)
    tok_str_free_str(str->data.str);
  tal_free(tokstr_alloc, str);
}

/* Ensure the TokenString buffer is heap-allocated.
   Returns the heap buffer pointer. Used when storing buffer refs in Sym->d/e.
   For empty buffers, returns NULL (safe to tok_str_free_str). */
ST_FUNC int *tok_str_ensure_heap(TokenString *s)
{
  if (s->len == 0)
    return NULL;
  if (s->allocated_len == 0)
  {
    /* Convert inline buffer to heap buffer */
    int *heap_buf = tcc_malloc(s->len * sizeof(int));
    memcpy(heap_buf, s->data.small_buf, s->len * sizeof(int));
    s->data.str = heap_buf;
    s->allocated_len = s->len;
  }
  return s->data.str;
}

ST_FUNC int *tok_str_realloc(TokenString *s, int new_size)
{
  int *str, size;

  /* Check if we can still use the inline buffer */
  if (new_size <= TOKSTR_SMALL_BUFSIZE && s->allocated_len == 0)
    return s->data.small_buf;

  /* Transition from inline to heap buffer */
  if (s->allocated_len == 0)
  {
    /* Allocate new heap buffer and copy inline data */
    size = TOKSTR_SMALL_BUFSIZE << 1;
    while (size < new_size)
      size <<= 1;
    str = tcc_malloc(size * sizeof(int));
    if (s->len > 0)
      memcpy(str, s->data.small_buf, s->len * sizeof(int));
    s->data.str = str;
    s->allocated_len = size;
    return str;
  }

  /* Already using heap buffer - grow if needed.  Doubling, not
   * tcc_grow_size: a kept string is trimmed to its length (tok_str_fit), so
   * its slack does not last, and the holes its outgrown buffers leave are the
   * sizes the next string asks for.  In tcc_grow_size's steps they were not:
   * a Zig kernel compile without the internal spill (8 MB of saved bodies)
   * peaked 0.7 MB higher, in free 4-8 KB holes. */
  size = s->allocated_len;
  while (size < new_size)
    size <<= 1;
  if (size > s->allocated_len)
  {
    str = tcc_realloc(s->data.str, size * sizeof(int));
    s->allocated_len = size;
    s->data.str = str;
  }
  return s->data.str;
}

/* Shrink heap-allocated token string buffer to exact size.
   With system malloc, shrinking returns memory properly. */
static void tok_str_shrink(TokenString *s)
{
  int exact = s->len;
  if (exact > 0 && s->allocated_len > exact * 2 && s->allocated_len - exact > TOKSTR_SMALL_BUFSIZE)
  {
    int *ns = tcc_realloc(s->data.str, exact * sizeof(int));
    if (ns)
    {
      s->data.str = ns;
      s->allocated_len = exact;
    }
  }
}

/* For a token string kept as long as a macro or a saved function body,
   usually the whole TU: give back all of the doubling slack, not only when
   it is over half the buffer (a 40-token body kept 64 slots). */
ST_FUNC void tok_str_fit(TokenString *s)
{
  if (s->allocated_len > s->len)
  {
    s->data.str = tcc_realloc(s->data.str, s->len * sizeof(int));
    s->allocated_len = s->len;
  }
}

/* A TokenString kept past the statement that made it (a saved function body)
   moves out of the TokenString arena: an arena only rewinds once every struct
   in it is freed, so one long-lived struct pins it, and the short-lived ones
   allocated after it keep adding arenas (48 KB of them at -O2 on regcomp.c).
   tok_str_free still takes it: tal_free hands foreign pointers to tcc_free. */
ST_FUNC TokenString *tok_str_keep(TokenString *s)
{
#ifdef USE_TAL
  TokenString *k = tcc_malloc(sizeof *k);
  *k = *s;
  tal_free(tokstr_alloc, s);
  if (tok_sys_ranges)
    sys_ranges_rekey(s, k);
  return k;
#else
  /* Saved bodies run to thousands; their 96-byte headers were 64 bytes of
   * small_buf they never use.  The tokens go to the heap if they were inline
   * (a kept string is never appended to again), and the header shrinks to
   * end at data.str. */
  if (s->allocated_len == 0)
  {
    int n = s->len > 0 ? s->len : 1;
    int *buf = tcc_malloc(n * sizeof(int));
    memcpy(buf, s->data.small_buf, s->len * sizeof(int));
    s->data.str = buf;
    s->allocated_len = n;
  }
  TokenString *k = tcc_realloc(s, TOKSTR_KEPT_SIZE);
  if (tok_sys_ranges)
    sys_ranges_rekey(s, k);
  return k;
#endif
}

/* Internal spill (-f[no-]internal-spill, on by default): kept function bodies
 * (saved for deferral, inlining or late re-optimisation) are written to a file
 * and their buffers freed; a body is read back when something accesses it
 * (tok_str_buf) and freed again at the next point where no token string is
 * being replayed.  Tokens hold no pointers -- values are inline, identifiers
 * are token numbers -- so a buffer is the same after the round trip.  The
 * prototype parameter lists of symtab.c (sym_params_release) share the file.
 * On Zig's 12.7 MB kernel.c the saved bodies were the largest block of the
 * heap.  The file is <output>.<pid>.spill, beside the output: on a board that
 * is the storage the object goes to, not a RAM /tmp. */
/* A spilled string keeps its copy's place in its header (spill_pos).  The
 * copies read back are listed, with the buffer each was read into, until they
 * are dropped (tok_str_release, tok_str_release_spilled) or their string is
 * freed (tok_str_spill_forget). */
typedef struct TokSpillLoaded
{
  TokenString *s;
  int *buf;
} TokSpillLoaded;

static TokSpillLoaded *tok_spill_loaded;
static int nb_tok_spill_loaded, cap_tok_spill_loaded;
static int tok_spill_fd = -1;
static int tok_spill_off; /* the file could not be made or written: spill no more this TU */
static char *tok_spill_path;

/* Writes collect here and reach the file only when it fills, so a small
 * compilation -- all its records in the buffer -- never creates the file, and
 * a large one writes in big pieces.  A record is either wholly in the file or
 * wholly in the buffer (at file offset spill_flushed + its position). */
#define SPILL_WBUF 8192
static unsigned char *spill_wbuf;
static size_t spill_wlen;
static long spill_flushed; /* bytes in the file */

/* A spilled body saves its buffer and that buffer's malloc header, so all
 * but the tiniest go: 64 ints kept 1441 bodies of a Zig kernel compile in
 * memory (0.16 MB), for no measurable time saved. */
#define TOK_SPILL_MIN_INTS 8

/* <output>.<pid>.spill: the -o file, else what the driver will name the
 * output (also for an output in /dev, -o /dev/null).  The pid keeps it this
 * compile's own: two compiles of one output at once (a build naming an
 * object twice, two tests on one fixture) each truncated and removed the
 * other's file, and one failed reading it back. */
static char *spill_path_for(TCCState *s1)
{
  char buf[1024];
  int pid = (int)getpid();
  if (s1->outfile && strcmp(s1->outfile, "-") && strncmp(s1->outfile, "/dev/", 5))
    snprintf(buf, sizeof buf, "%s.%d.spill", s1->outfile, pid);
  else if (s1->output_type == TCC_OUTPUT_OBJ && s1->current_filename && strcmp(s1->current_filename, "-"))
  {
    snprintf(buf, sizeof buf, "%s", tcc_basename(s1->current_filename));
    char *ext = tcc_fileextension(buf);
    snprintf(ext, sizeof buf - (size_t)(ext - buf), ".o.%d.spill", pid);
  }
  else
    snprintf(buf, sizeof buf, "a.out.%d.spill", pid);
  return tcc_strdup(buf);
}

static int tok_spill_open(void)
{
  if (tok_spill_fd >= 0)
    return 0;
  tok_spill_path = spill_path_for(tcc_state);
  tok_spill_fd = open(tok_spill_path, O_RDWR | O_CREAT | O_TRUNC | O_BINARY, 0600);
  if (tok_spill_fd < 0)
  {
    /* No warning: the output is the same either way, and a build with
     * -Werror or a read-only directory must not fail over it. */
    tok_spill_off = 1;
    return -1;
  }
  spill_flushed = 0;
  return 0;
}

/* Write the buffer out.  On failure it stays: what it holds is still read
 * from it, and nothing more is spilled. */
static int spill_flush(void)
{
  if (!spill_wlen)
    return 0;
  if (tok_spill_open() < 0)
    return -1;
  if (lseek(tok_spill_fd, spill_flushed, SEEK_SET) < 0 ||
      write(tok_spill_fd, spill_wbuf, spill_wlen) != (ssize_t)spill_wlen)
  {
    tok_spill_off = 1;
    return -1;
  }
  spill_flushed += (long)spill_wlen;
  spill_wlen = 0;
  return 0;
}

/* Spilling is on and has not failed. */
ST_FUNC int tcc_spill_active(void)
{
  return !tok_spill_off && tcc_state->internal_spill && tcc_state->output_type != TCC_OUTPUT_PREPROCESS;
}

/* Append N bytes: their offset, or -1 (not spilled; the caller keeps them). */
ST_FUNC long tcc_spill_append(const void *p, size_t n)
{
  long off;
  if (!tcc_spill_active())
    return -1;
  if (!spill_wbuf)
    spill_wbuf = tcc_malloc(SPILL_WBUF);
  if (spill_wlen + n > SPILL_WBUF && spill_flush() < 0)
    return -1;
  if (n > SPILL_WBUF)
  {
    /* the buffer is empty now: straight to the file */
    if (tok_spill_open() < 0)
      return -1;
    if (lseek(tok_spill_fd, spill_flushed, SEEK_SET) < 0 || write(tok_spill_fd, p, n) != (ssize_t)n)
    {
        tok_spill_off = 1;
      return -1;
    }
    off = spill_flushed;
    spill_flushed += (long)n;
    return off;
  }
  off = spill_flushed + (long)spill_wlen;
  memcpy(spill_wbuf + spill_wlen, p, n);
  spill_wlen += n;
  return off;
}

/* Read back N bytes appended at OFF. */
ST_FUNC void tcc_spill_read(long off, void *p, size_t n)
{
  size_t done = 0;
  if (off >= spill_flushed)
  {
    memcpy(p, spill_wbuf + (off - spill_flushed), n);
    return;
  }
  if (tok_spill_fd < 0 || lseek(tok_spill_fd, off, SEEK_SET) < 0)
    tcc_error("internal spill: cannot seek in '%s'", tok_spill_path);
  while (done < n)
  {
    ssize_t r = read(tok_spill_fd, (char *)p + done, n - done);
    if (r <= 0)
      tcc_error("internal spill: cannot read back '%s'", tok_spill_path);
    done += (size_t)r;
  }
}

/* Write S's buffer to the spill file and free it.  S must be a kept,
 * finished token string: it is never appended to again. */
ST_FUNC void tok_str_spill(TokenString *s)
{
  if (!tcc_spill_active() || s->allocated_len <= 0 || s->len < TOK_SPILL_MIN_INTS || s->spill_pos)
    return;
  size_t bytes = (size_t)s->len * sizeof(int);
  long off = tcc_spill_append(s->data.str, bytes);
  if (off < 0 || off >= 0x7fffffffL)
    return;
  tcc_free(s->data.str);
  s->data.str = NULL;
  s->allocated_len = -1;
  s->spill_pos = (int)off + 1;
}

int *tok_str_unspill(TokenString *s)
{
  if (!s->spill_pos)
    tcc_error("internal error: spilled token string without a copy");
  size_t bytes = (size_t)s->len * sizeof(int);
  int *buf = tcc_malloc(bytes);
  tcc_spill_read(s->spill_pos - 1L, buf, bytes);
  s->data.str = buf;
  s->allocated_len = s->len;
  if (nb_tok_spill_loaded == cap_tok_spill_loaded)
  {
    cap_tok_spill_loaded = cap_tok_spill_loaded ? cap_tok_spill_loaded * 2 : 64;
    tok_spill_loaded = tcc_realloc(tok_spill_loaded, cap_tok_spill_loaded * sizeof *tok_spill_loaded);
  }
  tok_spill_loaded[nb_tok_spill_loaded].s = s;
  tok_spill_loaded[nb_tok_spill_loaded].buf = buf;
  nb_tok_spill_loaded++;
  return buf;
}

/* Drop the copy read back into BUF for S, if S still holds exactly it; a
 * string that swapped its buffer since keeps the new one and leaves the
 * spill file. */
static void tok_spill_drop_loaded(TokenString *s, int *buf)
{
  if (s->data.str == buf && s->allocated_len == s->len)
  {
    tcc_free(buf);
    s->data.str = NULL;
    s->allocated_len = -1;
  }
  else
    s->spill_pos = 0;
}

/* S's read-back copy is no longer referenced: free it until the next access.
 * Only for a copy the caller itself caused to be read (S was spilled,
 * allocated_len < 0, before its access) or where nothing replays S: a replay
 * reads the buffer in place. */
ST_FUNC void tok_str_release(TokenString *s)
{
  if (!s->spill_pos)
    return;
  for (int i = nb_tok_spill_loaded; i-- > 0;)
    if (tok_spill_loaded[i].s == s)
    {
      TokSpillLoaded e = tok_spill_loaded[i];
      tok_spill_loaded[i] = tok_spill_loaded[--nb_tok_spill_loaded];
      tok_spill_drop_loaded(e.s, e.buf);
      return;
    }
}

/* S's read-back copy, which the macro stack replays: once every level reading
 * it sits on its final TOK_EOF (a function body past its closing brace), each
 * points at a static end instead and the copy goes until the next access.
 * Nothing changes while some level is still inside S. */
static void tok_str_release_replayed(TokenString *s)
{
  static const int at_end[1] = {TOK_EOF};
  if (!s || !s->spill_pos || s->allocated_len <= 0 || s->len <= 0)
    return;
  const uintptr_t b = (uintptr_t)s->data.str, last = b + (uintptr_t)(s->len - 1) * sizeof(int);
  if (s->data.str[s->len - 1] != TOK_EOF)
    return;
#define TOKSTR_INSIDE(p) ((uintptr_t)(p) >= b && (uintptr_t)(p) <= last + sizeof(int))
  if (TOKSTR_INSIDE(macro_ptr) && (uintptr_t)macro_ptr != last)
    return;
  for (TokenString *m = macro_stack; m; m = m->prev)
    if (TOKSTR_INSIDE(m->prev_ptr) && (uintptr_t)m->prev_ptr != last)
      return;
#undef TOKSTR_INSIDE
  if ((uintptr_t)macro_ptr == last)
    macro_ptr = at_end;
  for (TokenString *m = macro_stack; m; m = m->prev)
    if ((uintptr_t)m->prev_ptr == last)
      m->prev_ptr = at_end;
  tok_str_release(s);
}

/* The function being generated is parsed: the read-back copies its replay
 * holds -- the deferred body, and the body a compile replays (a borrowed
 * level's owner) -- are not read again before the backend is done. */
ST_FUNC void tok_str_release_parsed(void)
{
  tok_str_release_replayed(tcc_state->deferred_body);
  for (TokenString *m = macro_stack; m; m = m->prev)
    if (m->alloc == 2 && m->owner)
      tok_str_release_replayed(m->owner);
}

/* Free every read-back copy -- only where no token string is replayed, since
 * a replay (and the views inline expansion builds) reads the buffer itself. */
ST_FUNC void tok_str_release_spilled(void)
{
  if (macro_stack || !nb_tok_spill_loaded)
    return;
  for (int i = 0; i < nb_tok_spill_loaded; i++)
    tok_spill_drop_loaded(tok_spill_loaded[i].s, tok_spill_loaded[i].buf);
  nb_tok_spill_loaded = 0;
}

/* S is being freed: it leaves the list of copies read back. */
static void tok_str_spill_forget(TokenString *s)
{
  if (!s->spill_pos)
    return;
  for (int i = nb_tok_spill_loaded; i-- > 0;)
    if (tok_spill_loaded[i].s == s)
      tok_spill_loaded[i] = tok_spill_loaded[--nb_tok_spill_loaded];
  if (s->allocated_len < 0)
  {
    s->allocated_len = 0; /* nothing to free: the bytes are in the file */
    s->data.str = NULL;
  }
  s->spill_pos = 0;
}

/* End of the compilation: remove the spill file. */
ST_FUNC void tok_str_spill_close(void)
{
  if (tok_spill_fd >= 0)
  {
    close(tok_spill_fd);
    unlink(tok_spill_path);
  }
  tok_spill_fd = -1;
  tok_spill_off = 0;
  tcc_free(tok_spill_path);
  tok_spill_path = NULL;
  tcc_free(spill_wbuf);
  spill_wbuf = NULL;
  spill_wlen = 0;
  spill_flushed = 0;
  tcc_free(tok_spill_loaded);
  tok_spill_loaded = NULL;
  nb_tok_spill_loaded = cap_tok_spill_loaded = 0;
}

static int tok_str_word_count(const int *p)
{
  int t = *p;

  if (!TOK_HAS_VALUE(t))
    return 1;

  switch (t)
  {
#if LONG_SIZE == 4
  case TOK_CLONG:
#endif
  case TOK_CINT:
  case TOK_CCHAR:
  case TOK_LCHAR:
  case TOK_CINT_I:
  case TOK_LINENUM:
  case TOK_PACK_REPLAY:
#if LONG_SIZE == 4
  case TOK_CULONG:
#endif
  case TOK_CUINT:
  case TOK_CFLOAT:
  case TOK_CFLOAT_I:
    return 2;
  case TOK_STR:
  case TOK_LSTR:
  case TOK_PPNUM:
  case TOK_PPSTR:
    return 2 + (p[1] + sizeof(int) - 1) / sizeof(int);
  case TOK_CDOUBLE:
  case TOK_CDOUBLE_I:
  case TOK_CLLONG:
  case TOK_CULLONG:
#if LONG_SIZE == 8
  case TOK_CLONG:
  case TOK_CULONG:
#endif
    return 3;
  case TOK_CLDOUBLE:
  case TOK_CLDOUBLE_I:
#if LDOUBLE_SIZE == 8 || defined TCC_USING_DOUBLE_FOR_LDOUBLE
    return 3;
#elif LDOUBLE_SIZE == 12
    return 4;
#elif LDOUBLE_SIZE == 16
    return 5;
#else
#error add long double size support
#endif
  default:
    return 1;
  }
}

static void tok_str_add_words(TokenString *s, const int *src, int words)
{
  int len = s->len;
  int capacity = s->allocated_len > 0 ? s->allocated_len : TOKSTR_SMALL_BUFSIZE;
  int *dst = tok_str_buf(s);

  if (words <= 0)
    return;
  if (len + words > capacity)
    dst = tok_str_realloc(s, len + words);
  memcpy(dst + len, src, words * sizeof(int));
  s->len = len + words;
}

/* S's tokens from START on, with a yasc dialect marker before them (ON: 1
   yasc, 0 regular C), the token positions S keeps (xs) moved past it */
static void tok_str_insert_yasc_mark(TokenString *s, int start, int on)
{
  TokenString body;
  CValue cv;
  const int n = s->len - start;
  tok_str_new(&body);
  tok_str_add_words(&body, tok_str_buf(s) + start, n);
  s->len = start;
  cv.i = (TCC_PCH_REPLAY_YASC << 16) | on;
  tok_str_add2(s, TOK_PACK_REPLAY, &cv);
  if (s->xs)
    for (int i = 0; i < s->xs->n; i++)
      if (s->xs->b[i] >= start)
        s->xs->b[i] += s->len - start;
  tok_str_add_words(s, tok_str_buf(&body), n);
  if (body.allocated_len > 0)
    tok_str_free_str(body.data.str);
}

/* A saved function body that began in dialect ON, not its file's (decl.c
   save_function_body): its replay, which starts in the file's, says so first */
ST_FUNC void tok_str_lead_yasc(TokenString *s, int on)
{
  tok_str_insert_yasc_mark(s, 0, on);
}

static void tok_str_add_tokstream(TokenString *s, const int *src)
{
  const int *p = src;

  while (*p != TOK_EOF)
    p += tok_str_word_count(p);
  tok_str_add_words(s, src, p - src);
}

ST_FUNC void tok_str_add(TokenString *s, int t)
{
  int len, *str;

  len = s->len;
  str = tok_str_buf(s);
  if (len >= (s->allocated_len > 0 ? s->allocated_len : TOKSTR_SMALL_BUFSIZE))
    str = tok_str_realloc(s, len + 1);
  str[len++] = t;
  s->len = len;
}

/* A header replaying STR's first LEN tokens without copying them: begin_macro(.., 2)
 * frees the header and leaves the tokens to STR.  Compiling a saved body used to
 * copy it first (the largest body of a Zig kernel compile is 67 KB, held at its
 * peak as well as the original).  STR's buffer is read back now if it was
 * spilled, and stays until the next tok_str_release_spilled -- never inside a
 * function's compile. */
/* Is the innermost replay the compile of a function from its own saved tokens? */
ST_FUNC int tok_reading_compile_replay(void)
{
  return macro_stack && macro_stack == tcc_state->compile_replay;
}

ST_FUNC TokenString *tok_str_borrow(TokenString *str, int len)
{
  TokenString *b = tok_str_alloc();
  if (len > 0)
  {
    b->data.str = (int *)tok_str_buf(str);
    b->allocated_len = len;
    b->len = len;
  }
  b->src = str;
  return b;
}

ST_FUNC int pp_in_c(void)
{
  TCCState *s1 = tcc_state;
  return !s1->yasc && tcc_yasc_unit(s1);
}

ST_FUNC void begin_macro(TokenString *str, int alloc)
{
  str->alloc = alloc;
  str->prev = macro_stack;
  str->prev_ptr = macro_ptr;
  str->save_line_num = file->line_num;
  str->save_tok = tok;
  str->serial = ++tok_macro_serial;
  macro_ptr = tok_str_buf(str);
  macro_stack = str;
}

/* Replay STR, which may be a kept string (too short a header for the macro
 * stack's fields), and free it when the replay ends. */
ST_FUNC void begin_macro_kept(TokenString *str)
{
  TokenString *b = tok_str_borrow(str, str->len);
  b->owner = str;
  begin_macro(b, 3);
}

ST_FUNC void end_macro(void)
{
  TokenString *str = macro_stack;
  macro_stack = str->prev;
  macro_ptr = str->prev_ptr;
  file->line_num = str->save_line_num;
  if (str->alloc == 0)
  {
    /* matters if str not alloced, may be tokstr_buf */
    str->len = str->need_spc = 0;
    if (str->xs)
      str->xs->n = 0; /* tokstr_buf: kept for the next expansion */
    str->sys = 0;
  }
  else
  {
    TokenString *owner = str->alloc == 3 ? str->owner : NULL;
    if (str->alloc >= 2)
      str->data.str = NULL; /* don't free */
    tok_str_free(str);
    if (owner)
      tok_str_free(owner);
  }
}

/* Pop macro stack entries until 'target' is on top, then pop it too.
 * Used by try_inline_const_eval cleanup: speculative expression parsing
 * may push extra macro entries (e.g. unget_tok in decl_initializer_alloc),
 * so a single end_macro() isn't enough to unwind back to the expected state. */
ST_FUNC void end_macro_to(TokenString *target)
{
  while (macro_stack && macro_stack != target)
    end_macro();
  if (macro_stack == target)
    end_macro();
}

ST_FUNC void tok_str_add2(TokenString *s, int t, CValue *cv)
{
  int len, *str;
  int nb_words;
  int capacity;

  len = s->len;
  str = tok_str_buf(s);
  capacity = s->allocated_len > 0 ? s->allocated_len : TOKSTR_SMALL_BUFSIZE;

  if (!TOK_HAS_VALUE(t))
  {
    if (len >= capacity)
      str = tok_str_realloc(s, len + 1);
    str[len++] = t;
    s->len = len;
    return;
  }

  /* compute exact size needed based on token type */
  switch (t)
  {
  case TOK_CINT:
  case TOK_CUINT:
  case TOK_CCHAR:
  case TOK_LCHAR:
  case TOK_CFLOAT:
  case TOK_CFLOAT_I:
  case TOK_CINT_I:
  case TOK_LINENUM:
  case TOK_PACK_REPLAY:
#if LONG_SIZE == 4
  case TOK_CLONG:
  case TOK_CULONG:
#endif
    nb_words = 2;
    break;
  case TOK_CDOUBLE:
  case TOK_CDOUBLE_I:
  case TOK_CLLONG:
  case TOK_CULLONG:
#if LONG_SIZE == 8
  case TOK_CLONG:
  case TOK_CULONG:
#endif
    nb_words = 3;
    break;
  case TOK_CLDOUBLE:
  case TOK_CLDOUBLE_I:
#if LDOUBLE_SIZE == 8 || defined TCC_USING_DOUBLE_FOR_LDOUBLE
    nb_words = 3;
#elif LDOUBLE_SIZE == 12
    nb_words = 4;
#elif LDOUBLE_SIZE == 16
    nb_words = 5;
#else
#error add long double size support
#endif
    break;
  case TOK_PPNUM:
  case TOK_PPSTR:
  case TOK_STR:
  case TOK_LSTR:
    nb_words = 1 + (1 + (cv->str.size + sizeof(int) - 1) / sizeof(int));
    break;
  default:
    nb_words = 1;
    break;
  }

  if (len + nb_words > capacity)
    str = tok_str_realloc(s, len + nb_words);
  str[len++] = t;
  switch (t)
  {
  case TOK_CINT:
  case TOK_CUINT:
  case TOK_CCHAR:
  case TOK_LCHAR:
  case TOK_CFLOAT:
  case TOK_CFLOAT_I:
  case TOK_CINT_I:
  case TOK_LINENUM:
  case TOK_PACK_REPLAY:
#if LONG_SIZE == 4
  case TOK_CLONG:
  case TOK_CULONG:
#endif
    str[len++] = cv->tab[0];
    break;
  case TOK_PPNUM:
  case TOK_PPSTR:
  case TOK_STR:
  case TOK_LSTR:
  {
    /* Insert the string into the int array. */
    size_t str_words = 1 + (cv->str.size + sizeof(int) - 1) / sizeof(int);
    str[len] = cv->str.size;
    /* The last word's tail is padding: zero it, so equal strings compare
     * equal word by word (save_function_body) and the spill file is the
     * same from run to run. */
    str[len + str_words - 1] = 0;
    memcpy(&str[len + 1], cv->str.data, cv->str.size);
    len += str_words;
  }
  break;
  case TOK_CDOUBLE:
  case TOK_CDOUBLE_I:
  case TOK_CLLONG:
  case TOK_CULLONG:
#if LONG_SIZE == 8
  case TOK_CLONG:
  case TOK_CULONG:
#endif
    str[len++] = cv->tab[0];
    str[len++] = cv->tab[1];
    break;
  case TOK_CLDOUBLE:
  case TOK_CLDOUBLE_I:
#if LDOUBLE_SIZE == 8 || defined TCC_USING_DOUBLE_FOR_LDOUBLE
    /* When cross-compiling with LDOUBLE_SIZE == 8 (target long double is double)
     * but host long double is wider (e.g. 80-bit x87), we must convert to double
     * before saving, because cv->tab[0..1] only cover the first 8 bytes of the
     * host long double (the significand), losing the exponent. */
#if LDOUBLE_SIZE == 8 && !defined TCC_USING_DOUBLE_FOR_LDOUBLE && LDOUBLE_SIZE < 16
    if (sizeof(long double) > LDOUBLE_SIZE)
    {
      union
      {
        double d;
        int tab[2];
      } tmp;
      tmp.d = (double)cv->ld;
      str[len++] = tmp.tab[0];
      str[len++] = tmp.tab[1];
    }
    else
#endif
    {
      str[len++] = cv->tab[0];
      str[len++] = cv->tab[1];
    }
#elif LDOUBLE_SIZE == 12
    str[len++] = cv->tab[0];
    str[len++] = cv->tab[1];
    str[len++] = cv->tab[2];
#elif LDOUBLE_SIZE == 16
    str[len++] = cv->tab[0];
    str[len++] = cv->tab[1];
    str[len++] = cv->tab[2];
    str[len++] = cv->tab[3];
#else
#error add long double size support
#endif
    break;
  default:
    break;
  }
  s->len = len;
}

/* add the current parse token in token string 's' */
ST_FUNC void tok_str_add_tok(TokenString *s)
{
  CValue cval;

  /* save line number info */
  if (file->line_num != s->last_line_num)
  {
    s->last_line_num = file->line_num;
    cval.i = s->last_line_num;
    tok_str_add2(s, TOK_LINENUM, &cval);
  }
  tok_str_add2(s, tok, &tokc);
}

/* like tok_str_add2(), add a space if needed */
static void tok_str_add2_spc(TokenString *s, int t, CValue *cv)
{
  if (s->need_spc == 3 && !TOK_HAS_VALUE(t))
  {
    int len = s->len;
    int capacity = s->allocated_len > 0 ? s->allocated_len : TOKSTR_SMALL_BUFSIZE;
    int *str = tok_str_buf(s);

    if (len + 2 > capacity)
      str = tok_str_realloc(s, len + 2);
    str[len++] = ' ';
    str[len++] = t;
    s->len = len;
    s->need_spc = 2;
    return;
  }

  if (s->need_spc == 3)
    tok_str_add(s, ' ');
  s->need_spc = 2;
  tok_str_add2(s, t, cv);
}

/* get a token from an integer array and increment pointer. */
ST_FUNC HOT void tok_get(int *t, const int **pp, CValue *cv)
{
  const int *p = *pp;
  int n, *tab;

  tab = cv->tab;
  switch (*t = *p++)
  {
#if LONG_SIZE == 4
  case TOK_CLONG:
#endif
  case TOK_CINT:
  case TOK_CCHAR:
  case TOK_LCHAR:
  case TOK_CINT_I:
  case TOK_LINENUM:
  case TOK_PACK_REPLAY:
    cv->i = *p++;
    break;
#if LONG_SIZE == 4
  case TOK_CULONG:
#endif
  case TOK_CUINT:
    cv->i = (unsigned)*p++;
    break;
  case TOK_CFLOAT:
  case TOK_CFLOAT_I:
    tab[0] = *p++;
    break;
  case TOK_STR:
  case TOK_LSTR:
  case TOK_PPNUM:
  case TOK_PPSTR:
    cv->str.size = *p++;
    cv->str.data = (char *)p;
    p += (cv->str.size + sizeof(int) - 1) / sizeof(int);
    break;
  case TOK_CDOUBLE:
  case TOK_CDOUBLE_I:
  case TOK_CLLONG:
  case TOK_CULLONG:
#if LONG_SIZE == 8
  case TOK_CLONG:
  case TOK_CULONG:
#endif
    n = 2;
    goto copy;
  case TOK_CLDOUBLE:
  case TOK_CLDOUBLE_I:
#if LDOUBLE_SIZE == 8 || defined TCC_USING_DOUBLE_FOR_LDOUBLE
    /* Restore 2 words (double).  When the host long double is wider than
     * the target's (cross-compilation), the save side converted ld→double,
     * so we must convert back double→ld here. */
    *tab++ = *p++;
    *tab++ = *p++;
#if LDOUBLE_SIZE == 8 && !defined TCC_USING_DOUBLE_FOR_LDOUBLE && LDOUBLE_SIZE < 16
    if (sizeof(long double) > LDOUBLE_SIZE)
      cv->ld = (long double)cv->d;
#endif
    break;
#elif LDOUBLE_SIZE == 12
    n = 3;
    goto copy;
#elif LDOUBLE_SIZE == 16
    n = 4;
    goto copy;
#else
#error add long double size support
#endif
  copy:
    do
      *tab++ = *p++;
    while (--n);
    break;
  default:
    break;
  }
  *pp = p;
}

/* Apply a deferred #pragma pack action encoded in a TOK_PACK_REPLAY token when
   its saved token stream is replayed (see pp_pragma_capture / TOK_PACK_REPLAY).
   'code' is (kind<<16)|value, matching the TCC_PCH_REPLAY_PACK_* semantics. */
ST_FUNC void pp_apply_pack_replay(TCCState *s1, int code)
{
  int kind = (code >> 16) & 0xffff;
  int value = code & 0xffff;
  switch (kind)
  {
  case TCC_PCH_REPLAY_PACK_SET:
    *s1->pack_stack_ptr = value;
    break;
  case TCC_PCH_REPLAY_PACK_PUSH:
    if (s1->pack_stack_ptr >= s1->pack_stack + PACK_STACK_SIZE - 1)
      tcc_error("out of pack stack");
    *++s1->pack_stack_ptr = value;
    break;
  case TCC_PCH_REPLAY_PACK_PUSH_DUP:
    if (s1->pack_stack_ptr >= s1->pack_stack + PACK_STACK_SIZE - 1)
      tcc_error("out of pack stack");
    s1->pack_stack_ptr[1] = *s1->pack_stack_ptr;
    s1->pack_stack_ptr++;
    break;
  case TCC_PCH_REPLAY_PACK_POP:
    if (s1->pack_stack_ptr <= s1->pack_stack)
      tcc_error("out of pack stack");
    s1->pack_stack_ptr--;
    break;
  case TCC_PCH_REPLAY_YASC:
    s1->yasc = value & 1;
    break;
  case TCC_PCH_REPLAY_ERROR:
    tcc_error_stash_raise(s1, value);
    break;
  }
}

/* Apply every deferred #pragma pack action in the saved tokens [P, END) to the
   live pack state.  Not a c_begin / c_end (value bit 2): it took effect where it
   was read (pp_yasc_pragma), and the parse may be in another file by now. */
ST_FUNC void pp_apply_pack_replays(TCCState *s1, const int *p, const int *end)
{
  while (p < end)
  {
    int t;
    CValue cv;
    tok_get(&t, &p, &cv);
    if (t == TOK_PACK_REPLAY && (cv.i >> 16) != TCC_PCH_REPLAY_ERROR &&
        !((cv.i >> 16) == TCC_PCH_REPLAY_YASC && (cv.i & 2)))
      pp_apply_pack_replay(s1, cv.i);
  }
}

/* The #pragma pack state as {depth, stack[0..depth]}.  A saved function body
   replayed away from its definition (at a call site, or at the end of the TU)
   lays out its structs under the state it was defined in, not the state around
   the replay.  pp_pack_snapshot returns a heap copy, or NULL for the default
   state (the case for nearly every function); pp_pack_enter saves the live
   state into SAVED (PACK_STACK_SIZE + 1 ints) and installs SNAP, and
   pp_pack_leave puts SAVED back, so directives inside the body do not leak
   past the replay. */
static void pp_pack_install(TCCState *s1, const int *snap)
{
  if (!snap)
  {
    s1->pack_stack[0] = 0;
    s1->pack_stack_ptr = s1->pack_stack;
    return;
  }
  memcpy(s1->pack_stack, snap + 1, sizeof(int) * (snap[0] + 1));
  s1->pack_stack_ptr = s1->pack_stack + snap[0];
}

ST_FUNC int *pp_pack_snapshot(TCCState *s1)
{
  int depth = s1->pack_stack_ptr - s1->pack_stack;
  if (depth == 0 && s1->pack_stack[0] == 0)
    return NULL;
  int *snap = tcc_malloc(sizeof(int) * (depth + 2));
  snap[0] = depth;
  memcpy(snap + 1, s1->pack_stack, sizeof(int) * (depth + 1));
  return snap;
}

ST_FUNC void pp_pack_enter(TCCState *s1, const int *snap, int *saved)
{
  int depth = s1->pack_stack_ptr - s1->pack_stack;
  saved[0] = depth;
  memcpy(saved + 1, s1->pack_stack, sizeof(int) * (depth + 1));
  pp_pack_install(s1, snap);
}

ST_FUNC void pp_pack_leave(TCCState *s1, const int *saved)
{
  pp_pack_install(s1, saved);
}

#if 0
#define TOK_GET(t, p, c) tok_get(t, p, c)
#else
#define TOK_GET(t, p, c)                                                                                               \
  do                                                                                                                   \
  {                                                                                                                    \
    int _t = **(p);                                                                                                    \
    if (TOK_HAS_VALUE(_t))                                                                                             \
      tok_get(t, p, c);                                                                                                \
    else                                                                                                               \
      *(t) = _t, ++*(p);                                                                                               \
  } while (0)
#endif

static int macro_is_equal(const int *a, const int *b)
{
  CValue cv;
  int t;

  if (!a || !b)
    return 1;

  while (*a && *b)
  {
    cstr_reset(&tokcstr);
    TOK_GET(&t, &a, &cv);
    cstr_cat(&tokcstr, get_tok_str(t, &cv), 0);
    TOK_GET(&t, &b, &cv);
    if (strcmp(tokcstr.data, get_tok_str(t, &cv)))
      return 0;
  }
  return !(*a || *b);
}

/* defines handling */
/* The macro of each name that has one (TokenSym.has_define): open-addressed,
 * at most half full; an undefined name keeps its slot with a NULL macro. */
typedef struct DefineSlot
{
  int tok; /* 0 = empty */
  Sym *s;
} DefineSlot;
static DefineSlot *define_map;
static int define_map_nb, define_map_mask;

static Sym **define_slot(int tok, int create)
{
  unsigned h;
  if (!define_map)
  {
    if (!create)
      return NULL;
    define_map_mask = 63;
    define_map = tcc_mallocz((define_map_mask + 1) * sizeof *define_map);
  }
  for (h = (unsigned)tok * 2654435761u;; h++)
  {
    DefineSlot *e = &define_map[h & define_map_mask];
    if (e->tok == tok)
      return &e->s;
    if (e->tok)
      continue;
    if (!create)
      return NULL;
    if (2 * (define_map_nb + 1) > define_map_mask + 1)
    {
      DefineSlot *old = define_map;
      int n = define_map_mask + 1;
      define_map_mask = 2 * n - 1;
      define_map = tcc_mallocz(2 * n * sizeof *define_map);
      define_map_nb = 0;
      for (int i = 0; i < n; i++)
        if (old[i].tok)
          *define_slot(old[i].tok, 1) = old[i].s;
      tcc_free(old);
      return define_slot(tok, 1);
    }
    define_map_nb++;
    e->tok = tok;
    return &e->s;
  }
}

static void define_set(TokenSym *ts, Sym *s)
{
  Sym **slot = define_slot(ts->tok, s != NULL);
  if (slot)
    *slot = s;
  ts->has_define = s != NULL;
}

ST_INLN void define_push(int v, int macro_type, int *str, Sym *first_arg)
{
  Sym *s, *o;

  o = define_find(v);
  if (tcc_state->yasc)
    macro_type |= MACRO_YASC;
  {
    BufferedFile *f = file;
    while (f && f->filename[0] == ':' && f->prev)
      f = f->prev;
    if (f && f->sys)
      macro_type |= MACRO_SYS;
  }
  s = sym_push2_short(&define_stack, v, macro_type, 0);
  s->d = str;
  s->next = first_arg;
  /* v may be an as-yet-unmaterialized builtin (e.g. the startup defines for
     __LINE__ etc., or a #define of a builtin name) — ensure its slot. */
  define_set(tok_ensure(v), s);

  if (o && !macro_is_equal(o->d, s->d))
    tcc_warning("%s redefined", get_tok_str(v, NULL));
}

/* undefined a define symbol. Its name is just set to zero */
ST_FUNC void define_undef(Sym *s)
{
  int v = s->v;
  if (v >= TOK_IDENT && v < tok_ident)
  {
    TokenSym *ts = table_ident[v - TOK_IDENT];
    if (ts) /* lazy builtin never materialized => was never a macro */
      define_set(ts, NULL);
  }
}

ST_INLN Sym *define_find(int v)
{
  TokenSym *ts;
  v -= TOK_IDENT;
  if ((unsigned)v >= (unsigned)(tok_ident - TOK_IDENT))
  {
    return NULL;
  }
  ts = table_ident[v];
  if (!ts || !ts->has_define) /* NULL slot = lazy builtin, not a macro */
    return NULL;
  return *define_slot(ts->tok, 0);
}

static uint8_t *skip_logical_line(uint8_t *p)
{
  for (;;)
  {
    uint8_t *q = p + 1, *bs, *nl;
    size_t len = file->buf_end - q;
    int c;

    nl = memchr(q, '\n', len);
    /* Only this line's: see parse_comment. */
    bs = memchr(q, '\\', nl ? (size_t)(nl - q) : len);
    if (!bs)
    {
      if (nl)
        return nl;
      /* Neither found: the line continues past the I/O buffer.  Refill via
         the CH_EOB sentinel and keep scanning. */
      p = file->buf_end;
    }
    else
      p = bs;
    c = handle_bs(&p);
    if (c == CH_EOF || c == '\n')
      return p;
  }
}

/* free define stack until top reaches 'b' */
ST_FUNC void free_defines(Sym *b)
{
  while (define_stack != b)
  {
    Sym *top = define_stack;
    define_stack = top->prev;
    tok_str_free_str(top->d);
    define_undef(top);
    sym_free_short(top);
  }
}

/* fake the nth "#if defined test_..." for tcc -dt -run */
static void maybe_run_test(TCCState *s)
{
  const char *p;
  if (s->include_stack_ptr != s->include_stack)
    return;
  p = get_tok_str(tok, NULL);
  if (0 != memcmp(p, "test_", 5))
    return;
  if (0 != --s->run_test)
    return;
  fprintf(s->ppfp, &"\n[%s]\n"[!(s->dflag & 32)], p), fflush(s->ppfp);
  define_push(tok, MACRO_OBJ, NULL, NULL);
}

ST_FUNC void skip_to_eol(int warn)
{
  if (tok == TOK_LINEFEED)
    return;
  if (warn)
    tcc_warning("extra tokens after directive");
  file->buf_ptr = skip_logical_line(file->buf_ptr - 1);
  tok = TOK_LINEFEED;
}

static CachedInclude *search_cached_include(TCCState *s1, const char *filename, int add);

#ifndef _WIN32
/* What one include directory holds, read once on the first probe. A failed open
 * is the costly part of an include search on the device (a filesystem walk per
 * -I directory per header), and most probes are misses. Only names that may be
 * present get opened; a hash collision costs one open, as it did before. */
typedef struct IncDirList
{
  const char *path; /* the include_paths/sysinclude_paths entry; stable */
  int known;        /* 0: unreadable, so every name has to be tried */
  int nb;
  uint32_t hash[];
} IncDirList;

/* Folds case because FAT does: <STDIO.H> opens stdio.h there. */
static uint32_t inc_name_hash(const char *name, size_t len)
{
  uint32_t h = 2166136261u;
  for (size_t i = 0; i < len; i++)
  {
    unsigned char ch = name[i];
    if (ch >= 'A' && ch <= 'Z')
      ch += 'a' - 'A';
    h = (h ^ ch) * 16777619u;
  }
  return h;
}

static IncDirList *inc_dir_list(TCCState *s1, const char *path)
{
  for (int i = 0; i < s1->nb_inc_dir_lists; i++)
  {
    IncDirList *known = s1->inc_dir_lists[i];
    if (known->path == path)
      return known;
  }
  int cap = 32;
  IncDirList *list = tcc_malloc(sizeof(IncDirList) + cap * sizeof(uint32_t));
  list->path = path;
  list->nb = 0;
  DIR *dir = opendir(path);
  /* Nothing there holds nothing; a directory that will not open might. */
  list->known = dir != NULL || errno == ENOENT || errno == ENOTDIR;
  if (dir)
  {
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL)
    {
      if (list->nb == cap)
      {
        cap *= 2;
        list = tcc_realloc(list, sizeof(IncDirList) + cap * sizeof(uint32_t));
      }
      list->hash[list->nb++] = inc_name_hash(entry->d_name, strlen(entry->d_name));
    }
    closedir(dir);
  }
  dynarray_add(&s1->inc_dir_lists, &s1->nb_inc_dir_lists, list);
  return list;
}

/* May <dir>/<name> exist? For sys/types.h, that asks about sys. */
static int inc_dir_may_hold(TCCState *s1, const char *dir, const char *name)
{
  const char *slash = strchr(name, '/');
  size_t len = slash ? (size_t)(slash - name) : strlen(name);
  if (len == 0 || (name[0] == '.' && (len == 1 || (len == 2 && name[1] == '.'))))
    return 1;
  IncDirList *list = inc_dir_list(s1, dir);
  if (!list->known)
    return 1;
  uint32_t hash = inc_name_hash(name, len);
  for (int i = 0; i < list->nb; i++)
    if (list->hash[i] == hash)
      return 1;
  return 0;
}
#else
#define inc_dir_may_hold(s1, dir, name) 1
#endif

/* The header name of #include, #embed, __has_include or __has_embed: "name",
 * <name>, or tokens that expand to one.  Returns the opening delimiter, with
 * tok the token after the name. */
static int pp_header_name(char *name, int size, const char *directive)
{
  int c, i;
  char *p;

  c = skip_spaces();
  if (c == '<' || c == '\"')
  {
    cstr_reset(&tokcstr);
    file->buf_ptr = parse_pp_string(file->buf_ptr, c == '<' ? '>' : c, &tokcstr);
    i = tokcstr.size;
    pstrncpy(name, tokcstr.data, i >= size ? size - 1 : i);
    next_nomacro();
  }
  else
  {
    /* computed #include : concatenate tokens until result is one of
       the two accepted forms.  Don't convert pp-tokens to tokens here,
       but do again after the name: `__has_include(H) && 1` and #embed
       limit(2) read numbers. */
    int saved_parse_flags = parse_flags;
    parse_flags = PARSE_FLAG_PREPROCESS | PARSE_FLAG_LINEFEED | (parse_flags & PARSE_FLAG_ASM_FILE);
    name[0] = 0;
    for (;;)
    {
      next();
      p = name, i = strlen(p) - 1;
      if (i > 0 && ((p[0] == '"' && p[i] == '"') || (p[0] == '<' && p[i] == '>')))
        break;
      if (tok == TOK_LINEFEED)
        tcc_error("'#%s' expects \"FILENAME\" or <FILENAME>", directive);
      pstrcat(name, size, get_tok_str(tok, &tokc));
    }
    c = p[0];
    /* remove '<>|""' */
    memmove(p, p + 1, i - 1), p[i - 1] = 0;
    parse_flags = saved_parse_flags;
  }
  return c;
}

/* yasc: a module's interface names the headers its main file included, for
   importers to include before its declarations (decl/module.c).  <h> as
   written, so an importer searches its own paths; "h" as the file found. */
static void pp_note_main_include(TCCState *s1, int c, const char *name, const char *path)
{
  char *note;
  if (c == '<')
  {
    note = tcc_malloc(strlen(name) + 3);
    sprintf(note, "<%s>", name);
  }
  else
  {
    char *real = realpath(path, NULL);
    note = tcc_strdup(real ? real : path);
    if (real)
      libc_free(real);
  }
  dynarray_add(&s1->yasc_includes, &s1->nb_yasc_includes, note);
}

static void pp_yasc_file(TCCState *s1);

/* BF was found in an -iyasc directory, or next to a header that was */
static int pp_in_yasc_dir(TCCState *s1, BufferedFile *bf)
{
  int i = 0;
  while (bf && (i = bf->include_next_index) == 1)
    bf = bf->prev;
  return bf && i >= 2 && i - 2 < s1->nb_include_paths && tcc_yasc_include_dir(s1, s1->include_paths[i - 2]);
}

static int parse_include(TCCState *s1, int do_next, int test)
{
  int c, i;
  char name[1024], buf[1024], *p;
  CachedInclude *e;
  int note_main;

  c = pp_header_name(name, sizeof name, "include");
  note_main = !test && s1->yasc && s1->include_stack_ptr == s1->include_stack;

  if (!test)
    skip_to_eol(1);

  const int first = do_next ? file->include_next_index : -1;
  int skipped = 0, filter = 1;
  i = first;
  for (;;)
  {
    ++i;
    if (i == 0)
    {
      /* check absolute include path */
      if (!IS_ABSPATH(name))
        continue;
      buf[0] = '\0';
    }
    else if (i == 1)
    {
      /* search in file's dir if "header.h" */
      if (c != '\"')
        continue;
      p = file->true_filename;
      pstrncpy(buf, p, tcc_basename(p) - p);
    }
    else
    {
      int j = i - 2, k = j - s1->nb_include_paths;
      if (k < 0)
        p = s1->include_paths[j];
      else if (k < s1->nb_sysinclude_paths)
        p = s1->sysinclude_paths[k];
      else if (skipped && filter)
      {
        /* A listing can miss what an open would find (a mount on a name its
         * directory does not hold): before giving up, ask every directory. */
        filter = 0;
        i = first;
        continue;
      }
      else if (test)
        return 0;
      else
        tcc_error("include file '%s' not found", name);
      if (filter && !inc_dir_may_hold(s1, p, name))
      {
        skipped = 1;
        continue;
      }
      pstrcpy(buf, sizeof buf, p);
      pstrcat(buf, sizeof buf, "/");
    }
    pstrcat(buf, sizeof buf, name);
    e = search_cached_include(s1, buf, 0);
    if (e && (define_find(e->ifndef_macro) || e->once))
    {
      /* no need to parse the include because the 'ifndef macro'
         is defined (or had #pragma once) */
#ifdef INC_DEBUG
      printf("%s: skipping cached %s\n", file->filename, buf);
#endif
      if (note_main)
        pp_note_main_include(s1, c, name, buf);
      return 1;
    }
    if (tcc_open(s1, buf) >= 0)
      break;
  }
  if (note_main)
    pp_note_main_include(s1, c, name, buf);

  if (test)
  {
    tcc_close();
  }
  else
  {
    if (s1->include_stack_ptr >= s1->include_stack + INCLUDE_STACK_SIZE)
      tcc_error("#include recursion too deep");
    /* push previous file on stack */
    *s1->include_stack_ptr++ = file->prev;
    file->include_next_index = i;
    /* a system header: from a system include path, or "next to" one */
    file->sys = i == 1 ? file->prev->sys : i >= 2 && i - 2 >= s1->nb_include_paths;
    if (file->sys)
      tcc_sys_mark_file(s1, file->filename, 0);
    /* a yasc header: from an -iyasc directory, or "next to" one */
    if (s1->nb_yasc_include_paths && pp_in_yasc_dir(s1, file))
    {
      file->yasc_dir = 1; /* tcc -E writes the #pragma yasc it stands for */
      pp_yasc_file(s1);
    }
#ifdef INC_DEBUG
    printf("%s: including %s\n", file->prev->filename, file->filename);
#endif
    /* update target deps */
    if (s1->gen_deps)
    {
      BufferedFile *bf = file;
      while (i == 1 && (bf = bf->prev))
        i = bf->include_next_index;
      /* skip system include files */
      if (s1->include_sys_deps || i - 2 < s1->nb_include_paths)
        dynarray_add(&s1->target_deps, &s1->nb_target_deps, tcc_strdup(buf));
    }
    /* add include file debug info */
    tcc_debug_bincl(s1);
  }
  return 1;
}

/* C23 #embed: the resource's bytes as a comma-separated list of int
 * constants.  The list is pushed as a token string; preprocess() runs inside
 * the file lexer, which then returns TOK_EMBED_RESUME so next() reads it. */
static int pp_embed_pending;

static int64_t expr_preprocess(TCCState *s1, int close_paren);

typedef struct EmbedParams
{
  int64_t limit; /* -1: the whole resource */
  TokenString *prefix, *suffix, *if_empty;
  int unsupported; /* a parameter __has_embed answers 0 for */
} EmbedParams;

/* limit / prefix / suffix / if_empty, each also as __name__.  Their
 * names are not macro-expanded, their arguments are. */
static TokenString **embed_param_slot(EmbedParams *ep, const char *name, int *is_limit)
{
  char bare[32];
  int len = strlen(name);

  if (len > 4 && len - 4 < (int)sizeof bare && !strncmp(name, "__", 2) && !strcmp(name + len - 2, "__"))
  {
    memcpy(bare, name + 2, len - 4);
    bare[len - 4] = 0;
    name = bare;
  }
  *is_limit = !strcmp(name, "limit");
  if (!strcmp(name, "prefix"))
    return &ep->prefix;
  if (!strcmp(name, "suffix"))
    return &ep->suffix;
  if (!strcmp(name, "if_empty"))
    return &ep->if_empty;
  return NULL;
}

/* The parameters up to END: TOK_LINEFEED for #embed, ')' for __has_embed. */
static void embed_params(TCCState *s1, EmbedParams *ep, int end)
{
  memset(ep, 0, sizeof *ep);
  ep->limit = -1;
  while (tok != end)
  {
    char name[64];
    int is_limit = 0, vendor = 0;
    TokenString **slot;

    if (tok == TOK_LINEFEED || tok == TOK_EOF)
      tcc_error("missing ')' after __has_embed");
    if (tok < TOK_IDENT)
      tcc_error("#embed parameter expected, got '%s'", get_tok_str(tok, &tokc));
    pstrcpy(name, sizeof name, get_tok_str(tok, NULL));
    next_nomacro();
    if (tok == ':')
    {
      /* vendor::name: none is supported */
      next_nomacro();
      if (tok != ':')
        expect("'::' in #embed parameter");
      next_nomacro();
      if (tok < TOK_IDENT)
        expect("#embed parameter name");
      pstrcat(name, sizeof name, "::");
      pstrcat(name, sizeof name, get_tok_str(tok, NULL));
      next_nomacro();
      vendor = 1;
    }
    slot = vendor ? NULL : embed_param_slot(ep, name, &is_limit);
    if (!slot && !is_limit)
    {
      if (end == TOK_LINEFEED)
        tcc_error("unsupported #embed parameter '%s'", name);
      ep->unsupported = 1;
    }
    if (tok != '(')
    {
      if (slot || is_limit)
        tcc_error("'(' expected after #embed parameter '%s'", name);
      continue;
    }
    if (is_limit)
    {
      ep->limit = expr_preprocess(s1, 1);
      if (ep->limit < 0)
        tcc_error("negative #embed limit");
      next_nomacro();
      continue;
    }
    /* a balanced token sequence, kept for prefix / suffix / if_empty */
    TokenString *str = slot ? tok_str_alloc() : NULL;
    int depth = 0;
    for (;;)
    {
      next();
      if (tok == TOK_LINEFEED || tok == TOK_EOF)
        tcc_error("missing ')' in #embed parameter '%s'", name);
      if (tok == '(')
        depth++;
      else if (tok == ')' && depth-- == 0)
        break;
      if (str)
        tok_str_add2(str, tok, &tokc);
    }
    if (slot)
    {
      if (*slot)
        tok_str_free(*slot);
      *slot = str;
    }
    next_nomacro();
  }
}

static void embed_params_free(EmbedParams *ep)
{
  if (ep->prefix)
    tok_str_free(ep->prefix);
  if (ep->suffix)
    tok_str_free(ep->suffix);
  if (ep->if_empty)
    tok_str_free(ep->if_empty);
}

/* Found as #include finds a header: an absolute name as given, "name" beside
 * the including file, then the -I and system directories. */
static int embed_open(TCCState *s1, const char *name, int delim, char *path, int size)
{
  for (int i = 0;; i++)
  {
    if (i == 0)
    {
      if (!IS_ABSPATH(name))
        continue;
      path[0] = 0;
    }
    else if (IS_ABSPATH(name))
    {
      return -1;
    }
    else if (i == 1)
    {
      if (delim != '\"')
        continue;
      const char *p = file->true_filename;
      pstrncpy(path, p, tcc_basename(p) - p);
    }
    else
    {
      int j = i - 2, k = j - s1->nb_include_paths;
      if (k < 0)
        pstrcpy(path, size, s1->include_paths[j]);
      else if (k < s1->nb_sysinclude_paths)
        pstrcpy(path, size, s1->sysinclude_paths[k]);
      else
        return -1;
      pstrcat(path, size, "/");
    }
    pstrcat(path, size, name);
    int fd = open(path, O_RDONLY | O_BINARY);
    if (fd >= 0)
      return fd;
  }
}

static void embed_add_tokens(TokenString *str, TokenString *src)
{
  if (src)
    tok_str_add_words(str, tok_str_buf(src), src->len);
}

static void parse_embed(TCCState *s1)
{
  char name[1024], path[1024];
  unsigned char buf[512];
  EmbedParams ep;
  CValue cv;
  int delim, fd, n, count = 0;

  delim = pp_header_name(name, sizeof name, "embed");
  embed_params(s1, &ep, TOK_LINEFEED);
  fd = embed_open(s1, name, delim, path, sizeof path);
  if (fd < 0)
    tcc_error("embed file '%s' not found", name);
  if (s1->gen_deps)
    dynarray_add(&s1->target_deps, &s1->nb_target_deps, tcc_strdup(path));

  TokenString *str = tok_str_alloc();
  int64_t left = ep.limit;
  memset(&cv, 0, sizeof cv);
  while (left != 0)
  {
    int want = left > 0 && left < (int64_t)sizeof buf ? (int)left : (int)sizeof buf;
    n = read(fd, buf, want);
    if (n < 0)
    {
      close(fd);
      tcc_error("cannot read embed file '%s'", path);
    }
    if (n == 0)
      break;
    if (left > 0)
      left -= n;
    for (int i = 0; i < n; i++, count++)
    {
      if (count == 0)
        embed_add_tokens(str, ep.prefix);
      else
        tok_str_add(str, ',');
      cv.i = buf[i];
      tok_str_add2(str, TOK_CINT, &cv);
    }
  }
  close(fd);
  embed_add_tokens(str, count ? ep.suffix : ep.if_empty);
  embed_params_free(&ep);
  tok_str_add(str, 0);
  begin_macro(str, 1);
  pp_embed_pending = 1;
}

/* __has_embed( header-name params ): tok is '(' on entry, ')' on return. */
static int has_embed(TCCState *s1)
{
  char name[1024], path[1024];
  unsigned char byte;
  EmbedParams ep;
  int delim, fd, n;

  delim = pp_header_name(name, sizeof name, "__has_embed");
  embed_params(s1, &ep, ')');
  embed_params_free(&ep);
  if (ep.unsupported)
    return 0;
  fd = embed_open(s1, name, delim, path, sizeof path);
  if (fd < 0)
    return 0;
  n = ep.limit == 0 ? 0 : read(fd, &byte, 1);
  close(fd);
  return n > 0 ? 1 : 2;
}

static int pp_assertion_macro_defined(const char *name)
{
  int tok;
  char buf[256];
  int len;

  len = strlen(name);
  tok = tok_alloc(name, len)->tok;
  if (define_find(tok))
    return 1;

  if (len + 4 >= sizeof(buf))
    return 0;

  buf[0] = '_';
  buf[1] = '_';
  memcpy(buf + 2, name, len);
  memcpy(buf + 2 + len, "__", 3);
  tok = tok_alloc(buf, len + 4)->tok;
  if (define_find(tok))
    return 1;

  buf[2 + len] = '\0';
  tok = tok_alloc(buf, len + 2)->tok;
  return define_find(tok) != NULL;
}

static int pp_assertion_value(int kind_tok, int value_tok)
{
  const char *kind;
  const char *value;

  if (kind_tok < TOK_IDENT || value_tok < TOK_IDENT)
    return 0;

  kind = tok_name(table_ident[kind_tok - TOK_IDENT]);
  value = tok_name(table_ident[value_tok - TOK_IDENT]);

  if (!strcmp(kind, "cpu") || !strcmp(kind, "machine") || !strcmp(kind, "system"))
    return pp_assertion_macro_defined(value);

  return 0;
}

static void pp_parse_assertion(void)
{
  int kind_tok, value_tok;

  next();
  kind_tok = tok;
  if (kind_tok < TOK_IDENT)
    expect("identifier after '#'");

  next();
  if (tok != '(')
    expect("'(' after preprocessor assertion");

  next();
  value_tok = tok;
  if (value_tok < TOK_IDENT)
    expect("identifier in preprocessor assertion");

  next();
  if (tok != ')')
    expect("')'");

  tok = TOK_CINT;
  tokc.i = pp_assertion_value(kind_tok, value_tok);
}

/* The next preprocessor-expression token, never macro-expanded: from the
 * current substitution result when there is one, else from the file. */
static void pp_next_nosubst(void)
{
  if (macro_ptr)
    next();
  else
    next_nomacro();
}

/* eval an expression for #if/#elif (its truth), or with CLOSE_PAREN the
   value of #embed limit( ... ) up to its ')' */
static int64_t expr_preprocess(TCCState *s1, int close_paren)
{
  int t;
  int64_t c;
  int t0 = close_paren ? TOK_EMBED : tok;
  /* only a limit() inside an #if's __has_embed restores the outer pp_expr;
     an #if ends with it clear, as an error leaves it set */
  int depth = 0, saved_pp_expr = close_paren ? pp_expr : 0;
  TokenString *str;

  str = tok_str_alloc();
  pp_expr = 1;
  while (1)
  {
    next(); /* do macro subst */
    t = tok;
    if (tok == '#')
    {
      pp_parse_assertion();
    }
    else if (tok < TOK_IDENT)
    {
      if (tok == TOK_LINEFEED || tok == TOK_EOF)
      {
        if (close_paren)
          tcc_error("missing ')' in #embed limit");
        break;
      }
      if (close_paren && tok == '(')
        depth++;
      else if (close_paren && tok == ')' && depth-- == 0)
        break;
      if (tok >= TOK_STR && tok <= TOK_CLDOUBLE)
        tcc_error("invalid constant in preprocessor expression");
    }
    else if (tok == TOK_DEFINED)
    {
      next_nomacro();
      t = tok;
      if (t == '(')
        next_nomacro();
      if (tok < TOK_IDENT)
        expect("identifier after 'defined'");
      if (s1->run_test)
        maybe_run_test(s1);
      c = 0;
      if (define_find(tok) || tok == TOK___HAS_INCLUDE || tok == TOK___HAS_INCLUDE_NEXT || tok == TOK___HAS_EMBED ||
          tok == TOK___HAS_ATTRIBUTE)
        c = 1;
      if (t == '(')
      {
        next_nomacro();
        if (tok != ')')
          expect("')'");
      }
      tok = TOK_CINT;
      tokc.i = c;
    }
    else if (tok == TOK___HAS_ATTRIBUTE)
    {
      /* Usually reached through a macro (zig.h's zig_has_attribute), so the
       * operand is still in the substituted stream, which only next() reads;
       * the operand was kept unexpanded there (see macro_subst). Read straight
       * from the file, next() would expand it, so use the raw lexer. */
      pp_next_nosubst();
      if (tok != '(')
        expect("'(' after __has_attribute");
      pp_next_nosubst();
      if (tok < TOK_IDENT)
        expect("attribute name in __has_attribute");
      c = tcc_attribute_supported(tok);
      pp_next_nosubst();
      if (tok != ')')
        expect("')'");
      tok = TOK_CINT;
      tokc.i = c;
    }
    else if (tok == TOK___HAS_INCLUDE || tok == TOK___HAS_INCLUDE_NEXT)
    {
      t = tok;
      next();
      if (tok != '(')
        expect("'('");
      c = parse_include(s1, t - TOK___HAS_INCLUDE, 1);
      if (tok != ')')
        expect("')'");
      tok = TOK_CINT;
      tokc.i = c;
    }
    else if (tok == TOK___HAS_EMBED)
    {
      next();
      if (tok != '(')
        expect("'('");
      c = has_embed(s1);
      tok = TOK_CINT;
      tokc.i = c;
    }
    else
    {
      /* if undefined macro, replace with zero */
      tok = TOK_CINT;
      tokc.i = 0;
    }
    tok_str_add_tok(str);
  }
  if (0 == str->len)
    tcc_error(close_paren ? "#embed limit with no expression" : "#%s with no expression", get_tok_str(t0, 0));
  tok_str_add(str, TOK_EOF); /* simulate end of file */
  pp_expr = t0;              /* redirect pre-processor expression error messages */
  t = tok;
  /* now evaluate C constant expression */
  begin_macro(str, 1);
  next();
  c = expr_const64();
  if (tok != TOK_EOF)
    tcc_error("...");
  pp_expr = saved_pp_expr;
  end_macro();
  tok = t; /* restore LF, EOF or the limit's ')' */
  return close_paren ? c : c != 0;
}

/* __has_builtin: builtins parse_builtin and friends implement.  A "no" sends a
 * header to its portable C fallback: zig.h built its byte swaps from shifts
 * and ORs (five instructions a u16) while REV16 was there.
 *
 * The __builtin_*_overflow family is implemented but answered "no": zig.h's
 * builtin branch takes the result's address (`&full_res`, which then lives
 * in memory) and ORs the flag with range checks, and that costs more than its
 * fallback's `*res < lhs` -- zig.c -Os +7 KB.  Answer "yes" once both of
 * those compile as well as the fallback does. */
static const char *TCC_RODATA_REL const tcc_builtins_supported[] = {
    "__builtin_bswap16", "__builtin_bswap32", "__builtin_bswap64",
    "__builtin_clz", "__builtin_clzl", "__builtin_clzll", "__builtin_ctz", "__builtin_ctzl", "__builtin_ctzll",
    "__builtin_popcount", "__builtin_popcountl", "__builtin_popcountll",
    "__builtin_unreachable", "__builtin_trap", "__builtin_expect", "__builtin_frame_address",
    "__builtin_return_address", "__builtin_prefetch", "__builtin_nan", "__builtin_inf",
    "__builtin_constant_p", "__builtin_types_compatible_p", "__builtin_choose_expr",
    "__builtin_memcpy", "__builtin_memmove", "__builtin_memset", "__builtin_memcmp", "__builtin_strlen",
    "__builtin_abs", "__builtin_labs", "__builtin_llabs", "__builtin_alloca", "__builtin_offsetof",
    "__builtin_va_start", "__builtin_va_end", "__builtin_va_arg", "__builtin_va_copy",
    "__builtin_object_size", "__builtin_dynamic_object_size",
    /* _FORTIFY_SOURCE headers ask for these (builtin/chk.c) */
    "__builtin___memcpy_chk", "__builtin___memmove_chk", "__builtin___memset_chk", "__builtin___mempcpy_chk",
    "__builtin___strcpy_chk", "__builtin___stpcpy_chk", "__builtin___strcat_chk", "__builtin___strncpy_chk",
    "__builtin___stpncpy_chk", "__builtin___strncat_chk", "__builtin___sprintf_chk", "__builtin___snprintf_chk",
    "__builtin___vsprintf_chk", "__builtin___vsnprintf_chk",
    NULL};

ST_FUNC int tcc_builtin_supported(int t)
{
  const char *name = get_tok_str(t, NULL);
  for (int i = 0; tcc_builtins_supported[i]; i++)
    if (!strcmp(name, tcc_builtins_supported[i]))
      return 1;
  return 0;
}

ST_FUNC void pp_error(CString *cs)
{
  cstr_printf(cs, "bad preprocessor expression: #%s", get_tok_str(pp_expr, 0));
  macro_ptr = tok_str_buf(macro_stack);
  while (next(), tok != TOK_EOF)
    cstr_printf(cs, " %s", get_tok_str(tok, &tokc));
}

/* parse after #define */
ST_FUNC void parse_define(void)
{
  Sym *s, *first, **ps;
  int v, t, varg, is_vaargs, t0;
  int saved_parse_flags = parse_flags;
  TokenString str;

  v = tok;
  if (v < TOK_IDENT || v == TOK_DEFINED)
    tcc_error("invalid macro name '%s'", get_tok_str(tok, &tokc));
  first = NULL;
  t = MACRO_OBJ;
  /* We have to parse the whole define as if not in asm mode, in particular
     no line comment with '#' must be ignored.  Also for function
     macros the argument list must be parsed without '.' being an ID
     character.  */
  parse_flags = ((parse_flags & ~PARSE_FLAG_ASM_FILE) | PARSE_FLAG_SPACES);
  /* '(' must be just after macro definition for MACRO_FUNC */
  next_nomacro();
  parse_flags &= ~PARSE_FLAG_SPACES;
  is_vaargs = 0;
  if (tok == '(')
  {
    int dotid = set_idnum('.', 0);
    next_nomacro();
    ps = &first;
    if (tok != ')')
      for (;;)
      {
        varg = tok;
        next_nomacro();
        is_vaargs = 0;
        if (varg == TOK_DOTS)
        {
          varg = TOK___VA_ARGS__;
          is_vaargs = 1;
        }
        else if (tok == TOK_DOTS && gnu_ext)
        {
          is_vaargs = 1;
          next_nomacro();
        }
        if (varg < TOK_IDENT)
        bad_list:
          tcc_error("bad macro parameter list");
        s = sym_push2_short(&define_stack, varg | SYM_FIELD, is_vaargs, 0);
        *ps = s;
        ps = &s->next;
        if (tok == ')')
          break;
        if (tok != ',' || is_vaargs)
          goto bad_list;
        next_nomacro();
      }
    parse_flags |= PARSE_FLAG_SPACES;
    next_nomacro();
    t = MACRO_FUNC;
    set_idnum('.', dotid);
  }

  /* The body of a macro definition should be parsed such that identifiers
     are parsed like the file mode determines (i.e. with '.' being an
     ID character in asm mode).  But '#' should be retained instead of
     regarded as line comment leader, so still don't set ASM_FILE
     in parse_flags. */
  parse_flags |= PARSE_FLAG_ACCEPT_STRAYS | PARSE_FLAG_SPACES | PARSE_FLAG_LINEFEED;
  tok_str_new(&str);
  t0 = 0;
  while (tok != TOK_LINEFEED && tok != TOK_EOF)
  {
    if (is_space(tok))
    {
      str.need_spc |= 1;
    }
    else
    {
      if (TOK_TWOSHARPS == tok)
      {
        if (0 == t0)
          goto bad_twosharp;
        tok = TOK_PPJOIN;
        t |= MACRO_JOIN;
      }
      tok_str_add2_spc(&str, tok, &tokc);
      t0 = tok;
    }
    next_nomacro();
  }
  parse_flags = saved_parse_flags;
  tok_str_add(&str, 0);
  tok_str_fit(&str);
  if (t0 == TOK_PPJOIN)
  bad_twosharp:
    tcc_error("'##' cannot appear at either end of macro");
  define_push(v, t, tok_str_ensure_heap(&str), first);
  // tok_print(str.str, "#define (%d) %s %d:", t | is_vaargs * 4, get_tok_str(v,
  // 0));
}

static CachedInclude *search_cached_include(TCCState *s1, const char *filename, int add)
{
  const char *s, *basename;
  unsigned int h;
  CachedInclude *e;
  int c, i, len;

  s = basename = tcc_basename(filename);
  h = TOK_HASH_INIT;
  while ((c = (unsigned char)*s) != 0)
  {
#ifdef _WIN32
    h = TOK_HASH_FUNC(h, toup(c));
#else
    h = TOK_HASH_FUNC(h, c);
#endif
    s++;
  }
  h &= (CACHED_INCLUDES_HASH_SIZE - 1);

  i = s1->cached_includes_hash[h];
  for (;;)
  {
    if (i == 0)
      break;
    e = s1->cached_includes[i - 1];
    if (0 == PATHCMP(filename, e->filename))
      return e;
    if (e->once && 0 == PATHCMP(basename, tcc_basename(e->filename)) && 0 == normalized_PATHCMP(filename, e->filename))
      return e;
    i = e->hash_next;
  }
  if (!add)
    return NULL;

  e = tcc_malloc(sizeof(CachedInclude) + (len = strlen(filename)));
  memcpy(e->filename, filename, len + 1);
  e->ifndef_macro = e->once = 0;
  dynarray_add(&s1->cached_includes, &s1->nb_cached_includes, e);
  /* add in hash table */
  e->hash_next = s1->cached_includes_hash[h];
  s1->cached_includes_hash[h] = s1->nb_cached_includes;
#ifdef INC_DEBUG
  printf("adding cached '%s'\n", filename);
#endif
  return e;
}

/* #pragma ... diagnostic, after the word `diagnostic` */
static void pragma_diagnostic(TCCState *s1)
{
  static const char *const actions[] = {"push", "pop", "ignored", "warning", "error"};
  BufferedFile *f = file;
  const char *name, *option = NULL;
  int action = -1;
  /* a _Pragma's text is read from a ":pragma:" buffer: its place is the file's */
  while (f && f->filename[0] == ':' && f->prev)
    f = f->prev;
  next();
  name = get_tok_str(tok, &tokc);
  for (int i = 0; i < 5; i++)
    if (!strcmp(name, actions[i]))
      action = i;
  if (action < 0)
  {
    tcc_warning_c(warn_unsupported)("#pragma diagnostic %s ignored", name);
    return;
  }
  if (action >= DIAG_PRAGMA_IGNORED)
  {
    next();
    if (tok != TOK_STR)
    {
      tcc_warning_c(warn_unsupported)("#pragma diagnostic %s: a \"-W...\" string expected", name);
      return;
    }
    option = tokc.str.data;
  }
  tcc_pragma_diagnostic(s1, action, option, f->filename, f->line_num);
}

/* #pragma yasc, or a header found through -iyasc: this file is yasc from
   here on (and in every replay of a body saved from it); the files it
   includes stay what they are */
static void pp_yasc_file(TCCState *s1)
{
  if (!file->yasc)
  {
    file->yasc = 1;
    tcc_yasc_mark_file(s1, file->filename);
  }
  s1->yasc = 1;
  if (!define_find(tok_alloc("__YASC__", 8)->tok))
  {
    TokenString str;
    CValue cv;
    tok_str_new(&str);
    memset(&cv, 0, sizeof cv);
    cv.i = 1;
    tok_str_add2(&str, TOK_CINT, &cv);
    tok_str_add(&str, 0);
    tok_str_fit(&str);
    define_push(tok_alloc("__YASC__", 8)->tok, MACRO_OBJ, tok_str_ensure_heap(&str), NULL);
  }
}

/* #pragma GCC system_header: the rest of this file is a system header, as if
   found on a system include path: its warnings are not given, and the "..."
   includes next to it are system headers too */
static void pp_system_header(TCCState *s1)
{
  BufferedFile *f = file;
  while (f->filename[0] == ':' && f->prev) /* _Pragma */
    f = f->prev;
  if (s1->include_stack_ptr == s1->include_stack)
    tcc_warning_c(warn_all)("#pragma GCC system_header ignored outside include file");
  else
  {
    f->sys = 1;
    tcc_sys_mark_file(s1, f->filename, f->line_num);
  }
}

/* tcc -E, the pragma's first word read: #pragma GCC system_header is done
   here (the other pragmas are kept for the compile of the output).  Else the
   tokens are as they were. */
static int pp_system_header_pp(TCCState *s1)
{
  int t = tok;
  if (t < TOK_IDENT || strcmp(get_tok_str(t, NULL), "GCC"))
    return 0;
  next_nomacro();
  if (tok >= TOK_IDENT && !strcmp(get_tok_str(tok, NULL), "system_header"))
  {
    pp_system_header(s1);
    return 1;
  }
  unget_tok(t);
  return 0;
}

/* tcc -E in a yasc unit: a regular-C macro's expansion is regular C (the
   TOK_PACK_REPLAY "yasc off" / "yasc on" markers around it, macro_subst_tok).
   The output says so with `_Pragma("tcc c_begin")` before it and
   `_Pragma("tcc c_end")` after it, inline so the lines stay; compiling the
   output, the two do what the markers do (pragma_parse: also kept in a saved
   body as the same markers), so the output compiles as the source did.  The
   markers met by next() wait here for the output loop (tcc_preprocess). */
static unsigned char pp_yasc_marks[16];
static int pp_yasc_nmarks;
static int pp_yasc_pragma_set; /* through _Pragma: the dialect it set (handle_pragma_operator) */

static void pp_yasc_mark(int on)
{
  if (pp_yasc_nmarks < (int)sizeof pp_yasc_marks)
    pp_yasc_marks[pp_yasc_nmarks++] = on;
}

/* the markers waiting, written now (LEAD: a space first, after a token) */
static void pp_yasc_marks_flush(TCCState *s1, int lead)
{
  for (int i = 0; i < pp_yasc_nmarks; i++)
  {
    fputs(lead ? " " : "", s1->ppfp);
    fputs(pp_yasc_marks[i] ? "_Pragma(\"tcc c_end\")" : "_Pragma(\"tcc c_begin\")", s1->ppfp);
    fputs(lead ? "" : " ", s1->ppfp);
  }
  pp_yasc_nmarks = 0;
}

/* #pragma tcc c_begin / c_end (or through _Pragma): what the markers do */
static void pp_yasc_pragma(TCCState *s1, int on)
{
  CValue cv;
  cv.i = (TCC_PCH_REPLAY_YASC << 16) | 2 | on;
  if (pp_pragma_capture)
    tok_str_add2(pp_pragma_capture, TOK_PACK_REPLAY, &cv); /* a body saved: at its replay */
  /* now too: it may come in the lookahead past a saved body, before an #include */
  pp_apply_pack_replay(s1, cv.i);
  pp_yasc_pragma_set = on;
  if (file)
    file->yasc = on; /* an #include in between returns to it */
}

static int pragma_parse(TCCState *s1)
{
  next_nomacro();
  if (tok == TOK_push_macro || tok == TOK_pop_macro)
  {
    int t = tok, v;
    Sym *s;

    if (next(), tok != '(')
      goto pragma_err;
    if (next(), tok != TOK_STR)
      goto pragma_err;
    v = tok_alloc(tokc.str.data, tokc.str.size - 1)->tok;
    if (next(), tok != ')')
      goto pragma_err;
    if (t == TOK_push_macro)
    {
      while (NULL == (s = define_find(v)))
        define_push(v, 0, NULL, NULL);
      s->type.ref = s; /* set push boundary */
    }
    else
    {
      for (s = define_stack; s; s = s->prev)
        if (s->v == v && s->type.ref == s)
        {
          s->type.ref = NULL;
          break;
        }
    }
    if (s)
    {
      define_set(table_ident[v - TOK_IDENT], s->d ? s : NULL);
    }
    else
    {
      tcc_warning("unbalanced #pragma pop_macro");
    }
    pp_debug_tok = t, pp_debug_symv = v;
  }
  else if (tok == TOK_yasc)
  {
    pp_yasc_file(s1);
    if (s1->output_type == TCC_OUTPUT_PREPROCESS)
    {
      /* tcc -E: keep it for the compile of the output */
      unget_tok(' ');
      unget_tok(TOK_PRAGMA);
      unget_tok('#');
      unget_tok(TOK_LINEFEED);
    }
    else
      next_nomacro();
    return 1;
  }
  else if (tok == TOK_once)
  {
    search_cached_include(s1, file->true_filename, 1)->once = 1;
  }
  else if (s1->output_type == TCC_OUTPUT_PREPROCESS && pp_system_header_pp(s1))
  {
    return 0; /* tcc -E: the line goes, as with gcc; the line markers say it (flag 3) */
  }
  else if (s1->output_type == TCC_OUTPUT_PREPROCESS)
  {
    /* tcc -E: keep pragmas below unchanged */
    unget_tok(' ');
    unget_tok(TOK_PRAGMA);
    unget_tok('#');
    unget_tok(TOK_LINEFEED);
    return 1;
  }
  else if (tok == TOK_pack)
  {
    int rec_kind = 0;
    int rec_value = 0;
    /* When recording a function body for later token-stream replay
       (skip_or_save_block), pack directives must NOT mutate pack_stack now:
       struct layout for the body happens during the later replay, so an eager
       mutation here pushes AND pops before any struct is laid out, leaving the
       wrong pack state.  Instead defer the action into the saved stream as a
       TOK_PACK_REPLAY token, applied at the right position during replay. */
    int capturing = (pp_pragma_capture != NULL);
    /* This may be:
       #pragma pack(1) // set
       #pragma pack() // reset to default
       #pragma pack(push) // push current
       #pragma pack(push,1) // push & set
       #pragma pack(pop) // restore previous */
    next();
    skip('(');
    if (tok == TOK_ASM_pop)
    {
      next();
      if (!capturing && s1->pack_stack_ptr <= s1->pack_stack)
      {
      stk_error:
        tcc_error("out of pack stack");
      }
      if (!capturing)
        s1->pack_stack_ptr--;
      rec_kind = TCC_PCH_REPLAY_PACK_POP;
    }
    else
    {
      int val = 0;
      if (tok != ')')
      {
        if (tok == TOK_ASM_push)
        {
          next();
          if (!capturing && s1->pack_stack_ptr >= s1->pack_stack + PACK_STACK_SIZE - 1)
            goto stk_error;
          /* New top duplicates the current top unless an explicit value
             follows; read without advancing so capture mode stays inert. */
          val = *s1->pack_stack_ptr;
          if (!capturing)
            s1->pack_stack_ptr++;
          rec_kind = TCC_PCH_REPLAY_PACK_PUSH_DUP;
          if (tok != ',')
            goto pack_set;
          rec_kind = TCC_PCH_REPLAY_PACK_PUSH;
          next();
        }
        if (tok != TOK_CINT)
          goto pragma_err;
        val = tokc.i;
        if (val < 1 || val > 16 || (val & (val - 1)) != 0)
          goto pragma_err;
        next();
      }
    pack_set:
      if (!capturing)
        *s1->pack_stack_ptr = val;
      if (!rec_kind)
        rec_kind = TCC_PCH_REPLAY_PACK_SET;
      rec_value = val;
    }
    if (tok != ')')
      goto pragma_err;
    if (capturing)
    {
      CValue cv;
      cv.i = ((unsigned)rec_kind << 16) | (rec_value & 0xffff);
      tok_str_add2(pp_pragma_capture, TOK_PACK_REPLAY, &cv);
      pp_pack_captures++;
    }
  }
  else if (tok == TOK_comment)
  {
    char *p;
    int t;
    next();
    skip('(');
    t = tok;
    next();
    skip(',');
    if (tok != TOK_STR)
      goto pragma_err;
    p = tcc_strdup(tokc.str.data);
    next();
    if (tok != ')')
      goto pragma_err;
    if (t == TOK_lib)
    {
      dynarray_add(&s1->pragma_libs, &s1->nb_pragma_libs, p);
    }
    else
    {
      if (t == TOK_option)
      {
        tcc_set_options(s1, p);
      }
      tcc_free(p);
    }
  }
  else if (tok >= TOK_IDENT && (!strcmp(get_tok_str(tok, NULL), "GCC") || !strcmp(get_tok_str(tok, NULL), "tcc") ||
                                 !strcmp(get_tok_str(tok, NULL), "clang")))
  {
    /* #pragma GCC visibility push(default|hidden|internal|protected) / pop:
       the visibility of the declarations in between (see decl()).
       #pragma GCC|tcc|clang diagnostic push / pop / ignored / warning /
       error "-Wname" (pragma_diagnostic).  The other GCC pragmas stay
       ignored. */
    int gcc = !strcmp(get_tok_str(tok, NULL), "GCC"), tccw = !strcmp(get_tok_str(tok, NULL), "tcc");
    next();
    if (tccw && tok >= TOK_IDENT &&
        (!strcmp(get_tok_str(tok, NULL), "c_begin") || !strcmp(get_tok_str(tok, NULL), "c_end")))
    {
      /* a regular-C macro's expansion in yasc code, as tcc -E writes it */
      pp_yasc_pragma(s1, get_tok_str(tok, NULL)[2] == 'e');
      if (s1->output_type == TCC_OUTPUT_PREPROCESS)
        return 0;
      next();
      return 1;
    }
    if (tok >= TOK_IDENT && !strcmp(get_tok_str(tok, NULL), "diagnostic"))
    {
      pragma_diagnostic(s1);
      if (s1->output_type == TCC_OUTPUT_PREPROCESS)
        return 0; /* tcc -E: keep the line for the compile of the output */
      next();
      return 1;
    }
    if (gcc && tok >= TOK_IDENT && !strcmp(get_tok_str(tok, NULL), "system_header"))
    {
      pp_system_header(s1);
      next();
      return 1;
    }
    if (!gcc || tok != TOK_VISIBILITY1)
    {
      tcc_warning_c(warn_all)("#pragma GCC %s ignored", get_tok_str(tok, &tokc));
      return 0;
    }
    next();
    if (tok == TOK_ASM_pop)
    {
      if (s1->visibility_stack_depth == 0)
        tcc_error("#pragma GCC visibility pop without a push");
      s1->visibility_stack_depth--;
    }
    else if (tok == TOK_ASM_push)
    {
      const char *name;
      int vis;
      next();
      skip('(');
      name = get_tok_str(tok, &tokc);
      if (!strcmp(name, "default"))
        vis = STV_DEFAULT;
      else if (!strcmp(name, "hidden"))
        vis = STV_HIDDEN;
      else if (!strcmp(name, "internal"))
        vis = STV_INTERNAL;
      else if (!strcmp(name, "protected"))
        vis = STV_PROTECTED;
      else
        goto pragma_err;
      next();
      if (tok != ')')
        goto pragma_err;
      if (s1->visibility_stack_depth >= VISIBILITY_STACK_SIZE)
        tcc_error("#pragma GCC visibility push nested too deeply");
      s1->visibility_stack[s1->visibility_stack_depth++] = vis;
    }
    else
      goto pragma_err;
  }
  else
  {
    tcc_warning_c(warn_all)("#pragma %s ignored", get_tok_str(tok, &tokc));
    return 0;
  }
  next();
  return 1;
pragma_err:
  tcc_error("malformed #pragma directive");
}

/* Set the reported filename to one already resolved -- a name saved from
 * file->filename, as the deferred/inline body replays do.  Going through
 * tccpp_putfile would prepend the directory again on every replay. */
ST_FUNC void tccpp_setfile(const char *filename)
{
  if (0 == strcmp(file->filename, filename))
    return;
  if (file->true_filename == file->filename)
    file->true_filename = tcc_strdup(file->filename);
  pstrcpy(file->filename, sizeof file->filename, filename);
  tcc_debug_newfile(tcc_state);
}

/* A saved body replayed under the name of the file it came from: in that
   file's dialect too (not for #line, which only renames). */
ST_FUNC void tccpp_setfile_replay(const char *filename)
{
  tccpp_setfile(filename);
  file->yasc = tcc_state->nb_yasc_files && tcc_yasc_file(tcc_state, filename);
  tcc_state->yasc = file->yasc;
  /* a whole system header; one from its #pragma GCC system_header on is
     line by line (tcc_in_sys_header) */
  file->sys = tcc_sys_file(tcc_state, filename, 0);
}

/* put alternative filename (#line, .file): relative to the real file */
ST_FUNC void tccpp_putfile(const char *filename)
{
  char buf[1024];
  buf[0] = 0;
  if (!IS_ABSPATH(filename))
  {
    /* prepend directory from real file */
    pstrcpy(buf, sizeof buf, file->true_filename);
    *tcc_basename(buf) = 0;
  }
  pstrcat(buf, sizeof buf, filename);
#ifdef _WIN32
  normalize_slashes(buf);
#endif
  tccpp_setfile(buf);
}

/* is_bof is true if first non space token at beginning of file */
/* gcc's line marker `# N "file" flags`, which tcc -E writes: 1 enters a
 * header, 2 returns to the file that included it, 3 the text is a system
 * header's.  Compiling the output, the header is a system header again from
 * line N, and a file is in the dialect it had when a marker returns to it (a
 * header starts as regular C, its #pragma yasc is in the output).  Reads the
 * flags after the file name, up to the line feed. */
static void pp_line_marker(TCCState *s1, int line)
{
  int flags = 0;
  for (;;)
  {
    next_nomacro();
    if (tok != TOK_PPNUM || tokc.str.data[0] < '1' || tokc.str.data[0] > '4' || tokc.str.data[1])
      break;
    flags |= 1 << (tokc.str.data[0] - '0');
  }
  file->sys = !!(flags & 8);
  if (file->sys)
    tcc_sys_mark_file(s1, file->filename, line);
  if (flags & 2)
  {
    if (s1->pp_marker_depth < 32)
      s1->pp_marker_yasc = (s1->pp_marker_yasc << 1) | file->yasc;
    s1->pp_marker_depth++;
    file->yasc = 0;
  }
  else if ((flags & 4) && s1->pp_marker_depth > 0)
  {
    if (--s1->pp_marker_depth < 32)
    {
      file->yasc = s1->pp_marker_yasc & 1;
      s1->pp_marker_yasc >>= 1;
    }
  }
  s1->yasc = file->yasc;
  /* a body replayed under this name later is in this dialect too */
  if (file->yasc)
    tcc_yasc_mark_file(s1, file->filename);
}

ST_FUNC void preprocess(int is_bof)
{
  TCCState *s1 = tcc_state;
  int c, n, saved_parse_flags, linemarker = 0;
  char buf[1024], *q;
  Sym *s;

  saved_parse_flags = parse_flags;
  parse_flags = PARSE_FLAG_PREPROCESS | PARSE_FLAG_TOK_NUM | PARSE_FLAG_TOK_STR | PARSE_FLAG_LINEFEED |
                (parse_flags & PARSE_FLAG_ASM_FILE);

  next_nomacro();
redo:
  switch (tok)
  {
  case TOK_DEFINE:
    pp_debug_tok = tok;
    next_nomacro();
    pp_debug_symv = tok;
    parse_define();
    break;
  case TOK_UNDEF:
    pp_debug_tok = tok;
    next_nomacro();
    pp_debug_symv = tok;
    s = define_find(tok);
    /* undefine symbol by putting an invalid name */
    if (s)
      define_undef(s);
    next_nomacro();
    break;
  case TOK_INCLUDE:
  case TOK_INCLUDE_NEXT:
    parse_include(s1, tok - TOK_INCLUDE, 0);
    goto the_end;
  case TOK_EMBED:
    parse_embed(s1);
    goto the_end;
  case TOK_IFNDEF:
    c = 1;
    goto do_ifdef;
  case TOK_IF:
    c = expr_preprocess(s1, 0);
    goto do_if;
  case TOK_IFDEF:
    c = 0;
  do_ifdef:
    next_nomacro();
    if (tok < TOK_IDENT)
      tcc_error("invalid argument for '#if%sdef'", c ? "n" : "");
    if (is_bof)
    {
      if (c)
      {
#ifdef INC_DEBUG
        printf("#ifndef %s\n", get_tok_str(tok, NULL));
#endif
        file->ifndef_macro = tok;
      }
    }
    if (define_find(tok) || tok == TOK___HAS_INCLUDE || tok == TOK___HAS_INCLUDE_NEXT || tok == TOK___HAS_EMBED ||
        tok == TOK___HAS_ATTRIBUTE)
      c ^= 1;
    next_nomacro();
  do_if:
    if (s1->ifdef_stack_ptr >= s1->ifdef_stack + IFDEF_STACK_SIZE)
      tcc_error("memory full (ifdef)");
    *s1->ifdef_stack_ptr++ = c;
    goto test_skip;
  case TOK_ELSE:
    next_nomacro();
    if (s1->ifdef_stack_ptr == s1->ifdef_stack)
      tcc_error("#else without matching #if");
    if (s1->ifdef_stack_ptr[-1] & 2)
      tcc_error("#else after #else");
    c = (s1->ifdef_stack_ptr[-1] ^= 3);
    goto test_else;
  case TOK_ELIF:
    if (s1->ifdef_stack_ptr == s1->ifdef_stack)
      tcc_error("#elif without matching #if");
    c = s1->ifdef_stack_ptr[-1];
    if (c > 1)
      tcc_error("#elif after #else");
    /* last #if/#elif expression was true: we skip */
    if (c == 1)
    {
      skip_to_eol(0);
      c = 0;
    }
    else
    {
      c = expr_preprocess(s1, 0);
      s1->ifdef_stack_ptr[-1] = c;
    }
  test_else:
    if (s1->ifdef_stack_ptr == file->ifdef_stack_ptr + 1)
      file->ifndef_macro = 0;
  test_skip:
    if (!(c & 1))
    {
      skip_to_eol(1);
      preprocess_skip();
      is_bof = 0;
      goto redo;
    }
    break;
  case TOK_ENDIF:
    next_nomacro();
    if (s1->ifdef_stack_ptr <= file->ifdef_stack_ptr)
      tcc_error("#endif without matching #if");
    s1->ifdef_stack_ptr--;
    /* '#ifndef macro' was at the start of file. Now we check if
       an '#endif' is exactly at the end of file */
    if (file->ifndef_macro && s1->ifdef_stack_ptr == file->ifdef_stack_ptr)
    {
      file->ifndef_macro_saved = file->ifndef_macro;
      /* need to set to zero to avoid false matches if another
         #ifndef at middle of file */
      file->ifndef_macro = 0;
      tok_flags |= TOK_FLAG_ENDIF;
    }
    break;

  case TOK_LINE:
    parse_flags &= ~PARSE_FLAG_TOK_NUM;
    next();
    parse_flags |= PARSE_FLAG_TOK_NUM;
    if (tok != TOK_PPNUM)
    {
    _line_err:
      tcc_error("wrong #line format");
    }
    goto _line_num;
  case TOK_PPNUM:
    if (parse_flags & PARSE_FLAG_ASM_FILE)
      goto ignore;
    linemarker = 1; /* `# N "file" flags`, not #line */
  _line_num:
    for (n = 0, q = tokc.str.data; *q; ++q)
    {
      if (!isnum(*q))
        goto _line_err;
      n = n * 10 + *q - '0';
    }
    parse_flags &= ~PARSE_FLAG_TOK_STR;
    next();
    parse_flags |= PARSE_FLAG_TOK_STR;
    if (tok == TOK_PPSTR && tokc.str.data[0] == '"')
    {
      tokc.str.data[tokc.str.size - 2] = 0;
      tccpp_putfile(tokc.str.data + 1);
      if (linemarker)
        pp_line_marker(s1, n);
      if (tok != TOK_LINEFEED)
        n--; /* the line feed ending the directive is still to come */
      if (macro_ptr && *macro_ptr == 0)
        macro_stack->save_line_num = n;
    }
    else if (tok != TOK_LINEFEED)
      goto _line_err;
    if (file->fd > 0)
      total_lines += file->line_num - n;
    file->line_ref += file->line_num - n;
    file->line_num = n;
    goto ignore; /* skip optional level number */

  case TOK_ERROR:
  case TOK_WARNING:
  {
    q = buf;
    c = skip_spaces();
    while (c != '\n' && c != CH_EOF)
    {
      if ((q - buf) < sizeof(buf) - 1)
        *q++ = c;
      c = ninp();
    }
    *q = '\0';
    if (tok == TOK_ERROR || tok == TOK_WARNING)
    {
      /* in a body being saved: said where its parse reaches it */
      char m[sizeof buf + 16];
      snprintf(m, sizeof m, "#%s %s", tok == TOK_ERROR ? "error" : "warning", buf);
      if (tcc_pp_diag_deferred(tok == TOK_ERROR ? DIAG_STASH_NOABORT : DIAG_STASH_WARNING, m))
        goto diag_done;
    }
    if (tok == TOK_ERROR)
      tcc_error_noabort("#error %s", buf); /* an error, and the compile goes on (gcc) */
    else
    {
      s1->diag_any_header = 1; /* given in a system header too, as gcc */
      tcc_warning("#warning %s", buf);
    }
  diag_done:
    next_nomacro();
    break;
  }
  case TOK_PRAGMA:
    if (!pragma_parse(s1))
      goto ignore;
    break;
  case TOK_LINEFEED:
    goto the_end;
  default:
    /* ignore gas line comment in an 'S' file. */
    if (saved_parse_flags & PARSE_FLAG_ASM_FILE)
      goto ignore;
    if (tok == '!' && is_bof)
      /* '#!' is ignored at beginning to allow C scripts. */
      goto ignore;
    tcc_warning("Ignoring unknown preprocessing directive #%s", get_tok_str(tok, &tokc));
  ignore:
    skip_to_eol(0);
    goto the_end;
  }
  skip_to_eol(1);
the_end:
  parse_flags = saved_parse_flags;
}

/* evaluate escape codes in a string. */
static void parse_escape_string(CString *outstr, const uint8_t *buf, int is_long)
{
  int c, n, i;
  const uint8_t *p;

  p = buf;
  for (;;)
  {
    c = *p;
    if (c == '\0')
      break;
    if (c == '\\')
    {
      p++;
      /* escape */
      c = *p;
      switch (c)
      {
      case '0':
      case '1':
      case '2':
      case '3':
      case '4':
      case '5':
      case '6':
      case '7':
        /* at most three octal digits */
        n = c - '0';
        p++;
        c = *p;
        if (isoct(c))
        {
          n = n * 8 + c - '0';
          p++;
          c = *p;
          if (isoct(c))
          {
            n = n * 8 + c - '0';
            p++;
          }
        }
        c = n;
        goto add_char_nonext;
      case 'x':
        i = 0;
        goto parse_hex_or_ucn;
      case 'u':
        i = 4;
        goto parse_hex_or_ucn;
      case 'U':
        i = 8;
        goto parse_hex_or_ucn;
      parse_hex_or_ucn:
        p++;
        n = 0;
        do
        {
          c = *p;
          if (c >= 'a' && c <= 'f')
            c = c - 'a' + 10;
          else if (c >= 'A' && c <= 'F')
            c = c - 'A' + 10;
          else if (isnum(c))
            c = c - '0';
          else if (i >= 0)
            expect("more hex digits in universal-character-name");
          else
            goto add_hex_or_ucn;
          n = n * 16 + c;
          p++;
        } while (--i);
        if (is_long)
        {
        add_hex_or_ucn:
          c = n;
          goto add_char_nonext;
        }
        cstr_u8cat(outstr, n);
        continue;
      case 'a':
        c = '\a';
        break;
      case 'b':
        c = '\b';
        break;
      case 'f':
        c = '\f';
        break;
      case 'n':
        c = '\n';
        break;
      case 'r':
        c = '\r';
        break;
      case 't':
        c = '\t';
        break;
      case 'v':
        c = '\v';
        break;
      case 'e':
        if (!gnu_ext)
          goto invalid_escape;
        c = 27;
        break;
      case '\'':
      case '\"':
      case '\\':
      case '?':
        break;
      default:
      invalid_escape:
        if (c >= '!' && c <= '~')
          tcc_warning("unknown escape sequence: \'\\%c\'", c);
        else
          tcc_warning("unknown escape sequence: \'\\x%x\'", c);
        break;
      }
    }
    else if (is_long && c >= 0x80)
    {
      /* assume we are processing UTF-8 sequence */
      /* reference: The Unicode Standard, Version 10.0, ch3.9 */

      int cont; /* count of continuation bytes */
      int skip; /* how many bytes should skip when error occurred */
      int i;

      /* decode leading byte */
      if (c < 0xC2)
      {
        skip = 1;
        goto invalid_utf8_sequence;
      }
      else if (c <= 0xDF)
      {
        cont = 1;
        n = c & 0x1f;
      }
      else if (c <= 0xEF)
      {
        cont = 2;
        n = c & 0xf;
      }
      else if (c <= 0xF4)
      {
        cont = 3;
        n = c & 0x7;
      }
      else
      {
        skip = 1;
        goto invalid_utf8_sequence;
      }

      /* decode continuation bytes */
      for (i = 1; i <= cont; i++)
      {
        int l = 0x80, h = 0xBF;

        /* adjust limit for second byte */
        if (i == 1)
        {
          switch (c)
          {
          case 0xE0:
            l = 0xA0;
            break;
          case 0xED:
            h = 0x9F;
            break;
          case 0xF0:
            l = 0x90;
            break;
          case 0xF4:
            h = 0x8F;
            break;
          }
        }

        if (p[i] < l || p[i] > h)
        {
          skip = i;
          goto invalid_utf8_sequence;
        }

        n = (n << 6) | (p[i] & 0x3f);
      }

      /* advance pointer */
      p += 1 + cont;
      c = n;
      goto add_char_nonext;

      /* error handling */
    invalid_utf8_sequence:
      tcc_warning("ill-formed UTF-8 subsequence starting with: \'\\x%x\'", c);
      c = 0xFFFD;
      p += skip;
      goto add_char_nonext;
    }
    p++;
  add_char_nonext:
    if (!is_long)
      cstr_ccat(outstr, c);
    else
    {
#ifdef TCC_TARGET_PE
      /* store as UTF-16 */
      if (c < 0x10000)
      {
        cstr_wccat(outstr, c);
      }
      else
      {
        c -= 0x10000;
        cstr_wccat(outstr, (c >> 10) + 0xD800);
        cstr_wccat(outstr, (c & 0x3FF) + 0xDC00);
      }
#else
      cstr_wccat(outstr, c);
#endif
    }
  }
  /* add a trailing '\0' */
  if (!is_long)
    cstr_ccat(outstr, '\0');
  else
    cstr_wccat(outstr, '\0');
}

/* noinline + small buf: the compiler inlines this into next(), which sits at the
 * bottom of every recursive-descent chain, so a 1000-byte buffer here became a
 * 1 KiB hole in every parser stack on the 32 KiB target. */
__attribute__((noinline)) static void parse_string(const char *s, int len)
{
  uint8_t buf[128], *p = buf;
  int is_long, sep;

  if ((is_long = *s == 'L'))
    ++s, --len;
  sep = *s++;
  len -= 2;
  if (len >= sizeof buf)
    p = tcc_malloc(len + 1);
  memcpy(p, s, len);
  p[len] = 0;

  cstr_reset(&tokcstr);
  parse_escape_string(&tokcstr, p, is_long);
  if (p != buf)
    tcc_free(p);

  if (sep == '\'')
  {
    int char_size, i, n, c;
    if (!is_long)
      tok = TOK_CCHAR, char_size = 1;
    else
      tok = TOK_LCHAR, char_size = sizeof(nwchar_t);
    n = tokcstr.size / char_size - 1;
    if (n < 1)
      tcc_lex_error_deferred("empty character constant"); /* in a saved body: 0 */
    if (n > 1)
      tcc_warning_c(warn_all)("multi-character character constant");
    for (c = i = 0; i < n; ++i)
    {
      if (is_long)
        c = ((nwchar_t *)tokcstr.data)[i];
      else
        c = (c << 8) | ((unsigned char *)tokcstr.data)[i];
    }
    /* A single narrow character constant has the value of the target char
       converted to int.  Multi-character constants concatenate bytes, so
       their component bytes must remain unsigned. */
    if (!is_long && n == 1 && !tcc_state->char_is_unsigned)
      c = (signed char)c;
    tokc.i = c;
  }
  else
  {
    tokc.str.size = tokcstr.size;
    tokc.str.data = tokcstr.data;
    if (!is_long)
      tok = TOK_STR;
    else
      tok = TOK_LSTR;
  }
}

/* we use 64 bit numbers */
#define BN_SIZE 2

/* bn = (bn << shift) | or_val */
static void bn_lshift(unsigned int *bn, int shift, int or_val)
{
  int i;
  unsigned int v;
  for (i = 0; i < BN_SIZE; i++)
  {
    v = bn[i];
    bn[i] = (v << shift) | or_val;
    or_val = v >> (32 - shift);
  }
}

static void bn_zero(unsigned int *bn)
{
  int i;
  for (i = 0; i < BN_SIZE; i++)
  {
    bn[i] = 0;
  }
}

/* Accumulate one hex/binary float digit into the 64-bit mantissa.  Leading
   zeros carry no significance and use no room; a digit that no longer fits
   is dropped but still scales the value (integer part: `*exp_extra`) and
   feeds `*sticky` so the final rounding sees it. */
static void hexfloat_digit(unsigned int *bn, int shift, int t, int is_frac, int *frac_bits, int *exp_extra,
                           int *sticky)
{
  int used = 0;
  if (bn[1])
    used = 64 - __builtin_clz(bn[1]);
  else if (bn[0])
    used = 32 - __builtin_clz(bn[0]);
  if (used == 0 && t == 0)
  {
    if (is_frac)
      *frac_bits += shift;
    return;
  }
  if (used + shift <= BN_SIZE * 32)
  {
    bn_lshift(bn, shift, t);
    if (is_frac)
      *frac_bits += shift;
    return;
  }
  if (!is_frac)
    *exp_extra += shift;
  if (t)
    *sticky = 1;
}

/* parse number in null terminated string 'p' and return it in the
   current token */
/* A number's error: in a body being saved, a marker said where the parse
   reaches it, the number read as 0 (tcc_lex_error_deferred) */
#define NUM_ERROR(...)                                                                                                 \
  do                                                                                                                   \
  {                                                                                                                    \
    if (tcc_lex_error_deferred(__VA_ARGS__))                                                                           \
    {                                                                                                                  \
      tok = TOK_CINT;                                                                                                  \
      tokc.i = 0;                                                                                                      \
      return;                                                                                                          \
    }                                                                                                                  \
  } while (0)

static void parse_number(const char *p)
{
  int b, t, shift, frac_bits, s, exp_val, ch;
  char *q;
  unsigned int bn[BN_SIZE];
  double d;

  /* number */
  q = token_buf;
  ch = *p++;
  t = ch;
  ch = *p++;
  *q++ = t;
  b = 10;
  if (t == '.')
  {
    goto float_frac_parse;
  }
  else if (t == '0')
  {
    if (ch == 'x' || ch == 'X')
    {
      q--;
      ch = *p++;
      b = 16;
    }
    else if (tcc_state->tcc_ext && (ch == 'b' || ch == 'B'))
    {
      q--;
      ch = *p++;
      b = 2;
    }
  }
  /* parse all digits. cannot check octal numbers at this stage
     because of floating point constants */
  while (1)
  {
    if (ch >= 'a' && ch <= 'f')
      t = ch - 'a' + 10;
    else if (ch >= 'A' && ch <= 'F')
      t = ch - 'A' + 10;
    else if (isnum(ch))
      t = ch - '0';
    else
      break;
    if (t >= b)
      break;
    if (q >= token_buf + STRING_MAX_SIZE)
    {
    num_too_long:
      NUM_ERROR("number too long");
    }
    *q++ = ch;
    ch = *p++;
  }
  if (ch == '.' || ((ch == 'e' || ch == 'E') && b == 10) || ((ch == 'p' || ch == 'P') && (b == 16 || b == 2)))
  {
    if (b != 10)
    {
      /* NOTE: strtox should support that for hexa numbers, but
         non ISOC99 libcs do not support it, so we prefer to do
         it by hand */
      /* hexadecimal or binary floats */
      /* XXX: handle overflows */
      *q = '\0';
      if (b == 16)
        shift = 4;
      else
        shift = 1;
      bn_zero(bn);
      int exp_extra = 0, sticky = 0;
      frac_bits = 0;
      q = token_buf;
      while (1)
      {
        t = *q++;
        if (t == '\0')
        {
          break;
        }
        else if (t >= 'a')
        {
          t = t - 'a' + 10;
        }
        else if (t >= 'A')
        {
          t = t - 'A' + 10;
        }
        else
        {
          t = t - '0';
        }
        hexfloat_digit(bn, shift, t, 0, &frac_bits, &exp_extra, &sticky);
      }
      if (ch == '.')
      {
        ch = *p++;
        while (1)
        {
          t = ch;
          if (t >= 'a' && t <= 'f')
          {
            t = t - 'a' + 10;
          }
          else if (t >= 'A' && t <= 'F')
          {
            t = t - 'A' + 10;
          }
          else if (t >= '0' && t <= '9')
          {
            t = t - '0';
          }
          else
          {
            break;
          }
          if (t >= b)
            NUM_ERROR("invalid digit");
          hexfloat_digit(bn, shift, t, 1, &frac_bits, &exp_extra, &sticky);
          ch = *p++;
        }
      }
      if (ch != 'p' && ch != 'P')
        NUM_ERROR("%s expected", "exponent");
      ch = *p++;
      s = 1;
      exp_val = 0;
      if (ch == '+')
      {
        ch = *p++;
      }
      else if (ch == '-')
      {
        s = -1;
        ch = *p++;
      }
      if (ch < '0' || ch > '9')
        NUM_ERROR("%s expected", "exponent digits");
      while (ch >= '0' && ch <= '9')
      {
        exp_val = exp_val * 10 + ch - '0';
        ch = *p++;
      }
      exp_val = exp_val * s;

      /* now we can generate the number */
      /* XXX: should patch directly float number */
      /* dropped digits sit far below the double's last bit (the mantissa
         is full), so a sticky low bit is enough to round them correctly */
      if (sticky)
        bn[0] |= 1;
      d = (double)bn[1] * 4294967296.0 + (double)bn[0];
      d = ldexp(d, exp_val + exp_extra - frac_bits);
      t = toup(ch);
      if (t == 'F')
      {
        ch = *p++;
        tok = TOK_CFLOAT;
        /* float : should handle overflow */
        tokc.f = (float)d;
      }
      else if (t == 'L')
      {
        ch = *p++;
        tok = TOK_CLDOUBLE;
#ifdef TCC_USING_DOUBLE_FOR_LDOUBLE
        tokc.d = d;
#else
        /* XXX: not large enough */
        tokc.ld = (long double)d;
#endif
      }
      else if (t == 'D')
      {
        /* C2x decimal float suffixes: DF, DD, DL (approximated with binary FP) */
        ch = *p++;
        t = toup(ch);
        if (t == 'F')
        {
          ch = *p++;
          tok = TOK_CFLOAT;
          tokc.f = (float)d;
        }
        else if (t == 'L')
        {
          ch = *p++;
          tok = TOK_CLDOUBLE;
#ifdef TCC_USING_DOUBLE_FOR_LDOUBLE
          tokc.d = d;
#else
          tokc.ld = (long double)d;
#endif
        }
        else
        {
          /* DD suffix or bare D */
          if (t == 'D')
            ch = *p++;
          tok = TOK_CDOUBLE;
          tokc.d = d;
        }
      }
      else
      {
        tok = TOK_CDOUBLE;
        tokc.d = d;
      }
    }
    else
    {
      /* decimal floats */
      if (ch == '.')
      {
        if (q >= token_buf + STRING_MAX_SIZE)
          goto num_too_long;
        *q++ = ch;
        ch = *p++;
      float_frac_parse:
        while (ch >= '0' && ch <= '9')
        {
          if (q >= token_buf + STRING_MAX_SIZE)
            goto num_too_long;
          *q++ = ch;
          ch = *p++;
        }
      }
      if (ch == 'e' || ch == 'E')
      {
        if (q >= token_buf + STRING_MAX_SIZE)
          goto num_too_long;
        *q++ = ch;
        ch = *p++;
        if (ch == '-' || ch == '+')
        {
          if (q >= token_buf + STRING_MAX_SIZE)
            goto num_too_long;
          *q++ = ch;
          ch = *p++;
        }
        if (ch < '0' || ch > '9')
          NUM_ERROR("%s expected", "exponent digits");
        while (ch >= '0' && ch <= '9')
        {
          if (q >= token_buf + STRING_MAX_SIZE)
            goto num_too_long;
          *q++ = ch;
          ch = *p++;
        }
      }
      *q = '\0';
      t = toup(ch);
      errno = 0;
      if (t == 'F')
      {
        ch = *p++;
        tok = TOK_CFLOAT;
        tokc.f = strtof(token_buf, NULL);
      }
      else if (t == 'L')
      {
        ch = *p++;
        tok = TOK_CLDOUBLE;
#ifdef TCC_USING_DOUBLE_FOR_LDOUBLE
        tokc.d = strtod(token_buf, NULL);
#else
        tokc.ld = strtold(token_buf, NULL);
#endif
      }
      else if (t == 'D')
      {
        /* C2x decimal float suffixes: DF, DD, DL (approximated with binary FP) */
        ch = *p++;
        t = toup(ch);
        if (t == 'F')
        {
          ch = *p++;
          tok = TOK_CFLOAT;
          tokc.f = strtof(token_buf, NULL);
        }
        else if (t == 'L')
        {
          ch = *p++;
          tok = TOK_CLDOUBLE;
#ifdef TCC_USING_DOUBLE_FOR_LDOUBLE
          tokc.d = strtod(token_buf, NULL);
#else
          tokc.ld = strtold(token_buf, NULL);
#endif
        }
        else
        {
          /* DD suffix or bare D */
          if (t == 'D')
            ch = *p++;
          tok = TOK_CDOUBLE;
          tokc.d = strtod(token_buf, NULL);
        }
      }
      else
      {
        tok = TOK_CDOUBLE;
        tokc.d = strtod(token_buf, NULL);
      }
      /* GNU imaginary suffix: i, I, j, J
       * Can appear before or after type suffix (F/L).
       * e.g. 1.0Fi, 1.0iF, 1.0i, 1.0Li, 1.0iL */
      t = toup(ch);
      if (t == 'I' || t == 'J')
      {
        ch = *p++;
        /* Check for type suffix after imaginary suffix: iF, iL */
        if (tok == TOK_CDOUBLE)
        {
          int t2 = toup(ch);
          if (t2 == 'F')
          {
            ch = *p++;
            tok = TOK_CFLOAT_I;
            tokc.f = strtof(token_buf, NULL);
          }
          else if (t2 == 'L')
          {
            ch = *p++;
            tok = TOK_CLDOUBLE_I;
#ifdef TCC_USING_DOUBLE_FOR_LDOUBLE
            tokc.d = strtod(token_buf, NULL);
#else
            tokc.ld = strtold(token_buf, NULL);
#endif
          }
          else
          {
            tok = TOK_CDOUBLE_I;
          }
        }
        else if (tok == TOK_CFLOAT)
          tok = TOK_CFLOAT_I;
        else if (tok == TOK_CLDOUBLE)
          tok = TOK_CLDOUBLE_I;
        else
          tok = TOK_CDOUBLE_I;
      }
    }
  }
  else
  {
    unsigned long long n, n1;
    int lcount, ucount, ov = 0;
    const char *p1;

    /* integer number */
    *q = '\0';
    q = token_buf;
    if (b == 10 && *q == '0')
    {
      b = 8;
      q++;
    }
    n = 0;
    while (1)
    {
      t = *q++;
      /* no need for checks except for base 10 / 8 errors */
      if (t == '\0')
        break;
      else if (t >= 'a')
        t = t - 'a' + 10;
      else if (t >= 'A')
        t = t - 'A' + 10;
      else
        t = t - '0';
      if (t >= b)
        NUM_ERROR("invalid digit");
      n1 = n;
      n = n * b + t;
      /* detect overflow */
      if (n1 >= 0x1000000000000000ULL && n / b != n1)
        ov = 1;
    }

    /* Determine the characteristics (unsigned and/or 64bit) the type of
       the constant must have according to the constant suffix(es) */
    lcount = ucount = 0;
    p1 = p;
    for (;;)
    {
      t = toup(ch);
      if (t == 'L')
      {
        if (lcount >= 2)
          NUM_ERROR("three 'l's in integer constant");
        if (lcount && *(p - 1) != ch)
          NUM_ERROR("incorrect integer suffix: %s", p1);
        lcount++;
        ch = *p++;
      }
      else if (t == 'U')
      {
        if (ucount >= 1)
          NUM_ERROR("two 'u's in integer constant");
        ucount++;
        ch = *p++;
      }
      else
      {
        break;
      }
    }

    /* Determine if it needs 64 bits and/or unsigned in order to fit */
    if (ucount == 0 && b == 10)
    {
      if (lcount <= (LONG_SIZE == 4))
      {
        if (n >= 0x80000000U)
          lcount = (LONG_SIZE == 4) + 1;
      }
      if (n >= 0x8000000000000000ULL)
        ov = 1, ucount = 1;
    }
    else
    {
      if (lcount <= (LONG_SIZE == 4))
      {
        if (n >= 0x100000000ULL)
          lcount = (LONG_SIZE == 4) + 1;
        else if (n >= 0x80000000U)
          ucount = 1;
      }
      if (n >= 0x8000000000000000ULL)
        ucount = 1;
    }

    if (ov)
      tcc_warning("integer constant overflow");

    if (pp_expr)
    {
      /* C preprocessor integer arithmetic uses intmax_t / uintmax_t
         semantics, not the target's narrower int/long widths.  Keep
         only signedness from the suffix and evaluate everything as
         64-bit signed/unsigned integers. */
      tok = ucount ? TOK_CULLONG : TOK_CLLONG;
    }
    else
    {
      tok = TOK_CINT;
      if (lcount)
      {
        tok = TOK_CLONG;
        if (lcount == 2)
          tok = TOK_CLLONG;
      }
      if (ucount)
        ++tok; /* TOK_CU... */
    }
    tokc.i = n;

    /* GNU imaginary suffix: i, I, j, J on integer constants */
    t = toup(ch);
    if (t == 'I' || t == 'J')
    {
      ch = *p++;
      /* Integer imaginary: keep the integer value, mark as imaginary.
       * The value is the magnitude of the imaginary part. */
      tok = TOK_CINT_I;
      tokc.i = n;
    }
  }
  if (ch)
    NUM_ERROR("invalid number");
}

#define PARSE2(c1, tok1, c2, tok2)                                                                                     \
  case c1:                                                                                                             \
    PEEKC(c, p);                                                                                                       \
    if (c == c2)                                                                                                       \
    {                                                                                                                  \
      p++;                                                                                                             \
      tok = tok2;                                                                                                      \
    }                                                                                                                  \
    else                                                                                                               \
    {                                                                                                                  \
      tok = tok1;                                                                                                      \
    }                                                                                                                  \
    break;

/* where the token next_nomacro_raw read last starts (yasc_glue) */
static uint8_t *lex_tok_start;

/* return next token without macro substitution */
static void next_nomacro_raw(void)
{
  int t, c, is_long, len;
  TokenSym *ts;
  uint8_t *p, *p1;
  unsigned int h;

  p = file->buf_ptr;
redo_no_start:
  lex_tok_start = p;
  c = *p;
  switch (c)
  {
  case ' ':
  case '\t':
    tok = c;
    p++;
  maybe_space:
    if (parse_flags & PARSE_FLAG_SPACES)
      goto keep_tok_flags;
    while (isidnum_table[*p - CH_EOF] & IS_SPC)
      ++p;
    goto redo_no_start;
  case '\f':
  case '\v':
  case '\r':
    p++;
    goto redo_no_start;
  case '\\':
    /* first look if it is in fact an end of buffer */
    c = handle_stray(&p);
    if (c == '\\')
      goto parse_simple;
    if (c == CH_EOF)
    {
      TCCState *s1 = tcc_state;
      if (!(tok_flags & TOK_FLAG_BOL))
      {
        /* add implicit newline */
        goto maybe_newline;
      }
      else if (!(parse_flags & PARSE_FLAG_PREPROCESS))
      {
        tok = TOK_EOF;
      }
      else if (s1->ifdef_stack_ptr != file->ifdef_stack_ptr)
      {
        tcc_error("missing #endif");
      }
      else if (s1->include_stack_ptr == s1->include_stack)
      {
        /* no include left : end of file. */
        tok = TOK_EOF;
      }
      else
      {
        /* pop include file */

        /* test if previous '#endif' was after a #ifdef at
           start of file */
        if (tok_flags & TOK_FLAG_ENDIF)
        {
#ifdef INC_DEBUG
          printf("#endif %s\n", get_tok_str(file->ifndef_macro_saved, NULL));
#endif
          search_cached_include(s1, file->true_filename, 1)->ifndef_macro = file->ifndef_macro_saved;
          tok_flags &= ~TOK_FLAG_ENDIF;
        }

        /* add end of include file debug info */
        tcc_debug_eincl(tcc_state);
        /* pop include stack */
        tcc_close();
        s1->include_stack_ptr--;
        p = file->buf_ptr;
        goto maybe_newline;
      }
    }
    else
    {
      goto redo_no_start;
    }
    break;

  case '\n':
    file->line_num++;
    p++;
  maybe_newline:
    tok_flags |= TOK_FLAG_BOL;
    if (0 == (parse_flags & PARSE_FLAG_LINEFEED))
      goto redo_no_start;
    tok = TOK_LINEFEED;
    goto keep_tok_flags;

  case '#':
    /* XXX: simplify */
    PEEKC(c, p);
    if ((tok_flags & TOK_FLAG_BOL) && (parse_flags & PARSE_FLAG_PREPROCESS))
    {
      tok_flags &= ~TOK_FLAG_BOL;
      file->buf_ptr = p;
      preprocess(tok_flags & TOK_FLAG_BOF);
      p = file->buf_ptr;
      if (pp_embed_pending)
      {
        /* #embed pushed its list: next() reads it (after the directive's
           linefeed when linefeeds are tokens, as with -E) */
        pp_embed_pending = 0;
        if (!(parse_flags & PARSE_FLAG_LINEFEED))
        {
          tok_flags |= TOK_FLAG_BOL;
          tok = TOK_EMBED_RESUME;
          goto keep_tok_flags;
        }
      }
      goto maybe_newline;
    }
    else
    {
      if (c == '#')
      {
        p++;
        tok = TOK_TWOSHARPS;
      }
      else
      {
#if !defined(TCC_TARGET_ARM)
        if (parse_flags & PARSE_FLAG_ASM_FILE)
        {
          p = parse_line_comment(p - 1);
          goto redo_no_start;
        }
        else
#endif
        {
          tok = '#';
        }
      }
    }
    break;

  /* dollar is allowed to start identifiers when not parsing asm */
  case '$':
    if (!(isidnum_table['$' - CH_EOF] & IS_ID) || (parse_flags & PARSE_FLAG_ASM_FILE))
      goto parse_simple;

  case 'a':
  case 'b':
  case 'c':
  case 'd':
  case 'e':
  case 'f':
  case 'g':
  case 'h':
  case 'i':
  case 'j':
  case 'k':
  case 'l':
  case 'm':
  case 'n':
  case 'o':
  case 'p':
  case 'q':
  case 'r':
  case 's':
  case 't':
  case 'u':
  case 'v':
  case 'w':
  case 'x':
  case 'y':
  case 'z':
  case 'A':
  case 'B':
  case 'C':
  case 'D':
  case 'E':
  case 'F':
  case 'G':
  case 'H':
  case 'I':
  case 'J':
  case 'K':
  case 'M':
  case 'N':
  case 'O':
  case 'P':
  case 'Q':
  case 'R':
  case 'S':
  case 'T':
  case 'U':
  case 'V':
  case 'W':
  case 'X':
  case 'Y':
  case 'Z':
  case '_':
  parse_ident_fast:
    p1 = p;
    h = TOK_HASH_INIT;
    h = TOK_HASH_FUNC(h, c);
    while (c = *++p, isidnum_table[c - CH_EOF] & (IS_ID | IS_NUM))
      h = TOK_HASH_FUNC(h, c);
    len = p - p1;
    if (c != '\\')
    {
      TokenSym **pts;
      unsigned int h_full;

      /* fast case : no stray found, so we have the full token
         and we have already hashed it */
      h_full = h;
      ts = token_lookup_cache_find(h_full, (char *)p1, len);
      if (!ts) {
        h &= (TOK_HASH_SIZE - 1);
        pts = &hash_ident[h];
        for (;;)
        {
          ts = *pts;
          if (!ts)
            break;
          if (ts->len == len && !memcmp(tok_name(ts), p1, len)) {
            token_lookup_cache_store(h_full, len, ts);
            goto token_found;
          }
          pts = &(ts->hash_next);
        }
        /* lazy builtin (keyword, __builtin_xxx, asm-dir) materialization */
        ts = kw_lookup_materialize(h_full, (char *)p1, len);
        if (!ts)
          ts = tok_alloc_new(pts, (char *)p1, len);
        token_lookup_cache_store(h_full, len, ts);
      }
    token_found:;
    }
    else
    {
      /* slower case */
      cstr_reset(&tokcstr);
      cstr_cat(&tokcstr, (char *)p1, len);
      p--;
      PEEKC(c, p);
    parse_ident_slow:
      while (isidnum_table[c - CH_EOF] & (IS_ID | IS_NUM))
      {
        cstr_ccat(&tokcstr, c);
        PEEKC(c, p);
      }
      ts = tok_alloc(tokcstr.data, tokcstr.size);
    }
    tok = ts->tok;
    break;
  case 'L':
    t = p[1];
    if (t != '\\' && t != '\'' && t != '\"')
    {
      /* fast case */
      goto parse_ident_fast;
    }
    else
    {
      PEEKC(c, p);
      if (c == '\'' || c == '\"')
      {
        is_long = 1;
        goto str_const;
      }
      else
      {
        cstr_reset(&tokcstr);
        cstr_ccat(&tokcstr, 'L');
        goto parse_ident_slow;
      }
    }
    break;

  case '0':
  case '1':
  case '2':
  case '3':
  case '4':
  case '5':
  case '6':
  case '7':
  case '8':
  case '9':
    t = c;
    PEEKC(c, p);
    /* after the first digit, accept digits, alpha, '.' or sign if
       prefixed by 'eEpP' */
  parse_num:
    cstr_reset(&tokcstr);
    for (;;)
    {
      cstr_ccat(&tokcstr, t);
      if (!((isidnum_table[c - CH_EOF] & (IS_ID | IS_NUM)) || c == '.' ||
            ((c == '+' || c == '-') && (((t == 'e' || t == 'E') && !(parse_flags & PARSE_FLAG_ASM_FILE
                                                                     /* 0xe+1 is 3 tokens in asm */
                                                                     && ((char *)tokcstr.data)[0] == '0' &&
                                                                     toup(((char *)tokcstr.data)[1]) == 'X')) ||
                                        t == 'p' || t == 'P'))))
        break;
      t = c;
      PEEKC(c, p);
    }
    /* We add a trailing '\0' to ease parsing */
    cstr_ccat(&tokcstr, '\0');
    tokc.str.size = tokcstr.size;
    tokc.str.data = tokcstr.data;
    tok = TOK_PPNUM;
    break;

  case '.':
    /* special dot handling because it can also start a number */
    PEEKC(c, p);
    if (isnum(c))
    {
      t = '.';
      goto parse_num;
    }
    else if ((isidnum_table['.' - CH_EOF] & IS_ID) && (isidnum_table[c - CH_EOF] & (IS_ID | IS_NUM)))
    {
      *--p = c = '.';
      goto parse_ident_fast;
    }
    else if (c == '.')
    {
      PEEKC(c, p);
      if (c == '.')
      {
        p++;
        tok = TOK_DOTS;
      }
      else
      {
        *--p = '.'; /* may underflow into file->unget[] */
        tok = '.';
      }
    }
    else
    {
      tok = '.';
    }
    break;
  case '\'':
  case '\"':
    is_long = 0;
  str_const:
    cstr_reset(&tokcstr);
    if (is_long)
      cstr_ccat(&tokcstr, 'L');
    cstr_ccat(&tokcstr, c);
    p = parse_pp_string(p, c, &tokcstr);
    cstr_ccat(&tokcstr, c);
    cstr_ccat(&tokcstr, '\0');
    tokc.str.size = tokcstr.size;
    tokc.str.data = tokcstr.data;
    tok = TOK_PPSTR;
    break;

  case '<':
    PEEKC(c, p);
    if (c == '=')
    {
      p++;
      tok = TOK_LE;
    }
    else if (c == '<')
    {
      PEEKC(c, p);
      if (c == '=')
      {
        p++;
        tok = TOK_A_SHL;
      }
      else
      {
        tok = TOK_SHL;
      }
    }
    else
    {
      tok = TOK_LT;
    }
    break;
  case '>':
    PEEKC(c, p);
    if (c == '=')
    {
      p++;
      tok = TOK_GE;
    }
    else if (c == '>')
    {
      PEEKC(c, p);
      if (c == '=')
      {
        p++;
        tok = TOK_A_SAR;
      }
      else
      {
        tok = TOK_SAR;
      }
    }
    else
    {
      tok = TOK_GT;
    }
    break;

  case '&':
    PEEKC(c, p);
    if (c == '&')
    {
      p++;
      tok = TOK_LAND;
    }
    else if (c == '=')
    {
      p++;
      tok = TOK_A_AND;
    }
    else
    {
      tok = '&';
    }
    break;

  case '|':
    PEEKC(c, p);
    if (c == '|')
    {
      p++;
      tok = TOK_LOR;
    }
    else if (c == '=')
    {
      p++;
      tok = TOK_A_OR;
    }
    else
    {
      tok = '|';
    }
    break;

  case '+':
    PEEKC(c, p);
    if (c == '+')
    {
      p++;
      tok = TOK_INC;
    }
    else if (c == '=')
    {
      p++;
      tok = TOK_A_ADD;
    }
    else
    {
      tok = '+';
    }
    break;

  case '-':
    PEEKC(c, p);
    if (c == '-')
    {
      p++;
      tok = TOK_DEC;
    }
    else if (c == '=')
    {
      p++;
      tok = TOK_A_SUB;
    }
    else if (c == '>')
    {
      p++;
      tok = TOK_ARROW;
    }
    else
    {
      tok = '-';
    }
    break;

    PARSE2('!', '!', '=', TOK_NE)
    PARSE2('=', '=', '=', TOK_EQ)
    PARSE2('*', '*', '=', TOK_A_MUL)
    PARSE2('%', '%', '=', TOK_A_MOD)
    PARSE2('^', '^', '=', TOK_A_XOR)

    /* comments or operator */
  case '/':
    PEEKC(c, p);
    if (c == '*')
    {
      uint8_t *start = p + 1;
      unsigned int read_before = total_bytes, refills = ft_refills;
      p = parse_comment(p);
      /* a comment that crossed one buffer refill: the old buffer's tail was kept */
      if ((tcc_state->warn_implicit_fallthrough | tcc_diag_snaps_on(warn_implicit_fallthrough)) && read_before == total_bytes &&
          p - 2 >= start)
        fallthrough_comment(start, p - 2, p);
      else if ((tcc_state->warn_implicit_fallthrough | tcc_diag_snaps_on(warn_implicit_fallthrough)) && ft_refills == refills + 1 &&
               p > file->buffer)
        ft_comment_across(start, p - 2, p);
      /* comments replaced by a blank */
      tok = ' ';
      goto maybe_space;
    }
    else if (c == '/')
    {
      uint8_t *start = p + 1;
      unsigned int read_before = total_bytes, refills = ft_refills;
      p = parse_line_comment(p);
      if ((tcc_state->warn_implicit_fallthrough | tcc_diag_snaps_on(warn_implicit_fallthrough)) && read_before == total_bytes &&
          p >= start)
        fallthrough_comment(start, p, p);
      else if ((tcc_state->warn_implicit_fallthrough | tcc_diag_snaps_on(warn_implicit_fallthrough)) && ft_refills == refills + 1 &&
               p >= file->buffer)
        ft_comment_across(start, p, p);
      tok = ' ';
      goto maybe_space;
    }
    else if (c == '=')
    {
      p++;
      tok = TOK_A_DIV;
    }
    else
    {
      tok = '/';
    }
    break;

    /* simple tokens */
  case '(':
  case ')':
  case '[':
  case ']':
  case '{':
  case '}':
  case ',':
  case ';':
  case ':':
  case '?':
  case '~':
  parse_simple:
    tok = c;
    p++;
    break;
  case '@': /* only used in assembler */
#if defined(TCC_TARGET_ARM)
    /* GAS for ARM reads '@' as a line comment ('#' is the immediate prefix
       there, and types are spelled %function). */
    if (parse_flags & PARSE_FLAG_ASM_FILE)
    {
      p = parse_line_comment(p);
      goto redo_no_start;
    }
#endif
    goto parse_simple;
  default:
    if (c >= 0x80 && c <= 0xFF) /* utf8 identifiers */
      goto parse_ident_fast;
    if (parse_flags & PARSE_FLAG_ASM_FILE)
      goto parse_simple;
    file->buf_ptr = p + 1; /* past it: the recovery reads on */
    if (tcc_lex_error_deferred("unrecognized character \\x%02x", c))
    {
      p++; /* in a saved body: said where the parse reaches it */
      goto redo_no_start;
    }
    break;
  }
  tok_flags = 0;
keep_tok_flags:
  file->buf_ptr = p;
#if defined(PARSE_DEBUG)
  printf("token = %d %s\n", tok, get_tok_str(tok, &tokc));
#endif
}

/* yasc: a pointer's words go against their '*', one per '*' -- `*owned`,
   `*?borrows(s)`, `*@(const, owned)` -- and `?` is what `maybe` was
   (docs/yasc_cheatsheet.md).  Read here, as the file is lexed, so the parser
   and everything that keeps tokens (saved bodies, macros, interfaces) sees
   the words as before: `?` after a '*' or a '[', or against an int handle's
   owned / view (`int ?owned(close) fd`), is TOK_maybe (spelled "?");
   `*@(a, b)` is `* a b`.  Whitespace between a '*' and its first word, or a
   second word outside @( ), is an error.  Not for tcc -E: it writes what it
   reads. */
enum
{
  YG_IDLE,
  YG_STAR,  /* after a '*' */
  YG_MAYBE, /* after a '*?' (or a handle's '?') */
  YG_CQUAL, /* after a '*' and C qualifiers written as C writes them: `* const` */
  YG_WORD,  /* after a '*''s word (or its group) */
  YG_ARGS,  /* in that word's ( ) */
  YG_GROUP  /* in *@( ) */
};
static int yg_state, yg_depth, yg_word, yg_space, yg_prev;
static BufferedFile *yg_file;

static int yg_is_cqual(int t)
{
  return (t >= TOK_CONST1 && t <= TOK_CONST3) || (t >= TOK_VOLATILE1 && t <= TOK_VOLATILE3) ||
         (t >= TOK_RESTRICT1 && t <= TOK_RESTRICT3) || t == TOK__Atomic;
}

static int yg_is_word(int t)
{
  return t == TOK_owned || t == TOK_view || t == TOK_borrows || t == TOK_takes || t == TOK_c_api || yg_is_cqual(t);
}

/* the source at P starts with WORD, a whole identifier */
static int yg_at_word(const uint8_t *p, const char *word)
{
  size_t n = strlen(word);
  return !memcmp(p, word, n) && !(isidnum_table[p[n] - CH_EOF] & (IS_ID | IS_NUM));
}

/* 1: the token just read is left out (a group's '@(', ',' or ')') */
/* a macro's body (read with its spaces) is not held to the spacing: its
   expansion is not, either */
#define YG_ERROR(...) ((parse_flags & PARSE_FLAG_SPACES) ? 0 : tcc_lex_error_deferred(__VA_ARGS__))

static int yasc_glue(int glued)
{
  const int t = tok;
  int prev;
  if (t == ' ' || t == TOK_LINEFEED)
  {
    yg_space = 1; /* -E's spaces, a directive's end: what follows is not against it */
    if (t == TOK_LINEFEED)
      yg_state = YG_IDLE;
    return 0;
  }
  glued = glued && !yg_space;
  yg_space = 0;
  prev = yg_prev;
  yg_prev = t;
  switch (yg_state)
  {
  case YG_GROUP:
    if (t == '(')
      yg_depth++;
    else if (t == ')' && --yg_depth == 0)
    {
      yg_state = YG_WORD, yg_word = 0;
      return 1;
    }
    else if (t == ',' && yg_depth == 1)
      return 1;
    else if (t == '?')
      YG_ERROR("'?' in *@( ): write it against the '*', before the '@': '*?@(...)'");
    return 0;
  case YG_ARGS:
    if (t == '(')
      yg_depth++;
    else if (t == ')' && --yg_depth == 0)
      yg_state = YG_WORD;
    return 0;
  }
  if (yg_is_word(t))
  {
    /* C's own qualifiers may stand as C writes them (`char * const p`, a C
       file read in the default dialect); a yasc word after them goes in the
       group with them */
    if (yg_is_cqual(t) && !glued && (yg_state == YG_STAR || yg_state == YG_CQUAL))
    {
      yg_state = YG_CQUAL, yg_word = t;
      return 0;
    }
    if (yg_state == YG_CQUAL)
    {
      if (!yg_is_cqual(t))
        YG_ERROR("a pointer's words go in @( ) with its C qualifiers: write '*@(%s, %s)'", get_tok_str(yg_word, NULL),
                 get_tok_str(t, NULL));
      yg_state = YG_WORD, yg_word = t;
      return 0;
    }
    if (yg_state == YG_STAR || yg_state == YG_MAYBE)
    {
      if (!glued)
        YG_ERROR("a pointer's word goes against its '%s': write '%s%s'", yg_state == YG_STAR ? "*" : "*?",
                               yg_state == YG_STAR ? "*" : "*?", get_tok_str(t, NULL));
      yg_state = YG_WORD, yg_word = t;
      return 0;
    }
    if (yg_state == YG_WORD)
    {
      if (yg_is_cqual(t) && yg_is_cqual(yg_word))
        return 0; /* `char *const volatile p`: C's qualifier list */
      if (yg_word)
        YG_ERROR("one word against a '*': write '*@(%s, %s)'", get_tok_str(yg_word, NULL),
                               get_tok_str(t, NULL));
      else
        YG_ERROR("one word against a '*': write '%s' in its @( )", get_tok_str(t, NULL));
      yg_word = t;
      return 0;
    }
    yg_state = YG_IDLE;
    return 0;
  }
  switch (t)
  {
  case '*':
    yg_state = YG_STAR;
    return 0;
  case '?':
    if (yg_state == YG_STAR)
    {
      if (!glued)
        YG_ERROR("'?' goes against its '*': write '*?'");
      tok = TOK_maybe;
      yg_prev = TOK_maybe;
      yg_state = YG_MAYBE;
      return 0;
    }
    if (prev == '[' && glued)
    {
      tok = yg_prev = TOK_maybe; /* `int a[?4]`: the pointer the array becomes */
      yg_state = YG_IDLE;
      return 0;
    }
    if (yg_at_word(file->buf_ptr, "owned") || yg_at_word(file->buf_ptr, "view"))
    {
      tok = yg_prev = TOK_maybe; /* `int ?owned(close) fd`: never a ?: in yasc */
      yg_state = YG_MAYBE;
      return 0;
    }
    break;
  case '@':
    if ((yg_state == YG_STAR || yg_state == YG_MAYBE) && *file->buf_ptr == '(')
    {
      if (!glued)
        YG_ERROR("a pointer's words go against its '*': write '%s@('", yg_state == YG_STAR ? "*" : "*?");
      file->buf_ptr++;
      yg_state = YG_GROUP, yg_depth = 1;
      return 1;
    }
    if (yg_state == YG_STAR || yg_state == YG_MAYBE)
    {
      /* `a * @min(x, y)` is a builtin's call */
      const uint8_t *q = file->buf_ptr;
      while (*q == ' ' || *q == '\t')
        q++;
      if (*q == '(')
        YG_ERROR("a pointer's words: '%s@(a, b)', the '(' against the '@'",
                               yg_state == YG_STAR ? "*" : "*?");
    }
    break;
  case '(':
    if (yg_state == YG_WORD && glued &&
        (yg_word == TOK_owned || yg_word == TOK_borrows || yg_word == TOK_takes || yg_word == TOK_c_api))
    {
      yg_state = YG_ARGS, yg_depth = 1;
      return 0;
    }
    break;
  }
  yg_state = YG_IDLE;
  return 0;
}

/* noinline: next() sits on the parser's recursive paths and the compiler
   would take this into it (tests/selfhost/test_parser_stack_frames.py) */
__attribute__((noinline)) static void next_nomacro_yasc(void)
{
  TCCState *s1 = tcc_state;
  do
  {
    BufferedFile *f = file;
    uint8_t *p0 = file->buf_ptr;
    next_nomacro_raw();
    if (pp_in_c() || !s1->yasc)
    {
      yg_state = YG_IDLE;
      return;
    }
    if (file != yg_file)
      yg_file = file, yg_state = YG_IDLE;
    if (yasc_glue(f == file && lex_tok_start == p0))
      continue;
    return;
  } while (1);
}

static void next_nomacro(void)
{
  TCCState *s1 = tcc_state;
  if (s1->yasc && s1->output_type != TCC_OUTPUT_PREPROCESS && !(parse_flags & PARSE_FLAG_ASM_FILE))
  {
    next_nomacro_yasc();
    return;
  }
  yg_state = YG_IDLE;
  next_nomacro_raw();
}

#ifdef PP_DEBUG
static int indent;
static void define_print(TCCState *s1, int v);
static void pp_print(const char *msg, int v, const int *str)
{
  FILE *fp = tcc_state->ppfp;

  if (msg[0] == '#' && indent == 0)
    fprintf(fp, "\n");
  else if (msg[0] == '+')
    ++indent, ++msg;
  else if (msg[0] == '-')
    --indent, ++msg;

  fprintf(fp, "%*s", indent, "");
  if (msg[0] == '#')
  {
    define_print(tcc_state, v);
  }
  else
  {
    tok_print(str, v ? "%s %s" : "%s", msg, get_tok_str(v, 0));
  }
}
#define PP_PRINT(x) pp_print x
#else
#define PP_PRINT(x)
#endif

typedef struct MacroArg
{
  int v;
  unsigned char is_vaargs;
  int *d;
  int *e;
  PPSys *dx, *ex; /* which of d's and e's tokens a system macro spelled */
} MacroArg;

static MacroArg *macro_arg_find(MacroArg *args, int nb_args, int tok)
{
  int i;

  for (i = 0; i < nb_args; ++i)
  {
    if (args[i].v == tok)
      return &args[i];
  }
  return NULL;
}

/* substitute arguments in replacement lists in macro_str by the values in
   args (field d) and return allocated string */
static int macro_subst(TokenString *tok_str, Sym **nested_list, const int *macro_str, const PPSys *in);

static int *macro_arg_subst(Sym **nested_list, const int *macro_str, MacroArg *args, int nb_args, int body_sys,
                            PPSys **out)
{
  int t, t0, t1, t2, n;
  const int *st;
  MacroArg *arg;
  CValue cval;
  TokenString str;

#ifdef PP_DEBUG
  PP_PRINT(("asubst:", 0, macro_str));
  for (n = 0; n < nb_args; ++n)
  {
    tok_print(args[n].d, "%*s - arg: %s:", indent, "", get_tok_str(args[n].v, 0));
  }
#endif

  tok_str_new(&str);
  t0 = t1 = 0;
  while (1)
  {
    TOK_GET(&t, &macro_str, &cval);
    if (!t)
      break;
    if (t == '#')
    {
      /* stringize */
      do
        t = *macro_str++;
      while (t == ' ');
      arg = macro_arg_find(args, nb_args, t);
      if (arg)
      {
        cstr_reset(&tokcstr);
        cstr_ccat(&tokcstr, '\"');
        st = arg->d;
        while (*st != TOK_EOF)
        {
          const char *s;
          TOK_GET(&t, &st, &cval);
          s = get_tok_str(t, &cval);
          while (*s)
          {
            if (t == TOK_PPSTR && *s != '\'')
              add_char(&tokcstr, *s);
            else
              cstr_ccat(&tokcstr, *s);
            ++s;
          }
        }
        cstr_ccat(&tokcstr, '\"');
        cstr_ccat(&tokcstr, '\0');
        // printf("\nstringize: <%s>\n", (char *)tokcstr.data);
        /* add string */
        cval.str.size = tokcstr.size;
        cval.str.data = tokcstr.data;
        if (body_sys || str.xs)
          xs_mark(&str, body_sys);
        tok_str_add2(&str, TOK_PPSTR, &cval);
      }
      else
      {
        expect("macro parameter after '#'");
      }
    }
    else if (t >= TOK_IDENT)
    {
      arg = macro_arg_find(args, nb_args, t);
      if (arg)
      {
        const PPSys *sx = arg->dx;
        st = arg->d;
        n = 0;
        while ((t2 = macro_str[n]) == ' ')
          ++n;
        /* if '##' is present before or after, no arg substitution */
        if (t2 == TOK_PPJOIN || t1 == TOK_PPJOIN)
        {
          /* special case for var arg macros : ## eats the ','
             if empty VA_ARGS variable. */
          if (t1 == TOK_PPJOIN && t0 == ',' && gnu_ext && arg->is_vaargs)
          {
            int *str_buf = tok_str_buf(&str);
            int c = str_buf[str.len - 1];
            while (str_buf[--str.len] != ',')
              ;
            xs_trunc(&str);
            if (*st == TOK_EOF)
            {
              /* suppress ',' '##' */
            }
            else
            {
              /* suppress '##' and add variable */
              str.len++;
              str_buf = tok_str_buf(&str);
              if (c == ' ')
                str_buf[str.len++] = c;
              goto add_var;
            }
          }
          else
          {
            if (*st == TOK_EOF)
            {
              if (body_sys || str.xs)
                xs_mark(&str, body_sys);
              tok_str_add(&str, TOK_PLCHLDR);
            }
          }
        }
        else
        {
        add_var:
          if (!arg->e)
          {
            /* Expand arguments tokens and store them.  In most
               cases we could also re-expand each argument if
               used multiple times, but not if the argument
               contains the __COUNTER__ macro.  */
            TokenString str2;
            tok_str_new(&str2);
            macro_subst(&str2, nested_list, st, arg->dx);
            tok_str_add(&str2, TOK_EOF);
            arg->e = tok_str_ensure_heap(&str2);
            arg->ex = str2.xs;
          }
          st = arg->e;
          sx = arg->ex;
        }
        {
          const int at = str.len;
          tok_str_add_tokstream(&str, st);
          if (sx || str.xs)
            xs_copy(&str, at, sx, str.len - at);
        }
      }
      else
      {
        if (body_sys || str.xs)
          xs_mark(&str, body_sys);
        tok_str_add(&str, t);
      }
    }
    else
    {
      if (body_sys || str.xs)
        xs_mark(&str, body_sys);
      tok_str_add2(&str, t, &cval);
    }
    if (t != ' ')
      t0 = t1, t1 = t;
  }
  tok_str_add(&str, 0);
  tok_str_shrink(&str);
  PP_PRINT(("areslt:", 0, tok_str_buf(&str)));
  *out = str.xs;
  return tok_str_ensure_heap(&str);
}

/* handle the '##' operator. return the resulting string (which must be freed).
 */
static inline int *macro_twosharps(const int *ptr0, const PPSys *in, PPSys **out)
{
  int t1, t2, n, l;
  CValue cv1, cv2;
  TokenString macro_str1;
  const int *ptr;

  tok_str_new(&macro_str1);
  cstr_reset(&tokcstr);
  for (ptr = ptr0;;)
  {
    /* a token pasted from several is spelled where the first was */
    const int sys = in ? xs_state(in, (int)(ptr - ptr0)) : 0;
    TOK_GET(&t1, &ptr, &cv1);
    if (t1 == 0)
      break;
    for (;;)
    {
      n = 0;
      while ((t2 = ptr[n]) == ' ')
        ++n;
      if (t2 != TOK_PPJOIN)
        break;
      ptr += n;
      while ((t2 = *++ptr) == ' ' || t2 == TOK_PPJOIN)
        ;
      TOK_GET(&t2, &ptr, &cv2);
      if (t2 == TOK_PLCHLDR)
        continue;
      if (t1 != TOK_PLCHLDR)
      {
        cstr_cat(&tokcstr, get_tok_str(t1, &cv1), -1);
        t1 = TOK_PLCHLDR;
      }
      cstr_cat(&tokcstr, get_tok_str(t2, &cv2), -1);
    }
    if (tokcstr.size)
    {
      cstr_ccat(&tokcstr, 0);
      tcc_open_bf(tcc_state, ":paste:", tokcstr.size);
      memcpy(file->buffer, tokcstr.data, tokcstr.size);
      tok_flags = 0; /* don't interpret '#' */
      for (n = 0;; n = l)
      {
        next_nomacro();
        if (sys || macro_str1.xs)
          xs_mark(&macro_str1, sys);
        tok_str_add2(&macro_str1, tok, &tokc);
        if (*file->buf_ptr == 0)
          break;
        tok_str_add(&macro_str1, ' ');
        l = file->buf_ptr - file->buffer;
        tcc_warning("pasting \"%.*s\" and \"%s\" does not give a valid"
                    " preprocessing token",
                    l - n, file->buffer + n, file->buf_ptr);
      }
      tcc_close();
      cstr_reset(&tokcstr);
    }
    if (t1 != TOK_PLCHLDR)
    {
      if (sys || macro_str1.xs)
        xs_mark(&macro_str1, sys);
      tok_str_add2(&macro_str1, t1, &cv1);
    }
  }
  tok_str_add(&macro_str1, 0);
  PP_PRINT(("pasted:", 0, tok_str_buf(&macro_str1)));
  *out = macro_str1.xs;
  return tok_str_ensure_heap(&macro_str1);
}

static int peek_file(TokenString *ws_str)
{
  uint8_t *p = file->buf_ptr - 1;
  int c;
  for (;;)
  {
    PEEKC(c, p);
    switch (c)
    {
    case '/':
      PEEKC(c, p);
      if (c == '*')
        p = parse_comment(p);
      else if (c == '/')
        p = parse_line_comment(p);
      else
      {
        c = *--p = '/';
        goto leave;
      }
      --p, c = ' ';
      break;
    case ' ':
    case '\t':
      break;
    case '\f':
    case '\v':
    case '\r':
      continue;
    case '\n':
      file->line_num++, tok_flags |= TOK_FLAG_BOL;
      break;
    default:
    leave:
      file->buf_ptr = p;
      return c;
    }
    if (ws_str)
      tok_str_add(ws_str, c);
  }
  return 0; /* unreachable */
}

/* the token next_argstream read last was spelled in a system macro */
static int pp_arg_sys;
/* a system macro's tokens are told apart: some warning may be given and
   be quiet there (not under -w, nor for tcc -E) */
static int pp_sys_track;
/* the macro name macro_subst_tok expands was (its caller says) */
static int pp_name_sys;

/* peek or read [ws_str == NULL] next token from function macro call,
   walking up macro levels up to the file if necessary */
static int next_argstream(Sym **nested_list, TokenString *ws_str)
{
  int t;
  Sym *sa;

  while (macro_ptr)
  {
    const int *m = macro_ptr;
    while ((t = *m) != 0)
    {
      if (ws_str)
      {
        if (t != ' ')
          return t;
        ++m;
      }
      else
      {
        /* spelled in a system macro? (the string viewed says) */
        pp_arg_sys = macro_stack->xv ? xs_state(macro_stack->xv, (int)(macro_ptr - macro_stack->xv_base)) : 0;
        TOK_GET(&tok, &macro_ptr, &tokc);
        return tok;
      }
    }
    end_macro();
    /* also, end of scope for nested defined symbol */
    sa = *nested_list;
    if (sa)
      *nested_list = sa->prev, sym_free_short(sa);
  }
  if (ws_str)
  {
    return peek_file(ws_str);
  }
  else
  {
    pp_arg_sys = 0;
    next_nomacro();
    if (tok == TOK_EMBED_RESUME)
      tcc_error("#embed inside macro arguments is not supported");
    if (tok == '\t' || tok == TOK_LINEFEED)
      tok = ' ';
    return tok;
  }
  return 0; /* unreachable */
}

static int macro_subst_tok_1(TokenString *tok_str, Sym **nested_list, Sym *s);

/* Expanding regular-C macros inside yasc code (depth, outermost first) */
static int pp_c_macro_depth;

static int yasc_tok_matters(int t)
{
  /* owned .. error_name: the qualifiers and statements, consecutive in tcctok.h */
  return t == '*' || (t >= TOK_owned && t <= TOK_error_name);
}

/* T, after a '(': a type starts there -- a C cast, which yasc spells
   cast<...>(x): the macro's own is regular C */
static int yasc_tok_type_start(int t)
{
  Sym *s;
  /* (void)x is yasc too */
  if ((t >= TOK_UNSIGNED && t <= TOK_SIGNED3) || (t > TOK_VOID && t <= TOK_ENUM))
    return 1;
  return t >= TOK_UIDENT && (s = sym_find(t)) && (s->type.t & VT_TYPEDEF);
}

/* do macro substitution of current token with macro 's' and add
   result to (tok_str,tok_len). 'nested_list' is the list of all
   macros we got inside to avoid recursing. Return non zero if no
   substitution needs to be done.

   A macro defined in regular C (a system header's NULL, `(T *)0` casts, a
   parameter named `view`) expanded in yasc code reads as regular C: the
   expansion goes between two TOK_PACK_REPLAY "yasc off" / "yasc on" markers,
   which next() applies and saved bodies keep.  Only when it holds a token the
   dialect reads differently ('*' or a yasc keyword): the markers lengthen
   saved bodies, which the inliner sizes by tokens. */
static int macro_subst_tok(TokenString *tok_str, Sym **nested_list, Sym *s)
{
  int c_macro = tcc_state->yasc && !(s->type.t & MACRO_YASC) && !(parse_flags & PARSE_FLAG_ASM_FILE);
  int outer = c_macro && !pp_c_macro_depth, start = tok_str->len, ret;
  /* yasc writes the null pointer nullptr: NULL is regular C's.  Said where
     yasc code spelled it -- also in a macro's argument or a header of its
     own, as a 0 used as a pointer is -- not in a system header's macro
     (SIG_DFL, the body of one that compares with NULL) */
  if (c_macro && s->v == TOK_NULL && tcc_state->output_type != TCC_OUTPUT_PREPROCESS &&
      tcc_warn_wanted(warn_zero_as_null_pointer_constant) &&
      !(pp_subst_depth ? pp_name_sys : tcc_sys_macro_here())) /* rescanning an expansion: its caller knows */
    tcc_warning_once(warn_zero_as_null_pointer_constant, "NULL is regular C: write nullptr");
  pp_c_macro_depth += c_macro;
  pp_subst_depth++;
  ret = macro_subst_tok_1(tok_str, nested_list, s);
  pp_subst_depth--;
  pp_c_macro_depth -= c_macro;
  if (outer && tok_str->len > start)
  {
    const int *p = tok_str_buf(tok_str) + start, *end = tok_str_buf(tok_str) + tok_str->len;
    int matters = 0, prev = 0;
    while (p < end && !matters)
    {
      int t;
      CValue cv;
      tok_get(&t, &p, &cv);
      t &= ~SYM_FIELD;
      matters = yasc_tok_matters(t) || (prev == '(' && yasc_tok_type_start(t));
      prev = t;
    }
    if (matters)
    {
      /* move the expansion up one marker: [off] expansion [on] */
      CValue cv;
      tok_str_insert_yasc_mark(tok_str, start, 0);
      cv.i = (TCC_PCH_REPLAY_YASC << 16) | 1;
      tok_str_add2(tok_str, TOK_PACK_REPLAY, &cv);
    }
  }
  return ret;
}

/* A function-like macro used with the wrong number of arguments: the
   invocation is read to its ')', and its expansion is the error's marker
   (tcc_error_stash) -- said where the parser reads it, which goes on after
   the statement as after any error; the substitution leaves nothing half
   done.  As gcc: report, skip the invocation, go on. */
/* the arguments read so far (and the one being read, STR) of an invocation
   that ends in an error */
static void macro_args_free(MacroArg *args, int nb, TokenString *str)
{
  for (int i = 0; i < nb; ++i)
  {
    tok_str_free_str(args[i].d);
    xs_free(args[i].dx);
  }
  tcc_free(args);
  if (str)
  {
    if (str->allocated_len > 0)
      tok_str_free_str(str->data.str);
    xs_free(str->xs);
    str->xs = NULL;
  }
}

/* "EOF in invocation of macro": an error the parse goes on after
   (core/recover.c), so what the invocation read is freed first */
static NORETURN void macro_args_eof(MacroArg *args, int nb, TokenString *str, int v)
{
  macro_args_free(args, nb, str);
  tcc_error("EOF in invocation of macro '%s'", get_tok_str(v, 0));
}

static int macro_args_error(TokenString *tok_str, MacroArg *args, int nb, int saved_parse_flags, const char *fmt,
                            const char *name)
{
  CValue cv;
  macro_args_free(args, nb, NULL);
  parse_flags = saved_parse_flags;
  cv.i = tcc_error_stash(fmt, name);
  tok_str_add2(tok_str, TOK_PACK_REPLAY, &cv);
  return 0;
}

static int macro_subst_tok_1(TokenString *tok_str, Sym **nested_list, Sym *s)
{
  int t;
  int v = s->v;
  /* where its name and its body were spelled */
  const int name_sys = pp_name_sys, body_sys = (s->type.t & MACRO_SYS) && pp_sys_track;

  PP_PRINT(("#", v, s->d));
  if (s->d)
  {
    int *mstr = s->d;
    PPSys *mx = body_sys ? (PPSys *)&xs_all : NULL, *jx;

    int *jstr;
    Sym *sa;
    int ret;

    if (s->type.t & MACRO_FUNC)
    {
      int saved_parse_flags = parse_flags;
      TokenString str;
      int arg_index, nb_args, parlevel, i;
      MacroArg *args;
      Sym *param;

      parse_flags |= PARSE_FLAG_SPACES | PARSE_FLAG_LINEFEED | PARSE_FLAG_ACCEPT_STRAYS;

      tok_str_new(&str);
      /* peek next token from argument stream */
      t = next_argstream(nested_list, &str);
      if (t != '(')
      {
        /* not a macro substitution after all, restore the
         * macro token plus all whitespace we've read.
         * whitespace is intentionally not merged to preserve
         * newlines. */
        parse_flags = saved_parse_flags;
        if (name_sys || tok_str->xs)
          xs_mark(tok_str, name_sys);
        tok_str_add2_spc(tok_str, v, 0);
        if (parse_flags & PARSE_FLAG_SPACES)
          tok_str_add_words(tok_str, tok_str_buf(&str), str.len);
        if (str.allocated_len > 0)
          tok_str_free_str(str.data.str);
        return 0;
      }
      else
      {
        if (str.allocated_len > 0)
          tok_str_free_str(str.data.str);
      }

      /* argument macro */
      nb_args = 0;
      for (param = s->next; param; param = param->next)
        ++nb_args;
      args = nb_args ? tcc_mallocz(nb_args * sizeof(*args)) : NULL;
      sa = s->next;
      arg_index = 0;
      /* NOTE: empty args are allowed, except if no args */
      i = 2; /* eat '(' */
      for (;;)
      {
        do
        {
          t = next_argstream(nested_list, NULL);
        } while (t == ' ' || --i);

        if (!sa)
        {
          if (t == ')') /* handle '()' case */
            break;
          /* the rest of the invocation is read; its expansion is the
             error's marker (macro_args_error) */
          for (parlevel = 0; parlevel > 0 || t != ')'; t = next_argstream(nested_list, NULL))
          {
            if (t == TOK_EOF)
              macro_args_eof(args, arg_index, NULL, v);
            parlevel += (t == '(') - (t == ')');
          }
          return macro_args_error(tok_str, args, arg_index, saved_parse_flags, "macro '%s' used with too many args",
                                  get_tok_str(v, 0));
        }
      empty_arg:
        tok_str_new(&str);
        parlevel = 0;
        /* NOTE: non zero sa->type.t indicates VA_ARGS */
        while (parlevel > 0 || (t != ')' && (t != ',' || sa->type.t)))
        {
          if (t == TOK_EOF)
            macro_args_eof(args, arg_index, &str, v);
          if (t == '(')
            parlevel++;
          if (t == ')')
            parlevel--;
          if (t == ' ')
            str.need_spc |= 1;
          else
          {
            if (pp_arg_sys || str.xs)
              xs_mark(&str, pp_arg_sys);
            tok_str_add2_spc(&str, t, &tokc);
          }
          t = next_argstream(nested_list, NULL);
        }
        tok_str_add(&str, TOK_EOF);
        args[arg_index].v = sa->v & ~SYM_FIELD;
        args[arg_index].is_vaargs = sa->type.t != 0;
        args[arg_index].d = tok_str_ensure_heap(&str);
        args[arg_index].dx = str.xs;
        str.xs = NULL;
        arg_index++;
        sa = sa->next;
        if (t == ')')
        {
          if (!sa)
            break;
          /* special case for gcc var args: add an empty
             var arg argument if it is omitted */
          if (sa->type.t && gnu_ext)
            goto empty_arg;
          return macro_args_error(tok_str, args, arg_index, saved_parse_flags, "macro '%s' used with too few args",
                                  get_tok_str(v, 0));
        }
        i = 1;
      }

      /* now subst each arg */
      mstr = macro_arg_subst(nested_list, mstr, args, arg_index, body_sys, &mx);

      /* free memory */
      for (i = 0; i < arg_index; ++i)
      {
        tok_str_free_str(args[i].d);
        tok_str_free_str(args[i].e);
        xs_free(args[i].dx);
        xs_free(args[i].ex);
      }
      tcc_free(args);
      parse_flags = saved_parse_flags;
    }

    /* process '##'s (if present) */
    jstr = mstr;
    jx = mx;
    if (s->type.t & MACRO_JOIN)
      jstr = macro_twosharps(mstr, mx, &jx);

    sa = sym_push2_short(nested_list, v, 0, 0);
    ret = macro_subst(tok_str, nested_list, jstr, jx);
    /* pop nested defined symbol */
    if (sa == *nested_list)
      *nested_list = sa->prev, sym_free_short(sa);
    if (jstr != mstr)
      tok_str_free_str(jstr), xs_free(jx);
    if (mstr != s->d)
      tok_str_free_str(mstr), xs_free(mx);
    return ret;
  }
  else
  {
    CValue cval;
    char buf[32], *cstrval = buf;

    /* special macros */
    if (v == TOK___HAS_BUILTIN)
    {
      /* Anywhere, not only in #if: zig.h pastes the 0/1 answer into a name
       * (zig_expand_import_##has) to choose a libm import. */
      int saved_parse_flags = parse_flags, name;
      parse_flags |= PARSE_FLAG_SPACES | PARSE_FLAG_LINEFEED | PARSE_FLAG_ACCEPT_STRAYS;
      do
        t = next_argstream(nested_list, NULL);
      while (t == ' ' || t == TOK_LINEFEED);
      if (t != '(')
        expect("'(' after __has_builtin");
      do
        t = next_argstream(nested_list, NULL);
      while (t == ' ' || t == TOK_LINEFEED);
      name = t;
      do
        t = next_argstream(nested_list, NULL);
      while (t == ' ' || t == TOK_LINEFEED);
      parse_flags = saved_parse_flags;
      if (t != ')')
        expect("')' after the __has_builtin operand");
      snprintf(buf, sizeof(buf), "%d", name >= TOK_IDENT && tcc_builtin_supported(name));
      t = TOK_PPNUM;
      goto add_cstr1;
    }
    else if (v == TOK___LINE__ || v == TOK___COUNTER__)
    {
      t = v == TOK___LINE__ ? file->line_num : pp_counter++;
      snprintf(buf, sizeof(buf), "%d", t);
      t = TOK_PPNUM;
      goto add_cstr1;
    }
    else if (v == TOK___FILE__)
    {
      cstrval = file->filename;
      goto add_cstr;
    }
    else if (v == TOK___DATE__ || v == TOK___TIME__)
    {
      time_t ti;
      struct tm *tm;
      time(&ti);
      tm = localtime(&ti);
      if (v == TOK___DATE__)
      {
        static char const ab_month_name[12][4] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                                  "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
        snprintf(buf, sizeof(buf), "%s %2d %d", ab_month_name[tm->tm_mon], tm->tm_mday, tm->tm_year + 1900);
      }
      else
      {
        snprintf(buf, sizeof(buf), "%02d:%02d:%02d", tm->tm_hour, tm->tm_min, tm->tm_sec);
      }
    add_cstr:
      t = TOK_STR;
    add_cstr1:
      cval.str.size = strlen(cstrval) + 1;
      cval.str.data = cstrval;
      if (name_sys || tok_str->xs)
        xs_mark(tok_str, name_sys);
      tok_str_add2_spc(tok_str, t, &cval);
    }
    return 0;
  }
  /* unreachable - all branches above return, but TCC's flow analysis
     needs an explicit return to avoid 'function might return no value' */
  return 0;
}

/* do macro substitution of macro_str and add result to
   (tok_str,tok_len). 'nested_list' is the list of all macros we got
   inside to avoid recursing. */
static int macro_subst(TokenString *tok_str, Sym **nested_list, const int *macro_str, const PPSys *in)
{
  Sym *s;
  int t, nosubst = 0;
  CValue cval;
  TokenString macro_view;
  const int *const base = macro_str, *at;
  int xk = 0; /* in's toggles up to the token read now */

#ifdef PP_DEBUG
  int tlen = tok_str->len;
  PP_PRINT(("+expand:", 0, macro_str));
#endif

  while (1)
  {
    at = macro_str;
    TOK_GET(&t, &macro_str, &cval);
    if (t == 0 || t == TOK_EOF)
      break;
    if (in)
      while (xk < in->n && in->b[xk] <= (int)(at - base))
        xk++;
    if (t >= TOK_IDENT)
    {
      s = define_find(t);
      if (s == NULL || nosubst)
        goto no_subst;
      /* if nested substitution, do nothing */
      if (sym_find2(*nested_list, t))
      {
        /* and mark so it doesn't get subst'd again */
        t |= SYM_FIELD;
        goto no_subst;
      }
      tok_str_new(&macro_view);
      macro_view.data.str = (int *)macro_str; /* setup stream for possible arguments */
      macro_view.allocated_len = 1;           /* indicate heap buffer (read-only view) */
      macro_view.xv = in;                     /* its arguments' tokens are spelled where they were */
      macro_view.xv_base = base;
      begin_macro(&macro_view, 0);
      pp_name_sys = xk & 1;
      nosubst = macro_subst_tok(tok_str, nested_list, s);
      if (macro_stack != &macro_view)
      {
        /* already finished by reading function macro arguments */
        break;
      }
      macro_str = macro_ptr;
      end_macro();
    }
    else if (t == ' ')
    {
      if (parse_flags & PARSE_FLAG_SPACES)
        tok_str->need_spc |= 1;
    }
    else
    {
    no_subst:
      if (in || tok_str->xs)
        xs_mark(tok_str, xk & 1);
      tok_str_add2_spc(tok_str, t, &cval);
      if (nosubst && t != '(')
        nosubst = 0;
      /* GCC supports 'defined' as result of a macro substitution; the
         operand of __has_attribute is likewise an attribute name, not a
         macro to expand. */
      if ((t == TOK_DEFINED || t == TOK___HAS_ATTRIBUTE) && pp_expr)
        nosubst = 1;
    }
  }

#ifdef PP_DEBUG
  tok_str_add(tok_str, 0), --tok_str->len;
  PP_PRINT(("-result:", 0, tok_str_buf(tok_str) + tlen));
#endif
  return nosubst;
}

/* Lex the raw text 'text' (len bytes, no trailing NUL required) into the token
   stream 'out', preserving interior whitespace so the reconstructed directive
   keeps its original spacing.  Used to expand a destringized _Pragma operand
   into printable tokens for the -E rewrite.  A private input buffer is pushed
   and popped, leaving the surrounding lexer state untouched. */
static void pp_lex_into(TCCState *s1, TokenString *out, const char *text, int len)
{
  int saved_parse_flags = parse_flags;

  if (len <= 0)
    return;
  parse_flags = PARSE_FLAG_PREPROCESS | PARSE_FLAG_SPACES;
  tcc_open_bf(s1, ":pragma:", len + 1);
  memcpy(file->buffer, text, len);
  file->buffer[len] = 0;
  tok_flags = 0; /* don't interpret a leading '#' as a directive */
  for (;;)
  {
    next_nomacro();
    if (tok == TOK_EOF)
      break;
    tok_str_add2(out, tok, &tokc);
    if (*file->buf_ptr == 0)
      break;
  }
  tcc_close(); /* also restores tok_flags */
  parse_flags = saved_parse_flags;
}

/* Handle the C11 6.10.9 _Pragma operator.  On entry the current token is
   TOK__Pragma (which may itself have been produced by macro expansion, e.g.
   the common `#define DO_PRAGMA(x) _Pragma(#x)' idiom).  Parse the
   `( string-literal )' operand, destringize it, and act exactly as if the
   resulting text had appeared as a `#pragma' directive at this point: under
   -E rewrite it to `#pragma <text>' on its own line, otherwise run the real
   #pragma machinery so pack()/message/push_macro/... take effect. */
static void handle_pragma_operator(TCCState *s1)
{
  int saved_parse_flags = parse_flags;
  CString text;
  int len;

  /* Read the operand with next() so a macro-produced _Pragma consumes its
     parenthesized string from the same (macro) token stream it arrived on.
     Force TOK_STR conversion so the operand is decoded even under -E (which
     otherwise leaves strings as TOK_PPSTR); that decoding already collapses
     \" to " and \\ to \, i.e. exactly the destringizing the standard wants. */
  parse_flags = (parse_flags & ~PARSE_FLAG_ASM_FILE) | PARSE_FLAG_PREPROCESS | PARSE_FLAG_TOK_STR | PARSE_FLAG_TOK_NUM;
  next();
  if (tok != '(')
    goto err;
  next();
  if (tok != TOK_STR)
    goto err;
  /* Keep a private copy: reading ')' below reuses the shared token buffers. */
  cstr_new(&text);
  cstr_cat(&text, (char *)tokc.str.data, tokc.str.size - 1);
  len = text.size;
  cstr_ccat(&text, '\0');
  next();
  if (tok != ')')
  {
    cstr_free(&text);
    goto err;
  }
  parse_flags = saved_parse_flags;

  if (s1->output_type == TCC_OUTPUT_PREPROCESS && (!strcmp(text.data, "tcc c_begin") || !strcmp(text.data, "tcc c_end")))
  {
    /* tcc -E of tcc -E output: the marker stays where it is */
    pp_yasc_pragma(s1, text.data[6] == 'e');
    pp_yasc_mark(text.data[6] == 'e');
    cstr_free(&text);
    return;
  }
  if (s1->output_type == TCC_OUTPUT_PREPROCESS)
  {
    /* -E: rewrite to `#pragma <text>' on its own line for the tcc_preprocess()
       output loop.  Inject the directive as a throwaway macro string; the
       leading/trailing linefeeds keep it on a line of its own, mirroring the
       passthrough that pragma_parse() performs for real #pragma lines. */
    TokenString *inj = tok_str_alloc();
    tok_str_add(inj, TOK_LINEFEED);
    tok_str_add(inj, '#');
    tok_str_add(inj, TOK_PRAGMA);
    tok_str_add(inj, ' ');
    pp_lex_into(s1, inj, text.data, len);
    tok_str_add(inj, TOK_LINEFEED);
    tok_str_add(inj, 0);
    begin_macro(inj, 1);
    cstr_free(&text);
    return;
  }

  /* Real compile: feed `<text>\n' through the existing #pragma parser.  Push it
     as its own input buffer and isolate any active macro expansion so the
     directive body is read purely from that buffer, then restore. */
  {
    const int *saved_macro_ptr = macro_ptr;
    TokenString *saved_macro_stack = macro_stack;

    macro_ptr = NULL;
    macro_stack = NULL;
    parse_flags = PARSE_FLAG_PREPROCESS | PARSE_FLAG_TOK_NUM | PARSE_FLAG_TOK_STR | PARSE_FLAG_LINEFEED;
    /* the buffer's open and close set the dialect to the file's: inside a
       regular-C macro's expansion it is not (the markers), so it is kept --
       unless the pragma is one of those markers (pp_yasc_pragma) */
    const int saved_yasc = s1->yasc;
    pp_yasc_pragma_set = -1;
    tcc_open_bf(s1, ":pragma:", len + 1);
    memcpy(file->buffer, text.data, len);
    file->buffer[len] = '\n';
    tok_flags = 0;
    s1->yasc = saved_yasc;
    pragma_parse(s1);
    tcc_close(); /* also restores tok_flags */
    s1->yasc = pp_yasc_pragma_set >= 0 ? pp_yasc_pragma_set : saved_yasc;
    macro_ptr = saved_macro_ptr;
    macro_stack = saved_macro_stack;
    parse_flags = saved_parse_flags;
  }
  cstr_free(&text);
  return;

err:
  parse_flags = saved_parse_flags;
  tcc_error("_Pragma takes a parenthesized string literal");
}

/* the line of the token before the current one (yasc unsafe's line rule) */
ST_DATA int tok_prev_line;

/* return next token with macro substitution */
ST_FUNC HOT void next(void)
{
  int t;
  TCCState *s1 = tcc_state;

  tok_prev_line = file ? file->line_num : 0;

restart:
  while (macro_ptr)
  {
  redo:
    t = *macro_ptr;
    if (TOK_HAS_VALUE(t))
    {
      tok_get(&tok, &macro_ptr, &tokc);
      if (t == TOK_LINENUM)
      {
        file->line_num = tokc.i;
        goto redo;
      }
      if (t == TOK_PACK_REPLAY)
      {
        /* deferred #pragma pack action: apply it and stay invisible to the
           parser by fetching the next real token.  While a replayed body is
           itself being saved again (skip_or_save_block), keep the action in
           the new stream instead, as for a directive read from the file. */
        if (pp_pragma_capture && ((tokc.i >> 16) != TCC_PCH_REPLAY_ERROR || s1->save_body_jmp))
        {
          /* an error's marker goes on with a function body saved again,
             said where its parse reaches it; any other save says it now */
          tok_str_add2(pp_pragma_capture, TOK_PACK_REPLAY, &tokc);
          /* the yasc dialect is no pack state the definition must apply */
          if ((tokc.i >> 16) < TCC_PCH_REPLAY_YASC)
            pp_pack_captures++;
        }
        else
        {
          pp_apply_pack_replay(s1, tokc.i);
          if (s1->output_type == TCC_OUTPUT_PREPROCESS && (tokc.i >> 16) == TCC_PCH_REPLAY_YASC)
            pp_yasc_mark(tokc.i & 1); /* tcc -E writes it */
        }
        goto redo;
      }
      goto convert;
    }
    else if (t == 0)
    {
      /* end of macro or unget token string */
      if (macro_stack->sys && !macro_stack->prev)
      {
        /* the expansion ends on a system macro's token (its last mark: the
           markers and the end after it are not marked): the next expansion,
           if the file goes on with one, follows that token (pp_sys_tok) */
        pp_sys_end_pending = macro_stack->xs->n & 1;
      }
      end_macro();
      continue;
    }
    else if (t == TOK_EOF)
    {
      /* do nothing */
    }
    else
    {
      ++macro_ptr;
      t &= ~SYM_FIELD; /* remove 'nosubst' marker */
      if (t == '\\')
      {
        if (!(parse_flags & PARSE_FLAG_ACCEPT_STRAYS) && tcc_lex_error_deferred("stray '\\' in program"))
          goto restart; /* in a saved body: said where the parse reaches it */
      }
    }
    tok = t;
    goto check_pragma;
  }

  next_nomacro();
  t = tok;
  const int sys_end = pp_sys_end_pending;
  if (sys_end)
    pp_sys_end_set(); /* the token after a system macro's expansion */
  if (t >= TOK_IDENT && (parse_flags & PARSE_FLAG_PREPROCESS))
  {
    /* if reading from file, try to substitute macros */
    Sym *s = define_find(t);
    if (s)
    {
      Sym *nested_list = NULL;
      if (tokstr_buf.xs)
        tokstr_buf.xs->n = 0;
      pp_name_sys = 0;
      macro_subst_tok(&tokstr_buf, &nested_list, s);
      if (sys_end)
        pp_sys_end_set(); /* past its arguments */
      tok_str_add(&tokstr_buf, 0);
      begin_macro(&tokstr_buf, 0);
      /* tokens a system header's macro spelled: warnings about them are
         quiet (tcc_sys_macro_here) */
      tokstr_buf.sys = tokstr_buf.xs && tokstr_buf.xs->n;
      goto redo;
    }
    goto check_pragma;
  }
  goto convert;

check_pragma:
  /* C23's alignas, alignof, thread_local and constexpr are keywords: the
     C11 tokens (a macro of that name, stdalign.h's, has been expanded
     already), in C23 (as static_assert and bool); constexpr objects are const (not
     usable in constant expressions yet).  Not in #if, and not for tcc -E,
     which keeps the spelling. */
  if ((unsigned)(tok - TOK_alignas) <= TOK_constexpr - TOK_alignas && TCC_C23(s1) && !pp_expr &&
      s1->output_type != TCC_OUTPUT_PREPROCESS)
    tok = tok == TOK_alignas      ? TOK_ALIGNAS
          : tok == TOK_alignof    ? TOK_ALIGNOF3
          : tok == TOK_thread_local ? TOK_THREAD_LOCAL
                                  : TOK_CONST1;
  /* The C11 _Pragma operator is recognized here, after macro expansion, so
     that it also works when produced by a macro (e.g. DO_PRAGMA(x)). */
  if (tok == TOK__Pragma && (parse_flags & PARSE_FLAG_PREPROCESS))
  {
    handle_pragma_operator(s1);
    goto restart; /* fetch the token after the (now-processed) _Pragma */
  }
  if (fe_tokfp_on)
    tokfp_add();
  return;

convert:
  /* convert preprocessor tokens into C tokens */
  if (t == TOK_PPNUM)
  {
    if (parse_flags & PARSE_FLAG_TOK_NUM)
      parse_number(tokc.str.data);
  }
  else if (t == TOK_PPSTR)
  {
    if (parse_flags & PARSE_FLAG_TOK_STR)
      parse_string(tokc.str.data, tokc.str.size - 1);
  }
  else if (t == TOK_EMBED_RESUME)
  {
    goto restart; /* the #embed list is on the macro stack */
  }
  if (fe_tokfp_on)
    tokfp_add();
}

/* The number object-like macro S stands for when its body is one number,
   maybe in parentheses (an errno macro), else -1.  tok and tokc survive. */
static long long macro_number(Sym *s)
{
  const int saved_tok = tok;
  const CValue saved_tokc = tokc;
  const int *p;
  int t, depth = 0, numbers = 0, other = 0;
  long long value = -1;
  CValue cv;

  if (!s || (s->type.t & MACRO_FUNC) || !s->d) /* MACRO_YASC may be set */
    return -1;
  for (p = s->d; *p;)
  {
    tok_get(&t, &p, &cv);
    if (t == '(')
      depth++;
    else if (t == ')')
      depth--;
    else if (t == TOK_PPNUM)
    {
      parse_number(cv.str.data);
      if (tok == TOK_CINT || tok == TOK_CUINT || tok == TOK_CLONG || tok == TOK_CULONG)
        value = tokc.i, numbers++;
      else
        other = 1;
    }
    else if (t != TOK_LINENUM && t != ' ')
      other = 1;
  }
  tok = saved_tok;
  tokc = saved_tokc;
  return other || numbers != 1 || depth ? -1 : value;
}

/* yasc `error.NAME`, tok is the '.': read NAME without expanding it, so an
   errno name stays a name.  Returns the number NAME stands for when it is an
   object-like macro of one number (an errno macro), else -1; a number token
   (a saved body expanded it already) gives its value.  tok is NAME
   afterwards, unexpanded. */
ST_FUNC long long pp_error_name(void)
{
  const int saved_flags = parse_flags;

  parse_flags &= ~PARSE_FLAG_PREPROCESS;
  next();
  parse_flags = saved_flags;
  if (tok == TOK_CINT || tok == TOK_CUINT || tok == TOK_CLONG || tok == TOK_CULONG || tok == TOK_CLLONG ||
      tok == TOK_CULLONG)
    return tokc.i;
  if (tok >= TOK_IDENT)
    return macro_number(define_find(tok));
  return -1;
}

/* yasc error_name(): call CB for every errno macro defined now -- an
   E[A-Z0-9]+ object-like macro of one number in 1..255. */
ST_FUNC void pp_errno_macros(void (*cb)(int tok, int value))
{
  for (Sym *s = define_stack; s; s = s->prev)
  {
    const int v = s->v;
    long long n;
    const char *name;
    const char *c;
    if ((v & SYM_FIELD) || v < TOK_IDENT || define_find(v) != s)
      continue;
    /* the name first: evaluating every macro warns about the others'
       values (UINT64_MAX-like constants overflow) */
    name = get_tok_str(v, NULL);
    if (name[0] != 'E' || !name[1])
      continue;
    for (c = name + 1; (*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9'); c++)
      ;
    if (*c)
      continue;
    n = macro_number(s);
    if (n >= 1 && n <= 255)
      cb(v, (int)n);
  }
}

/* push back current token and set current token to 'last_tok'. Only
   identifier case handled for labels. */
ST_INLN void unget_tok(int last_tok)
{
  TokenString *str = &unget_buf;
  int alloc = 0;
  if (str->len) /* use static buffer except if already in use */
    str = tok_str_alloc(), alloc = 1;
  if (tok != TOK_EOF)
    tok_str_add2(str, tok, &tokc);
  tok_str_add(str, 0);
  begin_macro(str, alloc);
  tok = last_tok;
}

/* ------------------------------------------------------------------------- */
/* init preprocessor */

static const char *const target_os_defs =
#ifdef TCC_TARGET_PE
    "_WIN32\0"
#if PTR_SIZE == 8
    "_WIN64\0"
#endif
#else
#if defined TCC_TARGET_MACHO
    "__APPLE__\0"
#elif TARGETOS_FreeBSD
    "__FreeBSD__ 12\0"
#elif TARGETOS_FreeBSD_kernel
    "__FreeBSD_kernel__\0"
#elif TARGETOS_NetBSD
    "__NetBSD__\0"
#elif TARGETOS_OpenBSD
    "__OpenBSD__\0"
#elif TARGETOS_YasOS
    "__YasOS__\0"
    "__linux__\0"
    "__linux\0"
#else
    "__linux__\0"
    "__linux\0"
#if TARGETOS_ANDROID
    "__ANDROID__\0"
#endif

#if TARGETOS_YasOS
    "__yasos__\0"
#endif

#endif
    "__unix__\0"
    "__unix\0"
#endif
    ;

static void putdef(CString *cs, const char *p)
{
  cstr_printf(cs, "#define %s%s\n", p, &" 1"[!!strchr(p, ' ') * 2]);
}

static void putdefs(CString *cs, const char *p)
{
  while (*p)
    putdef(cs, p), p = strchr(p, 0) + 1;
}

/* TCC_NO_PROGRAMMATIC_DECLS forces the text path for the predefined
 * prototypes, for A/B validation against the programmatic (lazy) ones in
 * gen/decl/predef_protos.c. */
TCC_DBG_ENV_FLAG(pp_no_programmatic_decls, "TCC_NO_PROGRAMMATIC_DECLS")

#if !CONFIG_TCC_PREDEF_TABLE
/* Measurement-only: see the comment at its use site in tcc_predefs_base.
 *
 * Deliberately NOT a TCC_DBG_ENV_FLAG, and this is the interesting part: the
 * device compiler is built with -DCONFIG_TCC_DEBUG_ENV=0 (build_rootfs.sh),
 * which folds every knob declared that way to a compile-time constant.  So on
 * the RP2350 the "permanent A/B seam" TCC_NO_PROGRAMMATIC_DECLS is inert, and
 * `TCC_NO_PROGRAMMATIC_DECLS=1 tcc ...` measures the cost of one extra
 * environment variable rather than the arm it names.  This probe needs a live
 * knob in exactly that build, so it reads the environment directly. */
static int pp_skip_predef_macros(void)
{
  static signed char cached = -1;
  if (cached < 0)
    cached = getenv("TCC_SKIP_PREDEF_MACROS") != NULL;
  return cached;
}

/* TCC_DOUBLE_PREDEF_MACROS appends a second, identical copy of the tccdefs
 * block.  Identical redefinition is legal C, so unlike the skip arm this one
 * cannot change whether the compile succeeds -- and the second copy runs with
 * the tokenizer, the define path and the symbol table already hot.  That makes
 * its cost the *warm* price of the predefine content, and the gap between it
 * and the first copy's 17 ms is the first-touch component, i.e. exactly the
 * part a lazy scheme would relocate rather than remove. */
static int pp_double_predef_macros(void)
{
  static signed char cached = -1;
  if (cached < 0)
    cached = getenv("TCC_DOUBLE_PREDEF_MACROS") != NULL;
  return cached;
}

/* The predefine block, hoisted out of its cstr_cat call site so the doubling
 * probe can append it twice (see pp_double_predef_macros). */
static const char tcc_predef_macros_text[] =
    /* load more predefs and __builtins */
#if CONFIG_TCC_PREDEFS
#include "tccdefs_.h" /* include as strings */
#else
    "#include <tccdefs.h>\n" /* load at runtime */
#endif
    ;
#endif /* !CONFIG_TCC_PREDEF_TABLE */

/* ---- lazy predefined macros ------------------------------------------
 *
 * The ~97 predefines for this target used to be lexed and defined on every
 * invocation: 17.2 ms, 27% of a median -O0 compile on the RP2350, 83% of it
 * the first walk through the define path.  89% of the torture corpus names
 * none of them.  So the table (tccdefs_table_.h, generated by
 * gen_predef_table.c) is consulted on demand instead.
 *
 * The hook is tok_alloc_new -- the single point where an identifier is first
 * interned.  That means a predefine materialises exactly when its name first
 * appears anywhere: in the source, in `#ifdef`, inside `defined()`, or inside
 * another macro's body being tokenised.  Chains therefore resolve by
 * themselves (only 4 predefine bodies name another predefine) and define_find
 * stays untouched, so the hot lookup path costs nothing.
 *
 * Materialisation never re-enters the lexer: bodies arrive pre-tokenised, and
 * the only tcc helper used on them, parse_number(), reads solely from the
 * string it is handed.  It does write the globals tok/tokc, which the caller
 * may be in the middle of using, so they are saved and restored.
 */

#define PDT_FIXED 0
#define PDT_IDENT 1
#define PDT_NUM 2

typedef struct TCCPredefTok
{
  unsigned char kind;
  int tok;
  unsigned short text; /* PDT_IDENT/PDT_NUM: offset into tcc_predef_strs[] */
} TCCPredefTok;

#include "tccdefs_table_.h"

/* The name index (predef_hash_head/next, by bare-name hash with TOK_HASH_FUNC,
 * index+1 chains) is generated with the tables: static, no heap, no .bss --
 * it runs for every newly interned identifier. */
#define PREDEF_STR(off) (tcc_predef_strs + (off))

static unsigned predef_hash(const char *s, int len)
{
  unsigned h = TOK_HASH_INIT;
  int i;
  for (i = 0; i < len; i++)
    h = TOK_HASH_FUNC(h, ((const unsigned char *)s)[i]);
  return h & (PREDEF_HASH_SIZE - 1);
}

/* Is this entry active for the current TCCState? The two conditions that could
   not be resolved at build time depend on runtime options. */
static int predef_entry_active(TCCState *s1, const TCCPredefMacro *e)
{
  if ((e->flags & TCC_PREDEF_C11) && s1->cversion < 201112)
    return 0;
  if ((e->flags & TCC_PREDEF_LEADING_US) && !s1->leading_underscore)
    return 0;
  if ((e->flags & TCC_PREDEF_NO_LEADING_US) && s1->leading_underscore)
    return 0;
  return 1;
}

/* Replay one entry's pre-tokenised body and define it. */
static void predef_materialize(TokenSym *ts, int idx)
{
  const TCCPredefMacro *e = &tcc_predef_macros[idx];
  TokenString str;
  Sym *first = NULL, **ps = &first;
  int macro_type = MACRO_OBJ;
  int i;
  int saved_tok = tok;
  CValue saved_tokc = tokc;
  /* A predefine materialises at the point its name is first interned, which may
     be in the middle of a #if expression -- and parse_number() consults pp_expr
     to decide that every integer constant is intmax_t (C preprocessor
     arithmetic).  That rule is about the expression being evaluated, not about
     the macro body being stored: a body tokenised under it keeps the 64-bit type
     for the rest of the translation unit.  <limits.h> hits this on its very
     first line of real work (`#if __SCHAR_MAX__ == __INT_MAX__`), which is what
     made sizeof(__INT_MAX__) 8 and turned UINT_MAX into a signed long long, so
     `(unsigned)x < UINT_MAX` compiled to a *signed* compare.  Bodies must be
     tokenised exactly as parse_define would tokenise them, i.e. with pp_expr
     off; the pp evaluator promotes the resulting TOK_CINT itself. */
  int saved_pp_expr = pp_expr;
  pp_expr = 0;

  /* Parameters, for function-like macros: the table spells them inside the
     name field exactly as the text form did. */
  {
    const char *p = PREDEF_STR(e->name) + e->name_len;
    if (*p == '(')
    {
      macro_type = MACRO_FUNC;
      p++;
      while (*p && *p != ')')
      {
        const char *b = p;
        Sym *s;
        while (*p && *p != ',' && *p != ')')
          p++;
        s = sym_push2_short(&define_stack, tok_alloc(b, (int)(p - b))->tok | SYM_FIELD, 0, 0);
        *ps = s;
        ps = &s->next;
        if (*p == ',')
          p++;
      }
    }
  }

  tok_str_new(&str);
  for (i = 0; i < e->tok_count; i++)
  {
    const TCCPredefTok *t = &tcc_predef_toks[e->tok_off + i];
    switch (t->kind)
    {
    case PDT_IDENT:
      tok_str_add(&str, tok_alloc(PREDEF_STR(t->text), (int)strlen(PREDEF_STR(t->text)))->tok);
      break;
    case PDT_NUM:
      parse_number(PREDEF_STR(t->text));
      tok_str_add2(&str, tok, &tokc);
      break;
    default:
      tok_str_add(&str, t->tok);
      break;
    }
  }
  /* Terminate exactly as parse_define does. Without the 0, macro_is_equal()
     walks past the end when a source #define redefines a predefined name --
     which is a heap read overflow ASAN catches on the first such redefinition. */
  tok_str_add(&str, 0);
  tok_str_fit(&str);

  define_push(ts->tok, macro_type, tok_str_ensure_heap(&str), first);

  tok = saved_tok;
  tokc = saved_tokc;
  pp_expr = saved_pp_expr;
}

/* Called from tok_alloc_new once the TokenSym is linked into table_ident.
   Linking first matters: materialising re-enters tok_alloc for the body's own
   identifiers, and a half-built entry would be found by that recursion. */
static void tcc_predef_try_materialize(TokenSym *ts, const char *str, int len)
{
  unsigned h;
  int i;

  if (!tcc_state)
    return;

  h = predef_hash(str, len);
  for (i = predef_hash_head[h]; i; i = predef_hash_next[i - 1])
  {
    int idx = i - 1;
    if (tcc_predef_macros[idx].name_len == len && !memcmp(PREDEF_STR(tcc_predef_macros[idx].name), str, len))
    {
      if (predef_entry_active(tcc_state, &tcc_predef_macros[idx]))
        predef_materialize(ts, idx);
      /* keep scanning: __USER_LABEL_PREFIX__ has two entries, one per
         leading-underscore arm, and only one of them is active */
    }
  }
}

static void tcc_predefs_base(TCCState *s1, CString *cs, int is_asm, int include_base_file)
{
  cstr_printf(cs, "#define __TINYC__ 9%.2s\n", *&TCC_VERSION + 4);
  putdefs(cs, target_machine_defs);
  putdefs(cs, target_os_defs);
#if defined TCC_TARGET_ARM_ARCHV8M
  /* The ACLE description of ARMv8-M Mainline gcc and clang give a Cortex-M33
   * (pico-sdk's spin locks pick their LDREX path on __ARM_ARCH_8M_MAIN__).
   * DSP and CMSE are left out: code reaching for their intrinsics is not
   * code tcc has been taught to emit. */
  cstr_printf(cs, "#define __ARM_ARCH 8\n"
                  "#define __ARM_ARCH_8M_MAIN__ 1\n"
                  "#define __ARM_ARCH_ISA_THUMB 2\n"
                  "#define __ARM_ARCH_PROFILE 77\n"
                  "#define __ARM_FEATURE_CLZ 1\n"
                  "#define __ARM_FEATURE_IDIV 1\n"
                  "#define __ARM_FEATURE_LDREX 7\n"
                  "#define __ARM_FEATURE_QBIT 1\n"
                  "#define __ARM_FEATURE_SAT 1\n"
                  "#define __ARM_FEATURE_UNALIGNED 1\n");
#endif

#if defined(TCC_TARGET_ARM) || defined(TCC_TARGET_ARM_THUMB)
  if (s1->float_abi == ARM_HARD_FLOAT)
    putdef(cs, "__ARM_PCS_VFP");
  /* Define __ARM_FP based on FPU type for library compatibility */
  if (s1->float_abi != ARM_SOFT_FLOAT && s1->fpu_type != ARM_FPU_NONE)
  {
    int arm_fp = 0;
    switch (s1->fpu_type)
    {
    case ARM_FPU_FPV4_SP_D16:
    case ARM_FPU_FPV5_SP_D16:
      arm_fp = 0x04; /* Single precision only */
      break;
    case ARM_FPU_RP2350:
      /* Single precision is FPv5-SP; doubles run on the DCP, which is
         hardware but not fully IEEE — see the FTZ defines below. */
      arm_fp = 0x0C;
      break;
    case ARM_FPU_VFP:
    case ARM_FPU_VFPV3:
    case ARM_FPU_VFPV4:
    case ARM_FPU_FPV5_D16:
    case ARM_FPU_NEON:
    case ARM_FPU_NEON_VFPV4:
    case ARM_FPU_NEON_FP_ARMV8:
    case ARM_FPU_AUTO:
    default:
      arm_fp = 0x0C; /* Single + Double precision */
      break;
    case ARM_FPU_NONE:
      arm_fp = 0;
      break;
    }
    if (arm_fp)
      cstr_printf(cs, "#define __ARM_FP %d\n", arm_fp);
    putdef(cs, "__VFP_FP__");
    /* The RP2350 double coprocessor flushes subnormal operands and results to
       zero and offers no path that doesn't (pico-sdk's own double_aeabi_dcp.S
       behaves identically).  Code that must know — an IEEE conformance gate,
       or a numeric kernel that would rather be built -mfpu=none — has no other
       way to tell a DCP double from an FPv5-D16 one, since both report
       __ARM_FP 12. */
    if (s1->fpu_type == ARM_FPU_RP2350)
    {
      putdef(cs, "__TCC_FPU_RP2350__");
      putdef(cs, "__TCC_DOUBLE_FLUSHES_SUBNORMALS__");
    }
  }
#endif
  if (is_asm)
    putdef(cs, "__ASSEMBLER__");
  if (s1->output_type == TCC_OUTPUT_PREPROCESS)
    putdef(cs, "__TCC_PP__");
  if (s1->output_type == TCC_OUTPUT_MEMORY)
    putdef(cs, "__TCC_RUN__");
#ifdef CONFIG_TCC_BACKTRACE
  if (s1->do_backtrace)
    putdef(cs, "__TCC_BACKTRACE__");
#endif
#ifdef CONFIG_TCC_BCHECK
  if (s1->do_bounds_check)
    putdef(cs, "__TCC_BCHECK__");
#endif
  if (s1->char_is_unsigned)
    putdef(cs, "__CHAR_UNSIGNED__");
  /* gcc's: -fstack-protector, -all, -strong, -explicit */
  if (s1->stack_protector)
    putdef(cs, s1->stack_protector == SSP_ALL      ? "__SSP_ALL__ 2"
               : s1->stack_protector == SSP_STRONG ? "__SSP_STRONG__ 3"
               : s1->stack_protector == SSP_EXPLICIT ? "__SSP_EXPLICIT__ 4"
                                                     : "__SSP__ 1");
  if (TCC_OPT(s1, optimize) > 0)
    putdef(cs, "__OPTIMIZE__");
  if (TCC_OPT(s1, optimize_size))
    putdef(cs, "__OPTIMIZE_SIZE__");
  if (s1->option_pthread)
    putdef(cs, "_REENTRANT");
  if (s1->leading_underscore)
    putdef(cs, "__leading_underscore");
  cstr_printf(cs, "#define __SIZEOF_POINTER__ %d\n", PTR_SIZE);
  cstr_printf(cs, "#define __SIZEOF_LONG__ %d\n", LONG_SIZE);
  cstr_printf(cs, "#define __SIZEOF_FLOAT__ 4\n");
  cstr_printf(cs, "#define __SIZEOF_DOUBLE__ 8\n");
  cstr_printf(cs, "#define __SIZEOF_LONG_DOUBLE__ %d\n", LDOUBLE_SIZE);
  cstr_printf(cs, "#define __SIZEOF_WCHAR_T__ 4\n");
  cstr_printf(cs, "#define __SIZEOF_WINT_T__ 4\n");
  cstr_printf(cs, "#define __SIZEOF_SIZE_T__ %d\n", PTR_SIZE);
  cstr_printf(cs, "#define __SIZEOF_PTRDIFF_T__ %d\n", PTR_SIZE);
  if (!is_asm)
  {
    putdef(cs, "__STDC__");
    cstr_printf(cs, "#define __STDC_VERSION__ %dL\n", s1->cversion);
    if (s1->yasc_std)
      putdef(cs, "__YASC__");
    /* TCC_SKIP_PREDEF_MACROS drops the 7.2 KB tccdefs block from the
       <command line> buffer.  It is a MEASUREMENT knob, not a supported mode:
       a compile that references __SIZE_TYPE__, the va_arg machinery or any
       other predefine will fail under it.  It exists because the `predef
       macros` stamp (17.2 ms, 27% of a median -O0 compile) is only worth
       attacking if skipping the text actually recovers the time rather than
       relocating the cold-touch into whatever parses next -- the 2026-08-04
       probe measured only ~5 ms of a then-12 ms phase as non-relocating, and
       that verdict predates the footprint and transaction rounds.  Compare
       the stamp delta (what the phase loses) against the wall delta (what the
       compile keeps): the gap is the relocation. */
#if !CONFIG_TCC_PREDEF_TABLE
    /* Eager arm: the whole tccdefs text goes through the preprocessor on every
       invocation. Retained as the byte-identity A/B against the table. */
    if (!pp_skip_predef_macros())
      cstr_cat(cs, tcc_predef_macros_text, -1);
    /* Second copy for the doubling arm. It has to come from the same constant:
       tccdefs_.h carries `#pragma once`, so a second #include of it expands to
       nothing at all. */
    if (pp_double_predef_macros())
      cstr_cat(cs, tcc_predef_macros_text, -1);
#else
    /* Lazy arm: nothing goes into the buffer. Each predefine is materialised
       from the generated table when its name is first interned (see
       tcc_predef_try_materialize). The one non-#define line the text carried,
       `typedef char *__builtin_va_list;`, still has to be seen by the parser,
       so it is emitted here -- one line instead of 3.7 KB.  -E has no parser
       to feed, and anything put here lands in its output; see the
       TCC_OUTPUT_PREPROCESS arm below. */
    if (s1->output_type != TCC_OUTPUT_PREPROCESS)
    {
      const char *r;
      for (r = tcc_predef_raw; *r; r += strlen(r) + 1)
      {
        cstr_cat(cs, r, -1);
        cstr_ccat(cs, '\n');
      }
    }
#endif
    /* The builtin alias declarations (tccdecls.h). Parsing them costs ~30 ms
       per compile on the RP2350 — the macro-expander/parser alternation
       thrashes the 16 KiB XIP cache — so the default configuration builds
       the identical prototypes programmatically at tccgen_compile start
       instead. The text stays authoritative for the configurations it is
       conditional on (bcheck renames, leading underscore); -E never sees
       either form. TCC_NO_PROGRAMMATIC_DECLS forces the text path for A/B
       validation. */
    if (s1->output_type == TCC_OUTPUT_PREPROCESS)
    {
      /* Neither form, as the comment above says.  -E reproduces the
         translation unit, and these declarations are not part of it: no
         consumer of the output needs them, and one that reads the output as
         data gets them as stray text.  gnulib's configure greps
         `$CC $CFLAGS -E conftest.c` for the -Wno-* options the compiler
         accepts; with the prologue present it took the whole thing for a flag
         list, so GL_CFLAG_GNULIB_WARNINGS became
         `typedef char*__builtin_va_list; void* __builtin_memcpy (...) ...`
         and every compile line in the generated Makefiles died in the shell
         on the first `(`. */
    }
    else if (
#ifdef CONFIG_TCC_BCHECK
        !s1->do_bounds_check &&
#endif
        !s1->leading_underscore && !pp_no_programmatic_decls())
    {
      s1->predef_protos_pending = 1;
    }
    else
    {
      cstr_cat(cs,
#if CONFIG_TCC_PREDEFS
#include "tccdecls_.h"
#else
          "#include <tccdecls.h>\n"
#endif
               , -1);
    }
  }
  if (include_base_file)
    cstr_printf(cs, "#define __BASE_FILE__ \"%s\"\n", file->filename);
}

ST_FUNC void preprocess_start(TCCState *s1, int filetype)
{
  int is_asm = !!(filetype & (AFF_TYPE_ASM | AFF_TYPE_ASMPP));

  tccpp_new(s1);

  s1->include_stack_ptr = s1->include_stack;
  s1->ifdef_stack_ptr = s1->ifdef_stack;
  file->ifdef_stack_ptr = s1->ifdef_stack_ptr;
  s1->yasc = file->yasc; /* tcc_open_bf: the main file is yasc under -std=yasc */
  pp_sys_track = !s1->warn_none && s1->output_type != TCC_OUTPUT_PREPROCESS;
  s1->pp_marker_yasc = 0;
  s1->pp_marker_depth = 0;
  s1->warned_attributes = 0;
  dynarray_reset(&s1->yasc_includes, &s1->nb_yasc_includes);
  /* -Wtautological-compare's operand hash: only while it can be wanted */
  fe_tokfp_on = ((s1->warn_tautological_compare | s1->warn_sizeof_pointer_memaccess) & WARN_ON) &&
                !(filetype & (AFF_TYPE_ASM | AFF_TYPE_ASMPP));
  pp_expr = 0;
  pp_subst_depth = 0;
  pp_counter = 0;
  pp_debug_tok = pp_debug_symv = 0;
  s1->pack_stack[0] = 0;
  s1->pack_stack_ptr = s1->pack_stack;
  s1->visibility_stack_depth = 0;

  set_idnum('$', !is_asm && s1->dollars_in_identifiers ? IS_ID : 0);
  set_idnum('.', is_asm ? IS_ID : 0);

  if (!(filetype & AFF_TYPE_ASM))
  {
    CString cstr;
    cstr_new(&cstr);
    tcc_predefs_base(s1, &cstr, is_asm, 0);
    if (s1->cmdline_defs.size)
      cstr_cat(&cstr, s1->cmdline_defs.data, s1->cmdline_defs.size);
    if (s1->cmdline_incl.size)
      cstr_cat(&cstr, s1->cmdline_incl.data, s1->cmdline_incl.size);
    cstr_printf(&cstr, "#define __BASE_FILE__ \"%s\"\n", file->filename);
    // printf("%.*s\n", cstr.size, (char*)cstr.data);
    *s1->include_stack_ptr++ = file;
    tcc_open_bf(s1, "<command line>", cstr.size);
    memcpy(file->buffer, cstr.data, cstr.size);
    cstr_free(&cstr);
  }
  parse_flags = is_asm ? PARSE_FLAG_ASM_FILE : 0;
}

/* cleanup from error/setjmp */
ST_FUNC void preprocess_end(TCCState *s1)
{
  tcc_diag_reset(s1); /* the next unit starts from the command line's options */
  while (macro_stack)
    end_macro();
  macro_ptr = NULL;
  pp_embed_pending = 0;
  while (file)
    tcc_close();
#ifdef CONFIG_TCC_ASM
  tcc_asm_cleanup();
#endif
  tccpp_delete(s1);
}

ST_FUNC int set_idnum(int c, int val)
{
  int prev = isidnum_table[c - CH_EOF];
  isidnum_table[c - CH_EOF] = val;
  return prev;
}

ST_FUNC void tccpp_new(TCCState *s)
{
  int i;

  /* init isid table */
  /* Note: written as if-else chain instead of nested ternary to work around
     a TCC ARM codegen bug at -O1 where nested ternaries in a for-loop body
     cause the loop increment to be lost. */
  for (i = CH_EOF; i < 128; i++)
  {
    int val;
    if (is_space(i))
      val = IS_SPC;
    else if (isid(i))
      val = IS_ID;
    else if (isnum(i))
      val = IS_NUM;
    else
      val = 0;
    set_idnum(i, val);
  }

  for (i = 128; i < 256; i++)
    set_idnum(i, IS_ID);

  /* init allocators */
  tal_new(&tokstr_alloc, TOKSTR_TAL_LIMIT, TOKSTR_TAL_SIZE);

  table_ident_alloc = TOK_IDENT_PREALLOC;
  table_ident = tcc_malloc(table_ident_alloc * sizeof(TokenSym *));
  /* reserved builtin slots [0, NB_BUILTIN_TOKS) start lazy (NULL); the lazy
     interner allocates each only on first use.  tcc_malloc is not zeroed, so
     clear them explicitly. */
  memset(table_ident, 0, NB_BUILTIN_TOKS * sizeof(TokenSym *));
  token_lookup_cache_clear();
  memset(hash_ident, 0, TOK_HASH_SIZE * sizeof(TokenSym *));
  memset(s->cached_includes_hash, 0, sizeof s->cached_includes_hash);

  cstr_new(&tokcstr);
  cstr_new(&cstr_buf);
  cstr_realloc(&cstr_buf, STRING_MAX_SIZE);
  tok_str_new(&tokstr_buf);
  tok_str_realloc(&tokstr_buf, TOKSTR_MAX_SIZE);
  pp_sys_end_pending = 0;
  pp_sys_end_ptr = NULL;
  tok_str_new(&unget_buf);

  /* Reserve the whole builtin token id range.  Builtin TokenSyms are
     materialized lazily on first use rather than interned eagerly here. */
  tok_ident = TOK_IDENT + NB_BUILTIN_TOKS;

  /* we add dummy defines for some special macros to speed up tests
     and to have working defined() */
  define_push(TOK___LINE__, MACRO_OBJ, NULL, NULL);
  define_push(TOK___FILE__, MACRO_OBJ, NULL, NULL);
  define_push(TOK___DATE__, MACRO_OBJ, NULL, NULL);
  define_push(TOK___TIME__, MACRO_OBJ, NULL, NULL);
  define_push(TOK___COUNTER__, MACRO_OBJ, NULL, NULL);
  define_push(TOK___HAS_BUILTIN, MACRO_OBJ, NULL, NULL);
}

ST_FUNC void tccpp_delete(TCCState *s)
{
  int n;

  dynarray_reset(&s->cached_includes, &s->nb_cached_includes);

  /* free tokens */
  n = tok_ident - TOK_IDENT;
  if (n > total_idents)
    total_idents = n;
  toksym_free_all();
  tcc_free(define_map); /* the macros' map goes with the names */
  define_map = NULL;
  define_map_nb = define_map_mask = 0;
  tcc_free(table_ident);
  table_ident = NULL;
  table_ident_alloc = 0;
  token_lookup_cache_clear();

  /* String token statistics disabled
  if (str_total_added > 0) {
    fprintf(stderr, "String tokens: %lu, bytes: %lu\n",
            str_total_added, str_bytes_copied);
  }
  */

  /* free static buffers */
  cstr_free(&tokcstr);
  cstr_free(&cstr_buf);
  if (tokstr_buf.allocated_len > 0)
    tok_str_free_str(tokstr_buf.data.str);
  xs_free(tokstr_buf.xs);
  tokstr_buf.xs = NULL;
  while (xs_pool_nb)
  {
    PPSys *x = xs_pool[--xs_pool_nb];
    tcc_free(x->b);
    tcc_free(x);
  }
  /* the ranges of strings an error left unfreed */
  for (unsigned i = 0; sys_tab && i <= sys_tab_mask; i++)
    if (sys_tab[i])
    {
      tcc_free(sys_tab[i]->b);
      tcc_free(sys_tab[i]);
    }
  tcc_free(sys_tab);
  sys_tab = NULL;
  sys_tab_mask = 0;
  tok_sys_ranges = 0;
  if (unget_buf.allocated_len > 0)
    tok_str_free_str(unget_buf.data.str);

  /* free string pool (currently unused)
  tal_delete(strpool_alloc);
  strpool_alloc = NULL;
  memset(strpool_hash, 0, sizeof(strpool_hash));
  */

  /* free allocators */
#ifdef USE_TAL
  tal_delete(tokstr_alloc);
  tokstr_alloc = NULL;
#endif
}


/* ------------------------------------------------------------------------- */
/* tcc -E [-P[1]] [-dD} support */

static int pp_need_space(int a, int b);

static void tok_print(const int *str, const char *msg, ...)
{
  FILE *fp = tcc_state->ppfp;
  va_list ap;
  int t, t0, s;
  CValue cval;

  va_start(ap, msg);
  vfprintf(fp, msg, ap);
  va_end(ap);

  s = t0 = 0;
  while (str)
  {
    TOK_GET(&t, &str, &cval);
    if (t == 0 || t == TOK_EOF)
      break;
    if (pp_need_space(t0, t))
      s = 0;
    fprintf(fp, &" %s"[s], t == TOK_PLCHLDR ? "<>" : get_tok_str(t, &cval));
    s = 1, t0 = t;
  }
  fprintf(fp, "\n");
}

static void pp_line(TCCState *s1, BufferedFile *f, int level)
{
  int d = f->line_num - f->line_ref;

  if (s1->dflag & 4)
    return;

  if (s1->Pflag == LINE_MACRO_OUTPUT_FORMAT_NONE)
  {
    ;
  }
  else if (level == 0 && f->line_ref && d < 8 && (f->pp_sys == f->sys || s1->Pflag == LINE_MACRO_OUTPUT_FORMAT_STD))
  {
    while (d > 0)
      fputs("\n", s1->ppfp), --d;
  }
  else if (s1->Pflag == LINE_MACRO_OUTPUT_FORMAT_STD)
  {
    fprintf(s1->ppfp, "#line %d \"%s\"\n", f->line_num, f->filename);
  }
  else
  {
    fprintf(s1->ppfp, "# %d \"%s\"%s%s\n", f->line_num, f->filename, level > 0 ? " 1" : level < 0 ? " 2" : "",
            f->sys ? " 3" : "");
  }
  f->line_ref = f->line_num;
  f->pp_sys = f->sys;
}

static void define_print(TCCState *s1, int v)
{
  FILE *fp;
  Sym *s;

  s = define_find(v);
  if (NULL == s || NULL == s->d)
    return;

  fp = s1->ppfp;
  fprintf(fp, "#define %s", get_tok_str(v, NULL));
  if (s->type.t & MACRO_FUNC)
  {
    Sym *a = s->next;
    fprintf(fp, "(");
    if (a)
      for (;;)
      {
        fprintf(fp, "%s", get_tok_str(a->v, NULL));
        if (!(a = a->next))
          break;
        fprintf(fp, ",");
      }
    fprintf(fp, ")");
  }
  tok_print(s->d, "");
}

static void pp_debug_defines(TCCState *s1)
{
  int v, t;
  const char *vs;
  FILE *fp;

  t = pp_debug_tok;
  if (t == 0)
    return;

  file->line_num--;
  pp_line(s1, file, 0);
  file->line_ref = ++file->line_num;

  fp = s1->ppfp;
  v = pp_debug_symv;
  vs = get_tok_str(v, NULL);
  if (t == TOK_DEFINE)
  {
    define_print(s1, v);
  }
  else if (t == TOK_UNDEF)
  {
    fprintf(fp, "#undef %s\n", vs);
  }
  else if (t == TOK_push_macro)
  {
    fprintf(fp, "#pragma push_macro(\"%s\")\n", vs);
  }
  else if (t == TOK_pop_macro)
  {
    fprintf(fp, "#pragma pop_macro(\"%s\")\n", vs);
  }
  pp_debug_tok = 0;
}

/* Add a space between tokens a and b to avoid unwanted textual pasting */
static int pp_need_space(int a, int b)
{
  return 'E' == a                           ? '+' == b || '-' == b
         : '+' == a                         ? TOK_INC == b || '+' == b
         : '-' == a                         ? TOK_DEC == b || '-' == b
         : a >= TOK_IDENT || a == TOK_PPNUM ? b >= TOK_IDENT || b == TOK_PPNUM
                                            : 0;
}

/* maybe hex like 0x1e */
static int pp_check_he0xE(int t, const char *p)
{
  if (t == TOK_PPNUM && toup(strchr(p, 0)[-1]) == 'E')
    return 'E';
  return t;
}

/* -dM / -dD list the predefines too, as gcc does.  The table ones are defined
 * lazily, when their name is first interned, which never goes through the
 * #define path the dump hooks: intern each active entry and print it. */
static void pp_dump_table_predefs(TCCState *s1)
{
  int i;
  for (i = 0; i < (int)(sizeof(tcc_predef_macros) / sizeof(tcc_predef_macros[0])); i++)
  {
    const TCCPredefMacro *e = &tcc_predef_macros[i];
    if (!predef_entry_active(s1, e))
      continue;
    define_print(s1, tok_alloc(PREDEF_STR(e->name), e->name_len)->tok);
  }
}

/* Preprocess the current file */
ST_FUNC int tcc_preprocess(TCCState *s1)
{
  BufferedFile **iptr;
  int token_seen, spcs, level;
  const char *p;
  char white[400];

  parse_flags = PARSE_FLAG_PREPROCESS | (parse_flags & PARSE_FLAG_ASM_FILE) | PARSE_FLAG_LINEFEED | PARSE_FLAG_SPACES |
                PARSE_FLAG_ACCEPT_STRAYS;
  /* Credits to Fabrice Bellard's initial revision to demonstrate its
     capability to compile and run itself, provided all numbers are
     given as decimals. tcc -E -P10 will do. */
  if (s1->Pflag == LINE_MACRO_OUTPUT_FORMAT_P10)
    parse_flags |= PARSE_FLAG_TOK_NUM, s1->Pflag = 1;

  if (s1->do_bench)
  {
    /* for PP benchmarks */
    do
      next();
    while (tok != TOK_EOF);
    return 0;
  }

  if (s1->dflag & 3)
    pp_dump_table_predefs(s1);

  token_seen = TOK_LINEFEED, spcs = 0, level = 0;
  if (file->prev)
    pp_line(s1, file->prev, level++);
  pp_line(s1, file, level);

  for (;;)
  {
    iptr = s1->include_stack_ptr;
    next();
    if (tok == TOK_EOF)
      break;

    level = s1->include_stack_ptr - iptr;
    if (level)
    {
      if (level > 0)
        pp_line(s1, *iptr, 0);
      pp_line(s1, file, level);
    }
    if (s1->dflag & 7)
    {
      pp_debug_defines(s1);
      if (s1->dflag & 4)
        continue;
    }
    if (file->yasc_dir == 1 && tok != TOK_LINEFEED && !is_space(tok))
    {
      /* a header from an -iyasc directory: what its compile needs to know,
         before its first text (the line marker after it keeps the lines) */
      file->yasc_dir = 2;
      if (token_seen != TOK_LINEFEED)
        fputs("\n", s1->ppfp);
      fputs("#pragma yasc\n", s1->ppfp);
      file->line_ref = 0;
      token_seen = TOK_LINEFEED, spcs = 0;
    }

    /* yasc: a regular-C macro's expansion begins or ends (pp_yasc_marks):
       after the token before it, on its line; at a line's start, before the
       first token, after the line's marker */
    if (pp_yasc_nmarks && token_seen != TOK_LINEFEED && (is_space(tok) || tok == TOK_LINEFEED))
      pp_yasc_marks_flush(s1, 1);
    if (is_space(tok))
    {
      if (spcs < sizeof white - 1)
        white[spcs++] = tok;
      continue;
    }
    else if (tok == TOK_LINEFEED)
    {
      spcs = 0;
      if (token_seen == TOK_LINEFEED)
        continue;
      ++file->line_ref;
    }
    else if (token_seen == TOK_LINEFEED)
    {
      pp_line(s1, file, 0);
    }
    else if (spcs == 0 && pp_need_space(token_seen, tok))
    {
      white[spcs++] = ' ';
    }

    if (pp_yasc_nmarks && token_seen != TOK_LINEFEED && !spcs)
      white[spcs++] = ' '; /* not pasted to the token before */
    white[spcs] = 0, fputs(white, s1->ppfp), spcs = 0;
    if (pp_yasc_nmarks)
      pp_yasc_marks_flush(s1, 0);
    fputs(p = get_tok_str(tok, &tokc), s1->ppfp);
    token_seen = pp_check_he0xE(tok, p);
  }
  pp_yasc_nmarks = 0;
  return 0;
}

/* ------------------------------------------------------------------------- */
