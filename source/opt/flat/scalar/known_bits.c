/*
 *  TCC IR - Known-Bits Propagation (flat DSL pass)
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
#include "opt_alias.h"
#include "opt_engine.h"
#include "opt_utils.h"
#include "opt_dsl.h"
#include "opt/flat/known_bits.h"

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

/* Resolve a base pointer operand (e.g. STORE_INDEXED base) to a concrete
 * stack-frame offset when it is a direct Addr[StackLoc] or a single-def
 * TEMP/VAR holding such an address. */
static int kb_base_stack_off(const TCCIRState *ir, IROperand base,
                             const TmpKB *tmp_kb, int max_tmp_pos,
                             const VregAddrKB *var_addr, int max_var_pos,
                             int current_gen, int32_t *out_off)
{
  if (kb_is_direct_stackoff(base, 0))
  {
    *out_off = (int32_t)irop_get_imm64_ex(ir, base);
    return 1;
  }
  return vreg_addr_lookup(irop_get_vreg(base), tmp_kb, max_tmp_pos, var_addr,
                          max_var_pos, current_gen, out_off);
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
    /* An immediate already stores its actual signed/unsigned VALUE in u.imm32
     * (a signed char -56 holds -56; an unsigned char 208 holds 208).  Applying
     * sub-word width extension would re-interpret the low byte as a bit pattern
     * and sign-extend it — corrupting an `unsigned char` 208 (0xd0) to -48 when
     * the immediate's is_unsigned flag was dropped upstream (combo seed 1053).
     * Read immediates raw; only memory loads model sub-word extension. */
    *out = (uint64_t)irop_get_imm64_ex(ir, *op);
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

static int kb_const_compute(TccIrOp op, int dest_btype, int src1_btype,
                            uint64_t a, uint64_t b, uint64_t *out)
{
  int width = (dest_btype == IROP_BTYPE_INT64) ? 64 : 32;
  uint64_t mask = (width == 64) ? ~0ULL : 0xFFFFFFFFULL;

  switch (op)
  {
  case TCCIR_OP_ASSIGN:
  case TCCIR_OP_LOAD:
    *out = a;
    break;
  case TCCIR_OP_ZEXT:
  {
    /* Zero-extend from the SOURCE width. kb_operand_const_u64 sign-extends a
     * signed source to 64 bits, so a verbatim copy would poison the high half
     * (e.g. ZEXT(#-326:I32) must give 0x00000000FFFFFEBA, not ...FFFFFEBA). */
    uint64_t src_mask;
    switch (src1_btype)
    {
    case IROP_BTYPE_INT8:  src_mask = 0xFFULL;       break;
    case IROP_BTYPE_INT16: src_mask = 0xFFFFULL;     break;
    case IROP_BTYPE_INT32: src_mask = 0xFFFFFFFFULL; break;
    default:               src_mask = ~0ULL;         break;
    }
    *out = a & src_mask;
    break;
  }
  case TCCIR_OP_ADD:
    *out = a + b;
    break;
  case TCCIR_OP_SUB:
    *out = a - b;
    break;
  case TCCIR_OP_MUL:
    /* Low `width` bits of a product depend only on the low bits of the
     * operands, so the sign/zero-extended inputs give the correct result once
     * masked to width below. */
    *out = a * b;
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
    /* Logical shift: mask the source to the operation width first so the
     * sign-extended high bits (for a 32-bit op) are not shifted in. */
    *out = (a & mask) >> b;
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

#define KB_MAX_STACK_SLOTS 64

/* ============================================================================
 * Stateful flat-DSL dispatch
 *
 * The legacy single forward scan (per-TEMP / per-stack-slot known-bits lattice)
 * is expressed here as opcode-triggered gens sharing a KBState lattice via
 * ctx->pass_state, driven by tcc_ir_opt_run_stateful_gens (first-matching-gen
 * owns the instruction; the wildcard kb_dest is registered last).  The lattice
 * math (kb_compute / kb_operand / stack_kb_* / kb_apply_*_width, above) is
 * unchanged; only the per-instruction dispatch is decomposed.
 * ==========================================================================*/

typedef struct KBState
{
  int n;
  int active;
  int max_tmp_pos;
  int max_var_pos;
  TmpKB *tmp_kb;
  VregAddrKB *var_addr;
  int *block_start_seen;
  int *backedge_target;
  StackKB stack_slots[KB_MAX_STACK_SLOTS];
  int n_stack_slots;
  int block_gen;
  int current_gen;
  int stack_addr_escaped;
  int stack_dirty_since_split;
} KBState;

static void *kb_begin(IROptCtx *ctx)
{
  TCCIRState *ir = ctx->ir;
  KBState *st = tcc_mallocz(sizeof(KBState));
  int n = ir->next_instruction_index;
  st->n = n;
  st->block_gen = 1;
  st->current_gen = 1;

  if (n == 0)
    return st;

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
  st->max_tmp_pos = max_tmp_pos;
  st->max_var_pos = max_var_pos;
  if (max_tmp_pos == 0)
    return st;

  st->active = 1;
  st->tmp_kb = tcc_mallocz(sizeof(TmpKB) * (max_tmp_pos + 1));
  st->var_addr = tcc_mallocz(sizeof(VregAddrKB) * (max_var_pos + 1));
  st->block_start_seen = tcc_mallocz(sizeof(int) * n);
  st->backedge_target = tcc_mallocz(sizeof(int) * n);

  ir_opt_mark_block_starts(ir, st->block_start_seen, st->block_gen, n);
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_JUMPIF)
    {
      int target = (int)irop_get_imm64_ex(ir, tcc_ir_op_get_dest(ir, q));
      if (target >= 0 && target <= i && target < n)
        st->backedge_target[target] = 1;
    }
  }
  return st;
}

/* Runs for every instruction index before the NOP-skip: BB-boundary lattice
 * reset, faithful to the legacy top-of-loop logic. */
static void kb_each_pre(IROptCtx *ctx, int i)
{
  KBState *st = ctx->pass_state;
  if (!st->active)
    return;
  if (i != 0 && st->block_start_seen[i] == st->block_gen)
  {
    int old_gen = st->current_gen;
    st->current_gen++;
    if (st->stack_dirty_since_split || st->backedge_target[i])
      stack_kb_invalidate_all(st->stack_slots, st->n_stack_slots);
    else
      stack_kb_rebase_gen(st->stack_slots, st->n_stack_slots, old_gen,
                          st->current_gen);
    st->stack_dirty_since_split = 0;
  }
}

static void kb_end(IROptCtx *ctx)
{
  KBState *st = ctx->pass_state;
  if (!st)
    return;
  tcc_free(st->tmp_kb);
  tcc_free(st->var_addr);
  tcc_free(st->block_start_seen);
  tcc_free(st->backedge_target);
  tcc_free(st);
}

/* ---- has-dest tail, split into phases -------------------------------------
 * Shared by the wildcard kb_dest gen and the CALL/barrier gens (which
 * invalidate first, then fall into the same dest handling exactly as the legacy
 * scan did).  Each phase returns non-zero when it fully consumes the dest (the
 * caller then returns), mirroring the original chain of early `return`s. */

/* VAR dest holding Addr[StackLoc] (or losing that fact on any other def). */
static void kb_dest_track_var_addr(KBState *st, TCCIRState *ir, int op,
                                   int32_t dvr, IROperand s1_raw)
{
  int vpos = TCCIR_DECODE_VREG_POSITION(dvr);
  if (vpos > st->max_var_pos)
    return;
  if ((op == TCCIR_OP_ASSIGN || op == TCCIR_OP_LEA) &&
      kb_is_direct_stackoff(s1_raw, 0))
  {
    st->var_addr[vpos].gen = st->current_gen;
    st->var_addr[vpos].has_stack_off = 1;
    st->var_addr[vpos].stack_off = (int32_t)irop_get_imm64_ex(ir, s1_raw);
  }
  else
  {
    st->var_addr[vpos].gen = 0;
    st->var_addr[vpos].has_stack_off = 0;
  }
}

/* TMP dest holding Addr[StackLoc]: direct LEA/ASSIGN, an ASSIGN copy of another
 * address temp, or an ADD/SUB of a constant onto such a temp.  Returns 1 when a
 * stack-offset fact was recorded (dest fully consumed). */
static int kb_dest_track_tmp_addr(KBState *st, TCCIRState *ir, int op, int dpos,
                                  IROperand s1_raw, IROperand s2_raw)
{
  if ((op == TCCIR_OP_ASSIGN || op == TCCIR_OP_LEA) &&
      kb_is_direct_stackoff(s1_raw, 0))
  {
    int32_t off = (int32_t)irop_get_imm64_ex(ir, s1_raw);
    st->tmp_kb[dpos].gen = st->current_gen;
    st->tmp_kb[dpos].kz = 0;
    st->tmp_kb[dpos].ko = 0;
    st->tmp_kb[dpos].has_stack_off = 1;
    st->tmp_kb[dpos].stack_off = off;
    st->tmp_kb[dpos].has_const = 0;
    return 1;
  }
  if (op == TCCIR_OP_ASSIGN)
  {
    int32_t off;
    if (vreg_addr_lookup(irop_get_vreg(s1_raw), st->tmp_kb, st->max_tmp_pos,
                         st->var_addr, st->max_var_pos, st->current_gen, &off))
    {
      st->tmp_kb[dpos].gen = st->current_gen;
      st->tmp_kb[dpos].kz = 0;
      st->tmp_kb[dpos].ko = 0;
      st->tmp_kb[dpos].has_stack_off = 1;
      st->tmp_kb[dpos].stack_off = off;
      st->tmp_kb[dpos].has_const = 0;
      return 1;
    }
  }
  if ((op == TCCIR_OP_ADD || op == TCCIR_OP_SUB) &&
      irop_is_immediate(s2_raw) && !s2_raw.is_sym && !s2_raw.is_lval)
  {
    int32_t off;
    if (vreg_addr_lookup(irop_get_vreg(s1_raw), st->tmp_kb, st->max_tmp_pos,
                         st->var_addr, st->max_var_pos, st->current_gen, &off))
    {
      int64_t delta = irop_get_imm64_ex(ir, s2_raw);
      if (op == TCCIR_OP_SUB)
        delta = -delta;
      int64_t new_off = (int64_t)off + delta;
      if (new_off >= INT32_MIN && new_off <= INT32_MAX)
      {
        st->tmp_kb[dpos].gen = st->current_gen;
        st->tmp_kb[dpos].kz = 0;
        st->tmp_kb[dpos].ko = 0;
        st->tmp_kb[dpos].has_stack_off = 1;
        st->tmp_kb[dpos].stack_off = (int32_t)new_off;
        st->tmp_kb[dpos].has_const = 0;
        return 1;
      }
    }
  }
  return 0;
}

/* Both operands fully known -> fold to an immediate ASSIGN (subject to the
 * load-shape / already-folded suppression rules).  Returns 1 when consumed. */
static int kb_dest_try_const_fold(KBState *st, TCCIRState *ir, int i, int op,
                                  int dpos, IROperand dest, int dest_btype,
                                  IROperand s1, IROperand s2, int s1_btype,
                                  int *changes)
{
  IRQuadCompact *q = &ir->compact_instructions[i];
  uint64_t cv1 = 0, cv2 = 0, cres = 0;
  int h1 = 0, h2 = 0;
  if (irop_config[op].has_src1)
    h1 = kb_operand_const_u64(ir, &s1, st->tmp_kb, st->max_tmp_pos,
                              st->current_gen, st->var_addr, st->max_var_pos,
                              st->stack_slots, st->n_stack_slots, &cv1);
  if (irop_config[op].has_src2)
    h2 = kb_operand_const_u64(ir, &s2, st->tmp_kb, st->max_tmp_pos,
                              st->current_gen, st->var_addr, st->max_var_pos,
                              st->stack_slots, st->n_stack_slots, &cv2);
  if (!(h1 && (!irop_config[op].has_src2 || h2) &&
        kb_const_compute(op, dest_btype, s1_btype, cv1, cv2, &cres)))
    return 0;

  IROperand imm = kb_make_const_operand(ir, cres, dest_btype);
  imm.is_unsigned = dest.is_unsigned;
  int already_folded = (op == TCCIR_OP_ASSIGN &&
                        irop_is_immediate(s1) && !s1.is_sym &&
                        !s1.is_lval &&
                        irop_get_imm64_ex(ir, s1) == (int64_t)cres);
  int suppress_rewrite = 0;
  if (op == TCCIR_OP_LOAD)
  {
    uint32_t low_mask = (dest_btype == IROP_BTYPE_INT8) ? 0xFFu :
                        (dest_btype == IROP_BTYPE_INT16) ? 0xFFFFu : 0;
    if (low_mask && ((uint32_t)cres & low_mask) == low_mask)
      suppress_rewrite = 1;
  }
  if (op == TCCIR_OP_ASSIGN)
    suppress_rewrite = 1;
  if (op != TCCIR_OP_LOAD &&
      ((irop_config[op].has_src1 && irop_op_is_lval(s1)) ||
       (irop_config[op].has_src2 && irop_op_is_lval(s2))))
    suppress_rewrite = 1;
  if (!already_folded && !suppress_rewrite)
  {
    q->op = TCCIR_OP_ASSIGN;
    tcc_ir_set_src1(ir, i, imm);
    tcc_ir_set_src2(ir, i, IROP_NONE);
    (*changes)++;
  }
  st->tmp_kb[dpos].gen = st->current_gen;
  st->tmp_kb[dpos].kz = ~(uint32_t)cres;
  st->tmp_kb[dpos].ko = (uint32_t)cres;
  st->tmp_kb[dpos].const_val = cres;
  st->tmp_kb[dpos].has_const = 1;
  return 1;
}

/* A 64-bit operand whose low 32 bits are known: either an immediate, or a TEMP
 * carrying the is_low32 fact.  Writes its kb to out_kz/out_ko and returns 1 when
 * found.  `op` is by pointer to keep the packed flags byte across the call (same
 * cross-compile hazard as kb_operand_const_u64). */
static int kb_low32_of(const KBState *st, TCCIRState *ir, const IROperand *op,
                       uint32_t *kz, uint32_t *ko)
{
  if (irop_is_immediate(*op) && !op->is_sym && !op->is_lval)
  {
    uint32_t v = (uint32_t)irop_get_imm64_ex(ir, *op);
    *kz = ~v; *ko = v;
    return 1;
  }
  if (irop_has_vreg(*op))
  {
    int32_t vr = irop_get_vreg(*op);
    if (vr >= 0 && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_TEMP)
    {
      int sp = TCCIR_DECODE_VREG_POSITION(vr);
      if (sp >= 0 && sp <= st->max_tmp_pos &&
          st->tmp_kb[sp].gen == st->current_gen && st->tmp_kb[sp].is_low32)
      {
        *kz = st->tmp_kb[sp].kz;
        *ko = st->tmp_kb[sp].ko;
        return 1;
      }
    }
  }
  return 0;
}

/* Wide (INT64 / FP / STRUCT) dest: track only the low 32 bits of 64-bit values
 * through SHL>=32 / ADD / SUB / ASSIGN / ZEXT chains so 32-bit consumers can
 * still fold; everything else clears the dest kb.  Always consumes. */
static int kb_dest_track_wide_dest(KBState *st, TCCIRState *ir, int op, int dpos,
                                   int dest_btype, IROperand s1, IROperand s2)
{
  if (dest_btype == IROP_BTYPE_INT64)
  {
    if (op == TCCIR_OP_SHL && irop_is_immediate(s2) && !s2.is_sym && !s2.is_lval)
    {
      int64_t amt = irop_get_imm64_ex(ir, s2);
      if (amt >= 32)
      {
        st->tmp_kb[dpos].gen = st->current_gen;
        st->tmp_kb[dpos].kz = 0xFFFFFFFFu;
        st->tmp_kb[dpos].ko = 0;
        st->tmp_kb[dpos].has_const = 0;
        st->tmp_kb[dpos].is_low32 = 1;
        return 0;
      }
    }
    if (op == TCCIR_OP_SUB || op == TCCIR_OP_ADD)
    {
      uint32_t a_kz64 = 0, a_ko64 = 0, b_kz64 = 0, b_ko64 = 0;
      int h1 = kb_low32_of(st, ir, &s1, &a_kz64, &a_ko64);
      int h2 = kb_low32_of(st, ir, &s2, &b_kz64, &b_ko64);
      if ((h1 || h2) && (h1 || (a_kz64 == 0 && a_ko64 == 0)) &&
          (h2 || (b_kz64 == 0 && b_ko64 == 0)))
      {
        uint32_t dkz64, dko64;
        if (kb_compute(op, a_kz64, a_ko64, b_kz64, b_ko64, &dkz64, &dko64))
        {
          st->tmp_kb[dpos].gen = st->current_gen;
          st->tmp_kb[dpos].kz = dkz64;
          st->tmp_kb[dpos].ko = dko64;
          st->tmp_kb[dpos].has_const = 0;
          st->tmp_kb[dpos].is_low32 = 1;
          return 0;
        }
      }
    }
    if (op == TCCIR_OP_ASSIGN || op == TCCIR_OP_ZEXT)
    {
      uint32_t kz, ko;
      if (irop_has_vreg(s1) && kb_low32_of(st, ir, &s1, &kz, &ko))
      {
        st->tmp_kb[dpos].gen = st->current_gen;
        st->tmp_kb[dpos].kz = kz;
        st->tmp_kb[dpos].ko = ko;
        st->tmp_kb[dpos].has_const = 0;
        st->tmp_kb[dpos].is_low32 = 1;
        return 0;
      }
    }
  }
  st->tmp_kb[dpos].gen = 0;
  return 0;
}

/* 32-bit dest with a 64-bit/FP source: narrow a 64-bit operand whose low 32
 * bits are fully known to that immediate, then continue into the scalar path.
 * Returns 1 when the dest is consumed (still-wide source, no folding possible);
 * 0 to fall through with (possibly narrowed) operands. */
static int kb_dest_narrow_wide_src(KBState *st, TCCIRState *ir, int i, int op,
                                   int dpos, int dest_btype, IROperand *s1,
                                   IROperand *s2, int *s1_btype, int *s2_btype,
                                   int *changes)
{
  IRQuadCompact *q = &ir->compact_instructions[i];
  int wide_src =
      (*s1_btype == IROP_BTYPE_INT64) || (*s2_btype == IROP_BTYPE_INT64) ||
      (*s1_btype == IROP_BTYPE_FLOAT32) || (*s2_btype == IROP_BTYPE_FLOAT32) ||
      (*s1_btype == IROP_BTYPE_FLOAT64) || (*s2_btype == IROP_BTYPE_FLOAT64);
  if (!wide_src)
    return 0;

  if ((op == TCCIR_OP_SHL || op == TCCIR_OP_SHR || op == TCCIR_OP_SAR ||
       op == TCCIR_OP_AND || op == TCCIR_OP_SUB || op == TCCIR_OP_ADD ||
       op == TCCIR_OP_ASSIGN || op == TCCIR_OP_ZEXT) &&
      dest_btype != IROP_BTYPE_INT64)
  {
    int which = 0;
    int32_t low32_val = 0;
    for (int side = 1; side <= 2; side++)
    {
      IROperand sN = (side == 1) ? *s1 : *s2;
      int sN_btype = (side == 1) ? *s1_btype : *s2_btype;
      if (sN_btype != IROP_BTYPE_INT64) continue;
      if (!irop_has_vreg(sN)) continue;
      int32_t vr = irop_get_vreg(sN);
      if (vr < 0) continue;
      if (TCCIR_DECODE_VREG_TYPE(vr) != TCCIR_VREG_TYPE_TEMP) continue;
      int sp = TCCIR_DECODE_VREG_POSITION(vr);
      if (sp < 0 || sp > st->max_tmp_pos) continue;
      if (st->tmp_kb[sp].gen != st->current_gen || !st->tmp_kb[sp].is_low32) continue;
      if ((st->tmp_kb[sp].kz | st->tmp_kb[sp].ko) != 0xFFFFFFFFu) continue;
      which = side;
      low32_val = (int32_t)st->tmp_kb[sp].ko;
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
      (*changes)++;
      *s1 = tcc_ir_op_get_src1(ir, q);
      *s2 = tcc_ir_op_get_src2(ir, q);
      *s1_btype = irop_get_btype(*s1);
      *s2_btype = irop_get_btype(*s2);
      int still_wide =
          (*s1_btype == IROP_BTYPE_INT64) || (*s2_btype == IROP_BTYPE_INT64) ||
          (*s1_btype == IROP_BTYPE_FLOAT32) || (*s2_btype == IROP_BTYPE_FLOAT32) ||
          (*s1_btype == IROP_BTYPE_FLOAT64) || (*s2_btype == IROP_BTYPE_FLOAT64);
      if (still_wide)
      {
        st->tmp_kb[dpos].gen = 0;
        return 1;
      }
      return 0;
    }
  }
  st->tmp_kb[dpos].gen = 0;
  return 1;
}

/* Scalar (<=32-bit) path: gather operand kb per opcode, run kb_compute, and fold
 * to an immediate ASSIGN when the whole dest becomes known.  Always consumes;
 * returns the number of rewrites made. */
static int kb_dest_scalar_compute(KBState *st, TCCIRState *ir, int i, int op,
                                  int dpos, IROperand dest, int dest_btype,
                                  IROperand s1, IROperand s2)
{
  IRQuadCompact *q = &ir->compact_instructions[i];
  int changes = 0;
  uint32_t a_kz = 0, a_ko = 0, b_kz = 0, b_ko = 0;
  int have_kb = 0;

  if (op == TCCIR_OP_ASSIGN || op == TCCIR_OP_ZEXT ||
      op == TCCIR_OP_LOAD)
  {
    int suppress_load_kb = 0;
    if (op == TCCIR_OP_LOAD &&
        kb_load_feeds_same_slot_store(ir, i, s1, dest, st->tmp_kb,
                                      st->max_tmp_pos, st->var_addr, st->max_var_pos,
                                      st->current_gen))
    {
      suppress_load_kb = 1;
      if (kb_operand(ir, s1, st->tmp_kb, st->max_tmp_pos, st->current_gen,
                     st->var_addr, st->max_var_pos,
                     st->stack_slots, st->n_stack_slots, &a_kz, &a_ko))
      {
        kb_apply_load_width(irop_get_btype(dest), dest.is_unsigned, &a_kz, &a_ko);
        suppress_load_kb = ((a_kz | a_ko) != 0xFFFFFFFFu);
      }
      a_kz = 0;
      a_ko = 0;
    }

    if (!suppress_load_kb)
    {
      have_kb = kb_operand(ir, s1, st->tmp_kb, st->max_tmp_pos, st->current_gen,
                           st->var_addr, st->max_var_pos,
                           st->stack_slots, st->n_stack_slots, &a_kz, &a_ko);
      if (have_kb && op == TCCIR_OP_LOAD)
        kb_apply_load_width(irop_get_btype(dest), dest.is_unsigned, &a_kz, &a_ko);
    }
  }
  else if (op == TCCIR_OP_AND || op == TCCIR_OP_OR || op == TCCIR_OP_XOR)
  {
    int h1 = kb_operand(ir, s1, st->tmp_kb, st->max_tmp_pos, st->current_gen,
                        st->var_addr, st->max_var_pos,
                        st->stack_slots, st->n_stack_slots, &a_kz, &a_ko);
    int h2 = kb_operand(ir, s2, st->tmp_kb, st->max_tmp_pos, st->current_gen,
                        st->var_addr, st->max_var_pos,
                        st->stack_slots, st->n_stack_slots, &b_kz, &b_ko);
    have_kb = h1 || h2;
    if (!h1) { a_kz = 0; a_ko = 0; }
    if (!h2) { b_kz = 0; b_ko = 0; }
  }
  else if (op == TCCIR_OP_ADD || op == TCCIR_OP_SUB)
  {
    int h1 = kb_operand(ir, s1, st->tmp_kb, st->max_tmp_pos, st->current_gen,
                        st->var_addr, st->max_var_pos,
                        st->stack_slots, st->n_stack_slots, &a_kz, &a_ko);
    int h2 = kb_operand(ir, s2, st->tmp_kb, st->max_tmp_pos, st->current_gen,
                        st->var_addr, st->max_var_pos,
                        st->stack_slots, st->n_stack_slots, &b_kz, &b_ko);
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
        IROperand imm = irop_make_imm32(-1, 0, dest_btype);
        imm.is_unsigned = dest.is_unsigned;
        q->op = TCCIR_OP_ASSIGN;
        tcc_ir_set_src1(ir, i, imm);
        tcc_ir_set_src2(ir, i, IROP_NONE);
        st->tmp_kb[dpos].gen = st->current_gen;
        st->tmp_kb[dpos].kz = 0xFFFFFFFFu;
        st->tmp_kb[dpos].ko = 0;
        st->tmp_kb[dpos].has_const = 0;
        st->tmp_kb[dpos].is_low32 = 0;
        changes++;
        return changes;
      }
      else if (amt >= 0 && amt < 32)
      {
        int h1 = kb_operand(ir, s1, st->tmp_kb, st->max_tmp_pos, st->current_gen,
                            st->var_addr, st->max_var_pos,
                            st->stack_slots, st->n_stack_slots, &a_kz, &a_ko);
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
        changes++;
        st->tmp_kb[dpos].gen = st->current_gen;
        st->tmp_kb[dpos].kz = ~(uint32_t)val;
        st->tmp_kb[dpos].ko = (uint32_t)val;
        st->tmp_kb[dpos].const_val = (uint32_t)val;
        st->tmp_kb[dpos].has_const = 1;
        return changes;
      }
      st->tmp_kb[dpos].gen = st->current_gen;
      st->tmp_kb[dpos].kz = dkz;
      st->tmp_kb[dpos].ko = dko;
      if (((dkz | dko) == 0xFFFFFFFFu) && suppress_rewrite)
      {
        st->tmp_kb[dpos].const_val = (uint32_t)dko;
        st->tmp_kb[dpos].has_const = 1;
      }
      else
      {
        st->tmp_kb[dpos].has_const = 0;
      }
      return changes;
    }
  }

  st->tmp_kb[dpos].gen = 0;
  return changes;
}

