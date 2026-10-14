/*
 *  TCC IR - SSA byte-load combining (ssa:load_combine)
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

// See docs/bugs/kernel-streaming-wyhash-byte-assembly.md for the safety constraints.

#define USING_GLOBALS

#include "ir.h"
#include "ssa_opt.h"
#include "opt_range.h"

#define LC_MAX_TERMS 8
#define LC_MAX_DEPTH 48

typedef struct
{
  int32_t base; /* base vreg of the byte load */
  int32_t off;  /* constant byte offset from the base */
  int shift;    /* bit position in the combined value */
  int load_idx; /* instruction index of the byte load */
  IROperand load_op; /* the load's src1 (deref or base operand) */
  int indexed;
} LcTerm;

typedef struct
{
  TCCIRState *ir;
  int *def_idx;     /* per TEMP position: defining instruction, -1 none, -2 several */
  int ntemp;
  uint8_t *param_def; /* per PARAM position: defined somewhere */
  int nparam;
  int *jt_in; /* per instruction: jumps targeting it that are not "to next" */
  LcTerm terms[LC_MAX_TERMS];
  int nterms;
  int lo, hi; /* lowest / highest instruction index of the chain (loads and ops) */
} LcCtx;

static int lc_def_of(LcCtx *c, int32_t vr)
{
  if (vr < 0 || TCCIR_DECODE_VREG_TYPE(vr) != TCCIR_VREG_TYPE_TEMP)
    return -1;
  int p = TCCIR_DECODE_VREG_POSITION(vr);
  if (p < 0 || p >= c->ntemp)
    return -1;
  return c->def_idx[p] >= 0 ? c->def_idx[p] : -1;
}

/* A plain value read of a TEMP: a VREG operand, not dereferenced. */
static int lc_plain_temp(IROperand o, int32_t *vr)
{
  if (irop_get_tag(o) != IROP_TAG_VREG || o.is_lval || o.is_llocal || o.is_local)
    return 0;
  int32_t v = irop_get_vreg(o);
  if (v < 0 || TCCIR_DECODE_VREG_TYPE(v) != TCCIR_VREG_TYPE_TEMP)
    return 0;
  *vr = v;
  return 1;
}

static void lc_note(LcCtx *c, int d)
{
  if (d < c->lo)
    c->lo = d;
  if (d > c->hi)
    c->hi = d;
}

/* `AND x, #0xff` or `UBFX x, lsb 0, width 8`: the low byte of x, zero-extended.
 * *INNER gets x. */
static int lc_low_byte_op(TCCIRState *ir, IRQuadCompact *q, int32_t *inner)
{
  if (q->op != TCCIR_OP_UBFX && q->op != TCCIR_OP_AND)
    return 0;
  IROperand k = tcc_ir_op_get_src2(ir, q);
  if (!irop_is_immediate(k) || !lc_plain_temp(tcc_ir_op_get_src1(ir, q), inner))
    return 0;
  int64_t kv = irop_get_imm64_ex(ir, k);
  return q->op == TCCIR_OP_UBFX ? kv == (8 << 5) : (uint32_t)kv == 0xffu;
}

static int lc_base_stable(LcCtx *c, int32_t vr)
{
  if (vr < 0)
    return 0;
  int t = TCCIR_DECODE_VREG_TYPE(vr), p = TCCIR_DECODE_VREG_POSITION(vr);
  if (t == TCCIR_VREG_TYPE_TEMP)
    return p >= 0 && p < c->ntemp && c->def_idx[p] >= 0;
  if (t == TCCIR_VREG_TYPE_PARAM)
    return p >= 0 && p < c->nparam && !c->param_def[p];
  return 0;
}

/* V is the value of one byte loaded from base+off (through copies and
 * low-byte masks): fills T.  ANY_SIGN is set under a zero-extending mask,
 * which makes every extension below it irrelevant; unmasked, each step must
 * zero-extend. */
