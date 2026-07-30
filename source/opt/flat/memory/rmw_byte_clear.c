/*
 *  TCC IR - Read-Modify-Write Byte Clear (pre-SSA)
 *
 *  Copyright (c) 2025 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#define USING_GLOBALS

#include <limits.h>

#include "ir.h"
#include "opt.h"
#include "opt_engine.h"
#include "opt_du.h"
#include "opt_xform.h"
#include "opt_utils.h"
#include "opt_alias.h"
#include "opt_loop_utils.h"


/* ============================================================================
 * Read-Modify-Write Byte Clear (tcc_ir_opt_rmw_byte_clear)
 * ============================================================================
 *
 * Detects a load–AND–store pattern that clears an aligned byte within a word
 * and replaces it with a single byte store of zero.
 *
 * Pattern:
 *   [ADD  T_addr = T_base, #offset]          (optional)
 *   AND  T_val  = T_addr***DEREF*** AND #mask (mask clears one byte)
 *   STORE T_addr***DEREF*** = T_val
 *
 * When mask == 0xFFFFFF00 (clears byte 0):
 *   With ADD:    → STORE_INDEXED T_base, #0(INT8), #offset  (+ NOP ADD, AND)
 *   Without ADD: → STORE T_addr***DEREF*** = #0 (INT8)      (+ NOP AND)
 *
 * Fires on bitfield clears like `s->rLogin = s->lock = s->goodExit = 0;`
 * where the front-end generates word-wide read-modify-write but all bits
 * within a byte are zeroed, making a byte store semantically equivalent.
 */

typedef struct RMWByteAddr {
  int32_t base_vr;
  int32_t offset;
} RMWByteAddr;

typedef struct RMWByteClearPair {
  int and_idx;
  int store_idx;
  int add_idx;
  int can_fold_add;
  int32_t and_dest_vr;
  uint8_t clear_bits;
  RMWByteAddr addr;
  IROperand add_base_op;
} RMWByteClearPair;

static int rmw_byte_clear_resolve_addr(TCCIRState *ir, IROperand addr_op, int before_idx,
                                       RMWByteAddr *out)
{
  int32_t addr_vr;
  int def_idx;
  IRQuadCompact *defq;
  IROperand add_src1;
  IROperand add_src2;

  if (!out || !irop_op_is_lval(addr_op))
    return 0;

  addr_vr = irop_get_vreg(addr_op);
  if (addr_vr < 0)
    return 0;

  out->base_vr = addr_vr;
  out->offset = 0;

  def_idx = tcc_ir_find_defining_instruction(ir, addr_vr, before_idx);
  if (def_idx < 0)
    return 1;

  defq = &ir->compact_instructions[def_idx];
  if (defq->op != TCCIR_OP_ADD)
    return 1;

  add_src1 = tcc_ir_op_get_src1(ir, defq);
  add_src2 = tcc_ir_op_get_src2(ir, defq);
  if (!irop_has_vreg(add_src1) || irop_op_is_lval(add_src1) || !irop_is_immediate(add_src2))
    return 1;
  if (!ir_xform_same_block(ir, def_idx, before_idx))
    return 1;

  out->base_vr = irop_get_vreg(add_src1);
  out->offset = irop_get_imm32(add_src2);
  return 1;
}

static int rmw_byte_clear_same_addr(const RMWByteAddr *a, const RMWByteAddr *b)
{
  return a->base_vr == b->base_vr && a->offset == b->offset;
}

static int rmw_byte_clear_find_foldable_add(TCCIRState *ir, IROperand addr_op, int before_idx,
                                            int *out_add_idx, IROperand *out_base_op)
{
  int32_t addr_vr;
  int def_idx;
  IRQuadCompact *defq;
  IROperand add_src1;
  IROperand add_src2;
  int32_t offset;

  *out_add_idx = -1;
  *out_base_op = IROP_NONE;

  if (!irop_op_is_lval(addr_op))
    return 0;
  addr_vr = irop_get_vreg(addr_op);
  if (addr_vr < 0)
    return 0;

  def_idx = tcc_ir_find_defining_instruction(ir, addr_vr, before_idx);
  if (def_idx < 0 || !ir_xform_same_block(ir, def_idx, before_idx))
    return 0;

  defq = &ir->compact_instructions[def_idx];
  if (defq->op != TCCIR_OP_ADD)
    return 0;

  add_src1 = tcc_ir_op_get_src1(ir, defq);
  add_src2 = tcc_ir_op_get_src2(ir, defq);
  if (!irop_has_vreg(add_src1) || irop_op_is_lval(add_src1) || !irop_is_immediate(add_src2))
    return 0;

  offset = irop_get_imm32(add_src2);
  if (offset < -255 || offset > 4095)
    return 0;

  *out_add_idx = def_idx;
  *out_base_op = add_src1;
  return 1;
}

