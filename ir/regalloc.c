/*
 *  TCC IR - SSA-Aware Register Allocator
 *
 *  Operates directly on SSA-renamed IR with phi nodes.
 *  Replaces the tccls.c linear scan when -fssa-regalloc is enabled.
 *
 *  Copyright (c) 2025 Mateusz Stadnik
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
#include "opt/ssa_opt.h"
#include "opt/ssa/branch.h"
#include "const_string_fold.h"
#include "bitop_const_fold.h"
#include "opt/ssa/branch.h"
#include "opt/ssa/fold.h"
#include "opt/ssa/var_imm_prop.h"
#include "opt/ssa/cprop.h"
#include "global_addr_hoist.h"
#include "load_cse.h"
#include "diamond_store_fwd.h"

#include "opt/ssa/strength.h"
#include "opt/ssa/reassoc.h"
#include "opt/ssa/gvn.h"
#include "opt_pipeline.h"
#include "licm.h"

#include "memory/bitspan.h"
#include "memory/bit_matrix.h"

extern int tcc_ir_opt_pass_disabled(const char *name);

#define RA_DBG(fmt, ...) LOG_LS(fmt, ##__VA_ARGS__)

/* ============================================================================
 * SSA Live Interval
 * ============================================================================ */

typedef struct SSAInterval {
  int32_t vreg;
  uint32_t start;
  uint32_t end;
  int8_t r0;
  int8_t r1;
  int32_t stack_location;
  uint8_t crosses_call : 1;
  uint8_t addrtaken : 1;
  uint8_t is_param : 1;
  uint8_t reg_shared : 1; /* cur shares hr with another active interval (return-block tail); skip expire-free and active push */
  uint8_t loop_phi_locked : 1; /* absorbed a loop-phi partner (carries a loop-carried value across the whole loop body); must not be evicted — spilling it mid-loop would not reload the partner's uses and corrupts the IV */
  uint8_t reg_type;
  uint16_t use_count;
  uint16_t narrow_uses; /* static count of references from ops with 16-bit encodings (want r0-r7) */
  int8_t precolored;
  int8_t pref_reg; /* soft hint: prefer this physical reg if available (e.g. r0 for RETURNVALUE feeders) */
  int32_t hint_vreg;
  int32_t coalesce_to; /* graph coalescing: vreg of the representative this one merged into (-1 = rep / not merged) */
  uint8_t co_member;   /* 1 if part of a graph-coalesced class (rep or member) — the in-scan transfer must leave it alone */
} SSAInterval;

/* ============================================================================
 * Call-site prefix sum (reused from ir/live.c pattern)
 * ============================================================================ */

static int ir_op_is_implicit_call_ra(TccIrOp op)
{
  const FloatingPointConfig *fpu = architecture_config.fpu;
  if (!fpu)
    return 0;
  switch (op) {
  case TCCIR_OP_FADD: return !(fpu->has_fadd && fpu->has_dadd);
  case TCCIR_OP_FSUB: return !(fpu->has_fsub && fpu->has_dsub);
  case TCCIR_OP_FMUL: return !(fpu->has_fmul && fpu->has_dmul);
  case TCCIR_OP_FDIV: return !(fpu->has_fdiv && fpu->has_ddiv);
  case TCCIR_OP_FNEG: return !(fpu->has_fneg && fpu->has_dneg);
  case TCCIR_OP_FCMP: return !(fpu->has_fcmp && fpu->has_dcmp);
  case TCCIR_OP_CVT_FTOF: return !(fpu->has_ftof && fpu->has_dtof);
  case TCCIR_OP_CVT_ITOF: return !(fpu->has_itof && fpu->has_itod);
  case TCCIR_OP_CVT_FTOI: return !(fpu->has_ftoi && fpu->has_dtoi);
  default: return 0;
  }
}

static int *ra_build_call_prefix(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  if (n <= 0)
    return NULL;
  int *prefix = tcc_malloc(sizeof(int) * (n + 1));
  prefix[0] = 0;
  for (int i = 0; i < n; i++) {
    IRQuadCompact *q = &ir->compact_instructions[i];
    TccIrOp op = q->op;
    int is_call = (op == TCCIR_OP_FUNCCALLVOID || op == TCCIR_OP_FUNCCALLVAL ||
                   op == TCCIR_OP_BUILTIN_APPLY || ir_op_is_implicit_call_ra(op));
    /* A large BLOCK_COPY lowers to a memcpy() call in the backend, clobbering
     * the caller-saved registers.  The inline (small) lowering saves/restores
     * everything it touches, so only the memcpy-sized copies count as calls. */
    if (!is_call && op == TCCIR_OP_BLOCK_COPY) {
      int bc_size = (int)irop_get_imm64_ex(ir, tcc_ir_op_get_src2(ir, q));
      if (bc_size >= TCCIR_BLOCK_COPY_MEMCPY_MIN_BYTES)
        is_call = 1;
    }
    prefix[i + 1] = prefix[i] + is_call;
  }
  return prefix;
}

static int ra_has_call_in_range(const int *prefix, int start, int end, int n)
{
  if (!prefix || n <= 0)
    return 0;
  if (start < -1) start = -1;
  if (end > n) end = n;
  if (end <= start + 1) return 0;
  if (start + 1 >= n) return 0;
  return (prefix[end] - prefix[start + 1]) != 0;
}

/* Prefix sum of SWITCH_TABLE / SWITCH_LOAD dispatches.  The Thumb lowering of
 * both ops (tcc_gen_machine_switch_table_mop / _switch_load_mop in
 * arm-thumb-gen.c) uses R_IP (R12) as a fixed scratch for the jump-table base
 * and clobbers it.  R12 is caller-saved, so a value that is merely live across
 * the dispatch is not otherwise forced off it — see ra_has_switch_in_range. */
static int *ra_build_switch_prefix(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  if (n <= 0)
    return NULL;
  int *prefix = tcc_malloc(sizeof(int) * (n + 1));
  prefix[0] = 0;
  for (int i = 0; i < n; i++) {
    TccIrOp op = ir->compact_instructions[i].op;
    int is_switch = (op == TCCIR_OP_SWITCH_TABLE || op == TCCIR_OP_SWITCH_LOAD);
    prefix[i + 1] = prefix[i] + is_switch;
  }
  return prefix;
}

/* True if a SWITCH_TABLE/SWITCH_LOAD dispatch sits at any position k with
 * start < k <= end, i.e. the interval [start,end] is live across the dispatch.
 * `end` is inclusive (unlike ra_has_call_in_range): a value whose only use is
 * a *backward* switch target has its last use laid out before the dispatch in
 * IR order, with its interval extended forward by the back-edge pass to exactly
 * the dispatch position — so end == k must still count.  Such a value would be
 * read at a switch target *after* the R12 clobber, so it must avoid R12. */
static int ra_has_switch_in_range(const int *prefix, int start, int end, int n)
{
  if (!prefix || n <= 0)
    return 0;
  if (start < -1) start = -1;
  if (end > n - 1) end = n - 1;
  if (end < start + 1) return 0;
  return (prefix[end + 1] - prefix[start + 1]) != 0;
}

static const char *ra_vreg_type_char(int type)
{
  switch (type) {
  case TCCIR_VREG_TYPE_VAR: return "V";
  case TCCIR_VREG_TYPE_TEMP: return "T";
  case TCCIR_VREG_TYPE_PARAM: return "P";
  default: return "?";
  }
}

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
 * SSA Live Interval Building
 * ============================================================================ */

