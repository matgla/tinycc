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
ST_DATA const int *macro_ptr;
ST_DATA CString tokcstr; /* current parsed string, if any */
ST_DATA TokenString *pp_pragma_capture; /* see tcc.h */

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
static void parse_number(const char *p);
static void tcc_predef_try_materialize(TokenSym *ts, const char *str, int len);
static void parse_string(const char *p, int len);

static struct TinyAlloc *toksym_alloc;
static struct TinyAlloc *tokstr_alloc;

static TokenString *macro_stack;

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
  if (ts->len != len || memcmp(ts->str, str, len))
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

#define USE_TAL

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

#define TOKSYM_TAL_SIZE (48 * 1024) /* allocator for tiny TokenSym in table_ident */
#define TOKSTR_TAL_SIZE (8 * 1024)  /* allocator for TokenString structs only (not buffers) */
#define TOKSYM_TAL_LIMIT 256        /* prefer unique limits to distinguish allocators debug msgs */
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

      al = tal_new(pal, next->limit, next->size * 2);
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

#endif /* USE_TAL */
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
      new_alloc <<= 1;
    ptable = tcc_realloc(table_ident, new_alloc * sizeof(TokenSym *));
    table_ident = ptable;
    table_ident_alloc = new_alloc;
  }

  ts = tal_realloc(toksym_alloc, 0, sizeof(TokenSym) + len);

  table_ident[i] = ts;
  ts->tok = tok_ident++;
  ts->sym_define = NULL;
  ts->sym_label = NULL;
  ts->sym_struct = NULL;
  ts->sym_identifier = NULL;
  ts->len = len;
  ts->hash_next = NULL;
  memcpy(ts->str, str, len);
  ts->str[len] = '\0';
  *pts = ts;
  /* Last, and only once the entry is linked into table_ident: materialising a
     predefine re-enters tok_alloc for the identifiers in its own body, and a
     half-built entry would be found by that recursion. */
  tcc_predef_try_materialize(ts, str, len);
  return ts;
}

#define TOK_HASH_INIT 1
#define TOK_HASH_FUNC(h, c) ((h) + ((h) << 5) + ((h) >> 27) + (c))

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
 * the tcc_keywords blob (no heap), so unreferenced builtins cost nothing but
 * their reserved (NULL) table_ident slot. */
#define KW_HASH_SIZE 1024 /* power of two, > NB_BUILTIN_TOKS */
static const char *kw_str[NB_BUILTIN_TOKS];        /* ptr into tcc_keywords blob */
static unsigned short kw_len[NB_BUILTIN_TOKS];      /* its length */
static unsigned short kw_hash_head[KW_HASH_SIZE];   /* head index+1 (0 = empty) */
static unsigned short kw_hash_next[NB_BUILTIN_TOKS];/* chain link, index+1 (0 = end) */
static int kw_index_built;

/* Build the static keyword index from the tcc_keywords blob (one pass, no
 * heap).  Called once from tccpp_new. */
static void kw_index_build(void)
{
  const char *p = tcc_keywords;
  int idx = 0;
  unsigned int h;
  int i;
  memset(kw_hash_head, 0, sizeof kw_hash_head);
  while (*p)
  {
    const char *r = p;
    int len;
    while (*r)
      r++;
    len = (int)(r - p);
    kw_str[idx] = p;
    kw_len[idx] = (unsigned short)len;
    h = TOK_HASH_INIT;
    for (i = 0; i < len; i++)
      h = TOK_HASH_FUNC(h, ((unsigned char *)p)[i]);
    h &= (KW_HASH_SIZE - 1);
    kw_hash_next[idx] = kw_hash_head[h];
    kw_hash_head[h] = (unsigned short)(idx + 1);
    idx++;
    p = r + 1;
  }
  kw_index_built = 1;
}

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
  str = kw_str[i];
  len = kw_len[i];
  ts = tal_realloc(toksym_alloc, 0, sizeof(TokenSym) + len);
  ts->tok = tok;
  ts->sym_define = NULL;
  ts->sym_label = NULL;
  ts->sym_struct = NULL;
  ts->sym_identifier = NULL;
  ts->len = len;
  memcpy(ts->str, str, len);
  ts->str[len] = '\0';
  h = TOK_HASH_INIT;
  for (k = 0; k < len; k++)
    h = TOK_HASH_FUNC(h, ((unsigned char *)str)[k]);
  h &= (TOK_HASH_SIZE - 1);
  ts->hash_next = hash_ident[h];
  hash_ident[h] = ts;
  table_ident[i] = ts;
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
    if (kw_len[ki] == len && !memcmp(kw_str[ki], str, len))
      return tok_materialize_builtin(TOK_IDENT + ki);
  }
  return NULL;
}

