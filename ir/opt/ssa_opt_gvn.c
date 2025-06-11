/*
 *  TCC IR - SSA Global Value Numbering (GVN)
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
 * Dominator-Tree Global Value Numbering
 *
 * Walk the dominator tree in preorder. At each block, hash each pure
 * instruction by (opcode, src1, src2). If a matching hash exists from a
 * dominating block, convert the redundant instruction to ASSIGN dest = result.
 * The cprop pass will then propagate the copy in the next driver iteration.
 *
 * This avoids calling replace_all_uses which can extend live ranges and
 * corrupt phi resolution.
 * ============================================================================ */

#define GVN_HASH_SIZE 256

typedef struct GVNEntry {
  int op;
  int32_t src1;
  int32_t src2;
  int32_t src3;  /* 3rd operand (MLA accumulator); 0 for non-MLA */
  int32_t imm1;
  int32_t imm2;
  int32_t imm3;
  uint8_t s1_tag;
  uint8_t s2_tag;
  uint8_t s3_tag;
  int32_t result_vr;
  struct GVNEntry *next;
} GVNEntry;

static int gvn_is_pure_alu(int op)
{
  switch (op) {
  case TCCIR_OP_ADD:
  case TCCIR_OP_SUB:
  case TCCIR_OP_MUL:
  case TCCIR_OP_MLA: /* dest = src1 * src2 + accum (also pure, has 3rd operand) */
  case TCCIR_OP_AND:
  case TCCIR_OP_OR:
  case TCCIR_OP_XOR:
  case TCCIR_OP_SHL:
  case TCCIR_OP_SHR:
  case TCCIR_OP_SAR:
  case TCCIR_OP_ROR:
  case TCCIR_OP_BOOL_AND:
  case TCCIR_OP_BOOL_OR:
    return 1;
  default:
    return 0;
  }
}

static int gvn_is_commutative(int op)
{
  switch (op) {
  case TCCIR_OP_ADD:
  case TCCIR_OP_MUL:
  case TCCIR_OP_AND:
  case TCCIR_OP_OR:
  case TCCIR_OP_XOR:
  case TCCIR_OP_BOOL_AND:
  case TCCIR_OP_BOOL_OR:
    return 1;
  default:
    return 0;
  }
}

static uint32_t gvn_hash(int op, uint8_t s1_tag, int32_t s1, int32_t imm1,
                          uint8_t s2_tag, int32_t s2, int32_t imm2,
                          uint8_t s3_tag, int32_t s3, int32_t imm3)
{
  uint32_t h = (uint32_t)op * 2654435761u;
  h ^= ((uint32_t)s1_tag << 28) ^ (uint32_t)s1 * 2246822519u;
  h ^= (uint32_t)imm1 * 3266489917u;
  h ^= ((uint32_t)s2_tag << 28) ^ (uint32_t)s2 * 374761393u;
  h ^= (uint32_t)imm2 * 668265263u;
  h ^= ((uint32_t)s3_tag << 28) ^ (uint32_t)s3 * 1597334677u;
  h ^= (uint32_t)imm3 * 1442695041u;
  return h & (GVN_HASH_SIZE - 1);
}

static void gvn_operand_key(IROperand op, uint8_t *tag, int32_t *vr, int32_t *imm)
{
  *tag = op.tag;
  *vr = irop_get_vreg(op);
  if (op.tag == IROP_TAG_IMM32 || op.tag == IROP_TAG_F32)
    *imm = op.u.imm32;
  else if (op.tag == IROP_TAG_STACKOFF)
    *imm = op.u.imm32;
  else if (op.tag == IROP_TAG_SYMREF || op.tag == IROP_TAG_I64 || op.tag == IROP_TAG_F64)
    *imm = (int32_t)op.u.pool_idx;
  else
    *imm = 0;
}

static GVNEntry *gvn_find(GVNEntry **table, int op, uint8_t s1_tag, int32_t s1,
                           int32_t imm1, uint8_t s2_tag, int32_t s2, int32_t imm2,
                           uint8_t s3_tag, int32_t s3, int32_t imm3)
{
  uint32_t h = gvn_hash(op, s1_tag, s1, imm1, s2_tag, s2, imm2, s3_tag, s3, imm3);
  for (GVNEntry *e = table[h]; e; e = e->next) {
    if (e->op == op && e->s1_tag == s1_tag && e->src1 == s1 && e->imm1 == imm1 &&
        e->s2_tag == s2_tag && e->src2 == s2 && e->imm2 == imm2 &&
        e->s3_tag == s3_tag && e->src3 == s3 && e->imm3 == imm3)
      return e;
  }
  return NULL;
}

