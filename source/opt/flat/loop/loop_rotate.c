/*
 *  TCC IR - Loop rotation
 *
 *  Copyright (c) 2025 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#define USING_GLOBALS

#include "ir.h"
#include "licm.h"
#include "opt.h"
#include "opt_du.h"
#include "opt_xform.h"
#include "opt_utils.h"
#include "opt_loop_utils.h"

/* A VAR-vreg lval operand is a direct variable access (same convention as
 * ROT_LVAL_IS_INDIRECT below); every other lval dereferences the vreg. */
#define ROT_VAR_DIRECT(op_)                                                                                       \
  (TCCIR_DECODE_VREG_TYPE(irop_get_vreg(op_)) == TCCIR_VREG_TYPE_VAR && (op_).is_local)

/* NOTE: the former rot_iv_dead_after_exit veto (reject a call-body rotation
 * whose IV is read after the loop) is gone.  Its cost model — a live-out IV
 * pays a merge copy at BOTH exits — only holds while the zero-trip guard
 * survives.  rot_guard_provably_folds is now mandatory for these shapes, and a
 * folded guard leaves the rotated loop with a single exit, so the live-out IV
 * is one value and needs no phi.  Measured on the memclr chain, where the
 * liveness veto used to block every loop that shares `i`. */

/* 1 when the header's exit branch is provably untaken on loop entry: the IV
 * enters from a known constant and the CMP's other side is a literal.  A later
 * const fold (or, for the carried case, ssa:loop_guard_elim) then deletes the
 * pre-loop guard, so rotation costs nothing.  Without this the guard survives
 * (runtime entry value) and rotation trades the back-branch for a guard
 * CMP+branch — a net static loss (memclr loop 3 measured +2 per function).
 * Used to gate the call-body shapes only.
 *
 * Two sources of the entry constant:
 *   - a literal ASSIGN on the straight-line path into the header (the first
 *     loop of a function; ordinary const prop folds this guard);
 *   - the exit value of preceding counted loops over the same IV (loops 2..N
 *     of a sequential chain), which only tcc_ir_loop_seq_entry_const knows —
 *     SSA sees a phi at the header and cannot conclude i == A. */
static int rot_guard_provably_folds(TCCIRState *ir, int hi, IRQuadCompact *cmp_q, int cond)
{
  IROperand s1 = tcc_ir_op_get_src1(ir, cmp_q);
  IROperand s2 = tcc_ir_op_get_src2(ir, cmp_q);
  if (!irop_has_vreg(s1) || !irop_is_immediate(s2))
    return 0;
  int32_t iv = irop_get_vreg(s1);
  int64_t lim = irop_get_imm64_ex(ir, s2);
  for (int i = hi - 1; i >= 0; i--)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    int op = q->op;
    if (op == TCCIR_OP_NOP)
      continue;
    if (op == TCCIR_OP_JUMP || op == TCCIR_OP_JUMPIF || op == TCCIR_OP_IJUMP)
      break; /* not the straight-line entry path — try the carried value */
    int is_def = 0;
    if (irop_config[op].has_dest)
    {
      IROperand d = tcc_ir_op_get_dest(ir, q);
      is_def = irop_has_vreg(d) && irop_get_vreg(d) == iv && (!d.is_lval || ROT_VAR_DIRECT(d));
    }
    /* a join between the def and the header can bring a different value */
    if (q->is_jump_target && !is_def)
      break;
    if (!is_def)
      continue;
    IROperand v = tcc_ir_op_get_src1(ir, q);
    if (op != TCCIR_OP_ASSIGN || !irop_is_immediate(v))
      return 0;
    int64_t entry = irop_get_imm64_ex(ir, v);
    return evaluate_compare_condition_cmp_annotated(ir, cmp_q, entry, lim, cond, s1, s2) == 0;
  }

  if (tcc_ir_opt_pass_disabled("ssa:loop_guard_elim"))
    return 0;
  int64_t carried;
  if (!tcc_ir_loop_seq_entry_const(ir, hi, iv, &carried))
    return 0;
  return evaluate_compare_condition_cmp_annotated(ir, cmp_q, carried, lim, cond, s1, s2) == 0;
}

/* TCC_NO_COALESCE also restores the pre-coalesce body-scan bound here, so one
 * binary carries both arms of the rotation/coalescing A/B. */
TCC_DBG_ENV_FLAG(lr_no_coalesce, "TCC_NO_COALESCE")

