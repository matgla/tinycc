/*
 *  TCC IR - SSA Branch Folding
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#define USING_GLOBALS
#include "ir.h"
#include "ssa_opt.h"
#include "opt_alias.h"
#include "opt_utils.h"
#include "opt/ssa/branch.h"
#include "memory/small_sequence.h"

TCC_SMALL_SEQUENCE_DEFINE(BranchIntSeq, int, 128)
TCC_SMALL_SEQUENCE_DEFINE(BranchU8Seq, uint8_t, 128)

/* Result of a comparison token over known values; -1 = unknown token. */
static int eval_cond(int64_t v1, int64_t v2, int tok)
{
  switch (tok) {
  case TOK_EQ:  return v1 == v2;
  case TOK_NE:  return v1 != v2;
  case TOK_LT:  return v1 < v2;
  case TOK_GE:  return v1 >= v2;
  case TOK_LE:  return v1 <= v2;
  case TOK_GT:  return v1 > v2;
  case TOK_ULT: return (uint64_t)v1 < (uint64_t)v2;
  case TOK_UGE: return (uint64_t)v1 >= (uint64_t)v2;
  case TOK_ULE: return (uint64_t)v1 <= (uint64_t)v2;
  case TOK_UGT: return (uint64_t)v1 > (uint64_t)v2;
  default: return -1;
  }
}

static int ssa_block_for_instr(IRCFG *cfg, int instr_idx)
{
  if (!cfg || !cfg->instr_to_block) return -1;
  /* Later passes append instructions past instr_to_block's num_instrs sizing. */
  if (instr_idx < 0 || instr_idx >= cfg->num_instrs) return -1;
  return cfg->instr_to_block[instr_idx];
}

/* ---- phi and CFG maintenance ---- */

/* Drop phi operands flowing from `dead_pred_block` into phis at `target_block_idx`. */
void ssa_drop_phi_edge(IRSSAOptCtx *ctx, int dead_pred_block,
                       int target_block_idx)
{
  if (!ctx->ssa || !ctx->ssa->block_phis || !ctx->cfg) return;
  if (target_block_idx < 0 || target_block_idx >= ctx->cfg->num_blocks) return;

  for (IRPhiNode *phi = ctx->ssa->block_phis[target_block_idx]; phi; phi = phi->next) {
    int r = 0;
    while (r < phi->num_operands) {
      if (phi->operands[r].pred_block != dead_pred_block) {
        r++;
        continue;
      }

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

      /* Shifting operands down must decrement their vinfo slots to match. */
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
      phi->num_operands--;  /* don't advance r: r now holds the shifted operand */
    }
  }
}

/* Block reachability from the IR's current terminators, not the statically-built
 * CFG succs/preds (stale once a JUMPIF folds to a JUMP).  Returns a malloc'd
 * array of cfg->num_blocks flags, 1 = reachable from entry.  Caller frees. */
