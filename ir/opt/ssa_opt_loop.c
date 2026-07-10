/*
 *  TCC IR - SSA/CFG-era Loop Rotation
 *
 *  Copyright (c) 2025 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

/*
 * ssa_opt_loop_rotate() — SSA/CFG-era front-end for the legacy pre-SSA loop
 * rotation pass (tcc_ir_opt_loop_rotation in ir/opt_loop.c).
 *
 * It runs immediately before CFG/SSA construction in tcc_ir_ssa_regalloc, on
 * flat IR.  The improvement over the legacy pass is *loop detection*: candidate
 * natural loops are found from a real CFG + dominator tree (dominance-verified
 * back-edges) instead of the legacy range-scan detector (tcc_ir_detect_loops),
 * whose flat body-range heuristics were the documented root cause of several
 * rotation miscompiles.
 *
 * The rewrite itself reuses the regression-proven flat-IR mutator
 * try_rotate_loop(), which derives the back-edge / body / latch regions itself
 * from just header_idx + end_idx and self-validates the [CMP, JUMPIF, JUMP]
 * top-tested header, returning 0 on any shape it does not model.  That makes
 * this pass:
 *   - conservative: anything outside the modeled shape is left untouched;
 *   - idempotent: an already-rotated (bottom-tested) loop no longer has the
 *     top-tested header, so it is declined.  Safe to run alongside the legacy
 *     pass while that is still enabled (the legacy pass rotates first, so this
 *     pass sees bottom-tested loops and declines them), and safe to re-run.
 *
 * The mutator rewrites strictly in place (it NOPs [hi+2 .. body_end_jmp] and
 * writes the rotated code back into those slots), so instruction indices never
 * shift; only control flow changes.  That invalidates the CFG we built for
 * detection but not the flat instruction stream — so we detect from a fresh
 * CFG, free it, run the (index-stable) rewrites, then rebuild across passes to
 * pick up loops newly exposed by an earlier rotation.
 *
 * Deferred: the internal region scans inside try_rotate_loop still use flat
 * heuristics.  Replacing those with CFG facts is a later migration step; v1
 * deliberately keeps the proven rewrite and only swaps the loop detector.  See
 * docs/plan_legacy_loop_rotation_ssa.md.
 */

#include <string.h>

#include "ir.h"
#include "ssa_opt.h"
#include "licm.h"           /* IRLoop */
#include "opt_loop_utils.h" /* try_rotate_loop, loop_size_cmp */
#include "opt_utils.h"      /* evaluate_compare_condition */
#include "opt_loop_const_sim.h" /* lcs_fold_region shared engine */
#include "log.h"

/* Outer fixed-point pass cap, mirrors the legacy driver
 * tcc_ir_opt_loop_rotation. */
#define SSA_LOOP_ROTATE_MAX_PASSES 4

int ssa_opt_loop_rotate(TCCIRState *ir)
{
  if (!ir || ir->next_instruction_index == 0)
    return 0;

  int total = 0;
  for (int pass = 0; pass < SSA_LOOP_ROTATE_MAX_PASSES; pass++) {
    IRCFG *cfg = tcc_ir_cfg_build(ir);
    if (!cfg || cfg->num_blocks <= 1) {
      tcc_ir_cfg_free(cfg);
      break;
    }
    tcc_ir_cfg_compute_dominators(cfg);

    /* Collect candidate natural loops from dominance-verified back-edges: an
     * edge latch->header where the header dominates the latch.  header_idx is
     * the header block's first instruction (the [CMP,JUMPIF,JUMP] top, if the
     * loop is a rotatable top-tested one); end_idx is the latch block's
     * terminator (the back-edge branch), giving try_rotate_loop the flat upper
     * bound it scans for the back-edge.  Bounded by num_blocks; any back-edges
     * beyond that are picked up on a later pass. */
    int cand_cap = cfg->num_blocks;
    IRLoop *cands = tcc_mallocz(sizeof(IRLoop) * (size_t)cand_cap);
    int ncands = 0;
    for (int b = 0; b < cfg->num_blocks && ncands < cand_cap; b++) {
      IRBasicBlock *bb = &cfg->blocks[b];
      for (int si = 0; si < bb->num_succs && ncands < cand_cap; si++) {
        int h = bb->succs[si];
        if (h < 0 || h >= cfg->num_blocks)
          continue;
        if (!tcc_ir_cfg_dominates(cfg, h, b))
          continue; /* not a back-edge */
        IRLoop *lp = &cands[ncands++];
        lp->header_idx = cfg->blocks[h].start_idx;
        lp->start_idx = cfg->blocks[h].start_idx;
        lp->end_idx = cfg->blocks[b].end_idx;
        lp->preheader_idx = -1;
        lp->body_instrs = NULL;
        lp->num_body_instrs = 0;
        lp->body_instrs_capacity = 0;
        lp->depth = 0;
      }
    }

    /* Detection only needs the CFG; the in-place rewrite works on flat IR and
     * would invalidate this CFG, so drop it before rotating. */
    tcc_ir_cfg_free(cfg);

    /* Rotate inner (smallest) loops before outer ones, matching the legacy
     * driver's ordering. */
    if (ncands > 1)
      qsort(cands, ncands, sizeof(IRLoop), loop_size_cmp);

    int pass_rotated = 0;
    for (int i = 0; i < ncands; i++) {
      /* try_rotate_loop re-derives and re-validates everything from flat IR, so
       * a candidate made stale by an earlier rotation this pass is simply
       * declined (returns 0) rather than mis-rotated. */
      pass_rotated += try_rotate_loop(ir, &cands[i]);
    }
    tcc_free(cands);

    total += pass_rotated;
    if (pass_rotated == 0)
      break;
  }
  return total;
}

/* ============================================================================
 * First-iteration-exit value analysis
 *
 * Moved verbatim from ir/opt_loop_dead.c when the legacy pre-SSA driver
 * (tcc_ir_opt_loop_dead_first_iter) was retired; ssa_opt_first_iter_exit
 * below is now the only consumer.
 *
 * The first-iteration values are computed by a small linear walk from
 * function entry through the loop header up to the exit test, tracking VAR
 * and TEMP constants plus LEA-of-VAR addresses.  The walk bails on any
 * intervening JUMP/JUMPIF so the values genuinely reflect program-entry
 * flow.  Stores through unknown pointers and impure calls invalidate
 * address-taken VARs.
 * ============================================================================ */

#define LD_MAX_VARS 256
#define LD_MAX_TMPS 512

typedef enum {
  LD_UNKNOWN = 0,
  LD_CONST,
  LD_LEA_VAR, /* &VAR — address of a tracked VAR position */
} LdKind;

typedef struct {
  LdKind  kind;
  int64_t value;     /* LD_CONST */
  int     target;    /* LD_LEA_VAR: VAR position */
} LdInfo;

typedef struct {
  LdInfo  var_state[LD_MAX_VARS];
  LdInfo  tmp_state[LD_MAX_TMPS];
  /* address-taken VARs: set when an LEA producing &V is seen, used to
   * invalidate V's tracked value on impure operations (calls, unknown stores). */
  uint8_t var_addrtaken[(LD_MAX_VARS + 7) / 8];
} LdState;

static void ld_clear_all_addrtaken(LdState *st)
{
  for (int v = 0; v < LD_MAX_VARS; v++) {
    if (st->var_addrtaken[v / 8] & (1u << (v % 8)))
      st->var_state[v] = (LdInfo){0};
  }
}

static int ld_decode_vreg(IROperand op, int *out_kind, int *out_pos)
{
  int32_t vr = irop_get_vreg(op);
  if (vr < 0)
    return 0;
  int kind = TCCIR_DECODE_VREG_TYPE(vr);
  int pos  = TCCIR_DECODE_VREG_POSITION(vr);
  if (kind != TCCIR_VREG_TYPE_VAR && kind != TCCIR_VREG_TYPE_TEMP)
    return 0;
  *out_kind = kind;
  *out_pos  = pos;
  return 1;
}

static LdInfo *ld_slot(LdState *st, int kind, int pos)
{
  if (kind == TCCIR_VREG_TYPE_VAR && pos < LD_MAX_VARS)
    return &st->var_state[pos];
  if (kind == TCCIR_VREG_TYPE_TEMP && pos < LD_MAX_TMPS)
    return &st->tmp_state[pos];
  return NULL;
}

/* Resolve an operand's value at the current walk position.
 * Handles:
 *   - immediates
 *   - VAR value (is_lval=1 on STACKOFF tag, or VREG tag with is_lval=1)
 *   - VAR address (is_lval=0 on STACKOFF tag — "&V")
 *   - TEMP value (VREG tag, is_lval=0)
 *   - TEMP deref (VREG tag, is_lval=1 — "*T")
 * Stores result in *out and returns 1 on success, 0 if unknown.
 */
static int ld_resolve(TCCIRState *ir, LdState *st, IROperand op, LdInfo *out)
{
  if (irop_is_immediate(op)) {
    *out = (LdInfo){.kind = LD_CONST, .value = irop_get_imm64_ex(ir, op)};
    return 1;
  }

  int tag = irop_get_tag(op);

  /* STACKOFF tag refers to a stack slot. With is_lval=0 it's the address
   * ("&V"); with is_lval=1 it's the value read from V's slot. The vreg
   * encodes the VAR (or sometimes raw stack-offset without vreg). */
  if (tag == IROP_TAG_STACKOFF) {
    int kind, pos;
    if (!ld_decode_vreg(op, &kind, &pos))
      return 0;
    if (kind != TCCIR_VREG_TYPE_VAR || pos >= LD_MAX_VARS)
      return 0;
    if (!op.is_lval) {
      *out = (LdInfo){.kind = LD_LEA_VAR, .target = pos};
      return 1;
    }
    /* is_lval: read VAR's current value */
    *out = st->var_state[pos];
    return out->kind != LD_UNKNOWN;
  }

  /* VREG-tagged operand. is_lval=1 means "load via this TEMP/VAR address". */
  int kind, pos;
  if (!ld_decode_vreg(op, &kind, &pos))
    return 0;
  LdInfo *slot = ld_slot(st, kind, pos);
  if (!slot)
    return 0;

  if (!op.is_lval) {
    *out = *slot;
    return out->kind != LD_UNKNOWN;
  }

  /* Deref: slot must hold an LEA_VAR to know the load target. */
  if (slot->kind != LD_LEA_VAR || slot->target >= LD_MAX_VARS)
    return 0;
  *out = st->var_state[slot->target];
  return out->kind != LD_UNKNOWN;
}

/* Process one instruction in the linear walk. Returns 1 on success, 0 if
 * we encountered something that breaks the walk and we should bail. */