static int lc_byte_leaf(LcCtx *c, int32_t v, int any_sign, LcTerm *t, int depth)
{
  TCCIRState *ir = c->ir;
  if (depth > LC_MAX_DEPTH)
    return 0;
  int d = lc_def_of(c, v);
  if (d < 0)
    return 0;
  lc_note(c, d);
  IRQuadCompact *q = &ir->compact_instructions[d];
  int32_t inner;
  if (lc_low_byte_op(ir, q, &inner))
    return lc_byte_leaf(c, inner, 1, t, depth + 1);
  if (q->op == TCCIR_OP_ASSIGN || q->op == TCCIR_OP_ZEXT)
  {
    IROperand s = tcc_ir_op_get_src1(ir, q), dst = tcc_ir_op_get_dest(ir, q);
    int32_t sv;
    if (!lc_plain_temp(s, &sv))
      return 0;
    /* A copy keeps the low byte whatever its widths; unmasked it must keep
     * the zero extension too: the source read as a word (the recursion then
     * proves it a zero-extended byte, so a narrowing copy changes nothing) or
     * as an unsigned byte, into a word or an unsigned byte. */
    int sb = irop_get_btype(s), db = irop_get_btype(dst);
    if (sb != IROP_BTYPE_INT8 && sb != IROP_BTYPE_INT32 && sb != IROP_BTYPE_INT64)
      return 0;
    int zext = (sb == IROP_BTYPE_INT32 || sb == IROP_BTYPE_INT64 || s.is_unsigned) &&
               (db == IROP_BTYPE_INT32 || db == IROP_BTYPE_INT64 || (db == IROP_BTYPE_INT8 && dst.is_unsigned));
    if (!any_sign && !zext)
      return 0;
    return lc_byte_leaf(c, sv, any_sign, t, depth + 1);
  }
  if (q->op == TCCIR_OP_LOAD)
  {
    IROperand s = tcc_ir_op_get_src1(ir, q);
    IROperand dst = tcc_ir_op_get_dest(ir, q);
    if (irop_get_tag(s) == IROP_TAG_STACKOFF && s.is_lval && !s.is_llocal && !s.is_param &&
        irop_get_vreg(s) < 0 && irop_get_btype(s) == IROP_BTYPE_INT8 &&
        (any_sign || (s.is_unsigned && dst.is_unsigned)) && !tcc_ir_access_is_volatile(ir, s))
    {
      t->base = -1;
      t->off = irop_get_stack_offset(s);
      t->load_idx = d;
      t->load_op = s;
      t->indexed = 0;
      return 1;
    }
    if (irop_get_tag(s) != IROP_TAG_VREG || !s.is_lval || s.is_llocal || s.is_local || irop_get_btype(s) != IROP_BTYPE_INT8)
      return 0;
    /* Lowerings pick LDRB/LDRSB from either operand's flag: both must agree. */
    if (!any_sign && !(s.is_unsigned && dst.is_unsigned))
      return 0;
    if (tcc_ir_access_is_volatile(ir, s))
      return 0;
    int32_t b = irop_get_vreg(s);
    if (!lc_base_stable(c, b))
      return 0;
    t->base = b;
    t->off = 0;
    t->load_idx = d;
    t->load_op = s;
    t->indexed = 0;
    return 1;
  }
  if (q->op == TCCIR_OP_LOAD_INDEXED)
  {
    IROperand dst = tcc_ir_op_get_dest(ir, q);
    IROperand b = tcc_ir_op_get_src1(ir, q);
    IROperand ix = tcc_ir_op_get_src2(ir, q);
    IROperand sc = tcc_ir_op_get_scale(ir, q);
    if (irop_get_btype(dst) != IROP_BTYPE_INT8 || !irop_is_immediate(ix) || !irop_is_immediate(sc))
      return 0;
    if (!any_sign && !dst.is_unsigned)
      return 0;
    if (irop_get_tag(b) != IROP_TAG_VREG || b.is_lval || b.is_llocal || b.is_local)
      return 0;
    /* An index register with a fused shift is not the immediate it reads as. */
    if (ir->barrel_shifts && q->orig_index >= 0 && q->orig_index < ir->barrel_shifts_len &&
        ir->barrel_shifts[q->orig_index])
      return 0;
    if (tcc_ir_access_is_volatile(ir, b) || tcc_ir_access_is_volatile(ir, dst))
      return 0;
    int32_t bv = irop_get_vreg(b);
    int64_t sh = irop_get_imm64_ex(ir, sc), idx = irop_get_imm64_ex(ir, ix);
    if (sh < 0 || sh > 3 || idx < -(1 << 20) || idx > (1 << 20) || !lc_base_stable(c, bv))
      return 0;
    t->base = bv;
    t->off = (int32_t)(idx << sh);
    t->load_idx = d;
    t->load_op = b;
    t->indexed = 1;
    return 1;
  }
  return 0;
}

/* An operand of a word op that reads its TEMP's register as a word: typed
 * INT32, or narrower and unsigned (a zero-extended value; lc_byte_leaf checks
 * the def really zero-extends). */
