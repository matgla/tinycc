/*
 *  TCC IR - If-conversion: control-flow diamonds -> SELECT (pre- and post-RA)
 *
 *  Passes:
 *    setif_neg_to_select     - `0 - (cond?1:0)` -> SELECT(-1,0,cond)
 *    select                  - if/else value diamond -> SELECT
 *    post_ra_forward_diamond - post-RA coalesced forward diamond -> inverted JUMPIF
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
#include "opt_utils.h"
#include "opt_loop_utils.h"
#include "machine_op.h"
#include "opt/flat/if_convert.h"
#include "memory/small_sequence.h"

/* Inline-first per-instruction scratch (heap only past the inline cap). */
TCC_SMALL_SEQUENCE_DEFINE(IfcIntSeq, int, 256)

/* ============================================================================
 * SETIF + negate → SELECT mask
 *
 * The element-wise vector compare idiom `(a CMP b) ? -1 : 0` (e.g. the lowered
 * `*p = (*p ^ *q) == *q` in gcc.c-torture/compile/pr54713-3.c) emits, per
 * element:
 *     CMP a, b
 *     t <- SETIF(cond)     ; ITE cond; mov dst,#1; mov dst,#0     (3 insns)
 *     r <- #0 SUB t        ; rsb r, t, #0                          (1 insn)
 * Folding the negate into a SELECT(#-1, #0, cond) and dropping the now-dead
 * SETIF yields:
 *     CMP a, b
 *     r <- SELECT(#-1, #0, cond)  ; ITE cond; mvn r,#0; mov r,#0   (3 insns)
 * one instruction shorter per mask.  `0 - (cond ? 1 : 0)` equals
 * `cond ? -1 : 0` for every condition, so the rewrite is value-identical
 * regardless of which comparison cond encodes.
 *
 * Runs LATE (right after tcc_ir_opt_select, past the whole optimization
 * pipeline) for the same reason opt_promote's diamond→SELECT does: no
 * orphan-CMP pass runs afterward to mistake the flag-setting CMP — which the
 * new SELECT consumes only via flags, with no vreg link — for dead code, and
 * no value-tracking pass remains to mis-fold `(SELECT result) == const`.  The
 * resulting CMP+SELECT shape is exactly the one tcc_ir_opt_select already
 * produces here and that survives regalloc → codegen unchanged.
 *
 * Gates (all required):
 *   - the SETIF result feeds exactly one instruction;
 *   - that instruction is the immediately-following `r <- #0 SUB t`
 *     (src1 a literal 0, src2 the SETIF dest);
 *   - a flag-setting CMP/TEST_ZERO is the instruction immediately before the
 *     SETIF, so the CMP's flags reach the SELECT with nothing clobbering them
 *     in between (only the NOPed SETIF, plus NOPs, sit between).
 * ============================================================================ */
int tcc_ir_opt_setif_neg_to_select(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  int changes = 0;

  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *sif = &ir->compact_instructions[i];
    if (sif->op != TCCIR_OP_SETIF)
      continue;

    IROperand sif_dest = tcc_ir_op_get_dest(ir, sif);
    int32_t t = irop_get_vreg(sif_dest);
    if (t < 0)
      continue;

    /* The negate must be the next non-NOP instruction. */
    int j = ir_skip_nops_forward(ir, i + 1, n);
    if (j >= n)
      continue;
    IRQuadCompact *sub = &ir->compact_instructions[j];
    if (sub->op != TCCIR_OP_SUB)
      continue;

    IROperand sub_s1 = tcc_ir_op_get_src1(ir, sub);
    IROperand sub_s2 = tcc_ir_op_get_src2(ir, sub);
    if (!irop_is_immediate(sub_s1) || sub_s1.is_sym || irop_get_imm64_ex(ir, sub_s1) != 0)
      continue;
    if (irop_get_vreg(sub_s2) != t)
      continue;

    /* The SETIF result must feed only the negate, so NOPing it is safe. */
    if (!tcc_ir_vreg_has_single_use(ir, t, -1))
      continue;

    /* A flag setter must immediately precede the SETIF: with the SETIF NOPed
     * the SELECT inherits those flags, and nothing between clobbers them. */
    int k = i - 1;
    while (k >= 0 && ir->compact_instructions[k].op == TCCIR_OP_NOP)
      k--;
    if (k < 0)
      continue;
    int prev_op = ir->compact_instructions[k].op;
    if (prev_op != TCCIR_OP_CMP && prev_op != TCCIR_OP_TEST_ZERO)
      continue;

    IROperand sif_cond = tcc_ir_op_get_src1(ir, sif);
    if (!irop_is_immediate(sif_cond) || sif_cond.is_sym)
      continue;
    int cond = (int)irop_get_imm64_ex(ir, sif_cond);

    /* Rewrite the negate in place as SELECT(#-1, #0, cond), reusing its dest. */
    IROperand sub_dest = tcc_ir_op_get_dest(ir, sub);
    int dest_btype = irop_get_btype(sub_dest);
    if (!irop_btype_select_lowerable(dest_btype))
      continue;
    IROperand then_v = irop_make_imm32(-1, -1, dest_btype);
    IROperand else_v = irop_make_imm32(-1, 0, dest_btype);
    IROperand cond_op = irop_make_imm32(-1, cond, VT_INT);

    int pool_base = tcc_ir_iroperand_pool_add(ir, sub_dest);
    tcc_ir_iroperand_pool_add(ir, then_v);
    tcc_ir_iroperand_pool_add(ir, else_v);
    tcc_ir_iroperand_pool_add(ir, cond_op);

    sub->op = TCCIR_OP_SELECT;
    sub->operand_base = pool_base;

    sif->op = TCCIR_OP_NOP;
    changes++;
  }

  return changes;
}

