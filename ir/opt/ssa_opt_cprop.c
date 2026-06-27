/*
 *  TCC IR - SSA Copy Propagation
 *
 *  Copyright (c) 2025 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#define USING_GLOBALS
#include "ir.h"
#include "ssa_opt.h"
#include <limits.h>

/* ============================================================================
 * Generator: ssa_gen_cprop_assign
 *
 * Pattern: ASSIGN dest = src  (both TEMP vregs, no lval/deref)
 * Action:  replace all uses of dest with src, making the ASSIGN dead
 * ============================================================================ */

static int ssa_gen_cprop_assign(IRSSAOptCtx *ctx, int idx)
{
  TCCIRState *ir = ctx->ir;
  IRQuadCompact *q = &ir->compact_instructions[idx];

  IROperand dest = tcc_ir_op_get_dest(ir, q);
  IROperand src = tcc_ir_op_get_src1(ir, q);

  if (src.is_lval || src.is_llocal || src.is_local)
    return 0;

  int32_t dest_vr = irop_get_vreg(dest);
  int32_t src_vr = irop_get_vreg(src);
  if (dest_vr < 0 || src_vr < 0)
    return 0;
  if (TCCIR_DECODE_VREG_TYPE(dest_vr) != TCCIR_VREG_TYPE_TEMP)
    return 0;
  if (src.tag != IROP_TAG_VREG)
    return 0;

  /* Only propagate TEMP sources. PARAM and VAR vregs are not SSA-renamed,
   * so they may have multiple definitions; replacing uses of dest with a
   * non-versioned source is unsafe when the source is redefined between
   * the copy and a use (e.g. pointer increment in a loop body). */
  if (TCCIR_DECODE_VREG_TYPE(src_vr) != TCCIR_VREG_TYPE_TEMP)
    return 0;

  /* Width gate: an ASSIGN whose dest and src differ in btype is NOT a pure
   * copy — it is a width conversion.  A 32-bit src into a 64-bit dest zero/
   * sign-fills the high word; forwarding src into the dest's 64-bit uses drops
   * that extension, so a later 64-bit consumer (e.g. an OR chain reconstructing
   * a packed >32-bit bitfield from a folded SAR/SHL/OR sign-extend idiom) reads
   * a garbage high half.  Same gate as ssa_gen_cprop_copy_param below. */
  if (irop_get_btype(dest) != irop_get_btype(src))
    return 0;

  IRSSAVregInfo *vi = ssa_opt_vinfo(ctx, dest_vr);
  if (vi && vi->def_count > 1)
    return 0;

  /* Do not propagate a copy whose dest feeds a phi operand.  Such a copy
   * `T_dest <- T_src` often resolves a phi (e.g. a loop back-edge value):
   * folding T_dest away and naming T_src directly in the phi reintroduces the
   * lost-copy problem at out-of-SSA phi resolution, since T_src stays live past
   * the phi edge and its slot can be overwritten before the parallel copy runs
   * (fuzz seed 2698: the loop-carried `cs` back-edge copy was dropped, yielding
   * a wrong checksum).  Leaving the copy in place keeps phi resolution correct;
   * DCE still removes genuinely dead copies. */
  if (vi) {
    for (int u = 0; u < vi->use_count; u++)
      if (vi->uses[u].kind == SSA_USE_PHI)
        return 0;
  }

  int replaced = ssa_opt_replace_all_uses(ctx, dest_vr, src_vr);
  return replaced > 0 ? 1 : 0;
}

/* ============================================================================
 * Generator: ssa_gen_cprop_imm
 *
 * Pattern: ASSIGN dest = #imm32  (TEMP dest, immediate src)
 * Action:  replace all uses of dest with the immediate directly
 * ============================================================================ */

/* Currently unreachable: enabling immediate forwarding through ASSIGN
 * triggers latent SCCP/phi-simplify bugs on switch/goto/OR patterns where
 * branch-arm constants flow into a join point (see bug_switch_goto_or
 * test). Keep the implementation in case those bugs get fixed later. */
__attribute__((unused))
static int ssa_gen_cprop_imm(IRSSAOptCtx *ctx, int idx)
{
  TCCIRState *ir = ctx->ir;
  IRQuadCompact *q = &ir->compact_instructions[idx];

  IROperand dest = tcc_ir_op_get_dest(ir, q);
  IROperand src = tcc_ir_op_get_src1(ir, q);

  int32_t dest_vr = irop_get_vreg(dest);
  if (dest_vr < 0 || TCCIR_DECODE_VREG_TYPE(dest_vr) != TCCIR_VREG_TYPE_TEMP)
    return 0;

  if (src.tag != IROP_TAG_IMM32 && src.tag != IROP_TAG_F32)
    return 0;
  if (src.is_lval)
    return 0;

  IRSSAVregInfo *vi = ssa_opt_vinfo(ctx, dest_vr);
  if (!vi || vi->use_count == 0)
    return 0;
  if (vi->def_count > 1)
    return 0;

  int count = 0;
  while (vi->use_count > 0) {
    IRSSAUse use = vi->uses[vi->use_count - 1];
    if (use.kind != SSA_USE_INSTR) {
      /* phi uses keep the vreg */
      break;
    }

    IRQuadCompact *uq = &ir->compact_instructions[use.idx];

    int rewrote = 0;
    if (irop_config[uq->op].has_src1) {
      IROperand s = tcc_ir_op_get_src1(ir, uq);
      if (irop_get_vreg(s) == dest_vr && !s.is_lval) {
        tcc_ir_op_set_src1(ir, uq, src);
        rewrote = 1;
      }
    }
    if (!rewrote && irop_config[uq->op].has_src2) {
      IROperand s = tcc_ir_op_get_src2(ir, uq);
      if (irop_get_vreg(s) == dest_vr && !s.is_lval) {
        tcc_ir_op_set_src2(ir, uq, src);
        rewrote = 1;
      }
    }

    if (rewrote) {
      vi->use_count--;
      count++;
    } else {
      break;
    }
  }

  return count > 0 ? 1 : 0;
}

