/*
 *  TCC IR - SSA Copy Propagation
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#define USING_GLOBALS
#include "ir.h"
#include "ssa_opt.h"
#include "opt_dsl.h"
#include "opt_dsl_ssa.h"
#include "opt/ssa/cprop.h"
#include "opt/ssa/ssa_opt_helpers.h"
#include <limits.h>

/* ASSIGN dest=src, both TEMP vregs: forward src into all uses of dest.  Skip
 * width-converting ASSIGNs (differing btype) and non-TEMP srcs — PARAM/VAR are
 * multi-def, not SSA-renamed. */
OPT_GEN_SSA(cprop_assign, TCCIR_OP_ASSIGN) {
  PATTERN(.constraints = { .dest = IR_CONSTRAINT_ANY });
  GUARD(
    when(src1.tag == IROP_TAG_VREG);
    and_not(src1.is_lval || src1.is_llocal || src1.is_local);
    and(vreg(dest) >= 0 && vreg(src1) >= 0);
    and(TCCIR_DECODE_VREG_TYPE(vreg(dest)) == TCCIR_VREG_TYPE_TEMP);
    and(TCCIR_DECODE_VREG_TYPE(vreg(src1)) == TCCIR_VREG_TYPE_TEMP);
    and(irop_get_btype(dest) == irop_get_btype(src1)));

  int32_t dest_vr = vreg(dest);
  int32_t src_vr = vreg(src1);

  IRSSAVregInfo *vi = ssa_opt_vinfo(ctx, dest_vr);
  if (ssa_opt_def_total(vi) > 1)
    return 0;

  /* The SOURCE must be single-def too.  A TEMP is not automatically SSA: an
   * in-place `T <-- x [STORE]` (what a register-promoted local's assignment
   * lowers to) is a second def under the SAME name, so forwarding src into a
   * use that sits AFTER that store hands out the new value where the old one
   * was copied.  ptr fuzz seed 2513:
   *     T17 <-- #a [ASSIGN]      u5 = a
   *     T3  <-- T17 [ASSIGN]     u4 = u5      (copy of the OLD value)
   *     T17 <-- #b [STORE]       u5 = b
   *     PARAM1 T3                            -> rewritten to T17, i.e. #b.
   *
   * Both tests go through ssa_opt_def_total, not def_count: when the first def
   * is a PHI (the same local written on one arm of an if) def_count is 1 even
   * though the STORE makes two, and the plain check let exactly this shape
   * through a second time. */
  IRSSAVregInfo *svi = ssa_opt_vinfo(ctx, src_vr);
  if (ssa_opt_def_total(svi) > 1)
    return 0;

  /* dest feeding a phi: folding it away reintroduces the lost-copy problem at
   * out-of-SSA resolution (fuzz seed 2698). */
  if (vi) {
    for (int u = 0; u < vi->use_count; u++)
      if (vi->uses[u].kind == SSA_USE_PHI)
        return 0;
  }

  int replaced = ssa_opt_replace_all_uses(ctx, dest_vr, src_vr);
  return replaced > 0 ? 1 : 0;
}

/* ASSIGN dest=#imm32 (TEMP dest): materialize the immediate into every use of
 * dest.  Hand-written: rewrites many use sites, not instruction i. */
static int ssa_gen_cprop_imm(IRSSAOptCtx *ctx, int idx)
{
  TCCIRState *ir = ctx->ir;
  IRQuadCompact *q = &ir->compact_instructions[idx];

  IROperand dest = tcc_ir_op_get_dest(ir, q);
  IROperand src = tcc_ir_op_get_src1(ir, q);

  int32_t dest_vr = irop_get_vreg(dest);
  if (dest_vr < 0 || TCCIR_DECODE_VREG_TYPE(dest_vr) != TCCIR_VREG_TYPE_TEMP)
    return 0;

  if (!irop_is_immediate(src))
    return 0;
  if (src.is_lval)
    return 0;

  IRSSAVregInfo *vi = ssa_opt_vinfo(ctx, dest_vr);
  if (!vi || vi->use_count == 0)
    return 0;
  if (vi->def_count > 1)
    return 0;

  int instr_uses = 0;
  for (int u = 0; u < vi->use_count; u++) {
    IRSSAUse use = vi->uses[u];
    if (use.kind != SSA_USE_INSTR)
      continue;

    IRQuadCompact *uq = &ir->compact_instructions[use.idx];
    if (tcc_ir_barrel_shift_at(ir, uq))
      return 0;

    int found = 0;
    if (irop_config[uq->op].has_src1) {
      IROperand s = tcc_ir_op_get_src1(ir, uq);
      if (irop_get_vreg(s) == dest_vr) {
        if (s.is_lval || s.is_local || s.is_llocal)
          return 0;
        if (uq->op == TCCIR_OP_CMP &&
            !ssa_cprop_imm_other_operand_const(ir, uq, dest_vr, 1))
          return 0;
        if ((uq->op == TCCIR_OP_BOOL_AND || uq->op == TCCIR_OP_BOOL_OR) &&
            !ssa_cprop_imm_other_operand_const(ir, uq, dest_vr, 1))
          return 0;
        found = 1;
      }
    }
    if (irop_config[uq->op].has_src2) {
      IROperand s = tcc_ir_op_get_src2(ir, uq);
      if (irop_get_vreg(s) == dest_vr) {
        if (s.is_lval || s.is_local || s.is_llocal)
          return 0;
        if ((uq->op == TCCIR_OP_BOOL_AND || uq->op == TCCIR_OP_BOOL_OR) &&
            !ssa_cprop_imm_other_operand_const(ir, uq, dest_vr, 2))
          return 0;
        found = 1;
      }
    }
    if (uq->op == TCCIR_OP_MLA) {
      IROperand a = tcc_ir_op_get_accum(ir, uq);
      if (irop_get_vreg(a) == dest_vr) {
        if (a.is_lval || a.is_local || a.is_llocal)
          return 0;
        found = 1;
      }
    }
    if (uq->op == TCCIR_OP_STORE || uq->op == TCCIR_OP_STORE_INDEXED ||
        uq->op == TCCIR_OP_STORE_POSTINC) {
      IROperand d = tcc_ir_op_get_dest(ir, uq);
      if (irop_get_vreg(d) == dest_vr)
        return 0;
    }
    if (!found)
      return 0;
    instr_uses++;
  }

  if (instr_uses == 0)
    return 0;

  int count = 0;
  int original_use_count = vi->use_count;
  IRSSAUse *uses = tcc_mallocz(original_use_count * sizeof(IRSSAUse));
  memcpy(uses, vi->uses, original_use_count * sizeof(IRSSAUse));
  for (int u = 0; u < original_use_count; u++) {
    IRSSAUse use = uses[u];
    if (use.kind != SSA_USE_INSTR)
      continue;
    IRQuadCompact *uq = &ir->compact_instructions[use.idx];

    int rewrote = 0;
    if (irop_config[uq->op].has_src1) {
      IROperand s = tcc_ir_op_get_src1(ir, uq);
      if (irop_get_vreg(s) == dest_vr && !s.is_lval) {
        tcc_ir_op_set_src1(ir, uq, ssa_cprop_imm_for_use(src, s));
        rewrote = 1;
      }
    }
    if (irop_config[uq->op].has_src2) {
      IROperand s = tcc_ir_op_get_src2(ir, uq);
      if (irop_get_vreg(s) == dest_vr && !s.is_lval) {
        tcc_ir_op_set_src2(ir, uq, ssa_cprop_imm_for_use(src, s));
        rewrote = 1;
      }
    }
    if (uq->op == TCCIR_OP_MLA) {
      IROperand a = tcc_ir_op_get_accum(ir, uq);
      if (irop_get_vreg(a) == dest_vr && !a.is_lval) {
        tcc_ir_op_set_accum(ir, uq, ssa_cprop_imm_for_use(src, a));
        rewrote = 1;
      }
    }

    if (rewrote) {
      if (uq->op == TCCIR_OP_LOAD) {
        uq->op = TCCIR_OP_ASSIGN;
        tcc_ir_op_set_src2(ir, uq, IROP_NONE);
      }
      ssa_opt_remove_use_instr(vi, use.idx);
      count++;
    }
  }
  tcc_free(uses);

  if (vi->use_count == 0)
    ssa_opt_nop_instr(ctx, idx);
  return count > 0 ? 1 : 0;
}

