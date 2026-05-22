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
 * invalidated at jump targets and after CALL/IJUMP).
 */

#define USING_GLOBALS

#include "ir.h"
#include "opt_engine.h"
#include "opt_utils.h"

typedef struct
{
  int gen;        /* matches current_gen when entry is valid */
  uint32_t kz;    /* known-zero mask */
  uint32_t ko;    /* known-one mask */
  int32_t stack_off; /* if >= INT32_MIN+1, temp holds Addr[StackLoc[off]] */
  int has_stack_off;
} TmpKB;

typedef struct
{
  int32_t off;    /* stack offset (signed; negative for locals) */
  int gen;
  uint32_t kz;
  uint32_t ko;
} StackKB;

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

/* Get known bits for an operand. Returns 1 if any bit is known. */
static int kb_operand(const TCCIRState *ir, IROperand op,
                      const TmpKB *tmp_kb, int max_tmp_pos, int current_gen,
                      const StackKB *slots, int n_slots,
                      uint32_t *out_kz, uint32_t *out_ko)
{
  *out_kz = 0;
  *out_ko = 0;

  /* Direct StackLoc[X] lval — reading the slot's current value.
   * Must have no vreg (vreg_type==0): a VAR/TEMP/PARAM with STACKOFF tag
   * is a vreg-backed pseudoreg whose "stack offset" is a potential spill
   * slot, not a real direct stack reference. */
  if (op.is_local && op.is_lval && op.tag == IROP_TAG_STACKOFF &&
      op.vreg_type == 0 &&
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
    int32_t vr = irop_get_vreg(op);
    if (vr >= 0 && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_TEMP)
    {
      int pos = TCCIR_DECODE_VREG_POSITION(vr);
      if (pos <= max_tmp_pos && tmp_kb[pos].gen == current_gen &&
          tmp_kb[pos].has_stack_off)
      {
        return stack_kb_lookup(slots, n_slots, current_gen,
                               tmp_kb[pos].stack_off, out_kz, out_ko);
      }
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

int tcc_ir_opt_known_bits(TCCIRState *ir)
{
  int n = ir->next_instruction_index;

  if (n == 0)
    return 0;

  int max_tmp_pos = 0;
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP || !irop_config[q->op].has_dest)
      continue;
    IROperand dest = tcc_ir_op_get_dest(ir, q);
    int32_t vr = irop_get_vreg(dest);
    if (TCCIR_DECODE_VREG_TYPE(vr) != TCCIR_VREG_TYPE_TEMP)
      continue;
    int pos = TCCIR_DECODE_VREG_POSITION(vr);
    if (pos > max_tmp_pos)
      max_tmp_pos = pos;
  }
  if (max_tmp_pos == 0)
    return 0;

  size_t kb_bytes = sizeof(TmpKB) * (max_tmp_pos + 1);
  size_t bs_bytes = sizeof(int) * n;
  TmpKB *tmp_kb = tcc_mallocz(kb_bytes);
  int *block_start_seen = tcc_mallocz(bs_bytes);
  StackKB stack_slots[KB_MAX_STACK_SLOTS];
  int n_stack_slots = 0;
  int block_gen = 1;
  int current_gen = 1;
  int changes = 0;

  ir_opt_mark_block_starts(ir, block_start_seen, block_gen, n);

  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];

    /* BB boundary: invalidate temps and stack slots. */
    if (i != 0 && block_start_seen[i] == block_gen)
    {
      current_gen++;
      stack_kb_invalidate_all(stack_slots, n_stack_slots);
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

      /* Only handle 32-bit/narrower stores; skip wide types. */
      if (dest_btype == IROP_BTYPE_INT64 || dest_btype == IROP_BTYPE_FLOAT32 ||
          dest_btype == IROP_BTYPE_FLOAT64 || dest_btype == IROP_BTYPE_STRUCT)
        goto post_op;

      int32_t stack_off = INT32_MIN;
      int have_off = 0;

      /* Direct StackLoc[X] dest — must have no vreg attached (otherwise
       * the operand is a VAR/PARAM with STACKOFF spill encoding, not a
       * real stack reference). */
      if (dest.is_local && dest.is_lval && dest.tag == IROP_TAG_STACKOFF &&
          dest.vreg_type == 0)
      {
        stack_off = (int32_t)irop_get_imm64_ex(ir, dest);
        have_off = 1;
      }
      /* Temp-deref dest where temp is known to be Addr[StackLoc[X]]. */
      else if (dest.is_lval)
      {
        int32_t dvr = irop_get_vreg(dest);
        if (dvr >= 0 && TCCIR_DECODE_VREG_TYPE(dvr) == TCCIR_VREG_TYPE_TEMP)
        {
          int dpos = TCCIR_DECODE_VREG_POSITION(dvr);
          if (dpos <= max_tmp_pos && tmp_kb[dpos].gen == current_gen &&
              tmp_kb[dpos].has_stack_off)
          {
            stack_off = tmp_kb[dpos].stack_off;
            have_off = 1;
          }
        }
      }

      if (have_off)
      {
        uint32_t kz, ko;
        if (kb_operand(ir, src1, tmp_kb, max_tmp_pos, current_gen,
                       stack_slots, n_stack_slots, &kz, &ko))
        {
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

    /* CALL/IJUMP/SETJMP/INLINE_ASM: invalidate all stack slots conservatively. */
    if (op == TCCIR_OP_FUNCCALLVOID || op == TCCIR_OP_FUNCCALLVAL ||
        op == TCCIR_OP_IJUMP || op == TCCIR_OP_INLINE_ASM ||
        op == TCCIR_OP_SETJMP || op == TCCIR_OP_LONGJMP ||
        op == TCCIR_OP_VLA_ALLOC)
    {
      stack_kb_invalidate_all(stack_slots, n_stack_slots);
      /* FUNCCALLVAL has a dest TMP; clear its kb below via the fall-through
       * dest handler. */
    }

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
          }
          LOG_IR_GEN("OPTIMIZE: knownbits TEST_ZERO fold at i=%d "
                     "(ko=%08x, tok=0x%x -> %s)",
                     i, ko, tok, branch_taken ? "JUMP" : "NOP");
          changes++;
          goto post_op;
        }
      }
    }

    int has_dest = irop_config[op].has_dest;
    if (!has_dest)
      continue;

    IROperand dest = tcc_ir_op_get_dest(ir, q);
    int32_t dvr = irop_get_vreg(dest);
    int dest_is_tmp =
        (dvr >= 0) && (TCCIR_DECODE_VREG_TYPE(dvr) == TCCIR_VREG_TYPE_TEMP);
    int dest_is_lval = dest.is_lval;
    if (!dest_is_tmp || dest_is_lval)
      continue;
    int dpos = TCCIR_DECODE_VREG_POSITION(dvr);
    int dest_btype = irop_get_btype(dest);

    /* Track temp → stack-offset mapping for LEA/ASSIGN of Addr[StackLoc].
     * Same vreg_type guard as kb_operand's StackLoc read path. */
    IROperand s1_raw = tcc_ir_op_get_src1(ir, q);
    if ((op == TCCIR_OP_ASSIGN || op == TCCIR_OP_LEA) &&
        s1_raw.is_local && !s1_raw.is_lval && s1_raw.tag == IROP_TAG_STACKOFF &&
        s1_raw.vreg_type == 0)
    {
      int32_t off = (int32_t)irop_get_imm64_ex(ir, s1_raw);
      tmp_kb[dpos].gen = current_gen;
      tmp_kb[dpos].kz = 0;
      tmp_kb[dpos].ko = 0;
      tmp_kb[dpos].has_stack_off = 1;
      tmp_kb[dpos].stack_off = off;
      continue;
    }

    /* Clear any old has_stack_off on redefinition. */
    tmp_kb[dpos].has_stack_off = 0;

    if (dest_btype == IROP_BTYPE_INT64 ||
        dest_btype == IROP_BTYPE_FLOAT32 ||
        dest_btype == IROP_BTYPE_FLOAT64 ||
        dest_btype == IROP_BTYPE_STRUCT)
    {
      tmp_kb[dpos].gen = 0;
      continue;
    }

    IROperand s1 = s1_raw;
    IROperand s2 = tcc_ir_op_get_src2(ir, q);
    int s1_btype = irop_get_btype(s1);
    int s2_btype = irop_get_btype(s2);
    int wide_src =
        (s1_btype == IROP_BTYPE_INT64) || (s2_btype == IROP_BTYPE_INT64) ||
        (s1_btype == IROP_BTYPE_FLOAT32) || (s2_btype == IROP_BTYPE_FLOAT32) ||
        (s1_btype == IROP_BTYPE_FLOAT64) || (s2_btype == IROP_BTYPE_FLOAT64);
    if (wide_src)
    {
      tmp_kb[dpos].gen = 0;
      continue;
    }

    uint32_t a_kz = 0, a_ko = 0, b_kz = 0, b_ko = 0;
    int have_kb = 0;
    if (op == TCCIR_OP_ASSIGN || op == TCCIR_OP_ZEXT)
    {
      have_kb = kb_operand(ir, s1, tmp_kb, max_tmp_pos, current_gen,
                           stack_slots, n_stack_slots, &a_kz, &a_ko);
    }
    else if (op == TCCIR_OP_AND || op == TCCIR_OP_OR || op == TCCIR_OP_XOR)
    {
      int h1 = kb_operand(ir, s1, tmp_kb, max_tmp_pos, current_gen,
                          stack_slots, n_stack_slots, &a_kz, &a_ko);
      int h2 = kb_operand(ir, s2, tmp_kb, max_tmp_pos, current_gen,
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
                          stack_slots, n_stack_slots, &a_kz, &a_ko);
      int h2 = kb_operand(ir, s2, tmp_kb, max_tmp_pos, current_gen,
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
        if (amt >= 0 && amt < 32)
        {
          int h1 = kb_operand(ir, s1, tmp_kb, max_tmp_pos, current_gen,
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
        if (((dkz | dko) == 0xFFFFFFFFu) && op != TCCIR_OP_ASSIGN)
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
          changes++;
          continue;
        }
        tmp_kb[dpos].gen = current_gen;
        tmp_kb[dpos].kz = dkz;
        tmp_kb[dpos].ko = dko;
        continue;
      }
    }

    /* No kb info for this dest. */
    tmp_kb[dpos].gen = 0;
    continue;

  post_op:;
  }

  tcc_free(tmp_kb);
  tcc_free(block_start_seen);
  return changes;
}

int tcc_ir_opt_known_bits_ex(IROptCtx *ctx)
{
  return tcc_ir_opt_known_bits(ctx->ir);
}