/* ============================================================================
 * Generator: ssa_gen_cprop_load_redundant
 *
 * Pattern: T2 <-- V [LOAD]  where V is a vreg-source (no deref / not lval)
 *          and an earlier LOAD with the same V source exists in the same
 *          basic block with no intervening write to V.
 * Action:  rewrite to T2 <-- T1 [ASSIGN] where T1 is the earlier LOAD's dest.
 *          cprop_assign then forwards T1 into T2's uses.
 *
 * Without this, inlined sequences that read the same VAR vreg twice produce
 * back-to-back register copies (e.g. swap_adjacent inlined into main:
 *   mov ip, r2 ; str.w ip, [r0, #16] ; mov ip, r2 ; add r2, r3, ip
 * where the second `mov ip, r2` is redundant).
 * ============================================================================ */

static int ssa_gen_cprop_load_redundant(IRSSAOptCtx *ctx, int idx)
{
  TCCIRState *ir = ctx->ir;
  IRCFG *cfg = ctx->cfg;
  if (!cfg)
    return 0;

  IRQuadCompact *q = &ir->compact_instructions[idx];
  IROperand src = tcc_ir_op_get_src1(ir, q);
  /* Two LOADs from the same vreg-encoded source produce the same value as
   * long as no intervening instruction modifies that vreg's storage. The
   * source may be:
   *  - is_lval=0 plain VREG: register-held value, MOV at codegen.
   *  - is_lval=1 / is_local=1 STACKOFF: spilled VAR access, MOV when the
   *    allocator pinned it to a register, LDR when truly spilled — either
   *    way the two reads are identical if no store happened between them.
   * Skip llocal (double indirection) and tag mismatches. */
  if (src.is_llocal)
    return 0;
  if (src.tag != IROP_TAG_VREG && src.tag != IROP_TAG_STACKOFF)
    return 0;
  int32_t src_vr = irop_get_vreg(src);
  if (src_vr < 0)
    return 0;

  IROperand dest = tcc_ir_op_get_dest(ir, q);
  int32_t dest_vr = irop_get_vreg(dest);
  if (dest_vr < 0 || TCCIR_DECODE_VREG_TYPE(dest_vr) != TCCIR_VREG_TYPE_TEMP)
    return 0;

  int blk = cfg->instr_to_block[idx];
  if (blk < 0 || blk >= cfg->num_blocks)
    return 0;
  IRBasicBlock *bb = &cfg->blocks[blk];

  /* Scan backward in the same block for a prior LOAD of the same src vreg.
   * Bail on any intervening def of src_vr, store, call, or VLA op. */
  int prior_dest_vr = -1;
  for (int k = idx - 1; k >= bb->start_idx; k--) {
    IRQuadCompact *pq = &ir->compact_instructions[k];
    if (pq->op == TCCIR_OP_NOP)
      continue;

    /* Anything that may write memory or invalidate the source register's
     * meaning across the copy. We're copying a register value, but a
     * function call could clobber the underlying VAR's storage when the
     * VAR is on the stack and address-taken; be conservative.
     *
     * For deref-style LOADs (src.is_lval=1) any STORE may write the same
     * memory the second LOAD reads (we have no alias analysis), so bail. */
    switch (pq->op) {
    case TCCIR_OP_FUNCCALLVAL:
    case TCCIR_OP_FUNCCALLVOID:
    case TCCIR_OP_INLINE_ASM:
    case TCCIR_OP_VLA_ALLOC:
    case TCCIR_OP_SETJMP:
    case TCCIR_OP_LONGJMP:
    case TCCIR_OP_NL_SETJMP:
    case TCCIR_OP_NL_LONGJMP:
      return 0;
    case TCCIR_OP_STORE:
    case TCCIR_OP_STORE_INDEXED:
    case TCCIR_OP_STORE_POSTINC:
      if (src.is_lval)
        return 0;
      break;
    default:
      break;
    }

    /* Stop if anything writes to src_vr — directly (ALU/ASSIGN dest) or
     * through its memory storage (STORE with dest's vreg == src_vr, which
     * occurs for stack-spilled PARAM/VAR like `data <<= 1`). */
    if (irop_config[pq->op].has_dest &&
        pq->op != TCCIR_OP_FUNCPARAMVAL && pq->op != TCCIR_OP_FUNCPARAMVOID) {
      IROperand pd = tcc_ir_op_get_dest(ir, pq);
      if (irop_get_vreg(pd) == src_vr)
        return 0;
    }

    /* Match the prior LOAD: same op, same source flags+vreg, TEMP dest. */
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

  /* Rewrite: T2 <-- prior_dest [ASSIGN]. */
  IROperand new_src = src;
  irop_set_vreg(&new_src, prior_dest_vr);
  new_src.is_lval = 0;
  new_src.is_local = 0;
  new_src.is_llocal = 0;

  q->op = TCCIR_OP_ASSIGN;
  tcc_ir_op_set_src1(ir, q, new_src);

  /* Update use chains: remove this instr's use of src_vr, add use of
   * prior_dest_vr (only effective for TEMP — VAR/PARAM not tracked). */
  IRSSAVregInfo *svi = ssa_opt_vinfo(ctx, src_vr);
  if (svi)
    ssa_opt_remove_use_instr(svi, idx);
  IRSSAVregInfo *pvi = ssa_opt_vinfo(ctx, prior_dest_vr);
  if (pvi)
    ssa_opt_add_use_instr(pvi, idx);

  return 1;
}

/* ============================================================================
 * Generator: ssa_gen_cprop_symref_cse
 *
 * Pattern: T2 <-- GlobalSym(N) [ASSIGN]  with a prior ASSIGN of the same
 *          SYMREF (same sym + addend + lval flag) in the same basic block.
 * Action:  rewrite to T2 <-- T1 [ASSIGN], where T1 is the prior dest.
 *          cprop_assign then forwards T1 into T2's uses, eliminating the
 *          duplicate PC-relative literal load at codegen time.
 *
 * This handles cases like main where `&arr` is materialized 2-3 times for
 * separate uses; only the first ldr [pc, #N] needs to fire.
 * ============================================================================ */
static int ssa_gen_cprop_symref_cse(IRSSAOptCtx *ctx, int idx)
{
  TCCIRState *ir = ctx->ir;
  IRCFG *cfg = ctx->cfg;
  if (!cfg)
    return 0;

  IRQuadCompact *q = &ir->compact_instructions[idx];
  IROperand src = tcc_ir_op_get_src1(ir, q);
  if (src.tag != IROP_TAG_SYMREF)
    return 0;
  /* lval/local/llocal symrefs encode different access patterns; only fuse
   * plain symbol-address materializations. */
  if (src.is_lval || src.is_local || src.is_llocal)
    return 0;

  IROperand dest = tcc_ir_op_get_dest(ir, q);
  int32_t dest_vr = irop_get_vreg(dest);
  if (dest_vr < 0 || TCCIR_DECODE_VREG_TYPE(dest_vr) != TCCIR_VREG_TYPE_TEMP)
    return 0;

  IRPoolSymref *cur_sr = irop_get_symref_ex(ir, src);
  if (!cur_sr)
    return 0;

  int blk = cfg->instr_to_block[idx];
  if (blk < 0 || blk >= cfg->num_blocks)
    return 0;
  IRBasicBlock *bb = &cfg->blocks[blk];

  int32_t prior_dest_vr = -1;
  for (int k = idx - 1; k >= bb->start_idx; k--) {
    IRQuadCompact *pq = &ir->compact_instructions[k];
    if (pq->op == TCCIR_OP_NOP)
      continue;

    /* Calls / asm / VLA / setjmp can change reachable symbol mappings or
     * introduce side effects that make caching unsafe — bail. */
    switch (pq->op) {
    case TCCIR_OP_FUNCCALLVAL:
    case TCCIR_OP_FUNCCALLVOID:
    case TCCIR_OP_INLINE_ASM:
    case TCCIR_OP_VLA_ALLOC:
    case TCCIR_OP_SETJMP:
    case TCCIR_OP_LONGJMP:
    case TCCIR_OP_NL_SETJMP:
    case TCCIR_OP_NL_LONGJMP:
      return 0;
    default:
      break;
    }

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

  /* Rewrite this ASSIGN: src becomes prior dest as a VREG operand. */
  IROperand new_src = (IROperand){0};
  new_src.tag = IROP_TAG_VREG;
  irop_set_vreg(&new_src, prior_dest_vr);
  new_src.is_lval = 0;
  tcc_ir_op_set_src1(ir, q, new_src);

  IRSSAVregInfo *pvi = ssa_opt_vinfo(ctx, prior_dest_vr);
  if (pvi)
    ssa_opt_add_use_instr(pvi, idx);

  return 1;
}

/* ============================================================================
 * Generator: ssa_gen_cprop_copy_param
 *
 * Pattern: T_dest <-- P_src [LOAD or ASSIGN]  where:
 *   - src is a register-resident vreg (not lval, not local, not llocal)
 *   - src is a PARAM or VAR vreg
 *   - T_dest is a TEMP with single def
 *   - All uses of T_dest are in the same block as the copy, after it
 *   - No instruction between the copy and any use redefines src
 *   - No call/asm/VLA between the copy and any use (could clobber
 *     stack-spilled PARAM/VAR storage)
 *
 * Action: forward src into all uses of T_dest; nop the copy.
 *
 * cprop_assign refuses PARAM/VAR sources unconditionally because they
 * are not SSA-renamed (multi-def possible). This generator adds the
 * dataflow check that makes the propagation safe, eliminating the
 * `mov rN, rM` that the regalloc otherwise emits when T_dest and P_src
 * get different physical registers (e.g. arr[i] = value with `value`
 * as a parameter).
 * ============================================================================ */

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


  /* LOAD can perform implicit narrowing/widening when dest's btype or
   * signedness differs from src's (e.g. `unsigned char c = arg_int`).
   * Forwarding src into uses of dest would skip that conversion. Bail
   * unless the types are identical. */
  if (irop_get_btype(src) != irop_get_btype(dest) ||
      src.is_unsigned != dest.is_unsigned)
    return 0;

  /* For LOAD specifically, an additional subtlety: a sub-word PARAM/VAR
   * source carries AAPCS-promoted bits in its physical register even when
   * its IR btype is INT8/INT16. `T <-- P [LOAD]` is the point where those
   * upper bits get masked/sign-extended (UXTB/SXTB/UXTH/SXTH). Downstream
   * uses of T expect the narrowed value; forwarding P directly leaves the
   * AAPCS garbage in the register. Skip propagation in this case — only
   * ASSIGN (a pure copy) is safe here. */
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

  /* All uses must be in the same block as the copy, and after the copy. */
  int max_use_idx = idx;
  for (int u = 0; u < dvi->use_count; u++) {
    IRSSAUse use = dvi->uses[u];
    if (use.kind != SSA_USE_INSTR)
      return 0;
    if (cfg->instr_to_block[use.idx] != copy_blk)
      return 0;
    if (use.idx <= idx)
      return 0;
    if (use.idx > max_use_idx)
      max_use_idx = use.idx;
  }

  /* Scan from copy+1 through max_use_idx: bail on any redef of src or any
   * call/asm/VLA. STORE to a vreg (e.g. `P0 <-- #5 [STORE]`) is treated
   * as a redef. */
  for (int k = idx + 1; k <= max_use_idx; k++) {
    IRQuadCompact *kq = &ir->compact_instructions[k];
    if (kq->op == TCCIR_OP_NOP)
      continue;

    switch (kq->op) {
    case TCCIR_OP_FUNCCALLVAL:
    case TCCIR_OP_FUNCCALLVOID:
    case TCCIR_OP_ASM_INPUT:
    case TCCIR_OP_INLINE_ASM:
    case TCCIR_OP_ASM_OUTPUT:
    case TCCIR_OP_VLA_ALLOC:
    case TCCIR_OP_SETJMP:
    case TCCIR_OP_LONGJMP:
    case TCCIR_OP_NL_SETJMP:
    case TCCIR_OP_NL_LONGJMP:
      return 0;
    default:
      break;
    }

    if (irop_config[kq->op].has_dest &&
        kq->op != TCCIR_OP_FUNCPARAMVAL && kq->op != TCCIR_OP_FUNCPARAMVOID) {
      IROperand kd = tcc_ir_op_get_dest(ir, kq);
      if (irop_get_vreg(kd) == src_vr)
        return 0;
    }
  }

  /* Forward src into all uses of dest. ssa_opt_replace_all_uses handles
   * new_vi == NULL gracefully (PARAM/VAR don't have vinfo). */
  int replaced = ssa_opt_replace_all_uses(ctx, dest_vr, src_vr);
  if (replaced == 0)
    return 0;

  /* The copy is now dead; nop it. DCE would also remove it, but doing
   * it here keeps the change count meaningful. */
  ssa_opt_nop_instr(ctx, idx);
  return 1;
}

/* ============================================================================
 * Generator: ssa_gen_cprop_copy_var_stackoff
 *
 * Pattern: `T <-- V_stackoff [ASSIGN]` where V is a VAR encoded as a
 * STACKOFF operand (is_lval=1, is_local=1) — the canonical IR-gen form
 * for reading a local variable's value.  T is a TEMP with a single def
 * (this ASSIGN) and all uses in the same BB after the ASSIGN.
 *
 * Action: forward V's full operand (STACKOFF/is_lval=1/is_local=1) into
 * each use of T, then NOP the copy.  Mirrors cprop_copy_param but for
 * STACKOFF-tagged sources, which the regular path skips at its
 * `src.is_lval || src.is_local` guard.
 *
 * Eliminates `T = V [ASSIGN]; T2 = T + #imm` chains that the inlined
 * relops produce — without this, every `c->field` access re-emits the
 * `mov r1, r4` of V1 into a fresh TEMP.  Six checks × six fields = many
 * dead movs.  cprop_copy_param skips this case because V's source-side
 * encoding uses STACKOFF/is_local=1, not VREG.
 * ============================================================================ */

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

  /* Btype equality, with one accommodation: INT32 ↔ STRUCT can co-occur
   * on pointer-typed locals — both are 32-bit and the IR-gen sometimes
   * tags pointers with the pointee's btype. */
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

  int max_use_idx = idx;
  for (int u = 0; u < dvi->use_count; u++) {
    IRSSAUse use = dvi->uses[u];
    if (use.kind != SSA_USE_INSTR)
      return 0;
    if (cfg->instr_to_block[use.idx] != copy_blk)
      return 0;
    if (use.idx <= idx)
      return 0;
    if (use.idx > max_use_idx)
      max_use_idx = use.idx;

    /* Skip forwarding through a deref use site only when V's value could
     * be a stack address.  Reason: substituting `T → V` into a `*T`
     * use creates `*V_DEREF`; if V holds &StackLoc[N], the spill/reload
     * path for V loses the deref shape (regalloc rewrites `*V` to
     * `T_new = stack_slot; *(T_new)` and a subsequent SCCP/branch-fold
     * pass mistakes `*T_new` for `T_new` as a value — test 20000605-2
     * loop test broke because `*V_DEREF` got compared against the raw
     * pointer T_new rather than its deref).
     *
     * V's value is "stack-address-like" when its single def stores an
     * Addr[StackLoc[N]] (directly or via a TEMP chain).  When V's value
     * is a runtime-computed pointer (e.g. MLA result, malloc return)
     * the spill/reload path keeps the deref intact and forwarding is
     * safe — that's the common case for inlined-helper printf-arg
     * pointers in test_llong_relops. */
    IRQuadCompact *uq_check = &ir->compact_instructions[use.idx];
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
    if (has_deref_use) {
      /* Check V's def — if it stores a stack address, bail. */
      int32_t var_pos_check = TCCIR_DECODE_VREG_POSITION(src_vr);
      int unsafe = 0;
      for (int k = idx - 1; k >= 0 && !unsafe; k--) {
        IRQuadCompact *kq = &ir->compact_instructions[k];
        if (kq->op == TCCIR_OP_NOP)
          continue;
        if (!irop_config[kq->op].has_dest)
          continue;
        IROperand kd = tcc_ir_op_get_dest(ir, kq);
        int32_t kdv = irop_get_vreg(kd);
        if (kdv < 0 || TCCIR_DECODE_VREG_TYPE(kdv) != TCCIR_VREG_TYPE_VAR)
          continue;
        if (TCCIR_DECODE_VREG_POSITION(kdv) != var_pos_check)
          continue;
        /* Found a def of V.  Inspect the stored value. */
        IROperand kstored = tcc_ir_op_get_src1(ir, kq);
        if (kstored.tag == IROP_TAG_STACKOFF && !kstored.is_lval && kstored.is_local)
          unsafe = 1; /* V <- Addr[StackLoc[N]] */
        else {
          int32_t kvr = irop_get_vreg(kstored);
          if (kvr >= 0 && TCCIR_DECODE_VREG_TYPE(kvr) == TCCIR_VREG_TYPE_TEMP &&
              ssa_opt_resolve_lea_stackloc(ctx, kvr) != INT_MIN)
            unsafe = 1; /* V <- T where T resolves to &StackLoc */
        }
        break; /* only inspect the most recent def */
      }
      if (unsafe)
        return 0;
    }
  }

  /* Bail on barriers (calls, asm, VLA, setjmp/longjmp) and on any
   * STORE/ASSIGN that writes V's slot between the copy and last use. */
  for (int k = idx + 1; k <= max_use_idx; k++) {
    IRQuadCompact *kq = &ir->compact_instructions[k];
    if (kq->op == TCCIR_OP_NOP)
      continue;

    switch (kq->op) {
    case TCCIR_OP_FUNCCALLVAL:
    case TCCIR_OP_FUNCCALLVOID:
    case TCCIR_OP_ASM_INPUT:
    case TCCIR_OP_INLINE_ASM:
    case TCCIR_OP_ASM_OUTPUT:
    case TCCIR_OP_VLA_ALLOC:
    case TCCIR_OP_SETJMP:
    case TCCIR_OP_LONGJMP:
    case TCCIR_OP_NL_SETJMP:
    case TCCIR_OP_NL_LONGJMP:
      return 0;
    default:
      break;
    }

    if (irop_config[kq->op].has_dest &&
        kq->op != TCCIR_OP_FUNCPARAMVAL && kq->op != TCCIR_OP_FUNCPARAMVOID) {
      IROperand kd = tcc_ir_op_get_dest(ir, kq);
      if (irop_get_vreg(kd) == src_vr)
        return 0;
    }
  }

  /* Forward V into all uses of dest_vr by swapping the vreg ID only —
   * keeping each use site's tag/is_lval/is_local intact.  This is the
   * critical invariant: a use like `T_dst <-- T_DEREF [LOAD]` has
   * tag=VREG, is_lval=1, meaning "deref via this register".  If we
   * overwrite the whole operand with V's STACKOFF/lval/local encoding,
   * the codegen reads V's stack slot (its value) and then dereferences
   * THAT — an extra indirection that corrupts the load.  vreg-only swap
   * keeps the semantics of the use site, while regalloc/codegen looks
   * up V's home (register or stack) when materializing the operand. */
  int replaced = ssa_opt_replace_all_uses(ctx, dest_vr, src_vr);
  if (replaced == 0)
    return 0;

  ssa_opt_nop_instr(ctx, idx);
  return 1;
}

