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
#include "cfg.h"
#include "opt.h"
#include "opt_engine.h"
#include "opt_utils.h"
#include "opt_du.h"
#include "memory/vector.h"

/* A pointer VAR P whose every definition is `LEA P <-- &V` for the same V (a
 * non-volatile local VAR), and whose own address is never taken, holds &V at
 * every use: a use no definition reaches would read an uninitialized local,
 * which C leaves undefined anyway.  (The Zig C backend reuses one C local per
 * type, so `t1 = &t0` recurs, and it sets them in every block, not just the
 * entry.)  Every deref through P -- or through any TEMP whose defs all
 * copy P's value -- therefore denotes V's slot and is rewritten to a direct VAR
 * access.  Valid regardless of escapes: while any LEA of V survives, V stays
 * memory-resident, so *P and the direct access read/write the same storage.
 * Once DCE kills the then-dead copies and LEAs, V stops being address-taken and
 * SSA promotion + SCCP get their shot (the addrof_var_fwd migration doc shows
 * post-SSA folding cannot recover this).
 *
 * A pointer VAR whose defs are all `LEA <-- &V` but of DIFFERENT locals cannot
 * be qualified flow-insensitively; each copy use is resolved instead to the LEA
 * defs that reach it (a reaching-definition walk over the CFG below).  The Zig
 * C backend reuses one C local for the address temp of every inlined copy, so
 * after inlining a helper twice `t20 = &bases_copy` is one VAR with two
 * targets, and the same shape recurs in hand-written code
 * (`p = &a; ...; p = &b; ...`).
 *
 * The rewrite reuses a "template" operand cloned from an existing direct lval
 * access of V, so every encoding convention (tag/flags/aux) is preserved
 * exactly.  A V with no such access -- only ever set by a call, say -- gets
 * the canonical slot form of a 32-bit VAR read instead. */

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
  /* A nested function writes a captured P through the static chain, a def
   * this body never shows. */
  if ((tcc_state && tcc_state->nb_nested_funcs > 0) || ir->captured_count > 0)
    return 0;

  /* ptr_target[pos(P)] = composite vreg of V, or PLF_UNKNOWN. */
  int32_t *ptr_target = tcc_malloc(nvar * sizeof(int32_t));
  for (int i = 0; i < nvar; i++)
    ptr_target[i] = PLF_UNKNOWN;

  /* Every `LEA P <-- &V` must name the same V... */
  int candidates = 0, direct_lea = 0;
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_LEA || irop_config[q->op].has_src2)
      continue;
    IROperand src = tcc_ir_op_get_src1(ir, q);
    int32_t p_vr = tcc_ir_op_dest_vreg(ir, q);
    int32_t v_vr = irop_get_vreg(src);
    if (p_vr >= 0 && TCCIR_DECODE_VREG_TYPE(p_vr) == TCCIR_VREG_TYPE_TEMP &&
        v_vr >= 0 && TCCIR_DECODE_VREG_TYPE(v_vr) == TCCIR_VREG_TYPE_VAR &&
        !src.is_lval && !plf_vreg_is_volatile(ir, v_vr))
      direct_lea = 1;
    if (p_vr < 0 || TCCIR_DECODE_VREG_TYPE(p_vr) != TCCIR_VREG_TYPE_VAR)
      continue;
    int p_pos = TCCIR_DECODE_VREG_POSITION(p_vr);
    if (p_pos >= nvar)
      continue;
    int ok = v_vr >= 0 && TCCIR_DECODE_VREG_TYPE(v_vr) == TCCIR_VREG_TYPE_VAR && !src.is_lval &&
             !plf_vreg_is_volatile(ir, p_vr) && !plf_vreg_is_volatile(ir, v_vr);
    if (!ok)
      ptr_target[p_pos] = PLF_CONFLICT;
    else if (ptr_target[p_pos] == PLF_UNKNOWN)
      ptr_target[p_pos] = v_vr, candidates++;
    else if (ptr_target[p_pos] != v_vr)
      ptr_target[p_pos] = PLF_CONFLICT;
  }
  /* ...and P may have no other definition. */
  for (int i = 0; i < n && candidates; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP || q->op == TCCIR_OP_LEA || !irop_config[q->op].has_dest)
      continue;
    int32_t dvr = tcc_ir_op_dest_vreg(ir, q);
    if (dvr >= 0 && TCCIR_DECODE_VREG_TYPE(dvr) == TCCIR_VREG_TYPE_VAR && TCCIR_DECODE_VREG_POSITION(dvr) < nvar)
      ptr_target[TCCIR_DECODE_VREG_POSITION(dvr)] = PLF_CONFLICT;
  }
  candidates = 0;
  for (int i = 0; i < nvar; i++)
  {
    if (ptr_target[i] == PLF_CONFLICT)
      ptr_target[i] = PLF_UNKNOWN;
    candidates += ptr_target[i] != PLF_UNKNOWN;
  }

  /* Multi-target LEA VARs: every def names a valid local, just not the same
   * one.  mdef_* map each defining instruction to its VAR's def bit and the
   * target it names; multi_lea[p] survives the same disqualifications below
   * that a single-target P must pass. */
  uint8_t *multi_lea = tcc_mallocz(nvar);
  uint8_t *mdef_bit = n ? tcc_mallocz(n) : NULL; /* 0-63 at a def, 0xFF elsewhere */
  int32_t *mdef_tgt = n ? tcc_malloc(sizeof(int32_t) * n) : NULL;
  int32_t *mdef_var = n ? tcc_malloc(sizeof(int32_t) * n) : NULL;
  {
    int *lea_cnt = tcc_mallocz(sizeof(int) * nvar);
    uint8_t *other_def = tcc_mallocz(nvar);
    if (mdef_bit)
      memset(mdef_bit, 0xFF, n);
    for (int i = 0; i < n; i++)
    {
      IRQuadCompact *q = &ir->compact_instructions[i];
      if (q->op == TCCIR_OP_NOP || !irop_config[q->op].has_dest)
        continue;
      int32_t dvr = tcc_ir_op_dest_vreg(ir, q);
      if (dvr < 0 || TCCIR_DECODE_VREG_TYPE(dvr) != TCCIR_VREG_TYPE_VAR)
        continue;
      int p = TCCIR_DECODE_VREG_POSITION(dvr);
      if (p >= nvar)
        continue;
      int ok = q->op == TCCIR_OP_LEA && !irop_config[q->op].has_src2;
      if (ok)
      {
        IROperand src = tcc_ir_op_get_src1(ir, q);
        int32_t vvr = irop_get_vreg(src);
        ok = vvr >= 0 && TCCIR_DECODE_VREG_TYPE(vvr) == TCCIR_VREG_TYPE_VAR && !src.is_lval &&
             !plf_vreg_is_volatile(ir, dvr) && !plf_vreg_is_volatile(ir, vvr) && lea_cnt[p] < 64;
        if (ok)
        {
          mdef_bit[i] = (uint8_t)lea_cnt[p];
          mdef_tgt[i] = vvr;
          mdef_var[i] = p;
          lea_cnt[p]++;
        }
      }
      if (!ok)
        other_def[p] = 1;
    }
    int nmulti = 0;
    for (int p = 0; p < nvar; p++)
      if (lea_cnt[p] >= 2 && !other_def[p] && ptr_target[p] == PLF_UNKNOWN)
        multi_lea[p] = 1, nmulti++;
    tcc_free(lea_cnt);
    tcc_free(other_def);
    if (!nmulti)
    {
      tcc_free(multi_lea);
      tcc_free(mdef_bit);
      tcc_free(mdef_tgt);
      tcc_free(mdef_var);
      multi_lea = NULL, mdef_bit = NULL, mdef_tgt = NULL, mdef_var = NULL;
    }
  }

  if (!candidates && !multi_lea && !direct_lea)
  {
    tcc_free(ptr_target);
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
      int32_t svr = tcc_ir_op_src1_vreg(ir, q);
      if (svr >= 0 && TCCIR_DECODE_VREG_TYPE(svr) == TCCIR_VREG_TYPE_VAR &&
          TCCIR_DECODE_VREG_POSITION(svr) < nvar)
      {
        ptr_target[TCCIR_DECODE_VREG_POSITION(svr)] = PLF_UNKNOWN;
        if (multi_lea)
          multi_lea[TCCIR_DECODE_VREG_POSITION(svr)] = 0;
      }
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
        {
          ptr_target[TCCIR_DECODE_VREG_POSITION(vr)] = PLF_UNKNOWN;
          if (multi_lea)
            multi_lea[TCCIR_DECODE_VREG_POSITION(vr)] = 0;
        }
      }
    }
  }

  /* Inline bindings often copy &V through several single-definition VARs.
   * Their values are as stable as the LEA itself, unless their own home can
   * be changed through an alias or an implicit postincrement. */
  scoped_vector(int) copy_def = {0};
  vector_resize(&copy_def, nvar);
  for (int p = 0; p < nvar; p++)
    copy_def.data[p] = -1;
  for (int i = 0; i < n; i++) {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;
    for (int k = 0; k < 3; k++) {
      IROperand o = tcc_ir_op_get_slot(ir, q, k);
      int32_t v = irop_get_vreg(o);
      if (v < 0 || TCCIR_DECODE_VREG_TYPE(v) != TCCIR_VREG_TYPE_VAR)
        continue;
      int p = TCCIR_DECODE_VREG_POSITION(v);
      if (p >= nvar)
        continue;
      if ((q->op == TCCIR_OP_LEA && k == 1) || q->op == TCCIR_OP_LOAD_POSTINC ||
          q->op == TCCIR_OP_STORE_POSTINC)
        copy_def.data[p] = -2;
      else if (k == 0 && irop_dest_defines_vreg(o))
        copy_def.data[p] = copy_def.data[p] == -1 ? i : -2;
    }
  }
  for (int round = 0; round < 8; round++) {
    int changed = 0;
    for (int p = 0; p < nvar; p++) {
      int i = copy_def.data[p];
      if (i < 0 || ptr_target[p] != PLF_UNKNOWN ||
          plf_vreg_is_volatile(ir, TCCIR_ENCODE_VREG(TCCIR_VREG_TYPE_VAR, p)))
        continue;
      IRQuadCompact *q = &ir->compact_instructions[i];
      if (q->op != TCCIR_OP_LOAD && q->op != TCCIR_OP_ASSIGN)
        continue;
      IROperand src = tcc_ir_op_get_src1(ir, q);
      int32_t v = irop_get_vreg(src);
      if (tcc_ir_op_get_dest(ir, q).btype != IROP_BTYPE_INT32 ||
          !irop_is_vreg_value(src) || src.btype != IROP_BTYPE_INT32 || v < 0 ||
          TCCIR_DECODE_VREG_TYPE(v) != TCCIR_VREG_TYPE_VAR)
        continue;
      int sp = TCCIR_DECODE_VREG_POSITION(v);
      if (sp < nvar && ptr_target[sp] >= 0) {
        ptr_target[p] = ptr_target[sp];
        changed = 1;
      }
    }
    if (!changed)
      break;
  }

  candidates = 0;
  int any_multi = 0;
  for (int i = 0; i < nvar; i++)
  {
    if (multi_lea && multi_lea[i])
      any_multi = 1;
    else
      candidates += ptr_target[i] != PLF_UNKNOWN;
  }
  if (!candidates && !any_multi && !direct_lea)
  {
    tcc_free(ptr_target);
    tcc_free(multi_lea);
    tcc_free(mdef_bit);
    tcc_free(mdef_tgt);
    tcc_free(mdef_var);
    return 0;
  }

  /* Per-use resolution of the multi-target LEA VARs: a reaching-definition
   * walk over the CFG gives each `T <-- P` copy the one local every LEA
   * reaching it names (resol[i]), or nothing when they disagree or nothing
   * reaches -- both leave the copy alone, which is always safe while any LEA
   * of the local survives. */
  int32_t *resol = NULL;
  /* The walk is CFG-sensitive: an edge the CFG does not show (a computed
   * goto's targets, a returns_twice resume) could hide a disagreeing def, so
   * those functions get no resolution at all. */
  int resol_ok = any_multi && ntemp > 0 && !ir->func_has_label_addr &&
                 !tcc_ir_calls_returns_twice(ir);
  for (int i = 0; resol_ok && i < n; i++)
    if (ir->compact_instructions[i].op == TCCIR_OP_IJUMP)
      resol_ok = 0;
  if (resol_ok)
  {
    IRCFG *cfg = tcc_ir_cfg_build(ir);
    if (cfg && cfg->num_blocks > 0)
    {
      tcc_ir_cfg_compute_rpo(cfg);
      const int nb = cfg->num_blocks;
      int *midx = tcc_malloc(sizeof(int) * nvar);
      int nx = 0;
      for (int p = 0; p < nvar; p++)
        midx[p] = multi_lea[p] ? nx++ : -1;
      if (nx > 0)
      {
        /* btgt[x*64 + bit]: the local that def `bit` of multi VAR x names. */
        int32_t *btgt = tcc_mallocz(sizeof(int32_t) * (size_t)nx * 64);
        for (int i = 0; i < n; i++)
          if (mdef_bit[i] != 0xFF)
          {
            int x = midx[mdef_var[i]];
            if (x >= 0)
              btgt[x * 64 + mdef_bit[i]] = mdef_tgt[i];
          }
        uint64_t *ent = tcc_mallocz(sizeof(uint64_t) * (size_t)nb * nx);
        uint64_t *st = tcc_malloc(sizeof(uint64_t) * nx);
        int settled = 0;
        for (int round = 0; !settled && round < 64; round++)
        {
          int changed = 0;
          settled = 1;
          for (int r = 0; r < cfg->rpo_count; r++)
          {
            const int b = cfg->rpo_order[r];
            IRBasicBlock *bb = &cfg->blocks[b];
            memcpy(st, ent + (size_t)b * nx, sizeof(uint64_t) * nx);
            for (int i = bb->start_idx; i < bb->end_idx; i++)
            {
              if (mdef_bit[i] == 0xFF)
                continue;
              int x = midx[mdef_var[i]];
              if (x >= 0)
                st[x] = (uint64_t)1 << mdef_bit[i];
            }
            for (int e = 0; e < bb->num_succs; e++)
            {
              uint64_t *dst = ent + (size_t)bb->succs[e] * nx;
              for (int x = 0; x < nx; x++)
                if (~(dst[x] | ~st[x]))
                {
                  dst[x] |= st[x];
                  changed = 1;
                }
            }
          }
          if (changed)
            settled = 0;
        }
        /* A walk that did not settle under-approximates the reaching set and
         * could hide a disagreeing def: no resolution at all then. */
        if (settled)
        {
          resol = tcc_malloc(sizeof(int32_t) * n);
          for (int i = 0; i < n; i++)
            resol[i] = -1;
          for (int b = 0; b < nb; b++)
          {
            IRBasicBlock *bb = &cfg->blocks[b];
            memcpy(st, ent + (size_t)b * nx, sizeof(uint64_t) * nx);
            for (int i = bb->start_idx; i < bb->end_idx; i++)
            {
              IRQuadCompact *q = &ir->compact_instructions[i];
              if (q->op == TCCIR_OP_NOP)
                continue;
              if (mdef_bit[i] != 0xFF)
              {
                int x = midx[mdef_var[i]];
                if (x >= 0)
                  st[x] = (uint64_t)1 << mdef_bit[i];
                continue;
              }
              if (q->op != TCCIR_OP_ASSIGN && q->op != TCCIR_OP_LOAD)
                continue;
              IROperand d = tcc_ir_op_get_dest(ir, q);
              int32_t dvr = irop_get_vreg(d);
              if (d.is_lval || dvr < 0 || TCCIR_DECODE_VREG_TYPE(dvr) != TCCIR_VREG_TYPE_TEMP)
                continue;
              IROperand s = tcc_ir_op_get_src1(ir, q);
              int32_t svr = irop_get_vreg(s);
              if (svr < 0 || TCCIR_DECODE_VREG_TYPE(svr) != TCCIR_VREG_TYPE_VAR ||
                  !s.is_lval || s.is_llocal || TCCIR_DECODE_VREG_POSITION(svr) >= nvar)
                continue;
              int x = midx[TCCIR_DECODE_VREG_POSITION(svr)];
              if (x < 0)
                continue;
              uint64_t mask = st[x];
              int32_t tgt = -1;
              for (int bit = 0; mask; bit++, mask >>= 1)
              {
                if (!(mask & 1))
                  continue;
                if (tgt < 0)
                  tgt = btgt[x * 64 + bit];
                else if (tgt != btgt[x * 64 + bit])
                {
                  tgt = -1;
                  break;
                }
              }
              if (tgt >= 0)
                resol[i] = tgt;
            }
          }
        }
        tcc_free(btgt);
        tcc_free(ent);
        tcc_free(st);
      }
      tcc_free(midx);
    }
    if (cfg)
      tcc_ir_cfg_free(cfg);
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
  /* A V without one -- set only by a call's result, say -- gets the canonical
   * slot form of a VAR read, when every operand naming it is 32 bits wide. */
  {
    int8_t *vw = tcc_mallocz(nvar); /* 0 unseen, 1 all 32-bit, -1 mixed */
    for (int i = 0; i < n; i++)
    {
      IRQuadCompact *q = &ir->compact_instructions[i];
      if (q->op == TCCIR_OP_NOP || q->op == TCCIR_OP_LEA)
        continue;
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
        if (vr < 0 || TCCIR_DECODE_VREG_TYPE(vr) != TCCIR_VREG_TYPE_VAR || TCCIR_DECODE_VREG_POSITION(vr) >= nvar)
          continue;
        int pos = TCCIR_DECODE_VREG_POSITION(vr);
        int w32 = irop_get_btype(o) == IROP_BTYPE_INT32 && !o.is_llocal && !o.is_complex;
        if (vw[pos] == 0)
          vw[pos] = w32 ? 1 : -1;
        else if (!w32)
          vw[pos] = -1;
      }
    }
    for (int pos = 0; pos < nvar; pos++)
    {
      if (tmpl_ok[pos] || vw[pos] != 1)
        continue;
      int32_t v_vr = TCCIR_ENCODE_VREG(TCCIR_VREG_TYPE_VAR, pos);
      if (!tcc_ir_vreg_is_valid(ir, v_vr) || plf_vreg_is_volatile(ir, v_vr))
        continue;
      IRLiveInterval *iv = tcc_ir_get_live_interval(ir, v_vr);
      if (!iv)
        continue;
      tmpl[pos] = irop_make_stackoff(v_vr, iv->original_offset, 1, 0, 0, IROP_BTYPE_INT32);
      tmpl_ok[pos] = 2; /* synthesized: take the access marks from each use */
    }
    tcc_free(vw);
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
            /* A multi-target LEA VAR: the reaching-def walk named one local
             * for THIS copy. */
            if (tgt == PLF_UNKNOWN && resol && resol[i] >= 0)
              tgt = resol[i];
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
          int32_t svr = tcc_ir_op_src1_vreg(ir, q);
          if (svr >= 0 && TCCIR_DECODE_VREG_TYPE(svr) == TCCIR_VREG_TYPE_VAR && !tcc_ir_op_src1_is_lval(ir, q) &&
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
      IROperand rep = tmpl[v_pos];
      rep.is_unsigned = o.is_unsigned;
      if (tmpl_ok[v_pos] == 2)
        rep.aux = o.aux;
      if (irop_get_btype(o) != irop_get_btype(rep))
      {
        if (s != 0 || q->op != TCCIR_OP_LOAD || rep.btype != IROP_BTYPE_INT32 ||
            (o.btype != IROP_BTYPE_INT8 && o.btype != IROP_BTYPE_INT16) ||
            tcc_ir_access_is_volatile(ir, o))
          continue;
        IROperand d = tcc_ir_op_get_dest(ir, q);
        IROperand bits = irop_make_imm32(-1, (o.btype == IROP_BTYPE_INT8 ? 8 : 16) << 5, IROP_BTYPE_INT32);
        int base = tcc_ir_iroperand_pool_add(ir, d);
        tcc_ir_iroperand_pool_add(ir, rep);
        tcc_ir_iroperand_pool_add(ir, bits);
        q->operand_base = base;
        q->op = o.is_unsigned ? TCCIR_OP_UBFX : TCCIR_OP_SBFX;
        changes++;
        continue;
      }
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
        if (typ == TCCIR_VREG_TYPE_VAR && pos < nvar &&
            (ptr_target[pos] != PLF_UNKNOWN || (multi_lea && multi_lea[pos])) && !var_used[pos])
        {
          q->op = TCCIR_OP_NOP;
          cleaned = 1;
          changes++;
        }
        continue;
      }
      if (q->op != TCCIR_OP_ASSIGN && q->op != TCCIR_OP_LOAD)
        continue;
      if (typ == TCCIR_VREG_TYPE_VAR && pos < nvar && copy_def.data[pos] == i &&
          ptr_target[pos] >= 0 && !var_used[pos]) {
        q->op = TCCIR_OP_NOP;
        cleaned = 1;
        changes++;
        continue;
      }
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
  tcc_free(resol);
  tcc_free(multi_lea);
  tcc_free(mdef_bit);
  tcc_free(mdef_tgt);
  tcc_free(mdef_var);
  return changes;
}
