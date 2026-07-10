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

int tcc_ir_opt_add_deref_fold(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  int changes = 0;

  IROptDU du;
  ir_opt_du_build_mode(ir, &du, IR_DU_MODE_TMP_ONLY);

  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_ADD)
      continue;

    IROperand dest = tcc_ir_op_get_dest(ir, q);
    IROperand src1 = tcc_ir_op_get_src1(ir, q);
    IROperand src2 = tcc_ir_op_get_src2(ir, q);

    int32_t dest_vr = irop_get_vreg(dest);
    if (dest_vr < 0 || TCCIR_DECODE_VREG_TYPE(dest_vr) != TCCIR_VREG_TYPE_TEMP)
      continue;
    if (!irop_is_immediate(src2))
      continue;
    int32_t imm = (int32_t)irop_get_imm64_ex(ir, src2);
    if (imm < 0 || imm > 4095)
      continue;
    int32_t base_vr = irop_get_vreg(src1);
    if (base_vr < 0)
      continue;
    if (src1.is_local || src1.is_llocal)
      continue;
    /* Only fold PARAM bases: the explicit LOAD_INDEXED can expose stack
     * loads to constant propagation which may incorrectly fold across
     * calls that modify memory through aliased pointers.  PARAM vregs
     * point to caller-owned memory, safe from this issue.
     *
     * Peep-through: if the base is a TEMP whose only def is a plain
     * ASSIGN copy from a PARAM, treat that PARAM as the effective base.
     * The TEMP is just a shadow of the parameter — copy_prop typically
     * eliminates it but doesn't always run before this pass. */
    if (TCCIR_DECODE_VREG_TYPE(base_vr) != TCCIR_VREG_TYPE_PARAM)
    {
      if (TCCIR_DECODE_VREG_TYPE(base_vr) != TCCIR_VREG_TYPE_TEMP)
        continue;
      /* Peep-through: short bounded backward scan looking for an `ASSIGN
       * T <- PARAM` immediately preceding the ADD.  The frontend emits
       * the copy right before the ADD, so a window of ~16 instructions
       * is enough; falling back to a full-function scan would make this
       * O(n^2) on stress tests like 20001226-1 (16k compares).
       *
       * Bail on any branch/store/call before the def to keep the
       * semantics local — same constraints as the later same-block
       * and side-effect checks. */
      int copy_idx = -1;
      int max_back = 16;
      for (int j = i - 1; j >= 0 && (i - j) <= max_back; j--)
      {
        IRQuadCompact *cq = &ir->compact_instructions[j];
        if (cq->op == TCCIR_OP_NOP)
          continue;
        if (cq->op == TCCIR_OP_JUMP || cq->op == TCCIR_OP_JUMPIF ||
            cq->op == TCCIR_OP_STORE || cq->op == TCCIR_OP_STORE_INDEXED ||
            cq->op == TCCIR_OP_STORE_POSTINC || cq->op == TCCIR_OP_FUNCCALLVAL ||
            cq->op == TCCIR_OP_FUNCCALLVOID)
          break;
        if (irop_config[cq->op].has_dest)
        {
          IROperand cd = tcc_ir_op_get_dest(ir, cq);
          if (irop_get_vreg(cd) == base_vr && !cd.is_lval)
          {
            copy_idx = j;
            break;
          }
        }
      }
      if (copy_idx < 0)
        continue;
      IRQuadCompact *cq = &ir->compact_instructions[copy_idx];
      if (cq->op != TCCIR_OP_ASSIGN)
        continue;
      IROperand cs1 = tcc_ir_op_get_src1(ir, cq);
      IROperand cd = tcc_ir_op_get_dest(ir, cq);
      if (cs1.is_lval || cd.is_lval)
        continue;
      int32_t cs1_vr = irop_get_vreg(cs1);
      if (cs1_vr < 0 || TCCIR_DECODE_VREG_TYPE(cs1_vr) != TCCIR_VREG_TYPE_PARAM)
        continue;
      /* Use the PARAM source as the new base.  Don't NOP the copy — later
       * DCE will remove it if the TEMP becomes dead.  We don't verify "T is
       * used only here" because the existing use_count == 1 check on the
       * ADD's dest below covers what we actually need: the LOAD_INDEXED
       * still computes the same value regardless of how many extra readers
       * the TEMP base has, since the copy stays put. */
      src1 = cs1;
      base_vr = cs1_vr;
    }

    /* Fast pre-filter: skip if T has != 1 use (O(1) via shared DU). */
    if (ir_opt_du_uses(&du, dest_vr) != 1)
      continue;

    /* Find the single use and verify it's a DEREF. */
    int use_idx = -1;
    int use_is_deref = 0;
    int use_in_src2 = 0;
    for (int j = i + 1; j < n; j++)
    {
      IRQuadCompact *uq = &ir->compact_instructions[j];
      if (uq->op == TCCIR_OP_NOP)
        continue;
      if (irop_config[uq->op].has_src1)
      {
        IROperand s = tcc_ir_op_get_src1(ir, uq);
        if (irop_get_vreg(s) == dest_vr)
        { use_idx = j; use_is_deref = s.is_lval; use_in_src2 = 0; break; }
      }
      if (irop_config[uq->op].has_src2)
      {
        IROperand s = tcc_ir_op_get_src2(ir, uq);
        if (irop_get_vreg(s) == dest_vr)
        { use_idx = j; use_is_deref = s.is_lval; use_in_src2 = 1; break; }
      }
      if ((uq->op == TCCIR_OP_STORE || uq->op == TCCIR_OP_STORE_INDEXED) &&
          irop_get_vreg(tcc_ir_op_get_dest(ir, uq)) == dest_vr)
      { use_idx = j; break; }
    }

    if (!use_is_deref || use_idx < 0)
      continue;

    /* Same-block: no branch between ADD and its deref use.  Branches could
     * route through a path that stores to [base+imm], making the early
     * load see stale data. */
    {
      int cross_block = 0;
      for (int j = i + 1; j < use_idx; j++)
      {
        TccIrOp bop = ir->compact_instructions[j].op;
        if (bop == TCCIR_OP_JUMP || bop == TCCIR_OP_JUMPIF)
        {
          cross_block = 1;
          break;
        }
      }
      if (cross_block)
        continue;
    }

    /* The fold moves the load from the use site to the ADD site. If any
     * store or call occurs between them, the load might see stale data
     * (memory ordering violation). Bail if so. */
    {
      int has_side_effect = 0;
      for (int j = i + 1; j < use_idx; j++)
      {
        IRQuadCompact *sq = &ir->compact_instructions[j];
        if (sq->op == TCCIR_OP_STORE || sq->op == TCCIR_OP_STORE_INDEXED || sq->op == TCCIR_OP_STORE_POSTINC ||
            sq->op == TCCIR_OP_FUNCCALLVAL || sq->op == TCCIR_OP_FUNCCALLVOID)
        {
          has_side_effect = 1;
          break;
        }
      }
      if (has_side_effect)
        continue;
    }

    /* Get the DEREF use's btype — this determines the load width.
     * The ADD dest has a pointer btype which may differ from the
     * loaded value's type (e.g., struct pointer vs int field). */
    IRQuadCompact *uq_pre = &ir->compact_instructions[use_idx];
    IROperand use_op = use_in_src2 ? tcc_ir_op_get_src2(ir, uq_pre) : tcc_ir_op_get_src1(ir, uq_pre);
    int load_btype = irop_get_btype(use_op);

    /* Skip 64-bit and struct loads: LOAD_INDEXED uses LDRD which requires
     * 4-byte alignment.  Packed structs can place 64-bit fields at
     * unaligned offsets, causing a HardFault. */
    if (load_btype == IROP_BTYPE_INT64 || load_btype == IROP_BTYPE_FLOAT64 || load_btype == IROP_BTYPE_STRUCT)
      continue;

    /* Override the dest btype to the loaded value type */
    IROperand load_dest = dest;
    load_dest.btype = load_btype;

    /* Convert ADD to LOAD_INDEXED: allocate 4 contiguous pool entries */
    IROperand scale_op = irop_make_imm32(-1, 0, IROP_BTYPE_INT32);
    int new_base = tcc_ir_pool_add(ir, load_dest);
    tcc_ir_pool_add(ir, src1);
    tcc_ir_pool_add(ir, src2);
    tcc_ir_pool_add(ir, scale_op);
    q->operand_base = new_base;
    q->op = TCCIR_OP_LOAD_INDEXED;

    /* Clear DEREF on the use site — the value is now loaded, not a pointer. */
    IRQuadCompact *uq = &ir->compact_instructions[use_idx];
    if (irop_config[uq->op].has_src1)
    {
      IROperand s = tcc_ir_op_get_src1(ir, uq);
      if (irop_get_vreg(s) == dest_vr && s.is_lval)
      {
        s.is_lval = 0;
        tcc_ir_set_src1(ir, use_idx, s);
      }
    }
    if (irop_config[uq->op].has_src2)
    {
      IROperand s = tcc_ir_op_get_src2(ir, uq);
      if (irop_get_vreg(s) == dest_vr && s.is_lval)
      {
        s.is_lval = 0;
        tcc_ir_set_src2(ir, use_idx, s);
      }
    }

    changes++;
  }

  tcc_free(du.def);
  return changes;
}

