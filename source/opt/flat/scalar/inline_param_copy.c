/*
 *  TCC IR - Inline-parameter copy elimination
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
#include "opt_utils.h"

/* tcc_ir_opt_inline_param_copy_elim: drop the redundant local an inline
 * expansion creates for each parameter.
 *
 * Binding a parameter emits a copy through a fresh VT_LOCAL slot:
 *
 *   Tn  = Vsrc          [LOAD]     ; read the caller's variable
 *   Vpar= Tn            [STORE]    ; write the callee's parameter slot
 *   ... Vpar used as an ordinary operand ...
 *
 * Vpar is nothing but a second name for Vsrc, yet it costs a stack slot plus
 * the load/store pair, and `loc` is never given back between expansions, so a
 * function with many inlined helpers accumulates one slot per parameter
 * (lib/fp/soft's __aeabi_dadd reached ~100 slots / a 1132-byte frame).
 * Rewriting Vpar's uses to Vsrc lets DCE drop the copy and the slot with it.
 *
 * Why no dominance analysis is needed: the rewrite is gated on Vsrc having
 * exactly ONE definition in the whole function.  A single-assignment source
 * carries the same value at every point that can reach a use, so the copy and
 * its uses may sit in different blocks -- which they do, since the soft-float
 * helpers are consumed across the NaN/inf/zero branches, and that is precisely
 * where the straight-line var_tmp_fwd gives up.
 *
 * Both variables must be non-address-taken locals, so no pointer store and no
 * call can write either behind our back.
 */

/* One def site per VAR position, or -2 once a second def is seen. */
static int ipc_collect_var_defs(TCCIRState *ir, int n, int **out_def, int *out_max)
{
  int max_pos = 0;
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP || !irop_config[q->op].has_dest)
      continue;
    int32_t v = irop_get_vreg(tcc_ir_op_get_dest(ir, q));
    if (TCCIR_DECODE_VREG_TYPE(v) != TCCIR_VREG_TYPE_VAR)
      continue;
    int p = TCCIR_DECODE_VREG_POSITION(v);
    if (p > max_pos)
      max_pos = p;
  }
  if (max_pos == 0)
    return 0;

  int *def = tcc_malloc((max_pos + 1) * sizeof(int));
  for (int i = 0; i <= max_pos; i++)
    def[i] = -1;
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP || !irop_config[q->op].has_dest)
      continue;
    int32_t v = irop_get_vreg(tcc_ir_op_get_dest(ir, q));
    if (TCCIR_DECODE_VREG_TYPE(v) != TCCIR_VREG_TYPE_VAR)
      continue;
    int p = TCCIR_DECODE_VREG_POSITION(v);
    if (p > max_pos)
      continue;
    def[p] = (def[p] == -1) ? i : -2;
  }
  *out_def = def;
  *out_max = max_pos;
  return 1;
}

/* One def site per TEMP position, or -2 once a second def is seen.  A census,
 * not a backward scan from the use: tcc_ir_find_defining_instruction() returns
 * the NEAREST preceding def, which for a temp written in both arms of a diamond
 * is whichever arm happens to sit last in the instruction order.  Believing it
 * turned `V = T` (T set to one value in each arm) into `V = <the later arm's
 * source>` and deleted the diamond with it -- gcc.c-torture pr63209's
 * `(pa_minus_pb <= 0) ? a : b` returned `b` unconditionally. */
static void ipc_collect_tmp_defs(TCCIRState *ir, int n, int **out_def, int *out_max)
{
  int max_pos = 0;
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP || !irop_config[q->op].has_dest)
      continue;
    int32_t v = irop_get_vreg(tcc_ir_op_get_dest(ir, q));
    if (TCCIR_DECODE_VREG_TYPE(v) != TCCIR_VREG_TYPE_TEMP)
      continue;
    int p = TCCIR_DECODE_VREG_POSITION(v);
    if (p > max_pos)
      max_pos = p;
  }

  int *def = tcc_malloc((max_pos + 1) * sizeof(int));
  for (int i = 0; i <= max_pos; i++)
    def[i] = -1;
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP || !irop_config[q->op].has_dest)
      continue;
    int32_t v = irop_get_vreg(tcc_ir_op_get_dest(ir, q));
    if (TCCIR_DECODE_VREG_TYPE(v) != TCCIR_VREG_TYPE_TEMP)
      continue;
    int p = TCCIR_DECODE_VREG_POSITION(v);
    def[p] = (def[p] == -1) ? i : -2;
  }
  *out_def = def;
  *out_max = max_pos;
}