static int ld_step(TCCIRState *ir, LdState *st, IRQuadCompact *q)
{
  int op = q->op;

  switch (op) {
    case TCCIR_OP_NOP:
      return 1;

    /* Any branch / hard control flow ends the linear-walk's validity. */
    case TCCIR_OP_JUMP:
    case TCCIR_OP_JUMPIF:
    case TCCIR_OP_IJUMP:
    case TCCIR_OP_SWITCH_TABLE:
    case TCCIR_OP_RETURNVOID:
    case TCCIR_OP_RETURNVALUE:
      return 0;

    /* Calls may write to address-taken locals through stored pointers,
     * and have arbitrary side effects. Invalidate all addrtaken VARs and
     * any tracked dest TEMP/VAR. */
    case TCCIR_OP_FUNCCALLVAL:
    case TCCIR_OP_FUNCCALLVOID:
    case TCCIR_OP_VLA_ALLOC:
    case TCCIR_OP_VLA_SP_SAVE:
    case TCCIR_OP_VLA_SP_RESTORE:
    case TCCIR_OP_INLINE_ASM:
    case TCCIR_OP_SETJMP:
    case TCCIR_OP_LONGJMP:
    case TCCIR_OP_NL_SETJMP:
    case TCCIR_OP_NL_LONGJMP:
    case TCCIR_OP_BUILTIN_APPLY:
    case TCCIR_OP_BUILTIN_APPLY_ARGS:
    case TCCIR_OP_BUILTIN_RETURN:
      ld_clear_all_addrtaken(st);
      if (irop_config[op].has_dest) {
        IROperand dest = tcc_ir_op_get_dest(ir, q);
        int kind, pos;
        if (ld_decode_vreg(dest, &kind, &pos)) {
          LdInfo *s = ld_slot(st, kind, pos);
          if (s) *s = (LdInfo){0};
        }
      }
      return 1;

    case TCCIR_OP_LEA: {
      IROperand dest = tcc_ir_op_get_dest(ir, q);
      IROperand src1 = tcc_ir_op_get_src1(ir, q);
      int d_kind, d_pos, s_kind, s_pos;
      if (!ld_decode_vreg(dest, &d_kind, &d_pos))
        return 1;
      LdInfo *dslot = ld_slot(st, d_kind, d_pos);
      if (!dslot)
        return 1;
      if (ld_decode_vreg(src1, &s_kind, &s_pos) &&
          s_kind == TCCIR_VREG_TYPE_VAR && s_pos < LD_MAX_VARS) {
        st->var_addrtaken[s_pos / 8] |= (uint8_t)(1u << (s_pos % 8));
        *dslot = (LdInfo){.kind = LD_LEA_VAR, .target = s_pos};
      } else {
        *dslot = (LdInfo){0};
      }
      return 1;
    }

    case TCCIR_OP_ASSIGN: {
      IROperand dest = tcc_ir_op_get_dest(ir, q);
      IROperand src1 = tcc_ir_op_get_src1(ir, q);
      int d_kind, d_pos;
      if (!ld_decode_vreg(dest, &d_kind, &d_pos))
        return 1;
      /* TEMP destination with is_lval=1 would be a store-through-pointer.
       * ASSIGN normally doesn't use that form (STORE does), but be safe. */
      if (d_kind == TCCIR_VREG_TYPE_TEMP && dest.is_lval) {
        ld_clear_all_addrtaken(st);
        return 1;
      }
      LdInfo *dslot = ld_slot(st, d_kind, d_pos);
      if (!dslot)
        return 1;
      LdInfo v;
      if (ld_resolve(ir, st, src1, &v))
        *dslot = v;
      else
        *dslot = (LdInfo){0};
      return 1;
    }

    case TCCIR_OP_LOAD: {
      IROperand dest = tcc_ir_op_get_dest(ir, q);
      IROperand src1 = tcc_ir_op_get_src1(ir, q);
      int d_kind, d_pos;
      if (!ld_decode_vreg(dest, &d_kind, &d_pos))
        return 1;
      LdInfo *dslot = ld_slot(st, d_kind, d_pos);
      if (!dslot)
        return 1;
      LdInfo v;
      if (ld_resolve(ir, st, src1, &v))
        *dslot = v;
      else
        *dslot = (LdInfo){0};
      return 1;
    }

    case TCCIR_OP_STORE: {
      IROperand dest = tcc_ir_op_get_dest(ir, q);
      IROperand src1 = tcc_ir_op_get_src1(ir, q);

      int d_tag = irop_get_tag(dest);

      /* STORE to a stack-slot via STACKOFF dest: V.is_lval=1 means
       * "store into V's slot". */
      if (d_tag == IROP_TAG_STACKOFF && dest.is_lval) {
        int d_kind, d_pos;
        if (ld_decode_vreg(dest, &d_kind, &d_pos) &&
            d_kind == TCCIR_VREG_TYPE_VAR && d_pos < LD_MAX_VARS) {
          LdInfo v;
          if (ld_resolve(ir, st, src1, &v))
            st->var_state[d_pos] = v;
          else
            st->var_state[d_pos] = (LdInfo){0};
          return 1;
        }
      }

      /* STORE through a TEMP pointer: *T = src.  If T is a known LEA(&V),
       * update V; otherwise invalidate all addrtaken VARs. */
      if (d_tag == IROP_TAG_VREG && dest.is_lval) {
        int d_kind, d_pos;
        if (ld_decode_vreg(dest, &d_kind, &d_pos)) {
          LdInfo *aslot = ld_slot(st, d_kind, d_pos);
          if (aslot && aslot->kind == LD_LEA_VAR && aslot->target < LD_MAX_VARS) {
            LdInfo v;
            if (ld_resolve(ir, st, src1, &v))
              st->var_state[aslot->target] = v;
            else
              st->var_state[aslot->target] = (LdInfo){0};
            return 1;
          }
        }
      }

      /* Unknown store: pessimize all addrtaken VARs. */
      ld_clear_all_addrtaken(st);
      return 1;
    }

    case TCCIR_OP_TEST_ZERO:
    case TCCIR_OP_CMP:
      /* Test-only ops produce no value; ignore. */
      return 1;

    default: {
      /* Generic op: if it has a dest TEMP/VAR, mark it unknown. */
      if (irop_config[op].has_dest) {
        IROperand dest = tcc_ir_op_get_dest(ir, q);
        if (dest.is_lval) {
          /* Some "store-like" form we don't model; pessimize. */
          ld_clear_all_addrtaken(st);
        } else {
          int d_kind, d_pos;
          if (ld_decode_vreg(dest, &d_kind, &d_pos)) {
            LdInfo *dslot = ld_slot(st, d_kind, d_pos);
            if (dslot) *dslot = (LdInfo){0};
          }
        }
      }
      return 1;
    }
  }
}

/* JUMPIF condition tokens (from arm-thumb tokens; matches branch-fold). */
#define LD_TOK_EQ 0x94
#define LD_TOK_NE 0x95

/* Evaluate the static result of the exit branch using the walked state.
 * Returns 1 if proven taken, 0 if proven not-taken, -1 if unknown. */
static int ld_eval_branch(TCCIRState *ir, LdState *st, int test_idx,
                          int jumpif_idx)
{
  IRQuadCompact *test_q = &ir->compact_instructions[test_idx];
  IRQuadCompact *jump_q = &ir->compact_instructions[jumpif_idx];
  IROperand jcond = tcc_ir_op_get_src1(ir, jump_q);
  int tok = (int)irop_get_imm64_ex(ir, jcond);

  if (test_q->op == TCCIR_OP_TEST_ZERO) {
    IROperand src1 = tcc_ir_op_get_src1(ir, test_q);
    LdInfo v;
    if (!ld_resolve(ir, st, src1, &v) || v.kind != LD_CONST)
      return -1;
    if (tok == LD_TOK_EQ) return v.value == 0;
    if (tok == LD_TOK_NE) return v.value != 0;
    return -1;
  }

  if (test_q->op == TCCIR_OP_CMP) {
    IROperand s1 = tcc_ir_op_get_src1(ir, test_q);
    IROperand s2 = tcc_ir_op_get_src2(ir, test_q);
    LdInfo a, b;
    if (!ld_resolve(ir, st, s1, &a) || a.kind != LD_CONST)
      return -1;
    if (!ld_resolve(ir, st, s2, &b) || b.kind != LD_CONST)
      return -1;
    /* Reuse the engine's comparator. */
    int r = evaluate_compare_condition(a.value, b.value, tok);
    if (r < 0) return -1;
    return r;
  }

  return -1;
}

/* Walk linearly through [0..stop_at-1], updating the state. Bails (returns 0)
 * on the first JUMP/JUMPIF encountered before stop_at, which indicates the
 * function entry path is not straight-line into the loop. */
static int ld_walk_linear_to(TCCIRState *ir, LdState *st, int stop_at)
{
  for (int i = 0; i < stop_at; i++) {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (!ld_step(ir, st, q))
      return 0;
  }
  return 1;
}

/* First-iteration proof: walk linearly from function entry to jumpif_idx and
 * evaluate the TEST_ZERO/CMP at test_idx against the walked constants.
 * Returns 1 only when the exit branch is provably TAKEN on first entry; 0 on
 * a non-straight-line entry path (any branch before jumpif_idx) or an
 * unknown/not-taken outcome. */
static int ld_first_iter_prove(TCCIRState *ir, int test_idx, int jumpif_idx)
{
  if (!ir || test_idx < 0 || jumpif_idx <= test_idx ||
      jumpif_idx >= ir->next_instruction_index)
    return 0;

  /* LdState is ~18 KB (var_state[256] + tmp_state[512], 24 B each).  A stack
   * local here overflows the 32 KB target process stack when this loop pass is
   * reached deep in the gen_function call chain (STKOF on -O1 compiles of
   * functions containing loops).  Heap-allocate it. */
  LdState *st = tcc_malloc(sizeof(*st));
  memset(st, 0, sizeof(*st));
  int ok = ld_walk_linear_to(ir, st, jumpif_idx);
  int taken = ok ? ld_eval_branch(ir, st, test_idx, jumpif_idx) : -1;
  tcc_free(st);

  if (!ok) {
    LOG_LOOP_OPT("first_iter_exit: bail (non-straight-line path before jumpif)");
    return 0;
  }
  if (taken != 1) {
    LOG_LOOP_OPT("first_iter_exit: branch outcome=%d (need 1=taken)", taken);
    return 0;
  }
  return 1;
}

/* NOP any unconditional JUMP whose target is its own next non-NOP successor.
 * After loop elimination + compact_nops, the redirect JUMP often becomes a
 * no-op that nevertheless leaves an `is_jump_target` flag on its target,
 * which is_jump_target-sensitive analyses (e.g. stack_addr_nonnull_fold)
 * use as a tracking-reset signal — needlessly losing precision. */
