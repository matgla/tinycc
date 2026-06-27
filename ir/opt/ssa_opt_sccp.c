/*
 *  TCC IR - Sparse Conditional Constant Propagation (SCCP)
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
#include "licm.h"
#include <limits.h>

/* ============================================================================
 * SCCP: Sparse Conditional Constant Propagation
 *
 * Combines constant propagation with unreachable code elimination using
 * two lattices and two worklists:
 *
 * Value lattice per SSA variable:  TOP -> CONST(c) -> BOTTOM
 *   TOP    = not yet determined (optimistic assumption)
 *   CONST  = known constant value
 *   BOTTOM = varying (multiple possible values)
 *
 * Edge executability: each CFG edge (pred->succ) is executable or not.
 * A block is reachable when any incoming edge is executable.
 *
 * The algorithm processes two worklists until both are empty:
 *   CFG worklist: edges to mark executable (propagate reachability)
 *   SSA worklist: vregs whose lattice value changed (re-evaluate uses)
 *
 * Key advantage over iterative cprop+fold+branch: SCCP never evaluates
 * code in unreachable blocks, so dead paths can't pessimize the lattice.
 * ============================================================================ */

enum { SCCP_TOP = 0, SCCP_CONST = 1, SCCP_BOTTOM = 2 };

typedef struct {
  uint8_t state;
  int64_t value;
} SCCPCell;

/* Memory dependency: when a STORE→LOAD chain resolves using a TEMP's
 * lattice value, the LOAD destination must be re-evaluated if the source
 * TEMP later changes (TOP→CONST→BOTTOM).  The SSA worklist only tracks
 * direct vreg uses, not STORE→LOAD memory chains. */
typedef struct {
  int src_pos;   /* TEMP position of store source */
  int load_idx;  /* instruction index of the LOAD */
} SCCPMemDep;

typedef struct {
  IRSSAOptCtx *ctx;
  SCCPCell *cells;       /* indexed by TEMP vreg position */
  int cells_cap;
  uint8_t *block_reachable; /* 1 if any incoming edge is executable */
  uint8_t *edge_exec;       /* flattened [pred * num_blocks + succ] */
  int num_blocks;
  /* CFG worklist: edges to process */
  int *cfg_wl;
  int cfg_wl_count;
  int cfg_wl_cap;
  /* SSA worklist: vreg positions to re-evaluate */
  int *ssa_wl;
  int ssa_wl_count;
  int ssa_wl_cap;
  /* Memory dependencies: STORE source → LOAD instruction */
  SCCPMemDep *mem_deps;
  int mem_dep_count;
  int mem_dep_cap;
  /* Loop info (lazily computed) for back-edge-aware stack-load resolution. */
  IRLoops *loops;
  int loops_done;
} SCCPState;

static SCCPCell *sccp_cell(SCCPState *s, int32_t vreg)
{
  if (vreg < 0 || TCCIR_DECODE_VREG_TYPE(vreg) != TCCIR_VREG_TYPE_TEMP)
    return NULL;
  int pos = TCCIR_DECODE_VREG_POSITION(vreg);
  if (pos >= s->cells_cap)
    return NULL;
  return &s->cells[pos];
}

static int sccp_meet(SCCPCell *cell, int64_t value)
{
  if (cell->state == SCCP_TOP) {
    cell->state = SCCP_CONST;
    cell->value = value;
    return 1;
  }
  if (cell->state == SCCP_CONST && cell->value == value)
    return 0;
  cell->state = SCCP_BOTTOM;
  return 1;
}

static int sccp_set_bottom(SCCPCell *cell)
{
  if (cell->state == SCCP_BOTTOM)
    return 0;
  cell->state = SCCP_BOTTOM;
  return 1;
}

static void sccp_add_cfg_edge(SCCPState *s, int pred, int succ)
{
  if (pred < 0 || succ < 0 || pred >= s->num_blocks || succ >= s->num_blocks)
    return;
  int idx = pred * s->num_blocks + succ;
  if (s->edge_exec[idx])
    return;
  s->edge_exec[idx] = 1;
  if (s->cfg_wl_count >= s->cfg_wl_cap) {
    int nc = s->cfg_wl_cap ? s->cfg_wl_cap * 2 : 64;
    s->cfg_wl = tcc_realloc(s->cfg_wl, nc * sizeof(int));
    s->cfg_wl_cap = nc;
  }
  s->cfg_wl[s->cfg_wl_count++] = idx;
}

static void sccp_add_ssa(SCCPState *s, int pos)
{
  if (s->ssa_wl_count >= s->ssa_wl_cap) {
    int nc = s->ssa_wl_cap ? s->ssa_wl_cap * 2 : 64;
    s->ssa_wl = tcc_realloc(s->ssa_wl, nc * sizeof(int));
    s->ssa_wl_cap = nc;
  }
  s->ssa_wl[s->ssa_wl_count++] = pos;
}

static void sccp_add_mem_dep(SCCPState *s, int src_pos, int load_idx)
{
  for (int i = 0; i < s->mem_dep_count; i++) {
    if (s->mem_deps[i].src_pos == src_pos && s->mem_deps[i].load_idx == load_idx)
      return;
  }
  if (s->mem_dep_count >= s->mem_dep_cap) {
    int nc = s->mem_dep_cap ? s->mem_dep_cap * 2 : 16;
    s->mem_deps = tcc_realloc(s->mem_deps, nc * sizeof(SCCPMemDep));
    s->mem_dep_cap = nc;
  }
  s->mem_deps[s->mem_dep_count++] = (SCCPMemDep){ src_pos, load_idx };
}

static int sccp_get_operand_value(SCCPState *s, IROperand op, int64_t *out)
{
  if (irop_is_immediate(op)) {
    *out = irop_get_imm64_ex(s->ctx->ir, op);
    return SCCP_CONST;
  }
  int32_t vr = irop_get_vreg(op);
  SCCPCell *c = sccp_cell(s, vr);
  if (!c)
    return SCCP_BOTTOM;
  if (c->state == SCCP_CONST)
    *out = c->value;
  return c->state;
}

/* Forward decl: defined below. */
static int sccp_resolve_stack_load(SCCPState *s, int soff, int load_btype,
                                   int instr_idx, int64_t *out, int *dep_pos);

static int sccp_get_store_src_value(SCCPState *s, IROperand src, int64_t *out,
                                    int *src_pos_out)
{
  if (src_pos_out) *src_pos_out = -1;

  if (irop_is_immediate(src)) {
    *out = irop_get_imm64_ex(s->ctx->ir, src);
    return SCCP_CONST;
  }

  int32_t src_vr = irop_get_vreg(src);
  if (src_vr < 0)
    return SCCP_BOTTOM;
  SCCPCell *src_cell = sccp_cell(s, src_vr);
  if (src_cell && src_cell->state == SCCP_CONST) {
    *out = src_cell->value;
    if (src_pos_out) *src_pos_out = TCCIR_DECODE_VREG_POSITION(src_vr);
    return SCCP_CONST;
  }
  return SCCP_BOTTOM;
}

/* Variant that also resolves a STACKOFF-lvalue source via stack-store
 * scanning.  Needs the instruction index for backward dominator-tree walk. */
static int sccp_get_store_src_value_ex(SCCPState *s, IROperand src,
                                       int instr_idx, int64_t *out,
                                       int *src_pos_out)
{
  int st = sccp_get_store_src_value(s, src, out, src_pos_out);
  if (st != SCCP_BOTTOM)
    return st;

  int load_off = INT_MIN;
  /* Direct StackLoc lvalue: V <-- StackLoc[N] [STORE]. */
  if (src.tag == IROP_TAG_STACKOFF && src.is_lval && src.is_local && !src.is_llocal) {
    int32_t svr = irop_get_vreg(src);
    if (svr < 0 || TCCIR_DECODE_VREG_TYPE(svr) != TCCIR_VREG_TYPE_VAR)
      load_off = irop_get_stack_offset(src);
  }
  /* TEMP-DEREF lvalue: V <-- *T [STORE] where T resolves to &StackLoc[N]. */
  if (load_off == INT_MIN && src.tag == IROP_TAG_VREG && src.is_lval &&
      !src.is_local) {
    int32_t svr = irop_get_vreg(src);
    if (svr >= 0 && TCCIR_DECODE_VREG_TYPE(svr) == TCCIR_VREG_TYPE_TEMP)
      load_off = ssa_opt_resolve_lea_stackloc(s->ctx, svr);
  }

  if (load_off != INT_MIN) {
    int dep_pos = -1;
    int st2 = sccp_resolve_stack_load(s, load_off, irop_get_btype(src),
                                       instr_idx, out, &dep_pos);
    if (st2 == SCCP_CONST) {
      if (src_pos_out && dep_pos >= 0)
        *src_pos_out = dep_pos;
      return SCCP_CONST;
    }
  }
  return SCCP_BOTTOM;
}

/* Conservative byte size for an IROP_BTYPE.  Treats unknown/struct as 8
 * so unrelated stores can't be proven not to alias them. */
static int sccp_btype_bytes(int btype)
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

/* Try to identify the stack offset that a STORE-class instruction targets,
 * accounting for both direct StackLoc dests and TEMP-DEREF dests that
 * resolve back to LEA(StackLoc[N]).  Returns INT_MIN when the dest is
 * something else (global, escaping pointer, unresolved LEA, etc.). */
static int sccp_store_target_off(IRSSAOptCtx *ctx, IRQuadCompact *sq,
                                 int *out_btype)
{
  TCCIRState *ir = ctx->ir;
  IROperand sd = tcc_ir_op_get_dest(ir, sq);
  if (sq->op == TCCIR_OP_STORE) {
    if (sd.tag == IROP_TAG_STACKOFF && sd.is_lval && sd.is_local) {
      if (out_btype) *out_btype = irop_get_btype(sd);
      return irop_get_stack_offset(sd);
    }
    int off = ssa_opt_indirect_stack_offset(ctx, sq, SSA_OPT_INDIRECT_DEST);
    if (off != INT_MIN && out_btype)
      *out_btype = irop_get_btype(sd);
    return off;
  }
  if (sq->op == TCCIR_OP_STORE_INDEXED) {
    int off = ssa_opt_indirect_stack_offset(ctx, sq, SSA_OPT_INDIRECT_DEST);
    if (off != INT_MIN && out_btype)
      *out_btype = irop_get_btype(sd);
    return off;
  }
  return INT_MIN;
}

/* Could store sq potentially alias the global / unknown-pointer load
 * we're trying to resolve?  Returns 1 when we can't prove non-aliasing. */
