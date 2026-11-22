/*
 *  TCC - Tiny C Compiler
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 */

/*
 * -Warray-bounds=2: the value ranges of array subscripts, on the raw IR of a
 * function before any pass (as ir/lifetime.c), so what it reports is the
 * same at every -O.  Off unless the level is 2: plain -Warray-bounds (and
 * -Wall) is level 1, the frontend's constant and loop-pattern checks.
 *
 * The frontend records each `a[i]` of an array of known size whose index is
 * in a variable or a temp (tcc_ir_ab_site: where the address computation
 * starts, the index vreg, the element count, `&a[n]` allowed or not).  Here
 * an interval abstract interpretation over the raw IR's blocks follows the
 * vregs those indexes are computed from (the backward slice, and the other
 * side of each comparison of them): one interval per vreg and per block,
 * narrowed on the edges of `CMP` + `JUMPIF` and `TEST_ZERO` + `JUMPIF`.
 * The raw IR is not SSA -- a variable is assigned in many places -- but an
 * interval per variable and per program point needs none: a dense analysis
 * is the "check SSA" of the plan, built only when the option is on.
 *
 * Loops: a loop is unrolled iteration by iteration while each iteration is
 * DEFINITE -- its header state decides every exit, as for `i = 0; while (i
 * <= 10)` -- so the last trip is seen with its own values (i == 10), not a
 * widened [0, 10].  An iteration that may leave the loop on an unknown
 * condition (a break on a load, a return, a noreturn call) ends the
 * unrolling after that iteration (whose own subscripts are still checked,
 * as code under an unknown `if`): from there the usual widening (after two
 * plain joins) and two narrowing passes find a post-fixpoint, and a final
 * pass lets states out of the loop.  The widened trips are not checked:
 * they run only for some values of what is unknown (`j < n` past the trips
 * the header decided), so a range out of bounds there is not proven.
 *
 * A subscript is reported when its block is reached and the index's whole
 * interval lies outside [0, n - 1] ([0, n] for `&a[n]`): proven out of
 * bounds on a feasible path, never an unknown range.  Every subscript is
 * reported once, with gcc's text -- the first value found out of bounds
 * (the first such trip of a loop), or gcc's "[lo, hi] is outside" for a
 * range -- and a note with all the values; not when the level-1 loop
 * pattern already said it.
 *
 * Unknown: what an inlined body computes (the function is checked on its
 * own; so -O0 and -O2 agree), memory, globals, 64-bit and floating values,
 * variables whose address is taken.  Functions with computed gotos, asm
 * goto, setjmp or an irreducible CFG are skipped; caps on the blocks,
 * loops, followed vregs and visited instructions keep the cost bounded (a
 * function that runs out says only what it proved so far).
 */

#define USING_GLOBALS
#include "ir.h"
#include "cfg.h"

#define AB_MAX_SITES 512
#define AB_MAX_TRACK 128
#define AB_MAX_BLOCKS 8192
#define AB_MAX_LOOPS 512
#define AB_MAX_CELLS (1 << 20) /* blocks x followed vregs */
#define AB_UNROLL 4096         /* iterations of one loop, unrolled */
#define AB_BUDGET (1 << 21)    /* instructions visited per function */

/* a subscript the frontend recorded */
typedef struct IRAbSite
{
  int start, end; /* what gen_op('+') emitted for the address: [start, end) */
  int32_t vreg;   /* the index: a variable or a temp, read at start */
  int n, line;
  uint8_t is_unsigned, one_past, done;
  char type[64]; /* gcc's 'int[10]' */
} IRAbSite;

int tcc_ir_ab_active(void)
{
  TCCState *s1 = tcc_state;
  return s1->array_bounds_level >= 2 && ((s1->warn_array_bounds & WARN_ON) || s1->nb_diag_snaps) && !s1->warn_none;
}

void tcc_ir_ab_begin(TCCIRState *ir)
{
  ir->ab_on = tcc_ir_ab_active();
  ir->ab_nb_sites = 0;
  ir->ab_nb_inl = 0;
  ir->ab_nb_said = 0;
}

void tcc_ir_ab_free(TCCIRState *ir)
{
  tcc_free(ir->ab_sites);
  ir->ab_sites = NULL;
  ir->ab_nb_sites = 0;
  tcc_free(ir->ab_inl);
  ir->ab_inl = NULL;
  ir->ab_nb_inl = 0;
  tcc_free(ir->ab_said);
  ir->ab_said = NULL;
  ir->ab_nb_said = 0;
}

/* the frontend's loop pattern (objsize.c) reported subscript idx at line */
void tcc_ir_ab_said(TCCIRState *ir, int line, long long idx)
{
  if (ir->ab_nb_said >= AB_MAX_SITES)
    return;
  if (!(ir->ab_nb_said & (ir->ab_nb_said - 1)))
    ir->ab_said = tcc_realloc(ir->ab_said, (ir->ab_nb_said ? 2 * ir->ab_nb_said : 4) * 2 * sizeof(long long));
  ir->ab_said[2 * ir->ab_nb_said] = line;
  ir->ab_said[2 * ir->ab_nb_said + 1] = idx;
  ir->ab_nb_said++;
}

int tcc_ir_ab_site(TCCIRState *ir, int32_t vreg, int is_unsigned, int n, int line, const char *type)
{
  const int start = ir->next_instruction_index;
  /* the IR was rolled back over them (a folded call's arguments) */
  while (ir->ab_nb_sites && ir->ab_sites[ir->ab_nb_sites - 1].end > start)
    ir->ab_nb_sites--;
  if (ir->ab_nb_sites >= AB_MAX_SITES)
    return -1;
  if (!(ir->ab_nb_sites & (ir->ab_nb_sites - 1)))
    ir->ab_sites = tcc_realloc(ir->ab_sites, (ir->ab_nb_sites ? 2 * ir->ab_nb_sites : 4) * sizeof(IRAbSite));
  IRAbSite *s = &ir->ab_sites[ir->ab_nb_sites];
  s->start = start;
  s->end = INT32_MAX;
  s->vreg = vreg;
  s->n = n;
  s->line = line;
  s->is_unsigned = is_unsigned;
  s->one_past = 0;
  s->done = 0;
  pstrcpy(s->type, sizeof s->type, type);
  return ir->ab_nb_sites++;
}

void tcc_ir_ab_site_mid(TCCIRState *ir, int site)
{
  if (site < ir->ab_nb_sites)
    ir->ab_sites[site].end = ir->next_instruction_index;
}

