/*
 *  TCC IR - SETIF OR-chain tautology fold
 *
 *  Copyright (c) 2025 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

/* Folds OR-chains of CMP+SETIF booleans whose {LT,EQ,GT} cover mask covers all states to constant 1. */

#define USING_GLOBALS

#include "tcc.h"
#include "tccir.h"
#include "tccir_operand.h"
#include "ir.h"
#include "opt.h"
#include "opt_utils.h"
#include "opt_engine.h"
#include "log.h"
#include "memory/vector.h"

#ifndef LOG_SETIF_OR
#ifdef TCC_LOG_SETIF_OR
#define LOG_SETIF_OR(...) fprintf(stderr, "[SETIF_OR] " __VA_ARGS__), fprintf(stderr, "\n")
#else
#define LOG_SETIF_OR(...) ((void)0)
#endif
#endif

typedef struct
{
  uint32_t gen;
  int32_t s1_vr;
  int32_t s2_vr;
  int64_t s1_imm;
  int64_t s2_imm;
  uint8_t mask;
  uint8_t is_unsigned;
} BoolInfo;

typedef struct
{
  TCCIRState *ir;
  BoolInfo *tbl;
  int max_tmp;
  int *active_pos;
  int active_n;
  uint32_t current_gen;
  int changes;
} SetifOrCtx;

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

static int cond_is_unsigned(int tok)
{
  /* 0x92=ULT, 0x93=UGE, 0x96=ULE, 0x97=UGT — unsigned compares. */
  return (tok == 0x92 || tok == 0x93 || tok == 0x96 || tok == 0x97);
}

static int snapshot_cmp_operand(TCCIRState *ir, IROperand op,
                                int32_t *out_vr, int64_t *out_imm)
{
  if (irop_is_plain_imm(op))
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

static int setif_or_compute_max_tmp(TCCIRState *ir, int n)
{
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
  return max_tmp;
}

/* Returns 0 only when the dest isn't a plain TEMP, so the caller invalidates it. */
static int setif_or_handle_setif(SetifOrCtx *ctx, int i, IRQuadCompact *q)
{
  TCCIRState *ir = ctx->ir;
  IROperand dest = tcc_ir_op_get_dest(ir, q);
  int32_t dvr = irop_get_vreg(dest);
  if (dvr < 0 || TCCIR_DECODE_VREG_TYPE(dvr) != TCCIR_VREG_TYPE_TEMP || dest.is_lval)
    return 0;
  int dpos = TCCIR_DECODE_VREG_POSITION(dvr);

  /* The condition comes from the CMP immediately preceding this SETIF. */
  int cmp_idx = i - 1;
  while (cmp_idx >= 0 && ir->compact_instructions[cmp_idx].op == TCCIR_OP_NOP)
    cmp_idx--;
  if (cmp_idx < 0 || ir->compact_instructions[cmp_idx].op != TCCIR_OP_CMP)
  {
    ctx->tbl[dpos].gen = 0;
    return 1;
  }
  IRQuadCompact *cq = &ir->compact_instructions[cmp_idx];

  IROperand cs1 = tcc_ir_op_get_src1(ir, cq);
  IROperand cs2 = tcc_ir_op_get_src2(ir, cq);
  int bt1 = irop_get_btype(cs1);
  int bt2 = irop_get_btype(cs2);
  if (bt1 == IROP_BTYPE_FLOAT32 || bt1 == IROP_BTYPE_FLOAT64 ||
      bt2 == IROP_BTYPE_FLOAT32 || bt2 == IROP_BTYPE_FLOAT64)
  {
    ctx->tbl[dpos].gen = 0;
    return 1;
  }

  int tok = (int)irop_get_imm64_ex(ir, tcc_ir_op_get_src1(ir, q));
  uint8_t mask = cond_to_mask(tok);
  if (mask == 0)
  {
    ctx->tbl[dpos].gen = 0;
    return 1;
  }

  BoolInfo bi;
  bi.gen = ctx->current_gen;
  bi.mask = mask;
  bi.is_unsigned = (uint8_t)cond_is_unsigned(tok);
  if (!snapshot_cmp_operand(ir, cs1, &bi.s1_vr, &bi.s1_imm) ||
      !snapshot_cmp_operand(ir, cs2, &bi.s2_vr, &bi.s2_imm))
  {
    ctx->tbl[dpos].gen = 0;
    return 1;
  }
  ctx->tbl[dpos] = bi;
  ctx->active_pos[ctx->active_n++] = dpos;
  return 1;
}

/* Returns 0 when the operands aren't foldable, so the caller invalidates the dest. */
static int setif_or_handle_or(SetifOrCtx *ctx, int i, IRQuadCompact *q)
{
  TCCIRState *ir = ctx->ir;
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
    return 0;

  int p1 = TCCIR_DECODE_VREG_POSITION(v1);
  int p2 = TCCIR_DECODE_VREG_POSITION(v2);
  int dpos = TCCIR_DECODE_VREG_POSITION(dvr);
  if (p1 > ctx->max_tmp || p2 > ctx->max_tmp)
  {
    ctx->tbl[dpos].gen = 0;
    return 0;
  }
  if (!bool_info_compatible(&ctx->tbl[p1], &ctx->tbl[p2], ctx->current_gen))
  {
    ctx->tbl[dpos].gen = 0;
    return 0;
  }

  uint8_t combined = (uint8_t)(ctx->tbl[p1].mask | ctx->tbl[p2].mask);

  ctx->tbl[dpos] = ctx->tbl[p1];
  ctx->tbl[dpos].mask = combined;
  ctx->active_pos[ctx->active_n++] = dpos;

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
    ctx->changes++;
  }
  return 1;
}