/* ============================================================================
 * Generator Table
 * ============================================================================ */

/* Dispatcher: try the three ASSIGN-keyed rules in order. The gen-table
 * runner breaks after the first matching entry, so without this wrapper
 * only cprop_assign would ever run. */
static int ssa_gen_cprop_assign_any(IRSSAOptCtx *ctx, int idx)
{
  TCCIRState *ir = ctx->ir;
  IRQuadCompact *q = &ir->compact_instructions[idx];
  IROperand src = tcc_ir_op_get_src1(ir, q);

  /* Pick the right rule based on src tag. ssa_gen_cprop_assign handles
   * TEMP vreg sources; ssa_gen_cprop_copy_param handles PARAM/VAR vreg
   * sources with a dataflow safety scan; ssa_gen_cprop_symref_cse
   * handles symbol-address materializations. cprop_imm (immediate
   * forwarding) is intentionally skipped — enabling it currently triggers
   * latent SCCP/phi-simplify issues for switch/goto patterns
   * (see test_switch_goto_or). */
  if (src.tag == IROP_TAG_VREG) {
    int32_t src_vr = irop_get_vreg(src);
    if (src_vr >= 0 && TCCIR_DECODE_VREG_TYPE(src_vr) == TCCIR_VREG_TYPE_TEMP)
      return ssa_gen_cprop_assign(ctx, idx);
    return ssa_gen_cprop_copy_param(ctx, idx);
  }
  if (src.tag == IROP_TAG_STACKOFF)
    return ssa_gen_cprop_copy_var_stackoff(ctx, idx);
  if (src.tag == IROP_TAG_SYMREF)
    return ssa_gen_cprop_symref_cse(ctx, idx);
  return 0;
}

