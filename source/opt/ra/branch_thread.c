/*
 *  TCC IR - Jump threading over known values, after phi resolution
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 */

/*
 * A Zig `try` is a test of the error a call returned, and an inlined callee's
 * `return err` is a phi at its exit that the caller tests again:
 *
 *      T115 <- T114 AND #65535           T193 <- #0          (success path)
 *      TEST_ZERO T115                    ...
 *      JMP to OK if "=="            J:   T179 <- T193 [ASSIGN]
 *      T193 <- T115 [ASSIGN]             TEST_ZERO T179
 *      JMP to J                          JMP to Z if "=="
 *                                   F:   ...error return, reads T179
 *
 * On every edge into J the outcome of J's test is already known: T193 is the
 * constant 0 on one, and on the other it is T115, which the branch that
 * entered the block has just found non-zero.  Such an edge is redirected past
 * J to the successor the test would pick, taking with it J's copies that are
 * still read there.  When every edge threads J is unreachable and goes, and
 * with it the constant moves nothing reads any more (dead_def).
 *
 * Runs on the de-SSA'd IR (phis are explicit copies at predecessor ends),
 * after the last IR rewrite and before live intervals.  An edge threads when
 * the tested value is traced back, through copies of the same width, to a
 * constant or to the value an entry branch of a single-entry block tested.
 * J may hold only copies before its test, and the successor it reaches must
 * not start by reading flags J's compare would have set.
 *
 * A fall-through edge costs a new jump, so it threads only when it lets J go
 * or its constant move go with it; a jump edge is just retargeted.
 */

#define USING_GLOBALS
#include "ir.h"
#include "regalloc.h"
#include "opt_utils.h"

#define BT_MAX_COPIES 4
#define BT_MAX_WALK 64

TCC_DBG_ENV_FLAG(bt_stats_on, "TCC_BT_STATS")
static long bt_stat[16];
static int bt_why;
static long bt_whyc[400];
TCC_DBG_ENV_FLAG(bt_debug_on, "TCC_BT_DEBUG")

typedef struct BTJoin
{
  int start;                      /* first instruction of J */
  int ncopies;
  int copy_idx[BT_MAX_COPIES];
  int test_idx, jumpif_idx;
  int32_t tested;                 /* vreg the test reads, as it enters J */
  int bits, minbits, uniform;     /* widths along J's copies */
  int is_cmp;                     /* CMP x,#imm (else TEST_ZERO x) */
  int64_t imm;
  int tok;
  int taken, fall;                /* successors */
} BTJoin;

/* What is known about the tested value on one edge. */
enum { BT_UNKNOWN, BT_CONST, BT_EQ, BT_NE, BT_SETIF };

static int bt_eval(int64_t v1, int64_t v2, int tok)
{
  switch (tok)
  {
  case 0x94: return v1 == v2;
  case 0x95: return v1 != v2;
  case 0x9c: return v1 < v2;
  case 0x9d: return v1 >= v2;
  case 0x9e: return v1 <= v2;
  case 0x9f: return v1 > v2;
  case 0x92: return (uint64_t)v1 < (uint64_t)v2;
  case 0x93: return (uint64_t)v1 >= (uint64_t)v2;
  case 0x96: return (uint64_t)v1 <= (uint64_t)v2;
  case 0x97: return (uint64_t)v1 > (uint64_t)v2;
  default: return -1;
  }
}

static int bt_is_uncond(int op)
{
  switch (op)
  {
  case TCCIR_OP_JUMP:
  case TCCIR_OP_IJUMP:
  case TCCIR_OP_SWITCH_TABLE:
  case TCCIR_OP_RETURNVALUE:
  case TCCIR_OP_RETURNVOID:
  case TCCIR_OP_TRAP:
  case TCCIR_OP_LONGJMP:
  case TCCIR_OP_NL_LONGJMP:
  case TCCIR_OP_BUILTIN_RETURN:
    return 1;
  default:
    return 0;
  }
}

static int bt_reads_flags(int op)
{
  return op == TCCIR_OP_JUMPIF || op == TCCIR_OP_SETIF || op == TCCIR_OP_SELECT;
}

static int bt_sets_flags(int op)
{
  return op == TCCIR_OP_CMP || op == TCCIR_OP_TEST_ZERO || op == TCCIR_OP_FCMP || op == TCCIR_OP_FUNCCALLVAL ||
         op == TCCIR_OP_FUNCCALLVOID;
}

static int bt_has_slot3(int op)
{
  return tcc_ir_op_is_mac(op) || op == TCCIR_OP_LOAD_INDEXED || op == TCCIR_OP_STORE_INDEXED || op == TCCIR_OP_SELECT;
}

static IROperand bt_slot(TCCIRState *ir, IRQuadCompact *q, int s)
{
  if (s == 0 && irop_config[q->op].has_dest)
    return tcc_ir_op_get_dest(ir, q);
  if (s == 1 && irop_config[q->op].has_src1)
    return tcc_ir_op_get_src1(ir, q);
  if (s == 2 && irop_config[q->op].has_src2)
    return tcc_ir_op_get_src2(ir, q);
  if (s == 3 && bt_has_slot3(q->op))
    return ir->iroperand_pool[q->operand_base + 3];
  return IROP_NONE;
}

static int bt_annotated(TCCIRState *ir, IRQuadCompact *q)
{
  return tcc_ir_barrel_shift_at(ir, q) || tcc_ir_shift64_dead_half_at(ir, q) || tcc_ir_zero_half64_at(ir, q) ||
         tcc_ir_bfi_params_at(ir, q);
}

/* A plain register TEMP of a scalar width up to 32 bits. */
static int bt_scalar_temp(IROperand o)
{
  int32_t vr = irop_get_vreg(o);
  if (vr < 0 || irop_get_tag(o) != IROP_TAG_VREG || o.is_lval || o.is_llocal || o.is_local ||
      TCCIR_DECODE_VREG_TYPE(vr) != TCCIR_VREG_TYPE_TEMP)
    return 0;
  int bt = irop_get_btype(o);
  return bt == IROP_BTYPE_INT32 || bt == IROP_BTYPE_INT16 || bt == IROP_BTYPE_INT8;
}

