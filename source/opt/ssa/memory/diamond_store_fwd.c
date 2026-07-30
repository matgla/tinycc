/*
 *  TCC IR - SSA Diamond Store Forwarding
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#define USING_GLOBALS
#include "ir.h"
#include "opt.h"
#include "opt_utils.h"
#include "ssa_opt.h"
#include "diamond_store_fwd.h"

typedef struct {
  IROperand base;
  int base_use;
  int has_imm_index;
  int32_t index_vr;
  int64_t index_imm;
  int shift;
  int btype;
} DsfAddr;

static int dsf_is_candidate_store(int op)
{
  return op == TCCIR_OP_STORE || op == TCCIR_OP_STORE_INDEXED;
}

static int dsf_effect_barrier(int op)
{
  switch (op) {
  case TCCIR_OP_FUNCCALLVOID:
  case TCCIR_OP_FUNCCALLVAL:
  case TCCIR_OP_RETURNVALUE:
  case TCCIR_OP_RETURNVOID:
  case TCCIR_OP_INLINE_ASM:
  case TCCIR_OP_ASM_OUTPUT:
  case TCCIR_OP_STORE_POSTINC:
  case TCCIR_OP_BLOCK_COPY:
  case TCCIR_OP_IJUMP:
  case TCCIR_OP_SWITCH_TABLE:
  case TCCIR_OP_SETJMP:
  case TCCIR_OP_NL_SETJMP:
  case TCCIR_OP_LONGJMP:
  case TCCIR_OP_NL_LONGJMP:
  case TCCIR_OP_BUILTIN_APPLY:
  case TCCIR_OP_BUILTIN_APPLY_ARGS:
  case TCCIR_OP_BUILTIN_RETURN:
  case TCCIR_OP_VLA_ALLOC:
  case TCCIR_OP_TRAP:
  case TCCIR_OP_INIT_CHAIN_SLOT:
  case TCCIR_OP_SET_CHAIN:
    return 1;
  default:
    return 0;
  }
}

/* store dest slots hold the base pointer (a use), not a definition */
static int dsf_find_def(TCCIRState *ir, int32_t vr, int before)
{
  int d = tcc_ir_find_defining_instruction(ir, vr, before);
  while (d >= 0 && dsf_is_candidate_store(ir->compact_instructions[d].op))
    d = tcc_ir_find_defining_instruction(ir, vr, d);
  return d;
}

static int dsf_vreg_single_real_def(TCCIRState *ir, int32_t vr)
{
  int count = 0;
  for (int i = 0; i < ir->next_instruction_index; i++) {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP || !irop_config[q->op].has_dest)
      continue;
    if (dsf_is_candidate_store(q->op))
      continue;
    if (irop_get_vreg(tcc_ir_op_get_dest(ir, q)) == vr && ++count > 1)
      return 0;
  }
  return count == 1;
}

/* structural equality only: lval (deref) operands are rejected because the
 * diamond arms themselves write memory between the compared sites */
