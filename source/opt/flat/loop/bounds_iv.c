/*
 *  TCC IR - a compare its loop's own counter decides (flat, pre-SSA)
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

/* ssa:bounds_iv -- -fsanitize=bounds' check of a[i] (`i <u n`, a trap on the
 * other side) inside `for (i = 0; i < n; i++)`, folded on flat IR right after
 * rotation.  ssa:vrp proves the same (sv_try_fold_iv_bound), but after the
 * loop passes: with the check inside, ssa:loop_unroll keeps a loop of 16
 * stores and IV strength reduction keeps the index (and its compare) next to
 * the walking pointer.  Folded here, they see the loop the program would be
 * without -fsanitize.
 *
 * The loop (tcc_ir_detect_loops): a contiguous [start, end] entered only by
 * falling into start, whose counter V has one definition inside it,
 * `V <- V + 1` (through a copy), right before its last two instructions, the
 * back edge `CMP V, Y ; JUMPIF <S|<U start`; Y is a constant or a register
 * nothing in the loop writes (and whose address is not taken).  Each way in
 * brings V below Y: a guard `CMP V|#c, Y ; JUMPIF >=` on the instruction
 * before start, or a constant `V <- #c` falling into start with c < Y.  So at
 * every trip V < Y until its increment, which is the last thing the trip does
 * before the test.  Signed compares also need V >= 0 (a known constant start
 * >= 0); unsigned ones decide only unsigned compares (x = 5 is <u 0x80000000
 * but not <s).
 *
 * A compare before the increment reading V (or a copy of it made in the trip)
 * against Y, or against a constant at or above a constant Y, is decided: the
 * CMP goes, its JUMPIF becomes a JUMP or goes too.  The trap left behind is
 * unreachable; the passes after remove it. */

#include "ir.h"
#include "cfg.h"
#include "opt.h"
#include "opt_utils.h"
#include "opt_loop_utils.h"

#define BIV_MAX_BACK 16 /* instructions searched back from start for the entry value */

static int biv_next_real(TCCIRState *ir, int i, int limit)
{
  for (; i < limit; i++)
    if (ir->compact_instructions[i].op != TCCIR_OP_NOP)
      return i;
  return -1;
}

static int biv_prev_real(TCCIRState *ir, int i, int limit)
{
  for (; i >= limit; i--)
    if (ir->compact_instructions[i].op != TCCIR_OP_NOP)
      return i;
  return -1;
}

/* a plain read of a register's value: no memory access through it (a VAR
   with a frame home reads as is_lval + is_local, its own slot) */
static int biv_plain_vreg(IROperand op)
{
  int32_t vr = irop_get_vreg(op);
  if (vr < 0 || irop_is_immediate(op) || irop_is_sym(op) || irop_is_llocal(op) || op.btype != IROP_BTYPE_INT32)
    return 0;
  if (op.is_lval)
    return TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_VAR && irop_is_local(op);
  return 1;
}

static int biv_plain_imm(TCCIRState *ir, IROperand op, int64_t *v)
{
  if (!irop_is_immediate(op) || !irop_is_plain_imm(op) || op.btype == IROP_BTYPE_INT64)
    return 0;
  *v = (int64_t)(int32_t)irop_get_imm64_ex(ir, op);
  return 1;
}

/* the instruction defining vreg VR when it is a plain definition (not a store
   through it) */
static int biv_defines(TCCIRState *ir, IRQuadCompact *q, int32_t vr)
{
  if (q->op == TCCIR_OP_NOP || !irop_config[q->op].has_dest || q->op == TCCIR_OP_FUNCPARAMVAL ||
      q->op == TCCIR_OP_FUNCPARAMVOID)
    return 0;
  IROperand d = tcc_ir_op_get_dest(ir, q);
  if (q->op == TCCIR_OP_STORE_INDEXED || q->op == TCCIR_OP_STORE_POSTINC || (q->op == TCCIR_OP_STORE && d.is_lval))
    return 0;
  return irop_get_vreg(d) == vr;
}

static int biv_count_defs(TCCIRState *ir, int32_t vr, int from, int to, int *last)
{
  int n = 0;
  for (int i = from; i <= to; i++)
    if (biv_defines(ir, &ir->compact_instructions[i], vr))
    {
      n++;
      if (last)
        *last = i;
    }
  return n;
}

