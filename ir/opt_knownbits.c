/*
 *  TCC IR - Known-Bits Propagation
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

/* Tracks per-TMP and per-stack-slot "known bits" (which bits are known to be
 * 0 or 1) within each basic block, and rewrites operations to an immediate
 * ASSIGN whenever all bits of the destination become known.
 *
 * Motivating pattern is bitfield insert/extract:
 *
 *   T2 = (X AND  0xFFFFC07F) OR 0x1E80    ; insert: bits 7..13 forced to 61
 *   T5 = (T2 AND 0xFFFFFF80) OR 0x73      ; insert: bits 0..6 forced to 115
 *   *(StackLoc[-4]) = T5
 *   ... straight-line code ...
 *   T9 = StackLoc[-4] SHL 18              ; load stack-4, extract stage 1
 *   T10 = T9 SHR 25                       ; extract stage 2 → 61
 *
 * Both temps and stack slots flow kb through the lattice; reads of a
 * StackLoc[X] lval pick up the kb of the most recent store-source.
 *
 * Limitations: 32-bit lattice (skips INT64), single-BB scope (kb is
 * invalidated at jump targets, indirect control flow, and calls that can see
 * a local stack address).
 */

#define USING_GLOBALS

#include "ir.h"
#include "opt.h"
#include "opt_engine.h"
#include "opt_utils.h"

typedef struct
{
  int gen;        /* matches current_gen when entry is valid */
  uint32_t kz;    /* known-zero mask */
  uint32_t ko;    /* known-one mask */
  int32_t stack_off; /* if >= INT32_MIN+1, temp holds Addr[StackLoc[off]] */
  int has_stack_off;
  uint64_t const_val;
  int has_const;
  uint8_t is_low32; /* kz/ko track only the low 32 bits of a 64-bit value */
} TmpKB;

typedef struct
{
  int32_t off;    /* stack offset (signed; negative for locals) */
  int gen;
  uint32_t kz;
  uint32_t ko;
} StackKB;

typedef struct
{
  int gen;
  int32_t stack_off;
  int has_stack_off;
} VregAddrKB;

static uint32_t kb_width_mask(int btype)
{
  switch (btype)
  {
  case IROP_BTYPE_INT8:
    return 0xFFu;
  case IROP_BTYPE_INT16:
    return 0xFFFFu;
  default:
    return 0xFFFFFFFFu;
  }
}

static void kb_apply_store_width(int btype, uint32_t *kz, uint32_t *ko)
{
  uint32_t mask = kb_width_mask(btype);
  *kz &= mask;
  *ko &= mask;
}

/* btype/is_unsigned are passed as scalars (not the whole IROperand) on purpose:
 * passing a 9-byte __attribute__((packed)) IROperand by value miscompiles on the
 * self-hosted ARM cross — the 9th byte (the is_unsigned/flags byte) is dropped in
 * the caller's argument marshalling, so an unsigned sub-word load would be read as
 * signed and sign-extended (e.g. uint8_t 200 -> -56).  Reading the flags via a
 * direct field access in the caller and passing the bit through a register-sized
 * int sidesteps the bad struct-by-value path. */
static void kb_apply_load_width(int btype, int is_unsigned, uint32_t *kz, uint32_t *ko)
{
  uint32_t mask = kb_width_mask(btype);

  if (mask == 0xFFFFFFFFu)
    return;

  *kz &= mask;
  *ko &= mask;

  uint32_t high_mask = ~mask;
  if (is_unsigned)
  {
    *kz |= high_mask;
    return;
  }

  uint32_t sign_bit = (btype == IROP_BTYPE_INT8) ? 0x80u : 0x8000u;
  if (*ko & sign_bit)
    *ko |= high_mask;
  else if (*kz & sign_bit)
    *kz |= high_mask;
}

static int vreg_addr_lookup(int32_t vr, const TmpKB *tmp_kb,
                            int max_tmp_pos, const VregAddrKB *var_addr,
                            int max_var_pos, int current_gen,
                            int32_t *out_off)
{
  if (vr < 0)
    return 0;

  int type = TCCIR_DECODE_VREG_TYPE(vr);
  int pos = TCCIR_DECODE_VREG_POSITION(vr);

  if (type == TCCIR_VREG_TYPE_TEMP)
  {
    if (pos <= max_tmp_pos && tmp_kb[pos].gen == current_gen &&
        tmp_kb[pos].has_stack_off)
    {
      *out_off = tmp_kb[pos].stack_off;
      return 1;
    }
    return 0;
  }

  if (type == TCCIR_VREG_TYPE_VAR)
  {
    if (pos <= max_var_pos && var_addr[pos].gen == current_gen &&
        var_addr[pos].has_stack_off)
    {
      *out_off = var_addr[pos].stack_off;
      return 1;
    }
  }

  return 0;
}

static int kb_is_direct_stackoff(IROperand op, int is_lval)
{
  return op.is_local && op.is_lval == is_lval &&
         op.tag == IROP_TAG_STACKOFF && irop_get_vreg(op) < 0;
}

static int kb_lval_stack_off(const TCCIRState *ir, IROperand op,
                             const TmpKB *tmp_kb, int max_tmp_pos,
                             const VregAddrKB *var_addr, int max_var_pos,
                             int current_gen, int32_t *out_off)
{
  if (kb_is_direct_stackoff(op, 1))
  {
    *out_off = (int32_t)irop_get_imm64_ex(ir, op);
    return 1;
  }
  if (op.is_lval)
  {
    int32_t vr = irop_get_vreg(op);
    return vreg_addr_lookup(vr, tmp_kb, max_tmp_pos, var_addr, max_var_pos,
                            current_gen, out_off);
  }
  return 0;
}

static int kb_value_is_stack_addr(const TCCIRState *ir, IROperand op,
                                  const TmpKB *tmp_kb, int max_tmp_pos,
                                  const VregAddrKB *var_addr, int max_var_pos,
                                  int current_gen)
{
  int32_t off;

  if (kb_is_direct_stackoff(op, 0))
    return 1;

  return vreg_addr_lookup(irop_get_vreg(op), tmp_kb, max_tmp_pos, var_addr,
                          max_var_pos, current_gen, &off);
}

static int kb_call_exposes_stack_addr(TCCIRState *ir, int call_i,
                                      const TmpKB *tmp_kb, int max_tmp_pos,
                                      const VregAddrKB *var_addr,
                                      int max_var_pos, int current_gen)
{
  IROperand call_meta = tcc_ir_op_get_src2(ir, &ir->compact_instructions[call_i]);
  int argc = TCCIR_DECODE_CALL_ARGC((uint32_t)irop_get_imm64_ex(ir, call_meta));

  for (int p = 0; p < argc; p++)
  {
    IROperand arg;
    if (ir_opt_get_call_param_operand(ir, call_i, p, &arg) &&
        kb_value_is_stack_addr(ir, arg, tmp_kb, max_tmp_pos, var_addr,
                               max_var_pos, current_gen))
      return 1;
  }

  return 0;
}

static int kb_operand_depends_on_tmp(const TCCIRState *ir, int start, int end,
                                     IROperand op, int32_t root_vr)
{
  int32_t vr = irop_get_vreg(op);
  if (vr == root_vr)
    return 1;
  if (vr < 0 || TCCIR_DECODE_VREG_TYPE(vr) != TCCIR_VREG_TYPE_TEMP)
    return 0;

  for (int i = end; i >= start; i--)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP || !irop_config[q->op].has_dest)
      continue;
    IROperand dest = tcc_ir_op_get_dest(ir, q);
    if (irop_get_vreg(dest) != vr || dest.is_lval)
      continue;

    if (irop_config[q->op].has_src1 &&
        kb_operand_depends_on_tmp(ir, start, i - 1, tcc_ir_op_get_src1(ir, q),
                                  root_vr))
      return 1;
    if (irop_config[q->op].has_src2 &&
        kb_operand_depends_on_tmp(ir, start, i - 1, tcc_ir_op_get_src2(ir, q),
                                  root_vr))
      return 1;
    return 0;
  }

  return 0;
}