static int dsf_expr_equal(TCCIRState *ir, IROperand a, int a_use,
                          IROperand b, int b_use, int depth)
{
  if (depth > 12)
    return 0;

  if (irop_is_immediate(a) && irop_is_immediate(b))
    return irop_get_imm64_ex(ir, a) == irop_get_imm64_ex(ir, b);
  if (irop_is_immediate(a) || irop_is_immediate(b))
    return 0;
  if (a.is_lval || b.is_lval)
    return 0;

  int32_t a_vr = irop_get_vreg(a);
  int32_t b_vr = irop_get_vreg(b);
  if (a_vr < 0 || b_vr < 0)
    return 0;

  int a_def = dsf_find_def(ir, a_vr, a_use);
  int b_def = dsf_find_def(ir, b_vr, b_use);
  if (a_def < 0 || b_def < 0)
    return a_vr == b_vr && a_def == b_def;
  if (a_def == b_def)
    return 1;
  if (!dsf_vreg_single_real_def(ir, a_vr) || !dsf_vreg_single_real_def(ir, b_vr))
    return 0;

  IRQuadCompact *qa = &ir->compact_instructions[a_def];
  IRQuadCompact *qb = &ir->compact_instructions[b_def];
  if (qa->op != qb->op)
    return 0;

  int commutative;
  switch (qa->op) {
  case TCCIR_OP_ASSIGN:
    return dsf_expr_equal(ir, tcc_ir_op_get_src1(ir, qa), a_def,
                          tcc_ir_op_get_src1(ir, qb), b_def, depth + 1);
  case TCCIR_OP_ADD:
  case TCCIR_OP_MUL:
  case TCCIR_OP_OR:
  case TCCIR_OP_AND:
  case TCCIR_OP_XOR:
    commutative = 1;
    break;
  case TCCIR_OP_SUB:
  case TCCIR_OP_SHL:
  case TCCIR_OP_SHR:
  case TCCIR_OP_SAR:
    commutative = 0;
    break;
  default:
    return 0;
  }

  IROperand a1 = tcc_ir_op_get_src1(ir, qa), a2 = tcc_ir_op_get_src2(ir, qa);
  IROperand b1 = tcc_ir_op_get_src1(ir, qb), b2 = tcc_ir_op_get_src2(ir, qb);
  if (dsf_expr_equal(ir, a1, a_def, b1, b_def, depth + 1) &&
      dsf_expr_equal(ir, a2, a_def, b2, b_def, depth + 1))
    return 1;
  return commutative &&
         dsf_expr_equal(ir, a1, a_def, b2, b_def, depth + 1) &&
         dsf_expr_equal(ir, a2, a_def, b1, b_def, depth + 1);
}

static int dsf_btype_size(int btype)
{
  switch (btype) {
  case IROP_BTYPE_INT8:
    return 1;
  case IROP_BTYPE_INT16:
    return 2;
  case IROP_BTYPE_INT32:
  case IROP_BTYPE_FLOAT32:
    return 4;
  case IROP_BTYPE_INT64:
  case IROP_BTYPE_FLOAT64:
    return 8;
  default:
    return 0;
  }
}

static int dsf_match_scaled_index(TCCIRState *ir, IROperand side, int addr_def,
                                  int32_t *out_index_vr, int *out_shift)
{
  int32_t vr = irop_get_vreg(side);
  if (vr < 0 || side.is_lval)
    return 0;
  int def = dsf_find_def(ir, vr, addr_def);
  if (def < 0)
    return 0;
  IRQuadCompact *q = &ir->compact_instructions[def];
  if (q->op != TCCIR_OP_SHL)
    return 0;
  IROperand amount = tcc_ir_op_get_src2(ir, q);
  if (!irop_is_immediate(amount))
    return 0;
  int shift = (int)irop_get_imm64_ex(ir, amount);
  if (shift < 1 || shift > 3)
    return 0;
  IROperand idx_op = tcc_ir_op_get_src1(ir, q);
  int32_t index_vr = irop_get_vreg(idx_op);
  if (index_vr < 0 || idx_op.is_lval)
    return 0;
  *out_index_vr = index_vr;
  *out_shift = shift;
  return 1;
}