/* Scope tracking: record entries added per domtree level so we can pop them */
typedef struct { GVNEntry **slot; GVNEntry *prev; } GVNScopeUndo;

static GVNScopeUndo *undo_stack;
static int undo_count;
static int undo_cap;

static void gvn_scope_push(GVNEntry **table, GVNEntry *entry)
{
  uint32_t h = gvn_hash(entry->op, entry->s1_tag, entry->src1, entry->imm1,
                         entry->s2_tag, entry->src2, entry->imm2,
                         entry->s3_tag, entry->src3, entry->imm3);
  if (undo_count >= undo_cap) {
    int nc = undo_cap ? undo_cap * 2 : 64;
    undo_stack = tcc_realloc(undo_stack, nc * sizeof(GVNScopeUndo));
    undo_cap = nc;
  }
  undo_stack[undo_count++] = (GVNScopeUndo){ &table[h], table[h] };
  entry->next = table[h];
  table[h] = entry;
}

static void gvn_scope_pop_to(int saved_count)
{
  while (undo_count > saved_count) {
    GVNScopeUndo *u = &undo_stack[--undo_count];
    *u->slot = u->prev;
  }
}

static GVNEntry *entry_pool;
static int pool_count;
static int pool_cap;

static GVNEntry *gvn_alloc_entry(void)
{
  if (pool_count >= pool_cap)
    return NULL;
  GVNEntry *e = &entry_pool[pool_count++];
  memset(e, 0, sizeof(*e));
  return e;
}

/* PARAM mutation bitmap.  A PARAM that is the dest of any STORE/ASSIGN-write
 * is not safe to use as a GVN hash key — its value changes mid-function.
 * Built once per function in ssa_opt_gvn().  Indexed by PARAM position. */
static uint8_t *param_mutated;
static int param_mutated_cap;

static int gvn_param_is_stable(int32_t vreg)
{
  if (vreg < 0)
    return 0;
  if (TCCIR_DECODE_VREG_TYPE(vreg) != TCCIR_VREG_TYPE_PARAM)
    return 0;
  int pos = TCCIR_DECODE_VREG_POSITION(vreg);
  if (pos >= param_mutated_cap)
    return 0;
  return !(param_mutated[pos / 8] & (1u << (pos % 8)));
}

