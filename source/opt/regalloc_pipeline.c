/*
 *  TCC IR - SSA-Aware Register Allocator (Optimization Pipeline)
 *
 *  Optimization passes that operate on SSA-renamed IR in the register
 *  allocation pipeline: constant-branch folding, phi-const chain folding,
 *  incomplete-call repair, dead-assign elimination, narrow-weight tracking,
 *  accurate-liveness refinement, graph-based coalescing, dead-copy elimination,
 *  multi-def-temp promotion, and post-allocation move coalescing.
 *
 *  These passes are called from tcc_ir_ssa_regalloc() in ir/regalloc.c,
 *  which also owns the core allocator (interval building, linear scan,
 *  phi resolution, hint building).
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#define USING_GLOBALS
#include "ir.h"
#include "regalloc.h"
#include "cfg.h"
#include "ssa.h"
#include "tccir.h"

extern int tcc_ir_opt_pass_disabled(const char *name);

#define RA_DBG(fmt, ...) LOG_LS(fmt, ##__VA_ARGS__)

/* ============================================================================
 * Externs for helpers that live in ir/regalloc.c (core allocator)
 * ============================================================================ */

/* ra_instr_touches_vreg — used by tcc_ir_move_coalescing */
static int ra_instr_touches_vreg(TCCIRState *ir, IRQuadCompact *q, int32_t vreg);

/* ============================================================================
 * Post-Phi-Resolution Constant-Branch Folding
 *
 * Loop-rotation produces an entry guard like `i = 0; cmp i, N; jump if >=`.
 * Pre-SSA the comparison is foldable (i is a constant), but SSA construction
 * turns i into a phi destination which masks the constant. Phi resolution
 * then materializes the entry-path constant assignment right before the cmp,
 * so within that single basic block the cmp is foldable again — but no pass
 * runs after phi resolution.
 *
 * When the guard is dead, the fall-through block contains only the phi-
 * resolution copies for the carrier vregs (post-loopN values that flow
 * into round N+1). Removing it shrinks every carrier's live range to a
 * single short window between loop exit and the next round's entry,
 * lifting register pressure dramatically in code like SHA's chained loops.
 * ============================================================================ */

static int ra_eval_cmp_cond(int64_t v1, int64_t v2, int tok)
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

/* Resolve `op` to a constant by walking back from `cmp_idx` within the same
 * basic block. Returns 1 on success. The walk fails if any instruction in
 * (def, cmp_idx] is a jump target (multiple predecessors mean the def doesn't
 * dominate cmp_idx), if it crosses a control-flow op, hits a non-ASSIGN def,
 * or reaches an ASSIGN whose source isn't an immediate. */
static int ra_try_resolve_const_local(TCCIRState *ir, const uint8_t *is_target,
                                      IROperand op, int cmp_idx, int64_t *out)
{
  if (irop_is_immediate(op)) {
    *out = irop_get_imm64_ex(ir, op);
    return 1;
  }
  if (op.tag != IROP_TAG_VREG || op.is_lval) return 0;
  int32_t vr = irop_get_vreg(op);
  if (vr < 0) return 0;

  /* If cmp_idx itself is a join point, other paths can deliver a different
   * value bypassing any local def. */
  if (is_target && is_target[cmp_idx]) return 0;

  for (int k = cmp_idx - 1; k >= 0; k--) {
    IRQuadCompact *q = &ir->compact_instructions[k];
    TccIrOp opc = q->op;
    if (opc == TCCIR_OP_NOP) {
      /* A NOP at a jump-target position would still mark a join; bail. */
      if (is_target && is_target[k]) return 0;
      continue;
    }

    /* Stop at any control-flow op: those end the basic block above. */
    if (opc == TCCIR_OP_JUMP || opc == TCCIR_OP_JUMPIF ||
        opc == TCCIR_OP_IJUMP || opc == TCCIR_OP_SWITCH_TABLE ||
        opc == TCCIR_OP_RETURNVALUE || opc == TCCIR_OP_RETURNVOID)
      return 0;

    /* Skip stores: they don't define vregs, only memory. */
    if (opc == TCCIR_OP_STORE || opc == TCCIR_OP_STORE_INDEXED ||
        opc == TCCIR_OP_STORE_POSTINC) {
      if (is_target && is_target[k]) return 0;
      continue;
    }

    if (!irop_config[opc].has_dest) {
      if (is_target && is_target[k]) return 0;
      continue;
    }
    IROperand d = tcc_ir_op_get_dest(ir, q);
    if (d.is_lval) {
      if (is_target && is_target[k]) return 0;
      continue;
    }
    int32_t dv = irop_get_vreg(d);
    if (dv != vr) {
      if (is_target && is_target[k]) return 0;
      continue;
    }

    /* Found the def at line k. The def dominates cmp_idx only if no jump
     * target exists in (k, cmp_idx] — but we already checked cmp_idx and
     * every line in between via the bails above. */
    if (opc != TCCIR_OP_ASSIGN) return 0;
    IROperand s = tcc_ir_op_get_src1(ir, q);
    if (!irop_is_immediate(s) || s.is_lval) return 0;
    *out = irop_get_imm64_ex(ir, s);
    return 1;
  }
  return 0;
}

/* NOP every instruction starting at `start_idx` until reaching one that is
 * the target of any jump/branch elsewhere. Used to remove a now-unreachable
 * fall-through block after folding a JUMPIF into an unconditional JUMP. */
static int ra_nop_dead_block(TCCIRState *ir, const uint8_t *is_target, int start_idx)
{
  int n = ir->next_instruction_index;
  int nopped = 0;
  for (int i = start_idx; i < n; i++) {
    if (is_target[i]) break;
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP) continue;
    q->op = TCCIR_OP_NOP;
    nopped++;
  }
  return nopped;
}

/* Build a bitmap of instructions that are the target of any jump. */
static uint8_t *ra_build_jump_target_map(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  if (n <= 0) return NULL;
  uint8_t *map = tcc_mallocz((size_t)n);
  for (int i = 0; i < n; i++) {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_JUMPIF) {
      int t = (int)irop_get_imm64_ex(ir, tcc_ir_op_get_dest(ir, q));
      if (t >= 0 && t < n) map[t] = 1;
    } else if (q->op == TCCIR_OP_SWITCH_TABLE) {
      IROperand s2 = tcc_ir_op_get_src2(ir, q);
      int table_id = (int)irop_get_imm64_ex(ir, s2);
      if (table_id >= 0 && table_id < ir->num_switch_tables) {
        TCCIRSwitchTable *table = &ir->switch_tables[table_id];
        for (int j = 0; j < table->num_entries; j++) {
          int t = table->targets[j];
          if (t >= 0 && t < n) map[t] = 1;
        }
        int dt = table->default_target;
        if (dt >= 0 && dt < n) map[dt] = 1;
      }
    }
  }
  return map;
}

/* NOP any FUNCCALL left without its full FUNCPARAMVAL set (regalloc dead-code passes can strip a param off a dead fall-through while its call survives via another edge — only possible in dead code, as a param dominates its call in live code). */
static int ra_repair_incomplete_calls(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  int changed = 0;
  for (int i = 0; i < n; i++) {
    IRQuadCompact *cq = &ir->compact_instructions[i];
    if (cq->op != TCCIR_OP_FUNCCALLVAL && cq->op != TCCIR_OP_FUNCCALLVOID) continue;
    IROperand cs2 = tcc_ir_op_get_src2(ir, cq);
    if (irop_is_none(cs2)) continue;
    int call_id = TCCIR_DECODE_CALL_ID((uint32_t)cs2.u.imm32);
    int argc = TCCIR_DECODE_CALL_ARGC((uint32_t)cs2.u.imm32);
    if (argc <= 0) continue;
    int found = 0;
    for (int j = i - 1; j >= 0 && found < argc; j--) {
      IRQuadCompact *pq = &ir->compact_instructions[j];
      if (pq->op != TCCIR_OP_FUNCPARAMVAL) continue;
      IROperand ps2 = tcc_ir_op_get_src2(ir, pq);
      if (irop_is_none(ps2)) continue;
      if (TCCIR_DECODE_CALL_ID((uint32_t)ps2.u.imm32) == call_id) found++;
    }
    if (found >= argc) continue;
    cq->op = TCCIR_OP_NOP;
    for (int j = i - 1; j >= 0; j--) {
      IRQuadCompact *pq = &ir->compact_instructions[j];
      if (pq->op != TCCIR_OP_FUNCPARAMVAL && pq->op != TCCIR_OP_FUNCPARAMVOID) continue;
      IROperand ps2 = tcc_ir_op_get_src2(ir, pq);
      if (irop_is_none(ps2)) continue;
      if (TCCIR_DECODE_CALL_ID((uint32_t)ps2.u.imm32) == call_id) pq->op = TCCIR_OP_NOP;
    }
    changed++;
  }
  return changed;
}

/* Try to fold CMP + JUMPIF where both CMP operands resolve to constants
 * within the same basic block. Returns the number of branches folded. */
static int ra_fold_const_branches(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  if (n <= 0) return 0;

  uint8_t *is_target = ra_build_jump_target_map(ir);
  if (!is_target) return 0;

  int folds = 0;
  for (int i = 0; i < n; i++) {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_JUMPIF) continue;

    /* Find the most recent CMP that sets the flags JUMPIF reads. Walk back
     * through ops that don't write flags; stop at any other flag-setter or
     * at a basic block boundary. Phi resolution often inserts unrelated
     * ASSIGN copies between the CMP and JUMPIF that we must skip past. */
    int cmp_idx = -1;
    for (int j = i - 1; j >= 0; j--) {
      IRQuadCompact *pq = &ir->compact_instructions[j];
      TccIrOp pop = pq->op;
      if (pop == TCCIR_OP_NOP) continue;
      if (pop == TCCIR_OP_CMP) { cmp_idx = j; break; }
      /* Other flag-setting ops invalidate the CMP we'd want to read. */
      if (pop == TCCIR_OP_TEST_ZERO || pop == TCCIR_OP_FCMP) break;
      /* A call clobbers CPSR (AAPCS: flags are caller-saved), so a CMP before
       * it cannot be the JUMPIF's flag source.  Critically, the soft-float
       * compare helpers (__aeabi_cfcmple / cdcmple, ...) are FUNCCALLVOID
       * flag-setters: they ARE the branch's real flag source, and striding
       * past them would mis-attribute the branch to an earlier integer CMP and
       * wrongly NOP it (orphaning a SELECT that consumes it — fuzz seed 2049). */
      if (pop == TCCIR_OP_FUNCCALLVAL || pop == TCCIR_OP_FUNCCALLVOID) break;
      /* A flag-consumer between the CMP and this JUMPIF means the CMP has
       * another reader; folding the branch would still NOP the CMP and break
       * that consumer, so bail. */
      if (pop == TCCIR_OP_SETIF || pop == TCCIR_OP_SELECT) break;
      /* BB boundary. */
      if (pop == TCCIR_OP_JUMP || pop == TCCIR_OP_JUMPIF ||
          pop == TCCIR_OP_IJUMP || pop == TCCIR_OP_SWITCH_TABLE ||
          pop == TCCIR_OP_RETURNVALUE || pop == TCCIR_OP_RETURNVOID)
        break;
      /* Other ops (ASSIGN, ADD, LOAD, STORE, ...) don't write flags. */
    }
    if (cmp_idx < 0) continue;

    IRQuadCompact *cmp_q = &ir->compact_instructions[cmp_idx];
    IROperand src1 = tcc_ir_op_get_src1(ir, cmp_q);
    IROperand src2 = tcc_ir_op_get_src2(ir, cmp_q);

    int64_t v1, v2;
    if (!ra_try_resolve_const_local(ir, is_target, src1, cmp_idx, &v1)) continue;
    if (!ra_try_resolve_const_local(ir, is_target, src2, cmp_idx, &v2)) continue;

    /* Truncate to operand width to match comparison semantics. */
    int cmp_btype = irop_get_btype(src1);
    if (cmp_btype != IROP_BTYPE_INT64) {
      v1 = (int64_t)(int32_t)(uint32_t)v1;
      v2 = (int64_t)(int32_t)(uint32_t)v2;
    }

    IROperand cond = tcc_ir_op_get_src1(ir, q);
    int tok = (int)irop_get_imm64_ex(ir, cond);
    int result = ra_eval_cmp_cond(v1, v2, tok);
    if (result < 0) continue;

    if (result) {
      /* Always taken: convert JUMPIF into unconditional JUMP. */
      IROperand target = tcc_ir_op_get_dest(ir, q);
      cmp_q->op = TCCIR_OP_NOP;
      q->op = TCCIR_OP_JUMP;
      tcc_ir_set_dest(ir, i, target);
      tcc_ir_set_src1(ir, i, IROP_NONE);
      /* Fall-through is now unreachable up to the next jump target. */
      ra_nop_dead_block(ir, is_target, i + 1);
    } else {
      /* Never taken: drop both CMP and JUMPIF. */
      cmp_q->op = TCCIR_OP_NOP;
      q->op = TCCIR_OP_NOP;
    }
    folds++;
  }

  tcc_free(is_target);
  return folds;
}

