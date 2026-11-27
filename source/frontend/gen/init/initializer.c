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

/* initializer.c -- Initializer parsing, designators and element emission.
 * Split out of tccgen.c; see docs/plan_tccgen_split.md. */

#include "gen_priv.h"

/* This skips over a stream of tokens containing balanced {} and ()
   pairs, stopping at outer ',' ';' and '}' (or matching '}' if we started
   with a '{').  If STR then allocates and stores the skipped tokens
   in *STR.  This doesn't check if () and {} are nested correctly,
   i.e. "({)}" is accepted.  */
int skip_save_stop_semicolon;

void skip_or_save_block(TokenString **str)
{
  int braces = tok == '{';
  int level = 0;
  /* While recording (str != NULL), redirect #pragma pack directives consumed by
     next() into the saved stream as deferred TOK_PACK_REPLAY actions rather than
     letting them mutate pack_stack now — the body's structs are laid out later
     during replay, so the pack state must travel with the tokens. */
  TokenString *saved_capture = pp_pragma_capture;
  if (str)
    *str = tok_str_alloc();
  pp_pragma_capture = str ? *str : saved_capture;

  int recorded_len = 0, sys = 0;

  while (1)
  {
    int t = tok;
    if (level == 0 && (t == ',' || t == ';' || t == '}' || t == ')' || t == ']'))
      break;
    if (t == ';' && level == 1 && skip_save_stop_semicolon)
    {
      /* a static's initializer missing a '}' (a ';' right in its braces,
         not a struct body's in them): kept with the ';' its parse says the
         error at, which the declaration's parse then reads */
      if (str)
      {
        tok_str_add_tok(*str);
        recorded_len = (*str)->len;
      }
      break;
    }
    if (t == TOK_EOF)
    {
      if (str && braces && tcc_state->save_body_jmp)
      {
        /* a function body missing its '}': the parse of what was saved
           says so, as at -O0 (decl.c save_body_cut) */
        pp_pragma_capture = saved_capture;
        longjmp(*tcc_state->save_body_jmp, 2);
      }
      if (str || level > 0)
      {
        pp_pragma_capture = saved_capture;
        if (str)
        {
          /* the error leaves the caller: nothing else holds the tokens */
          tok_str_free(*str);
          *str = NULL;
        }
        tcc_error("unexpected end of file");
      }
      else
        break;
    }
    if (str)
    {
      /* where a system header's macro put its tokens (tccpp.c) */
      if (sys || tok_macro_stack())
        sys = tok_str_sys_save(*str, sys);
      tok_str_add_tok(*str);
      recorded_len = (*str)->len;
    }
    next();
    /* A braced block counts its braces only: a stray ')' or ']' in a body
       is the parser's error to report (and to recover from, as at -O0,
       where the body is not saved), not the end of the body. */
    if (braces ? t == '{' : t == '{' || t == '(' || t == '[')
    {
      level++;
    }
    else if (braces ? t == '}' : t == '}' || t == ')' || t == ']')
    {
      level--;
      if (level == 0 && braces && t == '}')
        break;
    }
  }
  pp_pragma_capture = saved_capture;
  if (str)
  {
    /* The lookahead read after the last recorded token (the token after a
       body's closing brace, or the terminator) ran any #pragma pack in
       between with capture still on.  Those directives are outside the saved
       tokens: apply them now, in file order, and drop them from the stream.
       Left in, `f(){...} #pragma pack(1) struct S {...};` laid S out unpacked
       and replayed pack(1) whenever f's body was. */
    if ((*str)->len > recorded_len)
    {
      const int *buf = tok_str_buf(*str);
      pp_apply_pack_replays(tcc_state, buf + recorded_len, buf + (*str)->len);
      (*str)->len = recorded_len;
    }
    tok_str_add(*str, TOK_EOF);
    /* A saved block is complete: give back the doubling slack.  At -O2 every
       function body of the TU waits here as tokens until the end of the TU,
       a quarter of it unused capacity on average. */
    if ((*str)->allocated_len > (*str)->len)
    {
      (*str)->data.str = tcc_realloc((*str)->data.str, (*str)->len * sizeof(int));
      (*str)->allocated_len = (*str)->len;
    }
  }
}

#define EXPR_CONST 1
#define EXPR_ANY 2

int init_self_lo, init_self_hi, init_self_tok, init_self_vreg = -1, init_self_on, init_self_depth;

/* the enclosing initializers' states (a statement expression or a compound
   literal inside one); past the last, nothing is checked */
static struct
{
  int lo, hi, tok, vreg, on;
} init_self_saved[16];

void init_self_push(int addr, int size, int tok, int vreg)
{
  if (init_self_depth < (int)(sizeof init_self_saved / sizeof *init_self_saved))
  {
    init_self_saved[init_self_depth].lo = init_self_lo;
    init_self_saved[init_self_depth].hi = init_self_hi;
    init_self_saved[init_self_depth].tok = init_self_tok;
    init_self_saved[init_self_depth].vreg = init_self_vreg;
    init_self_saved[init_self_depth].on = init_self_on;
  }
  else
    size = 0;
  init_self_depth++;
  init_self_on = 0;
  init_self_lo = addr, init_self_hi = addr + size, init_self_tok = tok, init_self_vreg = vreg;
}

void init_self_pop(void)
{
  if (--init_self_depth < (int)(sizeof init_self_saved / sizeof *init_self_saved))
  {
    init_self_lo = init_self_saved[init_self_depth].lo;
    init_self_hi = init_self_saved[init_self_depth].hi;
    init_self_tok = init_self_saved[init_self_depth].tok;
    init_self_vreg = init_self_saved[init_self_depth].vreg;
    init_self_on = init_self_saved[init_self_depth].on;
  }
  else
    init_self_on = 0, init_self_lo = init_self_hi = 0;
}

/* The initializer of the local in [init_self_lo, init_self_hi) reads it:
   `int x = x + 1;`, `u = { .a = v, .b = -u.b }` (that member is not set yet,
   and the order the elements are evaluated in is unspecified).  Once per
   declaration.  yasc enforces initialization: there, -Wlifetime. */
static void init_self_read(void)
{
  const char *name = get_tok_str(init_self_tok, NULL);
  init_self_on = 0;
  init_self_hi = init_self_lo; /* once */
  if (tcc_state->yasc && tcc_ir_lt_active())
    tcc_warning_c(warn_lifetime)("'%s' is used uninitialized: read in its own initializer", name);
  else
    tcc_warning_c(warn_uninitialized)("'%s' is used uninitialized: read in its own initializer", name);
}

