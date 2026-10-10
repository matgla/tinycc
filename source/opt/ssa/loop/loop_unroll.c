/*
 *  TCC IR - SSA loop: unrolling / constant-trip elimination
 *
 *  Copyright (c) 2025 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#include "ir.h"
#include "ssa_opt.h"
#include "opt_loop_utils.h"
#include "loop_cand.h"
#include "opt_range.h"
#include "opt_utils.h"

#define SSA_LOOP_UNROLL_MAX_PASSES 4
#define SSA_UNROLL_MAX_SPAN 256

/* Whether instruction Q names vreg VR in any operand slot. */
static int insn_mentions_vreg(TCCIRState *ir, IRQuadCompact *q, int32_t vr)
{
  if (q->op == TCCIR_OP_NOP)
    return 0;
  if (irop_config[q->op].has_dest && tcc_ir_op_dest_vreg(ir, q) == vr)
    return 1;
  if (irop_config[q->op].has_src1 && tcc_ir_op_src1_vreg(ir, q) == vr)
    return 1;
  if (irop_config[q->op].has_src2 && tcc_ir_op_src2_vreg(ir, q) == vr)
    return 1;
  if ((tcc_ir_op_is_mac(q->op) || q->op == TCCIR_OP_LOAD_INDEXED || q->op == TCCIR_OP_STORE_INDEXED) &&
      irop_get_vreg(ir->iroperand_pool[q->operand_base + 3]) == vr)
    return 1;
  return 0;
}

/* Whether source operand S reads a vreg's own value: a register vreg, or a
 * variable in its stack slot (a STACKOFF lvalue naming it) -- not a load
 * through the pointer it holds. */
static int src_reads_vreg_value(IROperand s)
{
  if (irop_get_vreg(s) < 0 || s.is_llocal)
    return 0;
  int tag = irop_get_tag(s);
  return tag == IROP_TAG_VREG ? !s.is_lval : tag == IROP_TAG_STACKOFF;
}

/* `DST <- SRC` between two plain 32-bit VARs (a VAR read is a LOAD or an ASSIGN). */
static int is_var_copy(TCCIRState *ir, IRQuadCompact *q, int32_t *dst, int32_t *src)
{
  if (q->op != TCCIR_OP_ASSIGN && q->op != TCCIR_OP_LOAD)
    return 0;
  IROperand d = tcc_ir_op_get_dest(ir, q), s = tcc_ir_op_get_src1(ir, q);
  int32_t dv = irop_get_vreg(d), sv = irop_get_vreg(s);
  if (dv < 0 || sv < 0 || TCCIR_DECODE_VREG_TYPE(dv) != TCCIR_VREG_TYPE_VAR ||
      TCCIR_DECODE_VREG_TYPE(sv) != TCCIR_VREG_TYPE_VAR || !irop_dest_defines_vreg(d) || !src_reads_vreg_value(s) ||
      irop_get_btype(d) != IROP_BTYPE_INT32 || irop_get_btype(s) != IROP_BTYPE_INT32)
    return 0;
  *dst = dv;
  *src = sv;
  return 1;
}

static int next_real(TCCIRState *ir, int i, int end)
{
  while (i <= end && ir->compact_instructions[i].op == TCCIR_OP_NOP)
    i++;
  return i;
}

static int prev_real(TCCIRState *ir, int i, int start)
{
  while (i >= start && ir->compact_instructions[i].op == TCCIR_OP_NOP)
    i--;
  return i;
}

/* Q computes a value equal to one of SAME[0..N): a copy of it, or it ANDed
 * with all ones (an immediate or a TEMP whose only def is `#-1`). */
static int cz_value_step(TCCIRState *ir, IRQuadCompact *q, const int32_t *same, int n)
{
  if (q->op != TCCIR_OP_ASSIGN && q->op != TCCIR_OP_LOAD && q->op != TCCIR_OP_STORE && q->op != TCCIR_OP_AND)
    return 0;
  IROperand d = tcc_ir_op_get_dest(ir, q), s = tcc_ir_op_get_src1(ir, q);
  int32_t dv = irop_get_vreg(d);
  if (dv < 0 || !irop_dest_defines_vreg(d) || !src_reads_vreg_value(s) ||
      irop_get_btype(d) != IROP_BTYPE_INT32)
    return 0;
  int in = 0;
  for (int k = 0; k < n; k++)
    in |= irop_get_vreg(s) == same[k];
  if (!in)
    return 0;
  if (q->op != TCCIR_OP_AND)
    return 1;
  /* A barrel-fused shift on the mask (side table only): not all ones. */
  if (tcc_ir_barrel_shift_at(ir, q))
    return 0;
  IROperand m = tcc_ir_op_get_src2(ir, q);
  if (irop_is_immediate(m))
    return (uint32_t)irop_get_imm64_ex(ir, m) == 0xffffffffu;
  int32_t mv = irop_get_vreg(m);
  if (mv < 0 || TCCIR_DECODE_VREG_TYPE(mv) != TCCIR_VREG_TYPE_TEMP || !src_reads_vreg_value(m))
    return 0;
  int defs = 0, ones = 0;
  for (int i = 0; i < ir->next_instruction_index; i++)
  {
    IRQuadCompact *dq = &ir->compact_instructions[i];
    if (dq->op == TCCIR_OP_NOP || !irop_config[dq->op].has_dest)
      continue;
    IROperand dd = tcc_ir_op_get_dest(ir, dq);
    if (irop_get_vreg(dd) != mv || !irop_dest_defines_vreg(dd))
      continue;
    defs++;
    ones = dq->op == TCCIR_OP_ASSIGN && tcc_ir_op_src1_is_imm(ir, dq) && (uint32_t)tcc_ir_op_src1_imm(ir, dq) == 0xffffffffu;
  }
  return defs == 1 && ones;
}
#define CZ_MAX_SLOTS 12