/* Return table_ident[v - TOK_IDENT], materializing a lazy builtin slot first
 * if needed.  For user ids the slot is always present (interned when the name
 * was first lexed), so this is just a deref; the range guard avoids touching
 * kw_str[] for non-builtin ids.  Used by writers that may target an as-yet-
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
    if (ts->len == len && !memcmp(ts->str, str, len))
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
    sprintf(p, "%u", (unsigned)cv->i);
#else
    sprintf(p, "%llu", (unsigned long long)cv->i);
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
        sprintf(p, "<\\x%02x>", v);
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
        return ts->str;
      /* Lazy builtin not materialized: its string lives in the blob -- but
         ONLY for the reserved builtin range.  A NULL slot outside it means a
         bogus token id, and indexing kw_str[] with it reads past the array
         and hands back whatever follows: on the RP2350 that surfaced as a
         HardFault inside tcc_ir_opt_const_aggregate_fold, whose callee-name
         strcmp walked the garbage pointer (device -O1; benign on the host,
         where the bytes after kw_str[] happen to be readable).  tok_ensure()
         guards the same array for the same reason. */
      if ((unsigned)(v - TOK_IDENT) < (unsigned)NB_BUILTIN_TOKS)
        return (char *)kw_str[v - TOK_IDENT];
      return NULL;
    }
    else if (v >= SYM_FIRST_ANOM)
    {
      /* special name for anonymous symbol */
      sprintf(p, "L.%u", v - SYM_FIRST_ANOM);
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
static int handle_eob(void)
{
  BufferedFile *bf = file;
  int len;

  /* only tries to read if really end of buffer */
  if (bf->buf_ptr >= bf->buf_end)
  {
    if (bf->fd >= 0)
    {
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

      len = file->buf_end - q;
      r = memchr(q, '\n', len);
      if (r && r < found)
        found = r;
      r = memchr(q, '*', len);
      if (r && r < found)
        found = r;
      r = memchr(q, '\\', len);
      if (r && r < found)
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
        tcc_error("unexpected end of file in comment");
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
        tcc_error("unexpected end of file in comment");
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
        tcc_error("missing terminating %c character", sep);
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

ST_FUNC void tok_str_free(TokenString *str)
{
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

  /* Already using heap buffer - grow if needed */
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

ST_FUNC void begin_macro(TokenString *str, int alloc)
{
  str->alloc = alloc;
  str->prev = macro_stack;
  str->prev_ptr = macro_ptr;
  str->save_line_num = file->line_num;
  macro_ptr = tok_str_buf(str);
  macro_stack = str;
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
  }
  else
  {
    if (str->alloc == 2)
      str->data.str = NULL; /* don't free */
    tok_str_free(str);
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
  case TCC_PCH_REPLAY_PACK_POP:
    if (s1->pack_stack_ptr <= s1->pack_stack)
      tcc_error("out of pack stack");
    s1->pack_stack_ptr--;
    break;
  }
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
ST_INLN void define_push(int v, int macro_type, int *str, Sym *first_arg)
{
  Sym *s, *o;

  o = define_find(v);
  s = sym_push2(&define_stack, v, macro_type, 0);
  s->d = str;
  s->next = first_arg;
  /* v may be an as-yet-unmaterialized builtin (e.g. the startup defines for
     __LINE__ etc., or a #define of a builtin name) — ensure its slot. */
  tok_ensure(v)->sym_define = s;

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
      ts->sym_define = NULL;
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
  return ts ? ts->sym_define : NULL; /* NULL slot = lazy builtin, not a macro */
}

static uint8_t *skip_logical_line(uint8_t *p)
{
  for (;;)
  {
    uint8_t *q = p + 1, *bs, *nl;
    size_t len = file->buf_end - q;
    int c;

    nl = memchr(q, '\n', len);
    bs = memchr(q, '\\', len);
    if (!bs || (nl && nl < bs))
      return nl ? nl : file->buf_end;

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
    sym_free(top);
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

static int parse_include(TCCState *s1, int do_next, int test)
{
  int c, i;
  char name[1024], buf[1024], *p;
  CachedInclude *e;

  c = skip_spaces();
  if (c == '<' || c == '\"')
  {
    cstr_reset(&tokcstr);
    file->buf_ptr = parse_pp_string(file->buf_ptr, c == '<' ? '>' : c, &tokcstr);
    i = tokcstr.size;
    pstrncpy(name, tokcstr.data, i >= sizeof name ? sizeof name - 1 : i);
    next_nomacro();
  }
  else
  {
    /* computed #include : concatenate tokens until result is one of
       the two accepted forms.  Don't convert pp-tokens to tokens here. */
    parse_flags = PARSE_FLAG_PREPROCESS | PARSE_FLAG_LINEFEED | (parse_flags & PARSE_FLAG_ASM_FILE);
    name[0] = 0;
    for (;;)
    {
      next();
      p = name, i = strlen(p) - 1;
      if (i > 0 && ((p[0] == '"' && p[i] == '"') || (p[0] == '<' && p[i] == '>')))
        break;
      if (tok == TOK_LINEFEED)
        tcc_error("'#include' expects \"FILENAME\" or <FILENAME>");
      pstrcat(name, sizeof name, get_tok_str(tok, &tokc));
    }
    c = p[0];
    /* remove '<>|""' */
    memmove(p, p + 1, i - 1), p[i - 1] = 0;
  }

  if (!test)
    skip_to_eol(1);

  i = do_next ? file->include_next_index : -1;
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
      else if (test)
        return 0;
      else
        tcc_error("include file '%s' not found", name);
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
      return 1;
    }
    if (tcc_open(s1, buf) >= 0)
      break;
  }

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

  kind = table_ident[kind_tok - TOK_IDENT]->str;
  value = table_ident[value_tok - TOK_IDENT]->str;

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

/* eval an expression for #if/#elif */
static int expr_preprocess(TCCState *s1)
{
  int t;
  int64_t c;
  int t0 = tok;
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
      if (define_find(tok) || tok == TOK___HAS_INCLUDE || tok == TOK___HAS_INCLUDE_NEXT)
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
    else
    {
      /* if undefined macro, replace with zero */
      tok = TOK_CINT;
      tokc.i = 0;
    }
    tok_str_add_tok(str);
  }
  if (0 == str->len)
    tcc_error("#%s with no expression", get_tok_str(t0, 0));
  tok_str_add(str, TOK_EOF); /* simulate end of file */
  pp_expr = t0;              /* redirect pre-processor expression error messages */
  t = tok;
  /* now evaluate C constant expression */
  begin_macro(str, 1);
  next();
  c = expr_const64();
  if (tok != TOK_EOF)
    tcc_error("...");
  pp_expr = 0;
  end_macro();
  tok = t; /* restore LF or EOF */
  return c != 0;
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
        s = sym_push2(&define_stack, varg | SYM_FIELD, is_vaargs, 0);
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
  tok_str_shrink(&str);
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
      table_ident[v - TOK_IDENT]->sym_define = s->d ? s : NULL;
    }
    else
    {
      tcc_warning("unbalanced #pragma pop_macro");
    }
    pp_debug_tok = t, pp_debug_symv = v;
  }
  else if (tok == TOK_once)
  {
    search_cached_include(s1, file->true_filename, 1)->once = 1;
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
          rec_kind = TCC_PCH_REPLAY_PACK_PUSH;
          if (tok != ',')
            goto pack_set;
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

/* put alternative filename */
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
  if (0 == strcmp(file->filename, buf))
    return;
  // printf("new file '%s'\n", buf);
  if (file->true_filename == file->filename)
    file->true_filename = tcc_strdup(file->filename);
  pstrcpy(file->filename, sizeof file->filename, buf);
  tcc_debug_newfile(tcc_state);
}

/* is_bof is true if first non space token at beginning of file */
ST_FUNC void preprocess(int is_bof)
{
  TCCState *s1 = tcc_state;
  int c, n, saved_parse_flags;
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
  case TOK_IFNDEF:
    c = 1;
    goto do_ifdef;
  case TOK_IF:
    c = expr_preprocess(s1);
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
    if (define_find(tok) || tok == TOK___HAS_INCLUDE || tok == TOK___HAS_INCLUDE_NEXT)
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
      c = expr_preprocess(s1);
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
      n--;
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
    if (tok == TOK_ERROR)
      tcc_error("#error %s", buf);
    else
      tcc_warning("#warning %s", buf);
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

static void parse_string(const char *s, int len)
{
  uint8_t buf[1000], *p = buf;
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
    /* XXX: make it portable */
    if (!is_long)
      tok = TOK_CCHAR, char_size = 1;
    else
      tok = TOK_LCHAR, char_size = sizeof(nwchar_t);
    n = tokcstr.size / char_size - 1;
    if (n < 1)
      tcc_error("empty character constant");
    if (n > 1)
      tcc_warning_c(warn_all)("multi-character character constant");
    for (c = i = 0; i < n; ++i)
    {
      if (is_long)
        c = ((nwchar_t *)tokcstr.data)[i];
      else
        c = (c << 8) | ((char *)tokcstr.data)[i];
    }
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

/* parse number in null terminated string 'p' and return it in the
   current token */
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
      tcc_error("number too long");
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
      int bn_used_bits = 0;
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
        bn_lshift(bn, shift, t);
        bn_used_bits += shift;
      }
      frac_bits = 0;
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
            tcc_error("invalid digit");
          /* Only accumulate digits that fit in the bignum.  Excess
             fractional digits beyond BN_SIZE*32 bits would overflow
             the fixed-width bignum and corrupt the result.  Silently
             ignore them (they are beyond double precision anyway). */
          if (bn_used_bits + shift <= BN_SIZE * 32)
          {
            bn_lshift(bn, shift, t);
            frac_bits += shift;
            bn_used_bits += shift;
          }
          ch = *p++;
        }
      }
      if (ch != 'p' && ch != 'P')
        expect("exponent");
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
        expect("exponent digits");
      while (ch >= '0' && ch <= '9')
      {
        exp_val = exp_val * 10 + ch - '0';
        ch = *p++;
      }
      exp_val = exp_val * s;

      /* now we can generate the number */
      /* XXX: should patch directly float number */
      d = (double)bn[1] * 4294967296.0 + (double)bn[0];
      d = ldexp(d, exp_val - frac_bits);
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
          expect("exponent digits");
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
        tcc_error("invalid digit");
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
          tcc_error("three 'l's in integer constant");
        if (lcount && *(p - 1) != ch)
          tcc_error("incorrect integer suffix: %s", p1);
        lcount++;
        ch = *p++;
      }
      else if (t == 'U')
      {
        if (ucount >= 1)
          tcc_error("two 'u's in integer constant");
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
    tcc_error("invalid number");
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

/* return next token without macro substitution */
static void next_nomacro(void)
{
  int t, c, is_long, len;
  TokenSym *ts;
  uint8_t *p, *p1;
  unsigned int h;

  p = file->buf_ptr;
redo_no_start:
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
          if (ts->len == len && !memcmp(ts->str, p1, len)) {
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
      p = parse_comment(p);
      /* comments replaced by a blank */
      tok = ' ';
      goto maybe_space;
    }
    else if (c == '/')
    {
      p = parse_line_comment(p);
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
  case '@': /* only used in assembler */
  parse_simple:
    tok = c;
    p++;
    break;
  default:
    if (c >= 0x80 && c <= 0xFF) /* utf8 identifiers */
      goto parse_ident_fast;
    if (parse_flags & PARSE_FLAG_ASM_FILE)
      goto parse_simple;
    tcc_error("unrecognized character \\x%02x", c);
    break;
  }
  tok_flags = 0;
keep_tok_flags:
  file->buf_ptr = p;
#if defined(PARSE_DEBUG)
  printf("token = %d %s\n", tok, get_tok_str(tok, &tokc));
#endif
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

static int macro_subst(TokenString *tok_str, Sym **nested_list, const int *macro_str);

typedef struct MacroArg
{
  int v;
  unsigned char is_vaargs;
  int *d;
  int *e;
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
static int *macro_arg_subst(Sym **nested_list, const int *macro_str, MacroArg *args, int nb_args)
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
              tok_str_add(&str, TOK_PLCHLDR);
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
            macro_subst(&str2, nested_list, st);
            tok_str_add(&str2, TOK_EOF);
            arg->e = tok_str_ensure_heap(&str2);
          }
          st = arg->e;
        }
        tok_str_add_tokstream(&str, st);
      }
      else
      {
        tok_str_add(&str, t);
      }
    }
    else
    {
      tok_str_add2(&str, t, &cval);
    }
    if (t != ' ')
      t0 = t1, t1 = t;
  }
  tok_str_add(&str, 0);
  tok_str_shrink(&str);
  PP_PRINT(("areslt:", 0, tok_str_buf(&str)));
  return tok_str_ensure_heap(&str);
}