void tcc_ir_ab_site_end(TCCIRState *ir, int site, int one_past_ok)
{
  if (site < ir->ab_nb_sites)
  {
    ir->ab_sites[site].one_past = !!one_past_ok;
    ir->ab_sites[site].done = 1;
  }
}

void tcc_ir_ab_inline_mark(TCCIRState *ir, int begin)
{
  if (begin)
  {
    if (!(ir->ab_nb_inl & (ir->ab_nb_inl - 1)))
      ir->ab_inl = tcc_realloc(ir->ab_inl, (ir->ab_nb_inl ? 2 * ir->ab_nb_inl : 4) * 2 * sizeof(int));
    ir->ab_inl[2 * ir->ab_nb_inl] = ir->next_instruction_index;
    ir->ab_inl[2 * ir->ab_nb_inl + 1] = INT32_MAX;
    ir->ab_nb_inl++;
  }
  else if (ir->ab_nb_inl)
    ir->ab_inl[2 * ir->ab_nb_inl - 1] = ir->next_instruction_index;
}

/* ------------------------------------------------------------------------ */

typedef struct AbIv
{
  int32_t lo, hi;
} AbIv;

#define AB_TOP ((AbIv){INT32_MIN, INT32_MAX})

typedef struct AbLoop
{
  int head, parent;
  int nbody, *body;   /* every block of it, nested loops' too */
  int nitems, *items; /* its own blocks and the headers of the loops right inside, in RPO */
  uint8_t has_site;
  /* while it is being walked */
  uint8_t active, quiet, has_exit, back_reach;
  AbIv *back; /* what its latches send to the header */
} AbLoop;

/* what a subscript was proven to be */
typedef struct AbHit
{
  uint8_t side; /* 0 none, 1 above, 2 below */
  uint8_t ranges; /* one of the proofs was a range, not a value */
  int64_t first;  /* the first value proven (the first trip out of bounds) */
  int64_t lo, hi; /* all of them */
} AbHit;

typedef struct AbCtx
{
  TCCIRState *ir;
  IRCFG *cfg;
  int n, nb, nt;
  int *map[4], mapn[4]; /* by vreg type, position -> slot, -1 not followed */
  AbIv *in;             /* nb x nt */
  uint8_t *reach;
  AbIv *cur, *tmp;
  uint8_t *inl; /* per instruction: in an inlined body */
  int *inner;   /* per block: the innermost loop, -1 none */
  int *head_loop;
  AbLoop *loops;
  int nloops;
  uint32_t *bits; /* nloops x bwords: the loops' blocks */
  int bwords;
  int ntop, *top;
  IRAbSite *sites;
  int nsites;
  int *site_first; /* per block */
  AbHit *hits;
  int act[AB_MAX_LOOPS], nact;
  int checking, abort;
  long visits;
} AbCtx;

static int ab_in_loop(AbCtx *c, int l, int b)
{
  return (c->bits[(size_t)l * c->bwords + (b >> 5)] >> (b & 31)) & 1;
}

/* the slot of a vreg, -1 not followed */
static int ab_slot(AbCtx *c, int32_t v)
{
  int t, p;
  if (v < 0)
    return -1;
  t = TCCIR_DECODE_VREG_TYPE(v), p = TCCIR_DECODE_VREG_POSITION(v);
  if (t < TCCIR_VREG_TYPE_VAR || t > TCCIR_VREG_TYPE_PARAM || p >= c->mapn[t])
    return -1;
  return c->map[t][p];
}

/* an integer operand of 32 bits or less */
static int ab_int_op(IROperand op)
{
  int bt;
  if (irop_is_none(op) || op.is_complex)
    return 0;
  bt = irop_get_btype(op);
  return bt == IROP_BTYPE_INT32 || bt == IROP_BTYPE_INT8 || bt == IROP_BTYPE_INT16;
}

/* the vreg whose value op reads (not memory through it, not its address), -1 */
static int32_t ab_value_vreg(IROperand op)
{
  if (!ab_int_op(op) || !irop_is_vreg_value(op))
    return -1;
  if (irop_get_tag(op) == IROP_TAG_STACKOFF && irop_is_local(op) && !op.is_lval)
    return -1;
  return irop_get_vreg(op);
}

/* may v be followed: its address is not taken */
static int ab_followable(AbCtx *c, int32_t v)
{
  int t = TCCIR_DECODE_VREG_TYPE(v), p = TCCIR_DECODE_VREG_POSITION(v);
  if (t < TCCIR_VREG_TYPE_VAR || t > TCCIR_VREG_TYPE_PARAM || p >= c->mapn[t])
    return 0;
  IRLiveInterval *iv = tcc_ir_try_get_live_interval(c->ir, v);
  return !iv || !iv->addrtaken;
}

static int ab_follow(AbCtx *c, int32_t v)
{
  int t, p;
  if (v < 0 || !ab_followable(c, v))
    return 0;
  t = TCCIR_DECODE_VREG_TYPE(v), p = TCCIR_DECODE_VREG_POSITION(v);
  if (c->map[t][p] >= 0 || c->nt >= AB_MAX_TRACK)
    return 0;
  c->map[t][p] = c->nt++;
  return 1;
}

/* The variable a temp copies at instruction at, in its block (from): the
   temp is an ASSIGN or LOAD of it, a few instructions back, with no write
   to it since.  -1 none. */
static int32_t ab_copy_of(AbCtx *c, int from, int at, int32_t t)
{
  TCCIRState *ir = c->ir;
  if (t < 0 || TCCIR_DECODE_VREG_TYPE(t) != TCCIR_VREG_TYPE_TEMP)
    return -1;
  for (int j = at - 1; j >= from && j >= at - 16; j--)
  {
    IRQuadCompact *q = &ir->compact_instructions[j];
    if (!irop_config[q->op].has_dest)
      continue;
    IROperand d = tcc_ir_op_get_dest(ir, q);
    if (irop_is_none(d) || !irop_dest_defines_vreg(d) || irop_get_vreg(d) != t)
      continue;
    if (q->op != TCCIR_OP_ASSIGN && q->op != TCCIR_OP_LOAD)
      return -1;
    int32_t v = ab_value_vreg(tcc_ir_op_get_src1(ir, q));
    if (v < 0 || TCCIR_DECODE_VREG_TYPE(v) == TCCIR_VREG_TYPE_TEMP)
      return -1;
    for (int k = j + 1; k < at; k++)
    {
      IRQuadCompact *w = &ir->compact_instructions[k];
      if (irop_config[w->op].has_dest)
      {
        IROperand wd = tcc_ir_op_get_dest(ir, w);
        if (!irop_is_none(wd) && irop_dest_defines_vreg(wd) && irop_get_vreg(wd) == v)
          return -1;
      }
    }
    return v;
  }
  return -1;
}

