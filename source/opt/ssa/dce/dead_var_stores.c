/*
 *  TCC IR - SSA DCE: dead VAR-slot stores
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
#include "dce_common.h"
#include "dce_passes.h"


static int var_addr_is_write_only(IRSSAOptCtx *ctx, int32_t vreg)
{
  IRSSAVregInfo *vi = ssa_opt_vinfo(ctx, vreg);
  if (!vi)
    return 0;
  TCCIRState *ir = ctx->ir;
  for (int u = 0; u < vi->use_count; u++) {
    if (vi->uses[u].kind == SSA_USE_PHI)
      return 0;
    int idx = vi->uses[u].idx;
    IRQuadCompact *q = &ir->compact_instructions[idx];
    if (q->op == TCCIR_OP_NOP)
      continue;
    if (q->op == TCCIR_OP_STORE || q->op == TCCIR_OP_STORE_INDEXED) {
      IROperand d = tcc_ir_op_get_dest(ir, q);
      if (irop_get_vreg(d) == vreg)
        continue;
    }
    return 0;
  }
  return 1;
}

int dce_dead_var_stores(IRSSAOptCtx *ctx)
{
  TCCIRState *ir = ctx->ir;
  int n = ir->next_instruction_index;
  int num_vars = ir->next_local_variable;
  int changes = 0;

  if (num_vars <= 0)
    return 0;

  /* Nested functions read parent VARs via the frame pointer, invisibly to this scan. */
  for (int i = 0; i < n; i++) {
    int op = ir->compact_instructions[i].op;
    if (op == TCCIR_OP_SET_CHAIN || op == TCCIR_OP_INIT_CHAIN_SLOT)
      return 0;
  }

  uint8_t *var_used = tcc_mallocz((num_vars + 7) / 8);
  uint8_t *var_addrtaken = tcc_mallocz((num_vars + 7) / 8);

  /* A volatile store is a mandated side effect even if the slot is never read. */
  for (int p = 0; p < num_vars && p < ir->variables_live_intervals_size; p++)
    if (ir->variables_live_intervals[p].is_volatile)
      var_used[p / 8] |= (1 << (p % 8));

  for (int i = 0; i < n; i++) {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;

    for (int oi = 0; oi < 2; oi++) {
      IROperand s;
      if (oi == 0 && irop_config[q->op].has_src1)
        s = tcc_ir_op_get_src1(ir, q);
      else if (oi == 1 && irop_config[q->op].has_src2)
        s = tcc_ir_op_get_src2(ir, q);
      else
        continue;
      int32_t vr = irop_get_vreg(s);
      if (vr < 0 || TCCIR_DECODE_VREG_TYPE(vr) != TCCIR_VREG_TYPE_VAR)
        continue;
      int pos = TCCIR_DECODE_VREG_POSITION(vr);
      if (pos >= num_vars)
        continue;
      if (s.is_local && !s.is_lval) {
        int safe = 0;
        if (irop_config[q->op].has_dest) {
          IROperand d = tcc_ir_op_get_dest(ir, q);
          int32_t dvr = irop_get_vreg(d);
          if (dvr >= 0 &&
              TCCIR_DECODE_VREG_TYPE(dvr) == TCCIR_VREG_TYPE_TEMP &&
              (!sl_temp_has_live_uses(ctx, dvr) ||
               var_addr_is_write_only(ctx, dvr)))
            safe = 1;
        }
        if (!safe)
          var_addrtaken[pos / 8] |= (1 << (pos % 8));
      } else {
        var_used[pos / 8] |= (1 << (pos % 8));
      }
    }

    /* MLA accumulator: third operand not covered by src1/src2 */
    if (q->op == TCCIR_OP_MLA) {
      IROperand a = tcc_ir_op_get_accum(ir, q);
      int32_t vr = irop_get_vreg(a);
      if (vr >= 0 && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_VAR) {
        int pos = TCCIR_DECODE_VREG_POSITION(vr);
        if (pos < num_vars)
          var_used[pos / 8] |= (1 << (pos % 8));
      }
    }

    /* is_lval && !is_local means the dest VAR's value is read as a pointer, not written. */
    if (q->op == TCCIR_OP_STORE || q->op == TCCIR_OP_STORE_INDEXED) {
      IROperand d = tcc_ir_op_get_dest(ir, q);
      int32_t dvr = irop_get_vreg(d);
      if (dvr >= 0 && TCCIR_DECODE_VREG_TYPE(dvr) == TCCIR_VREG_TYPE_VAR &&
          (q->op == TCCIR_OP_STORE_INDEXED || (d.is_lval && !d.is_local))) {
        int pos = TCCIR_DECODE_VREG_POSITION(dvr);
        if (pos < num_vars)
          var_used[pos / 8] |= (1 << (pos % 8));
      }
    }

    if (q->op == TCCIR_OP_LEA) {
      IROperand s = tcc_ir_op_get_src1(ir, q);
      int32_t vr = irop_get_vreg(s);
      if (vr >= 0 && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_VAR) {
        int pos = TCCIR_DECODE_VREG_POSITION(vr);
        if (pos < num_vars) {
          IROperand d = tcc_ir_op_get_dest(ir, q);
          int32_t dvr = irop_get_vreg(d);
          if (dvr < 0 ||
              TCCIR_DECODE_VREG_TYPE(dvr) != TCCIR_VREG_TYPE_TEMP ||
              (sl_temp_has_live_uses(ctx, dvr) &&
               !var_addr_is_write_only(ctx, dvr)))
            var_addrtaken[pos / 8] |= (1 << (pos % 8));
        }
      }
    }

    if (q->op == TCCIR_OP_LOAD || q->op == TCCIR_OP_ASSIGN) {
      IROperand s = tcc_ir_op_get_src1(ir, q);
      int32_t vr = irop_get_vreg(s);
      if (vr >= 0 && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_VAR) {
        int pos = TCCIR_DECODE_VREG_POSITION(vr);
        if (pos < num_vars)
          var_used[pos / 8] |= (1 << (pos % 8));
      }
    }

    if (q->op == TCCIR_OP_FUNCPARAMVAL) {
      IROperand s = tcc_ir_op_get_src1(ir, q);
      int32_t vr = irop_get_vreg(s);
      if (vr >= 0 && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_VAR) {
        int pos = TCCIR_DECODE_VREG_POSITION(vr);
        if (pos < num_vars)
          var_used[pos / 8] |= (1 << (pos % 8));
      }
    }
  }

  for (int i = 0; i < n; i++) {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;
    if (!irop_config[q->op].has_dest)
      continue;
    IROperand dest = tcc_ir_op_get_dest(ir, q);
    int32_t dvr = irop_get_vreg(dest);
    if (dvr < 0 || TCCIR_DECODE_VREG_TYPE(dvr) != TCCIR_VREG_TYPE_VAR)
      continue;
    /* STORE to a dead VAR is safe to eliminate; other side-effect ops are not */
    if (q->op != TCCIR_OP_STORE && ssa_opt_has_side_effects(q->op))
      continue;
    int pos = TCCIR_DECODE_VREG_POSITION(dvr);
    if (pos >= num_vars)
      continue;
    if (var_addrtaken[pos / 8] & (1 << (pos % 8)))
      continue;
    if (var_used[pos / 8] & (1 << (pos % 8)))
      continue;

    ssa_opt_nop_instr(ctx, i);
    changes++;
  }

  for (int i = 0; i < n; i++) {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_LEA && q->op != TCCIR_OP_ASSIGN)
      continue;
    if (q->op == TCCIR_OP_NOP)
      continue;
    if (!irop_config[q->op].has_src1)
      continue;
    IROperand s = tcc_ir_op_get_src1(ir, q);
    int32_t svr = irop_get_vreg(s);
    if (svr < 0 || TCCIR_DECODE_VREG_TYPE(svr) != TCCIR_VREG_TYPE_VAR)
      continue;
    if (!(s.is_local && !s.is_lval))
      continue;
    int pos = TCCIR_DECODE_VREG_POSITION(svr);
    if (pos >= num_vars)
      continue;
    if (var_addrtaken[pos / 8] & (1 << (pos % 8)))
      continue;
    if (var_used[pos / 8] & (1 << (pos % 8)))
      continue;
    IROperand d = tcc_ir_op_get_dest(ir, q);
    int32_t dvr = irop_get_vreg(d);
    if (dvr < 0 || TCCIR_DECODE_VREG_TYPE(dvr) != TCCIR_VREG_TYPE_TEMP)
      continue;
    if (!var_addr_is_write_only(ctx, dvr))
      continue;
    IRSSAVregInfo *vi = ssa_opt_vinfo(ctx, dvr);
    if (!vi)
      continue;
    for (int u = vi->use_count - 1; u >= 0; u--) {
      if (vi->uses[u].kind != SSA_USE_INSTR)
        continue;
      int si = vi->uses[u].idx;
      if (ir->compact_instructions[si].op == TCCIR_OP_NOP)
        continue;
      ssa_opt_nop_instr(ctx, si);
      changes++;
    }
    ssa_opt_nop_instr(ctx, i);
    changes++;
  }

  tcc_free(var_used);
  tcc_free(var_addrtaken);
  return changes;
}