static int lc_word_read(IROperand o)
{
  int bt = irop_get_btype(o);
  return bt == IROP_BTYPE_INT32 || bt == IROP_BTYPE_INT64 || ((bt == IROP_BTYPE_INT8 || bt == IROP_BTYPE_INT16) && o.is_unsigned);
}

/* Terms of the value V shifted left by SH bits. */
static int lc_collect(LcCtx *c, int32_t v, int sh, int limit, int depth)
{
  TCCIRState *ir = c->ir;
  if (depth > LC_MAX_DEPTH || sh >= limit)
    return 0;
  int d = lc_def_of(c, v);
  if (d < 0)
    return 0;
  lc_note(c, d);
  IRQuadCompact *q = &ir->compact_instructions[d];
  /* A fused barrel shift on src2 (type 1 = LSL; enc = type << 5 | amount) is
   * `OR a, b << amount`; on anything else it is a shape this pass does not model. */
  int s2_lsl = 0;
  if (ir->barrel_shifts && q->orig_index >= 0 && q->orig_index < ir->barrel_shifts_len && ir->barrel_shifts[q->orig_index])
  {
    uint8_t enc = ir->barrel_shifts[q->orig_index];
    if (q->op != TCCIR_OP_OR || (enc >> 5) != 1 || (enc & 7) || (enc & 31) == 0)
      return 0;
    s2_lsl = enc & 31;
  }
  int bt = tcc_ir_op_dest_btype(ir, q);
  int bits = bt == IROP_BTYPE_INT64 ? 64 : 32;
  if (limit > sh + bits)
    limit = sh + bits;
  int32_t a, b;
  switch (q->op)
  {
  case TCCIR_OP_ASSIGN:
  case TCCIR_OP_ZEXT:
  {
    if (bt != IROP_BTYPE_INT32 && bt != IROP_BTYPE_INT64)
      break; /* a narrowing copy: only a leaf can be one */
    IROperand src = tcc_ir_op_get_src1(ir, q);
    int sb = irop_get_btype(src);
    if (!lc_plain_temp(src, &a) || (sb != IROP_BTYPE_INT32 && sb != IROP_BTYPE_INT64) ||
        (q->op != TCCIR_OP_ZEXT && bits == 64 && sb == IROP_BTYPE_INT32 && !src.is_unsigned))
      break;
    return lc_collect(c, a, sh, limit, depth + 1);
  }
  case TCCIR_OP_OR:
    if ((bt != IROP_BTYPE_INT32 && bt != IROP_BTYPE_INT64) ||
        !lc_plain_temp(tcc_ir_op_get_src1(ir, q), &a) || !lc_plain_temp(tcc_ir_op_get_src2(ir, q), &b) ||
        !lc_word_read(tcc_ir_op_get_src1(ir, q)) || !lc_word_read(tcc_ir_op_get_src2(ir, q)))
      return 0;
    return lc_collect(c, a, sh, limit, depth + 1) && lc_collect(c, b, sh + s2_lsl, limit, depth + 1);
  case TCCIR_OP_SHL:
  {
    IROperand k = tcc_ir_op_get_src2(ir, q);
    if ((bt != IROP_BTYPE_INT32 && bt != IROP_BTYPE_INT64) ||
        !lc_plain_temp(tcc_ir_op_get_src1(ir, q), &a) || !irop_is_immediate(k) ||
        !lc_word_read(tcc_ir_op_get_src1(ir, q)))
      return 0;
    int64_t kv = irop_get_imm64_ex(ir, k);
    if (kv <= 0 || kv >= bits || (kv & 7))
      return 0;
    return lc_collect(c, a, sh + (int)kv, limit, depth + 1);
  }
  default:
    break;
  }
  /* A leaf: one zero-extended byte. */
  if (c->nterms >= LC_MAX_TERMS || sh + 8 > limit)
    return 0;
  LcTerm *t = &c->terms[c->nterms];
  if (!lc_byte_leaf(c, v, 0, t, depth + 1))
    return 0;
  t->shift = sh;
  c->nterms++;
  return 1;
}

/* No write, call, join or branch from the first instruction of the chain to
 * the root, and the root itself no join: every byte load and every op of the
 * chain runs, in this order, on the one path that reaches the root, and
 * memory does not change in between. */
