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
#include "opt_dsl.h"
#include "opt/flat/call_result.h"

/* Materialize dest/src1/src2 into ops[3]; absent slots become a none operand (vreg -1). */
static void call_result_ops3(TCCIRState *ir, const IRQuadCompact *p, IROperand ops[3])
{
  ops[0] = irop_config[p->op].has_dest ? tcc_ir_op_get_dest(ir, p) : (IROperand){0};
  ops[1] = irop_config[p->op].has_src1 ? tcc_ir_op_get_src1(ir, p) : (IROperand){0};
  ops[2] = irop_config[p->op].has_src2 ? tcc_ir_op_get_src2(ir, p) : (IROperand){0};
}

static int call_result_stackoff_in_range(IROperand op, int32_t base, int32_t size)
{
  if (irop_get_tag(op) != IROP_TAG_STACKOFF)
    return 0;
  int32_t off = (int32_t)irop_get_stack_offset(op);
  return off >= base && off < base + size;
}

OPT_GEN_FLAT(dead_call_result, TCCIR_OP_FUNCCALLVAL)
{
  PATTERN(.constraints = { .dest = IR_CONSTRAINT_ANY });
  const IROptDU *du = &ctx->du;

  int32_t dest_vr = irop_get_vreg(dest);
  if (TCCIR_DECODE_VREG_TYPE(dest_vr) == TCCIR_VREG_TYPE_PARAM)
    return 0;

  if (dest_vr >= 0) {
    if (ir_opt_du_uses(du, dest_vr) != 0)
      return 0;
  } else {
    /* TEMP_LOCAL dest (vr in [-9,-2]) isn't in the DU table: forward-scan and bail on any later ref (read or write). */
    if (dest_vr > -2 || dest_vr < -9) return 0;
    int n = ir->next_instruction_index;
    for (int j = i + 1; j < n; j++) {
      IRQuadCompact *p = &ir->compact_instructions[j];
      if (p->op == TCCIR_OP_NOP) continue;
      IROperand ops[3];
      call_result_ops3(ir, p, ops);
      for (int k = 0; k < 3; k++)
        if (irop_get_vreg(ops[k]) == dest_vr)
          return 0;
      /* MLA's 4th accumulator operand isn't in the 3-slot scan; a result used only as accumulator isn't dead. */
      if (p->op == TCCIR_OP_MLA) {
        IROperand accum = tcc_ir_op_get_accum(ir, p);
        if (irop_get_vreg(accum) == dest_vr)
          return 0;
      }
    }
  }

  ir->iroperand_pool[q->operand_base + 0] = src1;
  ir->iroperand_pool[q->operand_base + 1] = src2;
  REWRITE(.new_op = TCCIR_OP_FUNCCALLVOID);
}

OPT_GEN_FLAT(dead_sret_call, TCCIR_OP_FUNCCALLVAL)
{
  PATTERN(.constraints = { .dest = IR_CONSTRAINT_ANY });
  const IROptDU *du = &ctx->du;
  int n = ir->next_instruction_index;

  if (q->op == TCCIR_OP_FUNCCALLVAL) {
    int32_t dest_vr = irop_get_vreg(dest);
    if (dest_vr < 0)
      return 0;
    if (ir_opt_du_uses(du, dest_vr) != 0)
      return 0;
  }

  Sym *callee = irop_get_sym_ex(ir, src1);
  if (!callee)
    return 0;
  int callee_pure = callee->f.func_pure_via_sret;
  if (callee->type.ref)
    callee_pure |= callee->type.ref->f.func_pure_via_sret;
  if (!callee_pure)
    return 0;

  int call_id = TCCIR_DECODE_CALL_ID((uint32_t)irop_get_imm64_ex(ir, src2));
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
    call_result_ops3(ir, p, ops);
    for (int k = 0; k < 3 && !range_used_later; k++)
      if (call_result_stackoff_in_range(ops[k], sret_off, sret_size))
        range_used_later = 1;
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
    IROperand psrc1 = irop_config[p->op].has_src1 ? tcc_ir_op_get_src1(ir, p) : (IROperand){0};
    if (!(psrc1.is_local && !psrc1.is_lval &&
          call_result_stackoff_in_range(psrc1, sret_off, sret_size)))
      continue;
    IROperand pdest = tcc_ir_op_get_dest(ir, p);
    int32_t dest_vr = irop_get_vreg(pdest);
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
  REWRITE(.new_op = TCCIR_OP_NOP);
}

OPT_GEN_FLAT(fold_call_result_store, TCCIR_OP_FUNCCALLVAL)
{
  PATTERN(.constraints = { .dest = IR_CONSTRAINT_ANY });
  int n = ir->next_instruction_index;

  IROperand call_dest = dest;
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
    call_result_ops3(ir, p, ops);
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
  return 1;
}

const IROptGen call_result_gens[] = {
    {TCCIR_OP_FUNCCALLVAL, opt_dsl_dispatch_dead_sret_call_flat, "dead_sret_callval", 1},
    {TCCIR_OP_FUNCCALLVAL, opt_dsl_dispatch_dead_call_result_flat, "dead_call_result", 1},
    {TCCIR_OP_FUNCCALLVAL, opt_dsl_dispatch_fold_call_result_store_flat, "fold_call_result_store", 0},
    {TCCIR_OP_FUNCCALLVOID, opt_dsl_dispatch_dead_sret_call_flat, "dead_sret_callvoid", 1},
};

const int call_result_gens_count = sizeof(call_result_gens) / sizeof(call_result_gens[0]);