/* SV is the variable being initialized, or a part of it, as an lvalue (not
   an array: that is its address) */
static int init_self_in(const SValue *sv)
{
  /* a scalar register candidate has no slot of its own (its offset is the
     frame's top, shared): it is its vreg */
  if (init_self_vreg >= 0)
    return sv->vr == init_self_vreg;
  /* another variable's vreg is never this one, whatever its offset */
  if (sv->vr >= 0 && TCCIR_DECODE_VREG_TYPE(sv->vr) == TCCIR_VREG_TYPE_VAR)
    return 0;
  const long long o = (long long)sv->c.i; /* c.i is unsigned: frame offsets are negative */
  return o >= init_self_lo && o < init_self_hi;
}

static int init_self_lvalue(const SValue *sv)
{
  return init_self_on && !nocode_wanted && (sv->r & (VT_VALMASK | VT_LVAL)) == (VT_LOCAL | VT_LVAL) &&
         !(sv->type.t & (VT_ARRAY | VT_VLA)) && init_self_in(sv);
}

/* Vregs holding an address inside the variable: a member access computes
   &v + offset into a temporary and loads through it.  A few at a time,
   cleared per initializer element (parse_init_elem). */
static int init_self_ptr[8], init_self_nptr;

static int init_self_is_ptr(const SValue *sv)
{
  int i;
  if ((sv->r & VT_VALMASK) >= VT_CONST || sv->vr < 0)
    return 0;
  for (i = 0; i < init_self_nptr; i++)
    if (init_self_ptr[i] == sv->vr)
      return 1;
  return 0;
}

/* SV is the variable's address, or a vreg holding one */
static int init_self_addr(const SValue *sv)
{
  if (sv->r & VT_LVAL)
    return 0;
  if ((sv->r & VT_VALMASK) == VT_LOCAL)
    return init_self_in(sv);
  return init_self_is_ptr(sv);
}

/* An IR instruction emitted while the initializer expression is parsed: an
   operand read from the variable (directly, or through an address of it
   computed into a vreg) is reported; such an address is followed. */
void init_self_ir(int op, const SValue *src1, const SValue *src2, const SValue *dest)
{
  if (nocode_wanted & ~0x20000000)
    return;
  if ((src1 && (init_self_lvalue(src1) || ((src1->r & VT_LVAL) && init_self_is_ptr(src1)))) ||
      (src2 && (init_self_lvalue(src2) || ((src2->r & VT_LVAL) && init_self_is_ptr(src2)))))
  {
    init_self_read();
    return;
  }
  if (dest && dest->vr >= 0 && !(dest->r & VT_LVAL) &&
      (op == TCCIR_OP_LEA || op == TCCIR_OP_ASSIGN || op == TCCIR_OP_ADD || op == TCCIR_OP_SUB) &&
      ((src1 && init_self_addr(src1)) || (op == TCCIR_OP_ADD && src2 && init_self_addr(src2))))
  {
    if (init_self_nptr < (int)(sizeof init_self_ptr / sizeof *init_self_ptr))
      init_self_ptr[init_self_nptr++] = dest->vr;
  }
}

/* the value of an initializer element is the variable itself */
static void init_self_value(void)
{
  if (init_self_lvalue(vtop) || ((vtop->r & VT_LVAL) && !(vtop->type.t & (VT_ARRAY | VT_VLA)) && init_self_is_ptr(vtop)))
    init_self_read();
}

void parse_init_elem(int expr_type)
{
  int saved_global_expr;
  /* an IR operand read from the variable is reported while its own
     initializer expression is parsed (init_self_ir) -- not while the value
     is stored (a range designator's copy, a bit-field's read-modify-write) */
  init_self_on = init_self_hi > init_self_lo;
  init_self_nptr = 0;
  switch (expr_type)
  {
  case EXPR_CONST:
    /* compound literals must be allocated globally in this case */
    saved_global_expr = global_expr;
    global_expr = 1;
    expr_const1();
    global_expr = saved_global_expr;
    /* NOTE: symbols are accepted, as well as lvalue for anon symbols
       (compound literals).  */
    if (((vtop->r & (VT_VALMASK | VT_LVAL)) != VT_CONST &&
         ((vtop->r & (VT_SYM | VT_LVAL)) != (VT_SYM | VT_LVAL) || vtop->sym->v < SYM_FIRST_ANOM))
#ifdef TCC_TARGET_PE
        || ((vtop->r & VT_SYM) && vtop->sym->a.dllimport)
#endif
    )
      tcc_error("initializer element is not constant");
    break;
  case EXPR_ANY:
    if (local_stack && !NOEVAL_WANTED)
    {
      /* a pointer's initializer: what it points into (builtin/objsize.c) */
      ObjRec os_saved;
      objsize_rhs_begin(&os_saved);
      expr_eq();
      objsize_rhs_end(&os_saved);
    }
    else
      expr_eq();
    break;
  }
  /* `int x = x;`, `struct S s = s;`: the value itself is the variable */
  if (init_self_on)
    init_self_value();
  /* off again: an initializer met inside this one's expression (a statement
     expression's, a compound literal) brings it back (init_self_pop) */
  init_self_on = 0;
}

#if 1
void init_assert(init_params *p, int offset)
{
  if (p->sec ? !NOSTATIC_WANTED && offset > p->sec->data_offset : !nocode_wanted && offset > p->local_offset)
    tcc_internal_error("initializer overflow");
}
#else
#define init_assert(sec, offset)
#endif