static void ld_nop_fallthrough_jumps(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  for (int i = 0; i < n; i++) {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_JUMP)
      continue;
    IROperand dest = tcc_ir_op_get_dest(ir, q);
    int target = (int)irop_get_imm64_ex(ir, dest);
    int next_live = i + 1;
    while (next_live < n && ir->compact_instructions[next_live].op == TCCIR_OP_NOP)
      next_live++;
    if (target == next_live)
      q->op = TCCIR_OP_NOP;
  }
}

/*
 * ssa_opt_first_iter_exit() — SSA/CFG-era replacement for the legacy pre-SSA
 * first-iteration-exit peeling pass (tcc_ir_opt_loop_dead_first_iter,
 * formerly ir/opt_loop_dead.c — retired).
 * See docs/plan_legacy_loop_dead_first_iter_ssa.md.
 *
 * When a top-tested natural loop's header exit test (TEST_ZERO/CMP + JUMPIF
 * to a target outside the loop) is statically TRUE on first entry — proven by
 * the shared straight-line value walk ld_first_iter_prove() over VAR/TEMP
 * constants and LEA(&VAR) addresses — the loop never executes: the JUMPIF is
 * rewritten into an unconditional JUMP to the exit target and the loop's
 * member blocks are NOPed.
 *
 * Improvements over the legacy driver (the analysis + mutation semantics are
 * shared/unchanged):
 *   - candidates come from dominance-verified back-edges on a real CFG, not
 *     the flat range-scan detector (tcc_ir_detect_loops);
 *   - explicit single-entry guard: the header's predecessors must be exactly
 *     {function-entry block, latch}, and no other member block may have a
 *     predecessor outside the loop.  Requiring the non-latch predecessor to
 *     be the FUNCTION-ENTRY block also closes a legacy soundness gap: a jump
 *     target between a walked def and the header would let control re-enter
 *     the (rewritten) header later with values the linear walk never modeled;
 *   - only natural-loop MEMBER blocks are NOPed, so non-loop code interleaved
 *     inside the flat [header, latch] index range survives (flat-range NOPing
 *     is the documented failure class of the legacy loop passes).
 *
 * Idempotent: after firing, the loop's back-edge is NOPed, so it yields no
 * further candidate.  Known accepted difference from the legacy call site:
 * its cleanup cascade (const_prop + stack_addr_nonnull_fold + dead-alloca
 * rounds in tccgen.c) is not reproduced here; the downstream SSA pipeline
 * does the cleanup it can, and the residual post-loop code of the
 * 20070824-1.c shape (unreachable abort path, dead alloca) is accepted
 * until an SSA-level cleanup is warranted.
 */

/* Outer fixed-point cap.  One elimination per CFG build (the rewrite makes
 * the CFG stale); an eliminated inner loop can expose an outer candidate on
 * the next pass, mirroring the legacy smallest-first iteration. */
#define SSA_FIRST_ITER_EXIT_MAX_PASSES 4

/* The header must test-and-exit within a handful of instructions of its
 * start (mirrors the legacy ld_find_exit_branch lookahead). */
#define FIE_EXIT_TEST_LOOKAHEAD 6

typedef struct {
  int header_b;
  int latch_b;
  int size; /* flat instruction span, for smallest-first ordering */
} FieCand;

static int fie_cand_cmp(const void *a, const void *b)
{
  const FieCand *ca = a, *cb = b;
  if (ca->size != cb->size)
    return ca->size - cb->size;
  return ca->header_b - cb->header_b;
}

/* Natural-loop membership of back-edge latch->header: the header plus every
 * block backward-reachable from the latch without passing through the header.
 * Since the header dominates the latch, the walk cannot escape past it. */
static void fie_collect_members(IRCFG *cfg, int header_b, int latch_b,
                                uint8_t *member)
{
  memset(member, 0, (size_t)cfg->num_blocks);
  member[header_b] = 1;
  if (latch_b == header_b)
    return;

  int *stack = tcc_malloc(sizeof(int) * (size_t)cfg->num_blocks);
  int sp = 0;
  member[latch_b] = 1;
  stack[sp++] = latch_b;
  while (sp > 0) {
    IRBasicBlock *bb = &cfg->blocks[stack[--sp]];
    for (int i = 0; i < bb->num_preds; i++) {
      int p = bb->preds[i];
      if (p >= 0 && p < cfg->num_blocks && !member[p]) {
        member[p] = 1;
        stack[sp++] = p;
      }
    }
  }
  tcc_free(stack);
}

/* Validate one back-edge candidate and, if the exit is proven taken on first
 * entry, rewrite it.  Returns 1 if the loop was eliminated.  `member` is a
 * caller-provided num_blocks-sized scratch buffer. */
static int fie_try_candidate(TCCIRState *ir, IRCFG *cfg, int header_b,
                             int latch_b, uint8_t *member)
{
  IRBasicBlock *hb = &cfg->blocks[header_b];

  fie_collect_members(cfg, header_b, latch_b, member);

  /* Single-entry guard: header preds must be exactly {entry block, latch}.
   * The non-latch predecessor must be the function-entry block (start_idx 0)
   * so no label sits between the walked entry code and the header. */
  if (hb->num_preds != 2)
    return 0;
  int latch_seen = 0, entry_pred = -1;
  for (int i = 0; i < hb->num_preds; i++) {
    int p = hb->preds[i];
    if (p == latch_b && !latch_seen)
      latch_seen = 1;
    else
      entry_pred = p;
  }
  if (!latch_seen || entry_pred < 0 || member[entry_pred])
    return 0;
  if (cfg->blocks[entry_pred].start_idx != 0)
    return 0;

  /* No other member block may be entered from outside the loop. */
  for (int b = 0; b < cfg->num_blocks; b++) {
    if (!member[b] || b == header_b)
      continue;
    IRBasicBlock *bb = &cfg->blocks[b];
    for (int i = 0; i < bb->num_preds; i++)
      if (!member[bb->preds[i]])
        return 0;
  }

  /* Locate the exit branch in the header block: TEST_ZERO/CMP followed by a
   * JUMPIF whose target block lies outside the loop membership (the CFG
   * replacement for the legacy flat [start_idx, end_idx] range test). */
  int test_idx = -1, jumpif_idx = -1;
  for (int i = hb->start_idx;
       i < hb->end_idx && i <= hb->start_idx + FIE_EXIT_TEST_LOOKAHEAD; i++) {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;
    if (q->op == TCCIR_OP_TEST_ZERO || q->op == TCCIR_OP_CMP) {
      test_idx = i;
      continue;
    }
    if (q->op == TCCIR_OP_JUMPIF) {
      if (test_idx < 0)
        return 0;
      IROperand dest = tcc_ir_op_get_dest(ir, q);
      int target = (int)irop_get_imm64_ex(ir, dest);
      if (target < 0 || target >= cfg->num_instrs)
        return 0;
      if (member[cfg->instr_to_block[target]])
        return 0; /* not an exit branch */
      jumpif_idx = i;
      break;
    }
    /* Other instruction kinds before the exit branch are allowed; they
     * participate in the linear value walk. */
  }
  if (jumpif_idx < 0)
    return 0;

  /* Shared analysis: straight-line walk from function entry + branch fold.
   * Bails on any intervening branch, keeping the legacy conservatism. */
  if (!ld_first_iter_prove(ir, test_idx, jumpif_idx))
    return 0;

  /* Rewrite JUMPIF to unconditional JUMP (dest already holds the exit
   * target), then NOP the loop's member blocks — the body never runs, and
   * the header's pre-test defs are dead with the test itself. */
  IRQuadCompact *jump_q = &ir->compact_instructions[jumpif_idx];
  IROperand exit_dest = tcc_ir_op_get_dest(ir, jump_q);
  jump_q->op = TCCIR_OP_JUMP;
  tcc_ir_set_dest(ir, jumpif_idx, exit_dest);

  for (int b = 0; b < cfg->num_blocks; b++) {
    if (!member[b])
      continue;
    for (int i = cfg->blocks[b].start_idx; i < cfg->blocks[b].end_idx; i++) {
      if (i == jumpif_idx)
        continue;
      ir->compact_instructions[i].op = TCCIR_OP_NOP;
    }
  }
  return 1;
}

int ssa_opt_first_iter_exit(TCCIRState *ir)
{
  if (!ir || ir->next_instruction_index == 0)
    return 0;

  int total = 0;
  for (int pass = 0; pass < SSA_FIRST_ITER_EXIT_MAX_PASSES; pass++) {
    IRCFG *cfg = tcc_ir_cfg_build(ir);
    if (!cfg || cfg->num_blocks <= 1) {
      tcc_ir_cfg_free(cfg);
      break;
    }
    tcc_ir_cfg_compute_dominators(cfg);

    /* Dominance-verified back-edges latch->header, tried smallest-first
     * (inner loops before outer, matching the legacy loop_size_cmp order).
     * Bounded by num_blocks, as in ssa_opt_loop_rotate. */
    int cap = cfg->num_blocks;
    FieCand *cands = tcc_mallocz(sizeof(FieCand) * (size_t)cap);
    int nc = 0;
    for (int b = 0; b < cfg->num_blocks && nc < cap; b++) {
      IRBasicBlock *bb = &cfg->blocks[b];
      for (int si = 0; si < bb->num_succs && nc < cap; si++) {
        int h = bb->succs[si];
        if (h < 0 || h >= cfg->num_blocks)
          continue;
        if (!tcc_ir_cfg_dominates(cfg, h, b))
          continue;
        cands[nc].header_b = h;
        cands[nc].latch_b = b;
        cands[nc].size = cfg->blocks[b].end_idx - cfg->blocks[h].start_idx;
        nc++;
      }
    }

    /* Stop at the first elimination: the rewrite invalidates this CFG, and
     * the entry walk of any later candidate would cross the rewritten JUMP
     * anyway.  The outer fixed point rebuilds and retries. */
    int fired = 0;
    if (nc > 0) {
      qsort(cands, nc, sizeof(FieCand), fie_cand_cmp);
      uint8_t *member = tcc_malloc((size_t)cfg->num_blocks);
      for (int i = 0; i < nc && !fired; i++)
        fired = fie_try_candidate(ir, cfg, cands[i].header_b,
                                  cands[i].latch_b, member);
      tcc_free(member);
    }
    tcc_free(cands);
    tcc_ir_cfg_free(cfg);

    if (!fired)
      break;
    total++;
    /* JUMP-to-next-live hygiene after an elimination, as the legacy driver
     * did. */
    ld_nop_fallthrough_jumps(ir);
  }
  return total;
}

