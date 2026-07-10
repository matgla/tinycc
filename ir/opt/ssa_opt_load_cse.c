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

extern int tcc_ir_opt_pass_disabled(const char *name);

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
/* keep ILOAD_MAX at 16: 32 CSEs twice as many long ranges and spills (pr54713 f6/f7) */
#define ILOAD_MAX 16
#define VSLOT_MAX 16

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
  /* Identity of the stored-to location: -1 for a real direct stack slot (the
   * offset uniquely names it), or the VAR/PARAM base vreg for a `&VAR` address
   * whose offset is a placeholder shared by every distinct local.  A load only
   * forwards from this entry when its own resolved base matches (ptr fuzz seed
   * 67: `&u2` and `&u3` both resolve to offset 0 but must not alias). */
  int32_t base_var;
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

/* Named-local (VREG-tagged VAR) store-to-load forwarding.  At the SSA stage a
 * local is a plain VAR vreg, so `V <- T` (STORE/ASSIGN) makes the value in T
 * available at every dominated read of V.  The VAR vreg is the identity — no
 * stack offset is assigned yet — so entries key directly on var_vr.  Only
 * tracked for non-address-taken VARs, which no pointer can alias. */
typedef struct {
  int32_t var_vr;       /* VREG-tagged VAR vreg (the slot identity) */
  int btype;            /* btype of the stored value (matches read btype) */
  int32_t stored_vr;    /* TEMP vreg, or -1 if immediate */
  IROperand stored_imm; /* valid when stored_vr == -1 */
} VSlotEntry;

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
  VSlotEntry vslots[VSLOT_MAX];
  int vscount;
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

static void sstore_track_vr(GLoadState *st, int offset, int btype, int32_t stored_vr,
                            int32_t base_var)
{
  sstore_invalidate_overlap(st, offset, btype);
  int k = sstore_find(st, offset);
  if (k >= 0) {
    st->sstores[k].btype = btype;
    st->sstores[k].stored_vr = stored_vr;
    st->sstores[k].base_var = base_var;
    return;
  }
  if (st->scount >= SSTORE_MAX)
    return;
  SStoreEntry *e = &st->sstores[st->scount++];
  e->stack_offset = offset;
  e->btype = btype;
  e->stored_vr = stored_vr;
  e->base_var = base_var;
}