int try_rotate_loop(TCCIRState *ir, IRLoop *loop)
{
  int hi = loop->header_idx;
  int n = ir->next_instruction_index;

  /* header pattern needs 3 instructions: CMP, JUMPIF, JUMP */
  if (hi + 2 > loop->end_idx)
    return 0;

  IRQuadCompact *cmp_q = &ir->compact_instructions[hi];
  IRQuadCompact *jif_q = &ir->compact_instructions[hi + 1];
  IRQuadCompact *jmp_q = &ir->compact_instructions[hi + 2];

  if (cmp_q->op != TCCIR_OP_CMP)
    return 0;
  if (jif_q->op != TCCIR_OP_JUMPIF)
    return 0;
  if (jmp_q->op != TCCIR_OP_JUMP)
    return 0;

  IROperand exit_dest = tcc_ir_op_get_dest(ir, jif_q);
  int exit_target = (int)irop_get_imm64_ex(ir, exit_dest);
  IROperand cond_op = tcc_ir_op_get_src1(ir, jif_q);
  int cond = (int)irop_get_imm64_ex(ir, cond_op);

  IROperand body_entry_dest = tcc_ir_op_get_dest(ir, jmp_q);
  int body_start = (int)irop_get_imm64_ex(ir, body_entry_dest);

  /* end_idx can cover the body too, so find the back-edge by scanning for the first JUMP to hi */
  int backedge_idx = -1;
  for (int i = hi + 3; i <= loop->end_idx; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_JUMP)
    {
      IROperand jd = tcc_ir_op_get_dest(ir, q);
      int jt = (int)irop_get_imm64_ex(ir, jd);
      if (jt == hi)
      {
        backedge_idx = i;
        break;
      }
    }
  }
  if (backedge_idx < 0)
  {
    LOG_LOOP_OPT("Rotation: reject — no back-edge JUMP to header %d", hi);
    return 0;
  }
  LOG_LOOP_OPT("Rotation: backedge at %d, exit_target=%d, body_start=%d", backedge_idx, exit_target, body_start);

  /* exit must leave the core region [hi, backedge_idx]; end_idx is too coarse here */
  if (exit_target > hi && exit_target <= backedge_idx)
  {
    LOG_LOOP_OPT("Rotation: reject — exit_target %d inside [%d,%d]", exit_target, hi, backedge_idx);
    return 0;
  }

  /* body must sit after the back-edge (standard TCC layout) */
  if (body_start <= backedge_idx || body_start >= n)
  {
    LOG_LOOP_OPT("Rotation: reject — body_start %d not after backedge %d (n=%d)", body_start, backedge_idx, n);
    return 0;
  }

  /* a backward JUMPIF enclosing [hi, backedge_idx] is an already-rotated outer loop; double rotation miscompiles */
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_JUMPIF)
      continue;
    IROperand jd = tcc_ir_op_get_dest(ir, q);
    int jt = (int)irop_get_imm64_ex(ir, jd);
    if (jt >= 0 && jt < hi && i > backedge_idx)
    {
      LOG_LOOP_OPT("Rotation: reject — nested inside already-rotated loop [%d..%d]", jt, i);
      return 0;
    }
  }

  int latch_start = hi + 3;
  int latch_end = backedge_idx - 1; /* exclude back-edge JUMP */
  int latch_count = latch_end - latch_start + 1;
  if (latch_count < 0)
    latch_count = 0;

  /* latch must be small (IV save + increment, typically 2 instrs) */
  if (latch_count > 8)
    return 0;

  /* jump threading may have retargeted the body→latch JUMP anywhere inside the latch */
  int body_end_jmp = -1;
  int body_latch_target = -1;
  int body_end_is_implicit = 0;
  int cond_body = 0;
  /* first instruction of the cond_body cold tail (decide + 1); calls at or
   * after this index are allowed — the tail is proven terminating, so control
   * never returns from it into the loop */
  int cond_cold_start = -1;
  /* break_invert: body ends in a deciding JUMPIF-to-latch whose fall-through is the exit (`if (cond) break;`) */
  int break_invert = 0;
  int break_decide_idx = -1;
  /* bound scans to this loop's exit so a sibling loop's back-edge is not misread as an inner loop */
  int body_scan_limit = body_start + 100;
  if (!lr_no_coalesce()) {
    if (exit_target > body_start && exit_target < body_scan_limit)
      body_scan_limit = exit_target;
  }
  if (body_scan_limit > n) body_scan_limit = n;
  for (int i = body_start; i < body_scan_limit; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_JUMP)
    {
      IROperand jd = tcc_ir_op_get_dest(ir, q);
      int jt = (int)irop_get_imm64_ex(ir, jd);
      if (jt >= latch_start && jt <= backedge_idx)
      {
        body_end_jmp = i;
        body_latch_target = jt;
        break;
      }
    }
  }

  /* an inner loop's exit JUMPIF may target the latch directly; simple loops fall through to the exit instead */
  if (body_end_jmp < 0)
  {
    int has_inner_loop = 0;
    for (int i = body_start; i < body_scan_limit; i++)
    {
      IRQuadCompact *q = &ir->compact_instructions[i];
      if (q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_JUMPIF)
      {
        IROperand jd = tcc_ir_op_get_dest(ir, q);
        int jt = (int)irop_get_imm64_ex(ir, jd);
        if (jt < i && jt >= body_start)
        {
          has_inner_loop = 1;
          break;
        }
      }
    }
    if (has_inner_loop)
    {
      for (int i = body_start; i < body_scan_limit; i++)
      {
        IRQuadCompact *q = &ir->compact_instructions[i];
        if (q->op == TCCIR_OP_JUMPIF)
        {
          IROperand jd = tcc_ir_op_get_dest(ir, q);
          int jt = (int)irop_get_imm64_ex(ir, jd);
          if (jt >= latch_start && jt <= backedge_idx)
          {
            body_latch_target = jt;
            body_end_is_implicit = 1;
            body_end_jmp = exit_target - 1;
            break;
          }
        }
      }
    }
  }
  /* conditional-body shape: a single forward JUMPIF to the latch plus a cold tail; the JUMPIF is the body→latch edge */
  if (body_end_jmp < 0 && exit_target > body_start && exit_target <= n)
  {
    int decide = -1, decide_target = -1, branches = 0, bad = 0;
    for (int i = body_start; i < exit_target; i++)
    {
      IRQuadCompact *q = &ir->compact_instructions[i];
      if (q->op == TCCIR_OP_IJUMP || q->op == TCCIR_OP_JUMP)
      {
        bad = 1;
        break;
      }
      if (q->op == TCCIR_OP_JUMPIF)
      {
        IROperand jd = tcc_ir_op_get_dest(ir, q);
        int jt = (int)irop_get_imm64_ex(ir, jd);
        branches++;
        if (jt >= latch_start && jt <= backedge_idx && decide < 0)
        {
          decide = i;
          decide_target = jt;
        }
        else
        {
          bad = 1;
          break;
        }
      }
    }
    /* rotation makes the cold tail fall into the latch, so it must have no live fall-through of its own */
    int cold_terminates = 0;
    if (!bad && decide >= 0 && branches == 1 && decide < exit_target - 1)
    {
      int last = exit_target - 1;
      while (last > decide && ir->compact_instructions[last].op == TCCIR_OP_NOP)
        last--;
      IRQuadCompact *lq = &ir->compact_instructions[last];
      if (lq->op == TCCIR_OP_RETURNVALUE || lq->op == TCCIR_OP_RETURNVOID || lq->op == TCCIR_OP_TRAP)
        cold_terminates = 1;
      else if (lq->op == TCCIR_OP_FUNCCALLVOID || lq->op == TCCIR_OP_FUNCCALLVAL)
      {
        Sym *callee = irop_get_sym_ex(ir, tcc_ir_op_get_src1(ir, lq));
        if (tcc_ir_callee_is_noreturn(callee))
          cold_terminates = 1;
      }
    }
    if (!bad && decide >= 0 && branches == 1 && decide < exit_target - 1 && cold_terminates)
    {
      cond_body = 1;
      cond_cold_start = decide + 1;
      body_latch_target = decide_target;
      body_end_jmp = exit_target - 1;
      body_end_is_implicit = 1;
      LOG_LOOP_OPT("Rotation: conditional-body shape, decide JUMPIF@%d -> latch %d, cold tail [%d..%d] (terminating)",
                   decide, decide_target, decide + 1, exit_target - 1);
    }
    /* break-via-fall-through shape: deciding JUMPIF is the last body instruction, no cold tail */
    else if (!bad && decide >= 0 && branches == 1 && decide == exit_target - 1)
    {
      /* with a call, the split result copies survive coalescing and cost more than the rotation saves */
      int body_has_call = 0;
      for (int i = body_start; i <= decide; i++)
      {
        int bop = ir->compact_instructions[i].op;
        if (bop == TCCIR_OP_FUNCCALLVAL || bop == TCCIR_OP_FUNCCALLVOID)
        {
          body_has_call = 1;
          break;
        }
      }
      if (!body_has_call)
      {
        break_invert = 1;
        break_decide_idx = decide;
        body_latch_target = decide_target;
        body_end_jmp = decide;
        body_end_is_implicit = 1;
        LOG_LOOP_OPT("Rotation: break-fall-through shape, decide JUMPIF@%d -> latch %d, invert to exit %d", decide,
                     decide_target, exit_target);
      }
    }
  }
  if (body_end_jmp < 0)
  {
    LOG_LOOP_OPT("Rotation: reject — no body→latch JUMP from body_start=%d targeting [%d,%d]", body_start, latch_start,
                 backedge_idx);
    return 0;
  }
  LOG_LOOP_OPT("Rotation: body_end_jmp=%d, latch=[%d,%d], latch_count=%d", body_end_jmp, latch_start, latch_end,
               latch_count);
  int body_end = body_end_is_implicit ? body_end_jmp : body_end_jmp - 1;
  int body_count = body_end - body_start + 1;
  if (body_count < 0)
    body_count = 0;
  if (body_count > 128)
    return 0;

  /* an lval that is not a local VAR dereferences a vreg-carried address; later passes mishandle those once rotated */
