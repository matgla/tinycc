/*
 *  TCC IR - SSA DCE: dead overwrite stores (globals)
 *
 *  Copyright (c) 2025 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#define USING_GLOBALS
#include "ir.h"
#include "ssa_opt.h"
#include "dce_common.h"
#include "dce_passes.h"
#include <limits.h>


/* Globals with distinct Sym* never alias each other nor the stack, so (sym, off) keys suffice. */

static int gs_resolve_global_base(IRSSAOptCtx *ctx, int32_t vr,
                                  const Sym **out_sym, int64_t *out_off)
{
  TCCIRState *ir = ctx->ir;
  int64_t acc = 0;
  for (int hop = 0; hop < 64; hop++) {
    if (vr < 0 || TCCIR_DECODE_VREG_TYPE(vr) != TCCIR_VREG_TYPE_TEMP)
      return 0;
    IRSSAVregInfo *vi = ssa_opt_vinfo(ctx, vr);
    if (!vi || vi->def_instr < 0 || vi->def_count > 1)
      return 0;
    IRQuadCompact *dq = &ir->compact_instructions[vi->def_instr];
    if (dq->op == TCCIR_OP_LEA || dq->op == TCCIR_OP_ASSIGN ||
        (dq->op == TCCIR_OP_STORE && !tcc_ir_op_get_dest(ir, dq).is_lval)) {
      IROperand src = tcc_ir_op_get_src1(ir, dq);
      if (src.is_sym && !src.is_lval) {
        IRPoolSymref *sr = irop_get_symref_ex(ir, src);
        if (!sr || !sr->sym)
          return 0;
        *out_sym = sr->sym;
        *out_off = sr->addend + acc;
        return 1;
      }
      int32_t sv = irop_get_vreg(src);
      if (sv >= 0 && !src.is_lval && irop_get_tag(src) == IROP_TAG_VREG) {
        vr = sv;
        continue;
      }
      return 0;
    }
    if (dq->op == TCCIR_OP_ADD || dq->op == TCCIR_OP_SUB) {
      IROperand s1 = tcc_ir_op_get_src1(ir, dq);
      IROperand s2 = tcc_ir_op_get_src2(ir, dq);
      if (!s1.is_lval && irop_is_immediate(s2)) {
        int32_t s1vr = irop_get_vreg(s1);
        if (s1vr >= 0) {
          int d = irop_get_imm32(s2);
          acc += (dq->op == TCCIR_OP_ADD) ? d : -d;
          vr = s1vr;
          continue;
        }
      }
      return 0;
    }
    return 0;
  }
  return 0;
}

typedef struct { const Sym *sym; int64_t off; int width; } GsRef;

/* 2 = exact ref, 1 = whole global (sym only), 0 = not a global, -1 = may alias any global. */
static int gs_classify_read_op(IRSSAOptCtx *ctx, IROperand s, GsRef *r)
{
  if (!s.is_lval)
    return 0;
  TCCIRState *ir = ctx->ir;
  if (s.is_sym) {
    IRPoolSymref *sr = irop_get_symref_ex(ir, s);
    if (!sr || !sr->sym)
      return -1;
    int w = sl_store_byte_width(irop_get_btype(s));
    if (w == 0)
      return -1;
    r->sym = sr->sym;
    r->off = sr->addend;
    r->width = w;
    return 2;
  }
  if (irop_get_tag(s) == IROP_TAG_STACKOFF)
    return 0;
  if (irop_get_tag(s) == IROP_TAG_VREG) {
    int32_t sv = irop_get_vreg(s);
    if (sv >= 0 && TCCIR_DECODE_VREG_TYPE(sv) == TCCIR_VREG_TYPE_TEMP) {
      if (ssa_opt_resolve_lea_stackloc(ctx, sv) != INT_MIN)
        return 0;
      const Sym *gs;
      int64_t go;
      if (gs_resolve_global_base(ctx, sv, &gs, &go)) {
        int w = sl_store_byte_width(irop_get_btype(s));
        if (w == 0)
          return -1;
        r->sym = gs;
        r->off = go;
        r->width = w;
        return 2;
      }
    }
    return -1;
  }
  return -1;
}