/* DISABLED — too aggressive for current heuristic.
 *
 * Eliminate ASSIGN copies whose destination is overwritten before any read.
 * Phi resolution emits a copy per CFG edge for every phi; when an earlier
 * pass folded an edge away, its copies survive as dead stores. Naive linear
 * walk of "no use before redef" is unsound: a jump TO line i (post-i target)
 * can land between i and the redef without going through i's def, so on that
 * path the redef supplies V's value but i was bypassed entirely. Need a
 * proper post-dominance check before re-enabling.
 *
 * Kept here so the diagnosis isn't lost. */
__attribute__((unused))
static int ra_dead_assign_elim(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  if (n <= 1) return 0;

  int killed = 0;
  for (int i = 0; i < n - 1; i++) {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_ASSIGN) continue;

    IROperand d = tcc_ir_op_get_dest(ir, q);
    if (d.is_lval) continue;
    int32_t dv = irop_get_vreg(d);
    if (dv < 0) continue;

    int redef_idx = -1;
    int max_fwd_target = i;
    int aborted = 0;

    for (int k = i + 1; k < n; k++) {
      IRQuadCompact *qk = &ir->compact_instructions[k];
      TccIrOp opk = qk->op;
      if (opk == TCCIR_OP_NOP) continue;

      if (opk == TCCIR_OP_IJUMP || opk == TCCIR_OP_SWITCH_TABLE ||
          opk == TCCIR_OP_RETURNVALUE || opk == TCCIR_OP_RETURNVOID) {
        aborted = 1;
        break;
      }

      if (irop_config[opk].has_src1) {
        IROperand s = tcc_ir_op_get_src1(ir, qk);
        if (!s.is_lval && irop_get_vreg(s) == dv) { aborted = 1; break; }
      }
      if (irop_config[opk].has_src2) {
        IROperand s = tcc_ir_op_get_src2(ir, qk);
        if (!s.is_lval && irop_get_vreg(s) == dv) { aborted = 1; break; }
      }
      if (opk == TCCIR_OP_MLA) {
        IROperand s = tcc_ir_op_get_accum(ir, qk);
        if (!s.is_lval && irop_get_vreg(s) == dv) { aborted = 1; break; }
      }
      if (irop_config[opk].has_dest) {
        IROperand dk = tcc_ir_op_get_dest(ir, qk);
        if (dk.is_lval) {
          if (irop_get_vreg(dk) == dv) { aborted = 1; break; }
        } else if (irop_get_vreg(dk) == dv) {
          if (opk == TCCIR_OP_ASSIGN) {
            redef_idx = k;
            break;
          }
          aborted = 1;
          break;
        }
      }

      if (opk == TCCIR_OP_JUMP || opk == TCCIR_OP_JUMPIF) {
        int target = (int)irop_get_imm64_ex(ir, tcc_ir_op_get_dest(ir, qk));
        if (target > max_fwd_target) max_fwd_target = target;
      }
    }

    if (aborted || redef_idx < 0) continue;
    /* A forward jump targeting beyond the redef would skip it on some path. */
    if (max_fwd_target > redef_idx) continue;

    q->op = TCCIR_OP_NOP;
    killed++;
  }
  return killed;
}

/* ============================================================================
 * Phi-copy / constant-staging chain fold
 *
 * After ra_resolve_phis, switch-case bodies of the form
 *   case N: V0 = const_N; break;
 * arrive at register allocation as a two-instruction chain:
 *   T_case  <- const_N    (ASSIGN, the SSA-renamed original def)
 *   T_phi   <- T_case     (ASSIGN, the phi copy inserted before the JMP)
 *
 * When T_case has exactly one use (the phi copy) and the original ASSIGN's
 * source is a "freely duplicable" constant (IMM/SYMREF/STACKOFF/F32 with
 * inline payload, or another non-lval VREG), we fold the chain into a single
 * ASSIGN: T_phi <- const_N. This halves the per-case instruction count on
 * dense switches (gcc-torture/compile/pr34093.c).
 *
 * Implementation: rewrite the original def's dest from T_case to T_phi and
 * NOP the phi copy. This preserves the def's slot — which carries the
 * basic-block label (is_jump_target) for the case body — and keeps the
 * source operand exactly as it was, so its in-pool payload (symref idx,
 * stack offset, etc.) doesn't need to be rebuilt.
 * ============================================================================ */
static int ra_fold_phi_const_chain(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  if (n < 2)
    return 0;

  int max_tmp = -1;
  for (int i = 0; i < n; i++) {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP) continue;
    if (!irop_config[q->op].has_dest) continue;
    IROperand d = tcc_ir_op_get_dest(ir, q);
    if (d.is_lval) continue;
    int32_t v = irop_get_vreg(d);
    if (v < 0 || TCCIR_DECODE_VREG_TYPE(v) != TCCIR_VREG_TYPE_TEMP) continue;
    int pos = TCCIR_DECODE_VREG_POSITION(v);
    if (pos > max_tmp) max_tmp = pos;
  }
  if (max_tmp < 0)
    return 0;

  /* def_idx[t]: -1 = no def, -2 = multi-def, else index of single def
   * def_count[t]: number of times t is written (caps at 2). */
  int *def_idx = tcc_malloc(sizeof(int) * (max_tmp + 1));
  int *use_count = tcc_mallocz(sizeof(int) * (max_tmp + 1));
  int *def_count = tcc_mallocz(sizeof(int) * (max_tmp + 1));
  for (int p = 0; p <= max_tmp; p++) def_idx[p] = -1;

  for (int i = 0; i < n; i++) {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP) continue;
    int op = q->op;

    if (irop_config[op].has_dest) {
      IROperand d = tcc_ir_op_get_dest(ir, q);
      if (!d.is_lval) {
        int32_t v = irop_get_vreg(d);
        if (v >= 0 && TCCIR_DECODE_VREG_TYPE(v) == TCCIR_VREG_TYPE_TEMP) {
          int pos = TCCIR_DECODE_VREG_POSITION(v);
          if (pos <= max_tmp) {
            if (def_idx[pos] == -1) def_idx[pos] = i;
            else def_idx[pos] = -2;
            if (def_count[pos] < 3) def_count[pos]++;
          }
        }
      } else if (op == TCCIR_OP_STORE || op == TCCIR_OP_STORE_INDEXED ||
                 op == TCCIR_OP_STORE_POSTINC) {
        int32_t v = irop_get_vreg(d);
        if (v >= 0 && TCCIR_DECODE_VREG_TYPE(v) == TCCIR_VREG_TYPE_TEMP) {
          int pos = TCCIR_DECODE_VREG_POSITION(v);
          if (pos <= max_tmp) use_count[pos]++;
        }
      }
    }
    if (irop_config[op].has_src1) {
      IROperand s = tcc_ir_op_get_src1(ir, q);
      int32_t v = irop_get_vreg(s);
      if (v >= 0 && TCCIR_DECODE_VREG_TYPE(v) == TCCIR_VREG_TYPE_TEMP) {
        int pos = TCCIR_DECODE_VREG_POSITION(v);
        if (pos <= max_tmp) use_count[pos]++;
      }
    }
    if (irop_config[op].has_src2) {
      IROperand s = tcc_ir_op_get_src2(ir, q);
      int32_t v = irop_get_vreg(s);
      if (v >= 0 && TCCIR_DECODE_VREG_TYPE(v) == TCCIR_VREG_TYPE_TEMP) {
        int pos = TCCIR_DECODE_VREG_POSITION(v);
        if (pos <= max_tmp) use_count[pos]++;
      }
    }
    if (op == TCCIR_OP_MLA) {
      IROperand s = tcc_ir_op_get_accum(ir, q);
      int32_t v = irop_get_vreg(s);
      if (v >= 0 && TCCIR_DECODE_VREG_TYPE(v) == TCCIR_VREG_TYPE_TEMP) {
        int pos = TCCIR_DECODE_VREG_POSITION(v);
        if (pos <= max_tmp) use_count[pos]++;
      }
    }
  }

  int folded = 0;
  for (int i = 1; i < n; i++) {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_ASSIGN) continue;

    IROperand u_dest = tcc_ir_op_get_dest(ir, q);
    if (u_dest.is_lval) continue;
    int32_t u_dest_vr = irop_get_vreg(u_dest);
    /* Allow any non-lval dest: the def will inherit it. */
    if (u_dest_vr < 0) continue;

    IROperand u_src = tcc_ir_op_get_src1(ir, q);
    if (u_src.is_lval || u_src.is_llocal) continue;
    int32_t src_vr = irop_get_vreg(u_src);
    if (src_vr < 0 || TCCIR_DECODE_VREG_TYPE(src_vr) != TCCIR_VREG_TYPE_TEMP) continue;
    int src_pos = TCCIR_DECODE_VREG_POSITION(src_vr);
    if (src_pos > max_tmp) continue;

    if (use_count[src_pos] != 1) continue;
    int j = def_idx[src_pos];
    if (j < 0 || j >= i) continue;

    /* Only fold when u_dest is a phi-style target with multiple defs.
     * For straight-line single-def TMPs the chain has no payoff and the
     * fold is more aggressive than the IR optimiser intended (regression
     * source for non-switch tests like test_mul32wide_outparams). */
    int u_dest_type = TCCIR_DECODE_VREG_TYPE(u_dest_vr);
    if (u_dest_type != TCCIR_VREG_TYPE_TEMP) continue;
    int u_dest_pos = TCCIR_DECODE_VREG_POSITION(u_dest_vr);
    if (u_dest_pos > max_tmp) continue;
    if (def_count[u_dest_pos] < 2) continue;

    IRQuadCompact *def_q = &ir->compact_instructions[j];
    if (def_q->op != TCCIR_OP_ASSIGN) continue;

    IROperand def_src = tcc_ir_op_get_src1(ir, def_q);
    /* Only fold safe-to-duplicate constant-like sources. We rule out memory
     * reads (is_lval) because the use's dest may differ in btype and we'd
     * need to preserve the load width. Pure IMM/SYMREF/STACKOFF/F32 carry
     * their payload inline; F64/I64/SYMREF via pool_idx survive a copy. */
    if (def_src.is_lval || def_src.is_llocal) continue;
    int dtag = def_src.tag;
    int safe_const = (dtag == IROP_TAG_IMM32 || dtag == IROP_TAG_F32 ||
                      dtag == IROP_TAG_I64 || dtag == IROP_TAG_F64 ||
                      dtag == IROP_TAG_SYMREF || dtag == IROP_TAG_STACKOFF);
    if (!safe_const) continue;

    /* btype must match across the entire chain: u_dest_bt = def_dest_bt =
     * def_src_bt. Any width difference would change the semantics of the
     * implicit widen/narrow ASSIGN performs (e.g. ZEXT of a 32-bit constant
     * into a 64-bit T_src that the use then reads as a register pair). */
    int u_dest_bt = irop_get_btype(u_dest);
    int def_dest_bt = irop_get_btype(tcc_ir_op_get_dest(ir, def_q));
    int def_src_bt = irop_get_btype(def_src);
    if (u_dest_bt != def_dest_bt || u_dest_bt != def_src_bt)
      continue;
    /* And the source vreg's btype recorded on the use must match too, so we
     * never collapse a narrowing read of a wider T_src. */
    if (irop_get_btype(u_src) != u_dest_bt)
      continue;

    /* Same basic block: no jump-target landing zones between def and use.
     * The def itself can be a jump target (start of the case body); only
     * intervening landing zones break the chain. */
    int same_bb = 1;
    for (int k = j + 1; k <= i; k++) {
      if (ir->compact_instructions[k].is_jump_target) { same_bb = 0; break; }
    }
    if (!same_bb) continue;

    /* Make sure u_dest isn't redefined or read between j+1 and i-1 — if it
     * were, rewriting j's dest to u_dest would change semantics. */
    int conflict = 0;
    for (int k = j + 1; k < i && !conflict; k++) {
      IRQuadCompact *kq = &ir->compact_instructions[k];
      if (kq->op == TCCIR_OP_NOP) continue;
      if (irop_config[kq->op].has_dest) {
        IROperand kd = tcc_ir_op_get_dest(ir, kq);
        if (!kd.is_lval && irop_get_vreg(kd) == u_dest_vr) { conflict = 1; break; }
      }
      if (irop_config[kq->op].has_src1) {
        IROperand ks = tcc_ir_op_get_src1(ir, kq);
        if (irop_get_vreg(ks) == u_dest_vr) { conflict = 1; break; }
      }
      if (irop_config[kq->op].has_src2) {
        IROperand ks = tcc_ir_op_get_src2(ir, kq);
        if (irop_get_vreg(ks) == u_dest_vr) { conflict = 1; break; }
      }
    }
    if (conflict) continue;

    /* Apply fold: rewrite def's dest to u_dest, NOP the use. */
    tcc_ir_set_dest(ir, j, u_dest);
    q->op = TCCIR_OP_NOP;
    folded++;
  }

  tcc_free(def_idx);
  tcc_free(use_count);
  tcc_free(def_count);
  return folded;
}