#define ROT_LVAL_IS_INDIRECT(op_)                                                                               \
  ((op_).is_lval && irop_get_vreg(op_) >= 0 &&                                                                  \
   !(TCCIR_DECODE_VREG_TYPE(irop_get_vreg(op_)) == TCCIR_VREG_TYPE_VAR && (op_).is_local))

  {
    int32_t iv_vr = irop_get_vreg(tcc_ir_op_get_src1(ir, cmp_q));
    int32_t seen_reads[32];
    int nseen_reads = 0;
    int32_t carried_defs[8];
    int ncarried_defs = 0;

#define ROT_NOTE_READ(op_)                                                                                       \
    do {                                                                                                         \
      int32_t _vr = irop_get_vreg(op_);                                                                          \
      if (_vr >= 0 && _vr != iv_vr && TCCIR_DECODE_VREG_TYPE(_vr) == TCCIR_VREG_TYPE_VAR) {                     \
        int _seen = 0;                                                                                           \
        for (int _k = 0; _k < nseen_reads; _k++)                                                                 \
          if (seen_reads[_k] == _vr) {                                                                           \
            _seen = 1;                                                                                           \
            break;                                                                                               \
          }                                                                                                      \
        if (!_seen && nseen_reads < (int)(sizeof(seen_reads) / sizeof(seen_reads[0])))                           \
          seen_reads[nseen_reads++] = _vr;                                                                       \
      }                                                                                                          \
    } while (0)

#define ROT_NOTE_DEF(op_)                                                                                        \
    do {                                                                                                         \
      int32_t _vr = irop_get_vreg(op_);                                                                          \
      if (_vr >= 0 && _vr != iv_vr && TCCIR_DECODE_VREG_TYPE(_vr) == TCCIR_VREG_TYPE_VAR) {                     \
        int _read = 0;                                                                                           \
        for (int _k = 0; _k < nseen_reads; _k++)                                                                 \
          if (seen_reads[_k] == _vr) {                                                                           \
            _read = 1;                                                                                           \
            break;                                                                                               \
          }                                                                                                      \
        if (_read) {                                                                                             \
          int _carried = 0;                                                                                      \
          for (int _k = 0; _k < ncarried_defs; _k++)                                                             \
            if (carried_defs[_k] == _vr) {                                                                       \
              _carried = 1;                                                                                      \
              break;                                                                                             \
            }                                                                                                    \
          if (!_carried && ncarried_defs < (int)(sizeof(carried_defs) / sizeof(carried_defs[0])))                \
            carried_defs[ncarried_defs++] = _vr;                                                                 \
        }                                                                                                        \
      }                                                                                                          \
    } while (0)

    for (int i = body_start; i <= body_end; i++)
    {
      IRQuadCompact *q = &ir->compact_instructions[i];
      int op = q->op;
      if (irop_config[op].has_src1)
        ROT_NOTE_READ(tcc_ir_op_get_src1(ir, q));
      if (irop_config[op].has_src2)
        ROT_NOTE_READ(tcc_ir_op_get_src2(ir, q));
      if (op == TCCIR_OP_MLA)
        ROT_NOTE_READ(tcc_ir_op_get_accum(ir, q));
      if (irop_config[op].has_dest)
        ROT_NOTE_DEF(tcc_ir_op_get_dest(ir, q));
    }
    /* Exactly one loop-carried non-IV VAR.  This is a PERFORMANCE gate, not a
     * correctness one.
     *
     * It used to be a correctness gate: raising it produced wrong code at -O2
     * (>2 broke 10 ir_tests, >3 broke 23, >8 broke 41).  That was not about
     * carried values at all — write_instr_at_nop does not copy orig_index, and
     * the barrel_shift / shift64_dead_half / bfi_params side tables are keyed by
     * it, so a relocated instruction silently lost its annotation (a fused
     * `add rd, rn, rm lsr #2` came back as a plain `add`).  The guard was
     * accidentally hiding that by keeping most annotated loops from rotating.
     * Fixed by preserving orig_index across the rewrite (see body_origs /
     * latch_origs); with that in place `> 2` passes all 2364 ir_tests.
     *
     * It stays at 1 because rotating multi-carried loops is a measured
     * PESSIMISATION: `> 2` costs +16,418 cycles on the ir suite (13,823,142 ->
     * 13,839,560) for zero byte change.  Each extra carried value needs a copy
     * at both loop exits and another live register, which outweighs the one
     * branch per iteration that rotation saves.  Raise it only with a cycle
     * measurement that says otherwise.
     *
     * (`> 8` additionally still fails 296_fuzz_assign_strd_deref_src at -O2, so
     * there is at least one more latent bug further out.) */
    if (ncarried_defs > 1)
    {
      LOG_LOOP_OPT("Rotation: reject — body carries %d non-IV VARs", ncarried_defs);
      return 0;
    }

#undef ROT_NOTE_READ
#undef ROT_NOTE_DEF
  }

  /* Indexed loads/stores in the body are NOT rejected: that guard was vestigial.
   * The operand save/restore below already carries the 4th pool slot they need
   * (body_has_extra), so the rewrite reconstructs them intact.  Dropping it is
   * what lets ordinary array loops (`for (i) s += p[i]`) bottom-test — worth
   * ~28k cycles on the ir suite at no size cost.  The indirect-lvalue guard
   * below is a genuinely different condition and stays.
   *
   * a call in the body makes forwarding treat preheader/body copies of a call-clobbered value as interchangeable */
  /* memoized across the body scan: the carried-constant probe walks the whole
   * function, and a body can hold several calls */
  int guard_folds = -1;
  for (int i = body_start; i <= body_end; i++)
  {
    int op = ir->compact_instructions[i].op;
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (op == TCCIR_OP_FUNCCALLVAL || op == TCCIR_OP_FUNCCALLVOID)
    {
      /* A call that can return stays rejected (see the forwarding note
       * above).  Two provably-safe exceptions, both of which make the
       * clobber unreachable from the loop:
       *   - a noreturn callee: control never comes back, so the clobber
       *     cannot reach a loop-carried value.  This is the `for (i)
       *     if (v[i] != K) abort();` check-loop shape — the single largest
       *     un-rotated family in the corpus (memclr / memcpy-a*);
       *   - any call inside the cond_body cold tail, whose terminating shape
       *     cold_terminates already proved never falls back into the loop. */
      Sym *callee = irop_get_sym_ex(ir, tcc_ir_op_get_src1(ir, q));
      int in_cold_tail = cond_body && cond_cold_start >= 0 && i >= cond_cold_start;
      if (!tcc_ir_callee_is_noreturn(callee) && !in_cold_tail)
      {
        LOG_LOOP_OPT("Rotation: reject — body has returning hot-path call at %d", i);
        return 0;
      }
      /* Call-body rotation is only profitable when it is free: the pre-loop
       * guard must provably fold (a surviving guard trades the back-branch for
       * CMP+branch: +2 per function on memclr loop 3), and an IV that is read
       * after the loop must not cost a merge copy at the exit (+40,942 corpus
       * instructions ungated).
       *
       * A folding guard settles the second condition too: with the guard gone
       * the rotated loop has exactly ONE exit, so the live-out IV is a single
       * value needing no phi — which is precisely the memclr shape (sequential
       * check loops sharing one `i`).  Only when the guard survives does the
       * two-exit merge appear, and only then does the liveness veto apply. */
      if (guard_folds < 0)
        guard_folds = rot_guard_provably_folds(ir, hi, cmp_q, cond);
      if (!guard_folds)
      {
        LOG_LOOP_OPT("Rotation: reject — call body with non-foldable entry guard");
        return 0;
      }
    }
    if ((irop_config[op].has_src1 && ROT_LVAL_IS_INDIRECT(tcc_ir_op_get_src1(ir, q))) ||
        (irop_config[op].has_src2 && ROT_LVAL_IS_INDIRECT(tcc_ir_op_get_src2(ir, q))) ||
        (op == TCCIR_OP_MLA && ROT_LVAL_IS_INDIRECT(tcc_ir_op_get_accum(ir, q))) ||
        ((op == TCCIR_OP_STORE || op == TCCIR_OP_STORE_POSTINC) &&
         irop_config[op].has_dest && ROT_LVAL_IS_INDIRECT(tcc_ir_op_get_dest(ir, q))))
    {
      LOG_LOOP_OPT("Rotation: reject — body has indirect lvalue operand at %d", i);
      return 0;
    }
  }