/* ============================================================================
 * Pointer-IV Exit-Value Substitution (ssa:ptr_iv_exit_subst)
 *
 * For a counted loop with known trip count N > 0, a pointer IV initialized to
 * `Addr[StackLoc[X]]` with a unique in-loop `V = V + step` (or copy-through)
 * has exit value `Addr[StackLoc[X + step*N]]`; post-loop value-reads of V are
 * rewritten to it and the consuming CMP+JUMPIF is folded, killing pr49644-style
 * `if (p != &a[N]) abort();` checks.  Replaced the legacy pre-SSA driver
 * (ir/opt_loop.c); see docs/plan_legacy_loop_ptr_iv_exit_subst_ssa.md.
 * ============================================================================ */

typedef struct PtrIV
{
  int32_t vreg;       /* the pointer VAR */
  int32_t init_off;   /* StackLoc offset at preheader */
  int     step;       /* increment per iteration */
  int     init_idx;   /* index of the init ASSIGN */
  int     def_idx;    /* index of the in-loop self-add */
  int     is_llocal;  /* preserve llocal flag from init */
  int     is_param;   /* preserve param flag from init */
  int     btype;
} PtrIV;

#define PTRIV_MAX 8

/* Match a pointer-IV self-add at instr_idx: `V = V + #step` or copy-through
 * `T = V; V = T + #step`. */
static int ptr_iv_find_loop_step(TCCIRState *ir, IRLoop *loop, int instr_idx,
                                 int32_t *out_vreg, int *out_step)
{
  IRQuadCompact *q = &ir->compact_instructions[instr_idx];
  if (q->op != TCCIR_OP_ADD)
    return 0;
  IROperand dest = tcc_ir_op_get_dest(ir, q);
  IROperand src1 = tcc_ir_op_get_src1(ir, q);
  IROperand src2 = tcc_ir_op_get_src2(ir, q);
  int32_t d_vr = irop_get_vreg(dest);
  int32_t s1_vr = irop_get_vreg(src1);
  if (d_vr < 0 || TCCIR_DECODE_VREG_TYPE(d_vr) != TCCIR_VREG_TYPE_VAR)
    return 0;
  if (!irop_is_immediate(src2))
    return 0;
  int step = (int)irop_get_imm64_ex(ir, src2);
  if (step == 0)
    return 0;

  /* Direct pattern: dest_vr == src1_vr */
  if (s1_vr == d_vr) {
    *out_vreg = d_vr;
    *out_step = step;
    return 1;
  }

  /* Copy-through: scan back a few NOP-skipped instructions for `T = V`. */
  for (int k = instr_idx - 1; k >= loop->start_idx && k >= instr_idx - 3; k--) {
    IRQuadCompact *aq = &ir->compact_instructions[k];
    if (aq->op == TCCIR_OP_NOP)
      continue;
    if (aq->op != TCCIR_OP_ASSIGN)
      return 0;
    IROperand adest = tcc_ir_op_get_dest(ir, aq);
    IROperand asrc = tcc_ir_op_get_src1(ir, aq);
    if (irop_get_vreg(adest) == s1_vr && irop_get_vreg(asrc) == d_vr) {
      *out_vreg = d_vr;
      *out_step = step;
      return 1;
    }
    return 0;
  }
  return 0;
}

/* Walk back from preheader_idx looking for an unconditional def of `vreg`
 * of the form `vreg <- Addr[StackLoc[X]]`.  Returns 1 on success and writes
 * the offset/flags/init index. */
static int ptr_iv_find_init(TCCIRState *ir, int vreg, int preheader_idx,
                            int *out_off, int *out_is_llocal, int *out_is_param,
                            int *out_init_idx, int *out_btype)
{
  for (int j = preheader_idx; j >= 0; j--) {
    IRQuadCompact *q = &ir->compact_instructions[j];
    if (q->op == TCCIR_OP_NOP)
      continue;
    /* Stop at any jump target / merge point before the preheader. */
    if (j < preheader_idx && q->is_jump_target)
      return 0;
    /* Stop at any other def of vreg (we want the most recent). */
    if (!irop_config[q->op].has_dest)
      continue;
    IROperand dest = tcc_ir_op_get_dest(ir, q);
    if (irop_get_vreg(dest) != vreg)
      continue;
    /* STOREs through a vreg-deref do not redefine vreg itself. */
    if (q->op == TCCIR_OP_STORE && dest.is_lval && !dest.is_local)
      continue;
    if (q->op != TCCIR_OP_ASSIGN)
      return 0;
    IROperand src1 = tcc_ir_op_get_src1(ir, q);
    if (irop_get_tag(src1) != IROP_TAG_STACKOFF || src1.is_lval || !src1.is_local)
      return 0;
    *out_off = (int)irop_get_imm64_ex(ir, src1);
    *out_is_llocal = src1.is_llocal;
    *out_is_param = src1.is_param;
    *out_init_idx = j;
    *out_btype = irop_get_btype(src1);
    return 1;
  }
  return 0;
}

/* Verify vreg has exactly one def in [loop.start..loop.end] (the self-add at
 * def_idx) and no other write that could perturb its value. */
static int ptr_iv_unique_loop_def(TCCIRState *ir, IRLoop *loop, int vreg, int def_idx)
{
  for (int j = loop->start_idx; j <= loop->end_idx; j++) {
    IRQuadCompact *q = &ir->compact_instructions[j];
    if (q->op == TCCIR_OP_NOP)
      continue;
    if (!irop_config[q->op].has_dest)
      continue;
    IROperand dest = tcc_ir_op_get_dest(ir, q);
    if (irop_get_vreg(dest) != vreg)
      continue;
    /* STORE through V's deref doesn't redefine V. */
    if (q->op == TCCIR_OP_STORE && dest.is_lval && !dest.is_local)
      continue;
    if (j != def_idx)
      return 0;
  }
  return 1;
}

/* Replace VAR value-reads of `vreg` with `repl` at `idx`; returns 0..2.
 * Only the STACKOFF+is_lval form is a value-read — VREG-tag means deref. */
static int ptr_iv_subst_uses_in_instr(TCCIRState *ir, int idx, int vreg, IROperand repl)
{
  IRQuadCompact *q = &ir->compact_instructions[idx];
  int subs = 0;
  if (irop_config[q->op].has_src1) {
    IROperand s = tcc_ir_op_get_src1(ir, q);
    if (irop_get_vreg(s) == vreg && irop_get_tag(s) == IROP_TAG_STACKOFF) {
      tcc_ir_op_set_src1(ir, q, repl);
      subs++;
    }
  }
  if (irop_config[q->op].has_src2) {
    IROperand s = tcc_ir_op_get_src2(ir, q);
    if (irop_get_vreg(s) == vreg && irop_get_tag(s) == IROP_TAG_STACKOFF) {
      tcc_ir_op_set_src2(ir, q, repl);
      subs++;
    }
  }
  return subs;
}

/* Resolve a CMP operand to a constant frame offset, chasing LEA/ASSIGN/ADD±imm
 * reaching defs only within [exit_target, at) — merge-free by the walk's
 * retirement rules; defs below exit_target may be loop-carried.  LEA chasing
 * is what the flat cmp_stack_addr_fold lacks, which kept the legacy cascade
 * from ever folding the `p != &a[N]` compare. */
static int piv_resolve_frame_addr(TCCIRState *ir, IROperand op, int exit_target,
                                  int at, int depth,
                                  int32_t *out_off, int *out_is_param)
{
  if (depth > 8)
    return 0;

  /* Direct stack address: Addr[StackLoc[K]] (no vreg, not lval). */
  if (irop_get_tag(op) == IROP_TAG_STACKOFF && irop_get_vreg(op) == -1 &&
      !op.is_lval && op.is_local) {
    *out_off = (int32_t)irop_get_imm64_ex(ir, op);
    *out_is_param = op.is_param;
    return 1;
  }

  int32_t vr = irop_get_vreg(op);
  if (vr < 0 || op.is_lval)
    return 0;

  for (int j = at - 1; j >= exit_target; j--) {
    IRQuadCompact *q = &ir->compact_instructions[j];
    if (q->op == TCCIR_OP_NOP || !irop_config[q->op].has_dest)
      continue;
    IROperand dest = tcc_ir_op_get_dest(ir, q);
    if (irop_get_vreg(dest) != vr)
      continue;
    /* STOREs through vr's deref are uses of vr, not defs. */
    if (q->op == TCCIR_OP_STORE && dest.is_lval && !dest.is_local)
      continue;
    if (dest.is_lval)
      return 0; /* memory-form write — not a modeled vreg def */
    if (q->op == TCCIR_OP_LEA) {
      IROperand src = tcc_ir_op_get_src1(ir, q);
      if (irop_get_tag(src) != IROP_TAG_STACKOFF || irop_get_vreg(src) != -1 ||
          src.is_lval || !src.is_local)
        return 0;
      *out_off = (int32_t)irop_get_imm64_ex(ir, src);
      *out_is_param = src.is_param;
      return 1;
    }
    if (q->op == TCCIR_OP_ASSIGN)
      return piv_resolve_frame_addr(ir, tcc_ir_op_get_src1(ir, q), exit_target,
                                    j, depth + 1, out_off, out_is_param);
    if (q->op == TCCIR_OP_ADD || q->op == TCCIR_OP_SUB) {
      IROperand s2 = tcc_ir_op_get_src2(ir, q);
      int32_t base_off;
      int base_is_param;
      if (!irop_is_immediate(s2))
        return 0;
      if (!piv_resolve_frame_addr(ir, tcc_ir_op_get_src1(ir, q), exit_target,
                                  j, depth + 1, &base_off, &base_is_param))
        return 0;
      int64_t c = irop_get_imm64_ex(ir, s2);
      if (q->op == TCCIR_OP_SUB)
        c = -c;
      c += base_off;
      if (c != (int32_t)c)
        return 0;
      *out_off = (int32_t)c;
      *out_is_param = base_is_param;
      return 1;
    }
    return 0; /* some other op defines vr — give up */
  }
  return 0;
}

/* Fold a just-substituted CMP+JUMPIF whose operands prove EQUAL frame
 * addresses (proven-unequal is left alone); sets *out_cfg_changed on the
 * JUMPIF rewrite, which invalidates any CFG. */
