/*
 *  TCC IR - SSA boolean re-normalization elimination (ssa:bool_norm)
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

/* `CMP x,y` + `SETIF EQ/NE` materializing a 0/1 costs a compare, an IT block and
 * two conditional moves.  When x and y are already known to hold 0 or 1 the same
 * answer is one bitwise op: `x != y` is `x ^ y`, `x != 0` is `x` itself.
 *
 * The {0,1} range comes from the value's definition (a SETIF, a `& 1`, or a
 * bitwise combination of such values) and, for a local whose address is never
 * taken, from every store into it — which is what makes `volatile _Bool x = a<b;
 * ... return x ^ y;` cheap: the mandated load still happens, only the redundant
 * re-normalization of its result goes away (gcc PR107881's volatile half). */

#define USING_GLOBALS

#include "ir.h"
#include "ssa_opt.h"
#include "opt_utils.h"
#include "opt_alias.h"
#include "opt/ssa/bool_norm.h"

#define K01_MAX_DEPTH 8
#define K01_MAX_INSTRS 8000

static int k01_operand(IRSSAOptCtx *ctx, IROperand op, int depth, int vars_left);

/* Every write to this local puts a {0,1} value in it, and nothing outside the
 * function can reach it (address never taken, so no pointer, call or asm write
 * aliases it).  Volatile is irrelevant here: this is a range fact about the
 * object, not a claim that a particular load may be forwarded or removed.
 *
 * `read_bytes` is the width the caller reads it at; a writer narrower than that
 * leaves the bytes above it untouched, so the read is not the value stored. */
static int k01_var(IRSSAOptCtx *ctx, int32_t vr, int read_bytes, int vars_left)
{
  TCCIRState *ir = ctx->ir;
  IRLiveInterval *iv = tcc_ir_get_live_interval(ir, vr);
  if (!iv || iv->addrtaken)
    return 0;
  int n = ir->next_instruction_index;
  if (ir_opt_vreg_address_taken_between(ir, vr, -1, n))
    return 0;

  int defs = 0;
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;
    if (q->op == TCCIR_OP_INLINE_ASM)
      return 0;
    if (!irop_config[q->op].has_dest)
      continue;
    IROperand d = tcc_ir_op_get_dest(ir, q);
    if (irop_get_vreg(d) != vr)
      continue;
    /* Any writer other than a plain whole-value assignment (a BLOCK_COPY, a
     * call's sret slot, a partial store) could leave a non-boolean behind. */
    if (q->op != TCCIR_OP_ASSIGN && q->op != TCCIR_OP_STORE)
      return 0;
    int wb = ir_opt_store_btype_size_bytes(irop_get_btype(d));
    if (wb <= 0 || wb > 4 || wb < read_bytes)
      return 0;
    if (!k01_operand(ctx, tcc_ir_op_get_src1(ir, q), 0, vars_left))
      return 0;
    defs++;
  }
  /* No writer at all means the reads see an indeterminate value. */
  return defs > 0;
}

static int k01_temp(IRSSAOptCtx *ctx, int32_t vr, int depth, int vars_left)
{
  IRSSAVregInfo *vi = ssa_opt_vinfo(ctx, vr);
  if (!vi || vi->def_count != 1 || vi->def_instr < 0)
    return 0;
  TCCIRState *ir = ctx->ir;
  if (vi->def_instr >= ir->next_instruction_index)
    return 0;
  IRQuadCompact *q = &ir->compact_instructions[vi->def_instr];
  IROperand s1 = tcc_ir_op_get_src1(ir, q);
  IROperand s2 = tcc_ir_op_get_src2(ir, q);

  switch (q->op)
  {
  case TCCIR_OP_SETIF:
    return 1;
  case TCCIR_OP_ASSIGN:
  case TCCIR_OP_LOAD:
    return k01_operand(ctx, s1, depth + 1, vars_left);
  case TCCIR_OP_AND:
    /* `v & 1` is boolean whatever v holds. */
    if ((irop_is_plain_imm(s1) && !s1.is_sym && irop_get_imm64_ex(ir, s1) == 1) ||
        (irop_is_plain_imm(s2) && !s2.is_sym && irop_get_imm64_ex(ir, s2) == 1))
      return 1;
    /* fall through */
  case TCCIR_OP_OR:
  case TCCIR_OP_XOR:
    return k01_operand(ctx, s1, depth + 1, vars_left) &&
           k01_operand(ctx, s2, depth + 1, vars_left);
  default:
    return 0;
  }
}

/* Whether this operand's value is provably 0 or 1. */
static int k01_operand(IRSSAOptCtx *ctx, IROperand op, int depth, int vars_left)
{
  if (depth > K01_MAX_DEPTH)
    return 0;
  if (irop_is_plain_imm(op) && !op.is_sym)
  {
    int64_t v = irop_get_imm64_ex(ctx->ir, op);
    return v == 0 || v == 1;
  }
  int32_t vr = irop_get_vreg(op);
  if (vr < 0 || op.is_sym)
    return 0;
  int ty = TCCIR_DECODE_VREG_TYPE(vr);
  if (ty == TCCIR_VREG_TYPE_TEMP)
  {
    /* A deref THROUGH a temp reads memory this analysis says nothing about. */
    return op.is_lval ? 0 : k01_temp(ctx, vr, depth, vars_left);
  }
  if (ty == TCCIR_VREG_TYPE_VAR)
  {
    int rb = ir_opt_store_btype_size_bytes(irop_get_btype(op));
    if (rb <= 0 || rb > 4)
      return 0;
    return vars_left > 0 ? k01_var(ctx, vr, rb, vars_left - 1) : 0;
  }
  return 0;
}