static int gs_classify_indexed(IRSSAOptCtx *ctx, const IRQuadCompact *q,
                               int side, int width, GsRef *r)
{
  TCCIRState *ir = ctx->ir;
  if (ssa_opt_indirect_stack_offset(ctx, q, side) != INT_MIN)
    return 0;
  IROperand base = (side == SSA_OPT_INDIRECT_DEST) ? tcc_ir_op_get_dest(ir, q)
                                                   : tcc_ir_op_get_src1(ir, q);
  int32_t bvr = irop_get_vreg(base);
  if (bvr < 0)
    return -1;
  if (ssa_opt_resolve_lea_stackloc(ctx, bvr) != INT_MIN)
    return 0;
  const Sym *gs;
  int64_t go;
  if (!gs_resolve_global_base(ctx, bvr, &gs, &go))
    return -1;
  IROperand idx = tcc_ir_op_get_src2(ir, q);
  if (!irop_is_immediate(idx)) {
    r->sym = gs;
    return 1;
  }
  if (width == 0)
    return -1;
  int64_t sc = 0;
  IROperand scop = tcc_ir_op_get_scale(ir, q);
  if (irop_is_immediate(scop))
    sc = irop_get_imm64_ex(ir, scop);
  r->sym = gs;
  r->off = go + (irop_get_imm64_ex(ir, idx) << sc);
  r->width = width;
  return 2;
}

static int gs_classify_store(IRSSAOptCtx *ctx, const IRQuadCompact *q, GsRef *r)
{
  TCCIRState *ir = ctx->ir;
  if (q->op == TCCIR_OP_STORE_INDEXED) {
    int w = sl_store_byte_width(irop_get_btype(tcc_ir_op_get_src1(ir, q)));
    return gs_classify_indexed(ctx, q, SSA_OPT_INDIRECT_DEST, w, r);
  }
  IROperand dest = tcc_ir_op_get_dest(ir, q);
  if (!dest.is_lval)
    return 0;
  if (dest.is_sym) {
    IRPoolSymref *sr = irop_get_symref_ex(ir, dest);
    if (!sr || !sr->sym)
      return -1;
    int w = sl_store_byte_width(irop_get_btype(dest));
    if (w == 0)
      return -1;
    r->sym = sr->sym;
    r->off = sr->addend;
    r->width = w;
    return 2;
  }
  if (irop_get_tag(dest) == IROP_TAG_STACKOFF)
    return 0;
  if (irop_get_tag(dest) == IROP_TAG_VREG) {
    int32_t dv = irop_get_vreg(dest);
    if (dv >= 0 && TCCIR_DECODE_VREG_TYPE(dv) == TCCIR_VREG_TYPE_TEMP) {
      if (ssa_opt_resolve_lea_stackloc(ctx, dv) != INT_MIN)
        return 0;
      const Sym *gs;
      int64_t go;
      if (gs_resolve_global_base(ctx, dv, &gs, &go)) {
        int w = sl_store_byte_width(irop_get_btype(dest));
        if (w == 0)
          return -1;
        r->sym = gs;
        r->off = go;
        r->width = w;
        return 2;
      }
    }
    return -1;
  }
  return -1;
}