static int piv_fold_substituted_cmp(TCCIRState *ir, int cmp_idx, int exit_target,
                                    int *out_cfg_changed)
{
  IRQuadCompact *q = &ir->compact_instructions[cmp_idx];
  if (cmp_idx + 1 >= ir->next_instruction_index)
    return 0;
  IRQuadCompact *next = &ir->compact_instructions[cmp_idx + 1];
  if (next->op != TCCIR_OP_JUMPIF)
    return 0;

  int32_t off1, off2;
  int p1, p2;
  if (!piv_resolve_frame_addr(ir, tcc_ir_op_get_src1(ir, q), exit_target,
                              cmp_idx, 0, &off1, &p1))
    return 0;
  if (!piv_resolve_frame_addr(ir, tcc_ir_op_get_src2(ir, q), exit_target,
                              cmp_idx, 0, &off2, &p2))
    return 0;
  if (off1 != off2 || p1 != p2)
    return 0; /* could fold to !equal too, but be conservative */

  IROperand cond = tcc_ir_op_get_src1(ir, next);
  int tok = (int)irop_get_imm64_ex(ir, cond);
  int result = evaluate_compare_condition(0, 0, tok); /* equal-equal */
  if (result < 0)
    return 0;
  IROperand jmp_dest = tcc_ir_op_get_dest(ir, next);
  LOG_LOOP_OPT("ptr_iv_exit_subst: CMP fold at %d (off=%d, %s)",
               cmp_idx, off1, result ? "taken" : "not taken");
  q->op = TCCIR_OP_NOP;
  if (result) {
    next->op = TCCIR_OP_JUMP;
    tcc_ir_set_dest(ir, cmp_idx + 1, jmp_dest);
  } else {
    next->op = TCCIR_OP_NOP;
  }
  *out_cfg_changed = 1;
  return 1;
}

/* Per-loop core (extracted from the retired legacy driver): counter-IV
 * anchor, pointer-IV collection, entry-guard NOP, substitution walk.
 * `step_ok` must confirm a matched IV self-add runs once per iteration.
 * Returns substitutions; *out_cfg_changes reports guard NOPs + folded
 * JUMPIFs (CFG-invalidating). */
typedef int (*ptr_iv_instr_check_fn)(void *ctx, int instr_idx);
static int ptr_iv_exit_subst_loop(TCCIRState *ir, IRLoop *loop,
                                  ptr_iv_instr_check_fn step_ok, void *step_ok_ctx,
                                  int *out_cfg_changes)
{
  if (out_cfg_changes)
    *out_cfg_changes = 0;
  if (loop->start_idx < 0)
    return 0;

  /* Need a counter IV with known trip count to anchor exit-value computation. */
  InductionVar ivs[MAX_IV];
  int num_ivs = find_induction_vars_ex(ir, loop, ivs, MAX_IV, 1);
  int cmp_idx, jmpif_idx, limit, cond, exit_target;
  InductionVar *primary = NULL;
  for (int k = 0; k < num_ivs; k++) {
    if (step_ok && !step_ok(step_ok_ctx, ivs[k].def_idx))
      continue;
    if (find_loop_exit_condition(ir, loop, ivs[k].vreg, &cmp_idx, &jmpif_idx,
                                 &limit, &cond, &exit_target)) {
      primary = &ivs[k];
      break;
    }
  }
  if (!primary)
    return 0;
  int trip_count = compute_trip_count(primary->init_val, limit, primary->step, cond);
  if (trip_count <= 0)
    return 0;

  /* Scan loop body for pointer IVs. */
  PtrIV pivs[PTRIV_MAX];
  int n_pivs = 0;
  for (int j = loop->start_idx; j <= loop->end_idx && n_pivs < PTRIV_MAX; j++) {
    int32_t v_vr;
    int v_step;
    if (!ptr_iv_find_loop_step(ir, loop, j, &v_vr, &v_step))
      continue;
    /* Skip the counter IV — it's an integer IV, not a pointer one. */
    int is_counter = 0;
    for (int k = 0; k < num_ivs; k++) {
      if (ivs[k].vreg == v_vr) { is_counter = 1; break; }
    }
    if (is_counter)
      continue;

    /* The self-add must execute exactly once per iteration. */
    if (step_ok && !step_ok(step_ok_ctx, j))
      continue;

    /* Must have exactly one def in the loop (the self-add at j). */
    if (!ptr_iv_unique_loop_def(ir, loop, v_vr, j))
      continue;

    /* Find preheader init `V = Addr[StackLoc[off]]`. */
    int init_off, is_llocal, is_param, init_idx, btype;
    if (!ptr_iv_find_init(ir, v_vr, loop->preheader_idx,
                          &init_off, &is_llocal, &is_param, &init_idx, &btype))
      continue;

    /* Stack offsets are 32-bit; reject anything that won't fit. */
    int64_t final_off64 = (int64_t)init_off + (int64_t)v_step * (int64_t)trip_count;
    if (final_off64 != (int32_t)final_off64)
      continue;

    pivs[n_pivs].vreg      = v_vr;
    pivs[n_pivs].init_off  = init_off;
    pivs[n_pivs].step      = v_step;
    pivs[n_pivs].init_idx  = init_idx;
    pivs[n_pivs].def_idx   = j;
    pivs[n_pivs].is_llocal = is_llocal;
    pivs[n_pivs].is_param  = is_param;
    pivs[n_pivs].btype     = btype;
    n_pivs++;
  }

  if (n_pivs == 0)
    return 0;

  /* Trip > 0 proves a rotated loop's zero-trip guard (CMP+JUMPIF between the
   * counter init and the loop) never fires: NOP it so its stale
   * is_jump_target doesn't block the walk below. */
  for (int g = primary->init_idx + 1; g < loop->start_idx; g++) {
    IRQuadCompact *gq = &ir->compact_instructions[g];
    if (gq->op != TCCIR_OP_CMP)
      continue;
    IROperand gs1 = tcc_ir_op_get_src1(ir, gq);
    if (irop_get_vreg(gs1) != primary->vreg)
      continue;
    if (g + 1 >= loop->start_idx)
      break;
    IRQuadCompact *gjq = &ir->compact_instructions[g + 1];
    if (gjq->op != TCCIR_OP_JUMPIF)
      continue;
    IROperand gjd = tcc_ir_op_get_dest(ir, gjq);
    int gjt = (int)irop_get_imm64_ex(ir, gjd);
    if (gjt < loop->end_idx)
      continue; /* not the entry-guard shape (target must be past the loop) */
    gq->op = TCCIR_OP_NOP;
    gjq->op = TCCIR_OP_NOP;
    if (out_cfg_changes)
      (*out_cfg_changes)++;
    /* Clear the target's is_jump_target only if the guard was its sole in-edge. */
    if (gjt >= 0 && gjt < ir->next_instruction_index) {
      int has_other_in_edge = 0;
      for (int s = 0; s < ir->next_instruction_index && !has_other_in_edge; s++) {
        if (s == g + 1) continue;
        IRQuadCompact *sq = &ir->compact_instructions[s];
        if (sq->op != TCCIR_OP_JUMP && sq->op != TCCIR_OP_JUMPIF)
          continue;
        IROperand sd = tcc_ir_op_get_dest(ir, sq);
        int st = (int)irop_get_imm64_ex(ir, sd);
        if (st == gjt)
          has_other_in_edge = 1;
      }
      if (!has_other_in_edge)
        ir->compact_instructions[gjt].is_jump_target = 0;
    }
  }

  /* Walk forward from exit_target substituting value-reads of each live V;
   * retire V on redef, and ALL IVs at a backward jump or any is_jump_target
   * after exit_target (a merge could carry a different V). */
  int live[PTRIV_MAX];
  for (int p = 0; p < n_pivs; p++) live[p] = 1;

  int total = 0;
  int n = ir->next_instruction_index;
  for (int j = exit_target; j < n; j++) {
    IRQuadCompact *q = &ir->compact_instructions[j];
    if (q->op == TCCIR_OP_NOP)
      continue;

    if (j > exit_target && q->is_jump_target) {
      for (int p = 0; p < n_pivs; p++) live[p] = 0;
    }

    int any_live = 0;
    for (int p = 0; p < n_pivs; p++) if (live[p]) { any_live = 1; break; }
    if (!any_live)
      break;

    /* Substitute uses first (reads), then check for redef. */
    int subs_here = 0;
    for (int p = 0; p < n_pivs; p++) {
      if (!live[p]) continue;
      int32_t final_off = (int32_t)((int64_t)pivs[p].init_off +
                                    (int64_t)pivs[p].step * (int64_t)trip_count);
      IROperand repl = irop_make_stackoff(-1, final_off, /*is_lval*/ 0,
                                          pivs[p].is_llocal, pivs[p].is_param,
                                          pivs[p].btype);
      subs_here += ptr_iv_subst_uses_in_instr(ir, j, pivs[p].vreg, repl);
    }
    total += subs_here;

    /* Fold the consuming CMP+JUMPIF when both sides now prove equal. */
    if (subs_here > 0 && q->op == TCCIR_OP_CMP) {
      int folded_cfg = 0;
      piv_fold_substituted_cmp(ir, j, exit_target, &folded_cfg);
      if (folded_cfg && out_cfg_changes)
        (*out_cfg_changes)++;
    }

    /* Retire on redef (STORE through a vreg-deref doesn't redefine it). */
    if (irop_config[q->op].has_dest) {
      IROperand dest = tcc_ir_op_get_dest(ir, q);
      if (!(q->op == TCCIR_OP_STORE && dest.is_lval && !dest.is_local)) {
        int32_t dvr = irop_get_vreg(dest);
        if (dvr >= 0) {
          for (int p = 0; p < n_pivs; p++) {
            if (live[p] && pivs[p].vreg == dvr)
              live[p] = 0;
          }
        }
      }
    }

    /* Backward JUMP: bail on all remaining tracked IVs. */
    if (q->op == TCCIR_OP_JUMP) {
      IROperand jd = tcc_ir_op_get_dest(ir, q);
      int t = (int)irop_get_imm64_ex(ir, jd);
      if (t <= j) {
        for (int p = 0; p < n_pivs; p++) live[p] = 0;
      }
    }
  }

  return total;
}

/* ssa_opt_ptr_iv_exit_subst() — driver.  Improvements over the retired legacy
 * one: dominance-verified back-edge candidates whose synthetic IRLoop spans
 * ALL member blocks (the frontend lays loop bodies after the back-edge, which
 * the legacy flat [header, back-edge] range never contained — hence inert);
 * single-entry + preheader-in-entry-pred + step-dominates-latch legality; and
 * the pass-owned consumer fold above.  Idempotent. */

/* Fixed-point cap; the sweep stops at any CFG-invalidating change and rebuilds. */
#define SSA_PTR_IV_EXIT_SUBST_MAX_PASSES 4

typedef struct {
  IRCFG *cfg;
  const uint8_t *member;
  int latch_b;
} PivStepCtx;

/* step_ok: the self-add's block must be a member dominating the latch. */
static int piv_step_dominates_latch(void *vctx, int instr_idx)
{
  PivStepCtx *ctx = vctx;
  if (instr_idx < 0 || instr_idx >= ctx->cfg->num_instrs)
    return 0;
  int b = ctx->cfg->instr_to_block[instr_idx];
  if (b < 0 || b >= ctx->cfg->num_blocks || !ctx->member[b])
    return 0;
  return tcc_ir_cfg_dominates(ctx->cfg, b, ctx->latch_b);
}