/* Dispatcher for LOAD: tries the param-copy forwarding rule first (cheap,
 * narrowly scoped), then falls back to the BB-local redundant-load rule. */
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
  return ssa_gen_cprop_load_redundant(ctx, idx);
}

static const IRSSAOptGen cprop_gens[] = {
  { TCCIR_OP_ASSIGN, ssa_gen_cprop_assign_any, "cprop_assign_any" },
  { TCCIR_OP_LOAD,   ssa_gen_cprop_load_any,   "cprop_load_any"   },
};

/* ============================================================================
 * Pass: ssa_opt_symref_operand_cse
 *
 * Rewrites operands of the form `SYMREF(sym)***DEREF***` (tag=SYMREF,
 * is_lval=1) to `Tn***DEREF***` when an earlier ASSIGN `Tn = SYMREF(sym)`
 * (address-only, is_lval=0) exists in the same basic block with no
 * intervening write to Tn. The backend then uses the cached register
 * holding the symbol's address instead of re-emitting an `ldr [pc, #N]`
 * literal-pool load before the deref.
 *
 * Block-local only; barrier on calls/asm/VLA.
 * ============================================================================ */
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
    switch (pq->op) {
    case TCCIR_OP_FUNCCALLVAL:
    case TCCIR_OP_FUNCCALLVOID:
    case TCCIR_OP_INLINE_ASM:
    case TCCIR_OP_VLA_ALLOC:
    case TCCIR_OP_SETJMP:
    case TCCIR_OP_LONGJMP:
    case TCCIR_OP_NL_SETJMP:
    case TCCIR_OP_NL_LONGJMP:
      return 0;
    /* STOREs are safe across this rewrite — the SYMREF address is a
     * constant. Both `*&sym` and `*Tn` (with Tn holding &sym) read the
     * same memory location, so a STORE that updates that memory affects
     * both formulations identically. */
    default:
      break;
    }
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