static int dsf_decompose_store_addr(TCCIRState *ir, int store_idx, DsfAddr *out)
{
  IRQuadCompact *sq = &ir->compact_instructions[store_idx];
  IROperand dest = tcc_ir_op_get_dest(ir, sq);

  out->base_use = store_idx;
  out->btype = irop_get_btype(dest);
  out->has_imm_index = 0;
  out->index_vr = -1;
  out->index_imm = 0;

  if (sq->op == TCCIR_OP_STORE_INDEXED) {
    if (dest.is_lval || irop_get_vreg(dest) < 0)
      return 0;
    IROperand idx = tcc_ir_op_get_src2(ir, sq);
    IROperand sc = tcc_ir_op_get_scale(ir, sq);
    if (!irop_is_immediate(sc))
      return 0;
    int shift = (int)irop_get_imm64_ex(ir, sc);
    if (shift < 0 || shift > 3)
      return 0;
    if (irop_is_immediate(idx)) {
      out->has_imm_index = 1;
      out->index_imm = irop_get_imm64_ex(ir, idx);
    } else {
      if (idx.is_lval)
        return 0;
      out->index_vr = irop_get_vreg(idx);
      if (out->index_vr < 0)
        return 0;
    }
    out->base = dest;
    out->shift = shift;
    return 1;
  }

  if (sq->op != TCCIR_OP_STORE || !dest.is_lval)
    return 0;
  int32_t addr_vr = irop_get_vreg(dest);
  if (addr_vr < 0)
    return 0;

  int addr_def = dsf_find_def(ir, addr_vr, store_idx);
  if (addr_def < 0)
    return 0;
  IRQuadCompact *aq = &ir->compact_instructions[addr_def];
  if (aq->op != TCCIR_OP_ADD)
    return 0;

  IROperand a1 = tcc_ir_op_get_src1(ir, aq);
  IROperand a2 = tcc_ir_op_get_src2(ir, aq);
  out->base_use = addr_def;
  if (dsf_match_scaled_index(ir, a2, addr_def, &out->index_vr, &out->shift)) {
    out->base = a1;
    return 1;
  }
  if (dsf_match_scaled_index(ir, a1, addr_def, &out->index_vr, &out->shift)) {
    out->base = a2;
    return 1;
  }
  return 0;
}

static int dsf_index_same(const DsfAddr *a, const DsfAddr *b)
{
  if (a->has_imm_index != b->has_imm_index || a->shift != b->shift)
    return 0;
  return a->has_imm_index ? a->index_imm == b->index_imm
                          : a->index_vr == b->index_vr;
}

static int dsf_find_branch_store(TCCIRState *ir, int from, int limit,
                                 int64_t *out_const, int *out_merge_target,
                                 int *out_jump_idx)
{
  int store_idx = -1;
  *out_merge_target = -1;
  *out_jump_idx = -1;
  for (int j = from; j < limit; j++) {
    IRQuadCompact *q = &ir->compact_instructions[j];
    if (q->op == TCCIR_OP_NOP)
      continue;
    if (dsf_is_candidate_store(q->op)) {
      IROperand val = tcc_ir_op_get_src1(ir, q);
      if (!irop_is_immediate(val))
        return -1;
      *out_const = irop_get_imm64_ex(ir, val);
      store_idx = j;
      continue;
    }
    if (q->op == TCCIR_OP_JUMP) {
      *out_merge_target = (int)tcc_ir_op_get_dest(ir, q).u.imm32;
      *out_jump_idx = j;
      break;
    }
    if (q->op == TCCIR_OP_JUMPIF || dsf_effect_barrier(q->op))
      return -1;
  }
  return store_idx;
}

/* `(a==b) && (c==d)` heads: a run of CMP/JUMPIF pairs sharing the else target */
static int dsf_cond_chain_end(TCCIRState *ir, int jumpif_idx, int else_target)
{
  int last = jumpif_idx;
  for (int j = jumpif_idx + 1; j < else_target; j++) {
    IRQuadCompact *q = &ir->compact_instructions[j];
    if (q->op == TCCIR_OP_NOP || q->op == TCCIR_OP_CMP)
      continue;
    if (q->op != TCCIR_OP_JUMPIF)
      break;
    if ((int)tcc_ir_op_get_dest(ir, q).u.imm32 != else_target)
      break;
    last = j;
  }
  return last;
}

static int dsf_load_matches_addr(TCCIRState *ir, IRQuadCompact *q, int use_idx,
                                 const DsfAddr *addr)
{
  IROperand load_index = tcc_ir_op_get_src2(ir, q);
  if (addr->has_imm_index) {
    if (!irop_is_immediate(load_index) ||
        irop_get_imm64_ex(ir, load_index) != addr->index_imm)
      return 0;
  } else {
    if (load_index.is_lval || irop_get_vreg(load_index) != addr->index_vr)
      return 0;
  }
  IROperand scale = tcc_ir_op_get_scale(ir, q);
  if (!irop_is_immediate(scale) ||
      (int)irop_get_imm64_ex(ir, scale) != addr->shift)
    return 0;
  int store_size = dsf_btype_size(addr->btype);
  if (store_size == 0 ||
      store_size != dsf_btype_size(irop_get_btype(tcc_ir_op_get_dest(ir, q))))
    return 0;
  IROperand load_base = tcc_ir_op_get_src1(ir, q);
  if (load_base.is_lval)
    return 0;
  return dsf_expr_equal(ir, addr->base, addr->base_use, load_base, use_idx, 0);
}