static int bt_bits(IROperand o)
{
  int bt = irop_get_btype(o);
  return bt == IROP_BTYPE_INT8 ? 8 : bt == IROP_BTYPE_INT16 ? 16 : 32;
}

/* Widths a value passes through on its way to J's test. */
typedef struct BTWidth
{
  int bits;    /* the tested operand's */
  int minbits; /* narrowest on the way */
  int uniform; /* all the same */
} BTWidth;

static void bt_width_add(BTWidth *w, IROperand o)
{
  int b = bt_bits(o);
  if (b != w->bits)
    w->uniform = 0;
  if (b < w->minbits)
    w->minbits = b;
}

static int bt_imm(TCCIRState *ir, IROperand o, int64_t *v)
{
  if (irop_get_tag(o) != IROP_TAG_IMM32 || o.is_lval || o.is_sym)
    return 0;
  *v = (int64_t)(int32_t)irop_get_imm64_ex(ir, o);
  return 1;
}

/* `d <- s` between two TEMPs, or `d <- #k`. */
static int bt_copy(TCCIRState *ir, IRQuadCompact *q, int32_t *d, int32_t *s, int64_t *k, int *is_const)
{
  if (q->op != TCCIR_OP_ASSIGN || bt_annotated(ir, q))
    return 0;
  IROperand dst = tcc_ir_op_get_dest(ir, q), src = tcc_ir_op_get_src1(ir, q);
  if (!bt_scalar_temp(dst))
    return 0;
  *d = irop_get_vreg(dst);
  if (bt_imm(ir, src, k))
  {
    *is_const = 1;
    return 1;
  }
  if (!bt_scalar_temp(src))
    return 0;
  *is_const = 0;
  *s = irop_get_vreg(src);
  return 1;
}

/* Does q name vreg vr in any operand slot? */
static int bt_names(TCCIRState *ir, IRQuadCompact *q, int32_t vr)
{
  for (int s = 0; s < 4; s++)
    if (irop_get_vreg(bt_slot(ir, q, s)) == vr)
      return 1;
  return 0;
}

/* Does q write vr as its plain destination? */
static int bt_defines(TCCIRState *ir, IRQuadCompact *q, int32_t vr)
{
  if (!irop_config[q->op].has_dest)
    return 0;
  return !tcc_ir_op_dest_is_lval(ir, q) && tcc_ir_op_dest_vreg(ir, q) == vr;
}

typedef struct BTState
{
  TCCIRState *ir;
  int n;
  int *refs;       /* branches naming each index */
  int *ref_from;   /* the last of them */
  int *next_real;  /* first non-NOP at or after i (n if none) */
  int *prev_real;  /* last non-NOP at or before i (-1 if none) */
  int *seen;       /* liveness walk stamps */
  int stamp;
  int *stack;
} BTState;

/* The value J's test reads, followed back through J's own copies to what it
 * is when J is entered. */
static int bt_parse_join(BTState *st, int start, BTJoin *j)
{
  TCCIRState *ir = st->ir;
  int i = start;
  j->start = start;
  j->ncopies = 0;
  for (;; i = st->next_real[i + 1])
  {
    if (i >= st->n)
      return (bt_why = 1009, 0);
    if (i != start && st->refs[i])
      return (bt_why = 1011, 0);
    IRQuadCompact *q = &ir->compact_instructions[i];
    int32_t d, s;
    int64_t k;
    int c;
    if (q->op == TCCIR_OP_ASSIGN)
    {
      if (j->ncopies >= BT_MAX_COPIES || !bt_copy(ir, q, &d, &s, &k, &c) || c)
      {
        if (bt_debug_on())
        {
          IROperand dd = tcc_ir_op_get_dest(ir, q), ss = tcc_ir_op_get_src1(ir, q);
          fprintf(stderr, "[bt] copy fail d tag%d bt%d lv%d ll%d loc%d vt%d | s tag%d bt%d lv%d ll%d loc%d vt%d ann%d\n",
                  irop_get_tag(dd), irop_get_btype(dd), dd.is_lval, dd.is_llocal, dd.is_local, dd.vreg_type,
                  irop_get_tag(ss), irop_get_btype(ss), ss.is_lval, ss.is_llocal, ss.is_local, ss.vreg_type, bt_annotated(ir, q));
        }
        return (bt_why = 1019, 0);
      }
      j->copy_idx[j->ncopies++] = i;
      continue;
    }
    if (q->op != TCCIR_OP_TEST_ZERO && q->op != TCCIR_OP_CMP)
      return (bt_why = 1024, 0);
    IROperand x = tcc_ir_op_get_src1(ir, q);
    if (!bt_scalar_temp(x))
      return (bt_why = 1027, 0);
    j->is_cmp = q->op == TCCIR_OP_CMP;
    j->imm = 0;
    if (j->is_cmp && !bt_imm(ir, tcc_ir_op_get_src2(ir, q), &j->imm))
      return (bt_why = 1031, 0);
    j->test_idx = i;
    j->tested = irop_get_vreg(x);
    j->bits = j->minbits = bt_bits(x);
    j->uniform = 1;
    break;
  }
  int ji = st->next_real[j->test_idx + 1];
  if (ji >= st->n || st->refs[ji] || ir->compact_instructions[ji].op != TCCIR_OP_JUMPIF)
    return (bt_why = 1039, 0);
  IRQuadCompact *jq = &ir->compact_instructions[ji];
  j->jumpif_idx = ji;
  j->tok = (int)tcc_ir_op_src1_imm(ir, jq);
  j->taken = (int)tcc_ir_op_dest_imm(ir, jq);
  j->fall = ji + 1;
  if (j->taken < 0 || j->taken > st->n)
    return (bt_why = 1046, 0);
  /* Back through J's copies (last to first) to J's entry. */
  for (int c = j->ncopies - 1; c >= 0; c--)
  {
    IRQuadCompact *q = &ir->compact_instructions[j->copy_idx[c]];
    if (bt_defines(ir, q, j->tested))
    {
      BTWidth w = {j->bits, j->minbits, j->uniform};
      bt_width_add(&w, tcc_ir_op_get_dest(ir, q));
      bt_width_add(&w, tcc_ir_op_get_src1(ir, q));
      j->minbits = w.minbits;
      j->uniform = w.uniform;
      j->tested = tcc_ir_op_src1_vreg(ir, q);
    }
  }
  return 1;
}

