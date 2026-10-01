/*
 *  TCC IR - Fusion & Addressing Mode Optimization
 *
 *  Copyright (c) 2025 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#define USING_GLOBALS

#include "ir.h"
#include "opt_engine.h"
#include "opt_du.h"
#include "opt_loop_utils.h"
#include "opt_alias.h"
#include "opt_utils.h"

extern int gsym_cse_insert_before(TCCIRState *ir, int before_idx, IRQuadCompact *new_q);



/*
 * LEA + deref fold
 *
 * The frontend materializes `&local_var` as an explicit LEA computing the
 * stack-slot address into a TEMP vreg even when the next use is a deref, so
 * ARM emits `add rX,sp,#off; ldr rY,[rX]` instead of `ldr rY,[sp,#off]`.
 * Fold the LEA into a direct StackLoc access on the consumer.
 *
 * Roots handled:
 *   A: T  = LEA Addr[StackLoc]                    ; <op> ...T*DEREF*...
 *   B: T1 = LEA Addr[StackLoc] ; T2 = T1 ADD #K   ; <op> ...T2*DEREF*...
 *   C: T  = ASSIGN Addr[StackLoc]                 ; <op> ...T*DEREF*...
 * (C is A via a frontend copy chain, common in nested-function inlining.)
 *
 * Transform: replace the T-DEREF operand with the StackLoc at the folded
 * offset (is_lval=1); NOP the LEA/ASSIGN and any ADD interposer.
 *
 * Safety: LEA source must be Addr[StackLoc] (is_lval=0); the LEA (and the ADD
 * in B) result must have exactly one use; the consumer must be in the same
 * block and reference the result with is_lval=1 exactly once — never as a
 * STORE dest through the pointer (indistinguishable from a direct stack store).
 */

/* Where each TEMP occurs (as dest, src1 or src2), for the pass's use scans.
 * The fold only ever takes vregs out of instructions -- it NOPs the LEA and
 * the ADD and replaces the consumer's operand with a StackLoc -- so a TEMP can
 * appear nowhere but the instructions listed here when the pass starts, and
 * walking the list with the scans' own per-instruction tests gives exactly
 * what walking every instruction did.  Walking to the end of the function to
 * prove a LEA's result has one use made the pass quadratic: 2.8 G operand
 * checks on Zig's zig.c at -O2. */
typedef struct LfOcc
{
  int *start; /* per TEMP position: its instructions are idx[start[p] .. start[p + 1]) */
  int *idx;
  int npos;
  int *jumps; /* jumps[k]: non-NOP JUMP/JUMPIF before k (the pass keeps them all) */
} LfOcc;

static int lf_temp_pos(int32_t vr, int npos)
{
  if (vr < 0 || TCCIR_DECODE_VREG_TYPE(vr) != TCCIR_VREG_TYPE_TEMP)
    return -1;
  int p = TCCIR_DECODE_VREG_POSITION(vr);
  if (vr != TCCIR_ENCODE_VREG(TCCIR_VREG_TYPE_TEMP, p) || p >= npos)
    return -1;
  return p;
}

/* The TEMP positions instruction `q` names, at most three. */
static int lf_insn_temps(TCCIRState *ir, IRQuadCompact *q, int npos, int out[3])
{
  const IRRegistersConfig *cfg = &irop_config[q->op];
  int32_t v[3] = {-1, -1, -1};
  int m = 0;
  if (cfg->has_src1)
    v[0] = irop_get_vreg(tcc_ir_op_get_src1(ir, q));
  if (cfg->has_src2)
    v[1] = irop_get_vreg(tcc_ir_op_get_src2(ir, q));
  if (cfg->has_dest)
    v[2] = irop_get_vreg(tcc_ir_op_get_dest(ir, q));
  for (int k = 0; k < 3; k++)
  {
    int p = lf_temp_pos(v[k], npos);
    if (p < 0)
      continue;
    int dup = 0;
    for (int e = 0; e < m; e++)
      dup |= out[e] == p;
    if (!dup)
      out[m++] = p;
  }
  return m;
}