/* A diamond arm's value can be hoisted into an unconditionally-evaluated SELECT
 * only if reading it cannot fault or observe side effects.  An immediate is
 * always safe.  A plain non-lvalue register/param value is safe for integer
 * types; a LOAD through an lvalue (a pointer/global memory dereference —
 * is_lval) may fault on the not-taken path or be volatile, so it stays a
 * branch.  Float/double variable arms are excluded: their condition comes from
 * a soft-float compare CALL, whose flag/condition handling the SELECT lowering
 * does not yet reproduce (min/max float SELECT is a separate stage). */
static int ir_ifconv_arm_value_safe(TCCIRState *ir, const IRQuadCompact *q)
{
  IROperand v = tcc_ir_op_get_src1(ir, q);
  if (irop_is_immediate(v))
    return 1;
  if (irop_op_is_lval(v))
    return 0;
  int bt = irop_get_btype(v);
  return bt != IROP_BTYPE_FLOAT32 && bt != IROP_BTYPE_FLOAT64;
}

/* A then-arm compute may be speculated (run unconditionally before the SELECT)
 * only if it is a pure, non-faulting integer ALU op: no division (traps on
 * zero), no carry-dependent/multi-result forms (they read/produce flags), no
 * memory or calls.  The compute is the first then-block instruction so its
 * operands are already available before the diamond — hoisting cannot read an
 * undefined value.  The resulting `sub.w`/`add.w` is emitted flags-safe (the
 * CMP's flags stay live to the SELECT). */
static int ir_ifconv_is_safe_compute(TccIrOp op)
{
  switch (op)
  {
  case TCCIR_OP_ADD:
  case TCCIR_OP_SUB:
  case TCCIR_OP_MUL:
  case TCCIR_OP_AND:
  case TCCIR_OP_OR:
  case TCCIR_OP_XOR:
  case TCCIR_OP_SHL:
  case TCCIR_OP_SAR:
  case TCCIR_OP_SHR:
    return 1;
  default:
    return 0;
  }
}

int tcc_ir_ifconv_match_predicated_select(TCCIRState *ir, int i, int *sel_index_out, int *cond_out)
{
  IRQuadCompact *cq = &ir->compact_instructions[i];

  /* IR-level "can predicate" gate (mirror of the backend's negate check):
   * a 32-bit reverse-subtract negate `dest = 0 - src2`. */
  if (cq->op != TCCIR_OP_SUB)
    return 0;
  IROperand csrc1 = tcc_ir_op_get_src1(ir, cq);
  IROperand cdest = tcc_ir_op_get_dest(ir, cq);
  if (!(irop_is_immediate(csrc1) && irop_get_imm64_ex(ir, csrc1) == 0))
    return 0;
  if (irop_get_btype(cdest) == IROP_BTYPE_INT64)
    return 0;
  int32_t d_vr = irop_get_vreg(cdest);
  if (d_vr < 0)
    return 0;

  int sj = ir_skip_nops_forward(ir, i + 1, ir->next_instruction_index);
  if (sj >= ir->next_instruction_index || ir->compact_instructions[sj].op != TCCIR_OP_SELECT)
    return 0;
  IRQuadCompact *sq = &ir->compact_instructions[sj];

  /* SELECT then-arm is this compute's (single-use) result. */
  if (irop_get_vreg(tcc_ir_op_get_src1(ir, sq)) != d_vr ||
      !tcc_ir_vreg_has_single_use(ir, d_vr, -1))
    return 0;

  /* else-identity (post-RA): the SELECT else-arm and dest are the same
   * physical register, so the else path is a no-op and only the predicated
   * then-compute need be emitted. */
  IROperand s_dest = tcc_ir_op_get_dest(ir, sq);
  IROperand s_else = tcc_ir_op_get_src2(ir, sq);
  MachineOperand mdst = machine_op_from_ir(ir, &s_dest);
  MachineOperand mels = machine_op_from_ir(ir, &s_else);
  if (mdst.kind != MACH_OP_REG || mels.kind != MACH_OP_REG ||
      mdst.needs_deref || mels.needs_deref || mdst.u.reg.r0 != mels.u.reg.r0)
    return 0;

  *sel_index_out = sj;
  *cond_out = (int)irop_get_imm64_ex(ir, tcc_ir_op_get_cond(ir, sq));
  return 1;
}