static int sccp_store_may_escape(IRSSAOptCtx *ctx, IRQuadCompact *sq)
{
  TCCIRState *ir = ctx->ir;
  IROperand sd = tcc_ir_op_get_dest(ir, sq);
  /* Direct stack stores never alias unrelated stack slots; checked by
   * caller against soff. */
  if (sd.tag == IROP_TAG_STACKOFF && sd.is_lval && sd.is_local)
    return 0;
  /* TEMP-DEREF stores that resolve to a known stack slot likewise can
   * be reasoned about by offset.  Caller compares offsets. */
  if (sd.tag == IROP_TAG_VREG && sd.is_lval && !sd.is_local) {
    int off = ssa_opt_indirect_stack_offset(ctx, sq, SSA_OPT_INDIRECT_DEST);
    if (off != INT_MIN)
      return 0;
  }
  /* VAR stores: writing into a named local slot — separate from the stack
   * load we're tracking unless its address escaped (we conservatively bail
   * in those cases below). */
  if (sd.is_local && !sd.is_lval)
    return 0;
  return 1;
}

/* Scan one block backward looking for a stack store at offset `soff` that
 * matches load_btype.  Returns SCCP_CONST with *out set, SCCP_BOTTOM if a
 * potentially-aliasing store was hit before finding a match, or SCCP_TOP
 * if the block was scanned to its start with no aliasing/matching store. */
static int sccp_scan_block_for_stack_store(SCCPState *s, IRBasicBlock *bb,
                                           int start_idx, int soff,
                                           int load_btype, int64_t *out,
                                           int *dep_pos)
{
  TCCIRState *ir = s->ctx->ir;
  int load_size = sccp_btype_bytes(load_btype);
  int load_lo = soff;
  int load_hi = soff + load_size;
  for (int si = start_idx; si >= bb->start_idx; si--) {
    IRQuadCompact *sq = &ir->compact_instructions[si];
    if (sq->op == TCCIR_OP_NOP)
      continue;
    if (sq->op == TCCIR_OP_FUNCCALLVOID || sq->op == TCCIR_OP_FUNCCALLVAL)
      return SCCP_BOTTOM;
    if (sq->op == TCCIR_OP_STORE_POSTINC)
      return SCCP_BOTTOM;  /* writes to memory + updates pointer */
    if (sq->op == TCCIR_OP_STORE_INDEXED || sq->op == TCCIR_OP_STORE) {
      int store_btype = 0;
      int target = sccp_store_target_off(s->ctx, sq, &store_btype);
      if (target == INT_MIN) {
        if (sq->op == TCCIR_OP_STORE_INDEXED)
          return SCCP_BOTTOM;
        if (sccp_store_may_escape(s->ctx, sq))
          return SCCP_BOTTOM;
        continue;
      }
      /* Exact match: forward the stored value. */
      if (target == soff && store_btype == load_btype) {
        int st2 = sccp_get_store_src_value(s, tcc_ir_op_get_src1(ir, sq),
                                            out, dep_pos);
        if (st2 == SCCP_CONST)
          return SCCP_CONST;
        return SCCP_BOTTOM;
      }
      /* Aliasing check: bail if byte ranges overlap. */
      int store_size = sccp_btype_bytes(store_btype);
      int store_lo = target;
      int store_hi = target + store_size;
      if (store_hi > load_lo && load_hi > store_lo)
        return SCCP_BOTTOM;
      /* Disjoint stack ranges; keep scanning. */
      continue;
    }
  }
  return SCCP_TOP;
}

/* Resolve a STORE_INDEXED's base operand to a stack offset (the offset of
 * the array's first element), even when the index isn't an immediate.
 * Returns INT_MIN when the base doesn't LEA-resolve to a local stack
 * address.  Used by sccp_no_aliasing_between to bound the byte range that
 * an indexed write might touch — only writes whose base is the same array
 * (or whose array overlaps our load's offset) need to invalidate. */
static int sccp_store_indexed_base_off(IRSSAOptCtx *ctx, IRQuadCompact *q)
{
  if (q->op != TCCIR_OP_STORE_INDEXED && q->op != TCCIR_OP_STORE_POSTINC)
    return INT_MIN;
  TCCIRState *ir = ctx->ir;
  IROperand base = tcc_ir_op_get_dest(ir, q);
  if (base.tag != IROP_TAG_VREG || base.is_local)
    return INT_MIN;
  int32_t bvr = irop_get_vreg(base);
  if (bvr < 0 || TCCIR_DECODE_VREG_TYPE(bvr) != TCCIR_VREG_TYPE_TEMP)
    return INT_MIN;
  return ssa_opt_resolve_lea_stackloc(ctx, bvr);
}

/* Scan the linear IR range (store_idx, load_idx) for any potentially-aliasing
 * memory write that the dominator-tree walk in sccp_resolve_stack_load might
 * otherwise skip.  Returns 1 if the load can be safely forwarded from
 * `store_idx`, 0 if a possible aliasing write is found.
 *
 * Safe to scan the raw IR range because we only need to disprove aliasing:
 * any code path that flows from store to load is a subset of the IR range
 * [store_idx+1 .. load_idx-1], so checking that range is conservative. */
static int sccp_no_aliasing_between(SCCPState *s, int store_idx, int load_idx,
                                    int soff, int load_btype)
{
  TCCIRState *ir = s->ctx->ir;
  int load_size = sccp_btype_bytes(load_btype);
  int load_lo = soff;
  int load_hi = soff + load_size;
  /* Maximum stack-array size to assume when an indexed write resolves to a
   * known base but unresolved index.  Real arrays declared on a small
   * function's stack rarely exceed this; sized so a write to base [A] with
   * unknown index might touch [A, A+1024).  Conservative — too small and
   * we lose alias info on big arrays; too large and we bail unnecessarily
   * on small arrays that don't reach the load's offset. */
  const int LCS_INDEXED_MAX_ARRAY = 64;
  for (int i = store_idx + 1; i < load_idx; i++) {
    IRQuadCompact *q = &ir->compact_instructions[i];
    TccIrOp op = q->op;
    if (op == TCCIR_OP_NOP)
      continue;
    if (op == TCCIR_OP_FUNCCALLVOID || op == TCCIR_OP_FUNCCALLVAL)
      return 0;
    if (op == TCCIR_OP_BLOCK_COPY)
      return 0;
    if (op != TCCIR_OP_STORE && op != TCCIR_OP_STORE_INDEXED &&
        op != TCCIR_OP_STORE_POSTINC)
      continue;
    int store_btype = 0;
    int target = sccp_store_target_off(s->ctx, q, &store_btype);
    if (target != INT_MIN) {
      /* Fully resolved write: bail only on actual byte-range overlap. */
      int store_size = sccp_btype_bytes(store_btype);
      int store_lo = target;
      int store_hi = target + store_size;
      if (store_hi > load_lo && load_hi > store_lo)
        return 0;
      continue;
    }
    /* Unresolved offset.  For STORE_INDEXED / STORE_POSTINC try to recover
     * the base LEA — if the base resolves to a stack array whose plausible
     * extent doesn't overlap our load, treat as non-aliasing. */
    int base_off = sccp_store_indexed_base_off(s->ctx, q);
    if (base_off != INT_MIN) {
      int extent_lo = base_off;
      int extent_hi = base_off + LCS_INDEXED_MAX_ARRAY;
      if (extent_hi <= load_lo || extent_lo >= load_hi)
        continue; /* base array is far from our load — no aliasing */
      return 0;
    }
    /* Truly unknown memory write — could touch any stack slot. */
    return 0;
  }
  return 1;
}

/* Recover the base stack offset of an indexed/postinc store's destination
 * array, whether the base address is a direct STACKOFF operand
 * (Addr[StackLoc[off]], emitted for `arr[i] = v` where `arr` is a local array)
 * or a TEMP that LEA-resolves to a stack slot.  Returns INT_MIN when the base
 * cannot be pinned to a local stack address.  Unlike
 * sccp_store_indexed_base_off() this also accepts the direct-STACKOFF base
 * (vreg == -1) so the entry-block alias check below can bound an indexed
 * write whose index is not a known constant. */
static int sccp_indexed_store_base_off(IRSSAOptCtx *ctx, IRQuadCompact *q)
{
  if (q->op != TCCIR_OP_STORE_INDEXED && q->op != TCCIR_OP_STORE_POSTINC)
    return INT_MIN;
  TCCIRState *ir = ctx->ir;
  IROperand base = tcc_ir_op_get_dest(ir, q);
  if (base.tag == IROP_TAG_STACKOFF && base.is_local && irop_get_vreg(base) == -1)
    return irop_get_stack_offset(base);
  if (base.tag == IROP_TAG_VREG && !base.is_local) {
    int32_t bvr = irop_get_vreg(base);
    if (bvr >= 0 && TCCIR_DECODE_VREG_TYPE(bvr) == TCCIR_VREG_TYPE_TEMP)
      return ssa_opt_resolve_lea_stackloc(ctx, bvr);
  }
  return INT_MIN;
}

/* Entry-block initializers are usually allowed to forward broadly, but a later
 * write whose stack byte range resolves exactly still clobbers that value.
 * Keep this narrower than sccp_no_aliasing_between(): do not treat calls or
 * unresolved pointer stores as barriers here, preserving the older permissive
 * behavior for common aggregate-init shapes. */
static int sccp_resolved_stack_write_between(SCCPState *s, int store_idx, int load_idx,
                                             int soff, int load_btype)
{
  TCCIRState *ir = s->ctx->ir;
  int load_size = sccp_btype_bytes(load_btype);
  int load_lo = soff;
  int load_hi = soff + load_size;
  for (int i = store_idx + 1; i < load_idx; i++) {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_STORE && q->op != TCCIR_OP_STORE_INDEXED &&
        q->op != TCCIR_OP_STORE_POSTINC)
      continue;
    int store_btype = 0;
    int target = sccp_store_target_off(s->ctx, q, &store_btype);
    if (target == INT_MIN) {
      /* Unresolved concrete offset.  A STORE_INDEXED / STORE_POSTINC into a
       * stack array still clobbers our load when the array's extent covers the
       * load slot, even though the index is not a known constant during this
       * scan.  The entry-block exemption must NOT skip such a write: seed 3691
       * had a conditional `arr[i] = v` whose index was still a TEMP at SCCP
       * time, so sccp_store_target_off() returned INT_MIN and the array-init
       * LOAD wrongly folded back to the initializer.  Mirror the indexed-base
       * extent check sccp_no_aliasing_between() applies for the non-entry path. */
      if (q->op == TCCIR_OP_STORE_INDEXED || q->op == TCCIR_OP_STORE_POSTINC) {
        const int LCS_INDEXED_MAX_ARRAY = 64;
        int base_off = sccp_indexed_store_base_off(s->ctx, q);
        if (base_off == INT_MIN)
          return 1; /* indexed write to an unknown base — may alias the load */
        int extent_lo = base_off;
        int extent_hi = base_off + LCS_INDEXED_MAX_ARRAY;
        if (extent_hi > load_lo && load_hi > extent_lo)
          return 1; /* the array's plausible extent covers the load slot */
      }
      continue;
    }
    int store_lo = target;
    int store_hi = target + sccp_btype_bytes(store_btype);
    if (store_hi > load_lo && load_hi > store_lo)
      return 1;
  }
  return 0;
}

