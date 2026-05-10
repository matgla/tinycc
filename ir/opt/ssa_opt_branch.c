/*
 *  TCC IR - SSA Branch Folding
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
 * Branch Folding: when CMP or TEST_ZERO has constant operands (after cprop
 * propagated immediates), evaluate the comparison at compile time and convert
 * the JUMPIF to unconditional JUMP or NOP.
 *
 * Patterns:
 *   CMP #a, #b; JUMPIF cond → JUMP (if cond(a,b) is true)
 *   CMP #a, #b; JUMPIF cond → NOP  (if cond(a,b) is false)
 *   TEST_ZERO #a; JUMPIF EQ  → JUMP/NOP based on a==0
 *   CMP #a, #b; SETIF cond  → ASSIGN #0 or #1
 * ============================================================================ */

static int eval_cond(int64_t v1, int64_t v2, int tok)
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

/* Drop phi operands that flow from `dead_pred_block` to phis at
 * `target_block_idx`. Used after folding a JUMPIF: the dead edge no longer
 * exists, so phi resolution should not emit copies for it. */
void ssa_drop_phi_edge(IRSSAOptCtx *ctx, int dead_pred_block,
                       int target_block_idx)
{
  if (!ctx->ssa || !ctx->ssa->block_phis || !ctx->cfg) return;
  if (target_block_idx < 0 || target_block_idx >= ctx->cfg->num_blocks) return;

  for (IRPhiNode *phi = ctx->ssa->block_phis[target_block_idx]; phi; phi = phi->next) {
    /* Find every operand from dead_pred_block. Each removal shifts
     * remaining operands down, which means the vinfo `slot` field for those
     * operands must also be decremented to match. Process from low index
     * upward and recompute the loop bound after each removal. */
    int r = 0;
    while (r < phi->num_operands) {
      if (phi->operands[r].pred_block != dead_pred_block) {
        r++;
        continue;
      }

      /* Remove this operand's SSA_USE_PHI entry from its vreg's vinfo. */
      int32_t dropped_vr = phi->operands[r].vreg;
      if (dropped_vr >= 0) {
        IRSSAVregInfo *dvi = ssa_opt_vinfo(ctx, dropped_vr);
        if (dvi) {
          for (int u = 0; u < dvi->use_count; u++) {
            if (dvi->uses[u].kind == SSA_USE_PHI &&
                dvi->uses[u].idx == target_block_idx &&
                dvi->uses[u].slot == r) {
              dvi->uses[u] = dvi->uses[--dvi->use_count];
              break;
            }
          }
        }
      }

      /* Shift remaining operands down by one and decrement their vinfo
       * slots so SSA_USE_PHI entries keep pointing to the right operand. */
      for (int s = r + 1; s < phi->num_operands; s++) {
        phi->operands[s - 1] = phi->operands[s];
        int32_t v = phi->operands[s - 1].vreg;
        if (v < 0) continue;
        IRSSAVregInfo *vi = ssa_opt_vinfo(ctx, v);
        if (!vi) continue;
        for (int u = 0; u < vi->use_count; u++) {
          if (vi->uses[u].kind == SSA_USE_PHI &&
              vi->uses[u].idx == target_block_idx &&
              vi->uses[u].slot == s) {
            vi->uses[u].slot = s - 1;
            break;
          }
        }
      }
      phi->num_operands--;
      /* Don't advance r: the operand we just removed has been replaced by
       * what was at r+1, which we still need to inspect. */
    }
  }
}

static int ssa_block_for_instr(IRCFG *cfg, int instr_idx)
{
  if (!cfg || !cfg->instr_to_block) return -1;
  if (instr_idx < 0) return -1;
  return cfg->instr_to_block[instr_idx];
}

