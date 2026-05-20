/*
 *  TCC IR - Call-result dead elimination generator table (pre-SSA engine)
 *
 *  Generators:
 *    dead_sret_call    — eliminate calls to func_pure_via_sret with dead sret target
 *    dead_call_result  — convert FUNCCALLVAL→FUNCCALLVOID when result unused
 *    fold_call_result_store — fold CALL→TEMP_LOCAL+LOAD+STORE into direct CALL→*V
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
#include "opt_gens_call_result.h"

static int ir_gen_dead_call_result(IROptCtx *ctx, int i)
{
  TCCIRState *ir = ctx->ir;
  const IROptDU *du = &ctx->du;
  IRQuadCompact *q = &ir->compact_instructions[i];

  IROperand dest = tcc_ir_op_get_dest(ir, q);
  int32_t dest_vr = irop_get_vreg(dest);
  if (TCCIR_DECODE_VREG_TYPE(dest_vr) == TCCIR_VREG_TYPE_PARAM)
    return 0;

  if (dest_vr >= 0) {
    if (ir_opt_du_uses(du, dest_vr) != 0)
      return 0;
  } else {
    /* TEMP_LOCAL dest (vr in [-9, -2]): DU table doesn't cover these, so
     * do a manual forward scan.  Bail on any subsequent reference (read
     * OR write) to this TEMP_LOCAL — for complex types one CALL may write
     * only a half, so a later "write" can't be treated as a clobber that
     * makes our value dead.  Iterated pipeline catches back-to-back cases:
     * the truly-last write becomes eligible first, and after it's dropped
     * the next-to-last gets a clean forward window. */
    if (dest_vr > -2 || dest_vr < -9) return 0;
    int n = ir->next_instruction_index;
    for (int j = i + 1; j < n; j++) {
      IRQuadCompact *p = &ir->compact_instructions[j];
      if (p->op == TCCIR_OP_NOP) continue;
      for (int k = 0; k < 3; k++) {
        IROperand po;
        int has;
        if (k == 0) { has = irop_config[p->op].has_dest;
                      if (has) po = tcc_ir_op_get_dest(ir, p); }
        else if (k == 1) { has = irop_config[p->op].has_src1;
                           if (has) po = tcc_ir_op_get_src1(ir, p); }
        else { has = irop_config[p->op].has_src2;
               if (has) po = tcc_ir_op_get_src2(ir, p); }
        if (!has) continue;
        if (irop_get_vreg(po) == dest_vr)
          return 0;
      }
    }
  }

  IROperand src1 = ir->iroperand_pool[q->operand_base + 1];
  IROperand src2 = ir->iroperand_pool[q->operand_base + 2];
  q->op = TCCIR_OP_FUNCCALLVOID;
  ir->iroperand_pool[q->operand_base + 0] = src1;
  ir->iroperand_pool[q->operand_base + 1] = src2;

  LOG_IR_GEN("=== DEAD CALL RESULT: i=%d FUNCCALLVAL→FUNCCALLVOID (dest vr=%d) ===",
             i, dest_vr);
  return 1;
}