/* Ops that may change an operand other than a plain destination. */
static int bt_writes_operands(int op)
{
  return op == TCCIR_OP_LOAD_POSTINC || op == TCCIR_OP_STORE_POSTINC || op == TCCIR_OP_UMULL ||
         op == TCCIR_OP_ASM_INPUT || op == TCCIR_OP_ASM_OUTPUT || op == TCCIR_OP_INLINE_ASM;
}

/* How many low bits hold the value the TEMP vr has at `at`: an AND with a
 * small mask in the same straight run says; 32 otherwise. */
static int bt_value_bits(BTState *st, int at, int32_t vr)
{
  TCCIRState *ir = st->ir;
  /* A setter that is a jump target is reached by paths that skipped the run
   * above it. */
  if (st->refs[at])
    return 32;
  for (int i = at, walked = 0; i > 0 && walked < 16; walked++)
  {
    i = st->prev_real[i - 1];
    if (i < 0)
      break;
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (bt_is_uncond(q->op) || q->op == TCCIR_OP_JUMPIF)
      break;
    if (irop_config[q->op].has_dest && tcc_ir_op_dest_vreg(ir, q) == vr)
    {
      int64_t m;
      if (q->op != TCCIR_OP_AND || !bt_defines(ir, q, vr) || bt_annotated(ir, q) ||
          !bt_imm(ir, tcc_ir_op_get_src2(ir, q), &m) || m < 0)
        break;
      int bits = 0;
      while (bits < 32 && ((uint32_t)m >> bits))
        bits++;
      return bits;
    }
    if (bt_writes_operands(q->op) && bt_names(ir, q, vr))
      break;
    if (st->refs[i])
      break;
  }
  return 32;
}

/* Is vr a copy of x where the test at `at` reads x: `vr <- x` earlier in
 * the same straight run, neither written since?  The test itself may not be a
 * jump target either: a loop header that opens with the test is entered by the
 * back edge without running the copy, and vr holds a later value there. */
static int bt_copied_before(BTState *st, int at, int32_t vr, int32_t x, BTWidth *w)
{
  TCCIRState *ir = st->ir;
  for (int i = at, walked = 0; i > 0 && walked < 16; walked++)
  {
    if (st->refs[i])
      return 0;
    i = st->prev_real[i - 1];
    if (i < 0)
      return 0;
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (bt_is_uncond(q->op) || q->op == TCCIR_OP_JUMPIF)
      return 0;
    if (bt_writes_operands(q->op) && (bt_names(ir, q, vr) || bt_names(ir, q, x)))
      return 0;
    if (bt_defines(ir, q, x))
      return 0;
    if (bt_defines(ir, q, vr))
    {
      if (q->op != TCCIR_OP_ASSIGN || bt_annotated(ir, q) || tcc_ir_op_src1_vreg(ir, q) != x ||
          tcc_ir_op_src1_is_lval(ir, q))
        return 0;
      bt_width_add(w, tcc_ir_op_get_dest(ir, q));
      return 1;
    }
  }
  return 0;
}

/* The flag test feeding the JUMPIF at `jumpif`: what the branch says about
 * the TEMP vr on its taken (want_taken) or fall-through edge.  *fitbits: how
 * many low bits vr's value is known to fit. */
static int bt_branch_fact(BTState *st, int jumpif, int want_taken, int32_t vr, BTWidth *w, int64_t *c,
                          int *fitbits)
{
  TCCIRState *ir = st->ir;
  int setter = jumpif > 0 ? st->prev_real[jumpif - 1] : -1;
  if (setter < 0 || st->refs[jumpif])
    return (bt_why = 278, BT_UNKNOWN);
  IRQuadCompact *sq = &ir->compact_instructions[setter];
  if (sq->op != TCCIR_OP_TEST_ZERO && sq->op != TCCIR_OP_CMP)
    return (bt_why = 281, BT_UNKNOWN);
  IROperand x = tcc_ir_op_get_src1(ir, sq);
  if (!bt_scalar_temp(x))
    return (bt_why = 284, BT_UNKNOWN);
  if (irop_get_vreg(x) != vr && !bt_copied_before(st, setter, vr, irop_get_vreg(x), w))
    return (bt_why = 285, BT_UNKNOWN);
  bt_width_add(w, x);
  *c = 0;
  if (sq->op == TCCIR_OP_CMP && !bt_imm(ir, tcc_ir_op_get_src2(ir, sq), c))
    return (bt_why = 287, BT_UNKNOWN);
  *fitbits = bt_value_bits(st, setter, irop_get_vreg(x));
  int tok = (int)tcc_ir_op_src1_imm(ir, &ir->compact_instructions[jumpif]);
  if (tok == 0x94)
    return want_taken ? BT_EQ : BT_NE;
  if (tok == 0x95)
    return want_taken ? BT_NE : BT_EQ;
  return (bt_why = 293, BT_UNKNOWN);
}


/* The tested value where an edge leaves its predecessor: `end` is the jump
 * into J, or J's first instruction for the fall-through edge.  *def is the
 * constant move a BT_CONST came from. */
