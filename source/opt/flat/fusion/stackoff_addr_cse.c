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



/* Stack-address ADD-operand CSE.
 *
 * When one StackLoc[X] address appears as an inline literal source in two or
 * more ADDs (each with a vreg other operand), codegen re-materializes
 * `add rX, sp, #off` per use, and the downstream SHL+ADD fusion bails on the
 * is_local base.  Fix: hoist one ASSIGN of that StackLoc to a fresh TEMP at
 * function entry and replace each literal use with the TEMP, giving the ADDs
 * a register base so the indexed-memory fusion can fire.
 *
 * Safety: the ASSIGN sits at function entry (before anything can move the
 * frame pointer), so the address is constant for the whole function; the
 * TEMP holds the same FP-relative pointer as the literal.
 */
int tcc_ir_opt_stackoff_addr_cse(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  if (n < 2)
    return 0;

  /* Pass 1: count uses per unique StackLoc offset. */
#define SAC_MAX_OFFSETS 32
  struct {
    int32_t offset;
    int count;
    int32_t hoisted_vreg;
    IROperand sample; /* operand we cloned (for btype) */
  } slots[SAC_MAX_OFFSETS];
  int nslots = 0;

  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_ADD)
      continue;
    IROperand src1 = tcc_ir_op_get_src1(ir, q);
    IROperand src2 = tcc_ir_op_get_src2(ir, q);
    for (int sl = 0; sl < 2; sl++)
    {
      IROperand op = (sl == 0) ? src1 : src2;
      IROperand other = (sl == 0) ? src2 : src1;
      if (irop_get_tag(op) != IROP_TAG_STACKOFF)
        continue;
      if (op.is_lval)
        continue;
      /* Require a vreg other operand (the SHL+ADD pattern); constant-other
       * cases are already handled by stack_addr_cse. */
      if (!irop_has_vreg(other))
        continue;
      int32_t off = op.u.imm32;
      int slot = -1;
      for (int s = 0; s < nslots; s++)
        if (slots[s].offset == off) { slot = s; break; }
      if (slot < 0)
      {
        if (nslots >= SAC_MAX_OFFSETS)
          continue;
        slot = nslots++;
        slots[slot].offset = off;
        slots[slot].count = 0;
        slots[slot].hoisted_vreg = -1;
        slots[slot].sample = op;
      }
      slots[slot].count++;
    }
  }

  /* Pass 2: for each offset with >= 2 uses, hoist an ASSIGN at function entry. */
  int changes = 0;
  for (int s = 0; s < nslots; s++)
  {
    if (slots[s].count < 2)
      continue;

    int32_t t_anon = tcc_ir_vreg_alloc_temp(ir);
    if (t_anon < 0)
      continue;

    /* Mirror the sample operand's btype/sign to keep the IR consistent. */
    IROperand new_dest = irop_make_vreg(t_anon, slots[s].sample.btype);
    new_dest.is_unsigned = slots[s].sample.is_unsigned;
    IROperand new_src = slots[s].sample;
    IRQuadCompact assign_q = {0};
    assign_q.op = TCCIR_OP_ASSIGN;
    assign_q.operand_base = tcc_ir_pool_add(ir, new_dest);
    tcc_ir_pool_add(ir, new_src);

    if (gsym_cse_insert_before(ir, 0, &assign_q) < 0)
      continue;
    n++;
    slots[s].hoisted_vreg = t_anon;
  }

  if (changes >= 0)
  {
    /* Pass 3: rewrite uses (inserts shifted every index, so iterate fresh). */
    for (int i = 0; i < n; i++)
    {
      IRQuadCompact *q = &ir->compact_instructions[i];
      if (q->op != TCCIR_OP_ADD)
        continue;
      IROperand src1 = tcc_ir_op_get_src1(ir, q);
      IROperand src2 = tcc_ir_op_get_src2(ir, q);
      for (int sl = 0; sl < 2; sl++)
      {
        IROperand op = (sl == 0) ? src1 : src2;
        IROperand other = (sl == 0) ? src2 : src1;
        if (irop_get_tag(op) != IROP_TAG_STACKOFF || op.is_lval)
          continue;
        if (!irop_has_vreg(other))
          continue;
        int32_t off = op.u.imm32;
        int slot = -1;
        for (int s = 0; s < nslots; s++)
          if (slots[s].offset == off) { slot = s; break; }
        if (slot < 0 || slots[slot].hoisted_vreg < 0)
          continue;
        IROperand replacement = irop_make_vreg(slots[slot].hoisted_vreg, op.btype);
        replacement.is_unsigned = op.is_unsigned;
        if (sl == 0)
          tcc_ir_set_src1(ir, i, replacement);
        else
          tcc_ir_set_src2(ir, i, replacement);
        changes++;
      }
    }
  }

  LOG_IR_GEN("=== STACKOFF ADDR CSE: %d uses rewritten ===", changes);
  return changes;
