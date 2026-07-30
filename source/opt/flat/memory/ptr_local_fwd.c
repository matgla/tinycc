/*
 *  TCC IR - Pointer-to-local deref promotion (flat, pre-SSA)
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#define USING_GLOBALS


#include "ir.h"
#include "opt.h"
#include "opt_engine.h"
#include "opt_utils.h"
#include "opt_du.h"

/* A pointer VAR P written exactly once, in the entry block, by `LEA P <-- &V`
 * (V a non-volatile local VAR), and whose own address is never taken, holds &V
 * at every use.  Every deref through P -- or through any TEMP whose defs all
 * copy P's value -- therefore denotes V's slot and is rewritten to a direct VAR
 * access.  Valid regardless of escapes: while any LEA of V survives, V stays
 * memory-resident, so *P and the direct access read/write the same storage.
 * Once DCE kills the then-dead copies and LEAs, V stops being address-taken and
 * SSA promotion + SCCP get their shot (the addrof_var_fwd migration doc shows
 * post-SSA folding cannot recover this).
 *
 * The rewrite reuses a "template" operand cloned from an existing direct lval
 * access of V, so every encoding convention (tag/flags/aux) is preserved
 * exactly; vars with no direct access are skipped. */

#define PLF_UNKNOWN (-1)
#define PLF_CONFLICT (-2)

static int plf_vreg_is_volatile(TCCIRState *ir, int32_t vr)
{
  if (vr < 0 || !tcc_ir_vreg_is_valid(ir, vr))
    return 1;
  IRLiveInterval *iv = tcc_ir_get_live_interval(ir, vr);
  return iv && iv->is_volatile;
}

/* Value defs only: an lval TEMP dest is a store *through* the temp, not a def. */
static int plf_op_writes_temp_value(const IRQuadCompact *q, IROperand dest)
{
  if (!irop_config[q->op].has_dest)
    return 0;
  return !dest.is_lval;
}