static int rmw_byte_clear_vreg_used_elsewhere(TCCIRState *ir, int32_t vr, int skip_a, int skip_b,
                                              int skip_c)
{
  int n = ir->next_instruction_index;

  if (vr < 0)
    return 1;

  for (int k = 0; k < n; k++)
  {
    IRQuadCompact *q;

    if (k == skip_a || k == skip_b || k == skip_c)
      continue;

    q = &ir->compact_instructions[k];
    if (q->op == TCCIR_OP_NOP)
      continue;

    if (irop_config[q->op].has_dest)
    {
      IROperand d = tcc_ir_op_get_dest(ir, q);
      if (irop_has_vreg(d) && irop_get_vreg(d) == vr && d.is_lval)
        return 1;
    }
    if (irop_config[q->op].has_src1)
    {
      IROperand s1 = tcc_ir_op_get_src1(ir, q);
      if (irop_has_vreg(s1) && irop_get_vreg(s1) == vr)
        return 1;
    }
    if (irop_config[q->op].has_src2)
    {
      IROperand s2 = tcc_ir_op_get_src2(ir, q);
      if (irop_has_vreg(s2) && irop_get_vreg(s2) == vr)
        return 1;
    }
    if (q->op == TCCIR_OP_MLA)
    {
      IROperand acc = tcc_ir_op_get_accum(ir, q);
      if (irop_has_vreg(acc) && irop_get_vreg(acc) == vr)
        return 1;
    }
  }

  return 0;
}

static int rmw_byte_clear_match_pair(TCCIRState *ir, int and_idx, RMWByteClearPair *out)
{
  int n = ir->next_instruction_index;
  IRQuadCompact *q;
  IROperand and_dest;
  IROperand and_src1;
  IROperand and_src2;
  uint32_t mask;
  uint32_t cleared;
  int j;
  IRQuadCompact *sq;
  IROperand store_dest;
  IROperand store_src;

  if (!out || and_idx < 0 || and_idx >= n)
    return 0;

  q = &ir->compact_instructions[and_idx];
  if (q->op != TCCIR_OP_AND)
    return 0;

  and_dest = tcc_ir_op_get_dest(ir, q);
  and_src1 = tcc_ir_op_get_src1(ir, q);
  and_src2 = tcc_ir_op_get_src2(ir, q);

  if (!irop_op_is_lval(and_src1) || !irop_is_immediate(and_src2))
    return 0;

  mask = (uint32_t)irop_get_imm32(and_src2);
  cleared = ~mask;
  if (cleared == 0 || (cleared & ~0xffu) != 0)
    return 0;

  out->and_dest_vr = irop_get_vreg(and_dest);
  if (out->and_dest_vr < 0)
    return 0;
  if (!tcc_ir_vreg_has_single_use(ir, out->and_dest_vr, -1))
    return 0;

  j = and_idx + 1;
  while (j < n && ir->compact_instructions[j].op == TCCIR_OP_NOP)
    j++;
  if (j >= n || !ir_xform_same_block(ir, and_idx, j))
    return 0;

  sq = &ir->compact_instructions[j];
  if (sq->op != TCCIR_OP_STORE)
    return 0;

  store_dest = tcc_ir_op_get_dest(ir, sq);
  store_src = tcc_ir_op_get_src1(ir, sq);
  if (!irop_op_is_lval(store_dest))
    return 0;
  if (irop_get_vreg(store_src) != out->and_dest_vr)
    return 0;
  if (irop_get_vreg(store_dest) != irop_get_vreg(and_src1))
    return 0;
  if (!rmw_byte_clear_resolve_addr(ir, store_dest, j, &out->addr))
    return 0;

  out->and_idx = and_idx;
  out->store_idx = j;
  out->add_idx = -1;
  out->can_fold_add = rmw_byte_clear_find_foldable_add(ir, store_dest, j, &out->add_idx,
                                                       &out->add_base_op);
  out->clear_bits = (uint8_t)cleared;
  return 1;
}

