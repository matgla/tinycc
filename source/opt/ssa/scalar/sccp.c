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

enum { SCCP_TOP = 0, SCCP_CONST = 1, SCCP_BOTTOM = 2 };

typedef struct {
  uint8_t state;
  int64_t value;
} SCCPCell;

/* The SSA worklist tracks direct vreg uses only, not STORE→LOAD memory chains. */
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
  IRLoops *loops;
  int loops_done;
  /* Frame offsets whose address is passed to a call or stored as a value. */
  struct { int lo, hi; } *esc;
  int esc_count;
  int esc_cap;
  int esc_done;
  int esc_all;
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

/* Also resolves a STACKOFF-lvalue source; instr_idx drives the dominator walk. */
static int sccp_get_store_src_value_ex(SCCPState *s, IROperand src,
                                       int instr_idx, int64_t *out,
                                       int *src_pos_out)
{
  int st = sccp_get_store_src_value(s, src, out, src_pos_out);
  if (st != SCCP_BOTTOM)
    return st;

  int load_off = INT_MIN;
  if (src.tag == IROP_TAG_STACKOFF && src.is_lval && src.is_local && !src.is_llocal) {
    int32_t svr = irop_get_vreg(src);
    if (svr < 0 || TCCIR_DECODE_VREG_TYPE(svr) != TCCIR_VREG_TYPE_VAR)
      load_off = irop_get_stack_offset(src);
  }
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

/* Unknown/struct must report 8 so unrelated stores can't be proven non-aliasing. */
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

/* Stack offset a STORE-class instruction targets; INT_MIN when unresolvable. */
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

/* Returns 1 when non-aliasing with an unknown-pointer load cannot be proven. */
static int sccp_store_may_escape(IRSSAOptCtx *ctx, IRQuadCompact *sq)
{
  TCCIRState *ir = ctx->ir;
  IROperand sd = tcc_ir_op_get_dest(ir, sq);
  /* Resolvable stack dests are compared by offset in the caller. */
  if (sd.tag == IROP_TAG_STACKOFF && sd.is_lval && sd.is_local)
    return 0;
  if (sd.tag == IROP_TAG_VREG && sd.is_lval && !sd.is_local) {
    int off = ssa_opt_indirect_stack_offset(ctx, sq, SSA_OPT_INDIRECT_DEST);
    if (off != INT_MIN)
      return 0;
  }
  /* Named-local-slot writes are separate from the stack load being tracked. */
  if (sd.is_local && !sd.is_lval)
    return 0;
  return 1;
}

static void sccp_esc_add(SCCPState *s, int lo, int hi)
{
  if (s->esc_count >= s->esc_cap) {
    s->esc_cap = s->esc_cap ? s->esc_cap * 2 : 8;
    s->esc = tcc_realloc(s->esc, s->esc_cap * sizeof(*s->esc));
  }
  s->esc[s->esc_count].lo = lo;
  s->esc[s->esc_count].hi = hi;
  s->esc_count++;
}

enum { ESC_SAFE, ESC_ESCAPE, ESC_FLOOD };

/* role: 0=src1, 1=src2/accum, 2=dest-as-base.  ESC_FLOOD: address flows to dest. */
static int sccp_esc_classify(TccIrOp op, int role, int dest_is_lval)
{
  switch (op) {
  case TCCIR_OP_FUNCPARAMVAL:
  case TCCIR_OP_FUNCPARAMVOID:
  case TCCIR_OP_RETURNVALUE:
    return ESC_ESCAPE;
  case TCCIR_OP_STORE:
    if (role == 2)
      return ESC_SAFE;
    return dest_is_lval ? ESC_ESCAPE : ESC_FLOOD;
  case TCCIR_OP_STORE_INDEXED:
  case TCCIR_OP_STORE_POSTINC:
    return role == 2 ? ESC_SAFE : ESC_ESCAPE;
  case TCCIR_OP_LOAD_INDEXED:
  case TCCIR_OP_LOAD_POSTINC:
    return role == 0 ? ESC_SAFE : ESC_ESCAPE;
  case TCCIR_OP_BLOCK_COPY:
    return ESC_SAFE;
  case TCCIR_OP_CMP:
  case TCCIR_OP_TEST_ZERO:
    return ESC_SAFE;
  default:
    return ESC_FLOOD;
  }
}

/* Anything untrackable (VAR/PARAM vreg, phi) must poison the whole frame. */
static void sccp_esc_flood(SCCPState *s, int32_t vr, int lo, int hi,
                           uint8_t *visited, int depth)
{
  TCCIRState *ir = s->ctx->ir;
  if (depth > 128) {
    s->esc_all = 1;
    return;
  }
  if (vr < 0 || TCCIR_DECODE_VREG_TYPE(vr) != TCCIR_VREG_TYPE_TEMP) {
    s->esc_all = 1;
    return;
  }
  IRSSAVregInfo *vi = ssa_opt_vinfo(s->ctx, vr);
  int pos = TCCIR_DECODE_VREG_POSITION(vr);
  if (!vi) {
    s->esc_all = 1;
    return;
  }
  if (visited[pos])
    return;
  visited[pos] = 1;
  for (int u = 0; u < vi->use_count && !s->esc_all; u++) {
    if (vi->uses[u].kind != SSA_USE_INSTR) {
      sccp_esc_add(s, lo, hi);
      continue;
    }
    IRQuadCompact *q = &ir->compact_instructions[vi->uses[u].idx];
    TccIrOp op = q->op;
    if (op == TCCIR_OP_NOP)
      continue;
    IROperand dest = irop_config[op].has_dest ? tcc_ir_op_get_dest(ir, q)
                                              : IROP_NONE;
    int roles[3] = { -1, -1, -1 };
    int nroles = 0;
    if (irop_config[op].has_src1) {
      IROperand o = tcc_ir_op_get_src1(ir, q);
      if (o.tag == IROP_TAG_VREG && !o.is_lval && irop_get_vreg(o) == vr)
        roles[nroles++] = 0;
    }
    if (irop_config[op].has_src2) {
      IROperand o = tcc_ir_op_get_src2(ir, q);
      if (o.tag == IROP_TAG_VREG && !o.is_lval && irop_get_vreg(o) == vr)
        roles[nroles++] = 1;
    }
    if (op == TCCIR_OP_MLA) {
      IROperand o = tcc_ir_op_get_accum(ir, q);
      if (o.tag == IROP_TAG_VREG && !o.is_lval && irop_get_vreg(o) == vr)
        roles[nroles++] = 1;
    }
    if (op == TCCIR_OP_STORE || op == TCCIR_OP_STORE_INDEXED ||
        op == TCCIR_OP_STORE_POSTINC) {
      if (dest.tag == IROP_TAG_VREG && irop_get_vreg(dest) == vr)
        roles[nroles++] = 2;
    }
    for (int r = 0; r < nroles; r++) {
      int act = sccp_esc_classify(op, roles[r], dest.is_lval);
      if (act == ESC_ESCAPE) {
        sccp_esc_add(s, lo, hi);
      } else if (act == ESC_FLOOD) {
        if (!irop_config[op].has_dest) {
          sccp_esc_add(s, lo, hi);
          continue;
        }
        sccp_esc_flood(s, irop_get_vreg(dest), lo, hi, visited, depth + 1);
      }
    }
  }
}

/* SCCP_OBJ_BOUND caps how far the containing object extends past the base. */
static int sccp_slot_addr_escapes(SCCPState *s, int load_lo, int load_hi)
{
  enum { SCCP_OBJ_BOUND = 4096 };
  TCCIRState *ir = s->ctx->ir;
  if (!s->esc_done) {
    s->esc_done = 1;
    uint8_t *visited = tcc_mallocz(s->ctx->vinfo_cap ? s->ctx->vinfo_cap : 1);
    for (int i = 0; i < ir->next_instruction_index && !s->esc_all; i++) {
      IRQuadCompact *q = &ir->compact_instructions[i];
      TccIrOp op = q->op;
      if (op == TCCIR_OP_NOP)
        continue;
      if (op == TCCIR_OP_INLINE_ASM || op == TCCIR_OP_ASM_INPUT ||
          op == TCCIR_OP_ASM_OUTPUT || op == TCCIR_OP_SETJMP ||
          op == TCCIR_OP_NL_SETJMP || op == TCCIR_OP_VLA_ALLOC ||
          op == TCCIR_OP_LEA) {
        s->esc_all = 1;
        break;
      }
      IROperand dest = irop_config[op].has_dest ? tcc_ir_op_get_dest(ir, q)
                                                : IROP_NONE;
      IROperand ops[3];
      int roles[3];
      int nops = 0;
      if (irop_config[op].has_src1) {
        ops[nops] = tcc_ir_op_get_src1(ir, q);
        roles[nops++] = 0;
      }
      if (irop_config[op].has_src2) {
        ops[nops] = tcc_ir_op_get_src2(ir, q);
        roles[nops++] = 1;
      }
      if (op == TCCIR_OP_MLA) {
        ops[nops] = tcc_ir_op_get_accum(ir, q);
        roles[nops++] = 1;
      }
      for (int k = 0; k < nops && !s->esc_all; k++) {
        IROperand o = ops[k];
        if (o.is_lval || !o.is_local)
          continue;
        if (o.tag != IROP_TAG_STACKOFF || irop_get_vreg(o) != -1) {
          /* spill-encoded &VAR or unknown local encoding — untrackable */
          s->esc_all = 1;
          break;
        }
        int lo = irop_get_stack_offset(o);
        int hi = lo + SCCP_OBJ_BOUND;
        int act = sccp_esc_classify(op, roles[k], dest.is_lval);
        if (act == ESC_ESCAPE) {
          sccp_esc_add(s, lo, hi);
        } else if (act == ESC_FLOOD) {
          if (!irop_config[op].has_dest) {
            sccp_esc_add(s, lo, hi);
          } else {
            /* Fresh visited set per seed: a temp shared by two seeds must record both. */
            memset(visited, 0, s->ctx->vinfo_cap ? s->ctx->vinfo_cap : 1);
            sccp_esc_flood(s, irop_get_vreg(dest), lo, hi, visited, 0);
          }
        }
      }
      if (!s->esc_all &&
          (op == TCCIR_OP_STORE_INDEXED || op == TCCIR_OP_STORE_POSTINC) &&
          !dest.is_lval && dest.is_local &&
          !(dest.tag == IROP_TAG_STACKOFF && irop_get_vreg(dest) == -1))
        s->esc_all = 1;
    }
    tcc_free(visited);
  }
  if (s->esc_all)
    return 1;
  for (int e = 0; e < s->esc_count; e++)
    if (s->esc[e].hi > load_lo && load_hi > s->esc[e].lo)
      return 1;
  return 0;
}

/* SCCP_TOP means the block start was reached with no matching or aliasing store. */
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
    if (sq->op == TCCIR_OP_FUNCCALLVOID || sq->op == TCCIR_OP_FUNCCALLVAL) {
      /* A call only clobbers slots whose address escaped to callees. */
      if (sccp_slot_addr_escapes(s, load_lo, load_hi))
        return SCCP_BOTTOM;
      continue;
    }
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
      if (target == soff && store_btype == load_btype) {
        int st2 = sccp_get_store_src_value(s, tcc_ir_op_get_src1(ir, sq),
                                            out, dep_pos);
        if (st2 == SCCP_CONST)
          return SCCP_CONST;
        return SCCP_BOTTOM;
      }
      int store_size = sccp_btype_bytes(store_btype);
      int store_lo = target;
      int store_hi = target + store_size;
      if (store_hi > load_lo && load_hi > store_lo)
        return SCCP_BOTTOM;
      continue;
    }
  }
  return SCCP_TOP;
}

