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
#include "load_cse.h"
#include <limits.h>

extern int tcc_ir_opt_pass_disabled(const char *name);

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
  /* -1 for a real stack slot; else the &VAR base vreg whose offset is a shared placeholder (ptr fuzz seed 67) */
  int32_t base_var;
} SStoreEntry;

typedef struct {
  Sym *sym;
  int64_t addend;
  int btype;
  int32_t stored_vr;    /* TEMP vreg, or -1 if immediate */
  IROperand stored_imm; /* valid when stored_vr == -1 */
} GStoreEntry;

typedef struct {
  int32_t ptr_vr;
  int btype;
  int32_t stored_vr;      /* TEMP vreg, or -1 if immediate */
  IROperand stored_imm;   /* valid when stored_vr == -1 */
} TVStoreEntry;

typedef struct {
  int32_t var_vr;
  int btype;
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
    /* skip exact size+offset match; sstore_track_* overwrites it */
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

/* register-form write to V changes the slot a `&V`-keyed entry describes */
static void sstore_remove_base_var(GLoadState *st, int32_t var_vr)
{
  for (int k = 0; k < st->scount; k++) {
    if (st->sstores[k].base_var == var_vr) {
      st->sstores[k] = st->sstores[--st->scount];
      k--;
    }
  }
}

static int vslot_find(const GLoadState *st, int32_t var_vr, int btype)
{
  for (int k = 0; k < st->vscount; k++) {
    if (st->vslots[k].var_vr == var_vr && st->vslots[k].btype == btype)
      return k;
  }
  return -1;
}

static void vslot_remove_var(GLoadState *st, int32_t var_vr)
{
  for (int k = 0; k < st->vscount; k++) {
    if (st->vslots[k].var_vr == var_vr) {
      st->vslots[k] = st->vslots[--st->vscount];
      k--;
    }
  }
}

/* only meaningful at the regalloc-time run, where a TEMP can be reassigned */
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

/* forwardable only if not address-taken: no pointer can alias its slot */
static int vslot_var_forwardable(TCCIRState *ir, int32_t var_vr)
{
  if (var_vr < 0 || TCCIR_DECODE_VREG_TYPE(var_vr) != TCCIR_VREG_TYPE_VAR)
    return 0;
  IRLiveInterval *vi = tcc_ir_vreg_live_interval(ir, var_vr);
  if (!vi || vi->addrtaken)
    return 0;
  return 1;
}

static void vslot_track_store(IRSSAOptCtx *ctx, GLoadState *st, int32_t var_vr, IRQuadCompact *q)
{
  TCCIRState *ir = ctx->ir;
  IROperand src = tcc_ir_op_get_src1(ir, q);
  int32_t svr = irop_get_vreg(src);
  if (svr >= 0 && src.tag == IROP_TAG_VREG && !src.is_lval && !src.is_llocal &&
      TCCIR_DECODE_VREG_TYPE(svr) == TCCIR_VREG_TYPE_TEMP) {
    /* skip LEA-source TEMP: forwarding it breaks downstream stack DSE (var_tmp_fwd LEA-source guard) */
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

static int gstore_find(const GLoadState *st, Sym *sym, int64_t addend, int btype)
{
  for (int k = 0; k < st->gscount; k++) {
    if (st->gstores[k].sym == sym && st->gstores[k].addend == addend &&
        st->gstores[k].btype == btype)
      return k;
  }
  return -1;
}

/* exact (sym,addend,btype) match preserved; the calling tracker overwrites it */
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

/* a base with <=1 def is version-stable across copy chains; multi-def only safe in zero-hop `*V` form */
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
    /* POSTINC advances its pointer operand without encoding a def */
    if (q->op == TCCIR_OP_LOAD_POSTINC || q->op == TCCIR_OP_STORE_POSTINC) {
      lcse_count_def(dc, irop_get_vreg(tcc_ir_op_get_src1(ir, q)));
      lcse_count_def(dc, irop_get_vreg(tcc_ir_op_get_dest(ir, q)));
      continue;
    }
    if (!irop_config[q->op].has_dest)
      continue;
    IROperand d = tcc_ir_op_get_dest(ir, q);
    /* is_lval VREG dest writes through the pointer (not a def); STACKOFF dest carrying a vreg is the named local's slot form */
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

/* direct def of addr-taken VAR/PARAM writes its slot, aliasable via a `&V` pointer; drop vreg-keyed trackers (fuzz ptr seed 6734) */
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

/* different base_vr killed conservatively: distinct TEMPs may alias the same memory */
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

/* Stack provenance of an ILoadEntry base, for the case
 * ssa_opt_resolve_lea_stackloc() could NOT pin to an exact offset.
 *
 * "Unresolved" must not be read as "names global or heap memory": the common
 * miss is a frame address plus a RUNTIME term (`&arr[0] + (i << 4)` for
 * `arr[i][k]`), whose address still lands squarely in the frame — treating it
 * as non-aliasing let a direct `StackLoc[n] <- v` store keep a stale indexed
 * load alive across it (fuzz seed agg_deep:43933, test 397).
 *
 * So the answer defaults to "may be frame" and only turns into 0 when EVERY
 * reachable operand of the address computation is provably outside this frame:
 * a global/static symbol address, a caller-supplied PARAM pointer, or a plain
 * constant.  Any load-sourced or otherwise opaque term keeps the kill. */
static int iload_base_may_be_frame(IRSSAOptCtx *ctx, int32_t vr, int depth)
{
  if (vr < 0 || depth > 8)
    return 1;
  int type = TCCIR_DECODE_VREG_TYPE(vr);
  if (type == TCCIR_VREG_TYPE_PARAM)
    return 0;                     /* caller pointer: predates this frame */
  if (type != TCCIR_VREG_TYPE_TEMP)
    return 1;                     /* VAR &co: may hold &local */

  IRSSAVregInfo *vi = ssa_opt_vinfo(ctx, vr);
  if (!vi || vi->def_instr < 0 || vi->def_count != 1)
    return 1;

  TCCIRState *ir = ctx->ir;
  IRQuadCompact *dq = &ir->compact_instructions[vi->def_instr];
  switch (dq->op) {
  case TCCIR_OP_ASSIGN:
  case TCCIR_OP_LEA:
  case TCCIR_OP_ADD:
  case TCCIR_OP_SUB:
  case TCCIR_OP_OR:
  case TCCIR_OP_XOR:
  case TCCIR_OP_AND:
  case TCCIR_OP_SHL:
    break;
  default:
    return 1;                     /* LOAD/CALL/…: opaque provenance */
  }

  for (int s = 0; s < 2; s++) {
    if (s == 0 ? !irop_config[dq->op].has_src1 : !irop_config[dq->op].has_src2)
      continue;
    IROperand op = s == 0 ? tcc_ir_op_get_src1(ir, dq) : tcc_ir_op_get_src2(ir, dq);
    if (op.tag == IROP_TAG_STACKOFF || op.is_local || op.is_llocal)
      return 1;                   /* a frame address feeds the computation */
    if (op.is_lval)
      return 1;                   /* value loaded from memory: unknown */
    if (op.tag == IROP_TAG_SYMREF || op.is_sym)
      continue;                   /* global/static address term */
    if (irop_is_immediate(op))
      continue;                   /* constant offset/mask term */
    if (iload_base_may_be_frame(ctx, irop_get_vreg(op), depth + 1))
      return 1;
  }
  return 0;
}

/* store to the local frame can't alias PARAM/VAR-pointer or non-stack-TEMP iloads; same-base still needs byte-range overlap */
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
        /* TEMP base aliases if it resolves to a stack location (union punning)
         * OR if it merely *derives* from one (runtime-indexed frame address). */
        if (ssa_opt_resolve_lea_stackloc(ctx, e->base_vr) != INT_MIN ||
            iload_base_may_be_frame(ctx, e->base_vr, 0))
          kill = 1;
      } else if (e_type == TCCIR_VREG_TYPE_VAR) {
        /* VAR base may hold &local; kill conservatively */
        kill = 1;
      }
      /* PARAM base = caller pointer; a fresh local store can't reach it, skip kill */
    }
    if (kill) {
      st->iloads[k] = st->iloads[--st->ilcount];
      k--;
    }
  }
}