static void ra_build_intervals(TCCIRState *ir, IRCFG *cfg, IRSSAState *ssa,
                               SSAInterval **out_intervals, int *out_count,
                               const int *call_prefix, int *out_max_vreg_pos)
{
  int n = ir->next_instruction_index;
  int nb = cfg->num_blocks;
  int local_count = ir->next_local_variable;
  int temp_count = ir->next_temporary_variable;
  int param_count = ir->next_parameter;
  int max_vreg_pos = local_count;
  if (temp_count > max_vreg_pos) max_vreg_pos = temp_count;
  if (param_count > max_vreg_pos) max_vreg_pos = param_count;

  /* SWITCH_TABLE/SWITCH_LOAD dispatch clobbers R_IP (R12); see
   * ra_has_switch_in_range below. */
  int *switch_prefix = ra_build_switch_prefix(ir);

  /* Allocate per-vreg start/end tracking indexed by encoded vreg.
   * Use flat arrays indexed by (type * max_pos + position). */
  int table_size = 4 * max_vreg_pos;
  if (table_size <= 0) table_size = 1;
  uint32_t *starts = tcc_malloc(sizeof(uint32_t) * table_size);
  uint32_t *ends = tcc_malloc(sizeof(uint32_t) * table_size);
  uint16_t *uses = tcc_mallocz(sizeof(uint16_t) * table_size);
  for (int i = 0; i < table_size; i++) {
    starts[i] = INTERVAL_NOT_STARTED;
    ends[i] = 0;
  }

  #define VREG_IDX(vr) ((TCCIR_DECODE_VREG_TYPE(vr) * max_vreg_pos) + TCCIR_DECODE_VREG_POSITION(vr))

  /* Build per-instruction loop depth map for spill-cost weighting.
   * Uses at deeper loop nesting get exponentially higher weight so the
   * allocator prefers spilling values that live in shallow code. */
  uint8_t *instr_depth = tcc_mallocz(n);
  if (tcc_state->optimize > 0) {
    IRLoops *loops = tcc_ir_detect_loops(ir);
    if (loops) {
      for (int li = 0; li < loops->num_loops; li++) {
        IRLoop *lp = &loops->loops[li];
        for (int bi = 0; bi < lp->num_body_instrs; bi++) {
          int idx = lp->body_instrs[bi];
          if (idx >= 0 && idx < n && lp->depth > instr_depth[idx])
            instr_depth[idx] = (uint8_t)lp->depth;
        }
      }
      tcc_ir_free_loops(loops);
    }
  }

  /* Pre-pass: identify vregs that appear as a source operand in any non-NOP
   * instruction.  Used below to decide whether a STORE-class op's dest is
   * really a register def (promoted scalar with at least one read elsewhere)
   * or just an address being written through (no other reads — the vreg
   * represents an implicit stack address that the codegen materializes via
   * its origin, not via an IR-level def). */
  uint8_t *vreg_read_as_src = tcc_mallocz((table_size + 7) / 8);
  for (int i = 0; i < n; i++) {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP) continue;
    IROperand srcs[3];
    int nsrcs = 0;
    if (irop_config[q->op].has_src1) srcs[nsrcs++] = tcc_ir_op_get_src1(ir, q);
    if (irop_config[q->op].has_src2) srcs[nsrcs++] = tcc_ir_op_get_src2(ir, q);
    if (q->op == TCCIR_OP_MLA)       srcs[nsrcs++] = tcc_ir_op_get_accum(ir, q);
    for (int k = 0; k < nsrcs; k++) {
      int32_t svr = irop_get_vreg(srcs[k]);
      if (svr < 0 || !tcc_ir_vreg_is_valid(ir, svr)) continue;
      int sidx = VREG_IDX(svr);
      if (sidx < table_size)
        vreg_read_as_src[sidx >> 3] |= (uint8_t)(1u << (sidx & 7));
    }
  }

  /* Scan instructions for def/use */
  for (int i = 0; i < n; i++) {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP) continue;

    /* Weight = 4^depth: depth 0 → 1, depth 1 → 4, depth 2 → 16, depth 3 → 64 */
    uint16_t w = 1;
    if (instr_depth[i] > 0) {
      w = 1 << (2 * (instr_depth[i] < 7 ? instr_depth[i] : 7));
    }

    /* Uses: src1, src2 */
    if (irop_config[q->op].has_src1) {
      IROperand s1 = tcc_ir_op_get_src1(ir, q);
      int32_t vr = irop_get_vreg(s1);
      if (vr >= 0 && tcc_ir_vreg_is_valid(ir, vr)) {
        int idx = VREG_IDX(vr);
        if (idx < table_size) {
          if (starts[idx] == INTERVAL_NOT_STARTED) starts[idx] = 0;
          if (ends[idx] < (uint32_t)i) ends[idx] = i;
          if (uses[idx] <= 65535 - w) uses[idx] += w; else uses[idx] = 65535;
        }
      }
    }
    if (irop_config[q->op].has_src2) {
      IROperand s2 = tcc_ir_op_get_src2(ir, q);
      int32_t vr = irop_get_vreg(s2);
      if (vr >= 0 && tcc_ir_vreg_is_valid(ir, vr)) {
        int idx = VREG_IDX(vr);
        if (idx < table_size) {
          if (starts[idx] == INTERVAL_NOT_STARTED) starts[idx] = 0;
          if (ends[idx] < (uint32_t)i) ends[idx] = i;
          if (uses[idx] <= 65535 - w) uses[idx] += w; else uses[idx] = 65535;
        }
      }
    }
    /* MLA accumulator (4th operand) */
    if (q->op == TCCIR_OP_MLA) {
      IROperand acc = tcc_ir_op_get_accum(ir, q);
      int32_t vr = irop_get_vreg(acc);
      if (vr >= 0 && tcc_ir_vreg_is_valid(ir, vr)) {
        int idx = VREG_IDX(vr);
        if (idx < table_size) {
          if (starts[idx] == INTERVAL_NOT_STARTED) starts[idx] = 0;
          if (ends[idx] < (uint32_t)i) ends[idx] = i;
          if (uses[idx] <= 65535 - w) uses[idx] += w; else uses[idx] = 65535;
        }
      }
    }

    /* Def: dest (non-STORE) */
    if (irop_config[q->op].has_dest) {
      IROperand d = tcc_ir_op_get_dest(ir, q);
      int is_store_op = (q->op == TCCIR_OP_STORE || q->op == TCCIR_OP_STORE_INDEXED ||
                         q->op == TCCIR_OP_STORE_POSTINC);
      int32_t vr = irop_get_vreg(d);
      /* STORE-class ops nominally treat dest as an address being written
       * through, so the dest vreg must be live before this instruction
       * (start=0).  But when the optimizer register-promotes a scalar local,
       * the STORE becomes a register def and dest's lifetime starts here.
       * Promote STORE→DEF only when ALL of:
       *   (a) dest is a VAR (TEMPs frequently hold computed pointer values
       *       — a TEMP-dest STORE is a genuine memory write through the
       *       TEMP's address even when the TEMP itself isn't address-taken),
       *   (b) the vreg is not address-taken (no aliasing through &v),
       *   (c) the vreg is not lvalue-typed (not an address-of value like &a),
       *   (d) the vreg is read as a source somewhere — an unread STORE-only
       *       VAR may be an implicit stack address whose defining ASSIGN
       *       was optimized away, where the "live from entry" property is
       *       load-bearing for the codegen to materialize the address.
       * Together these identify register-promoted scalars whose STOREs are
       * really ASSIGNs.  Without this, every such vreg is marked live from
       * entry and forced into callee-saved registers by crosses_call. */
      int dest_is_use = is_store_op;
      if (is_store_op && vr >= 0 && tcc_ir_vreg_is_valid(ir, vr) &&
          TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_VAR) {
        IRLiveInterval *li = tcc_ir_vreg_live_interval(ir, vr);
        int didx = VREG_IDX(vr);
        int read_as_src = didx < table_size &&
                          ((vreg_read_as_src[didx >> 3] >> (didx & 7)) & 1);
        if (li && !li->addrtaken && !li->is_lvalue && read_as_src)
          dest_is_use = 0;
      }
      if (vr >= 0 && tcc_ir_vreg_is_valid(ir, vr)) {
        int idx = VREG_IDX(vr);
        if (idx < table_size) {
          if (starts[idx] == INTERVAL_NOT_STARTED)
            starts[idx] = dest_is_use ? 0 : i;
          if (ends[idx] < (uint32_t)i) ends[idx] = i;
          if (dest_is_use) {
            if (uses[idx] <= 65535 - w) uses[idx] += w; else uses[idx] = 65535;
          }
        }
      }
    }
  }

  /* Seed intervals for addrtaken vregs not referenced in any IR instruction.
   * Parameters captured by nested functions may have no uses in the parent's
   * IR, but must still get stack slots so the child can access them via the
   * static chain pointer. */
  for (int type = TCCIR_VREG_TYPE_VAR; type <= TCCIR_VREG_TYPE_PARAM; type++) {
    int limit = (type == TCCIR_VREG_TYPE_VAR) ? local_count :
                (type == TCCIR_VREG_TYPE_TEMP) ? temp_count : param_count;
    for (int pos = 0; pos < limit; pos++) {
      int idx = type * max_vreg_pos + pos;
      if (idx >= table_size) continue;
      if (starts[idx] != INTERVAL_NOT_STARTED) continue;
      int32_t vreg = TCCIR_ENCODE_VREG(type, pos);
      IRLiveInterval *li = tcc_ir_vreg_live_interval(ir, vreg);
      if (li && li->addrtaken) {
        starts[idx] = 0;
        ends[idx] = 0;
      }
    }
  }

  if (TCC_LOG_LS) {
    RA_DBG("SSA ra_build_intervals: after def/use scan (%d instructions)", n);
    for (int idx = 0; idx < table_size; idx++) {
      if (starts[idx] == INTERVAL_NOT_STARTED) continue;
      int type = idx / max_vreg_pos;
      int pos = idx % max_vreg_pos;
      RA_DBG("  %s%d range=[%u,%u] uses=%u", ra_vreg_type_char(type), pos,
             starts[idx], ends[idx], uses[idx]);
    }
  }

  /* Process phi nodes: extend operand intervals to pred block ends,
   * set phi dest starts to block start */
  for (int b = 0; b < nb; b++) {
    for (IRPhiNode *phi = ssa->block_phis[b]; phi; phi = phi->next) {
      int32_t dest_vr = phi->dest_vreg;
      if (dest_vr >= 0 && tcc_ir_vreg_is_valid(ir, dest_vr)) {
        int idx = VREG_IDX(dest_vr);
        if (idx < table_size) {
          uint32_t bstart = cfg->blocks[b].start_idx;
          if (starts[idx] == INTERVAL_NOT_STARTED || starts[idx] > bstart)
            starts[idx] = bstart;
        }
      }
      for (int pi = 0; pi < phi->num_operands; pi++) {
        int32_t op_vr = phi->operands[pi].vreg;
        int pred = phi->operands[pi].pred_block;
        if (op_vr < 0 || pred < 0 || pred >= nb) continue;
        if (!tcc_ir_vreg_is_valid(ir, op_vr)) continue;
        int idx = VREG_IDX(op_vr);
        if (idx < table_size) {
          uint32_t pred_end = cfg->blocks[pred].end_idx;
          if (pred_end > 0) pred_end--;
          if ((int)pred_end >= 0 && (int)pred_end < n) {
            IRQuadCompact *term = &ir->compact_instructions[pred_end];
            if (term->op == TCCIR_OP_JUMP || term->op == TCCIR_OP_JUMPIF) {
              int target = (int)tcc_ir_op_get_dest(ir, term).u.imm32;
              if (target >= 0 && target < (int)pred_end &&
                  starts[idx] != INTERVAL_NOT_STARTED &&
                  starts[idx] > (uint32_t)target &&
                  starts[idx] <= pred_end) {
                starts[idx] = target;
              }
            }
          }
          if (starts[idx] == INTERVAL_NOT_STARTED) starts[idx] = 0;
          if (ends[idx] < pred_end) ends[idx] = pred_end;
          /* When the phi operand is defined AFTER the predecessor block
           * (e.g., defined at instruction 144, predecessor ends at 15),
           * the value flows through a back-edge: def-block → pred-block → phi.
           * The interval must cover from the definition to the function end
           * AND from the start to the predecessor end. */
          if (starts[idx] != INTERVAL_NOT_STARTED && starts[idx] > pred_end) {
            if ((uint32_t)(n - 1) > ends[idx])
              ends[idx] = (uint32_t)(n - 1);
            starts[idx] = 0;
          }
        }
      }
    }
  }

  /* Tighten phi dest intervals: the def/use scan sets use-before-def
   * starts to 0.  For phi-defined TEMPs whose only "early" start came
   * from being used (not defined) before their phi block, pull the
   * start forward to the phi block start.  This prevents inner-loop
   * phi dests from spanning the entire function. */
  for (int b = 0; b < nb; b++) {
    for (IRPhiNode *phi = ssa->block_phis[b]; phi; phi = phi->next) {
      int32_t dest_vr = phi->dest_vreg;
      if (dest_vr < 0 || !tcc_ir_vreg_is_valid(ir, dest_vr))
        continue;
      int idx = VREG_IDX(dest_vr);
      if (idx >= table_size || starts[idx] == INTERVAL_NOT_STARTED)
        continue;
      uint32_t bstart = cfg->blocks[b].start_idx;
      /* Disabled: phi dest tightening causes regressions. Needs more
       * investigation into which phi dests are safe to tighten. */
      (void)bstart;
    }
  }

  if (TCC_LOG_LS) {
    RA_DBG("SSA ra_build_intervals: after phi extension");
    for (int idx = 0; idx < table_size; idx++) {
      if (starts[idx] == INTERVAL_NOT_STARTED) continue;
      int type = idx / max_vreg_pos;
      int pos = idx % max_vreg_pos;
      RA_DBG("  %s%d range=[%u,%u]", ra_vreg_type_char(type), pos,
             starts[idx], ends[idx]);
    }
  }

  /* Track vregs whose end was pushed to a CALL because they feed a
   * FUNCPARAMVAL of that call.  These are "consumed at the call" — they
   * don't need a callee-saved register.  Used below in the crosses_call
   * computation to avoid spuriously forcing R4-R11 for arg sources. */
  uint8_t *param_extended = NULL;
  if (table_size > 0)
    param_extended = tcc_mallocz((table_size + 7) / 8);

  /* Extend FUNCPARAMVAL intervals to their FUNCCALL.
   *
   * Find each PARAM's matching CALL by forward-scanning for the next CALL
   * whose call_id matches.  This handles two cases the original "build a
   * cid -> call_idx map" approach got wrong when functions had many calls:
   *
   *   1. Nested calls — PARAMs for an outer call can be emitted before
   *      inner calls complete.  Matching by cid (not just "next CALL")
   *      correctly skips over inner CALLs.
   *
   *   2. call_id wrap-around — the IR encodes call_id in 16 bits, so a
   *      function with >65536 calls reuses ids.  The original map kept
   *      only the LAST CALL per cid, so early PARAMs (cid=0 from the
   *      first call) wrongly pointed at the late-in-function CALL that
   *      had reused cid=0, ballooning the PARAM source's lifetime to
   *      function end.  Forward-scan stops at the first matching cid
   *      *after* the PARAM, picking the genuinely paired CALL. */
  for (int i = 0; i < n; i++) {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_FUNCPARAMVAL) continue;
    int cid = TCCIR_DECODE_CALL_ID(irop_get_imm64_ex(ir, tcc_ir_op_get_src2(ir, q)));
    if (cid < 0) continue;
    /* Find the next CALL after this PARAM with matching cid. */
    int cidx = -1;
    for (int j = i + 1; j < n; j++) {
      IRQuadCompact *qq = &ir->compact_instructions[j];
      if (qq->op != TCCIR_OP_FUNCCALLVOID && qq->op != TCCIR_OP_FUNCCALLVAL) continue;
      int ccid = TCCIR_DECODE_CALL_ID(irop_get_imm64_ex(ir, tcc_ir_op_get_src2(ir, qq)));
      if (ccid == cid) {
        cidx = j;
        break;
      }
    }
    if (cidx < 0) continue;
    IROperand s1 = tcc_ir_op_get_src1(ir, q);
    int32_t vr = irop_get_vreg(s1);
    if (vr >= 0 && tcc_ir_vreg_is_valid(ir, vr)) {
      int idx = VREG_IDX(vr);
      if (idx < table_size) {
        if (ends[idx] < (uint32_t)cidx) ends[idx] = cidx;
        if (starts[idx] == INTERVAL_NOT_STARTED) starts[idx] = 0;
        /* The PARAM source is consumed by the call as a register argument.
         * is_lval=1 nominally means the source is dereferenced (load from
         * its stack slot), but if the underlying vreg is a register-
         * promotable VAR (addrtaken=0, not lvalue-typed), the "deref" is a
         * plain register read and the value still doesn't outlive the
         * call — let it land in a caller-saved arg register. */
        int eligible = !s1.is_lval;
        if (!eligible && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_VAR) {
          IRLiveInterval *li = tcc_ir_vreg_live_interval(ir, vr);
          if (li && !li->addrtaken && !li->is_lvalue)
            eligible = 1;
        }
        if (param_extended && (int)ends[idx] == cidx && eligible) {
          int sbtype = irop_get_btype(s1);
          int is64 = (sbtype == IROP_BTYPE_INT64 || sbtype == IROP_BTYPE_FLOAT64);
          /* 64-bit register-pair sources consumed directly at the call may
           * also live in caller-saved arg registers: the parallel arg-move
           * resolver (thumb_emit_parallel_arg_moves) shuffles a pair from any
           * source registers into the AAPCS arg pair, breaking cycles with a
           * scratch.  This keeps a soft-float double / long long that feeds
           * the next helper call resident in r0:r1 instead of spilling it to a
           * stack home and reloading (the dominant cost in chained soft-float
           * expressions like Horner polynomials, pr58574).  Restrict the
           * 64-bit case to non-deref register sources — a 64-bit lval deref
           * loads from memory through a separate codegen path. */
          if (!is64 || !s1.is_lval)
            param_extended[idx >> 3] |= (uint8_t)(1u << (idx & 7));
        }
      }
    }
  }

  /* Extend lifetimes of addrtaken VAR vregs to cover pointer-derived uses.
   *
   * When `T = &V` materializes V's address into a temp T, V's stack slot is
   * effectively in use until T (or any pointer transitively derived from T)
   * dies.  Without this extension, V's vreg lifetime ends at the AddrOf
   * instruction even though the slot is still read via T at later
   * instructions (e.g. through a cleanup-attribute call).
   *
   * This extension makes V's lifetime cover its slot's true memory liveness,
   * allowing stack-slot reuse for non-overlapping addrtaken VARs in
   * ra_linear_scan below.
   *
   * Limitations: only simple flows are tracked (ASSIGN, LEA, ADD/SUB pointer
   * arithmetic).  Pointer escape via STORE to memory or PHI is conservatively
   * handled by extending the root V to function end.  C semantics make
   * post-scope access via stored pointers UB; we don't try to optimize that. */
  {
    int *taint_root = tcc_malloc(sizeof(int) * table_size);
    for (int i = 0; i < table_size; i++) taint_root[i] = -1;

    /* Pass 1: seed taint from direct address-of patterns.
     * Iterate to a fixed point to propagate through chained ASSIGNs. */
    int changed;
    do {
      changed = 0;
      for (int i = 0; i < n; i++) {
        IRQuadCompact *q = &ir->compact_instructions[i];
        if (q->op == TCCIR_OP_NOP) continue;
        /* STORE-class ops don't produce a value-holding dest. */
        if (q->op == TCCIR_OP_STORE || q->op == TCCIR_OP_STORE_INDEXED ||
            q->op == TCCIR_OP_STORE_POSTINC) continue;
        if (!irop_config[q->op].has_dest) continue;

        IROperand dest = tcc_ir_op_get_dest(ir, q);
        int32_t dvr = irop_get_vreg(dest);
        if (dvr < 0 || !tcc_ir_vreg_is_valid(ir, dvr)) continue;
        int didx = VREG_IDX(dvr);
        if (didx >= table_size) continue;
        if (taint_root[didx] >= 0) continue; /* already tainted */

        int new_root = -1;
        IROperand srcs[2];
        int nsrcs = 0;
        if (irop_config[q->op].has_src1) srcs[nsrcs++] = tcc_ir_op_get_src1(ir, q);
        if (irop_config[q->op].has_src2) srcs[nsrcs++] = tcc_ir_op_get_src2(ir, q);

        /* Only propagate through pointer-producing ops: ASSIGN, LEA, and
         * pointer arithmetic (ADD, SUB).  Other ops (LOAD, MUL, etc.) read
         * the pointer's value but don't produce a new pointer to the same
         * region. */
        int propagate = (q->op == TCCIR_OP_ASSIGN || q->op == TCCIR_OP_LEA ||
                         q->op == TCCIR_OP_ADD || q->op == TCCIR_OP_SUB);
        if (!propagate) continue;

        for (int k = 0; k < nsrcs && new_root < 0; k++) {
          IROperand s = srcs[k];
          /* Direct: src is &V where V is addrtaken VAR.
           * The is_local flag plus !is_lval distinguishes address-of from
           * load-from-stack-slot. */
          if (irop_get_tag(s) == IROP_TAG_STACKOFF && !s.is_lval && s.is_local) {
            int32_t v_vr = irop_get_vreg(s);
            if (v_vr >= 0 && tcc_ir_vreg_is_valid(ir, v_vr)) {
              IRLiveInterval *li = tcc_ir_vreg_live_interval(ir, v_vr);
              if (li && li->addrtaken) {
                int vidx = VREG_IDX(v_vr);
                if (vidx < table_size) new_root = vidx;
              }
            }
          }
          /* Transitive: src is a tainted vreg. */
          if (new_root < 0) {
            int32_t s_vr = irop_get_vreg(s);
            if (s_vr >= 0 && tcc_ir_vreg_is_valid(ir, s_vr)) {
              int sidx = VREG_IDX(s_vr);
              if (sidx < table_size && taint_root[sidx] >= 0)
                new_root = taint_root[sidx];
            }
          }
        }

        if (new_root >= 0) {
          taint_root[didx] = new_root;
          /* Extend root V's end to dest's end. */
          if (ends[didx] > ends[new_root]) {
            ends[new_root] = ends[didx];
            changed = 1;
          }
        }
      }
    } while (changed);

    tcc_free(taint_root);
  }

  if (TCC_LOG_LS) {
    RA_DBG("SSA ra_build_intervals: after addrtaken pointer-flow extension");
    for (int idx = 0; idx < table_size; idx++) {
      if (starts[idx] == INTERVAL_NOT_STARTED) continue;
      int type = idx / max_vreg_pos;
      int pos = idx % max_vreg_pos;
      RA_DBG("  %s%d range=[%u,%u]", ra_vreg_type_char(type), pos,
             starts[idx], ends[idx]);
    }
  }

  /* Extend intervals for backward jumps (loops).
   *
   * For each back-edge from instruction `i` to `target`, any variable that is
   * live-in at `target` must remain live until `i` (the loop iterates).
   *
   * "Live-in at target" means the value is available at the target and is still
   * needed.  Some loop-carried SSA temporaries are materialized by phi copies
   * at the loop target, so equality is live-in for the next back-edge too.
   *
   * Most SSA starts stay at their definition point.  Loop-carried temporaries
   * are the exception: a single linear interval cannot represent a wrapped
   * live range, so we conservatively move those starts to the loop target. */

  /* Bitset of vregs (by table_size index) that have a phi-resolution copy
   * — i.e., an ASSIGN dest — somewhere in the IR.  Computed once and reused
   * by every back-edge below.  An interval is "loop-carried" at a back-edge
   * (target, i) iff its vreg is ASSIGNed somewhere in [target, i): that's
   * the latch copy that materializes the next iteration's value.  This
   * catches the real loop-carried temps even though ssa->block_phis has
   * been cleared by pre-RA phi resolution. */
  uint8_t *vreg_has_assign = NULL;
  {
    int bitset_bytes = (table_size + 7) / 8;
    if (bitset_bytes > 0) {
      vreg_has_assign = tcc_mallocz(bitset_bytes);
      for (int i = 0; i < n; i++) {
        IRQuadCompact *q = &ir->compact_instructions[i];
        if (q->op != TCCIR_OP_ASSIGN) continue;
        int32_t dv = irop_get_vreg(tcc_ir_op_get_dest(ir, q));
        if (dv < 0 || !tcc_ir_vreg_is_valid(ir, dv)) continue;
        int didx = VREG_IDX(dv);
        if (didx >= 0 && didx < table_size)
          vreg_has_assign[didx >> 3] |= (uint8_t)(1u << (didx & 7));
      }
    }
  }

  #define RA_VREG_HAS_ASSIGN_IN_RANGE(vidx, lo, hi)                             \
    ({                                                                          \
      int found_ = 0;                                                          \
      if (vreg_has_assign && ((vreg_has_assign[(vidx) >> 3] >> ((vidx) & 7)) & 1)) { \
        int type__ = (vidx) / max_vreg_pos;                                    \
        int pos__ = (vidx) % max_vreg_pos;                                     \
        int32_t needle_ = TCCIR_ENCODE_VREG(type__, pos__);                    \
        for (int s_ = (lo); s_ < (hi); s_++) {                                 \
          IRQuadCompact *qa_ = &ir->compact_instructions[s_];                  \
          if (qa_->op != TCCIR_OP_ASSIGN) continue;                            \
          int32_t adv_ = irop_get_vreg(tcc_ir_op_get_dest(ir, qa_));           \
          if (adv_ == needle_) { found_ = 1; break; }                          \
        }                                                                       \
      }                                                                         \
      found_;                                                                  \
    })

  #define RA_EXTEND_BACKEDGE(i, target, broad)                                  \
    do {                                                                        \
      RA_DBG("  back-edge i=%d -> target=%d%s", (i), (target),                 \
             (broad) ? " (broad)" : "");                                        \
      for (int idx_ = 0; idx_ < table_size; idx_++) {                           \
        if (starts[idx_] == INTERVAL_NOT_STARTED) continue;                     \
        int type_ = idx_ / max_vreg_pos;                                        \
        int live_at_target_;                                                     \
        int live_at_backedge_ = ((int)starts[idx_] <= (i) &&                    \
                                 (int)ends[idx_] >= (i));                        \
        /* For IJMP the actual target is unknown at compile time, so use a      \
         * broad overlap check: any interval touching [target, i]. */           \
        int overlaps_loop_ = (broad) && ((int)starts[idx_] <= (i) &&            \
                                         (int)ends[idx_] >= (target));           \
        live_at_target_ = ((int)starts[idx_] <= (target) &&                     \
                           (int)ends[idx_] >= (target));                         \
        if (!live_at_target_ && (live_at_backedge_ || overlaps_loop_) &&        \
            (int)starts[idx_] > (target)) {                                     \
          /* Only extend when actually loop-carried.  An interval that's       \
           * "live at the back-edge JMP" without a phi-resolution ASSIGN in   \
           * [target, i) is just a within-iteration temp whose lifetime      \
           * happens to extend past the JMP via a fall-through use (failure  \
           * path); extending it would balloon a short range into a full-    \
           * loop span and cause spurious spills.  For broad (IJMP), keep   \
           * prior conservative behavior since the real target is unknown. */ \
          int is_loop_carried_ = (broad) ? 1 :                                  \
              RA_VREG_HAS_ASSIGN_IN_RANGE(idx_, (target), (i));                 \
          if (is_loop_carried_) {                                                \
            uint32_t old_s_ = starts[idx_];                                     \
            starts[idx_] = (target);                                            \
            RA_DBG("    %s%d [%u,%u] -> [%u,%u] (loop-carried)",               \
                   ra_vreg_type_char(type_), idx_ % max_vreg_pos, old_s_,       \
                   ends[idx_], starts[idx_], ends[idx_]);                        \
          }                                                                    \
        }                                                                       \
        if ((live_at_target_ || live_at_backedge_ || overlaps_loop_) &&         \
            (int)ends[idx_] < (i)) {                                            \
          uint32_t old_e_ = ends[idx_];                                         \
          ends[idx_] = (i);                                                     \
          RA_DBG("    %s%d [%u,%u] -> [%u,%u]", ra_vreg_type_char(type_),      \
                 idx_ % max_vreg_pos, starts[idx_], old_e_, starts[idx_],       \
                 ends[idx_]);                                                    \
        }                                                                       \
      }                                                                         \
    } while (0)

  for (int i = 0; i < n; i++) {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_JUMPIF) {
      int target = (int)tcc_ir_op_get_dest(ir, q).u.imm32;
      if (target >= 0 && target < n && target < i)
        RA_EXTEND_BACKEDGE(i, target, 0);
    } else if (q->op == TCCIR_OP_IJUMP) {
      if (i > 0)
        RA_EXTEND_BACKEDGE(i, 0, 1);
    } else if (q->op == TCCIR_OP_SWITCH_TABLE) {
      IROperand s2 = tcc_ir_op_get_src2(ir, q);
      int table_id = (int)irop_get_imm64_ex(ir, s2);
      if (table_id >= 0 && table_id < ir->num_switch_tables) {
        TCCIRSwitchTable *table = &ir->switch_tables[table_id];
        for (int j = 0; j < table->num_entries; j++) {
          int t = table->targets[j];
          if (t >= 0 && t < n && t < i)
            RA_EXTEND_BACKEDGE(i, t, 0);
        }
        int dt = table->default_target;
        if (dt >= 0 && dt < n && dt < i)
          RA_EXTEND_BACKEDGE(i, dt, 0);
      }
    }
  }

  #undef RA_EXTEND_BACKEDGE
  #undef RA_VREG_HAS_ASSIGN_IN_RANGE
  tcc_free(vreg_has_assign);

  if (TCC_LOG_LS) {
    RA_DBG("SSA ra_build_intervals: after backward jump extension");
    for (int idx = 0; idx < table_size; idx++) {
      if (starts[idx] == INTERVAL_NOT_STARTED) continue;
      int type = idx / max_vreg_pos;
      int pos = idx % max_vreg_pos;
      RA_DBG("  %s%d range=[%u,%u]", ra_vreg_type_char(type), pos,
             starts[idx], ends[idx]);
    }
  }

  /* Count active intervals */
  int count = 0;
  for (int idx = 0; idx < table_size; idx++) {
    if (starts[idx] != INTERVAL_NOT_STARTED && ends[idx] >= starts[idx]) count++;
  }

  SSAInterval *intervals = tcc_mallocz(sizeof(SSAInterval) * (count > 0 ? count : 1));
  int wi = 0;

  for (int type = 1; type <= 3; type++) {
    int limit = (type == TCCIR_VREG_TYPE_VAR) ? local_count :
                (type == TCCIR_VREG_TYPE_TEMP) ? temp_count : param_count;
    for (int pos = 0; pos < limit; pos++) {
      int idx = type * max_vreg_pos + pos;
      if (idx >= table_size || starts[idx] == INTERVAL_NOT_STARTED) continue;
      if (ends[idx] < starts[idx]) continue;
      int32_t vreg = TCCIR_ENCODE_VREG(type, pos);
      if (tcc_ir_vreg_is_ignored(ir, vreg)) continue;

      SSAInterval *iv = &intervals[wi];
      iv->vreg = vreg;
      iv->start = starts[idx];
      iv->end = ends[idx];
      iv->r0 = -1;
      iv->r1 = -1;
      iv->stack_location = 0;
      iv->use_count = uses[idx];
      iv->precolored = -1;
      iv->pref_reg = -1;
      iv->hint_vreg = -1;
      iv->coalesce_to = -1;
      iv->co_member = 0;
      iv->is_param = (type == TCCIR_VREG_TYPE_PARAM);
      iv->reg_shared = 0;
      iv->loop_phi_locked = 0;

      IRLiveInterval *li = tcc_ir_vreg_live_interval(ir, vreg);
      iv->addrtaken = li->addrtaken;
      iv->reg_type = tcc_ir_vreg_type_get(ir, vreg);

      /* Static chain vreg */
      if (ir->has_static_chain && vreg == ir->static_chain_vreg) {
        iv->end = n;
        iv->crosses_call = 1;
        iv->precolored = 10;
      }

      /* Call crossing: a call STRICTLY between def and last use.
       * If iv->end is at a call AND this vreg was extended to that call
       * by the FUNCPARAMVAL extension above, the value is being consumed
       * as a call argument — it does not outlive the call, so crosses_call
       * stays 0 (lets the allocator place it in a caller-saved arg reg).
       * For other vregs whose end lands at a call (e.g., the function
       * pointer of an indirect call), keep the conservative crosses_call=1. */
      if (!iv->crosses_call) {
        iv->crosses_call = ra_has_call_in_range(call_prefix, iv->start, iv->end, n);
        if (!iv->crosses_call && iv->end < (uint32_t)n) {
          TccIrOp eop = ir->compact_instructions[iv->end].op;
          if (eop == TCCIR_OP_FUNCCALLVAL || eop == TCCIR_OP_FUNCCALLVOID) {
            int idx = VREG_IDX(vreg);
            int is_param_use = (param_extended &&
                                ((param_extended[idx >> 3] >> (idx & 7)) & 1));
            if (!is_param_use)
              iv->crosses_call = 1;
          }
        }
      }

      /* Switch crossing: a SWITCH_TABLE/SWITCH_LOAD dispatch clobbers R_IP
       * (R12) as its jump-table scratch (tcc_gen_machine_switch_table_mop).
       * A value live across the dispatch must therefore not occupy R12.  R12
       * is caller-saved, so reuse crosses_call to force the value into a
       * callee-saved register — exactly what the -O1 allocator already does.
       * (fuzz seed 102: at -O2 the loop-carried checksum `cs` was placed in
       * R12 and clobbered by the switch dispatch, corrupting the result.) */
      if (!iv->crosses_call)
        iv->crosses_call = ra_has_switch_in_range(switch_prefix, iv->start, iv->end, n);

      /* Params: start at 0, precolor if in register.
       * Do NOT bump end past its last actual use — the pref_reg boundary
       * eviction (a->end == cur->start) relies on the param expiring at
       * the instruction that consumes it.  Phantom-extending end to 1
       * would force a return-value temporary defined at instruction 0
       * onto a different register and emit a redundant mov to r0. */
      if (type == TCCIR_VREG_TYPE_PARAM) {
        iv->start = 0;
        if (pos < 4 && !iv->crosses_call && li->incoming_reg0 >= 0)
          iv->precolored = li->incoming_reg0;
      } else if (li->incoming_reg0 >= 0 && iv->reg_type == LS_REG_TYPE_INT) {
        /* Non-PARAM with incoming_reg0 hint (set by setup_returnvalue_hint
         * in codegen): use as a soft preference. The linear scan will try
         * this register first and handle the boundary case where a PARAM
         * is just expiring at this start point. */
        iv->pref_reg = (int8_t)li->incoming_reg0;
      }

      wi++;
    }
  }

  tcc_free(starts);
  tcc_free(ends);
  tcc_free(uses);
  tcc_free(instr_depth);
  tcc_free(param_extended);
  tcc_free(vreg_read_as_src);

  if (TCC_LOG_LS) {
    RA_DBG("SSA ra_build_intervals: %d final intervals", wi);
    for (int i = 0; i < wi; i++) {
      SSAInterval *iv = &intervals[i];
      int type = TCCIR_DECODE_VREG_TYPE(iv->vreg);
      int pos = TCCIR_DECODE_VREG_POSITION(iv->vreg);
      RA_DBG("  %s%d range=[%u,%u] uses=%u xcall=%d addrtaken=%d precolored=%d regtype=%d",
             ra_vreg_type_char(type), pos, iv->start, iv->end, iv->use_count,
             iv->crosses_call, iv->addrtaken, iv->precolored, iv->reg_type);
    }
  }

  *out_intervals = intervals;
  *out_count = wi;
  if (out_max_vreg_pos)
    *out_max_vreg_pos = max_vreg_pos;
  if (switch_prefix) tcc_free(switch_prefix);
  #undef VREG_IDX
}

/* ============================================================================
 * Phi Register Hints
 * ============================================================================ */

static void ra_build_phi_hints(SSAInterval *intervals, int count,
                               IRSSAState *ssa, IRCFG *cfg, int max_vreg_pos)
{
  if (!ssa || !ssa->block_phis || !cfg || max_vreg_pos <= 0)
    return;

  int table_size = 4 * max_vreg_pos;
  int *vreg_to_iv = tcc_malloc(sizeof(int) * table_size);
  for (int i = 0; i < table_size; i++)
    vreg_to_iv[i] = -1;

  #define PHI_VREG_IDX(vr) \
    ((TCCIR_DECODE_VREG_TYPE(vr) * max_vreg_pos) + TCCIR_DECODE_VREG_POSITION(vr))

  for (int i = 0; i < count; i++) {
    int idx = PHI_VREG_IDX(intervals[i].vreg);
    if (idx >= 0 && idx < table_size)
      vreg_to_iv[idx] = i;
  }

  for (int b = 0; b < cfg->num_blocks; b++) {
    for (IRPhiNode *phi = ssa->block_phis[b]; phi; phi = phi->next) {
      int32_t dest_vr = phi->dest_vreg;
      if (dest_vr < 0)
        continue;
      int dest_tbl = PHI_VREG_IDX(dest_vr);
      if (dest_tbl < 0 || dest_tbl >= table_size)
        continue;
      int dest_iv = vreg_to_iv[dest_tbl];
      if (dest_iv < 0)
        continue;

      for (int pi = 0; pi < phi->num_operands; pi++) {
        int32_t op_vr = phi->operands[pi].vreg;
        if (op_vr < 0)
          continue;
        int op_tbl = PHI_VREG_IDX(op_vr);
        if (op_tbl < 0 || op_tbl >= table_size)
          continue;
        int op_iv = vreg_to_iv[op_tbl];
        if (op_iv < 0)
          continue;
        if (intervals[dest_iv].reg_type != intervals[op_iv].reg_type)
          continue;
        if (intervals[dest_iv].hint_vreg < 0)
          intervals[dest_iv].hint_vreg = op_vr;
        if (intervals[op_iv].hint_vreg < 0)
          intervals[op_iv].hint_vreg = dest_vr;
      }
    }
  }

  tcc_free(vreg_to_iv);
  #undef PHI_VREG_IDX
}

/* Build coalescing hints from explicit ASSIGN copies in the instruction
 * stream. After pre-RA phi resolution, block_phis is empty but the IR
 * carries `dest = src` copies at each former phi edge. Each such copy
 * is a place where we'd like dest and src to share a register so the
 * post-RA move-coalescing pass can erase the mov rX, rX. */
static void ra_build_assign_hints(SSAInterval *intervals, int count,
                                  TCCIRState *ir, int max_vreg_pos)
{
  if (max_vreg_pos <= 0) return;

  int table_size = 4 * max_vreg_pos;
  int *vreg_to_iv = tcc_malloc(sizeof(int) * table_size);
  for (int i = 0; i < table_size; i++)
    vreg_to_iv[i] = -1;

  #define ASSIGN_VREG_IDX(vr) \
    ((TCCIR_DECODE_VREG_TYPE(vr) * max_vreg_pos) + TCCIR_DECODE_VREG_POSITION(vr))

  for (int i = 0; i < count; i++) {
    int idx = ASSIGN_VREG_IDX(intervals[i].vreg);
    if (idx >= 0 && idx < table_size)
      vreg_to_iv[idx] = i;
  }

  int n = ir->next_instruction_index;
  for (int i = 0; i < n; i++) {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_ASSIGN) continue;
    IROperand d = tcc_ir_op_get_dest(ir, q);
    IROperand s = tcc_ir_op_get_src1(ir, q);
    int32_t dest_vr = irop_get_vreg(d);
    int32_t src_vr = irop_get_vreg(s);
    if (dest_vr < 0 || src_vr < 0) continue;
    /* A deref ASSIGN (`dest = *src` load, or `*dest = src` store) is NOT a
     * register copy: dest and src hold different values (the pointer vs the
     * loaded/stored word), so they must never be hinted onto the same register.
     * Hinting `T15 = *T2` onto T2's register let T15's def clobber the pointer
     * T2, which a deferred `PARAM *T2` deref at the following variadic call
     * still needed -> the arg register held the value, not the address, and the
     * call read through a garbage pointer (fuzz varargs 293237). */
    if (d.is_lval || s.is_lval) continue;
    /* Width gate: a `dest = src` ASSIGN whose dest and src differ in width is
     * NOT a pure register copy — it is an extension (i32->i64 zeroes/sign-fills
     * the high word) or a truncation.  Coalescing the two vregs into one
     * register makes the post-RA move-coalescing pass erase the `mov`, so the
     * high-word materialization the ASSIGN lowering would emit is lost and any
     * later 64-bit consumer reads a garbage high half (e.g. a packed >32-bit
     * bitfield read collapsed from a SAR/SHL/OR sign-extend idiom). */
    if (irop_is_64bit(d) != irop_is_64bit(s)) continue;
    int dest_tbl = ASSIGN_VREG_IDX(dest_vr);
    int src_tbl = ASSIGN_VREG_IDX(src_vr);
    if (dest_tbl < 0 || dest_tbl >= table_size) continue;
    if (src_tbl < 0 || src_tbl >= table_size) continue;
    int dest_iv = vreg_to_iv[dest_tbl];
    int src_iv = vreg_to_iv[src_tbl];
    if (dest_iv < 0 || src_iv < 0) continue;
    if (intervals[dest_iv].reg_type != intervals[src_iv].reg_type) continue;
    if (intervals[dest_iv].hint_vreg < 0)
      intervals[dest_iv].hint_vreg = src_vr;
    if (intervals[src_iv].hint_vreg < 0)
      intervals[src_iv].hint_vreg = dest_vr;
  }

  tcc_free(vreg_to_iv);
  #undef ASSIGN_VREG_IDX
}

/* Build coalescing hints from LOAD-of-PARAM copies.  TCC frontends emit
 * `Tn <-- Pk [LOAD]` at function entry to move each register-passed PARAM
 * into the local-variable temp that the function body actually reads.
 * For full-word (INT32) sources the LOAD is a pure copy, so we can hint
 * the temp toward the PARAM's register — the boundary case in the linear
 * scan (partner->end == cur->start) then lets cur take that register at
 * its def instruction, eliminating the copy.
 *
 * GATING (per feedback_load_narrowing memory): LOAD on a sub-word PARAM
 * (INT8/INT16) carries implicit AAPCS narrowing, so it is NOT a pure copy
 * and must be skipped.  INT64 needs a register pair and isn't expressible
 * as a single-reg hint.  is_lval sources are real memory dereferences,
 * not pass-through copies. */