/* ============================================================================
 * Graph-based register coalescing — helpers
 * ============================================================================ */

static int ra_coalesce_level(void)
{
  static int cached = -2;
  if (cached == -2) {
    if (getenv("TCC_NO_COALESCE"))
      cached = 0;
    else {
      const char *e = getenv("TCC_COALESCE");
      cached = (e && e[0]) ? atoi(e) : 2; /* default: apply, pure copies */
    }
  }
  return cached;
}

/* Collect an instruction's def and use vreg operands into out-params.
 * STORE-class dest is a USE (it is the address); MLA accumulator is a USE.
 * Matches ra_build_intervals' operand semantics exactly. */
static void ra_co_ops(TCCIRState *ir, IRQuadCompact *q,
                      int32_t *out_def, int *has_def, int32_t uses[4], int *nuse)
{
  *has_def = 0;
  *nuse = 0;
  if (q->op == TCCIR_OP_NOP) return;
  int store_class = (q->op == TCCIR_OP_STORE || q->op == TCCIR_OP_STORE_INDEXED ||
                     q->op == TCCIR_OP_STORE_POSTINC);
  if (irop_config[q->op].has_src1) {
    IROperand s = tcc_ir_op_get_src1(ir, q);
    if (irop_has_vreg(s) && !irop_is_immediate(s)) uses[(*nuse)++] = irop_get_vreg(s);
  }
  if (irop_config[q->op].has_src2) {
    IROperand s = tcc_ir_op_get_src2(ir, q);
    if (irop_has_vreg(s) && !irop_is_immediate(s)) uses[(*nuse)++] = irop_get_vreg(s);
  }
  if (q->op == TCCIR_OP_MLA) {
    IROperand s = tcc_ir_op_get_accum(ir, q);
    if (irop_has_vreg(s) && !irop_is_immediate(s)) uses[(*nuse)++] = irop_get_vreg(s);
  }
  if (irop_config[q->op].has_dest) {
    IROperand d = tcc_ir_op_get_dest(ir, q);
    if (irop_has_vreg(d)) {
      if (store_class) uses[(*nuse)++] = irop_get_vreg(d);
      else { *out_def = irop_get_vreg(d); *has_def = 1; }
    }
  }
}

/* Static counts of references from 16-bit-encodable ops; docs/regalloc_narrow_pref.md */
static void ra_build_narrow_weights(TCCIRState *ir, const RegAllocTarget *target,
                                    SSAInterval *intervals, int count, int max_vreg_pos)
{
  if (!target->op_narrow_capable || tcc_state->optimize < 1 || count <= 0 || max_vreg_pos <= 0)
    return;
  if (tcc_ir_opt_pass_disabled("ra:narrow_pref"))
    return;
  int tbl = 4 * max_vreg_pos;
  int32_t *iv_of = tcc_malloc(sizeof(int32_t) * tbl);
  for (int i = 0; i < tbl; i++) iv_of[i] = -1;
  for (int i = 0; i < count; i++) {
    int idx = TCCIR_DECODE_VREG_TYPE(intervals[i].vreg) * max_vreg_pos +
              TCCIR_DECODE_VREG_POSITION(intervals[i].vreg);
    if (idx >= 0 && idx < tbl) iv_of[idx] = i;
  }
  for (int i = 0; i < ir->next_instruction_index; i++) {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP) continue;
    int src2_imm = 0, scale = 0;
    if (irop_config[q->op].has_src2)
      src2_imm = irop_is_immediate(tcc_ir_op_get_src2(ir, q));
    if (q->op == TCCIR_OP_LOAD_INDEXED || q->op == TCCIR_OP_STORE_INDEXED) {
      IROperand s = tcc_ir_op_get_scale(ir, q);
      if (irop_get_tag(s) == IROP_TAG_IMM32) scale = s.u.imm32;
    }
    if (!target->op_narrow_capable(q->op, src2_imm, scale)) continue;
    int32_t def, uses[4];
    int has_def, nuse;
    ra_co_ops(ir, q, &def, &has_def, uses, &nuse);
    int32_t ops[5];
    int nops = 0;
    if (has_def) ops[nops++] = def;
    for (int k = 0; k < nuse; k++) ops[nops++] = uses[k];
    for (int k = 0; k < nops; k++) {
      int idx = TCCIR_DECODE_VREG_TYPE(ops[k]) * max_vreg_pos +
                TCCIR_DECODE_VREG_POSITION(ops[k]);
      if (idx < 0 || idx >= tbl || iv_of[idx] < 0) continue;
      SSAInterval *iv = &intervals[iv_of[idx]];
      if (iv->narrow_uses < UINT16_MAX) iv->narrow_uses++;
    }
  }
  tcc_free(iv_of);
}

/* ============================================================================
 * Accurate per-instruction liveness refinement
 *
 * Refine live_regs_by_instruction (the interval-derived approximation the
 * scratch-register picker consults) with ACCURATE per-instruction liveness from
 * a real CFG backward dataflow.
 *
 * The interval bitmap models each value as one contiguous [start,end] range.
 * For a loop-carried value (defined inside a rotated loop body and live across
 * the back-edge into the next iteration) that single range does NOT span the
 * loop-header prefix where the value is still live, so the bitmap under-reports
 * the value's register as free there.  The scratch picker then hands it out and
 * clobbers the loop-carried value (random-C O2 wrong-code / HardFault once loop
 * rotation is enabled — Finding #15 follow-up, seeds 244 et al).
 *
 * This dataflow (same ra_co_ops def/use model the graph-coalescer trusts) marks
 * every register holding a genuinely-live, register-resident vreg.  It is
 * strictly conservative for the picker: it can only ADD live bits, never remove
 * them, so it can never introduce a new clobber — it only prevents real ones.
 * Bails (leaving the interval bitmap as-is) on functions with un-enumerated
 * edges (IJUMP / SWITCH_*), matching the coalescer's own guard. */
static void ra_refine_live_regs_accurate(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  if (n <= 0) return;
  for (int i = 0; i < n; i++) {
    int op = ir->compact_instructions[i].op;
    if (op == TCCIR_OP_IJUMP || op == TCCIR_OP_SWITCH_TABLE || op == TCCIR_OP_SWITCH_LOAD)
      return;
  }
  IRCFG *cfg = tcc_ir_cfg_build(ir);
  if (!cfg) return;
  tcc_ir_cfg_compute_dominators(cfg);
  int nb = cfg->num_blocks;
  if (nb <= 0) { tcc_ir_cfg_free(cfg); return; }
  /* vreg index space */
  int maxpos = 1;
  for (int j = 0; j < ir->ls.next_interval_index; j++) {
    int p = TCCIR_DECODE_VREG_POSITION(ir->ls.intervals[j].vreg);
    if (p + 1 > maxpos) maxpos = p + 1;
  }
  int tbl = 4 * maxpos;
  int nw = (tbl + 63) / 64;
  #define DVIDX(vr) ((TCCIR_DECODE_VREG_TYPE(vr) * maxpos) + TCCIR_DECODE_VREG_POSITION(vr))
  /* vreg -> physical regs */
  int8_t *vr0 = tcc_malloc(tbl); int8_t *vr1 = tcc_malloc(tbl);
  for (int i = 0; i < tbl; i++) { vr0[i] = -1; vr1[i] = -1; }
  for (int j = 0; j < ir->ls.next_interval_index; j++) {
    LSLiveInterval *iv = &ir->ls.intervals[j];
    if (iv->stack_location != 0) continue;
    int vi = DVIDX(iv->vreg);
    if (vi < 0 || vi >= tbl) continue;
    vr0[vi] = (int8_t)iv->r0; vr1[vi] = (int8_t)iv->r1;
  }
  uint64_t *useb = tcc_mallocz(sizeof(uint64_t)*(size_t)nb*nw);
  uint64_t *defbk= tcc_mallocz(sizeof(uint64_t)*(size_t)nb*nw);
  uint64_t *livein=tcc_mallocz(sizeof(uint64_t)*(size_t)nb*nw);
  uint64_t *liveout=tcc_mallocz(sizeof(uint64_t)*(size_t)nb*nw);
  for (int b = 0; b < nb; b++) {
    uint64_t *ub = useb + (size_t)b*nw, *db = defbk + (size_t)b*nw;
    int s = cfg->blocks[b].start_idx, e = cfg->blocks[b].end_idx;
    for (int i = s; i < e && i < n; i++) {
      int32_t def=-1, hd=0, uses[4], nu=0;
      ra_co_ops(ir, &ir->compact_instructions[i], &def, &hd, uses, &nu);
      for (int k=0;k<nu;k++){ if(!tcc_ir_vreg_is_valid(ir,uses[k]))continue; int u=DVIDX(uses[k]); if(u<0||u>=tbl)continue; if(!RA_BS_TEST(db,u)) RA_BS_SET(ub,u);}
      if (hd && tcc_ir_vreg_is_valid(ir,def)){int d=DVIDX(def); if(d>=0&&d<tbl) RA_BS_SET(db,d);}
    }
  }
  int changed=1, guard=0;
  while (changed && guard++ < nb+4) {
    changed=0;
    for (int ri=cfg->rpo_count-1; ri>=0; ri--) {
      int b = cfg->rpo_order ? cfg->rpo_order[ri] : ri;
      if (b<0||b>=nb) continue;
      uint64_t *lo=liveout+(size_t)b*nw,*li=livein+(size_t)b*nw,*ub=useb+(size_t)b*nw,*db=defbk+(size_t)b*nw;
      for (int w=0;w<nw;w++) lo[w]=0;
      for (int si=0;si<cfg->blocks[b].num_succs;si++){int sb=cfg->blocks[b].succs[si]; if(sb<0||sb>=nb)continue; uint64_t*sli=livein+(size_t)sb*nw; for(int w=0;w<nw;w++) lo[w]|=sli[w];}
      for (int w=0;w<nw;w++){uint64_t nv=ub[w]|(lo[w]&~db[w]); if(nv!=li[w]){li[w]=nv;changed=1;}}
    }
  }
  /* Loop-liveness completion.  A value live at a loop header is live throughout
   * the ENTIRE loop body (it round-trips the back-edge), but the interval model
   * gives it a single [def,last-use] range that leaves the loop-header prefix
   * uncovered — the scratch picker then reuses its register inside the loop and
   * clobbers the loop-carried value (seed 244).  For each back-edge, OR the
   * registers live-IN at the loop header across the whole loop body [header,
   * back-edge].  Scoped to loop bodies on purpose: a blanket per-instruction
   * live-out refinement also marks straight-line liveness the interval model
   * intentionally omits, which over-constrains the scratch picker and perturbs
   * unrelated functions into latent-bug territory (seed 221). */
  for (int bi = 0; bi < n; bi++) {
    IRQuadCompact *q = &ir->compact_instructions[bi];
    if (q->op != TCCIR_OP_JUMP && q->op != TCCIR_OP_JUMPIF) continue;
    int t = (int)tcc_ir_op_get_dest(ir, q).u.imm32;
    if (t < 0 || t >= bi) continue; /* not a back-edge */
    /* live-in at the loop header t: find the block starting at t. */
    int hb = -1;
    for (int b = 0; b < nb; b++) if (cfg->blocks[b].start_idx == t) { hb = b; break; }
    if (hb < 0) continue;
    uint64_t *hli = livein + (size_t)hb*nw;
    uint32_t mask = 0;
    for (int vi=0; vi<tbl; vi++) {
      if (!RA_BS_TEST(hli,vi)) continue;
      if (vr0[vi]>=0 && vr0[vi]<16) mask |= (1u<<vr0[vi]);
      if (vr1[vi]>=0 && vr1[vi]<16) mask |= (1u<<vr1[vi]);
    }
    mask &= 0x1FFFu; /* R0-R12 */
    if (!mask || !ir->ls.live_regs_by_instruction) continue;
    int e = bi; if (e >= ir->ls.live_regs_by_instruction_size) e = ir->ls.live_regs_by_instruction_size - 1;
    for (int k = t; k <= e; k++)
      ir->ls.live_regs_by_instruction[k] |= mask;
  }
  #undef DVIDX
  tcc_free(vr0);tcc_free(vr1);tcc_free(useb);tcc_free(defbk);tcc_free(livein);tcc_free(liveout);
  tcc_ir_cfg_free(cfg);
}