static int ipc_var_ok(TCCIRState *ir, int32_t vr)
{
  IRLiveInterval *iv = tcc_ir_get_live_interval(ir, vr);
  if (!iv)
    return 0;
  /* addrtaken: a pointer store could write it. volatile/lvalue: memory
   * semantics we must not collapse. */
  if (iv->addrtaken || iv->is_volatile || iv->is_lvalue)
    return 0;
  return 1;
}


/* Does the store at `store_idx` dominate everything up to `last_use`?
 *
 * Forwarding a TEMP (unlike forwarding a single-assignment VAR) is only valid
 * where the store is guaranteed to have executed: the register has no memory
 * behind it, so a path that reaches a use without passing the store would read
 * whatever the allocator last left there -- and the temp's live interval need
 * not even cover that point.  This is the property single-def-of-the-VAR does
 * NOT supply, and getting it wrong miscompiled libsoftfp (13 suite failures).
 *
 * Rather than build dominator trees for a flat pre-SSA pass, test the property
 * directly: control can only enter (store_idx, last_use] from outside by a jump
 * that lands strictly inside it.  If no such edge exists, every path reaching a
 * use ran through the store.  Unknowable targets (computed jumps, switch
 * tables) anywhere in the function force a bail. */
static int ipc_store_dominates_window(TCCIRState *ir, int n, int store_idx, int last_use)
{
  for (int k = 0; k < n; k++)
  {
    IRQuadCompact *q = &ir->compact_instructions[k];
    if (q->op == TCCIR_OP_NOP)
      continue;
    if (q->op == TCCIR_OP_IJUMP || q->op == TCCIR_OP_SWITCH_TABLE)
      return 0; /* target set unknown */
    if (q->op != TCCIR_OP_JUMP && q->op != TCCIR_OP_JUMPIF)
      continue;
    if (k >= store_idx && k <= last_use)
      continue; /* an edge from inside the window cannot bypass the store */
    IROperand jd = tcc_ir_op_get_dest(ir, q);
    int jt = (int)jd.u.imm32;
    if (jt > store_idx && jt <= last_use)
      return 0; /* entry that skips the store */
  }
  return 1;
}

/* Last index at which `vr` is read, or -1. */
static int ipc_last_use(TCCIRState *ir, int n, int32_t vr)
{
  int last = -1;
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;
    if ((irop_config[q->op].has_src1 && irop_get_vreg(tcc_ir_op_get_src1(ir, q)) == vr) ||
        (irop_config[q->op].has_src2 && irop_get_vreg(tcc_ir_op_get_src2(ir, q)) == vr))
      last = i;
  }
  return last;
}

