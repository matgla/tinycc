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
 * A copy VAR with several defs resolves to V flow-insensitively when every def
 * copies a VAR (or LEA) that itself resolves to V.  A pointer VAR whose defs
 * are LEAs of DIFFERENT locals, or copies of VARs naming different locals,
 * cannot be qualified that way; for those a forward points-to dataflow over
 * the CFG (an LEA def generates its local, a copy def takes its source's set)
 * gives each `T <-- P` use the set of locals P may hold, and a use whose set
 * is exactly one local resolves to it.  The Zig C backend reuses one C local
 * for the address temp of every inlined copy and one pointer local per type
 * for record re-reads (`t74 = t72` in one loop, `t74 = t101` in the next),
 * and the same shape recurs in hand-written code (`p = &a; ...; p = &b;`).
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
  /* lea64[pos(P)]: every `LEA P <-- &V` names a 64-bit V.  Gates the value-copy
   * acceptance below: 32-bit pointer flows keep today's slot-read-only rule
   * exactly (their rewrites are already tuned by the eql/slice shapes), while
   * a 64-bit pointee had no rewrite path at all before the INT64 template. */
  uint8_t *lea64 = tcc_malloc(nvar);
  memset(lea64, 1, nvar);

  /* Every `LEA P <-- &V` must name the same V... */
  int candidates = 0, direct_lea = 0, any_var_lea = 0;
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
    else
    {
      if (irop_get_btype(src) != IROP_BTYPE_INT64)
        lea64[p_pos] = 0;
      any_var_lea = 1;
      if (ptr_target[p_pos] == PLF_UNKNOWN)
        ptr_target[p_pos] = v_vr, candidates++;
      else if (ptr_target[p_pos] != v_vr)
        ptr_target[p_pos] = PLF_CONFLICT;
    }
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

  if (!any_var_lea && !direct_lea)
  {
    tcc_free(ptr_target);
    tcc_free(lea64);
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

  /* Inline bindings often copy &V through several VARs, and the Zig C backend
   * reuses one pointer local per type, re-assigning it (`t101 = t100`) at
   * every field read.  A copy VAR whose every def is a 32-bit value copy of a
   * VAR (or an `LEA <-- &V`) that already resolves to the same V can hold
   * only &V or its uninitialized value -- the same flow-insensitive argument
   * as for P itself -- unless its own home can be changed through an alias or
   * an implicit postincrement.  copy_def[p]: the single def, -1 none, -2
   * disqualified, -3 several (listed in copy_defs). */
  scoped_vector(int) copy_def = {0};
  scoped_vector(int) copy_ndef = {0};
  vector_resize(&copy_def, nvar);
  vector_resize(&copy_ndef, nvar);
  for (int p = 0; p < nvar; p++)
    copy_def.data[p] = -1, copy_ndef.data[p] = 0;
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
      else if (k == 0 && irop_dest_defines_vreg(o) && copy_def.data[p] != -2) {
        copy_def.data[p] = copy_def.data[p] == -1 ? i : -3;
        copy_ndef.data[p]++;
      }
    }
  }
  /* copy_defs: every def of each multi-def copy VAR, grouped by VAR. */
  scoped_vector(int) copy_dstart = {0};
  scoped_vector(int) copy_defs = {0};
  vector_resize(&copy_dstart, nvar + 1);
  {
    int total = 0;
    for (int p = 0; p < nvar; p++) {
      copy_dstart.data[p] = total;
      if (copy_def.data[p] == -3)
        total += copy_ndef.data[p];
    }
    copy_dstart.data[nvar] = total;
    vector_resize(&copy_defs, total);
    for (int p = 0; p < nvar; p++)
      copy_ndef.data[p] = 0;
    for (int i = 0; i < n && total; i++) {
      IRQuadCompact *q = &ir->compact_instructions[i];
      if (q->op == TCCIR_OP_NOP)
        continue;
      IROperand o = tcc_ir_op_get_slot(ir, q, 0);
      int32_t v = irop_get_vreg(o);
      if (v < 0 || TCCIR_DECODE_VREG_TYPE(v) != TCCIR_VREG_TYPE_VAR || !irop_dest_defines_vreg(o))
        continue;
      int p = TCCIR_DECODE_VREG_POSITION(v);
      if (p < nvar && copy_def.data[p] == -3)
        copy_defs.data[copy_dstart.data[p] + copy_ndef.data[p]++] = i;
    }
  }
  /* copy_res[p]: p was resolved here, so all its defs are pure copies/LEAs. */
  uint8_t *copy_res = tcc_mallocz(nvar);
  for (int round = 0; round < 8; round++) {
    int changed = 0;
    for (int p = 0; p < nvar; p++) {
      if (copy_def.data[p] == -1 || copy_def.data[p] == -2 || ptr_target[p] != PLF_UNKNOWN ||
          plf_vreg_is_volatile(ir, TCCIR_ENCODE_VREG(TCCIR_VREG_TYPE_VAR, p)))
        continue;
      int single = copy_def.data[p];
      const int *defs = single >= 0 ? &single : copy_defs.data + copy_dstart.data[p];
      int ndefs = single >= 0 ? 1 : copy_dstart.data[p + 1] - copy_dstart.data[p];
      int32_t tgt = PLF_UNKNOWN;
      for (int d = 0; d < ndefs; d++) {
        IRQuadCompact *q = &ir->compact_instructions[defs[d]];
        int32_t this_tgt = PLF_UNKNOWN;
        if (q->op == TCCIR_OP_LEA && !irop_config[q->op].has_src2 && single < 0) {
          /* A direct `&V` def among the copies (single-def LEAs are already
           * in ptr_target). */
          IROperand src = tcc_ir_op_get_src1(ir, q);
          int32_t v = irop_get_vreg(src);
          if (tcc_ir_op_get_dest(ir, q).btype == IROP_BTYPE_INT32 && v >= 0 &&
              TCCIR_DECODE_VREG_TYPE(v) == TCCIR_VREG_TYPE_VAR && !src.is_lval && !plf_vreg_is_volatile(ir, v))
            this_tgt = v;
        } else if (q->op == TCCIR_OP_LOAD || q->op == TCCIR_OP_ASSIGN) {
          IROperand src = tcc_ir_op_get_src1(ir, q);
          int32_t v = irop_get_vreg(src);
          if (tcc_ir_op_get_dest(ir, q).btype == IROP_BTYPE_INT32 && irop_is_vreg_value(src) &&
              src.btype == IROP_BTYPE_INT32 && v >= 0 && TCCIR_DECODE_VREG_TYPE(v) == TCCIR_VREG_TYPE_VAR &&
              TCCIR_DECODE_VREG_POSITION(v) < nvar && ptr_target[TCCIR_DECODE_VREG_POSITION(v)] >= 0)
            this_tgt = ptr_target[TCCIR_DECODE_VREG_POSITION(v)];
        }
        /* Unresolved yet, or a def that is no copy of &V: wait / give up. */
        if (this_tgt < 0 || (tgt >= 0 && tgt != this_tgt)) {
          tgt = PLF_UNKNOWN;
          break;
        }
        tgt = this_tgt;
      }
      if (tgt >= 0) {
        ptr_target[p] = tgt;
        copy_res[p] = 1;
        changed = 1;
      }
    }
    if (!changed)
      break;
  }

  /* Pointer VARs no flow-insensitive rule resolves are tracked per use: LEA
   * defs naming different locals (`p = &a; ...; p = &b`, or the Zig C
   * backend's reused address temp after inlining a helper twice) and the
   * backend's reused copy locals (`t74 = t72` in one loop, `t74 = t101` in
   * the next).  tracked[p]: p is not address-taken, postincremented or
   * volatile, every def is an `LEA <-- &V` or a 32-bit value copy of a VAR,
   * and p is in the pointer closure -- one def is an LEA or copies a resolved
   * or another tracked VAR (plain integer copies pass the def-shape test
   * too).  A forward dataflow over the CFG gives each tracked VAR that some
   * `T <-- P` use reads, directly or through tracked copies, the set of
   * locals whose address it may hold (bit PLF_TOP: anything else), and a
   * `T <-- P` copy where that set is one local resolves to it (resol[i]).
   * An unreached use (empty set) or a disagreeing one is left alone, which
   * is always safe while any LEA of the local survives.  Every closure
   * round below is capped: a VAR left out is only resolved less. */
  uint8_t *tracked = tcc_mallocz(nvar);
  int ntracked = 0;
  for (int p = 0; p < nvar; p++)
  {
    if (copy_def.data[p] == -1 || copy_def.data[p] == -2 || ptr_target[p] != PLF_UNKNOWN ||
        plf_vreg_is_volatile(ir, TCCIR_ENCODE_VREG(TCCIR_VREG_TYPE_VAR, p)))
      continue;
    int single = copy_def.data[p];
    const int *defs = single >= 0 ? &single : copy_defs.data + copy_dstart.data[p];
    int ndefs = single >= 0 ? 1 : copy_dstart.data[p + 1] - copy_dstart.data[p];
    int ok = 1;
    for (int d = 0; d < ndefs && ok; d++)
    {
      IRQuadCompact *q = &ir->compact_instructions[defs[d]];
      IROperand src = tcc_ir_op_get_src1(ir, q);
      int32_t v = irop_get_vreg(src);
      if (tcc_ir_op_get_dest(ir, q).btype != IROP_BTYPE_INT32 || v < 0 ||
          TCCIR_DECODE_VREG_TYPE(v) != TCCIR_VREG_TYPE_VAR || TCCIR_DECODE_VREG_POSITION(v) >= nvar)
        ok = 0;
      else if (q->op == TCCIR_OP_LEA)
        ok = !irop_config[q->op].has_src2 && !src.is_lval;
      else
        /* A volatile source's reads must survive: an unread tracked VAR has
         * its copy defs swept below. */
        ok = (q->op == TCCIR_OP_LOAD || q->op == TCCIR_OP_ASSIGN) && irop_is_vreg_value(src) &&
             src.btype == IROP_BTYPE_INT32 && !plf_vreg_is_volatile(ir, v);
    }
    if (ok)
      tracked[p] = 1; /* shape only; 2 = in the pointer closure */
  }
  for (int round = 0, changed = 1; changed && round < 8; round++)
  {
    changed = 0;
    for (int p = 0; p < nvar; p++)
    {
      if (tracked[p] != 1)
        continue;
      int single = copy_def.data[p];
      const int *defs = single >= 0 ? &single : copy_defs.data + copy_dstart.data[p];
      int ndefs = single >= 0 ? 1 : copy_dstart.data[p + 1] - copy_dstart.data[p];
      for (int d = 0; d < ndefs; d++)
      {
        IRQuadCompact *q = &ir->compact_instructions[defs[d]];
        int s = TCCIR_DECODE_VREG_POSITION(irop_get_vreg(tcc_ir_op_get_src1(ir, q)));
        if (q->op == TCCIR_OP_LEA || ptr_target[s] >= 0 || tracked[s] == 2)
        {
          tracked[p] = 2, changed = 1, ntracked++;
          break;
        }
      }
    }
  }
  for (int p = 0; p < nvar; p++)
    tracked[p] = tracked[p] == 2;

  candidates = 0;
  for (int i = 0; i < nvar; i++)
    candidates += ptr_target[i] != PLF_UNKNOWN;
  if (!candidates && !ntracked && !direct_lea)
  {
    tcc_free(tracked);
    tcc_free(copy_res);
    tcc_free(ptr_target);
    tcc_free(lea64);
    return 0;
  }

  int32_t *resol = NULL;
  /* The walk is CFG-sensitive: an edge the CFG does not show (a computed
   * goto's targets, an asm goto's labels, a returns_twice resume) could hide a
   * disagreeing def, so those functions get no resolution at all. */
  int resol_ok = ntracked && ntemp > 0 && !ir->func_has_label_addr && !ir->func_has_asm_goto &&
                 !tcc_ir_calls_returns_twice(ir);
  for (int i = 0; resol_ok && i < n; i++)
    if (ir->compact_instructions[i].op == TCCIR_OP_IJUMP)
      resol_ok = 0;
  /* df_var[p]: p's set is computed -- a `T <-- P` use reads it, or it is
   * copied into such a VAR (tidx[p] then numbers it). */
  int *tidx = NULL;
  int nx = 0;
  if (resol_ok)
  {
    tidx = tcc_malloc(sizeof(int) * nvar);
    for (int p = 0; p < nvar; p++)
      tidx[p] = -1;
    for (int i = 0; i < n; i++)
    {
      IRQuadCompact *q = &ir->compact_instructions[i];
      if (q->op != TCCIR_OP_LOAD && q->op != TCCIR_OP_ASSIGN)
        continue;
      IROperand d = tcc_ir_op_get_dest(ir, q);
      int32_t dvr = irop_get_vreg(d);
      IROperand src = tcc_ir_op_get_src1(ir, q);
      int32_t svr = irop_get_vreg(src);
      if (dvr >= 0 && TCCIR_DECODE_VREG_TYPE(dvr) == TCCIR_VREG_TYPE_TEMP && !d.is_lval && !src.is_llocal &&
          svr >= 0 && TCCIR_DECODE_VREG_TYPE(svr) == TCCIR_VREG_TYPE_VAR &&
          TCCIR_DECODE_VREG_POSITION(svr) < nvar && tracked[TCCIR_DECODE_VREG_POSITION(svr)])
        tidx[TCCIR_DECODE_VREG_POSITION(svr)] = 0;
    }
    for (int round = 0, changed = 1; changed && round < 8; round++)
    {
      changed = 0;
      for (int p = nvar - 1; p >= 0; p--)
      {
        if (tidx[p] < 0 || copy_def.data[p] == -1)
          continue;
        int single = copy_def.data[p];
        const int *defs = single >= 0 ? &single : copy_defs.data + copy_dstart.data[p];
        int ndefs = single >= 0 ? 1 : copy_dstart.data[p + 1] - copy_dstart.data[p];
        for (int d = 0; d < ndefs; d++)
        {
          IRQuadCompact *q = &ir->compact_instructions[defs[d]];
          if (q->op == TCCIR_OP_LEA)
            continue;
          int s = TCCIR_DECODE_VREG_POSITION(irop_get_vreg(tcc_ir_op_get_src1(ir, q)));
          if (tracked[s] && tidx[s] < 0)
            tidx[s] = 0, changed = 1;
        }
      }
    }
    for (int p = 0; p < nvar; p++)
      if (tidx[p] >= 0)
        tidx[p] = nx++;
  }
  IRCFG *cfg = nx ? tcc_ir_cfg_build(ir) : NULL;
  /* The per-block entry sets are the dominant cost: cap them (and so the
   * function) at PLF_DF_CELLS 64-bit sets, 256 KiB. */