static int bt_edge_fact(BTState *st, int end, int32_t vr, BTWidth *w, int64_t *c, int *def, int *fitbits,
                        int *chain, int *nchain)
{
  TCCIRState *ir = st->ir;
  *def = -1;
  *fitbits = 32;
  *nchain = 0;
  if (end == 0)
    return (bt_why = 311, BT_UNKNOWN);
  int i = st->prev_real[end - 1];
  for (int walked = 0; i >= 0 && walked < BT_MAX_WALK; walked++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    /* `t <- SETIF cc` ending the edge, or followed only by copies of t: the
     * flags it read are still set, so J's test of t is a test of cc. */
    if (*nchain >= 0 && q->op == TCCIR_OP_SETIF && bt_defines(ir, q, vr) && !st->refs[i] && !bt_annotated(ir, q) &&
        i > 0)
    {
      int setter = st->prev_real[i - 1];
      int op = setter >= 0 ? ir->compact_instructions[setter].op : -1;
      *c = (int)tcc_ir_op_src1_imm(ir, q);
      if ((op == TCCIR_OP_CMP || op == TCCIR_OP_TEST_ZERO) && invert_cond_token((int)*c) >= 0 &&
          bt_scalar_temp(tcc_ir_op_get_dest(ir, q)))
      {
        *def = i;
        return BT_SETIF;
      }
    }
    if (bt_is_uncond(q->op))
    {
      /* Entered only by branches: one, a JUMPIF, whose taken edge tells. */
      int first = st->next_real[i + 1];
      if (first > end || st->refs[first] != 1)
        return (bt_why = 321, BT_UNKNOWN);
      int from = st->ref_from[first];
      if (ir->compact_instructions[from].op != TCCIR_OP_JUMPIF)
        return (bt_why = 324, BT_UNKNOWN);
      return bt_branch_fact(st, from, 1, vr, w, c, fitbits);
    }
    if (q->op == TCCIR_OP_JUMPIF)
      return bt_branch_fact(st, i, 0, vr, w, c, fitbits);
    if (q->op == TCCIR_OP_SWITCH_LOAD || q->op == TCCIR_OP_SETJMP || q->op == TCCIR_OP_NL_SETJMP)
      return (bt_why = 330, BT_UNKNOWN);
    if (bt_defines(ir, q, vr))
    {
      int32_t d, s;
      int is_const;
      if (!bt_copy(ir, q, &d, &s, c, &is_const))
      {
        if (bt_stats_on())
          bt_whyc[200 + (q->op < 190 ? q->op : 190)]++;
        return (bt_why = 336, BT_UNKNOWN);
      }
      bt_width_add(w, tcc_ir_op_get_dest(ir, q));
      if (is_const)
      {
        *def = i;
        return BT_CONST;
      }
      bt_width_add(w, tcc_ir_op_get_src1(ir, q));
      vr = s;
      if (*nchain >= 0 && *nchain < BT_MAX_COPIES)
        chain[(*nchain)++] = i;
      else
        *nchain = -1;
    }
    else
    {
      *nchain = -1;
      if (bt_writes_operands(q->op) && bt_names(ir, q, vr))
        return (bt_why = 345, BT_UNKNOWN);
    }
    if (st->refs[i])
      return (bt_why = 347, BT_UNKNOWN); /* other paths join here */
    if (i == 0)
      return (bt_why = 349, BT_UNKNOWN);
    i = st->prev_real[i - 1];
  }
  return (bt_why = 352, BT_UNKNOWN);
}

/* Which successor J's test picks given the fact (1 taken, 0 not); -1 when it
 * does not tell.  A compare reads the whole register.  Where a value passes
 * through different widths, or a narrow one, its register may have been
 * extended or truncated on the way: only equality that holds, or fails,
 * whatever happened to the bits above the narrowest width is decided. */
static int bt_decide(const BTJoin *j, const BTWidth *w, int fact, int64_t c, int fitbits, int *cc)
{
  int64_t imm = j->imm;
  int eqne = j->tok == 0x94 || j->tok == 0x95;
  if (fact == BT_SETIF)
  {
    /* The value is 1 where the flags satisfy c, else 0: 2 = J goes to its
     * taken successor exactly where they satisfy *cc. */
    if (!eqne)
      return -1;
    if ((int32_t)imm != 0 && (int32_t)imm != 1)
      return j->tok == 0x95;
    int taken_when_set = (j->tok == 0x94) == ((int32_t)imm == 1);
    *cc = taken_when_set ? (int)c : invert_cond_token((int)c);
    return *cc < 0 ? -1 : 2;
  }
  if (fact == BT_CONST || fact == BT_EQ)
  {
    if (w->uniform && (w->bits == 32 || fact == BT_EQ))
    {
      /* the register J tests holds exactly c */
      if (w->bits == 32)
        return bt_eval((int32_t)c, (int32_t)imm, j->tok);
      if (!eqne)
        return -1;
    }
    else if (!eqne)
      return -1;
    int eq;
    uint32_t mask = w->minbits >= 32 ? 0xffffffffu : (1u << w->minbits) - 1;
    if ((w->uniform && fact == BT_EQ) || (c >= 0 && c < ((int64_t)1 << (w->minbits - 1))))
      eq = (int32_t)c == (int32_t)imm;
    else if (((uint32_t)c & mask) != ((uint32_t)imm & mask))
      eq = 0;
    else
      return -1;
    return j->tok == 0x94 ? eq : !eq;
  }
  if (fact == BT_NE)
  {
    if (!eqne || (int32_t)c != (int32_t)imm)
      return -1;
    if (!w->uniform && !(c == 0 && fitbits <= w->minbits))
      return -1;
    return j->tok == 0x95;
  }
  return -1;
}

/* Successors of instruction i for the liveness walk; returns the count, -1
 * when control goes somewhere the walk cannot follow. */
static int bt_succs(BTState *st, int i, int *out)
{
  TCCIRState *ir = st->ir;
  IRQuadCompact *q = &ir->compact_instructions[i];
  switch (q->op)
  {
  case TCCIR_OP_JUMP:
    out[0] = (int)tcc_ir_op_dest_imm(ir, q);
    return 1;
  case TCCIR_OP_JUMPIF:
    out[0] = (int)tcc_ir_op_dest_imm(ir, q);
    out[1] = i + 1;
    return 2;
  case TCCIR_OP_RETURNVALUE:
  case TCCIR_OP_RETURNVOID:
  case TCCIR_OP_TRAP:
  case TCCIR_OP_BUILTIN_RETURN:
    return 0;
  case TCCIR_OP_IJUMP:
  case TCCIR_OP_SWITCH_TABLE:
  case TCCIR_OP_LONGJMP:
  case TCCIR_OP_NL_LONGJMP:
  case TCCIR_OP_SETJMP:
  case TCCIR_OP_NL_SETJMP:
  case TCCIR_OP_INLINE_ASM:
    return -1;
  default:
    out[0] = i + 1;
    return 1;
  }
}