static void ra_build_load_param_hints(SSAInterval *intervals, int count,
                                      TCCIRState *ir, int max_vreg_pos)
{
  if (max_vreg_pos <= 0) return;

  int table_size = 4 * max_vreg_pos;
  int *vreg_to_iv = tcc_malloc(sizeof(int) * table_size);
  for (int i = 0; i < table_size; i++)
    vreg_to_iv[i] = -1;

  #define LOAD_VREG_IDX(vr) \
    ((TCCIR_DECODE_VREG_TYPE(vr) * max_vreg_pos) + TCCIR_DECODE_VREG_POSITION(vr))

  for (int i = 0; i < count; i++) {
    int idx = LOAD_VREG_IDX(intervals[i].vreg);
    if (idx >= 0 && idx < table_size)
      vreg_to_iv[idx] = i;
  }

  int n = ir->next_instruction_index;
  for (int i = 0; i < n; i++) {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_LOAD) continue;
    IROperand d = tcc_ir_op_get_dest(ir, q);
    IROperand s = tcc_ir_op_get_src1(ir, q);
    if (s.is_lval) continue;                /* real memory load, not a copy */
    int32_t dest_vr = irop_get_vreg(d);
    int32_t src_vr = irop_get_vreg(s);
    if (dest_vr < 0 || src_vr < 0) continue;
    /* Source must be a PARAM (the only LOAD-as-copy pattern we trust). */
    if (TCCIR_DECODE_VREG_TYPE(src_vr) != TCCIR_VREG_TYPE_PARAM) continue;
    if (TCCIR_DECODE_VREG_TYPE(dest_vr) == TCCIR_VREG_TYPE_PARAM) continue;
    /* Width gate: only full-word INT32.  Sub-word carries AAPCS narrowing,
     * INT64 needs a pair, FP types use a different reg class. */
    int sbtype = irop_get_btype(s);
    if (sbtype != IROP_BTYPE_INT32) continue;
    int dbtype = irop_get_btype(d);
    if (dbtype != IROP_BTYPE_INT32) continue;
    int dest_tbl = LOAD_VREG_IDX(dest_vr);
    int src_tbl = LOAD_VREG_IDX(src_vr);
    if (dest_tbl < 0 || dest_tbl >= table_size) continue;
    if (src_tbl < 0 || src_tbl >= table_size) continue;
    int dest_iv = vreg_to_iv[dest_tbl];
    int src_iv = vreg_to_iv[src_tbl];
    if (dest_iv < 0 || src_iv < 0) continue;
    if (intervals[dest_iv].reg_type != intervals[src_iv].reg_type) continue;
    if (intervals[dest_iv].hint_vreg < 0)
      intervals[dest_iv].hint_vreg = src_vr;
    if (intervals[src_iv].hint_vreg < 0)
      intervals[src_iv].hint_vreg = dest_vr;
  }

  tcc_free(vreg_to_iv);
  #undef LOAD_VREG_IDX
}

/* Build a coalescing hint for BFI: the result reuses the host-word register.
 * BFI is two-address (`BFI Rd, Rn, #lsb, #w` with Rd preset to the host word);
 * the insert->BFI pass already NOP'd the host word's only other use (the field-
 * clearing AND), so the BFI is the word's last use and its interval ends exactly
 * where the result's begins.  Hinting the result toward the word's vreg lets the
 * linear scan's boundary case (partner->end == cur->start) give the result the
 * word's just-freed register, so the emitter skips the two-address `mov Rd,Rword`
 * (the dominant residual cost of BFI lowering).  Soft, one-directional hint: if
 * the register isn't available the emitter still falls back to the mov, so this
 * can only remove instructions, never add them. */
static void ra_build_bfi_hints(SSAInterval *intervals, int count,
                               TCCIRState *ir, int max_vreg_pos)
{
  if (max_vreg_pos <= 0) return;

  int table_size = 4 * max_vreg_pos;
  int *vreg_to_iv = tcc_malloc(sizeof(int) * table_size);
  for (int i = 0; i < table_size; i++)
    vreg_to_iv[i] = -1;

  #define BFI_VREG_IDX(vr) \
    ((TCCIR_DECODE_VREG_TYPE(vr) * max_vreg_pos) + TCCIR_DECODE_VREG_POSITION(vr))

  for (int i = 0; i < count; i++) {
    int idx = BFI_VREG_IDX(intervals[i].vreg);
    if (idx >= 0 && idx < table_size)
      vreg_to_iv[idx] = i;
  }

  int n = ir->next_instruction_index;
  for (int i = 0; i < n; i++) {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_BFI) continue;
    IROperand d = tcc_ir_op_get_dest(ir, q);
    IROperand s = tcc_ir_op_get_src1(ir, q);
    int32_t dest_vr = irop_get_vreg(d);
    int32_t src_vr = irop_get_vreg(s);
    if (dest_vr < 0 || src_vr < 0) continue;
    int dest_tbl = BFI_VREG_IDX(dest_vr);
    int src_tbl = BFI_VREG_IDX(src_vr);
    if (dest_tbl < 0 || dest_tbl >= table_size) continue;
    if (src_tbl < 0 || src_tbl >= table_size) continue;
    int dest_iv = vreg_to_iv[dest_tbl];
    int src_iv = vreg_to_iv[src_tbl];
    if (dest_iv < 0 || src_iv < 0) continue;
    if (intervals[dest_iv].reg_type != intervals[src_iv].reg_type) continue;
    if (intervals[dest_iv].hint_vreg < 0)
      intervals[dest_iv].hint_vreg = src_vr;
  }

  tcc_free(vreg_to_iv);
  #undef BFI_VREG_IDX
}

/* ============================================================================
 * Outgoing-PARAM Register Affinity
 *
 * When a TEMP feeds FUNCPARAMVAL(idx, src) with idx < 4, prefer the
 * matching AAPCS arg register (r0..r3) for that TEMP.  Without the hint
 * the regalloc picks any free int register and codegen emits a
 * `mov rN, rM` to shuffle the value into the arg slot at the CALL.
 * With the hint, the value lands in the right register up front.
 *
 * Pairs with the crosses_call relaxation above: TEMPs consumed at a
 * FUNCCALL no longer get crosses_call=1, so the soft-pref-reg path in
 * ra_linear_scan can pick caller-saved r0..r3 without bailing.
 *
 * Skips deref sources (PARAM *T uses T as an address pointer; the
 * loaded value goes into the arg at codegen time, not T itself) and
 * 64-bit args (need a register pair which single-reg affinity can't
 * express). */
static void ra_build_outgoing_param_hints(SSAInterval *intervals, int count,
                                           TCCIRState *ir, int max_vreg_pos)
{
  if (max_vreg_pos <= 0) return;

  int table_size = 4 * max_vreg_pos;
  int *vreg_to_iv = tcc_malloc(sizeof(int) * table_size);
  for (int i = 0; i < table_size; i++)
    vreg_to_iv[i] = -1;

  #define OPH_VREG_IDX(vr) \
    ((TCCIR_DECODE_VREG_TYPE(vr) * max_vreg_pos) + TCCIR_DECODE_VREG_POSITION(vr))

  for (int i = 0; i < count; i++) {
    int idx = OPH_VREG_IDX(intervals[i].vreg);
    if (idx >= 0 && idx < table_size)
      vreg_to_iv[idx] = i;
  }

  int n = ir->next_instruction_index;
  for (int i = 0; i < n; i++) {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_FUNCPARAMVAL) continue;

    IROperand src = tcc_ir_op_get_src1(ir, q);

    int sbtype = irop_get_btype(src);
    if (sbtype == IROP_BTYPE_INT64 || sbtype == IROP_BTYPE_FLOAT64) continue;

    int32_t src_vr = irop_get_vreg(src);
    if (src_vr < 0) continue;
    int src_type = TCCIR_DECODE_VREG_TYPE(src_vr);
    if (src_type != TCCIR_VREG_TYPE_TEMP && src_type != TCCIR_VREG_TYPE_VAR)
      continue;

    /* For TEMPs we require non-lval source.  For VARs we additionally accept
     * lval sources when the VAR is register-promotable (addrtaken=0 and
     * not lvalue-typed); after promotion the "deref" becomes a register
     * read, so we can place the value directly in the arg register and
     * avoid a mov from a callee-saved register. */
    if (src.is_lval) {
      if (src_type != TCCIR_VREG_TYPE_VAR) continue;
      IRLiveInterval *li = tcc_ir_vreg_live_interval(ir, src_vr);
      if (!li || li->addrtaken || li->is_lvalue) continue;
    }

    IROperand param_info = tcc_ir_op_get_src2(ir, q);
    int param_idx = TCCIR_DECODE_PARAM_IDX((int)param_info.u.imm32);
    if (param_idx < 0 || param_idx >= 4) continue;

    int tbl_idx = OPH_VREG_IDX(src_vr);
    if (tbl_idx < 0 || tbl_idx >= table_size) continue;
    int iv_idx = vreg_to_iv[tbl_idx];
    if (iv_idx < 0) continue;

    SSAInterval *iv = &intervals[iv_idx];
    if (iv->reg_type != LS_REG_TYPE_INT) continue;
    if (iv->precolored >= 0) continue;
    if (iv->crosses_call) continue;
    if (iv->pref_reg >= 0) continue;

    iv->pref_reg = (int8_t)param_idx;
  }

  tcc_free(vreg_to_iv);
  #undef OPH_VREG_IDX
}

/* ============================================================================
 * Linear Scan Allocation
 * ============================================================================ */

static int sort_by_start(const void *a, const void *b)
{
  const SSAInterval *ia = (const SSAInterval *)a;
  const SSAInterval *ib = (const SSAInterval *)b;
  if (ia->is_param && !ib->is_param) return -1;
  if (!ia->is_param && ib->is_param) return 1;
  if (ia->start < ib->start) return -1;
  if (ia->start > ib->start) return 1;
  /* Equal starts: precolored intervals first, so their fixed registers are
   * claimed before the scan hands the same register to a non-precolored
   * interval (the precolored assignment does not check int_free).  Then by
   * vreg — qsort is not stable, and leaving ties unspecified makes the
   * allocation depend on the libc's qsort (host glibc and the device libc
   * order equal elements differently). */
  if (ia->precolored >= 0 && ib->precolored < 0) return -1;
  if (ia->precolored < 0 && ib->precolored >= 0) return 1;
  if (ia->vreg < ib->vreg) return -1;
  if (ia->vreg > ib->vreg) return 1;
  return 0;
}

/* Safety check for loop-carried phi coalescing.
 *
 * Returns 1 if cur can take partner's physical register even though their
 * live intervals overlap.  The case: cur is defined inside the partner's
 * live range by an instruction that reads partner as a source, and
 * partner's only remaining use past that def is a single ASSIGN with
 * dest = partner, src1 = cur (i.e. the explicit back-edge phi copy left
 * over after SSA destruction).  Under that pattern partner's value is
 * "killed" at cur's def: after the defining instruction R holds cur's
 * value, the back-edge ASSIGN becomes mov R, R (elided), and R remains
 * the carrier for the same logical loop variable across iterations.
 *
 * Note: cur->start may have been pulled back to the loop entry by the
 * back-edge extension (so cur's "official" start is before its actual
 * def).  We locate the def position by scanning for the first instruction
 * in [cur->start, partner->end] whose dest is cur.
 *
 * Reads of partner *before* the def are fine: at those positions the
 * register holds partner's value (which equals what cur will be assigned
 * from in the previous iteration via the back-edge ASSIGN).  Reads
 * *after* the def — except for the back-edge ASSIGN itself — are not
 * fine: they would see cur's value, not partner's. */
static int ra_safe_loop_phi_coalesce(TCCIRState *ir, SSAInterval *cur, SSAInterval *partner)
{
  int n = ir->next_instruction_index;
  int cur_start = (int)cur->start;
  int partner_end = (int)partner->end;
  int32_t cur_vreg = cur->vreg;
  int32_t partner_vreg = partner->vreg;

  if (cur_start < 0 || cur_start >= n || partner_end < cur_start || partner_end >= n)
    return 0;

  /* cur (the loop update) must be consumed by the back-edge copy partner<-cur
   * at partner_end, so cur must NOT outlive partner.  If cur->end > partner_end
   * then cur and partner are two DISTINCT values that both span the loop body
   * (they interfere), and sharing one register conflates them.  This was a
   * self-host miscompile: the cross coalesced a pointer-holding interval with
   * an index-holding one in ra_coalesce_graph (cur.end > partner.end), yielding
   * a register used as both index and pointer, which corrupted that pass's own
   * coalescing decisions on later compiles.  Legitimate loop-IV updates have
   * cur.end <= partner.end (the update is dead after the back-edge copy). */
  if ((int)cur->end > partner_end)
    return 0;

  /* Locate cur's def: first instruction in [cur_start, partner_end] whose
   * non-STORE dest is cur AND which reads partner as a source. */
  int def_pos = -1;
  for (int j = cur_start; j <= partner_end; j++) {
    IRQuadCompact *q = &ir->compact_instructions[j];
    if (q->op == TCCIR_OP_NOP) continue;
    if (!irop_config[q->op].has_dest) continue;
    if (q->op == TCCIR_OP_STORE || q->op == TCCIR_OP_STORE_INDEXED ||
        q->op == TCCIR_OP_STORE_POSTINC) continue;
    IROperand d = tcc_ir_op_get_dest(ir, q);
    if (!irop_has_vreg(d) || irop_get_vreg(d) != cur_vreg) continue;

    int reads_partner = 0;
    if (irop_config[q->op].has_src1) {
      IROperand s = tcc_ir_op_get_src1(ir, q);
      if (irop_has_vreg(s) && irop_get_vreg(s) == partner_vreg) reads_partner = 1;
    }
    if (!reads_partner && irop_config[q->op].has_src2) {
      IROperand s = tcc_ir_op_get_src2(ir, q);
      if (irop_has_vreg(s) && irop_get_vreg(s) == partner_vreg) reads_partner = 1;
    }
    if (!reads_partner && q->op == TCCIR_OP_MLA) {
      IROperand s = tcc_ir_op_get_accum(ir, q);
      if (irop_has_vreg(s) && irop_get_vreg(s) == partner_vreg) reads_partner = 1;
    }
    if (!reads_partner) return 0;
    def_pos = j;
    break;
  }
  if (def_pos < 0) return 0;

  int found_back_copy = 0;
  for (int j = def_pos + 1; j <= partner_end; j++) {
    IRQuadCompact *q = &ir->compact_instructions[j];
    if (q->op == TCCIR_OP_NOP) continue;

    /* cur must be defined only at def_pos.  The override's correctness rests on
     * "after def_pos the register holds cur's value and the back-edge copy is
     * mov R,R"; a *second* def of cur before the back-edge breaks that — the
     * register then carries an intermediate value while partner is still
     * (textually) live, and coalescing conflates two distinct values.  This
     * happens when def_pos is a copy `cur <- partner` at the top of an OUTER
     * loop body and cur is then re-assigned inside a nested (rotated) inner
     * loop before the outer back-edge copy `partner <- cur` (longlong seed 218:
     * g12-carried hash T160<-T161, re-defined inside the rotated g16 loop). The
     * linear scan cannot model the inner back-edge, so reject conservatively. */
    if (irop_config[q->op].has_dest) {
      IROperand cd = tcc_ir_op_get_dest(ir, q);
      if (irop_has_vreg(cd) && irop_get_vreg(cd) == cur_vreg)
        return 0;
    }

    int uses_partner_as_src = 0;
    if (irop_config[q->op].has_src1) {
      IROperand s = tcc_ir_op_get_src1(ir, q);
      if (irop_has_vreg(s) && irop_get_vreg(s) == partner_vreg)
        uses_partner_as_src = 1;
    }
    if (!uses_partner_as_src && irop_config[q->op].has_src2) {
      IROperand s = tcc_ir_op_get_src2(ir, q);
      if (irop_has_vreg(s) && irop_get_vreg(s) == partner_vreg)
        uses_partner_as_src = 1;
    }
    if (!uses_partner_as_src && q->op == TCCIR_OP_MLA) {
      IROperand s = tcc_ir_op_get_accum(ir, q);
      if (irop_has_vreg(s) && irop_get_vreg(s) == partner_vreg)
        uses_partner_as_src = 1;
    }
    if (uses_partner_as_src) {
      if (q->op != TCCIR_OP_ASSIGN) return 0;
      IROperand d = tcc_ir_op_get_dest(ir, q);
      if (!irop_has_vreg(d) || irop_get_vreg(d) != partner_vreg) return 0;
      IROperand s = tcc_ir_op_get_src1(ir, q);
      if (!irop_has_vreg(s) || irop_get_vreg(s) != cur_vreg) return 0;
      if (found_back_copy) return 0;
      found_back_copy = 1;
      continue;
    }

    if (irop_config[q->op].has_dest) {
      int dest_is_use = (q->op == TCCIR_OP_STORE || q->op == TCCIR_OP_STORE_INDEXED ||
                         q->op == TCCIR_OP_STORE_POSTINC);
      if (dest_is_use) {
        IROperand d = tcc_ir_op_get_dest(ir, q);
        if (irop_has_vreg(d) && irop_get_vreg(d) == partner_vreg)
          return 0;
      } else {
        IROperand d = tcc_ir_op_get_dest(ir, q);
        if (irop_has_vreg(d) && irop_get_vreg(d) == partner_vreg) {
          if (q->op != TCCIR_OP_ASSIGN) return 0;
          IROperand s = tcc_ir_op_get_src1(ir, q);
          if (!irop_has_vreg(s) || irop_get_vreg(s) != cur_vreg) return 0;
          if (found_back_copy) return 0;
          found_back_copy = 1;
        }
      }
    }
  }
  return found_back_copy;
}

/* Returns 1 if instruction q references `vreg` as any source or destination
 * operand (read or write).  Mirrors the operand enumeration ra_build_intervals
 * uses (src1, src2, MLA accumulator, dest including STORE-class). */
static int ra_instr_touches_vreg(TCCIRState *ir, IRQuadCompact *q, int32_t vreg)
{
  if (q->op == TCCIR_OP_NOP)
    return 0;
  if (irop_config[q->op].has_src1) {
    IROperand s = tcc_ir_op_get_src1(ir, q);
    if (irop_has_vreg(s) && irop_get_vreg(s) == vreg) return 1;
  }
  if (irop_config[q->op].has_src2) {
    IROperand s = tcc_ir_op_get_src2(ir, q);
    if (irop_has_vreg(s) && irop_get_vreg(s) == vreg) return 1;
  }
  if (q->op == TCCIR_OP_MLA) {
    IROperand s = tcc_ir_op_get_accum(ir, q);
    if (irop_has_vreg(s) && irop_get_vreg(s) == vreg) return 1;
  }
  if (irop_config[q->op].has_dest) {
    IROperand d = tcc_ir_op_get_dest(ir, q);
    if (irop_has_vreg(d) && irop_get_vreg(d) == vreg) return 1;
  }
  return 0;
}

/* Control-flow-aware safety check for "exit-phi" coalescing.
 *
 * cur is defined by a pure copy `cur <- partner` (an ASSIGN whose only source
 * is partner — the SSA-destruction copy left at a loop-exit / block-merge
 * edge).  Linear scan models partner with a single [start,end] interval whose
 * end is partner's last *textual* use.  Under TCC's inverted-header loop
 * layout the loop body is emitted *after* the loop's exit edge, so partner's
 * body uses sit textually below the copy and the interval spuriously overlaps
 * cur — even though, in the actual control flow, reaching the copy means the
 * loop has exited and partner is dead.  ra_safe_loop_phi_coalesce only covers
 * the back-edge phi (`partner <- cur`), not this forward exit phi.
 *
 * Returns 1 when no instruction reachable from *after* the copy reads or
 * writes partner — i.e. partner is genuinely dead past the copy.  cur and
 * partner are then the same logical value over disjoint control-flow regions
 * and may share a register; the residual `mov R,R` is erased by the post-RA
 * move-coalescing pass.
 *
 * Reachability is computed directly on the current instruction stream (jump
 * targets define the edges).  The shared `cfg` cannot be used here: it is
 * built before ra_resolve_phis inserts these very copies and is stale by the
 * time the linear scan runs.  Any indirect/multi-target transfer (IJUMP,
 * SWITCH_*) whose successors we cannot enumerate forces a conservative
 * reject. */
static int ra_safe_exit_phi_coalesce(TCCIRState *ir, SSAInterval *cur, SSAInterval *partner)
{
  int n = ir->next_instruction_index;
  int32_t cur_vreg = cur->vreg;
  int32_t partner_vreg = partner->vreg;
  if (cur->start < 0 || (int)cur->start >= n)
    return 0;

  /* cur must be partner's forward continuation, which means it outlives
   * partner: cur->end >= partner->end.  When partner outlives cur, partner is
   * still needed on some path after cur dies (e.g. a parameter live across all
   * arms of a switch while cur is a copy in one arm); transferring partner's
   * register to cur and dropping partner from the active set would then free
   * the register while partner is still live elsewhere, corrupting it. */
  if ((int)partner->end > (int)cur->end)
    return 0;

  /* Locate cur's first def and require it to be a pure copy `cur <- partner`. */
  int def_pos = -1;
  for (int j = (int)cur->start; j < n; j++) {
    IRQuadCompact *q = &ir->compact_instructions[j];
    if (q->op == TCCIR_OP_NOP) continue;
    if (!irop_config[q->op].has_dest) continue;
    IROperand d = tcc_ir_op_get_dest(ir, q);
    if (!irop_has_vreg(d) || irop_get_vreg(d) != cur_vreg) continue;
    if (q->op != TCCIR_OP_ASSIGN) return 0;
    IROperand s = tcc_ir_op_get_src1(ir, q);
    if (!irop_has_vreg(s) || irop_get_vreg(s) != partner_vreg) return 0;
    def_pos = j;
    break;
  }
  if (def_pos < 0)
    return 0;

  /* DFS over instructions reachable from after the copy.  Each instruction is
   * marked visited *when pushed*, so it is enqueued at most once and the stack
   * never exceeds n entries.  If a back-edge re-enters def_pos itself, the
   * copy's own read of partner is seen and the case is conservatively rejected. */
  uint8_t *visited = tcc_mallocz(n);
  int *stack = tcc_malloc(sizeof(int) * n);
  int sp = 0, dead = 1;
#define RA_EXITPHI_PUSH(x) do { int _x = (x); \
    if (_x >= 0 && _x < n && !visited[_x]) { visited[_x] = 1; stack[sp++] = _x; } } while (0)
  RA_EXITPHI_PUSH(def_pos + 1);

  while (sp > 0) {
    int i = stack[--sp];
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP) {
      RA_EXITPHI_PUSH(i + 1);
      continue;
    }
    if (ra_instr_touches_vreg(ir, q, partner_vreg)) { dead = 0; break; }

    if (q->op == TCCIR_OP_RETURNVOID || q->op == TCCIR_OP_RETURNVALUE ||
        q->op == TCCIR_OP_TRAP)
      continue; /* no successor */
    if (q->op == TCCIR_OP_IJUMP || q->op == TCCIR_OP_SWITCH_TABLE ||
        q->op == TCCIR_OP_SWITCH_LOAD) { dead = 0; break; } /* unknown targets */
    if (q->op == TCCIR_OP_JUMP) {
      RA_EXITPHI_PUSH((int)irop_get_imm64_ex(ir, tcc_ir_op_get_dest(ir, q)));
      continue;
    }
    if (q->op == TCCIR_OP_JUMPIF) {
      RA_EXITPHI_PUSH((int)irop_get_imm64_ex(ir, tcc_ir_op_get_dest(ir, q)));
      RA_EXITPHI_PUSH(i + 1);
      continue;
    }
    /* Ordinary instruction (including calls — keep the fall-through; a
     * noreturn call's dead fall-through only makes the check more
     * conservative). */
    RA_EXITPHI_PUSH(i + 1);
  }