/* Validate one back-edge candidate and run the core; `member` is num_blocks
 * scratch. */
static int piv_try_candidate(TCCIRState *ir, IRCFG *cfg, int header_b,
                             int latch_b, uint8_t *member, int *out_cfg_changes)
{
  IRBasicBlock *hb = &cfg->blocks[header_b];

  *out_cfg_changes = 0;
  fie_collect_members(cfg, header_b, latch_b, member);

  /* Single-entry: header preds must be exactly {out-of-loop pred, latch}. */
  if (hb->num_preds != 2)
    return 0;
  int latch_seen = 0, entry_pred = -1;
  for (int i = 0; i < hb->num_preds; i++) {
    int p = hb->preds[i];
    if (p == latch_b && !latch_seen)
      latch_seen = 1;
    else
      entry_pred = p;
  }
  if (!latch_seen || entry_pred < 0 || member[entry_pred])
    return 0;

  /* No other member block may be entered from outside the loop. */
  for (int b = 0; b < cfg->num_blocks; b++) {
    if (!member[b] || b == header_b)
      continue;
    IRBasicBlock *bb = &cfg->blocks[b];
    for (int i = 0; i < bb->num_preds; i++)
      if (!member[bb->preds[i]])
        return 0;
  }

  /* preheader = first non-jump walking back from the header (legacy licm
   * walk); it must lie in the entry pred so inits come from the entering path. */
  int preheader = hb->start_idx - 1;
  while (preheader >= 0) {
    IRQuadCompact *ph = &ir->compact_instructions[preheader];
    if (ph->op != TCCIR_OP_JUMP && ph->op != TCCIR_OP_JUMPIF)
      break;
    preheader--;
  }
  if (preheader < 0)
    return 0;
  if (cfg->instr_to_block[preheader] != entry_pred)
    return 0;

  /* Synthetic IRLoop: end_idx (inclusive) = max member end, covering bodies
   * laid out beyond the latch in flat order. */
  IRLoop loop;
  memset(&loop, 0, sizeof(loop));
  loop.header_idx = hb->start_idx;
  loop.start_idx = hb->start_idx;
  loop.end_idx = cfg->blocks[latch_b].end_idx - 1;
  for (int b = 0; b < cfg->num_blocks; b++) {
    if (member[b] && cfg->blocks[b].end_idx - 1 > loop.end_idx)
      loop.end_idx = cfg->blocks[b].end_idx - 1;
  }
  loop.preheader_idx = preheader;

  PivStepCtx sctx = {cfg, member, latch_b};
  return ptr_iv_exit_subst_loop(ir, &loop, piv_step_dominates_latch, &sctx,
                                out_cfg_changes);
}

int ssa_opt_ptr_iv_exit_subst(TCCIRState *ir)
{
  if (!ir || ir->next_instruction_index == 0)
    return 0;

  int total = 0;
  for (int pass = 0; pass < SSA_PTR_IV_EXIT_SUBST_MAX_PASSES; pass++) {
    IRCFG *cfg = tcc_ir_cfg_build(ir);
    if (!cfg || cfg->num_blocks <= 1) {
      tcc_ir_cfg_free(cfg);
      break;
    }
    tcc_ir_cfg_compute_dominators(cfg);

    /* Dominance-verified back-edges, smallest-first (as first_iter_exit). */
    int cap = cfg->num_blocks;
    FieCand *cands = tcc_mallocz(sizeof(FieCand) * (size_t)cap);
    int nc = 0;
    for (int b = 0; b < cfg->num_blocks && nc < cap; b++) {
      IRBasicBlock *bb = &cfg->blocks[b];
      for (int si = 0; si < bb->num_succs && nc < cap; si++) {
        int h = bb->succs[si];
        if (h < 0 || h >= cfg->num_blocks)
          continue;
        if (!tcc_ir_cfg_dominates(cfg, h, b))
          continue;
        cands[nc].header_b = h;
        cands[nc].latch_b = b;
        cands[nc].size = cfg->blocks[b].end_idx - cfg->blocks[h].start_idx;
        nc++;
      }
    }

    /* Substitutions keep the CFG valid; stop the sweep at the first
     * control-flow change and let the fixed point rebuild. */
    int cfg_changed = 0;
    if (nc > 0) {
      qsort(cands, nc, sizeof(FieCand), fie_cand_cmp);
      uint8_t *member = tcc_malloc((size_t)cfg->num_blocks);
      for (int i = 0; i < nc && !cfg_changed; i++) {
        int cfg_changes = 0;
        int subs = piv_try_candidate(ir, cfg, cands[i].header_b,
                                     cands[i].latch_b, member, &cfg_changes);
        if (subs > 0 || cfg_changes > 0)
          LOG_LOOP_OPT("ssa:ptr_iv_exit_subst: header_b=%d latch_b=%d "
                       "subs=%d cfg_changes=%d",
                       cands[i].header_b, cands[i].latch_b, subs, cfg_changes);
        total += subs + cfg_changes;
        if (cfg_changes > 0)
          cfg_changed = 1;
      }
      tcc_free(member);
    }
    tcc_free(cands);
    tcc_ir_cfg_free(cfg);

    if (!cfg_changed)
      break;
  }
  return total;
}

/* ============================================================================
 * Loop constant simulation (ssa:loop_const_sim)
 *
 * SSA/CFG-era front-end for the shared LCS engine (lcs_fold_region in
 * ir/opt_loop_const_sim.c).  Candidates are dominance-verified natural loops;
 * contiguity + single-entry over exact member instructions replace the legacy
 * flat-range overlap-merge, external-entry scan (seed 589), and absorbed-tail
 * re-scan (seed 2426): the shapes those defended against decline by
 * construction.  The engine (IV selection, soft-float sim, pre-loop scan,
 * residual emission) is reused verbatim with the rotated-range extension off,
 * since membership is exact.  Register-only bodies only (has_memory decline).
 * See docs/plan_legacy_loop_const_sim_ssa.md.
 * ============================================================================ */

#define SSA_LOOP_CONST_SIM_MAX_PASSES 4
#define SSA_LCS_MAX_SPAN 256

/* Union natural-loop members of every dominance-verified back-edge into
 * header_b into `member`; `scratch` is num_blocks caller scratch. */
static void lcs_collect_header_members(IRCFG *cfg, int header_b, uint8_t *member,
                                       uint8_t *scratch)
{
  memset(member, 0, (size_t)cfg->num_blocks);
  for (int b = 0; b < cfg->num_blocks; b++) {
    IRBasicBlock *bb = &cfg->blocks[b];
    for (int si = 0; si < bb->num_succs; si++) {
      if (bb->succs[si] != header_b)
        continue;
      if (!tcc_ir_cfg_dominates(cfg, header_b, b))
        continue;
      fie_collect_members(cfg, header_b, b, scratch);
      for (int k = 0; k < cfg->num_blocks; k++)
        if (scratch[k])
          member[k] = 1;
    }
  }
}

/* Driver-level register-only decline, evaluated over the member span (matches
 * the legacy driver's body_instrs scan). */
static int lcs_span_has_memory(TCCIRState *ir, int start_idx, int end_idx)
{
  for (int i = start_idx; i <= end_idx; i++) {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;
    if (q->op == TCCIR_OP_LOAD || q->op == TCCIR_OP_STORE ||
        q->op == TCCIR_OP_LOAD_INDEXED || q->op == TCCIR_OP_STORE_INDEXED ||
        q->op == TCCIR_OP_LOAD_POSTINC || q->op == TCCIR_OP_STORE_POSTINC ||
        q->op == TCCIR_OP_BLOCK_COPY)
      return 1;
    if (irop_config[q->op].has_src1 && tcc_ir_op_get_src1(ir, q).is_lval)
      return 1;
    if (irop_config[q->op].has_src2 && tcc_ir_op_get_src2(ir, q).is_lval)
      return 1;
    if (q->op == TCCIR_OP_MLA && tcc_ir_op_get_accum(ir, q).is_lval)
      return 1;
  }
  return 0;
}

/* Validate one outermost header candidate against the checked facts and, if it
 * passes, fold via the shared engine.  Returns 1 if folded.  `member`/`scratch`
 * are num_blocks caller scratch. */
static int lcs_try_candidate(TCCIRState *ir, IRCFG *cfg, int header_b,
                             uint8_t *member, uint8_t *scratch)
{
  IRBasicBlock *hb = &cfg->blocks[header_b];
  lcs_collect_header_members(cfg, header_b, member, scratch);

  int eff_start = ir->next_instruction_index;
  int eff_end = -1;
  for (int b = 0; b < cfg->num_blocks; b++) {
    if (!member[b])
      continue;
    if (cfg->blocks[b].start_idx < eff_start)
      eff_start = cfg->blocks[b].start_idx;
    if (cfg->blocks[b].end_idx - 1 > eff_end)
      eff_end = cfg->blocks[b].end_idx - 1;
  }
  if (eff_end < eff_start)
    return 0;
  if (eff_end - eff_start > SSA_LCS_MAX_SPAN)
    return 0;

  /* Contiguity: every non-NOP instruction in the span belongs to a member. */
  for (int i = eff_start; i <= eff_end; i++) {
    if (ir->compact_instructions[i].op == TCCIR_OP_NOP)
      continue;
    int b = cfg->instr_to_block[i];
    if (b < 0 || b >= cfg->num_blocks || !member[b])
      return 0;
  }

  /* Single-entry: the header must dominate every member (a proper natural
   * loop).  A member the header does not dominate is a side entry into the
   * body that backward reachability absorbed; only the header may be entered
   * from outside the span, and a dominated member cannot have an external
   * pred, so this one check expresses fact (2). */
  for (int b = 0; b < cfg->num_blocks; b++) {
    if (!member[b])
      continue;
    if (!tcc_ir_cfg_dominates(cfg, header_b, b))
      return 0;
  }

  if (lcs_span_has_memory(ir, eff_start, eff_end))
    return 0;

  /* Preheader: last non-jump instruction before the span (legacy licm walk),
   * where find_induction_vars_ex looks back for the IV initialisation. */
  int preheader = eff_start - 1;
  while (preheader >= 0) {
    int pop = ir->compact_instructions[preheader].op;
    if (pop != TCCIR_OP_JUMP && pop != TCCIR_OP_JUMPIF)
      break;
    preheader--;
  }

  return lcs_fold_region(ir, eff_start, eff_end, hb->start_idx, preheader,
                         /*allow_extension*/ 0);
}