/* tcc_ir_opt_fusion_pass (MLA + indexed memory) replaced by generators in opt_gens_fusion.c */

/* ============================================================================
 * Rotation Fusion
 * ============================================================================
 *
 * Fuses the C rotation idiom into a single ROR instruction.
 * Pattern:
 *   t1 = SHL(x, #n)
 *   t2 = SHR(x, #(32-n))
 *   result = OR(t1, t2)       (or OR(t2, t1))
 *
 * Becomes:
 *   result = ROR(x, #(32-n))
 *   (SHL → NOP, SHR → NOP)
 */
/* tcc_ir_opt_rotate_fusion replaced by ir_gen_rotate_fusion in opt_gens_fusion.c */

/* ============================================================================
 * Late Barrel Shift Fusion (runs just before codegen)
 * ============================================================================
 *
 * Folds a single-use shift/rotate into the consuming ALU instruction's src2
 * using the ARM barrel shifter.  Results are written to ir->barrel_shifts[]
 * (a side-table), not into IRQuadCompact, so no intermediate pass can corrupt them.
 *
 * Pattern:
 *   t = SHL/SHR/SAR/ROR(x, #n)     -- single use, 32-bit
 *   result = ADD/SUB/AND/OR/XOR/CMP(y, t)
 *
 * Encoding: barrel_shifts[i] = (type<<5)|amount
 *   type: 1=SHL, 2=SHR, 3=SAR, 4=ROR.  amount: 0-31.
 */
void tcc_ir_barrel_shift_fusion(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  if (n == 0)
    return;

  ir->barrel_shifts = tcc_mallocz(ir->max_orig_index + 1);
  ir->barrel_shifts_len = ir->max_orig_index + 1;

  IROptDU du;
  ir_opt_du_build(ir, &du);

  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];

    int commutative = 0;
    switch (q->op)
    {
    case TCCIR_OP_ADD: case TCCIR_OP_AND: case TCCIR_OP_OR: case TCCIR_OP_XOR:
      commutative = 1;
      break;
    case TCCIR_OP_SUB: case TCCIR_OP_CMP:
      break;
    default:
      continue;
    }

    /* Try fusing on src2 first; for commutative ops, also try src1 (swapping
     * operands so the shift lands on src2 where the backend expects it). */
    for (int attempt = 0; attempt < (commutative ? 2 : 1); attempt++) {
      IROperand src2 = (attempt == 0) ? tcc_ir_op_get_src2(ir, q)
                                      : tcc_ir_op_get_src1(ir, q);
      if (!irop_has_vreg(src2))
        continue;

      int32_t vr2 = irop_get_vreg(src2);
      int shift_idx = ir_opt_du_def(&du, vr2, i);
      if (shift_idx < 0)
        continue;

      IRQuadCompact *sq = &ir->compact_instructions[shift_idx];
      int stype;
      switch (sq->op) {
      case TCCIR_OP_SHL:
        if (q->op == TCCIR_OP_ADD) continue;
        stype = 1; break;
      case TCCIR_OP_SHR: stype = 2; break;
      case TCCIR_OP_SAR: stype = 3; break;
      case TCCIR_OP_ROR: stype = 4; break;
      default: continue;
      }

      if (ir_opt_du_uses(&du, vr2) != 1)
        continue;

      IROperand shift_dest = tcc_ir_op_get_dest(ir, sq);
      if (shift_dest.btype == IROP_BTYPE_INT64)
        continue;

      IROperand consumer_dest = tcc_ir_op_get_dest(ir, q);
      if (consumer_dest.btype == IROP_BTYPE_INT64)
        continue;
      if (src2.btype == IROP_BTYPE_INT64)
        continue;

      IROperand shift_src2 = tcc_ir_op_get_src2(ir, sq);
      if (!irop_is_immediate(shift_src2))
        continue;

      int64_t amount = irop_get_imm64_ex(ir, shift_src2);
      if (amount < 0 || amount > 31)
        continue;

      /* A zero-amount right shift/rotate is an identity in the IR (x >> 0 == x),
       * but ARM's barrel shifter encodes an immediate field of 0 for LSR/ASR as
       * shift-by-32 (yielding 0 / sign-extend) and for ROR as RRX — NOT the
       * shift-by-0 we mean.  Only LSL #0 (stype 1) is a true no-op operand, so
       * refuse to fuse `x SHR/SAR/ROR #0`; leave the standalone shift for the
       * backend's shift-by-0 identity fold (arm-thumb-gen.c) to lower as MOV. */
      if (amount == 0 && stype != 1)
        continue;

      IROperand shift_src1 = tcc_ir_op_get_src1(ir, sq);
      if (!irop_has_vreg(shift_src1))
        continue;

      int32_t shift_src_vr = irop_get_vreg(shift_src1);

      IROperand other = (attempt == 0) ? tcc_ir_op_get_src1(ir, q)
                                        : tcc_ir_op_get_src2(ir, q);
      if (!irop_has_vreg(other))
        continue;
      if (irop_has_vreg(other) && irop_get_vreg(other) == shift_src_vr)
        continue;

      int safe = 1;
      for (int j = shift_idx + 1; j < i && safe; j++)
      {
        IRQuadCompact *jq = &ir->compact_instructions[j];
        TccIrOp bop = jq->op;
        if (bop == TCCIR_OP_JUMP || bop == TCCIR_OP_JUMPIF)
          safe = 0;
        if (bop == TCCIR_OP_NOP)
          continue;
        if (irop_config[bop].has_dest)
        {
          IROperand jdest = tcc_ir_op_get_dest(ir, jq);
          if (irop_has_vreg(jdest) && irop_get_vreg(jdest) == shift_src_vr)
            safe = 0;
        }
      }
      if (!safe)
        continue;

      /* For the swap path: rewrite src1 to the non-shift operand so the
       * backend sees `op dest, other, shifted`. The shift's source vreg
       * goes into src2 in both paths. */
      if (attempt == 1)
        tcc_ir_set_src1(ir, i, other);
      tcc_ir_set_src2(ir, i, shift_src1);
      ir->barrel_shifts[q->orig_index] = (uint8_t)((stype << 5) | (int)amount);
      sq->op = TCCIR_OP_NOP;
      break;
    }
  }

  tcc_free(du.def);
}