uint8_t *ssa_opt_compute_reachable_blocks(IRSSAOptCtx *ctx)
{
  IRCFG *cfg = ctx->cfg;
  TCCIRState *ir = ctx->ir;
  if (!cfg || cfg->num_blocks <= 0) return NULL;

  int nb = cfg->num_blocks;
  int n_instrs = cfg->num_instrs;   /* instr_to_block[] is sized to this */
  uint8_t *reachable = tcc_mallocz(nb);
  small_sequence(BranchIntSeq) worklist_owner = {0};
  BranchIntSeq_init(&worklist_owner, (size_t)nb);
  int *worklist = BranchIntSeq_data(&worklist_owner);
  int wl_head = 0, wl_tail = 0;

  /* Entry = block containing instruction 0; an empty function or bad mapping
   * falls back to assuming everything reachable. */
  int entry = (cfg->num_instrs > 0) ? cfg->instr_to_block[0] : -1;
  if (entry < 0 || entry >= nb) {
    for (int i = 0; i < nb; i++) reachable[i] = 1;
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

    /* Terminator = last non-NOP instruction in the block. */
    int term = -1;
    for (int i = bb->end_idx - 1; i >= bb->start_idx; i--) {
      if (ir->compact_instructions[i].op != TCCIR_OP_NOP) {
        term = i;
        break;
      }
    }

    int fall_block = -1;
    if (bb->end_idx < n_instrs)
      fall_block = cfg->instr_to_block[bb->end_idx];

    if (term < 0) {  /* all NOPs: fall through */
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

  return reachable;
}

/* Drop phi operands whose pred_block became unreachable, unblocking SCCP/cprop
 * on merges the dead operand kept multi-def.  Every pass that folds a branch
 * must call this: a stale operand whose def DCE later NOPs is unresolvable. */
int ssa_opt_prune_unreachable_phis(IRSSAOptCtx *ctx)
{
  if (!ctx->ssa || !ctx->ssa->block_phis || !ctx->cfg) return 0;

  uint8_t *reachable = ssa_opt_compute_reachable_blocks(ctx);
  if (!reachable) return 0;

  int nb = ctx->cfg->num_blocks;
  int changes = 0;

  /* ssa_drop_phi_edge handles every phi at the target, so one call per
   * (dead_pred, block) pair suffices. */
  small_sequence(BranchU8Seq) seen_pred_owner = {0};
  BranchU8Seq_init(&seen_pred_owner, (size_t)nb);
  uint8_t *seen_pred = BranchU8Seq_data(&seen_pred_owner);
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

  tcc_free(reachable);
  return changes;
}

/* Rewrite the decided JUMPIF at `j` into JUMP (taken) or NOP (not taken) and
 * prune the dead edge's phi operands — otherwise phi resolution still emits
 * copies for that edge, surfacing as dead writes to spilled carrier vregs. */
static void ssa_rewrite_jumpif(IRSSAOptCtx *ctx, int j, int taken)
{
  TCCIRState *ir = ctx->ir;
  IRQuadCompact *jump_q = &ir->compact_instructions[j];
  int jumpif_block = ssa_block_for_instr(ctx->cfg, j);
  int target_idx = (int)irop_get_imm64_ex(ir, tcc_ir_op_get_dest(ir, jump_q));
  int target_block = ssa_block_for_instr(ctx->cfg, target_idx);
  int fallthru_block = ssa_block_for_instr(ctx->cfg, j + 1);

  if (taken) {
    IROperand dest = tcc_ir_op_get_dest(ir, jump_q);
    jump_q->op = TCCIR_OP_JUMP;
    tcc_ir_set_dest(ir, j, dest);
    /* Fall-through edge dies. */
    if (jumpif_block >= 0 && fallthru_block >= 0 && fallthru_block != target_block)
      ssa_drop_phi_edge(ctx, jumpif_block, fallthru_block);
  } else {
    jump_q->op = TCCIR_OP_NOP;
    /* Target edge dies; fall-through is the surviving path. */
    if (jumpif_block >= 0 && target_block >= 0 && target_block != fallthru_block)
      ssa_drop_phi_edge(ctx, jumpif_block, target_block);
  }
}

/* Condition token of the flag consumer at `j`, or -1 when it isn't a foldable
 * JUMPIF/SETIF/SELECT.  A SELECT is foldable only when no later instruction
 * still reads the CMP flags (the fold NOPs the CMP) and it has no barrel. */
static int ssa_flag_consumer_tok(IRSSAOptCtx *ctx, int j)
{
  TCCIRState *ir = ctx->ir;
  IRQuadCompact *q = &ir->compact_instructions[j];
  if (q->op == TCCIR_OP_JUMPIF || q->op == TCCIR_OP_SETIF)
    return (int)irop_get_imm64_ex(ir, tcc_ir_op_get_src1(ir, q));
  if (q->op != TCCIR_OP_SELECT)
    return -1;
  if (tcc_ir_barrel_shift_at(ir, q) != 0)
    return -1;
  int n = ir->next_instruction_index;
  int k = ir_skip_nops_forward(ir, j + 1, n);
  if (k < n) {
    int kop = ir->compact_instructions[k].op;
    if (kop == TCCIR_OP_SETIF || kop == TCCIR_OP_SELECT ||
        kop == TCCIR_OP_JUMPIF)
      return -1;
  }
  return (int)irop_get_imm64_ex(ir, tcc_ir_op_get_cond(ir, q));
}

/* Rewrite the decided flag consumer at `j` for comparison outcome `result`:
 * JUMPIF folds to JUMP/NOP, SETIF to ASSIGN #0/#1, SELECT to an ASSIGN of the
 * winning arm (dropping the losing arm's use). */
static void ssa_rewrite_flag_consumer(IRSSAOptCtx *ctx, int j, int result)
{
  TCCIRState *ir = ctx->ir;
  IRQuadCompact *q = &ir->compact_instructions[j];
  if (q->op == TCCIR_OP_JUMPIF) {
    ssa_rewrite_jumpif(ctx, j, result);
  } else if (q->op == TCCIR_OP_SETIF) {
    IROperand dest = tcc_ir_op_get_dest(ir, q);
    q->op = TCCIR_OP_ASSIGN;
    tcc_ir_set_src1(ir, j, irop_make_imm32(0, result, dest.btype));
    tcc_ir_set_src2(ir, j, IROP_NONE);
  } else {  /* SELECT: dest = cond ? src1 : src2 */
    IROperand chosen = result ? tcc_ir_op_get_src1(ir, q)
                              : tcc_ir_op_get_src2(ir, q);
    IROperand dropped = result ? tcc_ir_op_get_src2(ir, q)
                               : tcc_ir_op_get_src1(ir, q);
    IROperand dest = tcc_ir_op_get_dest(ir, q);
    q->op = TCCIR_OP_ASSIGN;
    tcc_ir_set_src1(ir, j, chosen);
    tcc_ir_op_set_dest(ir, q, dest);
    IRSSAVregInfo *dvi = ssa_opt_vinfo(ctx, irop_get_vreg(dropped));
    if (dvi)
      ssa_opt_remove_use_instr(dvi, j);
  }
}

/* ---- bool normalisation: CMP X,#0 ; SETIF NE ---- */

/* A vreg is provably in {0,1} when its single def is a SETIF, a boolean AND/OR,
 * a single-bit UBFX, or an `AND #1` mask. */
static int ssa_vreg_is_bool01(IRSSAOptCtx *ctx, int32_t vr)
{
  if (vr < 0)
    return 0;
  IRSSAVregInfo *vi = ssa_opt_vinfo(ctx, vr);
  if (!vi || vi->def_count != 1 || vi->def_instr < 0)
    return 0;
  TCCIRState *ir = ctx->ir;
  IRQuadCompact *dq = &ir->compact_instructions[vi->def_instr];
  switch (dq->op) {
  case TCCIR_OP_SETIF:
  case TCCIR_OP_BOOL_AND:
  case TCCIR_OP_BOOL_OR:
    return 1;
  case TCCIR_OP_UBFX:
  case TCCIR_OP_AND: {
    if (irop_needs_pair(tcc_ir_op_get_dest(ir, dq)) ||
        tcc_ir_barrel_shift_at(ir, dq) != 0)
      return 0;
    IROperand d2 = tcc_ir_op_get_src2(ir, dq);
    if (!irop_is_immediate(d2) || d2.is_lval)
      return 0;
    int64_t p = irop_get_imm64_ex(ir, d2);
    /* UBFX src2 packs lsb | width<<5; a 1-bit field is {0,1}. */
    return dq->op == TCCIR_OP_UBFX ? ((p >> 5) & 63) == 1 : p == 1;
  }
  default:
    return 0;
  }
}

/* `CMP X,#0 ; SETIF NE` with X already {0,1} collapses to `V <- X`. */
static int ssa_bool_norm(IRSSAOptCtx *ctx, int cmp_idx)
{
  TCCIRState *ir = ctx->ir;
  int n = ir->next_instruction_index;
  IRQuadCompact *cmp_q = &ir->compact_instructions[cmp_idx];

  IROperand cmp_s2 = tcc_ir_op_get_src2(ir, cmp_q);
  if (!irop_is_immediate(cmp_s2) || cmp_s2.is_sym || cmp_s2.is_lval)
    return 0;
  if (irop_get_imm64_ex(ir, cmp_s2) != 0)
    return 0;

  IROperand cmp_s1 = tcc_ir_op_get_src1(ir, cmp_q);
  int32_t vr = irop_get_vreg(cmp_s1);
  if (vr < 0 || cmp_s1.is_lval || cmp_s1.is_sym)
    return 0;
  if (!ssa_vreg_is_bool01(ctx, vr))
    return 0;

  int j = ir_skip_nops_forward(ir, cmp_idx + 1, n);
  if (j >= n)
    return 0;
  IRQuadCompact *setif_q = &ir->compact_instructions[j];
  if (setif_q->op != TCCIR_OP_SETIF)
    return 0;
  if ((int)irop_get_imm64_ex(ir, tcc_ir_op_get_src1(ir, setif_q)) != TOK_NE)
    return 0;

  ssa_opt_nop_instr(ctx, cmp_idx);
  setif_q->op = TCCIR_OP_ASSIGN;
  tcc_ir_set_src1(ir, j, irop_make_vreg(vr, irop_get_btype(cmp_s1)));
  tcc_ir_set_src2(ir, j, IROP_NONE);
  IRSSAVregInfo *vi = ssa_opt_vinfo(ctx, vr);
  if (vi)
    ssa_opt_add_use_instr(vi, j);
  return 1;
}

/* ---- CMP value resolution, case 1: both operands constant ---- */

/* Scan `bb` backward from `use_idx` for the most recent unambiguous slot def of
 * VAR `vr`, bailing on any potentially-aliasing write and on volatile VARs.
 * Returns the def instruction index with its source in *src_out, or -1. */
static int ssa_var_block_def_src(IRSSAOptCtx *ctx, IRBasicBlock *bb,
                                 int use_idx, int32_t vr, IROperand *src_out)
{
  TCCIRState *ir = ctx->ir;
  int var_pos = TCCIR_DECODE_VREG_POSITION(vr);
  if (var_pos < ir->variables_live_intervals_size &&
      ir->variables_live_intervals[var_pos].is_volatile)
    return -1;
  for (int k = use_idx - 1; k >= bb->start_idx; k--) {
    IRQuadCompact *kq = &ir->compact_instructions[k];
    if (kq->op == TCCIR_OP_NOP)
      continue;
    if (kq->op == TCCIR_OP_FUNCCALLVOID || kq->op == TCCIR_OP_FUNCCALLVAL)
      return -1;
    if (kq->op == TCCIR_OP_STORE_INDEXED || kq->op == TCCIR_OP_STORE_POSTINC)
      return -1;  /* may alias VAR through pointer arithmetic */
    /* A slot write has kd.is_local=1; a pointer-deref through V inherited
     * is_local=0 from a TEMP and writes V's pointee, not V's slot. */
    if (irop_config[kq->op].has_dest) {
      IROperand kd = tcc_ir_op_get_dest(ir, kq);
      int32_t kdv = irop_get_vreg(kd);
      if (kdv >= 0 &&
          TCCIR_DECODE_VREG_TYPE(kdv) == TCCIR_VREG_TYPE_VAR &&
          TCCIR_DECODE_VREG_POSITION(kdv) == var_pos) {
        if (kq->op == TCCIR_OP_STORE && kd.is_lval && !kd.is_local)
          continue;
        if (kq->op == TCCIR_OP_ASSIGN || kq->op == TCCIR_OP_STORE) {
          *src_out = tcc_ir_op_get_src1(ir, kq);
          return k;
        }
        return -1;
      }
    }
    if (kq->op == TCCIR_OP_STORE) {
      IROperand kd = tcc_ir_op_get_dest(ir, kq);
      if (kd.tag == IROP_TAG_STACKOFF && kd.is_local && kd.is_lval)
        continue;  /* a known stack slot cannot alias this VAR */
      return -1;   /* TEMP-DEREF or global STORE: may alias via escaped pointer */
    }
  }
  return -1;
}

/* Constant value of a CMP operand: an immediate, a single-def TEMP defined by
 * `ASSIGN #imm`, or a VAR whose reaching same-block slot def stored one. */
static int ssa_operand_const(IRSSAOptCtx *ctx, IRBasicBlock *cmp_bb, int cmp_idx,
                             IROperand op, int64_t *val)
{
  TCCIRState *ir = ctx->ir;
  if (irop_is_immediate(op)) {
    *val = irop_get_imm64_ex(ir, op);
    return 1;
  }
  int32_t vr = irop_get_vreg(op);
  if (vr < 0)
    return 0;
  if (TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_TEMP &&
      op.tag == IROP_TAG_VREG && !op.is_lval) {
    IRSSAVregInfo *vi = ssa_opt_vinfo(ctx, vr);
    if (vi && vi->def_instr >= 0 && vi->def_count <= 1 && vi->def_phi_block < 0) {
      IRQuadCompact *dq = &ir->compact_instructions[vi->def_instr];
      if (dq->op == TCCIR_OP_ASSIGN) {
        IROperand ds = tcc_ir_op_get_src1(ir, dq);
        if (irop_is_immediate(ds) && !ds.is_lval) {
          *val = irop_get_imm64_ex(ir, ds);
          return 1;
        }
      }
    }
  }
  if (cmp_bb && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_VAR) {
    IROperand ks;
    if (ssa_var_block_def_src(ctx, cmp_bb, cmp_idx, vr, &ks) >= 0 &&
        irop_is_immediate(ks) && !ks.is_lval) {
      *val = irop_get_imm64_ex(ir, ks);
      return 1;
    }
  }
  return 0;
}

static int ssa_cmp_values_const(IRSSAOptCtx *ctx, IRBasicBlock *cmp_bb,
                                int cmp_idx, IROperand src1, IROperand src2,
                                int64_t *v1, int64_t *v2)
{
  if (!ssa_operand_const(ctx, cmp_bb, cmp_idx, src1, v1) ||
      !ssa_operand_const(ctx, cmp_bb, cmp_idx, src2, v2))
    return 0;
  /* Truncate to operand width to avoid sign-extension mismatches. */
  if (irop_get_btype(src1) != IROP_BTYPE_INT64) {
    *v1 = (int64_t)(int32_t)(uint32_t)*v1;
    *v2 = (int64_t)(int32_t)(uint32_t)*v2;
  }
  return 1;
}

/* ---- case 2: both operands are the same SSA value (CMP x, x) ---- */

/* Chase single-def ASSIGN copies to the root TEMP vreg (T6 = T0 → T0). */
static int32_t ssa_chase_temp_copies(IRSSAOptCtx *ctx, int32_t vr)
{
  TCCIRState *ir = ctx->ir;
  for (int hop = 0; hop < 4 && vr >= 0; hop++) {
    IRSSAVregInfo *vi = ssa_opt_vinfo(ctx, vr);
    if (!vi || vi->def_instr < 0 || vi->def_count > 1) break;
    IRQuadCompact *dq = &ir->compact_instructions[vi->def_instr];
    if (dq->op != TCCIR_OP_ASSIGN) break;
    IROperand ds = tcc_ir_op_get_src1(ir, dq);
    if (ds.is_lval || ds.tag != IROP_TAG_VREG) break;
    int32_t nv = irop_get_vreg(ds);
    if (nv < 0 || TCCIR_DECODE_VREG_TYPE(nv) != TCCIR_VREG_TYPE_TEMP) break;
    vr = nv;
  }
  return vr;
}

static int ssa_cmp_values_reflexive(IRSSAOptCtx *ctx, IROperand src1, IROperand src2)
{
  if (src1.tag != IROP_TAG_VREG || src2.tag != IROP_TAG_VREG ||
      src1.is_lval != src2.is_lval)
    return 0;
  int32_t vr1 = ssa_chase_temp_copies(ctx, irop_get_vreg(src1));
  int32_t vr2 = ssa_chase_temp_copies(ctx, irop_get_vreg(src2));
  return vr1 >= 0 && vr1 == vr2 &&
         TCCIR_DECODE_VREG_TYPE(vr1) == TCCIR_VREG_TYPE_TEMP;
}

/* ---- case 3: both operands extract the identical bitfield ---- */

/* Base value a CMP operand extracts its field from: an SSA vreg or a symref load. */
typedef struct {
  int32_t vreg;      /* base value's vreg, or -1 when symref-based */
  struct Sym *sym;   /* non-NULL: base is a load of this symref */
  int32_t addend;
  uint32_t flags;
  int btype;         /* load access width/type — must match to fold */
  int load_pos;      /* instruction holding the (folded) load, for the write scan */
} SsaCmpBase;

/* Same block and no memory write strictly between p1 and p2. */
static int ssa_no_write_between(TCCIRState *ir, IRCFG *cfg, int p1, int p2)
{
  if (!cfg)
    return 0;
  int lo = p1 < p2 ? p1 : p2, hi = p1 < p2 ? p2 : p1;
  if (lo == hi)
    return 1;
  int b = ssa_block_for_instr(cfg, lo);
  if (b < 0 || b != ssa_block_for_instr(cfg, hi))
    return 0;
  for (int k = lo + 1; k < hi; k++) {
    switch (ir->compact_instructions[k].op) {
    case TCCIR_OP_NOP:
      continue;
    case TCCIR_OP_STORE:
    case TCCIR_OP_STORE_INDEXED:
    case TCCIR_OP_STORE_POSTINC:
    case TCCIR_OP_FUNCCALLVOID:
    case TCCIR_OP_FUNCCALLVAL:
      return 0;
    default:
      break;
    }
  }
  return 1;
}

/* Decode CMP operand `slot` (0=src1, 1=src2) into the bitfield it compares:
 * base V, offset `lsb`, `width` bits, sign/zero extension.  Matches `UBFX`,
 * bare `SHR`/`SAR`, and `SHL #a` completed by the CMP's own `LSR/ASR #b`
 * barrel (src2 only): `(V<<a)>>b = UBFX(V, b-a, 32-b)`.  Returns 1 on success. */
static int ssa_cmp_extract_desc(IRSSAOptCtx *ctx, int cmp_idx, int slot,
                                SsaCmpBase *base_out, int *lsb_out, int *width_out,
                                int *sext_out)
{
  TCCIRState *ir = ctx->ir;
  IRQuadCompact *cmp_q = &ir->compact_instructions[cmp_idx];
  IROperand s = (slot == 0) ? tcc_ir_op_get_src1(ir, cmp_q)
                            : tcc_ir_op_get_src2(ir, cmp_q);
  if (s.is_lval || s.tag != IROP_TAG_VREG)
    return 0;
  int32_t vr = irop_get_vreg(s);
  if (vr < 0 || TCCIR_DECODE_VREG_TYPE(vr) != TCCIR_VREG_TYPE_TEMP)
    return 0;

  /* The CMP's barrel shift applies to src2 only. */
  int btype = 0, bamt = 0;
  if (slot == 1) {
    uint8_t bs = tcc_ir_barrel_shift_at(ir, cmp_q);
    if (bs) { btype = (bs >> 5) & 7; bamt = bs & 31; }
  }

  IRSSAVregInfo *vi = ssa_opt_vinfo(ctx, vr);
  if (!vi || vi->def_instr < 0 || vi->def_count > 1)
    return 0;
  IRQuadCompact *dq = &ir->compact_instructions[vi->def_instr];
  if (tcc_ir_barrel_shift_at(ir, dq) != 0)
    return 0;
  if (irop_is_64bit(tcc_ir_op_get_dest(ir, dq)))
    return 0;

  IROperand d1 = tcc_ir_op_get_src1(ir, dq);
  IROperand d2 = tcc_ir_op_get_src2(ir, dq);

  SsaCmpBase base = { -1, NULL, 0, 0, 0, -1 };
  if (d1.is_lval && d1.tag == IROP_TAG_SYMREF) {
    IRPoolSymref *sr = irop_get_symref_ex(ir, d1);
    if (!sr || !sr->sym || (sr->sym->type.t & VT_VOLATILE))
      return 0;
    base.sym = sr->sym; base.addend = sr->addend; base.flags = sr->flags;
    base.btype = irop_get_btype(d1); base.load_pos = vi->def_instr;
  } else if (!d1.is_lval && d1.tag == IROP_TAG_VREG) {
    base.vreg = irop_get_vreg(d1);
    if (base.vreg < 0)
      return 0;
    /* A base vreg that is itself one reload of a symref: record that identity
     * too, so two distinct reloads of the same global can fold. */
    IRSSAVregInfo *bvi = ssa_opt_vinfo(ctx, base.vreg);
    if (bvi && bvi->def_instr >= 0 && bvi->def_count == 1) {
      IRQuadCompact *bd = &ir->compact_instructions[bvi->def_instr];
      if (bd->op == TCCIR_OP_LOAD && tcc_ir_barrel_shift_at(ir, bd) == 0) {
        IROperand ls = tcc_ir_op_get_src1(ir, bd);
        if (ls.is_lval && ls.tag == IROP_TAG_SYMREF) {
          IRPoolSymref *sr = irop_get_symref_ex(ir, ls);
          if (sr && sr->sym && !(sr->sym->type.t & VT_VOLATILE)) {
            base.sym = sr->sym; base.addend = sr->addend; base.flags = sr->flags;
            base.btype = irop_get_btype(ls); base.load_pos = bvi->def_instr;
          }
        }
      }
    }
  } else {
    return 0;
  }

  int lsb, width, sext, def_shl = -1;
  switch (dq->op) {
  case TCCIR_OP_UBFX:
    if (!irop_is_immediate(d2)) return 0;
    { int p = (int)irop_get_imm64_ex(ir, d2); lsb = p & 31; width = (p >> 5) & 63; sext = 0; }
    break;
  case TCCIR_OP_SHR:
  case TCCIR_OP_SAR:
    if (!irop_is_immediate(d2)) return 0;
    { int sft = (int)irop_get_imm64_ex(ir, d2);
      if (sft < 0 || sft > 31) return 0;
      lsb = sft; width = 32 - sft; sext = (dq->op == TCCIR_OP_SAR); }
    break;
  case TCCIR_OP_SHL:
    if (!irop_is_immediate(d2)) return 0;
    def_shl = (int)irop_get_imm64_ex(ir, d2);
    if (def_shl < 1 || def_shl > 31) return 0;
    lsb = 0; width = 0; sext = 0;
    break;
  default:
    return 0;
  }

  if (def_shl >= 0) {
    /* `V << a` is only a low-aligned field if the CMP's src2 barrel is `>> b`, b >= a. */
    if (slot != 1 || (btype != 2 && btype != 3) || bamt < def_shl || bamt > 31)
      return 0;
    lsb = bamt - def_shl; width = 32 - bamt; sext = (btype == 3);
  } else if (slot == 1 && btype) {
    return 0;  /* extra barrel on an extracted operand composes an unmodelled shift */
  }

  if (width <= 0 || width > 32 || lsb < 0 || lsb > 31)
    return 0;

  *base_out = base; *lsb_out = lsb; *width_out = width; *sext_out = sext;
  return 1;
}

/* Both operands extract the identical bitfield via possibly-different forms
 * (UBFX vs SHL+CMP-barrel) that GVN cannot syntactically coalesce, so the
 * compared values are bit-identical and the compare is reflexive. */
static int ssa_cmp_values_bitfield(IRSSAOptCtx *ctx, int cmp_idx)
{
  SsaCmpBase b1, b2;
  int l1, l2, w1, w2, x1, x2;
  if (!ssa_cmp_extract_desc(ctx, cmp_idx, 0, &b1, &l1, &w1, &x1) ||
      !ssa_cmp_extract_desc(ctx, cmp_idx, 1, &b2, &l2, &w2, &x2) ||
      l1 != l2 || w1 != w2 || x1 != x2)
    return 0;
  if (b1.vreg >= 0 && b1.vreg == b2.vreg)
    return 1;  /* identical SSA base value */
  /* Two clean reloads of one global. */
  return b1.sym && b1.sym == b2.sym && b1.addend == b2.addend &&
         b1.flags == b2.flags && b1.btype == b2.btype &&
         ssa_no_write_between(ctx->ir, ctx->cfg, b1.load_pos, b2.load_pos);
}

/* ---- case 4: both operands are addresses of stack locals ---- */

/* Stack-address identity: >=0 is a VAR position (offset = pre-RA delta from its
 * base); SSA_SA_SLOT is a local_stack slot, whose offset is its real byte. */
#define SSA_SA_SLOT (-2)

/* Resolve a CMP operand to a stack-local address: a direct `&V`/`Addr[StackLoc]`,
 * a single-def TEMP chased through ASSIGN copies, or a VAR read whose reaching
 * same-block slot def stored one. */
static int ssa_resolve_stack_addr(IRSSAOptCtx *ctx, IROperand op, int use_idx,
                                  IRBasicBlock *use_bb,
                                  int32_t *id_out, int64_t *off_out)
{
  TCCIRState *ir = ctx->ir;
  for (int hop = 0; hop < 4; hop++) {
    if (is_stack_address_operand(op)) {
      if (op.is_llocal)
        return 0;
      int64_t off = irop_get_stack_offset(op);
      /* Addresses model as bias+offset; keep offsets far from the bias so
       * signed and unsigned orderings agree. */
      if (off <= -((int64_t)1 << 29) || off >= ((int64_t)1 << 29))
        return 0;
      int32_t vr = irop_get_vreg(op);
      if (vr >= 0 && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_VAR)
        *id_out = TCCIR_DECODE_VREG_POSITION(vr);
      else
        *id_out = SSA_SA_SLOT;
      *off_out = off;
      return 1;
    }
    int32_t vr = irop_get_vreg(op);
    if (vr < 0)
      return 0;
    if (TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_VAR) {
      /* Only a slot reference (STACKOFF-tagged lval) resolves; a deref through
       * a VAR-held pointer is VREG-tagged and must not. */
      if (op.tag != IROP_TAG_STACKOFF || !op.is_local || !op.is_lval)
        return 0;
      if (!use_bb)
        return 0;
      int def_idx = ssa_var_block_def_src(ctx, use_bb, use_idx, vr, &op);
      if (def_idx < 0)
        return 0;
      use_idx = def_idx;
      continue;
    }
    if (TCCIR_DECODE_VREG_TYPE(vr) != TCCIR_VREG_TYPE_TEMP ||
        op.tag != IROP_TAG_VREG || op.is_lval)
      return 0;
    IRSSAVregInfo *vi = ssa_opt_vinfo(ctx, vr);
    if (!vi || vi->def_instr < 0 || vi->def_count > 1)
      return 0;
    IRQuadCompact *dq = &ir->compact_instructions[vi->def_instr];
    if (dq->op != TCCIR_OP_LEA && dq->op != TCCIR_OP_ASSIGN &&
        dq->op != TCCIR_OP_LOAD)
      return 0;
    if (tcc_ir_barrel_shift_at(ir, dq) != 0)
      return 0;
    if (irop_is_64bit(tcc_ir_op_get_dest(ir, dq)))
      return 0;
    op = tcc_ir_op_get_src1(ir, dq);
  }
  return 0;
}

/* Same VAR base folds by the embedded offsets.  Distinct VARs are distinct
 * objects, and relational compares across them are UB (C11 6.5.8p5), so any
 * fixed order works.  Two local_stack slots may lie inside one aggregate placed
 * post-RA, and a stack address is only known to be non-null — both prove EQ/NE
 * only, reported via *eqne_only. */
static int ssa_cmp_values_stack_addr(IRSSAOptCtx *ctx, int cmp_idx,
                                     IRBasicBlock *cmp_bb,
                                     IROperand src1, IROperand src2,
                                     int64_t *v1, int64_t *v2, int *eqne_only)
{
  TCCIRState *ir = ctx->ir;
  IRQuadCompact *cmp_q = &ir->compact_instructions[cmp_idx];
  if (tcc_ir_barrel_shift_at(ir, cmp_q) != 0 ||
      irop_is_64bit(src1) || irop_is_64bit(src2))
    return 0;

  int32_t id1, id2;
  int64_t off1, off2;
  const int64_t bias = (int64_t)1 << 30;
  int r1 = ssa_resolve_stack_addr(ctx, src1, cmp_idx, cmp_bb, &id1, &off1);
  int r2 = ssa_resolve_stack_addr(ctx, src2, cmp_idx, cmp_bb, &id2, &off2);

  if (r1 && r2) {
    if (id1 >= 0 && id1 == id2) {
      *v1 = bias + off1;
      *v2 = bias + off2;
      return 1;
    }
    if (id1 >= 0 && id2 >= 0) {
      *v1 = bias;
      *v2 = bias + 1;
      return 1;
    }
    if (id1 == SSA_SA_SLOT && id2 == SSA_SA_SLOT) {
      *v1 = bias + off1;
      *v2 = bias + off2;
      *eqne_only = (off1 != off2);
      return 1;
    }
    return 0;
  }
  /* One side a stack address, the other #0 (a local's address is non-null) or
   * a link-time symbol address (a distinct object, even if weak): EQ/NE
   * decidable, both sides never equal. */
  IROperand other = r1 ? src2 : src1;
  if ((r1 ^ r2) == 0)
    return 0;
  int64_t other_val;
  if (irop_is_immediate(other) && !other.is_lval && !other.is_sym &&
      irop_get_imm64_ex(ir, other) == 0)
    other_val = 0;
  else if (!other.is_lval &&
           (other.tag == IROP_TAG_SYMREF ||
            (irop_is_immediate(other) && other.is_sym)))
    other_val = bias + 1;
  else
    return 0;
  *v1 = r1 ? bias : other_val;
  *v2 = r1 ? other_val : bias;
  *eqne_only = 1;
  return 1;
}

/* ---- case 5: both operands compute the same address/value (SSA analog of flat cmp_expr_fold) ---- */

/* Resolve a CMP operand to a symref identity (sym + addend + deref flag): a
 * direct non-lval symref, a single-def TEMP whose ASSIGN source is one (a symref
 * address is a constant, so chasing it is sound), or an inline lval symref load. */
static int ssa_operand_symref_id(IRSSAOptCtx *ctx, IRBasicBlock *bb, int use_idx,
                                 IROperand op, Sym **sym, int32_t *addend, int *is_deref)
{
  TCCIRState *ir = ctx->ir;
  IROperand s = op;
  /* A register value (VREG, non-lval) or a VAR slot read (STACKOFF, is_lval,
   * is_local) — the latter yields the stored value (an address), not a deref. */
  int32_t vr = -1;
  if (!s.is_sym) {
    if (s.tag == IROP_TAG_VREG && !s.is_lval)
      vr = irop_get_vreg(s);
    else if (s.tag == IROP_TAG_STACKOFF && s.is_lval && s.is_local)
      vr = irop_get_vreg(s);
  }
  int vt = (vr >= 0) ? TCCIR_DECODE_VREG_TYPE(vr) : -1;
  if (vt == TCCIR_VREG_TYPE_TEMP && s.tag == IROP_TAG_VREG) {
    IRSSAVregInfo *vi = ssa_opt_vinfo(ctx, vr);
    if (vi && vi->def_instr >= 0 && vi->def_count <= 1) {
      IRQuadCompact *dq = &ir->compact_instructions[vi->def_instr];
      if (dq->op == TCCIR_OP_ASSIGN) {
        IROperand ds = tcc_ir_op_get_src1(ir, dq);
        if (ds.is_sym && !ds.is_lval)
          s = ds;
      }
    }
  } else if (vt == TCCIR_VREG_TYPE_VAR) {
    /* Single-def, non-address-taken local holding an address constant: its
     * symref value holds everywhere it is live (no reassignment, no aliasing
     * store). addrtaken is sticky (conservative). */
    IRSSAVregInfo *vi = ssa_opt_vinfo(ctx, vr);
    int var_pos = TCCIR_DECODE_VREG_POSITION(vr);
    int addrtaken = 1;
    if (var_pos >= 0 && var_pos < ir->variables_live_intervals_size)
      addrtaken = ir->variables_live_intervals[var_pos].addrtaken;
    if (vi && vi->def_instr >= 0 && vi->def_count == 1 && !addrtaken) {
      IRQuadCompact *dq = &ir->compact_instructions[vi->def_instr];
      if (dq->op == TCCIR_OP_ASSIGN || dq->op == TCCIR_OP_STORE) {
        IROperand ds = tcc_ir_op_get_src1(ir, dq);
        if (ds.is_sym && !ds.is_lval)
          s = ds;
      }
    }
    /* Else a reassignable VAR: its reaching same-block slot def. */
    if (!s.is_sym && bb) {
      IROperand ks;
      if (ssa_var_block_def_src(ctx, bb, use_idx, vr, &ks) >= 0 &&
          ks.is_sym && !ks.is_lval)
        s = ks;
    }
  }
  if (!s.is_sym)
    return 0;
  IRPoolSymref *r = irop_get_symref_ex(ir, s);
  if (!r || !r->sym)
    return 0;
  *sym = r->sym;
  *addend = r->addend;
  *is_deref = s.is_lval;
  return 1;
}

/* A dereferenced global read is foldable only when the global is non-volatile
 * and integer-typed (NaN != NaN, and a volatile read is a mandated access). */
static int ssa_deref_sym_foldable(Sym *sym)
{
  int tt = sym->type.t, bt = tt & VT_BTYPE;
  return !(tt & VT_VOLATILE) && bt != VT_FLOAT && bt != VT_DOUBLE && bt != VT_LDOUBLE;
}

/* Both operands compute the same value: same symref address / same-global deref
 * (R1/R2/R3), or ADD/SUB of an equal immediate over reflexively-equal bases
 * (R4). Feeds the reflexive fold (any value → same result). Floats excluded. */
static int ssa_cmp_values_expr_equal(IRSSAOptCtx *ctx, IRBasicBlock *cmp_bb,
                                     int cmp_idx, IROperand src1, IROperand src2)
{
  TCCIRState *ir = ctx->ir;
  if (irop_get_btype(src1) == IROP_BTYPE_FLOAT32 || irop_get_btype(src1) == IROP_BTYPE_FLOAT64 ||
      irop_get_btype(src2) == IROP_BTYPE_FLOAT32 || irop_get_btype(src2) == IROP_BTYPE_FLOAT64)
    return 0;

  /* R1/R2/R3 symref identity is matched on the resolved (sym, addend, deref)
   * triple, which already distinguishes a deref from an address — so it does
   * not need the operand is_lval to match (a VAR slot holding an address is
   * is_lval yet its value is a non-deref address). */
  {
    Sym *xs1, *xs2;
    int32_t xa1, xa2;
    int xd1, xd2;
    if (ssa_operand_symref_id(ctx, cmp_bb, cmp_idx, src1, &xs1, &xa1, &xd1) &&
        ssa_operand_symref_id(ctx, cmp_bb, cmp_idx, src2, &xs2, &xa2, &xd2) &&
        xs1 == xs2 && xa1 == xa2 && xd1 == xd2)
      return !xd1 || ssa_deref_sym_foldable(xs1);
  }

  if (src1.is_lval != src2.is_lval)
    return 0;

  /* Same register value (CMP x, x) for any non-lval vreg — flat vr1==vr2 case;
   * ssa_cmp_values_reflexive covers only TEMPs, this adds PARAM/VAR. */
  int32_t rv1 = irop_get_vreg(src1), rv2 = irop_get_vreg(src2);
  if (rv1 >= 0 && rv1 == rv2 && !src1.is_lval &&
      src1.tag == IROP_TAG_VREG && src2.tag == IROP_TAG_VREG &&
      irop_get_btype(src1) == irop_get_btype(src2) &&
      src1.is_unsigned == src2.is_unsigned)
    return 1;

  /* R4: T1 = base±K, T2 = base±K with reflexively-equal bases. */
  int32_t vr1 = rv1, vr2 = rv2;
  if (vr1 < 0 || vr2 < 0 || src1.is_lval || src2.is_lval ||
      src1.tag != IROP_TAG_VREG || src2.tag != IROP_TAG_VREG)
    return 0;
  IRSSAVregInfo *vi1 = ssa_opt_vinfo(ctx, vr1), *vi2 = ssa_opt_vinfo(ctx, vr2);
  if (!vi1 || !vi2 || vi1->def_instr < 0 || vi2->def_instr < 0 ||
      vi1->def_count > 1 || vi2->def_count > 1 || vi1->def_instr == vi2->def_instr)
    return 0;
  IRQuadCompact *d1q = &ir->compact_instructions[vi1->def_instr];
  IRQuadCompact *d2q = &ir->compact_instructions[vi2->def_instr];
  if (d1q->op != d2q->op || (d1q->op != TCCIR_OP_ADD && d1q->op != TCCIR_OP_SUB))
    return 0;
  IROperand k1 = tcc_ir_op_get_src2(ir, d1q), k2 = tcc_ir_op_get_src2(ir, d2q);
  if (!irop_is_immediate(k1) || !irop_is_immediate(k2) ||
      irop_get_imm64_ex(ir, k1) != irop_get_imm64_ex(ir, k2))
    return 0;
  IROperand b1 = tcc_ir_op_get_src1(ir, d1q), b2 = tcc_ir_op_get_src1(ir, d2q);
  if (b1.is_lval != b2.is_lval)
    return 0;
  if (ssa_cmp_values_reflexive(ctx, b1, b2))
    return 1;
  int bb1 = ssa_block_for_instr(ctx->cfg, vi1->def_instr);
  int bb2 = ssa_block_for_instr(ctx->cfg, vi2->def_instr);
  Sym *bs1, *bs2;
  int32_t ba1, ba2;
  int bd1, bd2;
  if (ssa_operand_symref_id(ctx, bb1 >= 0 ? &ctx->cfg->blocks[bb1] : NULL,
                            vi1->def_instr, b1, &bs1, &ba1, &bd1) &&
      ssa_operand_symref_id(ctx, bb2 >= 0 ? &ctx->cfg->blocks[bb2] : NULL,
                            vi2->def_instr, b2, &bs2, &ba2, &bd2) &&
      bs1 == bs2 && ba1 == ba2 && bd1 == bd2) {
    if (!bd1)
      return 1;
    /* Deref bases live in separate ADD defs: a store between them could differ. */
    return ssa_deref_sym_foldable(bs1) &&
           ssa_no_write_between(ir, ctx->cfg, vi1->def_instr, vi2->def_instr);
  }
  return 0;
}

/* ---- CMP ; JUMPIF/SETIF folding ---- */

static void ssa_remove_cmp_uses(IRSSAOptCtx *ctx, int cmp_idx,
                                IROperand src1, IROperand src2)
{
  IRSSAVregInfo *vi = ssa_opt_vinfo(ctx, irop_get_vreg(src1));
  if (vi) ssa_opt_remove_use_instr(vi, cmp_idx);
  vi = ssa_opt_vinfo(ctx, irop_get_vreg(src2));
  if (vi) ssa_opt_remove_use_instr(vi, cmp_idx);
}

static int ssa_fold_cmp_jumpif(IRSSAOptCtx *ctx, int cmp_idx)
{
  TCCIRState *ir = ctx->ir;
  IRQuadCompact *cmp_q = &ir->compact_instructions[cmp_idx];
  int n = ir->next_instruction_index;

  if (ssa_bool_norm(ctx, cmp_idx))
    return 1;

  IROperand src1 = tcc_ir_op_get_src1(ir, cmp_q);
  IROperand src2 = tcc_ir_op_get_src2(ir, cmp_q);
  int cmp_block = ssa_block_for_instr(ctx->cfg, cmp_idx);
  IRBasicBlock *cmp_bb = (cmp_block >= 0) ? &ctx->cfg->blocks[cmp_block] : NULL;

  /* A barrel annotation means the CMP really compares src1 against
   * (src2 SHIFT #n).  In the constant path the shift is applied to v2 below;
   * every other proof (reflexive/bitfield/expr-equal/stack-addr) reasons
   * about the RAW src2 and is invalidated by the shift (fuzz seed
   * signed:840, test 386). */
  uint8_t cmp_bs = tcc_ir_barrel_shift_at(ir, cmp_q);

  /* Reduce the CMP to known values; eqne_only limits stack-address proofs
   * (non-null / possibly-one-aggregate) to EQ/NE conditions. */
  int64_t v1, v2;
  int eqne_only = 0;
  if (ssa_cmp_values_const(ctx, cmp_bb, cmp_idx, src1, src2, &v1, &v2)) {
    if (cmp_bs) {
      if (irop_is_64bit(src1) || irop_is_64bit(src2))
        return 0;
      uint32_t m = (uint32_t)v2;
      int amount = cmp_bs & 0x1F;
      switch (cmp_bs >> 5) {  /* same encoding as fold_apply_barrel */
      case 1: m = m << amount; break;
      case 2: m = m >> amount; break;
      case 3: m = (m >> amount) |
                  (((m & 0x80000000u) && amount) ? ~(0xFFFFFFFFu >> amount) : 0u);
        break;
      case 4: m = amount ? ((m >> amount) | (m << (32 - amount))) : m; break;
      default: return 0;
      }
      v2 = (int64_t)(int32_t)m;
    }
  } else if (cmp_bs) {
    return 0;
  } else if (ssa_cmp_values_reflexive(ctx, src1, src2) ||
             ssa_cmp_values_bitfield(ctx, cmp_idx) ||
             ssa_cmp_values_expr_equal(ctx, cmp_bb, cmp_idx, src1, src2)) {
    v1 = v2 = 0;  /* any value gives the same reflexive result */
  } else if (ssa_cmp_values_stack_addr(ctx, cmp_idx, cmp_bb, src1, src2,
                                       &v1, &v2, &eqne_only)) {
  } else {
    return 0;
  }

  /* Drop the barrel annotation so the reused NOP/JUMP slot can't inherit it. */
  if (ir->barrel_shifts && cmp_q->orig_index >= 0 &&
      cmp_q->orig_index < ir->barrel_shifts_len)
    ir->barrel_shifts[cmp_q->orig_index] = 0;

  int j = ir_skip_nops_forward(ir, cmp_idx + 1, n);
  if (j >= n)
    return 0;

  int tok = ssa_flag_consumer_tok(ctx, j);
  if (tok < 0)
    return 0;
  if (eqne_only && tok != TOK_EQ && tok != TOK_NE)
    return 0;
  int result = eval_cond(v1, v2, tok);
  if (result < 0)
    return 0;

  ssa_remove_cmp_uses(ctx, cmp_idx, src1, src2);
  cmp_q->op = TCCIR_OP_NOP;
  ssa_rewrite_flag_consumer(ctx, j, result);
  return 1;
}

/* ---- TEST_ZERO ; JUMPIF folding ---- */

/* Resolve a TEST_ZERO source to a constant: an immediate, or a single-def TEMP
 * defined by `ASSIGN #imm` (ssa:cprop leaves that shape in place). */
static int ssa_tz_const_src(IRSSAOptCtx *ctx, IROperand src1, int64_t *out)
{
  if (irop_is_immediate(src1)) {
    *out = irop_get_imm64_ex(ctx->ir, src1);
    return 1;
  }
  int32_t vr = irop_get_vreg(src1);
  if (vr < 0 || src1.is_lval || TCCIR_DECODE_VREG_TYPE(vr) != TCCIR_VREG_TYPE_TEMP)
    return 0;
  IRSSAVregInfo *vi = ssa_opt_vinfo(ctx, vr);
  /* A phi-defined vreg merges other values at its header: the lone
   * instruction def is only the entry-path constant. */
  if (!vi || vi->def_count != 1 || vi->def_instr < 0 || vi->def_phi_block >= 0)
    return 0;
  IRQuadCompact *dq = &ctx->ir->compact_instructions[vi->def_instr];
  if (dq->op != TCCIR_OP_ASSIGN)
    return 0;
  IROperand d = tcc_ir_op_get_dest(ctx->ir, dq);
  if (d.is_lval || irop_needs_pair(d))
    return 0;
  IROperand s = tcc_ir_op_get_src1(ctx->ir, dq);
  if (!irop_is_immediate(s) || s.is_lval)
    return 0;
  *out = irop_get_imm64_ex(ctx->ir, s);
  return 1;
}

static int ssa_fold_test_zero(IRSSAOptCtx *ctx, int tz_idx)
{
  TCCIRState *ir = ctx->ir;
  IRQuadCompact *tz_q = &ir->compact_instructions[tz_idx];
  int n = ir->next_instruction_index;

  int64_t val;
  IROperand src1 = tcc_ir_op_get_src1(ir, tz_q);
  if (!ssa_tz_const_src(ctx, src1, &val)) {
    /* A provable stack-local address is non-null (the inlined `if (!p)`
     * guard on a passed `&local`). */
    int32_t sa_id;
    int64_t sa_off;
    int tz_block = ssa_block_for_instr(ctx->cfg, tz_idx);
    IRBasicBlock *tz_bb = (tz_block >= 0) ? &ctx->cfg->blocks[tz_block] : NULL;
    if (tcc_ir_barrel_shift_at(ir, tz_q) != 0 || irop_is_64bit(src1) ||
        !ssa_resolve_stack_addr(ctx, src1, tz_idx, tz_bb, &sa_id, &sa_off))
      return 0;
    val = 1;
  }

  int j = ir_skip_nops_forward(ir, tz_idx + 1, n);
  if (j >= n)
    return 0;
  IRQuadCompact *next_q = &ir->compact_instructions[j];
  if (next_q->op != TCCIR_OP_JUMPIF) {
    /* SETIF/SELECT consumer: same generic fold the constant-CMP path uses.
     * A jump-target consumer also reads flags set on the other entry path;
     * keep the TEST_ZERO when a second consumer may still read its flags. */
    if (next_q->is_jump_target)
      return 0;
    int ctok = ssa_flag_consumer_tok(ctx, j);
    if (ctok < 0)
      return 0;
    int result = eval_cond(val, 0, ctok);
    if (result < 0)
      return 0;
    int k2 = ir_skip_nops_forward(ir, j + 1, n);
    int more_consumers =
        (k2 < n) && (ir->compact_instructions[k2].op == TCCIR_OP_SETIF ||
                     ir->compact_instructions[k2].op == TCCIR_OP_JUMPIF ||
                     ir->compact_instructions[k2].op == TCCIR_OP_SELECT);
    ssa_rewrite_flag_consumer(ctx, j, result);
    if (!more_consumers)
      ssa_opt_nop_instr(ctx, tz_idx);
    return 1;
  }

  int tok = (int)irop_get_imm64_ex(ir, tcc_ir_op_get_src1(ir, next_q));
  if (tok != TOK_EQ && tok != TOK_NE)
    return 0;
  int branch_taken = (tok == TOK_EQ) ? (val == 0) : (val != 0);

  ssa_opt_nop_instr(ctx, tz_idx);
  if (branch_taken) {
    IROperand dest = tcc_ir_op_get_dest(ir, next_q);
    next_q->op = TCCIR_OP_JUMP;
    tcc_ir_set_dest(ir, j, dest);
    return 1;
  }

  next_q->op = TCCIR_OP_NOP;
  /* A fall-through SETIF reads the now-NOPed flag state, so it must fold too
   * or codegen lowers it against garbage flags. */
  int k = ir_skip_nops_forward(ir, j + 1, n);
  if (k < n) {
    IRQuadCompact *setif_q = &ir->compact_instructions[k];
    if (setif_q->op == TCCIR_OP_SETIF && !setif_q->is_jump_target) {
      int stok = (int)irop_get_imm64_ex(ir, tcc_ir_op_get_src1(ir, setif_q));
      if (stok == TOK_EQ || stok == TOK_NE) {
        int sres = (stok == TOK_EQ) ? (val == 0) : (val != 0);
        IROperand dest = tcc_ir_op_get_dest(ir, setif_q);
        setif_q->op = TCCIR_OP_ASSIGN;
        tcc_ir_set_src1(ir, k, irop_make_imm32(-1, sres, irop_get_btype(dest)));
        tcc_ir_set_src2(ir, k, IROP_NONE);
      }
    }
  }
  return 1;
}

/* ---- non-negative soft-float compare folding ---- */

/* A soft-float flag compare (__aeabi_cdcmple/__aeabi_cfcmple) of a provably
 * non-negative value against 0.0 has a known outcome for the >= / < family. */
static const char *ssa_nonneg_func_names[] = {
    "fabs", "fabsf", "abs", "labs", "llabs", "strlen", "sizeof",
    "__aeabi_ui2d", "__aeabi_ui2f",  /* unsigned → float is always >= 0 */
};
static const char *ssa_flag_cmp_funcs[] = {
    "__aeabi_cdcmple", "__aeabi_cfcmple",
};

static const char *ssa_call_callee_name(TCCIRState *ir, IRQuadCompact *q)
{
  Sym *callee = irop_get_sym_ex(ir, tcc_ir_op_get_src1(ir, q));
  return callee ? get_tok_str(callee->v, NULL) : NULL;
}

static int ssa_name_in(const char *name, const char **tbl, int cnt)
{
  if (!name) return 0;
  for (int j = 0; j < cnt; j++)
    if (strcmp(name, tbl[j]) == 0) return 1;
  return 0;
}

/* Collect the first `np` param operands of the call at `call_idx` from the
 * contiguous FUNCPARAMVAL block preceding it.  Returns a bitmask of found
 * param indices. */
static unsigned ssa_call_params(TCCIRState *ir, int call_idx, IROperand *p, int np)
{
  IRQuadCompact *cq = &ir->compact_instructions[call_idx];
  uint32_t enc = (uint32_t)irop_get_imm64_ex(ir, tcc_ir_op_get_src2(ir, cq));
  int call_id = TCCIR_DECODE_CALL_ID(enc);
  unsigned found = 0;
  for (int k = call_idx - 1; k >= 0; k--) {
    IRQuadCompact *kq = &ir->compact_instructions[k];
    if (kq->op == TCCIR_OP_NOP || kq->op == TCCIR_OP_FUNCPARAMVOID) continue;
    if (kq->op != TCCIR_OP_FUNCPARAMVAL) break;
    uint32_t penc = (uint32_t)irop_get_imm64_ex(ir, tcc_ir_op_get_src2(ir, kq));
    if (TCCIR_DECODE_CALL_ID(penc) != call_id) break;
    int pidx = TCCIR_DECODE_PARAM_IDX(penc);
    if (pidx < np && !(found & (1u << pidx))) {
      p[pidx] = tcc_ir_op_get_src1(ir, kq);
      found |= 1u << pidx;
    }
  }
  return found;
}

static int ssa_call_param0_imm(TCCIRState *ir, int call_idx, int64_t *out)
{
  IROperand p0;
  if (ssa_call_params(ir, call_idx, &p0, 1) != 1 || !irop_is_immediate(p0))
    return 0;
  *out = irop_get_imm64_ex(ir, p0);
  return 1;
}

/* A call whose result is provably non-negative: fabs/abs/strlen/... or
 * __aeabi_f2d of a non-negative single-precision constant. */
static int ssa_call_result_nonneg(TCCIRState *ir, int def_idx)
{
  IRQuadCompact *dq = &ir->compact_instructions[def_idx];
  if (dq->op != TCCIR_OP_FUNCCALLVAL) return 0;
  const char *name = ssa_call_callee_name(ir, dq);
  if (!name) return 0;
  if (ssa_name_in(name, ssa_nonneg_func_names,
                  (int)(sizeof(ssa_nonneg_func_names) / sizeof(ssa_nonneg_func_names[0]))))
    return 1;
  if (strcmp(name, "__aeabi_f2d") == 0) {
    int64_t fimm;
    if (ssa_call_param0_imm(ir, def_idx, &fimm)) {
      uint32_t fbits = (uint32_t)fimm;
      uint32_t sign = (fbits >> 31) & 1;
      uint32_t exp = (fbits >> 23) & 0xFF;
      uint32_t mant = fbits & 0x7FFFFF;
      if (!sign && !(exp == 0xFF && mant != 0)) return 1;
    }
  }
  return 0;
}

/* Is the value read from `vreg` at `use_idx` provably non-negative?  TEMPs
 * resolve through SSA vinfo; unpromoted VAR slots scan back to the nearest slot
 * def, bailing on any potentially-aliasing write. */
static int ssa_vreg_is_nonneg(IRSSAOptCtx *ctx, int32_t vreg, int use_idx)
{
  if (vreg < 0) return 0;
  TCCIRState *ir = ctx->ir;

  if (TCCIR_DECODE_VREG_TYPE(vreg) == TCCIR_VREG_TYPE_TEMP) {
    IRSSAVregInfo *vi = ssa_opt_vinfo(ctx, vreg);
    if (!vi || vi->def_instr < 0 || vi->def_count > 1) return 0;
    return ssa_call_result_nonneg(ir, vi->def_instr);
  }

  if (TCCIR_DECODE_VREG_TYPE(vreg) != TCCIR_VREG_TYPE_VAR) return 0;
  int var_pos = TCCIR_DECODE_VREG_POSITION(vreg);
  for (int k = use_idx - 1; k >= 0; k--) {
    IRQuadCompact *kq = &ir->compact_instructions[k];
    if (kq->op == TCCIR_OP_NOP) continue;
    if (irop_config[kq->op].has_dest) {
      IROperand kd = tcc_ir_op_get_dest(ir, kq);
      int32_t kdv = irop_get_vreg(kd);
      if (kdv >= 0 && TCCIR_DECODE_VREG_TYPE(kdv) == TCCIR_VREG_TYPE_VAR &&
          TCCIR_DECODE_VREG_POSITION(kdv) == var_pos) {
        if (kq->op == TCCIR_OP_STORE && kd.is_lval && !kd.is_local)
          continue; /* pointer-deref through V, not a slot def */
        return ssa_call_result_nonneg(ir, k);
      }
    }
    if (kq->op == TCCIR_OP_STORE_INDEXED || kq->op == TCCIR_OP_STORE_POSTINC)
      return 0;
    if (kq->op == TCCIR_OP_STORE) {
      IROperand kd = tcc_ir_op_get_dest(ir, kq);
      if (kd.tag == IROP_TAG_STACKOFF && kd.is_local && kd.is_lval)
        continue;
      return 0;
    }
  }
  return 0;
}

static int ssa_fold_nonneg_cmp(IRSSAOptCtx *ctx, int call_idx)
{
  TCCIRState *ir = ctx->ir;
  IRQuadCompact *call_q = &ir->compact_instructions[call_idx];
  int n = ir->next_instruction_index;

  const char *cmp_name = ssa_call_callee_name(ir, call_q);
  if (!ssa_name_in(cmp_name, ssa_flag_cmp_funcs,
                   (int)(sizeof(ssa_flag_cmp_funcs) / sizeof(ssa_flag_cmp_funcs[0]))))
    return 0;

  IROperand p[2];
  if (ssa_call_params(ir, call_idx, p, 2) != 3)
    return 0;

  int nonneg_is_arg0;
  if (!irop_is_immediate(p[0]) && irop_is_immediate(p[1]) &&
      irop_get_imm64_ex(ir, p[1]) == 0 &&
      ssa_vreg_is_nonneg(ctx, irop_get_vreg(p[0]), call_idx))
    nonneg_is_arg0 = 1;
  else if (irop_is_immediate(p[0]) && irop_get_imm64_ex(ir, p[0]) == 0 &&
           !irop_is_immediate(p[1]) &&
           ssa_vreg_is_nonneg(ctx, irop_get_vreg(p[1]), call_idx))
    nonneg_is_arg0 = 0;
  else
    return 0;

  int j = ir_skip_nops_forward(ir, call_idx + 1, n);
  if (j >= n) return 0;

  int cond_tok = ssa_flag_consumer_tok(ctx, j);
  int fold_result = -1;
  if (nonneg_is_arg0) {
    if (cond_tok == TOK_GE || cond_tok == TOK_UGE) fold_result = 1;
    else if (cond_tok == TOK_LT || cond_tok == TOK_ULT) fold_result = 0;
  } else {
    if (cond_tok == TOK_LE || cond_tok == TOK_ULE) fold_result = 1;
    else if (cond_tok == TOK_GT || cond_tok == TOK_UGT) fold_result = 0;
  }
  if (fold_result < 0) return 0;

  ssa_rewrite_flag_consumer(ctx, j, fold_result);
  return 1;
}

/* ---- Return-constant register reuse ---- */

/* A `RETURNVALUE C` (C an integer immediate) whose block is entered *only*
 * through the equality edge of a `TEST_ZERO V` (C == 0) or `CMP V, #C`
 * returns the very value the comparison already proved V holds on that edge.
 * Returning V instead of C lets the backend reuse the register V already
 * lives in and drop the constant materialization.
 *
 * SSA analog of the retired flat return_reuse pass (ir/opt_dce.c).  Running
 * in the SSA stage means flat if-conversion (tcc_ir_opt_select) already
 * absorbed the constant-return diamonds it handles, so only the shapes
 * SELECT can't cover reach here (pr106433::bar, pr40570::baz, 920812-1::f).
 * -O2 only, mirroring the flat pass. */
static int ssa_fold_return_const_reuse(IRSSAOptCtx *ctx, int ret_idx)
{
  TCCIRState *ir = ctx->ir;
  int n = ir->next_instruction_index;
  IRQuadCompact *R = &ir->compact_instructions[ret_idx];
  IROperand rv = tcc_ir_op_get_src1(ir, R);
  if (rv.is_sym || rv.is_lval || !irop_is_immediate(rv))
    return 0;
  if (!tcc_state || tcc_state->optimize < 2)
    return 0;
  int64_t cval = irop_get_imm64_ex(ir, rv);

  /* (1) No fall-through into ret_idx: the instruction just before must be an
   * unconditional diversion, otherwise ret_idx has a second (non-equality)
   * predecessor on which V may not equal C. */
  int p = ret_idx - 1;
  while (p >= 0 && ir->compact_instructions[p].op == TCCIR_OP_NOP)
    p--;
  if (p >= 0)
  {
    TccIrOp pop = ir->compact_instructions[p].op;
    if (pop != TCCIR_OP_JUMP && pop != TCCIR_OP_RETURNVALUE && pop != TCCIR_OP_RETURNVOID &&
        pop != TCCIR_OP_TRAP && pop != TCCIR_OP_IJUMP && pop != TCCIR_OP_SWITCH_TABLE)
      return 0;
  }

  /* (2) Exactly one branch predecessor, an equality JUMPIF targeting
   * ret_idx. */
  int jif = -1, npred = 0, bad = 0;
  for (int j = 0; j < n && !bad; j++)
  {
    if (j == ret_idx)
      continue;
    IRQuadCompact *q = &ir->compact_instructions[j];
    int targets_r = 0;
    if (q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_JUMPIF)
    {
      if ((int)tcc_ir_op_get_dest(ir, q).u.imm32 == ret_idx)
        targets_r = 1;
    }
    else if (q->op == TCCIR_OP_SWITCH_TABLE)
    {
      IROperand s2 = tcc_ir_op_get_src2(ir, q);
      int tid = (int)irop_get_imm64_ex(ir, s2);
      if (tid >= 0 && tid < ir->num_switch_tables)
      {
        TCCIRSwitchTable *t = &ir->switch_tables[tid];
        for (int e = 0; e < t->num_entries; e++)
          if (t->targets[e] == ret_idx)
            targets_r = 1;
        if (t->default_target == ret_idx)
          targets_r = 1;
      }
    }
    if (!targets_r)
      continue;
    npred++;
    if (q->op == TCCIR_OP_JUMPIF && (int)tcc_ir_op_get_src1(ir, q).u.imm32 == TOK_EQ)
      jif = j;
    else
      bad = 1;
  }
  if (bad || npred != 1 || jif < 0)
    return 0;

  /* (3) The flag-setter just before the JUMPIF proves V == C, with V a plain
   * register value (no memory / sym deref) of the same width as the
   * return. */
  int t = jif - 1;
  while (t >= 0 && ir->compact_instructions[t].op == TCCIR_OP_NOP)
    t--;
  if (t < 0)
    return 0;
  IRQuadCompact *T = &ir->compact_instructions[t];
  IROperand vop = IROP_NONE;
  int matched = 0;
  if (T->op == TCCIR_OP_TEST_ZERO && cval == 0)
  {
    vop = tcc_ir_op_get_src1(ir, T);
    matched = 1;
  }
  else if (T->op == TCCIR_OP_CMP)
  {
    IROperand a = tcc_ir_op_get_src1(ir, T);
    IROperand b = tcc_ir_op_get_src2(ir, T);
    if (!a.is_sym && !a.is_lval && irop_get_vreg(a) >= 0 && irop_is_immediate(b) && !b.is_sym &&
        irop_get_imm64_ex(ir, b) == cval)
    {
      vop = a;
      matched = 1;
    }
    else if (!b.is_sym && !b.is_lval && irop_get_vreg(b) >= 0 && irop_is_immediate(a) && !a.is_sym &&
             irop_get_imm64_ex(ir, a) == cval)
    {
      vop = b;
      matched = 1;
    }
  }
  if (!matched || vop.is_sym || vop.is_lval || irop_get_vreg(vop) < 0)
    return 0;
  int vt = TCCIR_DECODE_VREG_TYPE(irop_get_vreg(vop));
  if (vt != TCCIR_VREG_TYPE_PARAM && vt != TCCIR_VREG_TYPE_VAR && vt != TCCIR_VREG_TYPE_TEMP)
    return 0;
  if (irop_get_btype(vop) != irop_get_btype(rv))
    return 0;

  /* Return the register the comparison proved equals C. */
  tcc_ir_set_src1(ir, ret_idx, vop);
  IRSSAVregInfo *vi = ssa_opt_vinfo(ctx, irop_get_vreg(vop));
  if (vi)
    ssa_opt_add_use_instr(vi, ret_idx);
  return 1;
}

/* ---- redundant compare-implication folds (SSA analog of flat float_branch) ---- */

/* True if instr[idx] tests a value against zero (TEST_ZERO, or CMP with a zero
 * operand); sets *expr to the tested value. */
static int ssa_match_zero_test(TCCIRState *ir, int idx, IROperand *expr)
{
  if (idx < 0 || idx >= ir->next_instruction_index)
    return 0;
  IRQuadCompact *q = &ir->compact_instructions[idx];
  if (q->op == TCCIR_OP_TEST_ZERO) {
    *expr = tcc_ir_op_get_src1(ir, q);
    return 1;
  }
  if (q->op != TCCIR_OP_CMP)
    return 0;
  IROperand s1 = tcc_ir_op_get_src1(ir, q);
  IROperand s2 = tcc_ir_op_get_src2(ir, q);
  if (irop_is_immediate(s2) && irop_get_imm64_ex(ir, s2) == 0) { *expr = s1; return 1; }
  if (irop_is_immediate(s1) && irop_get_imm64_ex(ir, s1) == 0) { *expr = s2; return 1; }
  return 0;
}

/* Two folds over a straight-line fallthrough run whose fact dominates the second
 * test (bail on any merge point):
 *   Loop 1 — soft-FP flag-cmp implication: `cmp(a,b); JUMPIF t1; <pure>; cmp(a,b
 *     or swapped); JUMPIF t2`, where negate(t1) implies/refutes effective t2.
 *   Loop 2 — redundant zero-test: two `TEST_ZERO/CMP,#0` of a pure-equal value,
 *     the first branch deciding the second (e.g. `isunordered(x,y)` recomputed).
 * The __aeabi_*cmple helpers are impure calls GVN never CSE-merges, so this stays
 * a dominance-scan. Built once (not per gen) to keep it linear across the corpus. */
static int ssa_fold_redundant_flag_cmps(IRSSAOptCtx *ctx)
{
  TCCIRState *ir = ctx->ir;
  int n = ir->next_instruction_index;
  if (n < 4)
    return 0;

  uint8_t *is_merge = ir_opt_build_merge_bitmap(ir, n);
  int changes = 0;

  for (int i = 0; i < n; i++) {
    IRQuadCompact *q = &ir->compact_instructions[i];

    if (q->op == TCCIR_OP_FUNCCALLVOID || q->op == TCCIR_OP_FUNCCALLVAL) {
      if (!ir_opt_is_flag_cmp_helper_name(ssa_call_callee_name(ir, q)))
        continue;

      IROperand arg0, arg1;
      if (!ir_opt_get_call_param_operand(ir, i, 0, &arg0) ||
          !ir_opt_get_call_param_operand(ir, i, 1, &arg1))
        continue;

      int jump1_idx = ir_opt_next_non_nop(ir, i + 1);
      if (jump1_idx < 0 || ir->compact_instructions[jump1_idx].op != TCCIR_OP_JUMPIF)
        continue;

      /* Scan index-by-index (not via next_non_nop): a merge point left as a NOP
       * by post-SSA DCE would otherwise be skipped, folding across a join. */
      int cmp2_idx = -1, jump2_idx = -1;
      for (int scan = jump1_idx + 1; scan < n; scan++) {
        if (is_merge[scan / 8] & (1 << (scan % 8)))
          break;
        IRQuadCompact *sq = &ir->compact_instructions[scan];
        if (sq->op == TCCIR_OP_NOP)
          continue;
        int is_call = sq->op == TCCIR_OP_FUNCCALLVOID || sq->op == TCCIR_OP_FUNCCALLVAL;
        if (!is_call || !ir_opt_is_flag_cmp_helper_name(ssa_call_callee_name(ir, sq))) {
          if (!ir_opt_is_pure_fallthrough_instruction(ir, scan))
            break;
          continue;
        }
        cmp2_idx = scan;
        jump2_idx = ir_opt_next_non_nop(ir, cmp2_idx + 1);
        break;
      }
      if (cmp2_idx < 0 || jump2_idx < 0 ||
          ir->compact_instructions[jump2_idx].op != TCCIR_OP_JUMPIF)
        continue;

      IROperand carg0, carg1;
      if (!ir_opt_get_call_param_operand(ir, cmp2_idx, 0, &carg0) ||
          !ir_opt_get_call_param_operand(ir, cmp2_idx, 1, &carg1))
        continue;

      IRQuadCompact *jump1 = &ir->compact_instructions[jump1_idx];
      IRQuadCompact *jump2 = &ir->compact_instructions[jump2_idx];
      int tok1 = (int)irop_get_imm64_ex(ir, tcc_ir_op_get_src1(ir, jump1));
      int tok2 = (int)irop_get_imm64_ex(ir, tcc_ir_op_get_src1(ir, jump2));
      int known_fact = vrp_negate_cmp_tok(tok1);
      if (known_fact < 0)
        continue;

      int effective_tok2 = -1, is_swapped = 0;
      if (ir_opt_pure_expr_equal(ir, arg0, i, carg0, cmp2_idx, 0) &&
          ir_opt_pure_expr_equal(ir, arg1, i, carg1, cmp2_idx, 0))
        effective_tok2 = tok2;
      else if (ir_opt_pure_expr_equal(ir, arg0, i, carg1, cmp2_idx, 0) &&
               ir_opt_pure_expr_equal(ir, arg1, i, carg0, cmp2_idx, 0)) {
        is_swapped = 1;
        effective_tok2 = vrp_swap_cmp_tok(tok2);
      }
      if (effective_tok2 < 0)
        continue;

      /* Swapped operands only order-compare soundly under UB (C11 6.5.8p5);
       * require matching targets or an ordering known_fact. */
      if (is_swapped &&
          tcc_ir_op_get_dest(ir, jump1).u.imm32 != tcc_ir_op_get_dest(ir, jump2).u.imm32) {
        switch (known_fact) {
        case TOK_LT: case TOK_GT: case TOK_ULT: case TOK_UGT: break;
        default: continue;
        }
      }

      if (fcmp_cmp_implies(known_fact, effective_tok2)) {
        ssa_opt_nop_instr(ctx, cmp2_idx);
        ssa_rewrite_jumpif(ctx, jump2_idx, 1);
        changes++;
      } else if (fcmp_cmp_implies(known_fact, vrp_negate_cmp_tok(effective_tok2))) {
        ssa_opt_nop_instr(ctx, cmp2_idx);
        ssa_rewrite_jumpif(ctx, jump2_idx, 0);
        changes++;
      }
    } else if (q->op == TCCIR_OP_TEST_ZERO || q->op == TCCIR_OP_CMP) {
      IROperand expr1;
      if (!ssa_match_zero_test(ir, i, &expr1))
        continue;

      int jump1_idx = ir_opt_next_non_nop(ir, i + 1);
      if (jump1_idx < 0 || ir->compact_instructions[jump1_idx].op != TCCIR_OP_JUMPIF)
        continue;
      int known_zero;
      switch ((int)irop_get_imm64_ex(ir,
                 tcc_ir_op_get_src1(ir, &ir->compact_instructions[jump1_idx]))) {
      case TOK_NE: known_zero = 1; break;
      case TOK_EQ: known_zero = 0; break;
      default: continue;
      }

      /* Index-by-index so a NOP merge point (post-SSA DCE) is not skipped. */
      for (int t2 = jump1_idx + 1; t2 + 1 < n; t2++) {
        if (is_merge[t2 / 8] & (1 << (t2 % 8)))
          break;
        if (ir->compact_instructions[t2].op == TCCIR_OP_NOP)
          continue;
        IROperand expr2;
        if (!ssa_match_zero_test(ir, t2, &expr2)) {
          if (!ir_opt_is_pure_fallthrough_instruction(ir, t2))
            break;
          continue;
        }
        int jump2_idx = ir_opt_next_non_nop(ir, t2 + 1);
        if (jump2_idx < 0 || ir->compact_instructions[jump2_idx].op != TCCIR_OP_JUMPIF)
          break;
        if (!ir_opt_pure_expr_equal(ir, expr1, i, expr2, t2, 0))
          continue;
        int tok2 = (int)irop_get_imm64_ex(ir,
                     tcc_ir_op_get_src1(ir, &ir->compact_instructions[jump2_idx]));
        if ((known_zero && tok2 == TOK_EQ) || (!known_zero && tok2 == TOK_NE)) {
          ssa_opt_nop_instr(ctx, t2);
          ssa_rewrite_jumpif(ctx, jump2_idx, 1);
          changes++;
        } else if ((known_zero && tok2 == TOK_NE) || (!known_zero && tok2 == TOK_EQ)) {
          ssa_opt_nop_instr(ctx, t2);
          ssa_rewrite_jumpif(ctx, jump2_idx, 0);
          changes++;
        }
        break;
      }
    }
  }

  tcc_free(is_merge);
  return changes;
}

/* ---- pass driver ---- */

static const IRSSAOptGen branch_gens[] = {
  { TCCIR_OP_CMP,          ssa_fold_cmp_jumpif, "branch_cmp" },
  { TCCIR_OP_TEST_ZERO,    ssa_fold_test_zero,  "branch_tz" },
  { TCCIR_OP_FUNCCALLVOID, ssa_fold_nonneg_cmp, "branch_nonneg" },
  { TCCIR_OP_RETURNVALUE,  ssa_fold_return_const_reuse, "branch_retreuse" },
};

int ssa_opt_branch(IRSSAOptCtx *ctx)
{
  int changes = ssa_opt_run_gens(ctx, branch_gens,
                                 sizeof(branch_gens) / sizeof(branch_gens[0]));
  changes += ssa_fold_redundant_flag_cmps(ctx);
  /* Only folding can strand blocks, so only prune when something folded. */
  if (changes > 0)
    changes += ssa_opt_prune_unreachable_phis(ctx);
  return changes;
}