/* The comparison block b's last jump tests: the CMP or TEST_ZERO right
   before it, and the condition (true on the taken edge).  -1 none. */
static int ab_cond(AbCtx *c, IRBasicBlock *bb, int *cond)
{
  TCCIRState *ir = c->ir;
  int last = bb->end_idx - 1;
  IRQuadCompact *lq = &ir->compact_instructions[last], *tq;
  if (lq->op != TCCIR_OP_JUMPIF || last <= bb->start_idx || c->inl[last - 1])
    return -1;
  tq = &ir->compact_instructions[last - 1];
  if (tq->op != TCCIR_OP_CMP && tq->op != TCCIR_OP_TEST_ZERO)
    return -1;
  *cond = (int)irop_get_imm64_ex(ir, tcc_ir_op_get_src1(ir, lq));
  switch (*cond)
  {
  case TOK_LT: case TOK_LE: case TOK_GT: case TOK_GE: case TOK_EQ: case TOK_NE:
  case TOK_ULT: case TOK_ULE: case TOK_UGT: case TOK_UGE:
    break;
  default:
    return -1;
  }
  if (!ab_int_op(tcc_ir_op_get_src1(ir, tq)) || (tq->op == TCCIR_OP_CMP && !ab_int_op(tcc_ir_op_get_src2(ir, tq))))
    return -1;
  return last - 1;
}

/* is v followed, or computed from a followed vreg (rel) */
static int ab_related(AbCtx *c, uint8_t **rel, int32_t v)
{
  int t, p;
  if (v < 0)
    return 0;
  if (ab_slot(c, v) >= 0)
    return 1;
  t = TCCIR_DECODE_VREG_TYPE(v), p = TCCIR_DECODE_VREG_POSITION(v);
  return t >= TCCIR_VREG_TYPE_VAR && t <= TCCIR_VREG_TYPE_PARAM && p < c->mapn[t] && rel[t][p];
}

/* The vregs worth following: the subscripts' indexes and what they are
   computed from; and both sides of each comparison of them or of a value
   computed from them (`i - 1 < 18`), with what those are computed from. */
static void ab_slice(AbCtx *c)
{
  TCCIRState *ir = c->ir;
  uint8_t *rel[4] = {0};
  for (int t = TCCIR_VREG_TYPE_VAR; t <= TCCIR_VREG_TYPE_PARAM; t++)
    rel[t] = tcc_mallocz(c->mapn[t] + 1);
  for (int s = 0; s < c->nsites; s++)
    ab_follow(c, c->sites[s].vreg);
  for (int pass = 0, changed = 1; changed && pass < 8; pass++)
  {
    changed = 0;
    for (int b = 0; b < c->nb; b++)
    {
      IRBasicBlock *bb = &c->cfg->blocks[b];
      for (int i = bb->start_idx; i < bb->end_idx; i++)
      {
        IRQuadCompact *q = &ir->compact_instructions[i];
        if (!irop_config[q->op].has_dest || c->inl[i])
          continue;
        IROperand d = tcc_ir_op_get_dest(ir, q);
        int32_t dv, s1, s2;
        if (irop_is_none(d) || !irop_dest_defines_vreg(d) || (dv = irop_get_vreg(d)) < 0 || !ab_followable(c, dv))
          continue;
        s1 = ab_value_vreg(tcc_ir_op_get_src1(ir, q));
        s2 = ab_value_vreg(tcc_ir_op_get_src2(ir, q));
        if (ab_slot(c, dv) >= 0)
        {
          changed |= ab_follow(c, s1);
          changed |= ab_follow(c, s2);
        }
        else if (!rel[TCCIR_DECODE_VREG_TYPE(dv)][TCCIR_DECODE_VREG_POSITION(dv)] &&
                 (ab_related(c, rel, s1) || ab_related(c, rel, s2)))
          rel[TCCIR_DECODE_VREG_TYPE(dv)][TCCIR_DECODE_VREG_POSITION(dv)] = 1, changed = 1;
      }
      int cond, ci = ab_cond(c, bb, &cond);
      if (ci < 0)
        continue;
      IRQuadCompact *tq = &ir->compact_instructions[ci];
      int32_t x = ab_value_vreg(tcc_ir_op_get_src1(ir, tq));
      int32_t y = tq->op == TCCIR_OP_CMP ? ab_value_vreg(tcc_ir_op_get_src2(ir, tq)) : -1;
      int32_t xv = ab_copy_of(c, bb->start_idx, ci, x), yv = ab_copy_of(c, bb->start_idx, ci, y);
      if (ab_related(c, rel, x) || ab_related(c, rel, xv) || ab_related(c, rel, y) || ab_related(c, rel, yv))
      {
        changed |= ab_follow(c, x);
        changed |= ab_follow(c, xv);
        changed |= ab_follow(c, y);
        changed |= ab_follow(c, yv);
      }
    }
  }
  for (int t = 0; t < 4; t++)
    tcc_free(rel[t]);
}

/* ---- intervals ---------------------------------------------------------- */

static AbIv ab_fit(int64_t lo, int64_t hi)
{
  if (lo < INT32_MIN || hi > INT32_MAX || lo > hi)
    return AB_TOP;
  return (AbIv){(int32_t)lo, (int32_t)hi};
}

/* what an operand holds, in state st */
static AbIv ab_val(AbCtx *c, const AbIv *st, IROperand op)
{
  if (!ab_int_op(op))
    return AB_TOP;
  if (irop_is_immediate(op) && !op.is_lval)
  {
    int32_t k = (int32_t)(uint32_t)irop_get_imm64_ex(c->ir, op);
    return (AbIv){k, k};
  }
  int s = ab_slot(c, ab_value_vreg(op));
  return s >= 0 ? st[s] : AB_TOP;
}

static int ab_const(AbIv a, int32_t *k)
{
  *k = a.lo;
  return a.lo == a.hi;
}