static int ir_gen_dead_sret_call(IROptCtx *ctx, int i)
{
  TCCIRState *ir = ctx->ir;
  const IROptDU *du = &ctx->du;
  IRQuadCompact *q = &ir->compact_instructions[i];
  int n = ir->next_instruction_index;

  if (q->op == TCCIR_OP_FUNCCALLVAL) {
    IROperand dest = tcc_ir_op_get_dest(ir, q);
    int32_t dest_vr = irop_get_vreg(dest);
    if (dest_vr < 0)
      return 0;
    if (ir_opt_du_uses(du, dest_vr) != 0)
      return 0;
  }

  Sym *callee = irop_get_sym_ex(ir, tcc_ir_op_get_src1(ir, q));
  if (!callee)
    return 0;
  int callee_pure = callee->f.func_pure_via_sret;
  if (callee->type.ref)
    callee_pure |= callee->type.ref->f.func_pure_via_sret;
  if (!callee_pure)
    return 0;

  IROperand call_src2 = tcc_ir_op_get_src2(ir, q);
  int call_id = TCCIR_DECODE_CALL_ID((uint32_t)irop_get_imm64_ex(ir, call_src2));
  IROperand param0;
  if (!ir_opt_get_call_param_operand(ir, i, 0, &param0))
    return 0;
  if (!param0.is_local || param0.is_lval)
    return 0;
  if (irop_get_tag(param0) != IROP_TAG_STACKOFF)
    return 0;
  int32_t sret_off = (int32_t)irop_get_stack_offset(param0);

  int sret_size = 0;
  {
    CType *ret_type = callee->type.ref ? &callee->type.ref->type : &callee->type;
    int align = 0;
    sret_size = type_size(ret_type, &align);
    if (sret_size <= 0)
      return 0;
    sret_size = (sret_size + 3) & ~3;
  }

  int range_used_later = 0;
  for (int j = i + 1; j < n && !range_used_later; j++) {
    IRQuadCompact *p = &ir->compact_instructions[j];
    if (p->op == TCCIR_OP_NOP)
      continue;
    IROperand ops[3];
    ops[0] = irop_config[p->op].has_dest ? tcc_ir_op_get_dest(ir, p) : (IROperand){0};
    ops[1] = irop_config[p->op].has_src1 ? tcc_ir_op_get_src1(ir, p) : (IROperand){0};
    ops[2] = irop_config[p->op].has_src2 ? tcc_ir_op_get_src2(ir, p) : (IROperand){0};
    for (int k = 0; k < 3 && !range_used_later; k++) {
      if (irop_is_none(ops[k]))
        continue;
      if (irop_get_tag(ops[k]) != IROP_TAG_STACKOFF)
        continue;
      int32_t off = (int32_t)irop_get_stack_offset(ops[k]);
      if (off >= sret_off && off < sret_off + sret_size)
        range_used_later = 1;
    }
  }
  if (range_used_later)
    return 0;

  int address_escaped = 0;
  for (int j = 0; j < i && !address_escaped; j++) {
    IRQuadCompact *p = &ir->compact_instructions[j];
    if (p->op == TCCIR_OP_NOP)
      continue;
    if (!irop_config[p->op].has_dest)
      continue;
    IROperand src1 = irop_config[p->op].has_src1 ? tcc_ir_op_get_src1(ir, p) : (IROperand){0};
    int is_addr_of_range = 0;
    if (src1.is_local && !src1.is_lval && irop_get_tag(src1) == IROP_TAG_STACKOFF) {
      int32_t off = (int32_t)irop_get_stack_offset(src1);
      if (off >= sret_off && off < sret_off + sret_size)
        is_addr_of_range = 1;
    }
    if (!is_addr_of_range)
      continue;
    IROperand dest = tcc_ir_op_get_dest(ir, p);
    int32_t dest_vr = irop_get_vreg(dest);
    if (dest_vr < 0)
      continue;
    for (int k = i + 1; k < n && !address_escaped; k++) {
      IRQuadCompact *pk = &ir->compact_instructions[k];
      if (pk->op == TCCIR_OP_NOP)
        continue;
      if ((pk->op == TCCIR_OP_FUNCPARAMVAL || pk->op == TCCIR_OP_FUNCPARAMVOID)) {
        uint32_t enc = (uint32_t)irop_get_imm64_ex(ir, tcc_ir_op_get_src2(ir, pk));
        if (TCCIR_DECODE_CALL_ID(enc) == call_id)
          continue;
      }
      if (irop_config[pk->op].has_src1 && irop_get_vreg(tcc_ir_op_get_src1(ir, pk)) == dest_vr)
        address_escaped = 1;
      if (!address_escaped && irop_config[pk->op].has_src2 &&
          irop_get_vreg(tcc_ir_op_get_src2(ir, pk)) == dest_vr)
        address_escaped = 1;
    }
  }
  if (address_escaped)
    return 0;

  ir_opt_nop_call_params(ir, i);
  q->op = TCCIR_OP_NOP;
  LOG_IR_GEN("=== DEAD SRET CALL: i=%d (sret_off=%d size=%d) ===",
             i, (int)sret_off, sret_size);
  return 1;
}

