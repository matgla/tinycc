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
#define TOK_IDENT_PREALLOC 8192
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
static void parse_string(const char *p, int len);
static void tcc_pch_capture_reset(TCCState *s1);
static int tcc_pch_in_root_closure(BufferedFile *bf);
static void tcc_pch_flush_replay_tokens(TCCState *s1);
static void tcc_pch_add_replay_record(TCCState *s1, int kind, unsigned arg0, unsigned arg1);
static void tcc_pch_record_pack(TCCState *s1, int kind, int value);
static void tcc_pch_try_auto_include(TCCState *s1, const char *filename);
static uint64_t tcc_pch_hash_init(void);
static uint64_t tcc_pch_hash_bytes(uint64_t hash, const void *data, size_t len);
static inline uint32_t tcc_pch_usec(void);

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
  return ts;
}

#define TOK_HASH_INIT 1
#define TOK_HASH_FUNC(h, c) ((h) + ((h) << 5) + ((h) >> 27) + (c))

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
      return table_ident[v - TOK_IDENT]->str;
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
  table_ident[v - TOK_IDENT]->sym_define = s;

  if (o && !macro_is_equal(o->d, s->d))
    tcc_warning("%s redefined", get_tok_str(v, NULL));
}

/* undefined a define symbol. Its name is just set to zero */
ST_FUNC void define_undef(Sym *s)
{
  int v = s->v;
  if (v >= TOK_IDENT && v < tok_ident)
  {

    table_ident[v - TOK_IDENT]->sym_define = NULL;
  }
}

ST_INLN Sym *define_find(int v)
{
  v -= TOK_IDENT;
  if ((unsigned)v >= (unsigned)(tok_ident - TOK_IDENT))
  {
    return NULL;
  }
  return table_ident[v]->sym_define;
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
static int tcc_pch_begin_include(TCCState *s1, const char *filename, int include_next_index);
static int tcc_pch_next_replay_token(TCCState *s1);

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
    if (!test && (!e || (!define_find(e->ifndef_macro) && !e->once)))
      tcc_pch_try_auto_include(s1, buf);
    if (!test && tcc_pch_begin_include(s1, buf, i))
      return 1;
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
    if (s1->output_type == TCC_OUTPUT_PCH && tcc_pch_in_root_closure(file))
    {
      search_cached_include(s1, file->true_filename, 1);
      dynarray_add(&s1->target_deps, &s1->nb_target_deps, tcc_strdup(file->true_filename));
    }
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
      if (s1->output_type == TCC_OUTPUT_PCH && tcc_pch_in_root_closure(file))
        s1->pch_macro_depth++;
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
      if (t == TOK_pop_macro && s1->output_type == TCC_OUTPUT_PCH && tcc_pch_in_root_closure(file))
        s1->pch_macro_depth--;
      table_ident[v - TOK_IDENT]->sym_define = s->d ? s : NULL;
    }
    else
    {
      if (s1->output_type == TCC_OUTPUT_PCH && tcc_pch_in_root_closure(file))
        tcc_error("unbalanced #pragma pop_macro is not supported in PCH generation");
      else
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
      if (s1->pack_stack_ptr <= s1->pack_stack)
      {
      stk_error:
        tcc_error("out of pack stack");
      }
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
          if (s1->pack_stack_ptr >= s1->pack_stack + PACK_STACK_SIZE - 1)
            goto stk_error;
          val = *s1->pack_stack_ptr++;
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
      *s1->pack_stack_ptr = val;
      if (!rec_kind)
        rec_kind = TCC_PCH_REPLAY_PACK_SET;
      rec_value = val;
    }
    if (tok != ')')
      goto pragma_err;
    if (s1->output_type == TCC_OUTPUT_PCH)
      tcc_pch_record_pack(s1, rec_kind, rec_value);
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
        if (s1->output_type == TCC_OUTPUT_PCH && tcc_pch_in_root_closure(file))
          tcc_error("#pragma option is not supported in PCH generation");
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
        if (s1->output_type == TCC_OUTPUT_PCH && (tok_flags & TOK_FLAG_ENDIF) && tcc_pch_in_root_closure(file))
          search_cached_include(s1, file->true_filename, 1)->ifndef_macro = file->ifndef_macro_saved;
        tok = TOK_EOF;
      }
      else if (s1->pch && s1->pch->replay_active && s1->pch->replay_file == file)
      {
        /* PCH replay buffer file hit EOF — don't pop it here.
           Return to next() which will inject replay tokens, then
           tcc_pch_finish_replay() will close this file. */
        file->buf_ptr = p;
        tok = TOK_LINEFEED;
        goto keep_tok_flags;
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

/* return next token with macro substitution */
ST_FUNC HOT void next(void)
{
  int t;
  TCCState *s1 = tcc_state;

retry_from_pch:
  if (tcc_pch_next_replay_token(s1))
    goto retry_from_pch;
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
    return;
  }

  if (s1->pch && s1->pch->replay_active)
    goto retry_from_pch;

  next_nomacro();
  /* If PCH replay was activated during preprocess() inside next_nomacro(),
     discard the dummy token and let the replay inject its tokens first. */
  if (s1->pch && s1->pch->replay_active)
    goto retry_from_pch;
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
    return;
  }

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
    cstr_cat(cs,
    /* load more predefs and __builtins */
#if CONFIG_TCC_PREDEFS
#include "tccdefs_.h" /* include as strings */
#else
        "#include <tccdefs.h>\n" /* load at runtime */
#endif
             , -1);
  }
  if (include_base_file)
    cstr_printf(cs, "#define __BASE_FILE__ \"%s\"\n", file->filename);
}