static int kb_handle_dest(KBState *st, TCCIRState *ir, int i)
{
  IRQuadCompact *q = &ir->compact_instructions[i];
  int op = q->op;
  if (!irop_config[op].has_dest)
    return 0;

  IROperand dest = tcc_ir_op_get_dest(ir, q);
  int32_t dvr = irop_get_vreg(dest);
  IROperand s1_raw = irop_config[op].has_src1 ? tcc_ir_op_get_src1(ir, q) : IROP_NONE;
  IROperand s2_raw = irop_config[op].has_src2 ? tcc_ir_op_get_src2(ir, q) : IROP_NONE;
  int dest_is_tmp =
      (dvr >= 0) && (TCCIR_DECODE_VREG_TYPE(dvr) == TCCIR_VREG_TYPE_TEMP);
  int dest_is_var =
      (dvr >= 0) && (TCCIR_DECODE_VREG_TYPE(dvr) == TCCIR_VREG_TYPE_VAR);
  int dest_is_lval = dest.is_lval;

  if (!dest_is_lval && dest_is_var)
    kb_dest_track_var_addr(st, ir, op, dvr, s1_raw);

  if (!dest_is_tmp || dest_is_lval)
    return 0;
  int dpos = TCCIR_DECODE_VREG_POSITION(dvr);
  int dest_btype = irop_get_btype(dest);

  if (kb_dest_track_tmp_addr(st, ir, op, dpos, s1_raw, s2_raw))
    return 0;

  st->tmp_kb[dpos].has_stack_off = 0;
  st->tmp_kb[dpos].has_const = 0;

  IROperand s1 = s1_raw;
  IROperand s2 = s2_raw;
  int s1_btype = irop_get_btype(s1);
  int s2_btype = irop_get_btype(s2);
  int changes = 0;

  if (kb_dest_try_const_fold(st, ir, i, op, dpos, dest, dest_btype, s1, s2,
                             s1_btype, &changes))
    return changes;

  if (dest_btype == IROP_BTYPE_INT64 ||
      dest_btype == IROP_BTYPE_FLOAT32 ||
      dest_btype == IROP_BTYPE_FLOAT64 ||
      dest_btype == IROP_BTYPE_STRUCT)
    return kb_dest_track_wide_dest(st, ir, op, dpos, dest_btype, s1, s2);

  if (kb_dest_narrow_wide_src(st, ir, i, op, dpos, dest_btype, &s1, &s2,
                              &s1_btype, &s2_btype, &changes))
    return changes;

  return changes + kb_dest_scalar_compute(st, ir, i, op, dpos, dest, dest_btype,
                                          s1, s2);
}

