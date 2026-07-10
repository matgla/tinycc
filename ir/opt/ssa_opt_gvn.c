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
  Sym *sym1;     /* resolved sym for SYMREF operands (NULL otherwise) */
  Sym *sym2;
  Sym *sym3;
  uint8_t s1_tag;
  uint8_t s2_tag;
  uint8_t s3_tag;
  uint8_t s1_lval; /* lval-flag per src — key identity + STORE invalidation (local cache) */
  uint8_t s2_lval;
  uint8_t s3_lval;
  uint8_t is64;    /* result width — btype is not part of the operand key */
  int32_t result_vr;
  int def_idx;
  struct GVNEntry *next;
} GVNEntry;

/* Non-lval STACKOFF without a backing vreg (Addr[StackLoc[-N]]) is a
 * compile-time-constant frame address — safe to key by offset alone. */
#define GVN_SRC_OK(s) \
  (!(s).is_lval && !(s).is_llocal && \
   (!(s).is_local || ((s).tag == IROP_TAG_STACKOFF && irop_get_vreg(s) < 0)))

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

static uint32_t gvn_hash(int op, uint8_t s1_tag, int32_t s1, int32_t imm1, Sym *sym1,
                          uint8_t s2_tag, int32_t s2, int32_t imm2, Sym *sym2,
                          uint8_t s3_tag, int32_t s3, int32_t imm3, Sym *sym3)
{
  uint32_t h = (uint32_t)op * 2654435761u;
  h ^= ((uint32_t)s1_tag << 28) ^ (uint32_t)s1 * 2246822519u;
  h ^= (uint32_t)imm1 * 3266489917u;
  h ^= ((uint32_t)s2_tag << 28) ^ (uint32_t)s2 * 374761393u;
  h ^= (uint32_t)imm2 * 668265263u;
  h ^= ((uint32_t)s3_tag << 28) ^ (uint32_t)s3 * 1597334677u;
  h ^= (uint32_t)imm3 * 1442695041u;
  h ^= (uint32_t)(uintptr_t)sym1 * 2654435761u;
  h ^= (uint32_t)(uintptr_t)sym2 * 2246822519u;
  h ^= (uint32_t)(uintptr_t)sym3 * 3266489917u;
  return h & (GVN_HASH_SIZE - 1);
}

/* SYMREF pool entries are never deduplicated, so key them by resolved
 * (sym, addend) — two references to the same global must compare equal. */
static void gvn_operand_key(TCCIRState *ir, IROperand op, uint8_t *tag, int32_t *vr,
                            int32_t *imm, Sym **sym)
{
  *tag = op.tag;
  *vr = irop_get_vreg(op);
  *sym = NULL;
  if (op.tag == IROP_TAG_IMM32 || op.tag == IROP_TAG_F32)
    *imm = op.u.imm32;
  else if (op.tag == IROP_TAG_STACKOFF)
    *imm = op.u.imm32;
  else if (op.tag == IROP_TAG_SYMREF) {
    IRPoolSymref *sr = irop_get_symref_ex(ir, op);
    if (sr) {
      *sym = sr->sym;
      *imm = (int32_t)sr->addend;
    } else
      *imm = (int32_t)op.u.pool_idx;
  } else if (op.tag == IROP_TAG_I64 || op.tag == IROP_TAG_F64)
    *imm = (int32_t)op.u.pool_idx;
  else
    *imm = 0;
}

