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




/* Call-chain result rename.
 *
 * Pattern: CALL_i -> V ; FUNCPARAMVAL[0] V ; CALL_(i+1) -> V (redef, no
 * intervening read).  Regalloc keeps V callee-saved because its lifetime
 * spans CALLs, forcing a mov to/from r0 around each call/param.  Rename V to
 * a fresh TEMP at just that (CALL.dest, PARAMVAL.src1) pair so its live range
 * no longer crosses a CALL and regalloc can keep it in r0.
 *
 * V's other defs/uses (e.g. the final call whose result flows out via an
 * external read like `return y`) are left alone, so V keeps the right value
 * at the function's external-visible points.
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
    /* PARAMVAL src may have is_lval=1 for a VAR (load V's storage into the
     * param reg).  After rename the value is already in the register, so the
     * new src is emitted is_lval=0; V's btype/is_unsigned are preserved. */
    IROperand pv_src2 = tcc_ir_op_get_src2(ir, next_q);
    int param_idx = TCCIR_DECODE_PARAM_IDX(irop_get_imm64_ex(ir, pv_src2));
    if (param_idx != 0)
      continue;

    /* Verify V is overwritten before any subsequent read.  Stop at a
     * control-flow boundary (JUMP/RETURN/target) — beyond it the rename is
     * unsafe as other paths might read V; also bail if any op reads V first. */
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

      if (irop_config[kq->op].has_dest)
      {
        IROperand d = tcc_ir_op_get_dest(ir, kq);
        if (irop_get_vreg(d) == v_vr && !d.is_lval)
        {
          /* Honest redefinition: V overwritten before any later read. */
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

    /* Rename V only at this CALL.dest and PARAMVAL.src1; other defs/uses stay intact. */
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