/* handle the '##' operator. return the resulting string (which must be freed).
 */
static inline int *macro_twosharps(const int *ptr0)
{
  int t1, t2, n, l;
  CValue cv1, cv2;
  TokenString macro_str1;
  const int *ptr;

  tok_str_new(&macro_str1);
  cstr_reset(&tokcstr);
  for (ptr = ptr0;;)
  {
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
      tok_str_add2(&macro_str1, t1, &cv1);
  }
  tok_str_add(&macro_str1, 0);
  PP_PRINT(("pasted:", 0, tok_str_buf(&macro_str1)));
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
        TOK_GET(&tok, &macro_ptr, &tokc);
        return tok;
      }
    }
    end_macro();
    /* also, end of scope for nested defined symbol */
    sa = *nested_list;
    if (sa)
      *nested_list = sa->prev, sym_free(sa);
  }
  if (ws_str)
  {
    return peek_file(ws_str);
  }
  else
  {
    next_nomacro();
    if (tok == '\t' || tok == TOK_LINEFEED)
      tok = ' ';
    return tok;
  }
  return 0; /* unreachable */
}

/* do macro substitution of current token with macro 's' and add
   result to (tok_str,tok_len). 'nested_list' is the list of all
   macros we got inside to avoid recursing. Return non zero if no
   substitution needs to be done */
