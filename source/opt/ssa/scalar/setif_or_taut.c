/*
 *  TCC IR - SSA SETIF OR-chain tautology fold (ssa:setif_or_taut)
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

/* A SETIF result is a boolean carrying a {LT,EQ,GT} state mask of its CMP.  An
 * OR of two such booleans over the SAME compared operands unions their masks; a
 * union of 0b111 (LT|EQ|GT) is a tautology folded to constant 1.  Single
 * assignment lets a recursive def-use resolve carry the mask through OR-chains. */

#define USING_GLOBALS

#include "ir.h"
#include "ssa_opt.h"
#include "opt_utils.h"
#include "opt/ssa/setif_or_taut.h"

typedef struct
{
  int ok;
  int32_t s1_vr, s2_vr;   /* compared operands (-1 = immediate) */
  int64_t s1_imm, s2_imm;
  uint8_t mask;           /* {GT,EQ,LT} bits */
  uint8_t is_unsigned;
} SoBool;

static SoBool so_fail(void) { SoBool b = { 0, 0, 0, 0, 0, 0, 0 }; return b; }

static uint8_t so_cond_to_mask(int tok)
{
  switch (tok)
  {
  case 0x94: return 0b010;             /* EQ  */
  case 0x95: return 0b101;             /* NE  */
  case 0x9c: case 0x92: return 0b001;  /* LT / ULT */
  case 0x9d: case 0x93: return 0b110;  /* GE / UGE */
  case 0x9e: case 0x96: return 0b011;  /* LE / ULE */
  case 0x9f: case 0x97: return 0b100;  /* GT / UGT */
  default: return 0;
  }
}

static int so_cond_is_unsigned(int tok)
{
  return (tok == 0x92 || tok == 0x93 || tok == 0x96 || tok == 0x97);
}

/* Whether any instruction directly writes vr (as a non-lval dest). */
static int so_vreg_ever_written(TCCIRState *ir, int32_t vr)
{
  int n = ir->next_instruction_index;
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP || !irop_config[q->op].has_dest)
      continue;
    IROperand d = tcc_ir_op_get_dest(ir, q);
    if (!d.is_lval && irop_get_vreg(d) == vr)
      return 1;
  }
  return 0;
}

/* A compared operand that holds ONE value across the OR'd SETIFs (else the
 * mask-union tautology is unsound): a single-def TEMP (SSA value), or a PARAM
 * never written and whose address is never taken (a constant input).  A VAR is
 * memory, and a multi-def TEMP (regalloc-fallback reuse) can vary — both rejected. */
static int so_stable(IRSSAOptCtx *ctx, int32_t vr)
{
  int ty = TCCIR_DECODE_VREG_TYPE(vr);
  if (ty == TCCIR_VREG_TYPE_TEMP)
  {
    IRSSAVregInfo *vi = ssa_opt_vinfo(ctx, vr);
    return vi && vi->def_count <= 1;
  }
  if (ty == TCCIR_VREG_TYPE_PARAM)
  {
    TCCIRState *ir = ctx->ir;
    int n = ir->next_instruction_index;
    return !so_vreg_ever_written(ir, vr) &&
           !ir_opt_vreg_address_taken_between(ir, vr, -1, n);
  }
  return 0;
}

/* Snapshot a CMP operand: plain immediate, or a single-valued vreg (so_stable). */
static int so_snapshot(IRSSAOptCtx *ctx, IROperand op, int32_t *out_vr, int64_t *out_imm)
{
  if (irop_is_plain_imm(op))
  {
    *out_vr = -1;
    *out_imm = irop_get_imm64_ex(ctx->ir, op);
    return 1;
  }
  int32_t vr = irop_get_vreg(op);
  if (vr < 0 || op.is_lval || op.is_sym || !so_stable(ctx, vr))
    return 0;
  *out_vr = vr;
  *out_imm = 0;
  return 1;
}

static int so_compatible(const SoBool *a, const SoBool *b)
{
  if (a->is_unsigned != b->is_unsigned)
    return 0;
  if (a->s1_vr != b->s1_vr || a->s2_vr != b->s2_vr)
    return 0;
  if (a->s1_vr < 0 && a->s1_imm != b->s1_imm)
    return 0;
  if (a->s2_vr < 0 && a->s2_imm != b->s2_imm)
    return 0;
  return 1;
}

/* Bool-info of a SETIF: its state mask over the CMP immediately preceding it. */
static SoBool so_from_setif(IRSSAOptCtx *ctx, int setif_idx, IRQuadCompact *sq)
{
  TCCIRState *ir = ctx->ir;
  int ci = setif_idx - 1;
  while (ci >= 0 && ir->compact_instructions[ci].op == TCCIR_OP_NOP)
    ci--;
  if (ci < 0 || ir->compact_instructions[ci].op != TCCIR_OP_CMP)
    return so_fail();
  IRQuadCompact *cq = &ir->compact_instructions[ci];
  IROperand cs1 = tcc_ir_op_get_src1(ir, cq);
  IROperand cs2 = tcc_ir_op_get_src2(ir, cq);
  int bt1 = irop_get_btype(cs1), bt2 = irop_get_btype(cs2);
  if (bt1 == IROP_BTYPE_FLOAT32 || bt1 == IROP_BTYPE_FLOAT64 ||
      bt2 == IROP_BTYPE_FLOAT32 || bt2 == IROP_BTYPE_FLOAT64)
    return so_fail();

  int tok = (int)irop_get_imm64_ex(ir, tcc_ir_op_get_src1(ir, sq));
  uint8_t mask = so_cond_to_mask(tok);
  if (mask == 0)
    return so_fail();
  SoBool b;
  b.ok = 1;
  b.mask = mask;
  b.is_unsigned = (uint8_t)so_cond_is_unsigned(tok);
  if (!so_snapshot(ctx, cs1, &b.s1_vr, &b.s1_imm) ||
      !so_snapshot(ctx, cs2, &b.s2_vr, &b.s2_imm))
    return so_fail();
  return b;
}