/* ---- gen helpers ---------------------------------------------------------- */

/* Index of the first non-NOP instruction at or after `from` (n if none). */
static int kb_next_nonnop(TCCIRState *ir, int from, int n)
{
  while (from < n && ir->compact_instructions[from].op == TCCIR_OP_NOP)
    from++;
  return from;
}

/* Invalidate every tracked stack slot whose 4-byte footprint overlaps
 * [off, off+width); when exclude_exact, the slot at exactly `off` is kept
 * (a store that rewrites that slot's own kb separately). */
static void kb_invalidate_overlapping_slots(KBState *st, int32_t off, int width,
                                            int exclude_exact)
{
  for (int s = 0; s < st->n_stack_slots; s++)
    if ((!exclude_exact || st->stack_slots[s].off != off) &&
        st->stack_slots[s].off < off + width &&
        st->stack_slots[s].off + 4 > off)
      st->stack_slots[s].gen = 0;
}

/* After a fallen-through TEST_ZERO of a provably-nonzero value, fold an
 * immediately-following SETIF that reads the same NE/EQ flag to its constant. */
static void kb_fold_fallthrough_setif(TCCIRState *ir, int k, int n)
{
  k = kb_next_nonnop(ir, k, n);
  if (k >= n)
    return;
  IRQuadCompact *setif_q = &ir->compact_instructions[k];
  if (setif_q->op != TCCIR_OP_SETIF || setif_q->is_jump_target)
    return;
  int setif_tok = (int)irop_get_imm64_ex(ir, tcc_ir_op_get_src1(ir, setif_q));
  int setif_result = (setif_tok == 0x95) ? 1 : (setif_tok == 0x94) ? 0 : -1;
  if (setif_result < 0)
    return;
  IROperand dest = tcc_ir_op_get_dest(ir, setif_q);
  IROperand imm = irop_make_imm32(-1, setif_result, irop_get_btype(dest));
  setif_q->op = TCCIR_OP_ASSIGN;
  tcc_ir_set_src1(ir, k, imm);
  tcc_ir_set_src2(ir, k, IROP_NONE);
}