/* Back-edge-aware clobber check.  sccp_no_aliasing_between only scans the
 * linear IR range between a dominating store and the load, on the assumption
 * that every path from store to load lies within that range.  That assumption
 * breaks for a load inside a loop: the loop body (which sits AFTER the load in
 * IR order) reaches the load again via the back-edge, so a store there
 * clobbers the value on the second and later iterations.  Returns 1 if the
 * load at `load_idx` is inside a loop whose body writes the slot — meaning the
 * loaded value is loop-carried and must not be treated as a constant.
 *
 * Fixes 931102-1/-2: `while ((reg.b.l & 1) == 0) reg.b.l >>= 1;` — SCCP
 * resolved the header load of reg.b.l to the preheader's `= 2` store, folded
 * the exit test to "always false", and the loop spun forever. */
static int sccp_loop_clobbers_slot(SCCPState *s, int load_idx, int soff, int load_btype)
{
  if (!s->loops_done) {
    s->loops = tcc_ir_detect_loops(s->ctx->ir);
    s->loops_done = 1;
  }
  if (!s->loops || s->loops->num_loops == 0)
    return 0;
  int load_size = sccp_btype_bytes(load_btype);
  int load_lo = soff, load_hi = soff + load_size;
  TCCIRState *ir = s->ctx->ir;
  for (int li = 0; li < s->loops->num_loops; li++) {
    IRLoop *loop = &s->loops->loops[li];
    if (load_idx < loop->start_idx || load_idx > loop->end_idx)
      continue;
    /* Load is in this loop — scan its body range for any write to the slot. */
    for (int i = loop->start_idx; i <= loop->end_idx; i++) {
      IRQuadCompact *q = &ir->compact_instructions[i];
      TccIrOp op = q->op;
      if (op == TCCIR_OP_NOP)
        continue;
      if (op == TCCIR_OP_FUNCCALLVOID || op == TCCIR_OP_FUNCCALLVAL || op == TCCIR_OP_BLOCK_COPY)
        return 1; /* may write anything */
      if (op != TCCIR_OP_STORE && op != TCCIR_OP_STORE_INDEXED && op != TCCIR_OP_STORE_POSTINC)
        continue;
      int store_btype = 0;
      int target = sccp_store_target_off(s->ctx, q, &store_btype);
      if (target != INT_MIN) {
        int store_lo = target, store_hi = target + sccp_btype_bytes(store_btype);
        if (store_hi > load_lo && load_hi > store_lo)
          return 1; /* byte ranges overlap */
        continue;
      }
      /* Unresolved store offset in the loop — conservatively assume it may
       * touch the slot. */
      return 1;
    }
  }
  return 0;
}

/* Companion to sccp_loop_clobbers_slot for a store->load FORWARD across the
 * dominator tree: returns 1 if a loop lying strictly BETWEEN the (dominating)
 * store at `from_idx` and the load at `to_idx` writes the slot.  The dominator
 * walk can forward an entry-block init store to a post-loop load while the
 * intervening linear alias scan is skipped (entry-block exemption), but the
 * loop body's store clobbers the value on every iteration — the post-loop load
 * is loop-carried, not the init constant.  Fixes the -O1 miscompile of
 * `struct{int x;}a; a.x=0; for(i=1;i<=k;i++) a.x+=i; return a.x;` (returned 0). */
static int sccp_loop_writes_slot_between(SCCPState *s, int from_idx, int to_idx,
                                         int soff, int load_btype)
{
  if (!s->loops_done) {
    s->loops = tcc_ir_detect_loops(s->ctx->ir);
    s->loops_done = 1;
  }
  if (!s->loops || s->loops->num_loops == 0)
    return 0;
  int load_size = sccp_btype_bytes(load_btype);
  int load_lo = soff, load_hi = soff + load_size;
  TCCIRState *ir = s->ctx->ir;
  for (int li = 0; li < s->loops->num_loops; li++) {
    IRLoop *loop = &s->loops->loops[li];
    /* Only loops whose body runs on the path from the store to the load. */
    if (!(loop->start_idx > from_idx && loop->end_idx < to_idx))
      continue;
    for (int i = loop->start_idx; i <= loop->end_idx; i++) {
      IRQuadCompact *q = &ir->compact_instructions[i];
      TccIrOp op = q->op;
      if (op == TCCIR_OP_NOP)
        continue;
      /* A call passing the load slot's ADDRESS by-reference may write it on
       * every iteration (e.g. `for(..) g(&s); return s.f;`).  Bail when a loop
       * FUNCPARAM hands a callee a non-lval stack address pointing at the object
       * containing the load slot.  Params that pass a VALUE (`*p`, an lval
       * deref) do NOT escape the slot's address — that is the scal-to-vec
       * vector-lowering shape, which must keep forwarding.  SCCP_OBJ_BOUND caps
       * how far one object can extend past the passed base. */
      if (op == TCCIR_OP_FUNCPARAMVAL || op == TCCIR_OP_FUNCPARAMVOID) {
        IROperand p = tcc_ir_op_get_src1(ir, q);
        if (!p.is_lval) {
          int aoff = INT_MIN;
          if (irop_get_tag(p) == IROP_TAG_STACKOFF && p.is_local && irop_get_vreg(p) == -1)
            aoff = irop_get_stack_offset(p);
          else {
            int32_t pvr = irop_get_vreg(p);
            if (pvr >= 0)
              aoff = ssa_opt_resolve_lea_stackloc(s->ctx, pvr);
          }
          const int SCCP_OBJ_BOUND = 4096;
          if (aoff != INT_MIN && aoff <= load_lo && load_lo - aoff < SCCP_OBJ_BOUND)
            return 1; /* slot's address escapes to a callee that may write it */
        }
        continue;
      }
      /* A plain call (no by-ref slot arg) or block-copy that doesn't resolve to
       * the load slot is left to the resolved-store check below; blanket-bailing
       * here regressed scal-to-vec (element-loop copies that miss the load). */
      if (op == TCCIR_OP_FUNCCALLVOID || op == TCCIR_OP_FUNCCALLVAL || op == TCCIR_OP_BLOCK_COPY)
        continue;
      if (op != TCCIR_OP_STORE && op != TCCIR_OP_STORE_INDEXED && op != TCCIR_OP_STORE_POSTINC)
        continue;
      int store_btype = 0;
      int target = sccp_store_target_off(s->ctx, q, &store_btype);
      if (target != INT_MIN) {
        int store_lo = target, store_hi = target + sccp_btype_bytes(store_btype);
        if (store_hi > load_lo && load_hi > store_lo)
          return 1; /* resolved write to the load's slot — genuine clobber */
        continue;
      }
      /* Unresolved store offset.  Two very different shapes land here:
       *
       *  - STORE_INDEXED with a variable index (scal-to-vec-style vector
       *    lowering): the writes provably hit distinct element positions and
       *    miss the scalar load slot.  Conservatively clobbering here would
       *    defeat the legitimate entry-block forward for those patterns, so
       *    keep forwarding.  The linear alias scan (sccp_no_aliasing_between)
       *    handles unresolved writes precisely for non-entry-block stores.
       *
       *  - A plain pointer-deref STORE (`*p = ...`) whose pointer provenance
       *    we cannot pin to a stack offset.  This is exactly the inlined-call
       *    accumulate shape: `for(..) gs(&s);` where gs is inlined to
       *    `*p = *p + 2` and `p` flows through the inlined param V-register
       *    that ssa_opt_resolve_lea_stackloc won't chase.  The pointer may
       *    well alias the load slot's object, so we must NOT forward the
       *    entry store across this loop.  STORE_POSTINC is likewise an opaque
       *    memory write (scan_block_for_stack_store already treats it as a
       *    full barrier). */
      if (op == TCCIR_OP_STORE || op == TCCIR_OP_STORE_POSTINC)
        return 1; /* opaque pointer-deref write may alias the load slot */
      continue;
    }
  }
  return 0;
}

static int sccp_resolve_stack_load(SCCPState *s, int soff, int load_btype,
                                   int instr_idx, int64_t *out, int *dep_pos)
{
  IRCFG *cfg = s->ctx->cfg;
  int block = cfg->instr_to_block[instr_idx];
  IRBasicBlock *bb = &cfg->blocks[block];

  /* A load inside a loop whose body stores the slot is loop-carried — the
   * preheader store does not solely reach it on later iterations. */
  if (sccp_loop_clobbers_slot(s, instr_idx, soff, load_btype))
    return SCCP_BOTTOM;

  int st = sccp_scan_block_for_stack_store(s, bb, instr_idx - 1, soff,
                                            load_btype, out, dep_pos);
  if (st != SCCP_TOP)
    return st;

  /* Walk up dominator tree if not found in current block.  When a match is
   * found in a dominator block, also verify no aliasing memory write sits
   * between the matching STORE and our load — the dominator-tree walk skips
   * intervening sibling blocks (e.g. loop bodies between an entry-block
   * residual STORE and a post-loop LOAD), and those blocks may contain
   * STORE_INDEXED / STORE_POSTINC / unresolved memory writes that would
   * invalidate the value. */
  int dom = bb->idom;
  while (dom >= 0 && dom != block) {
    IRBasicBlock *db = &cfg->blocks[dom];
    int saved_dep = dep_pos ? *dep_pos : -1;
    int64_t saved_out = *out;
    int dst = sccp_scan_block_for_stack_store(s, db, db->end_idx - 1, soff,
                                               load_btype, out, dep_pos);
    if (dst == SCCP_CONST) {
      /* Find the store index inside `db` that matched, so we can check the
       * IR range between it and our load for aliasing.  sccp_scan_block_for_stack_store
       * doesn't expose this directly, so we re-scan the dominator block to
       * pinpoint the matching STORE's IR index. */
      int matched_idx = -1;
      for (int si = db->end_idx - 1; si >= db->start_idx; si--) {
        IRQuadCompact *sq = &s->ctx->ir->compact_instructions[si];
        if (sq->op != TCCIR_OP_STORE && sq->op != TCCIR_OP_STORE_INDEXED)
          continue;
        int sb = 0;
        int target = sccp_store_target_off(s->ctx, sq, &sb);
        if (target == soff && sb == load_btype) {
          matched_idx = si;
          break;
        }
      }
      /* Mid-function stores — including LCS's residual STOREs that replace a
       * folded loop's memory writes — need the full alias check, because
       * intervening blocks can contain unresolved pointer writes.  Entry-block
       * stores stay more permissive for aggregate-init patterns, but a later
       * STORE_INDEXED/direct STORE that resolves to the same concrete stack
       * bytes still invalidates the initializer. */
      /* A loop between the (dominating) store and the load whose body writes
       * the slot makes the loaded value loop-carried, not the stored constant.
       * The linear alias scan below is skipped for entry-block stores, so this
       * back-edge-aware check runs UNCONDITIONALLY — otherwise an entry-block
       * init store forwards across an intervening accumulate loop (miscompile). */
      if (matched_idx >= 0 &&
          sccp_loop_writes_slot_between(s, matched_idx, instr_idx, soff, load_btype)) {
        *out = saved_out;
        if (dep_pos) *dep_pos = saved_dep;
        return SCCP_BOTTOM;
      }
      int entry_block = (cfg->num_blocks > 0) ? 0 : -1;
      int store_block = cfg->instr_to_block[matched_idx];
      int aliases_between = 0;
      if (matched_idx >= 0) {
        if (store_block == entry_block)
          aliases_between = sccp_resolved_stack_write_between(s, matched_idx, instr_idx, soff, load_btype);
        else
          aliases_between = !sccp_no_aliasing_between(s, matched_idx, instr_idx, soff, load_btype);
      }
      if (aliases_between) {
        /* Aliasing write in between — restore state and treat as unknown. */
        *out = saved_out;
        if (dep_pos) *dep_pos = saved_dep;
        return SCCP_BOTTOM;
      }
      return SCCP_CONST;
    }
    if (dst != SCCP_TOP)
      return dst;
    if (dom == db->idom)
      break;
    dom = db->idom;
  }
  return SCCP_BOTTOM;
}

