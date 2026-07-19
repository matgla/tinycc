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



/*
 * LEA + deref fold
 *
 * The frontend materializes `&local_var` as an explicit LEA computing the
 * stack-slot address into a TEMP vreg even when the next use is a deref, so
 * ARM emits `add rX,sp,#off; ldr rY,[rX]` instead of `ldr rY,[sp,#off]`.
 * Fold the LEA into a direct StackLoc access on the consumer.
 *
 * Roots handled:
 *   A: T  = LEA Addr[StackLoc]                    ; <op> ...T*DEREF*...
 *   B: T1 = LEA Addr[StackLoc] ; T2 = T1 ADD #K   ; <op> ...T2*DEREF*...
 *   C: T  = ASSIGN Addr[StackLoc]                 ; <op> ...T*DEREF*...
 * (C is A via a frontend copy chain, common in nested-function inlining.)
 *
 * Transform: replace the T-DEREF operand with the StackLoc at the folded
 * offset (is_lval=1); NOP the LEA/ASSIGN and any ADD interposer.
 *
 * Safety: LEA source must be Addr[StackLoc] (is_lval=0); the LEA (and the ADD
 * in B) result must have exactly one use; the consumer must be in the same
 * block and reference the result with is_lval=1 exactly once — never as a
 * STORE dest through the pointer (indistinguishable from a direct stack store).
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

    /* ADD Addr[StackLoc],#K is deliberately not an entry root: folding it to a
     * direct StackLoc can remove the only address-valued op tying a subslot
     * access to the enclosing aggregate, so later stack-slot passes miss
     * aliases. Keep it explicit unless it interposes after a real LEA/ASSIGN
     * root, which still carries the aggregate's address-taken info. */
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
    /* ir_opt_du_uses records STORE's dest slot as a definition, not a use, so
     * it undercounts real uses of T (a STORE derefs through T) and once folded
     * `LEA T; *T read; *T write` into a dangling `*T write`. Count uses by an
     * explicit linear scan that treats is_lval=1 dest positions as uses. */
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
          /* A dest with is_lval=1 derefs through the vreg (a use); a plain dest
           * redefines lea_vr and ends its live range — hard stop. Exception:
           * STORE/STORE_INDEXED/STORE_POSTINC put the base pointer (a use) in
           * dest, and disp_fusion clears is_lval on STORE_INDEXED's base, so
           * the is_lval test alone would misclassify it as a redef. */
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

    /* STRUCT btype keeps the offset in u.s.aux_data, not u.imm32; use the
     * accessor or a struct-typed Addr[] yields garbage. */
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

    /* Optional single ADD #K interposer between the LEA result and the deref
     * consumer, itself with exactly one use. */
    int add_idx = -1;
    int32_t add_offset = 0;
    IRQuadCompact *add_q = &ir->compact_instructions[cur_idx];
    if (add_q->op == TCCIR_OP_ADD)
    {
      IROperand a1 = tcc_ir_op_get_src1(ir, add_q);
      IROperand a2 = tcc_ir_op_get_src2(ir, add_q);
      /* Require `lea_vr + #K` (either order, other side IMM32) with the vreg
       * side is_lval=0: a DEREF flag means the ADD reads the value stored at
       * lea_vr (loaded-pointer arithmetic), which must not fold to a slot. */
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
        /* Explicit-scan use count; ir_opt_du_uses undercounts STORE-dest uses. */
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

    /* The final consumer must deref the folded vreg (is_lval=1) exactly once;
     * a non-deref use (PARAM, another ADD) can't fold — the address escapes. */
    int32_t deref_vr =
        (add_idx >= 0) ? irop_get_vreg(tcc_ir_op_get_dest(ir, &ir->compact_instructions[add_idx])) : lea_vr;

    /* LOAD_INDEXED special case: base in src1, constant offset in src2, scale
     * in slot 3. When base is the LEA vreg, scale==0, and src2 is IMM32, fold
     * to a direct StackLoc LOAD at base+add_offset+index_imm, unlocking later
     * stack-store-load forwarding. */
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
    /* which==3 is a STORE through the temp: keep it explicit so later
     * stack-slot passes don't miss aliases through the aggregate address.
     * Read-side folds are still safe. */
    if (which == 3)
      continue;

    IRQuadCompact *cons_q = &ir->compact_instructions[cur_idx];

    IROperand old_op = (which == 1)   ? tcc_ir_op_get_src1(ir, cons_q)
                       : (which == 2) ? tcc_ir_op_get_src2(ir, cons_q)
                                      : tcc_ir_op_get_dest(ir, cons_q);

    /* Skip a STRUCT-typed consumer operand: irop_make_stackoff writes u.imm32
     * unconditionally, corrupting ctype_idx for a struct read. A STRUCT
     * lea_src is fine — the scalar consumer carries its own non-struct btype. */
    if (old_op.btype == IROP_BTYPE_STRUCT)
      continue;

    /* Direct StackLoc at the folded offset (is_lval=1), built from scratch via
     * irop_make_stackoff so union members init cleanly, then copy the
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