#undef SAC_MAX_OFFSETS
}

/* ---------------------------------------------------------------------------
 * Late variant: park the frame base of an already-fused indexed access.
 *
 * An indexed access carries its base in the BASE slot, not in an ADD, so the
 * collector above cannot see it and codegen re-materializes `add rX, sp, #off`
 * at every one.  sha_transform's W-expansion loop is the extreme case: four
 * `LOAD_INDEXED Addr[StackLoc[-320]]` reads of w[i-3/8/14/16] paid four
 * `add r1, sp, #8` per iteration, while the STORE in the same loop already
 * used a hoisted register for the identical address (21 -> 17 instructions).
 *
 * This MUST run late — from tcc_ir_ssa_regalloc, beside global_addr_hoist,
 * after every alias-sensitive pass.  A `LOAD_INDEXED Addr[StackLoc[X]]` is a
 * DIRECT frame reference that slot-based alias analysis (opt/analysis/alias.c:
 * operand_references_slot / stackoff_same_slot) reasons about precisely;
 * rewriting the base to a vreg turns it into an indirect access those analyses
 * can no longer attribute to the slot.  Done early it miscompiles
 * gcc.c-torture pr51466 (`volatile int v[4]` written and read back through
 * `&v[i]`) at -O1 and -O2, which is why the pass above was left alone.
 * ------------------------------------------------------------------------- */
/* A prologue `ASSIGN Tn <- Addr[StackLoc[off]]` whose Tn is never redefined,
 * or -1.  The single-definition scan covers the whole function, not just the
 * prologue: a later write to Tn would make the reuse read a different address. */
static int32_t sib_find_prologue_addr_temp(TCCIRState *ir, int prologue_end, int32_t off)
{
  int n = ir->next_instruction_index;
  for (int i = 0; i < prologue_end; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_ASSIGN)
      continue;
    IROperand d = tcc_ir_op_get_dest(ir, q);
    IROperand s1 = tcc_ir_op_get_src1(ir, q);
    if (irop_get_tag(s1) != IROP_TAG_STACKOFF || s1.is_lval || s1.is_llocal)
      continue;
    if (s1.u.imm32 != off)
      continue;
    int32_t vr = irop_get_vreg(d);
    if (vr < 0 || TCCIR_DECODE_VREG_TYPE(vr) != TCCIR_VREG_TYPE_TEMP || d.is_lval)
      continue;
    int defs = 0;
    for (int j = 0; j < n; j++)
    {
      IRQuadCompact *dq = &ir->compact_instructions[j];
      if (dq->op == TCCIR_OP_NOP || !irop_config[dq->op].has_dest)
        continue;
      /* A store's dest slot holds the ADDRESS it writes through, not a vreg it
       * defines -- counting it as a def is what made the base temp look
       * multiply-defined and blocked every reuse (`arr[i] = v` in the same
       * function is enough). */
      if (dq->op == TCCIR_OP_STORE || dq->op == TCCIR_OP_STORE_INDEXED ||
          dq->op == TCCIR_OP_STORE_POSTINC)
        continue;
      IROperand dd = tcc_ir_op_get_dest(ir, dq);
      if (dd.is_lval)
        continue;
      if (irop_get_vreg(dd) == vr)
        defs++;
    }
    if (defs == 1)
      return vr;
  }
  return -1;
}