/* Resolve a VAR operand's value by scanning backward in the current block.
 * Handles direct ASSIGN and STORE-through-LEA patterns.
 * When the resolution uses a TEMP's lattice value through a STORE,
 * *dep_src_pos is set to that TEMP's position so the caller can record
 * a memory dependency for re-evaluation. */
static int sccp_resolve_var(SCCPState *s, int32_t var_vreg, int instr_idx,
                            int64_t *out, int *dep_src_pos)
{
  TCCIRState *ir = s->ctx->ir;
  IRCFG *cfg = s->ctx->cfg;
  int block = cfg->instr_to_block[instr_idx];
  IRBasicBlock *bb = &cfg->blocks[block];
  int var_pos = TCCIR_DECODE_VREG_POSITION(var_vreg);
  *dep_src_pos = -1;

  for (int i = instr_idx - 1; i >= bb->start_idx; i--) {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;

    if (q->op == TCCIR_OP_FUNCCALLVOID || q->op == TCCIR_OP_FUNCCALLVAL)
      return SCCP_BOTTOM;

    /* Check if this instruction writes to the VAR (any op with dest=V) */
    if (irop_config[q->op].has_dest && q->op != TCCIR_OP_STORE &&
        q->op != TCCIR_OP_STORE_INDEXED && q->op != TCCIR_OP_STORE_POSTINC) {
      IROperand dest = tcc_ir_op_get_dest(ir, q);
      int32_t dv = irop_get_vreg(dest);
      if (dv >= 0 && TCCIR_DECODE_VREG_TYPE(dv) == TCCIR_VREG_TYPE_VAR &&
          TCCIR_DECODE_VREG_POSITION(dv) == var_pos) {
        /* Found a definition of this VAR */
        if (q->op == TCCIR_OP_ASSIGN) {
          IROperand src = tcc_ir_op_get_src1(ir, q);
          if (irop_is_immediate(src)) {
            *out = irop_get_imm64_ex(ir, src);
            return SCCP_CONST;
          }
        }
        return SCCP_BOTTOM;
      }
    }

    /* Direct STORE to VAR: Vn <-- Tx [STORE] where dest is a VAR vreg.
     * STORE dests always have is_lval=1.  The VAR-vreg-type check alone
     * isn't enough to identify a slot write: cprop may rewrite a STORE
     * `T_DEREF <-- val` into `V_DEREF <-- val` when T was a copy of V
     * (cprop_copy_var_stackoff), which is a *pointer-deref through V's
     * value*, not a write to V's slot.  Require `dest.is_local=1` to
     * gate this branch — direct VAR-slot writes carry the is_local flag
     * inherited from the VT_LOCAL svalue, while pointer-deref dests
     * carry is_local=0 (they originated from a TEMP). */
    if (q->op == TCCIR_OP_STORE) {
      IROperand dest = tcc_ir_op_get_dest(ir, q);
      int32_t dv = irop_get_vreg(dest);
      if (dv >= 0 &&
          TCCIR_DECODE_VREG_TYPE(dv) == TCCIR_VREG_TYPE_VAR &&
          TCCIR_DECODE_VREG_POSITION(dv) == var_pos &&
          dest.is_local) {
        return sccp_get_store_src_value_ex(s, tcc_ir_op_get_src1(ir, q), i,
                                           out, dep_src_pos);
      }
    }

    /* STORE through pointer: *T = val where T ultimately resolves to &V */
    if (q->op == TCCIR_OP_STORE) {
      IROperand dest = tcc_ir_op_get_dest(ir, q);
      if (dest.tag == IROP_TAG_STACKOFF && dest.is_local && dest.is_lval) {
        IRLiveInterval *vi = tcc_ir_vreg_live_interval(ir, var_vreg);
        /* This matches `*(&V) <- val` (a store through V's address, which
         * lea_fold collapsed to a direct StackLoc[Voff] store) by offset.
         * But a VAR's slot offset and an *anonymous* local's offset live in
         * different namespaces and can collide numerically: lea_fold also
         * folds an unrelated anon aggregate `m` at the same offset into a
         * direct StackLoc store, which would then be mis-read as a write to V
         * (e.g. `m.kind=4` forwarded into a `stack_off` load).  A StackLoc
         * store can only be writing V's slot if V's address was actually
         * taken; otherwise V is register-resident and this is a foreign anon
         * local.  Require addrtaken to keep the offset-match sound. */
        if (vi && vi->addrtaken && vi->original_offset == irop_get_stack_offset(dest))
          return sccp_get_store_src_value(s, tcc_ir_op_get_src1(ir, q), out,
                                          dep_src_pos);
        continue;
      }

      int32_t addr_vr = irop_get_vreg(dest);
      if (addr_vr >= 0 && dest.is_lval) {
        /* Resolve addr_vr through ASSIGN/STORE chains within the block
         * to find if it ultimately points to &V (the var we're resolving).
         * Handles chains like: T18=&V0, V9=T18, T20=V9, so T20 is &V0. */
        int32_t cur_vr = addr_vr;
        int resolved = 0;
        for (int hop = 0; hop < 4 && !resolved; hop++) {
          int found_def = 0;
          for (int k = i - 1; k >= bb->start_idx; k--) {
            IRQuadCompact *lq = &ir->compact_instructions[k];
            if (lq->op == TCCIR_OP_NOP)
              continue;
            int is_def = 0;
            if (lq->op == TCCIR_OP_ASSIGN || lq->op == TCCIR_OP_LEA) {
              IROperand ld = tcc_ir_op_get_dest(ir, lq);
              if (irop_get_vreg(ld) == cur_vr)
                is_def = 1;
            }
            if (!is_def && lq->op == TCCIR_OP_STORE) {
              IROperand ld = tcc_ir_op_get_dest(ir, lq);
              int32_t sd = irop_get_vreg(ld);
              if (sd == cur_vr && !ld.is_lval)
                is_def = 1;
            }
            if (is_def) {
              IROperand ls = tcc_ir_op_get_src1(ir, lq);
              int32_t lsv = irop_get_vreg(ls);
              if (lsv >= 0 && ls.is_local && !ls.is_lval &&
                  TCCIR_DECODE_VREG_TYPE(lsv) == TCCIR_VREG_TYPE_VAR &&
                  TCCIR_DECODE_VREG_POSITION(lsv) == var_pos) {
                resolved = 1;
              } else if (lsv >= 0 && !ls.is_lval) {
                cur_vr = lsv;
                found_def = 1;
              }
              break;
            }
            if (irop_config[lq->op].has_dest) {
              IROperand ld = tcc_ir_op_get_dest(ir, lq);
              if (irop_get_vreg(ld) == cur_vr)
                break;
            }
          }
          if (!found_def)
            break;
        }
        if (resolved) {
          return sccp_get_store_src_value(s, tcc_ir_op_get_src1(ir, q), out,
                                          dep_src_pos);
        }

        /* Do not walk past unknown pointer stores.  They may alias the VAR
         * being resolved when its address escaped earlier in the block. */
        return SCCP_BOTTOM;
      }
    }
  }

  return SCCP_BOTTOM;
}

/* Get operand value, handling both TEMPs (via lattice) and VARs (via
 * backward scan within the current block). */
static int sccp_get_operand_value_ex(SCCPState *s, IROperand op,
                                     int instr_idx, int64_t *out)
{
  if (irop_is_immediate(op)) {
    *out = irop_get_imm64_ex(s->ctx->ir, op);
    return SCCP_CONST;
  }

  /* TEMP-DEREF operand: *T where T resolves to &StackLoc[N].  Forward to
   * stack-store scan at the resolved offset.
   *
   * VAR-DEREF (*V) is NOT a slot read of V — it dereferences V's value
   * (a pointer) and reads pointed-to memory.  Without alias info we
   * can't resolve it, so return BOTTOM rather than falling through to
   * sccp_resolve_var below (which would wrongly return V's slot value
   * as if it were *V).  Pattern appears after cprop_copy_var_stackoff
   * forwards a VAR into a deref-operand use site. */
  if (op.tag == IROP_TAG_VREG && op.is_lval && !op.is_local) {
    int32_t tvr = irop_get_vreg(op);
    if (tvr >= 0 && TCCIR_DECODE_VREG_TYPE(tvr) == TCCIR_VREG_TYPE_TEMP) {
      int load_off = ssa_opt_resolve_lea_stackloc(s->ctx, tvr);
      if (load_off != INT_MIN) {
        int dep_pos = -1;
        int st = sccp_resolve_stack_load(s, load_off, irop_get_btype(op),
                                          instr_idx, out, &dep_pos);
        if (st == SCCP_CONST)
          return SCCP_CONST;
      }
      return SCCP_BOTTOM;
    }
    if (tvr >= 0 && TCCIR_DECODE_VREG_TYPE(tvr) == TCCIR_VREG_TYPE_VAR)
      return SCCP_BOTTOM;
  }

  /* Direct StackLoc-lval operand: load from stack slot. */
  if (op.tag == IROP_TAG_STACKOFF && op.is_lval && op.is_local && !op.is_llocal) {
    int32_t svr = irop_get_vreg(op);
    if (svr < 0 || TCCIR_DECODE_VREG_TYPE(svr) != TCCIR_VREG_TYPE_VAR) {
      int dep_pos = -1;
      int st = sccp_resolve_stack_load(s, irop_get_stack_offset(op),
                                        irop_get_btype(op), instr_idx, out,
                                        &dep_pos);
      if (st == SCCP_CONST)
        return SCCP_CONST;
      return SCCP_BOTTOM;
    }
  }

  int32_t vr = irop_get_vreg(op);
  if (vr < 0)
    return SCCP_BOTTOM;

  if (TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_TEMP) {
    SCCPCell *c = sccp_cell(s, vr);
    if (!c)
      return SCCP_BOTTOM;
    if (c->state == SCCP_CONST)
      *out = c->value;
    return c->state;
  }

  if (TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_VAR) {
    int dep_src_pos;
    return sccp_resolve_var(s, vr, instr_idx, out, &dep_src_pos);
  }

  return SCCP_BOTTOM;
}