static void sstore_track_imm(GLoadState *st, int offset, int btype, IROperand imm,
                             int32_t base_var)
{
  sstore_invalidate_overlap(st, offset, btype);
  int k = sstore_find(st, offset);
  if (k >= 0) {
    st->sstores[k].btype = btype;
    st->sstores[k].stored_vr = -1;
    st->sstores[k].stored_imm = imm;
    st->sstores[k].base_var = base_var;
    return;
  }
  if (st->scount >= SSTORE_MAX)
    return;
  SStoreEntry *e = &st->sstores[st->scount++];
  e->stack_offset = offset;
  e->btype = btype;
  e->stored_vr = -1;
  e->stored_imm = imm;
  e->base_var = base_var;
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

/* ----- Named-local (VAR vreg) store-to-load forwarding ------------------- */

static int vslot_find(const GLoadState *st, int32_t var_vr, int btype)
{
  for (int k = 0; k < st->vscount; k++) {
    if (st->vslots[k].var_vr == var_vr && st->vslots[k].btype == btype)
      return k;
  }
  return -1;
}

/* Drop every entry for this VAR (any btype): a redefinition of V's slot
 * invalidates whatever value was tracked there. */
static void vslot_remove_var(GLoadState *st, int32_t var_vr)
{
  for (int k = 0; k < st->vscount; k++) {
    if (st->vslots[k].var_vr == var_vr) {
      st->vslots[k] = st->vslots[--st->vscount];
      k--;
    }
  }
}

/* Drop entries whose stored value was produced by vr — only meaningful at the
 * non-SSA regalloc-time run of this pass, where a TEMP can be reassigned. */
static void vslot_remove_stored_vr(GLoadState *st, int32_t vr)
{
  for (int k = 0; k < st->vscount; k++) {
    if (st->vslots[k].stored_vr == vr) {
      st->vslots[k] = st->vslots[--st->vscount];
      k--;
    }
  }
}

static void vslot_track_vr(GLoadState *st, int32_t var_vr, int btype, int32_t stored_vr)
{
  vslot_remove_var(st, var_vr);
  if (st->vscount >= VSLOT_MAX)
    return;
  VSlotEntry *e = &st->vslots[st->vscount++];
  e->var_vr = var_vr;
  e->btype = btype;
  e->stored_vr = stored_vr;
}

static void vslot_track_imm(GLoadState *st, int32_t var_vr, int btype, IROperand imm)
{
  vslot_remove_var(st, var_vr);
  if (st->vscount >= VSLOT_MAX)
    return;
  VSlotEntry *e = &st->vslots[st->vscount++];
  e->var_vr = var_vr;
  e->btype = btype;
  e->stored_vr = -1;
  e->stored_imm = imm;
}

/* A VAR is safe to forward only when no pointer can name its slot: it must
 * not be address-taken.  Mirrors var_tmp_fwd, which likewise skips aliasable
 * locals; a non-addrtaken local can only be reached through its own name, so
 * the sole modifiers are direct defs (handled) and calls / back-edges (which
 * clear the whole table). */
static int vslot_var_forwardable(TCCIRState *ir, int32_t var_vr)
{
  if (var_vr < 0 || TCCIR_DECODE_VREG_TYPE(var_vr) != TCCIR_VREG_TYPE_VAR)
    return 0;
  IRLiveInterval *vi = tcc_ir_vreg_live_interval(ir, var_vr);
  if (!vi || vi->addrtaken)
    return 0;
  return 1;
}

/* Record `var_vr <- q.src1` as forwardable when the source is a plain register
 * value or an immediate; otherwise the stored value has no simpler form to
 * forward, so drop the stale entry. */
static void vslot_track_store(IRSSAOptCtx *ctx, GLoadState *st, int32_t var_vr, IRQuadCompact *q)
{
  TCCIRState *ir = ctx->ir;
  IROperand src = tcc_ir_op_get_src1(ir, q);
  int32_t svr = irop_get_vreg(src);
  if (svr >= 0 && src.tag == IROP_TAG_VREG && !src.is_lval && !src.is_llocal &&
      TCCIR_DECODE_VREG_TYPE(svr) == TCCIR_VREG_TYPE_TEMP) {
    /* Skip a TEMP that holds a computed stack/symbol address (LEA source):
     * forwarding it and letting V die breaks downstream stack DSE, which keys
     * off the VAR that holds the address (var_tmp_fwd's LEA-source guard). */
    IRSSAVregInfo *svi = ssa_opt_vinfo(ctx, svr);
    if (svi && svi->def_count == 1 && svi->def_instr >= 0 &&
        ir->compact_instructions[svi->def_instr].op == TCCIR_OP_LEA) {
      vslot_remove_var(st, var_vr);
      return;
    }
    vslot_track_vr(st, var_vr, irop_get_btype(src), svr);
  } else if (irop_is_immediate(src) && !src.is_lval) {
    vslot_track_imm(st, var_vr, irop_get_btype(src), src);
  } else {
    vslot_remove_var(st, var_vr);
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

/* Textual def counts for VAR/PARAM vregs, swept once per pass invocation.
 * A canonical base with at most one def is version-stable across copy chains:
 * every in-walk def kills its entries, and single-def re-execution (loops)
 * always re-runs def -> copy -> deref in order.  Multi-def bases are only safe
 * in the zero-hop `*V` form, where the lookup reads the current value. */
typedef struct {
  uint8_t *var_defs;
  uint8_t *param_defs;
  int nvar;
  int nparam;
} LcseDefCounts;

static void lcse_count_def(const LcseDefCounts *dc, int32_t vr)
{
  if (vr < 0)
    return;
  int type = TCCIR_DECODE_VREG_TYPE(vr);
  int pos = TCCIR_DECODE_VREG_POSITION(vr);
  if (type == TCCIR_VREG_TYPE_VAR && pos < dc->nvar && dc->var_defs[pos] < 255)
    dc->var_defs[pos]++;
  else if (type == TCCIR_VREG_TYPE_PARAM && pos < dc->nparam && dc->param_defs[pos] < 255)
    dc->param_defs[pos]++;
}

static void lcse_count_defs(TCCIRState *ir, const LcseDefCounts *dc)
{
  for (int i = 0; i < ir->next_instruction_index; i++) {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;
    /* POSTINC ops advance their pointer operand without encoding a def. */
    if (q->op == TCCIR_OP_LOAD_POSTINC || q->op == TCCIR_OP_STORE_POSTINC) {
      lcse_count_def(dc, irop_get_vreg(tcc_ir_op_get_src1(ir, q)));
      lcse_count_def(dc, irop_get_vreg(tcc_ir_op_get_dest(ir, q)));
      continue;
    }
    if (!irop_config[q->op].has_dest)
      continue;
    IROperand d = tcc_ir_op_get_dest(ir, q);
    /* An is_lval VREG dest is a write through the pointer, not a def of it;
     * a STACKOFF dest carrying a vreg is the named local's slot form. */
    if (d.tag == IROP_TAG_VREG && !d.is_lval)
      lcse_count_def(dc, irop_get_vreg(d));
    else if (d.tag == IROP_TAG_STACKOFF)
      lcse_count_def(dc, irop_get_vreg(d));
  }
}

static int lcse_base_stable(const LcseDefCounts *dc, int32_t vr)
{
  int type = TCCIR_DECODE_VREG_TYPE(vr);
  int pos = TCCIR_DECODE_VREG_POSITION(vr);
  if (type == TCCIR_VREG_TYPE_TEMP)
    return 1;
  if (type == TCCIR_VREG_TYPE_VAR)
    return pos < dc->nvar && dc->var_defs[pos] <= 1;
  if (type == TCCIR_VREG_TYPE_PARAM)
    return pos < dc->nparam && dc->param_defs[pos] == 0;
  return 0;
}

/* A direct def of an address-taken VAR/PARAM (`V <-- T SUB #imm`, plain ALU
 * or ASSIGN — not a STORE op) still writes V's stack slot, memory that the
 * TEMP-pointer-keyed trackers (iloads, tvstores) may name through a `&V`
 * pointer.  Keying by vreg ID can't see that aliasing, so drop both trackers
 * (fuzz ptr seed 6734: `p = &u; ..= *p; u = expr; ..= *p` — the second read
 * CSE'd to the first across u's update). */
static void ptr_state_kill_for_addrtaken_def(TCCIRState *ir, GLoadState *st, int32_t dvr)
{
  int type = TCCIR_DECODE_VREG_TYPE(dvr);
  if (type != TCCIR_VREG_TYPE_VAR && type != TCCIR_VREG_TYPE_PARAM)
    return;
  IRLiveInterval *vi = tcc_ir_vreg_live_interval(ir, dvr);
  if (vi && !vi->addrtaken)
    return;
  st->ilcount = 0;
  st->tvcount = 0;
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
      } else if (e_type == TCCIR_VREG_TYPE_VAR) {
        /* VAR base: the pointer may hold &local (the store may hit the
         * pointee), and the stack store may even rewrite the pointer's own
         * slot.  Kill conservatively. */
        kill = 1;
      }
      /* PARAM base: caller-supplied pointer; a fresh local-stack store can't
       * reach its pointee unless the address escaped.  Skip kill. */
    }
    if (kill) {
      st->iloads[k] = st->iloads[--st->ilcount];
      k--;
    }
  }
}

/* Kill iload entries a *direct* stack store (`StackLoc[off] <- val`) may alias.
 *
 * The main STORE handler treats STACKOFF-dest stores as unable to alias the
 * iload tracker (store_aliases_globals=0) — a shortcut that was only sound when
 * iload held global-array LOAD_INDEXED entries.  The canonical TEMP-DEREF LOAD
 * CSE now also tracks entries whose base is a VAR pointer holding the address of
 * a *local* array (`p = &arr[i]`; ptr fuzz seed 380495: `*p5` CSE'd across the
 * direct stack store `arr4[1] = ...` that is the very slot `*p5` names).  Such
 * entries must be invalidated by a stack store.
 *
 * Precise for TEMP bases that resolve to a stack offset (when store_off_exact,
 * i.e. the store names a real slot rather than a placeholder-offset named
 * local); conservative for VAR bases (a possibly-multi-def named pointer that
 * may point into the frame).  PARAM bases and TEMP bases resolving to non-stack
 * (global/heap) memory cannot alias a fresh local slot, so they are preserved. */