/* The SETIF consuming this CMP's flags, or -1.  A second flag reader downstream
 * would lose the flags this rewrite stops producing. */
static int bn_setif_consumer(TCCIRState *ir, int cmp_idx)
{
  int n = ir->next_instruction_index;
  int j = cmp_idx + 1;
  while (j < n && ir->compact_instructions[j].op == TCCIR_OP_NOP)
    j++;
  if (j >= n || ir->compact_instructions[j].is_jump_target)
    return -1;
  if (ir->compact_instructions[j].op != TCCIR_OP_SETIF)
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

static void bn_drop_use(IRSSAOptCtx *ctx, IROperand op, int idx)
{
  int32_t vr = irop_get_vreg(op);
  IRSSAVregInfo *vi = (vr >= 0) ? ssa_opt_vinfo(ctx, vr) : NULL;
  if (vi)
    ssa_opt_remove_use_instr(vi, idx);
}

static void bn_add_use(IRSSAOptCtx *ctx, IROperand op, int idx)
{
  int32_t vr = irop_get_vreg(op);
  IRSSAVregInfo *vi = (vr >= 0) ? ssa_opt_vinfo(ctx, vr) : NULL;
  if (vi)
    ssa_opt_add_use_instr(vi, idx);
}

static int bn_is_imm(TCCIRState *ir, IROperand op, int64_t want)
{
  return irop_is_plain_imm(op) && !op.is_sym && irop_get_imm64_ex(ir, op) == want;
}

/* Retarget a SETIF (dest+src1) as a binary op (dest+src1+src2).  The pool packs
 * operands densely per opcode, so the third slot belongs to the NEXT
 * instruction — the block has to be reallocated at the pool tail. */
static void bn_become_binop(TCCIRState *ir, int idx, TccIrOp op,
                            IROperand dest, IROperand s1, IROperand s2)
{
  int lb = tcc_ir_pool_add(ir, dest);
  tcc_ir_pool_add(ir, s1);
  tcc_ir_pool_add(ir, s2);
  IRQuadCompact *q = &ir->compact_instructions[idx];
  q->op = op;
  q->operand_base = (uint32_t)lb;
}

static int bn_fold(IRSSAOptCtx *ctx, int i)
{
  TCCIRState *ir = ctx->ir;
  IRQuadCompact *q = &ir->compact_instructions[i];

  int j = bn_setif_consumer(ir, i);
  if (j < 0)
    return 0;
  IRQuadCompact *sq = &ir->compact_instructions[j];

  int tok = (int)irop_get_imm64_ex(ir, tcc_ir_op_get_src1(ir, sq));
  if (tok != TOK_EQ && tok != TOK_NE)
    return 0;

  IROperand a = tcc_ir_op_get_src1(ir, q);
  IROperand b = tcc_ir_op_get_src2(ir, q);
  /* Embedded derefs would move a memory read into the rewritten op; keep the
   * rewrite to plain register/immediate operands. */
  if (a.is_lval || b.is_lval)
    return 0;
  if (!k01_operand(ctx, a, 0, 1) || !k01_operand(ctx, b, 0, 1))
    return 0;

  /* `x == 0` / `x != 1` is the complement, and needs the XOR-with-1 form. */
  int a_const = bn_is_imm(ir, a, 0) || bn_is_imm(ir, a, 1);
  int b_const = bn_is_imm(ir, b, 0) || bn_is_imm(ir, b, 1);
  if (a_const && b_const)
    return 0;
  if (a_const)
  {
    IROperand t = a;
    a = b;
    b = t;
    int tc = a_const;
    a_const = b_const;
    b_const = tc;
  }

  IROperand dest = tcc_ir_op_get_dest(ir, sq);
  TccIrOp new_op;
  IROperand ns1 = a, ns2 = b;

  if (b_const)
  {
    int b_is_one = bn_is_imm(ir, b, 1);
    /* `x != 0` and `x == 1` are x; `x == 0` and `x != 1` are `x ^ 1`. */
    int identity = (tok == TOK_NE) ? !b_is_one : b_is_one;
    if (identity)
    {
      new_op = TCCIR_OP_ASSIGN;
      ns2 = IROP_NONE;
    }
    else
    {
      new_op = TCCIR_OP_XOR;
      ns2 = irop_make_imm32(-1, 1, irop_get_btype(a));
    }
  }
  else if (tok == TOK_NE)
  {
    new_op = TCCIR_OP_XOR;
  }
  else
  {
    /* `x == y` is `(x ^ y) ^ 1`: two ops, and only one slot is free here. */
    return 0;
  }

  bn_drop_use(ctx, tcc_ir_op_get_src1(ir, q), i);
  bn_drop_use(ctx, tcc_ir_op_get_src2(ir, q), i);
  q->op = TCCIR_OP_NOP;

  if (new_op == TCCIR_OP_ASSIGN)
  {
    /* Same operand-slot shape as SETIF, so the block can stay put. */
    sq->op = new_op;
    tcc_ir_set_src1(ir, j, ns1);
    tcc_ir_op_set_dest(ir, sq, dest);
  }
  else
  {
    bn_become_binop(ir, j, new_op, dest, ns1, ns2);
  }
  bn_add_use(ctx, ns1, j);
  bn_add_use(ctx, ns2, j);
  return 1;
}

int ssa_opt_bool_norm(IRSSAOptCtx *ctx)
{
  TCCIRState *ir = ctx->ir;
  int n = ir->next_instruction_index;
  if (n < 2 || n > K01_MAX_INSTRS)
    return 0;

  int changes = 0;
  for (int i = 0; i < n - 1; i++)
  {
    if (ir->compact_instructions[i].op != TCCIR_OP_CMP)
      continue;
    changes += bn_fold(ctx, i);
  }
  return changes;
}