/* the values of a type: the operand's width and signedness */
static AbIv ab_type_range(IROperand d)
{
  switch (irop_get_btype(d))
  {
  case IROP_BTYPE_INT8:
    return d.is_unsigned ? (AbIv){0, 255} : (AbIv){-128, 127};
  case IROP_BTYPE_INT16:
    return d.is_unsigned ? (AbIv){0, 65535} : (AbIv){-32768, 32767};
  default:
    return AB_TOP;
  }
}

static int64_t ab_smear(int64_t v)
{
  int64_t m = 0;
  while (m < v)
    m = 2 * m + 1;
  return m;
}

/* the value instruction q computes, in the current state */
static AbIv ab_eval(AbCtx *c, IRQuadCompact *q, IROperand d)
{
  TCCIRState *ir = c->ir;
  IROperand s1 = tcc_ir_op_get_src1(ir, q), s2 = tcc_ir_op_get_src2(ir, q);
  AbIv a, b;
  int32_t k;
  int64_t p[4], lo, hi;
  switch (q->op)
  {
  case TCCIR_OP_ASSIGN:
  case TCCIR_OP_LOAD:
    return ab_val(c, c->cur, s1);
  case TCCIR_OP_ZEXT:
  {
    int w = 32;
    a = ab_val(c, c->cur, s1);
    for (int i = 0; i < 2; i++)
    {
      int bt = irop_get_btype(i ? d : s1);
      if (bt == IROP_BTYPE_INT8 && w > 8)
        w = 8;
      else if (bt == IROP_BTYPE_INT16 && w > 16)
        w = 16;
    }
    if (w == 32)
      return a;
    if (a.lo >= 0 && a.hi < (1 << w))
      return a;
    return (AbIv){0, (1 << w) - 1};
  }
  case TCCIR_OP_ADD:
  case TCCIR_OP_SUB:
  case TCCIR_OP_MUL:
  case TCCIR_OP_SHL:
  case TCCIR_OP_SAR:
  case TCCIR_OP_SHR:
  case TCCIR_OP_AND:
  case TCCIR_OP_OR:
  case TCCIR_OP_XOR:
  case TCCIR_OP_DIV:
  case TCCIR_OP_PDIV:
  case TCCIR_OP_UDIV:
  case TCCIR_OP_IMOD:
  case TCCIR_OP_UMOD:
    break;
  default:
    return AB_TOP;
  }
  a = ab_val(c, c->cur, s1), b = ab_val(c, c->cur, s2);
  switch (q->op)
  {
  case TCCIR_OP_ADD:
    return ab_fit((int64_t)a.lo + b.lo, (int64_t)a.hi + b.hi);
  case TCCIR_OP_SUB:
    return ab_fit((int64_t)a.lo - b.hi, (int64_t)a.hi - b.lo);
  case TCCIR_OP_MUL:
    p[0] = (int64_t)a.lo * b.lo, p[1] = (int64_t)a.lo * b.hi, p[2] = (int64_t)a.hi * b.lo, p[3] = (int64_t)a.hi * b.hi;
    lo = hi = p[0];
    for (int i = 1; i < 4; i++)
      lo = p[i] < lo ? p[i] : lo, hi = p[i] > hi ? p[i] : hi;
    return ab_fit(lo, hi);
  case TCCIR_OP_SHL:
    if (!ab_const(b, &k) || k < 0 || k > 31)
      return AB_TOP;
    return ab_fit((int64_t)a.lo * ((int64_t)1 << k), (int64_t)a.hi * ((int64_t)1 << k));
  case TCCIR_OP_SAR:
    if (!ab_const(b, &k) || k < 0 || k > 31)
      return AB_TOP;
    return (AbIv){a.lo >> k, a.hi >> k};
  case TCCIR_OP_SHR:
    if (!ab_const(b, &k) || k < 0 || k > 31)
      return AB_TOP;
    if (k == 0)
      return a;
    if (a.lo >= 0)
      return (AbIv){a.lo >> k, a.hi >> k};
    if (a.hi < 0)
      return (AbIv){(int32_t)((uint32_t)a.lo >> k), (int32_t)((uint32_t)a.hi >> k)};
    return (AbIv){0, (int32_t)(0xffffffffu >> k)};
  case TCCIR_OP_AND:
    if (a.lo >= 0 && b.lo >= 0)
      return (AbIv){0, a.hi < b.hi ? a.hi : b.hi};
    if (a.lo >= 0)
      return (AbIv){0, a.hi};
    if (b.lo >= 0)
      return (AbIv){0, b.hi};
    return AB_TOP;
  case TCCIR_OP_OR:
  case TCCIR_OP_XOR:
    if (a.lo < 0 || b.lo < 0)
      return AB_TOP;
    hi = ab_smear(a.hi > b.hi ? a.hi : b.hi);
    lo = q->op == TCCIR_OP_OR ? (a.lo > b.lo ? a.lo : b.lo) : 0;
    return ab_fit(lo, hi);
  case TCCIR_OP_DIV:
  case TCCIR_OP_PDIV:
    if (!ab_const(b, &k) || k == 0)
      return AB_TOP;
    if (k > 0)
      return ab_fit((int64_t)a.lo / k, (int64_t)a.hi / k);
    return ab_fit((int64_t)a.hi / k, (int64_t)a.lo / k);
  case TCCIR_OP_UDIV:
    if (!ab_const(b, &k) || k <= 0)
      return AB_TOP;
    if (a.lo >= 0)
      return (AbIv){a.lo / k, a.hi / k};
    if (k == 1)
      return a;
    return (AbIv){0, (int32_t)(0xffffffffu / (uint32_t)k)};
  case TCCIR_OP_IMOD:
  {
    int64_t m;
    if (b.lo > 0)
      m = b.hi;
    else if (b.hi < 0 && b.lo > INT32_MIN)
      m = -(int64_t)b.lo;
    else
      return AB_TOP;
    if (a.lo >= 0)
      return ab_fit(0, a.hi < m - 1 ? a.hi : m - 1);
    if (a.hi <= 0)
      return ab_fit(a.lo > 1 - m ? a.lo : 1 - m, 0);
    return ab_fit(1 - m, m - 1);
  }
  case TCCIR_OP_UMOD:
    if (b.lo <= 0)
      return a.lo >= 0 && b.hi < 0 ? a : AB_TOP;
    if (a.lo >= 0)
      return (AbIv){0, a.hi < b.hi - 1 ? a.hi : b.hi - 1};
    return (AbIv){0, b.hi - 1};
  default:
    return AB_TOP;
  }
}