static int macro_subst_tok(TokenString *tok_str, Sym **nested_list, Sym *s)
{
  int t;
  int v = s->v;

  PP_PRINT(("#", v, s->d));
  if (s->d)
  {
    int *mstr = s->d;

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
          tcc_error("macro '%s' used with too many args", get_tok_str(v, 0));
        }
      empty_arg:
        tok_str_new(&str);
        parlevel = 0;
        /* NOTE: non zero sa->type.t indicates VA_ARGS */
        while (parlevel > 0 || (t != ')' && (t != ',' || sa->type.t)))
        {
          if (t == TOK_EOF)
            tcc_error("EOF in invocation of macro '%s'", get_tok_str(v, 0));
          if (t == '(')
            parlevel++;
          if (t == ')')
            parlevel--;
          if (t == ' ')
            str.need_spc |= 1;
          else
            tok_str_add2_spc(&str, t, &tokc);
          t = next_argstream(nested_list, NULL);
        }
        tok_str_add(&str, TOK_EOF);
        args[arg_index].v = sa->v & ~SYM_FIELD;
        args[arg_index].is_vaargs = sa->type.t != 0;
        args[arg_index].d = tok_str_ensure_heap(&str);
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
          tcc_error("macro '%s' used with too few args", get_tok_str(v, 0));
        }
        i = 1;
      }

      /* now subst each arg */
      mstr = macro_arg_subst(nested_list, mstr, args, arg_index);

      /* free memory */
      for (i = 0; i < arg_index; ++i)
      {
        tok_str_free_str(args[i].d);
        tok_str_free_str(args[i].e);
      }
      tcc_free(args);
      parse_flags = saved_parse_flags;
    }

    /* process '##'s (if present) */
    jstr = mstr;
    if (s->type.t & MACRO_JOIN)
      jstr = macro_twosharps(mstr);

    sa = sym_push2(nested_list, v, 0, 0);
    ret = macro_subst(tok_str, nested_list, jstr);
    /* pop nested defined symbol */
    if (sa == *nested_list)
      *nested_list = sa->prev, sym_free(sa);
    if (jstr != mstr)
      tok_str_free_str(jstr);
    if (mstr != s->d)
      tok_str_free_str(mstr);
    return ret;
  }
  else
  {
    CValue cval;
    char buf[32], *cstrval = buf;

    /* special macros */
    if (v == TOK___LINE__ || v == TOK___COUNTER__)
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
static int macro_subst(TokenString *tok_str, Sym **nested_list, const int *macro_str)
{
  Sym *s;
  int t, nosubst = 0;
  CValue cval;
  TokenString macro_view;

#ifdef PP_DEBUG
  int tlen = tok_str->len;
  PP_PRINT(("+expand:", 0, macro_str));
#endif

  while (1)
  {
    TOK_GET(&t, &macro_str, &cval);
    if (t == 0 || t == TOK_EOF)
      break;
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
      begin_macro(&macro_view, 0);
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
      tok_str_add2_spc(tok_str, t, &cval);
      if (nosubst && t != '(')
        nosubst = 0;
      /* GCC supports 'defined' as result of a macro substitution */
      if (t == TOK_DEFINED && pp_expr)
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
    tcc_open_bf(s1, ":pragma:", len + 1);
    memcpy(file->buffer, text.data, len);
    file->buffer[len] = '\n';
    tok_flags = 0;
    pragma_parse(s1);
    tcc_close(); /* also restores tok_flags */
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

/* return next token with macro substitution */
ST_FUNC HOT void next(void)
{
  int t;
  TCCState *s1 = tcc_state;

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
           parser by fetching the next real token. */
        pp_apply_pack_replay(s1, tokc.i);
        goto redo;
      }
      goto convert;
    }
    else if (t == 0)
    {
      /* end of macro or unget token string */
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
        if (!(parse_flags & PARSE_FLAG_ACCEPT_STRAYS))
          tcc_error("stray '\\' in program");
      }
    }
    tok = t;
    goto check_pragma;
  }

  next_nomacro();
  t = tok;
  if (t >= TOK_IDENT && (parse_flags & PARSE_FLAG_PREPROCESS))
  {
    /* if reading from file, try to substitute macros */
    Sym *s = define_find(t);
    if (s)
    {
      Sym *nested_list = NULL;
      macro_subst_tok(&tokstr_buf, &nested_list, s);
      tok_str_add(&tokstr_buf, 0);
      begin_macro(&tokstr_buf, 0);
      goto redo;
    }
    goto check_pragma;
  }
  goto convert;