/* Redundant LOAD: T2 <- V where a prior same-block LOAD of the same source V
 * (no intervening write) produced T1 -> rewrite to T2 <- T1 [ASSIGN].  Source V
 * is a plain VREG or a STACKOFF (spilled VAR); skip llocal / tag mismatches. */
OPT_GEN_SSA(cprop_load_redundant, TCCIR_OP_LOAD) {
  IRCFG *cfg = ctx->cfg;
  PATTERN(.constraints = { .dest = IR_CONSTRAINT_ANY });
  GUARD(
    when(cfg != NULL);
    and_not(src1.is_llocal);
    and(src1.tag == IROP_TAG_VREG || src1.tag == IROP_TAG_STACKOFF);
    and(vreg(src1) >= 0);
    and(vreg(dest) >= 0 && TCCIR_DECODE_VREG_TYPE(vreg(dest)) == TCCIR_VREG_TYPE_TEMP));

  IROperand src = src1;
  int32_t src_vr = vreg(src1);

  /* Never CSE a load of a volatile VAR against a prior load: each read is a
   * mandated memory access that must survive.  The live interval carries that
   * for a declared `volatile` local; for everything else — a deref of a
   * `volatile T *`, a volatile member, a cast — the operand's access mark is
   * the only record. */
  if (src_vr >= 0 && TCCIR_DECODE_VREG_TYPE(src_vr) == TCCIR_VREG_TYPE_VAR) {
    IRLiveInterval *si = tcc_ir_vreg_live_interval(ir, src_vr);
    if (si && si->is_volatile)
      return 0;
  }
  if (tcc_ir_access_is_volatile(ir, src1))
    return 0;

  int blk = cfg->instr_to_block[i];
  if (blk < 0 || blk >= cfg->num_blocks)
    return 0;
  IRBasicBlock *bb = &cfg->blocks[blk];

  /* Backward scan for a prior LOAD of src_vr; bail on any intervening def,
   * store, call, or VLA. */
  int prior_dest_vr = -1;
  for (int k = i - 1; k >= bb->start_idx; k--) {
    IRQuadCompact *pq = &ir->compact_instructions[k];
    if (pq->op == TCCIR_OP_NOP)
      continue;

    /* Barriers may clobber the source's storage; for deref LOADs (is_lval) any
     * STORE may alias the read memory (no alias analysis). */
    if (ssa_op_is_call_barrier(pq->op))
      return 0;
    if ((pq->op == TCCIR_OP_STORE || pq->op == TCCIR_OP_STORE_INDEXED ||
         pq->op == TCCIR_OP_STORE_POSTINC) && src.is_lval)
      return 0;

    /* Bail if anything writes src_vr, directly or via its stack slot. */
    if (irop_config[pq->op].has_dest &&
        pq->op != TCCIR_OP_FUNCPARAMVAL && pq->op != TCCIR_OP_FUNCPARAMVOID) {
      IROperand pd = tcc_ir_op_get_dest(ir, pq);
      int32_t pd_vr = irop_get_vreg(pd);
      if (pd_vr == src_vr)
        return 0;
      /* Deref LOAD via pointer: a def of an address-taken VAR/PARAM writes the
       * pointed-to slot too (fuzz ptr seed 6734). */
      if (src.is_lval && !src.is_local && pd_vr >= 0 &&
          TCCIR_DECODE_VREG_TYPE(pd_vr) != TCCIR_VREG_TYPE_TEMP) {
        IRLiveInterval *pdi =
            (TCCIR_DECODE_VREG_TYPE(pd_vr) == TCCIR_VREG_TYPE_VAR ||
             TCCIR_DECODE_VREG_TYPE(pd_vr) == TCCIR_VREG_TYPE_PARAM)
                ? tcc_ir_vreg_live_interval(ir, pd_vr)
                : NULL;
        if (!pdi || pdi->addrtaken)
          return 0;
      }
    }

    if (pq->op != TCCIR_OP_LOAD)
      continue;
    IROperand ps = tcc_ir_op_get_src1(ir, pq);
    if (ps.is_llocal)
      continue;
    if (ps.tag != src.tag)
      continue;
    if (ps.is_lval != src.is_lval || ps.is_local != src.is_local)
      continue;
    if (irop_get_vreg(ps) != src_vr)
      continue;
    IROperand pd = tcc_ir_op_get_dest(ir, pq);
    int32_t pd_vr = irop_get_vreg(pd);
    if (pd_vr < 0 || TCCIR_DECODE_VREG_TYPE(pd_vr) != TCCIR_VREG_TYPE_TEMP)
      continue;
    prior_dest_vr = pd_vr;
    break;
  }

  if (prior_dest_vr < 0)
    return 0;

  IROperand new_src = src;
  irop_set_vreg(&new_src, prior_dest_vr);
  new_src.is_lval = 0;
  new_src.is_local = 0;
  new_src.is_llocal = 0;

  /* Move the use edge from src_vr to prior_dest_vr. */
  IRSSAVregInfo *svi = ssa_opt_vinfo(ctx, src_vr);
  if (svi)
    ssa_opt_remove_use_instr(svi, i);
  IRSSAVregInfo *pvi = ssa_opt_vinfo(ctx, prior_dest_vr);
  if (pvi)
    ssa_opt_add_use_instr(pvi, i);

  REWRITE(.new_op = TCCIR_OP_ASSIGN, .src1 = new_src);
}

/* Symref CSE: T2 <- &sym where a prior same-block ASSIGN of the same symref
 * produced T1 -> rewrite to T2 <- T1, dropping the duplicate literal load.
 * Only plain address materializations (not lval/local/llocal symrefs). */