static int lc_range_ok(LcCtx *c, int lo, int hi)
{
  TCCIRState *ir = c->ir;
  const uint32_t mask = IR_HZ_ALL & ~(IR_HZ_MEM_READ | IR_HZ_SRC_LVAL | IR_HZ_BRANCH | IR_HZ_JOIN | IR_HZ_JOIN_END);
  for (int i = lo + 1; i <= hi; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->is_jump_target && c->jt_in[i])
      return 0;
    if (i == hi || q->op == TCCIR_OP_NOP)
      continue;
    if (q->op == TCCIR_OP_JUMP)
      continue; /* jt_in only counts the jumps that are not to the next instruction */
    if (ir_q_hazards(ir, q, mask))
      return 0;
    if (ir_op_has(q->op, IR_HZ_BRANCH) || ir_op_has(q->op, IROP_A_NO_FALLTHROUGH))
      return 0;
  }
  return 1;
}

static int lc_try_root(LcCtx *c, int r)
{
  TCCIRState *ir = c->ir;
  IRQuadCompact *q = &ir->compact_instructions[r];
  IROperand dest = tcc_ir_op_get_dest(ir, q);
  int32_t rv = irop_get_vreg(dest);
  if (q->op != TCCIR_OP_OR || rv < 0 || TCCIR_DECODE_VREG_TYPE(rv) != TCCIR_VREG_TYPE_TEMP || dest.is_lval ||
      (irop_get_btype(dest) != IROP_BTYPE_INT32 && irop_get_btype(dest) != IROP_BTYPE_INT64))
    return 0;
  c->nterms = 0;
  c->lo = r;
  c->hi = -1;
  if (!lc_collect(c, rv, 0, irop_get_btype(dest) == IROP_BTYPE_INT64 ? 64 : 32, 0))
    return 0;
  int n = c->nterms;
  if (n != 2 && n != 4 && n != 8)
    return 0;
  /* Order the terms by shift; shifts must be 0,8,.. and addresses consecutive. */
  LcTerm *byshift[LC_MAX_TERMS] = {0};
  for (int i = 0; i < n; i++)
  {
    int s = c->terms[i].shift;
    if ((s & 7) || s / 8 >= n || byshift[s / 8])
      return 0;
    byshift[s / 8] = &c->terms[i];
  }
  int32_t base = byshift[0]->base, off0 = byshift[0]->off;
  for (int j = 1; j < n; j++)
    if (byshift[j]->base != base || byshift[j]->off != off0 + j) /* the load forms may differ */
      return 0;
  /* The whole chain (the root included, recorded by lc_collect) lies in one
   * straight run that ends at the root. */
  if (c->hi != r || c->lo >= r || !lc_range_ok(c, c->lo, r))
    return 0;

  /* Build the wide load in place of the root.  May be unaligned: LDR/LDRH
   * are fine on ARMv8-M mainline (the only profile this backend targets;
   * mem_inline relies on the same), LDRD/LDM are not -- the UNDERALIGN hint
   * on every operand keeps the pairing peepholes away. */
  // Partial i64 chains retain their zero extension until a full eight-byte root is found.
  if (irop_get_btype(dest) == IROP_BTYPE_INT64 && n != 8)
    return 0;
  int load_bt = n == 8 ? IROP_BTYPE_INT64 : n == 4 ? IROP_BTYPE_INT32 : IROP_BTYPE_INT16;
  IROperand nd = dest;
  nd.is_unsigned = 1;
  nd.aux |= IROP_AUX_UNDERALIGN;
  int base_idx;
  if (!byshift[0]->indexed)
  {
    /* deref operand of the lowest byte, widened */
    IROperand src = irop_retype_scalar(byshift[0]->load_op, load_bt);
    src.is_unsigned = 1;
    src.aux |= IROP_AUX_UNDERALIGN;
    /* The dest carries the access width too, as in the indexed form: a later
     * LOAD -> LOAD_INDEXED fold reads the width off the dest, and a u32 dest
     * over a 16-bit read became an LDR. */
    nd = irop_retype_scalar(nd, load_bt);
    nd.is_unsigned = 1;
    base_idx = tcc_ir_pool_add(ir, nd);
    tcc_ir_pool_add(ir, src);
    tcc_ir_pool_add(ir, irop_make_none());
    q->op = TCCIR_OP_LOAD;
  }
  else
  {
    IROperand b = byshift[0]->load_op;
    b.aux |= IROP_AUX_UNDERALIGN;
    IROperand dd = irop_retype_scalar(nd, load_bt);
    dd.is_unsigned = 1;
    dd.aux |= IROP_AUX_UNDERALIGN;
    base_idx = tcc_ir_pool_add(ir, dd);
    tcc_ir_pool_add(ir, b);
    tcc_ir_pool_add(ir, irop_make_imm32(-1, off0, IROP_BTYPE_INT32));
    tcc_ir_pool_add(ir, irop_make_imm32(-1, 0, IROP_BTYPE_INT32));
    q->op = TCCIR_OP_LOAD_INDEXED;
  }
  q->operand_base = base_idx;
  /* A new instruction: a fresh orig_index, or it would inherit the OR's
   * side-table annotations (a fused `lsl` on src2 = the index). */
  q->orig_index = ++ir->max_orig_index;
  return 1;
}