#define PLF_DF_CELLS ((size_t)1 << 15)
  if (cfg && cfg->num_blocks > 0 && (size_t)cfg->num_blocks * nx <= PLF_DF_CELLS)
  {
#define PLF_TOP ((uint64_t)1 << 63)
    tcc_ir_cfg_compute_rpo(cfg);
    const int nb = cfg->num_blocks;
    /* The transfer function, as a sparse event list grouped by block: a
     * tracked VAR's def either copies another one's set (src >= 0) or writes
     * a constant set (gen), and a `T <-- P` copy is a use (var < 0, src = P). */
    typedef struct
    {
      int32_t insn, var, src;
      uint64_t gen;
    } PlfEvent;
    scoped_vector(PlfEvent) ev = {0};
    scoped_vector(int) ev_start = {0};
    vector_resize(&ev_start, nb + 1);
    int32_t tgt_of_bit[63];
    int nbits = 0;
    for (int b = 0; b < nb; b++)
    {
      ev_start.data[b] = (int)ev.size;
      IRBasicBlock *bb = &cfg->blocks[b];
      for (int i = bb->start_idx; i < bb->end_idx; i++)
      {
        IRQuadCompact *q = &ir->compact_instructions[i];
        if (q->op != TCCIR_OP_LEA && q->op != TCCIR_OP_LOAD && q->op != TCCIR_OP_ASSIGN)
          continue;
        IROperand d = tcc_ir_op_get_dest(ir, q);
        int32_t dvr = irop_get_vreg(d);
        IROperand src = tcc_ir_op_get_src1(ir, q);
        int32_t svr = irop_get_vreg(src);
        if (dvr < 0 || svr < 0 || TCCIR_DECODE_VREG_TYPE(svr) != TCCIR_VREG_TYPE_VAR ||
            TCCIR_DECODE_VREG_POSITION(svr) >= nvar)
          continue;
        int spos = TCCIR_DECODE_VREG_POSITION(svr);
        PlfEvent e = {i, -1, -1, 0};
        if (TCCIR_DECODE_VREG_TYPE(dvr) == TCCIR_VREG_TYPE_TEMP)
        {
          if (q->op == TCCIR_OP_LEA || d.is_lval || src.is_llocal || tidx[spos] < 0)
            continue;
          e.src = tidx[spos];
          vector_push_back(&ev, e);
          continue;
        }
        if (TCCIR_DECODE_VREG_TYPE(dvr) != TCCIR_VREG_TYPE_VAR || TCCIR_DECODE_VREG_POSITION(dvr) >= nvar ||
            tidx[TCCIR_DECODE_VREG_POSITION(dvr)] < 0 || !irop_dest_defines_vreg(d))
          continue;
        e.var = tidx[TCCIR_DECODE_VREG_POSITION(dvr)];
        int32_t tgt = -1;
        if (q->op == TCCIR_OP_LEA)
          tgt = plf_vreg_is_volatile(ir, svr) ? -1 : svr;
        else if (ptr_target[spos] >= 0)
          tgt = ptr_target[spos];
        else if (tidx[spos] >= 0)
          e.src = tidx[spos];
        if (e.src < 0)
        {
          e.gen = PLF_TOP;
          for (int k = 0; tgt >= 0 && k < nbits; k++)
            if (tgt_of_bit[k] == tgt)
              e.gen = (uint64_t)1 << k;
          if (tgt >= 0 && e.gen == PLF_TOP && nbits < 63)
          {
            tgt_of_bit[nbits] = tgt;
            e.gen = (uint64_t)1 << nbits++;
          }
        }
        vector_push_back(&ev, e);
      }
    }
    ev_start.data[nb] = (int)ev.size;
    uint64_t *ent = tcc_mallocz(sizeof(uint64_t) * (size_t)nb * nx);
    uint64_t *st = tcc_malloc(sizeof(uint64_t) * nx);
    int settled = 0;
    for (int round = 0; !settled && round < 64; round++)
    {
      settled = 1;
      for (int r = 0; r < cfg->rpo_count; r++)
      {
        const int b = cfg->rpo_order[r];
        IRBasicBlock *bb = &cfg->blocks[b];
        memcpy(st, ent + (size_t)b * nx, sizeof(uint64_t) * nx);
        for (int k = ev_start.data[b]; k < ev_start.data[b + 1]; k++)
        {
          const PlfEvent *e = &ev.data[k];
          if (e->var >= 0 && ir->compact_instructions[e->insn].op != TCCIR_OP_NOP)
            st[e->var] = e->src >= 0 ? st[e->src] : e->gen;
        }
        for (int s = 0; s < bb->num_succs; s++)
        {
          uint64_t *dst = ent + (size_t)bb->succs[s] * nx;
          for (int x = 0; x < nx; x++)
            if (st[x] & ~dst[x])
            {
              dst[x] |= st[x];
              settled = 0;
            }
        }
      }
    }
    /* A walk that did not settle under-approximates the sets and could hide
     * a disagreeing def: no resolution at all then. */
    for (int b = 0; settled && b < nb; b++)
    {
      memcpy(st, ent + (size_t)b * nx, sizeof(uint64_t) * nx);
      for (int k = ev_start.data[b]; k < ev_start.data[b + 1]; k++)
      {
        const PlfEvent *e = &ev.data[k];
        if (ir->compact_instructions[e->insn].op == TCCIR_OP_NOP)
          continue;
        if (e->var >= 0)
        {
          st[e->var] = e->src >= 0 ? st[e->src] : e->gen;
          continue;
        }
        uint64_t m = st[e->src];
        if (!m || (m & (m - 1)) || m == PLF_TOP)
          continue;
        int bit = 0;
        while (!(m & 1))
          m >>= 1, bit++;
        if (!resol)
        {
          resol = tcc_malloc(sizeof(int32_t) * n);
          for (int i = 0; i < n; i++)
            resol[i] = -1;
        }
        resol[e->insn] = tgt_of_bit[bit];
      }
    }
    tcc_free(ent);
    tcc_free(st);
#undef PLF_TOP
  }