static int ssa_fold_cmp_jumpif(IRSSAOptCtx *ctx, int cmp_idx)
{
  TCCIRState *ir = ctx->ir;
  IRQuadCompact *cmp_q = &ir->compact_instructions[cmp_idx];
  int n = ir->next_instruction_index;

  IROperand src1 = tcc_ir_op_get_src1(ir, cmp_q);
  IROperand src2 = tcc_ir_op_get_src2(ir, cmp_q);

  int64_t v1, v2;
  int have_values = 0;

  /* Case 1: both operands are immediates (chase ASSIGN #const) */
  {
    IROperand ops[2] = { src1, src2 };
    int64_t vals[2];
    int got[2] = { 0, 0 };
    int cmp_block = ssa_block_for_instr(ctx->cfg, cmp_idx);
    IRBasicBlock *cmp_bb = (cmp_block >= 0) ? &ctx->cfg->blocks[cmp_block] : NULL;
    for (int oi = 0; oi < 2; oi++) {
      if (irop_is_immediate(ops[oi])) {
        vals[oi] = irop_get_imm64_ex(ir, ops[oi]);
        got[oi] = 1;
      } else {
        int32_t vr = irop_get_vreg(ops[oi]);
        if (vr >= 0 && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_TEMP &&
            ops[oi].tag == IROP_TAG_VREG && !ops[oi].is_lval) {
          IRSSAVregInfo *vi = ssa_opt_vinfo(ctx, vr);
          if (vi && vi->def_instr >= 0 && vi->def_count <= 1) {
            IRQuadCompact *dq = &ir->compact_instructions[vi->def_instr];
            if (dq->op == TCCIR_OP_ASSIGN) {
              IROperand ds = tcc_ir_op_get_src1(ir, dq);
              if (irop_is_immediate(ds) && !ds.is_lval) {
                vals[oi] = irop_get_imm64_ex(ir, ds);
                got[oi] = 1;
              }
            }
          }
        }
        /* VAR operand: scan same block backward for the most recent def
         * of this VAR.  Bail on any potentially-aliasing intervening write
         * (call, indirect store, store through escaped pointer) or any
         * non-immediate definition. */
        if (!got[oi] && cmp_bb && vr >= 0 &&
            TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_VAR) {
          int var_pos = TCCIR_DECODE_VREG_POSITION(vr);
          for (int k = cmp_idx - 1; k >= cmp_bb->start_idx; k--) {
            IRQuadCompact *kq = &ir->compact_instructions[k];
            if (kq->op == TCCIR_OP_NOP)
              continue;
            if (kq->op == TCCIR_OP_FUNCCALLVOID || kq->op == TCCIR_OP_FUNCCALLVAL)
              break;
            if (kq->op == TCCIR_OP_STORE_INDEXED || kq->op == TCCIR_OP_STORE_POSTINC)
              break;  /* may alias VAR through pointer arithmetic */
            /* Any op that defines this VAR.  STORE/STORE_INDEXED dests
             * always have is_lval=1 — non-lval VAR dest signals direct
             * write to the var slot; lval dest is a write through *V. */
            if (irop_config[kq->op].has_dest) {
              IROperand kd = tcc_ir_op_get_dest(ir, kq);
              int32_t kdv = irop_get_vreg(kd);
              if (kdv >= 0 &&
                  TCCIR_DECODE_VREG_TYPE(kdv) == TCCIR_VREG_TYPE_VAR &&
                  TCCIR_DECODE_VREG_POSITION(kdv) == var_pos) {
                /* Found the most recent def of this VAR.  Try to extract
                 * an immediate value. */
                if (kq->op == TCCIR_OP_ASSIGN || kq->op == TCCIR_OP_STORE) {
                  IROperand ks = tcc_ir_op_get_src1(ir, kq);
                  if (irop_is_immediate(ks) && !ks.is_lval) {
                    vals[oi] = irop_get_imm64_ex(ir, ks);
                    got[oi] = 1;
                  }
                }
                break;
              }
            }
            if (kq->op == TCCIR_OP_STORE) {
              IROperand kd = tcc_ir_op_get_dest(ir, kq);
              /* STORE to a different VAR slot or to a known stack slot
               * cannot alias this VAR. */
              if (kd.tag == IROP_TAG_STACKOFF && kd.is_local && kd.is_lval)
                continue;
              /* TEMP-DEREF or global STORE: could alias through escaped
               * pointers.  Bail. */
              break;
            }
          }
        }
      }
    }
    if (got[0] && got[1]) {
      v1 = vals[0];
      v2 = vals[1];
      /* Truncate to operand width to avoid sign-extension mismatches */
      int cmp_btype = irop_get_btype(src1);
      if (cmp_btype != IROP_BTYPE_INT64) {
        v1 = (int64_t)(int32_t)(uint32_t)v1;
        v2 = (int64_t)(int32_t)(uint32_t)v2;
      }
      have_values = 1;
    }
  }

  /* Case 2: both operands resolve to the same SSA TEMP vreg (CMP x, x).
   * Chase single-def ASSIGN copies to find the root vreg.
   * Use 0,0 as representative — all reflexive comparisons give the
   * same boolean result regardless of the actual value. */
  if (!have_values) {
    int32_t vr1 = irop_get_vreg(src1);
    int32_t vr2 = irop_get_vreg(src2);

    /* Chase ASSIGN copies: T6 = T0 → root is T0 */
    for (int hop = 0; hop < 4 && vr1 >= 0; hop++) {
      IRSSAVregInfo *vi = ssa_opt_vinfo(ctx, vr1);
      if (!vi || vi->def_instr < 0 || vi->def_count > 1) break;
      IRQuadCompact *dq = &ir->compact_instructions[vi->def_instr];
      if (dq->op != TCCIR_OP_ASSIGN) break;
      IROperand ds = tcc_ir_op_get_src1(ir, dq);
      if (ds.is_lval || ds.tag != IROP_TAG_VREG) break;
      int32_t nv = irop_get_vreg(ds);
      if (nv < 0 || TCCIR_DECODE_VREG_TYPE(nv) != TCCIR_VREG_TYPE_TEMP) break;
      vr1 = nv;
    }
    for (int hop = 0; hop < 4 && vr2 >= 0; hop++) {
      IRSSAVregInfo *vi = ssa_opt_vinfo(ctx, vr2);
      if (!vi || vi->def_instr < 0 || vi->def_count > 1) break;
      IRQuadCompact *dq = &ir->compact_instructions[vi->def_instr];
      if (dq->op != TCCIR_OP_ASSIGN) break;
      IROperand ds = tcc_ir_op_get_src1(ir, dq);
      if (ds.is_lval || ds.tag != IROP_TAG_VREG) break;
      int32_t nv = irop_get_vreg(ds);
      if (nv < 0 || TCCIR_DECODE_VREG_TYPE(nv) != TCCIR_VREG_TYPE_TEMP) break;
      vr2 = nv;
    }

    if (vr1 >= 0 && vr1 == vr2 &&
        TCCIR_DECODE_VREG_TYPE(vr1) == TCCIR_VREG_TYPE_TEMP &&
        src1.is_lval == src2.is_lval &&
        src1.tag == IROP_TAG_VREG && src2.tag == IROP_TAG_VREG) {
      v1 = 0;
      v2 = 0;
      have_values = 1;
    }
  }

  if (!have_values)
    return 0;

  int j = cmp_idx + 1;
  while (j < n && ir->compact_instructions[j].op == TCCIR_OP_NOP)
    j++;
  if (j >= n)
    return 0;

  IRQuadCompact *next_q = &ir->compact_instructions[j];

  if (next_q->op == TCCIR_OP_JUMPIF) {
    IROperand cond = tcc_ir_op_get_src1(ir, next_q);
    int tok = (int)irop_get_imm64_ex(ir, cond);
    int result = eval_cond(v1, v2, tok);
    if (result < 0)
      return 0;

    /* Remove uses of CMP operands */
    IRSSAVregInfo *vi;
    vi = ssa_opt_vinfo(ctx, irop_get_vreg(src1));
    if (vi) ssa_opt_remove_use_instr(vi, cmp_idx);
    vi = ssa_opt_vinfo(ctx, irop_get_vreg(src2));
    if (vi) ssa_opt_remove_use_instr(vi, cmp_idx);

    /* Identify the dead edge so we can prune corresponding phi operands.
     * Without this, phi resolution still emits copies for that edge, which
     * surface as dead writes to spilled carrier vregs. */
    int jumpif_block = ssa_block_for_instr(ctx->cfg, j);
    int target_idx = (int)irop_get_imm64_ex(ir, tcc_ir_op_get_dest(ir, next_q));
    int target_block = ssa_block_for_instr(ctx->cfg, target_idx);
    int fallthru_block = ssa_block_for_instr(ctx->cfg, j + 1);

    if (result) {
      IROperand dest = tcc_ir_op_get_dest(ir, next_q);
      cmp_q->op = TCCIR_OP_NOP;
      next_q->op = TCCIR_OP_JUMP;
      tcc_ir_set_dest(ir, j, dest);
      /* Fall-through edge dies. */
      if (jumpif_block >= 0 && fallthru_block >= 0 && fallthru_block != target_block)
        ssa_drop_phi_edge(ctx, jumpif_block, fallthru_block);
    } else {
      cmp_q->op = TCCIR_OP_NOP;
      next_q->op = TCCIR_OP_NOP;
      /* Target edge dies; fall-through is the surviving path. */
      if (jumpif_block >= 0 && target_block >= 0 && target_block != fallthru_block)
        ssa_drop_phi_edge(ctx, jumpif_block, target_block);
    }
    return 1;
  }

  if (next_q->op == TCCIR_OP_SETIF) {
    IROperand cond = tcc_ir_op_get_src1(ir, next_q);
    int tok = (int)irop_get_imm64_ex(ir, cond);
    int result = eval_cond(v1, v2, tok);
    if (result < 0)
      return 0;

    IRSSAVregInfo *vi;
    vi = ssa_opt_vinfo(ctx, irop_get_vreg(src1));
    if (vi) ssa_opt_remove_use_instr(vi, cmp_idx);
    vi = ssa_opt_vinfo(ctx, irop_get_vreg(src2));
    if (vi) ssa_opt_remove_use_instr(vi, cmp_idx);

    IROperand dest = tcc_ir_op_get_dest(ir, next_q);
    IROperand imm = irop_make_imm32(0, result ? 1 : 0, dest.btype);
    cmp_q->op = TCCIR_OP_NOP;
    next_q->op = TCCIR_OP_ASSIGN;
    tcc_ir_set_src1(ir, j, imm);
    tcc_ir_set_src2(ir, j, IROP_NONE);
    return 1;
  }

  return 0;
}