#undef ROT_LVAL_IS_INDIRECT

  /* reject a trailing body JUMPIF whose fall-through reaches the exit: rotation would redirect it to the latch */
  if (body_end_is_implicit && !break_invert)
  {
    int last_real = body_end;
    while (last_real >= body_start && ir->compact_instructions[last_real].op == TCCIR_OP_NOP)
      last_real--;
    if (last_real >= body_start && ir->compact_instructions[last_real].op == TCCIR_OP_JUMPIF)
    {
      int ft = last_real + 1;
      while (ft < n && ir->compact_instructions[ft].op == TCCIR_OP_NOP)
        ft++;
      if (ft >= exit_target)
      {
        LOG_LOOP_OPT("Rotation: reject — body JUMPIF at %d falls through to exit_target %d", last_real, exit_target);
        return 0;
      }
    }
  }

  /* reject external entries into the loop; jumps from inside the header/latch/body regions are normal control flow */
  {
    int ext_entry = 0;
    for (int j = 0; j < n && !ext_entry; j++)
    {
      if (j >= loop->start_idx && j <= loop->end_idx)
        continue;
      if (j >= body_start && j <= body_end_jmp)
        continue;
      IRQuadCompact *jq = &ir->compact_instructions[j];
      if (jq->op == TCCIR_OP_JUMP || jq->op == TCCIR_OP_JUMPIF)
      {
        IROperand jdest = tcc_ir_op_get_dest(ir, jq);
        int jtarget = (int)irop_get_imm64_ex(ir, jdest);
        if (jtarget > loop->start_idx && jtarget <= loop->end_idx)
          ext_entry = 1;
        if (jtarget >= body_start && jtarget <= body_end_jmp)
          ext_entry = 1;
      }
    }
    if (ext_entry)
    {
      LOG_LOOP_OPT("Rotation: reject — external entry into loop body/latch");
      return 0;
    }
  }

  int body_has_branches = 0;
  {
    int has_inner_loop = 0;
    for (int i = body_start; i <= body_end; i++)
    {
      IRQuadCompact *q = &ir->compact_instructions[i];
      if (q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_JUMPIF)
      {
        IROperand jd = tcc_ir_op_get_dest(ir, q);
        int jt = (int)irop_get_imm64_ex(ir, jd);
        if (jt < i && jt >= body_start)
        {
          has_inner_loop = 1;
          break;
        }
      }
    }

    int region_start_5 = hi + 2;
    for (int i = body_start; i <= body_end; i++)
    {
      IRQuadCompact *q = &ir->compact_instructions[i];
      if (q->op == TCCIR_OP_NOP)
        continue;
      if (q->op == TCCIR_OP_IJUMP)
        return 0;
      if (q->op == TCCIR_OP_JUMPIF || q->op == TCCIR_OP_JUMP)
      {
        IROperand jd = tcc_ir_op_get_dest(ir, q);
        int jt = (int)irop_get_imm64_ex(ir, jd);
        /* internal body branches are safe: their targets get remapped into the relocated body */
        if (jt >= body_start && jt <= body_end_jmp)
        {
          body_has_branches = 1;
          continue;
        }
        /* branch to latch region — will be remapped */
        if (jt >= latch_start && jt <= backedge_idx)
        {
          body_has_branches = 1;
          continue;
        }
        /* an escaping branch is only safe when an explicit trailing JUMP keeps every fall-through inside the body */
        if (!has_inner_loop && !cond_body && !break_invert && body_end_is_implicit)
          return 0;
        /* a target in the gap (body_end_jmp, exit_target) means the real body continues past body_end_jmp */
        if (jt > body_end_jmp && jt < exit_target)
        {
          LOG_LOOP_OPT("Rotation: reject — body branch at %d escapes to gap %d in (%d,%d)", i, jt, body_end_jmp,
                       exit_target);
          return 0;
        }
        /* branch outside modified region — no remap needed */
        if (jt < region_start_5 || jt > body_end_jmp)
        {
          body_has_branches = 1;
          continue;
        }
        return 0;
      }
    }
  }

  /* body_latch_target is the effective latch start — it may skip leading NOPs */
  int eff_latch_start = body_latch_target;
  int eff_latch_count = latch_end - eff_latch_start + 1;
  if (eff_latch_count < 0)
    eff_latch_count = 0;
  for (int i = eff_latch_start; i <= latch_end; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_JUMPIF || q->op == TCCIR_OP_JUMP)
    {
      LOG_LOOP_OPT("Rotation: reject — latch has branch at i=%d (op=%d)", i, q->op);
      return 0;
    }
    /* latch save/restore below only keeps dest/src1/src2, so a 4-operand op would lose pool[base+3] */
    if (q->op == TCCIR_OP_MLA || q->op == TCCIR_OP_SELECT ||
        q->op == TCCIR_OP_LOAD_INDEXED || q->op == TCCIR_OP_STORE_INDEXED ||
        q->op == TCCIR_OP_LOAD_POSTINC || q->op == TCCIR_OP_STORE_POSTINC)
    {
      LOG_LOOP_OPT("Rotation: reject — latch has 4-operand op at i=%d (op=%d)", i, q->op);
      return 0;
    }
  }

  int region_start = hi + 2;     /* first slot to overwrite (was body-entry JUMP) */
  int region_end = body_end_jmp; /* last slot to overwrite (was body→latch JUMP) */
  int avail_slots = region_end - region_start + 1;

  /* the bottom test falls through to region_end+1; an explicit JUMP is needed when that misses exit_target */
  int need_exit_jump = 0;
  {
    int ft = region_end + 1;
    while (ft < n && ir->compact_instructions[ft].op == TCCIR_OP_NOP)
      ft++;
    if (ft != exit_target)
      need_exit_jump = 1;
  }

  /* 2 = tail CMP + JUMPIF */
  int needed = body_count + eff_latch_count + 2 + need_exit_jump;
  if (needed > avail_slots)
  {
    LOG_LOOP_OPT("Rotation: reject — needed %d > avail %d", needed, avail_slots);
    return 0;
  }

  int inv_cond = invert_condition(cond);
  if (inv_cond < 0)
  {
    LOG_LOOP_OPT("Rotation: reject — cannot invert cond 0x%x", cond);
    return 0;
  }

  /* check invertibility before the destructive rewrite below, while bailing out is still possible */
  int break_decide_inv_cond = -1;
  if (break_invert)
  {
    IRQuadCompact *dq = &ir->compact_instructions[break_decide_idx];
    if (dq->op != TCCIR_OP_JUMPIF)
      return 0;
    IROperand dcond = tcc_ir_op_get_src1(ir, dq);
    break_decide_inv_cond = invert_condition((int)irop_get_imm64_ex(ir, dcond));
    if (break_decide_inv_cond < 0)
    {
      LOG_LOOP_OPT("Rotation: reject — cannot invert break decide cond");
      return 0;
    }
  }
  LOG_LOOP_OPT("Rotation: all checks passed, rotating!");

  /* save CMP operands for the tail test before the region is overwritten */
  IROperand cmp_src1 = tcc_ir_op_get_src1(ir, cmp_q);
  IROperand cmp_src2 = tcc_ir_op_get_src2(ir, cmp_q);

  /* IROperand is packed (9 bytes), so every sub-array of this scratch buffer must be explicitly 8-aligned */
  int bc = body_count, lc = eff_latch_count;
  size_t _rsz = bc * (3 * sizeof(int) + 4 * sizeof(IROperand) + sizeof(uint32_t))
              + lc * (2 * sizeof(int) + 3 * sizeof(IROperand) + sizeof(uint32_t))
              + 14 * 8; /* per-sub-array alignment padding (<=7 bytes each) */
  char *_rbuf = (char *)tcc_mallocz(_rsz);
  /* distinct offset variables: the self-host cross wrongly CSEs a running align-and-advance pointer */