/* The Zig C backend spells every loop counter with two locals:
 *
 *     B = init;
 *   loop:
 *     A = B;  C = A; ...  if (!(C < n)) goto out;
 *     ... A ...
 *     A = A + step;  B = A;  goto loop;
 *
 * (C and the steps before the test are value copies: an inlined
 * zig_u32_bitCast_u32 binds its parameter and masks it with all ones.)  So A
 * is defined twice in the loop and B never incremented: no induction
 * variable.  When B is mentioned nowhere else and A, and its copies, are not
 * read after the loop, B only carries A round the back edge, and the loop is
 * the classic `A = init; loop: if (!(A < n)) goto out; ...; A += step;`.
 * Rewrite it so: the init defines A, both copies of the carrier go, the exit
 * test reads A and comes first in the loop, where find_loop_exit_condition
 * looks for it.  Returns 1 if rewritten.  Done only on a loop the unroller is
 * about to consider: the parked loop_var_coalesce pass merged such counters
 * in every loop of every function and shifted inline decisions. */
static int canonicalize_zig_counter(TCCIRState *ir, int start, int end)
{
  const int n = ir->next_instruction_index;
  int h = next_real(ir, start, end);
  int32_t a, b;
  if (h > end || !is_var_copy(ir, &ir->compact_instructions[h], &a, &b) || a == b)
    return 0;

  /* Between the copy and the exit test, only steps that pass A's value on
   * unchanged: copies (an inlined zig_u32_bitCast_u32 parameter binds it by
   * `V <- A [STORE]`) and an AND with all ones (its mask).  They stay; the
   * test reads A itself. */
#define CZ_MAX_CHAIN 8
  int32_t same[CZ_MAX_CHAIN + 1];
  int nsame = 0;
  same[nsame++] = a;
  int cmp = next_real(ir, h + 1, end);
  while (cmp <= end && ir->compact_instructions[cmp].op != TCCIR_OP_CMP)
  {
    IRQuadCompact *q = &ir->compact_instructions[cmp];
    if (nsame > CZ_MAX_CHAIN || !cz_value_step(ir, q, same, nsame))
      return 0;
    int32_t d = tcc_ir_op_dest_vreg(ir, q);
    if (d == a || d == b)
      return 0;
    same[nsame++] = d;
    cmp = next_real(ir, cmp + 1, end);
  }
  if (cmp > end)
    return 0;
  IRQuadCompact *cq = &ir->compact_instructions[cmp];
  IROperand cmp_src = tcc_ir_op_get_src1(ir, cq);
  int cmp_ok = 0;
  for (int k = 0; k < nsame; k++)
    cmp_ok |= irop_get_vreg(cmp_src) == same[k];
  if (!cmp_ok || !src_reads_vreg_value(cmp_src) || !tcc_ir_op_src2_is_imm(ir, cq))
    return 0;
#undef CZ_MAX_CHAIN
  int jif = next_real(ir, cmp + 1, end);
  if (jif > end || ir->compact_instructions[jif].op != TCCIR_OP_JUMPIF ||
      (int)tcc_ir_op_dest_imm(ir, &ir->compact_instructions[jif]) <= end)
    return 0;

  /* Latch: A <- A + #step; B <- A; JUMP header. */
  int back = prev_real(ir, end, start);
  if (back < start || ir->compact_instructions[back].op != TCCIR_OP_JUMP)
    return 0;
  int l1 = prev_real(ir, back - 1, start);
  int32_t l1d, l1s;
  if (l1 <= jif || !is_var_copy(ir, &ir->compact_instructions[l1], &l1d, &l1s) || l1d != b || l1s != a)
    return 0;
  int l0 = prev_real(ir, l1 - 1, start);
  if (l0 <= jif)
    return 0;
  IRQuadCompact *iq = &ir->compact_instructions[l0];
  if (iq->op != TCCIR_OP_ADD || tcc_ir_op_dest_vreg(ir, iq) != a ||
      tcc_ir_op_src1_vreg(ir, iq) != a || !tcc_ir_op_src2_is_imm(ir, iq))
    return 0;

  /* Init `B <- #imm` just before the loop (constants may be set up after it). */
  int init = -1;
  for (int i = prev_real(ir, start - 1, 0), k = 0; i >= 0 && k < 6; i = prev_real(ir, i - 1, 0), k++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_JUMPIF || ir->compact_instructions[i].is_jump_target)
      break;
    if (irop_config[q->op].has_dest && tcc_ir_op_dest_vreg(ir, q) == b)
    {
      init = i;
      break;
    }
  }
  if (init < 0 || ir->compact_instructions[init].op != TCCIR_OP_ASSIGN)
    return 0;
  IRQuadCompact *initq = &ir->compact_instructions[init];
  IROperand init_d = tcc_ir_op_get_dest(ir, initq);
  if (!irop_dest_defines_vreg(init_d) || !tcc_ir_op_src1_is_imm(ir, initq))
    return 0;

  /* The init moves to define A: a def or read of A between it and the loop
   * would then clobber the moved init or see it early. */
  for (int i = init + 1; i < start; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_NOP && insn_mentions_vreg(ir, q, a))
      return 0;
  }

  /* B: only the init, the header copy and the latch copy.  A: defined in the
   * loop only by the header copy and the increment, never read after it. */
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;
    if (i != init && i != h && i != l1 && insn_mentions_vreg(ir, q, b))
      return 0;
    /* A and the values copied from it keep their exit value, which
     * unrolling does not reproduce, so none may be read after the loop. */
    for (int k = 0; k < nsame; k++)
      if (i > end && insn_mentions_vreg(ir, q, same[k]))
        return 0;
    if (i >= start && i <= end && i != h && i != l0 && irop_config[q->op].has_dest &&
        tcc_ir_op_dest_vreg(ir, q) == a)
      return 0;
  }
  /* A jump into the loop other than to its header would skip `A = B`, and
   * one to a step between the copy and the exit test would land in the
   * reordered run below. */
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_JUMP && q->op != TCCIR_OP_JUMPIF)
      continue;
    int t = next_real(ir, (int)tcc_ir_op_dest_imm(ir, q), end);
    if ((i < start || i > end) && t > h && t <= end)
      return 0;
    if (t > h && t <= jif)
      return 0;
  }

  IROperand nd = init_d;
  irop_set_vreg(&nd, a);
  tcc_ir_op_set_dest(ir, initq, nd);
  IROperand ns = cmp_src;
  irop_set_vreg(&ns, a);
  tcc_ir_op_set_src1(ir, cq, ns);
  ir->compact_instructions[h].op = TCCIR_OP_NOP;
  ir->compact_instructions[l1].op = TCCIR_OP_NOP;
  /* A value step spelled `V <- A [STORE]` is a copy between variables: say
   * so, since a STORE-bound VAR reads as a load bound to a parameter, which
   * the unroller must keep as a loop. */
  for (int i = h + 1; i < cmp; i++)
    if (ir->compact_instructions[i].op == TCCIR_OP_STORE)
      ir->compact_instructions[i].op = TCCIR_OP_ASSIGN;
  /* The exit test goes first, the value steps after it (they are dead on the
   * exit path, nothing reads them after the loop): the loop is then the
   * classic top-tested shape whose test find_loop_exit_condition looks for at
   * the header.  Slots keep their jump-target flags; the quads move. */
  {
    int slots[CZ_MAX_SLOTS], ns = 0;
    IRQuadCompact moved[CZ_MAX_SLOTS];
    for (int i = h; i <= jif && ns < CZ_MAX_SLOTS; i++)
      if (ir->compact_instructions[i].op != TCCIR_OP_NOP || i == h)
        slots[ns++] = i;
    if (ns < CZ_MAX_SLOTS)
    {
      int m = 0;
      moved[m++] = ir->compact_instructions[cmp];
      moved[m++] = ir->compact_instructions[jif];
      for (int k = 1; k < ns; k++)
        if (slots[k] != cmp && slots[k] != jif)
          moved[m++] = ir->compact_instructions[slots[k]];
      for (; m < ns; m++)
      {
        moved[m] = ir->compact_instructions[h];
        moved[m].op = TCCIR_OP_NOP;
      }
      for (int k = 0; k < ns; k++)
      {
        IRQuadCompact *q = &ir->compact_instructions[slots[k]];
        int jt = q->is_jump_target;
        *q = moved[k];
        q->is_jump_target = jt;
      }
    }
  }
  LOG_LOOP_OPT("canonicalize_zig_counter: loop [%d..%d] counter VAR%d", start, end, TCCIR_DECODE_VREG_POSITION(a));
  return 1;
}