int dce_dead_global_stores(IRSSAOptCtx *ctx)
{
  TCCIRState *ir = ctx->ir;
  IRCFG *cfg = ctx->cfg;
  int changes = 0;

  if (!cfg || cfg->num_blocks == 0)
    return 0;

#define GS_PEND_MAX 32
  typedef struct { int idx; const Sym *sym; int64_t off; int width; } GsPending;

  for (int b = 0; b < cfg->num_blocks; b++) {
    IRBasicBlock *bb = &cfg->blocks[b];
    GsPending pending[GS_PEND_MAX];
    int npending = 0;

    for (int i = bb->start_idx; i < bb->end_idx; i++) {
      IRQuadCompact *q = &ir->compact_instructions[i];
      if (q->op == TCCIR_OP_NOP)
        continue;

      int is_store = (q->op == TCCIR_OP_STORE || q->op == TCCIR_OP_STORE_INDEXED);
      int flush = 0;

      /* STOREs are scanned too: the stored value may itself be a global deref. */
      if (!ssa_opt_has_side_effects(q->op) || is_store) {
        for (int side = 0; side < 3 && !flush; side++) {
          IROperand s;
          if (side == 0 && irop_config[q->op].has_src1)
            s = tcc_ir_op_get_src1(ir, q);
          else if (side == 1 && irop_config[q->op].has_src2)
            s = tcc_ir_op_get_src2(ir, q);
          else if (side == 2 && q->op == TCCIR_OP_MLA)
            s = tcc_ir_op_get_accum(ir, q);
          else
            continue;
          GsRef r;
          int k = gs_classify_read_op(ctx, s, &r);
          if (k == -1)
            flush = 1;
          else if (k == 1) {
            for (int p = 0; p < npending;)
              if (pending[p].sym == r.sym)
                pending[p] = pending[--npending];
              else
                p++;
          } else if (k == 2) {
            for (int p = 0; p < npending;)
              if (pending[p].sym == r.sym && r.off < pending[p].off + pending[p].width &&
                  r.off + r.width > pending[p].off)
                pending[p] = pending[--npending];
              else
                p++;
          }
        }
        if (!flush &&
            (q->op == TCCIR_OP_LOAD_INDEXED || q->op == TCCIR_OP_LOAD_POSTINC)) {
          int w = sl_store_byte_width(irop_get_btype(tcc_ir_op_get_dest(ir, q)));
          GsRef r;
          int k = gs_classify_indexed(ctx, q, SSA_OPT_INDIRECT_SRC1, w, &r);
          if (k == -1)
            flush = 1;
          else if (k == 1) {
            for (int p = 0; p < npending;)
              if (pending[p].sym == r.sym)
                pending[p] = pending[--npending];
              else
                p++;
          } else if (k == 2) {
            for (int p = 0; p < npending;)
              if (pending[p].sym == r.sym && r.off < pending[p].off + pending[p].width &&
                  r.off + r.width > pending[p].off)
                pending[p] = pending[--npending];
              else
                p++;
          }
        }
      }

      if (flush) {
        npending = 0;
        continue;
      }

      /* Any side-effecting op we do not model may read or alias a global. */
      if (ssa_opt_has_side_effects(q->op) && !is_store) {
        npending = 0;
        continue;
      }

      if (!is_store)
        continue;

      GsRef r;
      int k = gs_classify_store(ctx, q, &r);
      if (k == 0 || k == 1)
        continue; /* stack / value-def, or runtime-offset global store */
      if (k == -1) {
        npending = 0; /* store through unknown pointer — may alias any global */
        continue;
      }
      for (int p = 0; p < npending;) {
        if (pending[p].sym == r.sym && pending[p].off == r.off &&
            pending[p].width == r.width) {
          ssa_opt_nop_instr(ctx, pending[p].idx);
          changes++;
          pending[p] = pending[--npending];
        } else if (pending[p].sym == r.sym && r.off < pending[p].off + pending[p].width &&
                   r.off + r.width > pending[p].off) {
          pending[p] = pending[--npending];
        } else {
          p++;
        }
      }
      if (npending < GS_PEND_MAX) {
        pending[npending].idx = i;
        pending[npending].sym = r.sym;
        pending[npending].off = r.off;
        pending[npending].width = r.width;
        npending++;
      }
    }
  }

#undef GS_PEND_MAX
  return changes;
}
