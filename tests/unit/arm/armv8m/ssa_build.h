/*
 *  ssa_build.h - helpers for building SSA fixtures in unit tests
 *
 *  Provides the ssa_ctx_* helper family used by the ssa_opt unit-test
 *  suites (test_ssa_opt_*.c). These helpers build only the fields of a
 *  TCCIRState + IRSSAState + IRCFG + vinfo that a pass needs, mirroring
 *  what tcc_ir_ssa_opt_init + tcc_ir_ssa_opt_rebuild would compute for a
 *  real snippet.
 *
 *  Two layers:
 *    Layer A (hand-built): ssa_ctx_* helpers build a minimal valid ctx for
 *      passes that only need vinfo + straight-line IR. This is what
 *      test_ssa_opt_arm.c already does for its hand-built vinfo.
 *    Layer B (real construction): ssa_ctx_full_build() drives tcc_ir_cfg_build
 *      + tcc_ir_ssa_construct + tcc_ir_ssa_rename + tcc_ir_ssa_opt_init
 *      for passes that need a genuine SSA state with phis.
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software and/or modify it under the terms of the GNU
 * Lesser General Public License.
 */

#ifndef SSA_BUILD_H
#define SSA_BUILD_H

#include "ir_build.h"
#include "ir/opt/ssa_opt.h"

#include "ut.h"

#define USING_GLOBALS
#include "tcc.h"
#include "ir/opt/ssa_opt.h"

/* ========================================================================
 * ssa_ctx - hand-built SSA fixture
 *
 * Mirrors what tcc_ir_ssa_opt_init + tcc_ir_ssa_opt_rebuild compute from a
 * real IR: a TCCIRState with instructions, an IRCFG with blocks, an
 * IRSSAState with phi nodes, and a vinfo[] def/use table.
 *
 * For straight-line IR (no branches), we build a 1-block CFG and a minimal
 * SSA state with empty block_phis[]. For branching IR, we use the full
 * construction pipeline via ssa_ctx_full_build().
 * ======================================================================== */

#define SSA_CTX_MAX_TEMPS 64
#define SSA_CTX_MAX_BLOCKS 16
#define SSA_CTX_MAX_INSTRS 256

typedef struct ssa_ctx {
  TCCIRState *ir;
  IRCFG *cfg;
  IRSSAState *ssa;
  IRSSAOptCtx *ctx;       /* points into the struct for convenience */
  int num_temps;
  int num_blocks;
  int num_instrs;
} ssa_ctx;

/* ------------------------------------------------------------------ construction */

static inline ssa_ctx ssa_ctx_new(int blocks, int temps)
{
  ssa_ctx c;
  memset(&c, 0, sizeof(c));
  c.ir = utb_new();
  c.ir->iroperand_pool_capacity = UTB_MAX_OPERANDS;
  c.ir->next_temporary_variable = temps;
  c.ir->max_orig_index = UTB_MAX_INSTR - 1;

  /* Initialize the scalar pools so passes can emit I64/F64/SYMREF/CTYPE
   * constants.  utb_new() leaves these zeroed; SCCP folding a 64-bit value
   * into the I64 pool would otherwise see capacity 0 and abort.  Keep the
   * hand-built iroperand_pool at UTB_MAX_OPERANDS rather than using the
   * smaller IRPOOL_INIT_SIZE from tcc_ir_pools_init(). */
  c.ir->pool_i64_capacity = 64;
  c.ir->pool_i64 = (int64_t *)tcc_mallocz(sizeof(int64_t) * c.ir->pool_i64_capacity);
  c.ir->pool_f64_capacity = 64;
  c.ir->pool_f64 = (uint64_t *)tcc_mallocz(sizeof(uint64_t) * c.ir->pool_f64_capacity);
  c.ir->pool_symref_capacity = 64;
  c.ir->pool_symref = (IRPoolSymref *)tcc_mallocz(sizeof(IRPoolSymref) * c.ir->pool_symref_capacity);
  c.ir->pool_ctype_capacity = 64;
  c.ir->pool_ctype = (CType *)tcc_mallocz(sizeof(CType) * c.ir->pool_ctype_capacity);

  c.num_temps = temps;
  c.num_blocks = blocks;
  return c;
}

static inline void ssa_ctx_free(ssa_ctx *c)
{
  if (!c)
    return;
  if (c->ctx)
    tcc_ir_ssa_opt_free(c->ctx);
  if (c->ssa)
    tcc_ir_ssa_free(c->ssa);
  if (c->cfg)
    tcc_ir_cfg_free(c->cfg);
  if (c->ir)
    utb_free(c->ir);
  memset(c, 0, sizeof(*c));
}