#define _ROFF(prev, cnt, esz) ((((prev) + (size_t)(cnt) * (esz)) + 7u) & ~(size_t)7u)
  size_t _o_body_ops       = 0;
  size_t _o_body_has_extra = _ROFF(_o_body_ops, bc, sizeof(int));
  size_t _o_body_dests     = _ROFF(_o_body_has_extra, bc, sizeof(int));
  size_t _o_body_src1s     = _ROFF(_o_body_dests, bc, sizeof(IROperand));
  size_t _o_body_src2s     = _ROFF(_o_body_src1s, bc, sizeof(IROperand));
  size_t _o_body_extras    = _ROFF(_o_body_src2s, bc, sizeof(IROperand));
  size_t _o_body_lines     = _ROFF(_o_body_extras, bc, sizeof(IROperand));
  size_t _o_latch_ops      = _ROFF(_o_body_lines, bc, sizeof(uint32_t));
  size_t _o_latch_dests    = _ROFF(_o_latch_ops, lc, sizeof(int));
  size_t _o_latch_src1s    = _ROFF(_o_latch_dests, lc, sizeof(IROperand));
  size_t _o_latch_src2s    = _ROFF(_o_latch_src1s, lc, sizeof(IROperand));
  size_t _o_latch_lines    = _ROFF(_o_latch_src2s, lc, sizeof(IROperand));
  size_t _o_body_origs     = _ROFF(_o_latch_lines, lc, sizeof(uint32_t));
  size_t _o_latch_origs    = _ROFF(_o_body_origs, bc, sizeof(int));
