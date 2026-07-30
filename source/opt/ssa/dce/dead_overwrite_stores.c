/*
 *  TCC IR - SSA DCE: dead overwrite stores (stack slots)
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


/* Distinct `&VAR` objects all share offset 0, so aliasing needs (out_base, off) to match. */
static int sl_resolve_store_offset(IRSSAOptCtx *ctx, int instr_idx,
                                   int32_t *out_base, int *out_off, int *out_width,
                                   int *out_unknown)
{
  TCCIRState *ir = ctx->ir;
  IRQuadCompact *q = &ir->compact_instructions[instr_idx];
  IROperand dest = tcc_ir_op_get_dest(ir, q);
  *out_unknown = 0;
  *out_base = -1;

  if (q->op == TCCIR_OP_STORE_INDEXED) {
    int32_t bv = -1;
    int eff = ssa_opt_indirect_stack_offset_ex(ctx, q, SSA_OPT_INDIRECT_DEST, &bv);
    if (eff == INT_MIN) {
      *out_unknown = 1;
      return 0;
    }
    /* Width comes from the stored value: the dest btype is the pointer's, not the access. */
    IROperand src1 = tcc_ir_op_get_src1(ir, q);
    int w = sl_store_byte_width(irop_get_btype(src1));
    if (w == 0) {
      *out_unknown = 1;
      return 0;
    }
    *out_base = bv;
    *out_off = eff;
    *out_width = w;
    return 1;
  }

  if (q->op != TCCIR_OP_STORE)
    return 0;

  if (dest.tag == IROP_TAG_STACKOFF && dest.is_lval && dest.is_local && !dest.is_llocal) {
    int32_t dv = irop_get_vreg(dest);
    if (dv < 0 || TCCIR_DECODE_VREG_TYPE(dv) != TCCIR_VREG_TYPE_VAR) {
      int w = sl_store_byte_width(irop_get_btype(dest));
      if (w == 0) {
        *out_unknown = 1;
        return 0;
      }
      *out_off = irop_get_stack_offset(dest);
      *out_width = w;
      return 1;
    }
  }

  if (dest.tag == IROP_TAG_VREG && dest.is_lval) {
    int32_t dv = irop_get_vreg(dest);
    if (dv >= 0 && TCCIR_DECODE_VREG_TYPE(dv) == TCCIR_VREG_TYPE_TEMP) {
      int32_t bv = -1;
      int eff = ssa_opt_resolve_lea_stackloc_ex(ctx, dv, &bv);
      if (eff != INT_MIN) {
        int w = sl_store_byte_width(irop_get_btype(dest));
        if (w == 0) {
          *out_unknown = 1;
          return 0;
        }
        *out_base = bv;
        *out_off = eff;
        *out_width = w;
        return 1;
      }
    }
    *out_unknown = 1;
    return 0;
  }

  *out_unknown = 1;
  return 0;
}

static int sl_resolve_load_offset(IRSSAOptCtx *ctx, int instr_idx,
                                  int *out_off, int *out_width, int *out_unknown)
{
  TCCIRState *ir = ctx->ir;
  IRQuadCompact *q = &ir->compact_instructions[instr_idx];
  IROperand src1 = tcc_ir_op_get_src1(ir, q);
  *out_unknown = 0;

  if (q->op == TCCIR_OP_LOAD_INDEXED) {
    int eff = ssa_opt_indirect_stack_offset(ctx, q, SSA_OPT_INDIRECT_SRC1);
    if (eff == INT_MIN) {
      *out_unknown = 1;
      return 0;
    }
    int w = sl_store_byte_width(irop_get_btype(tcc_ir_op_get_dest(ir, q)));
    if (w == 0) {
      *out_unknown = 1;
      return 0;
    }
    *out_off = eff;
    *out_width = w;
    return 1;
  }

  if (q->op != TCCIR_OP_LOAD &&
      !(q->op == TCCIR_OP_ASSIGN && src1.is_lval))
    return 0;

  if (src1.tag == IROP_TAG_STACKOFF && src1.is_lval && src1.is_local && !src1.is_llocal) {
    int32_t sv = irop_get_vreg(src1);
    if (sv < 0 || TCCIR_DECODE_VREG_TYPE(sv) != TCCIR_VREG_TYPE_VAR) {
      int w = sl_store_byte_width(irop_get_btype(src1));
      if (w == 0) {
        *out_unknown = 1;
        return 0;
      }
      *out_off = irop_get_stack_offset(src1);
      *out_width = w;
      return 1;
    }
  }

  if (src1.tag == IROP_TAG_VREG && src1.is_lval) {
    int32_t sv = irop_get_vreg(src1);
    if (sv >= 0 && TCCIR_DECODE_VREG_TYPE(sv) == TCCIR_VREG_TYPE_TEMP) {
      int eff = ssa_opt_resolve_lea_stackloc(ctx, sv);
      if (eff != INT_MIN) {
        int w = sl_store_byte_width(irop_get_btype(tcc_ir_op_get_dest(ir, q)));
        if (w == 0) {
          *out_unknown = 1;
          return 0;
        }
        *out_off = eff;
        *out_width = w;
        return 1;
      }
    }
    *out_unknown = 1;
    return 0;
  }

  *out_unknown = 1;
  return 0;
}