OPT_GEN_FLAT(kb_store, TCCIR_OP_STORE)
{
  KBState *st = ctx->pass_state;
  if (!st->active)
    return 0;
  TCCIRState *ir = ctx->ir;
  IRQuadCompact *q = &ir->compact_instructions[i];
  int changes = 0;

  IROperand dest = tcc_ir_op_get_dest(ir, q);
  IROperand src1 = tcc_ir_op_get_src1(ir, q);
  int dest_btype = irop_get_btype(dest);

  if (kb_value_is_stack_addr(ir, src1, st->tmp_kb, st->max_tmp_pos,
                             st->var_addr, st->max_var_pos, st->current_gen))
    st->stack_addr_escaped = 1;
  st->stack_dirty_since_split = 1;

  int32_t stack_off = INT32_MIN;
  int have_off = 0;

  have_off = kb_lval_stack_off(ir, dest, st->tmp_kb, st->max_tmp_pos, st->var_addr,
                               st->max_var_pos, st->current_gen, &stack_off);

  if (dest_btype == IROP_BTYPE_INT64 || dest_btype == IROP_BTYPE_FLOAT32 ||
      dest_btype == IROP_BTYPE_FLOAT64 || dest_btype == IROP_BTYPE_STRUCT)
  {
    if (have_off && dest_btype != IROP_BTYPE_STRUCT)
      kb_invalidate_overlapping_slots(
          st, stack_off, (dest_btype == IROP_BTYPE_FLOAT32) ? 4 : 8, 0);
    else
      stack_kb_invalidate_all(st->stack_slots, st->n_stack_slots);
    return changes;
  }

  if (have_off)
  {
    int width = ir_opt_store_btype_size_bytes(dest_btype);
    if (width <= 0)
      width = 4;
    kb_invalidate_overlapping_slots(st, stack_off, width, 1);

    uint32_t kz, ko;
    if (kb_operand(ir, src1, st->tmp_kb, st->max_tmp_pos, st->current_gen,
                   st->var_addr, st->max_var_pos,
                   st->stack_slots, st->n_stack_slots, &kz, &ko))
    {
      kb_apply_store_width(dest_btype, &kz, &ko);
      stack_kb_set(st->stack_slots, &st->n_stack_slots, KB_MAX_STACK_SLOTS,
                   st->current_gen, stack_off, kz, ko);
    }
    else
    {
      for (int s = 0; s < st->n_stack_slots; s++)
        if (st->stack_slots[s].off == stack_off)
          st->stack_slots[s].gen = 0;
    }
  }
  else
  {
    stack_kb_invalidate_all(st->stack_slots, st->n_stack_slots);
  }
  return changes;
}