/* instruction i, on the current state */
static void ab_instr(AbCtx *c, int i)
{
  TCCIRState *ir = c->ir;
  IRQuadCompact *q = &ir->compact_instructions[i];
  if (!irop_config[q->op].has_dest)
    return;
  IROperand d = tcc_ir_op_get_dest(ir, q);
  if (irop_is_none(d) || !irop_dest_defines_vreg(d))
    return;
  int s = ab_slot(c, irop_get_vreg(d));
  if (s < 0)
    return;
  AbIv r = c->inl[i] || !ab_int_op(d) ? AB_TOP : ab_eval(c, q, d);
  AbIv t = ab_type_range(d);
  if (r.lo < t.lo || r.hi > t.hi)
    r = t;
  c->cur[s] = r;
}

/* x and y after `x COND y` holds; 0: it cannot */
static int ab_constrain(AbIv *x, int cond, AbIv *y)
{
  switch (cond)
  {
  case TOK_ULT: case TOK_ULE:
    if (x->lo >= 0 && y->lo >= 0)
      return ab_constrain(x, cond == TOK_ULT ? TOK_LT : TOK_LE, y);
    if (y->lo >= 0)
    {
      /* x, as unsigned, below y: x is not negative */
      int64_t h = (int64_t)y->hi - (cond == TOK_ULT);
      if (x->lo < 0)
        x->lo = 0;
      if (x->hi > h)
        x->hi = (int32_t)h;
    }
    else if (y->hi < 0 && x->hi < 0)
      return ab_constrain(x, cond == TOK_ULT ? TOK_LT : TOK_LE, y);
    break;
  case TOK_UGT: case TOK_UGE:
    return ab_constrain(y, cond == TOK_UGT ? TOK_ULT : TOK_ULE, x);
  case TOK_LT:
    if ((int64_t)y->hi - 1 < x->hi)
      x->hi = (int32_t)((int64_t)y->hi - 1 < INT32_MIN ? INT32_MIN : y->hi - 1);
    if ((int64_t)x->lo + 1 > y->lo)
      y->lo = (int32_t)((int64_t)x->lo + 1 > INT32_MAX ? INT32_MAX : x->lo + 1);
    if (y->hi == INT32_MIN || x->lo == INT32_MAX)
      return 0;
    break;
  case TOK_LE:
    if (y->hi < x->hi)
      x->hi = y->hi;
    if (x->lo > y->lo)
      y->lo = x->lo;
    break;
  case TOK_GT:
    return ab_constrain(y, TOK_LT, x);
  case TOK_GE:
    return ab_constrain(y, TOK_LE, x);
  case TOK_EQ:
    if (y->lo > x->lo)
      x->lo = y->lo;
    if (y->hi < x->hi)
      x->hi = y->hi;
    *y = *x;
    break;
  case TOK_NE:
    for (int k = 0; k < 2; k++)
    {
      AbIv *u = k ? y : x, *v = k ? x : y;
      if (v->lo != v->hi)
        continue;
      if (u->lo == v->lo && u->hi == v->lo)
        return 0;
      if (u->lo == v->lo)
        u->lo++;
      else if (u->hi == v->lo)
        u->hi--;
    }
    break;
  }
  return x->lo <= x->hi && y->lo <= y->hi;
}

static int ab_negate(int cond)
{
  switch (cond)
  {
  case TOK_LT: return TOK_GE;
  case TOK_GE: return TOK_LT;
  case TOK_LE: return TOK_GT;
  case TOK_GT: return TOK_LE;
  case TOK_EQ: return TOK_NE;
  case TOK_NE: return TOK_EQ;
  case TOK_ULT: return TOK_UGE;
  case TOK_UGE: return TOK_ULT;
  case TOK_ULE: return TOK_UGT;
  default: return TOK_ULE; /* TOK_UGT */
  }
}

/* narrow state st by `x COND y` (the comparison at ci); 0: infeasible */
static int ab_narrow(AbCtx *c, AbIv *st, int from, int ci, int cond)
{
  TCCIRState *ir = c->ir;
  IRQuadCompact *tq = &ir->compact_instructions[ci];
  IROperand xo = tcc_ir_op_get_src1(ir, tq);
  IROperand yo = tq->op == TCCIR_OP_CMP ? tcc_ir_op_get_src2(ir, tq) : irop_make_imm32(-1, 0, IROP_BTYPE_INT32);
  AbIv x = ab_val(c, st, xo), y = ab_val(c, st, yo);
  if (!ab_constrain(&x, cond, &y))
    return 0;
  for (int k = 0; k < 2; k++)
  {
    IROperand op = k ? yo : xo;
    AbIv v = k ? y : x;
    int32_t r = ab_value_vreg(op);
    int s = ab_slot(c, r), sv = ab_slot(c, ab_copy_of(c, from, ci, r));
    if (s >= 0)
      st[s] = v;
    if (sv >= 0)
    {
      AbIv *w = &st[sv];
      if (v.lo > w->lo)
        w->lo = v.lo;
      if (v.hi < w->hi)
        w->hi = v.hi;
      if (w->lo > w->hi)
        return 0;
    }
  }
  return 1;
}

/* ---- the walk ----------------------------------------------------------- */

static void ab_join(AbIv *dst, uint8_t *reached, const AbIv *src, int nt)
{
  if (!*reached)
  {
    memcpy(dst, src, nt * sizeof(AbIv));
    *reached = 1;
    return;
  }
  for (int s = 0; s < nt; s++)
  {
    if (src[s].lo < dst[s].lo)
      dst[s].lo = src[s].lo;
    if (src[s].hi > dst[s].hi)
      dst[s].hi = src[s].hi;
  }
}

/* state st leaves along an edge to block `to` (-1: out of the function) */
static void ab_edge(AbCtx *c, int to, const AbIv *st)
{
  int drop = 0;
  for (int k = c->nact - 1; k >= 0; k--)
  {
    AbLoop *l = &c->loops[c->act[k]];
    if (to >= 0 && to == l->head)
    {
      if (!drop)
        ab_join(l->back, &l->back_reach, st, c->nt);
      return;
    }
    if (to >= 0 && ab_in_loop(c, c->act[k], to))
      break;
    l->has_exit = 1;
    if (l->quiet)
      drop = 1;
  }
  if (!drop && to >= 0)
    ab_join(&c->in[(size_t)to * c->nt], &c->reach[to], st, c->nt);
}