/* put zeros for variable based init */
void init_putz(init_params *p, unsigned long c, int size)
{
  init_assert(p, c + size);
  if (p->sec)
  {
    /* nothing to do because globals are already set to zero */
  }
  else if (tcc_state->ir && size <= 32 && !(size & 3))
  {
    /* Small, word-aligned zero-init: expand to individual word stores
     * of #0 so the optimizer can see (and eliminate) them when
     * subsequent field stores overwrite every word. */
    CType word_type;
    word_type.t = VT_INT;
    word_type.ref = NULL;

    SValue zero;
    svalue_init(&zero);
    zero.type = word_type;
    zero.r = VT_CONST;
    zero.vr = -1;
    zero.c.i = 0;

    const int clear_from = tcc_state->ir->next_instruction_index;
    for (int off = 0; off < size; off += 4)
    {
      SValue d;
      svalue_init(&d);
      d.type = word_type;
      d.r = VT_LOCAL | VT_LVAL;
      d.vr = -1;
      d.c.i = c + off;
      tcc_ir_put(tcc_state->ir, TCCIR_OP_STORE, &zero, NULL, &d);
    }
    /* yasc: the zeroing is no null stored in a member that is not '?' */
    if (tcc_state->yasc && tcc_state->ir->lt_on)
      tcc_ir_lt_note_init_store(tcc_state->ir, clear_from, tcc_state->ir->next_instruction_index, -1);
  }
  else if (tcc_state->ir && size > 0 && size <= 16)
  {
    /* Small non-word-aligned zero-init: expand to individual byte stores
     * of #0.  Byte-granular stores let downstream byte loads match
     * exactly (no partial-overlap forwarding required), which is the
     * shape produced by partially-initialized char arrays / packed
     * structs (e.g. `const char X[10] = { 'A', 'B', 'C', 'D', 'E' };`).
     * Size capped at 16 to stay within the contributing-store limit of
     * tcc_ir_opt_memmove_to_indexed_stores; larger sizes fall through
     * to the memset call below so the optimizer can still recognize the
     * memset-shift pattern. */
    CType byte_type;
    byte_type.t = VT_BYTE | VT_UNSIGNED;
    byte_type.ref = NULL;

    SValue zero;
    svalue_init(&zero);
    zero.type = byte_type;
    zero.r = VT_CONST;
    zero.vr = -1;
    zero.c.i = 0;

    for (int off = 0; off < size; off++)
    {
      SValue d;
      svalue_init(&d);
      d.type = byte_type;
      d.r = VT_LOCAL | VT_LVAL;
      d.vr = -1;
      d.c.i = c + off;
      tcc_ir_put(tcc_state->ir, TCCIR_OP_STORE, &zero, NULL, &d);
    }
  }
  else
  {
    SValue src1;
    SValue dest;

    vseti(VT_LOCAL, c);
    vpushi(0);
    vpushs(size);

    svalue_init(&src1);
    src1.vr = -1;
    const int call_id = tcc_state->ir ? tcc_state->ir->next_call_id++ : 0;
    /* __aeabi_memset(dest, n, c) on ARM EABI; memset(dest, c, n) elsewhere.
     * TOK_memset maps to __aeabi_memset when TCC_ARM_EABI is defined.
     * Stack is: dest, c, n */
    src1.r = VT_CONST;
    src1.c.i = TCCIR_ENCODE_PARAM(call_id, 0);
    LOG_CODEGEN("FUNCPARAMVAL push: site=init_putz call_id=%d param_idx=%d vtop_r=0x%x vtop_vr=%d", call_id,
                TCCIR_DECODE_PARAM_IDX((uint32_t)src1.c.i), vtop[-2].r, vtop[-2].vr);
    tcc_ir_put(tcc_state->ir, TCCIR_OP_FUNCPARAMVAL, &vtop[-2], &src1, NULL);
    src1.c.i = TCCIR_ENCODE_PARAM(call_id, 2);
    LOG_CODEGEN("FUNCPARAMVAL push: site=init_putz call_id=%d param_idx=%d vtop_r=0x%x vtop_vr=%d", call_id,
                TCCIR_DECODE_PARAM_IDX((uint32_t)src1.c.i), vtop[-1].r, vtop[-1].vr);
    tcc_ir_put(tcc_state->ir, TCCIR_OP_FUNCPARAMVAL, &vtop[-1], &src1, NULL);
    src1.c.i = TCCIR_ENCODE_PARAM(call_id, 1);
    LOG_CODEGEN("FUNCPARAMVAL push: site=init_putz call_id=%d param_idx=%d vtop_r=0x%x vtop_vr=%d", call_id,
                TCCIR_DECODE_PARAM_IDX((uint32_t)src1.c.i), vtop[0].r, vtop[0].vr);
    tcc_ir_put(tcc_state->ir, TCCIR_OP_FUNCPARAMVAL, &vtop[0], &src1, NULL);

    vpush_helper_func(TOK_memset);
    svalue_init(&dest);
    dest.vr = tcc_ir_get_vreg_temp(tcc_state->ir);
    dest.type.t = vtop[-3].type.t;
    dest.r = 0;
    SValue call_id_sv = tcc_ir_svalue_call_id_argc(call_id, 3);
    tcc_ir_put(tcc_state->ir, TCCIR_OP_FUNCCALLVOID, &vtop[0], &call_id_sv, &dest);
    vtop -= 4;

    // vtop -= 4;
    // vtop->r = 0;
    // vtop->vr = dest.vr;
    // vtop->r = 0;
    // vtop->vr = dest.vr;

#if defined(TCC_TARGET_ARM) && defined TCC_ARM_EABI
    // vswap(); /* using __aeabi_memset(void*, size_t, int) */
#endif
    // gfunc_call(3);
  }
}

#define DIF_FIRST 1
#define DIF_SIZE_ONLY 2
#define DIF_HAVE_ELEM 4
#define DIF_CLEAR 8

/* delete relocations for specified range c ... c + size. Unfortunatly
   in very special cases, relocations may occur unordered */
static void decl_design_delrels(Section *sec, int c, int size)
{
  ElfW_Rel *rel, *rel2, *rel_end;
  if (!sec || !sec->reloc)
    return;
  rel = rel2 = (ElfW_Rel *)sec->reloc->data;
  rel_end = (ElfW_Rel *)(sec->reloc->data + sec->reloc->data_offset);
  while (rel < rel_end)
  {
    if (rel->r_offset >= c && rel->r_offset < c + size)
    {
      sec->reloc->data_offset -= sizeof *rel;
    }
    else
    {
      if (rel2 != rel)
        memcpy(rel2, rel, sizeof *rel);
      ++rel2;
    }
    ++rel;
  }
}

void decl_design_flex(init_params *p, Sym *ref, int index)
{
  if (ref == p->flex_array_ref)
  {
    if (p->flex_array_bounded && index >= p->flex_array_max)
      tcc_error("initializer of flexible array member does not fit in the union");
    if (index >= ref->c)
      ref->c = index + 1;
  }
  else if (ref->c < 0)
  {
    if (p->flex_array_ref)
      tcc_error("initialization of flexible array member in a nested context");
    tcc_error("flexible array has zero size in this context");
  }
}

