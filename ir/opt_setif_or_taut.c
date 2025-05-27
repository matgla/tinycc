/*
 *  TCC IR - SETIF OR-chain tautology fold
 *
 *  Detects bitwise-OR chains over multiple CMP+SETIF results that compare
 *  the same operands with conditions whose union covers every comparison
 *  outcome.  When the union covers all three states {LT, EQ, GT} of an
 *  integer compare, the OR is the constant 1 regardless of input values.
 *
 *  Target pattern (gcc.c-torture/compile/sc.c):
 *
 *    int foo(int a, int b)
 *    { return (a<0) | (a<=0) | (a==0) | (a!=0) | (a>=0) | (a>0); }
 *
 *  Each leaf is the boolean (a OP 0).  Encoding the OP as a 3-bit mask
 *  over {LT, EQ, GT}, the OR of two booleans gives the bitwise OR of
 *  their masks.  Once the accumulated mask reaches 0b111 (covering all
 *  three states), the chain is provably always 1 — fold to ASSIGN #1.
 *  Subsequent ORs in the chain inherit the all-set mask via the same
 *  tracker so the whole expression collapses.
 *
 *  Single forward pass.  Tracker state is keyed by the destination TEMP
 *  vreg of each SETIF / qualifying OR.  State resets at basic-block
 *  boundaries and is invalidated when any CMP operand vreg is rewritten.
 *
 *  Copyright (c) 2025 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#define USING_GLOBALS

#include "tcc.h"
#include "tccir.h"
#include "tccir_operand.h"
#include "ir.h"
#include "opt.h"
#include "opt_utils.h"
#include "opt_engine.h"
#include "log.h"

#ifndef LOG_SETIF_OR
#ifdef TCC_LOG_SETIF_OR
#define LOG_SETIF_OR(...) fprintf(stderr, "[SETIF_OR] " __VA_ARGS__), fprintf(stderr, "\n")
#else
#define LOG_SETIF_OR(...) ((void)0)
#endif
#endif

/* Per-TEMP tracker: this TEMP holds the boolean for a 3-way compare of
 * (s1_vr, s2_imm) with the given mask of LT/EQ/GT bits set. */
typedef struct
{
  uint32_t gen;         /* basic-block generation when recorded */
  int32_t s1_vr;        /* vreg of CMP src1; -1 if immediate */
  int32_t s2_vr;        /* vreg of CMP src2; -1 if immediate */
  int64_t s1_imm;       /* imm value if s1_vr < 0 */
  int64_t s2_imm;       /* imm value if s2_vr < 0 */
  uint8_t mask;         /* bit0=LT, bit1=EQ, bit2=GT */
  uint8_t is_unsigned;  /* 0 = signed CMP, 1 = unsigned CMP */
} BoolInfo;

/* Cover mask for each comparison token (TOK_*).
 *   bit 0 = LT outcome (a < b)
 *   bit 1 = EQ outcome (a == b)
 *   bit 2 = GT outcome (a > b)
 * Returns 0 (no bits) if the token isn't a recognized integer compare. */
static uint8_t cond_to_mask(int tok)
{
  switch (tok)
  {
  case 0x94: /* TOK_EQ */
    return 0b010;
  case 0x95: /* TOK_NE */
    return 0b101;
  case 0x9c: /* TOK_LT signed */
  case 0x92: /* TOK_ULT unsigned */
    return 0b001;
  case 0x9d: /* TOK_GE signed */
  case 0x93: /* TOK_UGE unsigned */
    return 0b110;
  case 0x9e: /* TOK_LE signed */
  case 0x96: /* TOK_ULE unsigned */
    return 0b011;
  case 0x9f: /* TOK_GT signed */
  case 0x97: /* TOK_UGT unsigned */
    return 0b100;
  default:
    return 0;
  }
}

/* Same-sign-domain bucket for the cond.  Signed and unsigned compares of the
 * same operands can have different outcomes (e.g. -1 vs 1) so they must not
 * be merged in the mask analysis. */
static int cond_is_unsigned(int tok)
{
  return (tok == 0x92 || tok == 0x93 || tok == 0x96 || tok == 0x97);
}

static int operand_is_imm(IROperand op)
{
  return irop_is_immediate(op) && !op.is_sym && !op.is_lval;
}

/* Snapshot a CMP operand into BoolInfo's (vreg, imm) pair. Returns 1 on
 * success, 0 if the operand isn't a plain vreg or plain immediate. */