/* Is vr read on some path from `from` before it is written whole? */
static int bt_live_at(BTState *st, int from, int32_t vr)
{
  TCCIRState *ir = st->ir;
  int sp = 0;
  st->stamp++;
  st->stack[sp++] = from;
  while (sp)
  {
    int i = st->stack[--sp];
    for (;;)
    {
      if (i >= st->n)
        break; /* the epilogue reads no TEMP */
      if (st->seen[i] == st->stamp)
        break;
      st->seen[i] = st->stamp;
      IRQuadCompact *q = &ir->compact_instructions[i];
      if (q->op != TCCIR_OP_NOP)
      {
        int reads = 0;
        for (int s = 1; s < 4 && !reads; s++)
          reads = irop_get_vreg(bt_slot(ir, q, s)) == vr;
        if (!reads && irop_config[q->op].has_dest && tcc_ir_op_dest_vreg(ir, q) == vr)
        {
          int whole = !tcc_ir_op_dest_is_lval(ir, q) && (q->op == TCCIR_OP_ASSIGN || q->op == TCCIR_OP_LOAD ||
                                     q->op == TCCIR_OP_FUNCCALLVAL || q->op == TCCIR_OP_ADD ||
                                     q->op == TCCIR_OP_SUB || q->op == TCCIR_OP_AND || q->op == TCCIR_OP_OR);
          if (!whole)
            return 1;
          break; /* killed on this path */
        }
        if (reads)
          return 1;
      }
      int succ[2];
      int ns = q->op == TCCIR_OP_NOP ? (succ[0] = i + 1, 1) : bt_succs(st, i, succ);
      if (ns < 0)
        return 1;
      if (ns == 0)
        break;
      if (ns == 2)
      {
        if (succ[1] < st->n && st->seen[succ[1]] != st->stamp)
          st->stack[sp++] = succ[1];
      }
      i = succ[0];
      if (i < 0)
        return 1;
    }
  }
  return 0;
}

/* The successor may not begin by reading the flags J's test would have set. */
static int bt_flags_safe(BTState *st, int s)
{
  TCCIRState *ir = st->ir;
  for (int i = s; i < st->n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;
    if (bt_reads_flags(q->op))
      return 0;
    if (bt_sets_flags(q->op) || bt_is_uncond(q->op))
      return 1;
  }
  return 1;
}


typedef struct BTInsert
{
  int before;    /* old index the instruction goes in front of */
  int seq;       /* queue order, kept among instructions at one point */
  int copy_from; /* J copy to duplicate, or -1 for a branch */
  int cond;      /* branch: -1 JUMP, else a JUMPIF on this condition */
  int jump_to;   /* branch: old index */
  int to_pad;    /* JUMP: to the instructions put in front of jump_to */
} BTInsert;

/* An old JUMP retargeted to the instructions put in front of `pad`. */
typedef struct BTPadJump
{
  int jump, pad;
} BTPadJump;

static int bt_ins_cmp(const void *a, const void *b)
{
  const BTInsert *x = a, *y = b;
  if (x->before != y->before)
    return x->before < y->before ? -1 : 1;
  return x->seq < y->seq ? -1 : x->seq > y->seq;
}

/* Put the queued instructions in, renumbering every branch target: a branch
 * to old index i lands on old instruction i, after anything put in front of
 * it; a pad jump lands on the first instruction put in front of its pad.
 * defs[] (old indices) are renumbered in place. */
static void bt_apply_inserts(TCCIRState *ir, BTInsert *ins, int nins, BTPadJump *pads, int npads, int *defs,
                             int ndefs)
{
  int n = ir->next_instruction_index;
  tcc_qsort(ins, nins, sizeof(BTInsert), bt_ins_cmp);
  /* at[i]: instructions put in front of old index i; shift[i]: in front of
   * old indices <= i */
  int *at = tcc_mallocz(sizeof(int) * (n + 1));
  int *shift = tcc_malloc(sizeof(int) * (n + 1));
  for (int k = 0; k < nins; k++)
    at[ins[k].before]++;
  for (int i = 0, acc = 0; i <= n; i++)
  {
    acc += at[i];
    shift[i] = acc;
  }
#define BT_MAP(old) ((old) + shift[old])
#define BT_PAD(old) ((old) + shift[old] - at[old])
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_JUMP && q->op != TCCIR_OP_JUMPIF)
      continue;
    int t = (int)tcc_ir_op_dest_imm(ir, q);
    if (t >= 0 && t <= n)
      tcc_ir_op_set_dest_imm32(ir, q, BT_MAP(t), IROP_BTYPE_INT32);
  }
  for (int k = 0; k < npads; k++)
    tcc_ir_op_set_dest_imm32(ir, &ir->compact_instructions[pads[k].jump], BT_PAD(pads[k].pad), IROP_BTYPE_INT32);
  for (int t = 0; t < ir->num_switch_tables; t++)
  {
    TCCIRSwitchTable *table = &ir->switch_tables[t];
    if (table->default_target >= 0 && table->default_target <= n)
      table->default_target = BT_MAP(table->default_target);
    for (int j = 0; table->targets && j < table->num_entries; j++)
      if (table->targets[j] >= 0 && table->targets[j] <= n)
        table->targets[j] = BT_MAP(table->targets[j]);
  }
  for (int k = 0; k < ndefs; k++)
    defs[k] = BT_MAP(defs[k]);
  int nn = n + nins;
  int size = ir->compact_instructions_size;
  while (size <= nn + 1)
    size <<= 1;
  IRQuadCompact *old = ir->compact_instructions;
  IRQuadCompact *nq = tcc_mallocz(sizeof(IRQuadCompact) * size);
  int w = 0, k = 0;
  for (int i = 0; i <= n; i++)
  {
    for (; k < nins && ins[k].before == i; k++)
    {
      IRQuadCompact *q = &nq[w++];
      q->operand_base = ir->iroperand_pool_count;
      if (ins[k].copy_from >= 0)
      {
        IRQuadCompact *src = &old[ins[k].copy_from];
        IROperand d = tcc_ir_op_get_dest(ir, src), s1 = tcc_ir_op_get_src1(ir, src);
        q->op = TCCIR_OP_ASSIGN;
        tcc_ir_pool_add(ir, d);
        tcc_ir_pool_add(ir, s1);
      }
      else
      {
        int t = ins[k].to_pad ? BT_PAD(ins[k].jump_to) : BT_MAP(ins[k].jump_to);
        q->op = ins[k].cond < 0 ? TCCIR_OP_JUMP : TCCIR_OP_JUMPIF;
        tcc_ir_pool_add(ir, irop_make_imm32(-1, t, IROP_BTYPE_INT32));
        if (ins[k].cond >= 0)
          tcc_ir_pool_add(ir, irop_make_imm32(-1, ins[k].cond, IROP_BTYPE_INT32));
      }
      q->orig_index = ++ir->max_orig_index;
      q->is_jump_target = 0;
    }
    if (i < n)
      nq[w++] = old[i];
  }