#undef PLF_DF_CELLS
  tcc_free(tidx);
  if (cfg)
    tcc_ir_cfg_free(cfg);

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
  /* A 64-bit local with no 32-bit access at all (the Zig C backend's scalar
   * u64 tuples, IntegerContext(u16).hash's `t8 = a; t11 = &t8; x = *t11`)
   * still has its slot spelled by an INT64 lval access: clone one of those
   * instead, and only same-btype rewrites use it (the UBFX narrowing path
   * below is INT32-template-only, unchanged). */
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
    if (!d.is_lval || d.is_llocal || irop_get_btype(d) != IROP_BTYPE_INT64)
      continue;
    tmpl[pos] = d;
    tmpl_ok[pos] = 3;
  }
  /* A V without one -- set only by a call's result, say -- gets the canonical
   * slot form of a VAR read, when every operand naming it is 32 bits wide. */
  {
    int8_t *vw = tcc_mallocz(nvar);  /* 0 unseen, 1 all 32-bit, -1 mixed */
    int8_t *vw64 = tcc_mallocz(nvar); /* 0 unseen, 1 all 64-bit, -1 mixed */
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
        int w64 = irop_get_btype(o) == IROP_BTYPE_INT64 && !o.is_llocal && !o.is_complex;
        if (vw[pos] == 0)
          vw[pos] = w32 ? 1 : -1;
        else if (!w32)
          vw[pos] = -1;
        if (vw64[pos] == 0)
          vw64[pos] = w64 ? 1 : -1;
        else if (!w64)
          vw64[pos] = -1;
      }
    }
    for (int pos = 0; pos < nvar; pos++)
    {
      if (tmpl_ok[pos])
        continue;
      int32_t v_vr = TCCIR_ENCODE_VREG(TCCIR_VREG_TYPE_VAR, pos);
      if (!tcc_ir_vreg_is_valid(ir, v_vr) || plf_vreg_is_volatile(ir, v_vr))
        continue;
      IRLiveInterval *iv = tcc_ir_get_live_interval(ir, v_vr);
      if (!iv)
        continue;
      /* All-32-bit locals keep the INT32 canonical form; an all-64-bit local
       * (a u64 the Zig C backend only ever reads through &t, so no lval
       * access exists to clone) gets an INT64 one -- same-width rewrites
       * only.  Mixed-width locals keep no synthesized template. */
      if (vw[pos] == 1)
      {
        tmpl[pos] = irop_make_stackoff(v_vr, iv->original_offset, 1, 0, 0, IROP_BTYPE_INT32);
        tmpl_ok[pos] = 2; /* synthesized: take the access marks from each use */
      }
      else if (vw64[pos] == 1)
      {
        tmpl[pos] = irop_make_stackoff(v_vr, iv->original_offset, 1, 0, 0, IROP_BTYPE_INT64);
        tmpl_ok[pos] = 2;
      }
    }
    tcc_free(vw);
    tcc_free(vw64);
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
          if (svr >= 0 && TCCIR_DECODE_VREG_TYPE(svr) == TCCIR_VREG_TYPE_VAR &&
              TCCIR_DECODE_VREG_POSITION(svr) < nvar && !s.is_llocal)
          {
            int32_t tgt = ptr_target[TCCIR_DECODE_VREG_POSITION(svr)];
            /* A multi-target LEA VAR: the reaching-def walk named one local
             * for THIS copy. */
            if (tgt == PLF_UNKNOWN && resol && resol[i] >= 0)
              tgt = resol[i];
            if (tgt != PLF_UNKNOWN)
            {
              /* T <-- P: either spelling of the hand-off.  A slot read
               * (`P` lval) is the classic one; the Zig C backend also spells
               * the copy as a value read (`t11 = &t8; T <-- t11` with P
               * non-lval, IntegerContext(u16).hash's tuple destructuring),
               * and P's defs are still all `LEA <-- &V`, so the value is the
               * same address either way.  Value-read copies only chase
               * 64-bit pointees, where no rewrite path existed before. */
              if (s.is_lval || (q->op == TCCIR_OP_ASSIGN && lea64[TCCIR_DECODE_VREG_POSITION(svr)]))
                val = tgt;
            }
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
      /* A volatile access through &V (`*(volatile int *)&x = v`) stays one:
       * the direct VAR access would be promoted to a register. */
      if (tcc_ir_access_is_volatile(ir, o))
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
            (o.btype != IROP_BTYPE_INT8 && o.btype != IROP_BTYPE_INT16))
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
            (ptr_target[pos] != PLF_UNKNOWN || tracked[pos]) && !var_used[pos])
        {
          q->op = TCCIR_OP_NOP;
          cleaned = 1;
          changes++;
        }
        continue;
      }
      if (q->op != TCCIR_OP_ASSIGN && q->op != TCCIR_OP_LOAD)
        continue;
      /* Unread copy VAR whose defs are all pure copies/LEAs. */
      if (typ == TCCIR_VREG_TYPE_VAR && pos < nvar && (copy_res[pos] || tracked[pos]) && !var_used[pos]) {
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
  tcc_free(copy_res);
  tcc_free(tmpl_ok);
  tcc_free(tmpl);
  tcc_free(ptr_target);
  tcc_free(lea64);
  tcc_free(resol);
  tcc_free(tracked);
  return changes;
}