OPT_GEN_SSA(cprop_symref_cse, TCCIR_OP_ASSIGN) {
  IRCFG *cfg = ctx->cfg;
  PATTERN(.constraints = { .dest = IR_CONSTRAINT_ANY });
  GUARD(
    when(cfg != NULL);
    and(src1.tag == IROP_TAG_SYMREF);
    and_not(src1.is_lval || src1.is_local || src1.is_llocal);
    and(vreg(dest) >= 0 && TCCIR_DECODE_VREG_TYPE(vreg(dest)) == TCCIR_VREG_TYPE_TEMP));

  IRPoolSymref *cur_sr = irop_get_symref_ex(ir, src1);
  if (!cur_sr)
    return 0;

  int blk = cfg->instr_to_block[i];
  if (blk < 0 || blk >= cfg->num_blocks)
    return 0;
  IRBasicBlock *bb = &cfg->blocks[blk];

  int32_t prior_dest_vr = -1;
  for (int k = i - 1; k >= bb->start_idx; k--) {
    IRQuadCompact *pq = &ir->compact_instructions[k];
    if (pq->op == TCCIR_OP_NOP)
      continue;

    /* Calls/asm/VLA/setjmp can make symbol-address caching unsafe. */
    if (ssa_op_is_call_barrier(pq->op))
      return 0;

    if (pq->op != TCCIR_OP_ASSIGN)
      continue;
    IROperand ps = tcc_ir_op_get_src1(ir, pq);
    if (ps.tag != IROP_TAG_SYMREF)
      continue;
    if (ps.is_lval || ps.is_local || ps.is_llocal)
      continue;
    IRPoolSymref *ps_sr = irop_get_symref_ex(ir, ps);
    if (!ps_sr || ps_sr->sym != cur_sr->sym || ps_sr->addend != cur_sr->addend)
      continue;
    IROperand pd = tcc_ir_op_get_dest(ir, pq);
    int32_t pd_vr = irop_get_vreg(pd);
    if (pd_vr < 0 || TCCIR_DECODE_VREG_TYPE(pd_vr) != TCCIR_VREG_TYPE_TEMP)
      continue;
    prior_dest_vr = pd_vr;
    break;
  }

  if (prior_dest_vr < 0)
    return 0;

  IROperand new_src = (IROperand){0};
  new_src.tag = IROP_TAG_VREG;
  irop_set_vreg(&new_src, prior_dest_vr);
  new_src.is_lval = 0;

  IRSSAVregInfo *pvi = ssa_opt_vinfo(ctx, prior_dest_vr);
  if (pvi)
    ssa_opt_add_use_instr(pvi, i);

  REWRITE(.src1 = new_src);
}

/* Verify every use of the single-def TEMP `dvi` is an instruction in copy_blk
 * strictly after idx; return 1 and set *max_use_idx, or 0 to bail. */
static int cprop_copy_uses_after_in_block(IRSSAOptCtx *ctx, IRSSAVregInfo *dvi,
                                          int idx, int copy_blk, int *max_use_idx)
{
  IRCFG *cfg = ctx->cfg;
  int mx = idx;
  for (int u = 0; u < dvi->use_count; u++) {
    IRSSAUse use = dvi->uses[u];
    if (use.kind != SSA_USE_INSTR)
      return 0;
    if (cfg->instr_to_block[use.idx] != copy_blk || use.idx <= idx)
      return 0;
    if (use.idx > mx)
      mx = use.idx;
  }
  *max_use_idx = mx;
  return 1;
}

/* Scan (idx, max_use_idx] for anything that invalidates forwarding src_vr: a
 * call/asm barrier, an aliasing store to an address-taken src, or a redef of
 * src_vr before its last use.  Returns 1 if the range is clear, 0 to bail. */
static int cprop_copy_redef_scan(TCCIRState *ir, int idx, int max_use_idx,
                                 int32_t src_vr, int src_addrtaken)
{
  for (int k = idx + 1; k <= max_use_idx; k++) {
    IRQuadCompact *kq = &ir->compact_instructions[k];
    if (kq->op == TCCIR_OP_NOP)
      continue;
    if (ssa_op_is_call_barrier(kq->op) || ssa_op_is_asm_operand(kq->op))
      return 0;
    /* A deref/indexed store may alias an address-taken src (no alias analysis);
     * a plain slot store (is_lval=0) cannot clobber a different src. */
    if (src_addrtaken &&
        (kq->op == TCCIR_OP_STORE || kq->op == TCCIR_OP_STORE_INDEXED ||
         kq->op == TCCIR_OP_STORE_POSTINC)) {
      IROperand kd = tcc_ir_op_get_dest(ir, kq);
      if (kd.is_lval || kq->op == TCCIR_OP_STORE_INDEXED ||
          kq->op == TCCIR_OP_STORE_POSTINC)
        return 0;
    }
    if (irop_config[kq->op].has_dest &&
        kq->op != TCCIR_OP_FUNCPARAMVAL && kq->op != TCCIR_OP_FUNCPARAMVOID) {
      IROperand kd = tcc_ir_op_get_dest(ir, kq);
      int dest_defines =
          !(kq->op == TCCIR_OP_STORE_INDEXED || kq->op == TCCIR_OP_STORE_POSTINC ||
            (kq->op == TCCIR_OP_STORE && kd.is_lval));
      if (dest_defines && irop_get_vreg(kd) == src_vr && k < max_use_idx)
        return 0;
    }
  }
  return 1;
}

/* Copy of a register-resident PARAM/VAR into a single-def TEMP: forward src
 * into all uses of the TEMP and nop the copy, when every use is same-block after
 * the copy and no redef/barrier intervenes.  cprop_assign refuses PARAM/VAR
 * (multi-def, not SSA-renamed); this adds the dataflow check that makes it safe.
 * Hand-written: full-BB def-to-use scan, does not fit OPT_GEN_SSA (plan B3). */
static int ssa_gen_cprop_copy_param(IRSSAOptCtx *ctx, int idx)
{
  TCCIRState *ir = ctx->ir;
  IRCFG *cfg = ctx->cfg;
  if (!cfg)
    return 0;

  IRQuadCompact *q = &ir->compact_instructions[idx];
  IROperand src = tcc_ir_op_get_src1(ir, q);
  IROperand dest = tcc_ir_op_get_dest(ir, q);

  if (src.is_lval || src.is_local || src.is_llocal)
    return 0;
  if (src.tag != IROP_TAG_VREG)
    return 0;
  int32_t src_vr = irop_get_vreg(src);
  if (src_vr < 0)
    return 0;
  int src_type = TCCIR_DECODE_VREG_TYPE(src_vr);
  if (src_type != TCCIR_VREG_TYPE_PARAM && src_type != TCCIR_VREG_TYPE_VAR)
    return 0;

  int32_t dest_vr = irop_get_vreg(dest);
  if (dest_vr < 0 || TCCIR_DECODE_VREG_TYPE(dest_vr) != TCCIR_VREG_TYPE_TEMP)
    return 0;

  /* Differing btype/signedness means the LOAD narrows/widens; forwarding src
   * would skip that conversion. */
  if (irop_get_btype(src) != irop_get_btype(dest) ||
      src.is_unsigned != dest.is_unsigned)
    return 0;

  /* A sub-word PARAM/VAR LOAD is where AAPCS-promoted upper bits get masked;
   * forwarding src leaves that garbage. Only ASSIGN (pure copy) is safe. */
  int src_btype = irop_get_btype(src);
  if (q->op == TCCIR_OP_LOAD &&
      (src_btype == IROP_BTYPE_INT8 || src_btype == IROP_BTYPE_INT16))
    return 0;
  IRSSAVregInfo *dvi = ssa_opt_vinfo(ctx, dest_vr);
  if (!dvi || dvi->def_count != 1 || dvi->use_count == 0)
    return 0;

  int copy_blk = cfg->instr_to_block[idx];
  if (copy_blk < 0 || copy_blk >= cfg->num_blocks)
    return 0;

  /* Address-taken src: an aliasing store can rewrite it without naming src_vr,
   * so the redef scan below must also bail on any memory store (combo seed 74935). */
  IRLiveInterval *src_li = tcc_ir_vreg_live_interval(ir, src_vr);
  int src_addrtaken = (src_li && src_li->addrtaken);

  /* All uses must be same-block and after the copy. */
  int max_use_idx;
  if (!cprop_copy_uses_after_in_block(ctx, dvi, idx, copy_blk, &max_use_idx))
    return 0;

  if (!cprop_copy_redef_scan(ir, idx, max_use_idx, src_vr, src_addrtaken))
    return 0;

  int replaced = ssa_opt_replace_all_uses(ctx, dest_vr, src_vr);
  if (replaced == 0)
    return 0;

  ssa_opt_nop_instr(ctx, idx);
  return 1;
}

