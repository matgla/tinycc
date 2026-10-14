/*
 *  TCC IR - word-run copies into an escaping slot become one helper call (flat)
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License
 * as published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 */

#define USING_GLOBALS

#include "ir.h"
#include "opt.h"
#include "opt_utils.h"

/* The frontend copies a by-value aggregate between frame slots as
 * width-uniform LOAD/STORE chunks (vstore's small-aggregate path) so
 * store-load forwarding and slot elimination can delete the copy outright.
 * When the destination's address escapes -- the Zig C backend's
 * `t5 = a1; t6 = &t5 + 32; seek(t6)` -- no pass can remove the slot, the
 * chunks never fold, and W words cost 2W scalar `ldr`/`str` pairs in the
 * prologue.  One __aeabi_memmove4 call with a constant size is 3 setup
 * instructions at the site plus the libtcc1 body the backend picks
 * (__tcc_wcopy_N for W >= 3, else unrolled LDM/STM pairs -- both cheaper
 * than the scalar pairs).  The kernel's romfs FileHeader.load paid 16
 * instructions per call for its 32-byte IFile copy where gcc emits two
 * LDM/STM pairs.
 *
 * Matched at a LOAD whose source is a stack-passed parameter home word:
 * a maximal strictly alternating run
 *   LOAD  Tk <-- [param + S + 4k]
 *   STORE [dst + D + 4k] <-- Tk
 * of 3..32 words covering [S, S+4W) and [D, D+4W) with word stride, no
 * jump target inside, whose destination extent's address is taken
 * somewhere (a non-lvalue STACKOFF reference overlapping it) -- the proof
 * that forwarding had its chance and the slot is real memory.  The first
 * four quads become the call's three PARAMs and the CALL; the rest NOP.
 * Each carrier temp must have this STORE as its only use, or dropping the
 * LOADs would orphan a reader. */

#define ECC_MAX_WORDS 32
#define ECC_MIN_WORDS 3

/* Every operand slot of `q` that is a non-lvalue STACKOFF address inside
 * [lo, hi).  Used for the destination escape proof; the run's own quads
 * only reference the extent as lvalue stores and so never match. */
static int ecc_addr_taken_in(TCCIRState *ir, int lo, int hi)
{
  int n = ir->next_instruction_index;
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;
    int nops = irop_config[q->op].has_dest + irop_config[q->op].has_src1 + irop_config[q->op].has_src2;
    if (ir_op_has(q->op, IROP_A_SLOT3))
      nops++;
    for (int s = 0; s < nops; s++)
    {
      IROperand op = ir->iroperand_pool[q->operand_base + s];
      if (irop_get_tag(op) != IROP_TAG_STACKOFF || op.is_lval || op.is_llocal)
        continue;
      int32_t off = irop_get_stack_offset(op);
      if (off >= lo && off < hi)
        return 1;
    }
  }
  return 0;
}

/* Uses of vreg `vr` as a value (any operand slot) outside `skip`. */
static int ecc_vreg_has_other_use(TCCIRState *ir, int32_t vr, int skip_from, int skip_to)
{
  int n = ir->next_instruction_index;
  for (int i = 0; i < n; i++)
  {
    if (i >= skip_from && i < skip_to)
      continue;
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;
    int nops = irop_config[q->op].has_dest + irop_config[q->op].has_src1 + irop_config[q->op].has_src2;
    if (ir_op_has(q->op, IROP_A_SLOT3))
      nops++;
    for (int s = 0; s < nops; s++)
    {
      IROperand op = ir->iroperand_pool[q->operand_base + s];
      if (irop_get_vreg(op) == vr && irop_has_vreg(op))
        return 1;
    }
  }
  return 0;
}

