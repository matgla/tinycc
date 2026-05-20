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
#define GSTORE_MAX 16
#define TVSTORE_MAX 16
#define ILOAD_MAX 16

typedef struct {
  Sym *sym;
  int64_t addend;
  int btype;
  int32_t result_vr;
} GLoadEntry;

/* LOAD_INDEXED CSE entry: tracks `T_dest = *(T_base + (idx << scale))` for
 * constant idx/scale.  Match is exact on (base_vr, idx, scale, btype). */
typedef struct {
  int32_t base_vr;
  int32_t result_vr;
  int btype;
  int32_t idx_imm;
  uint8_t scale;
} ILoadEntry;

typedef struct {
  int stack_offset;
  int btype;
  int32_t stored_vr;    /* TEMP vreg, or -1 if immediate */
  IROperand stored_imm; /* valid when stored_vr == -1 */
} SStoreEntry;

typedef struct {
  Sym *sym;
  int64_t addend;
  int btype;
  int32_t stored_vr;    /* TEMP vreg, or -1 if immediate */
  IROperand stored_imm; /* valid when stored_vr == -1 */
} GStoreEntry;

/* Track STOREs through a TEMP vreg used as a pointer: `T_vreg_DEREF = val`.
 * In SSA, T_vreg is single-def, so two references to the same T_vreg as
 * a pointer name the same memory.  This lets us forward the stored value
 * into subsequent reads of `T_vreg_DEREF` without resolving back to a
 * symbol — covers cases where the IR generator never normalised the
 * `&sym + offset` LEA into a plain SymRef operand. */
typedef struct {
  int32_t ptr_vr;         /* TEMP vreg used as the address */
  int btype;
  int32_t stored_vr;      /* TEMP vreg, or -1 if immediate */
  IROperand stored_imm;   /* valid when stored_vr == -1 */
} TVStoreEntry;