/* ============================================================================
 * Two-shift extract → UBFX / SBFX  (tcc_ir_opt_shift_pair_to_ubfx)
 * ============================================================================
 *
 * The canonical unsigned bitfield extract `(x << a) >> b` (b >= a, both
 * logical) isolates the (32-b)-bit field at bit offset (b-a) of x.  ARM
 * Thumb-2 does this in one instruction: `UBFX Rd, Rx, #(b-a), #(32-b)`.
 * The signed analog `(x << a) >> b` with an *arithmetic* outer shift (SAR)
 * sign-extends the same field and lowers to `SBFX Rd, Rx, #(b-a), #(32-b)`.
 *
 * MUST run AFTER tcc_ir_barrel_shift_fusion: that pass folds a single-use shift
 * into its consuming ALU op (ADD/SUB/AND/OR/XOR/CMP) for free via the barrel
 * shifter and NOPs the shift.  So a SHL+SHR pair that SURVIVES as real ops was
 * NOT foldable — its SHR feeds something that can't take a shifted operand (a
 * store, multiply, call arg, return value, or a value used more than once).
 * There the pair costs two instructions (`lsls`+`lsrs`) and UBFX is one — a
 * strict win.  A pair the barrel pass DID fold no longer has a real SHR for us
 * to match, so we never undo that (equal-cost) fusion and never grow code.
 *
 * Gate — each clause keeps the rewrite provably non-increasing:
 *   - inner is SHL #a, outer is SHR #b (UBFX) or SAR #b (SBFX), 1<=a<=b<=31,
 *     both 32-bit (the 64-bit shift-extract idiom is handled by
 *     shift64_dead_half);
 *   - the SHL result is single-use (only the outer shift), so NOPing the SHL
 *     drops exactly one instruction;
 *   - the SHL source is a plain (non-lval) register value, not redefined
 *     between the SHL and the outer shift (the BFX reads it at the outer
 *     shift's position) and with no control-flow edge between the two (same
 *     basic block).
 */
int tcc_ir_opt_shift_pair_to_ubfx(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  int changes = 0;

  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *shr_q = &ir->compact_instructions[i];
    if (shr_q->op != TCCIR_OP_SHR && shr_q->op != TCCIR_OP_SAR)
      continue;
    int is_signed = (shr_q->op == TCCIR_OP_SAR);
    if (tcc_ir_op_get_dest(ir, shr_q).btype == IROP_BTYPE_INT64)
      continue;
    IROperand shr_n = tcc_ir_op_get_src2(ir, shr_q);
    if (!irop_is_immediate(shr_n) || shr_n.is_sym)
      continue;
    int b = (int)irop_get_imm64_ex(ir, shr_n);
    if (b < 1 || b > 31)
      continue;

    IROperand shr_src1 = tcc_ir_op_get_src1(ir, shr_q);
    if (shr_src1.is_lval || !irop_has_vreg(shr_src1))
      continue;
    int32_t t1 = irop_get_vreg(shr_src1);
    if (t1 < 0 || TCCIR_DECODE_VREG_TYPE(t1) != TCCIR_VREG_TYPE_TEMP)
      continue;

    int shl_idx = tcc_ir_find_defining_instruction(ir, t1, i);
    if (shl_idx < 0)
      continue;
    IRQuadCompact *shl_q = &ir->compact_instructions[shl_idx];
    if (shl_q->op != TCCIR_OP_SHL)
      continue;
    if (tcc_ir_op_get_dest(ir, shl_q).btype == IROP_BTYPE_INT64)
      continue;
    IROperand shl_n = tcc_ir_op_get_src2(ir, shl_q);
    if (!irop_is_immediate(shl_n) || shl_n.is_sym)
      continue;
    int a = (int)irop_get_imm64_ex(ir, shl_n);
    if (a < 1 || a > b)
      continue;

    /* SHL result must feed only this SHR, so NOPing it is safe. */
    if (!tcc_ir_vreg_has_single_use(ir, t1, shl_idx))
      continue;

    IROperand t0 = tcc_ir_op_get_src1(ir, shl_q);
    if (t0.is_lval || !irop_has_vreg(t0))
      continue;
    int32_t t0_vr = irop_get_vreg(t0);

    /* T0 must be unchanged between the SHL and the SHR, and no control-flow
     * edge may separate them (UBFX recomputes from T0 at the SHR's site). */
    int safe = 1;
    for (int j = shl_idx + 1; j < i && safe; j++)
    {
      IRQuadCompact *jq = &ir->compact_instructions[j];
      if (jq->op == TCCIR_OP_NOP)
        continue;
      if (jq->op == TCCIR_OP_JUMP || jq->op == TCCIR_OP_JUMPIF ||
          jq->op == TCCIR_OP_IJUMP || jq->op == TCCIR_OP_SWITCH_TABLE || jq->is_jump_target)
      {
        safe = 0;
        break;
      }
      if (irop_config[jq->op].has_dest)
      {
        IROperand jd = tcc_ir_op_get_dest(ir, jq);
        if (irop_has_vreg(jd) && irop_get_vreg(jd) == t0_vr)
        {
          safe = 0;
          break;
        }
      }
    }
    if (!safe || shr_q->is_jump_target)
      continue;

    int lsb = b - a;
    int width = 32 - b;
    int32_t param = lsb | (width << 5);
    shr_q->op = is_signed ? TCCIR_OP_SBFX : TCCIR_OP_UBFX;
    tcc_ir_set_src1(ir, i, t0);
    tcc_ir_set_src2(ir, i, irop_make_imm32(-1, param, IROP_BTYPE_INT32));
    shl_q->op = TCCIR_OP_NOP;
    changes++;
    LOG_IR_GEN("SHIFT-PAIR->%s @%d: (x<<%d)>>%d -> lsb=%d width=%d (SHL@%d NOP)", is_signed ? "SBFX" : "UBFX", i, a, b,
               lsb, width, shl_idx);
  }

  return changes;
}


/* ============================================================================
 * Call-chain result rename
 * ============================================================================
 *
 * Pattern:
 *   CALL_i  --> V              (V is a VAR/TEMP receiving the call result)
 *   FUNCPARAMVAL[0] V           (V immediately consumed as next call's arg 0)
 *   CALL_(i+1) --> V            (overwrites V)
 *
 * The regalloc currently keeps V in a callee-saved register because V's
 * lifetime spans multiple CALL instructions, even though each segment of
 * V's value is short-lived (def at one CALL, single use at the next call's
 * PARAMVAL[0], then redefined).  The result is a `mov V_reg, r0` after
 * each call and `mov r0, V_reg` before each PARAMVAL — both wasted, since
 * the call's return is already in r0 and PARAMVAL[0] expects r0.
 *
 * Fix: for each (CALL → V; PARAMVAL[0] V; ... ; redef-of-V) segment where
 * V is overwritten by the next CALL with no intervening read, rename V at
 * just that one (CALL.dest, PARAMVAL.src1) pair to a fresh TEMP.  The
 * fresh TEMP has a tiny live range that doesn't cross any CALL, so the
 * regalloc can put it in r0 (the AAPCS return / arg0 reg), and the post-
 * allocation move-coalescer eats both `mov`s.
 *
 * V's other defs/uses (in particular the LAST call in a chain whose
 * result flows out via an external read like `return y`) are left alone,
 * so V keeps the right value at the function's external-visible points.
 */