/* -Woverride-init: the bit ranges [lo, hi) initialized so far, unordered and
   disjoint; most initializers are one range, grown at its end. */
struct InitOvr
{
  struct InitOvr *prev; /* the live ones, innermost first (init_ovr_top) */
  int n, cap;
  int warned; /* the const-template probe already reported this initializer */
  long long hw; /* no range ends above this */
  long long (*r)[2]; /* signed: a local's offsets are negative */
  /* the element decl_designator is at: its field (or none), and its array
     index, or -1 a struct's field, OVR_UNION a union's, OVR_SWITCHED a
     union member that replaced another */
  Sym *leaf_f;
  int leaf;
  int leaf_is_field; /* the designator ends at a field (its size is its type's) */
  /* per element in progress: its bits, and where its sub-elements' ranges start */
  int nstk, capstk;
  struct
  {
    long long lo, hi;
    int n0;
  } *stk;
  /* the member each union (at bit offset off, of type u) was given last */
  int nu, capu;
  struct
  {
    long long off;
    Sym *u, *m;
  } *um;
};

/* every allocated InitOvr, so that error recovery can free the ones whose
   initializer it left (init_ovr_unwind) */
struct InitOvr *init_ovr_top;

static __attribute__((noinline)) struct InitOvr *ovr_get(init_params *p)
{
  if (!p->ovr)
  {
    p->ovr = tcc_mallocz(sizeof *p->ovr);
    p->ovr->hw = -((long long)1 << 62); /* below every offset */
    p->ovr->prev = init_ovr_top;
    init_ovr_top = p->ovr;
  }
  return p->ovr;
}

static void ovr_release(struct InitOvr *o)
{
  tcc_free(o->r);
  tcc_free(o->um);
  tcc_free(o->stk);
  tcc_free(o);
}

/* error recovery: free what the initializers the longjmp left allocated */
void init_ovr_unwind(struct InitOvr *to)
{
  while (init_ovr_top && init_ovr_top != to)
  {
    struct InitOvr *o = init_ovr_top;
    init_ovr_top = o->prev;
    ovr_release(o);
  }
}

void init_ovr_free(init_params *p)
{
  struct InitOvr **pp;
  if (!p->ovr)
    return;
  for (pp = &init_ovr_top; *pp; pp = &(*pp)->prev)
    if (*pp == p->ovr)
    {
      *pp = p->ovr->prev;
      break;
    }
  ovr_release(p->ovr);
  p->ovr = NULL;
}

/* the warnings were given already: record, report nothing */
void init_ovr_quiet(init_params *p)
{
  ovr_get(p)->warned = 1;
}

static void ovr_push(struct InitOvr *o, long long lo, long long hi)
{
  if (o->n && o->r[o->n - 1][0] <= lo && o->r[o->n - 1][1] >= lo)
  {
    if (o->r[o->n - 1][1] < hi)
      o->r[o->n - 1][1] = hi;
  }
  else
  {
    if (o->n == o->cap)
    {
      o->cap = o->cap ? o->cap * 2 : 8;
      o->r = tcc_realloc(o->r, o->cap * sizeof *o->r);
    }
    o->r[o->n][0] = lo;
    o->r[o->n][1] = hi;
    o->n++;
  }
  if (hi > o->hw)
    o->hw = hi;
}

/* Take [lo, hi) out of the set; nonzero if any of it was there. */
static int ovr_cut(struct InitOvr *o, long long lo, long long hi)
{
  int i, hit = 0;
  if (lo >= o->hw)
    return 0;
  for (i = 0; i < o->n; i++)
  {
    long long a = o->r[i][0], b = o->r[i][1];
    if (b <= lo || a >= hi)
      continue;
    hit = 1;
    if (a < lo && b > hi)
    {
      o->r[i][1] = lo;
      ovr_push(o, hi, b); /* appended: i + 1 .. are the rest, still scanned */
    }
    else if (a < lo)
      o->r[i][1] = lo;
    else if (b > hi)
      o->r[i][0] = hi;
    else
    {
      o->r[i][0] = o->r[o->n - 1][0];
      o->r[i][1] = o->r[o->n - 1][1];
      o->n--, i--;
    }
  }
  /* what the unions in there were given is gone with it */
  if (hit)
    for (i = 0; i < o->nu; i++)
      if (o->um[i].off >= lo && o->um[i].off < hi)
        o->um[i] = o->um[--o->nu], i--;
  return hit;
}

/* Member M of union U at byte C gets a value: nonzero if the union was
   given another member before (whose bits, all of the union's, are gone) */
static __attribute__((noinline)) int ovr_union_member(init_params *p, unsigned long c, Sym *u, Sym *m)
{
  struct InitOvr *o = p->ovr;
  const long long off = (long long)(long)c * 8;
  int i, align, switched = 0;
  for (i = 0; i < o->nu; i++)
    if (o->um[i].off == off && o->um[i].u == u)
    {
      CType ut = {VT_UNION, u};
      if (o->um[i].m == m)
        return 0;
      o->um[i] = o->um[--o->nu];
      ovr_cut(o, off, off + (long long)type_size(&ut, &align) * 8);
      switched = 1;
      break;
    }
  if (o->nu == o->capu)
  {
    o->capu = o->capu ? o->capu * 2 : 4;
    o->um = tcc_realloc(o->um, o->capu * sizeof *o->um);
  }
  o->um[o->nu].off = off;
  o->um[o->nu].u = u;
  o->um[o->nu].m = m;
  o->nu++;
  return switched;
}

/* the bits an element of TYPE at C (SIZE bytes) takes */
static void ovr_range(const CType *type, unsigned long c, int size, long long *lo, long long *hi)
{
  *lo = (long long)(long)c * 8; /* c: a section offset, or a (negative) frame offset */
  if (type->t & VT_BITFIELD)
    *lo += BIT_POS(type->t), *hi = *lo + BIT_SIZE(type->t);
  else
    *hi = *lo + (long long)size * 8;
}

/* decl_designator's step to field F of struct or union TYPE at C (TYPE
   null: F is inside an anonymous member, not followed).  A union records
   its member; once one switched, the rest of the designator is inside what
   replaced the other.  Out of line, and the state in p->ovr: decl_designator
   recurses with nested initializers (the parser's stack budget). */