static void lf_occ_build(TCCIRState *ir, int n, LfOcc *o)
{
  int npos = 0;
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;
    const IRRegistersConfig *cfg = &irop_config[q->op];
    int32_t v[3] = {cfg->has_src1 ? irop_get_vreg(tcc_ir_op_get_src1(ir, q)) : -1,
                    cfg->has_src2 ? irop_get_vreg(tcc_ir_op_get_src2(ir, q)) : -1,
                    cfg->has_dest ? irop_get_vreg(tcc_ir_op_get_dest(ir, q)) : -1};
    for (int k = 0; k < 3; k++)
      if (v[k] >= 0 && TCCIR_DECODE_VREG_TYPE(v[k]) == TCCIR_VREG_TYPE_TEMP &&
          TCCIR_DECODE_VREG_POSITION(v[k]) >= npos)
        npos = TCCIR_DECODE_VREG_POSITION(v[k]) + 1;
  }
  o->npos = npos;
  o->start = tcc_mallocz(sizeof(int) * (npos + 1));
  o->jumps = tcc_malloc(sizeof(int) * (n + 1));
  int t[3];
  o->jumps[0] = 0;
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    o->jumps[i + 1] = o->jumps[i] + (q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_JUMPIF);
    if (q->op == TCCIR_OP_NOP)
      continue;
    int m = lf_insn_temps(ir, q, npos, t);
    for (int k = 0; k < m; k++)
      o->start[t[k] + 1]++;
  }
  for (int p = 0; p < npos; p++)
    o->start[p + 1] += o->start[p];
  o->idx = tcc_malloc(sizeof(int) * (o->start[npos] + 1));
  int *fill = tcc_malloc(sizeof(int) * (npos + 1));
  memcpy(fill, o->start, sizeof(int) * (npos + 1));
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;
    int m = lf_insn_temps(ir, q, npos, t);
    for (int k = 0; k < m; k++)
      o->idx[fill[t[k]]++] = i;
  }
  tcc_free(fill);
}

static void lf_occ_free(LfOcc *o)
{
  tcc_free(o->start);
  tcc_free(o->idx);
  tcc_free(o->jumps);
}

/* Instructions after `after` that may name `vr`, in order: its listed ones, or
 * every instruction for a vreg the index does not cover. */
typedef struct LfCursor
{
  const int *p, *end; /* listed: the remaining entries */
  int k, n;           /* unlisted: the next index and the bound */
} LfCursor;

static void lf_cursor_init(LfCursor *c, const LfOcc *o, int32_t vr, int after, int n)
{
  int pos = lf_temp_pos(vr, o->npos);
  if (pos < 0)
  {
    c->p = c->end = NULL;
    c->k = after + 1;
    c->n = n;
    return;
  }
  int lo = o->start[pos], hi = o->start[pos + 1];
  while (lo < hi)
  {
    int mid = (lo + hi) / 2;
    if (o->idx[mid] <= after)
      lo = mid + 1;
    else
      hi = mid;
  }
  c->p = o->idx + lo;
  c->end = o->idx + o->start[pos + 1];
  c->k = c->n = 0;
}

static int lf_cursor_next(LfCursor *c)
{
  if (c->p)
    return c->p < c->end ? *c->p++ : -1;
  return c->k < c->n ? c->k++ : -1;
}

/* A JUMP or JUMPIF strictly between `a` and `b`. */
static int lf_jump_between(const LfOcc *o, int a, int b)
{
  return b > a + 1 && o->jumps[b] - o->jumps[a + 1] > 0;
}

