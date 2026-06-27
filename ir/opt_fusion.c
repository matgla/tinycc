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


int tcc_ir_opt_postinc_fusion(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  int changes = 0;

  if (n == 0)
    return 0;

  LOG_IR_GEN("=== POSTINC FUSION START (n=%d) ===", n);

  /* ---------------------------------------------------------------------------
   * Revised post-increment fusion (LOAD and STORE).
   *
   * Previous implementation had three fundamental problems:
   *
   * 1. ASSIGN tracing:  tracing through ASSIGN to find an "original pointer"
   *    allowed the ADD search to match against orig_ptr_vr.  After earlier
   *    optimisation passes (copy-prop, store-load-fwd, redundant-store-elim)
   *    rearranged and merged instructions, a LOAD from the first *p++ could
   *    be incorrectly fused with the ADD from the *second* p++, because both
   *    ADDs reference the same original variable.
   *
   * 2. Implicit writeback not modelled:  ARM LOAD_POSTINC (ldr Rd,[Rn],#imm)
   *    updates Rn in-place, but the IR has no way to express this side-effect.
   *    The register allocator treats the pointer operand as input-only, so
   *    after LOAD_POSTINC the updated value can be lost through spilling or
   *    register re-use.
   *
   * 3. Overly aggressive NOP-ing:  the old code NOPed the ASSIGN (pointer
   *    copy), ADD (increment) and STORE (writeback) — removing the entire
   *    pointer update chain.  If the codegen failed to propagate the
   *    implicit ARM writeback, the pointer was never incremented.
   *
   * New rules
   * =========
   *
   * a)  Fuse LOAD and STORE instructions.
   *
   * b)  The LOAD's pointer must be a TEMP vreg (is_local=0) that holds a
   *     pointer value to dereference.
   *
   * c)  The matching ADD must be *immediately* after the LOAD (the very
   *     next non-NOP instruction — no search window).  This prevents
   *     cross-matching between interleaved post-increment operations.
   *
   * d)  The ADD's pointer source must be *exactly* ptr_vr (the LOAD's own
   *     pointer TEMP).  No ASSIGN tracing, no orig_ptr matching.
   *
   * e)  Instead of NOP-ing the ADD, transform it into
   *         ASSIGN  add_result, ptr_vr
   *     After the ARM LOAD_POSTINC instruction executes, the register
   *     holding ptr_vr contains ptr+offset.  The ASSIGN propagates that
   *     updated value to the ADD's original result vreg so that any
   *     downstream STORE (writing the incremented pointer back to the
   *     variable's stack slot) still works correctly.
   *
   * f)  Never NOP any ASSIGN or STORE instruction.  The original pointer
   *     copy (ASSIGN tmp, p) and writeback (STORE [p_slot], result) stay
   *     intact, guaranteeing the pointer update reaches its stack slot.
   *
   * Net effect: one fewer instruction executed per post-increment (the ADD
   * is replaced by a cheaper ASSIGN that the codegen can often elide) and
   * the ARM post-indexed addressing mode saves a cycle.
   * ------------------------------------------------------------------------ */

  for (int i = 0; i < n - 1; i++)
  {
    IRQuadCompact *mem_q = &ir->compact_instructions[i];

    /* (a) Fuse LOAD and STORE instructions. */
    int is_load = (mem_q->op == TCCIR_OP_LOAD);
    int is_store = (mem_q->op == TCCIR_OP_STORE);
    if (!is_load && !is_store)
      continue;

    /* LOAD: src1=pointer, dest=loaded_value
     * STORE: dest=pointer (is_lval), src1=stored_value */
    IROperand ptr_op, val_op;
    if (is_load)
    {
      ptr_op = tcc_ir_op_get_src1(ir, mem_q);
      val_op = tcc_ir_op_get_dest(ir, mem_q);
    }
    else
    {
      ptr_op = tcc_ir_op_get_dest(ir, mem_q);
      val_op = tcc_ir_op_get_src1(ir, mem_q);
    }

    /* (b) Pointer must be a TEMP vreg, not a stack-local variable. */
    if (!irop_has_vreg(ptr_op))
      continue;
    if (ptr_op.is_local)
      continue;

    int32_t ptr_vr = irop_get_vreg(ptr_op);

    /* Pointer must be a TEMP (register-resident). */
    if (TCCIR_DECODE_VREG_TYPE(ptr_vr) != TCCIR_VREG_TYPE_TEMP)
      continue;

    /* Loaded/stored value must not alias the pointer register. */
    if (irop_has_vreg(val_op) && irop_get_vreg(val_op) == ptr_vr)
      continue;

    /* (c) Find the ADD within a small window, ensuring ptr_vr is not used and no control flow changes. */
    int add_idx = -1;
    int unsafe = 0;
    for (int j = i + 1; j < n && j < i + 10; j++)
    {
      IRQuadCompact *uq = &ir->compact_instructions[j];
      if (uq->op == TCCIR_OP_NOP)
        continue;

      /* Stop at basic block boundaries */
      if (uq->is_jump_target || uq->op == TCCIR_OP_JUMP || uq->op == TCCIR_OP_JUMPIF || uq->op == TCCIR_OP_IJUMP)
      {
        unsafe = 1;
        break;
      }

      /* Check if this is our ADD */
      if (uq->op == TCCIR_OP_ADD)
      {
        IROperand s1 = tcc_ir_op_get_src1(ir, uq);
        IROperand s2 = tcc_ir_op_get_src2(ir, uq);
        int s1_vr = irop_get_vreg(s1);
        int s2_vr = irop_get_vreg(s2);
        if ((irop_has_vreg(s1) && s1_vr == ptr_vr) || (irop_has_vreg(s2) && s2_vr == ptr_vr))
        {
          add_idx = j;
          break;
        }
      }

      /* Check if ptr_vr is used or modified by this intermediate instruction */
      if (irop_config[uq->op].has_src1)
      {
        IROperand s1 = tcc_ir_op_get_src1(ir, uq);
        if (irop_has_vreg(s1) && irop_get_vreg(s1) == ptr_vr)
          unsafe = 1;
      }
      if (irop_config[uq->op].has_src2)
      {
        IROperand s2 = tcc_ir_op_get_src2(ir, uq);
        if (irop_has_vreg(s2) && irop_get_vreg(s2) == ptr_vr)
          unsafe = 1;
      }
      if (irop_config[uq->op].has_dest)
      {
        IROperand d = tcc_ir_op_get_dest(ir, uq);
        if (irop_has_vreg(d) && irop_get_vreg(d) == ptr_vr)
          unsafe = 1;
      }

      if (unsafe)
        break;
    }
    if (add_idx < 0 || unsafe)
      continue;

    IRQuadCompact *add_q = &ir->compact_instructions[add_idx];

    /* (d) The ADD must use exactly ptr_vr as one source. */
    IROperand add_src1 = tcc_ir_op_get_src1(ir, add_q);
    IROperand add_src2 = tcc_ir_op_get_src2(ir, add_q);
    int s1_vr = irop_get_vreg(add_src1);
    int s2_vr = irop_get_vreg(add_src2);
    int ptr_is_src1 = (irop_has_vreg(add_src1) && s1_vr == ptr_vr);
    int ptr_is_src2 = (irop_has_vreg(add_src2) && s2_vr == ptr_vr);
    if (!ptr_is_src1 && !ptr_is_src2)
      continue;

    /* The other operand must be an immediate constant in [1..255]. */
    IROperand offset_op = ptr_is_src1 ? add_src2 : add_src1;
    if (!offset_op.is_const)
      continue;
    int offset = offset_op.u.imm32;
    if (offset < 1 || offset > 255)
      continue;

    /* Ensure the operand pool has room for 4 slots. */
    int new_base_idx = ir->iroperand_pool_count;
    if (new_base_idx + 4 > ir->iroperand_pool_capacity)
      continue;

    /* ---- Apply transformation ---- */

    /* Allocate 4 operand slots: dest, src1, unused, offset */
    tcc_ir_pool_add(ir, IROP_NONE);
    tcc_ir_pool_add(ir, IROP_NONE);
    tcc_ir_pool_add(ir, IROP_NONE);
    tcc_ir_pool_add(ir, IROP_NONE);

    mem_q->operand_base = new_base_idx;
    if (is_load)
    {
      /* LOAD_POSTINC: slot0=loaded_value (dest), slot1=ptr (src1) */
      ir->iroperand_pool[new_base_idx + 0] = val_op; /* loaded value (dest) */
      ir->iroperand_pool[new_base_idx + 1] = ptr_op; /* pointer TEMP (input, updated by HW) */
    }
    else
    {
      /* STORE_POSTINC: slot0=ptr (dest, treated as USE by liveness),
       *                slot1=value (src1, data to store)
       * Clear is_lval on ptr: the STORE's dest has is_lval=1 (meaning
       * "dereference as address") but STORE_POSTINC codegen expects the
       * raw pointer register.  The post-indexed STR instruction handles
       * the dereference implicitly.  Keeping is_lval=1 would cause the
       * codegen to emit a spurious LDR to dereference the pointer first. */
      IROperand store_ptr = ptr_op;
      store_ptr.is_lval = 0;
      ir->iroperand_pool[new_base_idx + 0] = store_ptr; /* pointer (dest) */
      ir->iroperand_pool[new_base_idx + 1] = val_op;    /* value to store (src1) */
    }
    ir->iroperand_pool[new_base_idx + 2] = IROP_NONE; /* unused */
    ir->iroperand_pool[new_base_idx + 3] = irop_make_imm32(-1, offset, IROP_BTYPE_INT32);
    mem_q->op = is_load ? TCCIR_OP_LOAD_POSTINC : TCCIR_OP_STORE_POSTINC;

    /* (e) Transform the ADD into ASSIGN add_result := ptr_vr.
     *     After the ARM post-indexed load/store, the register holding
     *     ptr_vr contains ptr + offset.  The ASSIGN propagates that
     *     value to the original ADD result vreg so downstream code
     *     (especially the STORE that writes back to the variable's
     *     stack slot) sees the correct incremented pointer.
     *
     *     We reuse the ADD's existing operand slots: overwrite src1 with
     *     ptr_op (the TEMP pointer) and clear src2. The dest (add_result)
     *     stays unchanged.
     */
    add_q->op = TCCIR_OP_ASSIGN;
    {
      /* Build an ASSIGN source from ptr_op that is a plain register value
       * (not an lvalue dereference).  The original ptr_op comes from the
       * LOAD's src1 or STORE's dest, which has is_lval=1 (meaning
       * "dereference this register as a pointer").  For the ASSIGN we
       * want the *register contents* — the updated pointer value — not
       * another dereference. */
      IROperand assign_src = ptr_op;
      assign_src.is_lval = 0;
      tcc_ir_set_src1(ir, add_idx, assign_src);
    }
    /* ASSIGN has no src2 — the old src2 slot is ignored (has_src2=0 for ASSIGN). */

    changes++;

    LOG_IR_GEN("POSTINC FUSION: %s@%d + ADD@%d -> %s_POSTINC + ASSIGN (ptr_vr=%d, offset=%d)",
               is_load ? "LOAD" : "STORE", i, add_idx, is_load ? "LOAD" : "STORE", ptr_vr, offset);
  }

  /* ---------------------------------------------------------------------------
   * Reverse-order pattern: ADD new_ptr, ptr, #imm   ;   LOAD val, ptr
   *
   * The C idiom `c = *p++` is sometimes lowered as
   *     new_ptr = ptr + 1
   *     val     = *ptr            (uses pre-increment value)
   * — i.e. the increment is emitted *before* the load even though the load
   * uses the unincremented pointer.  The forward pass above only matches
   * LOAD-then-ADD, so this form was missed entirely (see strncmp-style loops).
   *
   * We accept the reverse order under stricter constraints:
   *   (1) `ptr` is a TEMP vreg, `new_ptr` is a different vreg.
   *   (2) The LOAD is the next non-NOP instruction; no jump-target / control
   *       flow / writes to ptr or new_ptr in between.
   *   (3) `ptr` is dead after the LOAD (no later reads anywhere — a re-def
   *       counts as killing the live range and is fine).
   *   (4) The instruction slot immediately after the LOAD is a NOP we can
   *       repurpose for the ASSIGN, which must be sequenced *after* the
   *       LOAD_POSTINC (so it observes the hardware writeback).
   *
   * Transform:
   *   ADD new_ptr, ptr, #imm   ->   NOP
   *   LOAD val, ptr            ->   LOAD_POSTINC val, ptr, #imm
   *   <NOP slot at load+1>     ->   ASSIGN new_ptr, ptr
   * ------------------------------------------------------------------------ */
  for (int i = 0; i < n - 1; i++)
  {
    IRQuadCompact *add_q = &ir->compact_instructions[i];
    if (add_q->op != TCCIR_OP_ADD)
      continue;

    IROperand add_dest = tcc_ir_op_get_dest(ir, add_q);
    IROperand add_s1 = tcc_ir_op_get_src1(ir, add_q);
    IROperand add_s2 = tcc_ir_op_get_src2(ir, add_q);

    LOG_IR_GEN("POSTINC FUSION (rev) try @%d: ADD dest_vr=%d local=%d is_temp=%d s1{vr=%d const=%d local=%d} s2{vr=%d const=%d local=%d}",
               i, irop_get_vreg(add_dest), add_dest.is_local,
               (irop_has_vreg(add_dest) && TCCIR_DECODE_VREG_TYPE(irop_get_vreg(add_dest)) == TCCIR_VREG_TYPE_TEMP),
               irop_get_vreg(add_s1), add_s1.is_const, add_s1.is_local,
               irop_get_vreg(add_s2), add_s2.is_const, add_s2.is_local);

    /* (1) Identify ptr (vreg) + offset (immediate). */
    IROperand ptr_op_local;
    int32_t ptr_vr = -1;
    int offset = 0;
    if (irop_has_vreg(add_s1) && add_s2.is_const && !add_s2.is_sym)
    {
      ptr_op_local = add_s1;
      ptr_vr = irop_get_vreg(add_s1);
      offset = add_s2.u.imm32;
    }
    else if (irop_has_vreg(add_s2) && add_s1.is_const && !add_s1.is_sym)
    {
      ptr_op_local = add_s2;
      ptr_vr = irop_get_vreg(add_s2);
      offset = add_s1.u.imm32;
    }
    else
    {
      LOG_IR_GEN("POSTINC FUSION (rev) @%d: skip - no ptr+imm pattern", i);
      continue;
    }

    if (ptr_vr < 0)
      continue;
    if (ptr_op_local.is_local)
    {
      LOG_IR_GEN("POSTINC FUSION (rev) @%d: skip - ptr is local vr=%d", i, ptr_vr);
      continue;
    }
    if (TCCIR_DECODE_VREG_TYPE(ptr_vr) != TCCIR_VREG_TYPE_TEMP)
    {
      LOG_IR_GEN("POSTINC FUSION (rev) @%d: skip - ptr not TEMP vr=%d type=%d", i, ptr_vr, TCCIR_DECODE_VREG_TYPE(ptr_vr));
      continue;
    }
    if (offset < 1 || offset > 255)
    {
      LOG_IR_GEN("POSTINC FUSION (rev) @%d: skip - bad offset %d", i, offset);
      continue;
    }

    if (!irop_has_vreg(add_dest))
      continue;
    int32_t new_vr = irop_get_vreg(add_dest);
    if (new_vr < 0 || new_vr == ptr_vr)
    {
      LOG_IR_GEN("POSTINC FUSION (rev) @%d: skip - bad new_vr=%d", i, new_vr);
      continue;
    }
    LOG_IR_GEN("POSTINC FUSION (rev) @%d: candidate ptr_vr=%d new_vr=%d offset=%d", i, ptr_vr, new_vr, offset);

    /* (2) Locate immediately-following LOAD on ptr_vr; bail on any
     *     intervening touch of ptr_vr or new_vr or control flow. */
    int load_idx = -1;
    int unsafe = 0;
    for (int j = i + 1; j < n && j < i + 10; j++)
    {
      IRQuadCompact *uq = &ir->compact_instructions[j];
      if (uq->op == TCCIR_OP_NOP)
        continue;

      if (uq->is_jump_target || uq->op == TCCIR_OP_JUMP || uq->op == TCCIR_OP_JUMPIF || uq->op == TCCIR_OP_IJUMP)
      {
        LOG_IR_GEN("POSTINC FUSION (rev) @%d: unsafe at j=%d op=%d (jump/jt)", i, j, uq->op);
        unsafe = 1;
        break;
      }

      if (uq->op == TCCIR_OP_LOAD)
      {
        IROperand l_s1 = tcc_ir_op_get_src1(ir, uq);
        IROperand l_d = tcc_ir_op_get_dest(ir, uq);
        LOG_IR_GEN("POSTINC FUSION (rev) @%d: LOAD@%d s1_vr=%d d_vr=%d (need ptr_vr=%d)", i, j,
                   irop_get_vreg(l_s1), irop_get_vreg(l_d), ptr_vr);
        if (irop_has_vreg(l_s1) && irop_get_vreg(l_s1) == ptr_vr)
        {
          /* Loaded value must not alias ptr_vr or new_vr. */
          if (irop_has_vreg(l_d))
          {
            int32_t l_d_vr = irop_get_vreg(l_d);
            if (l_d_vr == ptr_vr || l_d_vr == new_vr)
            {
              LOG_IR_GEN("POSTINC FUSION (rev) @%d: skip - dest aliases ptr/new", i);
              break;
            }
          }
          load_idx = j;
          break;
        }
      }

      /* Any other touch of ptr_vr / new_vr in the gap is unsafe. */
      if (irop_config[uq->op].has_src1)
      {
        IROperand s1 = tcc_ir_op_get_src1(ir, uq);
        if (irop_has_vreg(s1))
        {
          int32_t v = irop_get_vreg(s1);
          if (v == ptr_vr || v == new_vr)
          {
            unsafe = 1;
            break;
          }
        }
      }
      if (irop_config[uq->op].has_src2)
      {
        IROperand s2 = tcc_ir_op_get_src2(ir, uq);
        if (irop_has_vreg(s2))
        {
          int32_t v = irop_get_vreg(s2);
          if (v == ptr_vr || v == new_vr)
          {
            unsafe = 1;
            break;
          }
        }
      }
      if (irop_config[uq->op].has_dest)
      {
        IROperand d = tcc_ir_op_get_dest(ir, uq);
        if (irop_has_vreg(d))
        {
          int32_t v = irop_get_vreg(d);
          if (v == ptr_vr || v == new_vr)
          {
            unsafe = 1;
            break;
          }
        }
      }
    }
    if (load_idx < 0 || unsafe)
    {
      LOG_IR_GEN("POSTINC FUSION (rev) @%d: skip - load_idx=%d unsafe=%d", i, load_idx, unsafe);
      continue;
    }

    /* (3) ptr_vr must be dead after the LOAD (no later reads).  A re-def
     *     kills the live range — stop scanning at that point. */
    int has_later_use = 0;
    for (int k = load_idx + 1; k < n; k++)
    {
      IRQuadCompact *kq = &ir->compact_instructions[k];
      if (kq->op == TCCIR_OP_NOP)
        continue;

      int killed = 0;
      if (irop_config[kq->op].has_src1)
      {
        IROperand s1 = tcc_ir_op_get_src1(ir, kq);
        if (irop_has_vreg(s1) && irop_get_vreg(s1) == ptr_vr)
        {
          has_later_use = 1;
          break;
        }
      }
      if (irop_config[kq->op].has_src2)
      {
        IROperand s2 = tcc_ir_op_get_src2(ir, kq);
        if (irop_has_vreg(s2) && irop_get_vreg(s2) == ptr_vr)
        {
          has_later_use = 1;
          break;
        }
      }
      if (irop_config[kq->op].has_dest)
      {
        IROperand d = tcc_ir_op_get_dest(ir, kq);
        if (irop_has_vreg(d) && irop_get_vreg(d) == ptr_vr)
        {
          killed = 1;
        }
      }
      if (killed)
        break;
    }
    if (has_later_use)
    {
      LOG_IR_GEN("POSTINC FUSION (rev) @%d: skip - ptr_vr=%d has later use", i, ptr_vr);
      continue;
    }

    /* (3b) Walk ptr_vr's defining chain through ASSIGN/LOAD copies.  If the
     *      root of the chain is a PARAM or other longer-lived vreg that is
     *      still read after load_idx, refuse to fuse: copy-prop / regalloc
     *      coalescing will share the register, and the HW writeback would
     *      clobber that vreg's stored value.  This is the strncmp-vs-parse_int
     *      distinction — strncmp's P0 is dead after V0=P0, parse_int's is not. */
    {
      int chain_vr = ptr_vr;
      int chain_unsafe = 0;
      for (int d = 0; d < 4; d++)
      {
        int def_idx = tcc_ir_find_defining_instruction(ir, chain_vr, i);
        if (def_idx < 0)
          break;
        IRQuadCompact *def_q = &ir->compact_instructions[def_idx];
        if (def_q->op != TCCIR_OP_ASSIGN && def_q->op != TCCIR_OP_LOAD)
          break;
        IROperand def_s1 = tcc_ir_op_get_src1(ir, def_q);
        if (!irop_has_vreg(def_s1))
          break;
        int src_vr = irop_get_vreg(def_s1);
        if (src_vr < 0 || src_vr == chain_vr)
          break;
        /* Does src_vr have a read after load_idx?  A re-def kills it. */
        for (int k = load_idx + 1; k < n; k++)
        {
          IRQuadCompact *kq = &ir->compact_instructions[k];
          if (kq->op == TCCIR_OP_NOP)
            continue;
          int seen_use = 0, seen_def = 0;
          if (irop_config[kq->op].has_src1)
          {
            IROperand s1 = tcc_ir_op_get_src1(ir, kq);
            if (irop_has_vreg(s1) && irop_get_vreg(s1) == src_vr)
              seen_use = 1;
          }
          if (irop_config[kq->op].has_src2)
          {
            IROperand s2 = tcc_ir_op_get_src2(ir, kq);
            if (irop_has_vreg(s2) && irop_get_vreg(s2) == src_vr)
              seen_use = 1;
          }
          if (irop_config[kq->op].has_dest)
          {
            IROperand d = tcc_ir_op_get_dest(ir, kq);
            if (irop_has_vreg(d) && irop_get_vreg(d) == src_vr)
              seen_def = 1;
          }
          if (seen_use)
          {
            chain_unsafe = 1;
            break;
          }
          if (seen_def)
            break;
        }
        if (chain_unsafe)
          break;
        chain_vr = src_vr;
      }
      if (chain_unsafe)
      {
        LOG_IR_GEN("POSTINC FUSION (rev) @%d: skip - ptr_vr=%d derives from a vreg still live after LOAD@%d (regalloc may coalesce)",
                   i, ptr_vr, load_idx);
        continue;
      }
    }

    /* (4) The LOAD must not itself be a branch target — we move it earlier. */
    if (ir->compact_instructions[load_idx].is_jump_target)
    {
      LOG_IR_GEN("POSTINC FUSION (rev) @%d: skip - LOAD@%d is jump target", i, load_idx);
      continue;
    }

    /* ---- Apply transformation ----
     *
     * Put LOAD_POSTINC at the ADD's slot (earlier) and ASSIGN at the LOAD's
     * slot (later) so the ASSIGN is sequenced after the hardware writeback.
     * Any NOPs between are untouched. is_jump_target of the ADD slot is
     * preserved on what's now LOAD_POSTINC — safe because LOAD_POSTINC is
     * the first instruction of the fused sequence.
     */
    IRQuadCompact *load_q_orig = &ir->compact_instructions[load_idx];
    IROperand val_op = tcc_ir_op_get_dest(ir, load_q_orig);
    IROperand ptr_op = tcc_ir_op_get_src1(ir, load_q_orig);

    /* LOAD_POSTINC always dereferences implicitly; the src1 operand is the
     * raw pointer register, not an lvalue.  If we left is_lval=1 (as it is
     * on the LOAD's src1, signalling "deref this pointer"), the backend
     * would emit an extra `ldr ip, [ptr]` to follow the lvalue chain,
     * producing a wrong double-load.  Clear it here. */
    IROperand ptr_base = ptr_op;
    ptr_base.is_lval = 0;

    int new_load_base = ir->iroperand_pool_count;
    tcc_ir_pool_add(ir, IROP_NONE);
    tcc_ir_pool_add(ir, IROP_NONE);
    tcc_ir_pool_add(ir, IROP_NONE);
    tcc_ir_pool_add(ir, IROP_NONE);

    /* ADD slot becomes LOAD_POSTINC. */
    add_q->op = TCCIR_OP_LOAD_POSTINC;
    add_q->operand_base = new_load_base;
    ir->iroperand_pool[new_load_base + 0] = val_op;
    ir->iroperand_pool[new_load_base + 1] = ptr_base;
    ir->iroperand_pool[new_load_base + 2] = IROP_NONE;
    ir->iroperand_pool[new_load_base + 3] = irop_make_imm32(-1, offset, IROP_BTYPE_INT32);

    /* LOAD slot becomes ASSIGN new_ptr, ptr (post-writeback). */
    int new_assign_base = ir->iroperand_pool_count;
    tcc_ir_pool_add(ir, IROP_NONE);
    tcc_ir_pool_add(ir, IROP_NONE);
    tcc_ir_pool_add(ir, IROP_NONE);

    load_q_orig->op = TCCIR_OP_ASSIGN;
    load_q_orig->operand_base = new_assign_base;
    ir->iroperand_pool[new_assign_base + 0] = add_dest;
    ir->iroperand_pool[new_assign_base + 1] = ptr_base;
    ir->iroperand_pool[new_assign_base + 2] = IROP_NONE;

    changes++;

    LOG_IR_GEN("POSTINC FUSION (rev): ADD@%d + LOAD@%d -> LOAD_POSTINC@%d + ASSIGN@%d (ptr_vr=%d new_vr=%d offset=%d)",
               i, load_idx, i, load_idx, ptr_vr, new_vr, offset);
  }

  LOG_IR_GEN("=== POSTINC FUSION END: %d fusions ===", changes);

  return changes;
}