#undef RA_EXITPHI_PUSH

  /* The reachable set is now complete in `visited` (the loop only breaks early
   * when dead is already 0).  cur may legitimately take partner's register
   * only if every definition of cur is one of:
   *   (a) the copy itself (def_pos), or
   *   (b) reachable from the copy — cur is the forward continuation of partner
   *       (e.g. the loop increment), or
   *   (c) another pure copy `cur <- partner` from the SAME partner — a parallel
   *       SSA phi copy on a different incoming edge of the same merge; it also
   *       collapses to mov R,R once cur and partner share R, so it is harmless.
   * A def from a DIFFERENT source on a sibling path (a true merge phi, e.g. a
   * return-value phi `ret <- a` / `ret <- b`) makes cur hold a value unrelated
   * to partner there; coalescing cur into partner's register would corrupt it. */
  if (dead) {
    for (int j = 0; j < n; j++) {
      if (j == def_pos) continue;
      IRQuadCompact *q = &ir->compact_instructions[j];
      if (q->op == TCCIR_OP_NOP || !irop_config[q->op].has_dest) continue;
      if (q->op == TCCIR_OP_STORE || q->op == TCCIR_OP_STORE_INDEXED ||
          q->op == TCCIR_OP_STORE_POSTINC) continue;
      IROperand d = tcc_ir_op_get_dest(ir, q);
      if (!irop_has_vreg(d) || irop_get_vreg(d) != cur_vreg) continue;
      if (visited[j]) continue; /* (b) forward continuation */
      /* (c) a parallel `cur <- partner` copy is fine; anything else rejects. */
      int parallel_copy = 0;
      if (q->op == TCCIR_OP_ASSIGN) {
        IROperand s = tcc_ir_op_get_src1(ir, q);
        if (irop_has_vreg(s) && irop_get_vreg(s) == partner_vreg)
          parallel_copy = 1;
      }
      if (!parallel_copy) { dead = 0; break; }
    }
  }

  tcc_free(visited);
  tcc_free(stack);
  return dead;
}

static void ra_linear_scan(TCCIRState *ir, SSAInterval *intervals, int count,
                           const RegAllocTarget *target, int spill_base,
                           uint64_t *out_dirty_int, uint64_t *out_dirty_fp,
                           int max_vreg_pos, int has_call)
{
  if (count <= 0) return;

  qsort(intervals, count, sizeof(SSAInterval), sort_by_start);

  /* Build vreg -> interval lookup for phi hint resolution */
  int hint_tbl_size = (max_vreg_pos > 0) ? 4 * max_vreg_pos : 1;
  SSAInterval **vreg_to_iv = tcc_mallocz(sizeof(SSAInterval *) * hint_tbl_size);
  #define HINT_IDX(vr) \
    ((TCCIR_DECODE_VREG_TYPE(vr) * max_vreg_pos) + TCCIR_DECODE_VREG_POSITION(vr))
  for (int i = 0; i < count; i++) {
    int idx = HINT_IDX(intervals[i].vreg);
    if (idx >= 0 && idx < hint_tbl_size)
      vreg_to_iv[idx] = &intervals[i];
  }

  /* Register availability bitmaps */
  uint64_t int_avail = 0;
  for (int i = 0; i < target->int_class.num_caller_saved; i++)
    int_avail |= (1ull << target->int_class.caller_saved[i]);
  for (int i = 0; i < target->int_class.num_callee_saved; i++)
    int_avail |= (1ull << target->int_class.callee_saved[i]);

  uint64_t fp_avail = 0;
  for (int i = 0; i < target->fp_class.num_caller_saved; i++)
    fp_avail |= (1ull << target->fp_class.caller_saved[i]);
  for (int i = 0; i < target->fp_class.num_callee_saved; i++)
    fp_avail |= (1ull << target->fp_class.callee_saved[i]);

  /* Respect tcc_state->registers_for_allocator limit */
  uint64_t int_allowed = int_avail & tcc_state->registers_map_for_allocator;
  uint64_t fp_allowed = fp_avail & tcc_state->float_registers_map_for_allocator;

  /* Nested-function trampolines load the static chain into the static-chain
   * register (R10) and tail-jump to the nested function, clobbering R10
   * without restoring it — even though R10 is AAPCS callee-saved.  A parent
   * that defines nested functions calls them (directly, or via a function
   * pointer through an ABI-compliant helper) and would see any value the
   * allocator parked in R10 corrupted across that call (nestfunc-2: the
   * i/j/k loop counters lived in R10 and the inner foo() call clobbered it →
   * infinite loop).  Reserve R10 in such parents.  Nested functions
   * themselves (has_static_chain) hold the live chain in R10 and are handled
   * separately, so leave their allocation untouched. */
  if (tcc_state->nb_nested_funcs > 0 && !ir->has_static_chain)
    int_allowed &= ~(1ull << (uint64_t)architecture_config.static_chain_reg);

  uint64_t int_free = int_allowed;
  uint64_t fp_free = fp_allowed;
  uint64_t dirty_int = 0;
  uint64_t dirty_fp = 0;

  /* Active set sorted by end point */
  SSAInterval **active = tcc_malloc(sizeof(SSAInterval *) * count);
  int active_count = 0;

  /* Active addrtaken intervals — tracked separately because the main `active`
   * set is for register-resident intervals; addrtaken intervals always spill
   * and were previously dropped on the floor.  We track them so their stack
   * slots can be returned to a free list once the interval ends. */
  SSAInterval **active_addrtaken = tcc_malloc(sizeof(SSAInterval *) * count);
  int active_addrtaken_count = 0;
  uint32_t active_addrtaken_min_end = UINT32_MAX;

  /* Free list of expired 4-byte addrtaken stack slots, available for reuse
   * by later addrtaken intervals.  Only 4-byte slots are tracked here; other
   * sizes fall through to fresh allocation, matching the legacy behavior of
   * always assigning `spill_loc -= 4` regardless of value width. */
  int *free_slots_4 = tcc_malloc(sizeof(int) * count);
  int free_slots_4_count = 0;

  int spill_loc = spill_base;

  for (int i = 0; i < count; i++) {
    SSAInterval *cur = &intervals[i];

    /* Graph coalescing: non-representative members are merged into their
     * representative's interval and inherit its register after the scan.  Skip
     * them so they neither consume a register nor enter the active set. */
    if (cur->coalesce_to >= 0)
      continue;

    /* Expire old intervals.
     *
     * First compute which hard registers the SURVIVING (still-live) intervals
     * still occupy.  A register may be held by two active intervals at once
     * when a phi/merge copy coalesced them onto the same register: e.g. an
     * if/else merge temp for a variable inherits the register from one arm
     * (via its single hint_vreg) while the other arm's interval is still live
     * and also holds that register.  If the shorter of the two expires first,
     * returning its register to the free pool would let a later allocation
     * (a loop-body temp, say) clobber the value the longer, still-live
     * interval needs after the loop.  Only free a register when no surviving
     * interval still holds it. */
    uint64_t survivor_int = 0, survivor_fp = 0;
    for (int j = 0; j < active_count; j++) {
      SSAInterval *a = active[j];
      if (a->end >= cur->start && a->r0 >= 0 && a->stack_location == 0) {
        if (a->reg_type == LS_REG_TYPE_FLOAT || a->reg_type == LS_REG_TYPE_DOUBLE) {
          survivor_fp |= (1ull << a->r0);
          if (a->r1 >= 0) survivor_fp |= (1ull << a->r1);
        } else {
          survivor_int |= (1ull << a->r0);
          if (a->r1 >= 0) survivor_int |= (1ull << a->r1);
        }
      }
    }
    int w = 0;
    for (int j = 0; j < active_count; j++) {
      SSAInterval *a = active[j];
      if (a->end < cur->start) {
        /* Free register — but not if cur shared hr with another active
         * interval (reg_shared): the partner still logically owns hr,
         * so freeing it here would let a later allocation clobber the
         * loop body's view of partner.  Likewise skip any register a
         * surviving active interval still holds (coalesced sharing). */
        if (a->r0 >= 0 && a->stack_location == 0 && !a->reg_shared) {
          if (a->reg_type == LS_REG_TYPE_FLOAT || a->reg_type == LS_REG_TYPE_DOUBLE) {
            if (!(survivor_fp & (1ull << a->r0))) fp_free |= (1ull << a->r0);
            if (a->r1 >= 0 && !(survivor_fp & (1ull << a->r1))) fp_free |= (1ull << a->r1);
          } else {
            if (!(survivor_int & (1ull << a->r0))) int_free |= (1ull << a->r0);
            if (a->r1 >= 0 && !(survivor_int & (1ull << a->r1))) int_free |= (1ull << a->r1);
          }
        }
      } else {
        active[w++] = a;
      }
    }
    active_count = w;

    /* Expire old addrtaken intervals — return their 4-byte slots to the
     * free list so later non-overlapping addrtaken intervals can reuse them.
     * Skip the scan when no active entry can expire (min_end >= cur->start). */
    if (active_addrtaken_count > 0 && active_addrtaken_min_end < cur->start) {
      int wa = 0;
      active_addrtaken_min_end = UINT32_MAX;
      for (int j = 0; j < active_addrtaken_count; j++) {
        SSAInterval *a = active_addrtaken[j];
        if (a->end < cur->start) {
          free_slots_4[free_slots_4_count++] = a->stack_location;
        } else {
          active_addrtaken[wa++] = a;
          if (a->end < active_addrtaken_min_end)
            active_addrtaken_min_end = a->end;
        }
      }
      active_addrtaken_count = wa;
    }

    /* Address-taken: force spill.
     * Reuse an expired addrtaken slot when one is available; otherwise grow
     * the spill area.  Slot reuse is correct here because the addrtaken
     * extension pass in ra_build_intervals has already pushed V's end past
     * the death of any pointer derived from V — so two intervals with
     * non-overlapping (extended) lifetimes truly access disjoint memory
     * windows.  Track in active_addrtaken so the slot returns to the free
     * list when cur expires. */
    if (cur->addrtaken) {
      if (free_slots_4_count > 0) {
        cur->stack_location = free_slots_4[--free_slots_4_count];
      } else {
        spill_loc -= 4;
        cur->stack_location = spill_loc;
      }
      active_addrtaken[active_addrtaken_count++] = cur;
      if (cur->end < active_addrtaken_min_end)
        active_addrtaken_min_end = cur->end;
      continue;
    }

    /* Precolored: assign fixed register */
    if (cur->precolored >= 0) {
      int reg = cur->precolored;
      cur->r0 = reg;
      int_free &= ~(1ull << reg);
      dirty_int |= (1ull << reg);
      /* Insert into active set */
      active[active_count++] = cur;
      continue;
    }

    /* Float/double: use FP class */
    if (cur->reg_type == LS_REG_TYPE_FLOAT) {
      int reg = -1;
      if (fp_free) {
        reg = __builtin_ctzll(fp_free);
        fp_free &= ~(1ull << reg);
        dirty_fp |= (1ull << reg);
      }
      if (reg >= 0) {
        cur->r0 = LS_VFP_REG_BASE + reg;
        active[active_count++] = cur;
      } else {
        spill_loc -= 4;
        cur->stack_location = spill_loc;
      }
      continue;
    }

    if (cur->reg_type == LS_REG_TYPE_DOUBLE) {
      /* Need even-aligned pair */
      int reg = -1;
      for (int r = 0; r < 30; r += 2) {
        if ((fp_free & (3ull << r)) == (3ull << r)) {
          reg = r;
          break;
        }
      }
      if (reg >= 0) {
        fp_free &= ~(3ull << reg);
        dirty_fp |= (3ull << reg);
        cur->r0 = LS_VFP_REG_BASE + reg;
        cur->r1 = LS_VFP_REG_BASE + reg + 1;
        active[active_count++] = cur;
      } else {
        spill_loc -= 8;
        cur->stack_location = spill_loc;
      }
      continue;
    }

    /* 64-bit integer or soft-float double: need register pair */
    if (cur->reg_type == LS_REG_TYPE_LLONG || cur->reg_type == LS_REG_TYPE_DOUBLE_SOFT ||
        cur->reg_type == LS_REG_TYPE_COMPLEX_FLOAT) {
      int r0 = -1, r1 = -1;

      /* Return-pair preference for 64-bit call results.
       *
       * A call returns its 64-bit result in r0:r1.  When that result does not
       * cross another call (it is consumed before the next call — e.g. it
       * feeds that call's first argument), keeping it in r0:r1 avoids a move
       * from the return pair to a callee-saved/other pair.  This is the
       * dominant cost in chained soft-float expressions: in pr58574's Horner
       * polynomials each __aeabi_dmul / __aeabi_dadd result feeds the next
       * helper call, and parking the result anywhere but r0:r1 forces a dead
       * mov of the return value to its home pair.
       *
       * Boundary reuse: the pair may still be held by the just-returned call's
       * last argument(s), whose intervals end exactly at this call.  Those
       * values were copied into their AAPCS argument registers before the call
       * and are dead afterwards (the call clobbered r0:r1 with the result), so
       * the pair is free to take.  Evict any such boundary occupant — the same
       * idea as the single-reg pref_reg boundary eviction below. */
      if (!cur->crosses_call && (int)cur->start < ir->next_instruction_index &&
          ir->compact_instructions[cur->start].op == TCCIR_OP_FUNCCALLVAL &&
          1 < tcc_state->registers_for_allocator) {
        int want0 = 0, want1 = 1; /* r0:r1 */
        int avail0 = (int_free & (1ull << want0)) != 0;
        int avail1 = (int_free & (1ull << want1)) != 0;
        int evict0 = -1, evict1 = -1;
        for (int k = 0; k < active_count && (!avail0 || !avail1); k++) {
          SSAInterval *a = active[k];
          if (a->stack_location != 0 || a->end != cur->start)
            continue;
          if (!avail0 && (a->r0 == want0 || a->r1 == want0)) { evict0 = k; avail0 = 1; }
          if (!avail1 && (a->r0 == want1 || a->r1 == want1)) { evict1 = k; avail1 = 1; }
        }
        if (avail0 && avail1) {
          int idxs[2], ni = 0;
          if (evict0 >= 0) idxs[ni++] = evict0;
          if (evict1 >= 0 && evict1 != evict0) idxs[ni++] = evict1;
          if (ni == 2 && idxs[0] < idxs[1]) { int t = idxs[0]; idxs[0] = idxs[1]; idxs[1] = t; }
          for (int e = 0; e < ni; e++) {
            SSAInterval *a = active[idxs[e]];
            int_free |= (1ull << a->r0);
            if (a->r1 >= 0) int_free |= (1ull << a->r1);
            active[idxs[e]] = active[--active_count];
          }
          r0 = want0; r1 = want1;
        }
      }

      if (r0 < 0 && cur->crosses_call && target->int_class.pair_align) {
        /* Prefer callee-saved even-aligned pairs (R4:R5, R6:R7, R8:R9, R10:R11) */
        for (int r = 4; r < 12; r += 2) {
          if ((int_free & (3ull << r)) == (3ull << r)) {
            r0 = r; r1 = r + 1;
            break;
          }
        }
      }
      if (r0 < 0 && !cur->crosses_call && target->int_class.pair_align) {
        /* Try any even-aligned pair (only for non-call-crossing) */
        for (int r = 0; r < 12; r += 2) {
          if ((int_free & (3ull << r)) == (3ull << r)) {
            r0 = r; r1 = r + 1;
            break;
          }
        }
      }
      if (r0 < 0 && !cur->crosses_call) {
        /* Fallback: any two registers (only for non-call-crossing) */
        int first = -1;
        for (int r = 0; r < 13; r++) {
          if (int_free & (1ull << r)) {
            if (first < 0) first = r;
            else { r0 = first; r1 = r; break; }
          }
        }
      }
      if (r0 < 0 && cur->crosses_call && cur->reg_type == LS_REG_TYPE_LLONG) {
        /* Fallback: any two callee-saved registers (need not be an aligned
         * pair).  Thumb-2 LDRD/STRD accept any two distinct r0..r12/r14
         * (try_ldrd_pair / try_strd_pair in arm-thumb-gen.c only enforce
         * lo != hi and Rn != SP/PC).  Other 64-bit ops (cmp+sbcs, adds+adcs,
         * load/store as two 32-bit halves) likewise have no pair-alignment
         * requirement.  Without this fallback, a call-crossing 64-bit value
         * spills whenever the small set of aligned callee-saved pairs is
         * busy — e.g. when R7 is excluded as Thumb FP, R4:R5/R6:R7/R8:R9/
         * R10:R11 quickly collide with the loop IV, array base, and another
         * live 64-bit value.
         *
         * If two free callee-saved are not immediately available, evict up to
         * two single-INT victims (lowest-use, end > cur->end, currently in a
         * callee-saved reg) to free the pair.  Mirrors the eviction policy
         * used for single-INT spill below, just iterated to fill both slots.
         * Bail out if a candidate victim has use_count >= cur->use_count to
         * avoid spilling a hotter interval.
         *
         * Restricted to LS_REG_TYPE_LLONG to avoid evicting INT base pointers
         * for soft-double / complex-float pairs, where the cost/benefit shifts
         * (softfloat helpers reload args anyway, and complex temporaries are
         * often short-lived).  Enabling it for those types regressed
         * gcc-execute/20021120-1::foo by 200+ insns in benchmarks. */
        for (int round = 0; r0 < 0 && round < 3; round++) {
          int first = -1;
          for (int ci = 0; ci < target->int_class.num_callee_saved; ci++) {
            int r = target->int_class.callee_saved[ci];
            if (int_free & (1ull << r)) {
              if (first < 0) first = r;
              else { r0 = first; r1 = r; break; }
            }
          }
          if (r0 >= 0) break;
          SSAInterval *victim = NULL;
          int victim_idx = -1;
          uint16_t victim_uses = UINT16_MAX;
          for (int j = 0; j < active_count; j++) {
            SSAInterval *a = active[j];
            if (a->precolored >= 0) continue;
            if (a->reg_type != LS_REG_TYPE_INT) continue;
            if (a->end <= cur->end) continue;
            if (a->r0 < 0 || a->stack_location != 0) continue;
            /* Never evict a loop-phi-locked interval: it holds a loop-carried
             * value whose register is SHARED with a coalesce partner that stays
             * live (see the loop-phi coalescing above and the identical guard in
             * the single-register spill victim scan below).  Spilling it here
             * frees a register the partner still occupies, so a later pair
             * allocation would double-book it and clobber the loop-carried value
             * (combo_num seed 84127: the g16 loop counter lost to a 64-bit OR's
             * high half). */
            if (a->loop_phi_locked) continue;
            int is_callee = 0;
            for (int ci = 0; ci < target->int_class.num_callee_saved; ci++) {
              if (target->int_class.callee_saved[ci] == a->r0) { is_callee = 1; break; }
            }
            if (!is_callee) continue;
            if (a->use_count < victim_uses) {
              victim_uses = a->use_count;
              victim = a;
              victim_idx = j;
            }
          }
          if (!victim || victim_uses >= cur->use_count) break;
          int_free |= (1ull << victim->r0);
          spill_loc -= 4;
          victim->stack_location = spill_loc;
          victim->r0 = -1;
          active[victim_idx] = active[--active_count];
        }
      }
      if (r0 >= 0) {
        int_free &= ~((1ull << r0) | (1ull << r1));
        dirty_int |= ((1ull << r0) | (1ull << r1));
        cur->r0 = r0;
        cur->r1 = r1;
        active[active_count++] = cur;
      } else {
        spill_loc -= 8;
        cur->stack_location = spill_loc;
      }
      continue;
    }

    /* Complex double: always spill (128-bit) */
    if (cur->reg_type == LS_REG_TYPE_COMPLEX_DOUBLE) {
      spill_loc -= 16;
      cur->stack_location = spill_loc;
      continue;
    }

    /* Integer: single register */
    int reg = -1;
    RA_DBG("  alloc T%d [%u,%u] xcall=%d narrow=%u int_free=0x%llx active=%d",
           TCCIR_DECODE_VREG_POSITION(cur->vreg), cur->start, cur->end,
           cur->crosses_call, cur->narrow_uses, (unsigned long long)int_free, active_count);

    /* Phi-coalescing: try the hinted partner's register first.
     * ra_build_phi_hints set hint_vreg to a vreg this interval used to
     * share a phi edge with (now resolved into an explicit ASSIGN copy).
     * If the partner has expired and freed its register, taking that
     * same register here turns the explicit copy into mov rX, rX which
     * the post-RA move-coalescing pass erases.
     *
     * Boundary case: when partner->end == cur->start, the standard `<`
     * expiration kept partner active, so its register looks busy here.
     * But within a single ARM 3-operand instruction (or ASSIGN/`mov`),
     * sources are read before the dest is written — so the same register
     * can serve both. Allow the hint to take that register and force
     * partner out of the active set so subsequent allocations see it as
     * free. Restricted to single-register INT to avoid corrupting register
     * pairs (umull, ll-shift, etc.) where the architecture forbids dest/
     * source overlap. */
    if (cur->hint_vreg >= 0 && max_vreg_pos > 0 && !cur->co_member) {
      int hint_idx = HINT_IDX(cur->hint_vreg);
      if (hint_idx >= 0 && hint_idx < hint_tbl_size) {
        SSAInterval *partner = vreg_to_iv[hint_idx];
        if (partner && !partner->co_member && partner->r0 >= 0 && partner->r1 < 0 &&
            partner->stack_location == 0) {
          int hr = partner->r0;
          int hr_free = (int_free & (1ull << hr)) != 0;
          int boundary = !hr_free && partner->end == cur->start &&
                         cur->reg_type == LS_REG_TYPE_INT &&
                         partner->reg_type == LS_REG_TYPE_INT;
          if ((hr_free || boundary) &&
              hr < tcc_state->registers_for_allocator) {
            int ok = 1;
            if (cur->crosses_call) {
              ok = 0;
              for (int ci = 0; ci < target->int_class.num_callee_saved; ci++) {
                if (target->int_class.callee_saved[ci] == hr) { ok = 1; break; }
              }
            }
            if (ok) {
              reg = hr;
              if (boundary) {
                /* Force partner out of active so its register doesn't
                 * appear taken to subsequent intervals. */
                for (int k = 0; k < active_count; k++) {
                  if (active[k] == partner) {
                    int_free |= (1ull << hr);
                    active[k] = active[--active_count];
                    break;
                  }
                }
              }
            }
          }
        }
      }
    }

    /* Loop-carried phi coalescing.
     *
     * The standard boundary case above fires when partner expires exactly
     * at cur->start.  A loop-carried phi (sum_array's `sum + arr[i]`,
     * for example) extends the partner's live interval to the back-edge
     * JMP — past cur's def — so partner's register looks taken at the
     * point we want to coalesce.  If cur's defining instruction reads
     * partner and the only remaining use of partner before partner->end
     * is an ASSIGN `partner <-- cur` (the back-edge copy), the two share
     * a value from cur->start onward and can share the register.
     *
     * Transfer ownership of R from partner to cur in the active set:
     * extend cur->end to cover partner->end, remove partner from active,
     * and let cur represent R there.  Both vregs keep r0 = R so codegen
     * resolves either name to the same register, and the back-edge
     * ASSIGN becomes mov R, R which post-RA cleanup elides.  Without
     * this transfer R would be double-counted and the expire phase would
     * free it as soon as partner's original end lapses while cur is still
     * alive in R. */
    int coalesced_loop_phi = 0;
    if (reg < 0 && cur->hint_vreg >= 0 && max_vreg_pos > 0 && !cur->co_member &&
        cur->reg_type == LS_REG_TYPE_INT && tcc_state->optimize >= 1) {
      int hint_idx = HINT_IDX(cur->hint_vreg);
      if (hint_idx >= 0 && hint_idx < hint_tbl_size) {
        SSAInterval *partner = vreg_to_iv[hint_idx];
        if (partner && !partner->co_member && partner->r0 >= 0 && partner->r1 < 0 &&
            partner->stack_location == 0 &&
            partner->reg_type == LS_REG_TYPE_INT &&
            (int)partner->end > (int)cur->start) {
          int hr = partner->r0;
          int partner_active_idx = -1;
          for (int k = 0; k < active_count; k++) {
            if (active[k] == partner) { partner_active_idx = k; break; }
          }
          if (partner_active_idx >= 0 && !(int_free & (1ull << hr)) &&
              hr < tcc_state->registers_for_allocator) {
            int ok = 1;
            if (cur->crosses_call) {
              ok = 0;
              for (int ci = 0; ci < target->int_class.num_callee_saved; ci++) {
                if (target->int_class.callee_saved[ci] == hr) { ok = 1; break; }
              }
            }
            if (ok && (ra_safe_loop_phi_coalesce(ir, cur, partner) ||
                       ra_safe_exit_phi_coalesce(ir, cur, partner))) {
              reg = hr;
              coalesced_loop_phi = 1;
              if (partner->end > cur->end)
                cur->end = partner->end;
              cur->r0 = reg;
              /* cur now carries partner's loop-carried value over the extended
               * range; the partner is gone from active, so cur is the sole
               * holder of hr that the partner's remaining uses depend on.
               * Evicting cur mid-loop would spill it without reloading those
               * partner uses → IV corruption.  Lock it against eviction. */
              cur->loop_phi_locked = 1;
              active[partner_active_idx] = active[--active_count];
            }
          }
        }
      }
    }

    /* Soft preference: try iv->pref_reg first (e.g., r0 for vregs that
     * feed RETURNVALUE).  If free, take it.  If a still-active interval
     * ends exactly at cur->start and holds that register, evict it and
     * take it (boundary case — same logic as the phi-coalescing hint). */
    if (reg < 0 && cur->pref_reg >= 0 && cur->reg_type == LS_REG_TYPE_INT) {
      int hr = (int)cur->pref_reg;
      if (hr < tcc_state->registers_for_allocator) {
        int hr_free = (int_free & (1ull << hr)) != 0;
        int ok = 1;
        if (cur->crosses_call) {
          ok = 0;
          for (int ci = 0; ci < target->int_class.num_callee_saved; ci++) {
            if (target->int_class.callee_saved[ci] == hr) { ok = 1; break; }
          }
        }
        if (ok) {
          if (hr_free) {
            reg = hr;
          } else {
            /* Boundary: an active INT interval ending at cur->start in hr. */
            for (int k = 0; k < active_count; k++) {
              SSAInterval *a = active[k];
              if (a->r0 == hr && a->r1 < 0 && a->end == cur->start &&
                  a->stack_location == 0 && a->reg_type == LS_REG_TYPE_INT) {
                int_free |= (1ull << hr);
                active[k] = active[--active_count];
                reg = hr;
                break;
              }
            }
          }
          /* Return-block register sharing: cur is a single-use RETURNVALUE
           * feeder whose def at cur->start and consumer at cur->end form
           * a return tail.  Control returns to the caller at cur->end, so
           * we can SHARE hr with whatever interval `partner` is holding
           * it — cur's def overwrites hr, but we never re-execute past
           * the return on this control-flow path.
           *
           * Safety conditions:
           *   1. cur->end is RETURNVALUE/RETURNVOID.
           *   2. partner's vreg is not READ at any instruction in
           *      [cur->start, cur->end] — cur's def would clobber it.
           *
           * Sharing model: cur->r0 = hr, but we do NOT remove partner
           * from active, do NOT mark hr free in int_free, and set
           * cur->reg_shared so cur's expire phase does not free hr
           * (partner still logically owns it).  Other CFG paths through
           * partner's live range emit their own reads of hr unaffected —
           * those paths never execute cur's def, so partner's value
           * remains intact in hr along them. */
          if (reg < 0 && cur->end > cur->start &&
              (int)cur->end < ir->next_instruction_index) {
            IRQuadCompact *eq = &ir->compact_instructions[cur->end];
            if (eq->op == TCCIR_OP_RETURNVALUE || eq->op == TCCIR_OP_RETURNVOID) {
              for (int k = 0; k < active_count; k++) {
                SSAInterval *a = active[k];
                if (a->r0 != hr || a->r1 >= 0 || a->stack_location != 0 ||
                    a->reg_type != LS_REG_TYPE_INT)
                  continue;
                int conflict = 0;
                for (int p = (int)cur->start; p <= (int)cur->end && !conflict; p++) {
                  IRQuadCompact *pq = &ir->compact_instructions[p];
                  /* Any operand reference to the partner clobbers the share:
                   * cur's def at cur->start overwrites hr, so partner must not be
                   * needed anywhere in the range.  Use ra_instr_touches_vreg so a
                   * STORE-class op's dest (its base *pointer*, which the store
                   * READS) and an MLA accumulator count — a naive src1/src2 scan
                   * missed a partner used as a store base and shared hr anyway,
                   * emitting `str rX, [rX]` (value written through itself). */
                  if (ra_instr_touches_vreg(ir, pq, a->vreg)) { conflict = 1; break; }
                }
                if (!conflict) {
                  cur->r0 = hr;
                  cur->reg_shared = 1;
                  dirty_int |= (1ull << hr);
                  reg = hr;
                  break;
                }
              }
            }
          }
        }
      }
    }

    if (reg < 0 && cur->crosses_call) {
      /* Prefer callee-saved */
      for (int ci = 0; ci < target->int_class.num_callee_saved; ci++) {
        int r = target->int_class.callee_saved[ci];
        if (int_free & (1ull << r)) { reg = r; break; }
      }
    }
    if (reg < 0 && !cur->crosses_call &&
        (int)cur->start < ir->next_instruction_index) {
      IRQuadCompact *dq = &ir->compact_instructions[cur->start];
      if (dq->op == TCCIR_OP_MLA &&
          irop_get_vreg(tcc_ir_op_get_dest(ir, dq)) == cur->vreg) {
        IROperand accum = tcc_ir_op_get_accum(ir, dq);
        if (accum.is_sym) {
          static const int sym_mla_order[] = {10, 11, 8, 9};
          for (int oi = 0; oi < 4; oi++) {
            int r = sym_mla_order[oi];
            if (r < tcc_state->registers_for_allocator &&
                (int_free & (1ull << r))) {
              reg = r;
              break;
            }
          }
        }
      }
    }
    if (reg < 0 && !cur->crosses_call) {
      /* Narrow-dense intervals prefer r4-r7 over r12 for 16-bit encodings; gates in docs/regalloc_narrow_pref.md */
      static const int alloc_order[] = {0, 1, 2, 3, 12, 4, 5, 6, 7, 8, 9, 10, 11};
      static const int alloc_order_narrow[] = {0, 1, 2, 3, 4, 5, 6, 7, 12, 8, 9, 10, 11};
      int narrow_dense = cur->narrow_uses > 0 &&
                         (uint32_t)cur->narrow_uses * 8 >= (cur->end - cur->start);
      const int *order = narrow_dense ? alloc_order_narrow : alloc_order;
      for (int pass = 0; pass < 2 && reg < 0; pass++) {
        for (int oi = 0; oi < 13; oi++) {
          int r = order[oi];
          if (r >= tcc_state->registers_for_allocator) continue;
          if (!(int_free & (1ull << r))) continue;
          if (pass == 0 && order == alloc_order_narrow) {
            if (r >= 4 && r <= 7 && !(dirty_int & (1ull << r)) && !has_call &&
                cur->narrow_uses < 2)
              continue;
            if (r >= 8 && r != 12)
              continue;
          }
          reg = r;
          break;
        }
      }
    }

    if (cur->reg_shared) {
      /* Return-block share: cur->r0 was set in the pref_reg path.
       * Don't touch int_free (partner still owns hr) and don't add cur
       * to active (cur's expire would otherwise hit the !reg_shared
       * guard but adding it is just bookkeeping; the simpler invariant
       * is "shared cur never enters active"). */
    } else if (coalesced_loop_phi) {
      /* Partner was removed from active above; transfer R's ownership
       * to cur. int_free stays unchanged (R was taken via partner,
       * now taken via cur). */
      dirty_int |= (1ull << reg);
      active[active_count++] = cur;
    } else if (reg >= 0) {
      int_free &= ~(1ull << reg);
      dirty_int |= (1ull << reg);
      cur->r0 = reg;
      active[active_count++] = cur;
    } else {
      /* Spill: among intervals that extend past cur, evict the one
       * with the lowest spill cost (fewest loop-weighted uses).
       * use_count is already weighted by loop depth (4^depth per use),
       * so this prefers evicting intervals with few loop-hot uses. */
      SSAInterval *victim = NULL;
      int victim_idx = -1;
      uint16_t victim_uses = UINT16_MAX;
      for (int j = 0; j < active_count; j++) {
        SSAInterval *a = active[j];
        if (a->precolored >= 0) continue;
        if (a->reg_type != LS_REG_TYPE_INT) continue;
        if (a->end <= cur->end) continue;
        /* Never evict a loop-phi-locked interval: it holds a loop-carried value
         * (its absorbed partner's uses still read this register across the loop
         * body) and spilling it here would not reload those uses. */
        if (a->loop_phi_locked) continue;
        if (a->use_count < victim_uses ||
            (a->use_count == victim_uses && victim && a->end > victim->end)) {
          victim_uses = a->use_count;
          victim = a;
          victim_idx = j;
        }
      }
      if (victim) {
        /* Evict victim, give its register to cur */
        reg = victim->r0;
        victim->r0 = -1;
        spill_loc -= 4;
        victim->stack_location = spill_loc;
        /* Remove victim from active */
        active[victim_idx] = active[--active_count];
        cur->r0 = reg;
        active[active_count++] = cur;
      } else {
        spill_loc -= 4;
        cur->stack_location = spill_loc;
      }
    }
  }

  tcc_free(active);
  tcc_free(active_addrtaken);
  tcc_free(free_slots_4);
  tcc_free(vreg_to_iv);
  #undef HINT_IDX

  if (TCC_LOG_LS) {
    RA_DBG("SSA ra_linear_scan: allocation results (%d intervals)", count);
    for (int i = 0; i < count; i++) {
      SSAInterval *iv = &intervals[i];
      int type = TCCIR_DECODE_VREG_TYPE(iv->vreg);
      int pos = TCCIR_DECODE_VREG_POSITION(iv->vreg);
      if (iv->stack_location != 0) {
        RA_DBG("  %s%d [%u,%u] -> spill(%d)", ra_vreg_type_char(type), pos,
               iv->start, iv->end, iv->stack_location);
      } else if (iv->r1 >= 0) {
        RA_DBG("  %s%d [%u,%u] -> R%d:R%d", ra_vreg_type_char(type), pos,
               iv->start, iv->end, iv->r0, iv->r1);
      } else {
        RA_DBG("  %s%d [%u,%u] -> R%d", ra_vreg_type_char(type), pos,
               iv->start, iv->end, iv->r0);
      }
    }
    RA_DBG("  dirty_int=0x%llx dirty_fp=0x%llx", (unsigned long long)dirty_int, (unsigned long long)dirty_fp);
  }

  *out_dirty_int = dirty_int;
  *out_dirty_fp = dirty_fp;
}