static int gvn_process_block(IRSSAOptCtx *ctx, IRCFG *cfg, GVNEntry **table, int b)
{
  TCCIRState *ir = ctx->ir;
  IRBasicBlock *bb = &cfg->blocks[b];
  int changes = 0;
  int saved_undo = undo_count;

  for (int i = bb->start_idx; i < bb->end_idx; i++) {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;
    if (!gvn_is_pure_alu(q->op))
      continue;
    if (!irop_config[q->op].has_src1 || !irop_config[q->op].has_src2)
      continue;

    /* A barrel-shift side-table annotation (ir->barrel_shifts[orig_index],
     * set by tcc_ir_barrel_shift_fusion just before regalloc) folds a
     * single-use shift into this ALU op's src2 *without* changing the IR
     * operands — e.g. `t = crc SAR 8; r = t & 0xff` becomes `r = crc & 0xff`
     * with barrel_shifts[r]=SAR8.  GVN keys only on (op, operands), so such an
     * op looks identical to a genuinely-unshifted `crc & 0xff` and would be
     * wrongly merged with it (both CRC bytes end up the high byte).  The
     * annotation is part of the op's identity but not visible to the GVN key,
     * so exclude any shift-annotated op from value numbering. */
    if (ir->barrel_shifts && q->orig_index >= 0 &&
        q->orig_index <= ir->max_orig_index &&
        ir->barrel_shifts[q->orig_index])
      continue;

    IROperand dest = tcc_ir_op_get_dest(ir, q);
    int32_t dest_vr = irop_get_vreg(dest);
    if (dest_vr < 0 || TCCIR_DECODE_VREG_TYPE(dest_vr) != TCCIR_VREG_TYPE_TEMP)
      continue;

    IRSSAVregInfo *vi = ssa_opt_vinfo(ctx, dest_vr);
    if (vi && vi->def_count > 1)
      continue;

    IROperand src1 = tcc_ir_op_get_src1(ir, q);
    IROperand src2 = tcc_ir_op_get_src2(ir, q);

    if (src1.is_lval || src1.is_local || src1.is_llocal)
      continue;
    if (src2.is_lval || src2.is_local || src2.is_llocal)
      continue;

    int32_t s1v = irop_get_vreg(src1);
    int32_t s2v = irop_get_vreg(src2);
    if (s1v >= 0) {
      int t = TCCIR_DECODE_VREG_TYPE(s1v);
      if (t == TCCIR_VREG_TYPE_TEMP) {
        IRSSAVregInfo *s1vi = ssa_opt_vinfo(ctx, s1v);
        if (!s1vi || s1vi->def_count > 1)
          continue;
        if (s1vi->def_phi_block >= 0)
          continue;
      } else if (t == TCCIR_VREG_TYPE_PARAM) {
        if (!gvn_param_is_stable(s1v))
          continue;
      } else {
        continue;
      }
    }
    if (s2v >= 0) {
      int t = TCCIR_DECODE_VREG_TYPE(s2v);
      if (t == TCCIR_VREG_TYPE_TEMP) {
        IRSSAVregInfo *s2vi = ssa_opt_vinfo(ctx, s2v);
        if (!s2vi || s2vi->def_count > 1)
          continue;
        if (s2vi->def_phi_block >= 0)
          continue;
      } else if (t == TCCIR_VREG_TYPE_PARAM) {
        if (!gvn_param_is_stable(s2v))
          continue;
      } else {
        continue;
      }
    }

    /* MLA has a 3rd operand (accumulator) at pool[operand_base+3]. */
    IROperand accum = IROP_NONE;
    int32_t s3v = -1;
    int is_mla = (q->op == TCCIR_OP_MLA);
    if (is_mla) {
      accum = tcc_ir_op_get_accum(ir, q);
      if (accum.is_lval || accum.is_local || accum.is_llocal)
        continue;
      s3v = irop_get_vreg(accum);
      if (s3v >= 0) {
        int t = TCCIR_DECODE_VREG_TYPE(s3v);
        if (t == TCCIR_VREG_TYPE_TEMP) {
          IRSSAVregInfo *s3vi = ssa_opt_vinfo(ctx, s3v);
          if (!s3vi || s3vi->def_count > 1)
            continue;
          if (s3vi->def_phi_block >= 0)
            continue;
        } else if (t == TCCIR_VREG_TYPE_PARAM) {
          if (!gvn_param_is_stable(s3v))
            continue;
        } else {
          continue;
        }
      }
    }

    uint8_t s1_tag, s2_tag, s3_tag = 0;
    int32_t s1_vr, s2_vr, s1_imm, s2_imm;
    int32_t s3_vr = 0, s3_imm = 0;
    gvn_operand_key(src1, &s1_tag, &s1_vr, &s1_imm);
    gvn_operand_key(src2, &s2_tag, &s2_vr, &s2_imm);
    if (is_mla)
      gvn_operand_key(accum, &s3_tag, &s3_vr, &s3_imm);

    GVNEntry *existing = gvn_find(table, q->op, s1_tag, s1_vr, s1_imm,
                                   s2_tag, s2_vr, s2_imm, s3_tag, s3_vr, s3_imm);
    if (!existing && gvn_is_commutative(q->op))
      existing = gvn_find(table, q->op, s2_tag, s2_vr, s2_imm,
                           s1_tag, s1_vr, s1_imm, s3_tag, s3_vr, s3_imm);
    /* MLA: src1 * src2 is commutative — also try swapped src1<->src2. */
    if (!existing && is_mla)
      existing = gvn_find(table, q->op, s2_tag, s2_vr, s2_imm,
                           s1_tag, s1_vr, s1_imm, s3_tag, s3_vr, s3_imm);

    if (existing) {
      /* Convert to ASSIGN copy instead of replace_all_uses.
       * cprop will propagate the copy in the next iteration. */
      IROperand new_src;
      new_src = dest;
      new_src.vr = existing->result_vr;
      new_src.tag = IROP_TAG_VREG;
      new_src.is_lval = 0;
      new_src.is_local = 0;
      new_src.is_llocal = 0;
      new_src.u.imm32 = 0;

      /* Update use-def: remove uses of old operands */
      IRSSAVregInfo *s1vi = ssa_opt_vinfo(ctx, s1v);
      if (s1vi) ssa_opt_remove_use_instr(s1vi, i);
      IRSSAVregInfo *s2vi = ssa_opt_vinfo(ctx, s2v);
      if (s2vi) ssa_opt_remove_use_instr(s2vi, i);
      if (is_mla && s3v >= 0) {
        IRSSAVregInfo *s3vi = ssa_opt_vinfo(ctx, s3v);
        if (s3vi) ssa_opt_remove_use_instr(s3vi, i);
      }

      /* Add use of the result vreg */
      IRSSAVregInfo *rvi = ssa_opt_vinfo(ctx, existing->result_vr);
      if (rvi) ssa_opt_add_use_instr(rvi, i);

      q->op = TCCIR_OP_ASSIGN;
      tcc_ir_set_src1(ir, i, new_src);
      tcc_ir_set_src2(ir, i, IROP_NONE);
      changes++;
      continue;
    }

    GVNEntry *e = gvn_alloc_entry();
    if (!e) continue;
    e->op = q->op;
    e->s1_tag = s1_tag;
    e->src1 = s1_vr;
    e->imm1 = s1_imm;
    e->s2_tag = s2_tag;
    e->src2 = s2_vr;
    e->imm2 = s2_imm;
    e->s3_tag = s3_tag;
    e->src3 = s3_vr;
    e->imm3 = s3_imm;
    e->result_vr = dest_vr;
    gvn_scope_push(table, e);
  }

  /* Recurse into dominator-tree children — entries from this block
   * remain visible (the dominator guarantees availability). */
  for (int ci = 0; ci < bb->num_dom_children; ci++)
    changes += gvn_process_block(ctx, cfg, table, bb->dom_children[ci]);

  /* Restore hash table to state before this block */
  gvn_scope_pop_to(saved_undo);

  return changes;
}