static int unroll_try_candidate(TCCIRState *ir, IRCFG *cfg, int header_b,
                                uint8_t *member, uint8_t *scratch)
{
  LcsSpan sp;
  if (!lcs_cand_span(ir, cfg, header_b, member, scratch, SSA_UNROLL_MAX_SPAN, &sp))
    return 0;
  int eff_start = sp.start;
  int eff_end = sp.end;

  IRLoop loop = {0};
  loop.header_idx = sp.header;
  loop.start_idx = eff_start;
  loop.end_idx = eff_end; /* latch back-edge instruction */
  loop.preheader_idx = sp.preheader;
  loop.depth = 1;

  canonicalize_zig_counter(ir, eff_start, eff_end);

  if (try_eliminate_loop(ir, &loop))
    return 1;
  if (try_eliminate_loop_symbolic(ir, &loop))
    return 1;

  /* try_unroll_loop_ex enables its IR-growth path only when num_loops==1. */
  IRLoops one = {0};
  one.loops = &loop;
  one.num_loops = 1;
  one.capacity = 1;
  return try_unroll_loop_ex(ir, &loop, &one, 0);
}

int ssa_opt_loop_unroll(TCCIRState *ir)
{
  return lcs_run_outermost(ir, SSA_LOOP_UNROLL_MAX_PASSES, 1, unroll_try_candidate);
}

/* ------------------------------------------------------------------------
 * Constant-trip unrolling of loops with an exit anywhere in the body
 * (ssa:loop_unroll_midexit).
 *
 * The Zig C backend spells `mem.readInt` as a do-while whose exit test is in
 * the middle:
 *
 *   V5 = V0; <load byte V5, shift/or into the accumulator>; CMP V5,#0
 *   JUMPIF != L; V3 = acc; JUMP out; L: V1 = acc; V5 -= 1; V0 = V5; JUMP top
 *
 * which neither the top-tested nor the rotated unroller recognises.  This pass
 * does not look for an induction variable: it SIMULATES the loop's control on
 * the constants known at entry (a straight-line scan above the loop) and, if
 * every branch it meets is decided by known values and the loop leaves after
 * N <= 16 trips, replaces it by N verbatim copies of the body in sequence.
 * Values that only feed data (loaded bytes, the accumulator) stay unknown and
 * are never consulted, so the copies compute exactly what the loop did; the
 * SSA/propagation passes that follow fold the counter into constants.
 * Deny by default: any call, return, asm, VLA, switch, nested/extra back
 * edge, entry other than the header, or branch the simulation cannot decide
 * (a JUMPIF not fed by the CMP above it, a value of a VAR whose address the
 * function takes) leaves the loop alone.  Copies must not leave multi-def
 * temporaries behind, which SSA folds as if single-def: every TEMP the body
 * defines must be private to one iteration (renamed per copy), and a STORE
 * defining a vreg is only accepted as the narrow parameter binding of an
 * inlined helper, which is forwarded first (mx_narrow_store_plan).
 * Knob: TCC_DISABLE_PASS=ssa:loop_unroll_midexit. */

#define MX_MAX_TRIP 16
#define MX_MAX_TOTAL 192
#define MX_MAX_VARS 48
#define MX_MAX_RENAME 24

#define MX_MAX_ESC 32

/* VARs whose address the function takes (LEA / asm operand): memory writes
 * may change them behind the simulation's back, so their values are never
 * consulted.  More than MX_MAX_ESC of them and no VAR is. */
typedef struct
{
  int32_t vr[MX_MAX_ESC];
  int n;
  int overflow;
} MxEsc;

typedef struct
{
  int32_t vr[MX_MAX_VARS];
  int32_t val[MX_MAX_VARS];
  uint8_t known[MX_MAX_VARS];
  int n;
  const MxEsc *esc;
} MxState;

static void mx_collect_escaped(TCCIRState *ir, MxEsc *e)
{
  e->n = 0;
  e->overflow = 0;
  for (int i = 0; i < ir->next_instruction_index; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    int32_t vr = -1;
    if (q->op == TCCIR_OP_LEA || q->op == TCCIR_OP_ASM_INPUT)
      vr = tcc_ir_op_src1_vreg(ir, q);
    else if (q->op == TCCIR_OP_ASM_OUTPUT)
      vr = tcc_ir_op_dest_vreg(ir, q);
    else
      continue;
    if (vr < 0 || TCCIR_DECODE_VREG_TYPE(vr) != TCCIR_VREG_TYPE_VAR)
      continue;
    if (e->n >= MX_MAX_ESC)
    {
      e->overflow = 1;
      return;
    }
    e->vr[e->n++] = vr;
  }
}