/* a direct stack store may alias iloads whose VAR/stack-resolving-TEMP base points into the frame (ptr fuzz seed 380495) */
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
          /* placeholder store offset (named-local slot form): can't compare byte ranges, any stack base may alias */
          kill = 1;
        } else {
          int elo = base_off + (int)e->idx_imm * (1 << e->scale);
          int ehi = elo + slot_btype_bytes(e->btype);
          if (elo < store_hi && ehi > store_lo)
            kill = 1;
        }
      } else if (iload_base_may_be_frame(ctx, e->base_vr, 0)) {
        /* No exact offset, but the address still derives from the frame
         * (e.g. `&arr[0] + (i << 4)`): it may overlap this store. */
        kill = 1;
      }
    } else if (etype == TCCIR_VREG_TYPE_VAR) {
      /* VAR pointer may hold `&localarray[i]`; can't cheaply resolve its offset, invalidate conservatively */
      kill = 1;
    }
    if (kill) {
      st->iloads[k] = st->iloads[--st->ilcount];
      k--;
    }
  }
}

/* forward tracked `V <- value` into V reads in q's sources; mirrors var_tmp_fwd operand discipline, valid across the dom subtree */
static int vslot_forward_reads(IRSSAOptCtx *ctx, GLoadState *st, int i, IRQuadCompact *q)
{
  if (st->vscount == 0)
    return 0;
  TccIrOp op = q->op;
  if (op == TCCIR_OP_FUNCCALLVAL || op == TCCIR_OP_FUNCCALLVOID ||
      /* VLA_ALLOC clobbers its size operand's register; a forwarded live TEMP would be destroyed */
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

  /* iterative DFS (heap worklist) not recursion: deep branch nesting overflowed the 32 KB target stack */
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

  /* a non-dominator pred (back-edge/cross-edge) can modify tracked memory: drop ALL state incl available-LOAD caches */
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

    /* calls/BLOCK_COPY/INLINE_ASM/POSTINC touch unmodelled memory: kill all state */
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

    /* forward V reads before this op's dest handling so a `V <- V OP x` self-read still sees the prior value */
    if (var_fwd)
      changes += vslot_forward_reads(ctx, st, i, q);

    /* forward T_vreg-deref stores into non-STORE/LOAD ALU operands (drops the implicit LDR) */
    if (q->op != TCCIR_OP_STORE && q->op != TCCIR_OP_STORE_INDEXED &&
        q->op != TCCIR_OP_STORE_POSTINC &&
        q->op != TCCIR_OP_LOAD && q->op != TCCIR_OP_LOAD_INDEXED &&
        q->op != TCCIR_OP_LOAD_POSTINC &&
        /* VLA_ALLOC clobbers its size operand's register; a forwarded live vreg would be destroyed */
        q->op != TCCIR_OP_VLA_ALLOC) {
      int rewrites = 0;
      for (int side = 0; side < 2; side++) {
        IROperand op = side == 0 ? tcc_ir_op_get_src1(ir, q) : tcc_ir_op_get_src2(ir, q);
        /* global-deref ALU operand (bitfield RMW fused form): forward a tracked store/load */
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
        /* direct StackLoc operand: forward a tracked stack store; skip FUNCPARAMVAL aggregates; INT32 only (no hi/lo split or extension) */
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
              /* src2 may carry a fused barrel shift (dest = src1 OP (src2
               * SHIFT #n)); substituting an immediate would fold the
               * UN-shifted value (same bail as const_prop_tmp). */
              if (side == 1 && irop_is_immediate(new_op) &&
                  tcc_ir_barrel_shift_at(ir, q))
                continue;
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
          /* deref resolving to a tracked stack/&VAR slot imm: forward it (flat addrof_var_fwd shape); imm only — a forwarded TEMP extends live ranges into spills */
          if (!have_new && !ctx->no_stack_fwd) {
            int32_t lcse_base = -1;
            int lcse_off = ssa_opt_resolve_lea_stackloc_ex(ctx, pvr, &lcse_base);
            if (lcse_off != INT_MIN) {
              int sk = sstore_find(st, lcse_off);
              if (sk >= 0 && st->sstores[sk].btype == op_btype &&
                  st->sstores[sk].base_var == lcse_base &&
                  st->sstores[sk].stored_vr < 0) {
                new_op = st->sstores[sk].stored_imm;
                have_new = 1;
              }
            }
          }
        }
        /* no forwardable store: reuse an earlier canonical (base, offset) load; skip ASSIGN (main deref-CSE path retracks its dest) */
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
        /* Same barrel-shift bail as above: an immediate in an annotated src2
         * would be folded/encoded un-shifted. */
        if (side == 1 && irop_is_immediate(new_op) &&
            tcc_ir_barrel_shift_at(ir, q))
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

      /* kill aliasing iload entries before the store handlers nuke all state; STACKOFF/VAR/local dests don't alias global/heap */
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
          int store_to_stack = 0;

          if (dest.tag == IROP_TAG_VREG && store_base_vr >= 0 &&
              TCCIR_DECODE_VREG_TYPE(store_base_vr) == TCCIR_VREG_TYPE_TEMP) {
            if (q->op == TCCIR_OP_STORE && dest.is_lval) {
              can_check = 1;
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
            /* base TEMP resolving to a local stack address can't alias PARAM/VAR-pointer or non-stack-TEMP loads */
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

      /* forward a tracked stack store into a stack-loadable src; apply before tracking (which may invalidate the offset) */
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
                /* drop the pointer's use record: a stale entry corrupts its use-list (ptr fuzz seed 7226) */
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

      /* track StackLoc stores; record slot width so narrower subfield loads don't reuse wider values */
      if (dest.tag == IROP_TAG_STACKOFF) {
        /* a real stack write may alias any tvstore pointer (tracked only when not stack-resolved); drop them (ptr fuzz seed 8507) */
        if (dest.is_lval || q->op == TCCIR_OP_STORE_INDEXED)
          st->tvcount = 0;
        /* a direct stack write may alias frame-pointing iloads skipped above; runs before the no_stack_fwd gate (correctness) */
        if (st->ilcount > 0 && dest.is_lval && dest.is_local &&
            q->op == TCCIR_OP_STORE) {
          /* real slot: vreg -1 (offset is identity); named local: VAR vreg + placeholder offset */
          int off_exact = (irop_get_vreg(dest) < 0);
          iload_kill_for_direct_stack_store(ctx, st, irop_get_stack_offset(dest),
                                            slot_btype_bytes(irop_get_btype(dest)),
                                            off_exact);
        }
        IROperand src = tcc_ir_op_get_src1(ir, q);
        int32_t svr = irop_get_vreg(src);
        /* direct stack stores are StackLoc lvalues; non-lvalue STACKOFF operands are addresses, not writes */
        if (!ctx->no_stack_fwd && dest.is_local && dest.is_lval && !dest.is_llocal) {
          int store_btype = irop_get_btype(dest);
          /* dest vreg -1 = real slot (offset is identity); else VAR vreg = named local (placeholder offset) */
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
          /* indexed stack write: const index invalidates the exact slot, runtime index drops all stack/iload forwarding (fuzz seed 2657) */
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
          int off = irop_get_stack_offset(dest);
          int k = sstore_find(st, off);
          if (k >= 0)
            st->sstores[k] = st->sstores[--st->scount];
        }
        continue;
      }

      /* TEMP-DEREF stack store `*T = val` resolving to LEA(StackLoc[N]): STORE wraps dest in is_lval, STORE_INDEXED carries base as non-lval pointer */
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
        /* unresolved TEMP-DEREF store: track by TEMP vreg (SSA single-def); STORE + word-aligned only; invalidate other state, keep this entry */
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
              /* unknown pointer may alias any tracked address: drop other tvstores, global CSE/stores, and stack stores */
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
        /* unresolved indirect TEMP-DEREF store: fall through to the kill-all logic below */
      }

      if (dest.is_local) {
        continue;
      }

      /* direct VAR/TEMP non-lval STORE is a value copy, not a memory write: forget dest-keyed tracking, keep the rest; gated to STORE so indexed/postinc real writes fall through to kill-all */
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
            if (dtype == TCCIR_VREG_TYPE_VAR) {
              sstore_remove_base_var(st, dvr);
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
        /* an unresolved tvstore pointer may name this very global; a direct sym store must drop them */
        st->tvcount = 0;
        IRPoolSymref *sref = irop_get_symref_ex(ir, dest);
        if (sref && sref->sym) {
          for (int k = 0; k < st->count; k++) {
            if (st->entries[k].sym == sref->sym) {
              st->entries[k] = st->entries[--st->count];
              k--;
            }
          }
          /* track store for same-address LOAD forwarding; STORE only; skip sub-word (LDRB/LDRH re-extends the truncation), see pr78477 */
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
              /* stored vreg defined by `ASSIGN #imm`: track the imm (folds downstream; cprop/fold refuse cross-block vreg resolution) */
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
              gstore_invalidate_overlap(st, sref->sym, sref->addend, store_btype);
            }
          } else if (q->op == TCCIR_OP_STORE && !width_safe) {
            /* sub-word store: don't forward; invalidate a stale wider entry */
            gstore_invalidate_overlap(st, sref->sym, sref->addend, store_btype_chk);
          } else if (q->op == TCCIR_OP_STORE_INDEXED) {
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
      /* non-STORE write through a TEMP pointer `*T <- ...`: pointee unknown, kill all */
      if (qdest.is_lval && !qdest.is_local && !qdest.is_sym &&
          qdest.tag == IROP_TAG_VREG) {
        int32_t qpvr = irop_get_vreg(qdest);
        if (qpvr >= 0 && TCCIR_DECODE_VREG_TYPE(qpvr) == TCCIR_VREG_TYPE_TEMP) {
          st->count = 0;
          st->scount = 0;
          st->gscount = 0;
          st->tvcount = 0;
          st->ilcount = 0;
          /* vslot survives: a non-address-taken local can't be the pointee of an arbitrary `*T` write */
          continue;
        }
      }
      /* slot-form ASSIGN imm write: same memory semantics as the STORE-branch StackLoc tracker (addrof_var_fwd seed shape); imm only — a forwarded TEMP extends live ranges into spills */
      if (qdest.tag == IROP_TAG_STACKOFF && qdest.is_local) {
        int tracked_slot = 0;
        if (!ctx->no_stack_fwd && q->op == TCCIR_OP_ASSIGN && qdest.is_lval &&
            !qdest.is_llocal) {
          IROperand asrc = tcc_ir_op_get_src1(ir, q);
          if (irop_is_immediate(asrc) && !asrc.is_lval) {
            sstore_track_imm(st, irop_get_stack_offset(qdest), irop_get_btype(qdest),
                             asrc, irop_get_vreg(qdest));
            tracked_slot = 1;
          }
        }
        if (!tracked_slot)
          sstore_remove_offset(st, irop_get_stack_offset(qdest));
      }
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
        /* `V <- value` ASSIGN forwards to dominated reads; any other write to V just kills the entry */
        if (TCCIR_DECODE_VREG_TYPE(qdvr) == TCCIR_VREG_TYPE_VAR) {
          if (var_fwd && q->op == TCCIR_OP_ASSIGN && qdest.tag == IROP_TAG_VREG &&
              !qdest.is_lval && vslot_var_forwardable(ir, qdvr))
            vslot_track_store(ctx, st, qdvr, q);
          else
            vslot_remove_var(st, qdvr);
        }
      }
    }

    /* LOAD_INDEXED CSE: dedupe `T_dest = *(T_base + (#idx << #scale))` for const index/scale */
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
        /* zero-hop base read: any TEMP/VAR base is version-safe; PARAM needs zero textual defs (LcseDefCounts) */
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

      /* forward a tracked stack store into a LOAD_INDEXED whose base resolves to LEA(StackLoc[N]) (scale-0 byte-offset form; helper enforces scale==0, const idx) */
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
    /* ASSIGN with a pointer-deref source is a load in disguise: route through the LOAD paths */
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

    /* forward LOAD `T_vreg_DEREF` from a recent STORE through the same T_vreg */
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

    /* TEMP-DEREF LOAD CSE via canonical (base_vr, offset): reuse the iload tracker; btype in the key avoids 32/64-bit collision */
    if (src1.is_lval && !src1.is_sym && !src1.is_local && !src1.is_llocal &&
        src1.tag == IROP_TAG_VREG) {
      int32_t ptr_vr = irop_get_vreg(src1);
      if (ptr_vr >= 0) {
        int32_t canon_base = -1, canon_off = 0;
        /* unresolvable pointer def: key by the raw vreg (zero-hop, per-def invalidation keeps it sound) */
        if (!ssa_opt_resolve_temp_to_base_off(ctx, ptr_vr, &canon_base, &canon_off) ||
            canon_base < 0) {
          canon_base = ptr_vr;
          canon_off = 0;
        }
        /* base stability: TEMP single-def; VAR/PARAM need <=1 textual def; zero-hop form reads the current value */
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

      /* direct StackLoc load; skip a VAR-vreg operand whose offset may alias an unrelated StackLoc */
      if (src1.tag == IROP_TAG_STACKOFF) {
        int32_t svr = irop_get_vreg(src1);
        if (svr < 0 || TCCIR_DECODE_VREG_TYPE(svr) != TCCIR_VREG_TYPE_VAR)
          stack_off = irop_get_stack_offset(src1);
      }


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
          /* `&VAR` offset is a shared placeholder: the canonical base must match (ptr fuzz seed 67) */
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
          /* drop this LOAD's use of its old base pointer: a stale use entry corrupts the base's use-list (95_bitfields TEST2 PACKED RMW, -O1) */
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

    if (!src1.is_sym || !src1.is_lval)
      continue;

    IRPoolSymref *ref = irop_get_symref_ex(ir, src1);
    if (!ref || !ref->sym)
      continue;
    if (ref->sym->type.t & VT_VOLATILE)
      continue;

    /* prefer a tracked store over an earlier load: eliminates the LOAD and an immediate folds further */
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

  /* walk dom children from this block's END state; st passed by pointer + single-child iterated (not recursed) to stay off the 32 KB target stack */
  if (bb->num_dom_children == 1) {
    b = bb->dom_children[0];
    continue;
  }
  if (bb->num_dom_children == 0)
    break;
  /* multi-way: child[0] on the live state, other children pushed with their own heap snapshots (sibling subtrees are independent) */
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
  }

    tcc_free(st);
  }

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