/* ssa_opt_loop_const_sim() — driver.  Per CFG build, folds every outermost
 * dominance-verified natural loop (as the legacy driver folded every detected
 * loop per pass): their spans are provably disjoint (contiguity means a
 * candidate's span holds only its own members), and each fold touches only its
 * own span plus its IV init in its own preheader gap, so the folds do not
 * interfere.  Processing in ascending block (≈ flat) order lets a forward
 * cascade (loop B reading loop A's residual) resolve in one round: B's pre-loop
 * scan reads A's just-emitted residual ASSIGN/STOREs directly, no interleaved
 * cleanup.  The bounded fixed point then picks up backward cascades and loops a
 * fold newly exposed.  Idempotent (its output holds no back-edge) and inert
 * during coexistence (legacy already residualised qualifying loops before
 * regalloc). */
int ssa_opt_loop_const_sim(TCCIRState *ir)
{
  if (!ir || ir->next_instruction_index == 0)
    return 0;

  int total = 0;
  for (int pass = 0; pass < SSA_LOOP_CONST_SIM_MAX_PASSES; pass++) {
    IRCFG *cfg = tcc_ir_cfg_build(ir);
    if (!cfg || cfg->num_blocks <= 1) {
      tcc_ir_cfg_free(cfg);
      break;
    }
    tcc_ir_cfg_compute_dominators(cfg);

    int nb = cfg->num_blocks;
    uint8_t *is_header = tcc_mallocz((size_t)nb);
    for (int b = 0; b < nb; b++) {
      IRBasicBlock *bb = &cfg->blocks[b];
      for (int si = 0; si < bb->num_succs; si++) {
        int h = bb->succs[si];
        if (h >= 0 && h < nb && tcc_ir_cfg_dominates(cfg, h, b))
          is_header[h] = 1;
      }
    }

    uint8_t *member = tcc_malloc((size_t)nb);
    uint8_t *scratch = tcc_malloc((size_t)nb);
    uint8_t *other = tcc_malloc((size_t)nb);

    int folded = 0;
    for (int h = 0; h < nb; h++) {
      if (!is_header[h])
        continue;
      /* Outermost only (legacy depth>1 skip): decline a header nested in
       * another header's loop; simulate inner back-edges as internal flow. */
      int inner = 0;
      for (int h2 = 0; h2 < nb && !inner; h2++) {
        if (h2 == h || !is_header[h2])
          continue;
        lcs_collect_header_members(cfg, h2, other, scratch);
        if (other[h])
          inner = 1;
      }
      if (inner)
        continue;
      if (lcs_try_candidate(ir, cfg, h, member, scratch)) {
        folded++;
        LOG_IR_GEN("[ssa:loop_const_sim] folded header_b=%d", h);
      }
    }

    tcc_free(is_header);
    tcc_free(member);
    tcc_free(scratch);
    tcc_free(other);
    tcc_ir_cfg_free(cfg);

    total += folded;
    if (!folded)
      break;
  }
  return total;
}

/* ============================================================================
 * Loop Unrolling / Constant-Trip Elimination (ssa:loop_unroll)
 *
 * CFG/dominator-driven replacement for the legacy pre-SSA tcc_ir_opt_loop_unroll
 * (ir/opt_loop.c).  Per outermost natural loop, runs the shared mutators
 * try_eliminate_loop -> try_eliminate_loop_symbolic -> try_unroll_loop_ex
 * (ir/opt_loop_utils.c) on a synthetic contiguous single-entry IRLoop built from
 * CFG facts, replacing the flat detector's overlap-merge + external-entry scan.
 * The mutators own the register-only / no_unroll / trip-count guards.
 *
 * Outermost-only, matching legacy: the legacy overlap-merge collapsed a nest
 * into one loop that then declined on internal branches, so nested inner loops
 * were never unrolled.  It is also a soundness requirement here: a nested inner
 * loop's accumulator is loop-carried by its enclosing loop, but
 * find_induction_vars_ex would read its "init" from the enclosing loop's
 * preheader and eliminate it to a wrong closed form (NOPing the outer loop's
 * guard — 991216-4 infinite loop).  Only a non-nested single-entry loop has a
 * genuine once-established IV init.
 * See docs/plan_legacy_loop_unroll_ssa.md.
 * ============================================================================ */

#define SSA_LOOP_UNROLL_MAX_PASSES 4
#define SSA_UNROLL_MAX_SPAN 256

/* Build a synthetic contiguous single-entry IRLoop for header_b and run the
 * unroll mutators; returns 1 if the loop was eliminated/unrolled.  member and
 * scratch are num_blocks caller scratch. */
static int unroll_try_candidate(TCCIRState *ir, IRCFG *cfg, int header_b,
                                uint8_t *member, uint8_t *scratch)
{
  IRBasicBlock *hb = &cfg->blocks[header_b];
  lcs_collect_header_members(cfg, header_b, member, scratch);

  int eff_start = ir->next_instruction_index;
  int eff_end = -1;
  for (int b = 0; b < cfg->num_blocks; b++) {
    if (!member[b])
      continue;
    if (cfg->blocks[b].start_idx < eff_start)
      eff_start = cfg->blocks[b].start_idx;
    if (cfg->blocks[b].end_idx - 1 > eff_end)
      eff_end = cfg->blocks[b].end_idx - 1;
  }
  if (eff_end < eff_start)
    return 0;
  if (eff_end - eff_start > SSA_UNROLL_MAX_SPAN)
    return 0;

  /* Contiguity: every non-NOP in the span is a member (rejects split layouts
   * the legacy flat detector could not model). */
  for (int i = eff_start; i <= eff_end; i++) {
    if (ir->compact_instructions[i].op == TCCIR_OP_NOP)
      continue;
    int b = cfg->instr_to_block[i];
    if (b < 0 || b >= cfg->num_blocks || !member[b])
      return 0;
  }
  /* Single-entry: header dominates every member (a proper natural loop, so only
   * the header is entered from outside the span). */
  for (int b = 0; b < cfg->num_blocks; b++) {
    if (member[b] && !tcc_ir_cfg_dominates(cfg, header_b, b))
      return 0;
  }

  int preheader = eff_start - 1;
  while (preheader >= 0) {
    int pop = ir->compact_instructions[preheader].op;
    if (pop != TCCIR_OP_JUMP && pop != TCCIR_OP_JUMPIF)
      break;
    preheader--;
  }

  IRLoop loop = {0};
  loop.header_idx = hb->start_idx;
  loop.start_idx = eff_start;
  loop.end_idx = eff_end; /* latch back-edge instruction: no_unroll flag + NOP range */
  loop.preheader_idx = preheader;
  loop.depth = 1;

  if (try_eliminate_loop(ir, &loop))
    return 1;
  if (try_eliminate_loop_symbolic(ir, &loop))
    return 1;

  /* try_unroll_loop_ex enables its IR-growth path only when num_loops==1; wrap
   * the synthetic loop so the single-loop growth win stays available (the
   * sibling-patch loop is a no-op with one loop). */
  IRLoops one = {0};
  one.loops = &loop;
  one.num_loops = 1;
  one.capacity = 1;
  return try_unroll_loop_ex(ir, &loop, &one, 0);
}

/* ssa_opt_loop_unroll() — driver.  Dominance-verified outermost natural loops
 * (a header nested in another header's loop is skipped; the enclosing loop
 * declines on its internal branches), one transform per CFG build (a firing
 * removes the loop's back-edge and may grow the IR via insert_instr_at, both
 * invalidating the CFG), fixed point bounded by SSA_LOOP_UNROLL_MAX_PASSES.
 * Idempotent (output holds no back-edge) and inert during coexistence (legacy
 * already unrolled qualifying loops before regalloc). */
int ssa_opt_loop_unroll(TCCIRState *ir)
{
  if (!ir || ir->next_instruction_index == 0)
    return 0;

  int total = 0;
  for (int pass = 0; pass < SSA_LOOP_UNROLL_MAX_PASSES; pass++) {
    IRCFG *cfg = tcc_ir_cfg_build(ir);
    if (!cfg || cfg->num_blocks <= 1) {
      tcc_ir_cfg_free(cfg);
      break;
    }
    tcc_ir_cfg_compute_dominators(cfg);

    int nb = cfg->num_blocks;
    uint8_t *is_header = tcc_mallocz((size_t)nb);
    for (int b = 0; b < nb; b++) {
      IRBasicBlock *bb = &cfg->blocks[b];
      for (int si = 0; si < bb->num_succs; si++) {
        int h = bb->succs[si];
        if (h >= 0 && h < nb && tcc_ir_cfg_dominates(cfg, h, b))
          is_header[h] = 1;
      }
    }

    uint8_t *member = tcc_malloc((size_t)nb);
    uint8_t *scratch = tcc_malloc((size_t)nb);
    uint8_t *other = tcc_malloc((size_t)nb);

    int changed = 0;
    for (int h = 0; h < nb && !changed; h++) {
      if (!is_header[h])
        continue;
      /* Outermost only: skip a header whose loop is contained in another. */
      int inner = 0;
      for (int h2 = 0; h2 < nb && !inner; h2++) {
        if (h2 == h || !is_header[h2])
          continue;
        lcs_collect_header_members(cfg, h2, other, scratch);
        if (other[h])
          inner = 1;
      }
      if (inner)
        continue;
      changed = unroll_try_candidate(ir, cfg, h, member, scratch);
      if (changed)
        LOG_IR_GEN("[ssa:loop_unroll] transformed header_b=%d", h);
    }

    tcc_free(is_header);
    tcc_free(member);
    tcc_free(scratch);
    tcc_free(other);
    tcc_ir_cfg_free(cfg);

    total += changed;
    if (!changed)
      break;
  }
  return total;
}

/* ============================================================================
 * Induction-Variable Strength Reduction (ssa:iv_strength_reduction)
 *
 * CFG/dominator front-end for the legacy pre-SSA driver
 * tcc_ir_opt_iv_strength_reduction (ir/opt_loop.c, removed), relocated from the
 * tccgen Phase 6 call site.  Per outermost dominance-verified natural loop,
 * builds a synthetic contiguous single-entry IRLoop and runs the retained
 * transform engine iv_strength_reduction_core (ir/opt_loop_utils.c) on a
 * 1-element IRLoops wrapper.  body_instrs is the DENSE range [eff_start..eff_end]:
 * the CFG member span is verified contiguous, so it is exactly the range
 * find_derived_ivs / the transform's escape scan / the APPLY_SHIFT fixup read
 * (rotation ran first, so the "detached body after the back-edge" shape the
 * legacy +50 forward-jump body extension chased no longer reaches here; a
 * non-contiguous survivor is declined, not mis-scanned).  Transforms one DIV per
 * CFG build (a firing grows the IR via insert_instr_at, invalidating the CFG),
 * bounded fixed point.  Runs after ssa:loop_unroll and before
 * ssa:decrement_to_zero, preserving the legacy "consumers after IV-SR" order.
 * See docs/plan_legacy_loop_iv_strength_reduction_ssa.md.
 * ============================================================================ */