/* subscript s, in the current state */
static void ab_check(AbCtx *c, int si)
{
  IRAbSite *s = &c->sites[si];
  AbHit *h = &c->hits[si];
  int slot = ab_slot(c, s->vreg);
  AbIv v = slot >= 0 ? c->cur[slot] : AB_TOP;
  int64_t lo = v.lo, hi = v.hi, max = (int64_t)s->n - 1 + s->one_past;
  int side;
  if (s->is_unsigned)
  {
    if (v.hi < 0)
      lo += (int64_t)1 << 32, hi += (int64_t)1 << 32;
    else if (v.lo < 0)
      return;
  }
  if (lo > max)
    side = 1;
  else if (hi < 0)
    side = 2;
  else
    return;
  if (!h->side)
  {
    h->side = side, h->lo = lo, h->hi = hi, h->first = lo;
  }
  else if (h->side == side)
  {
    if (lo < h->lo)
      h->lo = lo;
    if (hi > h->hi)
      h->hi = hi;
  }
  else
    return;
  if (lo != hi)
    h->ranges = 1;
}

static void ab_block(AbCtx *c, int b)
{
  TCCIRState *ir = c->ir;
  IRBasicBlock *bb = &c->cfg->blocks[b];
  int si = c->site_first[b], last = bb->end_idx - 1, cond, ci;
  if ((c->visits += bb->end_idx - bb->start_idx) > AB_BUDGET)
  {
    c->abort = 1;
    return;
  }
  memcpy(c->cur, &c->in[(size_t)b * c->nt], c->nt * sizeof(AbIv));
  for (int i = bb->start_idx; i < bb->end_idx; i++)
  {
    for (; si < c->nsites && c->sites[si].start == i; si++)
      if (c->checking)
        ab_check(c, si);
    ab_instr(c, i);
  }
  IRQuadCompact *lq = &ir->compact_instructions[last];
  int next = bb->end_idx < c->n ? c->cfg->instr_to_block[bb->end_idx] : -1;
  if (lq->op == TCCIR_OP_JUMP)
  {
    int t = (int)tcc_ir_op_dest_imm(ir, lq);
    ab_edge(c, t >= 0 && t < c->n ? c->cfg->instr_to_block[t] : -1, c->cur);
  }
  else if (lq->op == TCCIR_OP_JUMPIF)
  {
    int t = (int)tcc_ir_op_dest_imm(ir, lq);
    int tb = t >= 0 && t < c->n ? c->cfg->instr_to_block[t] : -1;
    if ((ci = ab_cond(c, bb, &cond)) < 0)
    {
      ab_edge(c, tb, c->cur);
      ab_edge(c, next, c->cur);
      return;
    }
    memcpy(c->tmp, c->cur, c->nt * sizeof(AbIv));
    if (ab_narrow(c, c->tmp, bb->start_idx, ci, cond))
      ab_edge(c, tb, c->tmp);
    memcpy(c->tmp, c->cur, c->nt * sizeof(AbIv));
    if (ab_narrow(c, c->tmp, bb->start_idx, ci, ab_negate(cond)))
      ab_edge(c, next, c->tmp);
  }
  else if (lq->op == TCCIR_OP_RETURNVALUE || lq->op == TCCIR_OP_RETURNVOID)
    ab_edge(c, -1, c->cur);
  else if (tcc_ir_cfg_instr_ends_flow(ir, lq))
  {
    /* a noreturn call or a trap: the program may stop in this iteration
       (inside an inlined body it does not count, as the call it is) */
    if (!c->inl[last])
      for (int k = 0; k < c->nact; k++)
        c->loops[c->act[k]].has_exit = 1;
  }
  else
    ab_edge(c, next, c->cur);
}

static void ab_loop(AbCtx *c, int l);

/* the blocks of loop l (-1: the function) once, from their in-states */
static void ab_walk(AbCtx *c, int l)
{
  int nitems = l < 0 ? c->ntop : c->loops[l].nitems, *items = l < 0 ? c->top : c->loops[l].items;
  for (int k = 0; k < nitems && !c->abort; k++)
  {
    int b = items[k];
    if (!c->reach[b])
      continue;
    if (c->head_loop[b] >= 0 && c->head_loop[b] != l)
      ab_loop(c, c->head_loop[b]);
    else
      ab_block(c, b);
  }
}

/* one pass over loop l from header state h: what comes back in l->back */
static void ab_pass(AbCtx *c, AbLoop *l, const AbIv *h)
{
  for (int k = 0; k < l->nbody; k++)
    c->reach[l->body[k]] = 0;
  memcpy(&c->in[(size_t)l->head * c->nt], h, c->nt * sizeof(AbIv));
  c->reach[l->head] = 1;
  l->back_reach = 0;
  l->has_exit = 0;
  ab_walk(c, (int)(l - c->loops));
}

static int ab_leq(const AbIv *a, const AbIv *b, int nt)
{
  for (int s = 0; s < nt; s++)
    if (a[s].lo < b[s].lo || a[s].hi > b[s].hi)
      return 0;
  return 1;
}

