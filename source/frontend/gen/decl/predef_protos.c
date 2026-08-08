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

/* predef_protos.c -- Programmatic prototypes for predefined builtins.
 * Split out of tccgen.c; see docs/plan_tccgen_split.md. */

#include "gen_priv.h"

/* ------------------------------------------------------------------------- */
/* Programmatic builtin alias prototypes.

   These are the declarations of include/tccdecls.h — `ret __builtin_X(args)
   __asm__("X");` — built directly through the same calls parse would make
   (sym_push param chains, convert_parameter_type, external_sym with an asm
   label), because PARSING them costs ~30 ms of every compile on the RP2350:
   the macro-expander/declaration-parser alternation refetches ~45 KB of code
   per declaration through the 16 KiB XIP cache.  Only the default
   configuration takes this path (see tcc_predefs_base); bounds checking and
   -fleading-underscore change the rename spellings and keep the text.

   Signature codes, first char is the return type:
     v void   i int   u unsigned   z size_t (unsigned int here)
     l long   L unsigned long   x long long   X unsigned long long
     p void*  P const void*  s char*  S const char*   trailing '.' = ellipsis
   Keep the table in sync with tccdecls.h; the byte-identity A/B against the
   text path (TCC_NO_PROGRAMMATIC_DECLS=1) is the check. */


typedef struct TCCPredefProto
{
  const char *name;
  const char *rename; /* NULL = no asm label */
  const char *sig;
} TCCPredefProto;

static const TCCPredefProto tcc_predef_protos_table[] = {
    {"__builtin_memcpy", "memcpy", "ppPz"},
    {"__builtin_memmove", "memmove", "ppPz"},
    {"__builtin_memset", "memset", "ppiz"},
    {"__builtin_memcmp", "memcmp", "iPPz"},
    {"__builtin_strlen", "strlen", "zS"},
    {"__builtin_strcpy", "strcpy", "ssS"},
    {"__builtin_strncpy", "strncpy", "ssSz"},
    {"__builtin_strcmp", "strcmp", "iSS"},
    {"__builtin_strncmp", "strncmp", "iSSz"},
    {"__builtin_strcat", "strcat", "ssS"},
    {"__builtin_strncat", "strncat", "ssSz"},
    {"__builtin_strchr", "strchr", "sSi"},
    {"__builtin_strrchr", "strrchr", "sSi"},
    {"__builtin_strdup", "strdup", "sS"},
    {"__builtin_malloc", "malloc", "pz"},
    {"__builtin_realloc", "realloc", "ppz"},
    {"__builtin_calloc", "calloc", "pzz"},
    {"__builtin_memalign", "memalign", "pzz"},
    {"__builtin_free", "free", "vp"},
    {"__builtin_alloca", "alloca", "pz"},
    {"__builtin_abort", "abort", "v"},
    {"__builtin_exit", "exit", "vi"},
    {"__builtin_printf", "printf", "iS."},
    {"__builtin_puts", "puts", "iS"},
    {"__builtin_putchar", "putchar", "ii"},
    {"__builtin_fputc", "fputc", "iip"},
    {"__builtin_fwrite", "fwrite", "zPzzp"},
    {"__builtin_sprintf", "sprintf", "isS."},
    {"__builtin_snprintf", "snprintf", "iszS."},
    {"__builtin_index", "strchr", "sSi"},
    {"__builtin_rindex", "__tcc_strrchr", "sSi"},
    {"__builtin_bcopy", "bcopy", "vPpz"},
    {"__builtin_bzero", "bzero", "vpz"},
    {"__builtin_printf_unlocked", "printf_unlocked", "iS."},
    {"__builtin_fprintf_unlocked", "fprintf_unlocked", "ipS."},
    {"__builtin_fputs_unlocked", "fputs_unlocked", "iSp"},
    {"__builtin_uabs", "uabs", "ui"},
    {"__builtin_ulabs", "ulabs", "Ll"},
    {"__builtin_ullabs", "ullabs", "Xx"},
    {"__builtin_umaxabs", "umaxabs", "Xx"},
    {"__builtin_ffs", NULL, "ii"},
    {"__builtin_ffsl", NULL, "il"},
    {"__builtin_ffsll", NULL, "ix"},
    {"__builtin_clz", NULL, "iu"},
    {"__builtin_clzl", NULL, "iL"},
    {"__builtin_clzll", NULL, "iX"},
    {"__builtin_ctz", NULL, "iu"},
    {"__builtin_ctzl", NULL, "iL"},
    {"__builtin_ctzll", NULL, "iX"},
    {"__builtin_clrsb", NULL, "ii"},
    {"__builtin_clrsbl", NULL, "il"},
    {"__builtin_clrsbll", NULL, "ix"},
    {"__builtin_popcount", NULL, "iu"},
    {"__builtin_popcountl", NULL, "iL"},
    {"__builtin_popcountll", NULL, "iX"},
    {"__builtin_parity", NULL, "iu"},
    {"__builtin_parityl", NULL, "iL"},
    {"__builtin_parityll", NULL, "iX"},
};