static int biv_addrtaken(TCCIRState *ir, int32_t vr)
{
  if (TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_TEMP)
    return 0;
  IRLiveInterval *li = tcc_ir_try_get_live_interval(ir, vr);
  return !li || li->addrtaken;
}

/* each TEMP's definitions in the function: count and the last one, filled
   once per run (a TEMP is not always single-def: `T <-- x [STORE]`) */
static int *biv_tdefs, *biv_tlast, biv_ntemps;

static void biv_scan_temps(TCCIRState *ir)
{
  int n = ir->next_instruction_index, max = -1;
  for (int pass = 0; pass < 2; pass++)
  {
    for (int i = 0; i < n; i++)
    {
      IRQuadCompact *q = &ir->compact_instructions[i];
      if (q->op == TCCIR_OP_NOP || !irop_config[q->op].has_dest)
        continue;
      int32_t vr = irop_get_vreg(tcc_ir_op_get_dest(ir, q));
      if (vr < 0 || TCCIR_DECODE_VREG_TYPE(vr) != TCCIR_VREG_TYPE_TEMP || !biv_defines(ir, q, vr))
        continue;
      int pos = TCCIR_DECODE_VREG_POSITION(vr);
      if (pass == 0)
        max = pos > max ? pos : max;
      else
      {
        biv_tdefs[pos]++;
        biv_tlast[pos] = i;
      }
    }
    if (pass == 0)
    {
      biv_ntemps = max + 1;
      biv_tdefs = tcc_mallocz(sizeof(int) * (biv_ntemps > 0 ? biv_ntemps : 1));
      biv_tlast = tcc_mallocz(sizeof(int) * (biv_ntemps > 0 ? biv_ntemps : 1));
    }
  }
}

/* VR through copies `T <- V` (ASSIGN or LOAD of a plain read), each T's
   only definition, made in [from, to): the first register that is not such
   a copy */
static int32_t biv_through_copy(TCCIRState *ir, int32_t vr, int from, int to)
{
  for (int hop = 0; hop < 4; hop++)
  {
    int pos = TCCIR_DECODE_VREG_POSITION(vr);
    if (TCCIR_DECODE_VREG_TYPE(vr) != TCCIR_VREG_TYPE_TEMP || pos >= biv_ntemps || biv_tdefs[pos] != 1 ||
        biv_tlast[pos] < from || biv_tlast[pos] >= to)
      return vr;
    IRQuadCompact *q = &ir->compact_instructions[biv_tlast[pos]];
    IROperand s = tcc_ir_op_get_src1(ir, q);
    if ((q->op != TCCIR_OP_ASSIGN && q->op != TCCIR_OP_LOAD) || !biv_plain_vreg(s))
      return vr;
    vr = irop_get_vreg(s);
  }
  return vr;
}

/* The check read the counter through a copy `T <- V` (a sanitizer check uses
 * its index twice): with the check gone, T's other reads -- all in this trip,
 * before V's increment -- read V again, as the code without the check does,
 * so the loop passes see the counter itself as the index. */
static int biv_forward_copy(TCCIRState *ir, int32_t tvr, int from, int def)
{
  int pos = TCCIR_DECODE_VREG_POSITION(tvr), copy, n = ir->next_instruction_index;
  if (TCCIR_DECODE_VREG_TYPE(tvr) != TCCIR_VREG_TYPE_TEMP || pos >= biv_ntemps || biv_tdefs[pos] != 1)
    return 0;
  copy = biv_tlast[pos];
  if (copy < from || copy >= def)
    return 0;
  IRQuadCompact *cq = &ir->compact_instructions[copy];
  IROperand src = tcc_ir_op_get_src1(ir, cq);
  if ((cq->op != TCCIR_OP_ASSIGN && cq->op != TCCIR_OP_LOAD) || !biv_plain_vreg(src))
    return 0;
  /* every read: a plain src1/src2 in (copy, def) */
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP || i == copy)
      continue;
    /* an indexed access's 4th operand is its scale, an immediate; MLA's is
       a register (the accumulator) */
    for (int k = 0; k < 4; k++)
    {
      if (k == 0 ? !irop_config[q->op].has_dest
                 : k == 1 ? !irop_config[q->op].has_src1
                 : k == 2 ? !irop_config[q->op].has_src2
                          : q->op != TCCIR_OP_MLA)
        continue;
      IROperand o = k == 3 ? tcc_ir_op_get_accum(ir, q) : tcc_ir_op_get_slot(ir, q, k);
      if (irop_get_vreg(o) != tvr)
        continue;
      if (k == 0 || k == 3 || i <= copy || i >= def || o.is_lval || irop_is_local(o) || irop_is_llocal(o))
        return 0;
    }
  }
  for (int i = copy + 1; i < def; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;
    if (irop_config[q->op].has_src1 && irop_get_vreg(tcc_ir_op_get_src1(ir, q)) == tvr)
      tcc_ir_op_set_src1(ir, q, src);
    if (irop_config[q->op].has_src2 && irop_get_vreg(tcc_ir_op_get_src2(ir, q)) == tvr)
      tcc_ir_op_set_src2(ir, q, src);
  }
  cq->op = TCCIR_OP_NOP;
  biv_tdefs[pos] = 0;
  /* a copy of a copy: that one goes the same way */
  if (TCCIR_DECODE_VREG_TYPE(irop_get_vreg(src)) == TCCIR_VREG_TYPE_TEMP)
    biv_forward_copy(ir, irop_get_vreg(src), from, def);
  return 1;
}

