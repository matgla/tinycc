/*
 *  TCC IR - SSA Global Load CSE + Stack Store-Load Forwarding
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
#include <limits.h>

/* ============================================================================
 * Dominator-Tree Global Load CSE + Stack Forwarding
 *
 * 1. Deduplicate LOAD instructions from the same GlobalSym with no
 *    intervening aliasing store or function call.
 * 2. Forward stack stores through LEA+DEREF load patterns:
 *    StackLoc[N] <-- T [STORE] ... Ty <-- *Addr[StackLoc[N]] [LOAD] -> Ty = T
 *
 * Uses a dominator-tree walk so stores/loads in the entry block are
 * available to all dominated blocks, while invalidations in error arms
 * don't poison sibling continuation blocks.
 * ============================================================================ */

#define GLOAD_MAX 16
#define SSTORE_MAX 16

typedef struct {
  Sym *sym;
  int64_t addend;
  int btype;
  int32_t result_vr;
} GLoadEntry;

typedef struct {
  int stack_offset;
  int btype;
  int32_t stored_vr;    /* TEMP vreg, or -1 if immediate */
  IROperand stored_imm; /* valid when stored_vr == -1 */
} SStoreEntry;

typedef struct {
  GLoadEntry entries[GLOAD_MAX];
  int count;
  SStoreEntry sstores[SSTORE_MAX];
  int scount;
} GLoadState;

static int gload_find(const GLoadState *st, Sym *sym, int64_t addend, int btype)
{
  for (int k = 0; k < st->count; k++) {
    if (st->entries[k].sym == sym && st->entries[k].addend == addend &&
        st->entries[k].btype == btype)
      return k;
  }
  return -1;
}

static void gload_track(GLoadState *st, Sym *sym, int64_t addend, int btype,
                         int32_t result_vr)
{
  if (st->count >= GLOAD_MAX)
    return;
  GLoadEntry *e = &st->entries[st->count++];
  e->sym = sym;
  e->addend = addend;
  e->btype = btype;
  e->result_vr = result_vr;
}

static void gload_remove_vr(GLoadState *st, int32_t vr)
{
  for (int k = 0; k < st->count; k++) {
    if (st->entries[k].result_vr == vr) {
      st->entries[k] = st->entries[--st->count];
      return;
    }
  }
}

static int sstore_find(const GLoadState *st, int offset)
{
  for (int k = 0; k < st->scount; k++) {
    if (st->sstores[k].stack_offset == offset)
      return k;
  }
  return -1;
}

static void sstore_remove_offset(GLoadState *st, int offset)
{
  int k = sstore_find(st, offset);
  if (k >= 0)
    st->sstores[k] = st->sstores[--st->scount];
}

static void sstore_invalidate_overlap(GLoadState *st, int offset, int btype)
{
  int size = (btype == IROP_BTYPE_INT64 || btype == IROP_BTYPE_FLOAT64) ? 8 : 4;
  if (size > 4) {
    for (int w = 4; w < size; w += 4)
      sstore_remove_offset(st, offset + w);
  }
  for (int k = 0; k < st->scount; k++) {
    SStoreEntry *e = &st->sstores[k];
    int esize = (e->btype == IROP_BTYPE_INT64 || e->btype == IROP_BTYPE_FLOAT64) ? 8 : 4;
    if (esize > 4 && e->stack_offset != offset &&
        e->stack_offset < offset + size && e->stack_offset + esize > offset) {
      st->sstores[k] = st->sstores[--st->scount];
      k--;
    }
  }
}

static void sstore_track_vr(GLoadState *st, int offset, int btype, int32_t stored_vr)
{
  sstore_invalidate_overlap(st, offset, btype);
  int k = sstore_find(st, offset);
  if (k >= 0) {
    st->sstores[k].btype = btype;
    st->sstores[k].stored_vr = stored_vr;
    return;
  }
  if (st->scount >= SSTORE_MAX)
    return;
  SStoreEntry *e = &st->sstores[st->scount++];
  e->stack_offset = offset;
  e->btype = btype;
  e->stored_vr = stored_vr;
}

static void sstore_track_imm(GLoadState *st, int offset, int btype, IROperand imm)
{
  sstore_invalidate_overlap(st, offset, btype);
  int k = sstore_find(st, offset);
  if (k >= 0) {
    st->sstores[k].btype = btype;
    st->sstores[k].stored_vr = -1;
    st->sstores[k].stored_imm = imm;
    return;
  }
  if (st->scount >= SSTORE_MAX)
    return;
  SStoreEntry *e = &st->sstores[st->scount++];
  e->stack_offset = offset;
  e->btype = btype;
  e->stored_vr = -1;
  e->stored_imm = imm;
}