OPT_GEN_FLAT(kb_store_indexed, TCCIR_OP_STORE_INDEXED)
{
  KBState *st = ctx->pass_state;
  if (!st->active)
    return 0;
  TCCIRState *ir = ctx->ir;
  IRQuadCompact *q = &ir->compact_instructions[i];
  int changes = 0;

  IROperand base = tcc_ir_op_get_dest(ir, q);
  IROperand idx  = tcc_ir_op_get_src2(ir, q);
  IROperand sc   = tcc_ir_op_get_scale(ir, q);
  int32_t base_off;

  if (kb_base_stack_off(ir, base, st->tmp_kb, st->max_tmp_pos, st->var_addr,
                        st->max_var_pos, st->current_gen, &base_off) &&
      irop_is_immediate(idx) && !idx.is_sym &&
      irop_is_immediate(sc) && !sc.is_sym)
  {
    int shift = (int)irop_get_imm64_ex(ir, sc) & 3;
    int32_t off = base_off + ((int32_t)irop_get_imm64_ex(ir, idx) << shift);
    IROperand val = tcc_ir_op_get_src1(ir, q);
    int width = ir_opt_store_btype_size_bytes(irop_get_btype(val));
    if (width <= 0)
      width = 4;
    kb_invalidate_overlapping_slots(st, off, width, 0);
  }
  else
  {
    stack_kb_invalidate_all(st->stack_slots, st->n_stack_slots);
  }
  st->stack_dirty_since_split = 1;
  return changes;
}

