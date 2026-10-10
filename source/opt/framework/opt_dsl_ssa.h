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
#include "opt_dsl_helpers.h"

typedef struct {
  int             idx;    /* index of the defining instruction */
  int             op;     /* its opcode */
  IRSSAVregInfo  *vi;     /* vreg info of the linked source operand */
  IROperand       dest;
  IROperand       src1;
  IROperand       src2;
} OptDslPair;

/* vreg info of spec.link's single-def TEMP producer, or NULL on any spec mismatch. */
static inline IRSSAVregInfo *opt_dsl_pair_find(IRSSAOptCtx *ctx, int i, IROptPairSpec spec)
{
  TCCIRState *ir = ctx->ir;
  IRQuadCompact *q = &ir->compact_instructions[i];
  IROperand linked = (spec.link == IR_PAIR_DEF_OF_SRC2)
                       ? tcc_ir_op_get_src2(ir, q)
                       : tcc_ir_op_get_src1(ir, q);
  int32_t vr = irop_get_vreg(linked);
  if (vr < 0 || TCCIR_DECODE_VREG_TYPE(vr) != TCCIR_VREG_TYPE_TEMP)
    return NULL;
  if (linked.is_lval || linked.tag != IROP_TAG_VREG)
    return NULL;
  IRSSAVregInfo *vi = ssa_opt_vinfo(ctx, vr);
  if (!vi || vi->def_instr < 0 || vi->def_count > 1)
    return NULL;
  if (spec.single_use && vi->use_count != 1)
    return NULL;
  if (spec.op >= 0 && ir->compact_instructions[vi->def_instr].op != spec.op)
    return NULL;
  return vi;
}

/* opt_dsl_pair_find, with the producer's index, opcode and operands in *out. */
static inline int opt_dsl_pair_match(IRSSAOptCtx *ctx, int i,
                                     IROptPairSpec spec, OptDslPair *out)
{
  IRSSAVregInfo *vi = opt_dsl_pair_find(ctx, i, spec);
  if (!vi)
    return 0;
  TCCIRState *ir = ctx->ir;
  IRQuadCompact *def = &ir->compact_instructions[vi->def_instr];
  out->idx  = vi->def_instr;
  out->op   = def->op;
  out->vi   = vi;
  out->dest = tcc_ir_op_get_dest(ir, def);
  out->src1 = tcc_ir_op_get_src1(ir, def);
  out->src2 = tcc_ir_op_get_src2(ir, def);
  return 1;
}

/* i now reads new_linked instead of producer pidx; delete_second NOPs the producer, else drops one use edge. */
OPT_DSL_OUTLINE static inline void opt_dsl_pair_release(IRSSAOptCtx *ctx, int i, IRSSAVregInfo *pvi, int pidx,
                                        IROperand new_linked, int delete_second)
{
  IRSSAVregInfo *nvi = ssa_opt_vinfo(ctx, irop_get_vreg(new_linked));
  if (delete_second) {
    pvi->use_count = 0;
    ssa_opt_nop_instr(ctx, pidx);
  } else {
    ssa_opt_remove_use_instr(pvi, i);
  }
  if (nvi)
    ssa_opt_add_use_instr(nvi, i);
}

static inline void opt_dsl_pair_retire(IRSSAOptCtx *ctx, int i,
                                       const OptDslPair *p,
                                       IROperand new_linked, int delete_second)
{
  opt_dsl_pair_release(ctx, i, p->vi, p->idx, new_linked, delete_second);
}

/* Drop instruction i's use edge for *op's vreg (no-op unless a tracked TEMP). */
OPT_DSL_OUTLINE static inline void opt_dsl_drop_use(IRSSAOptCtx *ctx, const IROperand *op, int i)
{
  IRSSAVregInfo *vi = ssa_opt_vinfo(ctx, irop_get_vreg(*op));
  if (vi)
    ssa_opt_remove_use_instr(vi, i);
}

/* Add a use edge for a plain (non-sym) vreg operand *op consumed by instruction i. */
OPT_DSL_OUTLINE static inline void opt_dsl_add_use(IRSSAOptCtx *ctx, const IROperand *op, int i)
{
  if (op->tag != IROP_TAG_VREG || op->is_sym)
    return;
  IRSSAVregInfo *vi = ssa_opt_vinfo(ctx, irop_get_vreg(*op));
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

/* PAIR(...) — after MATCH, bind the SSA def of a source operand (pidx/pop/pvi) or bail the dispatch. */
#define PAIR(...) \
  IRSSAVregInfo *pvi; \
  do { \
    const IROptPairSpec _opt_dsl_ps = { __VA_ARGS__ }; \
    pvi = opt_dsl_pair_find(ctx, i, _opt_dsl_ps); \
  } while (0); \
  if (!pvi) \
    return 0; \
  int pidx = pvi->def_instr; \
  IRQuadCompact *_opt_dsl_pq = &ir->compact_instructions[pidx]; \
  int pop = _opt_dsl_pq->op; \
  (void)pidx; (void)pop

/* PBIND(slot) binds the producer's dest/src1/src2 as pdest/psrc1/psrc2; use it directly after PAIR. */
#define PBIND(name) IROperand p##name = tcc_ir_op_get_##name(ir, _opt_dsl_pq)

/* RETIRE_PAIR(new_linked, delete_second) — after the guards, before REWRITE: move use lists off the producer. */
#define RETIRE_PAIR(new_linked, delete_second) \
  opt_dsl_pair_release(ctx, i, pvi, pidx, (new_linked), (delete_second))