#define SSA_IVSR_MAX_PASSES 8

/* Build the synthetic contiguous single-entry IRLoop for header_b (dense
 * body_instrs) and run one iv_strength_reduction_core transform on it; returns
 * the change count.  member/scratch are num_blocks caller scratch. */
static int ivsr_try_candidate(TCCIRState *ir, IRCFG *cfg, int header_b,
                              uint8_t *member, uint8_t *scratch)
{
  IRBasicBlock *hb = &cfg->blocks[header_b];
  lcs_collect_header_members(cfg, header_b, member, scratch);

  int eff_start = ir->next_instruction_index;
  int eff_end = -1;
  for (int b = 0; b < cfg->num_blocks; b++) {
    if (!member[b])
      continue;
    if (cfg->blocks[b].start_idx < eff_start)
      eff_start = cfg->blocks[b].start_idx;
    if (cfg->blocks[b].end_idx - 1 > eff_end)
      eff_end = cfg->blocks[b].end_idx - 1;
  }
  if (eff_end < eff_start)
    return 0;

  /* Contiguity: every non-NOP in the span belongs to a member. */
  for (int i = eff_start; i <= eff_end; i++) {
    if (ir->compact_instructions[i].op == TCCIR_OP_NOP)
      continue;
    int b = cfg->instr_to_block[i];
    if (b < 0 || b >= cfg->num_blocks || !member[b])
      return 0;
  }
  /* Single-entry: header dominates every member (a proper natural loop). */
  for (int b = 0; b < cfg->num_blocks; b++) {
    if (member[b] && !tcc_ir_cfg_dominates(cfg, header_b, b))
      return 0;
  }

  int preheader = eff_start - 1;
  while (preheader >= 0) {
    int pop = ir->compact_instructions[preheader].op;
    if (pop != TCCIR_OP_JUMP && pop != TCCIR_OP_JUMPIF)
      break;
    preheader--;
  }

  int nbody = eff_end - eff_start + 1;
  IRLoop loop = {0};
  loop.header_idx = hb->start_idx;
  loop.start_idx = eff_start;
  loop.end_idx = eff_end;
  loop.preheader_idx = preheader;
  loop.depth = 1;
  loop.body_instrs = tcc_malloc(sizeof(int) * (size_t)nbody);
  loop.body_instrs_capacity = nbody;
  loop.num_body_instrs = nbody;
  for (int k = 0; k < nbody; k++)
    loop.body_instrs[k] = eff_start + k;

  IRLoops one = {0};
  one.loops = &loop;
  one.num_loops = 1;
  one.capacity = 1;

  int c = iv_strength_reduction_core(ir, &one);
  tcc_free(loop.body_instrs);
  return c;
}

/* ssa_opt_iv_strength_reduction() — driver.  Transforms one DIV per CFG build in
 * the first eligible outermost natural loop (matching the legacy driver, whose
 * core transformed one DIV then re-detected), bounded fixed point.  Idempotent
 * (a transformed DIV is a stride pointer, no longer an ADD-of-SHL). */
int ssa_opt_iv_strength_reduction(TCCIRState *ir)
{
  if (!ir || ir->next_instruction_index == 0)
    return 0;

  int total = 0;
  for (int pass = 0; pass < SSA_IVSR_MAX_PASSES; pass++) {
    IRCFG *cfg = tcc_ir_cfg_build(ir);
    if (!cfg || cfg->num_blocks <= 1) {
      tcc_ir_cfg_free(cfg);
      break;
    }
    tcc_ir_cfg_compute_dominators(cfg);

    int nb = cfg->num_blocks;
    uint8_t *is_header = tcc_mallocz((size_t)nb);
    for (int b = 0; b < nb; b++) {
      IRBasicBlock *bb = &cfg->blocks[b];
      for (int si = 0; si < bb->num_succs; si++) {
        int h = bb->succs[si];
        if (h >= 0 && h < nb && tcc_ir_cfg_dominates(cfg, h, b))
          is_header[h] = 1;
      }
    }

    uint8_t *member = tcc_malloc((size_t)nb);
    uint8_t *scratch = tcc_malloc((size_t)nb);
    uint8_t *other = tcc_malloc((size_t)nb);

    int changed = 0;
    for (int h = 0; h < nb && !changed; h++) {
      if (!is_header[h])
        continue;
      /* Outermost only: skip a header whose loop is contained in another. */
      int inner = 0;
      for (int h2 = 0; h2 < nb && !inner; h2++) {
        if (h2 == h || !is_header[h2])
          continue;
        lcs_collect_header_members(cfg, h2, other, scratch);
        if (other[h])
          inner = 1;
      }
      if (inner)
        continue;
      changed = ivsr_try_candidate(ir, cfg, h, member, scratch);
      if (changed)
        LOG_IR_GEN("[ssa:iv_strength_reduction] transformed header_b=%d", h);
    }

    tcc_free(is_header);
    tcc_free(member);
    tcc_free(scratch);
    tcc_free(other);
    tcc_ir_cfg_free(cfg);

    total += changed;
    if (!changed)
      break;
  }
  return total;
}

/* ============================================================================
 * Decrement-to-Zero (ssa:decrement_to_zero)
 *
 * CFG/dominator-driven replacement for the legacy pre-SSA
 * tcc_ir_opt_decrement_to_zero (ir/opt_loop.c, removed).  Runs after
 * ssa:loop_unroll on the rotated bottom-tested survivors: rewrites a count-up
 * pure-counter loop with a separate pre-test guard into count-down-to-zero so
 * the backend fuses the latch SUB+CMP#0 into a flag-setting SUBS.  The per-loop
 * detection + in-place rewrite engine (dtz_try_region, ir/opt_loop_utils.c) is
 * retained verbatim; only its candidate source becomes dominance-verified
 * outermost natural loops.  No has_memory decline — the candidate body is a
 * plain store to a fixed location (rotation already declined call/indexed/
 * indirect bodies, so those stay top-tested and dtz's own back-edge scan bails).
 * In-place (no IR growth), idempotent (output holds no count-up IV with a
 * separate guard), inert during coexistence (legacy inert at its tccgen site).
 * See docs/plan_legacy_loop_decrement_to_zero_ssa.md.
 * ============================================================================ */

#define SSA_DTZ_MAX_PASSES 4

/* Build the contiguous single-entry member span for header_b and run the
 * decrement-to-zero engine on it; returns 1 if rewritten.  member/scratch are
 * num_blocks caller scratch. */
static int dtz_try_candidate(TCCIRState *ir, IRCFG *cfg, int header_b,
                             uint8_t *member, uint8_t *scratch)
{
  IRBasicBlock *hb = &cfg->blocks[header_b];
  lcs_collect_header_members(cfg, header_b, member, scratch);

  int eff_start = ir->next_instruction_index;
  int eff_end = -1;
  for (int b = 0; b < cfg->num_blocks; b++) {
    if (!member[b])
      continue;
    if (cfg->blocks[b].start_idx < eff_start)
      eff_start = cfg->blocks[b].start_idx;
    if (cfg->blocks[b].end_idx - 1 > eff_end)
      eff_end = cfg->blocks[b].end_idx - 1;
  }
  if (eff_end < eff_start)
    return 0;

  /* Contiguity: every non-NOP in the span belongs to a member. */
  for (int i = eff_start; i <= eff_end; i++) {
    if (ir->compact_instructions[i].op == TCCIR_OP_NOP)
      continue;
    int b = cfg->instr_to_block[i];
    if (b < 0 || b >= cfg->num_blocks || !member[b])
      return 0;
  }
  /* Single-entry: header dominates every member (a proper natural loop). */
  for (int b = 0; b < cfg->num_blocks; b++) {
    if (member[b] && !tcc_ir_cfg_dominates(cfg, header_b, b))
      return 0;
  }

  int preheader = eff_start - 1;
  while (preheader >= 0) {
    int pop = ir->compact_instructions[preheader].op;
    if (pop != TCCIR_OP_JUMP && pop != TCCIR_OP_JUMPIF)
      break;
    preheader--;
  }

  return dtz_try_region(ir, eff_start, eff_end, hb->start_idx, preheader);
}

/* ssa_opt_decrement_to_zero() — driver.  Rewrites every dominance-verified
 * outermost natural loop that is a count-up pure-counter candidate; spans are
 * disjoint and the rewrite is in-place, so all fire in one CFG build (as
 * ssa:loop_const_sim folds every outermost loop per build).  Bounded fixed
 * point for uniformity, though one pass suffices (idempotent). */
int ssa_opt_decrement_to_zero(TCCIRState *ir)
{
  if (!ir || ir->next_instruction_index == 0)
    return 0;

  int total = 0;
  for (int pass = 0; pass < SSA_DTZ_MAX_PASSES; pass++) {
    IRCFG *cfg = tcc_ir_cfg_build(ir);
    if (!cfg || cfg->num_blocks <= 1) {
      tcc_ir_cfg_free(cfg);
      break;
    }
    tcc_ir_cfg_compute_dominators(cfg);

    int nb = cfg->num_blocks;
    uint8_t *is_header = tcc_mallocz((size_t)nb);
    for (int b = 0; b < nb; b++) {
      IRBasicBlock *bb = &cfg->blocks[b];
      for (int si = 0; si < bb->num_succs; si++) {
        int h = bb->succs[si];
        if (h >= 0 && h < nb && tcc_ir_cfg_dominates(cfg, h, b))
          is_header[h] = 1;
      }
    }

    uint8_t *member = tcc_malloc((size_t)nb);
    uint8_t *scratch = tcc_malloc((size_t)nb);
    uint8_t *other = tcc_malloc((size_t)nb);

    int fired = 0;
    for (int h = 0; h < nb; h++) {
      if (!is_header[h])
        continue;
      /* Outermost only: skip a header whose loop is contained in another. */
      int inner = 0;
      for (int h2 = 0; h2 < nb && !inner; h2++) {
        if (h2 == h || !is_header[h2])
          continue;
        lcs_collect_header_members(cfg, h2, other, scratch);
        if (other[h])
          inner = 1;
      }
      if (inner)
        continue;
      if (dtz_try_candidate(ir, cfg, h, member, scratch)) {
        fired++;
        LOG_IR_GEN("[ssa:decrement_to_zero] rewrote header_b=%d", h);
      }
    }

    tcc_free(is_header);
    tcc_free(member);
    tcc_free(scratch);
    tcc_free(other);
    tcc_ir_cfg_free(cfg);

    total += fired;
    if (!fired)
      break;
  }
  return total;
}
