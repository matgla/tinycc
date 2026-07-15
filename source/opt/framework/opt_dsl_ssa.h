/*
 *  TCC IR — Optimization DSL: SSA cross-instruction (pair) support
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation.
 */

#pragma once

/* SSA-only companion to opt_dsl.h: PAIR() matches a source operand's single SSA
 * def (a def-use peephole's producer) and binds its operands; RETIRE_PAIR()
 * fixes the use lists after a rewrite forwards past the producer.  Include ir.h
 * and ssa_opt.h first.  Do NOT include from flat (pre-SSA) DSL passes — the
 * matchers call the SSA use-def API. */

#include "ssa_opt.h"
#include "opt_dsl_types.h"

typedef struct {
  int             idx;    /* index of the defining instruction */
  int             op;     /* its opcode */
  IRSSAVregInfo  *vi;     /* vreg info of the linked source operand */
  IROperand       dest;
  IROperand       src1;
  IROperand       src2;
} OptDslPair;

/* Resolve spec.link's operand to its single-def TEMP-vreg producer; fills *out
 * and returns 1, or returns 0 when the operand is not a single-def TEMP vreg,
 * its opcode does not match spec.op (>= 0), or single_use is asked and unmet. */
static inline int opt_dsl_pair_match(IRSSAOptCtx *ctx, int i,
                                     IROptPairSpec spec, OptDslPair *out)
{
  TCCIRState *ir = ctx->ir;
  IRQuadCompact *q = &ir->compact_instructions[i];
  IROperand linked = (spec.link == IR_PAIR_DEF_OF_SRC2)
                       ? tcc_ir_op_get_src2(ir, q)
                       : tcc_ir_op_get_src1(ir, q);
  int32_t vr = irop_get_vreg(linked);
  if (vr < 0 || TCCIR_DECODE_VREG_TYPE(vr) != TCCIR_VREG_TYPE_TEMP)
    return 0;
  if (linked.is_lval || linked.tag != IROP_TAG_VREG)
    return 0;
  IRSSAVregInfo *vi = ssa_opt_vinfo(ctx, vr);
  if (!vi || vi->def_instr < 0 || vi->def_count > 1)
    return 0;
  if (spec.single_use && vi->use_count != 1)
    return 0;
  IRQuadCompact *def = &ir->compact_instructions[vi->def_instr];
  if (spec.op >= 0 && def->op != spec.op)
    return 0;
  out->idx  = vi->def_instr;
  out->op   = def->op;
  out->vi   = vi;
  out->dest = tcc_ir_op_get_dest(ir, def);
  out->src1 = tcc_ir_op_get_src1(ir, def);
  out->src2 = tcc_ir_op_get_src2(ir, def);
  return 1;
}

/* Instruction i has stopped using the paired producer's result and now uses
 * new_linked instead.  delete_second: the producer was used only here, so it is
 * dead — zero its use count and NOP it.  Otherwise just drop the one use edge
 * (the producer may survive, or is left for DCE). */
static inline void opt_dsl_pair_retire(IRSSAOptCtx *ctx, int i,
                                       const OptDslPair *p,
                                       IROperand new_linked, int delete_second)
{
  IRSSAVregInfo *nvi = ssa_opt_vinfo(ctx, irop_get_vreg(new_linked));
  if (delete_second) {
    p->vi->use_count = 0;
    ssa_opt_nop_instr(ctx, p->idx);
  } else {
    ssa_opt_remove_use_instr(p->vi, i);
  }
  if (nvi)
    ssa_opt_add_use_instr(nvi, i);
}

/* Drop instruction i's use edge for op's vreg (no-op unless a tracked TEMP). */
static inline void opt_dsl_drop_use(IRSSAOptCtx *ctx, IROperand op, int i)
{
  IRSSAVregInfo *vi = ssa_opt_vinfo(ctx, irop_get_vreg(op));
  if (vi)
    ssa_opt_remove_use_instr(vi, i);
}

/* Add a use edge for a plain (non-sym) vreg operand consumed by instruction i. */
static inline void opt_dsl_add_use(IRSSAOptCtx *ctx, IROperand op, int i)
{
  if (op.tag != IROP_TAG_VREG || op.is_sym)
    return;
  IRSSAVregInfo *vi = ssa_opt_vinfo(ctx, irop_get_vreg(op));
  if (vi)
    ssa_opt_add_use_instr(vi, i);
}

typedef int (*OptDslSSARule)(IRSSAOptCtx *ctx, int i);

/* Run rules against instruction i in table order until one fires — the chain
 * body for ops whose generator is a sequence of OPT_GEN_SSA dispatches. */
static inline int opt_dsl_chain(IRSSAOptCtx *ctx, int i,
                                const OptDslSSARule *rules, int count)
{
  for (int r = 0; r < count; r++) {
    int c = rules[r](ctx, i);
    if (c)
      return c;
  }
  return 0;
}

/* PAIR(...) — after PATTERN, bind the SSA def of a source operand and bail the
 * dispatch on mismatch.  Binds pidx/pop/pvi/pdest/psrc1/psrc2. */
#define PAIR(...) \
  OptDslPair _opt_dsl_pair; \
  do { \
    const IROptPairSpec _opt_dsl_ps = { __VA_ARGS__ }; \
    if (!opt_dsl_pair_match(ctx, i, _opt_dsl_ps, &_opt_dsl_pair)) \
      return 0; \
  } while (0); \
  int pidx = _opt_dsl_pair.idx; (void)pidx; \
  int pop = _opt_dsl_pair.op; (void)pop; \
  IRSSAVregInfo *pvi = _opt_dsl_pair.vi; (void)pvi; \
  IROperand pdest = _opt_dsl_pair.dest; (void)pdest; \
  IROperand psrc1 = _opt_dsl_pair.src1; (void)psrc1; \
  IROperand psrc2 = _opt_dsl_pair.src2; (void)psrc2

/* RETIRE_PAIR(new_linked, delete_second) — once all guards pass and just before
 * REWRITE, move the use lists off the paired producer (see opt_dsl_pair_retire). */
#define RETIRE_PAIR(new_linked, delete_second) \
  opt_dsl_pair_retire(ctx, i, &_opt_dsl_pair, (new_linked), (delete_second))