static int kb_load_feeds_same_slot_store(const TCCIRState *ir, int load_i,
                                         IROperand load_src, IROperand load_dest,
                                         const TmpKB *tmp_kb, int max_tmp_pos,
                                         const VregAddrKB *var_addr,
                                         int max_var_pos, int current_gen)
{
  int32_t load_off;
  int32_t load_vr = irop_get_vreg(load_dest);
  if (load_vr < 0 || !kb_lval_stack_off(ir, load_src, tmp_kb, max_tmp_pos,
                                        var_addr, max_var_pos, current_gen,
                                        &load_off))
    return 0;

  int end = ir->next_instruction_index;
  int scan_end = load_i + 8;
  if (scan_end < end)
    end = scan_end;

  for (int j = load_i + 1; j < end; j++)
  {
    IRQuadCompact *q = &ir->compact_instructions[j];
    if (q->op == TCCIR_OP_NOP)
      continue;
    if (q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_JUMPIF ||
        q->op == TCCIR_OP_IJUMP || q->op == TCCIR_OP_FUNCCALLVOID ||
        q->op == TCCIR_OP_FUNCCALLVAL)
      return 0;
    if (q->op != TCCIR_OP_STORE)
      continue;

    IROperand store_dest = tcc_ir_op_get_dest(ir, q);
    int32_t store_off;
    if (!kb_lval_stack_off(ir, store_dest, tmp_kb, max_tmp_pos, var_addr,
                           max_var_pos, current_gen, &store_off) ||
        store_off != load_off)
      continue;

    IROperand store_src = tcc_ir_op_get_src1(ir, q);
    return kb_operand_depends_on_tmp(ir, load_i + 1, j - 1, store_src,
                                     load_vr);
  }

  return 0;
}

/* Look up stack-slot kb for `off`. Returns 1 if a valid entry exists. */
static int stack_kb_lookup(const StackKB *slots, int n_slots, int current_gen,
                           int32_t off, uint32_t *out_kz, uint32_t *out_ko)
{
  for (int i = 0; i < n_slots; i++)
  {
    if (slots[i].gen == current_gen && slots[i].off == off)
    {
      *out_kz = slots[i].kz;
      *out_ko = slots[i].ko;
      return 1;
    }
  }
  return 0;
}

static int stack_kb_const32(const StackKB *slots, int n_slots, int current_gen,
                            int32_t off, uint32_t *out)
{
  uint32_t kz, ko;
  if (!stack_kb_lookup(slots, n_slots, current_gen, off, &kz, &ko))
    return 0;
  if ((kz | ko) != 0xFFFFFFFFu)
    return 0;
  *out = ko;
  return 1;
}

/* btype/is_unsigned are scalars, not a by-value IROperand: passing a 9-byte
 * __attribute__((packed)) IROperand by value miscompiles on the self-hosted ARM
 * cross (the 9th flags byte — is_unsigned/is_static/is_sym/is_param — is dropped
 * in the caller's argument marshalling), so an unsigned sub-word value would be
 * read as signed and sign-extended (uint8_t 200 -> -56).  Same hazard as
 * kb_apply_load_width; callers read the flags by direct field access. */
static uint64_t kb_apply_const_width(uint64_t v, int btype, int is_unsigned)
{
  switch (btype)
  {
  case IROP_BTYPE_INT8:
    v &= 0xFFu;
    if (!is_unsigned && (v & 0x80u))
      v |= ~0xFFULL;
    return v;
  case IROP_BTYPE_INT16:
    v &= 0xFFFFu;
    if (!is_unsigned && (v & 0x8000u))
      v |= ~0xFFFFULL;
    return v;
  case IROP_BTYPE_INT32:
    v &= 0xFFFFFFFFu;
    if (!is_unsigned && (v & 0x80000000u))
      v |= ~0xFFFFFFFFULL;
    return v;
  default:
    return v;
  }
}

/* `op` is passed by pointer (not by value) so the byte-8 flags (is_unsigned via
 * kb_apply_const_width) survive: a by-value 9-byte packed IROperand drops its 9th
 * byte in the cross's caller-side arg marshalling.  irop_get_btype()/
 * irop_is_immediate()/irop_get_imm64_ex() are called with *op (by value) but only
 * read word-0/word-1 fields, which marshal correctly. */
static int kb_operand_const_u64(const TCCIRState *ir, const IROperand *op,
                                const TmpKB *tmp_kb, int max_tmp_pos,
                                int current_gen,
                                const VregAddrKB *var_addr, int max_var_pos,
                                const StackKB *slots, int n_slots,
                                uint64_t *out)
{
  int btype = irop_get_btype(*op);
  if (irop_is_immediate(*op) && !op->is_sym && !op->is_lval)
  {
    /* FLOAT immediates encode a pool index in u.imm32 rather than the bit
     * pattern of the value, so reading them as integers would yield the
     * index and silently corrupt later folds.  STRUCT immediates have no
     * scalar representation. */
    if (btype == IROP_BTYPE_FLOAT32 || btype == IROP_BTYPE_FLOAT64 ||
        btype == IROP_BTYPE_STRUCT)
      return 0;
    *out = kb_apply_const_width((uint64_t)irop_get_imm64_ex(ir, *op), btype, op->is_unsigned);
    return 1;
  }

  if (op->is_lval)
  {
    int32_t stack_off;
    if (!kb_lval_stack_off(ir, *op, tmp_kb, max_tmp_pos, var_addr, max_var_pos,
                           current_gen, &stack_off))
      return 0;

    if (btype == IROP_BTYPE_INT64)
    {
      uint32_t lo, hi;
      if (!stack_kb_const32(slots, n_slots, current_gen, stack_off, &lo) ||
          !stack_kb_const32(slots, n_slots, current_gen, stack_off + 4, &hi))
        return 0;
      *out = ((uint64_t)hi << 32) | lo;
      return 1;
    }

    if (btype != IROP_BTYPE_FLOAT32 &&
        btype != IROP_BTYPE_FLOAT64 &&
        btype != IROP_BTYPE_STRUCT)
    {
      uint32_t v;
      if (!stack_kb_const32(slots, n_slots, current_gen, stack_off, &v))
        return 0;
      *out = kb_apply_const_width(v, btype, op->is_unsigned);
      return 1;
    }
    return 0;
  }

  int32_t vr = irop_get_vreg(*op);
  if (vr < 0 || TCCIR_DECODE_VREG_TYPE(vr) != TCCIR_VREG_TYPE_TEMP)
    return 0;

  int pos = TCCIR_DECODE_VREG_POSITION(vr);
  if (pos > max_tmp_pos || tmp_kb[pos].gen != current_gen ||
      !tmp_kb[pos].has_const)
    return 0;

  *out = kb_apply_const_width(tmp_kb[pos].const_val, btype, op->is_unsigned);
  return 1;
}

static IROperand kb_make_const_operand(TCCIRState *ir, uint64_t val, int btype)
{
  if (btype != IROP_BTYPE_INT64)
    return irop_make_imm32(-1, (int32_t)(uint32_t)val, btype);

  if ((int64_t)val == (int64_t)(int32_t)val)
    return irop_make_imm32(-1, (int32_t)val, btype);

  uint32_t pool_idx = tcc_ir_pool_add_i64(ir, (int64_t)val);
  return irop_make_i64(-1, pool_idx, btype);
}

static int kb_const_compute(TccIrOp op, int dest_btype,
                            uint64_t a, uint64_t b, uint64_t *out)
{
  int width = (dest_btype == IROP_BTYPE_INT64) ? 64 : 32;
  uint64_t mask = (width == 64) ? ~0ULL : 0xFFFFFFFFULL;

  switch (op)
  {
  case TCCIR_OP_ASSIGN:
  case TCCIR_OP_LOAD:
  case TCCIR_OP_ZEXT:
    *out = a;
    break;
  case TCCIR_OP_ADD:
    *out = a + b;
    break;
  case TCCIR_OP_SUB:
    *out = a - b;
    break;
  case TCCIR_OP_AND:
    *out = a & b;
    break;
  case TCCIR_OP_OR:
    *out = a | b;
    break;
  case TCCIR_OP_XOR:
    *out = a ^ b;
    break;
  case TCCIR_OP_SHL:
    if (b >= (uint64_t)width)
      return 0;
    *out = a << b;
    break;
  case TCCIR_OP_SHR:
    if (b >= (uint64_t)width)
      return 0;
    *out = a >> b;
    break;
  case TCCIR_OP_SAR:
    if (b >= (uint64_t)width)
      return 0;
    if (width == 64)
      *out = (uint64_t)((int64_t)a >> b);
    else
      *out = (uint64_t)(uint32_t)((int32_t)(uint32_t)a >> b);
    break;
  default:
    return 0;
  }

  *out &= mask;
  if (dest_btype != IROP_BTYPE_INT64 && (*out & 0x80000000ULL))
    *out |= ~0xFFFFFFFFULL;
  return 1;
}