int tcc_ir_opt_stackoff_indexed_base_cse(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  if (n < 2)
    return 0;

#define SIB_MAX_OFFSETS 32
  struct {
    int32_t offset;
    int count;
    int32_t hoisted_vreg;
    int32_t existing_vreg;   /* prologue TEMP already holding this address, or -1 */
    IROperand sample;
  } slots[SIB_MAX_OFFSETS];
  int nslots = 0;
  int changes = 0;

  /* Pass 0: a TEMP the prologue already loaded with `Addr[StackLoc[off]]`.
   *
   * Reusing one is strictly free -- no new instruction, no new live range,
   * since the temp is live from entry anyway -- so it beats the >=2 threshold
   * below, which exists only to pay for the hoisted ASSIGN.  One raw base left
   * behind in a loop costs an `add rX, sp, #off` per iteration, and after the
   * indexed-deref fusions a lone raw base is the common leftover: its sibling
   * accesses got the temp and it did not.
   *
   * Scanned only over the straight-line prologue (up to the first branch or
   * branch target), so the definition dominates every use in the function, and
   * only for a TEMP defined exactly once. */
  int prologue_end = 0;
  while (prologue_end < n)
  {
    IRQuadCompact *pq = &ir->compact_instructions[prologue_end];
    if (pq->op == TCCIR_OP_JUMP || pq->op == TCCIR_OP_JUMPIF ||
        pq->op == TCCIR_OP_IJUMP || pq->op == TCCIR_OP_SWITCH_TABLE)
      break;
    if (prologue_end > 0 && pq->is_jump_target)
      break;
    prologue_end++;
  }

  /* Pass 1: count STACKOFF bases per slot offset. */
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_LOAD_INDEXED && q->op != TCCIR_OP_STORE_INDEXED)
      continue;
    IROperand base = (q->op == TCCIR_OP_STORE_INDEXED) ? tcc_ir_op_get_dest(ir, q)
                                                       : tcc_ir_op_get_src1(ir, q);
    if (irop_get_tag(base) != IROP_TAG_STACKOFF || base.is_lval)
      continue;
    int32_t off = base.u.imm32;
    int slot = -1;
    for (int s = 0; s < nslots; s++)
      if (slots[s].offset == off) { slot = s; break; }
    if (slot < 0)
    {
      if (nslots >= SIB_MAX_OFFSETS)
        continue;
      slot = nslots++;
      slots[slot].offset = off;
      slots[slot].count = 0;
      slots[slot].hoisted_vreg = -1;
      slots[slot].existing_vreg = sib_find_prologue_addr_temp(ir, prologue_end, off);
      slots[slot].sample = base;
    }
    slots[slot].count++;
  }

  /* Pass 2: pick a register for each offset -- an existing prologue temp when
   * there is one (free), otherwise a fresh temp, but only once the >= 2 uses
   * justify the ASSIGN pass 4 inserts. */
  int hoisted = 0;
  for (int s = 0; s < nslots; s++)
  {
    if (slots[s].existing_vreg >= 0)
    {
      slots[s].hoisted_vreg = slots[s].existing_vreg;
      hoisted++;
      continue;
    }
    if (slots[s].count < 2)
      continue;
    int32_t t = tcc_ir_vreg_alloc_temp(ir);
    if (t < 0)
      continue;
    slots[s].hoisted_vreg = t;
    hoisted++;
  }
  if (!hoisted)
    return 0;

  /* Pass 3: rewrite the bases (indices are still pre-insertion here). */
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    int is_store = (q->op == TCCIR_OP_STORE_INDEXED);
    if (q->op != TCCIR_OP_LOAD_INDEXED && !is_store)
      continue;
    IROperand base = is_store ? tcc_ir_op_get_dest(ir, q) : tcc_ir_op_get_src1(ir, q);
    if (irop_get_tag(base) != IROP_TAG_STACKOFF || base.is_lval)
      continue;
    int slot = -1;
    for (int s = 0; s < nslots; s++)
      if (slots[s].offset == base.u.imm32) { slot = s; break; }
    if (slot < 0 || slots[slot].hoisted_vreg < 0)
      continue;
    IROperand rep = irop_make_vreg(slots[slot].hoisted_vreg, base.btype);
    rep.is_unsigned = base.is_unsigned;
    /* Keep the packed-access mark: the 64-bit indexed lowering reads it to
     * decide LDRD/STRD vs a split pair. */
    rep.aux = base.aux;
    if (is_store)
      tcc_ir_set_dest(ir, i, rep);
    else
      tcc_ir_set_src1(ir, i, rep);
    changes++;
  }

  /* Pass 4: materialize each base once at function entry, after the rewrites
   * so the shifting indices are harmless. */
  {
    int pos = 0;
    for (int s = 0; s < nslots; s++)
    {
      if (slots[s].hoisted_vreg < 0 || slots[s].existing_vreg >= 0)
        continue;   /* reused temp: the prologue already materializes it */
      IROperand dst = irop_make_vreg(slots[s].hoisted_vreg, IROP_BTYPE_INT32);
      IROperand src = slots[s].sample;
      src.is_lval = 0;
      if (insert_instr_at(ir, pos, TCCIR_OP_ASSIGN, dst, src, irop_make_none()) == 0)
      {
        pos++;
        changes++;
      }
    }
  }

  LOG_IR_GEN("=== STACKOFF INDEXED BASE CSE: %d rewrites ===", changes);
  return changes;
#undef SIB_MAX_OFFSETS
}