/* Emit one instruction into the ctx's IR. Returns its index. */
static inline int ssa_add_instr(ssa_ctx *c, TccIrOp op, IROperand dest,
                                IROperand src1)
{
  int i = utb_emit(c->ir, op, dest, src1, UTB_NONE);
  c->num_instrs = i + 1;
  return i;
}

static inline int ssa_add_instr3(ssa_ctx *c, TccIrOp op, IROperand dest,
                                 IROperand src1, IROperand src2)
{
  int i = utb_emit(c->ir, op, dest, src1, src2);
  c->num_instrs = i + 1;
  return i;
}

/* Emit with a 4th operand (MLA accumulator, SELECT cc, indexed scale). */
static inline int ssa_add_instr4(ssa_ctx *c, TccIrOp op, IROperand dest,
                                 IROperand src1, IROperand src2, IROperand op4)
{
  int i = utb_emit4(c->ir, op, dest, src1, src2, op4);
  c->num_instrs = i + 1;
  return i;
}

/* ------------------------------------------------------------------ CFG build */

/* Build a minimal CFG for the ctx's IR. For straight-line IR (no JUMP/JUMPIF),
 * this produces a single block spanning all instructions. For branching IR,
 * tcc_ir_cfg_build derives blocks from jump targets. */
static inline void ssa_ctx_build_cfg(ssa_ctx *c)
{
  c->cfg = tcc_ir_cfg_build(c->ir);
  if (c->cfg)
    tcc_ir_cfg_compute_dominators(c->cfg);
}

/* ------------------------------------------------------------------ SSA build */

/* Build a minimal SSA state. For straight-line IR with no VAR promotions,
 * this creates an SSA state with empty block_phis[]. For IR with VARs that
 * need promotion, use ssa_ctx_full_build() which drives the full pipeline. */
static inline void ssa_ctx_build_ssa_plain(ssa_ctx *c)
{
  if (!c->cfg || c->cfg->num_blocks == 0)
    return;
  c->ssa = tcc_mallocz(sizeof(*c->ssa));
  c->ssa->cfg = c->cfg;
  c->ssa->block_phis = tcc_malloc(c->cfg->num_blocks * sizeof(IRPhiNode *));
  memset(c->ssa->block_phis, 0, c->cfg->num_blocks * sizeof(IRPhiNode *));
  c->ssa->next_ssa_vreg = TCCIR_ENCODE_VREG(TCCIR_VREG_TYPE_TEMP,
                                             c->ir->next_temporary_variable);
  c->ssa->is_promotable = NULL;
  c->ssa->num_vars = 0;
}

/* Full SSA construction: tcc_ir_ssa_construct + tcc_ir_ssa_rename.
 * This is the proper pipeline for IR with VARs that need promotion.
 * Returns 1 on success, 0 if SSA construction bails (no promotable vars,
 * unsupported ops, etc.). */
static inline int ssa_ctx_full_build(ssa_ctx *c)
{
  ssa_ctx_build_cfg(c);
  if (!c->cfg)
    return 0;
  c->ssa = tcc_ir_ssa_construct(c->ir, c->cfg);
  if (!c->ssa)
    return 0;
  tcc_ir_ssa_rename(c->ir, c->ssa);
  return 1;
}

/* ------------------------------------------------------------------ vinfo build */

/* Build the vinfo def/use chains. Calls the real ssa_opt_build_chains via
 * tcc_ir_ssa_opt_init. Requires cfg + ssa to be built first. */
static inline void ssa_ctx_rebuild(ssa_ctx *c)
{
  if (!c->cfg || !c->ssa)
    return;
  c->ctx = tcc_mallocz(sizeof(*c->ctx));
  tcc_ir_ssa_opt_init(c->ctx, c->ir, c->ssa, c->cfg);
}

/* ------------------------------------------------------------------ accessors */

static inline IROperand ssa_instr_dest(ssa_ctx *c, int i)
{
  return utb_dest(c->ir, i);
}

static inline IROperand ssa_instr_src1(ssa_ctx *c, int i)
{
  return utb_src1(c->ir, i);
}

static inline IROperand ssa_instr_src2(ssa_ctx *c, int i)
{
  return utb_src2(c->ir, i);
}

static inline TccIrOp ssa_instr_op(ssa_ctx *c, int i)
{
  return utb_op(c->ir, i);
}

