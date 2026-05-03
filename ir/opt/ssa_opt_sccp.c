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
     * STORE dests always have is_lval=1; distinguish from pointer-deref
     * stores by checking the vreg type (VAR vs TEMP). */
    if (q->op == TCCIR_OP_STORE) {
      IROperand dest = tcc_ir_op_get_dest(ir, q);
      int32_t dv = irop_get_vreg(dest);
      if (dv >= 0 &&
          TCCIR_DECODE_VREG_TYPE(dv) == TCCIR_VREG_TYPE_VAR &&
          TCCIR_DECODE_VREG_POSITION(dv) == var_pos) {
        return sccp_get_store_src_value(s, tcc_ir_op_get_src1(ir, q), out,
                                        dep_src_pos);
      }
    }

    /* STORE through pointer: *T = val where T ultimately resolves to &V */
    if (q->op == TCCIR_OP_STORE) {
      IROperand dest = tcc_ir_op_get_dest(ir, q);
      if (dest.tag == IROP_TAG_STACKOFF && dest.is_local && dest.is_lval) {
        IRLiveInterval *vi = tcc_ir_vreg_live_interval(ir, var_vreg);
        if (vi && vi->original_offset == irop_get_stack_offset(dest))
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
    *result = v1 / v2; break;
  case TCCIR_OP_UDIV:
    if (v2 == 0) return 0;
    if (is_64)
      *result = (int64_t)((uint64_t)v1 / (uint64_t)v2);
    else
      *result = (int64_t)((uint32_t)(int32_t)v1 / (uint32_t)(int32_t)v2);
    break;
  case TCCIR_OP_IMOD:
    if (v2 == 0) return 0;
    *result = v1 % v2; break;
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
       * same offset within this block. */
      if (src.tag == IROP_TAG_STACKOFF && src.is_lval && src.is_local &&
          !src.is_llocal && (svr < 0 || TCCIR_DECODE_VREG_TYPE(svr) != TCCIR_VREG_TYPE_VAR)) {
        int soff = irop_get_stack_offset(src);
        int load_btype = irop_get_btype(src);
        int resolved = 0;
        int64_t sval = 0;
        int dep_pos = -1;
        IRBasicBlock *bb = &cfg->blocks[block];
        for (int si = idx - 1; si >= bb->start_idx; si--) {
          IRQuadCompact *sq = &ir->compact_instructions[si];
          if (sq->op == TCCIR_OP_NOP)
            continue;
          if (sq->op == TCCIR_OP_FUNCCALLVOID || sq->op == TCCIR_OP_FUNCCALLVAL)
            break;
          if (sq->op == TCCIR_OP_STORE_INDEXED || sq->op == TCCIR_OP_STORE_POSTINC)
            break;
          if (sq->op == TCCIR_OP_STORE) {
            IROperand sd = tcc_ir_op_get_dest(ir, sq);
            if (sd.tag == IROP_TAG_STACKOFF && sd.is_lval && sd.is_local &&
                irop_get_stack_offset(sd) == soff && irop_get_btype(sd) == load_btype) {
              int st2 = sccp_get_store_src_value(s, tcc_ir_op_get_src1(ir, sq),
                                                  &sval, &dep_pos);
              if (st2 == SCCP_CONST)
                resolved = 1;
              break;
            }
            if (sd.is_lval && sd.tag != IROP_TAG_STACKOFF)
              break;
          }
        }
        /* Walk up dominator tree if not found in current block */
        if (!resolved) {
          int dom = bb->idom;
          while (dom >= 0 && dom != block) {
            IRBasicBlock *db = &cfg->blocks[dom];
            int stop = 0;
            for (int si = db->end_idx - 1; si >= db->start_idx; si--) {
              IRQuadCompact *sq = &ir->compact_instructions[si];
              if (sq->op == TCCIR_OP_NOP)
                continue;
              if (sq->op == TCCIR_OP_FUNCCALLVOID ||
                  sq->op == TCCIR_OP_FUNCCALLVAL) {
                stop = 1;
                break;
              }
              if (sq->op == TCCIR_OP_STORE_INDEXED ||
                  sq->op == TCCIR_OP_STORE_POSTINC) {
                stop = 1;
                break;
              }
              if (sq->op == TCCIR_OP_STORE) {
                IROperand sd = tcc_ir_op_get_dest(ir, sq);
                if (sd.tag == IROP_TAG_STACKOFF && sd.is_lval && sd.is_local &&
                    irop_get_stack_offset(sd) == soff &&
                    irop_get_btype(sd) == load_btype) {
                  int st2 = sccp_get_store_src_value(
                      s, tcc_ir_op_get_src1(ir, sq), &sval, &dep_pos);
                  if (st2 == SCCP_CONST)
                    resolved = 1;
                  stop = 1;
                  break;
                }
                if (sd.is_lval && sd.tag != IROP_TAG_STACKOFF) {
                  stop = 1;
                  break;
                }
              }
            }
            if (stop || resolved)
              break;
            if (dom == db->idom)
              break;
            dom = db->idom;
          }
        }
        if (resolved) {
          int changed = sccp_meet(dest_cell, sval);
          if (dep_pos >= 0)
            sccp_add_mem_dep(s, dep_pos, idx);
          if (changed)
            sccp_add_ssa(s, TCCIR_DECODE_VREG_POSITION(dest_vr));
          goto handle_control_flow;
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

    /* Mark fallthrough edge (if block doesn't end with jump) */
    if (bb->end_idx > bb->start_idx) {
      int li = bb->end_idx - 1;
      while (li >= bb->start_idx && s->ctx->ir->compact_instructions[li].op == TCCIR_OP_NOP)
        li--;
      if (li >= bb->start_idx) {
        IRQuadCompact *last = &s->ctx->ir->compact_instructions[li];
        if (last->op != TCCIR_OP_JUMP && last->op != TCCIR_OP_JUMPIF &&
            last->op != TCCIR_OP_IJUMP && last->op != TCCIR_OP_RETURNVALUE &&
            last->op != TCCIR_OP_RETURNVOID && last->op != TCCIR_OP_SWITCH_TABLE) {
          for (int si = 0; si < bb->num_succs; si++)
            sccp_add_cfg_edge(s, succ, bb->succs[si]);
        }
      }
    }
  }
}

/* ============================================================================
 * Apply SCCP results: replace constants, fold branches, NOP dead code
 * ============================================================================ */

static int sccp_apply(SCCPState *s)
{
  TCCIRState *ir = s->ctx->ir;
  int changes = 0;

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
   * Guard against non-convergence (e.g., memory dependency cycles). */
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

  int changes = sccp_apply(&s);

  tcc_free(s.cells);
  tcc_free(s.block_reachable);
  tcc_free(s.edge_exec);
  tcc_free(s.cfg_wl);
  tcc_free(s.ssa_wl);
  tcc_free(s.mem_deps);

  return changes;
}