static int mx_escaped(TCCIRState *ir, const MxEsc *e, int32_t vr)
{
  if (TCCIR_DECODE_VREG_TYPE(vr) != TCCIR_VREG_TYPE_VAR)
    return 0;
  if (e->overflow || tcc_ir_vreg_flag_addrtaken_get(ir, vr))
    return 1;
  for (int i = 0; i < e->n; i++)
    if (e->vr[i] == vr)
      return 1;
  return 0;
}

static int mx_find(MxState *s, int32_t vr)
{
  for (int i = 0; i < s->n; i++)
    if (s->vr[i] == vr)
      return i;
  return -1;
}

/* Returns 0 when the table is full (the caller gives up). */
static int mx_set(MxState *s, int32_t vr, int known, int32_t val)
{
  int i = mx_find(s, vr);
  if (i < 0)
  {
    if (s->n >= MX_MAX_VARS)
      return 0;
    i = s->n++;
    s->vr[i] = vr;
  }
  s->known[i] = (uint8_t)known;
  s->val[i] = val;
  return 1;
}

/* Read a known integer value with the source operand's width and signedness. */
static int mx_read(TCCIRState *ir, MxState *s, IROperand o, int32_t *out)
{
  int btype = irop_get_btype(o);
  if (btype != IROP_BTYPE_INT32 && btype != IROP_BTYPE_INT8 && btype != IROP_BTYPE_INT16)
    return 0;
  if (irop_is_immediate(o))
  {
    *out = (int32_t)irop_get_imm64_ex(ir, o);
  }
  else
  {
    // Read the vreg's value, never its address or a pointer dereference.
    int tag = irop_get_tag(o);
    if (irop_get_vreg(o) < 0 || o.is_llocal ||
        !(tag == IROP_TAG_VREG ? !o.is_lval : tag == IROP_TAG_STACKOFF && o.is_lval))
      return 0;
    if (mx_escaped(ir, s->esc, irop_get_vreg(o)))
      return 0;
    int i = mx_find(s, irop_get_vreg(o));
    if (i < 0 || !s->known[i])
      return 0;
    *out = s->val[i];
  }
  if (btype == IROP_BTYPE_INT8)
    *out = o.is_unsigned ? (int32_t)(uint8_t)*out : (int32_t)(int8_t)*out;
  else if (btype == IROP_BTYPE_INT16)
    *out = o.is_unsigned ? (int32_t)(uint16_t)*out : (int32_t)(int16_t)*out;
  return 1;
}

static int mx_walk(TCCIRState *ir, int start, int end, int rewrite, const MxEsc *esc);

static int mx_has_barrel(TCCIRState *ir, IRQuadCompact *q)
{
  return ir->barrel_shifts && q->orig_index >= 0 && q->orig_index < ir->barrel_shifts_len &&
         ir->barrel_shifts[q->orig_index];
}

static int mx_cond(int cond, int32_t a, int32_t b, int *res)
{
  switch (cond)
  {
  case TOK_EQ: *res = a == b; return 1;
  case TOK_NE: *res = a != b; return 1;
  case TOK_LT: *res = a < b; return 1;
  case TOK_GE: *res = a >= b; return 1;
  case TOK_LE: *res = a <= b; return 1;
  case TOK_GT: *res = a > b; return 1;
  case TOK_ULT: *res = (uint32_t)a < (uint32_t)b; return 1;
  case TOK_UGE: *res = (uint32_t)a >= (uint32_t)b; return 1;
  case TOK_ULE: *res = (uint32_t)a <= (uint32_t)b; return 1;
  case TOK_UGT: *res = (uint32_t)a > (uint32_t)b; return 1;
  }
  return 0;
}

/* Constants known on entry: `V <- #imm` in the straight-line code just above
 * START (nearest definition wins; any other definition makes V unknown). */
static int mx_entry_state(TCCIRState *ir, int start, int end, MxState *s)
{
  int steps = 0;
  for (int i = start - 1; i >= 0 && steps < 96; i--)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
    {
      if (q->is_jump_target)
        break;
      continue;
    }
    steps++;
    if (q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_JUMPIF || ir_op_has(q->op, IR_HZ_RETURN | IR_HZ_NONLOCAL) ||
        ir_op_has(q->op, IROP_A_NO_FALLTHROUGH | IROP_A_RETURNS_TWICE | IROP_A_MAY_BRANCH))
      break;
    if (irop_config[q->op].has_dest)
    {
      IROperand d = tcc_ir_op_get_dest(ir, q);
      int32_t dv = irop_get_vreg(d);
      if (dv >= 0 && irop_dest_defines_vreg(d) && mx_find(s, dv) < 0)
      {
        int used = 0;
        for (int j = start; j <= end && !used; j++)
          used = insn_mentions_vreg(ir, &ir->compact_instructions[j], dv);
        if (!used) {
          if (q->is_jump_target)
            break;
          continue;
        }
        int known = 0;
        int32_t v = 0;
        if (q->op == TCCIR_OP_ASSIGN && irop_get_btype(d) == IROP_BTYPE_INT32 && !d.is_llocal)
        {
          IROperand src = tcc_ir_op_get_src1(ir, q);
          if (irop_is_immediate(src) && irop_get_btype(src) == IROP_BTYPE_INT32)
          {
            known = 1;
            v = (int32_t)irop_get_imm64_ex(ir, src);
          }
        }
        if (!mx_set(s, dv, known, v))
          return 0;
      }
    }
    if (q->is_jump_target)
      break;
  }
  return 1;
}

/* Trips the loop [start..end] makes before leaving, simulated on the entry
 * constants; 0 when the control cannot be decided or exceeds MX_MAX_TRIP. */