/* ============================================================================
 * Graph-based register coalescing
 *
 * The linear scan's per-decision coalescing (boundary / loop-phi / exit-phi
 * "transfer") merges only simple 2-vreg copy chains; it cannot merge a
 * multi-predecessor merge-phi (e.g. an induction variable that, after loop
 * rotation, enters the next loop via two edges).  And ra_build_intervals'
 * single-[start,end] model reports FALSE overlaps for SSA versions that are
 * live on mutually-exclusive paths, so they look like they interfere.
 *
 * This pass builds ACCURATE liveness (backward dataflow over a fresh CFG) and a
 * real interference graph, then conservatively coalesces copy-related
 * non-interfering vregs (union-find).  Merged classes are applied via the
 * `coalesce_to` interval-merge: one representative interval covers the union of
 * the class' ranges, non-reps are skipped by the scan and inherit the rep's
 * register, and the residual `mov R,R` is erased by tcc_ir_move_coalescing.
 *
 * On by default at -O1+ (level 2 = apply, INT single-register, pure copies).
 * Overrides: TCC_NO_COALESCE disables it; TCC_COALESCE=N forces level N
 * (1 = compute only, 2 = apply, 3 = also coalesce RMW two-address edges).
 * Bails on functions with un-enumerated CFG edges (IJUMP / SWITCH_*) or any
 * 64-bit (LLONG) value.
 * ============================================================================ */

#define RA_BS_SET(bs, i)  ((bs)[(i) >> 6] |= (1ull << ((i) & 63)))
#define RA_BS_CLR(bs, i)  ((bs)[(i) >> 6] &= ~(1ull << ((i) & 63)))
#define RA_BS_TEST(bs, i) (((bs)[(i) >> 6] >> ((i) & 63)) & 1ull)