int tcc_ir_opt_select(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  int changes = 0;

  /* Smallest pattern is the RETURN diamond: JUMPIF + RETURN + RETURN = 3 instr.
   * Other patterns (ASSIGN/CALL diamond) need more, but their inner checks
   * already bail out when the remaining suffix is too short. */
  if (n < 3)
    return 0;

  /* Precompute jump target counts: jt_cnt[target] = number of JUMP/JUMPIFs
   * targeting `target`. Lets ir_has_other_jump_to_fast (below) answer in O(1)
   * what would otherwise be an O(n) scan per query — opt_select calls the
   * predicate four times per JUMPIF, which on a function with thousands of
   * branches drives the pass into O(n^2). Decrement the count whenever we
   * NOP a JUMP/JUMPIF below to keep it consistent. */
  small_sequence(IfcIntSeq) jt_cnt_owner = {0};
  IfcIntSeq_init(&jt_cnt_owner, (size_t)n);
  int *jt_cnt = IfcIntSeq_data(&jt_cnt_owner);
  for (int j = 0; j < n; j++) {
    IRQuadCompact *q = &ir->compact_instructions[j];
    if (q->op != TCCIR_OP_JUMP && q->op != TCCIR_OP_JUMPIF)
      continue;
    IROperand d = tcc_ir_op_get_dest(ir, q);
    int t = (int)irop_get_imm64_ex(ir, d);
    if (t >= 0 && t < n)
      jt_cnt[t]++;
  }
  #define JT_HAS_OTHER(target, exclude_idx) \
    ir_has_other_jump_to_fast(ir, jt_cnt, (target), (exclude_idx))
  #define JT_NOP_JUMP(idx) do { \
    IRQuadCompact *_jq = &ir->compact_instructions[idx]; \
    if (_jq->op == TCCIR_OP_JUMP || _jq->op == TCCIR_OP_JUMPIF) { \
      IROperand _jd = tcc_ir_op_get_dest(ir, _jq); \
      int _jt = (int)irop_get_imm64_ex(ir, _jd); \
      if (_jt >= 0 && _jt < n && jt_cnt[_jt] > 0) jt_cnt[_jt]--; \
    } \
    _jq->op = TCCIR_OP_NOP; \
  } while (0)

  for (int i = 0; i < n - 2; i++)
  {
    IRQuadCompact *jumpif_q = &ir->compact_instructions[i];
    if (jumpif_q->op != TCCIR_OP_JUMPIF)
      continue;

    /* Get JUMPIF operands: dest=else_target, src1=condition */
    IROperand jumpif_dest = tcc_ir_op_get_dest(ir, jumpif_q);
    IROperand jumpif_cond = tcc_ir_op_get_src1(ir, jumpif_q);
    int branch_cond = (int)irop_get_imm64_ex(ir, jumpif_cond);
    int else_target = (int)irop_get_imm64_ex(ir, jumpif_dest);

    /* Normalize `JUMPIF C → A; JUMP → B` into `JUMPIF !C → B` when A is the
     * instruction immediately following the JUMP.  Without this, a ternary
     * lowering whose true-path falls through and false-path is an
     * unconditional JUMP keeps an extra branch that defeats the diamond
     * patterns below.  Conditions: the JUMP must be unconditional, nothing
     * else targets its position, and the original else_target (A) must be
     * the next real instruction after the JUMP. */
    if (else_target >= 0 && else_target < n) {
      int next_nop = ir_skip_nops_forward(ir, i + 1, n);
      if (next_nop < n) {
        IRQuadCompact *next_q = &ir->compact_instructions[next_nop];
        if (next_q->op == TCCIR_OP_JUMP &&
            !JT_HAS_OTHER(next_nop, -1) &&
            ir_skip_nops_forward(ir, next_nop + 1, n) == else_target) {
          IROperand new_target = tcc_ir_op_get_dest(ir, next_q);
          int new_else_target = (int)irop_get_imm64_ex(ir, new_target);
          if (new_else_target >= 0 && new_else_target < n) {
            jt_cnt[else_target]--;
            jt_cnt[new_else_target]++;
            tcc_ir_op_set_dest(ir, jumpif_q, new_target);
            IROperand new_cond = irop_make_imm32(-1, ir_negate_condition(branch_cond), VT_INT);
            tcc_ir_op_set_src1(ir, jumpif_q, new_cond);
            JT_NOP_JUMP(next_nop);
            if (!JT_HAS_OTHER(else_target, -1))
              ir->compact_instructions[else_target].is_jump_target = 0;
            branch_cond = ir_negate_condition(branch_cond);
            else_target = new_else_target;
            changes++;
          }
        }
      }
    }

    /* The "then" condition is the negation of the branch condition
     * (branch jumps to else when cond is true, so then runs when !cond) */
    int then_cond = ir_negate_condition(branch_cond);

    /* Scan forward past NOPs to find the then-block start */
    int then_start = ir_skip_nops_forward(ir, i + 1, n);
    if (then_start >= n)
      continue;

    /* Safety: the then-block (fall-through) must not be a jump target from
     * elsewhere, and the else-block must only be targeted by this JUMPIF.
     * Otherwise NOP'ing the blocks would break other control flow. */
    if (JT_HAS_OTHER(then_start, i))
      continue;
    if (JT_HAS_OTHER(else_target, i))
      continue;

    /* ----------------------------------------------------------------
     * Pattern: Call diamond (PARAM+CALL in both branches)
     * ----------------------------------------------------------------
     * then: PARAM0[call_A] val1, CALL func
     * JUMP to merge
     * else: PARAM0[call_B] val2, CALL func
     * merge: ...
     * ---------------------------------------------------------------- */
    IRQuadCompact *then_q1 = &ir->compact_instructions[then_start];
    if (then_q1->op == TCCIR_OP_FUNCPARAMVAL || then_q1->op == TCCIR_OP_FUNCPARAMVOID)
    {
      /* Check if next non-NOP is a FUNCCALLVOID */
      int then_call_idx = ir_skip_nops_forward(ir, then_start + 1, n);
      if (then_call_idx >= n)
        continue;
      IRQuadCompact *then_call_q = &ir->compact_instructions[then_call_idx];
      if (then_call_q->op != TCCIR_OP_FUNCCALLVOID)
        continue;

      /* Next should be unconditional JUMP to merge */
      int jump_idx = ir_skip_nops_forward(ir, then_call_idx + 1, n);
      if (jump_idx >= n)
        continue;
      IRQuadCompact *jump_q = &ir->compact_instructions[jump_idx];
      if (jump_q->op != TCCIR_OP_JUMP)
        continue;
      int merge_target = (int)irop_get_imm64_ex(ir, tcc_ir_op_get_dest(ir, jump_q));

      /* else_target should point to the else block */
      if (else_target < 0 || else_target >= n)
        continue;

      /* Find else block start (skip NOPs) */
      int else_start = ir_skip_nops_forward(ir, else_target, n);
      if (else_start >= n)
        continue;

      /* Else block: PARAM + CALL same function */
      IRQuadCompact *else_q1 = &ir->compact_instructions[else_start];
      if (else_q1->op != TCCIR_OP_FUNCPARAMVAL && else_q1->op != TCCIR_OP_FUNCPARAMVOID)
        continue;

      int else_call_idx = ir_skip_nops_forward(ir, else_start + 1, n);
      if (else_call_idx >= n)
        continue;
      IRQuadCompact *else_call_q = &ir->compact_instructions[else_call_idx];
      if (else_call_q->op != TCCIR_OP_FUNCCALLVOID)
        continue;

      /* Verify both calls target the same function */
      Sym *then_callee = irop_get_sym_ex(ir, tcc_ir_op_get_src1(ir, then_call_q));
      Sym *else_callee = irop_get_sym_ex(ir, tcc_ir_op_get_src1(ir, else_call_q));
      if (!then_callee || !else_callee || then_callee != else_callee)
        continue;

      /* Verify both params are param index 0 for their respective calls */
      IROperand then_param_enc = tcc_ir_op_get_src2(ir, then_q1);
      IROperand else_param_enc = tcc_ir_op_get_src2(ir, else_q1);
      int then_param_idx = TCCIR_DECODE_PARAM_IDX((uint32_t)irop_get_imm64_ex(ir, then_param_enc));
      int else_param_idx = TCCIR_DECODE_PARAM_IDX((uint32_t)irop_get_imm64_ex(ir, else_param_enc));
      if (then_param_idx != 0 || else_param_idx != 0)
        continue;

      /* Both calls have 1 argument (CALL #N where argc from encoded src2) */
      IROperand then_call_meta = tcc_ir_op_get_src2(ir, then_call_q);
      IROperand else_call_meta = tcc_ir_op_get_src2(ir, else_call_q);
      int then_argc = TCCIR_DECODE_CALL_ARGC((uint32_t)irop_get_imm64_ex(ir, then_call_meta));
      int else_argc = TCCIR_DECODE_CALL_ARGC((uint32_t)irop_get_imm64_ex(ir, else_call_meta));
      if (then_argc != 1 || else_argc != 1)
        continue;

      /* Verify merge_target is right after the else CALL (or after NOPs) */
      int after_else_call = ir_skip_nops_forward(ir, else_call_idx + 1, n);
      if (after_else_call != merge_target && else_call_idx + 1 != merge_target)
      {
        /* Also accept if merge_target equals the instruction after else_call directly */
        int next_real = ir_skip_nops_forward(ir, else_call_idx + 1, n);
        if (next_real != merge_target)
          continue;
      }

      /* Get the differing parameter values */
      IROperand then_val = tcc_ir_op_get_src1(ir, then_q1);
      IROperand else_val = tcc_ir_op_get_src1(ir, else_q1);

      /* Both must be compile-time constants (SYMREF or IMM32) — no vreg uses
       * since the vreg lifetimes would be disrupted by removing the branches */
      int then_tag = irop_get_tag(then_val);
      int else_tag = irop_get_tag(else_val);
      if (then_tag != IROP_TAG_SYMREF && then_tag != IROP_TAG_IMM32)
        continue;
      if (else_tag != IROP_TAG_SYMREF && else_tag != IROP_TAG_IMM32)
        continue;
      if (!irop_btype_select_lowerable(irop_get_btype(then_val)) ||
          !irop_btype_select_lowerable(irop_get_btype(else_val)))
        continue;

      /* ---- Transform ---- */

      /* Create a new temporary vreg for the SELECT result */
      int32_t select_vreg = tcc_ir_get_vreg_temp(ir);

      /* Allocate 4 pool entries for SELECT: dest, src1(then), src2(else), cond */
      IROperand sel_dest = irop_make_vreg(select_vreg, IROP_BTYPE_INT32);

      IROperand sel_cond = irop_make_imm32(-1, then_cond, VT_INT);

      int pool_base = tcc_ir_iroperand_pool_add(ir, sel_dest);
      tcc_ir_iroperand_pool_add(ir, then_val);
      tcc_ir_iroperand_pool_add(ir, else_val);
      tcc_ir_iroperand_pool_add(ir, sel_cond);

      /* Rewrite the JUMPIF as SELECT (drops one jump to else_target) */
      if (else_target >= 0 && else_target < n && jt_cnt[else_target] > 0)
        jt_cnt[else_target]--;
      jumpif_q->op = TCCIR_OP_SELECT;
      jumpif_q->operand_base = pool_base;

      /* Rewrite the then PARAM to use the SELECT result vreg,
       * and keep it pointing to the else call (which we'll keep) */
      /* Actually, we need to rewrite the else_q1 (PARAM) to use the
       * SELECT vreg as its value, then NOP the then-block entirely */

      /* Rewrite else PARAM0 to use the SELECT result */
      IROperand new_param_val = irop_make_vreg(select_vreg, IROP_BTYPE_INT32);
      tcc_ir_op_set_src1(ir, else_q1, new_param_val);

      /* NOP the then-block: then_param, then_call, unconditional jump */
      ir->compact_instructions[then_start].op = TCCIR_OP_NOP;
      ir->compact_instructions[then_call_idx].op = TCCIR_OP_NOP;
      JT_NOP_JUMP(jump_idx);

      /* Clear stale is_jump_target on positions no longer targeted
       * (the NOPed JUMP no longer jumps, so re-check with no exclusion) */
      if (!JT_HAS_OTHER(else_target, -1))
        ir->compact_instructions[else_target].is_jump_target = 0;
      if (merge_target >= 0 && merge_target < n && !JT_HAS_OTHER(merge_target, -1))
        ir->compact_instructions[merge_target].is_jump_target = 0;

      changes++;
      continue;
    }

    /* ----------------------------------------------------------------
     * Pattern: Simple ASSIGN diamond
     * ----------------------------------------------------------------
     * then: dest <-- val1 [ASSIGN, or LOAD of an immediate]
     * JUMP to merge
     * else: dest <-- val2 [same op]
     * merge: ...
     *
     * The frontend emits `T <-- #c [LOAD]` for `?:` ternary arms that are
     * integer constants — it's a register materialization, not a memory
     * load.  Accepting that form lets us fold `r = (c) ? 0 : 1` into a
     * SELECT/IT-block instead of a jump diamond.  A LOAD arm whose source is a
     * plain non-lvalue value (e.g. a param, `x>c?x:k`) is also foldable —
     * gated by ir_ifconv_arm_value_safe so a faulting deref is left alone. */
    int then_is_load = (then_q1->op == TCCIR_OP_LOAD);
    if (then_q1->op == TCCIR_OP_ASSIGN ||
        (then_is_load && ir_ifconv_arm_value_safe(ir, then_q1)))
    {
      IROperand then_dest = tcc_ir_op_get_dest(ir, then_q1);
      IROperand then_val = tcc_ir_op_get_src1(ir, then_q1);
      int32_t dest_vreg = irop_get_vreg(then_dest);

      /* Next should be unconditional JUMP to merge */
      int jump_idx = ir_skip_nops_forward(ir, then_start + 1, n);
      if (jump_idx >= n)
        continue;
      IRQuadCompact *jump_q = &ir->compact_instructions[jump_idx];
      if (jump_q->op != TCCIR_OP_JUMP)
        continue;

      /* Find else block */
      int else_start = ir_skip_nops_forward(ir, else_target, n);
      if (else_start >= n)
        continue;
      IRQuadCompact *else_q = &ir->compact_instructions[else_start];
      if (else_q->op != then_q1->op)
        continue;
      if (then_is_load && !ir_ifconv_arm_value_safe(ir, else_q))
        continue;

      /* Same destination vreg */
      IROperand else_dest = tcc_ir_op_get_dest(ir, else_q);
      IROperand else_val = tcc_ir_op_get_src1(ir, else_q);
      if (irop_get_vreg(else_dest) != dest_vreg)
        continue;

      /* The else block must be exactly one ASSIGN.  After it, the next
       * instruction must be the merge point (the JMP target from then).
       * Otherwise the else block has more instructions and it's not a
       * simple diamond — NOP'ing the else ASSIGN would break the rest. */
      int merge_target = (int)irop_get_imm64_ex(ir, tcc_ir_op_get_dest(ir, jump_q));
      int after_else = ir_skip_nops_forward(ir, else_start + 1, n);
      if (after_else != merge_target)
        continue;

      if (!irop_btype_select_lowerable(irop_get_btype(then_dest)) ||
          !irop_btype_select_lowerable(irop_get_btype(then_val)) ||
          !irop_btype_select_lowerable(irop_get_btype(else_val)))
        continue;

      /* Allocate 4 pool entries for SELECT */
      IROperand sel_cond = irop_make_imm32(-1, then_cond, VT_INT);
      int pool_base = tcc_ir_iroperand_pool_add(ir, then_dest);
      tcc_ir_iroperand_pool_add(ir, then_val);
      tcc_ir_iroperand_pool_add(ir, else_val);
      tcc_ir_iroperand_pool_add(ir, sel_cond);

      /* Rewrite JUMPIF as SELECT (drops one jump to else_target) */
      if (else_target >= 0 && else_target < n && jt_cnt[else_target] > 0)
        jt_cnt[else_target]--;
      jumpif_q->op = TCCIR_OP_SELECT;
      jumpif_q->operand_base = pool_base;

      /* NOP the then-assign, jump, and else-assign */
      ir->compact_instructions[then_start].op = TCCIR_OP_NOP;
      JT_NOP_JUMP(jump_idx);
      ir->compact_instructions[else_start].op = TCCIR_OP_NOP;

      /* Clear stale is_jump_target on positions no longer targeted
       * (the NOPed JUMP no longer jumps, so re-check with no exclusion) */
      if (!JT_HAS_OTHER(else_target, -1))
        ir->compact_instructions[else_target].is_jump_target = 0;
      if (merge_target >= 0 && merge_target < n && !JT_HAS_OTHER(merge_target, -1))
        ir->compact_instructions[merge_target].is_jump_target = 0;

      changes++;
      continue;
    }

    /* ----------------------------------------------------------------
     * Pattern: Computed-arm merge-temp diamond (the abs2 `x<0?-x:x` shape)
     * ----------------------------------------------------------------
     * then:  Tc <- <pure ALU compute>        (then_q1; dest Tc, single-use)
     *        JUMP -> mvia
     * else:  Tm <- Vel [LOAD/ASSIGN]          (safe value; writes merge var)
     *        JUMP -> merge
     * mvia:  Tm <- Tc [ASSIGN]                (then-side merge-assign)
     * merge: ... uses Tm ...
     *
     * Collapse to: keep the pure compute (now unconditional) and rewrite the
     * merge-assign as `Tm <- SELECT(Tc, Vel, then_cond)`.  The compute stays
     * between the CMP and the SELECT but is emitted flags-safe (non-flag
     * `sub.w`), so the CMP's condition still reaches the SELECT. */
    if (ir_ifconv_is_safe_compute(then_q1->op))
    {
      IROperand tc = tcc_ir_op_get_dest(ir, then_q1);
      int32_t tc_vr = irop_get_vreg(tc);
      int tc_bt = irop_get_btype(tc);
      /* 32-bit-or-narrower integer only: a 64-bit ALU op lowers to a SUBS/SBCS
       * carry chain that is forced flag-setting, which would clobber the CMP's
       * condition before the SELECT reads it (llabs miscompiled otherwise). */
      int tc_is_i32 = (tc_bt == IROP_BTYPE_INT32 || tc_bt == IROP_BTYPE_INT8 ||
                       tc_bt == IROP_BTYPE_INT16);
      int then_jmp = ir_skip_nops_forward(ir, then_start + 1, n);
      if (tc_vr >= 0 && tc_is_i32 &&
          then_jmp < n && ir->compact_instructions[then_jmp].op == TCCIR_OP_JUMP)
      {
        int mvia = (int)irop_get_imm64_ex(ir, tcc_ir_op_get_dest(ir, &ir->compact_instructions[then_jmp]));
        int mvia_i = (mvia >= 0 && mvia < n) ? ir_skip_nops_forward(ir, mvia, n) : n;
        if (mvia_i < n && ir->compact_instructions[mvia_i].op == TCCIR_OP_ASSIGN &&
            !JT_HAS_OTHER(mvia_i, then_jmp))
        {
          IRQuadCompact *masg = &ir->compact_instructions[mvia_i];
          IROperand tm = tcc_ir_op_get_dest(ir, masg);
          IROperand masg_src = tcc_ir_op_get_src1(ir, masg);
          int32_t tm_vr = irop_get_vreg(tm);
          int merge = ir_skip_nops_forward(ir, mvia_i + 1, n);
          int else_start2 = ir_skip_nops_forward(ir, else_target, n);
          if (tm_vr >= 0 && irop_btype_select_lowerable(irop_get_btype(tm)) &&
              irop_get_vreg(masg_src) == tc_vr && !irop_op_is_lval(masg_src) &&
              tcc_ir_vreg_has_single_use(ir, tc_vr, -1) && else_start2 < n)
          {
            IRQuadCompact *elq = &ir->compact_instructions[else_start2];
            int else_jmp = ir_skip_nops_forward(ir, else_start2 + 1, n);
            if ((elq->op == TCCIR_OP_LOAD || elq->op == TCCIR_OP_ASSIGN) &&
                irop_get_vreg(tcc_ir_op_get_dest(ir, elq)) == tm_vr &&
                ir_ifconv_arm_value_safe(ir, elq) &&
                irop_btype_select_lowerable(irop_get_btype(tcc_ir_op_get_src1(ir, elq))) &&
                else_jmp < n && ir->compact_instructions[else_jmp].op == TCCIR_OP_JUMP &&
                (int)irop_get_imm64_ex(ir, tcc_ir_op_get_dest(ir, &ir->compact_instructions[else_jmp])) == merge)
            {
              IROperand vel = tcc_ir_op_get_src1(ir, elq);
              IROperand sel_cond = irop_make_imm32(-1, then_cond, VT_INT);
              int pool_base = tcc_ir_iroperand_pool_add(ir, tm);
              tcc_ir_iroperand_pool_add(ir, tc);
              tcc_ir_iroperand_pool_add(ir, vel);
              tcc_ir_iroperand_pool_add(ir, sel_cond);
              masg->op = TCCIR_OP_SELECT;
              masg->operand_base = pool_base;

              JT_NOP_JUMP(i);          /* JUMPIF */
              JT_NOP_JUMP(then_jmp);   /* then-block JUMP */
              ir->compact_instructions[else_start2].op = TCCIR_OP_NOP; /* else LOAD/ASSIGN */
              JT_NOP_JUMP(else_jmp);   /* else-block JUMP */

              if (!JT_HAS_OTHER(else_target, -1))
                ir->compact_instructions[else_target].is_jump_target = 0;
              if (mvia >= 0 && mvia < n && !JT_HAS_OTHER(mvia, -1))
                ir->compact_instructions[mvia].is_jump_target = 0;
              if (merge >= 0 && merge < n && !JT_HAS_OTHER(merge, -1))
                ir->compact_instructions[merge].is_jump_target = 0;

              changes++;
              continue;
            }
          }
        }
      }
    }

    /* ----------------------------------------------------------------
     * Pattern: SETIF + ASSIGN(0) diamond collapse to bare SETIF
     * ----------------------------------------------------------------
     * then: T = SETIF setif_tok    (uses CPU flags from prior CMP/TEST_ZERO)
     * JUMP to merge
     * else: T = #0 [ASSIGN]
     * merge: ...
     *
     * When `setif_tok == ~branch_cond` (i.e. SETIF returns 1 exactly when the
     * fall-through path was taken), the diamond's result is identical to
     * SETIF alone: the SETIF's 0/1 already encodes the branch outcome, so the
     * explicit `T = 0` else branch is redundant.  Collapses the entire
     * diamond to a single SETIF, eliminating the JUMPIF / JUMP / else-ASSIGN.
     *
     * Safety: SETIF reads CPU flags set by the most recent CMP/TEST_ZERO; the
     * intervening JUMPIF doesn't modify flags, so removing it keeps the
     * SETIF's flag-source intact. */
    if (then_q1->op == TCCIR_OP_SETIF)
    {
      IROperand then_dest = tcc_ir_op_get_dest(ir, then_q1);
      IROperand setif_cond = tcc_ir_op_get_src1(ir, then_q1);
      int32_t dest_vreg = irop_get_vreg(then_dest);

      /* SETIF's condition must equal `then_cond` (the negation of the
       * JUMPIF's branch condition) so SETIF returns 1 precisely along the
       * fall-through (then) path and 0 along the taken (else) path. */
      if (!irop_is_immediate(setif_cond) || setif_cond.is_sym)
        goto setif_diamond_done;
      int setif_tok = (int)irop_get_imm64_ex(ir, setif_cond);
      if (setif_tok != then_cond)
        goto setif_diamond_done;

      /* Next should be unconditional JUMP to merge */
      int jump_idx = ir_skip_nops_forward(ir, then_start + 1, n);
      if (jump_idx >= n)
        goto setif_diamond_done;
      IRQuadCompact *jump_q = &ir->compact_instructions[jump_idx];
      if (jump_q->op != TCCIR_OP_JUMP)
        goto setif_diamond_done;
      int merge_target = (int)irop_get_imm64_ex(ir, tcc_ir_op_get_dest(ir, jump_q));

      /* Find else block (same vreg, ASSIGN of constant 0) */
      int else_start = ir_skip_nops_forward(ir, else_target, n);
      if (else_start >= n)
        goto setif_diamond_done;
      IRQuadCompact *else_q = &ir->compact_instructions[else_start];
      if (else_q->op != TCCIR_OP_ASSIGN)
        goto setif_diamond_done;
      IROperand else_dest = tcc_ir_op_get_dest(ir, else_q);
      IROperand else_val = tcc_ir_op_get_src1(ir, else_q);
      if (irop_get_vreg(else_dest) != dest_vreg)
        goto setif_diamond_done;
      if (!irop_is_immediate(else_val) || else_val.is_sym ||
          irop_get_imm64_ex(ir, else_val) != 0)
        goto setif_diamond_done;

      /* Merge must be the next real instruction after the else ASSIGN */
      int after_else = ir_skip_nops_forward(ir, else_start + 1, n);
      if (after_else != merge_target)
        goto setif_diamond_done;

      /* Collapse: NOP the JUMPIF, the JUMP, and the else ASSIGN.  SETIF
       * remains in place and now produces the same 0/1 result the diamond
       * would have produced. */
      JT_NOP_JUMP(i);                  /* the JUMPIF */
      JT_NOP_JUMP(jump_idx);           /* the unconditional JUMP */
      ir->compact_instructions[else_start].op = TCCIR_OP_NOP;

      if (!JT_HAS_OTHER(else_target, -1))
        ir->compact_instructions[else_target].is_jump_target = 0;
      if (merge_target >= 0 && merge_target < n && !JT_HAS_OTHER(merge_target, -1))
        ir->compact_instructions[merge_target].is_jump_target = 0;

      changes++;
      continue;
    setif_diamond_done:;
    }

    /* ----------------------------------------------------------------
     * Pattern: Return diamond
     * ----------------------------------------------------------------
     * then: RETURNVALUE val_then [const]
     * else_target: RETURNVALUE val_else [const]
     *
     * Both branches are terminal (no merge), so no JUMP between them.
     * The else_target falls immediately after the then RETURNVALUE.
     * ---------------------------------------------------------------- */
    if (then_q1->op == TCCIR_OP_RETURNVALUE)
    {
      IROperand then_val = tcc_ir_op_get_src1(ir, then_q1);

      /* Then-value must be a compile-time constant — vreg uses would
       * be disrupted by removing the branches. */
      int then_tag = irop_get_tag(then_val);
      if (then_tag != IROP_TAG_IMM32 && then_tag != IROP_TAG_SYMREF)
        continue;

      /* Find else block (skip NOPs starting at else_target) */
      int else_start = ir_skip_nops_forward(ir, else_target, n);
      if (else_start >= n)
        continue;
      IRQuadCompact *else_q = &ir->compact_instructions[else_start];
      if (else_q->op != TCCIR_OP_RETURNVALUE)
        continue;

      IROperand else_val = tcc_ir_op_get_src1(ir, else_q);
      int else_tag = irop_get_tag(else_val);
      if (else_tag != IROP_TAG_IMM32 && else_tag != IROP_TAG_SYMREF)
        continue;

      if (!irop_btype_select_lowerable(irop_get_btype(then_val)) ||
          !irop_btype_select_lowerable(irop_get_btype(else_val)))
        continue;

      /* Else block must immediately follow the then RETURNVALUE
       * (otherwise NOP'ing the else RETURNVALUE could break adjacent code). */
      int after_then = ir_skip_nops_forward(ir, then_start + 1, n);
      if (after_then != else_start)
        continue;

      /* Allocate 4 pool entries for SELECT */
      int32_t select_vreg = tcc_ir_get_vreg_temp(ir);
      IROperand sel_dest = irop_make_vreg(select_vreg, IROP_BTYPE_INT32);
      IROperand sel_cond = irop_make_imm32(-1, then_cond, VT_INT);

      int pool_base = tcc_ir_iroperand_pool_add(ir, sel_dest);
      tcc_ir_iroperand_pool_add(ir, then_val);
      tcc_ir_iroperand_pool_add(ir, else_val);
      tcc_ir_iroperand_pool_add(ir, sel_cond);

      /* Rewrite JUMPIF as SELECT (drops one jump to else_target) */
      if (else_target >= 0 && else_target < n && jt_cnt[else_target] > 0)
        jt_cnt[else_target]--;
      jumpif_q->op = TCCIR_OP_SELECT;
      jumpif_q->operand_base = pool_base;

      /* Rewrite the then RETURNVALUE to consume the SELECT result */
      IROperand new_ret_val = irop_make_vreg(select_vreg, IROP_BTYPE_INT32);
      tcc_ir_op_set_src1(ir, then_q1, new_ret_val);

      /* NOP the else RETURNVALUE (else_start) — unreachable now */
      ir->compact_instructions[else_start].op = TCCIR_OP_NOP;

      /* Clear stale is_jump_target on else_target if no longer targeted */
      if (!JT_HAS_OTHER(else_target, -1))
        ir->compact_instructions[else_target].is_jump_target = 0;

      changes++;
      continue;
    }
  }

  #undef JT_HAS_OTHER
  #undef JT_NOP_JUMP
  return changes;
}