static int dsf_find_merge_load(TCCIRState *ir, int merge, const DsfAddr *addr)
{
  for (int j = merge; j < ir->next_instruction_index; j++) {
    IRQuadCompact *q = &ir->compact_instructions[j];
    if (q->op == TCCIR_OP_NOP)
      continue;
    if (q->op == TCCIR_OP_LOAD_INDEXED && dsf_load_matches_addr(ir, q, j, addr))
      return j;
    if (q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_JUMPIF ||
        dsf_is_candidate_store(q->op) || dsf_effect_barrier(q->op))
      break;
  }
  return -1;
}

/* any edge into (then_start, load_idx] other than the diamond's own merge
 * jumps and the shared else entry reaches the load without both stores */
static int dsf_foreign_edge_into(TCCIRState *ir, int then_start, int else_target,
                                 int load_idx, int then_jump, int else_jump)
{
  for (int j = 0; j < ir->next_instruction_index; j++) {
    IRQuadCompact *q = &ir->compact_instructions[j];
    if (q->op == TCCIR_OP_IJUMP)
      return 1;
    if (q->op == TCCIR_OP_SWITCH_TABLE) {
      IROperand src2 = tcc_ir_op_get_src2(ir, q);
      int table_id = (int)irop_get_imm64_ex(ir, src2);
      if (table_id < 0 || table_id >= ir->num_switch_tables)
        return 1;
      TCCIRSwitchTable *table = &ir->switch_tables[table_id];
      for (int ti = 0; ti <= table->num_entries; ti++) {
        int t = ti < table->num_entries ? table->targets[ti]
                                        : table->default_target;
        if (t > then_start && t <= load_idx && t != else_target)
          return 1;
      }
      continue;
    }
    if (q->op != TCCIR_OP_JUMP && q->op != TCCIR_OP_JUMPIF)
      continue;
    if (j == then_jump || j == else_jump)
      continue;
    int t = (int)tcc_ir_op_get_dest(ir, q).u.imm32;
    if (t > then_start && t <= load_idx && t != else_target)
      return 1;
  }
  return 0;
}

static int64_t dsf_fold_value(int64_t c, IROperand load_dest)
{
  switch (irop_get_btype(load_dest)) {
  case IROP_BTYPE_INT8:
    return load_dest.is_unsigned ? (int64_t)(uint8_t)c : (int64_t)(int8_t)c;
  case IROP_BTYPE_INT16:
    return load_dest.is_unsigned ? (int64_t)(uint16_t)c : (int64_t)(int16_t)c;
  default:
    return c;
  }
}

static IROperand dsf_make_const(TCCIRState *ir, int btype, int64_t value)
{
  if (btype == IROP_BTYPE_FLOAT32)
    return irop_make_f32(-1, (uint32_t)value);
  if (btype == IROP_BTYPE_FLOAT64)
    return irop_make_f64(-1, tcc_ir_pool_add_f64(ir, (uint64_t)value));
  if (value == (int32_t)value)
    return irop_make_imm32(-1, (int32_t)value, btype);
  return irop_make_i64(-1, tcc_ir_pool_add_i64(ir, value), btype);
}

static void dsf_rewrite_load_to_assign(TCCIRState *ir, int load_idx, IROperand src)
{
  IRQuadCompact *q = &ir->compact_instructions[load_idx];
  IROperand dest = tcc_ir_op_get_dest(ir, q);
  q->op = TCCIR_OP_ASSIGN;
  ir->iroperand_pool[q->operand_base + 0] = dest;
  ir->iroperand_pool[q->operand_base + 1] = src;
  ir->iroperand_pool[q->operand_base + 2] = IROP_NONE;
  ir->iroperand_pool[q->operand_base + 3] = IROP_NONE;
}

