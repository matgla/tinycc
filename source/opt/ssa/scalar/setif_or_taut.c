/*
 *  TCC IR - SSA SETIF state-mask algebra (ssa:setif_or_taut, ssa:setif_mask_fold)
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

/* A SETIF result is a boolean carrying a {LT,EQ,GT} state mask of its CMP.  Two
 * such booleans over the SAME compared operands live in a closed 3-state
 * algebra: their OR unions the masks, AND intersects them, XOR symmetric-
 * differences them, and every resulting mask maps back to a single condition
 * (0b111 = always true, 0b000 = always false).  Single assignment lets a
 * recursive def-use resolve carry a mask through whole bool-expression chains.
 *
 * Two passes are built on that:
 *   ssa:setif_or_taut    - an OR whose union covers all three states folds to 1.
 *   ssa:setif_mask_fold  - a CMP whose operands are such booleans collapses the
 *                          whole chain to ONE compare of the original operands
 *                          (gcc PR107881: `(a<b) ^ (a>b)`  ==>  `a != b`). */

#define USING_GLOBALS

#include "ir.h"
#include "ssa_opt.h"
#include "opt_utils.h"
#include "opt/ssa/setif_or_taut.h"
#include <limits.h>

typedef struct
{
  int ok;
  int is_const;           /* a literal 0/1 boolean: mask is 0b000 / 0b111 */
  int all_single_use;     /* every value in the resolved chain has one use */
  int max_def;            /* highest instruction index the chain reads at */
  int32_t s1_vr, s2_vr;   /* compared operands (-1 = immediate) */
  int64_t s1_imm, s2_imm;
  IROperand s1_op, s2_op; /* the compared operands verbatim, to re-emit the CMP */
  uint8_t mask;           /* {GT,EQ,LT} bits */
  uint8_t is_unsigned;
} SoBool;

static SoBool so_fail(void) { SoBool b; memset(&b, 0, sizeof(b)); return b; }

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

/* Two snapshots name the same value AND the same view of it: a narrower btype or
 * a different signedness on the operand asks about different bits. */
static int so_operand_same(IROperand a, IROperand b)
{
  return irop_get_btype(a) == irop_get_btype(b) && a.is_unsigned == b.is_unsigned &&
         a.is_lval == b.is_lval && a.is_sym == b.is_sym;
}

static int so_compatible(const SoBool *a, const SoBool *b)
{
  /* A constant boolean carries no operands, so it pairs with anything. */
  if (a->is_const || b->is_const)
    return 1;
  if (a->is_unsigned != b->is_unsigned)
    return 0;
  if (a->s1_vr != b->s1_vr || a->s2_vr != b->s2_vr)
    return 0;
  if (a->s1_vr < 0 && a->s1_imm != b->s1_imm)
    return 0;
  if (a->s2_vr < 0 && a->s2_imm != b->s2_imm)
    return 0;
  return so_operand_same(a->s1_op, b->s1_op) && so_operand_same(a->s2_op, b->s2_op);
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
  SoBool b = so_fail();
  b.ok = 1;
  b.all_single_use = 1;
  b.max_def = ci;
  b.mask = mask;
  b.is_unsigned = (uint8_t)so_cond_is_unsigned(tok);
  b.s1_op = cs1;
  b.s2_op = cs2;
  if (!so_snapshot(ctx, cs1, &b.s1_vr, &b.s1_imm) ||
      !so_snapshot(ctx, cs2, &b.s2_vr, &b.s2_imm))
    return so_fail();
  return b;
}

/* Merge two compatible bool values under `mask`; the operands come from whichever
 * side is not a bare constant (a constant carries none). */
static SoBool so_merge(const SoBool *a, const SoBool *b, uint8_t mask)
{
  SoBool r = a->is_const ? *b : *a;
  r.mask = (uint8_t)(mask & 7);
  r.all_single_use = a->all_single_use && b->all_single_use;
  r.max_def = (a->max_def > b->max_def) ? a->max_def : b->max_def;
  return r;
}