/* Mark a PARAM as mutated.  A PARAM is mutated if any instruction other than
 * its single function-entry definition writes to it.  Conservatively any
 * STORE/ASSIGN with a PARAM-typed dest counts.
 *
 * Nested functions can mutate the enclosing function's PARAMs through the
 * static chain — those writes never appear in this function's IR.  When
 * SET_CHAIN / INIT_CHAIN_SLOT is present, treat all PARAMs as mutated. */
static void gvn_param_scan(TCCIRState *ir)
{
  int np = ir->next_parameter;
  if (np <= 0) {
    param_mutated_cap = 0;
    return;
  }
  param_mutated_cap = np;
  int bytes = (np + 7) / 8;
  param_mutated = tcc_mallocz(bytes);

  int has_chain = 0;
  for (int i = 0; i < ir->next_instruction_index; i++) {
    int op = ir->compact_instructions[i].op;
    if (op == TCCIR_OP_SET_CHAIN || op == TCCIR_OP_INIT_CHAIN_SLOT) {
      has_chain = 1;
      break;
    }
  }
  if (has_chain) {
    for (int i = 0; i < bytes; i++)
      param_mutated[i] = 0xFF;
    return;
  }

  for (int i = 0; i < ir->next_instruction_index; i++) {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;
    if (!irop_config[q->op].has_dest)
      continue;
    IROperand d = tcc_ir_op_get_dest(ir, q);
    int32_t vr = irop_get_vreg(d);
    if (vr < 0)
      continue;
    if (TCCIR_DECODE_VREG_TYPE(vr) != TCCIR_VREG_TYPE_PARAM)
      continue;
    int pos = TCCIR_DECODE_VREG_POSITION(vr);
    if (pos < np)
      param_mutated[pos / 8] |= (uint8_t)(1u << (pos % 8));
  }
}

int ssa_opt_gvn(IRSSAOptCtx *ctx)
{
  IRCFG *cfg = ctx->cfg;
  if (!cfg || cfg->num_blocks == 0)
    return 0;

  int n = ctx->ir->next_instruction_index;

  GVNEntry *table[GVN_HASH_SIZE];
  memset(table, 0, sizeof(table));

  undo_stack = NULL;
  undo_count = 0;
  undo_cap = 0;
  pool_count = 0;
  pool_cap = n;
  entry_pool = tcc_mallocz(n * sizeof(GVNEntry));

  param_mutated = NULL;
  param_mutated_cap = 0;
  gvn_param_scan(ctx->ir);

  int changes = gvn_process_block(ctx, cfg, table, 0);

  tcc_free(undo_stack);
  undo_stack = NULL;
  tcc_free(entry_pool);
  entry_pool = NULL;
  tcc_free(param_mutated);
  param_mutated = NULL;
  param_mutated_cap = 0;

  return changes;
}