/* ============================================================================
 * Loop-Aware Post-Increment Fusion
 * ============================================================================
 *
 * After IV strength reduction creates a pointer increment in the loop latch
 * (ptr += stride), this pass fuses the load and increment into a single
 * LOAD_POSTINC instruction.  It handles two patterns:
 *
 * Pattern A - Standalone LOAD:
 *   Before:  LOAD val, *ptr  ...  ptr = ptr + #4
 *   After:   LOAD_POSTINC val, ptr, #4; ASSIGN ptr, ptr  ...  NOP
 *
 * Pattern B - Embedded deref (needs NOP slot to extract the load):
 *   Before:  (nop) FUNCPARAMVAL ptr***DEREF***  ...  ptr = ptr + #4
 *   After:   LOAD_POSTINC tmp, ptr, #4; ASSIGN ptr, ptr; FUNCPARAMVAL tmp  ...  NOP
 *
 * The ASSIGN immediately after LOAD_POSTINC captures the hardware writeback
 * into the vreg so that if the pointer is later spilled, the spill slot
 * receives the updated value.  The latch ADD is NOP'd.
 *
 * If there is no room for the adjacent ASSIGN (no NOP slot), the pass falls
 * back to a plain LOAD + keeps the latch ADD, which is always safe.
 */
int tcc_ir_opt_loop_postinc_fusion(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  int changes = 0;

  if (n == 0)
    return 0;

  IRLoops *loops = tcc_ir_detect_loops(ir);
  if (!loops || loops->num_loops == 0)
  {
    tcc_ir_free_loops(loops);
    return 0;
  }

  for (int li = 0; li < loops->num_loops; li++)
  {
    IRLoop *loop = &loops->loops[li];

    /* Step 1: Find the latch ADD: ptr_vr = ptr_vr + #imm (self-update).
     * Scan backward from end_idx (the back-edge JUMP) looking for it. */
    int latch_add_idx = -1;
    int32_t ptr_vr = -1;
    int offset = 0;

    for (int i = loop->end_idx - 1; i >= loop->start_idx; i--)
    {
      IRQuadCompact *q = &ir->compact_instructions[i];
      if (q->op == TCCIR_OP_NOP || q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_CMP)
        continue;
      if (q->op != TCCIR_OP_ADD)
        break; /* First non-NOP/JUMP/CMP/ADD — stop searching */

      IROperand dest = tcc_ir_op_get_dest(ir, q);
      IROperand src1 = tcc_ir_op_get_src1(ir, q);
      IROperand src2 = tcc_ir_op_get_src2(ir, q);

      int d_vr = irop_get_vreg(dest);
      int s1_vr = irop_get_vreg(src1);

      /* Must be self-update: dest == src1, src2 is immediate.  ptr_vr must be
       * a TEMP: post-increment addressing only applies to pointer temporaries
       * (e.g. the running pointer created by IV strength reduction).  A VAR
       * self-increment is a scalar loop counter (`for (j=…; j<n; j++)`), not a
       * pointer; treating its lvalue uses (e.g. `CMP j, #10`) as memory derefs
       * and fusing them would insert a spurious LOAD and corrupt the loop.
       * The sibling fusion passes above apply the same TEMP restriction. */
      if (d_vr >= 0 && d_vr == s1_vr && irop_is_immediate(src2) &&
          TCCIR_DECODE_VREG_TYPE(d_vr) == TCCIR_VREG_TYPE_TEMP)
      {
        int imm = (int)irop_get_imm64_ex(ir, src2);
        if (imm >= 1 && imm <= 255)
        {
          latch_add_idx = i;
          ptr_vr = d_vr;
          offset = imm;
          break;
        }
      }
      break; /* Not a matching ADD — stop */
    }

    if (latch_add_idx < 0)
      continue;

    /* Step 2: Check for multiple exits — bail if the body has extra JUMPIFs
     * that jump outside the loop.  Use body_instrs to cover extended body. */
    {
      int extra_exits = 0;
      for (int bi = 0; bi < loop->num_body_instrs; bi++)
      {
        int i = loop->body_instrs[bi];
        IRQuadCompact *q = &ir->compact_instructions[i];
        if (q->op != TCCIR_OP_JUMPIF)
          continue;
        IROperand jdest = tcc_ir_op_get_dest(ir, q);
        int target = (int)irop_get_imm64_ex(ir, jdest);
        /* Allow the header exit, but count other exits outside the body range */
        int target_in_body = 0;
        for (int bj = 0; bj < loop->num_body_instrs; bj++)
        {
          if (loop->body_instrs[bj] == target)
          {
            target_in_body = 1;
            break;
          }
        }
        if (!target_in_body && i != loop->header_idx + 1)
          extra_exits++;
      }
      if (extra_exits > 0)
        continue;
    }

    /* Step 3: Find exactly one deref of ptr_vr in the loop body.
     * Search standalone LOADs, standalone STOREs, and embedded derefs
     * (ptr used as lval in non-LOAD/STORE ops). */
    int deref_idx = -1;
    int deref_src = 0; /* 1 = src1, 2 = src2 */
    int deref_count = 0;
    int deref_is_standalone_load = 0;
    int deref_is_standalone_store = 0;

    for (int bi = 0; bi < loop->num_body_instrs; bi++)
    {
      int i = loop->body_instrs[bi];
      IRQuadCompact *q = &ir->compact_instructions[i];
      if (q->op == TCCIR_OP_NOP)
        continue;
      if (i == latch_add_idx)
        continue;

      /* Check standalone LOAD: src1 is ptr_vr with is_lval */
      if (q->op == TCCIR_OP_LOAD)
      {
        IROperand s1 = tcc_ir_op_get_src1(ir, q);
        if (irop_get_vreg(s1) == ptr_vr && irop_op_is_lval(s1))
        {
          deref_idx = i;
          deref_src = 1;
          deref_is_standalone_load = 1;
          deref_is_standalone_store = 0;
          deref_count++;
        }
        continue;
      }

      /* Check standalone STORE: dest is ptr_vr with is_lval */
      if (q->op == TCCIR_OP_STORE)
      {
        IROperand d = tcc_ir_op_get_dest(ir, q);
        if (irop_get_vreg(d) == ptr_vr && irop_op_is_lval(d))
        {
          deref_idx = i;
          deref_src = 0;
          deref_is_standalone_load = 0;
          deref_is_standalone_store = 1;
          deref_count++;
        }
        continue;
      }

      /* Skip other memory ops */
      if (q->op == TCCIR_OP_LOAD_POSTINC || q->op == TCCIR_OP_STORE_POSTINC || q->op == TCCIR_OP_LOAD_INDEXED ||
          q->op == TCCIR_OP_STORE_INDEXED)
        continue;

      /* Check embedded deref in non-memory ops */
      if (irop_config[q->op].has_src1)
      {
        IROperand s1 = tcc_ir_op_get_src1(ir, q);
        if (irop_get_vreg(s1) == ptr_vr && irop_op_is_lval(s1))
        {
          deref_idx = i;
          deref_src = 1;
          deref_is_standalone_load = 0;
          deref_is_standalone_store = 0;
          deref_count++;
        }
      }
      if (irop_config[q->op].has_src2)
      {
        IROperand s2 = tcc_ir_op_get_src2(ir, q);
        if (irop_get_vreg(s2) == ptr_vr && irop_op_is_lval(s2))
        {
          deref_idx = i;
          deref_src = 2;
          deref_is_standalone_load = 0;
          deref_is_standalone_store = 0;
          deref_count++;
        }
      }
    }

    if (deref_count != 1)
      continue;

    /* Check byte type — LOAD/STORE_POSTINC operates on single words */
    {
      IRQuadCompact *dq = &ir->compact_instructions[deref_idx];
      IROperand deref_op;
      if (deref_is_standalone_load)
        deref_op = tcc_ir_op_get_src1(ir, dq);
      else if (deref_is_standalone_store)
        deref_op = tcc_ir_op_get_dest(ir, dq);
      else
        deref_op = (deref_src == 1) ? tcc_ir_op_get_src1(ir, dq) : tcc_ir_op_get_src2(ir, dq);
      int btype = irop_get_btype(deref_op);
      if (btype != IROP_BTYPE_INT32)
        continue;
    }

    /* Step 3b: Dominance check — the deref must execute every iteration. */
    {
      int dominated = 1;
      for (int bi = 0; bi < loop->num_body_instrs; bi++)
      {
        int i = loop->body_instrs[bi];
        if (i >= deref_idx)
          break;
        IRQuadCompact *q = &ir->compact_instructions[i];
        if (q->op != TCCIR_OP_JUMPIF)
          continue;
        if (i == loop->header_idx + 1)
          continue;
        {
          dominated = 0;
          break;
        }
      }
      if (!dominated)
        continue;
    }

    /* Step 4: Safety check — no non-deref use of ptr_vr between the deref
     * and the latch ADD. */
    {
      int unsafe = 0;
      for (int bi = 0; bi < loop->num_body_instrs; bi++)
      {
        int i = loop->body_instrs[bi];
        if (i <= deref_idx)
          continue;
        if (i == latch_add_idx)
          continue;

        IRQuadCompact *q = &ir->compact_instructions[i];
        if (q->op == TCCIR_OP_NOP || q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_JUMPIF || q->op == TCCIR_OP_CMP)
          continue;

        if (irop_config[q->op].has_src1)
        {
          IROperand s1 = tcc_ir_op_get_src1(ir, q);
          if (irop_get_vreg(s1) == ptr_vr && !irop_op_is_lval(s1))
            unsafe = 1;
        }
        if (irop_config[q->op].has_src2)
        {
          IROperand s2 = tcc_ir_op_get_src2(ir, q);
          if (irop_get_vreg(s2) == ptr_vr && !irop_op_is_lval(s2))
            unsafe = 1;
        }
        if (irop_config[q->op].has_dest)
        {
          IROperand d = tcc_ir_op_get_dest(ir, q);
          if (irop_get_vreg(d) == ptr_vr)
            unsafe = 1;
        }
      }
      if (unsafe)
        continue;
    }

    /* Step 5: Find NOP slots for the transformation.
     *
     * For standalone LOAD/STORE: we need one NOP immediately after deref_idx
     * for the ASSIGN that captures the hardware writeback.  The LOAD/STORE
     * itself is converted in-place to LOAD/STORE_POSTINC.
     *
     * For embedded deref: we need two consecutive NOPs before deref_idx
     * (one for LOAD_POSTINC, one for ASSIGN).  If only one NOP is
     * available, fall back to plain LOAD + keep latch ADD. */
    int assign_nop = -1; /* NOP slot for the writeback ASSIGN */
    int load_nop = -1;   /* NOP slot for LOAD_POSTINC (embedded deref only) */

    if (deref_is_standalone_load || deref_is_standalone_store)
    {
      /* Need a NOP right after the LOAD/STORE for the ASSIGN */
      if (deref_idx + 1 < n && ir->compact_instructions[deref_idx + 1].op == TCCIR_OP_NOP)
        assign_nop = deref_idx + 1;
    }
    else
    {
      /* Need two consecutive NOPs before the deref: load_nop, assign_nop */
      for (int i = deref_idx - 1; i >= 0; i--)
      {
        if (ir->compact_instructions[i].op != TCCIR_OP_NOP)
          break;
        if (assign_nop < 0)
          assign_nop = i;
        else
        {
          load_nop = i;
          break;
        }
      }
    }

    /* Build the pointer operand (no lval — LOAD/STORE_POSTINC handles the deref) */
    IRQuadCompact *deref_q = &ir->compact_instructions[deref_idx];
    IROperand orig_deref_op;
    if (deref_is_standalone_load)
      orig_deref_op = tcc_ir_op_get_src1(ir, deref_q);
    else if (deref_is_standalone_store)
      orig_deref_op = tcc_ir_op_get_dest(ir, deref_q);
    else
      orig_deref_op = (deref_src == 1) ? tcc_ir_op_get_src1(ir, deref_q) : tcc_ir_op_get_src2(ir, deref_q);
    IROperand ptr_op = orig_deref_op;
    ptr_op.is_lval = 0;

    if (assign_nop >= 0 && (deref_is_standalone_load || deref_is_standalone_store || load_nop >= 0))
    {
      /* Pool: 4 slots for LOAD/STORE_POSTINC */
      if (ir->iroperand_pool_count + 4 > ir->iroperand_pool_capacity)
      {
        tcc_ir_pool_ensure(ir, 4);
        if (ir->iroperand_pool_count + 4 > ir->iroperand_pool_capacity)
          continue;
      }

      if (deref_is_standalone_load)
      {
        /* Convert the existing LOAD in-place to LOAD_POSTINC */
        IROperand load_dest = tcc_ir_op_get_dest(ir, deref_q);
        int new_base = ir->iroperand_pool_count;
        tcc_ir_pool_add(ir, load_dest);
        tcc_ir_pool_add(ir, ptr_op);
        tcc_ir_pool_add(ir, IROP_NONE);
        tcc_ir_pool_add(ir, irop_make_imm32(-1, offset, IROP_BTYPE_INT32));

        deref_q->op = TCCIR_OP_LOAD_POSTINC;
        deref_q->operand_base = new_base;
      }
      else if (deref_is_standalone_store)
      {
        /* Convert the existing STORE in-place to STORE_POSTINC
         * STORE_POSTINC: slot0=ptr (dest, no lval), slot1=value (src1),
         *                slot2=unused, slot3=offset */
        IROperand store_val = tcc_ir_op_get_src1(ir, deref_q);
        int new_base = ir->iroperand_pool_count;
        tcc_ir_pool_add(ir, ptr_op);    /* dest = pointer (no lval) */
        tcc_ir_pool_add(ir, store_val); /* src1 = value to store */
        tcc_ir_pool_add(ir, IROP_NONE);
        tcc_ir_pool_add(ir, irop_make_imm32(-1, offset, IROP_BTYPE_INT32));

        deref_q->op = TCCIR_OP_STORE_POSTINC;
        deref_q->operand_base = new_base;
      }
      else
      {
        /* Allocate temp vreg and create LOAD_POSTINC in load_nop */
        int32_t loaded_vreg = tcc_ir_vreg_alloc_temp(ir);
        if (loaded_vreg < 0)
          continue;
        IROperand loaded_op = irop_make_vreg(loaded_vreg, IROP_BTYPE_INT32);

        int new_base = ir->iroperand_pool_count;
        tcc_ir_pool_add(ir, loaded_op);
        tcc_ir_pool_add(ir, ptr_op);
        tcc_ir_pool_add(ir, IROP_NONE);
        tcc_ir_pool_add(ir, irop_make_imm32(-1, offset, IROP_BTYPE_INT32));

        IRQuadCompact *lnop = &ir->compact_instructions[load_nop];
        lnop->op = TCCIR_OP_LOAD_POSTINC;
        lnop->operand_base = new_base;
        lnop->line_num = deref_q->line_num;

        /* Patch the deref instruction to use loaded_vreg (no deref) */
        IROperand patched_op = loaded_op;
        patched_op.is_lval = 0;
        if (deref_src == 1)
          tcc_ir_set_src1(ir, deref_idx, patched_op);
        else
          tcc_ir_set_src2(ir, deref_idx, patched_op);
      }

      /* Place ASSIGN ptr_vr = ptr_vr in assign_nop.  This is immediately
       * adjacent to the LOAD/STORE_POSTINC, so the register still holds the
       * post-incremented value and cannot have been spilled yet.  The
       * ASSIGN creates an explicit DEF for liveness, ensuring that any
       * later spill stores the updated pointer value. */
      if (ir->iroperand_pool_count + 2 > ir->iroperand_pool_capacity)
      {
        tcc_ir_pool_ensure(ir, 2);
        if (ir->iroperand_pool_count + 2 > ir->iroperand_pool_capacity)
          continue;
      }
      int assign_base = ir->iroperand_pool_count;
      tcc_ir_pool_add(ir, ptr_op); /* dest = ptr_vr */
      tcc_ir_pool_add(ir, ptr_op); /* src1 = ptr_vr */

      IRQuadCompact *anop = &ir->compact_instructions[assign_nop];
      anop->op = TCCIR_OP_ASSIGN;
      anop->operand_base = assign_base;
      anop->line_num = deref_q->line_num;

      /* NOP the latch ADD — the increment is handled by LOAD/STORE_POSTINC */
      ir->compact_instructions[latch_add_idx].op = TCCIR_OP_NOP;

      changes++;
      continue;
    }

    /* ---- Fallback: plain LOAD + keep latch ADD (always safe) ---- */
    if (!deref_is_standalone_load && !deref_is_standalone_store)
    {
      /* Need at least one NOP before the deref */
      int nop_slot = -1;
      for (int i = deref_idx - 1; i >= 0; i--)
      {
        if (ir->compact_instructions[i].op == TCCIR_OP_NOP)
        {
          nop_slot = i;
          break;
        }
        break;
      }
      if (nop_slot < 0)
        continue;

      int32_t loaded_vreg = tcc_ir_vreg_alloc_temp(ir);
      if (loaded_vreg < 0)
        continue;

      if (ir->iroperand_pool_count + 2 > ir->iroperand_pool_capacity)
      {
        tcc_ir_pool_ensure(ir, 2);
        if (ir->iroperand_pool_count + 2 > ir->iroperand_pool_capacity)
          continue;
      }

      IROperand loaded_op = irop_make_vreg(loaded_vreg, IROP_BTYPE_INT32);
      IROperand ptr_lval_op = orig_deref_op;
      ptr_lval_op.is_lval = 1;

      int new_base = ir->iroperand_pool_count;
      tcc_ir_pool_add(ir, loaded_op);
      tcc_ir_pool_add(ir, ptr_lval_op);

      IRQuadCompact *nop_q = &ir->compact_instructions[nop_slot];
      nop_q->op = TCCIR_OP_LOAD;
      nop_q->operand_base = new_base;
      nop_q->line_num = deref_q->line_num;

      IROperand patched_op = loaded_op;
      patched_op.is_lval = 0;
      if (deref_src == 1)
        tcc_ir_set_src1(ir, deref_idx, patched_op);
      else
        tcc_ir_set_src2(ir, deref_idx, patched_op);

      changes++;
    }
    /* For standalone LOAD without an ASSIGN slot: leave untouched */
  }

  tcc_ir_free_loops(loops);
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
 * Two-shift extract → UBFX  (tcc_ir_opt_shift_pair_to_ubfx)
 * ============================================================================
 *
 * The canonical unsigned bitfield extract `(x << a) >> b` (b >= a, both
 * logical) isolates the (32-b)-bit field at bit offset (b-a) of x.  ARM
 * Thumb-2 does this in one instruction: `UBFX Rd, Rx, #(b-a), #(32-b)`.
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
 *   - inner is SHL #a, outer is SHR #b (both logical), 1<=a<=b<=31, both
 *     32-bit (the 64-bit shift-extract idiom is handled by shift64_dead_half);
 *   - the SHL result is single-use (only the SHR), so NOPing the SHL drops
 *     exactly one instruction;
 *   - the SHL source is a plain (non-lval) register value, not redefined
 *     between the SHL and the SHR (UBFX reads it at the SHR's position) and
 *     with no control-flow edge between the two (same basic block).
 */
int tcc_ir_opt_shift_pair_to_ubfx(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  int changes = 0;

  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *shr_q = &ir->compact_instructions[i];
    if (shr_q->op != TCCIR_OP_SHR)
      continue;
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
    shr_q->op = TCCIR_OP_UBFX;
    tcc_ir_set_src1(ir, i, t0);
    tcc_ir_set_src2(ir, i, irop_make_imm32(-1, param, IROP_BTYPE_INT32));
    shl_q->op = TCCIR_OP_NOP;
    changes++;
    LOG_IR_GEN("SHIFT-PAIR->UBFX @%d: (x<<%d)>>%d -> UBFX lsb=%d width=%d (SHL@%d NOP)", i, a, b, lsb, width,
               shl_idx);
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
 * LEA CSE — collapse multiple LEAs of the same stack address
 * ============================================================================
 *
 * Pattern (emerges from per-element vector access into the same temp local
 * after macro-unrolling, e.g. `V v = ...; v[0] = ...; v[1] = ...`):
 *   i0:  T1 = LEA &?N              (anonymous local at offset O)
 *   i1:  T1***DEREF*** <- val      (or STORE_INDEXED [T1+#k] <- val)
 *   i2:  T3 = LEA &?N              ; same source — redundant LEA
 *   i3:  T4 = T3 ADD #4
 *   i4:  T4***DEREF*** <- val2
 *   ...
 *
 * Each subsequent LEA materializes the same `add rX, sp, #off` on ARM,
 * costing one instruction per access.  GCC keeps the address in a register
 * once and reuses it.
 *
 * Transform: within a basic block, the first LEA whose source operand
 * (STACKOFF + vreg + flags) matches a later LEA becomes the canonical
 * definition.  The later LEA is rewritten to `ASSIGN later_dest <-
 * first_dest`.  Copy propagation then forwards `first_dest` into all
 * downstream uses, and DCE removes the dead ASSIGN.
 *
 * Why not lea_fold?  That pass substitutes the LEA's source operand into
 * every deref use, which works for unique stack addresses (vreg=-1) but
 * breaks for vreg-backed anonymous locals: the stack-layout pass tracks
 * temp-local slot allocation by counting vreg references, so erasing every
 * reference makes the slot disappear from the frame while remaining LEAs
 * still target its original offset.  CSE preserves one canonical reference
 * to the vreg, so the slot stays allocated.
 *
 * Safety constraints:
 *   - Same basic block only (control flow may take a different path that
 *     reaches the second LEA without executing the first)
 *   - Source operand must compare equal under operand-by-operand match
 *     (tag, vreg, imm32, flag bits)
 *   - LEA dest must be a TEMP vreg with no other definition (SSA-like
 *     property — the rewrite produces an ASSIGN that copies from the
 *     canonical TEMP, so the dest must hold the same value through its
 *     entire lifetime)
 */
static int lea_cse_operand_equal(IROperand a, IROperand b)
{
  /* Strict on everything *except* ctype_idx for STRUCT operands: two LEAs
   * at the same stack offset with different ctype_idxes are still the same
   * numerical address — the type-view metadata doesn't affect what address
   * the LEA produces.  Comparing via irop_get_stack_offset masks the
   * ctype_idx half of u.s for STRUCT operands while keeping u.imm32 exact
   * for scalars. */
  if (irop_get_tag(a) != irop_get_tag(b))
    return 0;
  if (a.vr != b.vr)
    return 0;
  if (irop_get_stack_offset(a) != irop_get_stack_offset(b))
    return 0;
  /* For non-STRUCT operands, irop_get_stack_offset already covers u.imm32.
   * For STRUCT, also check the raw u.imm32 is otherwise consistent (e.g. we
   * still want to reject if the *non-offset* half varies in a way that
   * matters — but in practice ctype_idx is the only varying piece). */
  if (a.btype != IROP_BTYPE_STRUCT && a.u.imm32 != b.u.imm32)
    return 0;
  if (((const uint8_t *)&a)[8] != ((const uint8_t *)&b)[8])
    return 0;
  return 1;
}

int tcc_ir_opt_lea_cse(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  if (n < 2)
    return 0;

#define LEA_CSE_MAX_ACTIVE 32
  struct {
    IROperand src;
    int32_t dest_vr;
    int def_idx;
  } active[LEA_CSE_MAX_ACTIVE];
  int n_active = 0;
  int changes = 0;

  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;

    /* Basic-block boundary: reset the active map.  Any control transfer
     * (jump in or out) means the canonical LEA's dest vreg may not be
     * live on the other side. */
    if (q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_JUMPIF ||
        q->op == TCCIR_OP_IJUMP || q->op == TCCIR_OP_SWITCH_TABLE)
    {
      n_active = 0;
      continue;
    }

    /* CALLs end the live range of caller-saved registers — be conservative
     * and reset.  (We could be smarter if the canonical LEA's dest is in a
     * callee-saved register, but that's a regalloc-time fact unavailable here.) */
    if (q->op == TCCIR_OP_FUNCCALLVAL || q->op == TCCIR_OP_FUNCCALLVOID)
    {
      n_active = 0;
      continue;
    }

    /* Only LEA operations enter the table or hit it. */
    if (q->op != TCCIR_OP_LEA)
    {
      /* If this instruction redefines an active LEA's dest vreg (non-LEA
       * write), drop it from the active map.
       *
       * STORE/STORE_INDEXED/STORE_POSTINC are special: the IR keeps the
       * destination pointer/base in the dest slot, but semantically that
       * slot is a USE of the pointer — the op doesn't write to the dest
       * vreg, it writes through it.  Skip the redef bookkeeping for these
       * forms so the canonical LEA stays live across stores into the
       * region it points at. */
      int is_ptr_store = (q->op == TCCIR_OP_STORE || q->op == TCCIR_OP_STORE_INDEXED ||
                          q->op == TCCIR_OP_STORE_POSTINC);
      if (irop_config[q->op].has_dest && !is_ptr_store)
      {
        IROperand d = tcc_ir_op_get_dest(ir, q);
        if (irop_has_vreg(d) && !d.is_lval)
        {
          int32_t dvr = irop_get_vreg(d);
          for (int j = 0; j < n_active; j++)
          {
            if (active[j].dest_vr == dvr)
            {
              active[j] = active[--n_active];
              break;
            }
          }
        }
      }

      /* Also invalidate any active entry whose source vreg appears
       * directly in this op's operands (not through the LEA's dest
       * vreg).  This catches cases like `T5 <-- ?131070 [LOAD]` —
       * a direct read/write of the slot's anonymous vreg that bypasses
       * the LEA chain.  Extending the canonical LEA's live range across
       * such uses would extend register pressure unpredictably (the
       * regalloc didn't see the LEA's vreg as live across the direct
       * op), surfacing as overlapping register assignments in the final
       * MOP. */
      const IRRegistersConfig *cfg = &irop_config[q->op];
      IROperand check_ops[3];
      int ncheck = 0;
      if (cfg->has_src1) check_ops[ncheck++] = tcc_ir_op_get_src1(ir, q);
      if (cfg->has_src2) check_ops[ncheck++] = tcc_ir_op_get_src2(ir, q);
      if (cfg->has_dest) check_ops[ncheck++] = tcc_ir_op_get_dest(ir, q);
      for (int oi = 0; oi < ncheck; oi++)
      {
        if (irop_get_tag(check_ops[oi]) != IROP_TAG_STACKOFF)
          continue;
        int32_t ovr = irop_get_vreg(check_ops[oi]);
        if (ovr >= -1) /* only negative-vreg STACKOFFs are CSE-tracked */
          continue;
        for (int j = 0; j < n_active; j++)
        {
          if (irop_get_vreg(active[j].src) == ovr)
          {
            active[j] = active[--n_active];
            break;
          }
        }
      }
      continue;
    }

    IROperand src = tcc_ir_op_get_src1(ir, q);
    IROperand dest = tcc_ir_op_get_dest(ir, q);

    /* Only operate on TEMP destination — VAR/PARAM destinations have
     * multi-block liveness that we can't reason about here. */
    int32_t dest_vr = irop_get_vreg(dest);
    if (dest_vr < 0 || TCCIR_DECODE_VREG_TYPE(dest_vr) != TCCIR_VREG_TYPE_TEMP)
      continue;

    /* Source must be a STACKOFF — we're targeting stack-address LEAs only. */
    if (irop_get_tag(src) != IROP_TAG_STACKOFF)
      continue;
    if (src.is_lval) /* &<lvalue> wouldn't be a STACKOFF address-of */
      continue;

    /* Restrict to STACKOFFs with a *negative* vreg encoding — those are
     * anonymous temp locals where the offset lives in u.imm32 and the vreg
     * id is the only handle the stack-layout pass has on the slot.  Three
     * other shapes share the STACKOFF tag and must be skipped:
     *   - vreg == -1 (no vreg): plain `Addr[StackLoc[-N]]`.  lea_fold
     *     already folds each LEA+deref pair into a direct stack access;
     *     CSE'ing here would give the canonical LEA multiple uses and
     *     disable lea_fold's single-use precondition.
     *   - VAR/PARAM/TEMP positive vregs (`&V1`, `&P4`, `&T7`): the address
     *     itself is the same regardless of sign, but a downstream
     *     local-load CSE pass merges LOADs through the unified base vreg
     *     without consulting their sign/btype — merging the LEAs unmasks
     *     that bug (e.g. signed-vs-unsigned-short reads of a union slot in
     *     pr84071 / 20180131-1.c).  Stay clear until that CSE distinguishes
     *     load width/sign. */
    {
      int32_t src_vr = irop_get_vreg(src);
      if (src_vr >= -1)
        continue;
    }

    /* Search for an existing canonical entry with the same source. */
    int hit = -1;
    for (int j = 0; j < n_active; j++)
    {
      if (lea_cse_operand_equal(active[j].src, src))
      {
        hit = j;
        break;
      }
    }

    if (hit >= 0)
    {
      /* Rewrite this LEA as `ASSIGN dest <- canonical_dest`.  Subsequent
       * copy propagation will forward the canonical dest into the deref
       * consumers and DCE will reclaim this ASSIGN. */
      IROperand canon = irop_make_vreg(active[hit].dest_vr, dest.btype);
      canon.is_unsigned = dest.is_unsigned;
      q->op = TCCIR_OP_ASSIGN;
      tcc_ir_set_src1(ir, i, canon);
      tcc_ir_set_src2(ir, i, IROP_NONE);
      changes++;
      LOG_IR_GEN("LEA CSE: i=%d redundant LEA -> ASSIGN from i=%d", i, active[hit].def_idx);
      continue;
    }

    /* Record this LEA as the canonical definition. */
    if (n_active < LEA_CSE_MAX_ACTIVE)
    {
      active[n_active].src = src;
      active[n_active].dest_vr = dest_vr;
      active[n_active].def_idx = i;
      n_active++;
    }
  }

  LOG_IR_GEN("=== LEA CSE END: %d redundant LEAs collapsed ===", changes);
  return changes;
#undef LEA_CSE_MAX_ACTIVE
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

int tcc_ir_opt_postinc_fusion_ex(IROptCtx *ctx) { return tcc_ir_opt_postinc_fusion(ctx->ir); }
int tcc_ir_opt_assign_fuse_ex(IROptCtx *ctx) { return tcc_ir_opt_assign_fuse(ctx->ir); }