/* ============================================================================
 * Write Results to IR
 * ============================================================================ */

static void ra_write_results(TCCIRState *ir, SSAInterval *intervals, int count)
{
  /* Clear LS intervals and repopulate from SSA results */
  tcc_ls_clear_live_intervals(&ir->ls);

  for (int i = 0; i < count; i++) {
    SSAInterval *iv = &intervals[i];
    tcc_ls_add_live_interval(&ir->ls, iv->vreg, iv->start, iv->end,
                             iv->crosses_call, iv->addrtaken, iv->reg_type,
                             0, iv->precolored);
    LSLiveInterval *lsi = &ir->ls.intervals[ir->ls.next_interval_index - 1];
    lsi->r0 = iv->r0;
    lsi->r1 = iv->r1;
    lsi->stack_location = iv->stack_location;
    lsi->co_member = iv->co_member;

    /* Also write to IRLiveInterval for codegen */
    tcc_ir_stack_reg_assign(ir, iv->vreg, iv->stack_location, iv->r0, iv->r1);
    IRLiveInterval *li = tcc_ir_vreg_live_interval(ir, iv->vreg);
    if (li) {
      li->start = iv->start;
      li->end = iv->end;
      li->crosses_call = iv->crosses_call;
    }
  }
}

/* ============================================================================
 * Phi Resolution
 * ============================================================================ */

typedef struct RAPhiCopy {
  int32_t dest_vreg;
  int32_t src_vreg;
  int btype;
  uint8_t emitted;
} RAPhiCopy;

typedef struct RAPhiCopyRecord {
  int32_t src_vreg;
  int new_instr_idx;
} RAPhiCopyRecord;

typedef struct RAPhiStats {
  int phi_operands;
  int parallel_requested;
  int parallel_emitted;
  int cycle_temporaries;
  int participant_spills;
  int32_t *participants;
  int participant_count;
  int participant_cap;
} RAPhiStats;

static void ra_phi_stats_add_participant(RAPhiStats *stats, int32_t vreg)
{
  if (!TCC_LOG_LS || !stats || vreg < 0)
    return;

  for (int i = 0; i < stats->participant_count; i++)
    if (stats->participants[i] == vreg)
      return;

  if (stats->participant_count >= stats->participant_cap) {
    int cap = stats->participant_cap ? stats->participant_cap * 2 : 16;
    stats->participants = tcc_realloc(stats->participants,
                                      cap * sizeof(*stats->participants));
    stats->participant_cap = cap;
  }
  stats->participants[stats->participant_count++] = vreg;
}

static int ra_phi_stats_has_participant(RAPhiStats *stats, int32_t vreg)
{
  if (!TCC_LOG_LS || !stats)
    return 0;
  for (int i = 0; i < stats->participant_count; i++)
    if (stats->participants[i] == vreg)
      return 1;
  return 0;
}

#define RA_MAX_PHI_COPY_RECORDS 512

static int ra_interval_reg_count(IRLiveInterval *li, int regs[2])
{
  int n = 0;
  if (!li || li->allocation.offset != 0)
    return 0;
  int r0 = li->allocation.r0;
  if ((r0 & PREG_SPILLED) == 0 && r0 != PREG_NONE && r0 != 0xffff)
    regs[n++] = r0;
  int r1 = li->allocation.r1;
  if ((r1 & PREG_SPILLED) == 0 && r1 != PREG_NONE && r1 != 0xffff)
    regs[n++] = r1;
  return n;
}

static int ra_interval_regs_overlap(IRLiveInterval *a, IRLiveInterval *b)
{
  int ar[2], br[2];
  int an = ra_interval_reg_count(a, ar);
  int bn = ra_interval_reg_count(b, br);
  for (int i = 0; i < an; i++)
    for (int j = 0; j < bn; j++)
      if (ar[i] == br[j])
        return 1;
  return 0;
}

static int ra_interval_locations_overlap(IRLiveInterval *a, IRLiveInterval *b)
{
  if (!a || !b)
    return 0;
  if (a->allocation.offset != 0 || b->allocation.offset != 0)
    return a->allocation.offset != 0 && a->allocation.offset == b->allocation.offset;
  return ra_interval_regs_overlap(a, b);
}

/* Set to 1 while running ra_resolve_phis BEFORE register allocation, where
 * no allocation info exists. In that mode, copy-elision (identity check)
 * and physical-register clobber detection must fall back to vreg-level
 * reasoning. */
static int ra_phi_resolve_pre_ra_mode = 0;

/* Post-allocation dead register-copy elimination.
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

static int ra_phi_copy_is_identity(IRLiveInterval *dest_li, IRLiveInterval *src_li)
{
  if (!dest_li || !src_li)
    return 0;
  /* Pre-RA: never collapse; allocation may still place them in the same
   * register, in which case the post-RA move-coalescing pass will erase
   * the redundant copy. */
  if (ra_phi_resolve_pre_ra_mode)
    return 0;
  if (dest_li->allocation.offset != 0 || src_li->allocation.offset != 0)
    return dest_li->allocation.offset == src_li->allocation.offset;
  return dest_li->allocation.r0 == src_li->allocation.r0 &&
         dest_li->allocation.r1 == src_li->allocation.r1;
}

static int ra_phi_copy_dest_clobbers_pending_source(TCCIRState *ir, RAPhiCopy *copies,
                                                    int copy_count, int copy_idx)
{
  /* Pre-RA: clobber means "copy_idx writes a vreg that another pending
   * copy still needs to read". Compare by vreg, not by physical
   * register (which doesn't exist yet). */
  if (ra_phi_resolve_pre_ra_mode) {
    int32_t dest_vr = copies[copy_idx].dest_vreg;
    for (int i = 0; i < copy_count; i++) {
      if (i == copy_idx || copies[i].emitted)
        continue;
      if (copies[i].src_vreg == dest_vr)
        return 1;
    }
    return 0;
  }

  IRLiveInterval *dest_li = tcc_ir_vreg_live_interval(ir, copies[copy_idx].dest_vreg);
  if (!dest_li)
    return 0;

  for (int i = 0; i < copy_count; i++) {
    if (i == copy_idx || copies[i].emitted)
      continue;
    if (!tcc_ir_vreg_is_valid(ir, copies[i].src_vreg))
      continue;
    IRLiveInterval *src_li = tcc_ir_vreg_live_interval(ir, copies[i].src_vreg);
    if (ra_interval_locations_overlap(dest_li, src_li))
      return 1;
  }
  return 0;
}

static int ra_invert_cond(int tok)
{
  switch (tok) {
  case TOK_ULT: return TOK_UGE;
  case TOK_UGE: return TOK_ULT;
  case TOK_EQ: return TOK_NE;
  case TOK_NE: return TOK_EQ;
  case TOK_ULE: return TOK_UGT;
  case TOK_UGT: return TOK_ULE;
  case TOK_LT: return TOK_GE;
  case TOK_GE: return TOK_LT;
  case TOK_LE: return TOK_GT;
  case TOK_GT: return TOK_LE;
  default: return tok ^ 1;
  }
}

static int ra_phi_copy_needed(TCCIRState *ir, IRPhiNode *phi, int operand_idx)
{
  IRLiveInterval *dest_li = tcc_ir_vreg_live_interval(ir, phi->dest_vreg);
  if (!dest_li)
    return 0;
  /* Pre-RA: emit a copy for every operand. Post-RA coalescing will drop
   * any that turn out to land in the same physical register.  Copies into a
   * phi destination that is never read are NOT suppressed here: doing so
   * pre-RA changes register pressure and perturbs the whole allocation.  They
   * are stripped after allocation instead — see ra_eliminate_dead_reg_copies. */
  if (ra_phi_resolve_pre_ra_mode) {
    if (!tcc_ir_vreg_is_valid(ir, phi->operands[operand_idx].vreg))
      return 0;
    return 1;
  }
  if (dest_li->allocation.r0 == PREG_NONE && dest_li->allocation.offset == 0)
    return 0;
  IRLiveInterval *src_li = NULL;
  if (tcc_ir_vreg_is_valid(ir, phi->operands[operand_idx].vreg))
    src_li = tcc_ir_vreg_live_interval(ir, phi->operands[operand_idx].vreg);
  if (ra_phi_copy_is_identity(dest_li, src_li)) {
    if (src_li)
      src_li->phi_pinned = 1;
    return 0;
  }
  return 1;
}

static int ra_count_phi_copies_for_pred(TCCIRState *ir, IRCFG *cfg, IRSSAState *ssa,
                                        int pred_block, int succ_filter)
{
  int count = 0;
  for (int sb = 0; sb < cfg->num_blocks; sb++) {
    if (succ_filter >= 0 && sb != succ_filter)
      continue;
    for (IRPhiNode *phi = ssa->block_phis[sb]; phi; phi = phi->next) {
      for (int pi = 0; pi < phi->num_operands; pi++) {
        if (phi->operands[pi].pred_block != pred_block || phi->operands[pi].vreg < 0)
          continue;
        if (ra_phi_copy_needed(ir, phi, pi))
          count++;
      }
    }
  }
  return count;
}

static int ra_collect_phi_copies_for_pred(TCCIRState *ir, IRCFG *cfg, IRSSAState *ssa,
                                          int pred_block, int succ_filter,
                                          RAPhiCopy *copies)
{
  int copy_count = 0;
  for (int sb = 0; sb < cfg->num_blocks; sb++) {
    if (succ_filter >= 0 && sb != succ_filter)
      continue;
    for (IRPhiNode *phi = ssa->block_phis[sb]; phi; phi = phi->next) {
      for (int pi = 0; pi < phi->num_operands; pi++) {
        if (phi->operands[pi].pred_block != pred_block || phi->operands[pi].vreg < 0)
          continue;
        if (!ra_phi_copy_needed(ir, phi, pi))
          continue;
        copies[copy_count].dest_vreg = phi->dest_vreg;
        copies[copy_count].src_vreg = phi->operands[pi].vreg;
        copies[copy_count].btype = phi->btype;
        copies[copy_count].emitted = 0;
        copy_count++;
      }
    }
  }
  return copy_count;
}

static void ra_emit_phi_copy(TCCIRState *ir, IRQuadCompact *new_instrs, int *wp,
                             int *pool_wp, const RAPhiCopy *copy,
                             RAPhiCopyRecord *records, int *record_count)
{
  IROperand dest_op;
  memset(&dest_op, 0, sizeof(dest_op));
  irop_set_vreg(&dest_op, copy->dest_vreg);
  dest_op.tag = IROP_TAG_VREG;
  dest_op.btype = copy->btype;

  IROperand src_op;
  memset(&src_op, 0, sizeof(src_op));
  irop_set_vreg(&src_op, copy->src_vreg);
  src_op.tag = IROP_TAG_VREG;
  src_op.btype = copy->btype;

  ir->iroperand_pool[*pool_wp] = dest_op;
  ir->iroperand_pool[*pool_wp + 1] = src_op;

  if (records && record_count && *record_count < RA_MAX_PHI_COPY_RECORDS) {
    records[*record_count].src_vreg = copy->src_vreg;
    records[*record_count].new_instr_idx = *wp;
    (*record_count)++;
  }

  new_instrs[*wp].op = TCCIR_OP_ASSIGN;
  new_instrs[*wp].operand_base = *pool_wp;
  new_instrs[*wp].line_num = 0;
  new_instrs[*wp].is_jump_target = 0;
  (*wp)++;
  *pool_wp += 2;
}

static int ra_btype_stack_size(int btype)
{
  switch (btype) {
  case IROP_BTYPE_INT64:
  case IROP_BTYPE_FLOAT64:
    return 8;
  default:
    return 4;
  }
}

static void ra_note_vreg_type_for_btype(TCCIRState *ir, int32_t vreg, int btype)
{
  if (!tcc_ir_vreg_is_valid(ir, vreg))
    return;
  if (btype == IROP_BTYPE_FLOAT32 || btype == IROP_BTYPE_FLOAT64)
    tcc_ir_vreg_type_set_fp(ir, vreg, 1, btype == IROP_BTYPE_FLOAT64);
  else if (btype == IROP_BTYPE_INT64)
    tcc_ir_vreg_type_set_64bit(ir, vreg);
}

static void ra_record_stack_operand_min(IROperand op, int *min_offset)
{
  if (op.tag != IROP_TAG_STACKOFF)
    return;
  int off = irop_get_stack_offset(op);
  if (off < *min_offset)
    *min_offset = off;
}

static int ra_find_phi_spill_cursor(TCCIRState *ir)
{
  int min_offset = 0;
  for (int i = 0; i < ir->ls.next_interval_index; i++) {
    LSLiveInterval *lsi = &ir->ls.intervals[i];
    if (lsi->stack_location < min_offset)
      min_offset = lsi->stack_location;
  }
  for (int i = 0; i < ir->next_instruction_index; i++) {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (irop_config[q->op].has_dest)
      ra_record_stack_operand_min(tcc_ir_op_get_dest(ir, q), &min_offset);
    if (irop_config[q->op].has_src1)
      ra_record_stack_operand_min(tcc_ir_op_get_src1(ir, q), &min_offset);
    if (irop_config[q->op].has_src2)
      ra_record_stack_operand_min(tcc_ir_op_get_src2(ir, q), &min_offset);
    if (q->op == TCCIR_OP_MLA)
      ra_record_stack_operand_min(tcc_ir_op_get_accum(ir, q), &min_offset);
  }
  return min_offset;
}

static int32_t ra_create_phi_temp(TCCIRState *ir, int btype, int start, int end,
                                  int *phi_spill_cursor)
{
  int32_t tmp_vreg = tcc_ir_vreg_alloc_temp(ir);
  if (tmp_vreg < 0)
    return -1;

  int size = ra_btype_stack_size(btype);
  *phi_spill_cursor -= size;
  if (size > 4 && (*phi_spill_cursor & 7))
    *phi_spill_cursor &= ~7;

  ra_note_vreg_type_for_btype(ir, tmp_vreg, btype);
  tcc_ls_add_live_interval(&ir->ls, tmp_vreg, start, end, 0, 0,
                           tcc_ir_vreg_type_get(ir, tmp_vreg), 0, -1);
  LSLiveInterval *lsi = &ir->ls.intervals[ir->ls.next_interval_index - 1];
  lsi->r0 = -1;
  lsi->r1 = -1;
  lsi->stack_location = *phi_spill_cursor;

  tcc_ir_stack_reg_assign(ir, tmp_vreg, *phi_spill_cursor, -1, -1);
  IRLiveInterval *li = tcc_ir_vreg_live_interval(ir, tmp_vreg);
  if (li) {
    li->start = (uint32_t)start;
    li->end = (uint32_t)end;
  }

  return tmp_vreg;
}