static int mx_walk(TCCIRState *ir, int start, int end, int rewrite, const MxEsc *esc)
{
  MxState st;
  memset(&st, 0, sizeof st);
  st.esc = esc;
  if (!mx_entry_state(ir, start, end, &st))
    return 0;
  int back = 0, pc = start, steps = 0;
  while (steps++ < 4096)
  {
    if (pc < start || pc > end)
      return back + 1;
    IRQuadCompact *q = &ir->compact_instructions[pc];
    if (q->op == TCCIR_OP_NOP)
    {
      pc++;
      continue;
    }
    if (q->op == TCCIR_OP_JUMP)
    {
      int t = (int)tcc_ir_op_dest_imm(ir, q);
      if (pc == end && !rewrite)
      {
        if (++back >= MX_MAX_TRIP)
          return 0;
        pc = start;
      }
      else
        pc = t;
      continue;
    }
    if (q->op == TCCIR_OP_CMP || q->op == TCCIR_OP_TEST_ZERO)
    {
      int j = pc + 1;
      while (j <= end && ir->compact_instructions[j].op == TCCIR_OP_NOP)
        j++;
      if (j >= end)
        return 0;
      IRQuadCompact *use = &ir->compact_instructions[j];
      if (use->op != TCCIR_OP_JUMPIF && use->op != TCCIR_OP_SETIF)
        return 0;
      int32_t a, b;
      int res;
      // A barrel-fused comparison's operands alone do not describe its flags.
      if (mx_has_barrel(ir, q))
        return 0;
      b = 0;
      if (!mx_read(ir, &st, tcc_ir_op_get_src1(ir, q), &a) ||
          (q->op == TCCIR_OP_CMP && !mx_read(ir, &st, tcc_ir_op_get_src2(ir, q), &b)) ||
          !mx_cond((int)tcc_ir_op_src1_imm(ir, use), a, b, &res))
        return 0;
      if (use->op == TCCIR_OP_SETIF)
      {
        IROperand d = tcc_ir_op_get_dest(ir, use);
        int32_t dv = irop_get_vreg(d);
        if (dv < 0 || !irop_dest_defines_vreg(d) || irop_get_btype(d) != IROP_BTYPE_INT32 ||
            tcc_ir_operand_names_volatile_var(ir, d) || !mx_set(&st, dv, 1, res))
          return 0;
        pc = j + 1;
      }
      else
        pc = res ? (int)tcc_ir_op_dest_imm(ir, use) : j + 1;
      continue;
    }
    /* A conditional branch not decided by the CMP just above it (flags
     * from another op): the simulation cannot follow it. */
    if (q->op == TCCIR_OP_JUMPIF)
      return 0;
    if (rewrite && (q->op == TCCIR_OP_LOAD_INDEXED || q->op == TCCIR_OP_STORE_INDEXED) && !mx_has_barrel(ir, q))
    {
      /* An index that is a known constant here becomes an immediate, so the
       * address folds to a constant offset into its base. */
      IROperand ix = tcc_ir_op_get_src2(ir, q);
      int32_t iv;
      if (!irop_is_immediate(ix) && mx_read(ir, &st, ix, &iv))
        tcc_ir_op_set_src2(ir, q, irop_make_imm32(-1, iv, IROP_BTYPE_INT32));
    }
    /* Likewise an address `base + i` spelled as an ADD (the base hoisted out
     * of an enclosing loop): the access becomes base + #k, which SRA and the
     * stack forwarding see as a constant offset. */
    if (rewrite && (q->op == TCCIR_OP_ADD || q->op == TCCIR_OP_SUB) && !mx_has_barrel(ir, q) &&
        irop_get_btype(tcc_ir_op_get_dest(ir, q)) == IROP_BTYPE_INT32 && !irop_is_immediate(tcc_ir_op_get_src1(ir, q)))
    {
      IROperand k = tcc_ir_op_get_src2(ir, q);
      int32_t kv;
      if (!irop_is_immediate(k) && mx_read(ir, &st, k, &kv))
        tcc_ir_op_set_src2(ir, q, irop_make_imm32(-1, kv, IROP_BTYPE_INT32));
    }
    if (irop_config[q->op].has_dest)
    {
      IROperand d = tcc_ir_op_get_dest(ir, q);
      int32_t dv = irop_get_vreg(d);
      if (dv >= 0 && irop_dest_defines_vreg(d))
      {
        int known = 0;
        int32_t v = 0, a, b;
        if (irop_get_btype(d) == IROP_BTYPE_INT32 && !d.is_llocal && !mx_has_barrel(ir, q))
        {
          IROperand s1 = irop_config[q->op].has_src1 ? tcc_ir_op_get_src1(ir, q) : (IROperand){0};
          IROperand s2 = irop_config[q->op].has_src2 ? tcc_ir_op_get_src2(ir, q) : (IROperand){0};
          switch (q->op)
          {
          case TCCIR_OP_ASSIGN:
          case TCCIR_OP_LOAD:
            if (mx_read(ir, &st, s1, &a))
              known = 1, v = a;
            break;
          case TCCIR_OP_ADD:
            if (mx_read(ir, &st, s1, &a) && mx_read(ir, &st, s2, &b))
              known = 1, v = (int32_t)((uint32_t)a + (uint32_t)b);
            break;
          case TCCIR_OP_SUB:
            if (mx_read(ir, &st, s1, &a) && mx_read(ir, &st, s2, &b))
              known = 1, v = (int32_t)((uint32_t)a - (uint32_t)b);
            break;
          case TCCIR_OP_AND:
            if (mx_read(ir, &st, s1, &a) && mx_read(ir, &st, s2, &b))
              known = 1, v = a & b;
            break;
          case TCCIR_OP_OR:
            if (mx_read(ir, &st, s1, &a) && mx_read(ir, &st, s2, &b))
              known = 1, v = a | b;
            break;
          default:
            break;
          }
        }
        if ((known || mx_find(&st, dv) >= 0) && !mx_set(&st, dv, known, v))
          return 0;
      }
    }
    pc++;
  }
  return 0;
}

/* Whether TEMP VR can be renamed in each copy: one definition inside
 * [start..end], every mention inside, every use after the def with no join
 * between (so the def dominates every use). */