int tcc_ir_opt_lea_fold(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  int changes = 0;

  if (n == 0)
    return 0;

  IROptDU du;
  ir_opt_du_build(ir, &du);
  LfOcc occ;
  lf_occ_build(ir, n, &occ);

  LOG_IR_GEN("=== LEA FOLD START (n=%d) ===", n);

  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *lea_q = &ir->compact_instructions[i];

    /* ADD Addr[StackLoc],#K is deliberately not an entry root: folding it to a
     * direct StackLoc can remove the only address-valued op tying a subslot
     * access to the enclosing aggregate, so later stack-slot passes miss
     * aliases. Keep it explicit unless it interposes after a real LEA/ASSIGN
     * root, which still carries the aggregate's address-taken info. */
    if (lea_q->op == TCCIR_OP_ADD)
      continue;
    else if (lea_q->op == TCCIR_OP_ASSIGN)
    {
      /* ASSIGN must have no src2 (or NONE) to be a pure copy of src1. */
      IROperand s2 = tcc_ir_op_get_src2(ir, lea_q);
      if (!irop_is_none(s2))
        continue;
    }
    else if (lea_q->op != TCCIR_OP_LEA)
      continue;

    IROperand lea_src = tcc_ir_op_get_src1(ir, lea_q);
    if (irop_get_tag(lea_src) != IROP_TAG_STACKOFF)
      continue;
    if (lea_src.is_lval) /* already a deref — not an Addr[] form */
      continue;
    if (lea_src.is_llocal) /* double-indirect; keep backend logic */
      continue;

    /* Reject vreg-backed stack operands like `&V1` — these share the
     * STACKOFF tag with `Addr[StackLoc[-N]]` but their real offset comes
     * from the register allocator's spill slot for the vreg, not from
     * u.imm32 (which is 0 for those operands).  Folding would produce
     * StackLoc[0] and dissociate the access from V1's slot. */
    if (irop_get_vreg(lea_src) != -1)
      continue;

    IROperand lea_dest = tcc_ir_op_get_dest(ir, lea_q);
    int32_t lea_vr = irop_get_vreg(lea_dest);
    if (lea_vr < 0)
      continue;
    if (TCCIR_DECODE_VREG_TYPE(lea_vr) != TCCIR_VREG_TYPE_TEMP)
      continue;
    /* ir_opt_du_uses records STORE's dest slot as a definition, not a use, so
     * it undercounts real uses of T (a STORE derefs through T) and once folded
     * `LEA T; *T read; *T write` into a dangling `*T write`. Count uses by an
     * explicit linear scan that treats is_lval=1 dest positions as uses. */
    {
      int total_uses = 0;
      LfCursor cur;
      lf_cursor_init(&cur, &occ, lea_vr, i, n);
      for (int k = lf_cursor_next(&cur); k >= 0 && total_uses < 2; k = lf_cursor_next(&cur))
      {
        IRQuadCompact *uq = &ir->compact_instructions[k];
        if (uq->op == TCCIR_OP_NOP)
          continue;
        const IRRegistersConfig *cfg = &irop_config[uq->op];
        if (cfg->has_src1)
        {
          IROperand s = tcc_ir_op_get_src1(ir, uq);
          if (irop_has_vreg(s) && irop_get_vreg(s) == lea_vr)
            total_uses++;
        }
        if (cfg->has_src2)
        {
          IROperand s = tcc_ir_op_get_src2(ir, uq);
          if (irop_has_vreg(s) && irop_get_vreg(s) == lea_vr)
            total_uses++;
        }
        if (cfg->has_dest)
        {
          IROperand d = tcc_ir_op_get_dest(ir, uq);
          /* A dest with is_lval=1 derefs through the vreg (a use); a plain dest
           * redefines lea_vr and ends its live range — hard stop. Exception:
           * STORE/STORE_INDEXED/STORE_POSTINC put the base pointer (a use) in
           * dest, and disp_fusion clears is_lval on STORE_INDEXED's base, so
           * the is_lval test alone would misclassify it as a redef. */
          if (irop_has_vreg(d) && irop_get_vreg(d) == lea_vr)
          {
            int is_ptr_store = (uq->op == TCCIR_OP_STORE || uq->op == TCCIR_OP_STORE_INDEXED ||
                                uq->op == TCCIR_OP_STORE_POSTINC);
            if (d.is_lval || is_ptr_store)
              total_uses++;
            else
              break; /* lea_vr redefined; stop scanning */
          }
        }
      }
      if (total_uses != 1)
        continue;
    }

    /* STRUCT btype keeps the offset in u.s.aux_data, not u.imm32; use the
     * accessor or a struct-typed Addr[] yields garbage. */
    int32_t base_offset = irop_get_stack_offset(lea_src);

    /* Find the single use of the LEA result. */
    int cur_idx = -1;
    LfCursor ucur;
    lf_cursor_init(&ucur, &occ, lea_vr, i, n);
    for (int j = lf_cursor_next(&ucur); j >= 0; j = lf_cursor_next(&ucur))
    {
      IRQuadCompact *uq = &ir->compact_instructions[j];
      /* Same-block check: the use must precede any control-flow edge. */
      if (lf_jump_between(&occ, i, j))
        break;
      if (uq->op == TCCIR_OP_NOP)
        continue;
      if (uq->op == TCCIR_OP_JUMP || uq->op == TCCIR_OP_JUMPIF)
        break;
      const IRRegistersConfig *cfg = &irop_config[uq->op];
      int uses_lea = 0;
      if (cfg->has_src1)
      {
        IROperand s = tcc_ir_op_get_src1(ir, uq);
        if (irop_has_vreg(s) && irop_get_vreg(s) == lea_vr)
          uses_lea = 1;
      }
      if (!uses_lea && cfg->has_src2)
      {
        IROperand s = tcc_ir_op_get_src2(ir, uq);
        if (irop_has_vreg(s) && irop_get_vreg(s) == lea_vr)
          uses_lea = 1;
      }
      if (!uses_lea && cfg->has_dest)
      {
        IROperand d = tcc_ir_op_get_dest(ir, uq);
        if (irop_has_vreg(d) && irop_get_vreg(d) == lea_vr)
          uses_lea = 1;
      }
      if (uses_lea)
      {
        cur_idx = j;
        break;
      }
    }
    if (cur_idx < 0)
      continue;

    /* Optional single ADD #K interposer between the LEA result and the deref
     * consumer, itself with exactly one use. */
    int add_idx = -1;
    int32_t add_offset = 0;
    IRQuadCompact *add_q = &ir->compact_instructions[cur_idx];
    if (add_q->op == TCCIR_OP_ADD)
    {
      IROperand a1 = tcc_ir_op_get_src1(ir, add_q);
      IROperand a2 = tcc_ir_op_get_src2(ir, add_q);
      /* Require `lea_vr + #K` (either order, other side IMM32) with the vreg
       * side is_lval=0: a DEREF flag means the ADD reads the value stored at
       * lea_vr (loaded-pointer arithmetic), which must not fold to a slot. */
      int ok = 0;
      if (irop_has_vreg(a1) && irop_get_vreg(a1) == lea_vr && !a1.is_lval && irop_get_tag(a2) == IROP_TAG_IMM32)
      {
        add_offset = (int32_t)a2.u.imm32;
        ok = 1;
      }
      else if (irop_has_vreg(a2) && irop_get_vreg(a2) == lea_vr && !a2.is_lval && irop_get_tag(a1) == IROP_TAG_IMM32)
      {
        add_offset = (int32_t)a1.u.imm32;
        ok = 1;
      }
      if (ok)
      {
        IROperand add_dest = tcc_ir_op_get_dest(ir, add_q);
        int32_t add_vr = irop_get_vreg(add_dest);
        /* Explicit-scan use count; ir_opt_du_uses undercounts STORE-dest uses. */
        int add_uses_real = 0;
        if (add_vr >= 0 && TCCIR_DECODE_VREG_TYPE(add_vr) == TCCIR_VREG_TYPE_TEMP)
        {
          LfCursor acur;
          lf_cursor_init(&acur, &occ, add_vr, cur_idx, n);
          for (int k = lf_cursor_next(&acur); k >= 0 && add_uses_real < 2; k = lf_cursor_next(&acur))
          {
            IRQuadCompact *uq2 = &ir->compact_instructions[k];
            if (uq2->op == TCCIR_OP_NOP)
              continue;
            const IRRegistersConfig *cfg2 = &irop_config[uq2->op];
            if (cfg2->has_src1)
            {
              IROperand s = tcc_ir_op_get_src1(ir, uq2);
              if (irop_has_vreg(s) && irop_get_vreg(s) == add_vr)
                add_uses_real++;
            }
            if (cfg2->has_src2)
            {
              IROperand s = tcc_ir_op_get_src2(ir, uq2);
              if (irop_has_vreg(s) && irop_get_vreg(s) == add_vr)
                add_uses_real++;
            }
            if (cfg2->has_dest)
            {
              IROperand d = tcc_ir_op_get_dest(ir, uq2);
              if (irop_has_vreg(d) && irop_get_vreg(d) == add_vr)
              {
                if (d.is_lval)
                  add_uses_real++;
                else
                {
                  add_uses_real = 99;
                  break;
                }
              }
            }
          }
        }
        if (add_vr >= 0 && TCCIR_DECODE_VREG_TYPE(add_vr) == TCCIR_VREG_TYPE_TEMP && add_uses_real == 1)
        {
          add_idx = cur_idx;

          /* Find the consumer of add_vr in the same block. */
          int cons_idx = -1;
          LfCursor ccur;
          lf_cursor_init(&ccur, &occ, add_vr, add_idx, n);
          for (int k = lf_cursor_next(&ccur); k >= 0; k = lf_cursor_next(&ccur))
          {
            IRQuadCompact *ck = &ir->compact_instructions[k];
            if (lf_jump_between(&occ, add_idx, k))
              break;
            if (ck->op == TCCIR_OP_NOP)
              continue;
            if (ck->op == TCCIR_OP_JUMP || ck->op == TCCIR_OP_JUMPIF)
              break;
            const IRRegistersConfig *cfg = &irop_config[ck->op];
            int touches = 0;
            if (cfg->has_src1)
            {
              IROperand s = tcc_ir_op_get_src1(ir, ck);
              if (irop_has_vreg(s) && irop_get_vreg(s) == add_vr)
                touches = 1;
            }
            if (!touches && cfg->has_src2)
            {
              IROperand s = tcc_ir_op_get_src2(ir, ck);
              if (irop_has_vreg(s) && irop_get_vreg(s) == add_vr)
                touches = 1;
            }
            if (!touches && cfg->has_dest)
            {
              IROperand d = tcc_ir_op_get_dest(ir, ck);
              if (irop_has_vreg(d) && irop_get_vreg(d) == add_vr)
                touches = 1;
            }
            if (touches)
            {
              cons_idx = k;
              break;
            }
          }
          if (cons_idx < 0)
            continue; /* ADD dead-ends — let DCE handle it */
          cur_idx = cons_idx;
        }
      }
    }

    /* The final consumer must deref the folded vreg (is_lval=1) exactly once;
     * a non-deref use (PARAM, another ADD) can't fold — the address escapes. */
    int32_t deref_vr =
        (add_idx >= 0) ? irop_get_vreg(tcc_ir_op_get_dest(ir, &ir->compact_instructions[add_idx])) : lea_vr;

    /* LOAD_INDEXED special case: base in src1, constant offset in src2, scale
     * in slot 3. When base is the LEA vreg, scale==0, and src2 is IMM32, fold
     * to a direct StackLoc LOAD at base+add_offset+index_imm, unlocking later
     * stack-store-load forwarding. */
    {
      IRQuadCompact *cq = &ir->compact_instructions[cur_idx];
      int is_load_idx = (cq->op == TCCIR_OP_LOAD_INDEXED);
      if (is_load_idx)
      {
        IROperand base = tcc_ir_op_get_src1(ir, cq);
        if (irop_has_vreg(base) && irop_get_vreg(base) == deref_vr)
        {
          IROperand idx = tcc_ir_op_get_src2(ir, cq);
          IROperand scale = tcc_ir_op_get_scale(ir, cq);
          if (irop_get_tag(idx) == IROP_TAG_IMM32 && irop_get_tag(scale) == IROP_TAG_IMM32 &&
              scale.u.imm32 == 0)
          {
            int folded_off = base_offset + add_offset + (int32_t)idx.u.imm32;
            IROperand width_op = tcc_ir_op_get_dest(ir, cq);
            if (width_op.btype != IROP_BTYPE_STRUCT)
            {
              IROperand stack_op = irop_make_stackoff(-1, folded_off, /*is_lval*/ 1, /*is_llocal*/ 0,
                                                     /*is_param_flag*/ (int)lea_src.is_param,
                                                     width_op.btype);
              stack_op.is_unsigned = width_op.is_unsigned;
              stack_op.is_static = lea_src.is_static;
              /* An indexed access keeps its marks on the base operand. */
              irop_carry_access_marks(&stack_op, base);

              IROperand orig_dest = tcc_ir_op_get_dest(ir, cq);
              cq->op = TCCIR_OP_LOAD;
              tcc_ir_set_dest(ir, cur_idx, orig_dest);
              tcc_ir_set_src1(ir, cur_idx, stack_op);
              tcc_ir_set_src2(ir, cur_idx, IROP_NONE);

              lea_q->op = TCCIR_OP_NOP;
              if (add_idx >= 0)
                ir->compact_instructions[add_idx].op = TCCIR_OP_NOP;

              changes++;
              LOG_IR_GEN("LEA FOLD INDEXED: LEA@%d%s -> LOAD_INDEXED@%d -> LOAD  (offset=%d+%d+%d=%d)",
                         i, (add_idx >= 0 ? " + ADD" : ""), cur_idx, base_offset, add_offset,
                         (int32_t)idx.u.imm32, folded_off);
              continue;
            }
          }
        }
      }
    }

    int which = 0;
    if (!find_deref_use_operand(ir, cur_idx, deref_vr, &which))
      continue;
    /* which==3 is a STORE through the temp: keep it explicit so later
     * stack-slot passes don't miss aliases through the aggregate address.
     * Read-side folds are still safe. */
    if (which == 3)
      continue;

    IRQuadCompact *cons_q = &ir->compact_instructions[cur_idx];

    IROperand old_op = (which == 1)   ? tcc_ir_op_get_src1(ir, cons_q)
                       : (which == 2) ? tcc_ir_op_get_src2(ir, cons_q)
                                      : tcc_ir_op_get_dest(ir, cons_q);

    int folded_off = base_offset + add_offset;
    IROperand new_op;

    if (old_op.btype == IROP_BTYPE_STRUCT)
    {
      /* A struct-typed slot operand keeps its offset in u.s.aux_data and its
       * element type in u.s.ctype_idx, so irop_make_stackoff — which writes
       * u.imm32 over both — cannot build one.  Clone the LEA's own
       * Addr[StackLoc] source instead (same slot, same struct type) and turn it
       * into the deref at the folded offset.  This is what folds
       * `T = &local_struct; PARAM *T` into `PARAM StackLoc[..]`, dropping the
       * `add rX, sp, #off` that precedes every by-value struct argument.
       *
       * Requires a STRUCT lea_src to clone from, and an offset that fits the
       * int16_t aux_data field. */
      if (lea_src.btype != IROP_BTYPE_STRUCT)
        continue;
      if (folded_off < -32768 || folded_off > 32767)
        continue;
      /* Start from the CONSUMER's operand — it is the one carrying the struct's
       * pool ctype index (the LEA's Addr[] source need not) — and rewrite only
       * the addressing half: `u` survives the `vr` reset because ctype_idx and
       * aux_data live outside the bitfield word. */
      new_op = old_op;
      new_op.vr = 0;
      irop_set_vreg(&new_op, -1);
      new_op.tag = IROP_TAG_STACKOFF;
      new_op.is_lval = 1;
      new_op.is_llocal = 0;
      new_op.is_local = 1;
      new_op.is_const = 0;
      new_op.btype = IROP_BTYPE_STRUCT;
      new_op.u.s.aux_data = (int16_t)folded_off;
      irop_init_phys_regs(&new_op);
      new_op.is_param = lea_src.is_param;
      new_op.is_static = lea_src.is_static;
    }
    else
    {
      /* Direct StackLoc at the folded offset (is_lval=1), built from scratch via
       * irop_make_stackoff so union members init cleanly, then copy the
       * consumer's load-width info (btype/is_unsigned) onto it. */
      new_op = irop_make_stackoff(-1, folded_off, /*is_lval*/ 1, /*is_llocal*/ 0,
                                  /*is_param_flag*/ (int)lea_src.is_param, old_op.btype);
      new_op.is_unsigned = old_op.is_unsigned;
      new_op.is_static = lea_src.is_static;
      /* The access is the same one: its volatility and alignment marks go
       * with it.  Left unmarked, it reads as volatile in any function that
       * touches volatile memory at all (tcc_ir_access_is_volatile). */
      irop_carry_access_marks(&new_op, old_op);
    }

    if (which == 1)
      tcc_ir_op_set_src1(ir, cons_q, new_op);
    else if (which == 2)
      tcc_ir_op_set_src2(ir, cons_q, new_op);
    else
      tcc_ir_op_set_dest(ir, cons_q, new_op);

    lea_q->op = TCCIR_OP_NOP;
    if (add_idx >= 0)
      ir->compact_instructions[add_idx].op = TCCIR_OP_NOP;

    changes++;
    LOG_IR_GEN("LEA FOLD: LEA@%d%s -> consumer@%d  (offset=%d+%d=%d)", i, (add_idx >= 0 ? " + ADD" : ""), cur_idx,
               base_offset, add_offset, base_offset + add_offset);
  }

  LOG_IR_GEN("=== LEA FOLD END: %d folds ===", changes);

  tcc_free(du.def);
  lf_occ_free(&occ);
  return changes;
}