static int sccp_eval_binary(int op, int64_t v1, int64_t v2, int64_t *result,
                            int is_64)
{
  switch (op) {
  case TCCIR_OP_ADD: *result = v1 + v2; break;
  case TCCIR_OP_SUB: *result = v1 - v2; break;
  case TCCIR_OP_MUL: *result = v1 * v2; break;
  case TCCIR_OP_AND: *result = v1 & v2; break;
  case TCCIR_OP_OR:  *result = v1 | v2; break;
  case TCCIR_OP_XOR: *result = v1 ^ v2; break;
  case TCCIR_OP_SHL: {
    int mask = is_64 ? 63 : 31;
    *result = (int64_t)((uint64_t)v1 << (v2 & mask));
    break;
  }
  case TCCIR_OP_ROR: {
    uint32_t v = (uint32_t)v1;
    uint32_t n = (uint32_t)v2 & 31;
    *result = (int64_t)(int32_t)((v >> n) | (v << (32 - n)));
    break;
  }
  case TCCIR_OP_SHR: {
    int mask = is_64 ? 63 : 31;
    if (is_64)
      *result = (int64_t)((uint64_t)v1 >> (v2 & mask));
    else
      *result = (int64_t)((uint32_t)(int32_t)v1 >> (v2 & mask));
    break;
  }
  case TCCIR_OP_SAR: {
    int mask = is_64 ? 63 : 31;
    if (is_64)
      *result = v1 >> (v2 & mask);
    else
      *result = (int64_t)((int32_t)v1 >> (v2 & mask));
    break;
  }
  case TCCIR_OP_DIV:
    if (v2 == 0) return 0;
    /* INT_MIN / -1 is UB and traps on hardware divide.  Avoid folding so
     * the divisor's constant value doesn't propagate the trap into the
     * compiler itself. */
    if (v2 == -1) {
      if (is_64) {
        if (v1 == INT64_MIN) return 0;
      } else if ((int32_t)v1 == INT32_MIN) {
        return 0;
      }
    }
    if (is_64)
      *result = v1 / v2;
    else
      *result = (int64_t)((int32_t)v1 / (int32_t)v2);
    break;
  case TCCIR_OP_UDIV:
    if (v2 == 0) return 0;
    if (is_64)
      *result = (int64_t)((uint64_t)v1 / (uint64_t)v2);
    else
      *result = (int64_t)((uint32_t)(int32_t)v1 / (uint32_t)(int32_t)v2);
    break;
  case TCCIR_OP_IMOD:
    if (v2 == 0) return 0;
    if (v2 == -1) {
      if (is_64) {
        if (v1 == INT64_MIN) return 0;
      } else if ((int32_t)v1 == INT32_MIN) {
        return 0;
      }
    }
    if (is_64)
      *result = v1 % v2;
    else
      *result = (int64_t)((int32_t)v1 % (int32_t)v2);
    break;
  case TCCIR_OP_UMOD:
    if (v2 == 0) return 0;
    if (is_64)
      *result = (int64_t)((uint64_t)v1 % (uint64_t)v2);
    else
      *result = (int64_t)((uint32_t)(int32_t)v1 % (uint32_t)(int32_t)v2);
    break;
  default:
    return 0;
  }
  if (!is_64)
    *result = (int64_t)(int32_t)(uint32_t)*result;
  return 1;
}

static int sccp_eval_cond(int64_t v1, int64_t v2, int tok)
{
  switch (tok) {
  case 0x94: return v1 == v2;
  case 0x95: return v1 != v2;
  case 0x9c: return v1 < v2;
  case 0x9d: return v1 >= v2;
  case 0x9e: return v1 <= v2;
  case 0x9f: return v1 > v2;
  case 0x92: return (uint64_t)v1 < (uint64_t)v2;
  case 0x93: return (uint64_t)v1 >= (uint64_t)v2;
  case 0x96: return (uint64_t)v1 <= (uint64_t)v2;
  case 0x97: return (uint64_t)v1 > (uint64_t)v2;
  default: return -1;
  }
}

static void sccp_visit_phi(SCCPState *s, IRPhiNode *phi, int block)
{
  SCCPCell *dest_cell = sccp_cell(s, phi->dest_vreg);
  if (!dest_cell || dest_cell->state == SCCP_BOTTOM)
    return;

  int changed = 0;
  for (int i = 0; i < phi->num_operands; i++) {
    int pred = phi->operands[i].pred_block;
    if (pred < 0 || pred >= s->num_blocks)
      continue;
    if (!s->edge_exec[pred * s->num_blocks + block])
      continue;

    int32_t vr = phi->operands[i].vreg;
    SCCPCell *src = sccp_cell(s, vr);
    if (!src) {
      changed |= sccp_set_bottom(dest_cell);
      break;
    }
    if (src->state == SCCP_TOP)
      continue;
    if (src->state == SCCP_BOTTOM) {
      changed |= sccp_set_bottom(dest_cell);
      break;
    }
    changed |= sccp_meet(dest_cell, src->value);
    if (dest_cell->state == SCCP_BOTTOM)
      break;
  }

  if (changed) {
    int pos = TCCIR_DECODE_VREG_POSITION(phi->dest_vreg);
    sccp_add_ssa(s, pos);
  }
}

/* Conservative fixpoint repair for the optimistic-propagation gap documented at
 * the re-sweep loop: a phi (typically a loop-header phi in an un-rotated loop)
 * can settle at CONST while one of its operands — arriving on an EXECUTABLE
 * edge — is still TOP because its defining value was never lowered and the
 * worklist never re-propagated it.  sccp_visit_phi SKIPS TOP operands, so even
 * the re-sweep never widens such a phi.  At a true fixpoint no reachable value
 * stays TOP, so a TOP source on an executable edge is an inconsistency: trust
 * nothing and force the phi to BOTTOM rather than keep the partial constant
 * (which would fold the loop-carried value to its latch constant — a
 * miscompile, e.g. 990527-1's `for(...){j++; g(j); j=9;}` summing 9*10 instead
 * of 1+8*10).  Monotone (only descends cells), so convergence is preserved.
 * Returns the count of phis forced to BOTTOM. */
static int sccp_force_stuck_phis_bottom(SCCPState *s)
{
  IRSSAState *ssa = s->ctx->ssa;
  if (!ssa || !ssa->block_phis)
    return 0;
  int forced = 0;
  for (int blk = 0; blk < s->num_blocks; blk++) {
    if (!s->block_reachable[blk])
      continue;
    for (IRPhiNode *phi = ssa->block_phis[blk]; phi; phi = phi->next) {
      SCCPCell *dest = sccp_cell(s, phi->dest_vreg);
      if (!dest || dest->state != SCCP_CONST)
        continue;
      for (int i = 0; i < phi->num_operands; i++) {
        int pred = phi->operands[i].pred_block;
        if (pred < 0 || pred >= s->num_blocks)
          continue;
        if (!s->edge_exec[pred * s->num_blocks + blk])
          continue;
        SCCPCell *src = sccp_cell(s, phi->operands[i].vreg);
        if (src && src->state == SCCP_TOP) {
          if (sccp_set_bottom(dest)) {
            sccp_add_ssa(s, TCCIR_DECODE_VREG_POSITION(phi->dest_vreg));
            forced++;
          }
          break;
        }
      }
    }
  }
  return forced;
}