static void iload_kill_for_direct_stack_store(IRSSAOptCtx *ctx, GLoadState *st,
                                              int store_off, int store_size,
                                              int store_off_exact)
{
  int store_lo = store_off, store_hi = store_off + store_size;
  for (int k = 0; k < st->ilcount; k++) {
    const ILoadEntry *e = &st->iloads[k];
    int kill = 0;
    int etype = TCCIR_DECODE_VREG_TYPE(e->base_vr);
    if (etype == TCCIR_VREG_TYPE_TEMP) {
      int base_off = ssa_opt_resolve_lea_stackloc(ctx, e->base_vr);
      if (base_off != INT_MIN) {
        if (!store_off_exact) {
          /* Store offset is a placeholder (named-local slot form); we can't
           * compare byte ranges, so any stack-resolving base may alias. */
          kill = 1;
        } else {
          int elo = base_off + (int)e->idx_imm * (1 << e->scale);
          int ehi = elo + slot_btype_bytes(e->btype);
          if (elo < store_hi && ehi > store_lo)
            kill = 1;
        }
      }
      /* TEMP base that does not resolve to the stack names global/heap
       * memory — a local stack store cannot reach it. */
    } else if (etype == TCCIR_VREG_TYPE_VAR) {
      /* A VAR pointer may hold `&localarray[i]`; a direct stack store can
       * alias its pointee and we cannot cheaply resolve a (possibly
       * multi-def) VAR's stack offset.  Invalidate conservatively. */
      kill = 1;
    }
    if (kill) {
      st->iloads[k] = st->iloads[--st->ilcount];
      k--;
    }
  }
}

/* Forward tracked `V <- value` stores into reads of V in q's source operands.
 * Mirrors the operand-rewrite discipline of the legacy var_tmp_fwd: skip the
 * callee/call-id metadata slots and address operands, require an exact btype
 * match, and never touch a double-indirect (is_llocal) reference.  Unlike the
 * single-block legacy scan this is valid across the whole dominator subtree —
 * the caller only carries vslot state into dominated blocks. */
static int vslot_forward_reads(IRSSAOptCtx *ctx, GLoadState *st, int i, IRQuadCompact *q)
{
  if (st->vscount == 0)
    return 0;
  TccIrOp op = q->op;
  if (op == TCCIR_OP_FUNCCALLVAL || op == TCCIR_OP_FUNCCALLVOID ||
      /* VLA_ALLOC clobbers its size operand's register (r = SP - r); a
       * forwarded live TEMP would be destroyed (mirrors the deref path). */
      op == TCCIR_OP_VLA_ALLOC)
    return 0;

  TCCIRState *ir = ctx->ir;
  int can_src2 = (op != TCCIR_OP_FUNCPARAMVAL && op != TCCIR_OP_FUNCPARAMVOID);
  int src1_is_address = (op == TCCIR_OP_LOAD || op == TCCIR_OP_LOAD_POSTINC ||
                         op == TCCIR_OP_LEA || op == TCCIR_OP_LOAD_INDEXED);
  int rewrites = 0;

  for (int side = 0; side < 2; side++) {
    if (side == 0 && (src1_is_address || !irop_config[op].has_src1))
      continue;
    if (side == 1 && (!can_src2 || !irop_config[op].has_src2))
      continue;

    IROperand s = side == 0 ? tcc_ir_op_get_src1(ir, q) : tcc_ir_op_get_src2(ir, q);
    if (s.tag != IROP_TAG_VREG || s.is_lval || s.is_llocal || s.is_local)
      continue;
    int32_t vr = irop_get_vreg(s);
    if (vr < 0 || TCCIR_DECODE_VREG_TYPE(vr) != TCCIR_VREG_TYPE_VAR)
      continue;

    int k = vslot_find(st, vr, irop_get_btype(s));
    if (k < 0)
      continue;
    VSlotEntry *ve = &st->vslots[k];
    IROperand new_op;
    if (ve->stored_vr >= 0) {
      new_op = irop_make_vreg(ve->stored_vr, ve->btype);
      IRSSAVregInfo *rvi = ssa_opt_vinfo(ctx, ve->stored_vr);
      if (rvi)
        ssa_opt_add_use_instr(rvi, i);
    } else {
      new_op = ve->stored_imm;
    }
    if (side == 0)
      tcc_ir_set_src1(ir, i, new_op);
    else
      tcc_ir_set_src2(ir, i, new_op);
    IRSSAVregInfo *pvi = ssa_opt_vinfo(ctx, vr);
    if (pvi)
      ssa_opt_remove_use_instr(pvi, i);
    rewrites++;
  }
  return rewrites;
}

/* resolve_lea_stackloc moved to ssa_opt.c as ssa_opt_resolve_lea_stackloc. */
#define resolve_lea_stackloc ssa_opt_resolve_lea_stackloc

typedef struct GLoadWork {
  int block;
  GLoadState *state;
} GLoadWork;