int ssa_opt_load_combine(IRSSAOptCtx *ctx)
{
  TCCIRState *ir = ctx->ir;
  int n = ir->next_instruction_index;
  if (n < 4)
    return 0;
  /* Cheap reject: a root needs at least a SHL #8 and an OR. */
  int have_or = 0, have_load = 0;
  for (int i = 0; i < n && !(have_or && have_load); i++)
  {
    int op = ir->compact_instructions[i].op;
    have_or |= op == TCCIR_OP_OR;
    have_load |= op == TCCIR_OP_LOAD || op == TCCIR_OP_LOAD_INDEXED;
  }
  if (!have_or || !have_load)
    return 0;

  LcCtx c;
  memset(&c, 0, sizeof c);
  c.ir = ir;
  c.ntemp = ir->next_temporary_variable;
  c.nparam = ir->next_parameter;
  if (c.ntemp <= 0)
    return 0;
  c.def_idx = tcc_malloc(sizeof(int) * (size_t)c.ntemp);
  for (int i = 0; i < c.ntemp; i++)
    c.def_idx[i] = -1;
  c.param_def = tcc_mallocz((size_t)(c.nparam > 0 ? c.nparam : 1));
  c.jt_in = tcc_mallocz(sizeof(int) * (size_t)(n + 1));
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;
    if (q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_JUMPIF)
    {
      int t = (int)tcc_ir_op_dest_imm(ir, q);
      int nx = i + 1;
      while (nx < n && ir->compact_instructions[nx].op == TCCIR_OP_NOP)
        nx++;
      int tn = t;
      while (tn < n && ir->compact_instructions[tn].op == TCCIR_OP_NOP)
        tn++;
      if (!(q->op == TCCIR_OP_JUMP && tn == nx) && t >= 0 && t < n)
        c.jt_in[t]++;
    }
    if (!irop_config[q->op].has_dest)
      continue;
    IROperand d = tcc_ir_op_get_dest(ir, q);
    int32_t v = irop_get_vreg(d);
    if (v < 0 || !irop_dest_defines_vreg(d))
      continue;
    int t = TCCIR_DECODE_VREG_TYPE(v), p = TCCIR_DECODE_VREG_POSITION(v);
    if (t == TCCIR_VREG_TYPE_TEMP && p >= 0 && p < c.ntemp)
      c.def_idx[p] = c.def_idx[p] == -1 ? i : -2;
    else if (t == TCCIR_VREG_TYPE_PARAM && p >= 0 && p < c.nparam)
      c.param_def[p] = 1;
  }
  for (int ti = 0; ti < ir->num_switch_tables; ti++)
  {
    TCCIRSwitchTable *tb = &ir->switch_tables[ti];
    if (tb->default_target >= 0 && tb->default_target < n)
      c.jt_in[tb->default_target]++;
    for (int tj = 0; tj < tb->num_entries; tj++)
      if (tb->targets[tj] >= 0 && tb->targets[tj] < n)
        c.jt_in[tb->targets[tj]]++;
  }

  int changed = 0;
  /* Later roots first: the root of a chain must be seen before its inner ORs. */
  for (int r = n - 1; r >= 0; r--)
  {
    if (ir->compact_instructions[r].op != TCCIR_OP_OR)
      continue;
    if (lc_try_root(&c, r))
    {
      /* the root is now a load: later (earlier-index) roots that fed it are dead */
      c.def_idx[TCCIR_DECODE_VREG_POSITION(irop_get_vreg(tcc_ir_op_get_dest(ir, &ir->compact_instructions[r])))] = r;
      changed++;
    }
  }
  tcc_free(c.def_idx);
  tcc_free(c.param_def);
  tcc_free(c.jt_in);
  if (changed)
    tcc_ir_ssa_opt_rebuild(ctx);
  return changed;
}