static void sccp_visit_instr(SCCPState *s, int idx)
{
  TCCIRState *ir = s->ctx->ir;
  IRCFG *cfg = s->ctx->cfg;
  IRQuadCompact *q = &ir->compact_instructions[idx];

  if (q->op == TCCIR_OP_NOP)
    return;

  int block = cfg->instr_to_block[idx];
  if (!s->block_reachable[block])
    return;

  /* Handle instructions that define a TEMP vreg */
  if (irop_config[q->op].has_dest &&
      q->op != TCCIR_OP_STORE && q->op != TCCIR_OP_STORE_INDEXED &&
      q->op != TCCIR_OP_STORE_POSTINC &&
      q->op != TCCIR_OP_FUNCPARAMVAL && q->op != TCCIR_OP_FUNCPARAMVOID) {
    IROperand dest = tcc_ir_op_get_dest(ir, q);
    int32_t dest_vr = irop_get_vreg(dest);
    SCCPCell *dest_cell = sccp_cell(s, dest_vr);
    if (!dest_cell)
      goto handle_control_flow;
    if (dest_cell->state == SCCP_BOTTOM)
      goto handle_control_flow;

    /* Skip float operations; can't meaningfully fold at compile time */
    if (dest.btype == IROP_BTYPE_FLOAT32 || dest.btype == IROP_BTYPE_FLOAT64) {
      if (sccp_set_bottom(dest_cell))
        sccp_add_ssa(s, TCCIR_DECODE_VREG_POSITION(dest_vr));
      goto handle_control_flow;
    }

    /* A barrel-shift-fused ALU op (opt_fusion) carries a hidden shift applied to
     * one operand, recorded in ir->barrel_shifts[] and invisible in the IR
     * operands.  Lattice-evaluating it as a plain ALU op would compute the wrong
     * constant (e.g. `x & (y<<7)` folded as `x & y`), so force it to BOTTOM — the
     * same guard GVN already uses (ssa_opt_gvn.c).  Random-C O1 wrong-code,
     * seed 215. */
    if (ir->barrel_shifts && q->orig_index >= 0 &&
        q->orig_index <= ir->max_orig_index &&
        ir->barrel_shifts[q->orig_index]) {
      if (sccp_set_bottom(dest_cell))
        sccp_add_ssa(s, TCCIR_DECODE_VREG_POSITION(dest_vr));
      goto handle_control_flow;
    }

    int is_64 = (dest.btype == IROP_BTYPE_INT64);

    /* ASSIGN: propagate source value */
    if (q->op == TCCIR_OP_ASSIGN) {
      IROperand src = tcc_ir_op_get_src1(ir, q);
      if (src.is_lval || src.is_local || src.is_llocal) {
        if (sccp_set_bottom(dest_cell))
          sccp_add_ssa(s, TCCIR_DECODE_VREG_POSITION(dest_vr));
        goto handle_control_flow;
      }
      int64_t val;
      int st = sccp_get_operand_value(s, src, &val);
      int changed = 0;
      if (st == SCCP_CONST)
        changed = sccp_meet(dest_cell, val);
      else if (st == SCCP_BOTTOM)
        changed = sccp_set_bottom(dest_cell);
      if (changed)
        sccp_add_ssa(s, TCCIR_DECODE_VREG_POSITION(dest_vr));
      goto handle_control_flow;
    }

    /* Binary ALU ops */
    if (irop_config[q->op].has_src1 && irop_config[q->op].has_src2) {
      IROperand src1 = tcc_ir_op_get_src1(ir, q);
      IROperand src2 = tcc_ir_op_get_src2(ir, q);

      if (src1.is_lval || src1.is_local || src1.is_llocal ||
          src2.is_lval || src2.is_local || src2.is_llocal) {
        if (sccp_set_bottom(dest_cell))
          sccp_add_ssa(s, TCCIR_DECODE_VREG_POSITION(dest_vr));
        goto handle_control_flow;
      }

      int64_t v1, v2;
      int st1 = sccp_get_operand_value(s, src1, &v1);
      int st2 = sccp_get_operand_value(s, src2, &v2);

      if (st1 == SCCP_TOP || st2 == SCCP_TOP)
        goto handle_control_flow;
      if (st1 == SCCP_BOTTOM || st2 == SCCP_BOTTOM) {
        if (sccp_set_bottom(dest_cell))
          sccp_add_ssa(s, TCCIR_DECODE_VREG_POSITION(dest_vr));
        goto handle_control_flow;
      }

      int64_t result;
      int changed;
      if (sccp_eval_binary(q->op, v1, v2, &result, is_64))
        changed = sccp_meet(dest_cell, result);
      else
        changed = sccp_set_bottom(dest_cell);
      if (changed)
        sccp_add_ssa(s, TCCIR_DECODE_VREG_POSITION(dest_vr));
      goto handle_control_flow;
    }

    /* LOAD from VAR: resolve the VAR's value within this block. */
    if (q->op == TCCIR_OP_LOAD && irop_config[q->op].has_src1) {
      IROperand src = tcc_ir_op_get_src1(ir, q);
      int32_t svr = irop_get_vreg(src);
      if (svr >= 0 && TCCIR_DECODE_VREG_TYPE(svr) == TCCIR_VREG_TYPE_VAR) {
        int64_t val;
        int dep_src_pos;
        int st = sccp_resolve_var(s, svr, idx, &val, &dep_src_pos);
        if (dep_src_pos >= 0)
          sccp_add_mem_dep(s, dep_src_pos, idx);
        int changed = 0;
        if (st == SCCP_CONST)
          changed = sccp_meet(dest_cell, val);
        else if (st == SCCP_BOTTOM)
          changed = sccp_set_bottom(dest_cell);
        if (changed)
          sccp_add_ssa(s, TCCIR_DECODE_VREG_POSITION(dest_vr));
        goto handle_control_flow;
      }

      /* LOAD from StackLoc: scan backward for a constant store to the
       * same offset within this block (and dominators). */
      if (src.tag == IROP_TAG_STACKOFF && src.is_lval && src.is_local &&
          !src.is_llocal && (svr < 0 || TCCIR_DECODE_VREG_TYPE(svr) != TCCIR_VREG_TYPE_VAR)) {
        int64_t sval = 0;
        int dep_pos = -1;
        int rst = sccp_resolve_stack_load(s, irop_get_stack_offset(src),
                                           irop_get_btype(src), idx, &sval,
                                           &dep_pos);
        if (rst == SCCP_CONST) {
          int changed = sccp_meet(dest_cell, sval);
          if (dep_pos >= 0)
            sccp_add_mem_dep(s, dep_pos, idx);
          if (changed)
            sccp_add_ssa(s, TCCIR_DECODE_VREG_POSITION(dest_vr));
          goto handle_control_flow;
        }
      }

      /* LOAD via TEMP-LEA-DEREF: T <-- *Tp [LOAD] where Tp resolves to
       * &StackLoc[N].  Reuse the same backward scan after resolving the
       * effective offset. */
      if (src.tag == IROP_TAG_VREG && src.is_lval && !src.is_local) {
        int eff_off = ssa_opt_indirect_stack_offset(s->ctx, q, SSA_OPT_INDIRECT_SRC1);
        if (eff_off != INT_MIN) {
          int64_t sval = 0;
          int dep_pos = -1;
          int rst = sccp_resolve_stack_load(s, eff_off, irop_get_btype(src),
                                             idx, &sval, &dep_pos);
          if (rst == SCCP_CONST) {
            int changed = sccp_meet(dest_cell, sval);
            if (dep_pos >= 0)
              sccp_add_mem_dep(s, dep_pos, idx);
            if (changed)
              sccp_add_ssa(s, TCCIR_DECODE_VREG_POSITION(dest_vr));
            goto handle_control_flow;
          }
        }
      }
    }

    /* Anything else: BOTTOM */
    if (sccp_set_bottom(dest_cell))
      sccp_add_ssa(s, TCCIR_DECODE_VREG_POSITION(dest_vr));
  }

handle_control_flow:
  /* CMP/TEST_ZERO don't produce control flow themselves; forward to the
   * following JUMPIF so branch edges are re-evaluated when operands change. */
  if (q->op == TCCIR_OP_CMP || q->op == TCCIR_OP_TEST_ZERO) {
    int ni = idx + 1;
    while (ni < ir->next_instruction_index &&
           ir->compact_instructions[ni].op == TCCIR_OP_NOP)
      ni++;
    if (ni < ir->next_instruction_index &&
        ir->compact_instructions[ni].op == TCCIR_OP_JUMPIF)
      sccp_visit_instr(s, ni);
  }
  /* Handle control flow: determine which successor edges are executable */
  if (q->op == TCCIR_OP_JUMP) {
    IROperand dest = tcc_ir_op_get_dest(ir, q);
    int target = dest.u.imm32;
    int target_block = (target >= 0 && target < cfg->num_instrs) ?
                        cfg->instr_to_block[target] : -1;
    sccp_add_cfg_edge(s, block, target_block);
  }
  else if (q->op == TCCIR_OP_JUMPIF) {
    /* Find preceding CMP/TEST_ZERO */
    int ci = idx - 1;
    while (ci >= 0 && ir->compact_instructions[ci].op == TCCIR_OP_NOP)
      ci--;

    IROperand jmp_dest = tcc_ir_op_get_dest(ir, q);
    int target = jmp_dest.u.imm32;
    int target_block = (target >= 0 && target < cfg->num_instrs) ?
                        cfg->instr_to_block[target] : -1;
    /* Fallthrough block */
    IRBasicBlock *bb = &cfg->blocks[block];
    int fall_block = -1;
    for (int si = 0; si < bb->num_succs; si++) {
      if (bb->succs[si] != target_block) {
        fall_block = bb->succs[si];
        break;
      }
    }
    /* Degenerate conditional branch: the taken target IS the fall-through
     * block (a JUMPIF to the next instruction), so there is no successor
     * distinct from target_block and the loop above leaves fall_block = -1.
     * Both branch outcomes go to the same single successor — point the
     * fall-through there too, otherwise resolving the branch "not taken"
     * would add an edge to block -1 and leave the real successor (and any
     * definition it carries into a downstream phi) wrongly unreachable.
     * DCE collapsing the only instruction between a JUMPIF and its target
     * produces exactly this shape (seed 1454). */
    if (fall_block < 0)
      fall_block = target_block;

    int resolved = 0;
    if (ci >= 0) {
      IRQuadCompact *cmp_q = &ir->compact_instructions[ci];
      if (cmp_q->op == TCCIR_OP_CMP) {
        IROperand s1 = tcc_ir_op_get_src1(ir, cmp_q);
        IROperand s2 = tcc_ir_op_get_src2(ir, cmp_q);
        int64_t v1, v2;
        int st1 = sccp_get_operand_value_ex(s, s1, ci, &v1);
        int st2 = sccp_get_operand_value_ex(s, s2, ci, &v2);
        if (st1 == SCCP_CONST && st2 == SCCP_CONST) {
          /* Truncate to operand width to avoid sign-extension mismatches
           * (e.g., IMM32 0x83FD4005 sign-extends to -2080555003 while
           * I64 0x83FD4005 stays 2214412293). */
          int cmp_btype = irop_get_btype(s1);
          if (cmp_btype != IROP_BTYPE_INT64) {
            v1 = (int64_t)(int32_t)(uint32_t)v1;
            v2 = (int64_t)(int32_t)(uint32_t)v2;
          }
          IROperand cond = tcc_ir_op_get_src1(ir, q);
          int tok = (int)irop_get_imm64_ex(ir, cond);
          int result = sccp_eval_cond(v1, v2, tok);
          if (result >= 0) {
            if (result)
              sccp_add_cfg_edge(s, block, target_block);
            else
              sccp_add_cfg_edge(s, block, fall_block);
            resolved = 1;
          }
        }
      }
      else if (cmp_q->op == TCCIR_OP_TEST_ZERO) {
        IROperand s1 = tcc_ir_op_get_src1(ir, cmp_q);
        int64_t v1;
        int st1 = sccp_get_operand_value_ex(s, s1, ci, &v1);
        if (st1 == SCCP_CONST) {
          IROperand cond = tcc_ir_op_get_src1(ir, q);
          int tok = (int)irop_get_imm64_ex(ir, cond);
          int branch = (tok == 0x94) ? (v1 == 0) : (tok == 0x95) ? (v1 != 0) : -1;
          if (branch >= 0) {
            if (branch)
              sccp_add_cfg_edge(s, block, target_block);
            else
              sccp_add_cfg_edge(s, block, fall_block);
            resolved = 1;
          }
        }
      }
    }
    if (!resolved) {
      sccp_add_cfg_edge(s, block, target_block);
      sccp_add_cfg_edge(s, block, fall_block);
    }
  }
  else if (q->op == TCCIR_OP_SWITCH_TABLE || q->op == TCCIR_OP_IJUMP) {
    IRBasicBlock *bb = &cfg->blocks[block];
    for (int si = 0; si < bb->num_succs; si++)
      sccp_add_cfg_edge(s, block, bb->succs[si]);
  }
}