typedef struct {
  GLoadEntry entries[GLOAD_MAX];
  int count;
  SStoreEntry sstores[SSTORE_MAX];
  int scount;
  GStoreEntry gstores[GSTORE_MAX];
  int gscount;
  TVStoreEntry tvstores[TVSTORE_MAX];
  int tvcount;
  ILoadEntry iloads[ILOAD_MAX];
  int ilcount;
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

static int slot_btype_bytes(int btype)
{
  switch (btype) {
  case IROP_BTYPE_INT8: return 1;
  case IROP_BTYPE_INT16: return 2;
  case IROP_BTYPE_INT32:
  case IROP_BTYPE_FLOAT32:
  case IROP_BTYPE_FUNC: return 4;
  case IROP_BTYPE_INT64:
  case IROP_BTYPE_FLOAT64: return 8;
  default: return 8;
  }
}

static void sstore_invalidate_overlap(GLoadState *st, int offset, int btype)
{
  int size = slot_btype_bytes(btype);
  int lo = offset;
  int hi = offset + size;
  for (int k = 0; k < st->scount; k++) {
    SStoreEntry *e = &st->sstores[k];
    int esize = slot_btype_bytes(e->btype);
    int elo = e->stack_offset;
    int ehi = elo + esize;
    /* Drop entries whose byte range overlaps the new store and is not
     * an exact size+offset match (which sstore_track_* will overwrite). */
    if (e->stack_offset == offset && esize == size)
      continue;
    if (elo < hi && ehi > lo) {
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

/* ----- Global STORE → LOAD forwarding state ------------------------------ */

static int gstore_find(const GLoadState *st, Sym *sym, int64_t addend, int btype)
{
  for (int k = 0; k < st->gscount; k++) {
    if (st->gstores[k].sym == sym && st->gstores[k].addend == addend &&
        st->gstores[k].btype == btype)
      return k;
  }
  return -1;
}

/* Drop any entry whose byte range overlaps the new store. An exact
 * (sym,addend,btype) match is preserved here and overwritten by the
 * tracker which calls this. */
static void gstore_invalidate_overlap(GLoadState *st, Sym *sym, int64_t addend, int btype)
{
  int size = slot_btype_bytes(btype);
  int64_t lo = addend;
  int64_t hi = addend + size;
  for (int k = 0; k < st->gscount; k++) {
    GStoreEntry *e = &st->gstores[k];
    if (e->sym != sym)
      continue;
    int esize = slot_btype_bytes(e->btype);
    int64_t elo = e->addend;
    int64_t ehi = elo + esize;
    if (e->addend == addend && esize == size)
      continue;
    if (elo < hi && ehi > lo) {
      st->gstores[k] = st->gstores[--st->gscount];
      k--;
    }
  }
}

static void gstore_track_vr(GLoadState *st, Sym *sym, int64_t addend, int btype, int32_t stored_vr)
{
  gstore_invalidate_overlap(st, sym, addend, btype);
  int k = gstore_find(st, sym, addend, btype);
  if (k >= 0) {
    st->gstores[k].stored_vr = stored_vr;
    return;
  }
  if (st->gscount >= GSTORE_MAX)
    return;
  GStoreEntry *e = &st->gstores[st->gscount++];
  e->sym = sym;
  e->addend = addend;
  e->btype = btype;
  e->stored_vr = stored_vr;
}

static void gstore_track_imm(GLoadState *st, Sym *sym, int64_t addend, int btype, IROperand imm)
{
  gstore_invalidate_overlap(st, sym, addend, btype);
  int k = gstore_find(st, sym, addend, btype);
  if (k >= 0) {
    st->gstores[k].stored_vr = -1;
    st->gstores[k].stored_imm = imm;
    return;
  }
  if (st->gscount >= GSTORE_MAX)
    return;
  GStoreEntry *e = &st->gstores[st->gscount++];
  e->sym = sym;
  e->addend = addend;
  e->btype = btype;
  e->stored_vr = -1;
  e->stored_imm = imm;
}

static void gstore_remove_sym(GLoadState *st, Sym *sym)
{
  for (int k = 0; k < st->gscount; k++) {
    if (st->gstores[k].sym == sym) {
      st->gstores[k] = st->gstores[--st->gscount];
      k--;
    }
  }
}

static void gstore_remove_vr(GLoadState *st, int32_t vr)
{
  for (int k = 0; k < st->gscount; k++) {
    if (st->gstores[k].stored_vr == vr) {
      st->gstores[k] = st->gstores[--st->gscount];
      return;
    }
  }
}

/* ----- T_vreg-deref store forwarding ------------------------------------- */

static int tvstore_find(const GLoadState *st, int32_t ptr_vr, int btype)
{
  for (int k = 0; k < st->tvcount; k++) {
    if (st->tvstores[k].ptr_vr == ptr_vr && st->tvstores[k].btype == btype)
      return k;
  }
  return -1;
}

static void tvstore_track_imm(GLoadState *st, int32_t ptr_vr, int btype, IROperand imm)
{
  int k = tvstore_find(st, ptr_vr, btype);
  if (k >= 0) {
    st->tvstores[k].stored_vr = -1;
    st->tvstores[k].stored_imm = imm;
    return;
  }
  if (st->tvcount >= TVSTORE_MAX)
    return;
  TVStoreEntry *e = &st->tvstores[st->tvcount++];
  e->ptr_vr = ptr_vr;
  e->btype = btype;
  e->stored_vr = -1;
  e->stored_imm = imm;
}

static void tvstore_track_vr(GLoadState *st, int32_t ptr_vr, int btype, int32_t stored_vr)
{
  int k = tvstore_find(st, ptr_vr, btype);
  if (k >= 0) {
    st->tvstores[k].stored_vr = stored_vr;
    return;
  }
  if (st->tvcount >= TVSTORE_MAX)
    return;
  TVStoreEntry *e = &st->tvstores[st->tvcount++];
  e->ptr_vr = ptr_vr;
  e->btype = btype;
  e->stored_vr = stored_vr;
}

static void tvstore_remove_vr(GLoadState *st, int32_t vr)
{
  for (int k = 0; k < st->tvcount; k++) {
    if (st->tvstores[k].ptr_vr == vr || st->tvstores[k].stored_vr == vr) {
      st->tvstores[k] = st->tvstores[--st->tvcount];
      k--;
    }
  }
}

/* ----- LOAD_INDEXED CSE state ------------------------------------------- */

static int iload_find(const GLoadState *st, int32_t base_vr, int32_t idx_imm, int scale, int btype)
{
  for (int k = 0; k < st->ilcount; k++) {
    if (st->iloads[k].base_vr == base_vr && st->iloads[k].idx_imm == idx_imm &&
        st->iloads[k].scale == scale && st->iloads[k].btype == btype)
      return k;
  }
  return -1;
}

static void iload_track(GLoadState *st, int32_t base_vr, int32_t idx_imm, int scale, int btype, int32_t result_vr)
{
  if (st->ilcount >= ILOAD_MAX)
    return;
  ILoadEntry *e = &st->iloads[st->ilcount++];
  e->base_vr = base_vr;
  e->result_vr = result_vr;
  e->btype = btype;
  e->idx_imm = idx_imm;
  e->scale = (uint8_t)scale;
}

static void iload_remove_vr(GLoadState *st, int32_t vr)
{
  for (int k = 0; k < st->ilcount; k++) {
    if (st->iloads[k].base_vr == vr || st->iloads[k].result_vr == vr) {
      st->iloads[k] = st->iloads[--st->ilcount];
      k--;
    }
  }
}

/* Kill iload entries that may alias a store at byte range [store_lo, store_hi)
 * through base store_base_vr.  Entries with a different base_vr are killed
 * conservatively (different TEMP vregs may still alias the same memory). */
static void iload_kill_for_store(GLoadState *st, int32_t store_base_vr, int store_lo, int store_hi)
{
  for (int k = 0; k < st->ilcount; k++) {
    const ILoadEntry *e = &st->iloads[k];
    int kill = 0;
    if (e->base_vr != store_base_vr) {
      kill = 1;
    } else {
      int eo = (int)e->idx_imm * (1 << e->scale);
      int eh = eo + slot_btype_bytes(e->btype);
      if (eo < store_hi && eh > store_lo)
        kill = 1;
    }
    if (kill) {
      st->iloads[k] = st->iloads[--st->ilcount];
      k--;
    }
  }
}

/* Like iload_kill_for_store but knows that the store goes to the local
 * stack frame (caller resolved store base to a LEA-StackLoc).  Such a
 * store cannot alias an iload whose base is a PARAM/VAR (caller-supplied
 * pointer) or a TEMP that does NOT resolve to a stack location.
 *
 * Same-base entries still need precise byte-range overlap analysis. */
static void iload_kill_for_stack_store(IRSSAOptCtx *ctx, GLoadState *st, int32_t store_base_vr,
                                       int store_lo, int store_hi)
{
  for (int k = 0; k < st->ilcount; k++) {
    const ILoadEntry *e = &st->iloads[k];
    int kill = 0;
    if (e->base_vr == store_base_vr) {
      int eo = (int)e->idx_imm * (1 << e->scale);
      int eh = eo + slot_btype_bytes(e->btype);
      if (eo < store_hi && eh > store_lo)
        kill = 1;
    } else {
      int e_type = TCCIR_DECODE_VREG_TYPE(e->base_vr);
      if (e_type == TCCIR_VREG_TYPE_TEMP) {
        /* TEMP base: may or may not be a stack pointer.  If it does NOT
         * resolve to a stack location, the store can't reach it (different
         * memory region).  If it does resolve, treat as aliasing (different
         * stack slots can alias in unusual cases like union punning). */
        if (ssa_opt_resolve_lea_stackloc(ctx, e->base_vr) != INT_MIN)
          kill = 1;
      }
      /* PARAM/VAR base: caller-supplied or named-local register holding a
       * pointer.  Won't alias a fresh local-stack store unless the address
       * escaped, but the SSA load-CSE only tracks LOADs of such bases when
       * they look pointer-like.  Skip kill. */
    }
    if (kill) {
      st->iloads[k] = st->iloads[--st->ilcount];
      k--;
    }
  }
}

/* resolve_lea_stackloc moved to ssa_opt.c as ssa_opt_resolve_lea_stackloc. */
#define resolve_lea_stackloc ssa_opt_resolve_lea_stackloc

static int gload_process_block(IRSSAOptCtx *ctx, GLoadState state, int b)
{
  TCCIRState *ir = ctx->ir;
  IRCFG *cfg = ctx->cfg;
  IRBasicBlock *bb = &cfg->blocks[b];
  int changes = 0;

  /* If this block has any predecessor that is NOT its immediate dominator,
   * a non-dominator path (loop back-edge or cross-edge) can modify tracked
   * stack slots or global stores.  Conservatively drop forwarding state. */
  for (int pi = 0; pi < bb->num_preds; pi++) {
    if (bb->preds[pi] != bb->idom) {
      state.scount = 0;
      state.gscount = 0;
      state.tvcount = 0;
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
      state.gscount = 0;
      state.tvcount = 0;
      state.ilcount = 0;
      continue;
    }

    /* T_vreg-deref forwarding into ALU op operands: when an instruction
     * other than STORE reads `T_vreg_DEREF` and a recent STORE through the
     * same T_vreg stored a value, rewrite the operand to that value.
     * Eliminates the implicit LDR the backend would emit to materialise
     * the deref. */
    if (q->op != TCCIR_OP_STORE && q->op != TCCIR_OP_STORE_INDEXED &&
        q->op != TCCIR_OP_STORE_POSTINC &&
        q->op != TCCIR_OP_LOAD && q->op != TCCIR_OP_LOAD_INDEXED &&
        q->op != TCCIR_OP_LOAD_POSTINC) {
      int rewrites = 0;
      for (int side = 0; side < 2; side++) {
        IROperand op = side == 0 ? tcc_ir_op_get_src1(ir, q) : tcc_ir_op_get_src2(ir, q);
        if (!op.is_lval || op.is_llocal || op.is_sym || op.is_local)
          continue;
        if (op.tag != IROP_TAG_VREG)
          continue;
        int32_t pvr = irop_get_vreg(op);
        if (pvr < 0 || TCCIR_DECODE_VREG_TYPE(pvr) != TCCIR_VREG_TYPE_TEMP)
          continue;
        int op_btype = irop_get_btype(op);
        int tk = tvstore_find(&state, pvr, op_btype);
        if (tk < 0)
          continue;
        TVStoreEntry *te = &state.tvstores[tk];
        IROperand new_op;
        if (te->stored_vr >= 0) {
          new_op = irop_make_vreg(te->stored_vr, op_btype);
          IRSSAVregInfo *rvi = ssa_opt_vinfo(ctx, te->stored_vr);
          if (rvi)
            ssa_opt_add_use_instr(rvi, i);
        } else {
          new_op = te->stored_imm;
        }
        if (side == 0)
          tcc_ir_set_src1(ir, i, new_op);
        else
          tcc_ir_set_src2(ir, i, new_op);
        IRSSAVregInfo *pvi = ssa_opt_vinfo(ctx, pvr);
        if (pvi)
          ssa_opt_remove_use_instr(pvi, i);
        rewrites++;
      }
      if (rewrites > 0)
        changes += rewrites;
    }

    if (q->op == TCCIR_OP_STORE || q->op == TCCIR_OP_STORE_INDEXED) {
      IROperand dest = tcc_ir_op_get_dest(ir, q);

      /* LOAD_INDEXED CSE: kill matching iload entries via byte-range alias
       * analysis.  Done BEFORE the legacy store handlers below, which would
       * otherwise nuke all forwarding state on TEMP-DEREF / unresolved stores.
       *
       * Stack stores (STACKOFF dest) and direct VAR stores (non-lval VREG
       * dest with VAR tag) don't alias global/heap memory, so the iload
       * tracker can ignore them.  Everything else is treated as "may write
       * to anywhere through this base"; precise overlap analysis kicks in
       * when both base_vr and offsets are known. */
      if (state.ilcount > 0) {
        int store_aliases_globals = 1;
        if (dest.tag == IROP_TAG_STACKOFF)
          store_aliases_globals = 0;
        else if (dest.is_local)
          store_aliases_globals = 0;
        else if (!dest.is_lval && q->op == TCCIR_OP_STORE) {
          int32_t dvr = irop_get_vreg(dest);
          if (dvr >= 0 && TCCIR_DECODE_VREG_TYPE(dvr) == TCCIR_VREG_TYPE_VAR)
            store_aliases_globals = 0;
        }

        if (store_aliases_globals) {
          int32_t store_base_vr = irop_get_vreg(dest);
          int store_btype = irop_get_btype(dest);
          int size = slot_btype_bytes(store_btype);
          int store_lo = 0, store_hi = size;
          int can_check = 0;
          int store_to_stack = 0; /* base resolves to LEA-StackLoc */

          if (dest.tag == IROP_TAG_VREG && store_base_vr >= 0 &&
              TCCIR_DECODE_VREG_TYPE(store_base_vr) == TCCIR_VREG_TYPE_TEMP) {
            if (q->op == TCCIR_OP_STORE && dest.is_lval) {
              can_check = 1; /* *T = val: offset 0 from base T */
            } else if (q->op == TCCIR_OP_STORE_INDEXED) {
              IROperand idx = tcc_ir_op_get_src2(ir, q);
              IROperand sc = tcc_ir_op_get_scale(ir, q);
              if (irop_is_immediate(idx) && irop_is_immediate(sc)) {
                int io = (int)irop_get_imm32(idx) * (1 << irop_get_imm32(sc));
                store_lo = io;
                store_hi = io + size;
                can_check = 1;
              }
            }
            /* If this store's base TEMP resolves to a local stack address,
             * the store cannot alias loads through PARAM/VAR pointers or
             * through TEMPs that don't themselves resolve to stack. */
            if (can_check && ssa_opt_resolve_lea_stackloc(ctx, store_base_vr) != INT_MIN)
              store_to_stack = 1;
          }

          if (store_to_stack)
            iload_kill_for_stack_store(ctx, &state, store_base_vr, store_lo, store_hi);
          else if (can_check)
            iload_kill_for_store(&state, store_base_vr, store_lo, store_hi);
          else
            state.ilcount = 0;
        }
      }

      /* Source-side load forwarding: if the source is a stack-loadable
       * lvalue (direct StackLoc or *TEMP-resolving-to-LEA) and we have a
       * tracked constant or vreg there, rewrite the source.  Apply before
       * tracking, since tracking might invalidate the src offset. */
      if (q->op == TCCIR_OP_STORE && !ctx->no_stack_fwd) {
        IROperand src = tcc_ir_op_get_src1(ir, q);
        if (src.is_lval && !src.is_sym && !irop_is_immediate(src)) {
          int load_off = INT_MIN;
          int load_btype = irop_get_btype(src);
          if (src.tag == IROP_TAG_STACKOFF) {
            int32_t svr = irop_get_vreg(src);
            if (svr < 0 || TCCIR_DECODE_VREG_TYPE(svr) != TCCIR_VREG_TYPE_VAR)
              load_off = irop_get_stack_offset(src);
          } else if (src.tag == IROP_TAG_VREG && !src.is_local) {
            int32_t pvr = irop_get_vreg(src);
            load_off = resolve_lea_stackloc(ctx, pvr);
          }
          if (load_off != INT_MIN) {
            int sk = sstore_find(&state, load_off);
            if (sk >= 0 && state.sstores[sk].btype == load_btype) {
              SStoreEntry *se = &state.sstores[sk];
              if (se->stored_vr < 0) {
                tcc_ir_set_src1(ir, i, se->stored_imm);
                changes++;
                /* Refresh dest after rewrite (no-op for STORE; just use
                 * existing local to keep flow consistent). */
              }
            }
          }
        }
      }

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

      /* TEMP-DEREF stack stores: *T = val (STORE) or *(T + idx) = val
       * (STORE_INDEXED with scale=0), where T resolves to LEA(StackLoc[N]).
       * Treat as a direct stack store at the resolved offset.  STORE_INDEXED
       * carries its base in dest as a non-lvalue pointer; STORE wraps the
       * dest pointer in is_lval to express the deref. */
      int store_dest_is_temp_indir =
          (dest.tag == IROP_TAG_VREG && !dest.is_local &&
           ((q->op == TCCIR_OP_STORE && dest.is_lval) ||
            q->op == TCCIR_OP_STORE_INDEXED));
      if (store_dest_is_temp_indir) {
        int eff_off = ssa_opt_indirect_stack_offset(ctx, q, SSA_OPT_INDIRECT_DEST);
        if (eff_off != INT_MIN) {
          if (!ctx->no_stack_fwd) {
            IROperand src = tcc_ir_op_get_src1(ir, q);
            int store_btype = irop_get_btype(dest);
            int32_t svr = irop_get_vreg(src);
            if (svr >= 0 && TCCIR_DECODE_VREG_TYPE(svr) == TCCIR_VREG_TYPE_TEMP)
              sstore_track_vr(&state, eff_off, store_btype, svr);
            else if (irop_is_immediate(src))
              sstore_track_imm(&state, eff_off, store_btype, src);
            else
              sstore_remove_offset(&state, eff_off);
          }
          continue;
        }
        /* TEMP-DEREF store whose pointer doesn't resolve to a stack slot:
         * track by the TEMP vreg ID.  In SSA, a TEMP is single-def so two
         * references to the same T_vreg with deref name the same memory
         * (subject to alias kill on funccall / non-dom-pred / vreg redef).
         * Restrict to STORE (not STORE_INDEXED, whose runtime index makes
         * the address ambiguous) and to word-aligned types where no
         * narrowing happens during store + subsequent load.
         *
         * Because the pointer may alias unknown memory, also invalidate
         * other forwarding state — global CSE, global store entries, and
         * other tvstores at a different ptr_vr — but keep the new tvstore
         * entry for this exact ptr_vr (which IS the address just written).
         * Then continue to the next instruction so we don't fall into the
         * generic kill-all "else" branch below. */
        if (q->op == TCCIR_OP_STORE) {
          int store_btype = irop_get_btype(dest);
          int width_safe_tv = (store_btype == IROP_BTYPE_INT32 ||
                               store_btype == IROP_BTYPE_INT64 ||
                               store_btype == IROP_BTYPE_FLOAT32 ||
                               store_btype == IROP_BTYPE_FLOAT64 ||
                               store_btype == IROP_BTYPE_FUNC);
          int32_t ptr_vr = irop_get_vreg(dest);
          if (width_safe_tv && ptr_vr >= 0 &&
              TCCIR_DECODE_VREG_TYPE(ptr_vr) == TCCIR_VREG_TYPE_TEMP) {
            IROperand src = tcc_ir_op_get_src1(ir, q);
            int32_t svr = irop_get_vreg(src);
            int tracked = 0;
            if (irop_is_immediate(src)) {
              tvstore_track_imm(&state, ptr_vr, store_btype, src);
              tracked = 1;
            } else if (svr >= 0 &&
                       TCCIR_DECODE_VREG_TYPE(svr) == TCCIR_VREG_TYPE_TEMP &&
                       src.tag == IROP_TAG_VREG && !src.is_lval) {
              tvstore_track_vr(&state, ptr_vr, store_btype, svr);
              tracked = 1;
            }
            if (tracked) {
              /* Invalidate aliasing state: an unknown pointer may write
               * over any tracked address.  Drop all other tvstores (with
               * a different ptr_vr or btype), all global CSE entries, all
               * global stores, and all stack stores. */
              for (int kk = 0; kk < state.tvcount; kk++) {
                if (state.tvstores[kk].ptr_vr != ptr_vr ||
                    state.tvstores[kk].btype != store_btype) {
                  state.tvstores[kk] = state.tvstores[--state.tvcount];
                  kk--;
                }
              }
              state.count = 0;
              state.scount = 0;
              state.gscount = 0;
              continue;
            }
          }
        }
        /* Indirect TEMP-DEREF store with unresolved address.  We can't
         * prove which slot it touches, so the global-load aliasing logic
         * below applies (kill state).  Fall through. */
      }

      if (dest.is_local) {
        continue;
      }

      /* Direct VAR / TEMP non-lval stores (Tn <-- val, Vn <-- val) are
       * SSA value assignments — they write to a vreg/slot, not to arbitrary
       * memory, and so cannot alias tracked stack or global stores.  The
       * STORE op label here is an artefact of the IR encoding (some
       * frontends emit address-materialisation `T = Addr[StackLoc[N]]`
       * with op=STORE rather than ASSIGN/LEA); semantically it is a copy.
       *
       * Forget any tracking keyed by this dest vreg, but keep the rest of
       * the forward state intact. */
      if (!dest.is_lval) {
        int32_t dvr = irop_get_vreg(dest);
        int dtype = (dvr >= 0) ? TCCIR_DECODE_VREG_TYPE(dvr) : -1;
        if (dtype == TCCIR_VREG_TYPE_VAR || dtype == TCCIR_VREG_TYPE_TEMP) {
          if (dvr >= 0) {
            gload_remove_vr(&state, dvr);
            sstore_remove_vr(&state, dvr);
            gstore_remove_vr(&state, dvr);
            tvstore_remove_vr(&state, dvr);
            iload_remove_vr(&state, dvr);
          }
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
          /* Track this store for subsequent same-address LOADs to forward.
           * Only TCCIR_OP_STORE (not STORE_INDEXED): the indexed form has a
           * runtime index and we can't prove which sym+offset it touches.
           *
           * Skip sub-word stores: STORE on a char/short global narrows the
           * stored value to the storage width, and a subsequent LDRB/LDRH
           * zero/sign-extends back. Forwarding the original (wider) vreg or
           * immediate would skip that round-trip and yield a wrong value —
           * see test pr78477 where `b = x; b = 1 | (b << 5)` needs the LOAD
           * of `b` to observe the 16-bit truncation of `x`. INT32 (and
           * pointers/floats at word width) need no such round-trip, so
           * forwarding is safe there. */
          int store_btype_chk = irop_get_btype(dest);
          int width_safe = (store_btype_chk == IROP_BTYPE_INT32 ||
                            store_btype_chk == IROP_BTYPE_INT64 ||
                            store_btype_chk == IROP_BTYPE_FLOAT32 ||
                            store_btype_chk == IROP_BTYPE_FLOAT64 ||
                            store_btype_chk == IROP_BTYPE_FUNC);
          if (q->op == TCCIR_OP_STORE && !(sref->sym->type.t & VT_VOLATILE) && width_safe) {
            int store_btype = store_btype_chk;
            IROperand sval = tcc_ir_op_get_src1(ir, q);
            int32_t svr = irop_get_vreg(sval);
            if (irop_is_immediate(sval)) {
              gstore_track_imm(&state, sref->sym, sref->addend, store_btype, sval);
            } else if (svr >= 0 && TCCIR_DECODE_VREG_TYPE(svr) == TCCIR_VREG_TYPE_TEMP &&
                       sval.tag == IROP_TAG_VREG && !sval.is_lval) {
              gstore_track_vr(&state, sref->sym, sref->addend, store_btype, svr);
            } else {
              /* Value form we don't model — invalidate this slot. */
              gstore_invalidate_overlap(&state, sref->sym, sref->addend, store_btype);
            }
          } else if (q->op == TCCIR_OP_STORE && !width_safe) {
            /* Sub-word store: don't forward; also invalidate any stale entry
             * at this address so we don't propagate a wider stale value. */
            gstore_invalidate_overlap(&state, sref->sym, sref->addend, store_btype_chk);
          } else if (q->op == TCCIR_OP_STORE_INDEXED) {
            /* Runtime index touches an unknown offset within sym. Drop all
             * entries for this sym. */
            gstore_remove_sym(&state, sref->sym);
          }
        }
      } else {
        state.count = 0;
        state.scount = 0;
        state.gscount = 0;
        state.tvcount = 0;
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
        gstore_remove_vr(&state, qdvr);
        tvstore_remove_vr(&state, qdvr);
        iload_remove_vr(&state, qdvr);
      }
    }

    /* LOAD_INDEXED CSE: dedupe `T_dest = *(T_base + (#idx << #scale))` when
     * both index and scale are immediates and base is a TEMP vreg.  This
     * catches reads of the same global array element that disp_fusion /
     * add_deref_fold produced from separate inlined call sites. */
    if (q->op == TCCIR_OP_LOAD_INDEXED) {
      IROperand idx_dest = tcc_ir_op_get_dest(ir, q);
      IROperand idx_base = tcc_ir_op_get_src1(ir, q);
      IROperand idx_idx = tcc_ir_op_get_src2(ir, q);
      IROperand idx_sc = tcc_ir_op_get_scale(ir, q);

      int32_t il_dest_vr = irop_get_vreg(idx_dest);
      int32_t il_base_vr = irop_get_vreg(idx_base);
      if (il_dest_vr < 0 || TCCIR_DECODE_VREG_TYPE(il_dest_vr) != TCCIR_VREG_TYPE_TEMP)
        continue;
      if (il_base_vr < 0)
        continue;
      {
        int il_base_type = TCCIR_DECODE_VREG_TYPE(il_base_vr);
        /* Allow PARAM bases when the PARAM has exactly one definition (the
         * implicit entry-block ABI assignment).  A reassigned PARAM (e.g.
         * `c = &local;` after using `c` as a caller-supplied pointer) would
         * make a later CSE unsound — loads through the original PARAM value
         * don't correspond to loads through the reassigned pointer.  VAR
         * bases are similarly tracked as multi-def in the non-promoted case,
         * so skip them entirely. */
        if (il_base_type == TCCIR_VREG_TYPE_PARAM) {
          int writes = 0;
          for (int wi = 0; wi < ctx->ir->next_instruction_index; wi++) {
            IRQuadCompact *wq = &ctx->ir->compact_instructions[wi];
            if (!irop_config[wq->op].has_dest)
              continue;
            IROperand wd = tcc_ir_op_get_dest(ctx->ir, wq);
            if (irop_get_vreg(wd) == il_base_vr) {
              writes++;
              if (writes > 0) break;
            }
          }
          if (writes > 0)
            continue;
        } else if (il_base_type != TCCIR_VREG_TYPE_TEMP)
          continue;
      }
      if (idx_base.is_lval)
        continue;
      if (!irop_is_immediate(idx_idx) || !irop_is_immediate(idx_sc))
        continue;

      int32_t il_idx = irop_get_imm32(idx_idx);
      int il_scale = irop_get_imm32(idx_sc);
      int il_btype = irop_get_btype(idx_dest);

      /* Stack store-load forwarding for LOAD_INDEXED with constant index.
       * If the base resolves to LEA(StackLoc[N]) and scale is 0 (byte
       * offset form, which is what disp_fusion produces from ADD #imm +
       * DEREF), the effective offset is N + idx and we can forward a
       * tracked stack store at that offset.  ssa_opt_indirect_stack_offset
       * already enforces scale==0 and constant idx. */
      if (!ctx->no_stack_fwd) {
        int eff_off = ssa_opt_indirect_stack_offset(ctx, q, SSA_OPT_INDIRECT_SRC1);
        if (eff_off != INT_MIN) {
          int sk = sstore_find(&state, eff_off);
          if (sk >= 0 && state.sstores[sk].btype == il_btype) {
            SStoreEntry *se = &state.sstores[sk];
            IROperand new_src;
            if (se->stored_vr >= 0) {
              new_src = irop_make_vreg(se->stored_vr, il_btype);
              IRSSAVregInfo *rvi = ssa_opt_vinfo(ctx, se->stored_vr);
              if (rvi)
                ssa_opt_add_use_instr(rvi, i);
            } else {
              new_src = se->stored_imm;
            }
            IRSSAVregInfo *bvi = ssa_opt_vinfo(ctx, il_base_vr);
            if (bvi)
              ssa_opt_remove_use_instr(bvi, i);
            q->op = TCCIR_OP_ASSIGN;
            tcc_ir_set_src1(ir, i, new_src);
            tcc_ir_set_src2(ir, i, IROP_NONE);
            changes++;
            continue;
          }
        }
      }

      int found = iload_find(&state, il_base_vr, il_idx, il_scale, il_btype);
      if (found >= 0) {
        int32_t earlier_vr = state.iloads[found].result_vr;
        IROperand new_src = irop_make_vreg(earlier_vr, il_btype);

        IRSSAVregInfo *rvi = ssa_opt_vinfo(ctx, earlier_vr);
        if (rvi)
          ssa_opt_add_use_instr(rvi, i);

        IRSSAVregInfo *bvi = ssa_opt_vinfo(ctx, il_base_vr);
        if (bvi)
          ssa_opt_remove_use_instr(bvi, i);

        q->op = TCCIR_OP_ASSIGN;
        tcc_ir_set_src1(ir, i, new_src);
        tcc_ir_set_src2(ir, i, IROP_NONE);
        /* Track this load's dest so subsequent matching loads keep CSE'ing. */
        iload_track(&state, il_base_vr, il_idx, il_scale, il_btype, il_dest_vr);
        changes++;
        continue;
      }
      iload_track(&state, il_base_vr, il_idx, il_scale, il_btype, il_dest_vr);
      continue;
    }

    if (q->op != TCCIR_OP_LOAD)
      continue;

    IROperand src1 = tcc_ir_op_get_src1(ir, q);
    IROperand dest = tcc_ir_op_get_dest(ir, q);
    int32_t dest_vr = irop_get_vreg(dest);
    if (dest_vr < 0 || TCCIR_DECODE_VREG_TYPE(dest_vr) != TCCIR_VREG_TYPE_TEMP)
      continue;
    int dest_btype = irop_get_btype(dest);

    /* T_vreg-deref store-load forwarding: LOAD `T_vreg_DEREF` where a
     * recent STORE through the same T_vreg recorded the value. */
    if (src1.is_lval && !src1.is_sym && !src1.is_local && !src1.is_llocal &&
        src1.tag == IROP_TAG_VREG) {
      int32_t ptr_vr_l = irop_get_vreg(src1);
      if (ptr_vr_l >= 0 && TCCIR_DECODE_VREG_TYPE(ptr_vr_l) == TCCIR_VREG_TYPE_TEMP) {
        int tk = tvstore_find(&state, ptr_vr_l, dest_btype);
        if (tk >= 0) {
          TVStoreEntry *te = &state.tvstores[tk];
          IROperand new_src;
          if (te->stored_vr >= 0) {
            new_src = irop_make_vreg(te->stored_vr, dest_btype);
            IRSSAVregInfo *rvi = ssa_opt_vinfo(ctx, te->stored_vr);
            if (rvi)
              ssa_opt_add_use_instr(rvi, i);
          } else {
            new_src = te->stored_imm;
          }
          q->op = TCCIR_OP_ASSIGN;
          tcc_ir_set_src1(ir, i, new_src);
          tcc_ir_set_src2(ir, i, IROP_NONE);
          IRSSAVregInfo *pvi = ssa_opt_vinfo(ctx, ptr_vr_l);
          if (pvi)
            ssa_opt_remove_use_instr(pvi, i);
          changes++;
          continue;
        }
      }
    }

    /* TEMP-DEREF LOAD CSE via canonical (base_vr, offset).
     *
     * Two LOADs through different TEMP pointers can name the same
     * memory if both pointers resolve to the same canonical
     * (base_vr, offset) — e.g. `T9 = V1; T19 = V1; *T9; *T19` reads
     * V1's pointee twice via different TEMPs.  Standard load-CSE
     * keyed by vreg ID misses this; canonicalizing through ASSIGN/ADD
     * chains catches it.
     *
     * Reuse the iload tracker (originally for LOAD_INDEXED) — its
     * (base_vr, idx, scale=0, btype) key matches the canonical form
     * exactly, and the existing invalidation handles aliasing stores
     * and calls.  The btype is part of the key, so a 32-bit
     * LOAD_INDEXED and a 64-bit plain LOAD at the same offset don't
     * collide. */
    if (src1.is_lval && !src1.is_sym && !src1.is_local && !src1.is_llocal &&
        src1.tag == IROP_TAG_VREG) {
      int32_t ptr_vr = irop_get_vreg(src1);
      int ptr_type = ptr_vr >= 0 ? TCCIR_DECODE_VREG_TYPE(ptr_vr) : -1;
      int ptr_ok = (ptr_type == TCCIR_VREG_TYPE_TEMP);
      /* PARAM bases are also safe if the PARAM is never reassigned within
       * the function — the value is the caller-supplied pointer for all
       * uses.  A reassigned PARAM (e.g. `if (c==0) c=&local;`) is unsafe to
       * CSE through since later loads carry a different value. */
      if (ptr_vr >= 0 && ptr_type == TCCIR_VREG_TYPE_PARAM) {
        int writes = 0;
        for (int wi = 0; wi < ctx->ir->next_instruction_index && writes == 0; wi++) {
          IRQuadCompact *wq = &ctx->ir->compact_instructions[wi];
          if (!irop_config[wq->op].has_dest)
            continue;
          if (irop_get_vreg(tcc_ir_op_get_dest(ctx->ir, wq)) == ptr_vr)
            writes = 1;
        }
        if (writes == 0)
          ptr_ok = 1;
      }
      if (ptr_vr >= 0 && ptr_ok) {
        int32_t canon_base = -1, canon_off = 0;
        if (ssa_opt_resolve_temp_to_base_off(ctx, ptr_vr, &canon_base, &canon_off) &&
            canon_base >= 0) {
          int found = iload_find(&state, canon_base, canon_off, 0, dest_btype);
          if (found >= 0) {
            int32_t earlier_vr = state.iloads[found].result_vr;
            IROperand new_src = irop_make_vreg(earlier_vr, dest_btype);
            IRSSAVregInfo *rvi = ssa_opt_vinfo(ctx, earlier_vr);
            if (rvi)
              ssa_opt_add_use_instr(rvi, i);
            IRSSAVregInfo *pvi = ssa_opt_vinfo(ctx, ptr_vr);
            if (pvi)
              ssa_opt_remove_use_instr(pvi, i);
            q->op = TCCIR_OP_ASSIGN;
            tcc_ir_set_src1(ir, i, new_src);
            tcc_ir_set_src2(ir, i, IROP_NONE);
            iload_track(&state, canon_base, canon_off, 0, dest_btype, dest_vr);
            changes++;
            continue;
          }
          iload_track(&state, canon_base, canon_off, 0, dest_btype, dest_vr);
        }
      }
    }

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

    /* Store-to-load forwarding: prefer a tracked store value over an
     * earlier load, since forwarding eliminates the LOAD entirely and
     * the stored value (often an immediate) constant-folds further. */
    int gstore_k = gstore_find(&state, ref->sym, ref->addend, dest_btype);
    if (gstore_k >= 0) {
      GStoreEntry *ge = &state.gstores[gstore_k];
      IROperand new_src;
      if (ge->stored_vr >= 0) {
        new_src = irop_make_vreg(ge->stored_vr, dest_btype);
        IRSSAVregInfo *rvi = ssa_opt_vinfo(ctx, ge->stored_vr);
        if (rvi)
          ssa_opt_add_use_instr(rvi, i);
      } else {
        new_src = ge->stored_imm;
      }
      q->op = TCCIR_OP_ASSIGN;
      tcc_ir_set_src1(ir, i, new_src);
      tcc_ir_set_src2(ir, i, IROP_NONE);
      /* Track this load's dest as a fresh GLoad CSE entry so subsequent
       * non-aliased LOADs from the same address keep CSE'ing. */
      gload_track(&state, ref->sym, ref->addend, dest_btype, dest_vr);
      changes++;
      continue;
    }

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
  initial.gscount = 0;
  initial.tvcount = 0;
  initial.ilcount = 0;
  return gload_process_block(ctx, initial, 0);
}