/* Forward-diamond JUMPIF inversion (post-regalloc).
 *
 * Pattern:
 *   i:           JUMPIF cond -> T              ; T = jump_idx + 1
 *   i+1..jump_idx-1: ASSIGN copies, all coalesced no-ops (dest reg == src reg)
 *   jump_idx:    JUMP M                        ; M > T (forward merge)
 *   T:           <then-target>                 ; falls through to merge
 *
 * When register allocation coalesces the phi copies into no-ops, the entire
 * fall-through path between the JUMPIF and JUMP becomes empty.  Invert the
 * JUMPIF and retarget it to M; NOP the ASSIGNs and the JUMP.  Saves one
 * unconditional b.w per occurrence.
 *
 * Common after SWITCH_LOAD lowering where the out-of-range path carries the
 * pre-initialized default value via a phi copy that coalesces away. */
int tcc_ir_opt_post_ra_forward_diamond(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  if (n < 3) return 0;

  int changes = 0;

  for (int i = 0; i + 2 < n; i++) {
    IRQuadCompact *jif = &ir->compact_instructions[i];
    if (jif->op != TCCIR_OP_JUMPIF)
      continue;

    int exit_target = (int)irop_get_imm32(tcc_ir_op_get_dest(ir, jif));
    int cond = (int)tcc_ir_op_get_src1(ir, jif).u.imm32;

    if (exit_target <= i || exit_target >= n)
      continue;

    /* Count consecutive ASSIGNs after JUMPIF (allow 0 — degenerate case) */
    int num_assigns = 0;
    for (int j = i + 1; j < n; j++) {
      IRQuadCompact *q = &ir->compact_instructions[j];
      if (q->op == TCCIR_OP_ASSIGN)
        num_assigns++;
      else
        break;
    }
    if (num_assigns > 8)
      continue;

    int jump_idx = i + 1 + num_assigns;
    if (jump_idx >= n)
      continue;
    IRQuadCompact *jmp = &ir->compact_instructions[jump_idx];
    if (jmp->op != TCCIR_OP_JUMP)
      continue;

    int merge_target = (int)irop_get_imm32(tcc_ir_op_get_dest(ir, jmp));
    if (merge_target <= jump_idx || merge_target >= n)
      continue;

    /* Strict diamond: JUMPIF target must be the instruction right after JUMP */
    if (exit_target != jump_idx + 1)
      continue;

    /* No-op if both legs go to same place */
    if (merge_target == exit_target)
      continue;

    /* Any ASSIGN that survives between JUMPIF and JUMP must be a coalesced
     * no-op (dest and src in the same physical register, neither spilled). */
    int safe = 1;
    for (int j = 0; j < num_assigns && safe; j++) {
      IRQuadCompact *aq = &ir->compact_instructions[i + 1 + j];
      IROperand adst = tcc_ir_op_get_dest(ir, aq);
      IROperand asrc = tcc_ir_op_get_src1(ir, aq);
      int32_t adst_vr = irop_get_vreg(adst);
      int32_t asrc_vr = irop_get_vreg(asrc);
      if (adst_vr < 0 || asrc_vr < 0) { safe = 0; break; }

      int dst_reg = -2, dst_reg1 = -2, src_reg = -2, src_reg1 = -2;
      int dst_spilled = 0, src_spilled = 0;
      for (int k = 0; k < ir->ls.next_interval_index; k++) {
        LSLiveInterval *li = &ir->ls.intervals[k];
        if (li->vreg == (uint32_t)adst_vr) {
          if (li->stack_location != 0 || li->r0 < 0)
            dst_spilled = 1;
          else {
            dst_reg = li->r0;
            dst_reg1 = li->r1;
          }
        }
        if (li->vreg == (uint32_t)asrc_vr) {
          if (li->stack_location != 0 || li->r0 < 0)
            src_spilled = 1;
          else {
            src_reg = li->r0;
            src_reg1 = li->r1;
          }
        }
      }
      if (dst_spilled || src_spilled) { safe = 0; break; }
      if (dst_reg < 0 || src_reg < 0) { safe = 0; break; }
      /* Require identical reg pair (handles both 32-bit and 64-bit) */
      if (dst_reg != src_reg || dst_reg1 != src_reg1) { safe = 0; break; }
    }
    if (!safe)
      continue;

    /* Pin both sides of every eliminated no-op copy to their shared physical
     * register.  Without this, a later codegen scratch-conflict fixup
     * (try_reassign_scratch_conflict) can independently move just the dest
     * vreg's interval to a different register — the two vregs stop sharing a
     * register even though the copy that would keep them in sync no longer
     * exists in the IR, so the fall-through edge silently reads a register
     * that was never written on that path.  phi_pinned is the same guard
     * ra_phi_copy_needed() sets for the identical post-RA-identity case. */
    for (int j = 0; j < num_assigns; j++) {
      IRQuadCompact *aq = &ir->compact_instructions[i + 1 + j];
      int32_t adst_vr = irop_get_vreg(tcc_ir_op_get_dest(ir, aq));
      int32_t asrc_vr = irop_get_vreg(tcc_ir_op_get_src1(ir, aq));
      IRLiveInterval *dli = tcc_ir_vreg_live_interval(ir, adst_vr);
      IRLiveInterval *sli = tcc_ir_vreg_live_interval(ir, asrc_vr);
      if (dli) dli->phi_pinned = 1;
      if (sli) sli->phi_pinned = 1;
    }

    int inv_cond = invert_condition(cond);
    if (inv_cond < 0)
      continue;

    /* Retarget JUMPIF to merge and invert its condition */
    {
      IROperand new_dest = {0};
      new_dest.tag = IROP_TAG_IMM32;
      new_dest.u.imm32 = merge_target;
      tcc_ir_op_set_dest(ir, jif, new_dest);

      IROperand new_cond = {0};
      new_cond.tag = IROP_TAG_IMM32;
      new_cond.u.imm32 = inv_cond;
      tcc_ir_op_set_src1(ir, jif, new_cond);
    }

    /* Redirect any OTHER jump that targeted the original fall-through path
     * [i+1 .. jump_idx] straight to merge_target before we NOP those slots.
     * That region was "(coalesced no-op ASSIGNs); JUMP merge_target", so
     * entering it anywhere meant "go to merge_target"; once the JUMP is NOP'd a
     * stale target pointing into it would fall onto the inverted JUMPIF (jif)
     * and re-use jif's comparison flags — exactly the `if (A || B)`
     * short-circuit bug where A's equality branch ends up on B's relational
     * branch.  exit_target (= jump_idx+1) is outside the region, so jumps to
     * the then-body are untouched. */
    for (int k = 0; k < n; k++) {
      if (k >= i && k <= jump_idx)
        continue;
      IRQuadCompact *kq = &ir->compact_instructions[k];
      if (kq->op != TCCIR_OP_JUMP && kq->op != TCCIR_OP_JUMPIF)
        continue;
      int kt = (int)irop_get_imm32(tcc_ir_op_get_dest(ir, kq));
      if (kt < i + 1 || kt > jump_idx)
        continue;
      IROperand kd = {0};
      kd.tag = IROP_TAG_IMM32;
      kd.u.imm32 = merge_target;
      tcc_ir_op_set_dest(ir, kq, kd);
    }

    /* NOP the no-op ASSIGNs and the bridging JUMP */
    for (int j = 0; j < num_assigns; j++)
      ir->compact_instructions[i + 1 + j].op = TCCIR_OP_NOP;
    ir->compact_instructions[jump_idx].op = TCCIR_OP_NOP;

    /* merge_target was already a JUMP target; is_jump_target stays set.
     * exit_target loses one predecessor but is conservatively left flagged. */
    if (merge_target >= 0 && merge_target < n)
      ir->compact_instructions[merge_target].is_jump_target = 1;

    changes++;
  }

  return changes;
}