static int rmw_byte_clear_runs(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  int changes = 0;

  for (int i = 0; i < n; i++)
  {
    RMWByteClearPair first;
    RMWByteClearPair run[16];
    uint8_t bits;
    int count;
    int scan;

    if (!rmw_byte_clear_match_pair(ir, i, &first))
      continue;

    run[0] = first;
    bits = first.clear_bits;
    count = 1;
    scan = first.store_idx + 1;

    while (scan < n && count < (int)(sizeof(run) / sizeof(run[0])))
    {
      RMWByteClearPair next;

      while (scan < n && ir->compact_instructions[scan].op == TCCIR_OP_NOP)
        scan++;
      if (scan >= n)
        break;
      if (!ir_xform_same_block(ir, run[count - 1].store_idx, scan))
        break;
      if (!rmw_byte_clear_match_pair(ir, scan, &next))
        break;
      if (!rmw_byte_clear_same_addr(&first.addr, &next.addr))
        break;

      run[count++] = next;
      bits |= next.clear_bits;
      scan = next.store_idx + 1;
    }

    if (count < 2 || bits != 0xffu)
      continue;

    for (int k = 0; k < count - 1; k++)
    {
      IROperand store_dest = tcc_ir_op_get_dest(ir, &ir->compact_instructions[run[k].store_idx]);
      int32_t addr_vr = irop_get_vreg(store_dest);

      if (run[k].can_fold_add &&
          !rmw_byte_clear_vreg_used_elsewhere(ir, addr_vr, run[k].add_idx, run[k].and_idx,
                                              run[k].store_idx))
        ir_xform_nop(ir, run[k].add_idx);
    }
    for (int k = 0; k < count; k++)
      ir_xform_nop(ir, run[k].and_idx);
    for (int k = 0; k < count - 1; k++)
      ir_xform_nop(ir, run[k].store_idx);

    {
      IRQuadCompact *last_store = &ir->compact_instructions[run[count - 1].store_idx];
      IROperand store_dest = tcc_ir_op_get_dest(ir, last_store);
      IROperand new_src = irop_make_imm32(-1, 0, IROP_BTYPE_INT8);
      int32_t addr_vr = irop_get_vreg(store_dest);

      if (run[count - 1].can_fold_add &&
          !rmw_byte_clear_vreg_used_elsewhere(ir, addr_vr, run[count - 1].add_idx,
                                              run[count - 1].and_idx, run[count - 1].store_idx))
      {
        int new_base;
        IROperand base_op = run[count - 1].add_base_op;
        IROperand index_op = irop_make_imm32(0, run[count - 1].addr.offset, IROP_BTYPE_INT32);
        IROperand scale_op = irop_make_imm32(0, 0, IROP_BTYPE_INT32);

        tcc_ir_pool_ensure(ir, 4);
        new_base = ir->iroperand_pool_count;
        base_op.is_lval = 0;
        tcc_ir_pool_add(ir, base_op);
        tcc_ir_pool_add(ir, new_src);
        tcc_ir_pool_add(ir, index_op);
        tcc_ir_pool_add(ir, scale_op);

        last_store->op = TCCIR_OP_STORE_INDEXED;
        last_store->operand_base = new_base;
        ir_xform_nop(ir, run[count - 1].add_idx);
      }
      else
      {
        store_dest.btype = IROP_BTYPE_INT8;
        tcc_ir_op_set_dest(ir, last_store, store_dest);
        tcc_ir_op_set_src1(ir, last_store, new_src);
      }
    }

    changes++;
    i = run[count - 1].store_idx;
  }

  return changes;
}