static void ab_loop(AbCtx *c, int li)
{
  AbLoop *l = &c->loops[li];
  const int nt = c->nt;
  const size_t sz = nt * sizeof(AbIv);
  AbIv *h = tcc_malloc(3 * sz), *h0 = h + nt, *nh = h + 2 * nt;
  memcpy(h, &c->in[(size_t)l->head * nt], sz);
  l->back = tcc_malloc(sz);
  l->active = 1;
  l->quiet = 0;
  c->act[c->nact++] = li;

  /* definite iterations, one by one */
  int iter = 0;
  if (l->has_site)
    for (; iter < AB_UNROLL && !c->abort; iter++)
    {
      ab_pass(c, l, h);
      if (!l->back_reach)
        goto done; /* it ended */
      memcpy(h, l->back, sz);
      if (l->has_exit)
        break; /* the next trip may not happen */
    }
  if (c->abort)
    goto done;

  /* the rest: widen to a post-fixpoint, narrow, then the final pass */
  {
    const int saved_checking = c->checking;
    memcpy(h0, h, sz);
    c->checking = 0;
    l->quiet = 1;
    for (int k = 0; !c->abort; k++)
    {
      ab_pass(c, l, h);
      memcpy(nh, h0, sz);
      if (l->back_reach)
        for (int s = 0; s < nt; s++)
        {
          if (l->back[s].lo < nh[s].lo)
            nh[s].lo = l->back[s].lo;
          if (l->back[s].hi > nh[s].hi)
            nh[s].hi = l->back[s].hi;
        }
      if (ab_leq(nh, h, nt))
        break;
      for (int s = 0; s < nt; s++)
      {
        if (nh[s].lo < h[s].lo)
          h[s].lo = k < 2 ? nh[s].lo : nh[s].lo >= 0 && h[s].lo >= 0 ? 0 : INT32_MIN;
        if (nh[s].hi > h[s].hi)
          h[s].hi = k < 2 ? nh[s].hi : nh[s].hi < 0 && h[s].hi < 0 ? -1 : INT32_MAX;
      }
    }
    for (int k = 0; k < 2 && !c->abort; k++)
    {
      ab_pass(c, l, h);
      memcpy(nh, h0, sz);
      if (l->back_reach)
        for (int s = 0; s < nt; s++)
        {
          if (l->back[s].lo < nh[s].lo)
            nh[s].lo = l->back[s].lo;
          if (l->back[s].hi > nh[s].hi)
            nh[s].hi = l->back[s].hi;
        }
      for (int s = 0; s < nt; s++)
      {
        if (nh[s].lo > h[s].lo)
          h[s].lo = nh[s].lo;
        if (nh[s].hi < h[s].hi)
          h[s].hi = nh[s].hi;
      }
    }
    /* the final pass lets states out, and checks nothing: the trips it
       stands for happen only for some values of what is unknown (`j < n`
       past the trips the header decided), so even a range entirely out of
       bounds there is not proven on a path that is known to run */
    l->quiet = 0;
    if (!c->abort)
      ab_pass(c, l, h);
    c->checking = saved_checking;
  }
done:
  c->nact--;
  l->active = 0;
  tcc_free(l->back);
  l->back = NULL;
  tcc_free(h);
}

/* ---- setup -------------------------------------------------------------- */

/* the natural loops: 0, or -1 when the CFG is irreducible or too big */
static int ab_find_loops(AbCtx *c)
{
  IRCFG *cfg = c->cfg;
  const int nb = c->nb;
  int *stack = tcc_malloc(nb * sizeof(int));
  c->head_loop = tcc_malloc(nb * sizeof(int));
  c->inner = tcc_malloc(nb * sizeof(int));
  for (int b = 0; b < nb; b++)
    c->head_loop[b] = c->inner[b] = -1;
  c->bwords = (nb + 31) / 32;
  for (int r = 0; r < cfg->rpo_count; r++)
  {
    int u = cfg->rpo_order[r];
    IRBasicBlock *ub = &cfg->blocks[u];
    for (int k = 0; k < ub->num_succs; k++)
    {
      int h = ub->succs[k], l;
      if (cfg->blocks[h].rpo_number < 0 || cfg->blocks[h].rpo_number > r)
        continue;
      if (!tcc_ir_cfg_dominates(cfg, h, u))
        goto fail; /* irreducible */
      if ((l = c->head_loop[h]) < 0)
      {
        if (c->nloops >= AB_MAX_LOOPS)
          goto fail;
        l = c->head_loop[h] = c->nloops++;
        c->loops = tcc_realloc(c->loops, c->nloops * sizeof(AbLoop));
        c->bits = tcc_realloc(c->bits, (size_t)c->nloops * c->bwords * sizeof(uint32_t));
        memset(&c->loops[l], 0, sizeof(AbLoop));
        memset(&c->bits[(size_t)l * c->bwords], 0, c->bwords * sizeof(uint32_t));
        c->loops[l].head = h;
        c->bits[(size_t)l * c->bwords + (h >> 5)] |= 1u << (h & 31);
      }
      /* the blocks that reach the latch u without passing h */
      int sp = 0;
      uint32_t *bits = &c->bits[(size_t)l * c->bwords];
      if (!((bits[u >> 5] >> (u & 31)) & 1))
      {
        bits[u >> 5] |= 1u << (u & 31);
        stack[sp++] = u;
      }
      while (sp)
      {
        IRBasicBlock *xb = &cfg->blocks[stack[--sp]];
        for (int p = 0; p < xb->num_preds; p++)
        {
          int y = xb->preds[p];
          if (cfg->blocks[y].rpo_number < 0 || ((bits[y >> 5] >> (y & 31)) & 1))
            continue;
          bits[y >> 5] |= 1u << (y & 31);
          stack[sp++] = y;
        }
      }
    }
  }
  /* sizes, then the nesting: bigger loops first */
  int *order = tcc_malloc((c->nloops + 1) * sizeof(int));
  for (int l = 0; l < c->nloops; l++)
  {
    AbLoop *lp = &c->loops[l];
    lp->body = tcc_malloc(nb * sizeof(int));
    for (int r = 0; r < cfg->rpo_count; r++)
      if (ab_in_loop(c, l, cfg->rpo_order[r]))
        lp->body[lp->nbody++] = cfg->rpo_order[r];
    order[l] = l;
  }
  for (int i = 1; i < c->nloops; i++)
    for (int j = i; j > 0 && c->loops[order[j]].nbody > c->loops[order[j - 1]].nbody; j--)
    {
      int t = order[j];
      order[j] = order[j - 1], order[j - 1] = t;
    }
  for (int i = 0; i < c->nloops; i++)
  {
    AbLoop *lp = &c->loops[order[i]];
    lp->parent = c->inner[lp->head];
    for (int k = 0; k < lp->nbody; k++)
      c->inner[lp->body[k]] = order[i];
  }
  tcc_free(order);
  /* each loop's own items, and the function's, in RPO */
  c->top = tcc_malloc(nb * sizeof(int));
  for (int l = 0; l < c->nloops; l++)
    c->loops[l].items = tcc_malloc(c->loops[l].nbody * sizeof(int));
  for (int r = 0; r < cfg->rpo_count; r++)
  {
    int b = cfg->rpo_order[r], l = c->inner[b];
    int owner = c->head_loop[b] >= 0 ? c->loops[c->head_loop[b]].parent : l;
    if (c->head_loop[b] >= 0)
    {
      /* the header: an item of its loop (walked there) and of the parent */
      AbLoop *self = &c->loops[c->head_loop[b]];
      self->items[self->nitems++] = b;
    }
    if (owner < 0)
      c->top[c->ntop++] = b;
    else if (c->head_loop[b] >= 0 || l == owner)
      c->loops[owner].items[c->loops[owner].nitems++] = b;
  }
  tcc_free(stack);
  return 0;
fail:
  tcc_free(stack);
  return -1;
}