static void sccp_process_cfg_edge(SCCPState *s, int pred, int succ)
{
  IRCFG *cfg = s->ctx->cfg;
  IRSSAState *ssa = s->ctx->ssa;

  int first_visit = !s->block_reachable[succ];
  s->block_reachable[succ] = 1;

  /* Visit phi nodes in the successor with this new edge */
  if (ssa->block_phis) {
    for (IRPhiNode *phi = ssa->block_phis[succ]; phi; phi = phi->next)
      sccp_visit_phi(s, phi, succ);
  }

  if (first_visit) {
    /* First time this block is reachable: visit all instructions */
    IRBasicBlock *bb = &cfg->blocks[succ];
    for (int i = bb->start_idx; i < bb->end_idx; i++)
      sccp_visit_instr(s, i);

    /* Mark fallthrough edge unless the block ends in an explicit control-flow
     * terminator.  Scan back past trailing NOPs to find the last real
     * instruction.  Crucially, a block that is EMPTY or consists only of NOPs
     * (e.g. its sole jump-to-the-next-block was elided to a NOP by an earlier
     * pass such as jump-threading / fallthrough elimination) still falls
     * through to its successor at runtime, so its CFG successor edges must be
     * marked executable.  Failing to do so leaves the successor (and any loop
     * latch / back-edge reached through it) unreachable, which would let an
     * induction-variable phi optimistically fold to its loop-entry constant. */
    {
      int li = bb->end_idx - 1;
      while (li >= bb->start_idx && s->ctx->ir->compact_instructions[li].op == TCCIR_OP_NOP)
        li--;
      int ends_with_terminator = 0;
      if (li >= bb->start_idx) {
        int lop = s->ctx->ir->compact_instructions[li].op;
        ends_with_terminator = (lop == TCCIR_OP_JUMP || lop == TCCIR_OP_JUMPIF ||
                                lop == TCCIR_OP_IJUMP || lop == TCCIR_OP_RETURNVALUE ||
                                lop == TCCIR_OP_RETURNVOID || lop == TCCIR_OP_SWITCH_TABLE);
      }
      if (!ends_with_terminator) {
        for (int si = 0; si < bb->num_succs; si++)
          sccp_add_cfg_edge(s, succ, bb->succs[si]);
      }
    }
  }
}

/* ============================================================================
 * Apply SCCP results: replace constants, fold branches, NOP dead code
 * ============================================================================ */

void dbg_scan_imm_dest(TCCIRState *ir, const char *pass);
static int sccp_apply(SCCPState *s)
{
  TCCIRState *ir = s->ctx->ir;
  int changes = 0;

  if (getenv("DUMP_OB")) {
    fprintf(stderr, "=== operand layout at sccp_apply entry ===\n");
    for (int i = 0; i < ir->next_instruction_index && i < 12; i++) {
      IRQuadCompact *q = &ir->compact_instructions[i];
      int nops = irop_config[q->op].has_dest + irop_config[q->op].has_src1 + irop_config[q->op].has_src2;
      fprintf(stderr, "  insn %d: op=%d ob=%d nops=%d -> slots[%d..%d]\n",
              i, (int)q->op, q->operand_base, nops, q->operand_base, q->operand_base + nops - 1);
    }
  }

  /* Phase 1: Replace constant-valued instructions with ASSIGN #const */
  for (int pos = 0; pos < s->cells_cap; pos++) {
    if (s->cells[pos].state != SCCP_CONST)
      continue;
    IRSSAVregInfo *vi = &s->ctx->vinfo[pos];
    if (vi->def_instr < 0)
      continue;
    if (vi->def_count > 1)
      continue;

    IRQuadCompact *q = &ir->compact_instructions[vi->def_instr];
    if (q->op == TCCIR_OP_NOP)
      continue;
    if (ssa_opt_has_side_effects(q->op))
      continue;
    /* Already a constant ASSIGN; nothing to do */
    if (q->op == TCCIR_OP_ASSIGN) {
      IROperand src = tcc_ir_op_get_src1(ir, q);
      if (irop_is_immediate(src))
        continue;
    }

    IROperand dest = tcc_ir_op_get_dest(ir, q);
    if (getenv("SCAN_IMM_DEST"))
      fprintf(stderr, "SCCP rewrite def_instr=%d orig_op=%d has_dest=%d has_src1=%d has_src2=%d ob=%d\n",
              vi->def_instr, (int)q->op, irop_config[q->op].has_dest, irop_config[q->op].has_src1,
              irop_config[q->op].has_src2, q->operand_base);
    int64_t val = s->cells[pos].value;
    IROperand imm;
    if (val == (int64_t)(int32_t)val) {
      imm = irop_make_imm32(0, (int32_t)val, dest.btype);
    } else {
      uint32_t pool_idx = tcc_ir_pool_add_i64(ir, val);
      imm = irop_make_i64(0, pool_idx, dest.btype);
    }

    /* Remove uses of old operands */
    if (irop_config[q->op].has_src1) {
      IRSSAVregInfo *svi = ssa_opt_vinfo(s->ctx, irop_get_vreg(tcc_ir_op_get_src1(ir, q)));
      if (svi) ssa_opt_remove_use_instr(svi, vi->def_instr);
    }
    if (irop_config[q->op].has_src2) {
      IRSSAVregInfo *svi = ssa_opt_vinfo(s->ctx, irop_get_vreg(tcc_ir_op_get_src2(ir, q)));
      if (svi) ssa_opt_remove_use_instr(svi, vi->def_instr);
    }

    q->op = TCCIR_OP_ASSIGN;
    tcc_ir_set_src1(ir, vi->def_instr, imm);
    tcc_ir_set_src2(ir, vi->def_instr, IROP_NONE);
    changes++;
    if (getenv("SCAN_IMM_DEST")) {
      for (int j = 0; j < ir->next_instruction_index; j++) {
        IRQuadCompact *jq = &ir->compact_instructions[j];
        if (jq->op != TCCIR_OP_ASSIGN) continue;
        if (irop_get_tag(tcc_ir_op_get_dest(ir, jq)) == IROP_TAG_IMM32) {
          fprintf(stderr, "CORRUPT insn %d (ob=%d nops=%d) after rewriting def_instr=%d (op_now=%d ob=%d nops=%d)\n",
                  j, jq->operand_base, (irop_config[jq->op].has_dest + irop_config[jq->op].has_src1 + irop_config[jq->op].has_src2),
                  vi->def_instr, (int)q->op, q->operand_base, (irop_config[q->op].has_dest + irop_config[q->op].has_src1 + irop_config[q->op].has_src2));
          break;
        }
      }
    }
  }

  /* Phase 1.5: Rewrite CMP/TEST_ZERO operands that resolve to constants
   * via stack-load forwarding or VAR scanning.  Uses sccp_phase15_resolve
   * (stricter than the general resolve_var) to avoid being misled by
   * `vi->original_offset` heuristics for non-param VARs. */
  IRCFG *cfg = s->ctx->cfg;
  for (int i = 0; i < ir->next_instruction_index; i++) {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_CMP && q->op != TCCIR_OP_TEST_ZERO)
      continue;
    int n_srcs = (q->op == TCCIR_OP_CMP) ? 2 : 1;
    int block = cfg->instr_to_block[i];
    IRBasicBlock *bb = &cfg->blocks[block];
    for (int oi = 0; oi < n_srcs; oi++) {
      IROperand src = oi == 0 ? tcc_ir_op_get_src1(ir, q) : tcc_ir_op_get_src2(ir, q);
      if (irop_is_immediate(src))
        continue;

      int64_t val = 0;
      int got = 0;

      /* Case A: TEMP-DEREF *T where T resolves to &StackLoc[N]. */
      if (src.tag == IROP_TAG_VREG && src.is_lval && !src.is_local) {
        int32_t tvr = irop_get_vreg(src);
        if (tvr >= 0 && TCCIR_DECODE_VREG_TYPE(tvr) == TCCIR_VREG_TYPE_TEMP) {
          int load_off = ssa_opt_resolve_lea_stackloc(s->ctx, tvr);
          if (load_off != INT_MIN) {
            int dep_pos = -1;
            int st = sccp_resolve_stack_load(s, load_off, irop_get_btype(src),
                                              i, &val, &dep_pos);
            if (st == SCCP_CONST)
              got = 1;
          }
        }
      }

      /* Case B: direct StackLoc-lvalue (not a VAR). */
      if (!got && src.tag == IROP_TAG_STACKOFF && src.is_lval &&
          src.is_local && !src.is_llocal) {
        int32_t svr = irop_get_vreg(src);
        if (svr < 0 || TCCIR_DECODE_VREG_TYPE(svr) != TCCIR_VREG_TYPE_VAR) {
          int dep_pos = -1;
          int st = sccp_resolve_stack_load(s, irop_get_stack_offset(src),
                                            irop_get_btype(src), i, &val,
                                            &dep_pos);
          if (st == SCCP_CONST)
            got = 1;
        } else if (svr >= 0 && TCCIR_DECODE_VREG_TYPE(svr) == TCCIR_VREG_TYPE_VAR) {
          /* Case C: VAR operand.  Walk back in the same block looking
           * for the most recent def of this VAR, requiring it to be a
           * direct ASSIGN or STORE-to-VAR with an immediate src.  Bail
           * on any potentially-aliasing intervening write. */
          int var_pos = TCCIR_DECODE_VREG_POSITION(svr);
          for (int k = i - 1; k >= bb->start_idx; k--) {
            IRQuadCompact *kq = &ir->compact_instructions[k];
            if (kq->op == TCCIR_OP_NOP) continue;
            if (kq->op == TCCIR_OP_FUNCCALLVOID || kq->op == TCCIR_OP_FUNCCALLVAL)
              break;
            if (kq->op == TCCIR_OP_STORE_INDEXED || kq->op == TCCIR_OP_STORE_POSTINC)
              break;
            if (irop_config[kq->op].has_dest) {
              IROperand kd = tcc_ir_op_get_dest(ir, kq);
              int32_t kdv = irop_get_vreg(kd);
              if (kdv >= 0 &&
                  TCCIR_DECODE_VREG_TYPE(kdv) == TCCIR_VREG_TYPE_VAR &&
                  TCCIR_DECODE_VREG_POSITION(kdv) == var_pos) {
                if (kq->op == TCCIR_OP_ASSIGN || kq->op == TCCIR_OP_STORE) {
                  IROperand ks = tcc_ir_op_get_src1(ir, kq);
                  if (irop_is_immediate(ks) && !ks.is_lval) {
                    val = irop_get_imm64_ex(ir, ks);
                    got = 1;
                  }
                }
                break;
              }
            }
            if (kq->op == TCCIR_OP_STORE) {
              IROperand kd = tcc_ir_op_get_dest(ir, kq);
              if (kd.tag == IROP_TAG_STACKOFF && kd.is_local && kd.is_lval)
                continue;
              break;
            }
          }
          /* If the VAR is still read elsewhere AND the constant requires
           * a pool load, don't substitute — keep the VAR alive so a single
           * load satisfies both this CMP and the other use(s).  Mirrors
           * the guard in tcc_ir_opt_const_prop (pre-SSA).  SSA info isn't
           * reliable for pre-SSA-tracked VARs at this point, so count uses
           * via a direct IR scan. */
          if (got) {
            uint32_t uv = (uint32_t)val;
            int needs_pool = (uv > 0xFFFFu && uv < 0xFFFF0001u);
            if (needs_pool) {
              int other_uses = 0;
              int n2 = ir->next_instruction_index;
              for (int u = 0; u < n2 && other_uses < 2; u++) {
                if (u == i) continue;
                IRQuadCompact *uq = &ir->compact_instructions[u];
                if (uq->op == TCCIR_OP_NOP) continue;
                for (int oi = 0; oi < 2; oi++) {
                  if (oi == 0 && !irop_config[uq->op].has_src1) continue;
                  if (oi == 1 && !irop_config[uq->op].has_src2) continue;
                  IROperand op = oi == 0 ? tcc_ir_op_get_src1(ir, uq)
                                         : tcc_ir_op_get_src2(ir, uq);
                  if (irop_get_vreg(op) == svr &&
                      !(op.is_local && !op.is_lval)) {
                    other_uses++;
                    break;
                  }
                }
              }
              if (other_uses > 0)
                got = 0;
            }
          }
        }
      }

      if (!got)
        continue;
      IROperand imm;
      if (val == (int64_t)(int32_t)val)
        imm = irop_make_imm32(0, (int32_t)val, irop_get_btype(src));
      else
        imm = irop_make_i64(0, tcc_ir_pool_add_i64(ir, val), irop_get_btype(src));
      /* Drop the old operand's use entry so cascading DCE can fire when
       * its remaining uses go to zero. */
      int32_t old_vr = irop_get_vreg(src);
      if (old_vr >= 0) {
        IRSSAVregInfo *ovi = ssa_opt_vinfo(s->ctx, old_vr);
        if (ovi)
          ssa_opt_remove_use_instr(ovi, i);
      }
      if (oi == 0)
        tcc_ir_set_src1(ir, i, imm);
      else
        tcc_ir_set_src2(ir, i, imm);
      changes++;
    }
  }

  /* Branch folding is left to the ssa_opt_branch pass which runs after
   * cprop has propagated SCCP's constant replacements into CMP operands.
   * Folding branches here would invalidate the CFG that subsequent SSA
   * passes depend on. */

  /* Phase 3: Unreachable code removal is left to the pre-SSA DCE pass
   * which runs after SSA optimization. Branch folding in Phase 2 converts
   * conditional branches to unconditional JUMPs or NOPs, making the dead
   * edges unreachable for DCE to clean up. Direct NOP of unreachable
   * blocks here would require careful phi cleanup to avoid corrupting
   * SSA state for subsequent passes. */

  return changes;
}