static GVNEntry *gvn_find(GVNEntry **table, int op, uint8_t s1_tag, int32_t s1,
                           int32_t imm1, Sym *sym1, uint8_t s2_tag, int32_t s2,
                           int32_t imm2, Sym *sym2, uint8_t s3_tag, int32_t s3,
                           int32_t imm3, Sym *sym3)
{
  uint32_t h = gvn_hash(op, s1_tag, s1, imm1, sym1, s2_tag, s2, imm2, sym2,
                        s3_tag, s3, imm3, sym3);
  for (GVNEntry *e = table[h]; e; e = e->next) {
    if (e->op == op && e->s1_tag == s1_tag && e->src1 == s1 && e->imm1 == imm1 &&
        e->sym1 == sym1 && e->s2_tag == s2_tag && e->src2 == s2 && e->imm2 == imm2 &&
        e->sym2 == sym2 && e->s3_tag == s3_tag && e->src3 == s3 && e->imm3 == imm3 &&
        e->sym3 == sym3)
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
  uint32_t h = gvn_hash(entry->op, entry->s1_tag, entry->src1, entry->imm1, entry->sym1,
                         entry->s2_tag, entry->src2, entry->imm2, entry->sym2,
                         entry->s3_tag, entry->src3, entry->imm3, entry->sym3);
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

/* Block-local value numbering for sources GVN cannot scope across blocks:
 * VAR/memory reads (lval), multi-def vregs, mutated PARAMs.  Entries live in a
 * small per-block cache with the legacy local_alu_cse kill rules: killed on
 * source-vreg redef, on stores/addrtaken-writes when a source reads memory,
 * flushed on calls/BLOCK_COPY/INLINE_ASM (callees mutate addrtaken VARs). */
#define GVN_LOCAL_MAX 32

/* -1: never CSE (double indirection), 0: stable SSA value (dominator scope),
 *  1: block-local only */
static int gvn_src_class(IRSSAOptCtx *ctx, IROperand s)
{
  if (s.is_llocal)
    return -1;
  int32_t v = irop_get_vreg(s);
  if (s.is_lval)
    return 1;
  if (s.is_local)
    return (s.tag == IROP_TAG_STACKOFF && v < 0) ? 0 : 1;
  if (v >= 0) {
    int t = TCCIR_DECODE_VREG_TYPE(v);
    if (t == TCCIR_VREG_TYPE_TEMP) {
      IRSSAVregInfo *vi = ssa_opt_vinfo(ctx, v);
      return (vi && vi->def_count <= 1) ? 0 : 1;
    }
    if (t == TCCIR_VREG_TYPE_PARAM)
      return gvn_param_is_stable(v) ? 0 : 1;
    return 1;
  }
  return 0;
}

static int gvn_local_flushes(int op)
{
  switch (op) {
  case TCCIR_OP_JUMP:
  case TCCIR_OP_JUMPIF:
  case TCCIR_OP_IJUMP:
  case TCCIR_OP_SWITCH_TABLE:
  case TCCIR_OP_RETURNVALUE:
  case TCCIR_OP_RETURNVOID:
  case TCCIR_OP_FUNCCALLVAL:
  case TCCIR_OP_FUNCCALLVOID:
  case TCCIR_OP_BLOCK_COPY:
  case TCCIR_OP_INLINE_ASM:
    return 1;
  default:
    return 0;
  }
}

static int gvn_local_invalidate(TCCIRState *ir, GVNEntry *lc, int lcount, IRQuadCompact *q)
{
  if (gvn_local_flushes(q->op))
    return 0;
  int is_store_like = (q->op == TCCIR_OP_STORE || q->op == TCCIR_OP_STORE_INDEXED ||
                       q->op == TCCIR_OP_STORE_POSTINC);
  int32_t kill = -1;
  int aliases_mem = 0;
  if (irop_config[q->op].has_dest) {
    IROperand d = tcc_ir_op_get_dest(ir, q);
    kill = irop_get_vreg(d);
    if (kill >= 0) {
      IRLiveInterval *li = tcc_ir_vreg_live_interval(ir, kill);
      if (li && li->addrtaken)
        aliases_mem = 1;
    }
  }
  if (!is_store_like && kill < 0)
    return lcount;
  int w = 0;
  for (int c = 0; c < lcount; c++) {
    GVNEntry *e = &lc[c];
    int kills = 0;
    if ((is_store_like || aliases_mem) && (e->s1_lval || e->s2_lval || e->s3_lval))
      kills = 1;
    #define GVN_KILL_SRC(tg, vr) (((tg) == IROP_TAG_VREG || (tg) == IROP_TAG_STACKOFF) && (vr) == kill)
    if (kill >= 0 && (GVN_KILL_SRC(e->s1_tag, e->src1) || GVN_KILL_SRC(e->s2_tag, e->src2) ||
                      GVN_KILL_SRC(e->s3_tag, e->src3) || e->result_vr == kill))
      kills = 1;
    #undef GVN_KILL_SRC
    if (!kills)
      lc[w++] = *e;
  }
  return w;
}

static int gvn_local_eq(const GVNEntry *e,
                        uint8_t t1, uint8_t l1, int32_t v1, int32_t m1, Sym *y1,
                        uint8_t t2, uint8_t l2, int32_t v2, int32_t m2, Sym *y2,
                        uint8_t t3, uint8_t l3, int32_t v3, int32_t m3, Sym *y3)
{
  return e->s1_tag == t1 && e->s1_lval == l1 && e->src1 == v1 && e->imm1 == m1 && e->sym1 == y1 &&
         e->s2_tag == t2 && e->s2_lval == l2 && e->src2 == v2 && e->imm2 == m2 && e->sym2 == y2 &&
         e->s3_tag == t3 && e->s3_lval == l3 && e->src3 == v3 && e->imm3 == m3 && e->sym3 == y3;
}

/* Worklist item for the iterative dominator-tree walk below.  kind==0 is a
 * block to process; kind==1 is a deferred "restore the GVN scope to this undo
 * watermark", scheduled to run after the block's whole subtree completes. */
typedef struct GVNWork {
  int kind;
  int value;
} GVNWork;

/* Would consumer instr at use_idx itself CSE once cur_vr is replaced by rep_vr? */
static int gvn_consumer_would_cse(IRSSAOptCtx *ctx, GVNEntry **table, int use_idx,
                                  int32_t cur_vr, int32_t rep_vr)
{
  TCCIRState *ir = ctx->ir;
  if (use_idx < 0 || use_idx >= ir->next_instruction_index)
    return 0;
  IRQuadCompact *uq = &ir->compact_instructions[use_idx];
  if (!gvn_is_pure_alu(uq->op) || uq->op == TCCIR_OP_MLA)
    return 0;
  if (!irop_config[uq->op].has_src1 || !irop_config[uq->op].has_src2)
    return 0;
  IROperand ud = tcc_ir_op_get_dest(ir, uq);
  int32_t udv = irop_get_vreg(ud);
  if (udv < 0 || TCCIR_DECODE_VREG_TYPE(udv) != TCCIR_VREG_TYPE_TEMP)
    return 0;
  IROperand us1 = tcc_ir_op_get_src1(ir, uq);
  IROperand us2 = tcc_ir_op_get_src2(ir, uq);
  if (!GVN_SRC_OK(us1) || !GVN_SRC_OK(us2))
    return 0;
  uint8_t t1, t2;
  int32_t v1, v2, m1, m2;
  Sym *y1, *y2;
  gvn_operand_key(ir, us1, &t1, &v1, &m1, &y1);
  gvn_operand_key(ir, us2, &t2, &v2, &m2, &y2);
  if (t1 == IROP_TAG_VREG && v1 == cur_vr)
    v1 = rep_vr;
  if (t2 == IROP_TAG_VREG && v2 == cur_vr)
    v2 = rep_vr;
  if (gvn_find(table, uq->op, t1, v1, m1, y1, t2, v2, m2, y2, 0, 0, 0, NULL))
    return 1;
  if (gvn_is_commutative(uq->op) &&
      gvn_find(table, uq->op, t2, v2, m2, y2, t1, v1, m1, y1, 0, 0, 0, NULL))
    return 1;
  return 0;
}

/* LEA of a vreg-backed stack slot (address of an anonymous temp local, encoded
 * as a STACKOFF with vreg < -1) computes a frame address invariant across the
 * whole function.  CSE repeated LEAs of the same slot (dominator-scoped) into
 * one canonical LEA + ASSIGN copies, replacing the retired flat-IR lea_cse
 * pass.  vreg == -1 STACKOFFs are left to lea_fold, whose single-use
 * precondition CSE would break. */
static int gvn_try_lea(IRSSAOptCtx *ctx, GVNEntry **table, int i)
{
  TCCIRState *ir = ctx->ir;
  IRQuadCompact *q = &ir->compact_instructions[i];
  IROperand dest = tcc_ir_op_get_dest(ir, q);
  int32_t dest_vr = irop_get_vreg(dest);
  if (dest_vr < 0 || dest.is_lval || TCCIR_DECODE_VREG_TYPE(dest_vr) != TCCIR_VREG_TYPE_TEMP)
    return 0;
  IRSSAVregInfo *vi = ssa_opt_vinfo(ctx, dest_vr);
  if (vi && vi->def_count > 1)
    return 0;

  IROperand src = tcc_ir_op_get_src1(ir, q);
  if (irop_get_tag(src) != IROP_TAG_STACKOFF || src.is_lval || irop_get_vreg(src) >= -1)
    return 0;

  /* Key by the normalized frame offset (irop_get_stack_offset masks the
   * ctype_idx half of u.s for STRUCT operands) so two views of the same slot
   * that differ only in type metadata compare equal. */
  uint8_t s1_tag = irop_get_tag(src);
  int32_t s1_vr = irop_get_vreg(src);
  int32_t s1_imm = irop_get_stack_offset(src);

  GVNEntry *existing = gvn_find(table, TCCIR_OP_LEA, s1_tag, s1_vr, s1_imm, NULL,
                                 0, 0, 0, NULL, 0, 0, 0, NULL);
  if (existing && existing->def_idx > i)
    existing = NULL;

  if (existing) {
    IROperand new_src = dest;
    new_src.vr = existing->result_vr;
    new_src.tag = IROP_TAG_VREG;
    new_src.is_lval = 0;
    new_src.is_local = 0;
    new_src.is_llocal = 0;
    new_src.u.imm32 = 0;
    IRSSAVregInfo *rvi = ssa_opt_vinfo(ctx, existing->result_vr);
    if (rvi) ssa_opt_add_use_instr(rvi, i);
    q->op = TCCIR_OP_ASSIGN;
    tcc_ir_set_src1(ir, i, new_src);
    tcc_ir_set_src2(ir, i, IROP_NONE);
    return 1;
  }

  GVNEntry *e = gvn_alloc_entry();
  if (!e)
    return 0;
  e->op = TCCIR_OP_LEA;
  e->s1_tag = s1_tag;
  e->src1 = s1_vr;
  e->imm1 = s1_imm;
  e->result_vr = dest_vr;
  e->def_idx = i;
  gvn_scope_push(table, e);
  return 0;
}

/* 64-bit dup: ASSIGN pair-copies drop the high word downstream (seed 686), so substitute uses directly and NOP the dup */
static int gvn_subst_64bit(IRSSAOptCtx *ctx, int i, int32_t dest_vr,
                           int32_t result_vr, int result_def_idx)
{
  TCCIRState *ir = ctx->ir;
  if (result_vr == dest_vr || result_vr < 0)
    return 0;
  IROperand dest = tcc_ir_op_get_dest(ir, &ir->compact_instructions[i]);
  IROperand rdest = tcc_ir_op_get_dest(ir, &ir->compact_instructions[result_def_idx]);
  if (irop_get_btype(rdest) != irop_get_btype(dest))
    return 0;
  IRSSAVregInfo *dvi = ssa_opt_vinfo(ctx, dest_vr);
  IRSSAVregInfo *rvi = ssa_opt_vinfo(ctx, result_vr);
  if (!dvi || !rvi || rvi->def_count > 1)
    return 0;
  /* naming the survivor directly in a phi reintroduces the lost-copy problem at out-of-SSA (seed 2698) */
  for (int u = 0; u < dvi->use_count; u++)
    if (dvi->uses[u].kind == SSA_USE_PHI)
      return 0;
  if (ssa_opt_replace_all_uses(ctx, dest_vr, result_vr) == 0 && dvi->use_count > 0)
    return 0;
  ssa_opt_nop_instr(ctx, i);
  return 1;
}

/* Rewrite a redundant CMP+SETIF pair: NOP the CMP, turn the SETIF into an
 * ASSIGN copy of the earlier equal comparison's result vreg. */
static int gvn_cmp_setif_rewrite(IRSSAOptCtx *ctx, int cmp_idx, int setif_idx,
                                 int32_t result_vr, int result_def_idx)
{
  TCCIRState *ir = ctx->ir;
  IROperand pdest = tcc_ir_op_get_dest(ir, &ir->compact_instructions[result_def_idx]);
  IROperand src_vreg = irop_make_vreg(result_vr, irop_get_btype(pdest));
  ssa_opt_nop_instr(ctx, cmp_idx);
  IRSSAVregInfo *rvi = ssa_opt_vinfo(ctx, result_vr);
  if (rvi) ssa_opt_add_use_instr(rvi, setif_idx);
  ir->compact_instructions[setif_idx].op = TCCIR_OP_ASSIGN;
  tcc_ir_set_src1(ir, setif_idx, src_vreg);
  tcc_ir_set_src2(ir, setif_idx, IROP_NONE);
  return 1;
}

/* CMP+SETIF CSE, folded into GVN (replaces the standalone
 * tcc_ir_opt_cmp_setif_cse).  Called when q@setif_idx is a SETIF; looks back to
 * the immediately-preceding non-NOP CMP and value-numbers the pair keyed on
 * (cmp_s1, cmp_s2, btypes, cond) -> SETIF result vreg.  When both CMP operands
 * are GVN-stable (gvn_src_class 0) the pair goes in the dominator-scoped table
 * for cross-block reuse; otherwise it uses the block-local cache, inheriting
 * gvn_local_invalidate's store/call/redef bailouts. */
static int gvn_try_cmp_setif(IRSSAOptCtx *ctx, GVNEntry **table, GVNEntry *lcache,
                             int *lcountp, int setif_idx, IRBasicBlock *bb)
{
  TCCIRState *ir = ctx->ir;
  IRQuadCompact *setif = &ir->compact_instructions[setif_idx];

  int cmp_idx = setif_idx - 1;
  while (cmp_idx >= bb->start_idx && ir->compact_instructions[cmp_idx].op == TCCIR_OP_NOP)
    cmp_idx--;
  if (cmp_idx < bb->start_idx || ir->compact_instructions[cmp_idx].op != TCCIR_OP_CMP)
    return 0;
  IRQuadCompact *cmp = &ir->compact_instructions[cmp_idx];

  IROperand setif_dest = tcc_ir_op_get_dest(ir, setif);
  int32_t dest_vr = irop_get_vreg(setif_dest);
  if (dest_vr < 0 || setif_dest.is_lval)
    return 0;

  IROperand cmp_s1 = tcc_ir_op_get_src1(ir, cmp);
  IROperand cmp_s2 = tcc_ir_op_get_src2(ir, cmp);
  IROperand cond_op = tcc_ir_op_get_src1(ir, setif);
  int32_t cond = (int32_t)irop_get_imm64_ex(ir, cond_op);
  int32_t btkey = (irop_get_btype(cmp_s1) & 0xffff) | ((irop_get_btype(cmp_s2) & 0xffff) << 16);

  int c1 = gvn_src_class(ctx, cmp_s1);
  int c2 = gvn_src_class(ctx, cmp_s2);
  if (c1 < 0 || c2 < 0)
    return 0;
  int local = c1 | c2;

  uint8_t s1_tag, s2_tag;
  int32_t s1_vr, s2_vr, s1_imm, s2_imm;
  Sym *s1_sym, *s2_sym;
  gvn_operand_key(ir, cmp_s1, &s1_tag, &s1_vr, &s1_imm, &s1_sym);
  gvn_operand_key(ir, cmp_s2, &s2_tag, &s2_vr, &s2_imm, &s2_sym);
  if ((s1_sym && (s1_sym->type.t & VT_VOLATILE)) ||
      (s2_sym && (s2_sym->type.t & VT_VOLATILE)))
    return 0;
  uint8_t s1_lv = cmp_s1.is_lval, s2_lv = cmp_s2.is_lval;

  if (!local) {
    GVNEntry *existing = gvn_find(table, TCCIR_OP_SETIF, s1_tag, s1_vr, s1_imm, s1_sym,
                                  s2_tag, s2_vr, s2_imm, s2_sym, 0, btkey, cond, NULL);
    /* a dominating def at a later linear position is a rotated-loop guard */
    if (existing && existing->def_idx > setif_idx)
      existing = NULL;
    if (existing)
      return gvn_cmp_setif_rewrite(ctx, cmp_idx, setif_idx, existing->result_vr,
                                   existing->def_idx);
    IRSSAVregInfo *dvi = ssa_opt_vinfo(ctx, dest_vr);
    if (dvi && dvi->def_count > 1)
      return 0;
    GVNEntry *e = gvn_alloc_entry();
    if (!e)
      return 0;
    e->op = TCCIR_OP_SETIF;
    e->s1_tag = s1_tag; e->src1 = s1_vr; e->imm1 = s1_imm; e->sym1 = s1_sym;
    e->s2_tag = s2_tag; e->src2 = s2_vr; e->imm2 = s2_imm; e->sym2 = s2_sym;
    e->s3_tag = 0; e->src3 = btkey; e->imm3 = cond; e->sym3 = NULL;
    e->result_vr = dest_vr;
    e->def_idx = setif_idx;
    gvn_scope_push(table, e);
    return 0;
  }

  int lcount = *lcountp;
  for (int c = 0; c < lcount; c++) {
    GVNEntry *e = &lcache[c];
    if (e->op != TCCIR_OP_SETIF)
      continue;
    if (e->s1_tag == s1_tag && e->s1_lval == s1_lv && e->src1 == s1_vr &&
        e->imm1 == s1_imm && e->sym1 == s1_sym &&
        e->s2_tag == s2_tag && e->s2_lval == s2_lv && e->src2 == s2_vr &&
        e->imm2 == s2_imm && e->sym2 == s2_sym &&
        e->src3 == btkey && e->imm3 == cond)
      return gvn_cmp_setif_rewrite(ctx, cmp_idx, setif_idx, e->result_vr, e->def_idx);
  }

  IRSSAVregInfo *dvi = ssa_opt_vinfo(ctx, dest_vr);
  if (dvi && dvi->def_count > 1)
    return 0;
  if (lcount >= GVN_LOCAL_MAX)
    return 0;
  GVNEntry *e = &lcache[lcount++];
  memset(e, 0, sizeof(*e));
  e->op = TCCIR_OP_SETIF;
  e->s1_tag = s1_tag; e->s1_lval = s1_lv; e->src1 = s1_vr; e->imm1 = s1_imm; e->sym1 = s1_sym;
  e->s2_tag = s2_tag; e->s2_lval = s2_lv; e->src2 = s2_vr; e->imm2 = s2_imm; e->sym2 = s2_sym;
  e->src3 = btkey;
  e->imm3 = cond;
  e->result_vr = dest_vr;
  e->def_idx = setif_idx;
  *lcountp = lcount;
  return 0;
}

static int gvn_process_block(IRSSAOptCtx *ctx, IRCFG *cfg, GVNEntry **table, int b_root)
{
  TCCIRState *ir = ctx->ir;
  int changes = 0;

  /* Iterative DFS with a heap worklist instead of native recursion.  The
   * recursion was once-per-dominator-child deep, so functions with deep
   * branch nesting (one `if` per source statement) overflowed the 32 KB
   * target process stack.  The scoped-availability semantics are preserved
   * by pushing a POP marker (kind==1) before a block's children: it sits
   * below them on the stack and so runs only after the entire subtree, just
   * like the post-recursion gvn_scope_pop_to(saved_undo) it replaces. */
  GVNWork *stack = tcc_malloc(sizeof *stack * 16);
  int sp = 0, cap = 16;
  stack[sp].kind = 0;
  stack[sp].value = b_root;
  sp++;

  while (sp > 0) {
    sp--;
    if (stack[sp].kind == 1) {
      gvn_scope_pop_to(stack[sp].value);
      continue;
    }
    int b = stack[sp].value;
    IRBasicBlock *bb = &cfg->blocks[b];
    int saved_undo = undo_count;
    GVNEntry lcache[GVN_LOCAL_MAX];
    int lcount = 0;

  for (int i = bb->start_idx; i < bb->end_idx; i++) {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;
    if (lcount > 0)
      lcount = gvn_local_invalidate(ir, lcache, lcount, q);

    if (q->op == TCCIR_OP_LEA) {
      changes += gvn_try_lea(ctx, table, i);
      continue;
    }

    if (q->op == TCCIR_OP_SETIF) {
      changes += gvn_try_cmp_setif(ctx, table, lcache, &lcount, i, bb);
      continue;
    }

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
    if (tcc_ir_barrel_shift_at(ir, q))
      continue;

    IROperand dest = tcc_ir_op_get_dest(ir, q);
    int32_t dest_vr = irop_get_vreg(dest);
    if (dest_vr < 0 || dest.is_lval || TCCIR_DECODE_VREG_TYPE(dest_vr) != TCCIR_VREG_TYPE_TEMP)
      continue;

    int is64 = irop_is_64bit(dest);

    IRSSAVregInfo *vi = ssa_opt_vinfo(ctx, dest_vr);
    if (vi && vi->def_count > 1)
      continue;

    IROperand src1 = tcc_ir_op_get_src1(ir, q);
    IROperand src2 = tcc_ir_op_get_src2(ir, q);

    int c1 = gvn_src_class(ctx, src1);
    int c2 = gvn_src_class(ctx, src2);
    if (c1 < 0 || c2 < 0)
      continue;
    int local = c1 | c2;
    int32_t s1v = irop_get_vreg(src1);
    int32_t s2v = irop_get_vreg(src2);

    /* MLA has a 3rd operand (accumulator) at pool[operand_base+3]. */
    IROperand accum = IROP_NONE;
    int32_t s3v = -1;
    int is_mla = (q->op == TCCIR_OP_MLA);
    if (is_mla) {
      accum = tcc_ir_op_get_accum(ir, q);
      int c3 = gvn_src_class(ctx, accum);
      if (c3 < 0)
        continue;
      local |= c3;
      s3v = irop_get_vreg(accum);
    }

    uint8_t s1_tag, s2_tag, s3_tag = 0;
    int32_t s1_vr, s2_vr, s1_imm, s2_imm;
    int32_t s3_vr = 0, s3_imm = 0;
    Sym *s1_sym, *s2_sym, *s3_sym = NULL;
    gvn_operand_key(ir, src1, &s1_tag, &s1_vr, &s1_imm, &s1_sym);
    gvn_operand_key(ir, src2, &s2_tag, &s2_vr, &s2_imm, &s2_sym);
    if (is_mla)
      gvn_operand_key(ir, accum, &s3_tag, &s3_vr, &s3_imm, &s3_sym);

    if (local) {
      /* Volatile reads must all be emitted — never CSE them. */
      if ((s1_sym && (s1_sym->type.t & VT_VOLATILE)) || (s2_sym && (s2_sym->type.t & VT_VOLATILE)) ||
          (s3_sym && (s3_sym->type.t & VT_VOLATILE)))
        continue;
      uint8_t s1_lv = src1.is_lval, s2_lv = src2.is_lval;
      uint8_t s3_lv = is_mla ? accum.is_lval : 0;
      GVNEntry *le = NULL;
      for (int c = 0; c < lcount; c++) {
        GVNEntry *e = &lcache[c];
        if (e->op != q->op)
          continue;
        if (gvn_local_eq(e, s1_tag, s1_lv, s1_vr, s1_imm, s1_sym,
                         s2_tag, s2_lv, s2_vr, s2_imm, s2_sym,
                         s3_tag, s3_lv, s3_vr, s3_imm, s3_sym) ||
            ((gvn_is_commutative(q->op) || is_mla) &&
             gvn_local_eq(e, s2_tag, s2_lv, s2_vr, s2_imm, s2_sym,
                          s1_tag, s1_lv, s1_vr, s1_imm, s1_sym,
                          s3_tag, s3_lv, s3_vr, s3_imm, s3_sym))) {
          le = e;
          break;
        }
      }
      if (le && le->is64 != is64)
        le = NULL;
      if (le && is64) {
        changes += gvn_subst_64bit(ctx, i, dest_vr, le->result_vr, le->def_idx);
      } else if (le) {
        IROperand new_src = dest;
        new_src.vr = le->result_vr;
        new_src.tag = IROP_TAG_VREG;
        new_src.is_lval = 0;
        new_src.is_local = 0;
        new_src.is_llocal = 0;
        new_src.u.imm32 = 0;
        IRSSAVregInfo *s1vi = ssa_opt_vinfo(ctx, s1v);
        if (s1vi) ssa_opt_remove_use_instr(s1vi, i);
        IRSSAVregInfo *s2vi = ssa_opt_vinfo(ctx, s2v);
        if (s2vi) ssa_opt_remove_use_instr(s2vi, i);
        if (is_mla && s3v >= 0) {
          IRSSAVregInfo *s3vi = ssa_opt_vinfo(ctx, s3v);
          if (s3vi) ssa_opt_remove_use_instr(s3vi, i);
        }
        IRSSAVregInfo *rvi = ssa_opt_vinfo(ctx, le->result_vr);
        if (rvi) ssa_opt_add_use_instr(rvi, i);
        q->op = TCCIR_OP_ASSIGN;
        tcc_ir_set_src1(ir, i, new_src);
        tcc_ir_set_src2(ir, i, IROP_NONE);
        changes++;
      } else if (lcount < GVN_LOCAL_MAX) {
        GVNEntry *e = &lcache[lcount++];
        memset(e, 0, sizeof(*e));
        e->op = q->op;
        e->s1_tag = s1_tag;
        e->s1_lval = s1_lv;
        e->src1 = s1_vr;
        e->imm1 = s1_imm;
        e->sym1 = s1_sym;
        e->s2_tag = s2_tag;
        e->s2_lval = s2_lv;
        e->src2 = s2_vr;
        e->imm2 = s2_imm;
        e->sym2 = s2_sym;
        e->s3_tag = s3_tag;
        e->s3_lval = s3_lv;
        e->src3 = s3_vr;
        e->imm3 = s3_imm;
        e->sym3 = s3_sym;
        e->is64 = (uint8_t)is64;
        e->result_vr = dest_vr;
        e->def_idx = i;
      }
      continue;
    }

    GVNEntry *existing = gvn_find(table, q->op, s1_tag, s1_vr, s1_imm, s1_sym,
                                   s2_tag, s2_vr, s2_imm, s2_sym, s3_tag, s3_vr, s3_imm, s3_sym);
    if (!existing && gvn_is_commutative(q->op))
      existing = gvn_find(table, q->op, s2_tag, s2_vr, s2_imm, s2_sym,
                           s1_tag, s1_vr, s1_imm, s1_sym, s3_tag, s3_vr, s3_imm, s3_sym);
    /* MLA: src1 * src2 is commutative — also try swapped src1<->src2. */
    if (!existing && is_mla)
      existing = gvn_find(table, q->op, s2_tag, s2_vr, s2_imm, s2_sym,
                           s1_tag, s1_vr, s1_imm, s1_sym, s3_tag, s3_vr, s3_imm, s3_sym);

    /* dominating def at a later linear position (rotated-loop guard) would wrap the live range around the loop */
    if (existing && (existing->def_idx > i || existing->is64 != is64))
      existing = NULL;

    /* single-use imm shift (barrel/indexed fusion) or MUL-feeding-ADD (MLA fusion) of a phi folds into its consumer; CSE to multi-use un-fuses it */
    int fusable = 0;
    if (s2_tag == IROP_TAG_IMM32 &&
        (q->op == TCCIR_OP_SHL || q->op == TCCIR_OP_SHR || q->op == TCCIR_OP_SAR ||
         q->op == TCCIR_OP_ROR))
      fusable = 1;
    else if (q->op == TCCIR_OP_MUL)
      fusable = 2;
    if (existing && fusable && s1v >= 0) {
      IRSSAVregInfo *v1 = ssa_opt_vinfo(ctx, s1v);
      IRSSAVregInfo *vs2 = (fusable == 2 && s2v >= 0) ? ssa_opt_vinfo(ctx, s2v) : NULL;
      if ((v1 && v1->def_phi_block >= 0) || (vs2 && vs2->def_phi_block >= 0)) {
        IRSSAVregInfo *dvi = ssa_opt_vinfo(ctx, dest_vr);
        IRSSAVregInfo *evi = ssa_opt_vinfo(ctx, existing->result_vr);
        int d1 = dvi && dvi->use_count == 1 && dvi->uses[0].kind == SSA_USE_INSTR;
        int e1 = evi && evi->use_count == 1 && evi->uses[0].kind == SSA_USE_INSTR;
        if (fusable == 2) {
          d1 = d1 && ir->compact_instructions[dvi->uses[0].idx].op == TCCIR_OP_ADD;
          e1 = e1 && ir->compact_instructions[evi->uses[0].idx].op == TCCIR_OP_ADD;
        }
        if ((d1 || e1) &&
            !(d1 && gvn_consumer_would_cse(ctx, table, dvi->uses[0].idx,
                                           dest_vr, existing->result_vr)))
          existing = NULL;
      }
    }

    if (existing && is64) {
      changes += gvn_subst_64bit(ctx, i, dest_vr, existing->result_vr,
                                 existing->def_idx);
      continue;
    }

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
    e->sym1 = s1_sym;
    e->s2_tag = s2_tag;
    e->src2 = s2_vr;
    e->imm2 = s2_imm;
    e->sym2 = s2_sym;
    e->s3_tag = s3_tag;
    e->src3 = s3_vr;
    e->imm3 = s3_imm;
    e->sym3 = s3_sym;
    e->is64 = (uint8_t)is64;
    e->result_vr = dest_vr;
    e->def_idx = i;
    gvn_scope_push(table, e);
  }

    /* Schedule the hash-table restore for after this block's subtree, then
     * push the dominator-tree children above it.  Entries from this block
     * remain visible to its children (the dominator guarantees availability);
     * sibling subtrees are independent, so DFS order among them is irrelevant. */
    if (sp + 1 + bb->num_dom_children > cap) {
      while (sp + 1 + bb->num_dom_children > cap)
        cap *= 2;
      stack = tcc_realloc(stack, sizeof *stack * cap);
    }
    stack[sp].kind = 1;
    stack[sp].value = saved_undo;
    sp++;
    for (int ci = 0; ci < bb->num_dom_children; ci++) {
      stack[sp].kind = 0;
      stack[sp].value = bb->dom_children[ci];
      sp++;
    }
  } /* while (sp > 0) */

  tcc_free(stack);
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