int tcc_ir_opt_call_chain_rename(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  int changes = 0;
  if (n < 3)
    return 0;

  LOG_IR_GEN("=== CALL CHAIN RENAME START (n=%d) ===", n);

  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_FUNCCALLVAL)
      continue;

    IROperand dest = tcc_ir_op_get_dest(ir, q);
    int32_t v_vr = irop_get_vreg(dest);
    if (v_vr < 0 || dest.is_lval)
      continue;
    int v_type = TCCIR_DECODE_VREG_TYPE(v_vr);
    if (v_type != TCCIR_VREG_TYPE_VAR && v_type != TCCIR_VREG_TYPE_TEMP)
      continue;

    /* Next non-NOP must be FUNCPARAMVAL with src = V, param index 0. */
    int j = i + 1;
    while (j < n && ir->compact_instructions[j].op == TCCIR_OP_NOP)
      j++;
    if (j >= n)
      continue;
    IRQuadCompact *next_q = &ir->compact_instructions[j];
    if (next_q->op != TCCIR_OP_FUNCPARAMVAL || next_q->is_jump_target)
      continue;

    IROperand pv_src = tcc_ir_op_get_src1(ir, next_q);
    if (irop_get_vreg(pv_src) != v_vr)
      continue;
    /* PARAMVAL src may be is_lval=1 for VAR sources (semantically: load
     * V's storage into the param reg).  After our rename, the value is
     * already in T_anon as a register, so we'll emit the new src with
     * is_lval=0.  V's btype/is_unsigned are preserved. */
    IROperand pv_src2 = tcc_ir_op_get_src2(ir, next_q);
    int param_idx = TCCIR_DECODE_PARAM_IDX(irop_get_imm64_ex(ir, pv_src2));
    if (param_idx != 0)
      continue;

    /* Walk forward to verify V is overwritten before any subsequent read.
     * Stop at JUMP/JUMPIF/RETURN — beyond a control-flow boundary the
     * rename is unsafe (other paths might read V).  Also bail if any op
     * reads V before the redef. */
    int safe = 0;
    int redef_idx = -1;
    for (int k = j + 1; k < n; k++)
    {
      IRQuadCompact *kq = &ir->compact_instructions[k];
      if (kq->op == TCCIR_OP_NOP)
        continue;
      if (kq->op == TCCIR_OP_JUMP || kq->op == TCCIR_OP_JUMPIF || kq->op == TCCIR_OP_IJUMP ||
          kq->op == TCCIR_OP_RETURNVOID || kq->op == TCCIR_OP_RETURNVALUE || kq->op == TCCIR_OP_SWITCH_TABLE ||
          kq->is_jump_target)
        break;

      /* Check if op reads V. */
      int reads_v = 0;
      if (irop_config[kq->op].has_src1)
      {
        IROperand s = tcc_ir_op_get_src1(ir, kq);
        if (irop_get_vreg(s) == v_vr)
          reads_v = 1;
      }
      if (!reads_v && irop_config[kq->op].has_src2)
      {
        IROperand s = tcc_ir_op_get_src2(ir, kq);
        if (irop_get_vreg(s) == v_vr)
          reads_v = 1;
      }
      /* STORE.dest is also a read of the address vreg, not a redef. */
      if (!reads_v && (kq->op == TCCIR_OP_STORE || kq->op == TCCIR_OP_STORE_INDEXED ||
                       kq->op == TCCIR_OP_STORE_POSTINC))
      {
        IROperand d = tcc_ir_op_get_dest(ir, kq);
        if (irop_get_vreg(d) == v_vr)
          reads_v = 1;
      }
      if (reads_v)
        break; /* V is read between PARAMVAL and redef → can't rename */

      /* Check if op writes V. */
      if (irop_config[kq->op].has_dest)
      {
        IROperand d = tcc_ir_op_get_dest(ir, kq);
        if (irop_get_vreg(d) == v_vr && !d.is_lval)
        {
          /* Honest redefinition: V will be overwritten before any later read. */
          if (kq->op == TCCIR_OP_FUNCCALLVAL || kq->op == TCCIR_OP_ASSIGN || kq->op == TCCIR_OP_LOAD)
          {
            redef_idx = k;
            safe = 1;
          }
          break;
        }
      }
    }
    if (!safe)
      continue;
    (void)redef_idx;

    /* Allocate a fresh TEMP and rename V at this CALL.dest and
     * PARAMVAL.src1 only.  V's other defs/uses stay intact. */
    int32_t t_anon = tcc_ir_vreg_alloc_temp(ir);
    if (t_anon < 0)
      continue;

    IROperand new_dest = irop_make_vreg(t_anon, dest.btype);
    new_dest.is_unsigned = dest.is_unsigned;
    tcc_ir_set_dest(ir, i, new_dest);

    IROperand new_pv_src = irop_make_vreg(t_anon, pv_src.btype);
    new_pv_src.is_unsigned = pv_src.is_unsigned;
    tcc_ir_set_src1(ir, j, new_pv_src);

    changes++;
    LOG_IR_GEN("CALL CHAIN RENAME: V%d at CALL@%d/PARAMVAL@%d -> T%d", v_vr, i, j, t_anon);
  }

  LOG_IR_GEN("=== CALL CHAIN RENAME END: %d renames ===", changes);
  return changes;
}

/* ============================================================================
 * Stack-address ADD-operand CSE
 * ============================================================================
 *
 * Pattern (sha_transform expansion loop W[i-N] computation):
 *   T_a = Addr[StackLoc[X]] ADD R_idx_a
 *   T_b = Addr[StackLoc[X]] ADD R_idx_b
 *   T_c = Addr[StackLoc[X]] ADD R_idx_c
 *   ...
 *
 * Each `Addr[StackLoc[X]]` operand is an inline literal that the codegen
 * materializes as `add rX, sp, #off` per occurrence — N redundant
 * recomputes of the same address.  The downstream SHL+ADD fusion also
 * bails on `is_local` base, so the SHL/ADD chain can't fold to
 * LOAD_INDEXED with a shift-base.
 *
 * Fix: for each unique StackLoc offset that appears as a literal source
 * in two or more ADDs, hoist a single ASSIGN of that StackLoc to a fresh
 * TEMP at the function entry, and replace each literal use with the TEMP.
 * After this, the ADDs have a register base (not is_local), so the
 * subsequent SHL+ADD indexed-memory fusion can fire.
 *
 * Safety: the hoisted ASSIGN happens at function entry (before any code
 * that could modify the frame pointer), so the address is constant for
 * the whole function lifetime.  The TEMP's value is just an FP-relative
 * pointer — same semantics as the literal.
 */