#define OVR_UNION (-2)
#define OVR_SWITCHED (-3)
static __attribute__((noinline)) void ovr_field(init_params *p, const CType *type, unsigned long c, Sym *f)
{
  struct InitOvr *o = p->ovr;
  const int in_union = type && IS_UNION(type->t);
  const int sw = in_union && ovr_union_member(p, c, type->ref, f);
  if (o->leaf != OVR_SWITCHED)
    o->leaf_f = f, o->leaf = sw ? OVR_SWITCHED : in_union ? OVR_UNION : -1;
  o->leaf_is_field = 1;
}

/* find_field() for decl_designator, and its step recorded (ovr_field):
   one call in its loop, not two (the parser's stack budget) */
static __attribute__((noinline)) Sym *ovr_find_field(init_params *p, CType *type, int v, int *cumofs, unsigned long c)
{
  Sym *f = find_field(type, v, cumofs);
  if (p->ovr)
    ovr_field(p, *cumofs ? NULL : type, c, f);
  return f;
}

/* decl_designator's step to element INDEX of an array */
static __attribute__((noinline)) void ovr_index(init_params *p, int index)
{
  if (p->ovr->leaf != OVR_SWITCHED)
    p->ovr->leaf_f = NULL, p->ovr->leaf = index;
  p->ovr->leaf_is_field = 0;
}

/* decl_design_flex() for decl_designator's step to elements INDEX ..
   INDEX_LAST of an array, and the step recorded */
static __attribute__((noinline)) void ovr_design_flex(init_params *p, Sym *ref, int index, int index_last)
{
  decl_design_flex(p, ref, index_last);
  if (p->ovr)
    ovr_index(p, index);
}

/* What the element (p->ovr->leaf) takes, SIZE bytes of TYPE at C: an
   earlier element holding any of it loses that now. */
static __attribute__((noinline)) void ovr_before(init_params *p, const CType *type, unsigned long c, int size)
{
  long long lo, hi;
  Sym *f = p->ovr->leaf_f;
  const int leaf = p->ovr->leaf;
  int align;
  /* `[1].x`: decl_designator's elem_size is still the array element's */
  if (p->ovr->leaf_is_field)
    size = type_size(type, &align);
  ovr_range(type, c, size, &lo, &hi);
  if ((ovr_cut(p->ovr, lo, hi) || leaf == OVR_SWITCHED) && !p->ovr->warned)
  {
    const char *why = leaf <= OVR_UNION ? ": a union keeps only the member initialized last" : "";
    if (f && (f->v & ~SYM_FIELD) < SYM_FIRST_ANOM)
      tcc_warning_c(warn_override_init)("initializer for '%s' overrides an earlier one%s", get_tok_str(f->v & ~SYM_FIELD, NULL), why);
    else if (!f && leaf >= 0)
      tcc_warning_c(warn_override_init)("initializer for element [%d] overrides an earlier one", leaf);
    else
      tcc_warning_c(warn_override_init)("initializer overrides an earlier one%s", why);
  }
  if (p->ovr->nstk == p->ovr->capstk)
  {
    p->ovr->capstk = p->ovr->capstk ? p->ovr->capstk * 2 : 8;
    p->ovr->stk = tcc_realloc(p->ovr->stk, p->ovr->capstk * sizeof *p->ovr->stk);
  }
  p->ovr->stk[p->ovr->nstk].lo = lo;
  p->ovr->stk[p->ovr->nstk].hi = hi;
  p->ovr->stk[p->ovr->nstk++].n0 = p->ovr->n;
}

/* The element is done: recorded whole (a brace list sets every bit, the
   unnamed ones to zero) in place of its sub-elements.  Nothing pushed: the
   warning came on inside it (a pragma). */
static __attribute__((noinline)) void ovr_after(init_params *p)
{
  struct InitOvr *o = p->ovr;
  if (!o->nstk)
    return;
  o->nstk--;
  o->n = o->stk[o->nstk].n0;
  ovr_push(o, o->stk[o->nstk].lo, o->stk[o->nstk].hi);
}

/* t is the array or struct type. c is the array or struct
   address. cur_field is the pointer to the current
   field, for arrays the 'c' member contains the current start
   index.  'flags' is as in decl_initializer.
   'al' contains the already initialized length of the
   current container (starting at c).  This returns the new length of that.  */
/* the member decl_initializer is filling (an array element: none), for yasc's
   "null into a member that is not '?'" (yasc_init_null) */
Sym *yasc_init_field;