/* Uses of `vr` that a live (non-NOP) instruction or a phi still makes. */
static int so_live_use_count(IRSSAOptCtx *ctx, int32_t vr)
{
  IRSSAVregInfo *vi = ssa_opt_vinfo(ctx, vr);
  if (!vi)
    return INT_MAX;
  TCCIRState *ir = ctx->ir;
  int n = ir->next_instruction_index;
  int count = 0;
  for (int k = 0; k < vi->use_count; k++)
  {
    IRSSAUse *u = &vi->uses[k];
    if (u->kind == SSA_USE_PHI)
      count++;
    else if (u->idx >= 0 && u->idx < n && ir->compact_instructions[u->idx].op != TCCIR_OP_NOP)
      count++;
  }
  return count;
}

static SoBool so_resolve_operand(IRSSAOptCtx *ctx, IROperand op, int depth);

/* Resolve a TEMP to its boolean state-mask: a SETIF, or a bitwise combination of
 * two compatible bool values (recursing through the def-use graph).  AND/OR/XOR
 * of two 0/1 values stay 0/1, so the mask algebra composes without limit. */
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
  {
    SoBool b = so_from_setif(ctx, vi->def_instr, q);
    if (b.ok && so_live_use_count(ctx, vr) > 1)
      b.all_single_use = 0;
    return b;
  }
  if (q->op == TCCIR_OP_OR || q->op == TCCIR_OP_AND || q->op == TCCIR_OP_XOR)
  {
    IROperand s1 = tcc_ir_op_get_src1(ir, q);
    IROperand s2 = tcc_ir_op_get_src2(ir, q);
    if (s1.is_lval || s2.is_lval)
      return so_fail();
    SoBool a = so_resolve_operand(ctx, s1, depth + 1);
    SoBool b = so_resolve_operand(ctx, s2, depth + 1);
    if (!a.ok || !b.ok || !so_compatible(&a, &b))
      return so_fail();
    uint8_t m = (q->op == TCCIR_OP_OR)    ? (uint8_t)(a.mask | b.mask)
                : (q->op == TCCIR_OP_AND) ? (uint8_t)(a.mask & b.mask)
                                          : (uint8_t)(a.mask ^ b.mask);
    SoBool r = so_merge(&a, &b, m);
    if (r.ok && so_live_use_count(ctx, vr) > 1)
      r.all_single_use = 0;
    return r;
  }
  return so_fail();
}