/* Does any jump but the one at SKIP land on T? */
static int biv_jumped_to(TCCIRState *ir, int t, int skip)
{
  int n = ir->next_instruction_index;
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (i == skip)
      continue;
    if ((q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_JUMPIF) && (int)tcc_ir_op_dest_imm(ir, q) == t)
      return 1;
    if (q->op == TCCIR_OP_IJUMP || q->op == TCCIR_OP_SWITCH_TABLE)
      return 1;
  }
  return 0;
}

/* Is any instruction outside [start, end] a jump into (start, end]? */
static int biv_entered_from_outside(TCCIRState *ir, int start, int end)
{
  int n = ir->next_instruction_index;
  for (int i = 0; i < n; i++)
  {
    if (i >= start && i <= end)
      continue;
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_JUMPIF)
    {
      int t = (int)tcc_ir_op_dest_imm(ir, q);
      if (t >= start && t <= end)
        return 1;
    }
    else if (q->op == TCCIR_OP_IJUMP || q->op == TCCIR_OP_SWITCH_TABLE)
      return 1;
  }
  return 0;
}

/* Two operands with one value: one register, or one constant. */
static int biv_same(TCCIRState *ir, IROperand a, IROperand b)
{
  int64_t x, y;
  if (biv_plain_imm(ir, a, &x) && biv_plain_imm(ir, b, &y))
    return x == y;
  return biv_plain_vreg(a) && biv_plain_vreg(b) && irop_get_vreg(a) == irop_get_vreg(b);
}

/* The CMP before the JUMPIF at J, read as `v TOK y` for the counter V
   (through a copy made in [from, J)): 1 and the CMP's index, or 0 */
static int biv_cmp_of(TCCIRState *ir, int j, int32_t v, int from, IROperand *y, int *tok, int *cmp_idx)
{
  int c = biv_prev_real(ir, j - 1, from);
  if (c < 0)
    return 0;
  IRQuadCompact *cq = &ir->compact_instructions[c];
  if (cq->op != TCCIR_OP_CMP || tcc_ir_barrel_shift_at(ir, cq) || tcc_ir_instr_access_is_volatile(ir, cq))
    return 0;
  IROperand a = tcc_ir_op_get_src1(ir, cq), b = tcc_ir_op_get_src2(ir, cq);
  int t = (int)tcc_ir_op_src1_imm(ir, &ir->compact_instructions[j]);
  if (biv_plain_vreg(a) && biv_through_copy(ir, irop_get_vreg(a), from, c) == v)
    *y = b;
  else if (biv_plain_vreg(b) && biv_through_copy(ir, irop_get_vreg(b), from, c) == v)
  {
    *y = a;
    t = vrp_swap_cmp_tok(t);
  }
  else
    return 0;
  *tok = t;
  *cmp_idx = c;
  return t >= 0;
}

/* V's constant on the straight line falling into START: `V <- #c` reached
   only by falling through (no join between it and START; a guard's JUMPIF
   leaves that line, it does not join it) */