/* Writing ops that aren't a tracked SETIF/OR drop trackers they may clobber. */
static void setif_or_invalidate_writes(SetifOrCtx *ctx, int op, IRQuadCompact *q)
{
  /* No active chain => every current-gen entry is in active_pos (empty), nothing to clear. */
  if (ctx->active_n == 0 || !irop_config[op].has_dest)
    return;

  IROperand dest = tcc_ir_op_get_dest(ctx->ir, q);
  int32_t dvr = irop_get_vreg(dest);
  if (dvr >= 0 && !dest.is_lval)
  {
    if (TCCIR_DECODE_VREG_TYPE(dvr) == TCCIR_VREG_TYPE_TEMP)
    {
      int dpos = TCCIR_DECODE_VREG_POSITION(dvr);
      if (dpos <= ctx->max_tmp)
        ctx->tbl[dpos].gen = 0;
    }
    for (int k = 0; k < ctx->active_n; k++)
    {
      int j = ctx->active_pos[k];
      if (ctx->tbl[j].gen != ctx->current_gen)
        continue;
      if (ctx->tbl[j].s1_vr == dvr || ctx->tbl[j].s2_vr == dvr)
        ctx->tbl[j].gen = 0;
    }
  }
  /* lvalue stores can mutate aliased values; be conservative. */
  if (dest.is_lval)
  {
    for (int k = 0; k < ctx->active_n; k++)
      ctx->tbl[ctx->active_pos[k]].gen = 0;
    ctx->active_n = 0;
  }
}

int tcc_ir_opt_setif_or_tautology(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  if (n < 3)
    return 0;

  int max_tmp = setif_or_compute_max_tmp(ir, n);
  if (max_tmp < 0)
    return 0;

  scoped_vector(BoolInfo) tbl_owner = {0};
  scoped_vector(int) block_start_owner = {0};
  scoped_vector(int) active_pos_owner = {0};
  vector_resize(&tbl_owner, (size_t)(max_tmp + 1));
  vector_resize(&block_start_owner, (size_t)n);
  vector_reserve(&active_pos_owner, (size_t)n);

  int *block_start_seen = vector_data(&block_start_owner);
  int block_gen = 1;

  SetifOrCtx ctx = {
      .ir = ir,
      .tbl = vector_data(&tbl_owner),
      .max_tmp = max_tmp,
      .active_pos = vector_data(&active_pos_owner),
      .active_n = 0,
      .current_gen = 1,
      .changes = 0,
  };

  ir_opt_mark_block_starts(ir, block_start_seen, block_gen, n);

  for (int i = 0; i < n; i++)
  {
    if (i != 0 && block_start_seen[i] == block_gen)
    {
      ctx.current_gen++;
      ctx.active_n = 0;
    }

    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;

    int op = q->op;
    int handled;
    switch (op)
    {
    case TCCIR_OP_SETIF:
      handled = setif_or_handle_setif(&ctx, i, q);
      break;
    case TCCIR_OP_OR:
      handled = setif_or_handle_or(&ctx, i, q);
      break;
    default:
      handled = 0;
      break;
    }

    if (!handled)
      setif_or_invalidate_writes(&ctx, op, q);
  }

  return ctx.changes;
}

int tcc_ir_opt_setif_or_tautology_ex(IROptCtx *ctx)
{
  return tcc_ir_opt_setif_or_tautology(ctx->ir);
}