static void ra_emit_scheduled_phi_copies(TCCIRState *ir, IRQuadCompact *new_instrs,
                                         int *wp, int *pool_wp, RAPhiCopy *copies,
                                         int copy_count, int block,
                                         RAPhiCopyRecord *records, int *record_count,
                                         int *phi_spill_cursor, RAPhiStats *stats)
{
  int start_wp = *wp;
  int emitted = 0;
  while (emitted < copy_count) {
    int progress = 0;
    for (int ci = 0; ci < copy_count; ci++) {
      if (copies[ci].emitted)
        continue;
      if (ra_phi_copy_dest_clobbers_pending_source(ir, copies, copy_count, ci))
        continue;
      ra_emit_phi_copy(ir, new_instrs, wp, pool_wp, &copies[ci], records, record_count);
      copies[ci].emitted = 1;
      emitted++;
      progress = 1;
      break;
    }

    if (!progress) {
      int ci;
      for (ci = 0; ci < copy_count; ci++)
        if (!copies[ci].emitted)
          break;
      if (ci >= copy_count)
        break;

      int32_t saved_src = copies[ci].src_vreg;
      int32_t tmp_vreg = ra_create_phi_temp(ir, copies[ci].btype, *wp,
                                            *wp + copy_count - emitted,
                                            phi_spill_cursor);
      if (tmp_vreg < 0) {
        RA_DBG("SSA phi resolver: failed to allocate cycle temp in block %d", block);
        break;
      }

      RAPhiCopy save = {
        .dest_vreg = tmp_vreg,
        .src_vreg = saved_src,
        .btype = copies[ci].btype,
        .emitted = 0,
      };
      RA_DBG("SSA phi resolver: breaking cyclic parallel copy in block %d with T%d",
             block, TCCIR_DECODE_VREG_POSITION(tmp_vreg));
      if (TCC_LOG_LS) {
        stats->cycle_temporaries++;
        ra_phi_stats_add_participant(stats, tmp_vreg);
      }
      ra_emit_phi_copy(ir, new_instrs, wp, pool_wp, &save, records, record_count);
      copies[ci].src_vreg = tmp_vreg;
    }
  }
  if (TCC_LOG_LS)
    stats->parallel_emitted += *wp - start_wp;
}

static void ra_build_live_regs_bitmap(TCCIRState *ir);

static void ra_resolve_phis(TCCIRState *ir, IRCFG *cfg, IRSSAState *ssa,
                            RAPhiStats *stats)
{
  int nb = cfg->num_blocks;
  int old_n = ir->next_instruction_index;

  /* Count copies needed per predecessor block.
   *
   * Single-pass aggregation: walk every phi node once (O(total phi
   * operands)) and bucket each operand into its predecessor block's
   * counter. The previous version called ra_count_phi_copies_for_pred
   * per predecessor, which itself looped over every block, producing
   * O(blocks^2) work even when no phis existed — pathological for huge
   * branch-heavy functions (compile/20001226-1 has 16K blocks). */
  int *copies_per_block = tcc_mallocz(nb * sizeof(int));
  int total_copies = 0;
  /* For modified-JUMPIF detection we need the per-(pred,succ) count too,
   * but only for blocks that ended in a JUMPIF AND have any copies on the
   * target edge. Build a bitmap of (pred -> succ-block) edges that carry
   * at least one phi copy. We use a simple flat array indexed by pred. */
  int *copies_to_jumpif_succ = tcc_mallocz(nb * sizeof(int));
  for (int b = 0; b < nb; b++) copies_to_jumpif_succ[b] = -1;
  /* First, identify the JUMPIF-target succ_block per pred (if any). */
  for (int b = 0; b < nb; b++) {
    IRBasicBlock *bb = &cfg->blocks[b];
    int last_instr = bb->end_idx - 1;
    if (last_instr < bb->start_idx) continue;
    if (ir->compact_instructions[last_instr].op != TCCIR_OP_JUMPIF) continue;
    IROperand dest = tcc_ir_op_get_dest(ir, &ir->compact_instructions[last_instr]);
    int old_target = (int)irop_get_imm64_ex(ir, dest);
    int target_block = (old_target >= 0 && old_target < old_n) ? cfg->instr_to_block[old_target] : -1;
    copies_to_jumpif_succ[b] = target_block; /* may be -1 */
  }
  /* Now walk phis once. Per operand: increment copies_per_block[pred],
   * and if pred's JUMPIF target == this phi's succ block, also note that
   * the JUMPIF edge carries a copy. */
  int extra_jumps = 0;
  int modified_jumpifs = 0;
  uint8_t *jumpif_edge_has_copy = tcc_mallocz(nb);
  for (int sb = 0; sb < nb; sb++) {
    for (IRPhiNode *phi = ssa->block_phis[sb]; phi; phi = phi->next) {
      for (int pi = 0; pi < phi->num_operands; pi++) {
        if (TCC_LOG_LS)
          stats->phi_operands++;
        int pred = phi->operands[pi].pred_block;
        if (pred < 0 || pred >= nb) continue;
        if (phi->operands[pi].vreg < 0) continue;
        if (!ra_phi_copy_needed(ir, phi, pi)) continue;
        copies_per_block[pred]++;
        total_copies++;
        if (TCC_LOG_LS) {
          stats->parallel_requested++;
          ra_phi_stats_add_participant(stats, phi->dest_vreg);
          ra_phi_stats_add_participant(stats, phi->operands[pi].vreg);
        }
        if (copies_to_jumpif_succ[pred] == sb)
          jumpif_edge_has_copy[pred] = 1;
      }
    }
  }
  for (int b = 0; b < nb; b++) {
    if (jumpif_edge_has_copy[b]) {
      extra_jumps++;
      modified_jumpifs++;
    }
  }
  tcc_free(copies_to_jumpif_succ);
  tcc_free(jumpif_edge_has_copy);

  RA_DBG("SSA phi resolver: total_copies=%d extra_jumps=%d", total_copies, extra_jumps);
  for (int b = 0; b < nb; b++) {
    if (copies_per_block[b] > 0)
      RA_DBG("  block %d: %d copies", b, copies_per_block[b]);
  }

  /* Dump all phi nodes for debugging */
  for (int sb = 0; sb < nb; sb++) {
    for (IRPhiNode *phi = ssa->block_phis[sb]; phi; phi = phi->next) {
      IRLiveInterval *dest_li = tcc_ir_vreg_live_interval(ir, phi->dest_vreg);
      RA_DBG("  phi in block %d: dest=T%d (r0=%d off=%d) ops=%d",
             sb, TCCIR_DECODE_VREG_POSITION(phi->dest_vreg),
             dest_li ? dest_li->allocation.r0 : -99,
             dest_li ? dest_li->allocation.offset : -99,
             phi->num_operands);
      for (int pi = 0; pi < phi->num_operands; pi++) {
        IRLiveInterval *src_li = tcc_ir_vreg_is_valid(ir, phi->operands[pi].vreg) ?
          tcc_ir_vreg_live_interval(ir, phi->operands[pi].vreg) : NULL;
        int needed = ra_phi_copy_needed(ir, phi, pi);
        RA_DBG("    op[%d]: src=T%d pred=%d (r0=%d off=%d) needed=%d",
               pi, TCCIR_DECODE_VREG_POSITION(phi->operands[pi].vreg),
               phi->operands[pi].pred_block,
               src_li ? src_li->allocation.r0 : -99,
               src_li ? src_li->allocation.offset : -99,
               needed);
      }
    }
  }

  if (total_copies == 0) {
    tcc_free(copies_per_block);
    return;
  }

  /* Track emitted phi copies so we can extend source vreg live intervals */
  RAPhiCopyRecord *copy_records = tcc_mallocz(sizeof(RAPhiCopyRecord) * RA_MAX_PHI_COPY_RECORDS);
  int copy_record_count = 0;

  /* Build new instruction array with phi copies inserted */
  int new_n = old_n + total_copies + extra_jumps;
  int new_cap = new_n + 16;
  new_cap += total_copies;
  IRQuadCompact *new_instrs = tcc_mallocz(new_cap * sizeof(IRQuadCompact));
  int *old_to_new = tcc_malloc(old_n * sizeof(int));

  /* Grow operand pool */
  int pool_base = ir->iroperand_pool_count;
  int needed_pool = total_copies * 4 + extra_jumps + modified_jumpifs * 2;
  while (pool_base + needed_pool > ir->iroperand_pool_capacity) {
    int nc = ir->iroperand_pool_capacity ? ir->iroperand_pool_capacity * 2 : 256;
    ir->iroperand_pool = tcc_realloc(ir->iroperand_pool, nc * sizeof(IROperand));
    ir->iroperand_pool_capacity = nc;
  }

  int wp = 0;
  int pool_wp = pool_base;
  int phi_spill_cursor = ra_find_phi_spill_cursor(ir);
  int first_phi_temp_pos = ir->next_temporary_variable;

  for (int b = 0; b < nb; b++) {
    IRBasicBlock *bb = &cfg->blocks[b];
    int last_instr = bb->end_idx - 1;
    int insert_before = bb->end_idx;
    if (last_instr >= bb->start_idx) {
      TccIrOp op = ir->compact_instructions[last_instr].op;
      if (op == TCCIR_OP_JUMP || op == TCCIR_OP_JUMPIF ||
          op == TCCIR_OP_RETURNVALUE || op == TCCIR_OP_RETURNVOID ||
          op == TCCIR_OP_IJUMP || op == TCCIR_OP_SWITCH_TABLE)
        insert_before = last_instr;
    }

    if (copies_per_block[b] > 0 && last_instr >= bb->start_idx &&
        ir->compact_instructions[last_instr].op == TCCIR_OP_JUMPIF) {
      IRQuadCompact *term = &ir->compact_instructions[last_instr];
      IROperand old_dest = tcc_ir_op_get_dest(ir, term);
      int old_target = (int)irop_get_imm64_ex(ir, old_dest);
      int target_block = (old_target >= 0 && old_target < old_n) ? cfg->instr_to_block[old_target] : -1;
      int fallthrough_block = (b + 1 < nb) ? b + 1 : -1;
      int target_count = (target_block >= 0) ? ra_count_phi_copies_for_pred(ir, cfg, ssa, b, target_block) : 0;
      int fallthrough_count =
          (fallthrough_block >= 0) ? ra_count_phi_copies_for_pred(ir, cfg, ssa, b, fallthrough_block) : 0;

      for (int i = bb->start_idx; i < last_instr; i++) {
        old_to_new[i] = wp;
        new_instrs[wp++] = ir->compact_instructions[i];
      }

      old_to_new[last_instr] = wp;
      if (target_count > 0) {
        IROperand cond = tcc_ir_op_get_src1(ir, term);
        cond.u.imm32 = ra_invert_cond((int)irop_get_imm64_ex(ir, cond));

        IROperand skip_dest = old_dest;
        skip_dest.u.imm32 = -(wp + 1 + target_count + 1 + 1);
        int skip_dest_pool_idx = pool_wp;
        ir->iroperand_pool[pool_wp] = skip_dest;
        ir->iroperand_pool[pool_wp + 1] = cond;
        new_instrs[wp] = *term;
        new_instrs[wp].operand_base = pool_wp;
        wp++;
        pool_wp += 2;

        RAPhiCopy *copies = tcc_malloc(sizeof(RAPhiCopy) * target_count);
        int copy_count = ra_collect_phi_copies_for_pred(ir, cfg, ssa, b, target_block, copies);
        ra_emit_scheduled_phi_copies(ir, new_instrs, &wp, &pool_wp, copies, copy_count, b,
                                     copy_records, &copy_record_count, &phi_spill_cursor, stats);
        tcc_free(copies);

        ir->iroperand_pool[pool_wp] = old_dest;
        new_instrs[wp].op = TCCIR_OP_JUMP;
        new_instrs[wp].operand_base = pool_wp;
        new_instrs[wp].line_num = term->line_num;
        new_instrs[wp].is_jump_target = 0;
        wp++;
        pool_wp++;

        /* The inverted JUMPIF skips over the phi copies AND this back-edge JUMP,
         * landing on the instruction right after the JUMP we just wrote (== wp
         * now).  Compute the skip target HERE, after the JUMP write, using the
         * current wp — NOT before emitting the copies/JUMP.  Reading wp earlier
         * (the old `-(wp + 2)` before the copy emit) is fragile: the value used
         * must reflect the copies just emitted via ra_emit_scheduled_phi_copies
         * (which advances wp through &wp).  Encode as the negative sentinel the
         * "Fix jump targets" pass below decodes with `-old_target - 1`, so a
         * target of `wp` is stored as `-(wp + 1)`. */
        skip_dest = ir->iroperand_pool[skip_dest_pool_idx];
        skip_dest.u.imm32 = -(wp + 1);
        ir->iroperand_pool[skip_dest_pool_idx] = skip_dest;
      } else {
        new_instrs[wp++] = *term;
      }

      if (fallthrough_count > 0) {
        RAPhiCopy *copies = tcc_malloc(sizeof(RAPhiCopy) * fallthrough_count);
        int copy_count = ra_collect_phi_copies_for_pred(ir, cfg, ssa, b, fallthrough_block, copies);
        ra_emit_scheduled_phi_copies(ir, new_instrs, &wp, &pool_wp, copies, copy_count, b,
                                     copy_records, &copy_record_count, &phi_spill_cursor, stats);
        tcc_free(copies);
      }
      continue;
    }

    /* Keep conditional phi copies off the compare/test -> JUMPIF edge:
     * physical-register copies can otherwise clobber the condition flags. */
    for (int i = bb->start_idx; i < insert_before; i++) {
      old_to_new[i] = wp;
      new_instrs[wp++] = ir->compact_instructions[i];
    }

    int pre_copy_wp = wp;

    /* Insert phi copies for this block's successors */
    if (copies_per_block[b] > 0) {
      RAPhiCopy *copies = tcc_malloc(sizeof(RAPhiCopy) * copies_per_block[b]);
      int copy_count = ra_collect_phi_copies_for_pred(ir, cfg, ssa, b, -1, copies);
      ra_emit_scheduled_phi_copies(ir, new_instrs, &wp, &pool_wp, copies, copy_count, b,
                                   copy_records, &copy_record_count, &phi_spill_cursor, stats);
      tcc_free(copies);
    }

    /* Copy terminator and remaining instructions */
    for (int i = insert_before; i < bb->end_idx; i++) {
      if (i == bb->start_idx && copies_per_block[b] > 0)
        old_to_new[i] = pre_copy_wp;
      else
        old_to_new[i] = wp;
      new_instrs[wp++] = ir->compact_instructions[i];
    }
  }

  ir->iroperand_pool_count = pool_wp;

  /* Fix jump targets */
  for (int i = 0; i < wp; i++) {
    IRQuadCompact *q = &new_instrs[i];
    if (q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_JUMPIF) {
      IROperand dest = tcc_ir_op_get_dest(ir, q);
      int old_target = (int)irop_get_imm64_ex(ir, dest);
      if (old_target < 0) {
        dest.u.imm32 = -old_target - 1;
        ir->iroperand_pool[q->operand_base] = dest;
      } else if (old_target >= 0 && old_target < old_n) {
        dest.u.imm32 = old_to_new[old_target];
        ir->iroperand_pool[q->operand_base] = dest;
      } else if (old_target >= old_n) {
        dest.u.imm32 = wp + (old_target - old_n);
        ir->iroperand_pool[q->operand_base] = dest;
      }
    }
  }

  /* Fix switch table targets */
  for (int t = 0; t < ir->num_switch_tables; t++) {
    TCCIRSwitchTable *table = &ir->switch_tables[t];
    for (int ti = 0; ti < table->num_entries; ti++) {
      int ot = table->targets[ti];
      if (ot >= 0 && ot < old_n) table->targets[ti] = old_to_new[ot];
    }
    if (table->default_target >= 0 && table->default_target < old_n)
      table->default_target = old_to_new[table->default_target];
  }

  /* Replace instruction array */
  tcc_free(ir->compact_instructions);
  ir->compact_instructions = new_instrs;
  ir->compact_instructions_size = new_cap;
  ir->next_instruction_index = wp;

  /* Rebuild is_jump_target flags */
  for (int i = 0; i < wp; i++)
    new_instrs[i].is_jump_target = 0;
  for (int i = 0; i < wp; i++) {
    IRQuadCompact *q = &new_instrs[i];
    if (q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_JUMPIF) {
      IROperand dest = tcc_ir_op_get_dest(ir, q);
      int tgt = (int)irop_get_imm64_ex(ir, dest);
      if (tgt >= 0 && tgt < wp) new_instrs[tgt].is_jump_target = 1;
    }
  }
  for (int t = 0; t < ir->num_switch_tables; t++) {
    TCCIRSwitchTable *table = &ir->switch_tables[t];
    for (int ti = 0; ti < table->num_entries; ti++) {
      int tgt = table->targets[ti];
      if (tgt >= 0 && tgt < wp) new_instrs[tgt].is_jump_target = 1;
    }
  }

  /* The remaining steps (live-interval remap/extend, live_regs bitmap)
   * only apply when phi resolution runs after register allocation.
   * Pre-RA, no LS intervals or bitmap exist yet — skip. ra_build_intervals
   * will scan the freshly emitted ASSIGN copies and produce correct
   * intervals from scratch.
   *
   * Clear ssa->block_phis: the explicit copies we just inserted are now
   * the source of truth. Leaving phi nodes active confuses the interval
   * builder (it tries to extend phi-dest intervals as if the phi were
   * still semantically active, on top of the now-explicit defs). */
  if (ra_phi_resolve_pre_ra_mode) {
    /* Free each block's phi list before detaching it — the explicit copies are
     * now the source of truth, so these nodes are dead.  Merely NULLing the
     * heads (as before) orphaned every phi node + operand array: tcc_ir_ssa_free
     * later sees an empty block_phis and frees nothing, leaking on every compile. */
    for (int b = 0; b < nb; b++) {
      IRPhiNode *phi = ssa->block_phis[b];
      while (phi) {
        IRPhiNode *next = phi->next;
        tcc_free(phi->operands);
        tcc_free(phi);
        phi = next;
      }
      ssa->block_phis[b] = NULL;
    }
    tcc_free(old_to_new);
    tcc_free(copies_per_block);
    tcc_free(copy_records);
    return;
  }

  /* Remap live interval start/end */
  for (int i = 0; i < ir->ls.next_interval_index; i++) {
    LSLiveInterval *lsi = &ir->ls.intervals[i];
    int32_t vreg = lsi->vreg;
    IRLiveInterval *li = tcc_ir_vreg_live_interval(ir, vreg);
    if (!li) continue;
    if (TCCIR_DECODE_VREG_TYPE(vreg) == TCCIR_VREG_TYPE_TEMP &&
        TCCIR_DECODE_VREG_POSITION(vreg) >= first_phi_temp_pos)
      continue;
    /* Remap start */
    if (li->start < (uint32_t)old_n) {
      li->start = old_to_new[li->start];
      lsi->start = li->start;
    }
    /* Remap end */
    if (li->end < (uint32_t)old_n) {
      li->end = old_to_new[li->end];
      lsi->end = li->end;
    }
  }

  tcc_free(old_to_new);
  tcc_free(copies_per_block);

  /* Extend source vreg live intervals to cover phi copy instructions.
   * Phi copies read the source vreg at the copy's instruction index,
   * so the source must be alive there. Without this extension, the
   * scratch register allocator may reuse the source's register. */
  for (int i = 0; i < copy_record_count; i++) {
    int32_t sv = copy_records[i].src_vreg;
    int ci = copy_records[i].new_instr_idx;
    IRLiveInterval *li = tcc_ir_vreg_live_interval(ir, sv);
    if (!li) continue;
    if ((int)li->end < ci) {
      RA_DBG("  phi copy: extending vreg 0x%x end from %d to %d", sv, (int)li->end, ci);
      li->end = ci;
    }
    for (int j = 0; j < ir->ls.next_interval_index; j++) {
      LSLiveInterval *lsi = &ir->ls.intervals[j];
      if (lsi->vreg == sv && (int)lsi->end < ci) {
        lsi->end = ci;
        break;
      }
    }
  }
  tcc_free(copy_records);

  /* Re-extend intervals for backward jumps in the post-phi instruction stream.
   * The original backward-jump extension was computed on pre-phi indices; phi
   * copies inserted before a backward jump are therefore not covered.  Any
   * register live at the jump target must also be considered live during the
   * phi copies so that the scratch-register allocator does not clobber it. */
  {
    int new_n = ir->next_instruction_index;
    for (int i = 0; i < new_n; i++) {
      IRQuadCompact *q = &ir->compact_instructions[i];
      if (q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_JUMPIF) {
        int target = (int)tcc_ir_op_get_dest(ir, q).u.imm32;
        if (target >= 0 && target < new_n && target < i) {
          for (int j = 0; j < ir->ls.next_interval_index; j++) {
            LSLiveInterval *lsi = &ir->ls.intervals[j];
            IRLiveInterval *li = tcc_ir_vreg_live_interval(ir, lsi->vreg);
            if (!li) continue;
            if ((int)li->start <= target && (int)li->end >= target &&
                (int)li->end < i) {
              RA_DBG("  post-phi back-edge extend: vreg 0x%x [%d,%d] -> [%d,%d]",
                     lsi->vreg, (int)li->start, (int)li->end, (int)li->start, i);
              li->end = i;
              lsi->end = i;
            }
          }
        }
      }
    }
  }

  /* Build live_regs_by_instruction table */
  ra_build_live_regs_bitmap(ir);
}