static int snapshot_cmp_operand(TCCIRState *ir, IROperand op,
                                int32_t *out_vr, int64_t *out_imm)
{
  if (operand_is_imm(op))
  {
    *out_vr = -1;
    *out_imm = irop_get_imm64_ex(ir, op);
    return 1;
  }
  int32_t vr = irop_get_vreg(op);
  if (vr < 0 || op.is_lval || op.is_sym)
    return 0;
  *out_vr = vr;
  *out_imm = 0;
  return 1;
}

/* Two BoolInfos describe the same compare context when both were recorded
 * in the current basic block, with the same compare type (signed vs
 * unsigned) and matching (vreg or immediate) operands on both sides. */
static int bool_info_compatible(const BoolInfo *a, const BoolInfo *b, uint32_t gen)
{
  if (a->gen != gen || b->gen != gen)
    return 0;
  if (a->is_unsigned != b->is_unsigned)
    return 0;
  if (a->s1_vr != b->s1_vr)
    return 0;
  if (a->s2_vr != b->s2_vr)
    return 0;
  if (a->s1_vr < 0 && a->s1_imm != b->s1_imm)
    return 0;
  if (a->s2_vr < 0 && a->s2_imm != b->s2_imm)
    return 0;
  return 1;
}

int tcc_ir_opt_setif_or_tautology(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  if (n < 3)
    return 0;

  int max_tmp = -1;
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP || !irop_config[q->op].has_dest)
      continue;
    IROperand dest = tcc_ir_op_get_dest(ir, q);
    int32_t vr = irop_get_vreg(dest);
    if (vr >= 0 && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_TEMP)
    {
      int pos = TCCIR_DECODE_VREG_POSITION(vr);
      if (pos > max_tmp)
        max_tmp = pos;
    }
  }
  if (max_tmp < 0)
    return 0;

  size_t tbl_bytes = sizeof(BoolInfo) * (size_t)(max_tmp + 1);
  BoolInfo *tbl = (BoolInfo *)tcc_mallocz(tbl_bytes);
  int *block_start_seen = (int *)tcc_mallocz(sizeof(int) * n);
  int block_gen = 1;
  uint32_t current_gen = 1;
  int changes = 0;

  ir_opt_mark_block_starts(ir, block_start_seen, block_gen, n);

  for (int i = 0; i < n; i++)
  {
    /* BB boundary — invalidate all tracker entries. */
    if (i != 0 && block_start_seen[i] == block_gen)
      current_gen++;

    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;

    int op = q->op;

    if (op == TCCIR_OP_SETIF)
    {
      IROperand dest = tcc_ir_op_get_dest(ir, q);
      int32_t dvr = irop_get_vreg(dest);
      if (dvr < 0 || TCCIR_DECODE_VREG_TYPE(dvr) != TCCIR_VREG_TYPE_TEMP || dest.is_lval)
        goto invalidate_writes;
      int dpos = TCCIR_DECODE_VREG_POSITION(dvr);

      /* Locate the CMP that produced the flags this SETIF consumes:
       * the most-recent non-NOP instruction before i. */
      int cmp_idx = i - 1;
      while (cmp_idx >= 0 && ir->compact_instructions[cmp_idx].op == TCCIR_OP_NOP)
        cmp_idx--;
      if (cmp_idx < 0)
      {
        tbl[dpos].gen = 0;
        continue;
      }
      IRQuadCompact *cq = &ir->compact_instructions[cmp_idx];
      if (cq->op != TCCIR_OP_CMP)
      {
        tbl[dpos].gen = 0;
        continue;
      }

      IROperand cs1 = tcc_ir_op_get_src1(ir, cq);
      IROperand cs2 = tcc_ir_op_get_src2(ir, cq);
      int bt1 = irop_get_btype(cs1);
      int bt2 = irop_get_btype(cs2);
      if (bt1 == IROP_BTYPE_FLOAT32 || bt1 == IROP_BTYPE_FLOAT64 ||
          bt2 == IROP_BTYPE_FLOAT32 || bt2 == IROP_BTYPE_FLOAT64)
      {
        tbl[dpos].gen = 0;
        continue;
      }

      int tok = (int)irop_get_imm64_ex(ir, tcc_ir_op_get_src1(ir, q));
      uint8_t mask = cond_to_mask(tok);
      if (mask == 0)
      {
        tbl[dpos].gen = 0;
        continue;
      }

      BoolInfo bi;
      bi.gen = current_gen;
      bi.mask = mask;
      bi.is_unsigned = (uint8_t)cond_is_unsigned(tok);
      if (!snapshot_cmp_operand(ir, cs1, &bi.s1_vr, &bi.s1_imm) ||
          !snapshot_cmp_operand(ir, cs2, &bi.s2_vr, &bi.s2_imm))
      {
        tbl[dpos].gen = 0;
        continue;
      }
      tbl[dpos] = bi;
      continue;
    }

    if (op == TCCIR_OP_OR)
    {
      IROperand dest = tcc_ir_op_get_dest(ir, q);
      IROperand s1 = tcc_ir_op_get_src1(ir, q);
      IROperand s2 = tcc_ir_op_get_src2(ir, q);
      int32_t dvr = irop_get_vreg(dest);
      int32_t v1 = irop_get_vreg(s1);
      int32_t v2 = irop_get_vreg(s2);

      int valid_dest = (dvr >= 0 &&
                        TCCIR_DECODE_VREG_TYPE(dvr) == TCCIR_VREG_TYPE_TEMP &&
                        !dest.is_lval);
      int valid_srcs = (v1 >= 0 && v2 >= 0 &&
                        TCCIR_DECODE_VREG_TYPE(v1) == TCCIR_VREG_TYPE_TEMP &&
                        TCCIR_DECODE_VREG_TYPE(v2) == TCCIR_VREG_TYPE_TEMP &&
                        !s1.is_lval && !s2.is_lval);
      if (!valid_dest || !valid_srcs)
        goto invalidate_writes;

      int p1 = TCCIR_DECODE_VREG_POSITION(v1);
      int p2 = TCCIR_DECODE_VREG_POSITION(v2);
      int dpos = TCCIR_DECODE_VREG_POSITION(dvr);
      if (p1 > max_tmp || p2 > max_tmp)
      {
        tbl[dpos].gen = 0;
        goto invalidate_writes;
      }
      if (!bool_info_compatible(&tbl[p1], &tbl[p2], current_gen))
      {
        tbl[dpos].gen = 0;
        goto invalidate_writes;
      }

      uint8_t combined = (uint8_t)(tbl[p1].mask | tbl[p2].mask);

      /* Always record the combined mask for downstream ORs in the chain. */
      tbl[dpos] = tbl[p1];
      tbl[dpos].mask = combined;

      if (combined == 0b111)
      {
        int btype = irop_get_btype(s1);
        IROperand imm = irop_make_imm32(-1, 1, btype);
        imm.is_unsigned = dest.is_unsigned;
        q->op = TCCIR_OP_ASSIGN;
        tcc_ir_set_src1(ir, i, imm);
        tcc_ir_set_src2(ir, i, IROP_NONE);
        LOG_SETIF_OR("@%d: T%d = T%d | T%d folded to #1 (mask covers LT|EQ|GT)",
                     i, dpos, p1, p2);
        changes++;
      }
      continue;
    }

  invalidate_writes:
    /* Default path: if this op writes a TEMP, drop its tracker entry; if it
     * writes a non-TEMP vreg, invalidate every tracker entry that depends on
     * that vreg as a CMP operand. */
    if (irop_config[op].has_dest)
    {
      IROperand dest = tcc_ir_op_get_dest(ir, q);
      int32_t dvr = irop_get_vreg(dest);
      if (dvr >= 0 && !dest.is_lval)
      {
        if (TCCIR_DECODE_VREG_TYPE(dvr) == TCCIR_VREG_TYPE_TEMP)
        {
          int dpos = TCCIR_DECODE_VREG_POSITION(dvr);
          if (dpos <= max_tmp)
            tbl[dpos].gen = 0;
        }
        for (int j = 0; j <= max_tmp; j++)
        {
          if (tbl[j].gen != current_gen)
            continue;
          if (tbl[j].s1_vr == dvr || tbl[j].s2_vr == dvr)
            tbl[j].gen = 0;
        }
      }
      /* lvalue stores can mutate aliased values; be conservative. */
      if (dest.is_lval)
      {
        for (int j = 0; j <= max_tmp; j++)
          tbl[j].gen = 0;
      }
    }
  }

  tcc_free(tbl);
  tcc_free(block_start_seen);

  return changes;
}

int tcc_ir_opt_setif_or_tautology_ex(IROptCtx *ctx)
{
  return tcc_ir_opt_setif_or_tautology(ctx->ir);
}