ST_FUNC void preprocess_start(TCCState *s1, int filetype)
{
  int is_asm = !!(filetype & (AFF_TYPE_ASM | AFF_TYPE_ASMPP));

  tccpp_new(s1);
  tcc_pch_free(s1);
  tcc_pch_capture_reset(s1);

  s1->include_stack_ptr = s1->include_stack;
  s1->ifdef_stack_ptr = s1->ifdef_stack;
  file->ifdef_stack_ptr = s1->ifdef_stack_ptr;
  pp_expr = 0;
  pp_counter = 0;
  pp_debug_tok = pp_debug_symv = 0;
  s1->pack_stack[0] = 0;
  s1->pack_stack_ptr = s1->pack_stack;
  s1->pch_ident_start = tok_ident;
  s1->pch_filetype = filetype;
  tcc_pch_try_load(s1, filetype);

  set_idnum('$', !is_asm && s1->dollars_in_identifiers ? IS_ID : 0);
  set_idnum('.', is_asm ? IS_ID : 0);

  if (!(filetype & AFF_TYPE_ASM))
  {
    CString cstr;
    cstr_new(&cstr);
    /* Build predefines WITHOUT __BASE_FILE__ first so we can hash
       the content for PCH validation before appending the per-file part. */
    {
      tcc_predefs_base(s1, &cstr, is_asm, 0);
      if (s1->cmdline_defs.size)
        cstr_cat(&cstr, s1->cmdline_defs.data, s1->cmdline_defs.size);
      if (s1->cmdline_incl.size)
        cstr_cat(&cstr, s1->cmdline_incl.data, s1->cmdline_incl.size);
      /* Cache predefines hash now to avoid regenerating during PCH load. */
      if (!s1->pch_predefines_hash_valid)
      {
        s1->pch_predefines_hash_cached = tcc_pch_hash_bytes(tcc_pch_hash_init(), cstr.data, cstr.size);
        s1->pch_predefines_hash_valid = 1;
      }
    }
    /* Now append __BASE_FILE__ for actual preprocessing. */
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
  tcc_pch_free(s1);
  tcc_pch_capture_reset(s1);
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
  int i, c;
  const char *p, *r;

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
  token_lookup_cache_clear();
  memset(hash_ident, 0, TOK_HASH_SIZE * sizeof(TokenSym *));
  memset(s->cached_includes_hash, 0, sizeof s->cached_includes_hash);

  cstr_new(&tokcstr);
  cstr_new(&cstr_buf);
  cstr_realloc(&cstr_buf, STRING_MAX_SIZE);
  tok_str_new(&tokstr_buf);
  tok_str_realloc(&tokstr_buf, TOKSTR_MAX_SIZE);
  tok_str_new(&unget_buf);

  tok_ident = TOK_IDENT;
  p = tcc_keywords;
  while (*p)
  {
    r = p;
    for (;;)
    {
      c = *r++;
      if (c == '\0')
        break;
    }
    tok_alloc(p, r - p - 1);
    p = r;
  }

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

static void tcc_pch_capture_reset(TCCState *s1)
{
  if (s1->pch_token_blob.allocated_len > 0)
    tok_str_free_str(s1->pch_token_blob.data.str);
  tok_str_new(&s1->pch_token_blob);
  tcc_free(s1->pch_replay_records);
  s1->pch_replay_records = NULL;
  s1->nb_pch_replay_records = 0;
  s1->alloc_pch_replay_records = 0;
  s1->pch_replay_token_start = 0;
  s1->pch_macro_depth = 0;
}

static int tcc_pch_in_root_closure(BufferedFile *bf)
{
  while (bf)
  {
    if (!strcmp(bf->filename, "<command line>"))
      return 0;
    bf = bf->prev;
  }
  return 1;
}

static void tcc_pch_flush_replay_tokens(TCCState *s1)
{
  int len = s1->pch_token_blob.len - s1->pch_replay_token_start;

  if (len <= 0)
    return;
  tcc_pch_add_replay_record(s1, TCC_PCH_REPLAY_TOKENS, s1->pch_replay_token_start, len);
  s1->pch_replay_token_start = s1->pch_token_blob.len;
}

static void tcc_pch_add_replay_record(TCCState *s1, int kind, unsigned arg0, unsigned arg1)
{
  TCCPCHReplayRecord *rec;

  if (s1->nb_pch_replay_records >= s1->alloc_pch_replay_records)
  {
    int new_alloc = s1->alloc_pch_replay_records ? s1->alloc_pch_replay_records << 1 : 16;
    s1->pch_replay_records = tcc_realloc(s1->pch_replay_records, new_alloc * sizeof(*s1->pch_replay_records));
    s1->alloc_pch_replay_records = new_alloc;
  }
  rec = &s1->pch_replay_records[s1->nb_pch_replay_records++];
  rec->kind = kind;
  rec->arg0 = arg0;
  rec->arg1 = arg1;
}

static void tcc_pch_record_pack(TCCState *s1, int kind, int value)
{
  if (!tcc_pch_in_root_closure(file))
    return;
  tcc_pch_flush_replay_tokens(s1);
  tcc_pch_add_replay_record(s1, kind, value, 0);
}

/* Microsecond timestamp for PCH timing diagnostics. */
static inline uint32_t tcc_pch_usec(void)
{
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return (uint32_t)(tv.tv_sec * 1000000u + tv.tv_usec);
}

static uint64_t tcc_pch_hash_init(void)
{
  return 14695981039346656037ULL;
}

static uint64_t tcc_pch_hash_bytes(uint64_t hash, const void *data, size_t len)
{
  const unsigned char *p = data;

  while (len--)
  {
    hash ^= *p++;
    hash *= 1099511628211ULL;
  }
  return hash;
}

static uint64_t tcc_pch_hash_cstr(uint64_t hash, const char *str)
{
  return tcc_pch_hash_bytes(hash, str, strlen(str) + 1);
}

static uint32_t tcc_pch_host_endianness(void)
{
  const uint16_t value = 0x0102;
  return *(const unsigned char *)&value == 0x02 ? 1 : 2;
}

/* When cross-compiling with a non-trivial sysroot, strip the sysroot
   prefix from a resolved path so that PCH files store target-relative
   paths and hashes that match the native target compiler. */
static const char *tcc_pch_strip_sysroot(const char *path)
{
#ifdef CONFIG_SYSROOT
  static const char sysroot[] = CONFIG_SYSROOT;
  size_t len = sizeof(sysroot) - 1;
  if (len > 1 && strncmp(path, sysroot, len) == 0 && (path[len] == '/' || path[len] == '\0'))
    return path + len;
#endif
  return path;
}

/* Normalize a resolved path for PCH include-path hashing.  Replaces the
   tcc_lib_path prefix with the canonical "{B}" placeholder and strips
   CONFIG_SYSROOT from remaining paths.  This ensures cross-compiled and
   native PCH files produce identical include-path hashes. */
static const char *tcc_pch_normalize_for_hash(const char *resolved,
                                              const char *resolved_lib,
                                              size_t resolved_lib_len,
                                              char *buf, size_t buf_size)
{
  if (resolved_lib_len > 0 &&
      strncmp(resolved, resolved_lib, resolved_lib_len) == 0 &&
      (resolved[resolved_lib_len] == '/' || resolved[resolved_lib_len] == '\0'))
  {
    snprintf(buf, buf_size, "{B}%s", resolved + resolved_lib_len);
    return buf;
  }
  return tcc_pch_strip_sysroot(resolved);
}

static uint64_t tcc_pch_hash_include_paths(TCCState *s1)
{
  uint64_t hash;
  int i;
  size_t lib_len;
  char norm_buf[1024];

  if (s1->pch_include_path_hash_valid)
    return s1->pch_include_path_hash_cached;

  hash = tcc_pch_hash_init();
  lib_len = s1->tcc_lib_path ? strlen(s1->tcc_lib_path) : 0;

  /* Hash a canonical placeholder for tcc_lib_path so that cross-compiled
     and native PCH files produce identical hashes. */
  hash = tcc_pch_hash_cstr(hash, "{B}");
  if (s1->pch_verbose)
    fprintf(stderr, "pch: include path hash: tcc_lib_path='%s'\n",
      s1->tcc_lib_path ? s1->tcc_lib_path : "(null)");

  /* Normalize paths using direct string matching against tcc_lib_path
     and CONFIG_SYSROOT instead of expensive realpath() syscalls.
     The include paths are already absolute (set by configure/Makefile)
     and {B} was substituted with tcc_lib_path by tcc_split_path(),
     so string prefix matching produces the same canonical result. */
  for (i = 0; i < s1->nb_include_paths; ++i) {
    const char *norm = tcc_pch_normalize_for_hash(s1->include_paths[i],
                                 s1->tcc_lib_path, lib_len,
                                 norm_buf, sizeof(norm_buf));
    if (s1->pch_verbose)
      fprintf(stderr, "pch:   include[%d] raw='%s' norm='%s'\n", i, s1->include_paths[i], norm);
    hash = tcc_pch_hash_cstr(hash, norm);
  }
  for (i = 0; i < s1->nb_sysinclude_paths; ++i) {
    const char *norm = tcc_pch_normalize_for_hash(s1->sysinclude_paths[i],
                                 s1->tcc_lib_path, lib_len,
                                 norm_buf, sizeof(norm_buf));
    if (s1->pch_verbose)
      fprintf(stderr, "pch:   sysinclude[%d] raw='%s' norm='%s'\n", i, s1->sysinclude_paths[i], norm);
    hash = tcc_pch_hash_cstr(hash, norm);
  }
  s1->pch_include_path_hash_cached = hash;
  s1->pch_include_path_hash_valid = 1;
  return hash;
}

static uint64_t tcc_pch_hash_predefines(TCCState *s1, int filetype)
{
  int is_asm;
  CString cstr;
  uint64_t hash;

  if (s1->pch_predefines_hash_valid)
    return s1->pch_predefines_hash_cached;

  is_asm = !!(filetype & (AFF_TYPE_ASM | AFF_TYPE_ASMPP));
  cstr_new(&cstr);
  tcc_predefs_base(s1, &cstr, is_asm, 0);
  if (s1->cmdline_defs.size)
    cstr_cat(&cstr, s1->cmdline_defs.data, s1->cmdline_defs.size);
  if (s1->cmdline_incl.size)
    cstr_cat(&cstr, s1->cmdline_incl.data, s1->cmdline_incl.size);
  hash = tcc_pch_hash_bytes(tcc_pch_hash_init(), cstr.data, cstr.size);
  if (s1->pch_verbose)
    fprintf(stderr, "pch: predefines (%d bytes):\n%.*s\n", (int)cstr.size, (int)cstr.size, (const char *)cstr.data);
  cstr_free(&cstr);
  s1->pch_predefines_hash_cached = hash;
  s1->pch_predefines_hash_valid = 1;
  return hash;
}

static int tcc_pch_tokstream_words(const int *str)
{
  const int *start = str;
  CValue cv;
  int t;

  if (!str)
    return 0;
  for (;;)
  {
    TOK_GET(&t, &str, &cv);
    if (t == 0)
      break;
  }
  return str - start;
}

static uint64_t tcc_pch_stat_mtime_nsec(const struct stat *st)
{
#if defined(__APPLE__) && defined(__MACH__)
  return st->st_mtimespec.tv_nsec;
#elif defined(__linux__)
  return st->st_mtim.tv_nsec;
#else
  (void)st;
  return 0;
#endif
}

static uint32_t tcc_pch_add_string(CString *strings, const char *str)
{
  uint32_t off = strings->size;
  cstr_cat(strings, str, strlen(str) + 1);
  return off;
}

static void tcc_pch_append_bytes(CString *sec, const void *data, size_t size)
{
  if (size)
    cstr_cat(sec, data, size);
}

static const char *tcc_pch_get_string(const TCCPCHState *pch, uint32_t off)
{
  if (!pch || off >= (uint32_t)pch->strings_size)
    return NULL;
  return pch->strings + off;
}

static int tcc_pch_validate_string(const char *strings, int strings_size, uint32_t off, uint32_t len)
{
  const char *str;
  size_t remain;
  const char *end;

  if (off >= (uint32_t)strings_size)
    return 0;
  str = strings + off;
  remain = strings_size - off;
  end = memchr(str, '\0', remain);
  if (!end)
    return 0;
  if (len != (uint32_t)(end - str))
    return 0;
  return 1;
}

static int tcc_pch_validate_cstr_off(const char *strings, int strings_size, uint32_t off)
{
  if (off >= (uint32_t)strings_size)
    return 0;
  return memchr(strings + off, '\0', strings_size - off) != NULL;
}

static int tcc_pch_remap_token(int *token_map, int token_map_len, int tok)
{
  int sym_field = tok & SYM_FIELD;
  int base = tok & ~SYM_FIELD;
  int idx = base - TOK_IDENT;

  if ((unsigned)idx < (unsigned)token_map_len && token_map[idx])
    base = token_map[idx];
  return base | sym_field;
}

static void tcc_pch_remap_tokstream(int *dst, const int *src, int words, int *token_map, int token_map_len)
{
  int *p, *end;

  /* Bulk copy, then remap identifier tokens in-place */
  memcpy(dst, src, words * sizeof(int));
  p = dst;
  end = dst + words;
  while (p < end)
  {
    int t = *p;
    if ((t & ~SYM_FIELD) >= TOK_IDENT)
      *p = tcc_pch_remap_token(token_map, token_map_len, t);
    p += tok_str_word_count(p);
  }
}

static void tcc_pch_state_delete(TCCPCHState *pch)
{
  if (!pch)
    return;
  tcc_free(pch->filename);
  tcc_free(pch->root_filename);
  tcc_free(pch->generator_version);
  /* After tcc_pch_apply(), token_blob is a separate allocation (remapped
     copy).  Before apply, it aliases into raw and must NOT be freed.  The
     applied flag distinguishes the two cases. */
  if (pch->applied)
    tcc_free(pch->token_blob);
  tcc_free(pch->raw);
  tcc_free(pch);
}

ST_FUNC void tcc_pch_free(TCCState *s1)
{
  tcc_pch_state_delete(s1->pch);
  s1->pch = NULL;
  s1->pch_is_auto = 0;
}

ST_FUNC void tcc_pch_auto_reset(TCCState *s1)
{
  int i;

  if (s1->pch_is_auto)
    tcc_pch_free(s1);
  for (i = 0; i < s1->nb_auto_pch_entries; ++i)
  {
    tcc_free(s1->auto_pch_entries[i].header_path);
    tcc_free(s1->auto_pch_entries[i].pch_name);
  }
  tcc_free(s1->auto_pch_entries);
  s1->auto_pch_entries = NULL;
  s1->nb_auto_pch_entries = 0;
  s1->pch_auto_index_loaded = 0;
}

static int tcc_pch_section_expected_size(uint32_t actual, uint32_t count, size_t elem_size)
{
  return actual == count * elem_size;
}

static const char *tcc_pch_auto_subdir(void)
{
#ifdef CONFIG_TCC_CROSSPREFIX
  return CONFIG_TCC_CROSSPREFIX;
#else
  return "native";
#endif
}

static int tcc_pch_is_auto_common_header(const char *filename)
{
  static const char *const common_headers[] = {"stdio.h", "stdlib.h", "string.h", NULL};
  const char *name = tcc_basename(filename);
  int i;

  for (i = 0; common_headers[i]; ++i)
    if (!PATHCMP(name, common_headers[i]))
      return 1;
  return 0;
}

static void tcc_pch_auto_index_path(TCCState *s1, char *buf, size_t buf_size)
{
  snprintf(buf, buf_size, "%s/pch/%s/auto.index", s1->tcc_lib_path, tcc_pch_auto_subdir());
}

static void tcc_pch_auto_file_path(TCCState *s1, const char *pch_name, char *buf, size_t buf_size)
{
  snprintf(buf, buf_size, "%s/pch/%s/%s", s1->tcc_lib_path, tcc_pch_auto_subdir(), pch_name);
}

static void tcc_pch_auto_add_entry(TCCState *s1, const char *header_path, const char *pch_name)
{
  int idx = s1->nb_auto_pch_entries++;
  s1->auto_pch_entries = tcc_realloc(s1->auto_pch_entries, s1->nb_auto_pch_entries * sizeof(*s1->auto_pch_entries));
  s1->auto_pch_entries[idx].header_path = tcc_strdup(header_path);
  s1->auto_pch_entries[idx].pch_name = tcc_strdup(pch_name);
  s1->auto_pch_entries[idx].disabled = 0;
}

static int tcc_pch_auto_load_index(TCCState *s1)
{
  char index_path[1024];
  FILE *fp;
  char line[4096];

  if (s1->pch_auto_index_loaded)
    return s1->nb_auto_pch_entries != 0;

  s1->pch_auto_index_loaded = 1;
  tcc_pch_auto_index_path(s1, index_path, sizeof(index_path));
  fp = fopen(index_path, "r");
  if (!fp)
    return 0;

  while (fgets(line, sizeof(line), fp))
  {
    char *header_path;
    char *sep;
    char *end;

    for (header_path = line; *header_path == ' ' || *header_path == '\t'; ++header_path)
      ;
    if (*header_path == '\0' || *header_path == '\n' || *header_path == '#')
      continue;
    for (sep = header_path; *sep && *sep != '\t'; ++sep)
      ;
    if (*sep != '\t')
      continue;
    *sep++ = '\0';
    for (end = sep; *end && *end != '\r' && *end != '\n'; ++end)
      ;
    if (*end)
      *end = '\0';
    if (!*sep)
      continue;
    tcc_pch_auto_add_entry(s1, header_path, sep);
  }
  fclose(fp);
  return s1->nb_auto_pch_entries != 0;
}

static int tcc_pch_try_load_file(TCCState *s1, const char *filename, int filetype)
{
  char *saved = s1->pch_infile;
  int ret;

  s1->pch_infile = (char *)filename;
  ret = tcc_pch_try_load(s1, filetype);
  s1->pch_infile = saved;
  return ret;
}

/* Compare two paths ignoring redundant slashes (e.g. "//usr" == "/usr"). */
static int tcc_pch_pathcmp(const char *a, const char *b)
{
  for (;;) {
    while (*a == '/' && a[1] == '/') ++a;
    while (*b == '/' && b[1] == '/') ++b;
    if (*a != *b) return (unsigned char)*a - (unsigned char)*b;
    if (*a == '\0') return 0;
    ++a; ++b;
  }
}

static void tcc_pch_try_auto_include(TCCState *s1, const char *filename)
{
  char pch_path[1024];
  char *resolved_filename = NULL;
  int i;
  if (!s1->pch_auto_enabled || (s1->pch_infile && *s1->pch_infile) || !tcc_pch_is_auto_common_header(filename))
    return;
  if (s1->pch_filetype & (AFF_TYPE_ASM | AFF_TYPE_ASMPP))
    return;
  if (s1->pch)
  {
    if (s1->pch->replay_active || !s1->pch->consumed)
      return;
    if (!s1->pch_is_auto)
      return;
    tcc_pch_free(s1);
  }
  if (!tcc_pch_auto_load_index(s1))
  {
    if (s1->pch_verbose)
      fprintf(stderr, "pch: no auto.index found for '%s'\n", filename);
    return;
  }

  for (i = 0; i < s1->nb_auto_pch_entries; ++i)
  {
    TCCAutoPCHEntry *entry = &s1->auto_pch_entries[i];

    if (entry->disabled)
      continue;
    if (tcc_pch_pathcmp(entry->header_path, filename))
    {
      if (!resolved_filename)
        resolved_filename = realpath(filename, NULL);
      if (!resolved_filename || tcc_pch_pathcmp(entry->header_path, resolved_filename))
      {
        if (s1->pch_verbose)
          fprintf(stderr, "pch: path mismatch: entry='%s' file='%s' resolved='%s'\n",
            entry->header_path, filename, resolved_filename ? resolved_filename : "(null)");
        continue;
      }
    }
    tcc_pch_auto_file_path(s1, entry->pch_name, pch_path, sizeof(pch_path));
    if (s1->pch_verbose)
      fprintf(stderr, "pch: trying '%s' for '%s'\n", pch_path, filename);
    if (!tcc_pch_try_load_file(s1, pch_path, s1->pch_filetype))
    {
      entry->disabled = 1;
      continue;
    }
    if (!s1->pch)
    {
      entry->disabled = 1;
      continue;
    }
    if (tcc_pch_pathcmp(s1->pch->root_filename, filename))
    {
      if (!resolved_filename)
        resolved_filename = realpath(filename, NULL);
      if (!resolved_filename || tcc_pch_pathcmp(s1->pch->root_filename, resolved_filename))
      {
        entry->disabled = 1;
        tcc_pch_free(s1);
        continue;
      }
    }
    s1->pch_is_auto = 1;
    libc_free(resolved_filename);
    return;
  }
  libc_free(resolved_filename);
}

ST_FUNC int tcc_pch_try_load(TCCState *s1, int filetype)
{
  static const uint32_t section_kinds[] = {
      TCC_PCH_SEC_STRINGS,      TCC_PCH_SEC_IDENTS,      TCC_PCH_SEC_TOKEN_BLOBS, TCC_PCH_SEC_MACROS,
      TCC_PCH_SEC_MACRO_ARGS,   TCC_PCH_SEC_REPLAY_RECORDS, TCC_PCH_SEC_CACHED_INCLUDES,
      TCC_PCH_SEC_DEPENDENCIES, TCC_PCH_SEC_PRAGMA_LIBS, TCC_PCH_SEC_MANIFEST,
  };
  TCCPCHFileHeader hdr;
  TCCPCHSectionHeader dirs[sizeof(section_kinds) / sizeof(section_kinds[0])];
  TCCPCHManifestRecord manifest;
  TCCPCHState *pch = NULL;
  unsigned char *raw = NULL;
  FILE *fp = NULL;
  size_t raw_size;
  long raw_size_l;
  uint64_t dependency_hash = tcc_pch_hash_init();
  int expected_builtin_count = s1->pch_ident_start - TOK_IDENT;
  int builtin_token_delta = 0;
  int max_token;
  int i;

  tcc_pch_free(s1);
  if (!s1->pch_infile || !*s1->pch_infile)
    return 0;

  fp = fopen(s1->pch_infile, "rb");
  if (!fp)
  {
    tcc_warning("ignoring PCH '%s': could not open file", s1->pch_infile);
    return 0;
  }
  if (fseek(fp, 0, SEEK_END) != 0 || (raw_size_l = ftell(fp)) < 0 || fseek(fp, 0, SEEK_SET) != 0)
  {
    tcc_warning("ignoring PCH '%s': could not read file size", s1->pch_infile);
    fclose(fp);
    return 0;
  }
  raw_size = (size_t)raw_size_l;
  if (raw_size < sizeof(hdr) + sizeof(dirs))
  {
    tcc_warning("ignoring PCH '%s': file too small", s1->pch_infile);
    fclose(fp);
    return 0;
  }

  raw = tcc_malloc(raw_size);
  if (fread(raw, raw_size, 1, fp) != 1)
  {
    tcc_warning("ignoring PCH '%s': could not read file", s1->pch_infile);
    fclose(fp);
    tcc_free(raw);
    return 0;
  }
  fclose(fp);
  fp = NULL;

  memcpy(&hdr, raw, sizeof(hdr));
  memcpy(dirs, raw + sizeof(hdr), sizeof(dirs));

  if (hdr.magic != TCC_PCH_MAGIC)
    goto ignore_bad_magic;
  if (hdr.version != TCC_PCH_VERSION || hdr.header_size != sizeof(hdr))
  {
    tcc_warning("ignoring PCH '%s': unsupported format version", s1->pch_infile);
    goto fail;
  }
  if (hdr.section_count != sizeof(section_kinds) / sizeof(section_kinds[0]))
  {
    tcc_warning("ignoring PCH '%s': unexpected section count", s1->pch_infile);
    goto fail;
  }
  if (hdr.endianness != tcc_pch_host_endianness() || hdr.sizeof_int != sizeof(int) || hdr.long_size != LONG_SIZE ||
      hdr.ldouble_size != LDOUBLE_SIZE || hdr.ptr_size != PTR_SIZE || hdr.tok_ident_value != TOK_IDENT)
  {
    if (s1->pch_verbose)
      fprintf(stderr, "pch: ABI mismatch: endian=%u/%u sizeof_int=%u/%zu long=%u/%d ldouble=%u/%d ptr=%u/%d tok_ident=%u/%d\n",
        hdr.endianness, tcc_pch_host_endianness(), hdr.sizeof_int, sizeof(int),
        hdr.long_size, LONG_SIZE, hdr.ldouble_size, LDOUBLE_SIZE,
        hdr.ptr_size, PTR_SIZE, hdr.tok_ident_value, TOK_IDENT);
    tcc_warning("ignoring PCH '%s': ABI mismatch", s1->pch_infile);
    goto fail;
  }
  {
    uint64_t local_kw_hash = tcc_pch_hash_bytes(tcc_pch_hash_init(), tcc_keywords, sizeof(tcc_keywords));
    if (hdr.keywords_hash != local_kw_hash)
    {
      if (s1->pch_verbose)
        fprintf(stderr, "pch: keyword hash: pch=0x%016llx local=0x%016llx sizeof_keywords=%lu\n",
          (unsigned long long)hdr.keywords_hash, (unsigned long long)local_kw_hash, (unsigned long)sizeof(tcc_keywords));
      tcc_warning("ignoring PCH '%s': keyword table mismatch", s1->pch_infile);
      goto fail;
    }
  }
  builtin_token_delta = expected_builtin_count - (int)hdr.builtin_ident_count;
  if (hdr.filetype != (uint32_t)(filetype & AFF_TYPE_MASK))
  {
    tcc_warning("ignoring PCH '%s': file type mismatch", s1->pch_infile);
    goto fail;
  }
#if defined(TCC_TARGET_ARM) || defined(TCC_TARGET_ARM_THUMB)
  if (hdr.float_abi != (uint32_t)s1->float_abi || hdr.fpu_type != (uint32_t)s1->fpu_type)
  {
    tcc_warning("ignoring PCH '%s': float ABI mismatch", s1->pch_infile);
    goto fail;
  }
#endif
  if (hdr.predefines_hash != tcc_pch_hash_predefines(s1, filetype))
  {
    if (s1->pch_verbose)
      fprintf(stderr, "pch: predefines hash: pch=0x%016llx local=0x%016llx\n",
        (unsigned long long)hdr.predefines_hash,
        (unsigned long long)tcc_pch_hash_predefines(s1, filetype));
    tcc_warning("ignoring PCH '%s': predefined macro state mismatch", s1->pch_infile);
    goto fail;
  }
  if (hdr.include_path_hash != tcc_pch_hash_include_paths(s1))
  {
    if (s1->pch_verbose)
      fprintf(stderr, "pch: include path hash: pch=0x%016llx local=0x%016llx\n",
        (unsigned long long)hdr.include_path_hash,
        (unsigned long long)tcc_pch_hash_include_paths(s1));
    tcc_warning("ignoring PCH '%s': include path mismatch", s1->pch_infile);
    goto fail;
  }

  pch = tcc_mallocz(sizeof(*pch));
  pch->filename = tcc_strdup(s1->pch_infile);
  pch->counter_delta = hdr.counter_delta;
  pch->filetype = hdr.filetype;
  pch->builtin_token_delta = builtin_token_delta;
  pch->nb_idents = hdr.ident_count;
  pch->nb_macros = hdr.macro_count;
  pch->nb_macro_args = hdr.macro_arg_count;
  pch->nb_replay_records = hdr.replay_count;
  pch->nb_cached_includes = hdr.include_count;
  pch->nb_dependencies = hdr.dependency_count;
  pch->nb_pragma_libs = hdr.pragma_lib_count;

  for (i = 0; i < (int)(sizeof(section_kinds) / sizeof(section_kinds[0])); ++i)
  {
    size_t end_off = (size_t)dirs[i].offset + dirs[i].size;
    if (dirs[i].kind != section_kinds[i] || end_off < dirs[i].offset || end_off > raw_size)
    {
      tcc_warning("ignoring PCH '%s': malformed section directory", s1->pch_infile);
      goto fail;
    }
  }

  pch->strings_size = dirs[0].size;
  pch->strings = pch->strings_size ? (char *)(raw + dirs[0].offset) : NULL;

  if (!tcc_pch_section_expected_size(dirs[1].size, hdr.ident_count, sizeof(TCCPCHIdentRecord)) ||
      !tcc_pch_section_expected_size(dirs[3].size, hdr.macro_count, sizeof(TCCPCHMacroRecord)) ||
      !tcc_pch_section_expected_size(dirs[4].size, hdr.macro_arg_count, sizeof(TCCPCHMacroArgRecord)) ||
      !tcc_pch_section_expected_size(dirs[5].size, hdr.replay_count, sizeof(TCCPCHReplayRecord)) ||
      !tcc_pch_section_expected_size(dirs[6].size, hdr.include_count, sizeof(TCCPCHCachedIncludeRecord)) ||
      !tcc_pch_section_expected_size(dirs[7].size, hdr.dependency_count, sizeof(TCCPCHDependencyRecord)) ||
      !tcc_pch_section_expected_size(dirs[8].size, hdr.pragma_lib_count, sizeof(TCCPCHPragmaLibRecord)) ||
      dirs[9].size != sizeof(TCCPCHManifestRecord) || (dirs[2].size & (sizeof(int) - 1)))
  {
    tcc_warning("ignoring PCH '%s': malformed section sizes", s1->pch_infile);
    goto fail;
  }

  /* Point section pointers directly into the raw buffer to avoid
     malloc+memcpy overhead for each section. */
  if (hdr.ident_count)
    pch->idents = (TCCPCHIdentRecord *)(raw + dirs[1].offset);
  pch->token_blob_words = dirs[2].size / sizeof(int);
  if (dirs[2].size)
    pch->token_blob = (int *)(raw + dirs[2].offset);
  if (hdr.macro_count)
    pch->macros = (TCCPCHMacroRecord *)(raw + dirs[3].offset);
  if (hdr.macro_arg_count)
    pch->macro_args = (TCCPCHMacroArgRecord *)(raw + dirs[4].offset);
  if (hdr.replay_count)
    pch->replay_records = (TCCPCHReplayRecord *)(raw + dirs[5].offset);
  if (hdr.include_count)
    pch->cached_includes = (TCCPCHCachedIncludeRecord *)(raw + dirs[6].offset);
  if (hdr.dependency_count)
    pch->dependencies = (TCCPCHDependencyRecord *)(raw + dirs[7].offset);
  if (hdr.pragma_lib_count)
    pch->pragma_libs = (TCCPCHPragmaLibRecord *)(raw + dirs[8].offset);
  memcpy(&manifest, raw + dirs[9].offset, sizeof(manifest));

  if (!tcc_pch_validate_cstr_off(pch->strings, pch->strings_size, manifest.root_filename_off) ||
      !tcc_pch_validate_cstr_off(pch->strings, pch->strings_size, manifest.generator_version_off))
  {
    tcc_warning("ignoring PCH '%s': malformed manifest strings", s1->pch_infile);
    goto fail;
  }
  pch->root_filename = tcc_strdup(pch->strings + manifest.root_filename_off);
  pch->generator_version = tcc_strdup(pch->strings + manifest.generator_version_off);

  max_token = TOK_IDENT + hdr.builtin_ident_count + hdr.ident_count;
  for (i = 0; i < pch->nb_idents; ++i)
  {
    TCCPCHIdentRecord *rec = &pch->idents[i];
    if (rec->token != (uint32_t)(TOK_IDENT + hdr.builtin_ident_count + i) ||
        !tcc_pch_validate_string(pch->strings, pch->strings_size, rec->string_off, rec->len))
    {
      tcc_warning("ignoring PCH '%s': malformed identifier table", s1->pch_infile);
      goto fail;
    }
  }
  for (i = 0; i < pch->nb_macro_args; ++i)
  {
    TCCPCHMacroArgRecord *arg = &pch->macro_args[i];
    if (arg->token < TOK_IDENT || arg->token >= (uint32_t)max_token || arg->is_vaargs > 1)
    {
      tcc_warning("ignoring PCH '%s': malformed macro argument table", s1->pch_infile);
      goto fail;
    }
  }
  for (i = 0; i < pch->nb_macros; ++i)
  {
    TCCPCHMacroRecord *rec = &pch->macros[i];
    if (rec->name_token < TOK_IDENT || rec->name_token >= (uint32_t)max_token ||
        (rec->macro_type & ~(MACRO_FUNC | MACRO_JOIN)) != 0 ||
        rec->arg_start + rec->arg_count > (uint32_t)pch->nb_macro_args ||
        rec->tok_blob_off_words + rec->tok_blob_len_words > (uint32_t)pch->token_blob_words)
    {
      tcc_warning("ignoring PCH '%s': malformed macro table", s1->pch_infile);
      goto fail;
    }
  }
  for (i = 0; i < pch->nb_replay_records; ++i)
  {
    TCCPCHReplayRecord *rec = &pch->replay_records[i];
    switch (rec->kind)
    {
    case TCC_PCH_REPLAY_TOKENS:
      if (!rec->arg1 || rec->arg0 + rec->arg1 > (uint32_t)pch->token_blob_words)
      {
        tcc_warning("ignoring PCH '%s': malformed replay token range", s1->pch_infile);
        goto fail;
      }
      break;
    case TCC_PCH_REPLAY_PACK_SET:
    case TCC_PCH_REPLAY_PACK_PUSH:
      if (rec->arg0 > 16)
      {
        tcc_warning("ignoring PCH '%s': malformed replay pack record", s1->pch_infile);
        goto fail;
      }
      break;
    case TCC_PCH_REPLAY_PACK_POP:
      break;
    default:
      tcc_warning("ignoring PCH '%s': unsupported replay record kind", s1->pch_infile);
      goto fail;
    }
  }
  for (i = 0; i < pch->nb_cached_includes; ++i)
  {
    TCCPCHCachedIncludeRecord *rec = &pch->cached_includes[i];
    if (!tcc_pch_validate_cstr_off(pch->strings, pch->strings_size, rec->filename_off) ||
        (rec->ifndef_macro != 0xffffffffu && (rec->ifndef_macro < TOK_IDENT || rec->ifndef_macro >= (uint32_t)max_token)))
    {
      tcc_warning("ignoring PCH '%s': malformed cached-include table", s1->pch_infile);
      goto fail;
    }
  }
  for (i = 0; i < pch->nb_dependencies; ++i)
  {
    TCCPCHDependencyRecord *rec = &pch->dependencies[i];
    struct stat st;
    const char *dep;

    if (!tcc_pch_validate_cstr_off(pch->strings, pch->strings_size, rec->filename_off))
    {
      tcc_warning("ignoring PCH '%s': malformed dependency table", s1->pch_infile);
      goto fail;
    }
    dep = pch->strings + rec->filename_off;
    /* When mtime is zero the PCH was cross-generated; on a read-only
       rootfs the files cannot change, so skip the expensive stat()
       call and trust the stored values for the dependency hash. */
    if (rec->mtime_sec != 0)
    {
      /* Dependency paths are stored normalized: sysroot-stripped for
         system headers, or with {B} placeholder for tcc lib paths.
         Resolve to real filesystem paths for stat(). */
      char dep_buf[1024];
      const char *dep_path = dep;
      if (strncmp(dep, "{B}", 3) == 0) {
        snprintf(dep_buf, sizeof(dep_buf), "%s%s", s1->tcc_lib_path, dep + 3);
        dep_path = dep_buf;
      }
#ifdef CONFIG_SYSROOT
      else {
        static const char sysroot[] = CONFIG_SYSROOT;
        if (sizeof(sysroot) > 2 && dep[0] == '/') {
          snprintf(dep_buf, sizeof(dep_buf), "%s%s", sysroot, dep);
          dep_path = dep_buf;
        }
      }
#endif
      if (stat(dep_path, &st) < 0)
      {
        tcc_warning("ignoring PCH '%s': dependency '%s' is missing", s1->pch_infile, dep);
        goto fail;
      }
      if ((uint64_t)st.st_size != rec->size ||
          (uint64_t)st.st_mtime != rec->mtime_sec ||
          tcc_pch_stat_mtime_nsec(&st) != rec->mtime_nsec)
      {
        tcc_warning("ignoring PCH '%s': dependency '%s' changed", s1->pch_infile, dep);
        goto fail;
      }
    }
    dependency_hash = tcc_pch_hash_cstr(dependency_hash, dep);
    dependency_hash = tcc_pch_hash_bytes(dependency_hash, &rec->size, sizeof(rec->size));
    dependency_hash = tcc_pch_hash_bytes(dependency_hash, &rec->mtime_sec, sizeof(rec->mtime_sec));
    dependency_hash = tcc_pch_hash_bytes(dependency_hash, &rec->mtime_nsec, sizeof(rec->mtime_nsec));
  }
  if (dependency_hash != hdr.dependency_hash)
  {
    if (s1->pch_verbose)
      fprintf(stderr, "pch: dependency hash: pch=0x%016llx local=0x%016llx\n",
        (unsigned long long)hdr.dependency_hash,
        (unsigned long long)dependency_hash);
    tcc_warning("ignoring PCH '%s': dependency hash mismatch", s1->pch_infile);
    goto fail;
  }
  for (i = 0; i < pch->nb_pragma_libs; ++i)
  {
    if (!tcc_pch_validate_cstr_off(pch->strings, pch->strings_size, pch->pragma_libs[i].string_off))
    {
      tcc_warning("ignoring PCH '%s': malformed pragma-lib table", s1->pch_infile);
      goto fail;
    }
  }

  pch->raw = raw;
  s1->pch = pch;
  return 1;

ignore_bad_magic:
  tcc_warning("ignoring PCH '%s': bad magic", s1->pch_infile);
fail:
  if (fp)
    fclose(fp);
  tcc_free(raw);
  tcc_pch_state_delete(pch);
  return 0;
}

static int tcc_pch_apply(TCCState *s1)
{
  TCCPCHState *pch = s1->pch;
  unsigned char *macro_present = NULL;
  int *token_map = NULL;
  int token_map_len;
  int i;

  if (!pch || pch->applied)
    return 1;

  token_map_len = s1->pch_ident_start - TOK_IDENT + pch->nb_idents;
  token_map = tcc_mallocz(token_map_len * sizeof(*token_map));
  for (i = 0; i < pch->nb_idents; ++i)
  {
    TCCPCHIdentRecord *rec = &pch->idents[i];
    const char *name = tcc_pch_get_string(pch, rec->string_off);
    int toknum;
    int map_index = rec->token - TOK_IDENT + pch->builtin_token_delta;

    if (!name || (unsigned)map_index >= (unsigned)token_map_len)
      goto apply_fail;
    toknum = tok_alloc(name, rec->len)->tok;
    token_map[map_index] = toknum;
  }

  if (pch->token_blob_words)
  {
    int *remapped_blob = tcc_malloc(pch->token_blob_words * sizeof(int));
    tcc_pch_remap_tokstream(remapped_blob, pch->token_blob, pch->token_blob_words, token_map, token_map_len);
    /* token_blob aliases into pch->raw, so don't free it; raw is freed
       with the pch state.  Just redirect the pointer. */
    pch->token_blob = remapped_blob;
  }
  for (i = 0; i < pch->nb_macros; ++i)
    pch->macros[i].name_token = tcc_pch_remap_token(token_map, token_map_len, pch->macros[i].name_token);
  for (i = 0; i < pch->nb_macro_args; ++i)
    pch->macro_args[i].token = tcc_pch_remap_token(token_map, token_map_len, pch->macro_args[i].token);
  for (i = 0; i < pch->nb_cached_includes; ++i)
    if (pch->cached_includes[i].ifndef_macro != 0xffffffffu)
      pch->cached_includes[i].ifndef_macro =
          tcc_pch_remap_token(token_map, token_map_len, pch->cached_includes[i].ifndef_macro);

  if (!s1->pch_is_auto)
  {
    /* For explicit PCH the header is the first thing included, so
       we can safely clear all macros not carried by the PCH.  For
       auto-PCH other headers may have been included first -- their
       macros (e.g.  INT_MAX from <limits.h>) must survive. */
    macro_present = tcc_mallocz(tok_ident - TOK_IDENT);
    for (i = 0; i < pch->nb_macros; ++i)
      macro_present[pch->macros[i].name_token - TOK_IDENT] = 1;
    for (i = TOK_IDENT; i < tok_ident; ++i)
    {
      Sym *s = define_find(i);
      if (s && !macro_present[i - TOK_IDENT]
          /* Preserve special predefined macros that are expanded at
             preprocessing time and never captured in PCH files. */
          && i != TOK___LINE__ && i != TOK___FILE__
          && i != TOK___DATE__ && i != TOK___TIME__
          && i != TOK___COUNTER__)
        define_undef(s);
    }
    tcc_free(macro_present);
    macro_present = NULL;
  }

  for (i = 0; i < pch->nb_macros; ++i)
  {
    TCCPCHMacroRecord *rec = &pch->macros[i];
    Sym *first = NULL;
    Sym **ps = &first;
    uint32_t j;
    int *body = NULL;
    Sym *macro;

    for (j = 0; j < rec->arg_count; ++j)
    {
      TCCPCHMacroArgRecord *arg_rec = &pch->macro_args[rec->arg_start + j];
      Sym *arg = sym_push2(&define_stack, arg_rec->token | SYM_FIELD, arg_rec->is_vaargs, 0);
      *ps = arg;
      ps = &arg->next;
    }
    if (rec->tok_blob_len_words)
    {
      body = tcc_malloc(rec->tok_blob_len_words * sizeof(int));
      memcpy(body, pch->token_blob + rec->tok_blob_off_words, rec->tok_blob_len_words * sizeof(int));
    }
    macro = sym_push2(&define_stack, rec->name_token, rec->macro_type, 0);
    macro->d = body;
    macro->next = first;
    table_ident[rec->name_token - TOK_IDENT]->sym_define = macro;
  }

  if (!s1->pch_is_auto)
  {
    /* Explicit PCH: replace all cached includes with PCH state. */
    dynarray_reset(&s1->cached_includes, &s1->nb_cached_includes);
    memset(s1->cached_includes_hash, 0, sizeof s1->cached_includes_hash);
  }
  for (i = 0; i < pch->nb_cached_includes; ++i)
  {
    TCCPCHCachedIncludeRecord *rec = &pch->cached_includes[i];
    CachedInclude *inc = search_cached_include(s1, tcc_pch_get_string(pch, rec->filename_off), 1);
    inc->ifndef_macro = rec->ifndef_macro == 0xffffffffu ? 0 : rec->ifndef_macro;
    inc->once = rec->once != 0;
  }

  dynarray_reset(&s1->pragma_libs, &s1->nb_pragma_libs);
  for (i = 0; i < pch->nb_pragma_libs; ++i)
    dynarray_add(&s1->pragma_libs, &s1->nb_pragma_libs, tcc_strdup(tcc_pch_get_string(pch, pch->pragma_libs[i].string_off)));

  pp_counter += pch->counter_delta;
  pch->applied = 1;
  tcc_free(token_map);
  return 1;

apply_fail:
  tcc_warning("ignoring PCH '%s': could not remap identifier table", pch->filename);
  tcc_free(macro_present);
  tcc_free(token_map);
  return 0;
}

static void tcc_pch_finish_replay(TCCState *s1)
{
  TCCPCHState *pch = s1->pch;
  int free_auto = s1->pch_is_auto;

  if (!pch)
    return;
  pch->replay_active = 0;
  pch->replay_ptr = NULL;
  pch->replay_end = NULL;
  pch->replay_index = pch->nb_replay_records;
  if (pch->replay_file == file)
  {
    tcc_debug_eincl(s1);
    tcc_close();
    if (s1->include_stack_ptr != s1->include_stack)
      s1->include_stack_ptr--;
  }
  pch->replay_file = NULL;
  if (free_auto)
    tcc_pch_free(s1);
}

static void tcc_pch_start_replay(TCCState *s1, const char *filename, int include_next_index)
{
  TCCPCHState *pch = s1->pch;

  if (s1->include_stack_ptr >= s1->include_stack + INCLUDE_STACK_SIZE)
    tcc_error("#include recursion too deep");

  tcc_open_bf(s1, filename, 0);
  *s1->include_stack_ptr++ = file->prev;
  file->include_next_index = include_next_index;
  pch->replay_file = file;

  pch->replay_active = 1;
  pch->replay_index = 0;
  pch->replay_ptr = NULL;
  pch->replay_end = NULL;

  tcc_debug_bincl(s1);
}

static int tcc_pch_next_replay_token(TCCState *s1)
{
  TCCPCHState *pch = s1->pch;

  while (pch && pch->replay_active && !macro_ptr)
  {
    if (pch->replay_index >= pch->nb_replay_records)
    {
      tcc_pch_finish_replay(s1);
      return 1;
    }

    switch (pch->replay_records[pch->replay_index].kind)
    {
    case TCC_PCH_REPLAY_TOKENS:
    {
      TokenString *str = tok_str_alloc();
      const int *src = pch->token_blob + pch->replay_records[pch->replay_index].arg0;
      int len = pch->replay_records[pch->replay_index].arg1;
      tok_str_add_words(str, src, len);
      if (str->len && tok_str_buf(str)[str->len - 1] == TOK_EOF)
        tok_str_buf(str)[str->len - 1] = 0;
      else
        tok_str_add(str, 0);
      begin_macro(str, 1);
      break;
    }
    case TCC_PCH_REPLAY_PACK_SET:
      *s1->pack_stack_ptr = pch->replay_records[pch->replay_index].arg0;
      break;
    case TCC_PCH_REPLAY_PACK_PUSH:
      if (s1->pack_stack_ptr >= s1->pack_stack + PACK_STACK_SIZE - 1)
        tcc_error("out of pack stack during PCH replay");
      *++s1->pack_stack_ptr = pch->replay_records[pch->replay_index].arg0;
      break;
    case TCC_PCH_REPLAY_PACK_POP:
      if (s1->pack_stack_ptr <= s1->pack_stack)
        tcc_error("out of pack stack during PCH replay");
      s1->pack_stack_ptr--;
      break;
    default:
      tcc_error("unsupported PCH replay record");
    }
    ++pch->replay_index;
    return 1;
  }
  return 0;
}

static int tcc_pch_begin_include(TCCState *s1, const char *filename, int include_next_index)
{
  if (!s1->pch || s1->pch->consumed ||
      (PATHCMP(filename, s1->pch->root_filename) && normalized_PATHCMP(filename, s1->pch->root_filename)))
    return 0;
  if (macro_ptr || unget_buf.len)
  {
    tcc_warning("ignoring PCH '%s': include is not at a clean preprocessor boundary", s1->pch->filename);
    tcc_pch_free(s1);
    return 0;
  }
  if (!tcc_pch_apply(s1))
  {
    tcc_pch_free(s1);
    return 0;
  }
  if (s1->gen_deps)
    dynarray_add(&s1->target_deps, &s1->nb_target_deps, tcc_strdup(filename));
  s1->pch->consumed = 1;
  tcc_pch_start_replay(s1, filename, include_next_index);
  return 1;
}

static int tcc_pch_write_file(TCCState *s1, const char *root_filename, int filetype)
{
  static const uint32_t section_kinds[] = {
      TCC_PCH_SEC_STRINGS,        TCC_PCH_SEC_IDENTS,      TCC_PCH_SEC_TOKEN_BLOBS,   TCC_PCH_SEC_MACROS,
      TCC_PCH_SEC_MACRO_ARGS,     TCC_PCH_SEC_REPLAY_RECORDS, TCC_PCH_SEC_CACHED_INCLUDES,
      TCC_PCH_SEC_DEPENDENCIES,   TCC_PCH_SEC_PRAGMA_LIBS, TCC_PCH_SEC_MANIFEST,
  };
  CString sections[sizeof(section_kinds) / sizeof(section_kinds[0])];
  TCCPCHFileHeader hdr;
  TCCPCHSectionHeader dirs[sizeof(section_kinds) / sizeof(section_kinds[0])];
  TCCPCHManifestRecord manifest;
  uint64_t dependency_hash = tcc_pch_hash_init();
  FILE *fp = NULL;
  uint32_t root_filename_off;
  int ret = -1;
  int i;

  if (!s1->outfile)
  {
    tcc_error_noabort("missing output filename for -generate-pch");
    return -1;
  }

  for (i = 0; i < (int)(sizeof(sections) / sizeof(sections[0])); ++i)
    cstr_new(&sections[i]);

  {
    char *resolved_root = realpath(root_filename, NULL);
    const char *stored_root = resolved_root ? resolved_root : root_filename;
    stored_root = tcc_pch_strip_sysroot(stored_root);
    root_filename_off = tcc_pch_add_string(&sections[0], stored_root);
    libc_free(resolved_root);
  }
  manifest.root_filename_off = root_filename_off;
  manifest.generator_version_off = tcc_pch_add_string(&sections[0], TCC_VERSION);

  for (i = s1->pch_ident_start; i < tok_ident; ++i)
  {
    TCCPCHIdentRecord rec;
    TokenSym *ts = table_ident[i - TOK_IDENT];

    rec.token = i;
    rec.string_off = tcc_pch_add_string(&sections[0], ts->str);
    rec.len = ts->len;
    tcc_pch_append_bytes(&sections[1], &rec, sizeof(rec));
  }

  tcc_pch_append_bytes(&sections[2], tok_str_buf(&s1->pch_token_blob), s1->pch_token_blob.len * sizeof(int));

  for (i = TOK_IDENT; i < tok_ident; ++i)
  {
    Sym *macro = define_find(i);
    TCCPCHMacroRecord rec;
    Sym *arg;
    int arg_count = 0;
    int words;

    if (!macro || !macro->d)
      continue;

    for (arg = macro->next; arg; arg = arg->next)
    {
      TCCPCHMacroArgRecord arg_rec;

      arg_rec.token = arg->v & ~SYM_FIELD;
      arg_rec.is_vaargs = arg->r;
      tcc_pch_append_bytes(&sections[4], &arg_rec, sizeof(arg_rec));
      ++arg_count;
    }

    words = tcc_pch_tokstream_words(macro->d);
    rec.name_token = i;
    rec.macro_type = macro->type.t & (MACRO_FUNC | MACRO_JOIN);
    rec.arg_start = sections[4].size / sizeof(TCCPCHMacroArgRecord) - arg_count;
    rec.arg_count = arg_count;
    rec.tok_blob_off_words = sections[2].size / sizeof(int);
    rec.tok_blob_len_words = words;
    tcc_pch_append_bytes(&sections[2], macro->d, words * sizeof(int));
    tcc_pch_append_bytes(&sections[3], &rec, sizeof(rec));
  }

  if (s1->nb_pch_replay_records)
    tcc_pch_append_bytes(&sections[5], s1->pch_replay_records, s1->nb_pch_replay_records * sizeof(*s1->pch_replay_records));

  for (i = 0; i < s1->nb_cached_includes; ++i)
  {
    TCCPCHCachedIncludeRecord rec;
    CachedInclude *inc = s1->cached_includes[i];

    rec.filename_off = tcc_pch_add_string(&sections[0], tcc_pch_strip_sysroot(inc->filename));
    rec.ifndef_macro = inc->ifndef_macro ? inc->ifndef_macro : 0xffffffffu;
    rec.once = inc->once;
    tcc_pch_append_bytes(&sections[6], &rec, sizeof(rec));
  }

  for (i = 0; i < s1->nb_target_deps; ++i)
  {
    struct stat st;
    TCCPCHDependencyRecord rec;
    int dup = 0;
    int j;

    for (j = 0; j < i; ++j)
      if (!strcmp(s1->target_deps[i], s1->target_deps[j]))
      {
        dup = 1;
        break;
      }
    if (dup)
      continue;
    if (stat(s1->target_deps[i], &st) < 0)
    {
      tcc_error_noabort("could not stat '%s' for PCH generation", s1->target_deps[i]);
      goto cleanup;
    }
    {
      char *resolved_dep = realpath(s1->target_deps[i], NULL);
      const char *dep_name = resolved_dep ? resolved_dep : s1->target_deps[i];
      char norm_buf[1024];
      char *resolved_lib = s1->tcc_lib_path ? realpath(s1->tcc_lib_path, NULL) : NULL;
      size_t resolved_lib_len = resolved_lib ? strlen(resolved_lib) : 0;
      dep_name = tcc_pch_normalize_for_hash(dep_name, resolved_lib, resolved_lib_len,
                                             norm_buf, sizeof(norm_buf));
      rec.filename_off = tcc_pch_add_string(&sections[0], dep_name);
      rec.reserved = 0;
      rec.size = st.st_size;
      rec.mtime_sec = 0;
      rec.mtime_nsec = 0;
      dependency_hash = tcc_pch_hash_cstr(dependency_hash, dep_name);
      libc_free(resolved_lib);
      libc_free(resolved_dep);
    }
    dependency_hash = tcc_pch_hash_bytes(dependency_hash, &rec.size, sizeof(rec.size));
    dependency_hash = tcc_pch_hash_bytes(dependency_hash, &rec.mtime_sec, sizeof(rec.mtime_sec));
    dependency_hash = tcc_pch_hash_bytes(dependency_hash, &rec.mtime_nsec, sizeof(rec.mtime_nsec));
    tcc_pch_append_bytes(&sections[7], &rec, sizeof(rec));
  }

  for (i = 0; i < s1->nb_pragma_libs; ++i)
  {
    TCCPCHPragmaLibRecord rec;

    rec.string_off = tcc_pch_add_string(&sections[0], s1->pragma_libs[i]);
    tcc_pch_append_bytes(&sections[8], &rec, sizeof(rec));
  }

  tcc_pch_append_bytes(&sections[9], &manifest, sizeof(manifest));

  memset(&hdr, 0, sizeof(hdr));
  hdr.magic = TCC_PCH_MAGIC;
  hdr.version = TCC_PCH_VERSION;
  hdr.header_size = sizeof(hdr);
  hdr.section_count = sizeof(section_kinds) / sizeof(section_kinds[0]);
  hdr.endianness = tcc_pch_host_endianness();
  hdr.sizeof_int = sizeof(int);
  hdr.long_size = LONG_SIZE;
  hdr.ldouble_size = LDOUBLE_SIZE;
  hdr.ptr_size = PTR_SIZE;
  hdr.tok_ident_value = TOK_IDENT;
  hdr.builtin_ident_count = s1->pch_ident_start - TOK_IDENT;
  hdr.keywords_hash = tcc_pch_hash_bytes(tcc_pch_hash_init(), tcc_keywords, sizeof(tcc_keywords));
  hdr.filetype = filetype & AFF_TYPE_MASK;
#if defined(TCC_TARGET_ARM) || defined(TCC_TARGET_ARM_THUMB)
  hdr.float_abi = s1->float_abi;
  hdr.fpu_type = s1->fpu_type;
#endif
  hdr.predefines_hash = tcc_pch_hash_predefines(s1, filetype);
  hdr.include_path_hash = tcc_pch_hash_include_paths(s1);
  hdr.dependency_hash = dependency_hash;
  hdr.ident_count = sections[1].size / sizeof(TCCPCHIdentRecord);
  hdr.macro_count = sections[3].size / sizeof(TCCPCHMacroRecord);
  hdr.macro_arg_count = sections[4].size / sizeof(TCCPCHMacroArgRecord);
  hdr.replay_count = sections[5].size / sizeof(TCCPCHReplayRecord);
  hdr.include_count = sections[6].size / sizeof(TCCPCHCachedIncludeRecord);
  hdr.pragma_lib_count = sections[8].size / sizeof(TCCPCHPragmaLibRecord);
  hdr.dependency_count = sections[7].size / sizeof(TCCPCHDependencyRecord);
  hdr.counter_delta = pp_counter;

  {
    uint32_t off = sizeof(hdr) + sizeof(dirs);

    for (i = 0; i < (int)(sizeof(dirs) / sizeof(dirs[0])); ++i)
    {
      dirs[i].kind = section_kinds[i];
      dirs[i].offset = off;
      dirs[i].size = sections[i].size;
      off += sections[i].size;
    }
  }

  fp = fopen(s1->outfile, "wb");
  if (!fp)
  {
    tcc_error_noabort("could not write '%s'", s1->outfile);
    goto cleanup;
  }
  if (1 != fwrite(&hdr, sizeof(hdr), 1, fp) || 1 != fwrite(dirs, sizeof(dirs), 1, fp))
  {
    tcc_error_noabort("could not write '%s'", s1->outfile);
    goto cleanup;
  }
  for (i = 0; i < (int)(sizeof(sections) / sizeof(sections[0])); ++i)
  {
    if (sections[i].size && fwrite(sections[i].data, sections[i].size, 1, fp) != 1)
    {
      tcc_error_noabort("could not write '%s'", s1->outfile);
      goto cleanup;
    }
  }
  if (fclose(fp) != 0)
  {
    fp = NULL;
    tcc_error_noabort("could not finalize '%s'", s1->outfile);
    goto cleanup;
  }
  fp = NULL;
  ret = 0;

cleanup:
  if (fp)
    fclose(fp);
  if (ret < 0)
    remove(s1->outfile);
  for (i = 0; i < (int)(sizeof(sections) / sizeof(sections[0])); ++i)
    cstr_free(&sections[i]);
  return ret;
}

ST_FUNC int tcc_pch_generate(TCCState *s1, const char *filename, int filetype)
{
  BufferedFile *root_file;
  const char *root_filename;

  (void)filename;
  if (filetype & (AFF_TYPE_ASM | AFF_TYPE_ASMPP))
  {
    tcc_error_noabort("PCH generation only supports C headers");
    return -1;
  }

  root_file = s1->include_stack_ptr != s1->include_stack ? s1->include_stack[0] : file;
  root_filename = root_file->true_filename;
  dynarray_add(&s1->target_deps, &s1->nb_target_deps, tcc_strdup(root_filename));

  parse_flags = PARSE_FLAG_PREPROCESS | PARSE_FLAG_TOK_NUM | PARSE_FLAG_TOK_STR;
  tok_str_new(&s1->pch_token_blob);
  s1->pch_replay_token_start = 0;

  for (;;)
  {
    next();
    if (tok == TOK_EOF)
      break;
    if (!tcc_pch_in_root_closure(file))
      continue;
    tok_str_add_tok(&s1->pch_token_blob);
  }

  tok_str_add(&s1->pch_token_blob, TOK_EOF);
  tcc_pch_flush_replay_tokens(s1);

  if (s1->pch_macro_depth != 0)
  {
    tcc_error_noabort("PCH generation requires balanced #pragma push_macro/pop_macro");
    return -1;
  }
  return tcc_pch_write_file(s1, root_filename, filetype);
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