static CType tccgen_predef_sig_type(int c)
{
  CType t;
  t.ref = NULL;
  switch (c)
  {
  default:
  case 'v':
    t.t = VT_VOID;
    break;
  case 'i':
    t.t = VT_INT;
    break;
  case 'u':
  case 'z':
    t.t = VT_INT | VT_UNSIGNED;
    break;
  case 'l':
    t.t = VT_LONG | VT_INT;
    break;
  case 'L':
    t.t = VT_LONG | VT_INT | VT_UNSIGNED;
    break;
  case 'x':
    t.t = VT_LLONG;
    break;
  case 'X':
    t.t = VT_LLONG | VT_UNSIGNED;
    break;
  case 'p':
    t.t = VT_VOID;
    mk_pointer(&t);
    break;
  case 'P':
    t.t = VT_VOID | VT_CONSTANT;
    mk_pointer(&t);
    break;
  case 's':
    t = char_type;
    mk_pointer(&t);
    break;
  case 'S':
    t = char_type;
    t.t |= VT_CONSTANT;
    mk_pointer(&t);
    break;
  }
  return t;
}

ST_FUNC void tccgen_predef_protos(TCCState *s1)
{
  unsigned i;
  for (i = 0; i < sizeof(tcc_predef_protos_table) / sizeof(tcc_predef_protos_table[0]); i++)
  {
    const TCCPredefProto *pp = &tcc_predef_protos_table[i];
    const char *sig = pp->sig;
    CType type, pt;
    AttributeDef ad;
    Sym *first = NULL, **plast = &first, *s;
    int arg_size = 0, align, l = FUNC_NEW;

    memset(&ad, 0, sizeof(ad));
    type = tccgen_predef_sig_type(*sig++);

    ++local_scope;
    for (; *sig && *sig != '.'; sig++)
    {
      pt = tccgen_predef_sig_type(*sig);
      convert_parameter_type(&pt);
      arg_size += (type_size(&pt, &align) + PTR_SIZE - 1) / PTR_SIZE;
      s = sym_push(SYM_FIELD, &pt, VT_LOCAL | VT_LVAL, 0);
      *plast = s;
      plast = &s->next;
    }
    if (*sig == '.')
      l = FUNC_ELLIPSIS;
    if (first)
    {
      sym_pop(local_stack ? &local_stack : &global_stack, first->prev, 1);
      for (s = first; s; s = s->next)
        s->v |= SYM_FIELD;
    }
    --local_scope;

    ad.f.func_args = arg_size;
    ad.f.func_type = l;
    s = sym_push(SYM_FIELD, &type, 0, 0);
    s->a = ad.a;
    s->f = ad.f;
    s->next = first;
    type.t = VT_FUNC;
    type.ref = s;

    if (pp->rename)
      ad.asm_label = tok_alloc(pp->rename, strlen(pp->rename))->tok;
    merge_funcattr(&type.ref->f, &ad.f);
    type.t |= VT_EXTERN;
    external_sym(tok_alloc(pp->name, strlen(pp->name))->tok, &type, 0, &ad);
  }
}