OPT_GEN_FLAT(kb_call, TCCIR_OP_FUNCCALLVAL)
{
  KBState *st = ctx->pass_state;
  if (!st->active)
    return 0;
  TCCIRState *ir = ctx->ir;

  if (kb_call_exposes_stack_addr(ir, i, st->tmp_kb, st->max_tmp_pos,
                                 st->var_addr, st->max_var_pos, st->current_gen))
    st->stack_addr_escaped = 1;
  if (st->stack_addr_escaped)
  {
    stack_kb_invalidate_all(st->stack_slots, st->n_stack_slots);
    st->stack_dirty_since_split = 1;
  }
  return kb_handle_dest(st, ir, i);
}

OPT_GEN_FLAT(kb_barrier, TCCIR_OP_IJUMP)
{
  KBState *st = ctx->pass_state;
  if (!st->active)
    return 0;
  TCCIRState *ir = ctx->ir;
  int op = ir->compact_instructions[i].op;

  /* SET_CHAIN hands the frame pointer to a nested-function call, so the callee
   * can mutate any local through the static chain: mark the address escaped so
   * the next CALL is treated as fully aliasing. */
  if (op == TCCIR_OP_SET_CHAIN)
    st->stack_addr_escaped = 1;
  stack_kb_invalidate_all(st->stack_slots, st->n_stack_slots);
  st->stack_dirty_since_split = 1;
  return kb_handle_dest(st, ir, i);
}