int tcc_ir_opt_stackoff_addr_cse(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  if (n < 2)
    return 0;

  /* Pass 1: count uses per unique StackLoc imm32 offset that appears as
   * a non-lval source operand of an ADD with a vreg other operand. */
#define SAC_MAX_OFFSETS 32
  struct {
    int32_t offset;
    int count;
    int32_t hoisted_vreg;
    IROperand sample; /* operand we cloned (for btype) */
  } slots[SAC_MAX_OFFSETS];
  int nslots = 0;

  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_ADD)
      continue;
    IROperand src1 = tcc_ir_op_get_src1(ir, q);
    IROperand src2 = tcc_ir_op_get_src2(ir, q);
    for (int sl = 0; sl < 2; sl++)
    {
      IROperand op = (sl == 0) ? src1 : src2;
      IROperand other = (sl == 0) ? src2 : src1;
      if (irop_get_tag(op) != IROP_TAG_STACKOFF)
        continue;
      if (op.is_lval)
        continue;
      /* Only consider patterns where the OTHER operand is a vreg (a
       * register-shifted index) — that's the SHL+ADD pattern that the
       * fusion wants to fold.  Constants on the other side are handled
       * by stack_addr_cse already. */
      if (!irop_has_vreg(other))
        continue;
      int32_t off = op.u.imm32;
      int slot = -1;
      for (int s = 0; s < nslots; s++)
        if (slots[s].offset == off) { slot = s; break; }
      if (slot < 0)
      {
        if (nslots >= SAC_MAX_OFFSETS)
          continue;
        slot = nslots++;
        slots[slot].offset = off;
        slots[slot].count = 0;
        slots[slot].hoisted_vreg = -1;
        slots[slot].sample = op;
      }
      slots[slot].count++;
    }
  }

  /* Pass 2: for each offset with >= 2 uses, hoist an ASSIGN at function
   * entry and rewrite all matching uses.  We insert at index 0 by shifting
   * the IR — for K hoists, that's K shifts; tolerable since K <= 32. */
  int changes = 0;
  for (int s = 0; s < nslots; s++)
  {
    if (slots[s].count < 2)
      continue;

    int32_t t_anon = tcc_ir_vreg_alloc_temp(ir);
    if (t_anon < 0)
      continue;

    /* Build ASSIGN T_anon <- Addr[StackLoc[off]] and insert at index 0.
     * Mirror the sample operand's btype/sign to keep the IR consistent. */
    IROperand new_dest = irop_make_vreg(t_anon, slots[s].sample.btype);
    new_dest.is_unsigned = slots[s].sample.is_unsigned;
    IROperand new_src = slots[s].sample;
    IRQuadCompact assign_q = {0};
    assign_q.op = TCCIR_OP_ASSIGN;
    assign_q.operand_base = tcc_ir_pool_add(ir, new_dest);
    tcc_ir_pool_add(ir, new_src);

    if (gsym_cse_insert_before(ir, 0, &assign_q) < 0)
      continue;
    n++; /* IR grew by 1 */
    slots[s].hoisted_vreg = t_anon;
  }

  if (changes >= 0)
  {
    /* Pass 3: rewrite uses (the indexes have shifted by the number of
     * hoists already inserted; each insert shifted EVERYTHING from idx 0
     * onward, so iterate fresh). */
    for (int i = 0; i < n; i++)
    {
      IRQuadCompact *q = &ir->compact_instructions[i];
      if (q->op != TCCIR_OP_ADD)
        continue;
      IROperand src1 = tcc_ir_op_get_src1(ir, q);
      IROperand src2 = tcc_ir_op_get_src2(ir, q);
      for (int sl = 0; sl < 2; sl++)
      {
        IROperand op = (sl == 0) ? src1 : src2;
        IROperand other = (sl == 0) ? src2 : src1;
        if (irop_get_tag(op) != IROP_TAG_STACKOFF || op.is_lval)
          continue;
        if (!irop_has_vreg(other))
          continue;
        int32_t off = op.u.imm32;
        int slot = -1;
        for (int s = 0; s < nslots; s++)
          if (slots[s].offset == off) { slot = s; break; }
        if (slot < 0 || slots[slot].hoisted_vreg < 0)
          continue;
        IROperand replacement = irop_make_vreg(slots[slot].hoisted_vreg, op.btype);
        replacement.is_unsigned = op.is_unsigned;
        if (sl == 0)
          tcc_ir_set_src1(ir, i, replacement);
        else
          tcc_ir_set_src2(ir, i, replacement);
        changes++;
      }
    }
  }

  LOG_IR_GEN("=== STACKOFF ADDR CSE: %d uses rewritten ===", changes);
  return changes;
#undef SAC_MAX_OFFSETS
}

/* ============================================================================
 * LEA + deref fold
 * ============================================================================
 *
 * The frontend materializes `&local_var` as an explicit `LEA` op that
 * computes the stack-slot address into a TEMP vreg, even when the very next
 * use is a deref.  On ARM this becomes `add rX, sp, #off; ldr rY, [rX]`
 * (2 instructions) instead of `ldr rY, [sp, #off]` (1 instruction).  GCC
 * picks the one-instruction form because it doesn't split the address into
 * a vreg first.
 *
 * Pattern A — LEA + consumer-with-deref:
 *   i0: T = LEA Addr[StackLoc[-N]]     (STACKOFF, is_lval=0)
 *   i1: <op> ... T***DEREF*** ...       (is_lval=1 on a T-valued operand)
 *
 * Pattern B — LEA + ADD(#K) + consumer-with-deref:
 *   i0: T1 = LEA Addr[StackLoc[-N]]
 *   i1: T2 = T1 ADD #K
 *   i2: <op> ... T2***DEREF*** ...
 *
 * Pattern C — ASSIGN Addr[StackLoc] + consumer-with-deref (semantically
 * identical to pattern A; the frontend emits ASSIGN instead of LEA when
 * the address materialization is part of a copy chain, again common in
 * nested-function inlining):
 *   i0: T = ASSIGN Addr[StackLoc[-N]]
 *   i1: <op> ... T***DEREF*** ...
 *
 * Transform: substitute the T-DEREF operand with the StackLoc itself at the
 * appropriate offset (is_lval=1 so the backend emits a direct stack load).
 * NOP the LEA/ASSIGN/ADD (and the ADD-interposer in pattern B).
 *
 * Safety constraints:
 *   - LEA source must be STACKOFF with is_lval=0 (i.e. Addr[StackLoc]).
 *   - LEA result must have exactly one use.  For pattern B, the ADD must
 *     also have exactly one use.
 *   - Consumer must reside in the same basic block.
 *   - Consumer must reference the LEA/ADD result with is_lval=1 exactly
 *     once (either src1 or src2 — never as the destination of a STORE
 *     through a LEA'd pointer; that's indistinguishable from a direct
 *     stack-slot store and is handled separately).
 */