static inline int ssa_block_phi_count(ssa_ctx *c, int b)
{
  int count = 0;
  for (IRPhiNode *phi = c->ssa->block_phis[b]; phi; phi = phi->next)
    count++;
  return count;
}

static inline IRSSAVregInfo *ssa_vinfo(ssa_ctx *c, int32_t vreg)
{
  return ssa_opt_vinfo(c->ctx, vreg);
}

/* ------------------------------------------------------------------ manual CFG/SSA construction */

/* Manually create a minimal CFG with 1 block and an empty SSA state.
 * This is used by tests that need to add phis or instructions before
 * the real CFG build can work (which requires at least one instruction). */
static inline void ssa_ctx_init_manual(ssa_ctx *c)
{
  int blocks = c->num_blocks > 0 ? c->num_blocks : 1;

  c->cfg = tcc_mallocz(sizeof(*c->cfg));
  c->cfg->num_blocks = blocks;
  c->cfg->capacity = blocks;
  c->cfg->blocks = tcc_mallocz(sizeof(IRBasicBlock) * blocks);
  c->cfg->num_instrs = UTB_MAX_INSTR;
  c->cfg->instr_to_block = tcc_mallocz(sizeof(int) * UTB_MAX_INSTR);
  for (int b = 0; b < blocks; b++)
    c->cfg->blocks[b].idom = b == 0 ? -1 : 0;
  c->cfg->rpo_order = NULL;

  c->ssa = tcc_mallocz(sizeof(*c->ssa));
  c->ssa->cfg = c->cfg;
  c->ssa->block_phis = tcc_mallocz(sizeof(IRPhiNode *) * blocks);
  c->ssa->next_ssa_vreg = TCCIR_ENCODE_VREG(TCCIR_VREG_TYPE_TEMP,
                                             c->ir->next_temporary_variable);
  c->ssa->is_promotable = NULL;
  c->ssa->num_vars = 0;
}

static inline void ssa_ctx_manual_block_range(ssa_ctx *c, int block,
                                              int start_idx, int end_idx)
{
  if (!c || !c->cfg || block < 0 || block >= c->cfg->num_blocks)
    return;
  if (start_idx < 0)
    start_idx = 0;
  if (end_idx < start_idx)
    end_idx = start_idx;
  if (end_idx > UTB_MAX_INSTR)
    end_idx = UTB_MAX_INSTR;

  c->cfg->blocks[block].start_idx = start_idx;
  c->cfg->blocks[block].end_idx = end_idx;
  for (int i = start_idx; i < end_idx; i++)
    c->cfg->instr_to_block[i] = block;
}

/* ------------------------------------------------------------------ manual vinfo manipulation */

/* Record that instruction `def_idx` defines vreg `vr` (single definition). */
static inline void ssa_def(ssa_ctx *c, int32_t vr, int def_idx)
{
  IRSSAVregInfo *vi = ssa_opt_vinfo(c->ctx, vr);
  if (vi) {
    vi->def_instr = def_idx;
    vi->def_count = 1;
  }
}

/* Record that instruction `use_idx` uses vreg `vr`. */
static inline void ssa_use(ssa_ctx *c, int32_t vr, int use_idx)
{
  IRSSAVregInfo *vi = ssa_opt_vinfo(c->ctx, vr);
  if (vi)
    ssa_opt_add_use_instr(vi, use_idx);
}

/* Add a phi node to block b. dest_vreg is the phi's destination;
 * operand_vregs[0..n-1] are the incoming values (one per predecessor). */
static inline void ssa_add_phi(ssa_ctx *c, int block, int32_t dest_vreg,
                               const int32_t *operand_vregs, int num_preds)
{
  IRPhiNode *phi = tcc_mallocz(sizeof(*phi));
  phi->dest_vreg = dest_vreg;
  phi->orig_vreg = dest_vreg; /* not used for hand-built phis */
  phi->num_operands = num_preds;
  phi->cap_operands = num_preds;
  phi->btype = IROP_BTYPE_INT32;
  phi->operands = tcc_mallocz(num_preds * sizeof(IRPhiOperand));
  for (int i = 0; i < num_preds; i++) {
    phi->operands[i].vreg = operand_vregs[i];
    phi->operands[i].pred_block = i; /* placeholder; real construction sets from CFG preds */
  }
  phi->next = c->ssa->block_phis[block];
  c->ssa->block_phis[block] = phi;
}

#endif /* SSA_BUILD_H */