static void ra_coalesce_graph(TCCIRState *ir, SSAInterval *intervals, int count,
                              int max_vreg_pos)
{
  int level = ra_coalesce_level();
  if (level <= 0 || tcc_state->optimize < 1 || count <= 1 || max_vreg_pos <= 0)
    return;
  int n = ir->next_instruction_index;
  if (n <= 0) return;

  /* ---- Stage 1: fresh CFG (entry cfg is stale after phi resolution). ---- */
  IRCFG *cfg = tcc_ir_cfg_build(ir);
  if (!cfg) return;
  tcc_ir_cfg_compute_dominators(cfg); /* populates rpo_order/rpo_count for the dataflow */
  if (cfg->rpo_count <= 0) { tcc_ir_cfg_free(cfg); return; }
  for (int i = 0; i < n; i++) {
    int op = ir->compact_instructions[i].op;
    if (op == TCCIR_OP_IJUMP || op == TCCIR_OP_SWITCH_TABLE || op == TCCIR_OP_SWITCH_LOAD) {
      tcc_ir_cfg_free(cfg);
      return; /* un-enumerated edges → can't trust live-out */
    }
  }
  int nb = cfg->num_blocks;
  int tbl = 4 * max_vreg_pos;
  int nw = (tbl + 63) / 64;
  /* Guard pathologically large functions (compile-time / memory). */
  if (nb <= 0 || (long)nb * nw > (4L << 20)) { tcc_ir_cfg_free(cfg); return; }

  /* Bail on functions containing 64-bit-int (LLONG) values: their multiply
   * decomposition (UMULL + cross-term MUL/MLA) spills half-operands under
   * pressure through a path that is fragile to register-assignment changes —
   * coalescing perturbs it into a miscompile (gcc-torture bug_ull_mul).  Pure
   * 32-bit kernels (the loop code this targets, e.g. memclr) are unaffected. */
  for (int i = 0; i < count; i++)
    if (intervals[i].reg_type == LS_REG_TYPE_LLONG) { tcc_ir_cfg_free(cfg); return; }

  #define VIDX(vr) ((TCCIR_DECODE_VREG_TYPE(vr) * max_vreg_pos) + TCCIR_DECODE_VREG_POSITION(vr))

  int *iv_of = tcc_malloc(sizeof(int) * tbl);
  for (int i = 0; i < tbl; i++) iv_of[i] = -1;
  for (int i = 0; i < count; i++) {
    int idx = VIDX(intervals[i].vreg);
    if (idx >= 0 && idx < tbl) iv_of[idx] = i;
  }

  /* ---- Stage 2: backward liveness dataflow (live_in/live_out per block). ---- */
  uint64_t *useb   = tcc_mallocz(sizeof(uint64_t) * (size_t)nb * nw);
  uint64_t *defbk  = tcc_mallocz(sizeof(uint64_t) * (size_t)nb * nw);
  uint64_t *livein = tcc_mallocz(sizeof(uint64_t) * (size_t)nb * nw);
  uint64_t *liveout= tcc_mallocz(sizeof(uint64_t) * (size_t)nb * nw);

  for (int b = 0; b < nb; b++) {
    uint64_t *ub = useb + (size_t)b * nw, *db = defbk + (size_t)b * nw;
    int s = cfg->blocks[b].start_idx, e = cfg->blocks[b].end_idx;
    for (int i = s; i < e && i < n; i++) {
      int32_t def = -1, hd = 0, uses[4], nu = 0;
      ra_co_ops(ir, &ir->compact_instructions[i], &def, &hd, uses, &nu);
      for (int k = 0; k < nu; k++) {
        if (!tcc_ir_vreg_is_valid(ir, uses[k])) continue;
        int u = VIDX(uses[k]);
        if (u < 0 || u >= tbl) continue;
        if (!RA_BS_TEST(db, u)) RA_BS_SET(ub, u); /* upward-exposed use */
      }
      if (hd && tcc_ir_vreg_is_valid(ir, def)) {
        int d = VIDX(def);
        if (d >= 0 && d < tbl) RA_BS_SET(db, d);
      }
    }
  }

  /* Fixpoint over reverse-RPO order. */
  int changed = 1, guard = 0;
  while (changed && guard++ < nb + 4) {
    changed = 0;
    for (int ri = cfg->rpo_count - 1; ri >= 0; ri--) {
      int b = cfg->rpo_order ? cfg->rpo_order[ri] : ri;
      if (b < 0 || b >= nb) continue;
      uint64_t *lo = liveout + (size_t)b * nw, *li = livein + (size_t)b * nw;
      uint64_t *ub = useb + (size_t)b * nw, *db = defbk + (size_t)b * nw;
      /* live_out = union of successors' live_in */
      for (int w = 0; w < nw; w++) lo[w] = 0;
      for (int si = 0; si < cfg->blocks[b].num_succs; si++) {
        int sb = cfg->blocks[b].succs[si];
        if (sb < 0 || sb >= nb) continue;
        uint64_t *sli = livein + (size_t)sb * nw;
        for (int w = 0; w < nw; w++) lo[w] |= sli[w];
      }
      /* live_in = use ∪ (live_out − def) */
      for (int w = 0; w < nw; w++) {
        uint64_t nv = ub[w] | (lo[w] & ~db[w]);
        if (nv != li[w]) { li[w] = nv; changed = 1; }
      }
    }
  }

  /* ---- Register-pressure gate. ----
   * Coalescing can only ever ADD instructions (vs the baseline) by forcing a
   * spill: merging two non-interfering values reduces distinct values at every
   * point EXCEPT a liveness hole, where the merged interval occupies the
   * register and raises pressure by one.  If the function's peak INT pressure
   * leaves headroom (< K allocatable int regs), no spill can result, so
   * coalescing is a pure win (it only removes copies).  When pressure already
   * reaches K (spilling territory), coalescing's longer intervals can perturb
   * the linear scan into worse spills (observed: large high-pressure functions
   * regress).  Bail in that case — it costs only the high-pressure functions,
   * never the small loop kernels (e.g. memclr) this is built for. */
  {
    int K = tcc_state->registers_for_allocator;
    if (K <= 0 || K > 13) K = 13;
    uint64_t *isint = tcc_mallocz(sizeof(uint64_t) * nw);
    for (int i = 0; i < count; i++) {
      if (intervals[i].reg_type != LS_REG_TYPE_INT || intervals[i].r1 >= 0) continue;
      int idx = VIDX(intervals[i].vreg);
      if (idx >= 0 && idx < tbl) RA_BS_SET(isint, idx);
    }
    uint64_t *live = tcc_malloc(sizeof(uint64_t) * nw);
    int maxp = 0;
    for (int b = 0; b < nb && maxp < K; b++) {
      uint64_t *lo = liveout + (size_t)b * nw;
      for (int w = 0; w < nw; w++) live[w] = lo[w];
      int s = cfg->blocks[b].start_idx, e = cfg->blocks[b].end_idx;
      for (int i = e - 1; i >= s && i < n; i--) {
        int p = 0;
        for (int w = 0; w < nw; w++) p += __builtin_popcountll(live[w] & isint[w]);
        if (p > maxp) maxp = p;
        IRQuadCompact *q = &ir->compact_instructions[i];
        int32_t def = -1, hd = 0, uses[4], nu = 0;
        ra_co_ops(ir, q, &def, &hd, uses, &nu);
        if (hd && tcc_ir_vreg_is_valid(ir, def)) { int d = VIDX(def); if (d >= 0 && d < tbl) RA_BS_CLR(live, d); }
        for (int k = 0; k < nu; k++) { if (!tcc_ir_vreg_is_valid(ir, uses[k])) continue; int u = VIDX(uses[k]); if (u >= 0 && u < tbl) RA_BS_SET(live, u); }
      }
    }
    tcc_free(isint); tcc_free(live);
    if (maxp >= K) {
      RA_DBG("coalesce: skip — peak INT pressure %d >= K=%d", maxp, K);
      tcc_free(iv_of); tcc_free(useb); tcc_free(defbk); tcc_free(livein);
      tcc_free(liveout); tcc_free(instr_block);
      tcc_free(cand_id); tcc_free(edge_d); tcc_free(edge_s);
      tcc_ir_cfg_free(cfg);
      return;
    }
  }

  /* ---- Build per-block def bitmap to detect phi-related copies. ----
   * A copy dest that is defined in more than one block is a phi result
   * (explicit copies inserted after SSA phi resolution).  Coalescing such
   * a dest with its source can overwrite the source's value on a sibling
   * phi arm when the source is still live across the merge (seed 860). */
  uint64_t *def_blocks = tcc_mallocz(sizeof(uint64_t) * (size_t)nb * nw);
  int *instr_block = tcc_malloc(sizeof(int) * n);
  for (int i = 0; i < n; i++) instr_block[i] = -1;
  for (int b = 0; b < nb; b++) {
    int s = cfg->blocks[b].start_idx, e = cfg->blocks[b].end_idx;
    for (int i = s; i < e && i < n; i++) {
      instr_block[i] = b;
      IRQuadCompact *q = &ir->compact_instructions[i];
      int32_t def = -1, hd = 0, uses[4], nu = 0;
      ra_co_ops(ir, q, &def, &hd, uses, &nu);
      if (hd && tcc_ir_vreg_is_valid(ir, def)) {
        int d = VIDX(def);
        if (d >= 0 && d < tbl)
          RA_BS_SET(def_blocks + (size_t)b * nw, d);
      }
    }
  }

  /* ---- Collect copy edges + candidate set (Stage 4 prep). ---- */
  /* Copy edge kinds: ASSIGN dst<-src; two-address dst<-src OP imm (ADD/SUB). */
  int *cand_id = tcc_malloc(sizeof(int) * tbl);
  for (int i = 0; i < tbl; i++) cand_id[i] = -1;
  int ncand = 0;
  int ecap = 16, ne = 0;
  int32_t *edge_d = tcc_malloc(sizeof(int32_t) * ecap);
  int32_t *edge_s = tcc_malloc(sizeof(int32_t) * ecap);
  #define ADD_CAND(vidx) do { if (cand_id[vidx] < 0) cand_id[vidx] = ncand++; } while (0)
  for (int i = 0; i < n; i++) {
    IRQuadCompact *q = &ir->compact_instructions[i];
    int32_t dv = -1, sv = -1;
    if (q->op == TCCIR_OP_ASSIGN) {
      IROperand d = tcc_ir_op_get_dest(ir, q), s = tcc_ir_op_get_src1(ir, q);
      /* Pure register copy only: a dereferenced operand means this ASSIGN is a
       * load/store (`T1 = *T0`), NOT a copy — coalescing its operands is wrong. */
      if (irop_has_vreg(d) && irop_has_vreg(s) && !irop_is_immediate(s) &&
          !d.is_lval && !s.is_lval) {
        dv = irop_get_vreg(d); sv = irop_get_vreg(s);
      }
    } else if ((q->op == TCCIR_OP_ADD || q->op == TCCIR_OP_SUB) && level >= 3) {
      IROperand d = tcc_ir_op_get_dest(ir, q), s1 = tcc_ir_op_get_src1(ir, q),
                s2 = tcc_ir_op_get_src2(ir, q);
      if (irop_has_vreg(d) && irop_has_vreg(s1) && !irop_is_immediate(s1) &&
          irop_is_immediate(s2)) { /* dst <- src OP imm (RMW, two-address) */
        dv = irop_get_vreg(d); sv = irop_get_vreg(s1);
      }
    }
    if (dv < 0 || sv < 0 || dv == sv) continue;
    if (!tcc_ir_vreg_is_valid(ir, dv) || !tcc_ir_vreg_is_valid(ir, sv)) continue;
    int di = VIDX(dv), si = VIDX(sv);
    if (di < 0 || di >= tbl || si < 0 || si >= tbl) continue;
    if (iv_of[di] < 0 || iv_of[si] < 0) continue; /* both must have intervals */
    /* Reject unsafe phi-result copies: the dest is defined on multiple
     * incoming edges.  The dangerous case is when the source temp is itself
     * a copy of a VAR that is live-out of the merge block; coalescing the
     * phi result with that source (transitively with the VAR) lets a sibling
     * phi arm overwrite the still-live VAR (seed 860).  Latch-style loop
     * phis, where the source is computed in the latch, are unaffected. */
    {
      int def_bc = 0;
      for (int b = 0; b < nb; b++) {
        if (RA_BS_TEST(def_blocks + (size_t)b * nw, di)) {
          def_bc++;
          if (def_bc > 1) break;
        }
      }
      if (def_bc > 1) {
        /* Allow phi-copy coalescing only when the merge block is a loop header
         * (one of its predecessors is a back edge, i.e. the merge block dominates
         * that predecessor).  Loop phis coalesce safely because the latch source
         * is not live-out of the header.  Conditional-merge phis can have a
         * source equivalent to a variable live across the merge; coalescing them
         * lets the sibling arm overwrite that variable (seed 860). */
        int bi = instr_block[i];
        int is_loop_header = 0;
        if (bi >= 0 && bi < nb) {
          for (int pi = 0; pi < cfg->blocks[bi].num_preds; pi++) {
            int pb = cfg->blocks[bi].preds[pi];
            if (pb >= 0 && pb < nb && tcc_ir_cfg_dominates(cfg, bi, pb)) {
              is_loop_header = 1;
              break;
            }
          }
        }
        if (!is_loop_header) {
          continue;
        }
      }
    }
    ADD_CAND(di); ADD_CAND(si);
    if (ne >= ecap) { ecap *= 2; edge_d = tcc_realloc(edge_d, sizeof(int32_t)*ecap);
                      edge_s = tcc_realloc(edge_s, sizeof(int32_t)*ecap); }
    edge_d[ne] = di; edge_s[ne] = si; ne++;
  }

  tcc_free(def_blocks);

  if (ncand < 2 || ne == 0) {
    tcc_free(iv_of); tcc_free(useb); tcc_free(defbk); tcc_free(livein);
    tcc_free(liveout); tcc_free(instr_block);
    tcc_free(cand_id); tcc_free(edge_d); tcc_free(edge_s);
    tcc_ir_cfg_free(cfg);
    return;
  }

  /* Reverse map: candidate index -> VIDX. */
  int *cand_vidx = tcc_malloc(sizeof(int) * ncand);
  for (int i = 0; i < tbl; i++) if (cand_id[i] >= 0) cand_vidx[cand_id[i]] = i;

  /* ---- Stage 3: interference among candidates (per-def live-out). ---- */
  /* Two passes to build CSR adjacency: count degrees, then fill. */
  int *deg = tcc_mallocz(sizeof(int) * ncand);
  uint64_t *live = tcc_malloc(sizeof(uint64_t) * nw);

  /* Helper macro: iterate live candidate indices and run BODY with `c`. */
  #define FOR_LIVE_CAND(BODY) do { \
      for (int w = 0; w < nw; w++) { uint64_t bits = live[w]; \
        while (bits) { int bit = __builtin_ctzll(bits); bits &= bits - 1; \
          int vi = (w << 6) + bit; if (vi >= tbl) break; \
          int c = cand_id[vi]; if (c >= 0) { BODY } } } } while (0)

  for (int pass = 0; pass < 2; pass++) {
    int *adj_start = NULL, *adj = NULL, total = 0;
    if (pass == 1) {
      adj_start = tcc_malloc(sizeof(int) * (ncand + 1));
      adj_start[0] = 0;
      for (int c = 0; c < ncand; c++) adj_start[c + 1] = adj_start[c] + deg[c];
      total = adj_start[ncand];
      adj = total ? tcc_malloc(sizeof(int) * total) : tcc_malloc(1);
      for (int c = 0; c < ncand; c++) deg[c] = adj_start[c]; /* reuse as write cursor */
    }
    for (int b = 0; b < nb; b++) {
      uint64_t *lo = liveout + (size_t)b * nw;
      for (int w = 0; w < nw; w++) live[w] = lo[w];
      int s = cfg->blocks[b].start_idx, e = cfg->blocks[b].end_idx;
      for (int i = e - 1; i >= s && i < n; i--) {
        IRQuadCompact *q = &ir->compact_instructions[i];
        int32_t def = -1, hd = 0, uses[4], nu = 0;
        ra_co_ops(ir, q, &def, &hd, uses, &nu);
        /* Copy source to exclude from interference for this def.  ONLY for a
         * pure copy `D <- S` (D == S after, so they don't interfere here).  For
         * an RMW `D <- S OP imm` the result differs from S, so if S is live-out
         * it genuinely interferes with D — must NOT be excluded. */
        int32_t copy_src = -1;
        if (q->op == TCCIR_OP_ASSIGN) {
          IROperand s1 = tcc_ir_op_get_src1(ir, q);
          IROperand dd = tcc_ir_op_get_dest(ir, q);
          /* Only a pure register copy `D <- S` (no deref) makes D and S equal;
           * a deref ASSIGN is a load/store, so its operands genuinely interfere. */
          if (irop_has_vreg(s1) && !irop_is_immediate(s1) && !s1.is_lval && !dd.is_lval)
            copy_src = irop_get_vreg(s1);
        }
        if (hd && tcc_ir_vreg_is_valid(ir, def)) {
          int d = VIDX(def);
          if (d >= 0 && d < tbl && cand_id[d] >= 0) {
            int cd = cand_id[d];
            int csrc = (copy_src >= 0 && tcc_ir_vreg_is_valid(ir, copy_src)) ? VIDX(copy_src) : -1;
            FOR_LIVE_CAND({
              if (vi == d) continue;
              if (csrc >= 0 && vi == csrc) continue;
              if (pass == 0) { deg[cd]++; deg[c]++; }
              else { adj[deg[cd]++] = c; adj[deg[c]++] = cd; }
            });
          }
        }
        /* transition live: kill def, gen uses */
        if (hd && tcc_ir_vreg_is_valid(ir, def)) {
          int d = VIDX(def); if (d >= 0 && d < tbl) RA_BS_CLR(live, d);
        }
        for (int k = 0; k < nu; k++) {
          if (!tcc_ir_vreg_is_valid(ir, uses[k])) continue;
          int u = VIDX(uses[k]); if (u >= 0 && u < tbl) RA_BS_SET(live, u);
        }
      }
    }
    if (pass == 1) {
      /* ---- Stage 4: union-find conservative coalescing. ---- */
      int *parent = tcc_malloc(sizeof(int) * ncand);
      int *rank = tcc_mallocz(sizeof(int) * ncand);
      int *mnext = tcc_malloc(sizeof(int) * ncand); /* member linked list per root */
      int *seen = tcc_mallocz(sizeof(int) * ncand); /* generation-stamped neighbor set */
      int gen = 0;
      for (int c = 0; c < ncand; c++) { parent[c] = c; mnext[c] = -1; }
      #define UF_FIND(x) ({ int _r = (x); while (parent[_r] != _r) { parent[_r] = parent[parent[_r]]; _r = parent[_r]; } _r; })

      /* K for the Briggs degree test = number of allocatable int regs. */
      int K = tcc_state->registers_for_allocator;
      if (K <= 0 || K > 13) K = 13;

      int merged = 0;
      for (int ei = 0; ei < ne; ei++) {
        int ca = cand_id[edge_d[ei]], cb = cand_id[edge_s[ei]];
        if (ca < 0 || cb < 0) continue;
        int ra = UF_FIND(ca), rb = UF_FIND(cb);
        if (ra == rb) continue;
        SSAInterval *ia = &intervals[iv_of[cand_vidx[ra]]];
        SSAInterval *ib = &intervals[iv_of[cand_vidx[rb]]];
        /* gate: INT single-reg, not precolored/addrtaken/param/spill-fixed */
        if (ia->reg_type != LS_REG_TYPE_INT || ib->reg_type != LS_REG_TYPE_INT) continue;
        if (ia->r1 >= 0 || ib->r1 >= 0) continue;
        if (ia->addrtaken || ib->addrtaken || ia->precolored >= 0 || ib->precolored >= 0) continue;
        if (ia->is_param || ib->is_param) continue;
        /* non-interference: no member of class ra interferes with class rb */
        int interferes = 0;
        for (int m = ra; m >= 0 && !interferes; m = mnext[m]) {
          for (int a = adj_start[m]; a < adj_start[m + 1]; a++) {
            if (UF_FIND(adj[a]) == rb) { interferes = 1; break; }
          }
        }
        if (interferes) continue;
        /* Conservative pressure test: reject if the merged class would have >= K
         * DISTINCT interference-neighbor classes — such a node may be impossible
         * to color, so coalescing it risks forcing a spill (a size regression).
         * Counting all distinct neighbor classes (not just high-degree ones) is a
         * safe over-approximation of Briggs; the IV web has few neighbors so it
         * still coalesces, while high-pressure webs are correctly left alone. */
        {
          int over = 0, distinct = 0;
          gen++;
          for (int side = 0; side < 2 && !over; side++) {
            int r = side ? rb : ra;
            for (int m = r; m >= 0 && !over; m = mnext[m]) {
              for (int a = adj_start[m]; a < adj_start[m + 1]; a++) {
                int nr = UF_FIND(adj[a]);
                if (nr == ra || nr == rb) continue;
                if (seen[nr] != gen) { seen[nr] = gen; if (++distinct >= K) { over = 1; break; } }
              }
            }
          }
          if (over) continue;
        }
        /* union (rank) + splice member lists */
        if (rank[ra] < rank[rb]) { int t = ra; ra = rb; rb = t; }
        parent[rb] = ra;
        if (rank[ra] == rank[rb]) rank[ra]++;
        int tail = ra; while (mnext[tail] >= 0) tail = mnext[tail];
        mnext[tail] = rb;
        merged++;
      }

      /* ---- Stage 5: apply via coalesce_to interval-merge. ---- */
      if (level >= 2 && merged > 0) {
        /* For each root with >1 member, choose rep = earliest-start interval,
         * extend its range to the union, and flag the rest. */
        for (int c = 0; c < ncand; c++) {
          if (UF_FIND(c) != c) continue; /* roots only */
          if (mnext[c] < 0) continue;    /* singleton */
          /* gather members, pick rep */
          int rep_iv = -1; uint32_t lo_s = 0xffffffffu, hi_e = 0;
          int xcall = 0; uint32_t uc = 0, nuc = 0, sum_len = 0;
          for (int m = c; m >= 0; m = mnext[m]) {
            int ivi = iv_of[cand_vidx[m]];
            SSAInterval *iv = &intervals[ivi];
            if (iv->start < lo_s) lo_s = iv->start;
            if (iv->end > hi_e) hi_e = iv->end;
            xcall |= iv->crosses_call;
            uc += iv->use_count;
            nuc += iv->narrow_uses;
            sum_len += iv->end - iv->start + 1;
            if (rep_iv < 0 || iv->start < intervals[rep_iv].start) rep_iv = ivi;
          }
          if (rep_iv < 0) continue;
          /* Density gate: the rep covers the CONTIGUOUS union [lo,hi], occupying
           * the register across any gaps between members.  If the union is much
           * larger than the members' combined live length, those gaps are holes
           * the merge fills — raising register pressure and risking spills (a
           * size regression).  Members of one value have OVERLAPPING (false)
           * ranges, so sum_len >= union_len for the cases worth merging; reject
           * when the union exceeds the members' total (a real hole). */
          if ((hi_e - lo_s + 1) > sum_len)
            continue;
          intervals[rep_iv].start = lo_s;
          intervals[rep_iv].end = hi_e;
          intervals[rep_iv].crosses_call = xcall ? 1 : 0;
          intervals[rep_iv].use_count = (uc > 65535) ? 65535 : (uint16_t)uc;
          intervals[rep_iv].narrow_uses = (nuc > 65535) ? 65535 : (uint16_t)nuc;
          /* Store the representative's VREG (stable across the scan's qsort),
           * not its array index. */
          int32_t rep_vreg = intervals[rep_iv].vreg;
          intervals[rep_iv].co_member = 1;
          for (int m = c; m >= 0; m = mnext[m]) {
            int ivi = iv_of[cand_vidx[m]];
            intervals[ivi].co_member = 1;
            if (ivi != rep_iv) intervals[ivi].coalesce_to = rep_vreg;
          }
          RA_DBG("coalesce: root class -> rep T%d [%u,%u] xcall=%d",
                 TCCIR_DECODE_VREG_POSITION(intervals[rep_iv].vreg), lo_s, hi_e, xcall);
        }
      }
      RA_DBG("coalesce: %d candidates, %d edges, %d unions (level=%d)", ncand, ne, merged, level);

      tcc_free(parent); tcc_free(rank); tcc_free(mnext); tcc_free(seen);
      #undef UF_FIND
    }
    if (pass == 1) { tcc_free(adj_start); tcc_free(adj); }
  }

  #undef FOR_LIVE_CAND
  #undef ADD_CAND
  #undef VIDX
  tcc_free(deg); tcc_free(live);
  tcc_free(iv_of); tcc_free(useb); tcc_free(defbk); tcc_free(livein);
  tcc_free(liveout); tcc_free(instr_block);
  tcc_free(cand_id); tcc_free(cand_vidx); tcc_free(edge_d); tcc_free(edge_s);
  tcc_ir_cfg_free(cfg);
}