int tcc_ir_opt_ptr_local_fwd(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  int nvar = ir->next_local_variable;
  int ntemp = ir->next_temporary_variable;
  int changes = 0;

  if (n < 4 || nvar <= 0)
    return 0;

  int dc_stride = 0;
  uint8_t *dc = ir_opt_build_def_count(ir, n, &dc_stride);

  /* Entry block: everything before the first label or control transfer
   * dominates the whole function (calls do not end the block). */
  int entry_end = n;
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;
    if (i > 0 && q->is_jump_target)
    {
      entry_end = i;
      break;
    }
    if (q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_JUMPIF || q->op == TCCIR_OP_IJUMP ||
        q->op == TCCIR_OP_SWITCH_TABLE || q->op == TCCIR_OP_RETURNVALUE ||
        q->op == TCCIR_OP_RETURNVOID)
    {
      entry_end = i;
      break;
    }
  }

  /* ptr_target[pos(P)] = composite vreg of V, or PLF_UNKNOWN. */
  int32_t *ptr_target = tcc_malloc(nvar * sizeof(int32_t));
  for (int i = 0; i < nvar; i++)
    ptr_target[i] = PLF_UNKNOWN;

  int candidates = 0;
  for (int i = 0; i < entry_end; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_LEA || irop_config[q->op].has_src2)
      continue;
    IROperand dest = tcc_ir_op_get_dest(ir, q);
    IROperand src = tcc_ir_op_get_src1(ir, q);
    int32_t p_vr = irop_get_vreg(dest);
    int32_t v_vr = irop_get_vreg(src);
    if (p_vr < 0 || TCCIR_DECODE_VREG_TYPE(p_vr) != TCCIR_VREG_TYPE_VAR)
      continue;
    if (v_vr < 0 || TCCIR_DECODE_VREG_TYPE(v_vr) != TCCIR_VREG_TYPE_VAR || src.is_lval)
      continue;
    if (!DC_IS_SINGLE_DEF(dc, dc_stride, p_vr))
      continue;
    if (plf_vreg_is_volatile(ir, p_vr) || plf_vreg_is_volatile(ir, v_vr))
      continue;
    int p_pos = TCCIR_DECODE_VREG_POSITION(p_vr);
    if (p_pos >= nvar)
      continue;
    ptr_target[p_pos] = v_vr;
    candidates++;
  }

  if (!candidates)
  {
    tcc_free(ptr_target);
    tcc_free(dc);
    return 0;
  }

  /* Disqualify any P whose own address is taken (its slot could then be
   * rewritten through an alias), or that a postinc-family op touches (those
   * modify the pointer operand in place, invisible to the def count). */
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;
    if (q->op == TCCIR_OP_LEA)
    {
      /* `LEA ? <-- &svr` lets svr's slot be rewritten through an alias, so a
       * tracked pointer svr loses its single-def guarantee.  This includes a
       * qualifying `LEA P2 <-- &P1` def (pointer-to-pointer): P1 must drop. */
      int32_t svr = irop_get_vreg(tcc_ir_op_get_src1(ir, q));
      if (svr >= 0 && TCCIR_DECODE_VREG_TYPE(svr) == TCCIR_VREG_TYPE_VAR &&
          TCCIR_DECODE_VREG_POSITION(svr) < nvar)
        ptr_target[TCCIR_DECODE_VREG_POSITION(svr)] = PLF_UNKNOWN;
      continue;
    }
    if (q->op == TCCIR_OP_LOAD_POSTINC || q->op == TCCIR_OP_STORE_POSTINC)
    {
      for (int s = 0; s < 3; s++)
      {
        IROperand o;
        if (s == 0 && irop_config[q->op].has_dest)
          o = tcc_ir_op_get_dest(ir, q);
        else if (s == 1 && irop_config[q->op].has_src1)
          o = tcc_ir_op_get_src1(ir, q);
        else if (s == 2 && irop_config[q->op].has_src2)
          o = tcc_ir_op_get_src2(ir, q);
        else
          continue;
        int32_t vr = irop_get_vreg(o);
        if (vr >= 0 && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_VAR &&
            TCCIR_DECODE_VREG_POSITION(vr) < nvar)
          ptr_target[TCCIR_DECODE_VREG_POSITION(vr)] = PLF_UNKNOWN;
      }
    }
  }

  candidates = 0;
  for (int i = 0; i < nvar; i++)
    if (ptr_target[i] != PLF_UNKNOWN)
      candidates++;
  if (!candidates)
  {
    tcc_free(ptr_target);
    tcc_free(dc);
    return 0;
  }

  /* Template: first direct 32-bit lval access of V, cloned verbatim so the
   * rewritten operand keeps IR-gen's exact encoding for that slot. */
  IROperand *tmpl = tcc_malloc(nvar * sizeof(IROperand));
  uint8_t *tmpl_ok = tcc_mallocz(nvar);
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP || q->op == TCCIR_OP_LEA || !irop_config[q->op].has_dest)
      continue;
    IROperand d = tcc_ir_op_get_dest(ir, q);
    int32_t dvr = irop_get_vreg(d);
    if (dvr < 0 || TCCIR_DECODE_VREG_TYPE(dvr) != TCCIR_VREG_TYPE_VAR)
      continue;
    int pos = TCCIR_DECODE_VREG_POSITION(dvr);
    if (pos >= nvar || tmpl_ok[pos])
      continue;
    if (!d.is_lval || d.is_llocal || irop_get_btype(d) != IROP_BTYPE_INT32)
      continue;
    tmpl[pos] = d;
    tmpl_ok[pos] = 1;
  }

  /* temp_val[pos(T)]: composite vreg of the V whose address every def of T
   * yields, or PLF_UNKNOWN / PLF_CONFLICT.  Iterated for copy chains. */
  int32_t *temp_val = NULL;
  if (ntemp > 0)
  {
    temp_val = tcc_malloc(ntemp * sizeof(int32_t));
    for (int i = 0; i < ntemp; i++)
      temp_val[i] = PLF_UNKNOWN;

    for (int round = 0; round < 8; round++)
    {
      int changed = 0;
      for (int i = 0; i < n; i++)
      {
        IRQuadCompact *q = &ir->compact_instructions[i];
        if (q->op == TCCIR_OP_NOP || !irop_config[q->op].has_dest)
          continue;
        IROperand d = tcc_ir_op_get_dest(ir, q);
        int32_t dvr = irop_get_vreg(d);
        if (dvr < 0 || TCCIR_DECODE_VREG_TYPE(dvr) != TCCIR_VREG_TYPE_TEMP)
          continue;
        int dpos = TCCIR_DECODE_VREG_POSITION(dvr);
        if (dpos >= ntemp)
          continue;
        if (q->op == TCCIR_OP_LOAD_POSTINC || q->op == TCCIR_OP_STORE_POSTINC)
        {
          if (temp_val[dpos] != PLF_CONFLICT)
          {
            temp_val[dpos] = PLF_CONFLICT;
            changed = 1;
          }
          continue;
        }
        if (!plf_op_writes_temp_value(q, d))
          continue;

        int32_t val = PLF_CONFLICT;
        if (q->op == TCCIR_OP_ASSIGN || q->op == TCCIR_OP_LOAD)
        {
          IROperand s = tcc_ir_op_get_src1(ir, q);
          int32_t svr = irop_get_vreg(s);
          if (svr >= 0 && TCCIR_DECODE_VREG_TYPE(svr) == TCCIR_VREG_TYPE_VAR && s.is_lval &&
              !s.is_llocal && TCCIR_DECODE_VREG_POSITION(svr) < nvar)
          {
            int32_t tgt = ptr_target[TCCIR_DECODE_VREG_POSITION(svr)];
            if (tgt != PLF_UNKNOWN)
              val = tgt; /* T <-- P: slot read of a qualified pointer */
          }
          else if (q->op == TCCIR_OP_ASSIGN && svr >= 0 &&
                   TCCIR_DECODE_VREG_TYPE(svr) == TCCIR_VREG_TYPE_TEMP && !s.is_lval &&
                   TCCIR_DECODE_VREG_POSITION(svr) < ntemp)
          {
            val = temp_val[TCCIR_DECODE_VREG_POSITION(svr)]; /* copy chain */
          }
        }
        else if (q->op == TCCIR_OP_LEA && !irop_config[q->op].has_src2)
        {
          IROperand s = tcc_ir_op_get_src1(ir, q);
          int32_t svr = irop_get_vreg(s);
          if (svr >= 0 && TCCIR_DECODE_VREG_TYPE(svr) == TCCIR_VREG_TYPE_VAR && !s.is_lval &&
              !plf_vreg_is_volatile(ir, svr))
            val = svr; /* T <-- &V directly */
        }

        int32_t merged;
        if (temp_val[dpos] == PLF_UNKNOWN)
          merged = val;
        else if (temp_val[dpos] == val)
          merged = val;
        else
          merged = PLF_CONFLICT;
        if (merged != temp_val[dpos])
        {
          temp_val[dpos] = merged;
          changed = 1;
        }
      }
      if (!changed)
        break;
    }
  }

  /* Rewrite lval TEMP operands that provably address a template-backed V. */
  for (int i = 0; i < n && temp_val; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP || q->op == TCCIR_OP_LEA || q->op == TCCIR_OP_LOAD_POSTINC ||
        q->op == TCCIR_OP_STORE_POSTINC)
      continue;

    for (int s = 0; s < 3; s++)
    {
      IROperand o;
      if (s == 0 && irop_config[q->op].has_src1)
        o = tcc_ir_op_get_src1(ir, q);
      else if (s == 1 && irop_config[q->op].has_src2)
        o = tcc_ir_op_get_src2(ir, q);
      else if (s == 2 && irop_config[q->op].has_dest)
        o = tcc_ir_op_get_dest(ir, q);
      else
        continue;
      /* dest rewrite only for plain STOREs; other lval-dest forms keep their
       * pointer (and simply block the DCE payoff, never correctness). */
      if (s == 2 && q->op != TCCIR_OP_STORE)
        continue;

      int32_t vr = irop_get_vreg(o);
      if (vr < 0 || TCCIR_DECODE_VREG_TYPE(vr) != TCCIR_VREG_TYPE_TEMP)
        continue;
      if (!o.is_lval || o.is_llocal)
        continue;
      int pos = TCCIR_DECODE_VREG_POSITION(vr);
      if (pos >= ntemp)
        continue;
      int32_t v_vr = temp_val[pos];
      if (v_vr == PLF_UNKNOWN || v_vr == PLF_CONFLICT)
        continue;
      int v_pos = TCCIR_DECODE_VREG_POSITION(v_vr);
      if (v_pos >= nvar || !tmpl_ok[v_pos])
        continue;
      if (irop_get_btype(o) != irop_get_btype(tmpl[v_pos]))
        continue;

      IROperand rep = tmpl[v_pos];
      rep.is_unsigned = o.is_unsigned;
      if (s == 0)
        tcc_ir_set_src1(ir, i, rep);
      else if (s == 1)
        tcc_ir_set_src2(ir, i, rep);
      else
        tcc_ir_set_dest(ir, i, rep);
      changes++;
    }
  }

  /* Sweep the now-dead plumbing ourselves: flat dce leaves dead slot-read
   * copies (`T <-- P`) to late cleanup's dead_temp_local, which runs after
   * every const-prop opportunity is gone.  While the copies live, the LEA
   * dests look read, so const_var_prop's addrtaken refresh never clears V and
   * nothing folds.  NOP dead tracked temps' defs, then unread pointers' LEAs,
   * in this same group iteration. */
  int cleaned = changes ? 1 : 0;
  while (cleaned)
  {
    cleaned = 0;
    uint8_t *temp_used = ntemp > 0 ? tcc_mallocz(ntemp) : NULL;
    uint8_t *var_used = tcc_mallocz(nvar);
    for (int i = 0; i < n; i++)
    {
      IRQuadCompact *q = &ir->compact_instructions[i];
      if (q->op == TCCIR_OP_NOP)
        continue;
      for (int s = 0; s < 3; s++)
      {
        IROperand o;
        if (s == 0 && irop_config[q->op].has_src1)
          o = tcc_ir_op_get_src1(ir, q);
        else if (s == 1 && irop_config[q->op].has_src2)
          o = tcc_ir_op_get_src2(ir, q);
        else if (s == 2 && irop_config[q->op].has_dest)
          o = tcc_ir_op_get_dest(ir, q);
        else
          continue;
        int32_t vr = irop_get_vreg(o);
        if (vr < 0)
          continue;
        int typ = TCCIR_DECODE_VREG_TYPE(vr);
        int pos = TCCIR_DECODE_VREG_POSITION(vr);
        /* A dest is a use only when written *through* (lval TEMP) — a plain
         * value def or slot write keeps nothing alive. */
        if (s == 2 && !(typ == TCCIR_VREG_TYPE_TEMP && o.is_lval))
          continue;
        if (typ == TCCIR_VREG_TYPE_TEMP && temp_used && pos < ntemp)
          temp_used[pos] = 1;
        else if (typ == TCCIR_VREG_TYPE_VAR && pos < nvar && s != 2)
          var_used[pos] = 1;
      }
    }
    for (int i = 0; i < n; i++)
    {
      /* NOP in place (slot and is_jump_target flag stay; compact_nops
       * re-derives targets), same as flat dce. */
      IRQuadCompact *q = &ir->compact_instructions[i];
      if (q->op == TCCIR_OP_NOP || !irop_config[q->op].has_dest)
        continue;
      IROperand d = tcc_ir_op_get_dest(ir, q);
      int32_t dvr = irop_get_vreg(d);
      if (dvr < 0)
        continue;
      int typ = TCCIR_DECODE_VREG_TYPE(dvr);
      int pos = TCCIR_DECODE_VREG_POSITION(dvr);
      if (q->op == TCCIR_OP_LEA)
      {
        /* Unread tracked pointer: its single `LEA P <-- &V` def can go. */
        if (typ == TCCIR_VREG_TYPE_VAR && pos < nvar && ptr_target[pos] != PLF_UNKNOWN &&
            !var_used[pos])
        {
          q->op = TCCIR_OP_NOP;
          cleaned = 1;
          changes++;
        }
        continue;
      }
      if (q->op != TCCIR_OP_ASSIGN && q->op != TCCIR_OP_LOAD)
        continue;
      /* Unused tracked temp: all its value defs are pure P-slot reads/copies. */
      if (typ != TCCIR_VREG_TYPE_TEMP || d.is_lval || !temp_val || pos >= ntemp)
        continue;
      if (!temp_used || temp_used[pos])
        continue;
      if (temp_val[pos] == PLF_UNKNOWN || temp_val[pos] == PLF_CONFLICT)
        continue;
      q->op = TCCIR_OP_NOP;
      cleaned = 1;
      changes++;
    }
    tcc_free(temp_used);
    tcc_free(var_used);
  }

  tcc_free(temp_val);
  tcc_free(tmpl_ok);
  tcc_free(tmpl);
  tcc_free(ptr_target);
  tcc_free(dc);
  return changes;
}

int tcc_ir_opt_ptr_local_fwd_ex(IROptCtx *ctx) { return tcc_ir_opt_ptr_local_fwd(ctx->ir); }