/* Record/update stack-slot kb. */
static void stack_kb_set(StackKB *slots, int *n_slots, int slots_cap,
                         int current_gen, int32_t off,
                         uint32_t kz, uint32_t ko)
{
  for (int i = 0; i < *n_slots; i++)
  {
    if (slots[i].off == off)
    {
      slots[i].gen = current_gen;
      slots[i].kz = kz;
      slots[i].ko = ko;
      return;
    }
  }
  if (*n_slots < slots_cap)
  {
    slots[*n_slots].off = off;
    slots[*n_slots].gen = current_gen;
    slots[*n_slots].kz = kz;
    slots[*n_slots].ko = ko;
    (*n_slots)++;
  }
}

/* Invalidate all stack slots (e.g., after CALL or at BB boundary). */
static void stack_kb_invalidate_all(StackKB *slots, int n_slots)
{
  for (int i = 0; i < n_slots; i++)
    slots[i].gen = 0;
}

static void stack_kb_rebase_gen(StackKB *slots, int n_slots,
                                int old_gen, int new_gen)
{
  for (int i = 0; i < n_slots; i++)
    if (slots[i].gen == old_gen)
      slots[i].gen = new_gen;
}

/* Get known bits for an operand. Returns 1 if any bit is known. */
static int kb_operand(const TCCIRState *ir, IROperand op,
                      const TmpKB *tmp_kb, int max_tmp_pos, int current_gen,
                      const VregAddrKB *var_addr, int max_var_pos,
                      const StackKB *slots, int n_slots,
                      uint32_t *out_kz, uint32_t *out_ko)
{
  *out_kz = 0;
  *out_ko = 0;

  /* Direct StackLoc[X] lval — reading the slot's current value.
   * Must have no real vreg: a VAR/TEMP/PARAM with STACKOFF tag
   * is a vreg-backed pseudoreg whose "stack offset" is a potential spill
   * slot, not a real direct stack reference. */
  if (kb_is_direct_stackoff(op, 1) &&
      op.btype != IROP_BTYPE_INT64 &&
      op.btype != IROP_BTYPE_FLOAT32 && op.btype != IROP_BTYPE_FLOAT64)
  {
    int32_t off = (int32_t)irop_get_imm64_ex(ir, op);
    return stack_kb_lookup(slots, n_slots, current_gen, off, out_kz, out_ko);
  }

  /* Immediate constant operand. */
  if (irop_is_immediate(op) && !op.is_sym && !op.is_lval)
  {
    int btype = irop_get_btype(op);
    if (btype == IROP_BTYPE_INT64 || btype == IROP_BTYPE_FLOAT32 ||
        btype == IROP_BTYPE_FLOAT64)
      return 0;
    int64_t v = irop_get_imm64_ex(ir, op);
    uint32_t u = (uint32_t)v;
    *out_ko = u;
    *out_kz = ~u;
    return 1;
  }

  /* Temp deref (T***DEREF***): if the temp holds Addr[StackLoc[off]], use
   * stack_kb[off].  Otherwise unknown. */
  if (op.is_lval)
  {
    int32_t stack_off;
    if (kb_lval_stack_off(ir, op, tmp_kb, max_tmp_pos, var_addr, max_var_pos,
                          current_gen, &stack_off))
    {
      return stack_kb_lookup(slots, n_slots, current_gen, stack_off,
                             out_kz, out_ko);
    }
    return 0;
  }

  int32_t vr = irop_get_vreg(op);
  if (vr < 0 || TCCIR_DECODE_VREG_TYPE(vr) != TCCIR_VREG_TYPE_TEMP)
    return 0;
  int pos = TCCIR_DECODE_VREG_POSITION(vr);
  if (pos > max_tmp_pos)
    return 0;
  if (tmp_kb[pos].gen != current_gen)
    return 0;
  *out_kz = tmp_kb[pos].kz;
  *out_ko = tmp_kb[pos].ko;
  return (*out_kz | *out_ko) != 0;
}

/* Compute known bits for the destination of `op` given the operands' kb.
 * Returns 1 if any bit is known.  32-bit lattice. */
static int kb_compute(TccIrOp op, uint32_t a_kz, uint32_t a_ko,
                      uint32_t b_kz, uint32_t b_ko,
                      uint32_t *out_kz, uint32_t *out_ko)
{
  *out_kz = 0;
  *out_ko = 0;
  switch (op)
  {
  case TCCIR_OP_AND:
    *out_kz = a_kz | b_kz;
    *out_ko = a_ko & b_ko;
    break;
  case TCCIR_OP_OR:
    *out_ko = a_ko | b_ko;
    *out_kz = a_kz & b_kz;
    break;
  case TCCIR_OP_XOR:
    *out_ko = (a_ko & b_kz) | (a_kz & b_ko);
    *out_kz = (a_ko & b_ko) | (a_kz & b_kz);
    break;
  case TCCIR_OP_SHL:
  {
    if ((b_kz | b_ko) != 0xFFFFFFFFu)
      return 0;
    uint32_t n = b_ko & 31;
    if (n == 0)
    {
      *out_kz = a_kz;
      *out_ko = a_ko;
    }
    else
    {
      uint32_t low_mask = (1u << n) - 1u;
      *out_ko = (a_ko << n) & 0xFFFFFFFFu;
      *out_kz = ((a_kz << n) & 0xFFFFFFFFu) | low_mask;
    }
    break;
  }
  case TCCIR_OP_SHR:
  {
    if ((b_kz | b_ko) != 0xFFFFFFFFu)
      return 0;
    uint32_t n = b_ko & 31;
    if (n == 0)
    {
      *out_kz = a_kz;
      *out_ko = a_ko;
    }
    else
    {
      uint32_t high_mask = (0xFFFFFFFFu << (32 - n)) & 0xFFFFFFFFu;
      *out_ko = a_ko >> n;
      *out_kz = (a_kz >> n) | high_mask;
    }
    break;
  }
  case TCCIR_OP_SAR:
  {
    if ((b_kz | b_ko) != 0xFFFFFFFFu)
      return 0;
    uint32_t n = b_ko & 31;
    if (n == 0)
    {
      *out_kz = a_kz;
      *out_ko = a_ko;
    }
    else
    {
      uint32_t high_mask = (0xFFFFFFFFu << (32 - n)) & 0xFFFFFFFFu;
      uint32_t sign_bit_kz = (a_kz >> 31) & 1u;
      uint32_t sign_bit_ko = (a_ko >> 31) & 1u;
      *out_ko = a_ko >> n;
      *out_kz = a_kz >> n;
      if (sign_bit_ko)
        *out_ko |= high_mask;
      else if (sign_bit_kz)
        *out_kz |= high_mask;
    }
    break;
  }
  case TCCIR_OP_ASSIGN:
  case TCCIR_OP_LOAD:
  case TCCIR_OP_ZEXT:
    *out_kz = a_kz;
    *out_ko = a_ko;
    break;
  case TCCIR_OP_ADD:
  case TCCIR_OP_SUB:
  {
    /* Bit-by-bit half-add with carry propagation.  For SUB we add ~b + 1
     * (two's-complement negation): re-bias the b knownbits as ~b
     * (swap kz<->ko) and inject an initial carry of 1.  At each bit we
     * stop the moment any input bit or the incoming carry becomes
     * unknown — beyond that, both the bit and the outgoing carry are
     * unknown, so we can't tighten anything higher up. */
    uint32_t kz = 0, ko = 0;
    uint32_t carry_known = 1, carry_val = 0;
    if (op == TCCIR_OP_SUB)
    {
      uint32_t tmp = b_kz;
      b_kz = b_ko;
      b_ko = tmp;
      carry_val = 1; /* +1 for two's complement */
    }
    for (int i = 0; i < 32; i++)
    {
      uint32_t mask = 1u << i;
      int a_known = ((a_kz | a_ko) & mask) != 0;
      int b_known = ((b_kz | b_ko) & mask) != 0;
      if (!a_known || !b_known || !carry_known)
        break;
      uint32_t a_bit = (a_ko >> i) & 1u;
      uint32_t b_bit = (b_ko >> i) & 1u;
      uint32_t sum_bit = a_bit ^ b_bit ^ carry_val;
      uint32_t new_carry = (a_bit & b_bit) | ((a_bit ^ b_bit) & carry_val);
      if (sum_bit)
        ko |= mask;
      else
        kz |= mask;
      carry_val = new_carry;
    }
    *out_kz = kz;
    *out_ko = ko;
    break;
  }
  default:
    return 0;
  }
  return (*out_kz | *out_ko) != 0;
}