int tcc_ir_opt_rmw_byte_clear(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  int changes = rmw_byte_clear_runs(ir);

  for (int i = 0; i < n; i++) {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_AND)
      continue;

    IROperand and_dest = tcc_ir_op_get_dest(ir, q);
    IROperand and_src1 = tcc_ir_op_get_src1(ir, q);
    IROperand and_src2 = tcc_ir_op_get_src2(ir, q);

    if (!irop_op_is_lval(and_src1))
      continue;
    if (!irop_is_immediate(and_src2))
      continue;

    uint32_t mask = (uint32_t)irop_get_imm32(and_src2);
    if (mask != 0xFFFFFF00u)
      continue;

    int32_t and_dest_vr = irop_get_vreg(and_dest);
    int32_t addr_vr = irop_get_vreg(and_src1);
    if (and_dest_vr < 0 || addr_vr < 0)
      continue;

    int j = i + 1;
    while (j < n && ir->compact_instructions[j].op == TCCIR_OP_NOP)
      j++;
    if (j >= n)
      continue;
    if (!ir_xform_same_block(ir, i, j))
      continue;

    IRQuadCompact *sq = &ir->compact_instructions[j];
    if (sq->op != TCCIR_OP_STORE)
      continue;

    IROperand store_dest = tcc_ir_op_get_dest(ir, sq);
    IROperand store_src = tcc_ir_op_get_src1(ir, sq);

    if (irop_get_vreg(store_src) != and_dest_vr)
      continue;
    if (!irop_op_is_lval(store_dest))
      continue;
    if (irop_get_vreg(store_dest) != addr_vr)
      continue;

    if (!tcc_ir_vreg_has_single_use(ir, and_dest_vr, -1))
      continue;

    int add_idx = -1;
    IROperand add_base_op = IROP_NONE;
    int32_t offset = 0;
    int can_fold_add = 0;

    for (int k = i - 1; k >= 0; k--) {
      IRQuadCompact *kq = &ir->compact_instructions[k];
      if (kq->op == TCCIR_OP_NOP)
        continue;
      if (!irop_config[kq->op].has_dest)
        continue;
      IROperand d = tcc_ir_op_get_dest(ir, kq);
      if (!irop_has_vreg(d) || irop_get_vreg(d) != addr_vr)
        continue;
      if (kq->op == TCCIR_OP_ADD) {
        IROperand s1 = tcc_ir_op_get_src1(ir, kq);
        IROperand s2 = tcc_ir_op_get_src2(ir, kq);
        if (irop_is_immediate(s2) && irop_has_vreg(s1)) {
          int32_t imm = irop_get_imm32(s2);
          if (imm >= -255 && imm <= 4095) {
            add_idx = k;
            add_base_op = s1;
            offset = imm;
          }
        }
      }
      break;
    }

    if (add_idx >= 0) {
      int extra = 0;
      for (int k = 0; k < n && !extra; k++) {
        if (k == i || k == j)
          continue;
        IRQuadCompact *kq = &ir->compact_instructions[k];
        if (kq->op == TCCIR_OP_NOP)
          continue;
        int is_store_op = (kq->op == TCCIR_OP_STORE || kq->op == TCCIR_OP_STORE_INDEXED ||
                           kq->op == TCCIR_OP_STORE_POSTINC);
        if (irop_config[kq->op].has_dest) {
          IROperand d = tcc_ir_op_get_dest(ir, kq);
          if (irop_has_vreg(d) && irop_get_vreg(d) == addr_vr && is_store_op) {
            extra = 1;
            break;
          }
        }
        if (irop_config[kq->op].has_src1) {
          IROperand s1 = tcc_ir_op_get_src1(ir, kq);
          if (irop_has_vreg(s1) && irop_get_vreg(s1) == addr_vr) {
            extra = 1;
            break;
          }
        }
        if (irop_config[kq->op].has_src2) {
          IROperand s2 = tcc_ir_op_get_src2(ir, kq);
          if (irop_has_vreg(s2) && irop_get_vreg(s2) == addr_vr) {
            extra = 1;
            break;
          }
        }
      }
      if (!extra && ir_xform_same_block(ir, add_idx, j))
        can_fold_add = 1;
    }

    if (can_fold_add) {
      tcc_ir_pool_ensure(ir, 4);
      int new_base = ir->iroperand_pool_count;
      if (new_base + 4 > ir->iroperand_pool_capacity)
        continue;

      IROperand base_op = add_base_op;
      base_op.is_lval = 0;
      IROperand value_op = irop_make_imm32(-1, 0, IROP_BTYPE_INT8);
      IROperand index_op = irop_make_imm32(0, offset, IROP_BTYPE_INT32);
      IROperand scale_op = irop_make_imm32(0, 0, IROP_BTYPE_INT32);

      tcc_ir_pool_add(ir, base_op);
      tcc_ir_pool_add(ir, value_op);
      tcc_ir_pool_add(ir, index_op);
      tcc_ir_pool_add(ir, scale_op);

      sq->op = TCCIR_OP_STORE_INDEXED;
      sq->operand_base = new_base;

      ir_xform_nop(ir, i);
      ir_xform_nop(ir, add_idx);
    } else {
      ir_xform_nop(ir, i);

      IROperand new_src = irop_make_imm32(-1, 0, IROP_BTYPE_INT8);
      tcc_ir_op_set_src1(ir, sq, new_src);

      IROperand new_dest = store_dest;
      new_dest.btype = IROP_BTYPE_INT8;
      tcc_ir_op_set_dest(ir, sq, new_dest);
    }

    changes++;
  }

  return changes;
}