check_pragma:
  /* The C11 _Pragma operator is recognized here, after macro expansion, so
     that it also works when produced by a macro (e.g. DO_PRAGMA(x)). */
  if (tok == TOK__Pragma && (parse_flags & PARSE_FLAG_PREPROCESS))
  {
    handle_pragma_operator(s1);
    goto restart; /* fetch the token after the (now-processed) _Pragma */
  }
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
 * prototypes, for A/B validation against tccgen_predef_protos. */
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
  const char *text;
} TCCPredefTok;

#include "tccdefs_table_.h"

#define NB_PREDEF ((int)(sizeof(tcc_predef_macros) / sizeof(tcc_predef_macros[0])))

/* Name index: a small open hash over the table, built once per process. The
 * shape mirrors the lazy builtin-keyword index above -- static storage, no
 * heap -- because this runs for every newly interned identifier. */
#define PREDEF_HASH_SIZE 256 /* power of two, > NB_PREDEF */
static unsigned short predef_hash_head[PREDEF_HASH_SIZE];
static unsigned short predef_hash_next[NB_PREDEF];
static unsigned char predef_index_built;
/* Bare macro name length (the table's name field carries the parameter list
 * for function-like macros, e.g. "__builtin_offsetof(type,field)"). */
static unsigned char predef_name_len[NB_PREDEF];