static int gload_process_block(IRSSAOptCtx *ctx, const LcseDefCounts *dc,
                               const uint8_t *reachable, GLoadState *st_init,
                               int b_init, int var_fwd)
{
  TCCIRState *ir = ctx->ir;
  IRCFG *cfg = ctx->cfg;
  int changes = 0;

  /* Iterative DFS over the dominator tree with a heap worklist instead of
   * native recursion.  Functions with deep branch nesting (one `if` per
   * source statement, e.g. memcpy-bi's 80 inlined `check()` bound checks)
   * recursed once per branch level and overflowed the 32 KB target process
   * stack in this function's prologue.  Each pending work item OWNS a heap
   * GLoadState snapshot — the same heap profile the recursive code already
   * had (it malloc'd one snapshot per branch level), now with O(1) native
   * call-stack depth. */
  GLoadWork *work = tcc_malloc(sizeof *work * 8);
  int sp = 0, cap = 8;
  {
    GLoadState *seed = tcc_malloc(sizeof *seed);
    *seed = *st_init;
    work[sp].block = b_init;
    work[sp].state = seed;
    sp++;
  }

  while (sp > 0) {
    sp--;
    int b = work[sp].block;
    GLoadState *st = work[sp].state;

  for (;;) {
  IRBasicBlock *bb = &cfg->blocks[b];

  /* If this block has any predecessor that is NOT its immediate dominator,
   * a non-dominator path (loop back-edge or cross-edge) can modify tracked
   * memory.  Conservatively drop ALL forwarding state — not just the store
   * trackers but also the available-LOAD caches: a load made available in a
   * dominator is NOT valid here if the loop body (reached via the back-edge)
   * contains a store or CALL that modifies that memory between iterations.
   * Dropping only the store trackers left e.g. a global `tok` load CSE'd
   * across a loop whose body calls functions that modify `tok` (the C
   * expression parser's `while(...) { next(); unary(); ...; t = tok; }` —
   * the loop-end reload of `t` was eliminated, so the loop spun on a stale
   * operator token and tcc rejected `#if A >= B` with "expression expected"). */
  for (int pi = 0; pi < bb->num_preds; pi++) {
    if (reachable && !reachable[bb->preds[pi]])
      continue;
    if (bb->preds[pi] != bb->idom) {
      st->count = 0;
      st->scount = 0;
      st->gscount = 0;
      st->tvcount = 0;
      st->ilcount = 0;
      st->vscount = 0;
      break;
    }
  }

  for (int i = bb->start_idx; i < bb->end_idx; i++) {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;

    /* Calls and opaque memory writers: BLOCK_COPY writes a byte range we
     * don't model, INLINE_ASM can touch anything, and the POSTINC forms
     * (formed before the regalloc-time run of this pass) both write memory
     * and advance their pointer vreg without an encoded def. */
    if (q->op == TCCIR_OP_FUNCCALLVOID || q->op == TCCIR_OP_FUNCCALLVAL ||
        q->op == TCCIR_OP_BLOCK_COPY || q->op == TCCIR_OP_INLINE_ASM ||
        q->op == TCCIR_OP_STORE_POSTINC || q->op == TCCIR_OP_LOAD_POSTINC) {
      st->count = 0;
      st->scount = 0;
      st->gscount = 0;
      st->tvcount = 0;
      st->ilcount = 0;
      st->vscount = 0;
      continue;
    }

    /* Named-local store-to-load forwarding: rewrite reads of V in this op's
     * source operands to the value most recently stored into V's slot on the
     * dominating path.  Runs before the op's own dest handling so a self-read
     * (`V <- V OP x`) still sees the prior value. */
    if (var_fwd)
      changes += vslot_forward_reads(ctx, st, i, q);

    /* T_vreg-deref forwarding into ALU op operands: when an instruction
     * other than STORE reads `T_vreg_DEREF` and a recent STORE through the
     * same T_vreg stored a value, rewrite the operand to that value.
     * Eliminates the implicit LDR the backend would emit to materialise
     * the deref. */
    if (q->op != TCCIR_OP_STORE && q->op != TCCIR_OP_STORE_INDEXED &&
        q->op != TCCIR_OP_STORE_POSTINC &&
        q->op != TCCIR_OP_LOAD && q->op != TCCIR_OP_LOAD_INDEXED &&
        q->op != TCCIR_OP_LOAD_POSTINC &&
        /* VLA_ALLOC clobbers its size operand's register (r = SP - r) — a
         * forwarded live vreg would be destroyed; slot loads use a scratch. */
        q->op != TCCIR_OP_VLA_ALLOC) {
      int rewrites = 0;
      for (int side = 0; side < 2; side++) {
        IROperand op = side == 0 ? tcc_ir_op_get_src1(ir, q) : tcc_ir_op_get_src2(ir, q);
        /* Global-deref ALU operand (`T <- GlobalSym_DEREF AND #imm`): forward
         * a tracked global store value / earlier load like the LOAD path
         * below — the fused form is what bitfield RMW sequences produce. */
        if (op.is_lval && op.is_sym && !op.is_llocal && !op.is_local) {
          IRPoolSymref *ref = irop_get_symref_ex(ir, op);
          if (ref && ref->sym && !(ref->sym->type.t & VT_VOLATILE)) {
            int op_btype = irop_get_btype(op);
            IROperand new_op;
            int have_new = 0;
            int gk = gstore_find(st, ref->sym, ref->addend, op_btype);
            if (gk >= 0) {
              GStoreEntry *ge = &st->gstores[gk];
              if (ge->stored_vr >= 0) {
                new_op = irop_make_vreg(ge->stored_vr, op_btype);
                IRSSAVregInfo *rvi = ssa_opt_vinfo(ctx, ge->stored_vr);
                if (rvi)
                  ssa_opt_add_use_instr(rvi, i);
              } else {
                new_op = ge->stored_imm;
              }
              have_new = 1;
            } else {
              int fk = gload_find(st, ref->sym, ref->addend, op_btype);
              if (fk >= 0) {
                new_op = irop_make_vreg(st->entries[fk].result_vr, op_btype);
                IRSSAVregInfo *rvi = ssa_opt_vinfo(ctx, st->entries[fk].result_vr);
                if (rvi)
                  ssa_opt_add_use_instr(rvi, i);
                have_new = 1;
              }
            }
            if (have_new) {
              if (side == 0)
                tcc_ir_set_src1(ir, i, new_op);
              else
                tcc_ir_set_src2(ir, i, new_op);
              rewrites++;
            }
          }
          continue;
        }
        /* Direct StackLoc operand (`CMP StackLoc[N], #imm`): forward a tracked
         * direct stack store like the LOAD path below (same identity rules).
         * FUNCPARAMVAL slots can denote multi-word aggregates (complex/struct
         * by value) whose size the operand btype doesn't carry — skip them.
         * INT32 only: 64-bit operands need hi/lo-half splitting the embedded
         * path can't do, subword reads carry extension semantics. */
        if (op.is_lval && !op.is_sym && !op.is_llocal &&
            op.tag == IROP_TAG_STACKOFF && q->op != TCCIR_OP_ASSIGN &&
            q->op != TCCIR_OP_FUNCPARAMVAL &&
            irop_get_btype(op) == IROP_BTYPE_INT32) {
          int32_t svr = irop_get_vreg(op);
          if (svr < 0 || TCCIR_DECODE_VREG_TYPE(svr) != TCCIR_VREG_TYPE_VAR) {
            int op_btype = irop_get_btype(op);
            int sk = sstore_find(st, irop_get_stack_offset(op));
            if (sk >= 0 && st->sstores[sk].btype == op_btype &&
                st->sstores[sk].base_var == -1) {
              SStoreEntry *se = &st->sstores[sk];
              IROperand new_op;
              if (se->stored_vr >= 0) {
                new_op = irop_make_vreg(se->stored_vr, op_btype);
                IRSSAVregInfo *rvi = ssa_opt_vinfo(ctx, se->stored_vr);
                if (rvi)
                  ssa_opt_add_use_instr(rvi, i);
              } else {
                new_op = se->stored_imm;
              }
              if (side == 0)
                tcc_ir_set_src1(ir, i, new_op);
              else
                tcc_ir_set_src2(ir, i, new_op);
              rewrites++;
            }
          }
          continue;
        }
        if (!op.is_lval || op.is_llocal || op.is_local)
          continue;
        if (op.tag != IROP_TAG_VREG)
          continue;
        int32_t pvr = irop_get_vreg(op);
        if (pvr < 0)
          continue;
        int op_btype = irop_get_btype(op);
        IROperand new_op;
        int have_new = 0;
        if (TCCIR_DECODE_VREG_TYPE(pvr) == TCCIR_VREG_TYPE_TEMP) {
          int tk = tvstore_find(st, pvr, op_btype);
          if (tk >= 0) {
            TVStoreEntry *te = &st->tvstores[tk];
            if (te->stored_vr >= 0) {
              new_op = irop_make_vreg(te->stored_vr, op_btype);
              IRSSAVregInfo *rvi = ssa_opt_vinfo(ctx, te->stored_vr);
              if (rvi)
                ssa_opt_add_use_instr(rvi, i);
            } else {
              new_op = te->stored_imm;
            }
            have_new = 1;
          }
        }
        /* No forwardable store: reuse an earlier load of the same canonical
         * (base, offset) — turns the embedded deref into a register read.
         * ASSIGN is left to the main deref-load CSE path, which also
         * re-tracks its dest to keep the CSE chain alive. */
        if (!have_new && q->op != TCCIR_OP_ASSIGN && st->ilcount > 0) {
          int32_t cb = -1, co = 0;
          if (!ssa_opt_resolve_temp_to_base_off(ctx, pvr, &cb, &co) || cb < 0) {
            cb = pvr;
            co = 0;
          }
          if (cb == pvr || lcse_base_stable(dc, cb)) {
            int fk = iload_find(st, cb, co, 0, op_btype);
            if (fk >= 0) {
              new_op = irop_make_vreg(st->iloads[fk].result_vr, op_btype);
              IRSSAVregInfo *rvi = ssa_opt_vinfo(ctx, st->iloads[fk].result_vr);
              if (rvi)
                ssa_opt_add_use_instr(rvi, i);
              have_new = 1;
            }
          }
        }
        if (!have_new)
          continue;
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
      if (st->ilcount > 0) {
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
            iload_kill_for_stack_store(ctx, st, store_base_vr, store_lo, store_hi);
          else if (can_check)
            iload_kill_for_store(st, store_base_vr, store_lo, store_hi);
          else
            st->ilcount = 0;
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
          int32_t load_base = -1;
          if (src.tag == IROP_TAG_STACKOFF) {
            int32_t svr = irop_get_vreg(src);
            if (svr < 0 || TCCIR_DECODE_VREG_TYPE(svr) != TCCIR_VREG_TYPE_VAR)
              load_off = irop_get_stack_offset(src);
          } else if (src.tag == IROP_TAG_VREG && !src.is_local) {
            int32_t pvr = irop_get_vreg(src);
            load_off = ssa_opt_resolve_lea_stackloc_ex(ctx, pvr, &load_base);
          }
          if (load_off != INT_MIN) {
            int sk = sstore_find(st, load_off);
            if (sk >= 0 && st->sstores[sk].btype == load_btype &&
                st->sstores[sk].base_var == load_base) {
              SStoreEntry *se = &st->sstores[sk];
              if (se->stored_vr < 0) {
                /* The deref source is replaced by the forwarded immediate,
                 * so the pointer vreg is no longer referenced here — drop
                 * its use record, like every sibling forwarding path.  A
                 * stale entry corrupts the pointer's use list (ptr fuzz
                 * seed 7226: a later swap-remove + count-only rebuild left
                 * the wrong entry, a live deref use vanished, and cprop/DCE
                 * deleted the pointer's def while a deref still read it). */
                if (src.tag == IROP_TAG_VREG) {
                  IRSSAVregInfo *pvi = ssa_opt_vinfo(ctx, irop_get_vreg(src));
                  if (pvi)
                    ssa_opt_remove_use_instr(pvi, i);
                }
                tcc_ir_set_src1(ir, i, se->stored_imm);
                changes++;
              }
            }
          }
        }
      }

      /* Track StackLoc stores for stack forwarding.  Record the stored
       * slot width so narrower subfield loads do not reuse wider values. */
      if (dest.tag == IROP_TAG_STACKOFF) {
        /* A real stack memory write may alias any TVStore pointer: a
         * tvstore is only tracked when its pointer did NOT resolve to a
         * stack slot, so nothing proves it doesn't point right here (ptr
         * fuzz seed 8507: `*T = const` with T = Addr[StackLoc[-32]]+4
         * survived the direct store `StackLoc[-28] <- u2` to the same
         * address and forwarded the stale constant into a later deref). */
        if (dest.is_lval || q->op == TCCIR_OP_STORE_INDEXED)
          st->tvcount = 0;
        /* A direct stack write (`StackLoc[off] <- val`) can alias iload
         * entries whose base points into the local frame (a VAR pointer
         * `&arr[i]`, or a TEMP resolving to an overlapping stack slot).  The
         * store_aliases_globals shortcut above skipped iload handling for
         * STACKOFF dests, so invalidate those entries here.  Runs before the
         * no_stack_fwd gate below — correctness, not forwarding. */
        if (st->ilcount > 0 && dest.is_lval && dest.is_local &&
            q->op == TCCIR_OP_STORE) {
          /* A real stack slot encodes its identity in the offset (vreg -1); a
           * named local carries the VAR vreg and a placeholder offset. */
          int off_exact = (irop_get_vreg(dest) < 0);
          iload_kill_for_direct_stack_store(ctx, st, irop_get_stack_offset(dest),
                                            slot_btype_bytes(irop_get_btype(dest)),
                                            off_exact);
        }
        IROperand src = tcc_ir_op_get_src1(ir, q);
        int32_t svr = irop_get_vreg(src);
        /* Direct stack stores are encoded as StackLoc lvalues.  Non-lvalue
         * STACKOFF operands are stack addresses, not memory writes. */
        if (!ctx->no_stack_fwd && dest.is_local && dest.is_lval && !dest.is_llocal) {
          int store_btype = irop_get_btype(dest);
          /* irop_get_vreg(dest) is -1 for a real stack slot (offset is the
           * identity) or the VAR vreg for a named local addressed by its slot
           * encoding (offset is a placeholder; the vreg is the identity). */
          int32_t dest_base = irop_get_vreg(dest);
          if (svr >= 0 && TCCIR_DECODE_VREG_TYPE(svr) == TCCIR_VREG_TYPE_TEMP)
            sstore_track_vr(st, irop_get_stack_offset(dest), store_btype, svr, dest_base);
          else if (irop_is_immediate(src))
            sstore_track_imm(st, irop_get_stack_offset(dest), store_btype, src, dest_base);
          else {
            int off = irop_get_stack_offset(dest);
            sstore_invalidate_overlap(st, off, store_btype);
            sstore_remove_offset(st, off);
          }
        } else if (q->op == TCCIR_OP_STORE_INDEXED) {
          /* Indexed write through a stack base address (Addr[StackLoc[B]] +
           * idx*scale).  The base-offset-only removal in the plain branch below
           * dropped just the B slot, leaving the sibling slots forwardable even
           * though a runtime index can land on any of them (fuzz seed 2657:
           * `arr[i]=v` with runtime i, then a fully-unrolled `for k arr[k]` whose
           * reads wrongly forwarded the initializer values for k != B).  With a
           * constant index invalidate just the exact slot; with a runtime index
           * conservatively drop all stack-store and indexed-load forwarding. */
          IROperand idx = tcc_ir_op_get_src2(ir, q);
          IROperand sc = tcc_ir_op_get_scale(ir, q);
          if (irop_is_immediate(idx) && irop_is_immediate(sc)) {
            int off = irop_get_stack_offset(dest) +
                      (int)irop_get_imm32(idx) * (1 << irop_get_imm32(sc));
            sstore_invalidate_overlap(st, off, irop_get_btype(dest));
            sstore_remove_offset(st, off);
            st->ilcount = 0;
          } else {
            st->scount = 0;
            st->ilcount = 0;
          }
        } else {
          /* A non-direct STACKOFF write may expose the address. */
          int off = irop_get_stack_offset(dest);
          int k = sstore_find(st, off);
          if (k >= 0)
            st->sstores[k] = st->sstores[--st->scount];
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
        int32_t store_base = -1;
        int eff_off = ssa_opt_indirect_stack_offset_ex(ctx, q, SSA_OPT_INDIRECT_DEST, &store_base);
        if (eff_off != INT_MIN) {
          if (!ctx->no_stack_fwd) {
            IROperand src = tcc_ir_op_get_src1(ir, q);
            int store_btype = irop_get_btype(dest);
            int32_t svr = irop_get_vreg(src);
            if (svr >= 0 && TCCIR_DECODE_VREG_TYPE(svr) == TCCIR_VREG_TYPE_TEMP)
              sstore_track_vr(st, eff_off, store_btype, svr, store_base);
            else if (irop_is_immediate(src))
              sstore_track_imm(st, eff_off, store_btype, src, store_base);
            else
              sstore_remove_offset(st, eff_off);
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
              tvstore_track_imm(st, ptr_vr, store_btype, src);
              tracked = 1;
            } else if (svr >= 0 &&
                       TCCIR_DECODE_VREG_TYPE(svr) == TCCIR_VREG_TYPE_TEMP &&
                       src.tag == IROP_TAG_VREG && !src.is_lval) {
              tvstore_track_vr(st, ptr_vr, store_btype, svr);
              tracked = 1;
            }
            if (tracked) {
              /* Invalidate aliasing state: an unknown pointer may write
               * over any tracked address.  Drop all other tvstores (with
               * a different ptr_vr or btype), all global CSE entries, all
               * global stores, and all stack stores. */
              for (int kk = 0; kk < st->tvcount; kk++) {
                if (st->tvstores[kk].ptr_vr != ptr_vr ||
                    st->tvstores[kk].btype != store_btype) {
                  st->tvstores[kk] = st->tvstores[--st->tvcount];
                  kk--;
                }
              }
              st->count = 0;
              st->scount = 0;
              st->gscount = 0;
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
       * the forward state intact.
       *
       * Gated to q->op == TCCIR_OP_STORE: STORE_INDEXED / STORE_POSTINC
       * with a non-lval dest are real memory writes through a vreg-held
       * base (the indexed form's dest IS the base pointer).  Those must
       * NOT be treated as register-copy assignments — fall through to the
       * generic invalidate-all path below so subsequent loads can't
       * forward stale stack/global values across the indexed write.
       * (Earlier branches already handled the cases where eff_off resolves
       * to a specific stack slot; reaching here means the index didn't
       * resolve, so the write could touch arbitrary memory.) */
      if (!dest.is_lval && q->op == TCCIR_OP_STORE) {
        int32_t dvr = irop_get_vreg(dest);
        int dtype = (dvr >= 0) ? TCCIR_DECODE_VREG_TYPE(dvr) : -1;
        if (dtype == TCCIR_VREG_TYPE_VAR || dtype == TCCIR_VREG_TYPE_TEMP) {
          if (dvr >= 0) {
            gload_remove_vr(st, dvr);
            sstore_remove_vr(st, dvr);
            gstore_remove_vr(st, dvr);
            tvstore_remove_vr(st, dvr);
            iload_remove_vr(st, dvr);
            vslot_remove_stored_vr(st, dvr);
            ptr_state_kill_for_addrtaken_def(ir, st, dvr);
            /* `V <- value [STORE]`: forward the value to dominated reads of the
             * non-address-taken local V. */
            if (dtype == TCCIR_VREG_TYPE_VAR) {
              if (var_fwd && vslot_var_forwardable(ir, dvr))
                vslot_track_store(ctx, st, dvr, q);
              else
                vslot_remove_var(st, dvr);
            }
          }
          continue;
        }
      }

      if (dest.is_sym && dest.is_lval) {
        /* Same aliasing gap as stack stores: an unresolved TVStore pointer
         * may name this very global (`&sym + off` LEAs that never became
         * SymRef operands are exactly what tvstores track), so a direct
         * sym store must drop them. */
        st->tvcount = 0;
        IRPoolSymref *sref = irop_get_symref_ex(ir, dest);
        if (sref && sref->sym) {
          for (int k = 0; k < st->count; k++) {
            if (st->entries[k].sym == sref->sym) {
              st->entries[k] = st->entries[--st->count];
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
              gstore_track_imm(st, sref->sym, sref->addend, store_btype, sval);
            } else if (svr >= 0 && TCCIR_DECODE_VREG_TYPE(svr) == TCCIR_VREG_TYPE_TEMP &&
                       sval.tag == IROP_TAG_VREG && !sval.is_lval) {
              /* Stored vreg whose single def is `ASSIGN #imm`: track the
               * immediate.  Sound across blocks (the SSA def dominates this
               * store), and unlike a vreg entry the forwarded constant keeps
               * folding downstream — cprop/fold refuse cross-block vreg
               * resolution. */
              IRSSAVregInfo *svi = ssa_opt_vinfo(ctx, svr);
              IROperand dimm = IROP_NONE;
              int have_imm = 0;
              if (svi && svi->def_count == 1 && svi->def_instr >= 0) {
                IRQuadCompact *dq = &ir->compact_instructions[svi->def_instr];
                if (dq->op == TCCIR_OP_ASSIGN) {
                  IROperand dsrc = tcc_ir_op_get_src1(ir, dq);
                  if (irop_is_immediate(dsrc) && !dsrc.is_lval &&
                      irop_get_btype(dsrc) == store_btype) {
                    dimm = dsrc;
                    have_imm = 1;
                  }
                }
              }
              if (have_imm)
                gstore_track_imm(st, sref->sym, sref->addend, store_btype, dimm);
              else
                gstore_track_vr(st, sref->sym, sref->addend, store_btype, svr);
            } else {
              /* Value form we don't model — invalidate this slot. */
              gstore_invalidate_overlap(st, sref->sym, sref->addend, store_btype);
            }
          } else if (q->op == TCCIR_OP_STORE && !width_safe) {
            /* Sub-word store: don't forward; also invalidate any stale entry
             * at this address so we don't propagate a wider stale value. */
            gstore_invalidate_overlap(st, sref->sym, sref->addend, store_btype_chk);
          } else if (q->op == TCCIR_OP_STORE_INDEXED) {
            /* Runtime index touches an unknown offset within sym. Drop all
             * entries for this sym. */
            gstore_remove_sym(st, sref->sym);
          }
        }
      } else {
        st->count = 0;
        st->scount = 0;
        st->gscount = 0;
        st->tvcount = 0;
        st->ilcount = 0;
      }
      continue;
    }

    if (irop_config[q->op].has_dest && q->op != TCCIR_OP_STORE &&
        q->op != TCCIR_OP_STORE_INDEXED && q->op != TCCIR_OP_STORE_POSTINC) {
      IROperand qdest = tcc_ir_op_get_dest(ir, q);
      /* Non-STORE op writing through a TEMP pointer (`*T <-- ...`): the
       * pointee is unknown, so every tracked location may be stale. */
      if (qdest.is_lval && !qdest.is_local && !qdest.is_sym &&
          qdest.tag == IROP_TAG_VREG) {
        int32_t qpvr = irop_get_vreg(qdest);
        if (qpvr >= 0 && TCCIR_DECODE_VREG_TYPE(qpvr) == TCCIR_VREG_TYPE_TEMP) {
          st->count = 0;
          st->scount = 0;
          st->gscount = 0;
          st->tvcount = 0;
          st->ilcount = 0;
          /* vslot survives: a non-address-taken local can't be the pointee of
           * an arbitrary `*T` write. */
          continue;
        }
      }
      if (qdest.tag == IROP_TAG_STACKOFF && qdest.is_local)
        sstore_remove_offset(st, irop_get_stack_offset(qdest));
      int32_t qdvr = irop_get_vreg(qdest);
      if (qdvr >= 0 && TCCIR_DECODE_VREG_TYPE(qdvr) == TCCIR_VREG_TYPE_VAR &&
          qdest.tag != IROP_TAG_STACKOFF)
        st->scount = 0;
      if (qdvr >= 0) {
        gload_remove_vr(st, qdvr);
        sstore_remove_vr(st, qdvr);
        gstore_remove_vr(st, qdvr);
        tvstore_remove_vr(st, qdvr);
        iload_remove_vr(st, qdvr);
        vslot_remove_stored_vr(st, qdvr);
        ptr_state_kill_for_addrtaken_def(ir, st, qdvr);
        /* `V <- value [ASSIGN]` forwards value to dominated reads of a
         * non-address-taken local; any other write to V (computed ALU result,
         * LOAD, STACKOFF slot form) just kills the stale entry. */
        if (TCCIR_DECODE_VREG_TYPE(qdvr) == TCCIR_VREG_TYPE_VAR) {
          if (var_fwd && q->op == TCCIR_OP_ASSIGN && qdest.tag == IROP_TAG_VREG &&
              !qdest.is_lval && vslot_var_forwardable(ir, qdvr))
            vslot_track_store(ctx, st, qdvr, q);
          else
            vslot_remove_var(st, qdvr);
        }
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
        /* The base is read directly at this instruction (zero-hop), so any
         * TEMP/VAR base is version-safe: every redef kills its entries.
         * PARAM bases still require zero textual defs — see LcseDefCounts. */
        int il_base_type = TCCIR_DECODE_VREG_TYPE(il_base_vr);
        if (il_base_type == TCCIR_VREG_TYPE_PARAM) {
          if (!lcse_base_stable(dc, il_base_vr))
            continue;
        } else if (il_base_type != TCCIR_VREG_TYPE_TEMP &&
                   il_base_type != TCCIR_VREG_TYPE_VAR)
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
        int32_t load_base = -1;
        int eff_off = ssa_opt_indirect_stack_offset_ex(ctx, q, SSA_OPT_INDIRECT_SRC1, &load_base);
        if (eff_off != INT_MIN) {
          int sk = sstore_find(st, eff_off);
          if (sk >= 0 && st->sstores[sk].btype == il_btype &&
              st->sstores[sk].base_var == load_base) {
            SStoreEntry *se = &st->sstores[sk];
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

      int found = iload_find(st, il_base_vr, il_idx, il_scale, il_btype);
      if (found >= 0) {
        int32_t earlier_vr = st->iloads[found].result_vr;
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
        iload_track(st, il_base_vr, il_idx, il_scale, il_btype, il_dest_vr);
        changes++;
        continue;
      }
      iload_track(st, il_base_vr, il_idx, il_scale, il_btype, il_dest_vr);
      continue;
    }

    if (q->op != TCCIR_OP_LOAD && q->op != TCCIR_OP_ASSIGN)
      continue;

    IROperand src1 = tcc_ir_op_get_src1(ir, q);
    /* ASSIGN with a pointer-deref source (`T2 <-- T1***DEREF***`) is a load
     * in disguise — the frontend's dominant encoding for pointer reads.
     * Route it through the same forwarding/CSE paths as LOAD. */
    if (q->op == TCCIR_OP_ASSIGN &&
        !(src1.is_lval && !src1.is_sym && !src1.is_local && !src1.is_llocal &&
          src1.tag == IROP_TAG_VREG))
      continue;
    IROperand dest = tcc_ir_op_get_dest(ir, q);
    int32_t dest_vr = irop_get_vreg(dest);
    if (dest_vr < 0 || TCCIR_DECODE_VREG_TYPE(dest_vr) != TCCIR_VREG_TYPE_TEMP)
      continue;
    if (dest.is_lval)
      continue;
    int dest_btype = irop_get_btype(dest);
    if (q->op == TCCIR_OP_ASSIGN && irop_get_btype(src1) != dest_btype)
      continue;

    /* T_vreg-deref store-load forwarding: LOAD `T_vreg_DEREF` where a
     * recent STORE through the same T_vreg recorded the value. */
    if (src1.is_lval && !src1.is_sym && !src1.is_local && !src1.is_llocal &&
        src1.tag == IROP_TAG_VREG) {
      int32_t ptr_vr_l = irop_get_vreg(src1);
      if (ptr_vr_l >= 0 && TCCIR_DECODE_VREG_TYPE(ptr_vr_l) == TCCIR_VREG_TYPE_TEMP) {
        int tk = tvstore_find(st, ptr_vr_l, dest_btype);
        if (tk >= 0) {
          TVStoreEntry *te = &st->tvstores[tk];
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
      if (ptr_vr >= 0) {
        int32_t canon_base = -1, canon_off = 0;
        /* Unresolvable pointer def (e.g. itself a deref load): key by the
         * raw vreg — zero-hop, so per-def invalidation keeps it sound. */
        if (!ssa_opt_resolve_temp_to_base_off(ctx, ptr_vr, &canon_base, &canon_off) ||
            canon_base < 0) {
          canon_base = ptr_vr;
          canon_off = 0;
        }
        /* Base stability: TEMP roots are single-def by construction; VAR and
         * PARAM roots need at most one textual def (LcseDefCounts) so copy
         * chains can't smuggle a stale pointer version.  The zero-hop form
         * always reads the current value, so any def count is fine —
         * per-def entry invalidation keeps it sound. */
        if (canon_base == ptr_vr || lcse_base_stable(dc, canon_base)) {
          int found = iload_find(st, canon_base, canon_off, 0, dest_btype);
          if (found >= 0) {
            int32_t earlier_vr = st->iloads[found].result_vr;
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
            iload_track(st, canon_base, canon_off, 0, dest_btype, dest_vr);
            changes++;
            continue;
          }
          iload_track(st, canon_base, canon_off, 0, dest_btype, dest_vr);
        }
      }
    }

    /* Stack store-load forwarding */
    if (src1.is_lval && !src1.is_sym) {
      int stack_off = INT_MIN;
      int32_t load_base = -1;

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
        stack_off = ssa_opt_resolve_lea_stackloc_ex(ctx, ptr_vr, &load_base);
      }

      if (stack_off != INT_MIN) {
        int sk = sstore_find(st, stack_off);
        if (sk >= 0) {
          SStoreEntry *se = &st->sstores[sk];
          if (se->btype != dest_btype)
            continue;
          /* Only forward when the store and this load name the same location:
           * for `&VAR` addresses the offset is a shared placeholder, so the
           * canonical base must match (ptr fuzz seed 67). */
          if (se->base_var != load_base)
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
          /* Drop this LOAD's use of its old base pointer: the deref source is
           * being replaced by the forwarded value, so the base vreg is no
           * longer referenced here.  Omitting this (unlike every sibling
           * forwarding path above) leaves a stale use entry that corrupts the
           * base's use-list — a later swap-remove then drops the wrong entry
           * (e.g. a still-live STORE-through-base address use), so a
           * subsequent copy-prop fails to rewrite that store's address and it
           * dereferences an undefined spill slot (95_bitfields TEST2 PACKED
           * RMW store at -O1). */
          IRSSAVregInfo *pvi = ssa_opt_vinfo(ctx, irop_get_vreg(src1));
          if (pvi)
            ssa_opt_remove_use_instr(pvi, i);
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
    int gstore_k = gstore_find(st, ref->sym, ref->addend, dest_btype);
    if (gstore_k >= 0) {
      GStoreEntry *ge = &st->gstores[gstore_k];
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
      gload_track(st, ref->sym, ref->addend, dest_btype, dest_vr);
      changes++;
      continue;
    }

    int found = gload_find(st, ref->sym, ref->addend, dest_btype);

    if (found >= 0) {
      int32_t earlier_vr = st->entries[found].result_vr;
      IROperand new_src = irop_make_vreg(earlier_vr, dest_btype);

      IRSSAVregInfo *rvi = ssa_opt_vinfo(ctx, earlier_vr);
      if (rvi)
        ssa_opt_add_use_instr(rvi, i);

      q->op = TCCIR_OP_ASSIGN;
      tcc_ir_set_src1(ir, i, new_src);
      tcc_ir_set_src2(ir, i, IROP_NONE);
      changes++;
    } else {
      gload_track(st, ref->sym, ref->addend, dest_btype, dest_vr);
    }
  }

  /* Walk the dominator children.  Each child must start from the forwarding
   * state as it stands at the END of this block.
   *
   * Two stack-frugality measures keep this off the device's 32 KB process
   * stack on deeply nested functions:
   *   1. `st` is passed by POINTER, not by value — the GLoadState is ~2.6 KB,
   *      and a by-value parameter multiplied by the dominator-tree depth blew
   *      the stack (USAGE-STKOF in this function's prologue).
   *   2. A single-child block is iterated, not recursed: the child inherits
   *      this block's exact end state with no sibling to preserve, so we just
   *      advance `b` and loop.  That collapses long straight-line dominator
   *      chains (the common deep case) to O(1) stack; recursion depth is then
   *      only the branch-nesting depth.
   * For genuine multi-way branches, children mutate `*st` in place, so we
   * snapshot/restore around every child except the last; the snapshot lives on
   * the heap, not this recursion frame. */
  if (bb->num_dom_children == 1) {
    b = bb->dom_children[0];
    continue;
  }
  if (bb->num_dom_children == 0)
    break;
  /* Multi-way: continue with child[0] on the live state, and push every
   * other child with its own heap snapshot of this block's end state.
   * Sibling subtrees are independent given the start state, so the DFS
   * order among them does not matter. */
  for (int ci = 1; ci < bb->num_dom_children; ci++) {
    if (sp == cap) {
      cap *= 2;
      work = tcc_realloc(work, sizeof *work * cap);
    }
    GLoadState *snap = tcc_malloc(sizeof *snap);
    *snap = *st;
    work[sp].block = bb->dom_children[ci];
    work[sp].state = snap;
    sp++;
  }
  b = bb->dom_children[0];
  continue;
  } /* for (;;) */

    tcc_free(st);
  } /* while (sp > 0) */

  tcc_free(work);
  return changes;
}

int ssa_opt_load_cse(IRSSAOptCtx *ctx)
{
  IRCFG *cfg = ctx->cfg;
  if (!cfg || cfg->num_blocks == 0)
    return 0;

  LcseDefCounts dc;
  dc.nvar = ctx->ir->next_local_variable;
  dc.nparam = ctx->ir->next_parameter;
  dc.var_defs = tcc_mallocz(dc.nvar > 0 ? dc.nvar : 1);
  dc.param_defs = tcc_mallocz(dc.nparam > 0 ? dc.nparam : 1);
  lcse_count_defs(ctx->ir, &dc);

  GLoadState initial;
  initial.count = 0;
  initial.scount = 0;
  initial.gscount = 0;
  initial.tvcount = 0;
  initial.ilcount = 0;
  initial.vscount = 0;
  uint8_t *reachable = ssa_opt_compute_reachable_blocks(ctx);
  int var_fwd = !tcc_ir_opt_pass_disabled("ssa:load_cse:var_fwd");
  int changes = gload_process_block(ctx, &dc, reachable, &initial, 0, var_fwd);
  tcc_free(reachable);
  tcc_free(dc.var_defs);
  tcc_free(dc.param_defs);
  return changes;
}
