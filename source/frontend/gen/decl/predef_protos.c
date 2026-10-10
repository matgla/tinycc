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


/* Inline strings, no pointers: a pointer table needs load-time relocation,
   which on YasOS put it in every compiler process's RAM. */
#define PP_PREFIX "__builtin_"
#define PP_PREFIX_LEN 10
typedef struct TCCPredefProto
{
  char name[17];   /* after PP_PREFIX, which every name starts with */
  char rename[17]; /* "" = no asm label */
  char sig[6];
} TCCPredefProto;

static const TCCPredefProto tcc_predef_protos_table[] = {
    {"memcpy", "memcpy", "ppPz"},
    {"memmove", "memmove", "ppPz"},
    {"memset", "memset", "ppiz"},
    {"memcmp", "memcmp", "iPPz"},
    {"strlen", "strlen", "zS"},
    {"strcpy", "strcpy", "ssS"},
    {"strncpy", "strncpy", "ssSz"},
    {"strcmp", "strcmp", "iSS"},
    {"strncmp", "strncmp", "iSSz"},
    {"strcat", "strcat", "ssS"},
    {"strncat", "strncat", "ssSz"},
    {"strchr", "strchr", "sSi"},
    {"strrchr", "strrchr", "sSi"},
    {"strdup", "strdup", "sS"},
    {"malloc", "malloc", "pz"},
    {"realloc", "realloc", "ppz"},
    {"calloc", "calloc", "pzz"},
    {"memalign", "memalign", "pzz"},
    {"free", "free", "vp"},
    {"alloca", "alloca", "pz"},
    {"abort", "abort", "v"},
    {"exit", "exit", "vi"},
    {"printf", "printf", "iS."},
    {"puts", "puts", "iS"},
    {"putchar", "putchar", "ii"},
    {"fputc", "fputc", "iip"},
    {"fwrite", "fwrite", "zPzzp"},
    {"sprintf", "sprintf", "isS."},
    {"snprintf", "snprintf", "iszS."},
    {"index", "strchr", "sSi"},
    {"rindex", "__tcc_strrchr", "sSi"},
    {"bcopy", "bcopy", "vPpz"},
    {"bzero", "bzero", "vpz"},
    {"printf_unlocked", "printf_unlocked", "iS."},
    {"fprintf_unlocked", "fprintf_unlocked", "ipS."},
    {"fputs_unlocked", "fputs_unlocked", "iSp"},
    {"uabs", "uabs", "ui"},
    {"ulabs", "ulabs", "Ll"},
    {"ullabs", "ullabs", "Xx"},
    {"umaxabs", "umaxabs", "Xx"},
    {"ffs", "", "ii"},
    {"ffsl", "", "il"},
    {"ffsll", "", "ix"},
    {"clz", "", "iu"},
    {"clzl", "", "iL"},
    {"clzll", "", "iX"},
    {"ctz", "", "iu"},
    {"ctzl", "", "iL"},
    {"ctzll", "", "iX"},
    {"clrsb", "", "ii"},
    {"clrsbl", "", "il"},
    {"clrsbll", "", "ix"},
    {"popcount", "", "iu"},
    {"popcountl", "", "iL"},
    {"popcountll", "", "iX"},
    {"parity", "", "iu"},
    {"parityl", "", "iL"},
    {"parityll", "", "iX"},
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

#define NB_PREDEF_PROTOS ((int)(sizeof(tcc_predef_protos_table) / sizeof(tcc_predef_protos_table[0])))

/* ------------------------------------------------------------------------- */
/* Lazy materialisation.

   Built eagerly, the 58 prototypes cost ~200 Syms (params, pointer types,
   function types, the extern) and ~60 TokenSyms on every compile, and most
   translation units name none of them.  So tccgen_compile only arms the
   table; a prototype is built the first time its name is interned (lexed,
   or referenced by fixed id through tok_ensure), and lazily even then: the
   intern hook just queues it, and the next identifier lookup or push
   (sym_find, sym_push, global_identifier_push) builds everything queued.

   Why that is indistinguishable from the eager build:
   - Nothing can observe the identifier before it is interned, and a chain
     is only ever started by sym_push / global_identifier_push and looked up
     by sym_find (the direct readers -- sym_copy, nested typedefs, the
     inliner's shadow check -- see names those already pushed), so the
     prototype is at the bottom of its chain before user code sees the
     name: a redeclaration merges with it and a local shadows it as before.
     sym_find on a builtin slot never interned builds it too.
   - It is built in the start-of-TU context: no local stack, scope 0 and no
     function IR (sym_push would otherwise hand the parameters vregs of the
     function being compiled, and file them on its local stack).
   - Its Syms are spliced into global_stack where the eager build put them,
     above what was there at tccgen_compile start and in table order.  That
     keeps declaration-order scans (alias targets by asm label) and the
     keep-pops of parameter lists that may be open on global_stack when the
     build runs from inside sym_push intact.
   - ELF symbols were, and are, only created when the prototype is
     referenced.
   Interning a name runs no tcc code beyond the queueing, so the lexer state
   a lazy predefine once tripped over is never touched.  What does change is
   the numbering of user identifiers (the eager build interned ~35 names and
   asm labels ahead of them); nothing emitted depends on token ids. */

ST_DATA unsigned char tcc_predef_protos_armed;
ST_DATA unsigned char tcc_predef_protos_queued;
static Sym *pp_anchor;           /* global_stack top when armed */
static Sym **pp_top;             /* built prototype -> its topmost Sym; allocated on first build */
static uint32_t pp_queue[2];     /* table indices interned but not built yet */
typedef char pp_queue_fits[NB_PREDEF_PROTOS <= 64 ? 1 : -1];
static unsigned char pp_flushing;

static int tccgen_predef_proto_index(const char *str, int len)
{
  int i;
  if (len <= PP_PREFIX_LEN || memcmp(str, PP_PREFIX, PP_PREFIX_LEN))
    return -1;
  str += PP_PREFIX_LEN;
  len -= PP_PREFIX_LEN;
  if (len >= (int)sizeof(tcc_predef_protos_table[0].name))
    return -1;
  for (i = 0; i < NB_PREDEF_PROTOS; i++)
  {
    const char *n = tcc_predef_protos_table[i].name;
    if (!strncmp(n, str, len) && !n[len])
      return i;
  }
  return -1;
}

/* The full name of entry i into buf (PP_PREFIX + name); returns its length. */
static int tccgen_predef_proto_name(char *buf, int i)
{
  int len = (int)strlen(tcc_predef_protos_table[i].name);
  memcpy(buf, PP_PREFIX, PP_PREFIX_LEN);
  memcpy(buf + PP_PREFIX_LEN, tcc_predef_protos_table[i].name, len + 1);
  return PP_PREFIX_LEN + len;
}

static void tccgen_predef_proto_queue(int i)
{
  if (pp_top && pp_top[i])
    return;
  pp_queue[i >> 5] |= 1u << (i & 31);
  tcc_predef_protos_queued = 1;
}

ST_FUNC void tccgen_predef_protos_arm(void)
{
  int i;
  pp_anchor = global_stack;
  pp_top = NULL;
  pp_queue[0] = pp_queue[1] = 0;
  pp_flushing = 0;
  tcc_predef_protos_queued = 0;
  tcc_predef_protos_armed = 1;
  /* Names interned before arming (a -D on the command line) are due now. */
  for (i = 0; i < NB_PREDEF_PROTOS; i++)
  {
    char n[PP_PREFIX_LEN + sizeof(tcc_predef_protos_table[0].name)];
    int len = tccgen_predef_proto_name(n, i);
    if (tok_find(n, len))
      tccgen_predef_proto_queue(i);
  }
}

ST_FUNC void tccgen_predef_protos_disarm(void)
{
  tcc_predef_protos_armed = 0;
  tcc_predef_protos_queued = 0;
  pp_queue[0] = pp_queue[1] = 0;
  pp_flushing = 0;
  pp_anchor = NULL;
  tcc_free(pp_top);
  pp_top = NULL;
}

ST_FUNC void tccgen_predef_proto_interned(TokenSym *ts)
{
  int i = tccgen_predef_proto_index(ts->str, ts->len);
  if (i >= 0)
    tccgen_predef_proto_queue(i);
}

/* sym_find on a builtin slot that was never interned: the eager build had
   interned it, so a prototype name must be built here to be found. */
ST_FUNC Sym *tccgen_predef_proto_find_unseen(int v)
{
  const char *n = get_tok_str(v, NULL);
  if (!n || strncmp(n, "__builtin_", 10) || tccgen_predef_proto_index(n, strlen(n)) < 0)
    return NULL;
  tok_ensure(v); /* the intern hook queues it */
  tccgen_predef_protos_flush();
  return sym_find(v);
}

static void tccgen_predef_proto_build(const TCCPredefProto *pp)
{
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

  if (pp->rename[0])
    ad.asm_label = tok_alloc(pp->rename, strlen(pp->rename))->tok;
  merge_funcattr(&type.ref->f, &ad.f);
  type.t |= VT_EXTERN;
  {
    char name[PP_PREFIX_LEN + sizeof(pp->name)];
    int len = tccgen_predef_proto_name(name, (int)(pp - tcc_predef_protos_table));
    external_sym(tok_alloc(name, len)->tok, &type, 0, &ad);
  }
}

static void tccgen_predef_proto_materialize(int i)
{
  Sym *saved_local_stack = local_stack;
  int saved_local_scope = local_scope;
  TCCIRState *saved_ir = tcc_state->ir;
  Sym *old_top = global_stack, *top, *bot, *at, *x;
  int j;

  local_stack = NULL;
  local_scope = 0;
  tcc_state->ir = NULL;
  tccgen_predef_proto_build(&tcc_predef_protos_table[i]);
  local_stack = saved_local_stack;
  local_scope = saved_local_scope;
  tcc_state->ir = saved_ir;

  if (!pp_top)
    pp_top = tcc_mallocz(NB_PREDEF_PROTOS * sizeof(Sym *));
  top = global_stack;
  pp_top[i] = top;

  /* Splice [top .. bot] down to sit on the nearest earlier-built prototype,
     or on the anchor. */
  at = pp_anchor;
  for (j = i - 1; j >= 0; j--)
    if (pp_top[j])
    {
      at = pp_top[j];
      break;
    }
  if (at == old_top)
    return;
  for (bot = top; bot->prev != old_top; bot = bot->prev)
    ;
  for (x = old_top; x->prev != at; x = x->prev)
    ;
  global_stack = old_top;
  x->prev = top;
  bot->prev = at;
}

ST_FUNC void tccgen_predef_protos_flush(void)
{
  int i;
  if (pp_flushing)
    return; /* building one: the lookups inside it must not start another */
  pp_flushing = 1;
  while (pp_queue[0] | pp_queue[1])
    for (i = 0; i < NB_PREDEF_PROTOS; i++)
    {
      if (!(pp_queue[i >> 5] & (1u << (i & 31))))
        continue;
      pp_queue[i >> 5] &= ~(1u << (i & 31));
      tccgen_predef_proto_materialize(i);
    }
  tcc_predef_protos_queued = 0;
  pp_flushing = 0;
}