/* ============================================================================
 * Pass Entry Point
 * ============================================================================ */

int ssa_opt_cprop(IRSSAOptCtx *ctx)
{
  int changes = ssa_opt_run_gens(ctx, cprop_gens,
                                  sizeof(cprop_gens) / sizeof(cprop_gens[0]));
  changes += ssa_opt_symref_operand_cse(ctx);
  return changes;
}

/* ============================================================================
 * VAR Value Forwarding (multi-use safe)
 *
 * Pattern:
 *   V <- val [STORE]    (V is a single-def, non-address-taken VAR)
 *   ...
 *   use1(V), use2(V), ...   (all reads of V as a value, no deref-via-V)
 *
 * Action: rewrite each use's V operand to val directly; NOP the STORE.
 *
 * Inlined helpers like check1 materialise printf args into VARs before
 * the conditional branch:  `V4 = name; V5 = got; V6 = exp; cmp got, exp;
 * jeq success; PARAM ... V4; PARAM ... V5; PARAM ... V6; call printf`.
 * On the success path the VAR stores are pure waste — they spill to the
 * stack only to be re-read in the FAIL branch.  Forwarding V into all
 * uses lets DCE remove the STOREs entirely.
 *
 * Constraints (vs. broader ssa_opt_var_forward):
 *   - Only value uses (is_lval=0 V operand) — never forward into a
 *     deref-via-V site, because the codegen rewrites can expose SCCP
 *     stack-load alias issues (test 20000605-2: a deref through V's
 *     pointer-value got folded to a stale stack-init constant).
 *   - Stored value must not itself be a deref (is_lval=0): the codegen
 *     for STORE-with-deref-src is already efficient (one ldr), and
 *     duplicating the deref into multiple uses costs more loads.
 *   - Stored value must not be a stack-address constant
 *     (Addr[StackLoc[N]]): exposes the same SCCP alias issue.
 *
 * The pass operates on the entire function (not just a basic block) and
 * uses dominator checks for cross-block uses, so the FAIL-path PARAMs in
 * separate BBs from the def are handled.
 * ============================================================================ */