/* Base array offset of an indexed store; INT_MIN when it isn't LEA-resolvable. */
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

/* Any store-to-load path is a subset of the linear IR range, so scanning it is sound. */
static int sccp_no_aliasing_between(SCCPState *s, int store_idx, int load_idx,
                                    int soff, int load_btype)
{
  TCCIRState *ir = s->ctx->ir;
  int load_size = sccp_btype_bytes(load_btype);
  int load_lo = soff;
  int load_hi = soff + load_size;
  /* Assumed extent of a stack array when an indexed write has an unresolved index. */
  const int LCS_INDEXED_MAX_ARRAY = 64;
  for (int i = store_idx + 1; i < load_idx; i++) {
    IRQuadCompact *q = &ir->compact_instructions[i];
    TccIrOp op = q->op;
    if (op == TCCIR_OP_NOP)
      continue;
    if (op == TCCIR_OP_FUNCCALLVOID || op == TCCIR_OP_FUNCCALLVAL) {
      /* A call only clobbers slots whose address escaped to callees. */
      if (sccp_slot_addr_escapes(s, load_lo, load_hi))
        return 0;
      continue;
    }
    if (op == TCCIR_OP_BLOCK_COPY)
      return 0;
    if (op != TCCIR_OP_STORE && op != TCCIR_OP_STORE_INDEXED &&
        op != TCCIR_OP_STORE_POSTINC)
      continue;
    int store_btype = 0;
    int target = sccp_store_target_off(s->ctx, q, &store_btype);
    if (target != INT_MIN) {
      int store_size = sccp_btype_bytes(store_btype);
      int store_lo = target;
      int store_hi = target + store_size;
      if (store_hi > load_lo && load_hi > store_lo)
        return 0;
      continue;
    }
    int base_off = sccp_store_indexed_base_off(s->ctx, q);
    if (base_off != INT_MIN) {
      int extent_lo = base_off;
      int extent_hi = base_off + LCS_INDEXED_MAX_ARRAY;
      if (extent_hi <= load_lo || extent_lo >= load_hi)
        continue;
      return 0;
    }
    /* Truly unknown memory write — could touch any stack slot. */
    return 0;
  }
  return 1;
}