static int biv_entry_const(TCCIRState *ir, int32_t v, int start, int64_t *c)
{
  for (int i = start - 1, k = 0; i >= 0 && k < BIV_MAX_BACK; i--, k++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (biv_defines(ir, q, v))
      return (q->op == TCCIR_OP_ASSIGN || q->op == TCCIR_OP_STORE) && biv_plain_imm(ir, tcc_ir_op_get_src1(ir, q), c);
    if (q->is_jump_target || q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_IJUMP || q->op == TCCIR_OP_SWITCH_TABLE)
      return 0;
  }
  return 0;
}

static int biv_loop(TCCIRState *ir, IRLoop *loop)
{
  const int start = loop->start_idx, end = loop->end_idx;
  int changes = 0;
  if (loop->header_idx != start || start <= 0 || end <= start)
    return 0;
  /* the back edge: CMP V, Y ; JUMPIF <S|<U start */
  int jb = biv_prev_real(ir, end, start);
  if (jb < 0 || ir->compact_instructions[jb].op != TCCIR_OP_JUMPIF ||
      (int)tcc_ir_op_dest_imm(ir, &ir->compact_instructions[jb]) != start)
    return 0;
  int cb = biv_prev_real(ir, jb - 1, start);
  if (cb < 0 || ir->compact_instructions[cb].op != TCCIR_OP_CMP)
    return 0;
  IROperand ca = tcc_ir_op_get_src1(ir, &ir->compact_instructions[cb]);
  if (!biv_plain_vreg(ca) || TCCIR_DECODE_VREG_TYPE(irop_get_vreg(ca)) == TCCIR_VREG_TYPE_TEMP)
    return 0;
  const int32_t v = irop_get_vreg(ca);
  IROperand y;
  int btok, dummy;
  if (!biv_cmp_of(ir, jb, v, start, &y, &btok, &dummy) || dummy != cb || (btok != TOK_LT && btok != TOK_ULT))
    return 0;
  /* V's one definition in the loop: V <- V + 1, right before the test */
  int def = -1;
  if (biv_addrtaken(ir, v) || biv_count_defs(ir, v, start, end, &def) != 1 || biv_prev_real(ir, cb - 1, start) != def)
    return 0;
  IRQuadCompact *dq = &ir->compact_instructions[def];
  IROperand d1 = tcc_ir_op_get_src1(ir, dq), d2 = tcc_ir_op_get_src2(ir, dq);
  int64_t step;
  if (dq->op != TCCIR_OP_ADD || tcc_ir_barrel_shift_at(ir, dq) || !biv_plain_vreg(d1) ||
      biv_through_copy(ir, irop_get_vreg(d1), start, def) != v || !biv_plain_imm(ir, d2, &step) || step != 1)
    return 0;
  /* Y: one value through the loop */
  int64_t yc = 0;
  const int y_const = biv_plain_imm(ir, y, &yc);
  if (!y_const && (!biv_plain_vreg(y) || biv_addrtaken(ir, irop_get_vreg(y)) ||
                   biv_count_defs(ir, irop_get_vreg(y), start, end, NULL) != 0))
    return 0;
  if (biv_entered_from_outside(ir, start, end))
    return 0;
  /* the way in brings V below Y */
  int64_t init = 0;
  const int init_known = biv_entry_const(ir, v, start, &init);
  int entry_ok = 0;
  int jg = biv_prev_real(ir, start - 1, 0);
  if (jg >= 0 && ir->compact_instructions[jg].op == TCCIR_OP_JUMPIF)
  {
    IROperand gy;
    int gtok, gc;
    int t = (int)tcc_ir_op_dest_imm(ir, &ir->compact_instructions[jg]);
    int64_t gv;
    /* the guard compares V, or V's constant start */
    if ((t < start || t > end) && biv_cmp_of(ir, jg, v, 0, &gy, &gtok, &gc) &&
        vrp_negate_cmp_tok(gtok) == btok && biv_same(ir, gy, y))
      entry_ok = 1;
    else if ((t < start || t > end) && init_known)
    {
      IRQuadCompact *gq = &ir->compact_instructions[biv_prev_real(ir, jg - 1, 0)];
      if (gq->op == TCCIR_OP_CMP)
      {
        IROperand a = tcc_ir_op_get_src1(ir, gq), b = tcc_ir_op_get_src2(ir, gq);
        int tok = (int)tcc_ir_op_src1_imm(ir, &ir->compact_instructions[jg]);
        if (biv_plain_imm(ir, a, &gv) && gv == init && biv_same(ir, b, y) && vrp_negate_cmp_tok(tok) == btok)
          entry_ok = 1;
      }
    }
  }
  if (!entry_ok && init_known && y_const)
    entry_ok = btok == TOK_LT ? init < yc : (uint32_t)init < (uint32_t)yc;
  if (!entry_ok)
    return 0;
  const int signed_ok = btok == TOK_LT && init_known && init >= 0;
  if (btok == TOK_LT && !signed_ok)
    return 0; /* signed guards from an unknown start: V may be negative */

  /* the compares before the increment */
  for (int j = start; j < def; j++)
  {
    IRQuadCompact *jq = &ir->compact_instructions[j];
    if (jq->op != TCCIR_OP_JUMPIF)
      continue;
    IROperand k;
    int tok, c;
    if (!biv_cmp_of(ir, j, v, start, &k, &tok, &c) || c < start)
      continue;
    int64_t kc;
    if (!biv_same(ir, k, y) && !(y_const && biv_plain_imm(ir, k, &kc) && kc >= yc && (uint32_t)kc >= (uint32_t)yc))
      continue;
    int verdict = -1;
    switch (tok)
    {
    case TOK_ULT: case TOK_ULE: case TOK_NE:
      verdict = 1;
      break;
    case TOK_UGE: case TOK_UGT: case TOK_EQ:
      verdict = 0;
      break;
    case TOK_LT: case TOK_LE:
      verdict = signed_ok ? 1 : -1;
      break;
    case TOK_GE: case TOK_GT:
      verdict = signed_ok ? 0 : -1;
      break;
    default:
      break;
    }
    if (verdict < 0)
      continue;
    /* nothing after the JUMPIF reads these flags */
    int nx = biv_next_real(ir, j + 1, ir->next_instruction_index);
    if (nx >= 0)
    {
      int op = ir->compact_instructions[nx].op;
      if (op == TCCIR_OP_SETIF || op == TCCIR_OP_SELECT || op == TCCIR_OP_JUMPIF)
        continue;
    }
    IROperand jdst = tcc_ir_op_get_dest(ir, jq);
    const int target = (int)tcc_ir_op_dest_imm(ir, jq);
    IROperand xa = tcc_ir_op_get_src1(ir, &ir->compact_instructions[c]);
    IROperand xb = tcc_ir_op_get_src2(ir, &ir->compact_instructions[c]);
    ir->compact_instructions[c].op = TCCIR_OP_NOP;
    if (biv_plain_vreg(xa) && irop_get_vreg(xa) != v)
      biv_forward_copy(ir, irop_get_vreg(xa), start, def);
    if (biv_plain_vreg(xb) && irop_get_vreg(xb) != v)
      biv_forward_copy(ir, irop_get_vreg(xb), start, def);
    if (verdict)
    {
      jq->op = TCCIR_OP_JUMP;
      tcc_ir_set_dest(ir, j, jdst);
      /* the check's trap, jumped over and reached from nowhere else: gone
         with the jump, so the loop is the one the passes after know */
      int dead = target > j && target <= def;
      for (int t = j + 1; dead && t < target; t++)
      {
        IRQuadCompact *tq = &ir->compact_instructions[t];
        if (tq->is_jump_target || (tq->op != TCCIR_OP_NOP && tq->op != TCCIR_OP_TRAP))
          dead = 0;
      }
      if (dead)
      {
        for (int t = j; t < target; t++)
          ir->compact_instructions[t].op = TCCIR_OP_NOP;
        if (!biv_jumped_to(ir, target, j))
          ir->compact_instructions[target].is_jump_target = 0;
      }
    }
    else
      jq->op = TCCIR_OP_NOP;
    changes++;
  }
  return changes;
}

int tcc_ir_opt_bounds_iv(TCCIRState *ir)
{
  if (!ir || ir->next_instruction_index == 0 || !tcc_ir_cfg_flat_has_backedge(ir))
    return 0;
  IRLoops *loops = tcc_ir_detect_loops(ir);
  if (!loops)
    return 0;
  int changes = 0;
  biv_scan_temps(ir);
  for (int l = 0; l < loops->num_loops; l++)
    changes += biv_loop(ir, &loops->loops[l]);
  tcc_free(biv_tdefs);
  tcc_free(biv_tlast);
  biv_tdefs = biv_tlast = NULL;
  biv_ntemps = 0;
  tcc_ir_free_loops(loops);
  return changes;
}