/* Helper: is block `def_blk` an ancestor of `use_blk` in the dominator tree? */
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

/* Helper: is `stored_val` safe to fan out across multiple uses? */
static int v2v_is_safe_value(IROperand op)
{
  if (op.is_lval || op.is_llocal)
    return 0;
  /* Refuse stack-address sources (Addr[StackLoc[N]]): forwarding into a
   * deref-via-V chain would expose SCCP stack-load aliasing issues. */
  if (op.tag == IROP_TAG_STACKOFF && !op.is_lval && op.is_local)
    return 0;
  return 1;
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

  /* Nested functions read parent VARs through the static chain without
   * explicit IR uses — use_count would underestimate.  Bail out only when
   * this function actually contains chain-setup ops; otherwise trust the
   * per-VAR addrtaken bit below (gen_function clears it for VARs whose
   * nested-func captors have all been inlined). */
  for (int i = 0; i < ir->next_instruction_index; i++) {
    int op = ir->compact_instructions[i].op;
    if (op == TCCIR_OP_SET_CHAIN || op == TCCIR_OP_INIT_CHAIN_SLOT)
      return 0;
  }

  int n = ir->next_instruction_index;
  int *var_def_count = tcc_mallocz(num_vars * sizeof(int));
  int *var_def_instr = tcc_mallocz(num_vars * sizeof(int));
  uint8_t *var_addrtaken = tcc_mallocz((num_vars + 7) / 8);
  uint8_t *var_bad_use = tcc_mallocz((num_vars + 7) / 8); /* deref/lval use, can't forward */
  for (int i = 0; i < num_vars; i++)
    var_def_instr[i] = -1;

  /* Pass 1: discover def sites, address-taken status, and bad-use status. */
  for (int i = 0; i < n; i++) {
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
        if (pos < num_vars) {
          var_def_count[pos]++;
          var_def_instr[pos] = i;
          /* A `*V <-- val` STORE (deref-via-V's-pointer-value) is a
           * use of V's value, not a slot write.  Discriminator: V's
           * slot dest carries is_local=1 (VT_LOCAL encoding); a deref
           * dest carries is_local=0 (TEMP-style pointer encoding).
           * Only mark the deref case as bad_use; treat the slot case
           * as the canonical def. */
          if ((q->op == TCCIR_OP_STORE || q->op == TCCIR_OP_STORE_INDEXED) &&
              d.is_lval && !d.is_local)
            var_bad_use[pos / 8] |= (1 << (pos % 8));
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
      if (pos >= num_vars)
        continue;
      /* &V (address-of): is_local && !is_lval. */
      if (s.is_local && !s.is_lval) {
        var_addrtaken[pos / 8] |= (1 << (pos % 8));
        continue;
      }
      /* Distinguish two is_lval=1 encodings on V operands:
       *   - VREG/is_lval=1/!is_local: deref V's value (treat V's register
       *     as a pointer and read pointed-to memory).  UNSAFE to forward
       *     — would duplicate the deref at the use site.
       *   - STACKOFF/is_lval=1/is_local=1: slot-read (read V's stored
       *     value from its stack home).  SAFE — equivalent to a plain
       *     value read; forwarding just rerouts the source.
       *   - VREG/is_lval=0: register-resident value read.  SAFE.
       * Mark only the deref case as bad. */
      if (s.is_lval && s.tag == IROP_TAG_VREG && !s.is_local)
        var_bad_use[pos / 8] |= (1 << (pos % 8));
    }
    /* MLA accum operand. */
    if (q->op == TCCIR_OP_MLA) {
      IROperand a = tcc_ir_op_get_accum(ir, q);
      int32_t vr = irop_get_vreg(a);
      if (vr >= 0 && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_VAR) {
        int pos = TCCIR_DECODE_VREG_POSITION(vr);
        if (pos < num_vars && a.is_lval && a.tag == IROP_TAG_VREG && !a.is_local)
          var_bad_use[pos / 8] |= (1 << (pos % 8));
      }
    }
    if (q->op == TCCIR_OP_LEA) {
      IROperand s = tcc_ir_op_get_src1(ir, q);
      int32_t vr = irop_get_vreg(s);
      if (vr >= 0 && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_VAR) {
        int pos = TCCIR_DECODE_VREG_POSITION(vr);
        if (pos < num_vars)
          var_addrtaken[pos / 8] |= (1 << (pos % 8));
      }
    }
  }

  int changes = 0;

  /* Pass 2: for each candidate V, walk its uses and forward if all are
   * value-uses dominated by the def. */
  for (int pos = 0; pos < num_vars; pos++) {
    if (var_def_count[pos] != 1)
      continue;
    if (var_addrtaken[pos / 8] & (1 << (pos % 8)))
      continue;
    if (var_bad_use[pos / 8] & (1 << (pos % 8)))
      continue;
    int def_idx = var_def_instr[pos];
    if (def_idx < 0)
      continue;

    IRQuadCompact *def_q = &ir->compact_instructions[def_idx];
    if (def_q->op != TCCIR_OP_STORE && def_q->op != TCCIR_OP_ASSIGN)
      continue;

    IROperand stored_val = tcc_ir_op_get_src1(ir, def_q);
    if (!v2v_is_safe_value(stored_val))
      continue;
    int32_t stored_vr = irop_get_vreg(stored_val);
    /* Don't forward a VAR-typed value into another VAR's uses — risks
     * creating dependence chains that break other passes. */
    if (stored_vr >= 0 && TCCIR_DECODE_VREG_TYPE(stored_vr) == TCCIR_VREG_TYPE_VAR)
      continue;
    /* Don't forward a TEMP whose value is a stack address — fanning out
     * an `Addr[StackLoc]` into multiple use sites lets SCCP's
     * stack-load alias tracker reach the use through the new TEMP
     * chain and fold a stale stack-init value (test 20000605-2 loop). */
    if (stored_vr >= 0 &&
        TCCIR_DECODE_VREG_TYPE(stored_vr) == TCCIR_VREG_TYPE_TEMP &&
        ssa_opt_resolve_lea_stackloc(ctx, stored_vr) != INT_MIN)
      continue;

    int def_blk = cfg->instr_to_block[def_idx];

    /* Collect use sites; check each is dominated by def and there's no
     * barrier on any path from def to use. */
    int32_t target_vr = TCCIR_ENCODE_VREG(TCCIR_VREG_TYPE_VAR, pos);
    int safe = 1;
    int max_use_idx = def_idx;
    int found_any = 0;
    for (int j = def_idx + 1; j < n && safe; j++) {
      IRQuadCompact *uq = &ir->compact_instructions[j];
      if (uq->op == TCCIR_OP_NOP)
        continue;

      int touches = 0;
      int nsrc = irop_config[uq->op].has_src1 + irop_config[uq->op].has_src2;
      for (int oi = 0; oi < nsrc; oi++) {
        IROperand s = oi == 0 ? tcc_ir_op_get_src1(ir, uq) : tcc_ir_op_get_src2(ir, uq);
        if (irop_get_vreg(s) == target_vr) {
          touches = 1;
          break;
        }
      }
      if (uq->op == TCCIR_OP_MLA) {
        IROperand a = tcc_ir_op_get_accum(ir, uq);
        if (irop_get_vreg(a) == target_vr)
          touches = 1;
      }
      if (!touches)
        continue;

      if (!v2v_dominates(cfg, def_blk, cfg->instr_to_block[j])) {
        safe = 0;
        break;
      }
      found_any = 1;
      if (j > max_use_idx)
        max_use_idx = j;
    }
    if (!safe || !found_any)
      continue;

    /* Scan instructions between def and last use for barriers that could
     * invalidate the forwarded value.  Conservative — drops any
     * call/asm/VLA/setjmp.  Plain stores are OK because V is non-address-
     * taken and stored_val is non-lval (so no aliasing). */
    for (int k = def_idx + 1; k <= max_use_idx && safe; k++) {
      int op = ir->compact_instructions[k].op;
      switch (op) {
      case TCCIR_OP_FUNCCALLVAL:
      case TCCIR_OP_FUNCCALLVOID:
      case TCCIR_OP_INLINE_ASM:
      case TCCIR_OP_ASM_INPUT:
      case TCCIR_OP_ASM_OUTPUT:
      case TCCIR_OP_VLA_ALLOC:
      case TCCIR_OP_SETJMP:
      case TCCIR_OP_LONGJMP:
      case TCCIR_OP_NL_SETJMP:
      case TCCIR_OP_NL_LONGJMP:
        /* If stored_val's vreg is a TEMP whose live range doesn't cross
         * a call, we'd need the call to NOT clobber it.  Conservative:
         * if stored_val is a TEMP, bail on any call between def and use.
         * Symref/immediate values are call-safe (the codegen
         * rematerializes them at each use). */
        if (stored_val.tag == IROP_TAG_VREG)
          safe = 0;
        break;
      default:
        break;
      }
    }
    if (!safe)
      continue;

    /* Forward V into all uses.
     *
     * For LOAD/ASSIGN uses we must also rewrite the op to ASSIGN: a
     * `T <-- V [LOAD]` reads V's slot value, but after substituting V
     * with a non-slot operand (e.g. a TEMP value), `T <-- val [LOAD]`
     * would be interpreted as `T = *val` (deref-through-val) and emit
     * an erroneous ldr.  Other ops (CMP, FUNCPARAMVAL, ADD, etc.) carry
     * the operand purely as a value and need no op change. */
    int local_changes = 0;
    for (int j = def_idx + 1; j <= max_use_idx; j++) {
      IRQuadCompact *uq = &ir->compact_instructions[j];
      if (uq->op == TCCIR_OP_NOP)
        continue;

      int touched = 0;
      if (irop_config[uq->op].has_src1) {
        IROperand s = tcc_ir_op_get_src1(ir, uq);
        if (irop_get_vreg(s) == target_vr) {
          tcc_ir_set_src1(ir, j, stored_val);
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
    if (local_changes > 0) {
      ssa_opt_nop_instr(ctx, def_idx);
      changes += local_changes;
    }
  }

  tcc_free(var_def_count);
  tcc_free(var_def_instr);
  tcc_free(var_addrtaken);
  tcc_free(var_bad_use);
  return changes;
}

/* ============================================================================
 * VAR Forwarding: propagate single-def VAR values into their uses.
 *
 * Pattern:  Vn <-- Tx [STORE]   (single def within block)
 *           Ty <-- Vn [ASSIGN]  (use in same block, after def)
 * Action:   Ty <-- Tx [ASSIGN]
 *
 * VARs from inline expansion are typically single-def and used within
 * the same block.  Without SSA promotion, the optimizer can't see through
 * them; this pass makes their values visible to load_cse and SCCP.
 * ============================================================================ */

int ssa_opt_var_forward(IRSSAOptCtx *ctx)
{
  TCCIRState *ir = ctx->ir;
  IRCFG *cfg = ctx->cfg;
  if (!cfg || cfg->num_blocks == 0)
    return 0;

  int num_vars = ir->next_local_variable;
  if (num_vars <= 0)
    return 0;

  /* Count defs per VAR across entire function */
  int *var_def_count = tcc_mallocz(num_vars * sizeof(int));
  int *var_def_instr = tcc_mallocz(num_vars * sizeof(int));
  uint8_t *var_addrtaken = tcc_mallocz((num_vars + 7) / 8);

  for (int i = 0; i < num_vars; i++)
    var_def_instr[i] = -1;

  for (int i = 0; i < ir->next_instruction_index; i++) {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;

    /* Mark address-taken VARs — check all instructions for src operands
     * that reference a VAR with is_local && !is_lval (address-of-local). */
    {
      int nops = irop_config[q->op].has_src1 + irop_config[q->op].has_src2;
      for (int oi = 0; oi < nops; oi++) {
        IROperand s = oi == 0 ? tcc_ir_op_get_src1(ir, q) : tcc_ir_op_get_src2(ir, q);
        if (s.is_local && !s.is_lval) {
          int32_t vr = irop_get_vreg(s);
          if (vr >= 0 && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_VAR) {
            int pos = TCCIR_DECODE_VREG_POSITION(vr);
            if (pos < num_vars)
              var_addrtaken[pos / 8] |= (1 << (pos % 8));
          }
        }
      }
      if (q->op == TCCIR_OP_LEA) {
        IROperand s = tcc_ir_op_get_src1(ir, q);
        int32_t vr = irop_get_vreg(s);
        if (vr >= 0 && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_VAR) {
          int pos = TCCIR_DECODE_VREG_POSITION(vr);
          if (pos < num_vars)
            var_addrtaken[pos / 8] |= (1 << (pos % 8));
        }
      }
    }

    /* Count ALL defs to VARs (any instruction with a VAR as dest) */
    if (irop_config[q->op].has_dest &&
        q->op != TCCIR_OP_FUNCPARAMVAL && q->op != TCCIR_OP_FUNCPARAMVOID) {
      IROperand dest = tcc_ir_op_get_dest(ir, q);
      int32_t dvr = irop_get_vreg(dest);
      if (dvr >= 0 && TCCIR_DECODE_VREG_TYPE(dvr) == TCCIR_VREG_TYPE_VAR) {
        int pos = TCCIR_DECODE_VREG_POSITION(dvr);
        if (pos < num_vars) {
          var_def_count[pos]++;
          var_def_instr[pos] = i;
        }
      }
    }
  }

  int changes = 0;

  /* For each single-def, non-address-taken VAR: replace uses with stored value */
  for (int i = 0; i < ir->next_instruction_index; i++) {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_ASSIGN && q->op != TCCIR_OP_LOAD)
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

    /* A function call between def and use may modify the VAR through
     * a closure chain (nested functions).  Skip forwarding in that case. */
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

    /* Replace the source with the stored value */
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

  tcc_free(var_def_count);
  tcc_free(var_def_instr);
  tcc_free(var_addrtaken);
  return changes;
}

/* ============================================================================
 * VAR Self-Update Constant Fold: collapse `Vx = Vx OP #imm` against a
 * dominating prior `Vx = #const` in the same block.
 *
 * The standard fold pass requires both operands to be immediate-tagged, which
 * misses VARs whose value was just stored as a constant (no SSA promotion for
 * single-block multi-def vars). This peephole walks back within the block,
 * bailing on anything that could alias or rewrite Vx, and folds the read-side
 * using the prior store.  The prior store is NOPed (now dead).
 * ============================================================================ */

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
  if (src2.tag != IROP_TAG_IMM32 || src2.is_lval)
    return 0;
  /* src1 must be a read of the same VAR. Accept either bare VREG or lval
   * STACKOFF encoding — the frontend uses the latter when the VAR's value is
   * read into an arithmetic op. */
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

    /* Anything that could alias Vx through memory or call kills our fold. */
    switch (pq->op) {
    case TCCIR_OP_FUNCCALLVAL:
    case TCCIR_OP_FUNCCALLVOID:
    case TCCIR_OP_STORE:
    case TCCIR_OP_STORE_INDEXED:
    case TCCIR_OP_STORE_POSTINC:
    case TCCIR_OP_BLOCK_COPY:
    case TCCIR_OP_INLINE_ASM:
    case TCCIR_OP_VLA_ALLOC:
    case TCCIR_OP_SETJMP:
    case TCCIR_OP_LONGJMP:
    case TCCIR_OP_NL_SETJMP:
    case TCCIR_OP_NL_LONGJMP:
      return 0;
    default:
      break;
    }

    if (irop_config[pq->op].has_dest &&
        pq->op != TCCIR_OP_FUNCPARAMVAL && pq->op != TCCIR_OP_FUNCPARAMVOID) {
      IROperand pd = tcc_ir_op_get_dest(ir, pq);
      /* var_forward treats any instruction with a VAR-encoded dest as a def
       * (regardless of is_lval), since writes to a VAR may be expressed via
       * either bare-vreg or lval-stackoff encoding.  Match that here. */
      if (irop_get_vreg(pd) == dest_vr) {
        if (pq->op == TCCIR_OP_ASSIGN) {
          IROperand ps = tcc_ir_op_get_src1(ir, pq);
          if (ps.tag == IROP_TAG_IMM32 && !ps.is_lval) {
            prior_idx = k;
            prior_val = ps.u.imm32;
          }
        }
        /* Found a write to Vx — either captured constant or unknown. Stop. */
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

  IROperand imm = irop_make_imm32(0, (int32_t)result, dest.btype);
  q->op = TCCIR_OP_ASSIGN;
  tcc_ir_op_set_src1(ir, q, imm);
  tcc_ir_op_set_src2(ir, q, IROP_NONE);

  ir->compact_instructions[prior_idx].op = TCCIR_OP_NOP;
  return 1;
}

int ssa_opt_var_const_fold(IRSSAOptCtx *ctx)
{
  TCCIRState *ir = ctx->ir;

  /* Bail on functions with computed goto or switch-table jumps: the CFG
   * does not enumerate every IJMP target, so the basic block containing a
   * self-update may actually be re-entered mid-block via a label-as-value.
   * Walking back to a "prior store" then folds against the function-entry
   * value rather than the per-iteration value (regression in 920501-3). */
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