int decl_designator(init_params *p, CType *type, unsigned long c, Sym **cur_field, int flags, int al)
{
  Sym *s, *f, *yf = NULL;
  int index, index_last, align, l, nb_elems, elem_size;
  unsigned long corig = c;
  /* -Woverride-init: what this element is goes to p->ovr (ovr_before) */
  /* a size-only pass runs before any element is recorded: p->ovr is null in it */
#define OVR_ON (p->ovr)
  if (!(flags & DIF_SIZE_ONLY) && tcc_state->warn_override_init)
    ovr_get(p)->leaf_f = NULL, p->ovr->leaf = -1, p->ovr->leaf_is_field = 0;
  /* an array element keeps the byte order of its array; a member takes its
     own (big-endian scalar_storage_order marks every member) */
  int sso_be = p->sso_be;

  elem_size = 0;
  nb_elems = 1;

  if (flags & DIF_HAVE_ELEM)
    goto no_designator;

  if (gnu_ext && tok >= TOK_UIDENT)
  {
    l = tok, next();
    if (tok == ':')
      goto struct_field;
    unget_tok(l);
  }

  /* NOTE: we only support ranges for last designator */
  while (nb_elems == 1 && (tok == '[' || tok == '.'))
  {
    if (tok == '[')
    {
      if (!(type->t & VT_ARRAY))
        expect("array type");
      next();
      index = index_last = expr_const();
      if (tok == TOK_DOTS && gnu_ext)
      {
        next();
        index_last = expr_const();
      }
      skip(']');
      s = type->ref;
      ovr_design_flex(p, s, index, index_last);
      if (index < 0 || index_last >= s->c || index_last < index)
        tcc_error("index exceeds array bounds or range is empty");
      if (cur_field)
        (*cur_field)->c = index_last;
      type = pointed_type(type);
      elem_size = type_size(type, &align);
      c += index * elem_size;
      nb_elems = index_last - index + 1;
      yf = NULL;
    }
    else
    {
      int cumofs;
      next();
      l = tok;
    struct_field:
      next();
      f = ovr_find_field(p, type, l, &cumofs, c);
      if (cur_field)
        *cur_field = f;
      type = &f->type;
      c += cumofs;
      sso_be = f->a.sso_be;
      yf = f;
    }
    cur_field = NULL;
  }
  if (!cur_field)
  {
    if (tok == '=')
    {
      next();
    }
    else if (!gnu_ext)
    {
      expect("=");
    }
  }
  else
  {
  no_designator:
    if (type->t & VT_ARRAY)
    {
      index = (*cur_field)->c;
      s = type->ref;
      decl_design_flex(p, s, index);
      if (index >= s->c)
        tcc_error("too many initializers");
      if (OVR_ON)
        ovr_index(p, index);
      type = pointed_type(type);
      elem_size = type_size(type, &align);
      c += index * elem_size;
      yf = NULL;
    }
    else
    {
      f = *cur_field;
      /* Skip bitfield padding. Also with size 32 and 64. */
      while (f && (f->v & SYM_FIRST_ANOM) && is_integer_btype(f->type.t & VT_BTYPE))
        *cur_field = f = f->next;
      if (!f)
        tcc_error("too many initializers");
      if (OVR_ON)
        ovr_field(p, type, c, f);
      type = &f->type;
      c += f->c;
      sso_be = f->a.sso_be;
      yf = f;
    }
  }

  if (!elem_size) /* for structs */
    elem_size = type_size(type, &align);

  /* Using designators the same element can be initialized more
     than once.  In that case we need to delete possibly already
     existing relocations. */
  if (!(flags & DIF_SIZE_ONLY) && c - corig < al)
  {
    decl_design_delrels(p->sec, c, elem_size * nb_elems);
    flags &= ~DIF_CLEAR; /* mark stack dirty too */
  }

  /* -Woverride-init.  The element's bits: an earlier element holding any of
     them loses them now.  Its own sub-elements are checked against the
     rest (they are added after this cut), and once they are done the
     element is recorded whole. */
  if (OVR_ON)
    ovr_before(p, type, c, elem_size * nb_elems);

  const int sso_be_outer = p->sso_be;
  Sym *const yf_outer = yasc_init_field;
  p->sso_be = sso_be;
  yasc_init_field = yf;
  decl_initializer(p, type, c, flags & ~DIF_FIRST, -1);
  yasc_init_field = yf_outer;
  p->sso_be = sso_be_outer;

  if (OVR_ON)
    ovr_after(p);
#undef OVR_ON

  if (!(flags & DIF_SIZE_ONLY) && nb_elems > 1)
  {
    Sym aref = {0};
    CType t1;
    int i;
    if (p->sec || (type->t & VT_ARRAY))
    {
      /* make init_putv/vstore believe it were a struct */
      aref.c = elem_size;
      t1.t = VT_STRUCT, t1.ref = &aref;
      type = &t1;
    }
    if (p->sec)
    {
      vpush_ref(type, p->sec, c, elem_size);
      for (i = 1; i < nb_elems; i++)
      {
        vdup();
        init_putv(p, type, c + elem_size * i, -1);
      }
      vpop();
    }
    else
    {
      /* Local range designators: copy the first element's value into each
         subsequent slot using vstore, so stack-relative addressing stays
         correct. */
      for (i = 1; i < nb_elems; i++)
      {
        vset(type, VT_LOCAL | VT_LVAL, c + elem_size * i); /* dest */
        vset(type, VT_LOCAL | VT_LVAL, c);                 /* src */
        vstore();
        vpop(); /* drop dest/result left by vstore */
      }
    }
  }

  c += nb_elems * elem_size;
  if (c - corig > al)
    al = c - corig;
  return al;
}

/* The value just written at sec+c is `&&lab1 - &&lab0` (vtop carries the label
 * difference marker): remember to add the label offset difference, in `size`
 * bytes, once the function's code is laid out. */
static void label_diff_record(Section *sec, unsigned long c, int size)
{
  LabelDiffFixup *fixup = tcc_malloc(sizeof(LabelDiffFixup));
  fixup->sec = sec;
  fixup->offset = c;
  fixup->size = size;
  fixup->sym_plus = pending_label_diff_plus;
  fixup->sym_minus = pending_label_diff_minus;
  fixup->next = tcc_state->label_diff_fixups;
  tcc_state->label_diff_fixups = fixup;
  pending_label_diff_plus = NULL;
  pending_label_diff_minus = NULL;
}

static void sso_reverse_bytes(unsigned char *b, int n)
{
  for (int i = 0; i < n / 2; i++)
  {
    unsigned char t = b[i];
    b[i] = b[n - 1 - i];
    b[n - 1 - i] = t;
  }
}

