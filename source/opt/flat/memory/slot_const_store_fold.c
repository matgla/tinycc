/*
 *  TCC IR - Fold constant partial stores into a frame word (flat, pre-SSA)
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
#include "opt_engine.h"

/* A compound literal or initializer zero-fills its local, then stores each
 * field:
 *
 *   StackLoc[-28] <-- #0      StackLoc[-16] <-- #0
 *   StackLoc[-28] <-- #1 (u8) StackLoc[-16] <-- #2 (u8)  StackLoc[-15] <-- #32 (u8)
 *
 * When the local was read back word by word (the old by-reference argument
 * copy), store-to-load forwarding folded each word into one constant.  Passed
 * by value, it is read as a whole by the call, so nothing did: every bool or
 * byte field cost its own STRB.W.  Within a block, a constant byte/halfword
 * store into a word an earlier constant word store fully set -- with nothing
 * reading the word in between -- is folded into that store's immediate. */

#define SCSF_MAX 64

typedef struct ScsfWord
{
  int32_t off;   /* word-aligned frame offset */
  uint32_t val;  /* the word's bytes */
  int store;     /* the word store holding them */
} ScsfWord;

static int scsf_width(int btype)
{
  switch (btype)
  {
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
    return -1; /* struct and the like: unknown extent */
  }
}

/* A concrete slot of this function's own frame. */
static int scsf_slot(IROperand op, int32_t *off)
{
  if (irop_is_none(op) || irop_get_tag(op) != IROP_TAG_STACKOFF || !op.is_local || op.is_param || op.is_llocal ||
      irop_get_vreg(op) >= 0)
    return 0;
  *off = irop_get_stack_offset(op);
  return 1;
}

static int scsf_nops(int op)
{
  int n = irop_config[op].has_dest + irop_config[op].has_src1 + irop_config[op].has_src2;
  if (ir_op_has(op, IROP_A_SLOT3) && !ir_opset_has(IR_LEGACY_GAP_OPS(TCCIR_OP_UMAAL), op))
    n++;
  return n;
}

static void scsf_forget(ScsfWord *w, int *nw, int32_t lo, int32_t hi)
{
  for (int k = 0; k < *nw;)
  {
    if (w[k].off < hi && w[k].off + 4 > lo)
      w[k] = w[--*nw];
    else
      k++;
  }
}

static int scsf_find(ScsfWord *w, int nw, int32_t off)
{
  for (int k = 0; k < nw; k++)
    if (w[k].off == off)
      return k;
  return -1;
}

int tcc_ir_opt_slot_const_store_fold(TCCIRState *ir)
{
  if (!ir)
    return 0;
  const int n = ir->next_instruction_index;
  ScsfWord w[SCSF_MAX];
  int nw = 0, changes = 0;

  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->is_jump_target)
      nw = 0;
    if (q->op == TCCIR_OP_NOP)
      continue;

    if (q->op == TCCIR_OP_STORE)
    {
      IROperand d = tcc_ir_op_get_dest(ir, q), v = tcc_ir_op_get_src1(ir, q);
      int32_t off;
      if (scsf_slot(d, &off) && d.is_lval && irop_get_tag(v) == IROP_TAG_IMM32 &&
          !tcc_ir_access_is_volatile(ir, d))
      {
        const int width = scsf_width(d.btype);
        const uint32_t imm = (uint32_t)irop_get_imm32(v);
        if (width == 4 && d.btype == IROP_BTYPE_INT32 && !(off & 3))
        {
          int k = scsf_find(w, nw, off);
          if (k < 0 && nw < SCSF_MAX)
            k = nw++;
          if (k >= 0)
            w[k] = (ScsfWord){.off = off, .val = imm, .store = i};
          continue;
        }
        if ((width == 1 || (width == 2 && !(off & 1))))
        {
          int k = scsf_find(w, nw, off & ~3);
          if (k >= 0)
          {
            const int sh = (off & 3) * 8;
            const uint32_t mask = (width == 1 ? 0xffu : 0xffffu) << sh;
            w[k].val = (w[k].val & ~mask) | ((imm << sh) & mask);
            IRQuadCompact *sq = &ir->compact_instructions[w[k].store];
            IROperand sv = tcc_ir_op_get_src1(ir, sq);
            sv.u.imm32 = (int32_t)w[k].val;
            tcc_ir_op_set_src1(ir, sq, sv);
            q->op = TCCIR_OP_NOP;
            changes++;
            continue;
          }
        }
      }
    }

    /* Control transfer, a call, or a store through a pointer: forget all. */
    switch (q->op)
    {
    case TCCIR_OP_JUMP:
    case TCCIR_OP_JUMPIF:
    case TCCIR_OP_IJUMP:
    case TCCIR_OP_SWITCH_TABLE:
    case TCCIR_OP_SWITCH_LOAD:
    case TCCIR_OP_RETURNVALUE:
    case TCCIR_OP_RETURNVOID:
    case TCCIR_OP_FUNCCALLVAL:
    case TCCIR_OP_FUNCCALLVOID:
    case TCCIR_OP_SETJMP:
    case TCCIR_OP_NL_SETJMP:
    case TCCIR_OP_INLINE_ASM:
    case TCCIR_OP_ASM_INPUT:
    case TCCIR_OP_ASM_OUTPUT:
    case TCCIR_OP_VLA_ALLOC:
    case TCCIR_OP_VLA_SP_SAVE:
    case TCCIR_OP_VLA_SP_RESTORE:
    case TCCIR_OP_STORE_INDEXED:
    case TCCIR_OP_STORE_POSTINC:
    case TCCIR_OP_BLOCK_COPY:
      nw = 0;
      continue;
    default:
      break;
    }
    if (q->op == TCCIR_OP_STORE)
    {
      int32_t off;
      if (!scsf_slot(tcc_ir_op_get_dest(ir, q), &off))
      {
        nw = 0;
        continue;
      }
    }

    /* Any other reference to a tracked word -- a read, its address, a
     * non-constant or wider store -- ends what is known about it. */
    const int nops = scsf_nops(q->op);
    for (int k = 0; k < nops && nw; k++)
    {
      IROperand op = ir->iroperand_pool[q->operand_base + k];
      int32_t off;
      if (!scsf_slot(op, &off))
        continue;
      int width = op.is_lval ? scsf_width(op.btype) : -1;
      if (width < 0)
        nw = 0; /* a whole struct, or an address: its extent is unknown */
      else
        scsf_forget(w, &nw, off, off + width);
    }
  }
  return changes;
}