static void dsf_insert_assign_before(TCCIRState *ir, int idx, IROperand dest,
                                     IROperand src)
{
  IRQuadCompact q = {0};
  q.op = TCCIR_OP_ASSIGN;
  q.operand_base = tcc_ir_pool_add(ir, dest);
  tcc_ir_pool_add(ir, src);
  tcc_ir_insert_instruction_before(ir, idx, &q);
}

int ssa_opt_diamond_store_fwd_core(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  int changes = 0;

  if (n < 8)
    return 0;

  for (int i = 0; i < n - 4; i++) {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_JUMPIF)
      continue;

    int else_target = (int)tcc_ir_op_get_dest(ir, q).u.imm32;
    if (else_target <= i || else_target >= n)
      continue;

    int then_start = dsf_cond_chain_end(ir, i, else_target) + 1;
    if (then_start >= else_target)
      continue;

    int64_t then_const = 0;
    int then_merge = -1, then_jump = -1;
    int then_store = dsf_find_branch_store(ir, then_start, else_target,
                                           &then_const, &then_merge, &then_jump);
    if (then_store < 0 || then_merge < 0)
      continue;

    int64_t else_const = 0;
    int else_merge = -1, else_jump = -1;
    int else_limit = then_merge < n ? then_merge : n;
    int else_store = dsf_find_branch_store(ir, else_target, else_limit,
                                           &else_const, &else_merge, &else_jump);
    if (else_store < 0)
      continue;
    if (else_merge >= 0 && else_merge != then_merge)
      continue;

    DsfAddr then_addr, else_addr;
    if (!dsf_decompose_store_addr(ir, then_store, &then_addr) ||
        !dsf_decompose_store_addr(ir, else_store, &else_addr))
      continue;
    if (!dsf_index_same(&then_addr, &else_addr) ||
        then_addr.btype != else_addr.btype)
      continue;
    if (!dsf_expr_equal(ir, then_addr.base, then_addr.base_use,
                        else_addr.base, else_addr.base_use, 0))
      continue;

    int load_idx = dsf_find_merge_load(ir, then_merge, &then_addr);
    if (load_idx < 0)
      continue;

    if (dsf_foreign_edge_into(ir, then_start, else_target, load_idx,
                              then_jump, else_jump))
      continue;

    IROperand ldest =
        tcc_ir_op_get_dest(ir, &ir->compact_instructions[load_idx]);
    int lbtype = irop_get_btype(ldest);
    int64_t vthen = dsf_fold_value(then_const, ldest);
    int64_t velse = dsf_fold_value(else_const, ldest);

    if (vthen == velse) {
      dsf_rewrite_load_to_assign(ir, load_idx, dsf_make_const(ir, lbtype, vthen));
      changes++;
      continue;
    }

    int32_t tmp_vr = tcc_ir_vreg_alloc_temp(ir);
    if (tmp_vr < 0)
      continue;
    IROperand tmp_op = irop_make_vreg(tmp_vr, lbtype);
    tmp_op.is_unsigned = ldest.is_unsigned;

    /* insert the higher-index (else) def first so then-side indices stay valid */
    int else_ins = else_jump >= 0 ? else_jump : then_merge;
    dsf_insert_assign_before(ir, else_ins, tmp_op,
                             dsf_make_const(ir, lbtype, velse));
    dsf_insert_assign_before(ir, then_jump, tmp_op,
                             dsf_make_const(ir, lbtype, vthen));
    dsf_rewrite_load_to_assign(ir, load_idx + 2, tmp_op);
    return changes + 1;
  }

  return changes;
}

int ssa_opt_diamond_store_fwd(IRSSAOptCtx *ctx)
{
  if (tcc_ir_opt_pass_disabled("ssa:diamond_store_fwd"))
    return 0;
  int changes = ssa_opt_diamond_store_fwd_core(ctx->ir);
  if (changes)
    tcc_ir_ssa_opt_rebuild(ctx);
  return changes;
}