OPT_GEN_FLAT(kb_test_zero, TCCIR_OP_TEST_ZERO)
{
  KBState *st = ctx->pass_state;
  if (!st->active)
    return 0;
  TCCIRState *ir = ctx->ir;
  IRQuadCompact *q = &ir->compact_instructions[i];
  int n = st->n;

  IROperand src1 = tcc_ir_op_get_src1(ir, q);
  uint32_t kz, ko;
  if (!(kb_operand(ir, src1, st->tmp_kb, st->max_tmp_pos, st->current_gen,
                   st->var_addr, st->max_var_pos,
                   st->stack_slots, st->n_stack_slots, &kz, &ko) &&
        ko != 0))
    return 0;

  int j = kb_next_nonnop(ir, i + 1, n);
  if (!(j < n && ir->compact_instructions[j].op == TCCIR_OP_JUMPIF &&
        !ir->compact_instructions[j].is_jump_target))
    return 0;

  IRQuadCompact *jq = &ir->compact_instructions[j];
  int tok = (int)irop_get_imm64_ex(ir, tcc_ir_op_get_src1(ir, jq));
  int branch_taken;
  if (tok == 0x94)
    branch_taken = 0;
  else if (tok == 0x95)
    branch_taken = 1;
  else
    return 0;

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
    kb_fold_fallthrough_setif(ir, j + 1, n);
  }
  LOG_IR_GEN("OPTIMIZE: knownbits TEST_ZERO fold at i=%d "
             "(ko=%08x, tok=0x%x -> %s)",
             i, ko, tok, branch_taken ? "JUMP" : "NOP");
  return 1;
}

/* Fold CMP operands that are fully-known deref/stack lvals to their immediates,
 * so downstream branch folding can consume the compare. */
static int kb_cmp_fold_lval_operands(KBState *st, TCCIRState *ir, int i,
                                     IRQuadCompact *q)
{
  int changes = 0;
  for (int si = 0; si < 2; si++)
  {
    if (si == 0 && !irop_config[TCCIR_OP_CMP].has_src1)
      continue;
    if (si == 1 && !irop_config[TCCIR_OP_CMP].has_src2)
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
    if (!kb_operand_const_u64(ir, &sop, st->tmp_kb, st->max_tmp_pos, st->current_gen,
                              st->var_addr, st->max_var_pos,
                              st->stack_slots, st->n_stack_slots, &cv))
      continue;
    IROperand imm = kb_make_const_operand(ir, cv, sop_btype);
    imm.is_unsigned = sop.is_unsigned;
    if (si == 0)
      tcc_ir_set_src1(ir, i, imm);
    else
      tcc_ir_set_src2(ir, i, imm);
    changes++;
  }
  return changes;
}

/* Decide a signed `CMP src1, #0` + JUMPIF `tok` purely from src1's known sign:
 * returns 1 (branch always taken), 0 (never taken), -1 (undecidable). */
static int kb_cmp_sign_branch_taken(int sign_one, int sign_zero, int nonzero,
                                    int tok)
{
  if (sign_one)
  {
    if (tok == 0x9c || tok == 0x9e) return 1;
    if (tok == 0x9d || tok == 0x9f) return 0;
    if (tok == 0x94) return 0;
    if (tok == 0x95) return 1;
  }
  else if (sign_zero && nonzero)
  {
    if (tok == 0x9c || tok == 0x9e) return 0;
    if (tok == 0x9d || tok == 0x9f) return 1;
    if (tok == 0x94) return 0;
    if (tok == 0x95) return 1;
  }
  else if (sign_zero)
  {
    if (tok == 0x9c) return 0;
    if (tok == 0x9d) return 1;
  }
  return -1;
}

/* CMP src1, #0 + JUMPIF: fold the branch when src1's sign bit is known.
 * Returns 1 when folded. */
static int kb_cmp_fold_sign_branch(KBState *st, TCCIRState *ir, int i,
                                   IRQuadCompact *q)
{
  int n = st->n;
  IROperand cmp_src1 = tcc_ir_op_get_src1(ir, q);
  IROperand cmp_src2 = tcc_ir_op_get_src2(ir, q);
  uint32_t kz, ko;
  if (!(irop_get_btype(cmp_src1) == IROP_BTYPE_INT32 &&
        irop_get_btype(cmp_src2) == IROP_BTYPE_INT32 &&
        irop_is_immediate(cmp_src2) && !cmp_src2.is_sym &&
        irop_get_imm64_ex(ir, cmp_src2) == 0 &&
        kb_operand(ir, cmp_src1, st->tmp_kb, st->max_tmp_pos, st->current_gen,
                   st->var_addr, st->max_var_pos,
                   st->stack_slots, st->n_stack_slots, &kz, &ko)))
    return 0;

  int j = kb_next_nonnop(ir, i + 1, n);
  if (!(j < n && ir->compact_instructions[j].op == TCCIR_OP_JUMPIF &&
        !ir->compact_instructions[j].is_jump_target))
    return 0;

  IRQuadCompact *jq = &ir->compact_instructions[j];
  int tok = (int)irop_get_imm64_ex(ir, tcc_ir_op_get_src1(ir, jq));
  int branch_taken = kb_cmp_sign_branch_taken((ko >> 31) & 1u, (kz >> 31) & 1u,
                                              ko != 0, tok);
  if (branch_taken < 0)
    return 0;
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
  return 1;
}