/* ============================================================================
 * Dead Register-Copy Elimination (post-allocation)
 *
 * Phi resolution (ra_resolve_phis) emits an ASSIGN copy for every phi operand,
 * including phi destinations that are never read.  Such a copy is dead code —
 * but because its destination has no uses (an empty live range), the linear
 * scan freely gives it a register that simultaneously holds a DIFFERENT value
 * which is live across the merge (the two do not interfere).  The dead copy
 * then overwrites that register, destroying the live value.
 *   switch fuzz seed 198468 -O1: at a loop-exit merge the dead exit-phi copy
 *   `R6(T121) <- T130` clobbered R6, which still held the live u6 read one
 *   instruction later by `T106 <- R6(T104)`, so u6 read back as 0.
 *
 * These copies must NOT be suppressed before allocation: removing them changes
 * register pressure and perturbs every allocation decision, regressing
 * unrelated code by shifting the scan onto latent bugs.  They are NOP'd here,
 * after allocation AND after every liveness / scratch bitmap has been built, so
 * the allocation is byte-identical to before and only the emitted code changes.
 *
 * A copy is removable when its destination is an SSA TEMP — never
 * address-taken, so every read is an explicit operand — that appears as a read
 * operand nowhere.  Reads are enumerated exactly as ra_build_intervals counts
 * uses: src1, src2, the MLA accumulator, and the address of a STORE-class op.
 * Iterate to a fixed point so a chain of dead copies (each read only by the
 * next) collapses completely. */
static void ra_eliminate_dead_reg_copies(TCCIRState *ir)
{
  int max_pos = ir->next_local_variable;
  if (ir->next_temporary_variable > max_pos) max_pos = ir->next_temporary_variable;
  if (ir->next_parameter > max_pos) max_pos = ir->next_parameter;
  if (max_pos <= 0)
    return;
  size_t map_size = (size_t)4 * max_pos;
  uint8_t *read_map = tcc_malloc(map_size);
  int n = ir->next_instruction_index;

  #define RA_DEADCOPY_IDX(vr) \
      (TCCIR_DECODE_VREG_TYPE(vr) * max_pos + TCCIR_DECODE_VREG_POSITION(vr))
  #define RA_DEADCOPY_MARK(vr) do { \
      int32_t _v = (vr); \
      if (_v >= 0) { \
        long _i = RA_DEADCOPY_IDX(_v); \
        if (_i >= 0 && (size_t)_i < map_size) read_map[_i] = 1; \
      } \
    } while (0)

  for (;;) {
    memset(read_map, 0, map_size);

    for (int i = 0; i < n; i++) {
      IRQuadCompact *q = &ir->compact_instructions[i];
      if (q->op == TCCIR_OP_NOP)
        continue;
      if (irop_config[q->op].has_src1) {
        IROperand s = tcc_ir_op_get_src1(ir, q);
        if (irop_has_vreg(s)) RA_DEADCOPY_MARK(irop_get_vreg(s));
      }
      if (irop_config[q->op].has_src2) {
        IROperand s = tcc_ir_op_get_src2(ir, q);
        if (irop_has_vreg(s)) RA_DEADCOPY_MARK(irop_get_vreg(s));
      }
      if (q->op == TCCIR_OP_MLA) {
        IROperand s = tcc_ir_op_get_accum(ir, q);
        if (irop_has_vreg(s)) RA_DEADCOPY_MARK(irop_get_vreg(s));
      }
      /* STORE-class ops read their "dest" operand (the memory address). */
      if (q->op == TCCIR_OP_STORE || q->op == TCCIR_OP_STORE_INDEXED ||
          q->op == TCCIR_OP_STORE_POSTINC) {
        IROperand d = tcc_ir_op_get_dest(ir, q);
        if (irop_has_vreg(d)) RA_DEADCOPY_MARK(irop_get_vreg(d));
      }
    }

    int changed = 0;
    for (int i = 0; i < n; i++) {
      IRQuadCompact *q = &ir->compact_instructions[i];
      if (q->op != TCCIR_OP_ASSIGN)
        continue;
      IROperand d = tcc_ir_op_get_dest(ir, q);
      if (!irop_has_vreg(d))
        continue;
      int32_t dv = irop_get_vreg(d);
      if (TCCIR_DECODE_VREG_TYPE(dv) != TCCIR_VREG_TYPE_TEMP)
        continue;
      long idx = RA_DEADCOPY_IDX(dv);
      if (idx < 0 || (size_t)idx >= map_size || read_map[idx])
        continue;   /* destination is read somewhere — live copy, keep */
      /* Dead copy into a never-read temp: NOP it.  The slot (and any
       * is_jump_target flag on it) is preserved so branch targets still
       * resolve to a valid index; codegen skips NOPs. */
      q->op = TCCIR_OP_NOP;
      changed = 1;
    }

    if (!changed)
      break;
  }

  #undef RA_DEADCOPY_MARK
  #undef RA_DEADCOPY_IDX
  tcc_free(read_map);
}

/* ============================================================================
 * Multi-Def TEMP Promotion
 *
 * Promote multiply-block-defined TEMPs to fresh VARs so SSA construction places
 * phis for them.  The frontend emits a single TEMP written on BOTH arms of a
 * branch-lowered ternary (`cond ? a : b` where an arm has a side effect / call,
 * so it cannot lower to SELECT) — e.g. `T323 <- a` in one block and `T323 <- b`
 * in another, then a merge-block use.  That violates the SSA-by-construction
 * assumption the renamer makes for TEMPs (it renames only VARs and leaves such a
 * TEMP untouched), so the merge use resolves to ONE arm's definition
 * unconditionally — random-C O1/O2 wrong-code, seeds 100/118 (the value reached a
 * later inlined-csmix use as the else-arm value regardless of the condition).
 * Converting the TEMP to a VAR routes it through the normal var→SSA promotion,
 * which inserts the phi.  VAR and TEMP operands share the IROP_TAG_VREG encoding
 * and differ only in the type bits, so irop_set_vreg suffices; tcc_ir_vreg_alloc_var
 * grows the live-interval array.  Only fires for the rare multi-block-def TEMP. */