int tcc_ir_opt_escape_copy_call(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  int changes = 0;
  if (n == 0)
    return 0;

  for (int i = 0; i + 2 * ECC_MIN_WORDS <= n; i++)
  {
    IRQuadCompact *lq = &ir->compact_instructions[i];
    if (lq->op != TCCIR_OP_LOAD || lq->is_jump_target)
      continue;

    /* Word 0: LOAD of a stack-passed parameter home word. */
    IROperand src0 = tcc_ir_op_get_src1(ir, lq);
    if (irop_get_tag(src0) != IROP_TAG_STACKOFF || !src0.is_lval || src0.is_llocal ||
        !src0.is_param || irop_is_64bit(src0) || irop_get_btype(src0) != IROP_BTYPE_INT32 ||
        tcc_ir_access_is_volatile(ir, src0))
      continue;
    int32_t param_vr = irop_get_vreg(src0);
    if (param_vr < 0 || TCCIR_DECODE_VREG_TYPE(param_vr) != TCCIR_VREG_TYPE_PARAM)
      continue;
    int32_t s0 = irop_get_stack_offset(src0);
    if (s0 & 3)
      continue;

    /* Walk the alternating run. */
    int load_idx[ECC_MAX_WORDS];
    int store_idx[ECC_MAX_WORDS];
    int w = 0;
    int j = i;
    while (w < ECC_MAX_WORDS && j + 1 < n)
    {
      IRQuadCompact *L = &ir->compact_instructions[j];
      IRQuadCompact *S = &ir->compact_instructions[j + 1];
      if (L->op != TCCIR_OP_LOAD || S->op != TCCIR_OP_STORE || L->is_jump_target || S->is_jump_target)
        break;
      IROperand ls = tcc_ir_op_get_src1(ir, L);
      IROperand sd = tcc_ir_op_get_dest(ir, S);
      IROperand ss = tcc_ir_op_get_src1(ir, S);
      if (irop_get_tag(ls) != IROP_TAG_STACKOFF || !ls.is_lval || ls.is_llocal || !ls.is_param ||
          irop_get_vreg(ls) != param_vr || irop_is_64bit(ls) ||
          irop_get_btype(ls) != IROP_BTYPE_INT32 || tcc_ir_access_is_volatile(ir, ls))
        break;
      if (irop_get_tag(sd) != IROP_TAG_STACKOFF || !sd.is_lval || sd.is_llocal || !sd.is_local ||
          sd.is_param || irop_get_vreg(sd) >= 0 || irop_is_64bit(sd) ||
          irop_get_btype(sd) != IROP_BTYPE_INT32 || tcc_ir_access_is_volatile(ir, sd))
        break;
      if (irop_get_stack_offset(ls) != s0 + 4 * w)
        break;
      if (w > 0)
      {
        if (irop_get_stack_offset(sd) != irop_get_stack_offset(tcc_ir_op_get_dest(ir, &ir->compact_instructions[store_idx[0]])) + 4 * w)
          break;
      }
      /* STORE source is exactly the LOAD's destination temp. */
      if (!irop_has_vreg(ss) || irop_get_vreg(ss) < 0 ||
          TCCIR_DECODE_VREG_TYPE(irop_get_vreg(ss)) != TCCIR_VREG_TYPE_TEMP || ss.is_lval)
        break;
      if (!tcc_ir_op_dest_has_vreg(ir, L) || tcc_ir_op_dest_vreg(ir, L) < 0 ||
          TCCIR_DECODE_VREG_TYPE(tcc_ir_op_dest_vreg(ir, L)) != TCCIR_VREG_TYPE_TEMP ||
          irop_get_vreg(ss) != tcc_ir_op_dest_vreg(ir, L))
        break;
      load_idx[w] = j;
      store_idx[w] = j + 1;
      w++;
      j += 2;
    }
    if (w < ECC_MIN_WORDS)
      continue;

    int32_t d0 = (int32_t)irop_get_stack_offset(tcc_ir_op_get_dest(ir, &ir->compact_instructions[store_idx[0]]));
    if ((d0 & 3) || (s0 & 3))
      continue;
    const int size = 4 * w;

    /* The destination extent must be real escaping memory: forwarding and
     * slot elimination have run and left the chunks here, so the only way
     * this copy still exists as chunks is that its slot outlives it. */
    if (!ecc_addr_taken_in(ir, d0, d0 + size))
      continue;

    /* Dropping the LOADs must not orphan any other reader of a carrier. */
    int orphan = 0;
    for (int k = 0; k < w && !orphan; k++)
      orphan = ecc_vreg_has_other_use(ir, tcc_ir_op_dest_vreg(ir, &ir->compact_instructions[load_idx[k]]),
                                       load_idx[k], load_idx[k] + 2);
    if (orphan)
      continue;

    /* Rewrites below rewrite memory in place; a reader between the run's
     * stores is impossible (strict alternation), but a SECOND run from the
     * same param into an overlapping extent later is not -- the call writes
     * the whole extent at once.  Overlapping destinations already had
     * word-interleaved stores, which the run walk rejected, so the extents
     * are either identical or disjoint.  Identical is fine (same bytes). */

    const int call_id = ir->next_call_id++;
    Sym *helper = external_global_sym(tok_alloc_const("__aeabi_memmove4"), &func_old_type);

    IROperand dst_addr = tcc_ir_op_get_dest(ir, &ir->compact_instructions[store_idx[0]]);
    dst_addr.is_lval = 0;
    IROperand src_addr = tcc_ir_op_get_src1(ir, &ir->compact_instructions[load_idx[0]]);
    src_addr.is_lval = 0;
    IROperand size_op = irop_make_imm32(-1, size, IROP_BTYPE_INT32);

    /* FUNCPARAMVAL pool layout: [src1, src2]; CALL: [src1, src2]. */
    IRQuadCompact *p0 = &ir->compact_instructions[load_idx[0]];
    int pb0 = tcc_ir_iroperand_pool_add(ir, dst_addr);
    tcc_ir_iroperand_pool_add(ir, irop_make_imm32(-1, TCCIR_ENCODE_PARAM(call_id, 0), IROP_BTYPE_INT32));
    p0->op = TCCIR_OP_FUNCPARAMVAL;
    p0->operand_base = pb0;

    IRQuadCompact *p1 = &ir->compact_instructions[store_idx[0]];
    int pb1 = tcc_ir_iroperand_pool_add(ir, src_addr);
    tcc_ir_iroperand_pool_add(ir, irop_make_imm32(-1, TCCIR_ENCODE_PARAM(call_id, 1), IROP_BTYPE_INT32));
    p1->op = TCCIR_OP_FUNCPARAMVAL;
    p1->operand_base = pb1;

    IRQuadCompact *p2 = &ir->compact_instructions[load_idx[1]];
    int pb2 = tcc_ir_iroperand_pool_add(ir, size_op);
    tcc_ir_iroperand_pool_add(ir, irop_make_imm32(-1, TCCIR_ENCODE_PARAM(call_id, 2), IROP_BTYPE_INT32));
    p2->op = TCCIR_OP_FUNCPARAMVAL;
    p2->operand_base = pb2;

    IRQuadCompact *cq = &ir->compact_instructions[store_idx[1]];
    uint32_t sym_pool = tcc_ir_pool_add_symref(ir, helper, 0, 0);
    int cb = tcc_ir_iroperand_pool_add(ir, irop_make_symref(-1, sym_pool, 0, 0, 0, IROP_BTYPE_FUNC));
    tcc_ir_iroperand_pool_add(ir, irop_make_imm32(-1, TCCIR_ENCODE_CALL(call_id, 3), IROP_BTYPE_INT32));
    cq->op = TCCIR_OP_FUNCCALLVOID;
    cq->operand_base = cb;

    /* quads [0..3] are the three PARAMs and the CALL; words 2.. fade out. */
    for (int k = 2; k < w; k++)
    {
      ir->compact_instructions[load_idx[k]].op = TCCIR_OP_NOP;
      ir->compact_instructions[store_idx[k]].op = TCCIR_OP_NOP;
    }

    changes++;
    i = store_idx[w - 1];
  }

  return changes;
}