#define KB_MAX_STACK_SLOTS 32

static int tcc_ir_opt_known_bits__timed(TCCIRState *ir);
int tcc_ir_opt_known_bits(TCCIRState *ir)
{
  tcc_pass_timing_init();
  if (!tcc_pass_timing_on) return tcc_ir_opt_known_bits__timed(ir);
  unsigned long _t = tcc_pass_clk_us();
  int _r = tcc_ir_opt_known_bits__timed(ir);
  tcc_pass_timing_add("known_bits", tcc_pass_clk_us() - _t);
  return _r;
}
static int tcc_ir_opt_known_bits__timed(TCCIRState *ir)
{
  int n = ir->next_instruction_index;

  if (n == 0)
    return 0;

  int max_tmp_pos = 0;
  int max_var_pos = 0;
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP || !irop_config[q->op].has_dest)
      continue;
    IROperand dest = tcc_ir_op_get_dest(ir, q);
    int32_t vr = irop_get_vreg(dest);
    int type = TCCIR_DECODE_VREG_TYPE(vr);
    int pos = TCCIR_DECODE_VREG_POSITION(vr);
    if (type == TCCIR_VREG_TYPE_TEMP && pos > max_tmp_pos)
      max_tmp_pos = pos;
    else if (type == TCCIR_VREG_TYPE_VAR && pos > max_var_pos)
      max_var_pos = pos;
  }
  if (max_tmp_pos == 0)
    return 0;

  size_t kb_bytes = sizeof(TmpKB) * (max_tmp_pos + 1);
  size_t var_addr_bytes = sizeof(VregAddrKB) * (max_var_pos + 1);
  size_t bs_bytes = sizeof(int) * n;
  TmpKB *tmp_kb = tcc_mallocz(kb_bytes);
  VregAddrKB *var_addr = tcc_mallocz(var_addr_bytes);
  int *block_start_seen = tcc_mallocz(bs_bytes);
  int *backedge_target = tcc_mallocz(bs_bytes);
  StackKB stack_slots[KB_MAX_STACK_SLOTS];
  int n_stack_slots = 0;
  int block_gen = 1;
  int current_gen = 1;
  int stack_addr_escaped = 0;
  int stack_dirty_since_split = 0;
  int changes = 0;

  ir_opt_mark_block_starts(ir, block_start_seen, block_gen, n);
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_JUMPIF)
    {
      int target = (int)irop_get_imm64_ex(ir, tcc_ir_op_get_dest(ir, q));
      if (target >= 0 && target <= i && target < n)
        backedge_target[target] = 1;
    }
  }

  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];

    /* BB boundary: invalidate temps and stack slots. */
    if (i != 0 && block_start_seen[i] == block_gen)
    {
      int old_gen = current_gen;
      current_gen++;
      if (stack_dirty_since_split || backedge_target[i])
        stack_kb_invalidate_all(stack_slots, n_stack_slots);
      else
        stack_kb_rebase_gen(stack_slots, n_stack_slots, old_gen, current_gen);
      stack_dirty_since_split = 0;
    }

    if (q->op == TCCIR_OP_NOP)
      continue;

    int op = q->op;

    /* STORE: record stack-slot kb if dest is a known stack slot. */
    if (op == TCCIR_OP_STORE)
    {
      IROperand dest = tcc_ir_op_get_dest(ir, q);
      IROperand src1 = tcc_ir_op_get_src1(ir, q);
      int dest_btype = irop_get_btype(dest);

      if (kb_value_is_stack_addr(ir, src1, tmp_kb, max_tmp_pos,
                                 var_addr, max_var_pos, current_gen))
        stack_addr_escaped = 1;
      stack_dirty_since_split = 1;

      int32_t stack_off = INT32_MIN;
      int have_off = 0;

      /* Direct StackLoc[X] dest — must have no real vreg attached (otherwise
       * the operand is a VAR/PARAM with STACKOFF spill encoding, not a
       * real stack reference). */
      have_off = kb_lval_stack_off(ir, dest, tmp_kb, max_tmp_pos, var_addr,
                                   max_var_pos, current_gen, &stack_off);

      /* Wide / non-integer stores: no kb is recorded for them, but they still
       * overwrite the slot — a value tracked from an earlier narrow store
       * (e.g. a union initializer's zero-fill) must not survive them.
       * FLOAT32 clobbers 4 bytes, INT64/FLOAT64 8; STRUCT has unknown width,
       * and an unknown destination may alias any slot. */
      if (dest_btype == IROP_BTYPE_INT64 || dest_btype == IROP_BTYPE_FLOAT32 ||
          dest_btype == IROP_BTYPE_FLOAT64 || dest_btype == IROP_BTYPE_STRUCT)
      {
        if (have_off && dest_btype != IROP_BTYPE_STRUCT)
        {
          int32_t width = (dest_btype == IROP_BTYPE_FLOAT32) ? 4 : 8;
          for (int s = 0; s < n_stack_slots; s++)
            if (stack_slots[s].off + 4 > stack_off &&
                stack_slots[s].off < stack_off + width)
              stack_slots[s].gen = 0;
        }
        else
        {
          stack_kb_invalidate_all(stack_slots, n_stack_slots);
        }
        goto post_op;
      }

      if (have_off)
      {
        uint32_t kz, ko;
        if (kb_operand(ir, src1, tmp_kb, max_tmp_pos, current_gen,
                       var_addr, max_var_pos,
                       stack_slots, n_stack_slots, &kz, &ko))
        {
          kb_apply_store_width(dest_btype, &kz, &ko);
          stack_kb_set(stack_slots, &n_stack_slots, KB_MAX_STACK_SLOTS,
                       current_gen, stack_off, kz, ko);
        }
        else
        {
          /* Unknown source: invalidate stack-slot. */
          for (int s = 0; s < n_stack_slots; s++)
            if (stack_slots[s].off == stack_off)
              stack_slots[s].gen = 0;
        }
      }
      else
      {
        /* STORE through unknown pointer: conservatively invalidate all slots. */
        stack_kb_invalidate_all(stack_slots, n_stack_slots);
      }
      goto post_op;
    }

    /* CALL: stack locals only become externally mutable after their address
     * escapes.  Indirect control flow and asm remain fully conservative. */
    if (op == TCCIR_OP_FUNCCALLVOID || op == TCCIR_OP_FUNCCALLVAL)
    {
      if (kb_call_exposes_stack_addr(ir, i, tmp_kb, max_tmp_pos,
                                     var_addr, max_var_pos, current_gen))
        stack_addr_escaped = 1;
      if (stack_addr_escaped)
      {
        stack_kb_invalidate_all(stack_slots, n_stack_slots);
        stack_dirty_since_split = 1;
      }
      /* FUNCCALLVAL has a dest TMP; clear its kb below via the fall-through
       * dest handler. */
    }
    else if (op == TCCIR_OP_IJUMP || op == TCCIR_OP_INLINE_ASM ||
             op == TCCIR_OP_SETJMP || op == TCCIR_OP_LONGJMP ||
             op == TCCIR_OP_VLA_ALLOC)
    {
      stack_kb_invalidate_all(stack_slots, n_stack_slots);
      stack_dirty_since_split = 1;
    }
    else if (op == TCCIR_OP_SET_CHAIN)
    {
      /* SET_CHAIN hands the current frame pointer to a nested-function call,
       * so the callee can mutate any of this frame's locals through the
       * static chain.  We can't see what the callee touches, so invalidate
       * everything and mark the address as escaped — the next CALL must be
       * treated as fully aliasing. */
      stack_addr_escaped = 1;
      stack_kb_invalidate_all(stack_slots, n_stack_slots);
      stack_dirty_since_split = 1;
    }

    if (op == TCCIR_OP_JUMPIF)
      stack_dirty_since_split = 0;

    /* TEST_ZERO + JUMPIF EQ/NE folding using known-bits.  When kb proves
     * src1 has any known-one bit (ko != 0), the value is provably non-zero
     * and the EQ branch is dead / NE branch unconditional.  branch_folding
     * can't see this — it requires src1 to already be an immediate.  Catches
     * the `~(p_10 | 1) + 1 != 0` shape (pr43255) where the low bit is set
     * by OR #1, propagated through XOR/ADD via the bit-by-bit kb_compute. */
    if (op == TCCIR_OP_TEST_ZERO)
    {
      IROperand src1 = tcc_ir_op_get_src1(ir, q);
      uint32_t kz, ko;
      if (kb_operand(ir, src1, tmp_kb, max_tmp_pos, current_gen,
                     var_addr, max_var_pos,
                     stack_slots, n_stack_slots, &kz, &ko) &&
          ko != 0)
      {
        int j = i + 1;
        while (j < n && ir->compact_instructions[j].op == TCCIR_OP_NOP)
          j++;
        if (j < n && ir->compact_instructions[j].op == TCCIR_OP_JUMPIF &&
            !ir->compact_instructions[j].is_jump_target)
        {
          IRQuadCompact *jq = &ir->compact_instructions[j];
          IROperand cond = tcc_ir_op_get_src1(ir, jq);
          int tok = (int)irop_get_imm64_ex(ir, cond);
          int branch_taken;
          if (tok == 0x94)        /* EQ: would-jump iff value == 0 */
            branch_taken = 0;
          else if (tok == 0x95)   /* NE: would-jump iff value != 0 */
            branch_taken = 1;
          else
            goto post_op;
          if (branch_taken)
          {
            IROperand dest = tcc_ir_op_get_dest(ir, jq);
            q->op = TCCIR_OP_NOP;
            jq->op = TCCIR_OP_JUMP;
            tcc_ir_set_dest(ir, j, dest);
          }
          else
          {
            q->op = TCCIR_OP_NOP;
            jq->op = TCCIR_OP_NOP;
            /* When the JUMPIF wasn't taken, control falls through.  If the
             * next op is a SETIF that reads the same flag state we just
             * NOPed, codegen would lower it consuming garbage flags (the
             * value we tested via kb has provably bit-set so ko != 0 means
             * non-zero — fold the SETIF to its constant result).  Mirrors
             * the SETIF fold in branch_fold_test_zero (opt_gens_branch.c). */
            int k = j + 1;
            while (k < n && ir->compact_instructions[k].op == TCCIR_OP_NOP)
              k++;
            if (k < n)
            {
              IRQuadCompact *setif_q = &ir->compact_instructions[k];
              if (setif_q->op == TCCIR_OP_SETIF && !setif_q->is_jump_target)
              {
                IROperand setif_cond = tcc_ir_op_get_src1(ir, setif_q);
                int setif_tok = (int)irop_get_imm64_ex(ir, setif_cond);
                int setif_result = -1;
                /* ko != 0 → value is non-zero → NE true, EQ false. */
                if (setif_tok == 0x95) setif_result = 1;      /* NE */
                else if (setif_tok == 0x94) setif_result = 0; /* EQ */
                if (setif_result >= 0)
                {
                  IROperand dest = tcc_ir_op_get_dest(ir, setif_q);
                  IROperand imm = irop_make_imm32(-1, setif_result, irop_get_btype(dest));
                  setif_q->op = TCCIR_OP_ASSIGN;
                  tcc_ir_set_src1(ir, k, imm);
                  tcc_ir_set_src2(ir, k, IROP_NONE);
                }
              }
            }
          }
          LOG_IR_GEN("OPTIMIZE: knownbits TEST_ZERO fold at i=%d "
                     "(ko=%08x, tok=0x%x -> %s)",
                     i, ko, tok, branch_taken ? "JUMP" : "NOP");
          changes++;
          goto post_op;
        }
      }
    }

    /* CMP source folding: when src1 or src2 is a deref / direct stack lval
     * whose value is fully known via kb, rewrite the operand to the
     * immediate.  The const-fold path below only fires for ops with a dest
     * tmp, so CMP patterns like `CMP T_addr***DEREF***, #0` would otherwise
     * be left for sl_forward — but the aggressive kb folding compacts the IR
     * so any later CALL invalidates sl_forward's tracked stores before it
     * reaches such CMPs.  Folding the operand here keeps the CMP foldable by
     * downstream branch_folding/const_prop. */
    if (op == TCCIR_OP_CMP)
    {
      for (int si = 0; si < 2; si++)
      {
        if (si == 0 && !irop_config[op].has_src1)
          continue;
        if (si == 1 && !irop_config[op].has_src2)
          continue;
        IROperand sop = (si == 0) ? tcc_ir_op_get_src1(ir, q)
                                  : tcc_ir_op_get_src2(ir, q);
        if (!sop.is_lval)
          continue;
        int sop_btype = irop_get_btype(sop);
        if (sop_btype == IROP_BTYPE_FLOAT32 || sop_btype == IROP_BTYPE_FLOAT64 ||
            sop_btype == IROP_BTYPE_STRUCT)
          continue;
        uint64_t cv;
        if (!kb_operand_const_u64(ir, &sop, tmp_kb, max_tmp_pos, current_gen,
                                  var_addr, max_var_pos,
                                  stack_slots, n_stack_slots, &cv))
          continue;
        IROperand imm = kb_make_const_operand(ir, cv, sop_btype);
        imm.is_unsigned = sop.is_unsigned;
        if (si == 0)
          tcc_ir_set_src1(ir, i, imm);
        else
          tcc_ir_set_src2(ir, i, imm);
        changes++;
      }
    }

    /* CMP src1, #0 + JUMPIF tok: fold signed comparisons against zero when
     * src1's sign bit is known via knownbits.  The full-constant CMP fold
     * above only catches values that are completely known; this catches the
     * common pattern where only the sign is determined — e.g. sign-extend
     * of (X ^ K) with K's high bit set, which is always negative regardless
     * of X.  Restricted to 32-bit operands: a CMP with INT64 / sub-word
     * operands carries different signed-comparison semantics that the
     * 32-bit sign-bit reasoning would mis-fold. */
    if (op == TCCIR_OP_CMP)
    {
      IROperand cmp_src1 = tcc_ir_op_get_src1(ir, q);
      IROperand cmp_src2 = tcc_ir_op_get_src2(ir, q);
      int s1_bt = irop_get_btype(cmp_src1);
      int s2_bt = irop_get_btype(cmp_src2);
      uint32_t kz, ko;
      if (s1_bt == IROP_BTYPE_INT32 && s2_bt == IROP_BTYPE_INT32 &&
          irop_is_immediate(cmp_src2) && !cmp_src2.is_sym &&
          irop_get_imm64_ex(ir, cmp_src2) == 0 &&
          kb_operand(ir, cmp_src1, tmp_kb, max_tmp_pos, current_gen,
                     var_addr, max_var_pos,
                     stack_slots, n_stack_slots, &kz, &ko))
      {
        int j = i + 1;
        while (j < n && ir->compact_instructions[j].op == TCCIR_OP_NOP)
          j++;
        if (j < n && ir->compact_instructions[j].op == TCCIR_OP_JUMPIF &&
            !ir->compact_instructions[j].is_jump_target)
        {
          IRQuadCompact *jq = &ir->compact_instructions[j];
          IROperand cond = tcc_ir_op_get_src1(ir, jq);
          int tok = (int)irop_get_imm64_ex(ir, cond);
          int sign_one = (ko >> 31) & 1u;      /* bit 31 known 1 → src1 < 0 */
          int sign_zero = (kz >> 31) & 1u;     /* bit 31 known 0 → src1 >= 0 */
          int nonzero = (ko != 0);              /* any known-one bit → src1 != 0 */
          int branch_taken = -1;
          if (sign_one) /* src1 < 0 (always non-zero) */
          {
            if (tok == 0x9c || tok == 0x9e) branch_taken = 1; /* <S, <=S */
            else if (tok == 0x9d || tok == 0x9f) branch_taken = 0; /* >=S, >S */
            else if (tok == 0x94) branch_taken = 0; /* == */
            else if (tok == 0x95) branch_taken = 1; /* != */
          }
          else if (sign_zero && nonzero) /* src1 > 0 */
          {
            if (tok == 0x9c || tok == 0x9e) branch_taken = 0; /* <S, <=S */
            else if (tok == 0x9d || tok == 0x9f) branch_taken = 1; /* >=S, >S */
            else if (tok == 0x94) branch_taken = 0; /* == */
            else if (tok == 0x95) branch_taken = 1; /* != */
          }
          else if (sign_zero) /* src1 >= 0 (could be 0) */
          {
            if (tok == 0x9c) branch_taken = 0; /* <S */
            else if (tok == 0x9d) branch_taken = 1; /* >=S */
          }
          if (branch_taken >= 0)
          {
            if (branch_taken)
            {
              IROperand dest = tcc_ir_op_get_dest(ir, jq);
              q->op = TCCIR_OP_NOP;
              jq->op = TCCIR_OP_JUMP;
              tcc_ir_set_dest(ir, j, dest);
            }
            else
            {
              q->op = TCCIR_OP_NOP;
              jq->op = TCCIR_OP_NOP;
            }
            LOG_IR_GEN("OPTIMIZE: knownbits CMP+JUMPIF sign fold at i=%d "
                       "(ko=%08x kz=%08x tok=0x%x -> %s)",
                       i, ko, kz, tok, branch_taken ? "JUMP" : "NOP");
            changes++;
            goto post_op;
          }
        }
      }
    }

    int has_dest = irop_config[op].has_dest;
    if (!has_dest)
      continue;

    IROperand dest = tcc_ir_op_get_dest(ir, q);
    int32_t dvr = irop_get_vreg(dest);
    IROperand s1_raw = IROP_NONE;
    IROperand s2_raw = IROP_NONE;
    if (irop_config[op].has_src1)
      s1_raw = tcc_ir_op_get_src1(ir, q);
    if (irop_config[op].has_src2)
      s2_raw = tcc_ir_op_get_src2(ir, q);
    int dest_is_tmp =
        (dvr >= 0) && (TCCIR_DECODE_VREG_TYPE(dvr) == TCCIR_VREG_TYPE_TEMP);
    int dest_is_var =
        (dvr >= 0) && (TCCIR_DECODE_VREG_TYPE(dvr) == TCCIR_VREG_TYPE_VAR);
    int dest_is_lval = dest.is_lval;

    if (!dest_is_lval && dest_is_var)
    {
      int vpos = TCCIR_DECODE_VREG_POSITION(dvr);
      if (vpos <= max_var_pos &&
          (op == TCCIR_OP_ASSIGN || op == TCCIR_OP_LEA) &&
          kb_is_direct_stackoff(s1_raw, 0))
      {
        var_addr[vpos].gen = current_gen;
        var_addr[vpos].has_stack_off = 1;
        var_addr[vpos].stack_off = (int32_t)irop_get_imm64_ex(ir, s1_raw);
      }
      else if (vpos <= max_var_pos)
      {
        var_addr[vpos].gen = 0;
        var_addr[vpos].has_stack_off = 0;
      }
    }

    if (!dest_is_tmp || dest_is_lval)
      continue;
    int dpos = TCCIR_DECODE_VREG_POSITION(dvr);
    int dest_btype = irop_get_btype(dest);

    /* Track temp → stack-offset mapping for LEA/ASSIGN of Addr[StackLoc].
     * Same direct-stack guard as kb_operand's StackLoc read path. */
    if ((op == TCCIR_OP_ASSIGN || op == TCCIR_OP_LEA) &&
        kb_is_direct_stackoff(s1_raw, 0))
    {
      int32_t off = (int32_t)irop_get_imm64_ex(ir, s1_raw);
      tmp_kb[dpos].gen = current_gen;
      tmp_kb[dpos].kz = 0;
      tmp_kb[dpos].ko = 0;
      tmp_kb[dpos].has_stack_off = 1;
      tmp_kb[dpos].stack_off = off;
      tmp_kb[dpos].has_const = 0;
      continue;
    }
    if (op == TCCIR_OP_ASSIGN)
    {
      int32_t off;
      if (vreg_addr_lookup(irop_get_vreg(s1_raw), tmp_kb, max_tmp_pos,
                           var_addr, max_var_pos, current_gen, &off))
      {
        tmp_kb[dpos].gen = current_gen;
        tmp_kb[dpos].kz = 0;
        tmp_kb[dpos].ko = 0;
        tmp_kb[dpos].has_stack_off = 1;
        tmp_kb[dpos].stack_off = off;
        tmp_kb[dpos].has_const = 0;
        continue;
      }
    }
    if ((op == TCCIR_OP_ADD || op == TCCIR_OP_SUB) &&
        irop_is_immediate(s2_raw) && !s2_raw.is_sym && !s2_raw.is_lval)
    {
      int32_t off;
      if (vreg_addr_lookup(irop_get_vreg(s1_raw), tmp_kb, max_tmp_pos,
                           var_addr, max_var_pos, current_gen, &off))
      {
        int64_t delta = irop_get_imm64_ex(ir, s2_raw);
        if (op == TCCIR_OP_SUB)
          delta = -delta;
        int64_t new_off = (int64_t)off + delta;
        if (new_off >= INT32_MIN && new_off <= INT32_MAX)
        {
          tmp_kb[dpos].gen = current_gen;
          tmp_kb[dpos].kz = 0;
          tmp_kb[dpos].ko = 0;
          tmp_kb[dpos].has_stack_off = 1;
          tmp_kb[dpos].stack_off = (int32_t)new_off;
          tmp_kb[dpos].has_const = 0;
          continue;
        }
      }
    }

    /* Clear any old address/constant facts on redefinition. */
    tmp_kb[dpos].has_stack_off = 0;
    tmp_kb[dpos].has_const = 0;


    IROperand s1 = s1_raw;
    IROperand s2 = s2_raw;
    int s1_btype = irop_get_btype(s1);
    int s2_btype = irop_get_btype(s2);
    {
      uint64_t cv1 = 0, cv2 = 0, cres = 0;
      int h1 = 0, h2 = 0;
      if (irop_config[op].has_src1)
        h1 = kb_operand_const_u64(ir, &s1, tmp_kb, max_tmp_pos, current_gen,
                                  var_addr, max_var_pos,
                                  stack_slots, n_stack_slots, &cv1);
      if (irop_config[op].has_src2)
        h2 = kb_operand_const_u64(ir, &s2, tmp_kb, max_tmp_pos, current_gen,
                                  var_addr, max_var_pos,
                                  stack_slots, n_stack_slots, &cv2);
      if (h1 && (!irop_config[op].has_src2 || h2) &&
          kb_const_compute(op, dest_btype, cv1, cv2, &cres))
      {
        IROperand imm = kb_make_const_operand(ir, cres, dest_btype);
        imm.is_unsigned = dest.is_unsigned;
        int already_folded = (op == TCCIR_OP_ASSIGN &&
                              irop_is_immediate(s1) && !s1.is_sym &&
                              !s1.is_lval &&
                              irop_get_imm64_ex(ir, s1) == (int64_t)cres);
        /* See sub-word LOAD comment in kb_compute path below. */
        int suppress_rewrite = 0;
        if (op == TCCIR_OP_LOAD)
        {
          uint32_t low_mask = (dest_btype == IROP_BTYPE_INT8) ? 0xFFu :
                              (dest_btype == IROP_BTYPE_INT16) ? 0xFFFFu : 0;
          if (low_mask && ((uint32_t)cres & low_mask) == low_mask)
            suppress_rewrite = 1;
        }
        if (!already_folded && !suppress_rewrite)
        {
          q->op = TCCIR_OP_ASSIGN;
          tcc_ir_set_src1(ir, i, imm);
          tcc_ir_set_src2(ir, i, IROP_NONE);
          changes++;
        }
        tmp_kb[dpos].gen = current_gen;
        tmp_kb[dpos].kz = ~(uint32_t)cres;
        tmp_kb[dpos].ko = (uint32_t)cres;
        tmp_kb[dpos].const_val = cres;
        tmp_kb[dpos].has_const = 1;
        continue;
      }
    }

    if (dest_btype == IROP_BTYPE_INT64 ||
        dest_btype == IROP_BTYPE_FLOAT32 ||
        dest_btype == IROP_BTYPE_FLOAT64 ||
        dest_btype == IROP_BTYPE_STRUCT)
    {
      /* Track low 32 bits of 64-bit integer values through SHL/SUB/ASSIGN
       * chains so that 32-bit consumers (shift amounts, truncations) can
       * constant-fold.  Example: bswap64(zext32(x)) = bswap32(x)<<32,
       * then y = (uint32_t)(32 - result) = 32 always. */
      if (dest_btype == IROP_BTYPE_INT64)
      {
        /* SHL by constant >= 32: low 32 bits are all zero */
        if (op == TCCIR_OP_SHL && irop_is_immediate(s2) && !s2.is_sym && !s2.is_lval)
        {
          int64_t amt = irop_get_imm64_ex(ir, s2);
          if (amt >= 32)
          {
            tmp_kb[dpos].gen = current_gen;
            tmp_kb[dpos].kz = 0xFFFFFFFFu;
            tmp_kb[dpos].ko = 0;
            tmp_kb[dpos].has_const = 0;
            tmp_kb[dpos].is_low32 = 1;
            continue;
          }
        }
        /* SUB/ADD with 64-bit operands: propagate low 32 bits */
        if (op == TCCIR_OP_SUB || op == TCCIR_OP_ADD)
        {
          uint32_t a_kz64 = 0, a_ko64 = 0, b_kz64 = 0, b_ko64 = 0;
          int h1 = 0, h2 = 0;
          if (irop_is_immediate(s1) && !s1.is_sym && !s1.is_lval)
          {
            uint32_t v = (uint32_t)irop_get_imm64_ex(ir, s1);
            a_kz64 = ~v; a_ko64 = v; h1 = 1;
          }
          else if (irop_has_vreg(s1))
          {
            int32_t vr = irop_get_vreg(s1);
            if (vr >= 0 && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_TEMP)
            {
              int sp = TCCIR_DECODE_VREG_POSITION(vr);
              if (sp >= 0 && sp <= max_tmp_pos && tmp_kb[sp].gen == current_gen && tmp_kb[sp].is_low32)
              { a_kz64 = tmp_kb[sp].kz; a_ko64 = tmp_kb[sp].ko; h1 = 1; }
            }
          }
          if (irop_is_immediate(s2) && !s2.is_sym && !s2.is_lval)
          {
            uint32_t v = (uint32_t)irop_get_imm64_ex(ir, s2);
            b_kz64 = ~v; b_ko64 = v; h2 = 1;
          }
          else if (irop_has_vreg(s2))
          {
            int32_t vr = irop_get_vreg(s2);
            if (vr >= 0 && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_TEMP)
            {
              int sp = TCCIR_DECODE_VREG_POSITION(vr);
              if (sp >= 0 && sp <= max_tmp_pos && tmp_kb[sp].gen == current_gen && tmp_kb[sp].is_low32)
              { b_kz64 = tmp_kb[sp].kz; b_ko64 = tmp_kb[sp].ko; h2 = 1; }
            }
          }
          if ((h1 || h2) && (h1 || (a_kz64 == 0 && a_ko64 == 0)) &&
              (h2 || (b_kz64 == 0 && b_ko64 == 0)))
          {
            uint32_t dkz64, dko64;
            if (kb_compute(op, a_kz64, a_ko64, b_kz64, b_ko64, &dkz64, &dko64))
            {
              tmp_kb[dpos].gen = current_gen;
              tmp_kb[dpos].kz = dkz64;
              tmp_kb[dpos].ko = dko64;
              tmp_kb[dpos].has_const = 0;
              tmp_kb[dpos].is_low32 = 1;
              continue;
            }
          }
        }
        /* ASSIGN/ZEXT of 64-bit to 64-bit: propagate low32 kb */
        if (op == TCCIR_OP_ASSIGN || op == TCCIR_OP_ZEXT)
        {
          if (irop_has_vreg(s1))
          {
            int32_t vr = irop_get_vreg(s1);
            int vtype = TCCIR_DECODE_VREG_TYPE(vr);
            int sp = TCCIR_DECODE_VREG_POSITION(vr);
            if (vr >= 0 && vtype == TCCIR_VREG_TYPE_TEMP)
            {
              if (sp >= 0 && sp <= max_tmp_pos && tmp_kb[sp].gen == current_gen && tmp_kb[sp].is_low32)
              {
                tmp_kb[dpos].gen = current_gen;
                tmp_kb[dpos].kz = tmp_kb[sp].kz;
                tmp_kb[dpos].ko = tmp_kb[sp].ko;
                tmp_kb[dpos].has_const = 0;
                tmp_kb[dpos].is_low32 = 1;
                continue;
              }
            }
          }
        }
      }
      tmp_kb[dpos].gen = 0;
      continue;
    }

    int wide_src =
        (s1_btype == IROP_BTYPE_INT64) || (s2_btype == IROP_BTYPE_INT64) ||
        (s1_btype == IROP_BTYPE_FLOAT32) || (s2_btype == IROP_BTYPE_FLOAT32) ||
        (s1_btype == IROP_BTYPE_FLOAT64) || (s2_btype == IROP_BTYPE_FLOAT64);
    if (wide_src)
    {
      /* 32-bit shift/and with 64-bit shift amount: if the amount's low 32
       * bits are fully known, rewrite src2 to an immediate constant. */
      if ((op == TCCIR_OP_SHL || op == TCCIR_OP_SHR || op == TCCIR_OP_SAR ||
           op == TCCIR_OP_AND || op == TCCIR_OP_SUB || op == TCCIR_OP_ADD ||
           op == TCCIR_OP_ASSIGN || op == TCCIR_OP_ZEXT) &&
          dest_btype != IROP_BTYPE_INT64)
      {
        int which = 0; /* 1 = s1 is 64-bit with known low32, 2 = s2 */
        int32_t low32_val = 0;
        for (int side = 1; side <= 2; side++)
        {
          IROperand sN = (side == 1) ? s1 : s2;
          int sN_btype = (side == 1) ? s1_btype : s2_btype;
          if (sN_btype != IROP_BTYPE_INT64) continue;
          if (!irop_has_vreg(sN)) continue;
          int32_t vr = irop_get_vreg(sN);
          if (vr < 0) continue;
          if (TCCIR_DECODE_VREG_TYPE(vr) != TCCIR_VREG_TYPE_TEMP) continue;
          int sp = TCCIR_DECODE_VREG_POSITION(vr);
          if (sp < 0 || sp > max_tmp_pos) continue;
          if (tmp_kb[sp].gen != current_gen || !tmp_kb[sp].is_low32) continue;
          if ((tmp_kb[sp].kz | tmp_kb[sp].ko) != 0xFFFFFFFFu) continue;
          which = side;
          low32_val = (int32_t)tmp_kb[sp].ko;
          break;
        }
        if (which)
        {
          IROperand imm = irop_make_imm32(-1, low32_val, IROP_BTYPE_INT32);
          if (which == 1)
            tcc_ir_set_src1(ir, i, imm);
          else
            tcc_ir_set_src2(ir, i, imm);
          LOG_IR_GEN("OPTIMIZE: low32 narrow 64-bit operand to #%d at i=%d", low32_val, i);
          changes++;
          /* Re-fetch operands and fall through to normal 32-bit kb tracking */
          s1 = tcc_ir_op_get_src1(ir, q);
          s2 = tcc_ir_op_get_src2(ir, q);
          s1_btype = irop_get_btype(s1);
          s2_btype = irop_get_btype(s2);
          goto recheck_wide;
        }
      }
      tmp_kb[dpos].gen = 0;
      continue;
recheck_wide:;
      int wide_src2 =
          (s1_btype == IROP_BTYPE_INT64) || (s2_btype == IROP_BTYPE_INT64) ||
          (s1_btype == IROP_BTYPE_FLOAT32) || (s2_btype == IROP_BTYPE_FLOAT32) ||
          (s1_btype == IROP_BTYPE_FLOAT64) || (s2_btype == IROP_BTYPE_FLOAT64);
      if (wide_src2)
      {
        tmp_kb[dpos].gen = 0;
        continue;
      }
    }

    uint32_t a_kz = 0, a_ko = 0, b_kz = 0, b_ko = 0;
    int have_kb = 0;
    if (op == TCCIR_OP_ASSIGN || op == TCCIR_OP_ZEXT ||
        op == TCCIR_OP_LOAD)
    {
      int suppress_load_kb = 0;
      if (op == TCCIR_OP_LOAD &&
          kb_load_feeds_same_slot_store(ir, i, s1, dest, tmp_kb,
                                        max_tmp_pos, var_addr, max_var_pos,
                                        current_gen))
      {
        suppress_load_kb = 1;
        if (kb_operand(ir, s1, tmp_kb, max_tmp_pos, current_gen,
                       var_addr, max_var_pos,
                       stack_slots, n_stack_slots, &a_kz, &a_ko))
        {
          kb_apply_load_width(irop_get_btype(dest), dest.is_unsigned, &a_kz, &a_ko);
          suppress_load_kb = ((a_kz | a_ko) != 0xFFFFFFFFu);
        }
        a_kz = 0;
        a_ko = 0;
      }

      if (!suppress_load_kb)
      {
        have_kb = kb_operand(ir, s1, tmp_kb, max_tmp_pos, current_gen,
                             var_addr, max_var_pos,
                             stack_slots, n_stack_slots, &a_kz, &a_ko);
        if (have_kb && op == TCCIR_OP_LOAD)
          kb_apply_load_width(irop_get_btype(dest), dest.is_unsigned, &a_kz, &a_ko);
      }
    }
    else if (op == TCCIR_OP_AND || op == TCCIR_OP_OR || op == TCCIR_OP_XOR)
    {
      int h1 = kb_operand(ir, s1, tmp_kb, max_tmp_pos, current_gen,
                          var_addr, max_var_pos,
                          stack_slots, n_stack_slots, &a_kz, &a_ko);
      int h2 = kb_operand(ir, s2, tmp_kb, max_tmp_pos, current_gen,
                          var_addr, max_var_pos,
                          stack_slots, n_stack_slots, &b_kz, &b_ko);
      have_kb = h1 || h2;
      if (!h1) { a_kz = 0; a_ko = 0; }
      if (!h2) { b_kz = 0; b_ko = 0; }
    }
    else if (op == TCCIR_OP_ADD || op == TCCIR_OP_SUB)
    {
      /* Need BOTH operands fully tracked through their low bits — partial
       * knowledge of only one side gives no information about the sum.
       * kb_compute (above) walks LSB→MSB and stops at the first unknown
       * bit, so a missing operand maps to "everything unknown" and the
       * result has nothing to fold. */
      int h1 = kb_operand(ir, s1, tmp_kb, max_tmp_pos, current_gen,
                          var_addr, max_var_pos,
                          stack_slots, n_stack_slots, &a_kz, &a_ko);
      int h2 = kb_operand(ir, s2, tmp_kb, max_tmp_pos, current_gen,
                          var_addr, max_var_pos,
                          stack_slots, n_stack_slots, &b_kz, &b_ko);
      have_kb = h1 && h2;
      if (!h1) { a_kz = 0; a_ko = 0; }
      if (!h2) { b_kz = 0; b_ko = 0; }
    }
    else if (op == TCCIR_OP_SHL || op == TCCIR_OP_SHR || op == TCCIR_OP_SAR)
    {
      if (irop_is_immediate(s2) && !s2.is_sym && !s2.is_lval)
      {
        int64_t amt = irop_get_imm64_ex(ir, s2);
        if (amt >= 32 && dest_btype != IROP_BTYPE_INT64 &&
            (op == TCCIR_OP_SHL || op == TCCIR_OP_SHR))
        {
          /* 32-bit SHL/SHR by >= 32: result is always 0.
           * Replace with ASSIGN #0 directly. */
          IROperand imm = irop_make_imm32(-1, 0, dest_btype);
          imm.is_unsigned = dest.is_unsigned;
          q->op = TCCIR_OP_ASSIGN;
          tcc_ir_set_src1(ir, i, imm);
          tcc_ir_set_src2(ir, i, IROP_NONE);
          tmp_kb[dpos].gen = current_gen;
          tmp_kb[dpos].kz = 0xFFFFFFFFu;
          tmp_kb[dpos].ko = 0;
          tmp_kb[dpos].has_const = 0;
          tmp_kb[dpos].is_low32 = 0;
          changes++;
          continue;
        }
        else if (amt >= 0 && amt < 32)
        {
          int h1 = kb_operand(ir, s1, tmp_kb, max_tmp_pos, current_gen,
                              var_addr, max_var_pos,
                              stack_slots, n_stack_slots, &a_kz, &a_ko);
          if (!h1) { a_kz = 0; a_ko = 0; }
          have_kb = h1 || (amt > 0 && op != TCCIR_OP_SAR);
          b_ko = (uint32_t)amt;
          b_kz = ~b_ko;
        }
      }
    }

    if (have_kb)
    {
      uint32_t dkz, dko;
      if (kb_compute(op, a_kz, a_ko, b_kz, b_ko, &dkz, &dko))
      {
        /* Rewriting a sub-word LOAD whose value is "all-ones for the load
         * width" (0xFF for INT8, 0xFFFF for INT16) into an immediate ASSIGN
         * stamps the width-masked value as the literal — and downstream
         * vector / byte-array identity passes look for `T XOR #-1` rather
         * than `T XOR #255`.  Keep the LOAD shape in those cases so the
         * pattern matchers can still fold them; the kb is still recorded
         * for the CMP-source fold and other consumers. */
        int suppress_rewrite = 0;
        if (op == TCCIR_OP_LOAD && ((dkz | dko) == 0xFFFFFFFFu))
        {
          uint32_t low_mask = (dest_btype == IROP_BTYPE_INT8) ? 0xFFu :
                              (dest_btype == IROP_BTYPE_INT16) ? 0xFFFFu : 0;
          if (low_mask && (dko & low_mask) == low_mask)
            suppress_rewrite = 1;
        }
        if (((dkz | dko) == 0xFFFFFFFFu) && op != TCCIR_OP_ASSIGN &&
            !suppress_rewrite)
        {
          int32_t val = (int32_t)dko;
          IROperand imm = irop_make_imm32(-1, val, dest_btype);
          imm.is_unsigned = dest.is_unsigned;
          q->op = TCCIR_OP_ASSIGN;
          tcc_ir_set_src1(ir, i, imm);
          tcc_ir_set_src2(ir, i, IROP_NONE);
          LOG_IR_GEN(
              "OPTIMIZE: knownbits fold TMP:%d = #%d at i=%d (kz=%08x ko=%08x)",
              dpos, val, i, dkz, dko);
          tmp_kb[dpos].gen = current_gen;
          tmp_kb[dpos].kz = ~(uint32_t)val;
          tmp_kb[dpos].ko = (uint32_t)val;
          tmp_kb[dpos].const_val = (uint32_t)val;
          tmp_kb[dpos].has_const = 1;
          changes++;
          continue;
        }
        tmp_kb[dpos].gen = current_gen;
        tmp_kb[dpos].kz = dkz;
        tmp_kb[dpos].ko = dko;
        if (((dkz | dko) == 0xFFFFFFFFu) && suppress_rewrite)
        {
          tmp_kb[dpos].const_val = (uint32_t)dko;
          tmp_kb[dpos].has_const = 1;
        }
        else
        {
          tmp_kb[dpos].has_const = 0;
        }
        continue;
      }
    }

    /* No kb info for this dest. */
    tmp_kb[dpos].gen = 0;
    continue;

  post_op:;
  }

  tcc_free(tmp_kb);
  tcc_free(var_addr);
  tcc_free(block_start_seen);
  tcc_free(backedge_target);
  return changes;
}

int tcc_ir_opt_known_bits_ex(IROptCtx *ctx)
{
  return tcc_ir_opt_known_bits(ctx->ir);
}