static int mx_temp_local(TCCIRState *ir, int start, int end, int32_t vr)
{
  int def = -1, last_use = -1;
  for (int i = 0; i < ir->next_instruction_index; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;
    int is_def = 0, is_use = 0;
    for (int k = 0; k < 4; k++)
    {
      IROperand o;
      if (k == 0)
      {
        if (!irop_config[q->op].has_dest)
          continue;
        o = tcc_ir_op_get_dest(ir, q);
      }
      else if (k == 1)
      {
        if (!irop_config[q->op].has_src1)
          continue;
        o = tcc_ir_op_get_src1(ir, q);
      }
      else if (k == 2)
      {
        if (!irop_config[q->op].has_src2)
          continue;
        o = tcc_ir_op_get_src2(ir, q);
      }
      else
      {
        if (!ir_op_has(q->op, IROP_A_SLOT3))
          continue;
        o = ir->iroperand_pool[q->operand_base + 3];
      }
      if (irop_get_vreg(o) != vr)
        continue;
      if (k == 0 && irop_dest_defines_vreg(o))
        is_def = 1;
      else
        is_use = 1;
    }
    if (!is_def && !is_use)
      continue;
    if (i < start || i > end)
      return 0;
    if (is_def)
    {
      if (def >= 0 || is_use)
        return 0;
      def = i;
    }
    else
    {
      if (def < 0)
        return 0;
      last_use = i;
    }
  }
  if (def < 0)
    return 0;
  for (int i = def + 1; i <= last_use; i++)
    if (ir->compact_instructions[i].is_jump_target)
      return 0;
  return 1;
}

/* A value read of a vreg (register, or a variable in its slot): what the
 * simulation and the store forwarding below treat as "the value of VR". */
static int mx_value_read(IROperand o)
{
  int tag = irop_get_tag(o);
  return irop_get_vreg(o) >= 0 && !o.is_llocal && (tag == IROP_TAG_VREG ? !o.is_lval : tag == IROP_TAG_STACKOFF && o.is_lval);
}

/* Operand slot K (0 dest, 1 src1, 2 src2, 3 fourth) of Q into *O; 0 if absent. */
static int mx_slot(TCCIRState *ir, IRQuadCompact *q, int k, IROperand *o)
{
  if (k == 0 ? !irop_config[q->op].has_dest
             : k == 1 ? !irop_config[q->op].has_src1 : k == 2 ? !irop_config[q->op].has_src2 : !ir_op_has(q->op, IROP_A_SLOT3))
    return 0;
  *o = k == 0 ? tcc_ir_op_get_dest(ir, q)
              : k == 1 ? tcc_ir_op_get_src1(ir, q) : k == 2 ? tcc_ir_op_get_src2(ir, q) : ir->iroperand_pool[q->operand_base + 3];
  return 1;
}

#define MX_MAX_FWD_READS 4

/* An inlined helper's narrow parameter (Zig's zig_u32_intCast_u8) binds its
 * argument with `V <- x [STORE]` into a byte/halfword VAR.  SSA keeps such a
 * STORE as one in-place-renamed multi-def TEMP (ssa_store_slot_def_pos),
 * sound for the single copy a loop has, not for N copies of it in straight
 * line (the fuzz 308/191/212 family, see collect_body_instructions).  When
 * the VAR lives only between the STORE and its unsigned reads, with no join
 * and no redefinition of X in between, each read is `x & mask`: forward it
 * and drop the STORE.  X may also be a byte/halfword memory read (`V <- *p`):
 * each read then becomes that load, zero-extending into a value of V's own
 * type, with no memory write in between.  Fills READS for STORE S; 1 if
 * forwardable. */
static int mx_narrow_store_plan(TCCIRState *ir, int s, int end, const MxEsc *esc, int *reads, int *nreads)
{
  IRQuadCompact *sq = &ir->compact_instructions[s];
  IROperand d = tcc_ir_op_get_dest(ir, sq), x = tcc_ir_op_get_src1(ir, sq);
  int32_t dv = irop_get_vreg(d), xv = irop_get_vreg(x);
  int bt = irop_get_btype(d);
  if (dv < 0 || TCCIR_DECODE_VREG_TYPE(dv) != TCCIR_VREG_TYPE_VAR || !irop_dest_defines_vreg(d) || d.is_llocal ||
      (bt != IROP_BTYPE_INT8 && bt != IROP_BTYPE_INT16) || mx_escaped(ir, esc, dv))
    return 0;
  int x_imm = irop_get_tag(x) == IROP_TAG_IMM32 && !x.is_lval;
  /* A load through a pointer vreg at V's own width. */
  int x_mem = irop_get_tag(x) == IROP_TAG_VREG && x.is_lval && !x.is_llocal && xv >= 0 && irop_get_btype(x) == bt;
  if (x_mem && (mx_escaped(ir, esc, xv) || tcc_ir_access_is_volatile(ir, x)))
    return 0;
  /* X read at any integer width up to a word: the STORE keeps its low
   * bits, and so does `x & mask` whatever extension the read applied. */
  int xbt = irop_get_btype(x);
  if (!x_imm && !x_mem &&
      (!mx_value_read(x) || mx_escaped(ir, esc, xv) ||
       (xbt != IROP_BTYPE_INT32 && xbt != IROP_BTYPE_INT16 && xbt != IROP_BTYPE_INT8)))
    return 0;
  *nreads = 0;
  int last = s;
  for (int i = 0; i < ir->next_instruction_index; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP || i == s)
      continue;
    int mentions = 0;
    for (int k = 0; k < 4; k++)
    {
      IROperand o;
      if (mx_slot(ir, q, k, &o) && irop_get_vreg(o) == dv)
        mentions |= 1 << k;
    }
    if (!mentions)
      continue;
    /* Only `T <- V` value reads after the STORE, in the loop, unsigned at V's width. */
    if (i < s || i > end || mentions != 2 || (q->op != TCCIR_OP_LOAD && q->op != TCCIR_OP_ASSIGN) ||
        *nreads >= MX_MAX_FWD_READS)
      return 0;
    /* The forwarded value `x & mask` is V's zero extension: the read must
     * zero-extend, into a word or an unsigned value at least V's width. */
    IROperand r = tcc_ir_op_get_src1(ir, q), rd = tcc_ir_op_get_dest(ir, q);
    int rbt = irop_get_btype(rd);
    int rd_ok = rbt == IROP_BTYPE_INT32 || (rd.is_unsigned && (rbt == bt || (rbt == IROP_BTYPE_INT16 && bt == IROP_BTYPE_INT8)));
    if (x_mem) /* the read becomes the load itself: typed exactly like one */
      rd_ok = rd.is_unsigned && rbt == bt;
    if (!mx_value_read(r) || irop_get_btype(r) != bt || !r.is_unsigned || irop_get_vreg(rd) < 0 ||
        !irop_dest_defines_vreg(rd) || !rd_ok || rd.is_llocal ||
        irop_get_vreg(rd) == dv || (!x_imm && irop_get_vreg(rd) == xv))
      return 0;
    reads[(*nreads)++] = i;
    last = i;
  }
  if (!*nreads)
    return 0;
  /* A forwarded load reads memory later than the STORE did: nothing may
   * write it, call, or touch volatile memory in between. */
  const uint32_t mem_hz = IR_HZ_MEM_WRITE | IR_HZ_CALL | IR_HZ_CALL_PARAM | IR_HZ_CALL_SEQ | IR_HZ_UPDATES_SRC |
                          IR_HZ_ASM | IR_HZ_VLA | IR_HZ_NONLOCAL | IR_HZ_CHAIN | IR_HZ_VOLATILE | IR_HZ_DEST_LVAL |
                          IR_HZ_DEST_STACKOFF;
  for (int i = s + 1; i <= last; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->is_jump_target)
      return 0;
    if (x_mem && i < last && q->op != TCCIR_OP_NOP && ir_q_hazards(ir, q, mem_hz))
      return 0;
    if (q->op == TCCIR_OP_NOP || x_imm || !irop_config[q->op].has_dest)
      continue;
    IROperand o = tcc_ir_op_get_dest(ir, q);
    if (irop_get_vreg(o) == xv && i < last)
      return 0; /* X redefined before a read */
  }
  return 1;
}