/* Like sccp_store_indexed_base_off but also accepts a direct-STACKOFF base. */
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

/* Narrower than sccp_no_aliasing_between: calls are not barriers here. */
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
      /* A non-constant index still clobbers when the array's extent covers the slot. */
      if (q->op == TCCIR_OP_STORE_INDEXED || q->op == TCCIR_OP_STORE_POSTINC) {
        const int LCS_INDEXED_MAX_ARRAY = 64;
        int base_off = sccp_indexed_store_base_off(s->ctx, q);
        if (base_off == INT_MIN)
          return 1;
        int extent_lo = base_off;
        int extent_hi = base_off + LCS_INDEXED_MAX_ARRAY;
        if (extent_hi > load_lo && load_hi > extent_lo)
          return 1;
      }
      /* A pointer STORE with no concrete stack slot can write any address-taken slot. */
      else if (q->op == TCCIR_OP_STORE && sccp_store_may_escape(s->ctx, q)) {
        return 1;
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

/* A load in a loop whose body writes the slot is loop-carried, not constant. */
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
    for (int i = loop->start_idx; i <= loop->end_idx; i++) {
      IRQuadCompact *q = &ir->compact_instructions[i];
      TccIrOp op = q->op;
      if (op == TCCIR_OP_NOP)
        continue;
      if (op == TCCIR_OP_FUNCCALLVOID || op == TCCIR_OP_FUNCCALLVAL || op == TCCIR_OP_BLOCK_COPY)
        return 1;
      if (op != TCCIR_OP_STORE && op != TCCIR_OP_STORE_INDEXED && op != TCCIR_OP_STORE_POSTINC)
        continue;
      int store_btype = 0;
      int target = sccp_store_target_off(s->ctx, q, &store_btype);
      if (target != INT_MIN) {
        int store_lo = target, store_hi = target + sccp_btype_bytes(store_btype);
        if (store_hi > load_lo && load_hi > store_lo)
          return 1;
        continue;
      }
      return 1;
    }
  }
  return 0;
}

