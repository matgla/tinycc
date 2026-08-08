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

/* strlit_pool.c -- Read-only string-literal dedupe pool and external symbol pushes.
 * Split out of tccgen.c; see docs/plan_tccgen_split.md. */

#include "gen_priv.h"

/* String-literal pool: dedupe identical read-only string literals so the same
 * bytes are emitted once in rodata (identical literals may share storage per
 * C11 6.4.5p7).  Content-keyed, per-translation-unit; see str_lit_pool_merge. */
typedef struct StrLitEntry
{
  unsigned int hash;
  addr_t off;
  addr_t len;
} StrLitEntry;
static StrLitEntry *str_lit_entries;
static int str_lit_nb, str_lit_cap;

static unsigned int str_lit_hash(const unsigned char *p, addr_t n)
{
  unsigned int h = 2166136261u;
  for (addr_t k = 0; k < n; k++)
    h = (h ^ p[k]) * 16777619u;
  return h;
}

void str_lit_pool_reset(void)
{
  str_lit_nb = 0;
}

void str_lit_pool_free(void)
{
  tcc_free(str_lit_entries);
  str_lit_entries = NULL;
  str_lit_nb = str_lit_cap = 0;
}

static int str_lit_pool_find(unsigned int h, const unsigned char *bytes, addr_t len, addr_t *out)
{
  for (int k = 0; k < str_lit_nb; k++)
    if (str_lit_entries[k].hash == h && str_lit_entries[k].len == len &&
        memcmp(rodata_section->data + str_lit_entries[k].off, bytes, len) == 0)
    {
      *out = str_lit_entries[k].off;
      return 1;
    }
  return 0;
}

static void str_lit_pool_add(unsigned int h, addr_t off, addr_t len)
{
  if (str_lit_nb >= str_lit_cap)
  {
    int nc = str_lit_cap ? str_lit_cap * 2 : 64;
    str_lit_entries = tcc_realloc(str_lit_entries, nc * sizeof(*str_lit_entries));
    str_lit_cap = nc;
  }
  str_lit_entries[str_lit_nb].hash = h;
  str_lit_entries[str_lit_nb].off = off;
  str_lit_entries[str_lit_nb].len = len;
  str_lit_nb++;
}

/* Called right after a string-literal expression is materialized in rodata
 * (vtop is the anonymous rodata reference, its bytes freshly appended at the
 * tail).  If an identical literal was already emitted, roll back the duplicate
 * bytes and repoint vtop at the existing copy; otherwise record this one. */
void str_lit_pool_merge(addr_t pre_off)
{
#ifdef CONFIG_TCC_BCHECK
  if (tcc_state->do_bounds_check)
    return; /* bound-check padding breaks the tail-append invariant */
#endif
  if (NODATA_WANTED)
    return;
  if (!vtop->sym || vtop->c.i != 0)
    return;
  addr_t cur = rodata_section->data_offset;
  addr_t off = (pre_off + 3) & ~(addr_t)3; /* string literals use 4-byte align */
  if (off >= cur)
    return;
  addr_t len = cur - off;
  unsigned char *bytes = rodata_section->data + off;
  unsigned int h = str_lit_hash(bytes, len);
  addr_t found;
  if (str_lit_pool_find(h, bytes, len, &found))
  {
    rodata_section->data_offset = pre_off;
    /* Re-zero the reclaimed window; string-literal init relies on section-grow zero-fill for the terminator. */
    memset(rodata_section->data + pre_off, 0, cur - pre_off);
    /* Repoint this literal's anon symbol at the shared copy instead of pushing a fresh ref, so no orphan symbol is left pointing into the reclaimed window. */
    put_extern_sym2(vtop->sym, rodata_section->sh_num, found, len, 1);
  }
  else
  {
    str_lit_pool_add(h, off, len);
  }
}

/* define a new external reference to a symbol 'v' of type 'u' */
ST_FUNC Sym *external_global_sym(int v, CType *type)
{
  Sym *s;

  s = sym_find(v);
  if (!s)
  {
    /* push forward reference */
    s = global_identifier_push(v, type->t | VT_EXTERN, 0);
    s->type.ref = type->ref;
  }
  else if (IS_ASM_SYM(s))
  {
    s->type.t = type->t | (s->type.t & VT_EXTERN);
    s->type.ref = type->ref;
    update_storage(s);
  }
  return s;
}

/* create an external reference with no specific type similar to asm labels.
   This avoids type conflicts if the symbol is used from C too */
ST_FUNC Sym *external_helper_sym(int v)
{
  CType ct = {VT_ASM_FUNC, NULL};
  return external_global_sym(v, &ct);
}

/* push a reference to an helper function (such as memmove) */
ST_FUNC void vpush_helper_func(int v)
{
  vpushsym(&func_old_type, external_helper_sym(v));
}

/* push a reference to a helper/library function with a specific return type */
ST_FUNC void vpush_typed_helper_func(int v, CType *type)
{
  vpushsym(type, external_global_sym(v, type));
}