/* Like cprop_copy_param but for a VAR read encoded as a STACKOFF source
 * (is_lval=1, is_local=1), which cprop_copy_param skips: forward V into each use
 * of the single-def TEMP and nop the copy.  Hand-written: full-BB def-to-use
 * scan with stack-address deref safety checks, does not fit OPT_GEN_SSA (B3). */
static int ssa_gen_cprop_copy_var_stackoff(IRSSAOptCtx *ctx, int idx)
{
  TCCIRState *ir = ctx->ir;
  IRCFG *cfg = ctx->cfg;
  if (!cfg)
    return 0;

  IRQuadCompact *q = &ir->compact_instructions[idx];
  if (q->op != TCCIR_OP_ASSIGN)
    return 0;

  IROperand src = tcc_ir_op_get_src1(ir, q);
  IROperand dest = tcc_ir_op_get_dest(ir, q);

  if (src.tag != IROP_TAG_STACKOFF)
    return 0;
  if (!src.is_lval || !src.is_local || src.is_llocal || src.is_sym)
    return 0;

  int32_t src_vr = irop_get_vreg(src);
  if (src_vr < 0)
    return 0;
  if (TCCIR_DECODE_VREG_TYPE(src_vr) != TCCIR_VREG_TYPE_VAR)
    return 0;

  int32_t dest_vr = irop_get_vreg(dest);
  if (dest_vr < 0 || TCCIR_DECODE_VREG_TYPE(dest_vr) != TCCIR_VREG_TYPE_TEMP)
    return 0;

  /* Btype must match; INT32/STRUCT are interchangeable on pointer-typed locals. */
  {
    int sb = irop_get_btype(src);
    int db = irop_get_btype(dest);
    int both_word = (sb == IROP_BTYPE_INT32 || sb == IROP_BTYPE_STRUCT) &&
                    (db == IROP_BTYPE_INT32 || db == IROP_BTYPE_STRUCT);
    if (sb != db && !both_word)
      return 0;
    if (src.is_unsigned != dest.is_unsigned)
      return 0;
  }

  IRSSAVregInfo *dvi = ssa_opt_vinfo(ctx, dest_vr);
  if (!dvi || dvi->def_count != 1 || dvi->use_count == 0)
    return 0;

  int copy_blk = cfg->instr_to_block[idx];
  if (copy_blk < 0 || copy_blk >= cfg->num_blocks)
    return 0;

  int max_use_idx;
  if (!cprop_copy_uses_after_in_block(ctx, dvi, idx, copy_blk, &max_use_idx))
    return 0;

  /* Forwarding into a deref use `*T` is unsafe only when V holds a stack
   * address: the spill/reload path then loses the deref shape and SCCP folds
   * a stale value (test 20000605-2).  Runtime pointers stay safe. */
  for (int u = 0; u < dvi->use_count; u++) {
    IRQuadCompact *uq_check = &ir->compact_instructions[dvi->uses[u].idx];
    int nsrc_chk = irop_config[uq_check->op].has_src1 + irop_config[uq_check->op].has_src2;
    int has_deref_use = 0;
    for (int oi = 0; oi < nsrc_chk; oi++) {
      IROperand uop = oi == 0 ? tcc_ir_op_get_src1(ir, uq_check) : tcc_ir_op_get_src2(ir, uq_check);
      if (irop_get_vreg(uop) == dest_vr && uop.is_lval) {
        has_deref_use = 1;
        break;
      }
    }
    if (!has_deref_use &&
        (uq_check->op == TCCIR_OP_STORE || uq_check->op == TCCIR_OP_STORE_INDEXED)) {
      IROperand ud = tcc_ir_op_get_dest(ir, uq_check);
      if (irop_get_vreg(ud) == dest_vr && ud.is_lval)
        has_deref_use = 1;
    }
    if (!has_deref_use)
      continue;

    /* Bail if V's most recent def stores a stack address. */
    int32_t var_pos_check = TCCIR_DECODE_VREG_POSITION(src_vr);
    for (int k = idx - 1; k >= 0; k--) {
      IRQuadCompact *kq = &ir->compact_instructions[k];
      if (kq->op == TCCIR_OP_NOP || !irop_config[kq->op].has_dest)
        continue;
      IROperand kd = tcc_ir_op_get_dest(ir, kq);
      int32_t kdv = irop_get_vreg(kd);
      if (kdv < 0 || TCCIR_DECODE_VREG_TYPE(kdv) != TCCIR_VREG_TYPE_VAR)
        continue;
      if (TCCIR_DECODE_VREG_POSITION(kdv) != var_pos_check)
        continue;
      IROperand kstored = tcc_ir_op_get_src1(ir, kq);
      if (kstored.tag == IROP_TAG_STACKOFF && !kstored.is_lval && kstored.is_local)
        return 0; /* V <- Addr[StackLoc[N]] */
      int32_t kvr = irop_get_vreg(kstored);
      if (kvr >= 0 && TCCIR_DECODE_VREG_TYPE(kvr) == TCCIR_VREG_TYPE_TEMP &&
          ssa_opt_resolve_lea_stackloc(ctx, kvr) != INT_MIN)
        return 0; /* V <- T where T resolves to &StackLoc */
      break; /* only inspect the most recent def */
    }
  }

  /* When V is address-taken its stack slot is aliasable: a store through a
   * pointer holding &V rewrites V's value without naming src_vr as a def, so
   * the redef scan below (which only catches direct writes to src_vr) misses
   * it.  Forwarding V past such a store re-reads the clobbered slot at the use
   * site — e.g. `T = u; *p = k; use(T)` with `p == &u` must not become
   * `T = u; *p = k; use(u)` (combo seed 74935).  Flag it so the scan bails on
   * any intervening memory store we cannot disambiguate. */
  IRLiveInterval *src_li = tcc_ir_vreg_live_interval(ir, src_vr);
  int src_addrtaken = (src_li && src_li->addrtaken);

  if (!cprop_copy_redef_scan(ir, idx, max_use_idx, src_vr, src_addrtaken))
    return 0;

  /* Swap the vreg ID only, keeping each use site's tag/is_lval/is_local: a deref
   * use must stay a deref, and regalloc looks up V's home when materializing. */
  int replaced = ssa_opt_replace_all_uses(ctx, dest_vr, src_vr);
  if (replaced == 0)
    return 0;

  ssa_opt_nop_instr(ctx, idx);
  return 1;
}

/* Chain dispatcher: ssa_opt_run_gens breaks after the first table entry per op,
 * so one wrapper per op picks the right leaf by src tag. */
static int ssa_gen_cprop_assign_any(IRSSAOptCtx *ctx, int idx)
{
  TCCIRState *ir = ctx->ir;
  IRQuadCompact *q = &ir->compact_instructions[idx];
  IROperand src = tcc_ir_op_get_src1(ir, q);

  if (src.tag == IROP_TAG_VREG) {
    int32_t src_vr = irop_get_vreg(src);
    if (src_vr >= 0 && TCCIR_DECODE_VREG_TYPE(src_vr) == TCCIR_VREG_TYPE_TEMP)
      return opt_dsl_dispatch_cprop_assign(ctx, idx);
    return ssa_gen_cprop_copy_param(ctx, idx);
  }
  if (src.tag == IROP_TAG_STACKOFF)
    return ssa_gen_cprop_copy_var_stackoff(ctx, idx);
  if (src.tag == IROP_TAG_SYMREF)
    return opt_dsl_dispatch_cprop_symref_cse(ctx, idx);
  if (src.tag == IROP_TAG_IMM32 || src.tag == IROP_TAG_F32)
    return ssa_gen_cprop_imm(ctx, idx);
  return 0;
}