/* A boolean operand: a literal 0/1 (masks 0b000 / 0b111), or a resolvable vreg. */
static SoBool so_resolve_operand(IRSSAOptCtx *ctx, IROperand op, int depth)
{
  if (op.is_lval)
    return so_fail();
  if (irop_is_plain_imm(op) && !op.is_sym)
  {
    int64_t v = irop_get_imm64_ex(ctx->ir, op);
    if (v != 0 && v != 1)
      return so_fail();
    SoBool b = so_fail();
    b.ok = 1;
    b.is_const = 1;
    b.all_single_use = 1;
    b.mask = v ? 0b111 : 0b000;
    return b;
  }
  return so_resolve(ctx, irop_get_vreg(op), depth);
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

/* ==========================================================================
 * ssa:setif_mask_fold - collapse a bool-expression chain to a single compare
 * ========================================================================== */

/* Inverse of so_cond_to_mask; 0b000 / 0b111 are the constant cases, which the
 * caller must have handled (no condition denotes them). */
static int so_mask_to_cond(uint8_t mask, int is_unsigned)
{
  switch (mask & 7)
  {
  case 0b010: return TOK_EQ;
  case 0b101: return TOK_NE;
  case 0b001: return is_unsigned ? TOK_ULT : TOK_LT;
  case 0b110: return is_unsigned ? TOK_UGE : TOK_GE;
  case 0b011: return is_unsigned ? TOK_ULE : TOK_LE;
  case 0b100: return is_unsigned ? TOK_UGT : TOK_GT;
  default: return 0;
  }
}

/* `x <tok> y` for two booleans, as a state mask.  Both live in {0,1}, so the
 * relational predicates reduce to boolean connectives (`x < y` is `!x && y`)
 * and signed/unsigned agree. */
static int so_combine_masks(int tok, uint8_t ma, uint8_t mb, uint8_t *out)
{
  uint8_t m;
  switch (tok)
  {
  case TOK_EQ:                  m = (uint8_t)~(ma ^ mb); break;
  case TOK_NE:                  m = (uint8_t)(ma ^ mb);  break;
  case TOK_LT: case TOK_ULT:    m = (uint8_t)(~ma & mb); break;
  case TOK_GT: case TOK_UGT:    m = (uint8_t)(ma & ~mb); break;
  case TOK_LE: case TOK_ULE:    m = (uint8_t)(~ma | mb); break;
  case TOK_GE: case TOK_UGE:    m = (uint8_t)(ma | ~mb); break;
  default: return 0;
  }
  *out = (uint8_t)(m & 7);
  return 1;
}

/* The instruction that consumes this CMP's flags, or -1.  A SECOND reader would
 * see the flags of a compare this fold either retargets or deletes, so a CMP
 * with more than one consumer is left alone. */
static int so_flag_consumer(TCCIRState *ir, int cmp_idx)
{
  int n = ir->next_instruction_index;
  int j = cmp_idx + 1;
  while (j < n && ir->compact_instructions[j].op == TCCIR_OP_NOP)
    j++;
  if (j >= n || ir->compact_instructions[j].is_jump_target)
    return -1;
  TccIrOp op = ir->compact_instructions[j].op;
  if (op != TCCIR_OP_SETIF && op != TCCIR_OP_SELECT && op != TCCIR_OP_JUMPIF)
    return -1;
  int k = j + 1;
  while (k < n && ir->compact_instructions[k].op == TCCIR_OP_NOP)
    k++;
  if (k < n)
  {
    TccIrOp next = ir->compact_instructions[k].op;
    if (next == TCCIR_OP_SETIF || next == TCCIR_OP_SELECT || next == TCCIR_OP_JUMPIF)
      return -1;
  }
  return j;
}

static void so_drop_use(IRSSAOptCtx *ctx, IROperand op, int idx)
{
  int32_t vr = irop_get_vreg(op);
  if (vr < 0)
    return;
  IRSSAVregInfo *vi = ssa_opt_vinfo(ctx, vr);
  if (vi)
    ssa_opt_remove_use_instr(vi, idx);
}

static void so_add_use(IRSSAOptCtx *ctx, IROperand op, int idx)
{
  int32_t vr = irop_get_vreg(op);
  if (vr < 0)
    return;
  IRSSAVregInfo *vi = ssa_opt_vinfo(ctx, vr);
  if (vi)
    ssa_opt_add_use_instr(vi, idx);
}

/* `CMP x,y` where x and y are booleans built from compares of the SAME operand
 * pair collapses to one compare of that pair: the whole bool chain is a state
 * mask, and `x <tok> y` is just mask algebra (gcc PR107881).  The now-dead
 * SETIFs and their CMPs are left to ssa:dce. */
static int so_fold_cmp(IRSSAOptCtx *ctx, int i)
{
  TCCIRState *ir = ctx->ir;
  IRQuadCompact *q = &ir->compact_instructions[i];

  int j = so_flag_consumer(ir, i);
  if (j < 0)
    return 0;
  IRQuadCompact *use = &ir->compact_instructions[j];
  int is_select = (use->op == TCCIR_OP_SELECT);
  int is_setif = (use->op == TCCIR_OP_SETIF);

  IROperand cs1 = tcc_ir_op_get_src1(ir, q);
  IROperand cs2 = tcc_ir_op_get_src2(ir, q);
  if (cs1.is_lval || cs2.is_lval)
    return 0;

  SoBool a = so_resolve_operand(ctx, cs1, 0);
  SoBool b = so_resolve_operand(ctx, cs2, 0);
  /* At least one real bool chain, or there is nothing to collapse. */
  if (!a.ok || !b.ok || (a.is_const && b.is_const))
    return 0;
  if (!a.all_single_use || !b.all_single_use)
    return 0;
  if (!so_compatible(&a, &b))
    return 0;

  IROperand cond_op = is_select ? ir->iroperand_pool[use->operand_base + 3]
                                : tcc_ir_op_get_src1(ir, use);
  if (irop_get_tag(cond_op) != IROP_TAG_IMM32)
    return 0;
  uint8_t mask;
  if (!so_combine_masks((int)cond_op.u.imm32, a.mask, b.mask, &mask))
    return 0;

  const SoBool *src = a.is_const ? &b : &a;
  /* The rewritten CMP re-reads the chain's operands HERE, so every compare it
   * was resolved through must already have executed — a backedge-carried chain
   * whose CMP sits textually later would read values that do not exist yet. */
  if (src->max_def >= i)
    return 0;

  if (mask == 0 || mask == 0b111)
  {
    int result = (mask != 0);
    if (is_setif)
    {
      IROperand dest = tcc_ir_op_get_dest(ir, use);
      q->op = TCCIR_OP_NOP;
      use->op = TCCIR_OP_ASSIGN;
      tcc_ir_set_src1(ir, j, irop_make_imm32(-1, result, irop_get_btype(dest)));
      tcc_ir_set_src2(ir, j, IROP_NONE);
      tcc_ir_op_set_dest(ir, use, dest);
    }
    else if (is_select)
    {
      IROperand keep = result ? tcc_ir_op_get_src1(ir, use) : tcc_ir_op_get_src2(ir, use);
      IROperand drop = result ? tcc_ir_op_get_src2(ir, use) : tcc_ir_op_get_src1(ir, use);
      so_drop_use(ctx, drop, j);
      q->op = TCCIR_OP_NOP;
      use->op = TCCIR_OP_ASSIGN;
      tcc_ir_set_src1(ir, j, keep);
      tcc_ir_set_src2(ir, j, IROP_NONE);
    }
    else
    {
      /* JUMPIF: folding the branch away needs phi-edge maintenance; leave the
       * always-taken/never-taken decision to ssa:branch and just skip. */
      return 0;
    }
  }
  else
  {
    int tok = so_mask_to_cond(mask, src->is_unsigned);
    if (!tok)
      return 0;
    /* Nothing to do if this already IS that compare (keeps the pass at fixpoint). */
    if (tok == (int)cond_op.u.imm32 && irop_get_vreg(cs1) == src->s1_vr &&
        irop_get_vreg(cs2) == src->s2_vr)
      return 0;
    tcc_ir_set_src1(ir, i, src->s1_op);
    tcc_ir_set_src2(ir, i, src->s2_op);
    cond_op.u.imm32 = tok;
    if (is_select)
      ir->iroperand_pool[use->operand_base + 3] = cond_op;
    else
      tcc_ir_set_src1(ir, j, cond_op);
    so_add_use(ctx, src->s1_op, i);
    so_add_use(ctx, src->s2_op, i);
  }

  so_drop_use(ctx, cs1, i);
  so_drop_use(ctx, cs2, i);
  return 1;
}

int ssa_opt_setif_mask_fold(IRSSAOptCtx *ctx)
{
  TCCIRState *ir = ctx->ir;
  int n = ir->next_instruction_index;
  if (n < 3)
    return 0;

  int changes = 0;
  for (int i = 0; i < n - 1; i++)
  {
    if (ir->compact_instructions[i].op != TCCIR_OP_CMP)
      continue;
    changes += so_fold_cmp(ctx, i);
  }
  return changes;
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