#undef BT_MAP
#undef BT_PAD
  tcc_free(old);
  ir->compact_instructions = nq;
  ir->compact_instructions_size = size;
  ir->next_instruction_index = nn;
  tcc_free(shift);
  tcc_free(at);
}

/* NOP what no path from the entry reaches; mark the targets of the branches
 * that remain. */
static int bt_sweep_unreachable(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  uint8_t *reach = tcc_mallocz(n + 1);
  int *stack = tcc_malloc(sizeof(int) * (n + 1));
  int removed = 0;
  IrReachList rl = {reach, stack, 0, n};
  reach[0] = 1;
  stack[rl.top++] = 0;
  while (rl.top)
  {
    int i = stack[--rl.top];
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_JUMPIF)
      ir_opt_reach_push(&rl, (int)tcc_ir_op_dest_imm(ir, q));
    if (q->op == TCCIR_OP_SWITCH_TABLE)
    {
      int id = (int)tcc_ir_op_src2_imm(ir, q);
      if (id >= 0 && id < ir->num_switch_tables)
      {
        TCCIRSwitchTable *table = &ir->switch_tables[id];
        ir_opt_reach_push(&rl, table->default_target);
        for (int j = 0; table->targets && j < table->num_entries; j++)
          ir_opt_reach_push(&rl, table->targets[j]);
      }
    }
    if (q->op == TCCIR_OP_NOP || !bt_is_uncond(q->op))
      ir_opt_reach_push(&rl, i + 1);
  }
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (!reach[i] && q->op != TCCIR_OP_NOP)
    {
      q->op = TCCIR_OP_NOP;
      removed++;
    }
  }
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_JUMPIF)
    {
      int t = (int)tcc_ir_op_dest_imm(ir, q);
      if (t >= 0 && t < n)
        ir->compact_instructions[t].is_jump_target = 1;
    }
  }
  tcc_free(stack);
  tcc_free(reach);
  return removed;
}

/* J's copies the path to s still needs, in order. */
static int bt_needed_copies(BTState *st, const BTJoin *j, int s, int *out)
{
  TCCIRState *ir = st->ir;
  int need[BT_MAX_COPIES] = {0};
  int32_t srcs[BT_MAX_COPIES];
  int nsrcs = 0;
  for (int c = j->ncopies - 1; c >= 0; c--)
  {
    IRQuadCompact *q = &ir->compact_instructions[j->copy_idx[c]];
    int32_t d = tcc_ir_op_dest_vreg(ir, q);
    int32_t sv = tcc_ir_op_src1_vreg(ir, q);
    int later_def = 0, read_later = 0;
    for (int l = c + 1; l < j->ncopies; l++)
      if (tcc_ir_op_dest_vreg(ir, &ir->compact_instructions[j->copy_idx[l]]) == d)
        later_def = 1;
    for (int l = 0; l < nsrcs; l++)
      if (srcs[l] == d)
        read_later = 1;
    if (read_later || (!later_def && bt_live_at(st, s, d)))
    {
      need[c] = 1;
      if (nsrcs < BT_MAX_COPIES)
        srcs[nsrcs++] = sv;
    }
  }
  int k = 0;
  for (int c = 0; c < j->ncopies; c++)
    if (need[c])
      out[k++] = j->copy_idx[c];
  return k;
}

/* Running J's copies twice gives what running them once does: no copy reads
 * what one of them writes. */
static int bt_copies_idempotent(TCCIRState *ir, const BTJoin *j)
{
  for (int a = 0; a < j->ncopies; a++)
  {
    int32_t s = tcc_ir_op_src1_vreg(ir, &ir->compact_instructions[j->copy_idx[a]]);
    for (int b = 0; b < j->ncopies; b++)
      if (tcc_ir_op_dest_vreg(ir, &ir->compact_instructions[j->copy_idx[b]]) == s)
        return 0;
  }
  return 1;
}

/* One edge into a join, and where it can go instead. */
typedef struct BTEdge
{
  int end;       /* the branch into J, or J's first instruction (fall-through) */
  int side;      /* 1: J's taken successor, 0: its fall-through, 2: taken where
                    the flags satisfy cc; -1 undecided */
  int cc;
  int def;       /* constant move or SETIF the fact came from, or -1 */
  int chain[BT_MAX_COPIES], nchain; /* SETIF: the copies of its value after it */
} BTEdge;

typedef struct BTQueue
{
  BTInsert *ins;
  int nins, cap;
  BTPadJump *pads;
  int npads, pcap;
  int *defs;
  int ndefs, dcap;
} BTQueue;

static void bt_q_insert(BTQueue *qu, int before, int copy_from, int cond, int jump_to, int to_pad)
{
  if (qu->nins >= qu->cap)
  {
    qu->cap = qu->cap * 2 + 16;
    qu->ins = tcc_realloc(qu->ins, sizeof(BTInsert) * qu->cap);
  }
  BTInsert *x = &qu->ins[qu->nins];
  x->before = before;
  x->seq = qu->nins++;
  x->copy_from = copy_from;
  x->cond = cond;
  x->jump_to = jump_to;
  x->to_pad = to_pad;
}

static void bt_q_pad(BTQueue *qu, int jump, int pad)
{
  if (qu->npads >= qu->pcap)
  {
    qu->pcap = qu->pcap * 2 + 16;
    qu->pads = tcc_realloc(qu->pads, sizeof(BTPadJump) * qu->pcap);
  }
  qu->pads[qu->npads].jump = jump;
  qu->pads[qu->npads++].pad = pad;
}

static void bt_q_def(BTQueue *qu, int def)
{
  if (qu->ndefs >= qu->dcap)
  {
    qu->dcap = qu->dcap * 2 + 16;
    qu->defs = tcc_realloc(qu->defs, sizeof(int) * qu->dcap);
  }
  qu->defs[qu->ndefs++] = def;
}