/* LOAD dispatcher: param-copy forwarding first, then BB-local redundant-load. */
static int ssa_gen_cprop_load_any(IRSSAOptCtx *ctx, int idx)
{
  TCCIRState *ir = ctx->ir;
  IRQuadCompact *q = &ir->compact_instructions[idx];
  IROperand src = tcc_ir_op_get_src1(ir, q);

  if (src.tag == IROP_TAG_VREG && !src.is_lval && !src.is_local && !src.is_llocal) {
    int32_t src_vr = irop_get_vreg(src);
    if (src_vr >= 0) {
      int t = TCCIR_DECODE_VREG_TYPE(src_vr);
      if (t == TCCIR_VREG_TYPE_PARAM || t == TCCIR_VREG_TYPE_VAR) {
        int r = ssa_gen_cprop_copy_param(ctx, idx);
        if (r)
          return r;
      }
    }
  }
  return opt_dsl_dispatch_cprop_load_redundant(ctx, idx);
}

static const IRSSAOptGen cprop_gens[] = {
  { TCCIR_OP_ASSIGN, ssa_gen_cprop_assign_any, "cprop_assign_any" },
  { TCCIR_OP_LOAD,   ssa_gen_cprop_load_any,   "cprop_load_any"   },
};

/* Symref deref-operand CSE: rewrite `*&sym` to `*Tn` when a prior same-block
 * ASSIGN `Tn = &sym` (no intervening write to Tn) already holds the address,
 * reusing the cached register instead of re-emitting the literal load.
 * Block-local; barrier on calls/asm/VLA. */
static int ssa_opt_symref_operand_cse_rewrite_one(TCCIRState *ir, IRSSAOptCtx *ctx,
                                                   IROperand *opnd_io, int instr_idx,
                                                   const IRBasicBlock *bb)
{
  if (opnd_io->tag != IROP_TAG_SYMREF)
    return 0;
  if (!opnd_io->is_lval || opnd_io->is_local || opnd_io->is_llocal)
    return 0;
  IRPoolSymref *cur_sr = irop_get_symref_ex(ir, *opnd_io);
  if (!cur_sr)
    return 0;

  for (int k = instr_idx - 1; k >= bb->start_idx; k--) {
    IRQuadCompact *pq = &ir->compact_instructions[k];
    if (pq->op == TCCIR_OP_NOP)
      continue;
    /* STOREs are safe: `*&sym` and `*Tn` alias the same memory identically. */
    if (ssa_op_is_call_barrier(pq->op))
      return 0;
    if (pq->op != TCCIR_OP_ASSIGN)
      continue;
    IROperand ps = tcc_ir_op_get_src1(ir, pq);
    if (ps.tag != IROP_TAG_SYMREF)
      continue;
    if (ps.is_lval || ps.is_local || ps.is_llocal)
      continue;
    IRPoolSymref *ps_sr = irop_get_symref_ex(ir, ps);
    if (!ps_sr || ps_sr->sym != cur_sr->sym || ps_sr->addend != cur_sr->addend)
      continue;
    IROperand pd = tcc_ir_op_get_dest(ir, pq);
    int32_t pd_vr = irop_get_vreg(pd);
    if (pd_vr < 0 || TCCIR_DECODE_VREG_TYPE(pd_vr) != TCCIR_VREG_TYPE_TEMP)
      continue;

    /* Rewrite the operand: change tag to VREG, point at pd_vr, keep
     * is_lval=1 (so codegen still emits the deref). Other flags clear. */
    uint8_t saved_btype = opnd_io->btype;
    *opnd_io = (IROperand){0};
    opnd_io->tag = IROP_TAG_VREG;
    opnd_io->is_lval = 1;
    opnd_io->btype = saved_btype;
    irop_set_vreg(opnd_io, pd_vr);

    /* Record the new use of pd_vr by this instruction. */
    IRSSAVregInfo *pvi = ssa_opt_vinfo(ctx, pd_vr);
    if (pvi)
      ssa_opt_add_use_instr(pvi, instr_idx);
    return 1;
  }
  return 0;
}

int ssa_opt_symref_operand_cse(IRSSAOptCtx *ctx)
{
  TCCIRState *ir = ctx->ir;
  IRCFG *cfg = ctx->cfg;
  if (!cfg)
    return 0;

  int changes = 0;
  for (int i = 0; i < ir->next_instruction_index; i++) {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;
    if (i >= cfg->num_instrs)   /* instr appended after CFG build: no block map */
      continue;
    int blk = cfg->instr_to_block[i];
    if (blk < 0 || blk >= cfg->num_blocks)
      continue;
    const IRBasicBlock *bb = &cfg->blocks[blk];

    if (irop_config[q->op].has_src1) {
      IROperand s = tcc_ir_op_get_src1(ir, q);
      if (ssa_opt_symref_operand_cse_rewrite_one(ir, ctx, &s, i, bb)) {
        tcc_ir_op_set_src1(ir, q, s);
        changes++;
      }
    }
    if (irop_config[q->op].has_src2) {
      IROperand s = tcc_ir_op_get_src2(ir, q);
      if (ssa_opt_symref_operand_cse_rewrite_one(ir, ctx, &s, i, bb)) {
        tcc_ir_op_set_src2(ir, q, s);
        changes++;
      }
    }
  }
  return changes;
}

int ssa_opt_cprop(IRSSAOptCtx *ctx)
{
  int changes = ssa_opt_run_gens(ctx, cprop_gens,
                                  sizeof(cprop_gens) / sizeof(cprop_gens[0]));
  changes += ssa_opt_symref_operand_cse(ctx);
  return changes;
}

/* VAR value forwarding (multi-use, whole-function): for a single-def,
 * non-address-taken VAR `V <- val`, forward val into every value use dominated
 * by the def and NOP the store (DCE cleans up).  Value uses only — never a
 * deref-via-V site or a stack-address `val` (SCCP stack-load alias, test
 * 20000605-2). */

/* Is block `def_blk` an ancestor of `use_blk` in the dominator tree? */
static int v2v_dominates(IRCFG *cfg, int def_blk, int use_blk)
{
  if (def_blk == use_blk)
    return 1;
  IRBasicBlock *ub = &cfg->blocks[use_blk];
  int d = ub->idom;
  while (d >= 0) {
    if (d == def_blk)
      return 1;
    if (d == cfg->blocks[d].idom)
      break;
    d = cfg->blocks[d].idom;
  }
  return 0;
}

/* Is `stored_val` safe to fan out across multiple uses? */
static int v2v_is_safe_value(IROperand op)
{
  if (op.is_lval || op.is_llocal)
    return 0;
  /* Stack-address sources expose SCCP stack-load aliasing when fanned out. */
  if (op.tag == IROP_TAG_STACKOFF && !op.is_lval && op.is_local)
    return 0;
  return 1;
}

/* Per-VAR fact tables for var_to_param_forward, keyed by VAR position. */
typedef struct {
  int num_vars;
  int *def_count;
  int *def_instr;      /* last def site, -1 = none */
  uint8_t *addrtaken;  /* &V taken (LEA or is_local && !is_lval src) */
  uint8_t *bad_use;    /* deref-via-V's-value use — cannot forward */
} VarFacts;

#define VARF_BIT(bits, pos) ((bits)[(pos) / 8] & (1 << ((pos) % 8)))
#define VARF_SET(bits, pos) ((bits)[(pos) / 8] |= (1 << ((pos) % 8)))

/* Pass 1: for every VAR, record def sites, address-taken status, and whether it
 * has a use we can't forward (a deref through V's pointer value).  Shared by
 * var_to_param_forward and var_forward; the latter ignores the bad_use bits. */