int tcc_ir_opt_inline_param_copy_elim(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  int changes = 0;
  int *var_def = NULL;
  int max_var = 0;
  int *tmp_def = NULL;
  int max_tmp = 0;

  if (n < 3)
    return 0;
  if (!ipc_collect_var_defs(ir, n, &var_def, &max_var))
    return 0;
  ipc_collect_tmp_defs(ir, n, &tmp_def, &max_tmp);

  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_STORE)
      continue;

    /* dest must be the single def of a plain local VAR */
    IROperand dest = tcc_ir_op_get_dest(ir, q);
    int32_t dst_vr = irop_get_vreg(dest);
    if (TCCIR_DECODE_VREG_TYPE(dst_vr) != TCCIR_VREG_TYPE_VAR)
      continue;
    int dpos = TCCIR_DECODE_VREG_POSITION(dst_vr);
    if (dpos > max_var || var_def[dpos] != i)
      continue;
    if (!ipc_var_ok(ir, dst_vr))
      continue;

    /* src must be a TEMP whose single def is a LOAD of another local VAR */
    IROperand src = tcc_ir_op_get_src1(ir, q);
    if (src.is_lval)
      continue;
    int32_t t_vr = irop_get_vreg(src);
    if (TCCIR_DECODE_VREG_TYPE(t_vr) != TCCIR_VREG_TYPE_TEMP)
      continue;
    int t_def = tcc_ir_find_defining_instruction(ir, t_vr, i);
    if (t_def < 0)
      continue;
    /* `V = T` only means "V holds what that def produced" while T has exactly
     * one def function-wide.  With two (a value computed in each arm of a
     * diamond) the nearest-preceding def above is one of them chosen by layout,
     * and forwarding its source makes the other arm's value disappear. */
    {
      int tpos = TCCIR_DECODE_VREG_POSITION(t_vr);
      if (tpos > max_tmp || tmp_def[tpos] != t_def)
        continue;
    }
    /* (a) The temp merely reloads another single-assignment local
     *     (`T = Vsrc [LOAD]`) -- the parameter-binding shape.  Forward to Vsrc.
     *     Single ASSIGNMENT means every path reaching a use sees the same
     *     value, so this needs no dominance reasoning at all.
     * (b) Otherwise the temp holds a computed value -- an inlined helper's
     *     *local*.  Forward the temp itself so the local needs no slot, but
     *     only where the store provably dominates the uses and the temp has a
     *     single def function-wide. */
    IROperand lsrc;
    int32_t src_vr = -1;
    int forwarding_temp = 0;
    int matched_var = 0;
    IRQuadCompact *lq = &ir->compact_instructions[t_def];
    if (lq->op == TCCIR_OP_LOAD)
    {
      IROperand cand = tcc_ir_op_get_src1(ir, lq);
      int32_t cvr = irop_get_vreg(cand);
      if (cand.is_lval && TCCIR_DECODE_VREG_TYPE(cvr) == TCCIR_VREG_TYPE_VAR)
      {
        int cpos = TCCIR_DECODE_VREG_POSITION(cvr);
        if (cpos <= max_var && var_def[cpos] >= 0 && var_def[cpos] <= t_def && ipc_var_ok(ir, cvr))
        {
          lsrc = cand;
          src_vr = cvr;
          matched_var = 1;
        }
      }
    }
    if (!matched_var)
    {
      int lu = ipc_last_use(ir, n, dst_vr);
      if (lu < 0)
        continue;
      /* A use at a LOWER index than the store can still execute AFTER it: a
       * backedge makes linear order and execution order diverge.  Such a use is
       * never visited by the forward rewrite below, so NOPing the store would
       * leave it reading a slot nothing writes any more.  Only the forward
       * window is provably covered, so require the whole live range to sit in
       * it. */
      int used_before = 0;
      for (int b = 0; b < i && !used_before; b++)
      {
        IRQuadCompact *bq = &ir->compact_instructions[b];
        if (bq->op == TCCIR_OP_NOP)
          continue;
        if ((irop_config[bq->op].has_src1 && irop_get_vreg(tcc_ir_op_get_src1(ir, bq)) == dst_vr) ||
            (irop_config[bq->op].has_src2 && irop_get_vreg(tcc_ir_op_get_src2(ir, bq)) == dst_vr))
          used_before = 1;
      }
      if (used_before)
        continue;
      if (!ipc_store_dominates_window(ir, n, i, lu))
        continue;
      lsrc = src; /* the stored temp */
      src_vr = t_vr;
      forwarding_temp = 1;
    }

    /* Same representation, or the rewritten uses would read a different
     * width / signedness than they were generated for. */
    if (irop_get_btype(dest) != irop_get_btype(lsrc) || dest.is_unsigned != lsrc.is_unsigned ||
        irop_needs_pair(dest) != irop_needs_pair(lsrc))
      continue;

    /* Rewrite every later use of the parameter to name the source instead.
     * `left_behind` tracks any use we could NOT rewrite: the copy may only be
     * deleted when every one of them was redirected, otherwise the survivors
     * would read a slot nothing writes any more. */
    int rewrote = 0;
    int left_behind = 0;
    for (int j = i + 1; j < n; j++)
    {
      IRQuadCompact *u = &ir->compact_instructions[j];
      if (u->op == TCCIR_OP_NOP)
        continue;
      /* A redef of either variable ends the window.  var_def guarantees there
       * is none, but a defensive stop keeps the pass correct if that ever
       * loosens.  Uses beyond it are unexamined, so the copy has to stay. */
      if (irop_config[u->op].has_dest)
      {
        int32_t dv = irop_get_vreg(tcc_ir_op_get_dest(ir, u));
        if (dv == dst_vr || dv == src_vr)
        {
          left_behind = 1;
          break;
        }
      }
      /* Substitute the source operand wholesale (the identity, stack offset
       * and flags all live in fields that must travel together).  Only for a
       * use that reads the parameter exactly as the LOAD read the source --
       * same width, sign and lval-ness -- so a narrowing or address-taking
       * use is left alone rather than silently retyped. */
      /* Ops whose src1 names an ADDRESS rather than a value: substituting a
       * value there would reinterpret the operand (var_tmp_fwd guards the
       * same set). */
      int src1_is_address = (u->op == TCCIR_OP_LOAD || u->op == TCCIR_OP_LOAD_POSTINC ||
                             u->op == TCCIR_OP_LEA || u->op == TCCIR_OP_LOAD_INDEXED);
      if (irop_config[u->op].has_src1)
      {
        IROperand s = tcc_ir_op_get_src1(ir, u);
        if (irop_get_vreg(s) == dst_vr)
        {
          /* Every branch that does NOT rewrite must fall through to
           * left_behind -- including the address-operand skip.  Letting that
           * skip bypass the bookkeeping is what NOPed a store while a later
           * `T = V [LOAD]` still read the slot, so the reload returned a dead
           * frame word (libsoftfp fmul: subnormal products came back 0). */
          if (!(src1_is_address && forwarding_temp) && irop_get_btype(s) == irop_get_btype(lsrc) &&
              s.is_unsigned == lsrc.is_unsigned && (forwarding_temp || s.is_lval == lsrc.is_lval) &&
              !s.is_llocal)
          {
            tcc_ir_set_src1(ir, j, lsrc);
            rewrote++;
          }
          else
            left_behind = 1;
        }
      }
      if (irop_config[u->op].has_src2)
      {
        IROperand s = tcc_ir_op_get_src2(ir, u);
        if (irop_get_vreg(s) == dst_vr && irop_get_btype(s) == irop_get_btype(lsrc) &&
            s.is_unsigned == lsrc.is_unsigned && (forwarding_temp || s.is_lval == lsrc.is_lval) &&
            !s.is_llocal)
        {
          tcc_ir_set_src2(ir, j, lsrc);
          rewrote++;
        }
        else if (irop_get_vreg(s) == dst_vr)
          left_behind = 1;
      }
    }

    if (rewrote && !left_behind)
    {
      LOG_IR_GEN("OPTIMIZE: inline_param_copy_elim V:%d -> %s:%d (%d uses) at i=%d", dpos,
                 forwarding_temp ? "T" : "V", TCCIR_DECODE_VREG_POSITION(src_vr), rewrote, i);
      q->op = TCCIR_OP_NOP; /* every use now names the source; the copy is dead */
      changes++;
    }
  }

  tcc_free(var_def);
  tcc_free(tmp_def);
  return changes;
}

int tcc_ir_opt_inline_param_copy_elim_ex(IROptCtx *ctx) { return tcc_ir_opt_inline_param_copy_elim(ctx->ir); }