int tcc_ir_opt_lea_fold(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  int changes = 0;

  if (n == 0)
    return 0;

  IROptDU du;
  ir_opt_du_build(ir, &du);

  LOG_IR_GEN("=== LEA FOLD START (n=%d) ===", n);

  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *lea_q = &ir->compact_instructions[i];

    /* Two entry shapes are handled:
     *   - LEA Addr[StackLoc[X]] -> T            (classic LEA form)
     *   - ASSIGN Addr[StackLoc[X]] -> T         (semantically identical to LEA;
     *     emitted by the frontend when materializing &local for nested-function
     *     inlining or other capture-via-address patterns)
     *
     * The combined ADD Addr[StackLoc[X]], #K form is deliberately not an entry
     * root here.  Folding it to a direct StackLoc access can remove the only
     * address-valued operation tying a constant subslot access to the enclosing
     * aggregate; later stack-slot passes then miss aliases through other
     * Addr[StackLoc] indexed accesses.  Keep that form explicit unless it is an
     * interposer after a real LEA/ASSIGN root, where the root still carries the
     * address-taken information for the aggregate. */
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
    /* `ir_opt_du_uses` relies on a def/use table where STORE's dest slot is
     * recorded as a *definition*, not a use — even though `T***DEREF*** <-- val
     * [STORE]` semantically uses T.  That undercounts real uses and caused
     * the pass to fold `LEA T; *T read; *T write` into `load StackLoc; NOP;
     * *T write` (dangling T).  Do an explicit linear-scan use count that
     * considers dest positions when is_lval=1. */
    {
      int total_uses = 0;
      for (int k = i + 1; k < n && total_uses < 2; k++)
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
          /* A dest with is_lval=1 is a *use* of the vreg (we deref through
           * it), not a redefinition. A dest without is_lval would redefine
           * lea_vr and end its live range — treat as a hard stop.
           *
           * Exception: STORE/STORE_INDEXED/STORE_POSTINC place the *base
           * pointer* in the dest slot, which is a USE.  disp_fusion clears
           * is_lval on STORE_INDEXED's base, so the is_lval test alone
           * would mis-classify it as a redef. */
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

    /* Use the accessor — STRUCT btype stores the offset in u.s.aux_data,
     * not u.imm32.  Reading u.imm32 directly on a struct-typed Addr[] gives
     * the concatenation of ctype_idx + aux_data and produces garbage. */
    int32_t base_offset = irop_get_stack_offset(lea_src);

    /* Find the single use of the LEA result. */
    int cur_idx = -1;
    for (int j = i + 1; j < n; j++)
    {
      IRQuadCompact *uq = &ir->compact_instructions[j];
      if (uq->op == TCCIR_OP_NOP)
        continue;
      /* Same-block check: the use must precede any control-flow edge. */
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

    /* Optional ADD #K interposer: a single intermediate ADD that consumes
     * the LEA result and adds a constant, whose own result has exactly one
     * use (the eventual deref consumer). */
    int add_idx = -1;
    int32_t add_offset = 0;
    IRQuadCompact *add_q = &ir->compact_instructions[cur_idx];
    if (add_q->op == TCCIR_OP_ADD)
    {
      IROperand a1 = tcc_ir_op_get_src1(ir, add_q);
      IROperand a2 = tcc_ir_op_get_src2(ir, add_q);
      /* Must be `lea_vr + #K` or `#K + lea_vr` with the other side IMM32 —
       * AND the vreg side must have is_lval=0.  A DEREF flag on that operand
       * means the ADD reads the value *stored at* lea_vr and adds K to
       * that (loaded-pointer arithmetic), not pointer-plus-offset.  Folding
       * this into a direct stack slot would read the struct layout instead
       * of following the loaded pointer. */
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
        /* Explicit-scan use count — ir_opt_du_uses undercounts STORE-dest
         * uses (treats `T***DEREF*** <-- val [STORE]` as a redefinition of
         * T rather than a use). */
        int add_uses_real = 0;
        if (add_vr >= 0 && TCCIR_DECODE_VREG_TYPE(add_vr) == TCCIR_VREG_TYPE_TEMP)
        {
          for (int k = cur_idx + 1; k < n && add_uses_real < 2; k++)
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
          for (int k = add_idx + 1; k < n; k++)
          {
            IRQuadCompact *ck = &ir->compact_instructions[k];
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

    /* Require the final consumer to reference the folded vreg exactly once
     * with is_lval=1.  Reject pure non-deref uses (e.g. the vreg flows into
     * a PARAM or another ADD) since the semantic change only holds when the
     * op actually dereferences through the address. */
    int32_t deref_vr =
        (add_idx >= 0) ? irop_get_vreg(tcc_ir_op_get_dest(ir, &ir->compact_instructions[add_idx])) : lea_vr;

    /* STORE_INDEXED / LOAD_INDEXED special case: their base pointer lives in
     * dest (STORE_INDEXED) or src1 (LOAD_INDEXED), the constant offset in
     * src2, and the shift amount in slot 3 (scale).  When base is the LEA's
     * vreg, scale==0, and src2 is IMM32, fold to a direct StackLoc STORE/LOAD
     * at offset (base + add_offset + index_imm).  This unlocks subsequent
     * stack-store-load forwarding and DSE on aggregate field writes. */
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
    /* Keep stores through the address temp explicit.  A direct StackLoc store
     * followed by direct StackLoc loads lets later scalar stack-slot passes
     * reason about one field while missing other aliases through the aggregate
     * address.  Read-side folds are still safe and keep the common load win. */
    if (which == 3)
      continue;

    IRQuadCompact *cons_q = &ir->compact_instructions[cur_idx];

    IROperand old_op = (which == 1)   ? tcc_ir_op_get_src1(ir, cons_q)
                       : (which == 2) ? tcc_ir_op_get_src2(ir, cons_q)
                                      : tcc_ir_op_get_dest(ir, cons_q);

    /* The *consumer* side determines where the folded offset is stored.
     * When the consumer reads the slot as a struct, its btype==STRUCT and
     * the offset must go through u.s.aux_data (irop_make_stackoff writes
     * u.imm32 unconditionally, which would corrupt ctype_idx).  Skip only
     * that case.
     *
     * lea_src.btype==STRUCT is fine — the source is the address of a
     * struct, but the scalar consumer (CMP/AND/LOAD of an int field) has
     * its own non-struct btype and we rebuild the operand from scratch
     * with irop_get_stack_offset() handling the struct-side read. */
    if (old_op.btype == IROP_BTYPE_STRUCT)
      continue;

    /* Build the substituted operand: direct StackLoc at the folded offset,
     * is_lval=1.  Build from scratch via irop_make_stackoff so bit-fields
     * and unused union members are cleanly initialized, then copy the
     * consumer's load-width info (btype/is_unsigned) onto it. */
    int folded_off = base_offset + add_offset;
    IROperand new_op = irop_make_stackoff(-1, folded_off, /*is_lval*/ 1, /*is_llocal*/ 0,
                                          /*is_param_flag*/ (int)lea_src.is_param, old_op.btype);
    new_op.is_unsigned = old_op.is_unsigned;
    new_op.is_static = lea_src.is_static;

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
  return changes;
}

/* ============================================================================
 * LEA read-modify-write fold
 * ============================================================================
 *
 * Generalizes tcc_ir_opt_lea_fold's single-use case to a plain
 * `Addr[StackLoc[X]]` LEA whose *every* use is a same-block stack-slot
 * dereference.  The canonical shape is `u.field++` / `u.field--`, which
 * materializes the field address once and dereferences it twice (load +
 * store):
 *
 *   T  = Addr[StackLoc[X]]            ; LEA / ASSIGN
 *   v  = T***DEREF***                 ; load  u.field
 *   v' = v <op> #k
 *   T***DEREF*** = v'  [STORE]        ; store u.field
 *
 * The single-use pass requires the LEA result to have exactly one use, so it
 * leaves these untouched.  Both derefs target the same slot, so each can be
 * rewritten to a direct StackLoc[X] access and the LEA dropped — exactly the
 * substitution the single-use path performs, just applied to every deref.  An
 * optional single `T2 = T ADD #K` interposer (for a field at a non-zero
 * struct offset) folds K into the offset.
 *
 * Safety: every use of the LEA result (and of any interposer result) within
 * the function must be a same-block deref — an is_lval load operand or a plain
 * STORE base at the folded offset.  Any non-deref use (the address escaping
 * into a PARAM/call/non-lval op, a STORE_INDEXED/LOAD_INDEXED base, a struct
 * read, or a use past a control-flow edge) disables the fold for that LEA.  No
 * instruction is moved; only operand forms change from pointer-deref to
 * direct-slot, so program order and aliasing are preserved.
 *
 * Two further restrictions keep the *direct StackLoc* form (which the
 * downstream DSE chain reasons about more precisely than an opaque LEA-deref)
 * from exposing partial-overwrite hazards — see the inline comments at the
 * deref-site classification:
 *   - accesses must be 8 bytes wide (long long / double), so a folded store is
 *     never a strict sub-range of a wider store to the same slot; and
 *   - a STORE whose value is a masked bit-merge (OR/AND of a load of the same
 *     slot — the bitfield write-back idiom) is left as an LEA-deref, since the
 *     initializing store stays semantically live under it.
 */

#define LEA_RMW_MAX_SITES 32

int tcc_ir_opt_lea_rmw_fold(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  int changes = 0;
  if (n == 0)
    return 0;

  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *lea_q = &ir->compact_instructions[i];

    /* Entry shape: plain LEA / ASSIGN of Addr[StackLoc[X]] (no vreg base,
     * no double-indirect) into a TEMP. */
    if (lea_q->op == TCCIR_OP_ASSIGN)
    {
      IROperand s2 = tcc_ir_op_get_src2(ir, lea_q);
      if (!irop_is_none(s2))
        continue;
    }
    else if (lea_q->op != TCCIR_OP_LEA)
      continue;

    IROperand lea_src = tcc_ir_op_get_src1(ir, lea_q);
    if (irop_get_tag(lea_src) != IROP_TAG_STACKOFF)
      continue;
    if (lea_src.is_lval || lea_src.is_llocal)
      continue;
    if (irop_get_vreg(lea_src) != -1) /* vreg-backed spill slot — see lea_fold */
      continue;

    IROperand lea_dest = tcc_ir_op_get_dest(ir, lea_q);
    int32_t lea_vr = irop_get_vreg(lea_dest);
    if (lea_vr < 0 || TCCIR_DECODE_VREG_TYPE(lea_vr) != TCCIR_VREG_TYPE_TEMP)
      continue;

    int32_t base_offset = irop_get_stack_offset(lea_src);

    /* Block end: first control-flow edge after the LEA.  Any use of the LEA
     * result at or beyond this point crosses a basic-block boundary. */
    int bb_end = n;
    for (int k = i + 1; k < n; k++)
    {
      IRQuadCompact *q = &ir->compact_instructions[k];
      if (q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_JUMPIF)
      {
        bb_end = k;
        break;
      }
    }

    /* Worklist of address vregs derived from the LEA, tagged with their
     * offset from the slot base and their defining index (skipped on scan).
     * Entry 0 is the LEA result itself at offset 0. */
    int32_t wl_vr[LEA_RMW_MAX_SITES];
    int32_t wl_off[LEA_RMW_MAX_SITES];
    int wl_def[LEA_RMW_MAX_SITES];
    int wl_n = 1;
    wl_vr[0] = lea_vr;
    wl_off[0] = 0;
    wl_def[0] = i;

    /* Deref sites to redirect at a direct StackLoc. */
    int rw_idx[LEA_RMW_MAX_SITES];
    int rw_which[LEA_RMW_MAX_SITES];
    int32_t rw_off[LEA_RMW_MAX_SITES];
    int rw_n = 0;

    int ok = 1;
    for (int w = 0; w < wl_n && ok; w++)
    {
      int32_t av = wl_vr[w];
      int32_t aoff = wl_off[w];
      int adef = wl_def[w];

      for (int k = i + 1; k < n && ok; k++)
      {
        if (k == adef)
          continue;
        IRQuadCompact *q = &ir->compact_instructions[k];
        if (q->op == TCCIR_OP_NOP)
          continue;
        const IRRegistersConfig *cfg = &irop_config[q->op];

        IROperand s1 = cfg->has_src1 ? tcc_ir_op_get_src1(ir, q) : IROP_NONE;
        IROperand s2 = cfg->has_src2 ? tcc_ir_op_get_src2(ir, q) : IROP_NONE;
        IROperand d = cfg->has_dest ? tcc_ir_op_get_dest(ir, q) : IROP_NONE;

        int ref_lval = 0, ref_nonlval = 0, which_lval = -1;
        if (cfg->has_src1 && irop_has_vreg(s1) && irop_get_vreg(s1) == av)
        {
          if (s1.is_lval) { ref_lval++; which_lval = 1; }
          else ref_nonlval++;
        }
        if (cfg->has_src2 && irop_has_vreg(s2) && irop_get_vreg(s2) == av)
        {
          if (s2.is_lval) { ref_lval++; which_lval = 2; }
          else ref_nonlval++;
        }
        if (cfg->has_dest && irop_has_vreg(d) && irop_get_vreg(d) == av)
        {
          if (d.is_lval) { ref_lval++; which_lval = 0; }
          else ref_nonlval++;
        }

        if (ref_lval == 0 && ref_nonlval == 0)
          continue; /* instruction does not touch av */

        if (k >= bb_end)
        {
          ok = 0;
          break;
        }

        /* Interposer `new = av + #K` (av as a plain pointer value). */
        if (q->op == TCCIR_OP_ADD && ref_lval == 0 && ref_nonlval == 1)
        {
          int32_t kk;
          if (irop_has_vreg(s1) && irop_get_vreg(s1) == av && !s1.is_lval &&
              irop_get_tag(s2) == IROP_TAG_IMM32)
            kk = (int32_t)s2.u.imm32;
          else if (irop_has_vreg(s2) && irop_get_vreg(s2) == av && !s2.is_lval &&
                   irop_get_tag(s1) == IROP_TAG_IMM32)
            kk = (int32_t)s1.u.imm32;
          else
          {
            ok = 0;
            break;
          }
          int32_t nvr = irop_get_vreg(d);
          if (nvr < 0 || TCCIR_DECODE_VREG_TYPE(nvr) != TCCIR_VREG_TYPE_TEMP || d.is_lval ||
              wl_n >= LEA_RMW_MAX_SITES)
          {
            ok = 0;
            break;
          }
          wl_vr[wl_n] = nvr;
          wl_off[wl_n] = aoff + kk;
          wl_def[wl_n] = k;
          wl_n++;
          continue;
        }

        /* Otherwise must be a single clean deref (load operand or STORE base)
         * of a non-struct width. */
        if (ref_lval != 1 || ref_nonlval != 0 || rw_n >= LEA_RMW_MAX_SITES)
        {
          ok = 0;
          break;
        }
        IROperand dref = (which_lval == 1) ? s1 : (which_lval == 2) ? s2 : d;
        /* Restrict to 8-byte (long long / double) accesses — the pr92904
         * struct-field RMW this pass targets.  Converting an LEA-deref store
         * into a *direct* StackLoc store changes how the downstream DSE chain
         * reasons about it, and narrower stores are unsafe to fold: a
         * byte/halfword field store lands at a sub-offset of a wider store to
         * the same slot (e.g. a 4-byte param store spanning a `char` field),
         * and DSE then drops the wider store once its only exact-offset reader
         * — the now-dead narrow RMW load — is DCE'd.  8-byte field RMW is
         * naturally aligned and never a strict sub-range of another store, so
         * the sub-offset hazard cannot arise; narrower accesses stay as
         * LEA-derefs, which the DSE chain treats opaquely and handles
         * correctly. */
        if (dref.btype != IROP_BTYPE_INT64 && dref.btype != IROP_BTYPE_FLOAT64)
        {
          ok = 0;
          break;
        }
        /* Reject *bitfield* write-backs even at 8-byte width.  A bitfield store
         * is a partial-bits update — its value is a masked merge of the slot's
         * prior content (`(load & ~mask) | bits`), so the initializing store
         * stays semantically live.  As a direct StackLoc store, however, the
         * DSE/loop passes treat it as a clean full-word overwrite and drop the
         * write-back (or the init), miscompiling e.g. `unsigned long long b:1`
         * decremented in a loop.  The merge always tops out in an OR/AND that
         * consumes a load of this same slot, so a STORE whose value is defined
         * by OR/AND is conservatively left as an LEA-deref.  Plain arithmetic
         * RMW (`a++`, `a += k`, `a -= k`) — the pr92904 case — feeds the store
         * from ADD/SUB/FADD/FSUB and is unaffected. */
        if (which_lval == 0 && q->op == TCCIR_OP_STORE && irop_has_vreg(s1) && !s1.is_lval)
        {
          int32_t vvr = irop_get_vreg(s1);
          for (int d2 = k - 1; d2 >= 0; d2--)
          {
            IRQuadCompact *dq = &ir->compact_instructions[d2];
            if (dq->op == TCCIR_OP_NOP)
              continue;
            if (!irop_config[dq->op].has_dest)
              continue;
            if (irop_get_vreg(tcc_ir_op_get_dest(ir, dq)) != vvr)
              continue;
            if (dq->op == TCCIR_OP_OR || dq->op == TCCIR_OP_AND)
              ok = 0;
            break; /* found the def */
          }
          if (!ok)
            break;
        }
        rw_idx[rw_n] = k;
        rw_which[rw_n] = which_lval;
        rw_off[rw_n] = aoff;
        rw_n++;
      }
    }

    if (!ok || rw_n == 0)
      continue;

    /* Apply: redirect every deref operand at a direct StackLoc, then NOP the
     * LEA and every interposer ADD. */
    for (int r = 0; r < rw_n; r++)
    {
      IRQuadCompact *cq = &ir->compact_instructions[rw_idx[r]];
      int which = rw_which[r];
      IROperand old_op = (which == 1)   ? tcc_ir_op_get_src1(ir, cq)
                         : (which == 2) ? tcc_ir_op_get_src2(ir, cq)
                                        : tcc_ir_op_get_dest(ir, cq);
      int32_t folded_off = base_offset + rw_off[r];
      IROperand new_op = irop_make_stackoff(-1, folded_off, /*is_lval*/ 1, /*is_llocal*/ 0,
                                            /*is_param_flag*/ (int)lea_src.is_param, old_op.btype);
      new_op.is_unsigned = old_op.is_unsigned;
      new_op.is_static = lea_src.is_static;
      if (which == 1)
        tcc_ir_op_set_src1(ir, cq, new_op);
      else if (which == 2)
        tcc_ir_op_set_src2(ir, cq, new_op);
      else
        tcc_ir_op_set_dest(ir, cq, new_op);
    }

    lea_q->op = TCCIR_OP_NOP;
    for (int w = 1; w < wl_n; w++)
      ir->compact_instructions[wl_def[w]].op = TCCIR_OP_NOP;

    changes++;
    LOG_IR_GEN("LEA RMW FOLD: LEA@%d base=%d -> %d deref sites, %d interposers", i, base_offset, rw_n,
               wl_n - 1);
  }

  return changes;
}

/* ============================================================================
 * Combined Boolean Pass
 * ============================================================================
 *
 * Runs cse_bool and bool_idempotent in a single forward loop (one scan instead
 * of two).  Within each BOOL_AND/BOOL_OR instruction, idempotent simplification
 * runs first; if it fires the CSE table lookup is skipped for that instruction.
 *
 * do_idempotent: run bool_idempotent logic (a&&a→a, a&&1→a, a||0→a)
 * do_cse:        run cse_bool logic (eliminate duplicate bool ops)
 *
 * Returns total number of changes.
 */

int tcc_ir_opt_assign_fuse(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  int changes = 0;
  if (n < 2)
    return 0;

  IROptDU du;
  ir_opt_du_build_mode(ir, &du, IR_DU_MODE_TMP_ONLY);

  for (int i = 1; i < n; i++)
  {
    IRQuadCompact *q_asn = &ir->compact_instructions[i];
    if (q_asn->op != TCCIR_OP_ASSIGN)
      continue;
    if (q_asn->is_jump_target)
      continue;

    IROperand asn_src = tcc_ir_op_get_src1(ir, q_asn);
    int32_t src_vr = irop_get_vreg(asn_src);
    if (TCCIR_DECODE_VREG_TYPE(src_vr) != TCCIR_VREG_TYPE_TEMP)
      continue;
    if (ir_opt_du_uses(&du, src_vr) != 1 || !ir_opt_du_is_single_def(&du, src_vr))
      continue;
    if (asn_src.is_lval)
      continue;

    int def_i = ir_opt_du_def(&du, src_vr, n);
    if (def_i < 0 || def_i >= i)
      continue;

    /* The producer must be the immediately preceding non-NOP instruction
     * in the same basic block (no jump targets between them). */
    int between_ok = 1;
    for (int j = def_i + 1; j < i; j++)
    {
      IRQuadCompact *qj = &ir->compact_instructions[j];
      if (qj->op != TCCIR_OP_NOP) { between_ok = 0; break; }
      if (qj->is_jump_target) { between_ok = 0; break; }
    }
    if (!between_ok)
      continue;

    IRQuadCompact *q_def = &ir->compact_instructions[def_i];
    /* Only fuse defs whose dest semantics is a plain register write. */
    switch (q_def->op)
    {
    case TCCIR_OP_NOP:
    case TCCIR_OP_STORE:
    case TCCIR_OP_STORE_INDEXED:
    case TCCIR_OP_STORE_POSTINC:
    case TCCIR_OP_FUNCPARAMVAL:
    case TCCIR_OP_FUNCCALLVAL:  /* call result lands in a fixed register */
    case TCCIR_OP_CMP:
    case TCCIR_OP_TEST_ZERO:
    case TCCIR_OP_JUMP:
    case TCCIR_OP_JUMPIF:
    case TCCIR_OP_IJUMP:
    case TCCIR_OP_RETURNVOID:
    case TCCIR_OP_RETURNVALUE:
    case TCCIR_OP_VLA_ALLOC:
    case TCCIR_OP_VLA_SP_SAVE:
    case TCCIR_OP_VLA_SP_RESTORE:
      continue;
    default:
      break;
    }

    /* Skip if the ASSIGN's dest types differ from the source's: a
     * sub-word ASSIGN may truncate or widen, which the producer can't
     * faithfully reproduce by writing to a different dest. */
    IROperand asn_dest = tcc_ir_op_get_dest(ir, q_asn);
    IROperand def_dest = tcc_ir_op_get_dest(ir, q_def);
    if (irop_get_btype(asn_dest) != irop_get_btype(def_dest))
      continue;
    if (asn_dest.is_lval)
      continue;

    /* Rewrite: producer's dest = ASSIGN's dest; NOP the ASSIGN. */
    LOG_IR_GEN("OPTIMIZE: assign_fuse def_i=%d asn_i=%d (T%d → T%d)", def_i, i,
               TCCIR_DECODE_VREG_POSITION(src_vr), TCCIR_DECODE_VREG_POSITION(irop_get_vreg(asn_dest)));
    tcc_ir_set_dest(ir, def_i, asn_dest);
    q_asn->op = TCCIR_OP_NOP;
    changes++;
  }

  tcc_free(du.def);
  return changes;
}

int tcc_ir_opt_assign_fuse_ex(IROptCtx *ctx) { return tcc_ir_opt_assign_fuse(ctx->ir); }