/* Resolve a TEMP to its boolean state-mask: a SETIF, or an OR of two compatible
 * bool values (recursing through OR-chains via the def-use graph). */
static SoBool so_resolve(IRSSAOptCtx *ctx, int32_t vr, int depth)
{
  if (depth > 16 || vr < 0 || TCCIR_DECODE_VREG_TYPE(vr) != TCCIR_VREG_TYPE_TEMP)
    return so_fail();
  IRSSAVregInfo *vi = ssa_opt_vinfo(ctx, vr);
  if (!vi || vi->def_count != 1 || vi->def_instr < 0)
    return so_fail();
  TCCIRState *ir = ctx->ir;
  IRQuadCompact *q = &ir->compact_instructions[vi->def_instr];
  if (q->op == TCCIR_OP_SETIF)
    return so_from_setif(ctx, vi->def_instr, q);
  if (q->op == TCCIR_OP_OR)
  {
    IROperand s1 = tcc_ir_op_get_src1(ir, q);
    IROperand s2 = tcc_ir_op_get_src2(ir, q);
    if (s1.is_lval || s2.is_lval)
      return so_fail();
    SoBool a = so_resolve(ctx, irop_get_vreg(s1), depth + 1);
    SoBool b = so_resolve(ctx, irop_get_vreg(s2), depth + 1);
    if (!a.ok || !b.ok || !so_compatible(&a, &b))
      return so_fail();
    a.mask = (uint8_t)(a.mask | b.mask);
    return a;
  }
  return so_fail();
}

/* Whether the OR at index i folds to constant 1 (mask union covers LT|EQ|GT). */
static int so_or_is_taut(IRSSAOptCtx *ctx, int i)
{
  TCCIRState *ir = ctx->ir;
  IRQuadCompact *q = &ir->compact_instructions[i];
  IROperand dest = tcc_ir_op_get_dest(ir, q);
  IROperand s1 = tcc_ir_op_get_src1(ir, q);
  IROperand s2 = tcc_ir_op_get_src2(ir, q);
  int32_t dvr = irop_get_vreg(dest);
  if (dvr < 0 || TCCIR_DECODE_VREG_TYPE(dvr) != TCCIR_VREG_TYPE_TEMP || dest.is_lval)
    return 0;
  if (s1.is_lval || s2.is_lval)
    return 0;
  SoBool a = so_resolve(ctx, irop_get_vreg(s1), 0);
  SoBool b = so_resolve(ctx, irop_get_vreg(s2), 0);
  if (!a.ok || !b.ok || !so_compatible(&a, &b))
    return 0;
  return (uint8_t)(a.mask | b.mask) == 0b111;
}

int ssa_opt_setif_or_taut(IRSSAOptCtx *ctx)
{
  TCCIRState *ir = ctx->ir;
  int n = ir->next_instruction_index;
  if (n < 3)
    return 0;

  /* Collect first (reading the original def-use graph), then rewrite, so an
   * inner tautological OR feeding an outer OR still resolves. */
  uint8_t *fold = tcc_mallocz((size_t)n);
  int changes = 0;
  for (int i = 0; i < n; i++)
    if (ir->compact_instructions[i].op == TCCIR_OP_OR && so_or_is_taut(ctx, i))
      fold[i] = 1;

  for (int i = 0; i < n; i++)
  {
    if (!fold[i])
      continue;
    IRQuadCompact *q = &ir->compact_instructions[i];
    IROperand dest = tcc_ir_op_get_dest(ir, q);
    IROperand s1 = tcc_ir_op_get_src1(ir, q);
    IROperand s2 = tcc_ir_op_get_src2(ir, q);
    int btype = irop_get_btype(s1);
    IROperand imm = irop_make_imm32(-1, 1, btype);
    imm.is_unsigned = dest.is_unsigned;
    q->op = TCCIR_OP_ASSIGN;
    tcc_ir_set_src1(ir, i, imm);
    tcc_ir_set_src2(ir, i, IROP_NONE);

    IRSSAVregInfo *v1 = ssa_opt_vinfo(ctx, irop_get_vreg(s1));
    if (v1)
      ssa_opt_remove_use_instr(v1, i);
    IRSSAVregInfo *v2 = ssa_opt_vinfo(ctx, irop_get_vreg(s2));
    if (v2)
      ssa_opt_remove_use_instr(v2, i);
    changes++;
  }

  tcc_free(fold);
  return changes;
}