static void ra_promote_multidef_temps_to_vars(TCCIRState *ir, IRCFG *cfg)
{
  int n = ir->next_instruction_index;
  int ntmp = ir->next_temporary_variable;
  if (n <= 0 || ntmp <= 0 || !cfg || cfg->num_blocks <= 1)
    return;

  /* Skip functions that take label addresses (GCC labels-as-values, `&&label`):
   * their exact machine-code layout is observable at runtime via the label-offset
   * map, so the phi-resolution copies this promotion introduces would shift those
   * offsets (96_nodata_wanted measures code size with `&&label` arithmetic).
   * Such functions also have inlining disabled (tccgen gates auto-inline on
   * !func_has_label_addr), so they never hit the inlined-ternary miscompile this
   * promotion fixes — skipping them is free of correctness cost. */
  if (ir->func_has_label_addr)
    return;

  /* Only run when SSA construction will actually proceed and rename the new VARs
   * back into SSA temps.  SSA construction BAILS on un-enumerable control flow
   * (IJUMP / computed goto, SETJMP); if we promoted there, the converted VARs
   * would be left as unpromoted stack slots and change codegen for the worse
   * (96_nodata_wanted's `&&label` arithmetic).  Mirror ssa_has_unsupported_ops. */
  for (int i = 0; i < n; i++) {
    TccIrOp op = ir->compact_instructions[i].op;
    if (op == TCCIR_OP_IJUMP || op == TCCIR_OP_SETJMP || op == TCCIR_OP_NL_SETJMP)
      return;
  }

  /* def_block[t] = the block of t's first def, or -2 = multi-block, -1 = none. */
  int *def_block = tcc_malloc(sizeof(int) * ntmp);
  for (int t = 0; t < ntmp; t++) def_block[t] = -1;

  for (int i = 0; i < n; i++) {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP || !irop_config[q->op].has_dest)
      continue;
    if (q->op == TCCIR_OP_STORE || q->op == TCCIR_OP_STORE_INDEXED ||
        q->op == TCCIR_OP_STORE_POSTINC || q->op == TCCIR_OP_FUNCPARAMVAL ||
        q->op == TCCIR_OP_FUNCPARAMVOID)
      continue;
    IROperand d = tcc_ir_op_get_dest(ir, q);
    int32_t vr = irop_get_vreg(d);
    if (vr < 0 || TCCIR_DECODE_VREG_TYPE(vr) != TCCIR_VREG_TYPE_TEMP) continue;
    if (d.is_lval) continue; /* a deref store target, not a plain TEMP def */
    int t = TCCIR_DECODE_VREG_POSITION(vr);
    if (t < 0 || t >= ntmp) continue;
    int blk = cfg->instr_to_block[i];
    if (def_block[t] == -1) def_block[t] = blk;
    else if (def_block[t] != blk) def_block[t] = -2; /* multi-block */
  }

  /* A multi-block-defined TEMP only needs a phi (and only then is its renaming
   * actually wrong) when it has a USE in a block that does not itself define it —
   * a value flowing across a merge.  A TEMP whose uses are all in its own
   * def-blocks reaches each use from the local def and is already correct;
   * promoting it would insert needless phi-copies and grow code (96_nodata_wanted
   * measures code size via `&&label` arithmetic and is sensitive to this).  For
   * each multi-block TEMP, mark its def-blocks and require a use elsewhere. */
  int32_t *temp_to_var = tcc_malloc(sizeof(int32_t) * ntmp);
  for (int t = 0; t < ntmp; t++) temp_to_var[t] = -1;
  uint8_t *needs_phi = tcc_mallocz(ntmp);
  {
    uint8_t *isdef = tcc_mallocz(cfg->num_blocks);
    for (int t = 0; t < ntmp; t++) {
      if (def_block[t] != -2) continue;
      memset(isdef, 0, cfg->num_blocks);
      /* collect def-blocks of t */
      for (int i = 0; i < n; i++) {
        IRQuadCompact *q = &ir->compact_instructions[i];
        if (q->op == TCCIR_OP_NOP || !irop_config[q->op].has_dest) continue;
        if (q->op == TCCIR_OP_STORE || q->op == TCCIR_OP_STORE_INDEXED ||
            q->op == TCCIR_OP_STORE_POSTINC || q->op == TCCIR_OP_FUNCPARAMVAL ||
            q->op == TCCIR_OP_FUNCPARAMVOID) continue;
        IROperand d = tcc_ir_op_get_dest(ir, q);
        int32_t vr = irop_get_vreg(d);
        if (vr >= 0 && !d.is_lval && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_TEMP &&
            TCCIR_DECODE_VREG_POSITION(vr) == t)
          isdef[cfg->instr_to_block[i]] = 1;
      }
      /* a use in a non-def block ⇒ needs a phi */
      for (int i = 0; i < n && !needs_phi[t]; i++) {
        IRQuadCompact *q = &ir->compact_instructions[i];
        if (q->op == TCCIR_OP_NOP) continue;
        int blk = cfg->instr_to_block[i];
        if (isdef[blk]) continue;
        int32_t uses[5]; int nu = 0;
        if (irop_config[q->op].has_src1) uses[nu++] = irop_get_vreg(tcc_ir_op_get_src1(ir, q));
        if (irop_config[q->op].has_src2) uses[nu++] = irop_get_vreg(tcc_ir_op_get_src2(ir, q));
        if (q->op == TCCIR_OP_MLA) uses[nu++] = irop_get_vreg(tcc_ir_op_get_accum(ir, q));
        if (q->op == TCCIR_OP_STORE || q->op == TCCIR_OP_STORE_INDEXED || q->op == TCCIR_OP_STORE_POSTINC)
          uses[nu++] = irop_get_vreg(tcc_ir_op_get_dest(ir, q));
        for (int u = 0; u < nu; u++)
          if (uses[u] >= 0 && TCCIR_DECODE_VREG_TYPE(uses[u]) == TCCIR_VREG_TYPE_TEMP &&
              TCCIR_DECODE_VREG_POSITION(uses[u]) == t) { needs_phi[t] = 1; break; }
      }
    }
    tcc_free(isdef);
  }
  int any = 0;
  for (int t = 0; t < ntmp; t++) {
    if (needs_phi[t]) { temp_to_var[t] = tcc_ir_vreg_alloc_var(ir); any = 1; }
  }
  tcc_free(needs_phi);
  if (!any) { tcc_free(def_block); tcc_free(temp_to_var); return; }

  /* Rewrite every operand referencing a promoted TEMP to its VAR (type bits only;
   * is_local/is_lval/tag are preserved). */
  #define REMAP(getter, setter)                                                                                         \
    do {                                                                                                                \
      IROperand o = getter(ir, q);                                                                                      \
      int32_t ovr = irop_get_vreg(o);                                                                                   \
      if (ovr >= 0 && TCCIR_DECODE_VREG_TYPE(ovr) == TCCIR_VREG_TYPE_TEMP) {                                            \
        int op_t = TCCIR_DECODE_VREG_POSITION(ovr);                                                                     \
        if (op_t >= 0 && op_t < ntmp && temp_to_var[op_t] >= 0) {                                                       \
          irop_set_vreg(&o, temp_to_var[op_t]);                                                                         \
          setter(ir, q, o);                                                                                             \
        }                                                                                                               \
      }                                                                                                                 \
    } while (0)

  for (int i = 0; i < n; i++) {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP) continue;
    if (irop_config[q->op].has_dest) REMAP(tcc_ir_op_get_dest, tcc_ir_op_set_dest);
    if (irop_config[q->op].has_src1) REMAP(tcc_ir_op_get_src1, tcc_ir_op_set_src1);
    if (irop_config[q->op].has_src2) REMAP(tcc_ir_op_get_src2, tcc_ir_op_set_src2);
    if (q->op == TCCIR_OP_MLA) REMAP(tcc_ir_op_get_accum, tcc_ir_op_set_accum);
  }
  #undef REMAP

  tcc_free(def_block);
  tcc_free(temp_to_var);
}

/* ============================================================================
 * Post-Allocation Move Coalescing
 *
 * Eliminates register-to-register copies (ASSIGN dest = src) by making both
 * sides share the same physical register.  The source must die at the ASSIGN
 * instruction, the new register must be free for the destination's entire
 * live range, and call-crossing safety must be preserved.
 *
 * The code generator already elides identity moves (mov rX, rX), so a
 * successful coalescing eliminates the copy without modifying the IR.
 * ============================================================================ */