/* Returns 1 if a loop strictly between the store and the load writes the slot. */
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
    if (!(loop->start_idx > from_idx && loop->end_idx < to_idx))
      continue;
    for (int i = loop->start_idx; i <= loop->end_idx; i++) {
      IRQuadCompact *q = &ir->compact_instructions[i];
      TccIrOp op = q->op;
      if (op == TCCIR_OP_NOP)
        continue;
      /* Only a non-lval address param escapes the slot; an lval deref passes a value. */
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
            return 1;
        }
        continue;
      }
      /* Calls with no by-ref slot arg are left to the resolved-store check below. */
      if (op == TCCIR_OP_FUNCCALLVOID || op == TCCIR_OP_FUNCCALLVAL || op == TCCIR_OP_BLOCK_COPY)
        continue;
      if (op != TCCIR_OP_STORE && op != TCCIR_OP_STORE_INDEXED && op != TCCIR_OP_STORE_POSTINC)
        continue;
      int store_btype = 0;
      int target = sccp_store_target_off(s->ctx, q, &store_btype);
      if (target != INT_MIN) {
        int store_lo = target, store_hi = target + sccp_btype_bytes(store_btype);
        if (store_hi > load_lo && load_hi > store_lo)
          return 1;
        continue;
      }
      /* Unresolved indexed writes keep forwarding; opaque pointer derefs may alias. */
      if (op == TCCIR_OP_STORE || op == TCCIR_OP_STORE_POSTINC)
        return 1;
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

  if (sccp_loop_clobbers_slot(s, instr_idx, soff, load_btype))
    return SCCP_BOTTOM;

  int st = sccp_scan_block_for_stack_store(s, bb, instr_idx - 1, soff,
                                            load_btype, out, dep_pos);
  if (st != SCCP_TOP)
    return st;

  /* The dominator walk skips sibling blocks, so a match must also pass an alias check. */
  int dom = bb->idom;
  while (dom >= 0 && dom != block) {
    IRBasicBlock *db = &cfg->blocks[dom];
    int saved_dep = dep_pos ? *dep_pos : -1;
    int64_t saved_out = *out;
    int dst = sccp_scan_block_for_stack_store(s, db, db->end_idx - 1, soff,
                                               load_btype, out, dep_pos);
    if (dst == SCCP_CONST) {
      /* Re-scan to pinpoint the matching STORE's IR index for the alias check. */
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
      /* Unconditional: the alias scan below is skipped for entry-block stores. */
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

/* *dep_src_pos reports the TEMP a STORE-based resolution depends on, or -1. */
static int sccp_resolve_var(SCCPState *s, int32_t var_vreg, int instr_idx,
                            int64_t *out, int *dep_src_pos)
{
  TCCIRState *ir = s->ctx->ir;
  IRCFG *cfg = s->ctx->cfg;
  int block = cfg->instr_to_block[instr_idx];
  IRBasicBlock *bb = &cfg->blocks[block];
  int var_pos = TCCIR_DECODE_VREG_POSITION(var_vreg);
  *dep_src_pos = -1;

  /* A volatile VAR's load is a mandated memory access, never a constant. */
  if (var_pos < ir->variables_live_intervals_size &&
      ir->variables_live_intervals[var_pos].is_volatile)
    return SCCP_BOTTOM;

  for (int i = instr_idx - 1; i >= bb->start_idx; i--) {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;

    if (q->op == TCCIR_OP_FUNCCALLVOID || q->op == TCCIR_OP_FUNCCALLVAL)
      return SCCP_BOTTOM;

    if (irop_config[q->op].has_dest && q->op != TCCIR_OP_STORE &&
        q->op != TCCIR_OP_STORE_INDEXED && q->op != TCCIR_OP_STORE_POSTINC) {
      IROperand dest = tcc_ir_op_get_dest(ir, q);
      int32_t dv = irop_get_vreg(dest);
      if (dv >= 0 && TCCIR_DECODE_VREG_TYPE(dv) == TCCIR_VREG_TYPE_VAR &&
          TCCIR_DECODE_VREG_POSITION(dv) == var_pos) {
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

    /* dest.is_local separates a VAR-slot write from a pointer-deref through V. */
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

    if (q->op == TCCIR_OP_STORE) {
      IROperand dest = tcc_ir_op_get_dest(ir, q);
      if (dest.tag == IROP_TAG_STACKOFF && dest.is_local && dest.is_lval) {
        IRLiveInterval *vi = tcc_ir_vreg_live_interval(ir, var_vreg);
        /* VAR and anon-local offsets can collide, so addrtaken must gate the match. */
        if (vi && vi->addrtaken && vi->original_offset == irop_get_stack_offset(dest))
          return sccp_get_store_src_value(s, tcc_ir_op_get_src1(ir, q), out,
                                          dep_src_pos);
        continue;
      }

      int32_t addr_vr = irop_get_vreg(dest);
      if (addr_vr >= 0 && dest.is_lval) {
        /* Chase addr_vr through in-block ASSIGN/STORE chains to see if it is &V. */
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

        /* Never walk past an unknown pointer store: it may alias this VAR. */
        return SCCP_BOTTOM;
      }
    }
  }

  return SCCP_BOTTOM;
}

/* Handles TEMPs via the lattice and VARs via an in-block backward scan. */
static int sccp_get_operand_value_ex(SCCPState *s, IROperand op,
                                     int instr_idx, int64_t *out)
{
  if (irop_is_immediate(op)) {
    *out = irop_get_imm64_ex(s->ctx->ir, op);
    return SCCP_CONST;
  }

  /* *V is not a slot read of V, so it must return BOTTOM, never resolve_var. */
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
    /* INT_MIN / -1 traps on hardware divide: never fold it. */
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

/* A CONST phi with a TOP operand on an executable edge must widen to BOTTOM. */
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

  if (idx >= cfg->num_instrs)   /* instr appended after CFG build: no block map */
    return;
  int block = cfg->instr_to_block[idx];
  if (!s->block_reachable[block])
    return;

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

    if (dest.btype == IROP_BTYPE_FLOAT32 || dest.btype == IROP_BTYPE_FLOAT64) {
      if (sccp_set_bottom(dest_cell))
        sccp_add_ssa(s, TCCIR_DECODE_VREG_POSITION(dest_vr));
      goto handle_control_flow;
    }

    /* A fused barrel shift is invisible in the operands: never evaluate it. */
    if (tcc_ir_barrel_shift_at(ir, q)) {
      if (sccp_set_bottom(dest_cell))
        sccp_add_ssa(s, TCCIR_DECODE_VREG_POSITION(dest_vr));
      goto handle_control_flow;
    }

    int is_64 = (dest.btype == IROP_BTYPE_INT64);

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

    if (sccp_set_bottom(dest_cell))
      sccp_add_ssa(s, TCCIR_DECODE_VREG_POSITION(dest_vr));
  }

handle_control_flow:
  /* Forward to the following JUMPIF so branch edges re-evaluate on operand change. */
  if (q->op == TCCIR_OP_CMP || q->op == TCCIR_OP_TEST_ZERO) {
    int ni = idx + 1;
    while (ni < ir->next_instruction_index &&
           ir->compact_instructions[ni].op == TCCIR_OP_NOP)
      ni++;
    if (ni < ir->next_instruction_index &&
        ir->compact_instructions[ni].op == TCCIR_OP_JUMPIF)
      sccp_visit_instr(s, ni);
  }
  if (q->op == TCCIR_OP_JUMP) {
    IROperand dest = tcc_ir_op_get_dest(ir, q);
    int target = dest.u.imm32;
    int target_block = (target >= 0 && target < cfg->num_instrs) ?
                        cfg->instr_to_block[target] : -1;
    sccp_add_cfg_edge(s, block, target_block);
  }
  else if (q->op == TCCIR_OP_JUMPIF) {
    int ci = idx - 1;
    while (ci >= 0 && ir->compact_instructions[ci].op == TCCIR_OP_NOP)
      ci--;

    IROperand jmp_dest = tcc_ir_op_get_dest(ir, q);
    int target = jmp_dest.u.imm32;
    int target_block = (target >= 0 && target < cfg->num_instrs) ?
                        cfg->instr_to_block[target] : -1;
    IRBasicBlock *bb = &cfg->blocks[block];
    int fall_block = -1;
    for (int si = 0; si < bb->num_succs; si++) {
      if (bb->succs[si] != target_block) {
        fall_block = bb->succs[si];
        break;
      }
    }
    /* Taken target equal to the fall-through must not resolve to block -1. */
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
          /* Truncate to operand width to avoid sign-extension mismatches. */
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

  if (ssa->block_phis) {
    for (IRPhiNode *phi = ssa->block_phis[succ]; phi; phi = phi->next)
      sccp_visit_phi(s, phi, succ);
  }

  if (first_visit) {
    IRBasicBlock *bb = &cfg->blocks[succ];
    for (int i = bb->start_idx; i < bb->end_idx; i++)
      sccp_visit_instr(s, i);

    /* An empty or all-NOP block still falls through to its successors. */
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

/* Idempotent: a rewritten operand no longer matches old_vr. */
static void sccp_set_instr_operands_imm(IRSSAOptCtx *ctx, int idx, int32_t old_vr, IROperand imm)
{
  TCCIRState *ir = ctx->ir;
  IRQuadCompact *q = &ir->compact_instructions[idx];
  if (irop_config[q->op].has_src1 && irop_get_vreg(tcc_ir_op_get_src1(ir, q)) == old_vr)
    tcc_ir_set_src1(ir, idx, imm);
  if (irop_config[q->op].has_src2 && irop_get_vreg(tcc_ir_op_get_src2(ir, q)) == old_vr)
    tcc_ir_set_src2(ir, idx, imm);
}

/* Drops the SSA_USE_PHI entries for this (block, slot) so dead defs can be DCE'd. */
static void sccp_drop_phi_operand_uses(IRSSAOptCtx *ctx, int block, IRPhiNode *phi)
{
  for (int i = 0; i < phi->num_operands; i++) {
    IRSSAVregInfo *vi = ssa_opt_vinfo(ctx, phi->operands[i].vreg);
    if (!vi)
      continue;
    for (int u = 0; u < vi->use_count; u++) {
      if (vi->uses[u].kind == SSA_USE_PHI && vi->uses[u].idx == block &&
          vi->uses[u].slot == i) {
        vi->uses[u] = vi->uses[--vi->use_count];
        break;
      }
    }
  }
}

/* A CONST phi has no defining instruction, so Phase 1 cannot rewrite it. */
static int sccp_materialize_const_phis(SCCPState *s)
{
  IRSSAOptCtx *ctx = s->ctx;
  IRSSAState *ssa = ctx->ssa;
  TCCIRState *ir = ctx->ir;
  if (!ssa || !ssa->block_phis)
    return 0;

  int changes = 0;
  for (int b = 0; b < s->num_blocks; b++) {
    IRPhiNode **pp = &ssa->block_phis[b];
    while (*pp) {
      IRPhiNode *phi = *pp;
      int32_t dvr = phi->dest_vreg;
      int pos = (dvr >= 0 && TCCIR_DECODE_VREG_TYPE(dvr) == TCCIR_VREG_TYPE_TEMP)
                    ? TCCIR_DECODE_VREG_POSITION(dvr)
                    : -1;
      IRSSAVregInfo *dvi = (pos >= 0) ? ssa_opt_vinfo(ctx, dvr) : NULL;
      if (pos < 0 || pos >= s->cells_cap || s->cells[pos].state != SCCP_CONST || !dvi) {
        pp = &phi->next;
        continue;
      }

      /* Every use must read the dest as an immediate-capable src1/src2 operand. */
      int ok = 1;
      for (int u = 0; u < dvi->use_count; u++) {
        IRSSAUse use = dvi->uses[u];
        if (use.kind != SSA_USE_INSTR) { ok = 0; break; }
        IRQuadCompact *uq = &ir->compact_instructions[use.idx];
        if (tcc_ir_barrel_shift_at(ir, uq)) { ok = 0; break; }
        int in_src = 0;
        if (irop_config[uq->op].has_src1 &&
            irop_get_vreg(tcc_ir_op_get_src1(ir, uq)) == dvr)
          in_src = 1;
        if (irop_config[uq->op].has_src2 &&
            irop_get_vreg(tcc_ir_op_get_src2(ir, uq)) == dvr)
          in_src = 1;
        if (!in_src) { ok = 0; break; }   /* dest/accum/other position */
      }
      if (!ok) {
        pp = &phi->next;
        continue;
      }

      int64_t val = s->cells[pos].value;
      IROperand imm;
      if (val == (int64_t)(int32_t)val)
        imm = irop_make_imm32(0, (int32_t)val, phi->btype);
      else
        imm = irop_make_i64(0, tcc_ir_pool_add_i64(ir, val), phi->btype);

      while (dvi->use_count > 0) {
        IRSSAUse use = dvi->uses[--dvi->use_count];
        sccp_set_instr_operands_imm(ctx, use.idx, dvr, imm);
      }
      sccp_drop_phi_operand_uses(ctx, b, phi);
      if (dvi->def_phi_block == b)
        dvi->def_phi_block = -1;
      *pp = phi->next;
      tcc_free(phi->operands);
      tcc_free(phi);
      changes++;
    }
  }
  return changes;
}

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

  /* Phase 0: phi-defined constants; Phase 1 only handles instruction defs. */
  changes += sccp_materialize_const_phis(s);

  /* Phase 1: constant-valued instructions become ASSIGN #const. */
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

  /* Phase 1.5: CMP/TEST_ZERO operands resolved via stack-load or VAR scanning. */
  IRCFG *cfg = s->ctx->cfg;
  for (int i = 0; i < ir->next_instruction_index; i++) {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_CMP && q->op != TCCIR_OP_TEST_ZERO)
      continue;
    int n_srcs = (q->op == TCCIR_OP_CMP) ? 2 : 1;
    if (i >= cfg->num_instrs)   /* instr appended after CFG build: no block map */
      continue;
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

      int32_t src_vr = irop_get_vreg(src);
      int src_is_direct_var = (src.tag == IROP_TAG_VREG && !src.is_lval &&
                               src_vr >= 0 &&
                               TCCIR_DECODE_VREG_TYPE(src_vr) == TCCIR_VREG_TYPE_VAR);

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
        }
      }

      /* Case C: VAR operand, as a stack-backed lvalue or a direct VAR vreg. */
      if (!got) {
        int32_t svr = src_vr;
        int src_is_stack_var = (src.tag == IROP_TAG_STACKOFF && src.is_lval &&
                                src.is_local && !src.is_llocal &&
                                svr >= 0 &&
                                TCCIR_DECODE_VREG_TYPE(svr) == TCCIR_VREG_TYPE_VAR);
        int src_var_pos = (svr >= 0) ? TCCIR_DECODE_VREG_POSITION(svr) : -1;
        int src_var_volatile = (src_var_pos >= 0 &&
                                src_var_pos < ir->variables_live_intervals_size &&
                                ir->variables_live_intervals[src_var_pos].is_volatile);
        if ((src_is_direct_var || src_is_stack_var) && !src_var_volatile) {
          int var_pos = TCCIR_DECODE_VREG_POSITION(src_vr);
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
          /* Keep a still-read VAR alive when the constant would need a pool load. */
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
      /* Drop the old use entry so cascading DCE can fire. */
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

  /* Branch folding and block removal stay out: they invalidate CFG and SSA state. */

  return changes;
}

int ssa_opt_sccp(IRSSAOptCtx *ctx)
{
  IRCFG *cfg = ctx->cfg;
  IRSSAState *ssa = ctx->ssa;
  if (!cfg || cfg->num_blocks == 0 || !ssa)
    return 0;

  int nb = cfg->num_blocks;
  int ntmp = ctx->vinfo_cap;

  /* Edge matrix is quadratic in block count. */
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

  /* Entry definitions have no def site; they must start BOTTOM, not TOP. */
  for (int pos = 0; pos < ntmp; pos++) {
    IRSSAVregInfo *vi = &ctx->vinfo[pos];
    if (vi->def_instr < 0 && vi->def_phi_block < 0)
      s.cells[pos].state = SCCP_BOTTOM;
  }

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

  /* Worklist propagation has gaps, so a monotone re-sweep runs after each drain. */
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
            if (use->idx >= cfg->num_instrs)   /* appended after CFG build: no block map */
              continue;
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

        /* LOADs resolved through a STORE source TEMP need explicit re-evaluation. */
        for (int m = 0; m < s.mem_dep_count; m++) {
          if (s.mem_deps[m].src_pos == pos) {
            int li = s.mem_deps[m].load_idx;
            if (li >= cfg->num_instrs)   /* appended after CFG build: no block map */
              continue;
            int blk = cfg->instr_to_block[li];
            if (s.block_reachable[blk])
              sccp_visit_instr(&s, li);
          }
        }
      }
    }

    /* Empty worklists after this sweep mean a sound fixpoint. */
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
  tcc_free(s.esc);
  if (s.loops)
    tcc_ir_free_loops(s.loops);

  return changes;
}