static int ssa_fold_test_zero(IRSSAOptCtx *ctx, int tz_idx)
{
  TCCIRState *ir = ctx->ir;
  IRQuadCompact *tz_q = &ir->compact_instructions[tz_idx];
  int n = ir->next_instruction_index;

  IROperand src1 = tcc_ir_op_get_src1(ir, tz_q);
  if (!irop_is_immediate(src1))
    return 0;

  int64_t val = irop_get_imm64_ex(ir, src1);

  int j = tz_idx + 1;
  while (j < n && ir->compact_instructions[j].op == TCCIR_OP_NOP)
    j++;
  if (j >= n)
    return 0;

  IRQuadCompact *next_q = &ir->compact_instructions[j];
  if (next_q->op != TCCIR_OP_JUMPIF)
    return 0;

  IROperand cond = tcc_ir_op_get_src1(ir, next_q);
  int tok = (int)irop_get_imm64_ex(ir, cond);

  int branch_taken;
  if (tok == 0x94)
    branch_taken = (val == 0);
  else if (tok == 0x95)
    branch_taken = (val != 0);
  else
    return 0;

  if (branch_taken) {
    IROperand dest = tcc_ir_op_get_dest(ir, next_q);
    tz_q->op = TCCIR_OP_NOP;
    next_q->op = TCCIR_OP_JUMP;
    tcc_ir_set_dest(ir, j, dest);
  } else {
    tz_q->op = TCCIR_OP_NOP;
    next_q->op = TCCIR_OP_NOP;
  }
  return 1;
}