int tcc_ir_move_coalescing(TCCIRState *ir)
{
  LSLiveIntervalState *ls = &ir->ls;
  if (!ls->live_regs_by_instruction || ls->live_regs_by_instruction_size <= 0)
    return 0;

  int coalesced = 0;
  const int n = ir->next_instruction_index;
  const int tbl_size = ls->live_regs_by_instruction_size;

  /* Track vregs already reverse-coalesced to prevent chains where a src
   * gets moved to register A, then a later ASSIGN moves it back to B. */
  uint32_t *rev_done = NULL;
  int rev_done_size = 0;

  for (int i = 0; i < n; ++i)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    /* LOAD with a VREG source where the underlying interval ended up in a
     * register (no spill) is a register copy at codegen — treat like ASSIGN
     * for coalescing.  Catches inlined "temp = var" patterns the IR
     * generator emits as LOAD even when no memory access is involved.
     * Accept is_lval=1 for local VAR reads as long as the VAR is reg-only;
     * skip is_llocal (true memory load via pointer) and is_sym (global). */
    /* LOAD from a vreg whose interval ended up in a register (no spill)
     * is a register copy at codegen — treat like ASSIGN for coalescing.
     * Catches inlined "temp = var" patterns where the IR generator emits
     * LOAD with VREG/STACKOFF source even though no memory access happens.
     * Skip is_llocal (double-indirection via pointer) and is_sym (global).
     * Skip sub-word btypes: those LOADs emit UXTB/SXTB/UXTH/SXTH alongside
     * the mov to truncate; coalescing them away would skip the narrowing
     * and yield wrong values (see pr69447, fp-cmp-8 sub-word args). */
    int is_copy_load = 0;
    if (q->op == TCCIR_OP_LOAD) {
      IROperand ts = tcc_ir_op_get_src1(ir, q);
      int valid_tag = (ts.tag == IROP_TAG_VREG ||
                       (ts.tag == IROP_TAG_STACKOFF && ts.is_local));
      int dest_bt = irop_get_btype(tcc_ir_op_get_dest(ir, q));
      int src_bt = irop_get_btype(ts);
      int width_safe = (dest_bt == src_bt) &&
                       (dest_bt == IROP_BTYPE_INT32 ||
                        dest_bt == IROP_BTYPE_INT64 ||
                        dest_bt == IROP_BTYPE_FUNC);
      if (width_safe && valid_tag && !ts.is_llocal && !ts.is_sym) {
        int32_t tsv = irop_get_vreg(ts);
        if (tsv >= 0 && tcc_ir_vreg_is_valid(ir, tsv)) {
          for (int j = 0; j < ls->next_interval_index; ++j) {
            if (ls->intervals[j].vreg == (uint32_t)tsv) {
              /* Only a source in a REAL register is a register copy.  PREG_NONE
               * (0x1F) is >= 0 but means "not allocated" — e.g. a stack-passed
               * parameter that lives in the caller's frame, not a register.
               * Coalescing the LOAD result into such a source would make it
               * inherit PREG_NONE and be mis-lowered as a spill at frame offset
               * 0 (clobbering the saved frame pointer). */
              if (ls->intervals[j].r0 >= 0 && ls->intervals[j].r0 < PREG_NONE &&
                  ls->intervals[j].stack_location == 0)
                is_copy_load = 1;
              break;
            }
          }
        }
      }
    }
    if (q->op != TCCIR_OP_ASSIGN && !is_copy_load)
      continue;

    const IROperand src1 = tcc_ir_op_get_src1(ir, q);
    const IROperand dest = tcc_ir_op_get_dest(ir, q);
    /* For is_copy_load (LOAD from in-register VAR) the src has is_lval=1
     * but it behaves as a register copy — don't skip it on that basis. */
    if ((src1.is_lval && !is_copy_load) || dest.is_lval) continue;
    int32_t sv = irop_get_vreg(src1);
    if (sv < 0 || !tcc_ir_vreg_is_valid(ir, sv))
      continue;
    int32_t dv = irop_get_vreg(dest);
    if (dv < 0 || !tcc_ir_vreg_is_valid(ir, dv))
      continue;

    LSLiveInterval *src_iv = NULL, *dst_iv = NULL;
    for (int j = 0; j < ls->next_interval_index; ++j)
    {
      if (ls->intervals[j].vreg == (uint32_t)sv) src_iv = &ls->intervals[j];
      if (ls->intervals[j].vreg == (uint32_t)dv) dst_iv = &ls->intervals[j];
      if (src_iv && dst_iv) break;
    }
    if (!src_iv || !dst_iv) continue;
    /* Both endpoints must live in a REAL register.  PREG_NONE (0x1F) and
     * PREG_SPILLED (0x20) are >= 0 but are NOT registers (e.g. a stack-passed
     * parameter resident in the caller's frame).  Coalescing onto such an
     * endpoint propagates PREG_NONE into a live value, which is then mis-lowered
     * as a spill at frame offset 0 — clobbering a saved register at [FP,#0]. */
    if (src_iv->r0 < 0 || src_iv->r0 >= PREG_NONE ||
        dst_iv->r0 < 0 || dst_iv->r0 >= PREG_NONE) continue;
    if (src_iv->stack_location != 0 || dst_iv->stack_location != 0) continue;
    if (src_iv->r0 == dst_iv->r0) continue;
    /* Never reassign a graph-coalesced interval: it shares one register with
     * its whole class, and reassigning one member here would split the class
     * (the other members keep the class register), corrupting the value. */
    if (src_iv->co_member || dst_iv->co_member) continue;

    /* Forward direction: reassign dest to use src's register.
     * Requires src to die at this ASSIGN. */
    if (src_iv->end == (uint32_t)i) {
      int src_reg = src_iv->r0;
      if (dst_iv->crosses_call && !(src_reg >= 4 && src_reg <= 11))
        goto try_reverse;

      int conflict = 0;
      for (int k = i + 1; k <= (int)dst_iv->end && k < tbl_size; ++k)
      {
        if (ls->live_regs_by_instruction[k] & (1u << src_reg)) {
          /* Two-address relaxation: at k == dst.end the dst's last use is
           * an instruction that consumes dst and writes a fresh result.
           * If that result lands in src_reg (some interval starts at k in
           * src_reg) AND the same instruction reads dst, sharing src_reg
           * is safe — ARM ops read all sources before writing dest, so
           * `OP src_reg, ..., src_reg` is a valid two-operand form. */
          int safe = 0;
          if (k == (int)dst_iv->end) {
            IRQuadCompact *kq = &ir->compact_instructions[k];
            int reads_dst = 0;
            if (irop_config[kq->op].has_src1 &&
                irop_get_vreg(tcc_ir_op_get_src1(ir, kq)) == dv)
              reads_dst = 1;
            if (!reads_dst && irop_config[kq->op].has_src2 &&
                irop_get_vreg(tcc_ir_op_get_src2(ir, kq)) == dv)
              reads_dst = 1;
            if (reads_dst) {
              for (int j2 = 0; j2 < ls->next_interval_index; j2++) {
                LSLiveInterval *xi = &ls->intervals[j2];
                if (xi->r0 == src_reg && xi->stack_location == 0 &&
                    xi->start == (uint32_t)k) {
                  safe = 1;
                  break;
                }
              }
            }
          }
          if (!safe) { conflict = 1; break; }
        }
      }
      if (!conflict) {
        for (int k = (int)dst_iv->start; k < i && k < tbl_size; ++k)
        {
          if (ls->live_regs_by_instruction[k] & (1u << src_reg))
          { conflict = 1; break; }
        }
      }
      if (!conflict) {
        int old_reg = dst_iv->r0;
        dst_iv->r0 = src_reg;
        for (int k = (int)dst_iv->start; k <= (int)dst_iv->end && k < tbl_size; ++k)
        {
          /* old_reg's bit may be shared with another interval that coalesced
           * onto it earlier (in-place two-address ops overlap on purpose) —
           * only clear positions where no other claimant is still live. */
          if (!tcc_ls_reg_held_by_other(ls, old_reg, k, dst_iv))
            ls->live_regs_by_instruction[k] &= ~(1u << old_reg);
          ls->live_regs_by_instruction[k] |= (1u << src_reg);
        }
        RA_DBG("move_coalesce fwd @%d: T%d R%d->R%d [%u,%u]", i,
               (int)(dv & 0xffffff), old_reg, src_reg, dst_iv->start, dst_iv->end);
        coalesced++;
        continue;
      }
    }

    /* Reverse direction: reassign src to use dest's register.
     * Works for loop-carried phi copies where src = f(dest, ...) and
     * dest's register is only occupied by dest during src's range.
     * Safety: src must not be redefined between the ASSIGN and dest's
     * last use, otherwise the shared register would get clobbered. */
try_reverse:;
    /* Skip if this src vreg was already reverse-coalesced */
    {
      int already = 0;
      for (int ri = 0; ri < rev_done_size; ri++) {
        if (rev_done[ri] == (uint32_t)sv) { already = 1; break; }
      }
      if (already) continue;
    }
    int dest_reg = dst_iv->r0;
    if (src_iv->crosses_call && !(dest_reg >= 4 && dest_reg <= 11))
      continue;

    int conflict = 0;

    /* Conservative: src must be defined directly FROM dest (reads dest
     * as src1), like `src = dest + 1` or `src = dest * x + acc`.
     * This guarantees ARM's read-before-write makes the in-place
     * operation correct. */
    {
      int def_idx = (int)src_iv->start;
      if (def_idx < 0 || def_idx >= n) { conflict = 1; goto rev_check_done; }
      IRQuadCompact *qdef = &ir->compact_instructions[def_idx];
      if (!irop_config[qdef->op].has_src1) { conflict = 1; goto rev_check_done; }
      IROperand s1 = tcc_ir_op_get_src1(ir, qdef);
      if (irop_get_vreg(s1) != dv) { conflict = 1; goto rev_check_done; }
    }

    /* Check src is not redefined while dest is still live */
    for (int k = i + 1; k <= (int)dst_iv->end && k < n; ++k)
    {
      IRQuadCompact *qk = &ir->compact_instructions[k];
      if (qk->op == TCCIR_OP_NOP) continue;
      if (irop_config[qk->op].has_dest) {
        IROperand dk = tcc_ir_op_get_dest(ir, qk);
        int is_mem_store = (qk->op == TCCIR_OP_STORE || qk->op == TCCIR_OP_STORE_INDEXED ||
                            qk->op == TCCIR_OP_STORE_POSTINC) && dk.is_lval;
        if (!is_mem_store) {
          int32_t dkvr = irop_get_vreg(dk);
          if (dkvr == sv) { conflict = 1; break; }
        }
      }
    }
    if (conflict) goto rev_check_done;

    /* Symmetric guard (dest side): after this copy src and dest share
     * dest_reg holding the same value.  If dest is given a NEW, independent
     * value while src is still live, that write clobbers dest_reg and src's
     * remaining uses read the wrong value.  The loop-carried phi copy this
     * pass targets has src dying at the copy (src_iv->end == i), so the range
     * below is empty and legitimate coalescing is unaffected; the guard only
     * fires when src OUTLIVES the copy and dest is re-defined underneath it
     * (bitfield 40979: `u4 = u3` copy, then `u4 = const` clobbers the shared
     * register while `u3` is still read).  A redefinition at exactly src's
     * last use that also reads src is the two-address read-before-write case
     * and stays safe. */
    for (int k = i + 1; k <= (int)src_iv->end && k < n; ++k)
    {
      IRQuadCompact *qk = &ir->compact_instructions[k];
      if (qk->op == TCCIR_OP_NOP) continue;
      if (!irop_config[qk->op].has_dest) continue;
      IROperand dk = tcc_ir_op_get_dest(ir, qk);
      int is_mem_store = (qk->op == TCCIR_OP_STORE || qk->op == TCCIR_OP_STORE_INDEXED ||
                          qk->op == TCCIR_OP_STORE_POSTINC) && dk.is_lval;
      if (is_mem_store) continue;
      if (irop_get_vreg(dk) != dv) continue;
      if (k == (int)src_iv->end) {
        int reads_src = 0;
        if (irop_config[qk->op].has_src1 &&
            irop_get_vreg(tcc_ir_op_get_src1(ir, qk)) == sv) reads_src = 1;
        if (!reads_src && irop_config[qk->op].has_src2 &&
            irop_get_vreg(tcc_ir_op_get_src2(ir, qk)) == sv) reads_src = 1;
        if (!reads_src && qk->op == TCCIR_OP_MLA &&
            irop_get_vreg(tcc_ir_op_get_accum(ir, qk)) == sv) reads_src = 1;
        if (reads_src) continue;
      }
      conflict = 1;
      break;
    }
    if (conflict) goto rev_check_done;

    /* Check dest not used between src's def and the ASSIGN.
     * src's def overwrites dest_reg; any intervening use of dest
     * would read the wrong value. */
    for (int k = (int)src_iv->start + 1; k < i && k < n; ++k)
    {
      IRQuadCompact *qk = &ir->compact_instructions[k];
      if (qk->op == TCCIR_OP_NOP) continue;
      if (irop_config[qk->op].has_src1) {
        if (irop_get_vreg(tcc_ir_op_get_src1(ir, qk)) == dv) { conflict = 1; break; }
      }
      if (!conflict && irop_config[qk->op].has_src2) {
        if (irop_get_vreg(tcc_ir_op_get_src2(ir, qk)) == dv) { conflict = 1; break; }
      }
      if (!conflict && irop_config[qk->op].has_dest) {
        IROperand dk = tcc_ir_op_get_dest(ir, qk);
        if (dk.is_lval && irop_get_vreg(dk) == dv) { conflict = 1; break; }
      }
      if (!conflict && qk->op == TCCIR_OP_MLA) {
        if (irop_get_vreg(tcc_ir_op_get_accum(ir, qk)) == dv) { conflict = 1; break; }
      }
    }
    if (conflict) goto rev_check_done;

    /* Check no control-flow escape between src's def and the ASSIGN.
     * src's def overwrites dest_reg; the ASSIGN re-establishes dest's value
     * only on the path that reaches it.  A JUMP/JUMPIF in (def, ASSIGN) that
     * targets outside [def, ASSIGN] lets control reach later uses of dest
     * with dest_reg clobbered and the restoring copy skipped — e.g. a
     * top-tested pointer-chase loop (`while (p->next) p = p->next;`) whose
     * exit edge branches past the back-edge copy while `p` is still live. */
    for (int k = (int)src_iv->start; k < i && k < n; ++k)
    {
      IRQuadCompact *qk = &ir->compact_instructions[k];
      if (qk->op == TCCIR_OP_NOP) continue;
      if (qk->op == TCCIR_OP_IJUMP || qk->op == TCCIR_OP_SWITCH_TABLE ||
          qk->op == TCCIR_OP_SWITCH_LOAD) { conflict = 1; break; }
      if (qk->op == TCCIR_OP_JUMP || qk->op == TCCIR_OP_JUMPIF) {
        int jt = (int)irop_get_imm64_ex(ir, tcc_ir_op_get_dest(ir, qk));
        if (jt < (int)src_iv->start || jt > i) { conflict = 1; break; }
      }
    }
rev_check_done:
    if (conflict) continue;

    /* Check dest_reg not occupied by other intervals during src's range.
     * Identity-based: earlier coalesces may have moved a third interval onto
     * dest_reg inside dst_iv's range, so "position within dst_iv's range" is
     * not proof the claim is dst_iv's own. */
    for (int k = (int)src_iv->start; k <= (int)src_iv->end && k < tbl_size; ++k)
    {
      if (ls->live_regs_by_instruction[k] & (1u << dest_reg))
      {
        if (tcc_ls_reg_held_by_other(ls, dest_reg, k, dst_iv))
        { conflict = 1; break; }
        /* dest_reg is live here — only OK if it's from dest_iv itself */
        if (k < (int)dst_iv->start || k > (int)dst_iv->end)
        { conflict = 1; break; }
      }
    }
    if (conflict) continue;

    int old_reg = src_iv->r0;
    src_iv->r0 = dest_reg;
    for (int k = (int)src_iv->start; k <= (int)src_iv->end && k < tbl_size; ++k)
    {
      /* old_reg's bit may be shared with another interval that coalesced
       * onto it earlier — only clear positions with no other live claimant
       * (volatile 36818: T175 leaving R5 wiped T212's in-place-XOR claim,
       * and the phase-3 scratch fixup then put the outer loop counter there). */
      if (!tcc_ls_reg_held_by_other(ls, old_reg, k, src_iv))
        ls->live_regs_by_instruction[k] &= ~(1u << old_reg);
      ls->live_regs_by_instruction[k] |= (1u << dest_reg);
    }
    RA_DBG("move_coalesce rev @%d: T%d R%d->R%d [%u,%u]", i,
           (int)(sv & 0xffffff), old_reg, dest_reg, src_iv->start, src_iv->end);
    /* Record this src vreg as reverse-coalesced */
    rev_done = tcc_realloc(rev_done, sizeof(uint32_t) * (rev_done_size + 1));
    rev_done[rev_done_size++] = (uint32_t)sv;
    coalesced++;
  }

  if (rev_done)
    tcc_free(rev_done);

  if (coalesced > 0)
    tcc_ls_recompute_dirty_registers(ls);

  return coalesced;
}