static void var_collect_facts(TCCIRState *ir, VarFacts *f)
{
  int nv = f->num_vars;
  for (int i = 0; i < ir->next_instruction_index; i++) {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;

    /* Count VAR defs (any op with VAR dest, except FUNCPARAM). */
    if (irop_config[q->op].has_dest &&
        q->op != TCCIR_OP_FUNCPARAMVAL && q->op != TCCIR_OP_FUNCPARAMVOID) {
      IROperand d = tcc_ir_op_get_dest(ir, q);
      int32_t dv = irop_get_vreg(d);
      if (dv >= 0 && TCCIR_DECODE_VREG_TYPE(dv) == TCCIR_VREG_TYPE_VAR) {
        int pos = TCCIR_DECODE_VREG_POSITION(dv);
        if (pos < nv) {
          f->def_count[pos]++;
          f->def_instr[pos] = i;
          /* A `*V <-- val` deref STORE (is_lval && !is_local) uses V's value,
           * not its slot — mark it bad_use, not a canonical def. */
          if ((q->op == TCCIR_OP_STORE || q->op == TCCIR_OP_STORE_INDEXED) &&
              d.is_lval && !d.is_local)
            VARF_SET(f->bad_use, pos);
        }
      }
    }

    /* Scan src operands for VAR uses. */
    int nsrc = irop_config[q->op].has_src1 + irop_config[q->op].has_src2;
    for (int oi = 0; oi < nsrc; oi++) {
      IROperand s = oi == 0 ? tcc_ir_op_get_src1(ir, q) : tcc_ir_op_get_src2(ir, q);
      int32_t vr = irop_get_vreg(s);
      if (vr < 0 || TCCIR_DECODE_VREG_TYPE(vr) != TCCIR_VREG_TYPE_VAR)
        continue;
      int pos = TCCIR_DECODE_VREG_POSITION(vr);
      if (pos >= nv)
        continue;
      /* &V (address-of): is_local && !is_lval. */
      if (s.is_local && !s.is_lval)
        VARF_SET(f->addrtaken, pos);
      /* Only a VREG/is_lval/!is_local deref use is unsafe to forward (would
       * duplicate the deref); slot-reads and value reads are fine. */
      else if (s.is_lval && s.tag == IROP_TAG_VREG && !s.is_local)
        VARF_SET(f->bad_use, pos);
    }
    if (q->op == TCCIR_OP_MLA) {
      IROperand a = tcc_ir_op_get_accum(ir, q);
      int32_t vr = irop_get_vreg(a);
      if (vr >= 0 && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_VAR) {
        int pos = TCCIR_DECODE_VREG_POSITION(vr);
        if (pos < nv && a.is_lval && a.tag == IROP_TAG_VREG && !a.is_local)
          VARF_SET(f->bad_use, pos);
      }
    }
    if (q->op == TCCIR_OP_LEA) {
      IROperand s = tcc_ir_op_get_src1(ir, q);
      int32_t vr = irop_get_vreg(s);
      if (vr >= 0 && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_VAR) {
        int pos = TCCIR_DECODE_VREG_POSITION(vr);
        if (pos < nv)
          VARF_SET(f->addrtaken, pos);
      }
    }
  }
  /* Volatile VARs must never be forwarded to their uses: every load is a
   * mandated access.  Treat them like address-taken for both consumers. */
  for (int v = 0; v < nv && v < ir->variables_live_intervals_size; v++)
    if (ir->variables_live_intervals[v].is_volatile)
      VARF_SET(f->addrtaken, v);
}

/* Forward single-def VAR `pos` (def at `def_idx`) into all its dominated value
 * uses, then NOP the def.  Returns the number of use sites rewritten. */
static int v2pf_forward_one(IRSSAOptCtx *ctx, int pos, int def_idx, int n)
{
  TCCIRState *ir = ctx->ir;
  IRCFG *cfg = ctx->cfg;

  IRQuadCompact *def_q = &ir->compact_instructions[def_idx];
  if (def_q->op != TCCIR_OP_STORE && def_q->op != TCCIR_OP_ASSIGN)
    return 0;

  IROperand stored_val = tcc_ir_op_get_src1(ir, def_q);
  if (!v2v_is_safe_value(stored_val))
    return 0;
  int32_t stored_vr = irop_get_vreg(stored_val);
  /* A VAR-typed value would create cross-VAR dependence chains. */
  if (stored_vr >= 0 && TCCIR_DECODE_VREG_TYPE(stored_vr) == TCCIR_VREG_TYPE_VAR)
    return 0;
  /* A TEMP holding a stack address fanned out lets SCCP fold a stale
   * stack-init value through the new chain (test 20000605-2). */
  if (stored_vr >= 0 &&
      TCCIR_DECODE_VREG_TYPE(stored_vr) == TCCIR_VREG_TYPE_TEMP &&
      ssa_opt_resolve_lea_stackloc(ctx, stored_vr) != INT_MIN)
    return 0;

  int def_blk = cfg->instr_to_block[def_idx];
  int32_t target_vr = TCCIR_ENCODE_VREG(TCCIR_VREG_TYPE_VAR, pos);

  /* Collect use sites; each must be dominated by the def and free of a
   * barrel-shift fusion (an immediate can't carry the pinned shift, seed 16558). */
  int max_use_idx = def_idx;
  int found_any = 0;
  for (int j = def_idx + 1; j < n; j++) {
    IRQuadCompact *uq = &ir->compact_instructions[j];
    if (uq->op == TCCIR_OP_NOP || !ssa_op_reads_vreg(ir, uq, target_vr))
      continue;
    if (tcc_ir_barrel_shift_at(ir, uq) ||
        !v2v_dominates(cfg, def_blk, cfg->instr_to_block[j]))
      return 0;
    found_any = 1;
    max_use_idx = j;
  }
  if (!found_any)
    return 0;

  /* A barrier between def and last use invalidates a TEMP stored_val
   * (symref/immediate values rematerialize and stay call-safe). */
  if (stored_val.tag == IROP_TAG_VREG) {
    for (int k = def_idx + 1; k <= max_use_idx; k++) {
      int op = ir->compact_instructions[k].op;
      if (ssa_op_is_call_barrier(op) || ssa_op_is_asm_operand(op))
        return 0;
    }
  }

  /* Forward V into all uses; a LOAD use must become ASSIGN so the non-slot
   * operand isn't reinterpreted as a deref. */
  int local_changes = 0;
  for (int j = def_idx + 1; j <= max_use_idx; j++) {
    IRQuadCompact *uq = &ir->compact_instructions[j];
    if (uq->op == TCCIR_OP_NOP)
      continue;

    int touched = 0;
    if (irop_config[uq->op].has_src1) {
      IROperand s = tcc_ir_op_get_src1(ir, uq);
      if (irop_get_vreg(s) == target_vr) {
        IROperand ns = stored_val;
        /* STORE_INDEXED takes its width from the value operand's btype;
         * preserve the original's so a narrower stored_val doesn't shrink the
         * store (seed struct_byval 182993). */
        if (uq->op == TCCIR_OP_STORE_INDEXED)
          ns.btype = s.btype;
        tcc_ir_set_src1(ir, j, ns);
        touched = 1;
      }
    }
    if (irop_config[uq->op].has_src2) {
      IROperand s = tcc_ir_op_get_src2(ir, uq);
      if (irop_get_vreg(s) == target_vr) {
        tcc_ir_set_src2(ir, j, stored_val);
        touched = 1;
      }
    }
    if (uq->op == TCCIR_OP_MLA) {
      IROperand a = tcc_ir_op_get_accum(ir, uq);
      if (irop_get_vreg(a) == target_vr) {
        tcc_ir_op_set_accum(ir, uq, stored_val);
        touched = 1;
      }
    }
    if (touched) {
      if (uq->op == TCCIR_OP_LOAD) {
        uq->op = TCCIR_OP_ASSIGN;
        tcc_ir_set_src2(ir, j, IROP_NONE);
      }
      if (stored_vr >= 0) {
        IRSSAVregInfo *svi = ssa_opt_vinfo(ctx, stored_vr);
        if (svi)
          ssa_opt_add_use_instr(svi, j);
      }
      local_changes++;
    }
  }
  if (local_changes > 0)
    ssa_opt_nop_instr(ctx, def_idx);
  return local_changes;
}