static int ab_site_cmp(const void *a, const void *b)
{
  const IRAbSite *x = a, *y = b;
  return x->start < y->start ? -1 : x->start > y->start;
}

/* a recorded subscript the IR still has: its address computation reads
   the index */
static int ab_site_valid(AbCtx *c, IRAbSite *s)
{
  TCCIRState *ir = c->ir;
  if (!s->done || s->start >= s->end || s->end > c->n || c->inl[s->start])
    return 0;
  for (int i = s->start; i < s->end; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;
    if (ab_value_vreg(tcc_ir_op_get_src1(ir, q)) == s->vreg || ab_value_vreg(tcc_ir_op_get_src2(ir, q)) == s->vreg)
      return 1;
  }
  return 0;
}

static void ab_report(AbCtx *c)
{
  for (int i = 0; i < c->nsites; i++)
  {
    IRAbSite *s = &c->sites[i];
    AbHit *h = &c->hits[i];
    int said = 0;
    if (!h->side)
      continue;
    for (int k = 0; k < c->ir->ab_nb_said && !said; k++)
      said = h->side == 1 && !h->ranges && c->ir->ab_said[2 * k] == s->line && c->ir->ab_said[2 * k + 1] >= h->lo &&
             c->ir->ab_said[2 * k + 1] <= h->hi;
    if (said)
      continue;
    if (h->ranges)
      tcc_warning_once_at(warn_array_bounds, s->line, "array subscript [%lld, %lld] is outside array bounds of '%s'",
                          (long long)h->lo, (long long)h->hi, s->type);
    else
      tcc_warning_once_at(warn_array_bounds, s->line, "array subscript %lld is %s array bounds of '%s'",
                          (long long)h->first, h->side == 1 ? "above" : "below", s->type);
    if (h->lo == h->hi)
      tcc_note("%i:the subscript is %lld on a path that reaches it", s->line, (long long)h->lo);
    else
      tcc_note("%i:the subscript is in [%lld, %lld] on the paths that reach it", s->line, (long long)h->lo,
               (long long)h->hi);
  }
}

void tcc_ir_ab_check(TCCIRState *ir)
{
  AbCtx c;
  const int n = ir->next_instruction_index;
  memset(&c, 0, sizeof c);
  c.ir = ir;
  c.n = n;
  c.sites = ir->ab_sites;
  c.nsites = ir->ab_nb_sites;
  if (!c.nsites || !n)
    goto out;
  for (int i = 0; i < n; i++)
  {
    int op = ir->compact_instructions[i].op;
    if (op == TCCIR_OP_IJUMP || op == TCCIR_OP_SETJMP || op == TCCIR_OP_NL_SETJMP || op == TCCIR_OP_SWITCH_TABLE ||
        (op == TCCIR_OP_INLINE_ASM && tcc_ir_asm_goto_target(ir, &ir->compact_instructions[i], 0) >= 0))
      goto out;
  }
  c.inl = tcc_mallocz(n);
  for (int k = 0; k < ir->ab_nb_inl; k++)
    for (int i = ir->ab_inl[2 * k]; i < ir->ab_inl[2 * k + 1] && i < n; i++)
      c.inl[i] = 1;
  /* the subscripts that are still there, in order */
  tcc_qsort(c.sites, c.nsites, sizeof(IRAbSite), ab_site_cmp);
  int ns = 0;
  for (int i = 0; i < c.nsites; i++)
    if (ab_site_valid(&c, &c.sites[i]))
      c.sites[ns++] = c.sites[i];
  if (!(c.nsites = ns))
    goto out;

  c.cfg = tcc_ir_cfg_build(ir);
  if (!c.cfg || (c.nb = c.cfg->num_blocks) > AB_MAX_BLOCKS)
    goto out;
  tcc_ir_cfg_compute_dominators(c.cfg);
  if (ab_find_loops(&c) < 0)
    goto out;

  c.mapn[TCCIR_VREG_TYPE_VAR] = ir->next_local_variable;
  c.mapn[TCCIR_VREG_TYPE_TEMP] = ir->next_temporary_variable;
  c.mapn[TCCIR_VREG_TYPE_PARAM] = ir->next_parameter;
  for (int t = TCCIR_VREG_TYPE_VAR; t <= TCCIR_VREG_TYPE_PARAM; t++)
  {
    c.map[t] = tcc_malloc((c.mapn[t] + 1) * sizeof(int));
    for (int p = 0; p < c.mapn[t]; p++)
      c.map[t][p] = -1;
  }
  ab_slice(&c);
  if (!c.nt || (size_t)c.nt * c.nb > AB_MAX_CELLS)
    goto out;

  c.in = tcc_malloc((size_t)c.nb * c.nt * sizeof(AbIv));
  c.reach = tcc_mallocz(c.nb);
  c.cur = tcc_malloc(2 * c.nt * sizeof(AbIv));
  c.tmp = c.cur + c.nt;
  c.hits = tcc_mallocz(c.nsites * sizeof(AbHit));
  c.site_first = tcc_malloc(c.nb * sizeof(int));
  for (int b = 0, si = 0; b < c.nb; b++)
  {
    while (si < c.nsites && c.sites[si].start < c.cfg->blocks[b].start_idx)
      si++;
    c.site_first[b] = si;
  }
  for (int i = 0; i < c.nsites; i++)
  {
    int b = c.cfg->instr_to_block[c.sites[i].start];
    for (int l = c.inner[b]; l >= 0; l = c.loops[l].parent)
      c.loops[l].has_site = 1;
  }
  for (int s = 0; s < c.nt; s++)
    c.in[s] = AB_TOP;
  c.reach[0] = 1;
  c.checking = 1;
  ab_walk(&c, -1);
  ab_report(&c);

out:
  tcc_free(c.inl);
  for (int t = 0; t < 4; t++)
    tcc_free(c.map[t]);
  for (int l = 0; l < c.nloops; l++)
  {
    tcc_free(c.loops[l].body);
    tcc_free(c.loops[l].items);
  }
  tcc_free(c.loops);
  tcc_free(c.bits);
  tcc_free(c.head_loop);
  tcc_free(c.inner);
  tcc_free(c.top);
  tcc_free(c.in);
  tcc_free(c.reach);
  tcc_free(c.cur);
  tcc_free(c.hits);
  tcc_free(c.site_first);
  if (c.cfg)
    tcc_ir_cfg_free(c.cfg);
  tcc_ir_ab_free(ir);
}