static int ir_gen_fold_call_result_store(IROptCtx *ctx, int i)
{
  TCCIRState *ir = ctx->ir;
  IRQuadCompact *q = &ir->compact_instructions[i];
  int n = ir->next_instruction_index;

  IROperand call_dest = tcc_ir_op_get_dest(ir, q);
  int32_t call_dest_vr = irop_get_vreg(call_dest);
  if (call_dest_vr >= 0 || call_dest_vr < -9)
    return 0;
  if (!call_dest.is_lval || !call_dest.is_local ||
      irop_get_tag(call_dest) != IROP_TAG_STACKOFF)
    return 0;
  int64_t call_dest_off = irop_get_imm64_ex(ir, call_dest);

  int load_idx = -1;
  int32_t load_dest_vr = -1;
  int multi_use = 0;
  for (int j = i + 1; j < n && !multi_use; j++) {
    IRQuadCompact *p = &ir->compact_instructions[j];
    if (p->op == TCCIR_OP_NOP)
      continue;
    IROperand ops[3];
    ops[0] = irop_config[p->op].has_dest ? tcc_ir_op_get_dest(ir, p) : (IROperand){0};
    ops[1] = irop_config[p->op].has_src1 ? tcc_ir_op_get_src1(ir, p) : (IROperand){0};
    ops[2] = irop_config[p->op].has_src2 ? tcc_ir_op_get_src2(ir, p) : (IROperand){0};
    for (int k = 0; k < 3 && !multi_use; k++) {
      if (irop_is_none(ops[k]))
        continue;
      if (irop_get_vreg(ops[k]) != call_dest_vr)
        continue;
      if (irop_get_tag(ops[k]) != IROP_TAG_STACKOFF)
        continue;
      if (irop_get_imm64_ex(ir, ops[k]) != call_dest_off)
        continue;
      if (p->op == TCCIR_OP_LOAD && k == 1 && load_idx < 0) {
        load_idx = j;
        load_dest_vr = irop_get_vreg(tcc_ir_op_get_dest(ir, p));
      } else {
        multi_use = 1;
      }
    }
  }
  if (multi_use || load_idx < 0 || load_dest_vr < 0)
    return 0;

  int store_idx = -1;
  int load_dst_misuse = 0;
  for (int k = 0; k < n && !load_dst_misuse; k++) {
    if (k == load_idx)
      continue;
    IRQuadCompact *p = &ir->compact_instructions[k];
    if (p->op == TCCIR_OP_NOP)
      continue;
    int uses = 0;
    if (irop_config[p->op].has_src1 && irop_get_vreg(tcc_ir_op_get_src1(ir, p)) == load_dest_vr)
      uses = 1;
    if (irop_config[p->op].has_src2 && irop_get_vreg(tcc_ir_op_get_src2(ir, p)) == load_dest_vr)
      uses = 2;
    if ((p->op == TCCIR_OP_STORE || p->op == TCCIR_OP_STORE_INDEXED || p->op == TCCIR_OP_STORE_POSTINC) &&
        irop_get_vreg(tcc_ir_op_get_dest(ir, p)) == load_dest_vr)
      uses = 3;
    if (!uses)
      continue;
    if (p->op != TCCIR_OP_STORE || uses != 1 || store_idx >= 0) {
      load_dst_misuse = 1;
      break;
    }
    store_idx = k;
  }
  if (load_dst_misuse || store_idx < 0)
    return 0;

  IRQuadCompact *store_q = &ir->compact_instructions[store_idx];
  IROperand store_dst = tcc_ir_op_get_dest(ir, store_q);
  int32_t store_dst_vr = irop_get_vreg(store_dst);
  if (store_dst_vr < 0)
    return 0;
  if (!store_dst.is_lval)
    return 0;

  int avail_at_call = 0;
  if (TCCIR_DECODE_VREG_TYPE(store_dst_vr) == TCCIR_VREG_TYPE_PARAM) {
    avail_at_call = 1;
  } else {
    for (int k = 0; k < n; k++) {
      IRQuadCompact *p = &ir->compact_instructions[k];
      if (p->op == TCCIR_OP_NOP)
        continue;
      if (!irop_config[p->op].has_dest)
        continue;
      if (irop_get_vreg(tcc_ir_op_get_dest(ir, p)) != store_dst_vr)
        continue;
      if (k < i) {
        avail_at_call = 1;
        break;
      }
      if (p->op == TCCIR_OP_ASSIGN) {
        IROperand src = tcc_ir_op_get_src1(ir, p);
        int32_t src_vr = irop_get_vreg(src);
        if (src_vr >= 0 && TCCIR_DECODE_VREG_TYPE(src_vr) == TCCIR_VREG_TYPE_PARAM &&
            !src.is_lval && irop_get_tag(src) == IROP_TAG_VREG) {
          store_dst = src;
          store_dst.is_lval = 1;
          store_dst_vr = src_vr;
          avail_at_call = 1;
        }
      }
      break;
    }
  }
  if (!avail_at_call)
    return 0;

  ir->iroperand_pool[q->operand_base + 0] = store_dst;
  ir->compact_instructions[load_idx].op = TCCIR_OP_NOP;
  ir->compact_instructions[store_idx].op = TCCIR_OP_NOP;
  LOG_IR_GEN("=== FOLD_CALL_RESULT_STORE: CALL@%d → *vreg%d (was TEMP_LOCAL+%ld) ===",
             i, (int)store_dst_vr, (long)call_dest_off);
  return 1;
}

const IROptGen call_result_gens[] = {
    {TCCIR_OP_FUNCCALLVAL, ir_gen_dead_sret_call, "dead_sret_callval", 1},
    {TCCIR_OP_FUNCCALLVAL, ir_gen_dead_call_result, "dead_call_result", 1},
    {TCCIR_OP_FUNCCALLVAL, ir_gen_fold_call_result_store, "fold_call_result_store", 0},
    {TCCIR_OP_FUNCCALLVOID, ir_gen_dead_sret_call, "dead_sret_callvoid", 1},
};

const int call_result_gens_count = sizeof(call_result_gens) / sizeof(call_result_gens[0]);

const IROptGen call_result_post_gens[] = {
    {TCCIR_OP_FUNCCALLVAL, ir_gen_dead_sret_call, "dead_sret_post_callval", 1},
    {TCCIR_OP_FUNCCALLVOID, ir_gen_dead_sret_call, "dead_sret_post_callvoid", 1},
};

const int call_result_post_gens_count = sizeof(call_result_post_gens) / sizeof(call_result_post_gens[0]);