int ssa_opt_var_to_param_forward(IRSSAOptCtx *ctx)
{
  TCCIRState *ir = ctx->ir;
  IRCFG *cfg = ctx->cfg;
  if (!cfg || cfg->num_blocks == 0)
    return 0;

  int num_vars = ir->next_local_variable;
  if (num_vars <= 0)
    return 0;

  /* Static-chain reads of parent VARs don't show as IR uses; bail if this
   * function sets up a chain. */
  for (int i = 0; i < ir->next_instruction_index; i++) {
    int op = ir->compact_instructions[i].op;
    if (op == TCCIR_OP_SET_CHAIN || op == TCCIR_OP_INIT_CHAIN_SLOT)
      return 0;
  }

  int n = ir->next_instruction_index;
  VarFacts f = {
    .num_vars  = num_vars,
    .def_count = tcc_mallocz(num_vars * sizeof(int)),
    .def_instr = tcc_malloc(num_vars * sizeof(int)),
    .addrtaken = tcc_mallocz((num_vars + 7) / 8),
    .bad_use   = tcc_mallocz((num_vars + 7) / 8),
  };
  for (int i = 0; i < num_vars; i++)
    f.def_instr[i] = -1;

  var_collect_facts(ir, &f);

  int changes = 0;
  for (int pos = 0; pos < num_vars; pos++) {
    if (f.def_count[pos] != 1 || f.def_instr[pos] < 0)
      continue;
    if (VARF_BIT(f.addrtaken, pos) || VARF_BIT(f.bad_use, pos))
      continue;
    changes += v2pf_forward_one(ctx, pos, f.def_instr[pos], n);
  }

  tcc_free(f.def_count);
  tcc_free(f.def_instr);
  tcc_free(f.addrtaken);
  tcc_free(f.bad_use);
  return changes;
}

#undef VARF_BIT
#undef VARF_SET

/* VAR forwarding: `Ty <- Vn` where Vn is a single-def, non-address-taken VAR
 * dominated by its def -> rewrite to `Ty <- stored_val [ASSIGN]`, making inline-
 * expansion VAR values visible to load_cse and SCCP. */

int ssa_opt_var_forward(IRSSAOptCtx *ctx)
{
  TCCIRState *ir = ctx->ir;
  IRCFG *cfg = ctx->cfg;
  if (!cfg || cfg->num_blocks == 0)
    return 0;

  int num_vars = ir->next_local_variable;
  if (num_vars <= 0)
    return 0;

  VarFacts f = {
    .num_vars  = num_vars,
    .def_count = tcc_mallocz(num_vars * sizeof(int)),
    .def_instr = tcc_malloc(num_vars * sizeof(int)),
    .addrtaken = tcc_mallocz((num_vars + 7) / 8),
    .bad_use   = tcc_mallocz((num_vars + 7) / 8),
  };
  for (int i = 0; i < num_vars; i++)
    f.def_instr[i] = -1;
  var_collect_facts(ir, &f);

  int *var_def_count = f.def_count;
  int *var_def_instr = f.def_instr;
  uint8_t *var_addrtaken = f.addrtaken;

  int changes = 0;

  /* For each single-def, non-address-taken VAR: replace uses with stored value */
  for (int i = 0; i < ir->next_instruction_index; i++) {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_ASSIGN && q->op != TCCIR_OP_LOAD)
      continue;
    if (i >= cfg->num_instrs)   /* appended after CFG build: no block map */
      continue;

    IROperand src = tcc_ir_op_get_src1(ir, q);
    int32_t src_vr = irop_get_vreg(src);
    if (src_vr < 0 || TCCIR_DECODE_VREG_TYPE(src_vr) != TCCIR_VREG_TYPE_VAR)
      continue;

    int var_pos = TCCIR_DECODE_VREG_POSITION(src_vr);
    if (var_pos >= num_vars) { continue; }
    if (var_def_count[var_pos] != 1)
      continue;
    if (var_addrtaken[var_pos / 8] & (1 << (var_pos % 8)))
      continue;

    int def_idx = var_def_instr[var_pos];
    if (def_idx < 0 || def_idx >= i)
      continue;

    /* Def must dominate use. Same-block is always safe; cross-block
     * requires the def's block to dominate the use's block. */
    {
      int def_blk = cfg->instr_to_block[def_idx];
      int use_blk = cfg->instr_to_block[i];
      if (def_blk != use_blk) {
        int dominated = 0;
        IRBasicBlock *ub = &cfg->blocks[use_blk];
        int d = ub->idom;
        while (d >= 0) {
          if (d == def_blk) { dominated = 1; break; }
          if (d == cfg->blocks[d].idom) break;
          d = cfg->blocks[d].idom;
        }
        if (!dominated)
          continue;
      }
    }

    /* A call between def and use may modify the VAR via a closure chain. */
    {
      int has_call = 0;
      for (int k = def_idx + 1; k < i && !has_call; k++) {
        int op = ir->compact_instructions[k].op;
        if (op == TCCIR_OP_FUNCCALLVOID || op == TCCIR_OP_FUNCCALLVAL)
          has_call = 1;
      }
      if (has_call)
        continue;
    }

    IRQuadCompact *def_q = &ir->compact_instructions[def_idx];
    if (def_q->op != TCCIR_OP_STORE && def_q->op != TCCIR_OP_ASSIGN)
      continue;

    IROperand stored_val = tcc_ir_op_get_src1(ir, def_q);
    if (stored_val.is_lval)
      continue;
    int32_t stored_vr = irop_get_vreg(stored_val);
    if (stored_vr >= 0 && TCCIR_DECODE_VREG_TYPE(stored_vr) == TCCIR_VREG_TYPE_VAR)
      continue;

    q->op = TCCIR_OP_ASSIGN;
    tcc_ir_set_src1(ir, i, stored_val);
    tcc_ir_set_src2(ir, i, IROP_NONE);

    if (stored_vr >= 0) {
      IRSSAVregInfo *svi = ssa_opt_vinfo(ctx, stored_vr);
      if (svi)
        ssa_opt_add_use_instr(svi, i);
    }
    changes++;
  }

  tcc_free(f.def_count);
  tcc_free(f.def_instr);
  tcc_free(f.addrtaken);
  tcc_free(f.bad_use);
  return changes;
}

/* VAR self-update const fold: `Vx = Vx OP #imm` against a dominating prior
 * `Vx = #const` in the same block -> `Vx = #folded`, NOPing the dead prior def.
 * Catches single-block multi-def VARs the standard both-immediate fold misses. */