/* Compute block reachability based on the IR's current terminators (not the
 * statically-built CFG succs/preds, which aren't updated when JUMPIFs are
 * folded to JUMPs).  Returns a malloc'd uint8_t array of size cfg->num_blocks
 * with 1 = reachable from entry, 0 = unreachable.  Caller frees. */
static uint8_t *ssa_compute_reachable_blocks(IRSSAOptCtx *ctx)
{
  IRCFG *cfg = ctx->cfg;
  TCCIRState *ir = ctx->ir;
  if (!cfg || cfg->num_blocks <= 0) return NULL;

  int nb = cfg->num_blocks;
  int n_instrs = ir->next_instruction_index;
  uint8_t *reachable = tcc_mallocz(nb);
  int *worklist = tcc_malloc(nb * sizeof(int));
  int wl_head = 0, wl_tail = 0;

  /* Entry = block containing instruction 0.  Conservative fallback: if the
   * function is empty or the mapping is bad, assume all blocks reachable. */
  int entry = (cfg->num_instrs > 0) ? cfg->instr_to_block[0] : -1;
  if (entry < 0 || entry >= nb) {
    for (int i = 0; i < nb; i++) reachable[i] = 1;
    tcc_free(worklist);
    return reachable;
  }

  reachable[entry] = 1;
  worklist[wl_tail++] = entry;

#define MARK(blk_)                                                            \
  do {                                                                        \
    int _b = (blk_);                                                          \
    if (_b >= 0 && _b < nb && !reachable[_b]) {                               \
      reachable[_b] = 1;                                                      \
      worklist[wl_tail++] = _b;                                               \
    }                                                                         \
  } while (0)

  while (wl_head < wl_tail) {
    int b = worklist[wl_head++];
    IRBasicBlock *bb = &cfg->blocks[b];

    /* Find terminator: last non-NOP instruction in the block. */
    int term = -1;
    for (int i = bb->end_idx - 1; i >= bb->start_idx; i--) {
      if (ir->compact_instructions[i].op != TCCIR_OP_NOP) {
        term = i;
        break;
      }
    }

    /* Helper: fall through to the block containing bb->end_idx. */
    int fall_block = -1;
    if (bb->end_idx < n_instrs)
      fall_block = cfg->instr_to_block[bb->end_idx];

    if (term < 0) {
      /* All NOPs: fall through. */
      MARK(fall_block);
      continue;
    }

    IRQuadCompact *q = &ir->compact_instructions[term];
    if (q->op == TCCIR_OP_JUMP) {
      int target = (int)tcc_ir_op_get_dest(ir, q).u.imm32;
      int tb = (target >= 0 && target < cfg->num_instrs) ?
               cfg->instr_to_block[target] : -1;
      MARK(tb);
    } else if (q->op == TCCIR_OP_JUMPIF) {
      int target = (int)tcc_ir_op_get_dest(ir, q).u.imm32;
      int tb = (target >= 0 && target < cfg->num_instrs) ?
               cfg->instr_to_block[target] : -1;
      MARK(tb);
      MARK(fall_block);
    } else if (q->op == TCCIR_OP_RETURNVALUE || q->op == TCCIR_OP_RETURNVOID ||
               q->op == TCCIR_OP_TRAP) {
      /* No successors. */
    } else if (q->op == TCCIR_OP_IJUMP || q->op == TCCIR_OP_SWITCH_TABLE) {
      /* Conservative: keep all CFG successors reachable. */
      for (int si = 0; si < bb->num_succs; si++)
        MARK(bb->succs[si]);
    } else {
      MARK(fall_block);
    }
  }

#undef MARK

  tcc_free(worklist);
  return reachable;
}