static void sstore_remove_vr(GLoadState *st, int32_t vr)
{
  for (int k = 0; k < st->scount; k++) {
    if (st->sstores[k].stored_vr == vr) {
      st->sstores[k] = st->sstores[--st->scount];
      return;
    }
  }
}

/* Resolve a TEMP vreg backward to find if it's Addr[StackLoc[N]].
 * Returns the stack offset, or INT_MIN if not resolvable. */
static int resolve_lea_stackloc(IRSSAOptCtx *ctx, int32_t vr)
{
  if (vr < 0 || TCCIR_DECODE_VREG_TYPE(vr) != TCCIR_VREG_TYPE_TEMP)
    return INT_MIN;
  IRSSAVregInfo *vi = ssa_opt_vinfo(ctx, vr);
  if (!vi || vi->def_instr < 0 || vi->def_count > 1)
    return INT_MIN;
  TCCIRState *ir = ctx->ir;
  IRQuadCompact *dq = &ir->compact_instructions[vi->def_instr];
  if (dq->op == TCCIR_OP_LEA) {
    IROperand src = tcc_ir_op_get_src1(ir, dq);
    if (src.tag == IROP_TAG_STACKOFF || src.is_local)
      return irop_get_stack_offset(src);
  }
  if (dq->op == TCCIR_OP_ASSIGN) {
    IROperand src = tcc_ir_op_get_src1(ir, dq);
    if (src.tag == IROP_TAG_STACKOFF && !src.is_lval)
      return irop_get_stack_offset(src);
    /* Chase through TEMP copies */
    int32_t sv = irop_get_vreg(src);
    if (sv >= 0 && !src.is_lval)
      return resolve_lea_stackloc(ctx, sv);
  }
  return INT_MIN;
}