static void bt_state_init(BTState *st, TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  st->ir = ir;
  st->n = n;
  st->refs = tcc_mallocz(sizeof(int) * (n + 1));
  st->ref_from = tcc_malloc(sizeof(int) * (n + 1));
  st->next_real = tcc_malloc(sizeof(int) * (n + 2));
  st->prev_real = tcc_malloc(sizeof(int) * (n + 1));
  st->seen = tcc_mallocz(sizeof(int) * (n + 1));
  st->stack = tcc_malloc(sizeof(int) * (2 * n + 4));
  st->stamp = 0;
  st->next_real[n] = n;
  st->next_real[n + 1] = n;
  for (int i = n - 1; i >= 0; i--)
    st->next_real[i] = ir->compact_instructions[i].op == TCCIR_OP_NOP ? st->next_real[i + 1] : i;
  for (int i = 0, last = -1; i < n; i++)
  {
    if (ir->compact_instructions[i].op != TCCIR_OP_NOP)
      last = i;
    st->prev_real[i] = last;
  }
}

static void bt_state_free(BTState *st)
{
  tcc_free(st->stack);
  tcc_free(st->seen);
  tcc_free(st->prev_real);
  tcc_free(st->next_real);
  tcc_free(st->ref_from);
  tcc_free(st->refs);
}

static int bt_run(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  BTState st;
  bt_state_init(&st, ir);
  /* Jump edges by (normalized) target, CSR. */
  int *jcount = tcc_mallocz(sizeof(int) * (n + 2));
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_JUMPIF)
    {
      int t = (int)tcc_ir_op_dest_imm(ir, q);
      if (t < 0 || t > n)
        continue;
      t = st.next_real[t];
      st.refs[t]++;
      st.ref_from[t] = i;
      jcount[t + 1]++;
    }
    else if (q->op == TCCIR_OP_SWITCH_TABLE)
    {
      int id = (int)tcc_ir_op_src2_imm(ir, q);
      if (id < 0 || id >= ir->num_switch_tables)
        continue;
      TCCIRSwitchTable *table = &ir->switch_tables[id];
      for (int j = -1; j < table->num_entries; j++)
      {
        int t = j < 0 ? table->default_target : table->targets[j];
        if (t < 0 || t > n)
          continue;
        t = st.next_real[t];
        st.refs[t]++;
        st.ref_from[t] = i;
      }
    }
  }
  for (int i = 1; i <= n + 1; i++)
    jcount[i] += jcount[i - 1];
  int *jsrc = tcc_malloc(sizeof(int) * (jcount[n + 1] + 1));
  int *jfill = tcc_mallocz(sizeof(int) * (n + 1));
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_JUMP && q->op != TCCIR_OP_JUMPIF)
      continue;
    int t = (int)tcc_ir_op_dest_imm(ir, q);
    if (t < 0 || t > n)
      continue;
    t = st.next_real[t];
    jsrc[jcount[t] + jfill[t]++] = i;
  }

  BTQueue qu;
  memset(&qu, 0, sizeof qu);
  int threaded = 0;
  BTEdge *edges = NULL;
  int ecap = 0;
  for (int start = 0; start < n; start++)
  {
    if (!st.refs[start] || ir->compact_instructions[start].op == TCCIR_OP_NOP)
      continue;
    BTJoin j;
    if (!bt_parse_join(&st, start, &j))
    {
      if (bt_debug_on())
        fprintf(stderr, "[bt] J%d parse fail %d\n", start, bt_why);
      continue;
    }
    if (bt_stats_on())
      bt_stat[0]++;
    int njump = jcount[start + 1] - jcount[start];
    int prev = start > 0 ? st.prev_real[start - 1] : -1;
    int fallthrough = prev >= 0 && !bt_is_uncond(ir->compact_instructions[prev].op);
    int nedge = njump + fallthrough;
    int blocking = st.refs[start] - njump; /* switch table edges */
    if (nedge > ecap)
    {
      ecap = nedge * 2;
      edges = tcc_realloc(edges, sizeof(BTEdge) * ecap);
    }
    /* Per successor: J's copies still read there (-1 not computed yet, more
     * than BT_MAX_COPIES: cannot be placed).  They go in front of it, which
     * needs every other path into it to be a jump -- or J's own fall-through,
     * when a second run of them changes nothing. */
    int ncopy[2] = {-1, -1}, copy[2][BT_MAX_COPIES], succ[2] = {j.fall, j.taken}, ok[2] = {-1, -1};
    int decided = 0;
    for (int e = 0; e < nedge; e++)
    {
      BTEdge *ed = &edges[e];
      ed->end = e < njump ? jsrc[jcount[start] + e] : start;
      ed->side = -1;
      int is_cond = ed->end != start && ir->compact_instructions[ed->end].op == TCCIR_OP_JUMPIF;
      if (ed->end != start && !is_cond && st.refs[ed->end])
        continue; /* the jump is itself a join */
      int64_t c;
      BTWidth w = {j.bits, j.minbits, j.uniform};
      int fitbits = 32, fact = BT_UNKNOWN;
      ed->def = -1;
      ed->nchain = 0;
      if (is_cond)
        fact = bt_branch_fact(&st, ed->end, 1, j.tested, &w, &c, &fitbits);
      if (fact == BT_UNKNOWN)
      {
        w = (BTWidth){j.bits, j.minbits, j.uniform};
        fact = bt_edge_fact(&st, ed->end, j.tested, &w, &c, &ed->def, &fitbits, ed->chain, &ed->nchain);
        if (is_cond && fact == BT_SETIF)
          fact = BT_UNKNOWN;
      }
      if (bt_stats_on())
      {
        bt_stat[fact == BT_SETIF ? 11 : 1 + fact]++;
        if (fact == BT_UNKNOWN && bt_why < 400)
          bt_whyc[bt_why]++;
      }
      if (bt_debug_on())
        fprintf(stderr, "[bt] J%d edge %d fact %d why %d\n", start, ed->end, fact, bt_why);
      int cc = -1;
      int r = fact == BT_UNKNOWN ? -1 : bt_decide(&j, &w, fact, c, fitbits, &cc);
      if (r < 0)
        continue;
      int good = 1;
      for (int side = 0; side < 2 && good; side++)
      {
        if (r != 2 && r != side)
          continue;
        if (ok[side] < 0)
        {
          int s = succ[side];
          ok[side] = !(s >= j.start && s <= j.jumpif_idx) && bt_flags_safe(&st, s);
          if (ok[side])
          {
            ncopy[side] = bt_needed_copies(&st, &j, s, copy[side]);
            int sp = s > 0 ? st.prev_real[s - 1] : -1;
            if (ncopy[side] > 0 && !(s < n && (s == j.fall ? bt_copies_idempotent(ir, &j)
                                                           : (sp >= 0 && bt_is_uncond(ir->compact_instructions[sp].op)))))
              ok[side] = 0;
          }
        }
        good = ok[side];
      }
      if (!good)
        continue;
      if (r == 2)
      {
        /* The SETIF goes: only where nothing past the edge reads its value. */
        int live = 0;
        for (int k = -1; k < ed->nchain && !live; k++)
        {
          int at = k < 0 ? ed->def : ed->chain[k];
          int32_t t = tcc_ir_op_dest_vreg(ir, &ir->compact_instructions[at]);
          for (int side = 0; side < 2 && !live; side++)
          {
            live = bt_live_at(&st, succ[side], t);
            for (int m = 0; m < ncopy[side] && !live; m++)
              live = tcc_ir_op_src1_vreg(ir, &ir->compact_instructions[copy[side][m]]) == t;
          }
        }
        if (live)
          continue;
      }
      ed->side = r;
      ed->cc = cc;
      decided++;
    }
    int all = blocking == 0 && decided == nedge && nedge > 0;
    if (bt_stats_on())
    {
      bt_stat[5] += nedge;
      bt_stat[6] += decided;
      bt_stat[7] += all;
      bt_stat[8] += blocking > 0;
    }
    int pad_used[2] = {0, 0};
    for (int e = 0; e < nedge; e++)
    {
      BTEdge *ed = &edges[e];
      if (ed->side < 0)
        continue;
      int is_fall = ed->end == start;
      /* A fall-through edge gains a jump: worth it only when J goes. */
      if (!all && is_fall)
        continue;
      int r0 = ed->side == 2 ? 0 : ed->side; /* where the jump (or JMP) goes */
      if (ed->side == 2)
      {
        /* JUMPIF cc -> taken side, in front of the edge's end */
        bt_q_insert(&qu, ed->end, -1, ed->cc, succ[1], ncopy[1] > 0);
        pad_used[1] |= ncopy[1] > 0;
      }
      int pad = ncopy[r0] > 0;
      pad_used[r0] |= pad;
      if (is_fall)
        bt_q_insert(&qu, start, -1, -1, succ[r0], pad);
      else if (pad)
        bt_q_pad(&qu, ed->end, succ[r0]);
      else
        tcc_ir_op_set_dest_imm32(ir, &ir->compact_instructions[ed->end], succ[r0], IROP_BTYPE_INT32);
      if (ed->side == 2)
      {
        /* dead past the edge, checked above */
        ir->compact_instructions[ed->def].op = TCCIR_OP_NOP;
        for (int k = 0; k < ed->nchain; k++)
          ir->compact_instructions[ed->chain[k]].op = TCCIR_OP_NOP;
      }
      else if (ed->def >= 0)
        bt_q_def(&qu, ed->def);
      threaded++;
      if (bt_stats_on())
        bt_stat[9 + is_fall]++;
    }
    for (int r = 0; r < 2; r++)
      for (int c = 0; pad_used[r] && c < ncopy[r]; c++)
        bt_q_insert(&qu, succ[r], copy[r][c], -1, -1, 0);
  }
  if (qu.nins || qu.npads)
    bt_apply_inserts(ir, qu.ins, qu.nins, qu.pads, qu.npads, qu.defs, qu.ndefs);
  if (threaded)
  {
    bt_sweep_unreachable(ir);
    /* The constant moves a threaded edge carried may be read nowhere now. */
    BTState ls;
    bt_state_init(&ls, ir);
    for (int k = 0; k < qu.ndefs; k++)
    {
      IRQuadCompact *q = &ir->compact_instructions[qu.defs[k]];
      if (q->op != TCCIR_OP_ASSIGN && q->op != TCCIR_OP_SETIF)
        continue;
      int32_t d = tcc_ir_op_dest_vreg(ir, q);
      if (!bt_live_at(&ls, qu.defs[k] + 1, d))
        q->op = TCCIR_OP_NOP;
    }
    bt_state_free(&ls);
  }
  tcc_free(edges);
  tcc_free(qu.ins);
  tcc_free(qu.pads);
  tcc_free(qu.defs);
  tcc_free(jfill);
  tcc_free(jsrc);
  tcc_free(jcount);
  bt_state_free(&st);
  return threaded;
}