/* store a value or an expression directly in global data or in local array */
void init_putv(init_params *p, CType *type, unsigned long c, int vreg)
{
  int bt;
  void *ptr;
  CType dtype;
  int size, align;
  Section *sec = p->sec;
  uint64_t val;

  dtype = *type;
  dtype.t &= ~VT_CONSTANT; /* need to do that to avoid false warning */
  if (type->t & VT_RODATA_REL)
  {
    /* The relocation encodes it (rodata_rel_init); only a static object has
       one, an automatic one would be filled by a store. */
    if (!sec)
      rodata_rel_store_error();
    dtype.t &= ~VT_RODATA_REL;
  }

  size = type_size(type, &align);
  if (type->t & VT_BITFIELD)
    size = (BIT_POS(type->t) + BIT_SIZE(type->t) + 7) / 8;
  init_assert(p, c + size);
  /* bytes of a byte-reversed scalar (big-endian scalar_storage_order) */
  const int sso_size = p->sso_be ? sso_scalar_size(type) : 0;

  if (p->const_probe)
  {
    /* Template probe (see init_params): capture a plain load-time-constant
     * integer/pointer scalar into the host-side template buffer, emit nothing.
     * Anything else (runtime value, symbol/relocation, bitfield, float/struct,
     * out-of-range offset) aborts the templating attempt. */
    int pbt = type->t & VT_BTYPE;
    int rel = (int)c - p->const_probe_base;
    /* Convert to the element type first, as vstore does: a float constant into
       an int element, or any non-zero constant into a _Bool, must not be
       written raw.  The probe fails below unless the result is still a plain
       constant. */
    if ((vtop->r & (VT_VALMASK | VT_LVAL | VT_SYM)) == VT_CONST && !(type->t & VT_BITFIELD) &&
        !(type->t & VT_COMPLEX) && pbt != VT_STRUCT)
      gen_assign_cast(&dtype);
    if ((vtop->r & (VT_VALMASK | VT_LVAL | VT_SYM)) == VT_CONST && !(type->t & VT_BITFIELD) &&
        !(type->t & VT_COMPLEX) &&
        (vtop->type.t & VT_BTYPE) != VT_FLOAT && (vtop->type.t & VT_BTYPE) != VT_DOUBLE &&
        (vtop->type.t & VT_BTYPE) != VT_LDOUBLE &&
        (pbt == VT_BOOL || pbt == VT_BYTE || pbt == VT_SHORT || pbt == VT_INT || pbt == VT_LLONG || pbt == VT_PTR) &&
        rel >= 0 && rel + size <= p->const_probe_size)
    {
      uint64_t cval = (uint64_t)vtop->c.i;
      unsigned char *d = p->const_probe_data + rel;
      switch (size)
      {
      case 1:
        d[0] = (unsigned char)cval;
        break;
      case 2:
        write16le(d, (uint16_t)cval);
        break;
      case 4:
        write32le(d, (uint32_t)cval);
        break;
      case 8:
        write64le(d, cval);
        break;
      default:
        p->const_probe_failed = 1;
        break;
      }
      sso_reverse_bytes(d, sso_size);
    }
    else
    {
      p->const_probe_failed = 1;
    }
    vtop--;
    return;
  }

  if (sec)
  {
    /* XXX: not portable */
    /* XXX: generate error if incorrect relocation */
    gen_assign_cast(&dtype);
    bt = type->t & VT_BTYPE;

    int is_label_diff = (vtop->r & VT_SYM) && vtop->sym == &label_diff_marker;
    if (is_label_diff && (type->t & VT_BITFIELD || (bt != VT_BYTE && bt != VT_SHORT && bt != VT_INT && bt != VT_PTR)))
      tcc_error("initializer element is not computable at load time");
    if (!is_label_diff && (vtop->r & VT_SYM) && bt != VT_PTR &&
        (bt != (PTR_SIZE == 8 ? VT_LLONG : VT_INT) || (type->t & VT_BITFIELD)) &&
        !((vtop->r & VT_CONST) && vtop->sym->v >= SYM_FIRST_ANOM))
      tcc_error("initializer element is not computable at load time");

    if (NOSTATIC_WANTED)
    {
      vtop--;
      print_vstack("init_putv");
      return;
    }

    ptr = sec->data + c;
    val = vtop->c.i;

    /* XXX: make code faster ? */
    if ((vtop->r & (VT_SYM | VT_CONST)) == (VT_SYM | VT_CONST) && vtop->sym->v >= SYM_FIRST_ANOM &&
        /* XXX This rejects compound literals like
           '(void *){ptr}'.  The problem is that '&sym' is
           represented the same way, which would be ruled out
           by the SYM_FIRST_ANOM check above, but also '"string"'
           in 'char *p = "string"' is represented the same
           with the type being VT_PTR and the symbol being an
           anonymous one.  That is, there's no difference in vtop
           between '(void *){x}' and '&(void *){x}'.  Ignore
           pointer typed entities here.  Hopefully no real code
           will ever use compound literals with scalar type.  */
        (vtop->type.t & VT_BTYPE) != VT_PTR)
    {
      /* These come from compound literals, memcpy stuff over.  */
      Section *ssec;
      ElfSym *esym;
      ElfW_Rel *rel;
      esym = elfsym(vtop->sym);
      ssec = tcc_state->sections[esym->st_shndx];
      const unsigned long base = esym->st_value + (int)vtop->c.i;
      memmove(ptr, ssec->data + base, size);
      if (ssec->reloc)
      {
        /* We need to copy over all memory contents, and that
           includes relocations.  Use the fact that relocs are
           created it order, so look from the end of relocs
           until we hit one before the copied region.  */
        unsigned long relofs = ssec->reloc->data_offset;
        while (relofs >= sizeof(*rel))
        {
          relofs -= sizeof(*rel);
          rel = (ElfW_Rel *)(ssec->reloc->data + relofs);
          if (rel->r_offset >= base + size)
            continue;
          if (rel->r_offset < base)
            break;
          put_elf_reloca(symtab_section, sec, c + rel->r_offset - base, ELFW(R_TYPE)(rel->r_info),
                         ELFW(R_SYM)(rel->r_info),
#if PTR_SIZE == 8
                         rel->r_addend
#else
                         0
#endif
          );
        }
      }
    }
    else
    {
      if (type->t & VT_BITFIELD)
      {
        int bit_pos, bit_size, bits, n;
        unsigned char *p, v, m;
        /* a big-endian scalar_storage_order unit: its bit position counts
           from the LSB of the swapped value */
        const Sym *bf = type->ref;
        const int unit = bf && bf->a.sso_be ? bf->r : 0;
        sso_reverse_bytes(ptr, unit);
        bit_pos = BIT_POS(vtop->type.t);
        bit_size = BIT_SIZE(vtop->type.t);
        p = (unsigned char *)ptr + (bit_pos >> 3);
        bit_pos &= 7, bits = 0;
        while (bit_size)
        {
          n = 8 - bit_pos;
          if (n > bit_size)
            n = bit_size;
          v = val >> bits << bit_pos;
          m = ((1 << n) - 1) << bit_pos;
          *p = (*p & ~m) | (v & m);
          bits += n, bit_size -= n, bit_pos = 0, ++p;
        }
        sso_reverse_bytes(ptr, unit);
      }
      else if (type->t & VT_COMPLEX)
      {
        /* Complex integer types: write packed representation directly.
         * The value is packed as [real | imag] in CValue.i,
         * matching little-endian memory layout. */
        int complex_size = type_size(type, &align);
        if (complex_size == 2)
          write16le(ptr, val);
        else if (complex_size == 4)
          write32le(ptr, val);
        else if (complex_size == 8)
          write64le(ptr, val);
        else
          memcpy(ptr, &vtop->c, complex_size);
      }
      else
        switch (bt)
        {
        case VT_BOOL:
          *(char *)ptr = val != 0;
          break;
        case VT_BYTE:
          *(char *)ptr = val;
          if (is_label_diff)
            label_diff_record(sec, c, 1);
          break;
        case VT_SHORT:
          write16le(ptr, val);
          if (is_label_diff)
            label_diff_record(sec, c, 2);
          break;
        case VT_FLOAT:
          write32le(ptr, val);
          break;
        case VT_DOUBLE:
          write64le(ptr, val);
          break;
        case VT_LDOUBLE:
#if defined TCC_IS_NATIVE_387
          /* Host and target platform may be different but both have x87.
             On windows, tcc does not use VT_LDOUBLE, except when it is a
             cross compiler.  In this case a mingw gcc as host compiler
             comes here with 10-byte long doubles, while msvc or tcc won't.
             tcc itself can still translate by asm.
             In any case we avoid possibly random bytes 11 and 12.
          */
          if (sizeof(long double) >= 10)
            memcpy(ptr, &vtop->c.ld, 10);
#ifdef __TINYC__
          else if (sizeof(long double) == sizeof(double))
            __asm__("fldl %1\nfstpt %0\n" : "=m"(*ptr) : "m"(vtop->c.ld));
#endif
          else
#endif
            /* For other platforms it should work natively, but may not work
               for cross compilers */
            if (sizeof(long double) == LDOUBLE_SIZE)
              memcpy(ptr, &vtop->c.ld, LDOUBLE_SIZE);
            else if (sizeof(double) == LDOUBLE_SIZE)
              *(double *)ptr = (double)vtop->c.ld;
            else if (0 == memcmp(ptr, &vtop->c.ld, LDOUBLE_SIZE))
              ; /* nothing to do for 0.0 */
#ifndef TCC_CROSS_TEST
            else
              tcc_error("can't cross compile long double constants");
#endif
          break;

#if PTR_SIZE == 8
        /* intptr_t may need a reloc too, see tcctest.c:relocation_test() */
        case VT_LLONG:
        case VT_PTR:
          if (vtop->r & VT_SYM)
            greloca(sec, gfunc_regpair_address_sym(vtop->sym), c, R_DATA_PTR, val);
          else
            write64le(ptr, val);
          break;
        case VT_INT:
          write32le(ptr, val);
          break;
#else
        case VT_LLONG:
          write64le(ptr, val);
          break;
        case VT_PTR:
        case VT_INT:
          if (type->t & VT_RODATA_REL)
          {
            rodata_rel_init(sec, c);
            break;
          }
          if ((vtop->r & VT_SYM) && vtop->sym == &label_diff_marker)
          {
            /* &&lab1 - &&lab0: the value written is the addend difference;
               the label offsets are added once the function is generated. */
            label_diff_record(sec, c, 4);
          }
          else if (vtop->r & VT_SYM)
          {
            /* Debug check for garbage symbol */
            if (!vtop->sym || vtop->sym->v >= SYM_FIRST_ANOM + 100000)
            {
              tcc_error("internal error: init_putv has garbage sym (v=0x%x, r=0x%x)", vtop->sym ? vtop->sym->v : 0,
                        vtop->r);
            }
            greloc(sec, gfunc_regpair_address_sym(vtop->sym), c, R_DATA_PTR);
          }
          write32le(ptr, val);
          break;
#endif
        default:
          // tcc_internal_error("unexpected type");
          break;
        }
      if (sso_size)
      {
        if (vtop->r & VT_SYM)
          tcc_error("initializer element is not computable at load time");
        sso_reverse_bytes(ptr, sso_size);
      }
    }
    vtop--;
    print_vstack("init_putv(2)");
  }
  else
  {
    /* Capture scalar constant into the tracked sym's const_init_data
     * before vstore (which pops the value). Buffer was zeroed at
     * allocation time, so zero values can be silently dropped. */
    SymLocalFacts *cf = p->const_init_sym ? sym_facts_peek(p->const_init_sym) : NULL;
    if (cf && cf->const_init_valid)
    {
      int rel_off = (int)c - p->const_init_base;
      int bt = type->t & VT_BTYPE;
      /* An element the buffer cannot record (a whole struct or vector, a
       * bitfield) leaves its bytes unknown.  A copy from a source with known
       * bytes re-validates the buffer in vstore. */
      if (rel_off + size > 0 && rel_off < cf->const_init_size && ((type->t & VT_BITFIELD) || bt == VT_STRUCT))
        cf->const_init_valid = 0;
      if (rel_off >= 0 && rel_off + size <= cf->const_init_size && !(type->t & VT_BITFIELD) &&
          bt != VT_STRUCT)
      {
        /* Convert to the element type first, as vstore does: the buffer must
           hold the stored representation, not the source constant's bits. */
        if ((vtop->r & (VT_VALMASK | VT_LVAL | VT_SYM)) == VT_CONST && !(type->t & VT_COMPLEX))
          gen_assign_cast(&dtype);
        if ((vtop->r & (VT_VALMASK | VT_LVAL | VT_SYM)) == VT_CONST)
        {
          uint64_t cval = (uint64_t)vtop->c.i;
          unsigned char *dst = cf->const_init_data + rel_off;
          switch (size)
          {
          case 1:
            dst[0] = (unsigned char)cval;
            break;
          case 2:
            write16le(dst, (uint16_t)cval);
            break;
          case 4:
            write32le(dst, (uint32_t)cval);
            break;
          case 8:
            write64le(dst, cval);
            break;
          default:
            cf->const_init_valid = 0;
            break;
          }
          sso_reverse_bytes(dst, sso_size);
        }
        else
        {
          cf->const_init_valid = 0;
        }
      }
    }
    vset(&dtype, VT_LOCAL | VT_LVAL, c);
    if (vreg == -1)
    {
      /* Array element initialization: do NOT create a new vreg.
       * Instead, keep vr = -1 so that vstore() will recognize this
       * as a memory store, not a variable assignment.
       * The stack offset 'c' in vtop->c.i identifies the destination. */
      vtop->vr = -1;
    }
    else
    {
      vtop->vr = vreg;
      /* Mark long long variables for proper register allocation */
      if ((dtype.t & VT_BTYPE) == VT_LLONG)
      {
        tcc_ir_set_llong_type(tcc_state->ir, vtop->vr);
      }
    }
    vtop->sso_reversed = sso_size != 0;
    vswap();
    vstore();
    vpop();
  }
}