static int ssa_var_const_fold_one(IRSSAOptCtx *ctx, int idx)
{
  TCCIRState *ir = ctx->ir;
  IRCFG *cfg = ctx->cfg;
  if (!cfg)
    return 0;

  IRQuadCompact *q = &ir->compact_instructions[idx];
  int op = q->op;

  switch (op) {
  case TCCIR_OP_ADD: case TCCIR_OP_SUB: case TCCIR_OP_MUL:
  case TCCIR_OP_AND: case TCCIR_OP_OR:  case TCCIR_OP_XOR:
  case TCCIR_OP_SHL: case TCCIR_OP_SHR: case TCCIR_OP_SAR:
    break;
  default:
    return 0;
  }

  IROperand src1 = tcc_ir_op_get_src1(ir, q);
  IROperand src2 = tcc_ir_op_get_src2(ir, q);
  IROperand dest = tcc_ir_op_get_dest(ir, q);

  int32_t dest_vr = irop_get_vreg(dest);
  int32_t src1_vr = irop_get_vreg(src1);
  if (dest_vr < 0 || src1_vr != dest_vr)
    return 0;
  if (TCCIR_DECODE_VREG_TYPE(dest_vr) != TCCIR_VREG_TYPE_VAR)
    return 0;
  /* An address-taken or volatile slot may be aliased between prior def and
   * self-update; folding would drop mandated accesses. */
  {
    int vpos = TCCIR_DECODE_VREG_POSITION(dest_vr);
    if (vpos < ir->variables_live_intervals_size &&
        (ir->variables_live_intervals[vpos].addrtaken ||
         ir->variables_live_intervals[vpos].is_volatile))
      return 0;
  }
  if (src2.tag != IROP_TAG_IMM32 || src2.is_lval)
    return 0;
  /* src1 must read the same VAR (bare VREG or lval STACKOFF encoding). */
  if (!(src1.tag == IROP_TAG_VREG && !src1.is_lval) &&
      !(src1.tag == IROP_TAG_STACKOFF && src1.is_lval))
    return 0;

  int blk = cfg->instr_to_block[idx];
  if (blk < 0 || blk >= cfg->num_blocks)
    return 0;
  IRBasicBlock *bb = &cfg->blocks[blk];

  int prior_idx = -1;
  int32_t prior_val = 0;
  for (int k = idx - 1; k >= bb->start_idx; k--) {
    IRQuadCompact *pq = &ir->compact_instructions[k];
    if (pq->op == TCCIR_OP_NOP)
      continue;

    /* A slot STORE of our own Vx is a prior def (handled below), not an alias. */
    if (pq->op == TCCIR_OP_STORE &&
        irop_get_vreg(tcc_ir_op_get_dest(ir, pq)) == dest_vr) {
    } else {
      /* Anything that could alias Vx through memory or a call kills the fold. */
      if (ssa_op_is_call_barrier(pq->op) ||
          pq->op == TCCIR_OP_STORE || pq->op == TCCIR_OP_STORE_INDEXED ||
          pq->op == TCCIR_OP_STORE_POSTINC || pq->op == TCCIR_OP_BLOCK_COPY)
        return 0;
    }

    if (irop_config[pq->op].has_dest &&
        pq->op != TCCIR_OP_FUNCPARAMVAL && pq->op != TCCIR_OP_FUNCPARAMVOID) {
      IROperand pd = tcc_ir_op_get_dest(ir, pq);
      /* Any VAR-encoded dest counts as a def (bare-vreg or lval-stackoff). */
      if (irop_get_vreg(pd) == dest_vr) {
        /* Capture a prior constant def (ASSIGN or slot STORE); then stop. */
        if (pq->op == TCCIR_OP_ASSIGN || pq->op == TCCIR_OP_STORE) {
          IROperand ps = tcc_ir_op_get_src1(ir, pq);
          if (ps.tag == IROP_TAG_IMM32 && !ps.is_lval) {
            prior_idx = k;
            prior_val = ps.u.imm32;
          }
        }
        break;
      }
    }
  }

  if (prior_idx < 0)
    return 0;

  int32_t v1 = prior_val;
  int32_t v2 = src2.u.imm32;
  int64_t result;
  switch (op) {
  case TCCIR_OP_ADD: result = (int64_t)((uint64_t)(uint32_t)v1 + (uint64_t)(uint32_t)v2); break;
  case TCCIR_OP_SUB: result = (int64_t)((uint64_t)(uint32_t)v1 - (uint64_t)(uint32_t)v2); break;
  case TCCIR_OP_MUL: result = (int64_t)((uint64_t)(uint32_t)v1 * (uint64_t)(uint32_t)v2); break;
  case TCCIR_OP_AND: result = v1 & v2; break;
  case TCCIR_OP_OR:  result = v1 | v2; break;
  case TCCIR_OP_XOR: result = v1 ^ v2; break;
  case TCCIR_OP_SHL:
    if ((uint32_t)v2 >= 32) result = 0;
    else result = (int64_t)((uint32_t)v1 << (uint32_t)v2);
    break;
  case TCCIR_OP_SHR:
    if ((uint32_t)v2 >= 32) result = 0;
    else result = (uint32_t)v1 >> (uint32_t)v2;
    break;
  case TCCIR_OP_SAR:
    if ((uint32_t)v2 >= 32) result = v1 >> 31;
    else result = v1 >> v2;
    break;
  default:
    return 0;
  }

  /* Only NOP the prior def if nothing in (prior_idx, idx) reads Vx — an
   * intervening read would otherwise see an undefined value. */
  int prior_used = 0;
  for (int k = prior_idx + 1; k < idx; k++) {
    IRQuadCompact *uq = &ir->compact_instructions[k];
    if (uq->op == TCCIR_OP_NOP)
      continue;
    IROperand us1 = tcc_ir_op_get_src1(ir, uq);
    IROperand us2 = tcc_ir_op_get_src2(ir, uq);
    if (irop_get_vreg(us1) == dest_vr || irop_get_vreg(us2) == dest_vr) {
      prior_used = 1;
      break;
    }
  }

  IROperand imm = irop_make_imm32(0, (int32_t)result, dest.btype);
  q->op = TCCIR_OP_ASSIGN;
  tcc_ir_op_set_src1(ir, q, imm);
  tcc_ir_op_set_src2(ir, q, IROP_NONE);

  if (!prior_used)
    ir->compact_instructions[prior_idx].op = TCCIR_OP_NOP;
  return 1;
}

int ssa_opt_var_const_fold(IRSSAOptCtx *ctx)
{
  TCCIRState *ir = ctx->ir;

  /* Computed goto / switch-table jumps can re-enter a block mid-body, so a
   * backward "prior store" scan is unsound (regression 920501-3). */
  for (int i = 0; i < ir->next_instruction_index; i++) {
    int op = ir->compact_instructions[i].op;
    if (op == TCCIR_OP_IJUMP || op == TCCIR_OP_SWITCH_TABLE)
      return 0;
  }

  int changes = 0;
  for (int i = 0; i < ir->next_instruction_index; i++) {
    if (ir->compact_instructions[i].op == TCCIR_OP_NOP)
      continue;
    changes += ssa_var_const_fold_one(ctx, i);
  }
  return changes;
}

/* ssa:const_prop_tmp — SSA-time analog of the flat block-local const_prop_tmp,
 * reusing the flat core (tcc_ir_opt_const_prop_tmp_core) as the single source of
 * truth.  It never moves defs across blocks, but it DOES fold decided branches
 * (cpt_try_softfp_cmp_fold rewrites a constant __aeabi_c[df]cmp + JUMPIF into
 * JUMP/NOP), so like every branch-folding SSA pass it must prune phi operands
 * whose pred block became unreachable — otherwise ra_resolve_phis later emits
 * the dead arm's copy onto the surviving straight-line path, clobbering the
 * live value (fuzz seed fp_round:10, test 383). */
int ssa_opt_const_prop_tmp(IRSSAOptCtx *ctx)
{
  int changes = tcc_ir_opt_const_prop_tmp_core(ctx->ir);
  if (changes) {
    tcc_ir_ssa_opt_rebuild(ctx);
    changes += ssa_opt_prune_unreachable_phis(ctx);
  }
  return changes;
}