/* ============================================================================
 * Pass Entry Point
 * ============================================================================ */

int ssa_opt_sccp(IRSSAOptCtx *ctx)
{
  IRCFG *cfg = ctx->cfg;
  IRSSAState *ssa = ctx->ssa;
  if (!cfg || cfg->num_blocks == 0 || !ssa)
    return 0;

  int nb = cfg->num_blocks;
  int ntmp = ctx->vinfo_cap;

  /* Guard against excessive memory for large CFGs */
  if ((int64_t)nb * nb > 100000)
    return 0;

  SCCPState s;
  memset(&s, 0, sizeof(s));
  s.ctx = ctx;
  s.num_blocks = nb;
  s.cells_cap = ntmp;
  s.cells = tcc_mallocz(ntmp * sizeof(SCCPCell));
  s.block_reachable = tcc_mallocz(nb);
  s.edge_exec = tcc_mallocz((size_t)nb * nb);
  int max_edges = nb * nb < 4096 ? nb * nb : 4096;
  s.cfg_wl_cap = max_edges;
  s.cfg_wl = tcc_mallocz(max_edges * sizeof(int));
  s.ssa_wl_cap = ntmp;
  s.ssa_wl = tcc_mallocz(ntmp * sizeof(int));

  /* Entry definitions (function params, uninitialized locals) have no
   * defining instruction or phi; mark them BOTTOM so they don't stay
   * TOP and cause PHIs to optimistically fold to a single constant. */
  for (int pos = 0; pos < ntmp; pos++) {
    IRSSAVregInfo *vi = &ctx->vinfo[pos];
    if (vi->def_instr < 0 && vi->def_phi_block < 0)
      s.cells[pos].state = SCCP_BOTTOM;
  }

  /* Seed: entry block is reachable */
  s.block_reachable[0] = 1;
  if (ssa->block_phis) {
    for (IRPhiNode *phi = ssa->block_phis[0]; phi; phi = phi->next)
      sccp_visit_phi(&s, phi, 0);
  }
  IRBasicBlock *entry = &cfg->blocks[0];
  for (int i = entry->start_idx; i < entry->end_idx; i++)
    sccp_visit_instr(&s, i);
  {
    int li = entry->end_idx - 1;
    while (li >= entry->start_idx && ctx->ir->compact_instructions[li].op == TCCIR_OP_NOP)
      li--;
    if (li >= entry->start_idx) {
      IRQuadCompact *last = &ctx->ir->compact_instructions[li];
      if (last->op != TCCIR_OP_JUMP && last->op != TCCIR_OP_JUMPIF &&
          last->op != TCCIR_OP_IJUMP && last->op != TCCIR_OP_RETURNVALUE &&
          last->op != TCCIR_OP_RETURNVOID && last->op != TCCIR_OP_SWITCH_TABLE) {
        for (int si = 0; si < entry->num_succs; si++)
          sccp_add_cfg_edge(&s, 0, entry->succs[si]);
      }
    }
  }

  /* Main loop: process both worklists until empty.
   * Guard against non-convergence (e.g., memory dependency cycles).
   *
   * Soundness re-sweep: the use-def worklist propagation has gaps (e.g. a
   * loop-header phi first folds to its preheader entry constant, that CONST
   * is consumed by a dependent instruction, and when the phi later widens to
   * BOTTOM the dependent's cell is not always re-evaluated — seen with
   * `T = n SHR #32` keeping a stale CONST 0 after `n` went BOTTOM, which
   * truncated 64-bit switch-case constants in parse_number).  Rather than
   * chase every missing edge, re-visit ALL reachable phis and instructions
   * after the worklists drain; any change re-seeds the worklists and we run
   * another round.  sccp_visit_* is monotone (cells only descend
   * TOP→CONST→BOTTOM, edges only become executable), so this converges. */
  for (;;) {
    while (s.cfg_wl_count > 0 || s.ssa_wl_count > 0) {
      while (s.cfg_wl_count > 0) {
        int edge = s.cfg_wl[--s.cfg_wl_count];
        int pred = edge / nb;
        int succ = edge % nb;
        sccp_process_cfg_edge(&s, pred, succ);
      }

      while (s.ssa_wl_count > 0) {
        int pos = s.ssa_wl[--s.ssa_wl_count];
        int32_t vreg = TCCIR_ENCODE_VREG(TCCIR_VREG_TYPE_TEMP, pos);
        IRSSAVregInfo *vi = ssa_opt_vinfo(ctx, vreg);
        if (!vi)
          continue;

        for (int u = 0; u < vi->use_count; u++) {
          IRSSAUse *use = &vi->uses[u];
          if (use->kind == SSA_USE_INSTR) {
            int blk = cfg->instr_to_block[use->idx];
            if (s.block_reachable[blk])
              sccp_visit_instr(&s, use->idx);
          } else {
            int blk = use->idx;
            if (s.block_reachable[blk] && ssa->block_phis) {
              for (IRPhiNode *phi = ssa->block_phis[blk]; phi; phi = phi->next) {
                for (int pi = 0; pi < phi->num_operands; pi++) {
                  if (phi->operands[pi].vreg == vreg) {
                    sccp_visit_phi(&s, phi, blk);
                    break;
                  }
                }
              }
            }
          }
        }

        /* Re-evaluate LOADs that depend on this TEMP through STORE→LOAD
         * memory chains. Without this, a LOAD whose value was resolved
         * via a STORE source TEMP would stay at CONST even after the
         * source TEMP moves to BOTTOM. */
        for (int m = 0; m < s.mem_dep_count; m++) {
          if (s.mem_deps[m].src_pos == pos) {
            int li = s.mem_deps[m].load_idx;
            int blk = cfg->instr_to_block[li];
            if (s.block_reachable[blk])
              sccp_visit_instr(&s, li);
          }
        }
      }
    }

    /* Re-sweep all reachable code; if nothing changed the worklists stay
     * empty and the result is a sound fixpoint. */
    for (int blk = 0; blk < nb; blk++) {
      if (!s.block_reachable[blk])
        continue;
      if (ssa->block_phis) {
        for (IRPhiNode *phi = ssa->block_phis[blk]; phi; phi = phi->next)
          sccp_visit_phi(&s, phi, blk);
      }
      IRBasicBlock *bb = &cfg->blocks[blk];
      for (int i = bb->start_idx; i < bb->end_idx; i++)
        sccp_visit_instr(&s, i);
    }
    /* Repair optimistic-fold gaps: any phi left CONST with a TOP operand on an
     * executable edge is widened to BOTTOM, re-seeding the worklists so its
     * dependents re-evaluate before we accept the fixpoint. */
    sccp_force_stuck_phis_bottom(&s);
    if (s.cfg_wl_count == 0 && s.ssa_wl_count == 0)
      break;
  }

  int changes = sccp_apply(&s);

  tcc_free(s.cells);
  tcc_free(s.block_reachable);
  tcc_free(s.edge_exec);
  tcc_free(s.cfg_wl);
  tcc_free(s.ssa_wl);
  tcc_free(s.mem_deps);
  if (s.loops)
    tcc_ir_free_loops(s.loops);

  return changes;
}