int dce_dead_overwrite_stores(IRSSAOptCtx *ctx)
{
  TCCIRState *ir = ctx->ir;
  IRCFG *cfg = ctx->cfg;
  int n = ir->next_instruction_index;
  int changes = 0;

  if (!cfg || cfg->num_blocks == 0 || n == 0)
    return 0;

#define DOS_PEND_MAX 32
  typedef struct { int idx; int32_t base; int off; int width; } DosPending;

  for (int b = 0; b < cfg->num_blocks; b++) {
    IRBasicBlock *bb = &cfg->blocks[b];
    DosPending pending[DOS_PEND_MAX];
    int npending = 0;

    for (int i = bb->start_idx; i < bb->end_idx; i++) {
      IRQuadCompact *q = &ir->compact_instructions[i];
      if (q->op == TCCIR_OP_NOP)
        continue;

      /* Calls and terminators may touch arbitrary memory through escaped pointers. */
      if (q->op == TCCIR_OP_FUNCCALLVOID || q->op == TCCIR_OP_FUNCCALLVAL ||
          q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_JUMPIF ||
          q->op == TCCIR_OP_IJUMP || q->op == TCCIR_OP_SWITCH_TABLE ||
          q->op == TCCIR_OP_RETURNVALUE || q->op == TCCIR_OP_RETURNVOID) {
        npending = 0;
        continue;
      }

      /* Any is_lval source is a memory read; an unresolvable one may alias any store. */
      int saw_unresolved_deref = 0;
      for (int side = 0; side < 2; side++) {
        if (side == 0 && !irop_config[q->op].has_src1)
          continue;
        if (side == 1 && !irop_config[q->op].has_src2)
          continue;
        IROperand s = side ? tcc_ir_op_get_src2(ir, q) : tcc_ir_op_get_src1(ir, q);
        if (!s.is_lval)
          continue;
        int eff = INT_MIN;
        int width = sl_store_byte_width(irop_get_btype(s));
        if (s.tag == IROP_TAG_STACKOFF && s.is_local && !s.is_llocal) {
          int32_t sv = irop_get_vreg(s);
          if (sv < 0 || TCCIR_DECODE_VREG_TYPE(sv) != TCCIR_VREG_TYPE_VAR)
            eff = irop_get_stack_offset(s);
        } else if (s.tag == IROP_TAG_VREG) {
          int32_t sv = irop_get_vreg(s);
          if (sv >= 0 && TCCIR_DECODE_VREG_TYPE(sv) == TCCIR_VREG_TYPE_TEMP)
            eff = ssa_opt_resolve_lea_stackloc(ctx, sv);
        }
        if (eff == INT_MIN || width == 0) {
          saw_unresolved_deref = 1;
          break;
        }
        for (int k = 0; k < npending;) {
          int po = pending[k].off, pw = pending[k].width;
          if (eff < po + pw && eff + width > po)
            pending[k] = pending[--npending];
          else
            k++;
        }
      }
      if (saw_unresolved_deref) {
        npending = 0;
        continue;
      }

      /* Pointer+index form: src1 is a non-lval base, so the sweep above misses it. */
      if (q->op == TCCIR_OP_LOAD_INDEXED || q->op == TCCIR_OP_LOAD_POSTINC) {
        int lo = 0, lw = 0, lu = 0;
        if (sl_resolve_load_offset(ctx, i, &lo, &lw, &lu)) {
          for (int k = 0; k < npending;) {
            int po = pending[k].off, pw = pending[k].width;
            if (lo < po + pw && lo + lw > po)
              pending[k] = pending[--npending];
            else
              k++;
          }
        } else if (lu) {
          npending = 0;
        }
        continue;
      }

      /* Plain LOAD was already handled by the source-side sweep. */
      if (q->op == TCCIR_OP_LOAD)
        continue;

      if (q->op == TCCIR_OP_STORE || q->op == TCCIR_OP_STORE_INDEXED) {
        int so = 0, sw = 0, su = 0;
        int32_t sbase = -1;
        (void)su;
        int resolved = sl_resolve_store_offset(ctx, i, &sbase, &so, &sw, &su);
        if (!resolved) {
          /* An unresolved store goes through an external pointer: never a local slot. */
          continue;
        }

        for (int k = 0; k < npending;) {
          if (pending[k].base != sbase) {
            k++;
            continue;
          }
          int po = pending[k].off, pw = pending[k].width;
          if (po == so && pw == sw) {
            ssa_opt_nop_instr(ctx, pending[k].idx);
            changes++;
            pending[k] = pending[--npending];
          } else if (so < po + pw && so + sw > po) {
            pending[k] = pending[--npending];
          } else {
            k++;
          }
        }

        if (npending < DOS_PEND_MAX) {
          pending[npending].idx = i;
          pending[npending].base = sbase;
          pending[npending].off = so;
          pending[npending].width = sw;
          npending++;
        }
        continue;
      }

      /* STORE_POSTINC updates its base, so tracked offsets become unreliable. */
      if (q->op == TCCIR_OP_STORE_POSTINC) {
        npending = 0;
        continue;
      }

      /* Other ops may take a stack address as a value (BLOCK_COPY) — treat as a read. */
      for (int side = 0; side < 2; side++) {
        if (side == 0 && !irop_config[q->op].has_src1)
          continue;
        if (side == 1 && !irop_config[q->op].has_src2)
          continue;
        IROperand s = side ? tcc_ir_op_get_src2(ir, q) : tcc_ir_op_get_src1(ir, q);
        if (s.is_lval || s.tag != IROP_TAG_VREG)
          continue;
        int32_t sv = irop_get_vreg(s);
        if (sv < 0 || TCCIR_DECODE_VREG_TYPE(sv) != TCCIR_VREG_TYPE_TEMP)
          continue;
        int eff = ssa_opt_resolve_lea_stackloc(ctx, sv);
        if (eff == INT_MIN)
          continue;
        /* Access size is unknown, so evict everything within a conservative window. */
        for (int k = 0; k < npending;) {
          int po = pending[k].off, pw = pending[k].width;
          if (po + pw > eff && po < eff + 256)
            pending[k] = pending[--npending];
          else
            k++;
        }
      }
    }
  }

#undef DOS_PEND_MAX
  return changes;
}