#undef _ROFF
  int *body_ops          = (int *)(_rbuf + _o_body_ops);
  int *body_has_extra    = (int *)(_rbuf + _o_body_has_extra);
  IROperand *body_dests  = (IROperand *)(_rbuf + _o_body_dests);
  IROperand *body_src1s  = (IROperand *)(_rbuf + _o_body_src1s);
  IROperand *body_src2s  = (IROperand *)(_rbuf + _o_body_src2s);
  IROperand *body_extras = (IROperand *)(_rbuf + _o_body_extras);
  uint32_t *body_lines   = (uint32_t *)(_rbuf + _o_body_lines);
  int *latch_ops         = (int *)(_rbuf + _o_latch_ops);
  IROperand *latch_dests = (IROperand *)(_rbuf + _o_latch_dests);
  IROperand *latch_src1s = (IROperand *)(_rbuf + _o_latch_src1s);
  IROperand *latch_src2s = (IROperand *)(_rbuf + _o_latch_src2s);
  uint32_t *latch_lines  = (uint32_t *)(_rbuf + _o_latch_lines);
  /* orig_index keys the per-instruction side tables (barrel_shifts,
   * shift64_dead_half, bfi_params).  write_instr_at_nop does not set it, so a
   * moved instruction would inherit the destination NOP's index and silently
   * lose its annotation — a fused `add rd, rn, rm lsr #2` came back as a plain
   * `add rd, rn, rm`, dropping the shift (probe: an inlined hash mixer's
   * `h + (h >> 2)` in a rotated loop). */
  int *body_origs        = (int *)(_rbuf + _o_body_origs);
  int *latch_origs       = (int *)(_rbuf + _o_latch_origs);

  for (int b = 0; b < body_count; b++)
  {
    IRQuadCompact *bq = &ir->compact_instructions[body_start + b];
    int op = bq->op;
    body_ops[b] = op;
    body_lines[b] = bq->line_num;
    body_origs[b] = bq->orig_index;
    /* _rbuf is pre-zeroed; never write (IROperand){0} into it — the packed operand lowers to an unaligned STRD */
    /* ops carrying a 4th pool operand: dropping it would rebuild e.g. a SELECT with cond=0 */
    body_has_extra[b] = (op == TCCIR_OP_MLA || op == TCCIR_OP_LOAD_INDEXED ||
                         op == TCCIR_OP_STORE_INDEXED || op == TCCIR_OP_SELECT ||
                         op == TCCIR_OP_LOAD_POSTINC || op == TCCIR_OP_STORE_POSTINC);
    if (irop_config[op].has_dest)
      body_dests[b] = ir->iroperand_pool[bq->operand_base];
    if (irop_config[op].has_src1)
      body_src1s[b] = ir->iroperand_pool[bq->operand_base + irop_config[op].has_dest];
    if (irop_config[op].has_src2)
      body_src2s[b] = ir->iroperand_pool[bq->operand_base + irop_config[op].has_dest + irop_config[op].has_src1];
    if (body_has_extra[b])
      body_extras[b] = ir->iroperand_pool[bq->operand_base + 3];
  }

  for (int l = 0; l < eff_latch_count; l++)
  {
    IRQuadCompact *lq = &ir->compact_instructions[eff_latch_start + l];
    int op = lq->op;
    latch_ops[l] = op;
    latch_lines[l] = lq->line_num;
    latch_origs[l] = lq->orig_index;
    if (irop_config[op].has_dest)
      latch_dests[l] = ir->iroperand_pool[lq->operand_base];
    if (irop_config[op].has_src1)
      latch_src1s[l] = ir->iroperand_pool[lq->operand_base + irop_config[op].has_dest];
    if (irop_config[op].has_src2)
      latch_src2s[l] = ir->iroperand_pool[lq->operand_base + irop_config[op].has_dest + irop_config[op].has_src1];
  }

  for (int i = region_start; i <= region_end; i++)
  {
    ir->compact_instructions[i].op = TCCIR_OP_NOP;
    ir->compact_instructions[i].is_jump_target = 0;
  }

  int wp = region_start;

  int body_target = wp; /* back-edge will target this */
  for (int b = 0; b < body_count; b++)
  {
    write_instr_at_nop(ir, wp, body_ops[b], body_dests[b], body_src1s[b], body_src2s[b]);
    if (body_has_extra[b])
      tcc_ir_pool_add(ir, body_extras[b]); /* 4th operand at operand_base+3 */
    ir->compact_instructions[wp].line_num = body_lines[b];
    ir->compact_instructions[wp].orig_index = body_origs[b];
    wp++;
  }

  if (body_has_branches)
  {
    int body_offset = region_start - body_start;
    int latch_new_start = region_start + body_count;
    int latch_offset = latch_new_start - eff_latch_start;
    for (int i = body_target; i < body_target + body_count; i++)
    {
      IRQuadCompact *q = &ir->compact_instructions[i];
      if (q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_JUMPIF)
      {
        IROperand *dest = &ir->iroperand_pool[q->operand_base];
        int old_target = dest->u.imm32;
        int new_target = -1;
        if (old_target >= body_start && old_target <= body_end_jmp)
        {
          new_target = old_target + body_offset;
        }
        else if (old_target >= eff_latch_start && old_target <= latch_end)
        {
          new_target = old_target + latch_offset;
        }
        if (new_target >= 0)
        {
          dest->u.imm32 = new_target;
          if (new_target < n)
            ir->compact_instructions[new_target].is_jump_target = 1;
        }
      }
    }
  }

  /* the latch now follows the deciding JUMPIF, so retarget it to the exit and let the fall-through continue */
  if (break_invert)
  {
    int decide_pos = body_target + body_count - 1;
    IRQuadCompact *dq = &ir->compact_instructions[decide_pos];
    if (dq->op == TCCIR_OP_JUMPIF)
    {
      IROperand *ddest = &ir->iroperand_pool[dq->operand_base];
      IROperand *dcond = &ir->iroperand_pool[dq->operand_base + 1];
      ddest->u.imm32 = exit_target;
      dcond->u.imm32 = break_decide_inv_cond;
      if (exit_target < n)
        ir->compact_instructions[exit_target].is_jump_target = 1;
    }
  }

  /* latch instructions only — the back-edge JUMP is replaced by the tail test below */
  for (int l = 0; l < eff_latch_count; l++)
  {
    write_instr_at_nop(ir, wp, latch_ops[l], latch_dests[l], latch_src1s[l], latch_src2s[l]);
    ir->compact_instructions[wp].line_num = latch_lines[l];
    ir->compact_instructions[wp].orig_index = latch_origs[l];
    wp++;
  }

  write_instr_at_nop(ir, wp, TCCIR_OP_CMP, (IROperand){0}, cmp_src1, cmp_src2);
  wp++;

  {
    IROperand jmp_dest = irop_make_imm32(-1, body_target, IROP_BTYPE_INT32);
    IROperand inv_cond_op = irop_make_imm32(-1, inv_cond, IROP_BTYPE_INT32);
    write_instr_at_nop(ir, wp, TCCIR_OP_JUMPIF, jmp_dest, inv_cond_op, (IROperand){0});
    wp++;
  }

  if (need_exit_jump)
  {
    IROperand exit_dest = irop_make_imm32(-1, exit_target, IROP_BTYPE_INT32);
    write_instr_at_nop(ir, wp, TCCIR_OP_JUMP, exit_dest, (IROperand){0}, (IROperand){0});
    wp++;
  }

  ir->compact_instructions[body_target].is_jump_target = 1;

  /* the header CMP and exit_target keep their is_jump_target flags: gotos and the guard JUMPIF may still target them */

  LOG_IR_GEN("[LOOP-ROTATE] Rotated loop header=%d body=[%d..%d] latch=[%d..%d] → bottom-tested at %d", hi, body_start,
             body_end, latch_start, latch_end, body_target);

  tcc_free(_rbuf);
  return 1;
}

int loop_size_cmp(const void *a, const void *b)
{
  const IRLoop *la = (const IRLoop *)a;
  const IRLoop *lb = (const IRLoop *)b;
  int sa = la->end_idx - la->start_idx;
  int sb = lb->end_idx - lb->start_idx;
  return sa - sb;
}