/* CMP x, #C (C != 0) + JUMPIF EQ/NE: when a known bit of x contradicts C
 * (a known-1 where C is 0, or a known-0 where C is 1), x == C is impossible, so
 * the equality branch is statically decided.  EQ/NE are sign-agnostic, so this
 * needs no range/sign reasoning.  Returns 1 when folded. */
static int kb_cmp_fold_const_eq_neq(KBState *st, TCCIRState *ir, int i,
                                    IRQuadCompact *q)
{
  int n = st->n;
  IROperand cmp_src1 = tcc_ir_op_get_src1(ir, q);
  IROperand cmp_src2 = tcc_ir_op_get_src2(ir, q);
  uint32_t kz, ko;
  if (!(irop_get_btype(cmp_src1) == IROP_BTYPE_INT32 &&
        irop_get_btype(cmp_src2) == IROP_BTYPE_INT32 &&
        irop_is_immediate(cmp_src2) && !cmp_src2.is_sym &&
        kb_operand(ir, cmp_src1, st->tmp_kb, st->max_tmp_pos, st->current_gen,
                   st->var_addr, st->max_var_pos,
                   st->stack_slots, st->n_stack_slots, &kz, &ko)))
    return 0;
  uint32_t c = (uint32_t)irop_get_imm64_ex(ir, cmp_src2);
  if (c == 0)
    return 0; /* zero handled by the sign fold */
  if (!((ko & ~c) != 0 || (kz & c) != 0))
    return 0; /* no contradicting known bit -> equality undecidable */

  int j = kb_next_nonnop(ir, i + 1, n);
  if (!(j < n && ir->compact_instructions[j].op == TCCIR_OP_JUMPIF &&
        !ir->compact_instructions[j].is_jump_target))
    return 0;
  IRQuadCompact *jq = &ir->compact_instructions[j];
  int tok = (int)irop_get_imm64_ex(ir, tcc_ir_op_get_src1(ir, jq));
  int branch_taken;
  if (tok == 0x94)       /* EQ: x == C is impossible -> never taken */
    branch_taken = 0;
  else if (tok == 0x95)  /* NE: always */
    branch_taken = 1;
  else
    return 0;
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
  LOG_IR_GEN("OPTIMIZE: knownbits CMP #%x EQ/NE fold at i=%d "
             "(kz=%08x ko=%08x -> %s)",
             c, i, kz, ko, branch_taken ? "JUMP" : "NOP");
  return 1;
}

OPT_GEN_FLAT(kb_cmp, TCCIR_OP_CMP)
{
  KBState *st = ctx->pass_state;
  if (!st->active)
    return 0;
  TCCIRState *ir = ctx->ir;
  IRQuadCompact *q = &ir->compact_instructions[i];
  int changes = kb_cmp_fold_lval_operands(st, ir, i, q);
  int folded = kb_cmp_fold_sign_branch(st, ir, i, q);
  if (!folded)
    folded = kb_cmp_fold_const_eq_neq(st, ir, i, q);
  return changes + folded;
}

OPT_GEN_FLAT(kb_dest, -1)
{
  KBState *st = ctx->pass_state;
  if (!st->active)
    return 0;
  return kb_handle_dest(st, ctx->ir, i);
}

static const IROptStatefulOps kb_ops = {
    kb_begin,
    kb_each_pre,
    kb_end,
};

const IROptGen known_bits_gens[] = {
    OPT_GEN_ENTRY_FLAT(kb_store, TCCIR_OP_STORE),
    OPT_GEN_ENTRY_FLAT(kb_store_indexed, TCCIR_OP_STORE_INDEXED),
    OPT_GEN_ENTRY_FLAT(kb_store_indexed, TCCIR_OP_STORE_POSTINC),
    OPT_GEN_ENTRY_FLAT(kb_call, TCCIR_OP_FUNCCALLVOID),
    OPT_GEN_ENTRY_FLAT(kb_call, TCCIR_OP_FUNCCALLVAL),
    OPT_GEN_ENTRY_FLAT(kb_barrier, TCCIR_OP_IJUMP),
    OPT_GEN_ENTRY_FLAT(kb_barrier, TCCIR_OP_INLINE_ASM),
    OPT_GEN_ENTRY_FLAT(kb_barrier, TCCIR_OP_SETJMP),
    OPT_GEN_ENTRY_FLAT(kb_barrier, TCCIR_OP_LONGJMP),
    OPT_GEN_ENTRY_FLAT(kb_barrier, TCCIR_OP_VLA_ALLOC),
    OPT_GEN_ENTRY_FLAT(kb_barrier, TCCIR_OP_SET_CHAIN),
    OPT_GEN_ENTRY_FLAT(kb_test_zero, TCCIR_OP_TEST_ZERO),
    OPT_GEN_ENTRY_FLAT(kb_cmp, TCCIR_OP_CMP),
    OPT_GEN_ENTRY_FLAT(kb_dest, -1),
};

const int known_bits_gens_count =
    (int)(sizeof(known_bits_gens) / sizeof(known_bits_gens[0]));

int tcc_ir_opt_known_bits_ex(IROptCtx *ctx)
{
  return tcc_ir_opt_run_stateful_gens(ctx, known_bits_gens,
                                      known_bits_gens_count, &kb_ops);
}

static int tcc_ir_opt_known_bits__run(TCCIRState *ir)
{
  IROptCtx ctx;
  tcc_ir_opt_ctx_init(&ctx, ir);
  int changes = tcc_ir_opt_known_bits_ex(&ctx);
  tcc_ir_opt_ctx_free(&ctx);
  return changes;
}

int tcc_ir_opt_known_bits(TCCIRState *ir)
{
  if (tcc_ir_opt_pass_disabled("known_bits")) return 0;
  tcc_pass_timing_init();
  if (!tcc_pass_timing_on) return tcc_ir_opt_known_bits__run(ir);
  unsigned long _t = tcc_pass_clk_us();
  int _r = tcc_ir_opt_known_bits__run(ir);
  tcc_pass_timing_add("known_bits", tcc_pass_clk_us() - _t);
  return _r;
}