static void ra_build_live_regs_bitmap(TCCIRState *ir)
{
  uint32_t max_end = 0;
  for (int i = 0; i < ir->ls.next_interval_index; i++) {
    LSLiveInterval *lsi = &ir->ls.intervals[i];
    if (lsi->stack_location != 0 || lsi->r0 < 0) continue;
    if (lsi->reg_type != LS_REG_TYPE_INT && lsi->reg_type != LS_REG_TYPE_LLONG &&
        lsi->reg_type != LS_REG_TYPE_DOUBLE_SOFT && lsi->reg_type != LS_REG_TYPE_COMPLEX_FLOAT)
      continue;
    if (lsi->end > max_end) max_end = lsi->end;
  }
  int sz = (int)max_end + 1;
  if (sz < ir->next_instruction_index) sz = ir->next_instruction_index;
  if (sz > 0) {
    if (ir->ls.live_regs_by_instruction)
      tcc_free(ir->ls.live_regs_by_instruction);
    ir->ls.live_regs_by_instruction = tcc_mallocz(sizeof(uint32_t) * sz);
    ir->ls.live_regs_by_instruction_size = sz;
    RA_DBG("SSA live_regs_by_instruction build (sz=%d)", sz);
    for (int i = 0; i < ir->ls.next_interval_index; i++) {
      LSLiveInterval *lsi = &ir->ls.intervals[i];
      if (lsi->stack_location != 0 || lsi->r0 < 0) continue;
      if (lsi->reg_type != LS_REG_TYPE_INT && lsi->reg_type != LS_REG_TYPE_LLONG &&
          lsi->reg_type != LS_REG_TYPE_DOUBLE_SOFT && lsi->reg_type != LS_REG_TYPE_COMPLEX_FLOAT)
        continue;
      uint32_t mask = 0;
      if (lsi->r0 >= 0 && lsi->r0 < 16) mask |= (1u << lsi->r0);
      if (lsi->r1 >= 0 && lsi->r1 < 16) mask |= (1u << lsi->r1);
      if (!mask) continue;
      int s = (int)lsi->start, e = (int)lsi->end;
      if (s < 0) s = 0;
      if (e >= sz) e = sz - 1;
      RA_DBG("  interval vreg=0x%x r0=R%d r1=%d range=[%d,%d] mask=0x%x",
             lsi->vreg, lsi->r0, lsi->r1, s, e, mask);
      for (int k = s; k <= e; k++)
        ir->ls.live_regs_by_instruction[k] |= mask;
    }

    if (TCC_LOG_LS) {
      for (int k = 0; k < sz; k++)
        RA_DBG("  instr[%d] live=0x%x", k, ir->ls.live_regs_by_instruction[k]);
    }
  }
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

/* Backward-liveness bit matrix: nb blocks x tbl vreg-slots, one allocation,
 * auto-freed on every exit path. Rows feed the tcc_bitspan_* word kernels. */
TCC_BIT_MATRIX_DEFINE(RaLiveMatrix)

/* Refine live_regs_by_instruction (the interval-derived approximation the
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
 * edges (IJUMP / SWITCH_TABLE), matching the coalescer's own guard. */
static void ra_refine_live_regs_accurate(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  if (n <= 0) return;
  int has_backedge = 0;
  for (int i = 0; i < n; i++) {
    IRQuadCompact *q = &ir->compact_instructions[i];
    int op = q->op;
    if (op == TCCIR_OP_IJUMP || op == TCCIR_OP_SWITCH_TABLE || op == TCCIR_OP_SWITCH_LOAD)
      return;
    if (op == TCCIR_OP_JUMP || op == TCCIR_OP_JUMPIF) {
      int t = (int)tcc_ir_op_get_dest(ir, q).u.imm32;
      if (t >= 0 && t < i)
        has_backedge = 1;
    }
  }
  if (!has_backedge) return;
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
  /* Only register-resident slots can contribute to the final mask; liveness is
   * per-vreg independent, so restricting the dataflow universe to them is exact. */
  int *dense = tcc_malloc(sizeof(int) * tbl);
  int ndense = 0;
  for (int i = 0; i < tbl; i++) {
    int tracked = (vr0[i] >= 0 && vr0[i] < 16) || (vr1[i] >= 0 && vr1[i] < 16);
    dense[i] = tracked ? ndense++ : -1;
  }
  if (ndense == 0) {
    tcc_free(vr0); tcc_free(vr1); tcc_free(dense);
    tcc_ir_cfg_free(cfg);
    return;
  }
  int nw = (ndense + 63) / 64;
  bit_matrix(RaLiveMatrix) useb = {0};
  bit_matrix(RaLiveMatrix) defbk = {0};
  bit_matrix(RaLiveMatrix) livein = {0};
  bit_matrix(RaLiveMatrix) liveout = {0};
  RaLiveMatrix_init(&useb, nb, ndense);
  RaLiveMatrix_init(&defbk, nb, ndense);
  RaLiveMatrix_init(&livein, nb, ndense);
  RaLiveMatrix_init(&liveout, nb, ndense);
  for (int b = 0; b < nb; b++) {
    uint64_t *ub = RaLiveMatrix_row(&useb, b), *db = RaLiveMatrix_row(&defbk, b);
    int s = cfg->blocks[b].start_idx, e = cfg->blocks[b].end_idx;
    for (int i = s; i < e && i < n; i++) {
      int32_t def=-1, hd=0, uses[4], nu=0;
      ra_co_ops(ir, &ir->compact_instructions[i], &def, &hd, uses, &nu);
      for (int k=0;k<nu;k++){ if(!tcc_ir_vreg_is_valid(ir,uses[k]))continue; int u=DVIDX(uses[k]); if(u<0||u>=tbl)continue; int ud=dense[u]; if(ud<0)continue; if(!tcc_bitspan_test(db,ud)) tcc_bitspan_set(ub,ud);}
      if (hd && tcc_ir_vreg_is_valid(ir,def)){int d=DVIDX(def); if(d>=0&&d<tbl && dense[d]>=0) tcc_bitspan_set(db,dense[d]);}
    }
  }
  int changed=1, guard=0;
  while (changed && guard++ < nb+4) {
    changed=0;
    for (int ri=cfg->rpo_count-1; ri>=0; ri--) {
      int b = cfg->rpo_order ? cfg->rpo_order[ri] : ri;
      if (b<0||b>=nb) continue;
      uint64_t *lo = RaLiveMatrix_row(&liveout, b), *li = RaLiveMatrix_row(&livein, b);
      uint64_t *ub = RaLiveMatrix_row(&useb, b), *db = RaLiveMatrix_row(&defbk, b);
      tcc_bitspan_zero(lo, nw);
      for (int si=0;si<cfg->blocks[b].num_succs;si++){int sb=cfg->blocks[b].succs[si]; if(sb<0||sb>=nb)continue; tcc_bitspan_or(lo, RaLiveMatrix_row(&livein, sb), nw);}
      changed |= tcc_bitspan_or_andnot(li, ub, lo, db, nw);
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
    uint64_t *hli = RaLiveMatrix_row(&livein, hb);
    uint32_t mask = 0;
    for (int vi=0; vi<tbl; vi++) {
      if (dense[vi] < 0 || !tcc_bitspan_test(hli,dense[vi])) continue;
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
  tcc_free(vr0);tcc_free(vr1);tcc_free(dense);
  tcc_ir_cfg_free(cfg);
}

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
  bit_matrix(RaLiveMatrix) useb = {0};
  bit_matrix(RaLiveMatrix) defbk = {0};
  bit_matrix(RaLiveMatrix) livein = {0};
  bit_matrix(RaLiveMatrix) liveout = {0};
  RaLiveMatrix_init(&useb, nb, tbl);
  RaLiveMatrix_init(&defbk, nb, tbl);
  RaLiveMatrix_init(&livein, nb, tbl);
  RaLiveMatrix_init(&liveout, nb, tbl);

  for (int b = 0; b < nb; b++) {
    uint64_t *ub = RaLiveMatrix_row(&useb, b), *db = RaLiveMatrix_row(&defbk, b);
    int s = cfg->blocks[b].start_idx, e = cfg->blocks[b].end_idx;
    for (int i = s; i < e && i < n; i++) {
      int32_t def = -1, hd = 0, uses[4], nu = 0;
      ra_co_ops(ir, &ir->compact_instructions[i], &def, &hd, uses, &nu);
      for (int k = 0; k < nu; k++) {
        if (!tcc_ir_vreg_is_valid(ir, uses[k])) continue;
        int u = VIDX(uses[k]);
        if (u < 0 || u >= tbl) continue;
        if (!tcc_bitspan_test(db, u)) tcc_bitspan_set(ub, u); /* upward-exposed use */
      }
      if (hd && tcc_ir_vreg_is_valid(ir, def)) {
        int d = VIDX(def);
        if (d >= 0 && d < tbl) tcc_bitspan_set(db, d);
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
      uint64_t *lo = RaLiveMatrix_row(&liveout, b), *li = RaLiveMatrix_row(&livein, b);
      uint64_t *ub = RaLiveMatrix_row(&useb, b), *db = RaLiveMatrix_row(&defbk, b);
      /* live_out = union of successors' live_in */
      tcc_bitspan_zero(lo, nw);
      for (int si = 0; si < cfg->blocks[b].num_succs; si++) {
        int sb = cfg->blocks[b].succs[si];
        if (sb < 0 || sb >= nb) continue;
        tcc_bitspan_or(lo, RaLiveMatrix_row(&livein, sb), nw);
      }
      /* live_in = use ∪ (live_out − def) */
      changed |= tcc_bitspan_or_andnot(li, ub, lo, db, nw);
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
      if (idx >= 0 && idx < tbl) tcc_bitspan_set(isint, idx);
    }
    uint64_t *live = tcc_malloc(sizeof(uint64_t) * nw);
    int maxp = 0;
    for (int b = 0; b < nb && maxp < K; b++) {
      uint64_t *lo = RaLiveMatrix_row(&liveout, b);
      int p = 0;
      tcc_bitspan_copy(live, lo, nw);
      p += tcc_bitspan_and_popcount(live, isint, nw);
      int s = cfg->blocks[b].start_idx, e = cfg->blocks[b].end_idx;
      for (int i = e - 1; i >= s && i < n; i--) {
        if (p > maxp) maxp = p;
        IRQuadCompact *q = &ir->compact_instructions[i];
        int32_t def = -1, hd = 0, uses[4], nu = 0;
        ra_co_ops(ir, q, &def, &hd, uses, &nu);
        if (hd && tcc_ir_vreg_is_valid(ir, def)) {
          int d = VIDX(def);
          if (d >= 0 && d < tbl && tcc_bitspan_test(live, d)) { p -= tcc_bitspan_test(isint, d); tcc_bitspan_reset(live, d); }
        }
        for (int k = 0; k < nu; k++) {
          if (!tcc_ir_vreg_is_valid(ir, uses[k])) continue;
          int u = VIDX(uses[k]);
          if (u >= 0 && u < tbl && !tcc_bitspan_test(live, u)) { p += tcc_bitspan_test(isint, u); tcc_bitspan_set(live, u); }
        }
      }
    }
    tcc_free(isint); tcc_free(live);
    if (maxp >= K) {
      RA_DBG("coalesce: skip — peak INT pressure %d >= K=%d", maxp, K);
      tcc_free(iv_of);
      tcc_ir_cfg_free(cfg);
      return;
    }
  }

  /* ---- Build per-block def bitmap to detect phi-related copies. ----
   * A copy dest that is defined in more than one block is a phi result
   * (explicit copies inserted after SSA phi resolution).  Coalescing such
   * a dest with its source can overwrite the source's value on a sibling
   * phi arm when the source is still live across the merge (seed 860). */
  uint8_t *def_block_count = tcc_mallocz(tbl);
  int *def_last_block = tcc_malloc(sizeof(int) * tbl);
  for (int i = 0; i < tbl; i++) def_last_block[i] = -1;
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
        if (d >= 0 && d < tbl && def_last_block[d] != b) {
          def_last_block[d] = b;
          if (def_block_count[d] < 2) def_block_count[d]++;
        }
      }
    }
  }
  tcc_free(def_last_block);

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
      if (def_block_count[di] > 1) {
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

  tcc_free(def_block_count);

  if (ncand < 2 || ne == 0) {
    tcc_free(iv_of); tcc_free(instr_block);
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
      uint64_t *lo = RaLiveMatrix_row(&liveout, b);
      tcc_bitspan_copy(live, lo, nw);
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
            tcc_bitspan_for_each_set(live, nw, tbl, vi) {
              int c = cand_id[vi];
              if (c < 0) continue;
              if (vi == d) continue;
              if (csrc >= 0 && vi == csrc) continue;
              if (pass == 0) { deg[cd]++; deg[c]++; }
              else { adj[deg[cd]++] = c; adj[deg[c]++] = cd; }
            }
          }
        }
        /* transition live: kill def, gen uses */
        if (hd && tcc_ir_vreg_is_valid(ir, def)) {
          int d = VIDX(def); if (d >= 0 && d < tbl) tcc_bitspan_reset(live, d);
        }
        for (int k = 0; k < nu; k++) {
          if (!tcc_ir_vreg_is_valid(ir, uses[k])) continue;
          int u = VIDX(uses[k]); if (u >= 0 && u < tbl) tcc_bitspan_set(live, u);
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

  #undef ADD_CAND
  #undef VIDX
  tcc_free(deg); tcc_free(live);
  tcc_free(iv_of);
  tcc_free(instr_block);
  tcc_free(cand_id); tcc_free(cand_vidx); tcc_free(edge_d); tcc_free(edge_s);
  tcc_ir_cfg_free(cfg);
}

/* ============================================================================
 * Entry Point
 * ============================================================================ */

void dbg_scan_imm_dest(TCCIRState *ir, const char *pass);
void dbg_scan_overlap(TCCIRState *ir, const char *pass);
/* Promote multiply-block-defined TEMPs to fresh VARs so SSA construction places
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

/* Mark single-def TEMP intervals whose value is a plain int32 constant as
 * rematerializable: when such a value gets spilled, machine_op recomputes it
 * with `mov reg, #imm` at each use instead of a stack reload — cheaper (no
 * memory) and it removes a spill load per use, directly shrinking the
 * spill-reload traffic that widens tcc's load count vs gcc.  Runs on the final
 * post-allocation IR so the recorded immediate matches what codegen sees. */
static void ra_mark_rematerializable(TCCIRState *ir)
{
  int nt = ir->temporary_variables_live_intervals_size;
  if (nt <= 0)
    return;
  for (int t = 0; t < nt; t++)
    ir->temporary_variables_live_intervals[t].remat_kind = 0;

  int n = ir->next_instruction_index;
  int *def_count = tcc_mallocz(nt * sizeof(int));
  int *def_idx = tcc_malloc(nt * sizeof(int));
  for (int t = 0; t < nt; t++)
    def_idx[t] = -1;

  for (int i = 0; i < n; i++) {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP || !irop_config[q->op].has_dest)
      continue;
    int32_t vr = irop_get_vreg(tcc_ir_op_get_dest(ir, q));
    if (vr < 0 || TCCIR_DECODE_VREG_TYPE(vr) != TCCIR_VREG_TYPE_TEMP)
      continue;
    int p = TCCIR_DECODE_VREG_POSITION(vr);
    if (p >= nt)
      continue;
    def_count[p]++;
    def_idx[p] = i;
  }

  /* Slot-reading use: an is_lval deref of the value (machine_op reloads it — see
   * the use_llocal path); such an appearance needs the spill store to stay.
   * Includes the DEST (a deref store `*T = x` reads T as the address). */
  uint8_t *needs_slot = tcc_mallocz(nt);
  for (int i = 0; i < n; i++) {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;
    for (int slot = 0; slot < 4; slot++) {
      IROperand s;
      if (slot == 0) { if (!irop_config[q->op].has_dest) continue; s = tcc_ir_op_get_dest(ir, q); }
      else if (slot == 1) { if (!irop_config[q->op].has_src1) continue; s = tcc_ir_op_get_src1(ir, q); }
      else if (slot == 2) { if (!irop_config[q->op].has_src2) continue; s = tcc_ir_op_get_src2(ir, q); }
      else { if (q->op != TCCIR_OP_MLA) continue; s = tcc_ir_op_get_accum(ir, q); }
      if (!(s.is_lval && !s.is_local && !s.is_llocal))
        continue;
      int32_t vr = irop_get_vreg(s);
      if (vr < 0 || TCCIR_DECODE_VREG_TYPE(vr) != TCCIR_VREG_TYPE_TEMP)
        continue;
      int p = TCCIR_DECODE_VREG_POSITION(vr);
      if (p < nt)
        needs_slot[p] = 1;
    }
  }

  for (int t = 0; t < nt; t++) {
    if (def_count[t] != 1)
      continue;
    IRQuadCompact *dq = &ir->compact_instructions[def_idx[t]];
    if (dq->op != TCCIR_OP_ASSIGN)
      continue;
    IROperand dd = tcc_ir_op_get_dest(ir, dq);
    if (dd.is_lval || irop_get_btype(dd) != IROP_BTYPE_INT32)
      continue;
    IROperand s = tcc_ir_op_get_src1(ir, dq);
    if (s.tag != IROP_TAG_IMM32 || s.is_lval || s.is_sym || s.is_local || s.is_llocal)
      continue;
    IRLiveInterval *iv = &ir->temporary_variables_live_intervals[t];
    iv->remat_kind = 1;
    iv->remat_imm = irop_get_imm32(s);

    /* If the value is spilled and no use reads it from the slot, every use now
     * recomputes the constant — the def's mov+store is dead; drop it. */
    int spilled = (iv->allocation.r0 == PREG_NONE) ||
                  (iv->allocation.r0 & PREG_SPILLED) ||
                  iv->allocation.offset != 0;
    if (spilled && !needs_slot[t])
      dq->op = TCCIR_OP_NOP;
  }

  tcc_free(needs_slot);
  tcc_free(def_count);
  tcc_free(def_idx);
}

void tcc_ir_ssa_regalloc(TCCIRState *ir, const RegAllocTarget *target, int spill_base)
{
  if (!ir || !target) return;
  RAPhiStats phi_stats = {0};
  dbg_scan_overlap(ir, "ssa_regalloc_entry");

  /* ssa:mem_init — frontend memory-init lowering (memset(0)+const stores →
   * rodata BLOCK_COPY; small stack/global zero-memset → direct STORE #0).
   * Runs first on raw flat IR, before cfg_cleanup and the loop transforms
   * below, matching its former position at the head of the tccgen.c pipeline.
   * Ungated by -O (the lowering also fires at -O0); knob
   * TCC_DISABLE_PASS=ssa:mem_init.  See docs/plan_legacy_flat_ir_ssa_retire.md. */
  if (tcc_state && !tcc_ir_opt_pass_disabled("ssa:mem_init"))
    ssa_opt_mem_init(ir);
  tcc_ir_dump_after_pass(ir, "ssa:mem_init");

  /* ssa:cfg_cleanup — GCC-style cleanup_cfg at SSA-pipeline entry: thread jump
   * chains / drop fall-through jumps / NOP orphan flag-setters left by the flat
   * pipeline, so the flat-region loop transforms below (reroll/licm/loop_rotate)
   * see normalized control flow instead of trampoline chains they would rotate
   * around and duplicate tests for.  When the cascade fired, re-run call-result
   * demotion + the flat late_cleanup group (self-gated passes): collapsed
   * diamonds expose dead pure calls / VLA allocs / stores those passes could
   * not see on their earlier run. */
  if (tcc_state && tcc_state->optimize >= 1 && tcc_state->opt_jump_threading &&
      !tcc_ir_opt_pass_disabled("ssa:cfg_cleanup")) {
    const IRPassGroup *groups;
    int group_count;
    tcc_ir_opt_get_pipeline(IR_OPT_LEVEL_2, &groups, &group_count);
    const IRPassGroup *cleanup_group = &groups[group_count - 1];
    for (int outer = 0; outer < 3; outer++) {
      int ch = 0;
      for (int i = 0; i < 8; i++) {
        int c = tcc_ir_opt_jump_threading(ir);
        c += tcc_ir_opt_eliminate_fallthrough(ir);
        c += tcc_ir_opt_jumpif_invert(ir, 0);
        if (c)
          tcc_ir_opt_compact_nops(ir);
        if (tcc_state->opt_dce) {
          c += tcc_ir_opt_orphan_cmp_elim(ir);
          c += tcc_ir_opt_dce(ir);
        }
        ch += c;
        if (!c)
          break;
      }
      if (outer == 0 && !ch)
        break;
      IROptCtx cl_ctx;
      tcc_ir_opt_ctx_init(&cl_ctx, ir);
      if (tcc_state->opt_dead_store)
        ch += tcc_ir_opt_gens_call_result_ex(&cl_ctx);
      ch += tcc_ir_opt_run_group(&cl_ctx, cleanup_group);
      tcc_ir_opt_ctx_free(&cl_ctx);
      if (!ch)
        break;
    }
  }
  tcc_ir_dump_after_pass(ir, "ssa:cfg_cleanup");

  /* ssa:struct_copy_roundtrip — drop the memmove(B,A);memmove(A,B) pair left
   * by an inlined identity `y = retme(y)` helper.  The matcher needs the two
   * copies in one straight line; the inline expansion leaves a fall-through
   * JMP + target marker between them that only cfg_cleanup removes, so the
   * tccgen-time call misses the pattern (20040709-2 fn1* family). */
  if (tcc_state && tcc_state->opt_redundant_store &&
      !tcc_ir_opt_pass_disabled("ssa:struct_copy_roundtrip"))
    tcc_ir_opt_struct_copy_roundtrip_elim(ir);
  tcc_ir_dump_after_pass(ir, "ssa:struct_copy_roundtrip");

  /* ssa:or_bool_diamond — fold `acc |= (cond ? 1 : 0)` stack-slot
   * materialization into per-arm ORs.  Needs the STORE-slot/OR adjacency that
   * cfg_cleanup's eliminate_fallthrough just created.  Gate matches the legacy
   * tccgen.c call (opt_const_prop); knob TCC_DISABLE_PASS=ssa:or_bool_diamond. */
  if (tcc_state && tcc_state->opt_const_prop && !tcc_ir_opt_pass_disabled("ssa:or_bool_diamond"))
    ssa_opt_or_bool_diamond(ir);
  tcc_ir_dump_after_pass(ir, "ssa:or_bool_diamond");

  /* ssa:stack_addr_simplify — deref-of-known-stack-addr → direct StackLoc; legacy gate; knob TCC_DISABLE_PASS=ssa:stack_addr_simplify */
  if (tcc_state && tcc_state->opt_const_prop && !tcc_ir_opt_pass_disabled("ssa:stack_addr_simplify"))
    tcc_ir_opt_stack_addr_simplify(ir);
  tcc_ir_dump_after_pass(ir, "ssa:stack_addr_simplify");

  /* ssa:reroll — re-roll runs of identical macro-unrolled blocks into a counted
   * loop.  Runs first in the flat region (preserving reroll's legacy "earliest
   * loop transform" order) but now post-propagation: foldable runs are already
   * collapsed by downstream const-prop, so only non-foldable repetition survives
   * to re-roll, which fixes the legacy pre-propagation counterproductivity.
   * Gated opt_reroll (-O2); knob TCC_DISABLE_PASS=ssa:reroll.
   * See docs/plan_legacy_loop_reroll_ssa.md. */
  if (tcc_state && tcc_state->opt_reroll && !tcc_ir_opt_pass_disabled("ssa:reroll")) {
    /* compact the (N-1)*P NOPs a successful re-roll leaves so CFG/SSA below don't iterate them */
    if (ssa_opt_reroll(ir))
      tcc_ir_opt_compact_nops(ir);
  }
  tcc_ir_dump_after_pass(ir, "ssa:reroll");

  /* ssa:licm — hoist loop invariants (arithmetic + pure/const calls) to the
   * preheader before the other loop transforms and ssa:iv_strength_reduction,
   * preserving the legacy "LICM before IV-SR" order now that both left tccgen.
   * Reuses the proven licm.c engine; gated opt_licm (-O2); knob
   * TCC_DISABLE_PASS=ssa:licm. */
  if (tcc_state && tcc_state->opt_licm && !tcc_ir_opt_pass_disabled("ssa:licm"))
    ssa_opt_licm(ir);
  tcc_ir_dump_after_pass(ir, "ssa:licm");

  /* Loop rotation (ssa:loop_rotate): convert safe top-tested loops to
   * bottom-tested on flat IR, before the CFG/SSA below are built, so the
   * downstream SSA passes and regalloc see the rotated shape.  CFG/dominator
   * based natural-loop detection driving the proven flat-IR rewrite; gated to
   * -O1+ (matching the legacy pass and the SSA opt tier) and disableable via
   * TCC_DISABLE_PASS=ssa:loop_rotate. */
  if (tcc_state && tcc_state->optimize >= 1 && !tcc_ir_opt_pass_disabled("ssa:loop_rotate"))
    ssa_opt_loop_rotate(ir);
  tcc_ir_dump_after_pass(ir, "ssa:loop_rotate");

  /* First-iteration-exit peeling (ssa:first_iter_exit): eliminate top-tested
   * loops whose header exit test is provably true on first entry, on flat IR
   * after rotation (rotation declines these shapes; a rotated loop's guard is
   * outside the loop so this pass declines rotated shapes — no overlap).
   * Gate matches the legacy tccgen.c pass exactly (-O1+ with const-prop, so
   * -fno-const-prop keeps its meaning for bisection); disableable via
   * TCC_DISABLE_PASS=ssa:first_iter_exit. */
  if (tcc_state && tcc_state->optimize >= 1 && tcc_state->opt_const_prop &&
      !tcc_ir_opt_pass_disabled("ssa:first_iter_exit"))
    ssa_opt_first_iter_exit(ir);
  tcc_ir_dump_after_pass(ir, "ssa:first_iter_exit");

  /* Pointer-IV exit-value substitution (ssa:ptr_iv_exit_subst): rewrite
   * post-loop pointer-IV reads to the closed-form exit address and fold the
   * consuming `p != &a[N]` compares (pass-owned; nothing downstream folds
   * them).  Gate matches the legacy tccgen.c pass (-O1+ with const-prop). */
  if (tcc_state && tcc_state->optimize >= 1 && tcc_state->opt_const_prop &&
      !tcc_ir_opt_pass_disabled("ssa:ptr_iv_exit_subst"))
    ssa_opt_ptr_iv_exit_subst(ir);
  tcc_ir_dump_after_pass(ir, "ssa:ptr_iv_exit_subst");

  /* Loop constant simulation (ssa:loop_const_sim): collapse register-only
   * bounded-trip loops to residual final values on flat IR.  Gate matches the
   * legacy tccgen.c Phase 4e pass exactly (opt_loop_unroll, -O2 default;
   * -floop-unroll reaches it at lower levels; -fno-loop-unroll disables both
   * it and the unroller, keeping the shared bisection knob).  Disableable via
   * TCC_DISABLE_PASS=ssa:loop_const_sim. */
  if (tcc_state && tcc_state->opt_loop_unroll &&
      !tcc_ir_opt_pass_disabled("ssa:loop_const_sim"))
    ssa_opt_loop_const_sim(ir);
  tcc_ir_dump_after_pass(ir, "ssa:loop_const_sim");

  /* Loop unrolling / constant-trip elimination (ssa:loop_unroll): fully unroll
   * or close-form-eliminate small constant/symbolic-trip register-only loops on
   * flat IR after const_sim has starved it of its own candidates.  Same gate as
   * the legacy tccgen.c Phase 5a pass (opt_loop_unroll); the SSA/regalloc
   * pipeline folds the residual arithmetic (no post-unroll cascade replicated).
   * Disableable via TCC_DISABLE_PASS=ssa:loop_unroll. */
  if (tcc_state && tcc_state->opt_loop_unroll &&
      !tcc_ir_opt_pass_disabled("ssa:loop_unroll"))
    ssa_opt_loop_unroll(ir);
  tcc_ir_dump_after_pass(ir, "ssa:loop_unroll");

  /* Induction-variable strength reduction (ssa:iv_strength_reduction): transform
   * array-indexing recurrences base + i*stride into a maintained stride pointer
   * (enabling post-increment addressing) and optionally eliminate the counter IV
   * against a hoisted end pointer, on flat IR after rotation/const_sim/unroll
   * have normalized and starved the loops.  Runs before ssa:decrement_to_zero,
   * preserving the legacy "decrement_to_zero after IV-SR" order.  Gate matches
   * the legacy tccgen.c Phase 6 pass (opt_iv_strength_red, -O1+); disableable via
   * TCC_DISABLE_PASS=ssa:iv_strength_reduction. */
  if (tcc_state && tcc_state->opt_iv_strength_red &&
      !tcc_ir_opt_pass_disabled("ssa:iv_strength_reduction"))
    ssa_opt_iv_strength_reduction(ir);
  tcc_ir_dump_after_pass(ir, "ssa:iv_strength_reduction");

  /* Decrement-to-zero (ssa:decrement_to_zero): rewrite count-up pure-counter
   * loops that ssa:loop_rotate turned bottom-tested (and const_sim/unroll left
   * as side-effecting survivors) into count-down-to-zero so the backend fuses
   * the latch SUB+CMP#0 into a flag-setting SUBS.  Flat IR before the CFG build
   * below, so the NOPed guard's control-flow change is reflected downstream.
   * Gated -O1+ (matches ssa:loop_rotate, the shape provider); disableable via
   * TCC_DISABLE_PASS=ssa:decrement_to_zero. */
  if (tcc_state && tcc_state->optimize >= 1 &&
      !tcc_ir_opt_pass_disabled("ssa:decrement_to_zero"))
    ssa_opt_decrement_to_zero(ir);
  tcc_ir_dump_after_pass(ir, "ssa:decrement_to_zero");

  /* Switch-value IPCP: fold calls to single-arg pure dispatchers whose arg is
   * constant into ASSIGN #const, replaying any captured global stores at the
   * call site.  Runs as the last flat transform, immediately before the CFG
   * build, so the replayed stores are only ever seen by the SSA pipeline
   * (which handles them correctly) and not by the legacy cfg_cleanup DSE,
   * which drops a store still read by a following CMP.  (const_call_replace is
   * store-free and runs early in gen_function so its constant cascades.) */
  if (tcc_state && tcc_state->opt_ipc) {
    int ipc_ch = 0;
    if (!tcc_ir_opt_pass_disabled("ssa:switch_call_replace"))
      ipc_ch += tcc_ir_opt_switch_call_replace(ir);
    /* Re-run the flat propagation group so the freshly folded ASSIGN #const
     * cascades through the caller.  switch_call_replace runs here (not early
     * with const_call_replace) because it replays captured global stores that
     * must bypass the legacy cfg_cleanup DSE; this re-run recovers the caller
     * cascade it would otherwise lose.  Gated on an actual rewrite so non-IPCP
     * functions pay nothing. */
    if (ipc_ch && tcc_state->optimize >= 1) {
      const IRPassGroup *groups;
      int group_count;
      tcc_ir_opt_get_pipeline(IR_OPT_LEVEL_2, &groups, &group_count);
      IROptCtx ipc_ctx;
      tcc_ir_opt_ctx_init(&ipc_ctx, ir);
      tcc_ir_opt_run_group(&ipc_ctx, &groups[0]);
      tcc_ir_opt_ctx_free(&ipc_ctx);
    }
    tcc_ir_dump_after_pass(ir, "ssa:switch_call_replace");
  }

  /* Build CFG + dominators */
  IRCFG *cfg = tcc_ir_cfg_build(ir);
  if (!cfg) {
    /* Fallback: no CFG means trivial function, use old allocator path */
    return;
  }
  tcc_ir_cfg_compute_dominators(cfg);
  tcc_ir_cfg_compute_dom_frontiers(cfg);

  ra_promote_multidef_temps_to_vars(ir, cfg);
  tcc_ir_dump_after_pass(ir, "ssa_promote");

  /* Construct SSA */
  IRSSAState *ssa = tcc_ir_ssa_construct(ir, cfg);
  int had_promotable = (ssa != NULL);
  if (!ssa) {
    /* No promotable variables; still build intervals from flat IR */
    ssa = tcc_mallocz(sizeof(IRSSAState));
    ssa->cfg = cfg;
    ssa->block_phis = tcc_mallocz(cfg->num_blocks * sizeof(IRPhiNode *));
    ssa->num_vars = ir->next_local_variable;
  } else {
    tcc_ir_ssa_rename(ir, ssa);
  }
  tcc_ir_dump_after_pass(ir, "ssa_rename");
  dbg_scan_imm_dest(ir, "ssa_rename"); dbg_scan_overlap(ir, "ssa_rename");

  /* SSA optimization passes.
   * At -O0: only run DCE to remove dead phi definitions that could
   * confuse phi resolution.  Skip copy propagation and target generators
   * which can break VLA/alignment code in unoptimized IR.
   * At -O1+: run the full optimization engine. When no variables were
   * promoted (all address-taken), run only load CSE and branch folding
   * which operate safely on TEMP vregs without phi nodes. */
  {
    IRSSAOptCtx ssa_opt_ctx;
    tcc_ir_ssa_opt_init(&ssa_opt_ctx, ir, ssa, cfg);
    if (tcc_state->optimize >= 1) {
      if (had_promotable) {
        tcc_ir_ssa_opt_run(&ssa_opt_ctx);
      } else {
        /* Run a pass, then make it observable to -dump-ir-passes=<name>
         * golden snapshots (same names as the tcc_ir_ssa_opt_run driver). */
#define RUN_SSA(name, call)                                                                                            \
  do                                                                                                                   \
  {                                                                                                                    \
    if (!tcc_ir_opt_pass_disabled(name))                                                                               \
      np_changes += (call);                                                                                            \
    tcc_ir_dump_after_pass(ir, name);                                                                                  \
  } while (0)
        ssa_opt_ctx.no_stack_fwd = 0;
        for (int np_iter = 0; np_iter < 5; np_iter++) {
          int np_changes = 0;
          RUN_SSA("ssa:var_const_fold", ssa_opt_var_const_fold(&ssa_opt_ctx));
          RUN_SSA("ssa:var_forward", ssa_opt_var_forward(&ssa_opt_ctx));
          RUN_SSA("ssa:sccp", ssa_opt_sccp(&ssa_opt_ctx));
          RUN_SSA("ssa:load_cse", ssa_opt_load_cse(&ssa_opt_ctx));
          RUN_SSA("ssa:diamond_store_fwd", ssa_opt_diamond_store_fwd(&ssa_opt_ctx));
          RUN_SSA("ssa:const_string_fold", tcc_ir_ssa_opt_const_string_fold(&ssa_opt_ctx));
          RUN_SSA("ssa:bitop_const_fold", tcc_ir_ssa_opt_bitop_const_fold(&ssa_opt_ctx));
          RUN_SSA("ssa:ptr_store_dse", tcc_ir_ssa_opt_ptr_store_dse(&ssa_opt_ctx));
          RUN_SSA("ssa:cprop", ssa_opt_cprop(&ssa_opt_ctx));
          RUN_SSA("ssa:fold", ssa_opt_fold(&ssa_opt_ctx));
          RUN_SSA("ssa:var_imm_prop", ssa_opt_var_imm_prop(&ssa_opt_ctx));
          RUN_SSA("ssa:const_prop_tmp", ssa_opt_const_prop_tmp(&ssa_opt_ctx));
          RUN_SSA("ssa:branch", ssa_opt_branch(&ssa_opt_ctx));
          RUN_SSA("ssa:reassoc", ssa_opt_reassoc(&ssa_opt_ctx));
          RUN_SSA("ssa:strength", ssa_opt_strength(&ssa_opt_ctx));
          RUN_SSA("ssa:narrow", ssa_opt_narrow(&ssa_opt_ctx));
          RUN_SSA("ssa:gvn", ssa_opt_gvn(&ssa_opt_ctx));
          RUN_SSA("ssa:phi_simplify", ssa_opt_phi_simplify(&ssa_opt_ctx));
          RUN_SSA("ssa:dce", ssa_opt_dce(&ssa_opt_ctx));
          np_changes += tcc_ir_ssa_opt_guard_collapse(&ssa_opt_ctx);
          if (!np_changes)
            break;
        }
        /* Target-specific fusions (MLA, LOAD/STORE_INDEXED on ARM). These
         * don't need promotable vars or phi nodes — they pattern-match on
         * existing TEMP vregs. */
        tcc_ir_ssa_opt_run_target(&ssa_opt_ctx);
#undef RUN_SSA
      }
    } else {
      ssa_opt_cprop(&ssa_opt_ctx);
      ssa_opt_dce(&ssa_opt_ctx);
    }
    if (tcc_state && tcc_state->optimize >= 1 && tcc_state->opt_redundant_store &&
        !tcc_ir_opt_pass_disabled("ssa:rmw_byte_clear")) {
      int c = tcc_ir_opt_rmw_byte_clear(ir);
      if (c && tcc_state->opt_dce)
        ssa_opt_dce(&ssa_opt_ctx);
    }
    tcc_ir_dump_after_pass(ir, "ssa:rmw_byte_clear");
    /* ssa:memmove_global_fwd — rerun of the tccgen-time init-copy-from-global
     * forwarding.  Its read-only-slot precondition only becomes true here:
     * ssa:struct_copy_roundtrip removes the retme pair and ssa:dce ret_store
     * kills the write-back, both inside this pipeline — after the tccgen call
     * already ran (20040709-2 fn1* family). */
    if (tcc_state && tcc_state->optimize >= 1 && tcc_state->opt_redundant_store &&
        !tcc_ir_opt_pass_disabled("ssa:memmove_global_fwd")) {
      if (tcc_ir_opt_memmove_global_load_fwd(ir) > 0 && tcc_state->opt_dce) {
        /* Flat pass: rebuild the SSA use chains it left stale, or the DCE
         * worklist can't cascade the now-dead dest-LEA/ADD address temps. */
        for (int p = 0; p < ssa_opt_ctx.vinfo_cap; p++)
          ssa_opt_ctx.vinfo[p].use_count = 0;
        for (int i = 0; i < ir->next_instruction_index; i++) {
          IRQuadCompact *q = &ir->compact_instructions[i];
          if (q->op != TCCIR_OP_NOP)
            ssa_opt_scan_instr_uses(&ssa_opt_ctx, i, q);
        }
        ssa_opt_dce(&ssa_opt_ctx);
      }
    }
    tcc_ir_dump_after_pass(ir, "ssa:memmove_global_fwd");
    /* Park a global address that is reloaded across calls in a callee-saved
     * register for its whole live range, instead of reloading it from the
     * literal pool at each call-separated use.  Runs last so target fusions
     * have already consumed the load/store-indexed bases they hoist; -O2 only
     * (GCC likewise only parks globals at -O2).
     * docs/plans/gap_a_ssa_var_index_addr_prop.md (gap ②). */
    tcc_ir_ssa_opt_free(&ssa_opt_ctx);
  }
  dbg_scan_imm_dest(ir, "ssa_opt_block"); dbg_scan_overlap(ir, "ssa_opt_block");

  /* Set types from operand btypes (same as tcc_ir_live_analysis).
   * Skip lvalue operands: when is_lval=1 the vreg holds a pointer (32-bit)
   * and the btype describes the pointed-to value, not the pointer itself. */
  for (int i = 0; i < ir->next_instruction_index; i++) {
    IRQuadCompact *q = &ir->compact_instructions[i];
    IROperand dest = tcc_ir_op_get_dest(ir, q);
    if (irop_config[q->op].has_dest && tcc_ir_vreg_is_valid(ir, irop_get_vreg(dest)) && !dest.is_lval) {
      int btype = irop_get_btype(dest);
      if (btype == IROP_BTYPE_FLOAT32 || btype == IROP_BTYPE_FLOAT64)
        tcc_ir_vreg_type_set_fp(ir, irop_get_vreg(dest), 1, btype == IROP_BTYPE_FLOAT64);
      else if (btype == IROP_BTYPE_INT64)
        tcc_ir_vreg_type_set_64bit(ir, irop_get_vreg(dest));
      if (dest.is_complex)
        tcc_ir_vreg_type_set_complex(ir, irop_get_vreg(dest));
    }
    IROperand src1 = tcc_ir_op_get_src1(ir, q);
    if (irop_config[q->op].has_src1 && tcc_ir_vreg_is_valid(ir, irop_get_vreg(src1)) && !src1.is_lval) {
      int btype = irop_get_btype(src1);
      if (btype == IROP_BTYPE_FLOAT32 || btype == IROP_BTYPE_FLOAT64)
        tcc_ir_vreg_type_set_fp(ir, irop_get_vreg(src1), 1, btype == IROP_BTYPE_FLOAT64);
      else if (btype == IROP_BTYPE_INT64)
        tcc_ir_vreg_type_set_64bit(ir, irop_get_vreg(src1));
      if (src1.is_complex)
        tcc_ir_vreg_type_set_complex(ir, irop_get_vreg(src1));
    }
    IROperand src2 = tcc_ir_op_get_src2(ir, q);
    if (irop_config[q->op].has_src2 && tcc_ir_vreg_is_valid(ir, irop_get_vreg(src2)) && !src2.is_lval) {
      int btype = irop_get_btype(src2);
      if (btype == IROP_BTYPE_FLOAT32 || btype == IROP_BTYPE_FLOAT64)
        tcc_ir_vreg_type_set_fp(ir, irop_get_vreg(src2), 1, btype == IROP_BTYPE_FLOAT64);
      else if (btype == IROP_BTYPE_INT64)
        tcc_ir_vreg_type_set_64bit(ir, irop_get_vreg(src2));
      if (src2.is_complex)
        tcc_ir_vreg_type_set_complex(ir, irop_get_vreg(src2));
    }
  }

  /* Propagate types from phi nodes to their dest AND operand vregs.
   * SSA rename creates new TEMPs that may only appear with INT32 btype
   * in their defining instruction, but the phi btype reflects the
   * original variable's type.  Phi resolution will insert ASSIGN copies
   * with the phi btype, so codegen will expect 64-bit values from these
   * vregs even if their defs used INT32 btype. */
  if (ssa->block_phis) {
    for (int b = 0; b < cfg->num_blocks; b++) {
      for (IRPhiNode *phi = ssa->block_phis[b]; phi; phi = phi->next) {
        int is_fp = (phi->btype == IROP_BTYPE_FLOAT32 || phi->btype == IROP_BTYPE_FLOAT64);
        int is_i64 = (phi->btype == IROP_BTYPE_INT64);
        int is_dbl = (phi->btype == IROP_BTYPE_FLOAT64);
        /* Check original variable type as fallback */
        if (!is_fp && !is_i64 && phi->orig_vreg >= 0 &&
            tcc_ir_vreg_is_valid(ir, phi->orig_vreg)) {
          IRLiveInterval *orig_li = tcc_ir_vreg_live_interval(ir, phi->orig_vreg);
          if (orig_li) {
            if (orig_li->is_llong) is_i64 = 1;
            if (orig_li->is_float) { is_fp = 1; is_dbl = orig_li->is_double; }
          }
        }
        if (!is_fp && !is_i64) continue;

        /* Propagate to dest vreg */
        int32_t dv = phi->dest_vreg;
        if (dv >= 0 && tcc_ir_vreg_is_valid(ir, dv)) {
          if (is_fp) tcc_ir_vreg_type_set_fp(ir, dv, 1, is_dbl);
          if (is_i64) tcc_ir_vreg_type_set_64bit(ir, dv);
        }
        /* Propagate to all operand vregs (phi sources) */
        for (int pi = 0; pi < phi->num_operands; pi++) {
          int32_t ov = phi->operands[pi].vreg;
          if (ov >= 0 && tcc_ir_vreg_is_valid(ir, ov)) {
            if (is_fp) tcc_ir_vreg_type_set_fp(ir, ov, 1, is_dbl);
            if (is_i64) tcc_ir_vreg_type_set_64bit(ir, ov);
          }
        }
      }
    }
  }

  /* Resolve phis BEFORE register allocation: insert ASSIGN copies at
   * predecessor block ends so phi-DSTs and phi-SRCs become regular SSA
   * temps with non-overlapping intervals. Without this, the linear scan
   * sees phi-DST intervals that span the whole loop body alongside their
   * phi-SRC operands' intervals (also spanning the body), creating
   * artificial register pressure across loops. After this pass the IR
   * is no longer in SSA form; ra_build_intervals scans the explicit
   * copies and produces concrete intervals. */
  ra_phi_resolve_pre_ra_mode = 1;
  ra_resolve_phis(ir, cfg, ssa, &phi_stats);
  ra_phi_resolve_pre_ra_mode = 0;
  dbg_scan_imm_dest(ir,"ra_resolve_phis");

  /* Collapse "TMP <- const; T_phi <- TMP" chains the phi resolver leaves
   * behind in dense switch case bodies. Each fold drops one ASSIGN and one
   * SSA temp from the case body, cutting both the per-case instruction count
   * and the phi-temp live ranges that drive the linear scan into spills. */
  ra_fold_phi_const_chain(ir);
  dbg_scan_imm_dest(ir,"ra_fold_phi_const_chain");

  /* Once the per-case bodies are canonicalised to "T_phi <- const; JMP merge",
   * try to rewrite the entire SWITCH_TABLE dispatch into a single SWITCH_LOAD
   * against an inline value table.  Must run after the phi-const fold above
   * (which produces the canonical body shape) and before live-interval
   * construction (which would otherwise see the now-dead case bodies). */
  tcc_ir_opt_switch_to_data(ir);
  dbg_scan_imm_dest(ir,"switch_to_data");

  /* Fold CMP + JUMPIF where both operands resolve to constants within the
   * same basic block. Phi resolution often materializes the entry-path
   * constant of a loop counter right before its bound check; folding the
   * dead skip-loop block removes the carrier-vreg copies it contains and
   * cuts the carriers' live ranges, easing register pressure. */
  ra_fold_const_branches(ir);
  dbg_scan_imm_dest(ir,"ra_fold_const_branches");

  /* const_memcpy_fwd: by this point the SSA opt fold (ssa_opt_fold's
   * bit-complement / SCCP / GVN) has materialised compile-time-constant
   * aggregate values (e.g. pr60502's `*x |= *x ^ {-1,...}` → all-0xFF), and
   * the IR is de-SSA'd flat form.  Rewrite a constant-filled non-escaping
   * stack buffer copied by an aligned AEABI mem* helper into direct wide
   * constant stores to the destination, dropping the buffer + the call.  Must
   * run AFTER the SSA fold (which produces the constants) and BEFORE call
   * prefix / interval construction (which must see the call/stores removed).
   * The codegen STRD-imm peephole then pairs the word stores into `strd`. */
  if (tcc_state->optimize >= 1)
    tcc_ir_opt_const_memcpy_to_dest(ir);

  /* Park a global address reloaded across calls in a callee-saved register for
   * its whole live range instead of reloading it from the literal pool at each
   * call-separated use.  Runs here — after phi resolution de-SSA'd the IR — so
   * prepending entry materializations can't desync phi resolution; block_phis
   * is emptied, so rebuilding the CFG (which the inserts invalidate) is safe.
   * -O2 only (GCC likewise only parks globals at -O2).
   * docs/plans/gap_a_ssa_var_index_addr_prop.md (gap ②). */
  if (tcc_state && tcc_state->optimize >= 2 &&
      !tcc_ir_opt_pass_disabled("ssa:global_addr_hoist") &&
      tcc_ir_ssa_opt_global_addr_hoist(ir) > 0) {
    tcc_ir_cfg_free(cfg);
    cfg = tcc_ir_cfg_build(ir);
    tcc_ir_cfg_compute_dominators(cfg);
    ssa->cfg = cfg;
    /* block_phis is sized to the old block count and consumed by
     * ra_build_intervals; the fresh CFG may have a different count. It is
     * all-NULL post-resolution, so resize and zero it to the new count. */
    ssa->block_phis = tcc_realloc(ssa->block_phis, cfg->num_blocks * sizeof(IRPhiNode *));
    memset(ssa->block_phis, 0, cfg->num_blocks * sizeof(IRPhiNode *));
  }
  tcc_ir_dump_after_pass(ir, "ssa:global_addr_hoist");

  /* Build call prefix for call-crossing detection */
  int *call_prefix = ra_build_call_prefix(ir);

  /* Build SSA live intervals */
  SSAInterval *intervals = NULL;
  int interval_count = 0;
  int max_vreg_pos = 0;
  ra_build_intervals(ir, cfg, ssa, &intervals, &interval_count, call_prefix, &max_vreg_pos);
  ra_build_narrow_weights(ir, target, intervals, interval_count, max_vreg_pos);

  /* Build phi register hints. block_phis is empty after pre-RA resolution,
   * so the phi-based pass is a no-op; the assign-based pass picks up
   * the explicit copies emitted at predecessor block ends. */
  ra_build_phi_hints(intervals, interval_count, ssa, cfg, max_vreg_pos);
  ra_build_assign_hints(intervals, interval_count, ir, max_vreg_pos);
  ra_build_load_param_hints(intervals, interval_count, ir, max_vreg_pos);
  ra_build_bfi_hints(intervals, interval_count, ir, max_vreg_pos);
  ra_build_outgoing_param_hints(intervals, interval_count, ir, max_vreg_pos);

  /* Graph-based coalescing (accurate liveness + interference) — merges
   * copy-related non-interfering vregs, including multi-predecessor merge-phis
   * the in-scan transfer cannot handle.  Gated by TCC_COALESCE. */
  ra_coalesce_graph(ir, intervals, interval_count, max_vreg_pos);

  /* Run linear scan */
  uint64_t dirty_int = 0, dirty_fp = 0;
  int n_instr = ir->next_instruction_index;
  int has_call = call_prefix && n_instr > 0 && call_prefix[n_instr] > 0;
  ra_linear_scan(ir, intervals, interval_count, target, spill_base, &dirty_int, &dirty_fp,
                 max_vreg_pos, has_call);

  /* Propagate each coalesced member's allocation from its representative (which
   * the scan allocated; members were skipped).  coalesce_to holds the rep's
   * vreg (stable across the scan's qsort); resolve via a vreg->interval search. */
  {
    int any = 0;
    for (int i = 0; i < interval_count && !any; i++)
      if (intervals[i].coalesce_to >= 0) any = 1;
    if (any) {
      for (int i = 0; i < interval_count; i++) {
        if (intervals[i].coalesce_to < 0) continue;
        for (int j = 0; j < interval_count; j++) {
          if (intervals[j].vreg != intervals[i].coalesce_to || intervals[j].coalesce_to >= 0)
            continue;
          intervals[i].r0 = intervals[j].r0;
          intervals[i].r1 = intervals[j].r1;
          intervals[i].stack_location = intervals[j].stack_location;
          break;
        }
      }
    }
  }

  if (TCC_LOG_LS) {
    for (int i = 0; i < interval_count; i++) {
      if (intervals[i].stack_location != 0 &&
          ra_phi_stats_has_participant(&phi_stats, intervals[i].vreg))
        phi_stats.participant_spills++;
    }
    LOG_LS("phi_stats operands=%d parallel_requested=%d parallel_emitted=%d cycle_temporaries=%d participant_spills=%d",
           phi_stats.phi_operands, phi_stats.parallel_requested,
           phi_stats.parallel_emitted, phi_stats.cycle_temporaries,
           phi_stats.participant_spills);
  }

  /* Write results to IR + LS state */
  ra_write_results(ir, intervals, interval_count);
  ir->ls.dirty_registers = dirty_int;
  ir->ls.dirty_float_registers = dirty_fp;

  /* Phi resolution already happened before ra_build_intervals (above).
   * The instruction stream now has explicit ASSIGN copies; ssa->block_phis
   * is cleared. We just need to build the live_regs bitmap from the
   * intervals the linear scan produced. */
  ra_build_live_regs_bitmap(ir);
  ra_refine_live_regs_accurate(ir);

  /* Strip dead phi copies now that allocation and every liveness/scratch
   * bitmap are final.  A phi copy into a never-read temp is dead code whose
   * register the scan may have reused for a value live across the merge; left
   * in, it clobbers that value (seed 198468).  Running it here — after the
   * bitmaps — keeps the allocation identical and only drops dead instructions. */
  ra_eliminate_dead_reg_copies(ir);

  ra_repair_incomplete_calls(ir);

  ra_mark_rematerializable(ir);

  /* Cleanup */
  tcc_free(phi_stats.participants);
  tcc_free(intervals);
  if (call_prefix) tcc_free(call_prefix);
  tcc_ir_ssa_free(ssa);
  tcc_ir_cfg_free(cfg);
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

static int ra_count_copies_reaching_codegen(TCCIRState *ir)
{
  int copies = 0;

  for (int i = 0; i < ir->next_instruction_index; i++) {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_ASSIGN)
      continue;

    IROperand dest = tcc_ir_op_get_dest(ir, q);
    IROperand src = tcc_ir_op_get_src1(ir, q);
    int32_t dest_vreg = irop_get_vreg(dest);
    int32_t src_vreg = irop_get_vreg(src);
    if (dest.is_lval || src.is_lval || dest_vreg < 0 || src_vreg < 0)
      continue;
    if (irop_get_btype(dest) != irop_get_btype(src)) {
      copies++;
      continue;
    }

    IRLiveInterval *dest_li = tcc_ir_vreg_live_interval(ir, dest_vreg);
    IRLiveInterval *src_li = tcc_ir_vreg_live_interval(ir, src_vreg);
    if (!ra_phi_copy_is_identity(dest_li, src_li))
      copies++;
  }

  return copies;
}

int tcc_ir_move_coalescing(TCCIRState *ir)
{
  LSLiveIntervalState *ls = &ir->ls;
  if (!ls->live_regs_by_instruction || ls->live_regs_by_instruction_size <= 0) {
    if (TCC_LOG_LS)
      LOG_LS("copy_stats coalesced=0 reaching_codegen=%d",
             ra_count_copies_reaching_codegen(ir));
    return 0;
  }

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

  if (TCC_LOG_LS)
    LOG_LS("copy_stats coalesced=%d reaching_codegen=%d", coalesced,
           ra_count_copies_reaching_codegen(ir));

  return coalesced;
}