static void mx_narrow_store_apply(TCCIRState *ir, int s, const int *reads, int nreads)
{
  IRQuadCompact *sq = &ir->compact_instructions[s];
  IROperand d = tcc_ir_op_get_dest(ir, sq), x = tcc_ir_op_get_src1(ir, sq);
  uint32_t mask = irop_get_btype(d) == IROP_BTYPE_INT8 ? 0xffu : 0xffffu;
  for (int r = 0; r < nreads; r++)
  {
    IRQuadCompact *q = &ir->compact_instructions[reads[r]];
    IROperand rd = tcc_ir_op_get_dest(ir, q);
    uint32_t line = q->line_num;
    q->op = TCCIR_OP_NOP; /* write_instr_at_nop fills a NOP slot */
    if (irop_get_tag(x) == IROP_TAG_VREG && x.is_lval)
    {
      IROperand ld = x;
      ld.is_unsigned = 1;
      write_instr_at_nop(ir, reads[r], TCCIR_OP_LOAD, rd, ld, irop_make_imm32(-1, 0, IROP_BTYPE_INT32));
    }
    else if (irop_get_tag(x) == IROP_TAG_IMM32)
      write_instr_at_nop(ir, reads[r], TCCIR_OP_ASSIGN, rd,
                         irop_make_imm32(-1, (int32_t)((uint32_t)irop_get_imm64_ex(ir, x) & mask), IROP_BTYPE_INT32),
                         (IROperand){0});
    else
      write_instr_at_nop(ir, reads[r], TCCIR_OP_AND, rd, x, irop_make_imm32(-1, (int32_t)mask, IROP_BTYPE_INT32));
    ir->compact_instructions[reads[r]].line_num = line;
  }
  ir->compact_instructions[s].op = TCCIR_OP_NOP;
}

#define MX_MAX_FWD 8