/* After branch folding, blocks may be transitively unreachable.  Walk every
 * phi in the function and drop operands whose pred_block is no longer
 * reachable.  This is what unblocks SCCP/cprop on values like
 *   merge_phi(rA from live_block, rB from now-dead_block)
 * which previously kept def_count > 1 and prevented constant folding. */
static int ssa_branch_prune_unreachable_phis(IRSSAOptCtx *ctx)
{
  if (!ctx->ssa || !ctx->ssa->block_phis || !ctx->cfg) return 0;

  uint8_t *reachable = ssa_compute_reachable_blocks(ctx);
  if (!reachable) return 0;

  int nb = ctx->cfg->num_blocks;
  int changes = 0;

  /* For each block whose phis we want to clean, gather the unique set of
   * unreachable predecessors and drop each in turn.  ssa_drop_phi_edge
   * walks every phi at the target and removes all matching operands, so
   * one call per (dead_pred, target_block) pair handles all phis there. */
  uint8_t *seen_pred = tcc_malloc(nb);
  for (int b = 0; b < nb; b++) {
    if (!reachable[b]) continue;
    if (!ctx->ssa->block_phis[b]) continue;

    memset(seen_pred, 0, nb);
    int has_dead = 0;
    for (IRPhiNode *phi = ctx->ssa->block_phis[b]; phi; phi = phi->next) {
      for (int i = 0; i < phi->num_operands; i++) {
        int pred = phi->operands[i].pred_block;
        if (pred >= 0 && pred < nb && !reachable[pred] && !seen_pred[pred]) {
          seen_pred[pred] = 1;
          has_dead = 1;
        }
      }
    }
    if (!has_dead) continue;

    /* Count operands before drop for change accounting. */
    int before = 0;
    for (IRPhiNode *phi = ctx->ssa->block_phis[b]; phi; phi = phi->next)
      before += phi->num_operands;

    for (int p = 0; p < nb; p++) {
      if (seen_pred[p])
        ssa_drop_phi_edge(ctx, p, b);
    }

    int after = 0;
    for (IRPhiNode *phi = ctx->ssa->block_phis[b]; phi; phi = phi->next)
      after += phi->num_operands;
    changes += before - after;
  }
  tcc_free(seen_pred);

  tcc_free(reachable);
  return changes;
}

static const IRSSAOptGen branch_gens[] = {
  { TCCIR_OP_CMP,       ssa_fold_cmp_jumpif, "branch_cmp" },
  { TCCIR_OP_TEST_ZERO, ssa_fold_test_zero,  "branch_tz" },
};

int ssa_opt_branch(IRSSAOptCtx *ctx)
{
  int changes = ssa_opt_run_gens(ctx, branch_gens,
                                 sizeof(branch_gens) / sizeof(branch_gens[0]));
  /* Folding may have created transitively-unreachable blocks whose phi
   * operands still pollute multi-def merges.  Prune them so SCCP/cprop on
   * the next iteration sees clean single-def values. */
  changes += ssa_branch_prune_unreachable_phis(ctx);
  return changes;
}