static int gload_process_block(IRSSAOptCtx *ctx, GLoadState state, int b)
{
  TCCIRState *ir = ctx->ir;
  IRCFG *cfg = ctx->cfg;
  IRBasicBlock *bb = &cfg->blocks[b];
  int changes = 0;

  /* If this block has any predecessor that is NOT its immediate dominator,
   * a non-dominator path (loop back-edge or cross-edge) can modify tracked
   * stack slots.  Conservatively drop all stack-store forwarding state. */
  for (int pi = 0; pi < bb->num_preds; pi++) {
    if (bb->preds[pi] != bb->idom) {
      state.scount = 0;
      break;
    }
  }

  for (int i = bb->start_idx; i < bb->end_idx; i++) {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;

    if (q->op == TCCIR_OP_FUNCCALLVOID || q->op == TCCIR_OP_FUNCCALLVAL) {
      state.count = 0;
      state.scount = 0;
      continue;
    }

    if (q->op == TCCIR_OP_STORE || q->op == TCCIR_OP_STORE_INDEXED) {
      IROperand dest = tcc_ir_op_get_dest(ir, q);

      /* Track StackLoc stores for stack forwarding.  Record the stored
       * slot width so narrower subfield loads do not reuse wider values. */
      if (dest.tag == IROP_TAG_STACKOFF) {
        IROperand src = tcc_ir_op_get_src1(ir, q);
        int32_t svr = irop_get_vreg(src);
        /* Direct stack stores are encoded as StackLoc lvalues.  Non-lvalue
         * STACKOFF operands are stack addresses, not memory writes. */
        if (!ctx->no_stack_fwd && dest.is_local && dest.is_lval && !dest.is_llocal) {
          int store_btype = irop_get_btype(dest);
          if (svr >= 0 && TCCIR_DECODE_VREG_TYPE(svr) == TCCIR_VREG_TYPE_TEMP)
            sstore_track_vr(&state, irop_get_stack_offset(dest), store_btype, svr);
          else if (irop_is_immediate(src))
            sstore_track_imm(&state, irop_get_stack_offset(dest), store_btype, src);
        } else {
          /* A non-direct STACKOFF write may expose the address. */
          int off = irop_get_stack_offset(dest);
          int k = sstore_find(&state, off);
          if (k >= 0)
            state.sstores[k] = state.sstores[--state.scount];
        }
        continue;
      }

      if (dest.is_local) {
        continue;
      }

      /* Direct VAR stores (Vn <-- val) write to a local slot and
       * cannot alias globals or other stack locations. */
      if (!dest.is_lval) {
        int32_t dvr = irop_get_vreg(dest);
        if (dvr >= 0 && TCCIR_DECODE_VREG_TYPE(dvr) == TCCIR_VREG_TYPE_VAR) {
          continue;
        }
      }

      if (dest.is_sym && dest.is_lval) {
        IRPoolSymref *sref = irop_get_symref_ex(ir, dest);
        if (sref && sref->sym) {
          for (int k = 0; k < state.count; k++) {
            if (state.entries[k].sym == sref->sym) {
              state.entries[k] = state.entries[--state.count];
              k--;
            }
          }
        }
      } else {
        state.count = 0;
        state.scount = 0;
      }
      continue;
    }

    if (irop_config[q->op].has_dest && q->op != TCCIR_OP_STORE &&
        q->op != TCCIR_OP_STORE_INDEXED && q->op != TCCIR_OP_STORE_POSTINC) {
      IROperand qdest = tcc_ir_op_get_dest(ir, q);
      if (qdest.tag == IROP_TAG_STACKOFF && qdest.is_local)
        sstore_remove_offset(&state, irop_get_stack_offset(qdest));
      int32_t qdvr = irop_get_vreg(qdest);
      if (qdvr >= 0 && TCCIR_DECODE_VREG_TYPE(qdvr) == TCCIR_VREG_TYPE_VAR &&
          qdest.tag != IROP_TAG_STACKOFF)
        state.scount = 0;
      if (qdvr >= 0) {
        gload_remove_vr(&state, qdvr);
        sstore_remove_vr(&state, qdvr);
      }
    }

    if (q->op != TCCIR_OP_LOAD)
      continue;

    IROperand src1 = tcc_ir_op_get_src1(ir, q);
    IROperand dest = tcc_ir_op_get_dest(ir, q);
    int32_t dest_vr = irop_get_vreg(dest);
    if (dest_vr < 0 || TCCIR_DECODE_VREG_TYPE(dest_vr) != TCCIR_VREG_TYPE_TEMP)
      continue;
    int dest_btype = irop_get_btype(dest);

    /* Stack store-load forwarding */
    if (src1.is_lval && !src1.is_sym) {
      int stack_off = INT_MIN;

      /* Direct StackLoc load: T <-- StackLoc[N] [LOAD].
       * Skip if the operand carries a VAR vreg — that's a load from a
       * named variable whose offset may alias an unrelated StackLoc. */
      if (src1.tag == IROP_TAG_STACKOFF) {
        int32_t svr = irop_get_vreg(src1);
        if (svr < 0 || TCCIR_DECODE_VREG_TYPE(svr) != TCCIR_VREG_TYPE_VAR)
          stack_off = irop_get_stack_offset(src1);
      }


      /* LEA+DEREF load: T <-- *Addr[StackLoc[N]] [LOAD] */
      if (stack_off == INT_MIN) {
        int32_t ptr_vr = irop_get_vreg(src1);
        stack_off = resolve_lea_stackloc(ctx, ptr_vr);
      }

      if (stack_off != INT_MIN) {
        int sk = sstore_find(&state, stack_off);
        if (sk >= 0) {
          SStoreEntry *se = &state.sstores[sk];
          if (se->btype != dest_btype)
            continue;
          IROperand new_src;
          if (se->stored_vr >= 0) {
            new_src = irop_make_vreg(se->stored_vr, dest_btype);
            IRSSAVregInfo *rvi = ssa_opt_vinfo(ctx, se->stored_vr);
            if (rvi)
              ssa_opt_add_use_instr(rvi, i);
          } else {
            new_src = se->stored_imm;
          }
          q->op = TCCIR_OP_ASSIGN;
          tcc_ir_set_src1(ir, i, new_src);
          tcc_ir_set_src2(ir, i, IROP_NONE);
          changes++;
          continue;
        }
      }
    }

    /* Global load CSE */
    if (!src1.is_sym || !src1.is_lval)
      continue;

    IRPoolSymref *ref = irop_get_symref_ex(ir, src1);
    if (!ref || !ref->sym)
      continue;
    if (ref->sym->type.t & VT_VOLATILE)
      continue;

    int found = gload_find(&state, ref->sym, ref->addend, dest_btype);

    if (found >= 0) {
      int32_t earlier_vr = state.entries[found].result_vr;
      IROperand new_src = irop_make_vreg(earlier_vr, dest_btype);

      IRSSAVregInfo *rvi = ssa_opt_vinfo(ctx, earlier_vr);
      if (rvi)
        ssa_opt_add_use_instr(rvi, i);

      q->op = TCCIR_OP_ASSIGN;
      tcc_ir_set_src1(ir, i, new_src);
      tcc_ir_set_src2(ir, i, IROP_NONE);
      changes++;
    } else {
      gload_track(&state, ref->sym, ref->addend, dest_btype, dest_vr);
    }
  }

  for (int ci = 0; ci < bb->num_dom_children; ci++)
    changes += gload_process_block(ctx, state, bb->dom_children[ci]);

  return changes;
}

int ssa_opt_load_cse(IRSSAOptCtx *ctx)
{
  IRCFG *cfg = ctx->cfg;
  if (!cfg || cfg->num_blocks == 0)
    return 0;

  GLoadState initial;
  initial.count = 0;
  initial.scount = 0;
  return gload_process_block(ctx, initial, 0);
}