static int mx_unroll(TCCIRState *ir, int start, int end)
{
  /* Structure: [start..end] ends in the only backward jump, to its first
   * instruction; nothing else enters or leaves except forward jumps/exits. */
  if (end <= start || end - start > SSA_UNROLL_MAX_SPAN)
    return 0;
  IRQuadCompact *eq = &ir->compact_instructions[end];
  if (eq->op != TCCIR_OP_JUMP || next_real(ir, (int)tcc_ir_op_dest_imm(ir, eq), end) != next_real(ir, start, end))
    return 0;
  int real = 0;
  for (int i = start; i <= end; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;
    real++;
    int is_jump = q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_JUMPIF;
    if (ir_op_has(q->op, IR_HZ_RETURN | IR_HZ_CALL | IR_HZ_CALL_PARAM | IR_HZ_CALL_SEQ | IR_HZ_UPDATES_SRC | IR_HZ_ASM |
                             IR_HZ_VLA | IR_HZ_NONLOCAL | IR_HZ_CHAIN | IR_HZ_TRAP | IR_HZ_HINT) ||
        ir_op_has(q->op, IROP_A_RETURNS_TWICE | IROP_A_MAY_BRANCH))
      return 0;
    if (!is_jump && ir_op_has(q->op, IR_HZ_BRANCH | IROP_A_NO_FALLTHROUGH))
      return 0;
    if (ir_op_has(q->op, IROP_A_SLOT3) &&
        !(irop_config[q->op].has_dest && irop_config[q->op].has_src1 && irop_config[q->op].has_src2))
      return 0;
    if ((q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_JUMPIF) && i != end)
    {
      int t = (int)tcc_ir_op_dest_imm(ir, q);
      if (t <= i && t >= start)
        return 0; /* another back edge */
      if (t < start)
        return 0; /* backward out of the loop */
    }
  }
  for (int i = 0; i < ir->next_instruction_index; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if ((q->op != TCCIR_OP_JUMP && q->op != TCCIR_OP_JUMPIF) || (i >= start && i <= end))
      continue;
    int t = (int)tcc_ir_op_dest_imm(ir, q);
    if (t >= start && t <= end)
      return 0; /* entered other than by falling into the header */
  }
  for (int ti = 0; ti < ir->num_switch_tables; ti++)
  {
    TCCIRSwitchTable *tb = &ir->switch_tables[ti];
    if (tb->default_target >= start && tb->default_target <= end)
      return 0;
    for (int tj = 0; tj < tb->num_entries; tj++)
      if (tb->targets[tj] >= start && tb->targets[tj] <= end)
        return 0;
  }
  MxEsc esc;
  mx_collect_escaped(ir, &esc);
  int trips = mx_walk(ir, start, end, 0, &esc);
  if (trips < 2 || trips > MX_MAX_TRIP || trips * real > MX_MAX_TOTAL)
    return 0;

  /* A STORE defining a vreg in the body: a narrow VAR bound by an inlined
   * helper is forwarded (mx_narrow_store_plan); anything else stays a loop. */
  int fwd_s[MX_MAX_FWD], fwd_r[MX_MAX_FWD][MX_MAX_FWD_READS], fwd_n[MX_MAX_FWD], nfwd = 0;
  for (int i = start; i <= end; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_STORE)
      continue;
    IROperand d = tcc_ir_op_get_dest(ir, q);
    if (irop_get_vreg(d) < 0 || !irop_dest_defines_vreg(d))
      continue;
    if (nfwd >= MX_MAX_FWD || !mx_narrow_store_plan(ir, i, end, &esc, fwd_r[nfwd], &fwd_n[nfwd]))
      return 0;
    fwd_s[nfwd++] = i;
  }

  /* Every TEMP the body defines must be private to one iteration: it gets a
   * fresh vreg in every copy but the first (straight-line copies of one
   * TEMP def are exactly the multi-def shape SSA mis-folds). */
  int32_t rn_old[MX_MAX_RENAME];
  int rn_count = 0;
  for (int i = start; i <= end; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP || !irop_config[q->op].has_dest)
      continue;
    IROperand d = tcc_ir_op_get_dest(ir, q);
    int32_t vr = irop_get_vreg(d);
    if (vr < 0 || TCCIR_DECODE_VREG_TYPE(vr) != TCCIR_VREG_TYPE_TEMP || !irop_dest_defines_vreg(d))
      continue;
    int seen = 0;
    for (int r = 0; r < rn_count; r++)
      seen |= rn_old[r] == vr;
    if (seen)
      continue;
    if (rn_count >= MX_MAX_RENAME || !mx_temp_local(ir, start, end, vr))
      return 0;
    rn_old[rn_count++] = vr;
  }

  for (int f = 0; f < nfwd; f++)
    mx_narrow_store_apply(ir, fwd_s[f], fwd_r[f], fwd_n[f]);

  const int L = end - start + 1;
  const int extra = (trips - 1) * L;
  for (int k = 0; k < extra; k++)
    if (insert_instr_at(ir, end + 1, TCCIR_OP_NOP, (IROperand){0}, (IROperand){0}, (IROperand){0}) < 0)
      return 1; /* the forwarding above already changed the (still correct) loop */

  for (int k = 1; k < trips; k++)
  {
    int32_t rn_new[MX_MAX_RENAME];
    for (int r = 0; r < rn_count; r++)
      rn_new[r] = tcc_ir_vreg_alloc_temp(ir);
    for (int i = start; i < end; i++)
    {
      IRQuadCompact *q = &ir->compact_instructions[i];
      if (q->op == TCCIR_OP_NOP)
        continue;
      int op = q->op, slot = i + k * L;
      IROperand o[4];
      memset(o, 0, sizeof o);
      for (int x = 0; x < 4; x++)
        mx_slot(ir, q, x, &o[x]);
      int has4 = ir_op_has(op, IROP_A_SLOT3);
      for (int x = 0; x < 4; x++)
      {
        int32_t vr = irop_get_vreg(o[x]);
        if (vr < 0)
          continue;
        for (int r = 0; r < rn_count; r++)
          if (rn_old[r] == vr)
            irop_set_vreg(&o[x], rn_new[r]);
      }
      if (op == TCCIR_OP_JUMP || op == TCCIR_OP_JUMPIF)
      {
        int t = (int)tcc_ir_op_dest_imm(ir, q);
        if (t >= start && t <= end)
          t += k * L;
        o[0] = irop_make_imm32(-1, t, IROP_BTYPE_INT32);
      }
      int orig = q->orig_index;
      uint32_t line = q->line_num;
      write_instr_at_nop(ir, slot, (TccIrOp)op, o[0], o[1], o[2]);
      if (has4)
        tcc_ir_pool_add(ir, o[3]);
      IRQuadCompact *nq = &ir->compact_instructions[slot];
      nq->line_num = line;
      tcc_ir_copy_orig_annotations(ir, orig, nq->orig_index);
    }
  }
  /* Jump-target flags last: write_instr_at_nop clears the flag of the slot
   * it fills, so a forward jump's target written after the jump lost it. */
  for (int i = start + L; i <= end + extra; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_JUMPIF)
    {
      int t = (int)tcc_ir_op_dest_imm(ir, q);
      if (t > end && t <= end + extra)
        ir->compact_instructions[t].is_jump_target = 1;
    }
  }
  /* The back edge is gone from every copy (the last one never took it). */
  ir->compact_instructions[end].op = TCCIR_OP_NOP;
  mx_walk(ir, start, end + extra, 1, &esc);
  tcc_ir_frame_scope_widen(ir, start, end + extra);
  LOG_LOOP_OPT("loop_unroll_midexit: loop [%d..%d] unrolled x%d", start, end, trips);
  return 1;
}

static int mx_try_candidate(TCCIRState *ir, IRCFG *cfg, int header_b, uint8_t *member, uint8_t *scratch)
{
  /* Unlike lcs_cand_span the range may hold non-member blocks (the exit
   * block of a mid-exit loop sits inside it); mx_unroll's own entry checks
   * make the header the only way in. */
  lcs_collect_header_members(cfg, header_b, member, scratch);
  int start = ir->next_instruction_index, end = -1;
  for (int b = 0; b < cfg->num_blocks; b++)
  {
    if (!member[b])
      continue;
    if (cfg->blocks[b].start_idx < start)
      start = cfg->blocks[b].start_idx;
    if (cfg->blocks[b].end_idx - 1 > end)
      end = cfg->blocks[b].end_idx - 1;
    if (!tcc_ir_cfg_dominates(cfg, header_b, b))
      return 0;
  }
  if (end < start || start != cfg->blocks[header_b].start_idx)
    return 0;
  return mx_unroll(ir, start, end);
}

int ssa_opt_loop_unroll_midexit(TCCIRState *ir)
{
  /* Leaf loops: Zig's readInt loop also sits inside other loops (blake3's
   * message-word loop). */
  return lcs_run_leaf(ir, 24, 1, mx_try_candidate);
}