static void bt_print_stats(void)
{
  fprintf(stderr, "[bt] joins %ld facts unk %ld const %ld eq %ld ne %ld | edges %ld decided %ld all %ld blocked %ld | "
                  "thr jump %ld fall %ld setif %ld\n",
          bt_stat[0], bt_stat[1], bt_stat[2], bt_stat[3], bt_stat[4], bt_stat[5], bt_stat[6], bt_stat[7], bt_stat[8],
          bt_stat[9], bt_stat[10], bt_stat[11]);
  for (int i = 0; i < 400; i++)
    if (bt_whyc[i])
      fprintf(stderr, "[bt] why line %d: %ld\n", i, bt_whyc[i]);
}

int ra_thread_known_branches(TCCIRState *ir)
{
  if (!ir || ir->next_instruction_index < 4 || ir->func_has_label_addr || tcc_ir_calls_returns_twice(ir))
    return 0;
  for (int i = 0; i < ir->next_instruction_index; i++)
  {
    int op = ir->compact_instructions[i].op;
    if (op == TCCIR_OP_IJUMP || op == TCCIR_OP_NL_SETJMP || op == TCCIR_OP_SETJMP)
      return 0;
  }
  int total = 0;
  for (int round = 0; round < 3; round++)
  {
    int c = bt_run(ir);
    total += c;
    if (!c)
      break;
  }
  if (bt_stats_on())
  {
    static int registered;
    if (!registered)
    {
      registered = 1;
      atexit(bt_print_stats);
    }
  }
  return total;
}