static unsigned predef_hash(const char *s, int len)
{
  unsigned h = TOK_HASH_INIT;
  int i;
  for (i = 0; i < len; i++)
    h = TOK_HASH_FUNC(h, ((const unsigned char *)s)[i]);
  return h & (PREDEF_HASH_SIZE - 1);
}

static void predef_build_index(void)
{
  int i;
  for (i = 0; i < NB_PREDEF; i++)
  {
    const char *n = tcc_predef_macros[i].name;
    int len = 0;
    while (n[len] && n[len] != '(')
      len++;
    predef_name_len[i] = (unsigned char)len;
    {
      unsigned h = predef_hash(n, len);
      predef_hash_next[i] = predef_hash_head[h];
      predef_hash_head[h] = (unsigned short)(i + 1); /* index+1; 0 = empty */
    }
  }
  predef_index_built = 1;
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
    const char *p = e->name;
    while (*p && *p != '(')
      p++;
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
        s = sym_push2(&define_stack, tok_alloc(b, (int)(p - b))->tok | SYM_FIELD, 0, 0);
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
      tok_str_add(&str, tok_alloc(t->text, (int)strlen(t->text))->tok);
      break;
    case PDT_NUM:
      parse_number(t->text);
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
  tok_str_shrink(&str);

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
  if (!predef_index_built)
    predef_build_index();

  h = predef_hash(str, len);
  for (i = predef_hash_head[h]; i; i = predef_hash_next[i - 1])
  {
    int idx = i - 1;
    if (predef_name_len[idx] == len && !memcmp(tcc_predef_macros[idx].name, str, len))
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
  if (s1->optimize > 0)
    putdef(cs, "__OPTIMIZE__");
  if (s1->option_pthread)
    putdef(cs, "_REENTRANT");
  if (s1->leading_underscore)
    putdef(cs, "__leading_underscore");
  cstr_printf(cs, "#define __SIZEOF_POINTER__ %d\n", PTR_SIZE);
  cstr_printf(cs, "#define __SIZEOF_LONG__ %d\n", LONG_SIZE);
  cstr_printf(cs, "#define __SIZEOF_INT__ 4\n");
  cstr_printf(cs, "#define __SIZEOF_SHORT__ 2\n");
  cstr_printf(cs, "#define __SIZEOF_LONG_LONG__ 8\n");
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
       so it is emitted here -- one line instead of 3.7 KB. */
    {
      int ri;
      for (ri = 0; tcc_predef_raw[ri]; ri++)
      {
        cstr_cat(cs, tcc_predef_raw[ri], -1);
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
    if (s1->output_type != TCC_OUTPUT_PREPROCESS &&
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
  pp_expr = 0;
  pp_counter = 0;
  pp_debug_tok = pp_debug_symv = 0;
  s1->pack_stack[0] = 0;
  s1->pack_stack_ptr = s1->pack_stack;

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
  while (macro_stack)
    end_macro();
  macro_ptr = NULL;
  while (file)
    tcc_close();
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
  tal_new(&toksym_alloc, TOKSYM_TAL_LIMIT, TOKSYM_TAL_SIZE);
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
  tok_str_new(&unget_buf);

  /* Reserve the whole builtin token id range; build the static keyword index
     (no heap).  Builtin TokenSyms are materialized lazily on first use rather
     than interned eagerly here. */
  if (!kw_index_built)
    kw_index_build();
  tok_ident = TOK_IDENT + NB_BUILTIN_TOKS;

  /* we add dummy defines for some special macros to speed up tests
     and to have working defined() */
  define_push(TOK___LINE__, MACRO_OBJ, NULL, NULL);
  define_push(TOK___FILE__, MACRO_OBJ, NULL, NULL);
  define_push(TOK___DATE__, MACRO_OBJ, NULL, NULL);
  define_push(TOK___TIME__, MACRO_OBJ, NULL, NULL);
  define_push(TOK___COUNTER__, MACRO_OBJ, NULL, NULL);
}

ST_FUNC void tccpp_delete(TCCState *s)
{
  int i, n;

  dynarray_reset(&s->cached_includes, &s->nb_cached_includes);

  /* free tokens */
  n = tok_ident - TOK_IDENT;
  if (n > total_idents)
    total_idents = n;
  for (i = 0; i < n; i++)
    tal_free(toksym_alloc, table_ident[i]);
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
  if (unget_buf.allocated_len > 0)
    tok_str_free_str(unget_buf.data.str);

  /* free string pool (currently unused)
  tal_delete(strpool_alloc);
  strpool_alloc = NULL;
  memset(strpool_hash, 0, sizeof(strpool_hash));
  */

  /* free allocators */
  tal_delete(toksym_alloc);
  toksym_alloc = NULL;
  tal_delete(tokstr_alloc);
  tokstr_alloc = NULL;
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
  else if (level == 0 && f->line_ref && d < 8)
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
    fprintf(s1->ppfp, "# %d \"%s\"%s\n", f->line_num, f->filename, level > 0 ? " 1" : level < 0 ? " 2" : "");
  }
  f->line_ref = f->line_num;
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

    white[spcs] = 0, fputs(white, s1->ppfp), spcs = 0;
    fputs(p = get_tok_str(tok, &tokc), s1->ppfp);
    token_seen = pp_check_he0xE(tok, p);
  }
  return 0;
}

/* ------------------------------------------------------------------------- */
