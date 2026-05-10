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
#include "licm.h"

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
  uint8_t reg_type;
  uint16_t use_count;
  int8_t precolored;
  int8_t pref_reg; /* soft hint: prefer this physical reg if available (e.g. r0 for RETURNVALUE feeders) */
  int32_t hint_vreg;
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
    TccIrOp op = ir->compact_instructions[i].op;
    int is_call = (op == TCCIR_OP_FUNCCALLVOID || op == TCCIR_OP_FUNCCALLVAL ||
                   op == TCCIR_OP_BUILTIN_APPLY || ir_op_is_implicit_call_ra(op));
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
      int dest_is_use = (q->op == TCCIR_OP_STORE || q->op == TCCIR_OP_STORE_INDEXED ||
                         q->op == TCCIR_OP_STORE_POSTINC);
      IROperand d = tcc_ir_op_get_dest(ir, q);
      int32_t vr = irop_get_vreg(d);
      if (vr >= 0 && tcc_ir_vreg_is_valid(ir, vr)) {
        int idx = VREG_IDX(vr);
        if (idx < table_size) {
          if (starts[idx] == INTERVAL_NOT_STARTED)
            starts[idx] = dest_is_use ? 0 : i;
          if (ends[idx] < (uint32_t)i) ends[idx] = i;
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

  /* Extend FUNCPARAMVAL intervals to their FUNCCALL */
  {
    int max_call_id = ir->next_call_id;
    int *call_idx_by_id = NULL;
    if (max_call_id > 0) {
      call_idx_by_id = tcc_malloc(sizeof(int) * max_call_id);
      for (int i = 0; i < max_call_id; i++) call_idx_by_id[i] = -1;
      for (int i = 0; i < n; i++) {
        IRQuadCompact *q = &ir->compact_instructions[i];
        if (q->op == TCCIR_OP_FUNCCALLVOID || q->op == TCCIR_OP_FUNCCALLVAL) {
          int cid = TCCIR_DECODE_CALL_ID(irop_get_imm64_ex(ir, tcc_ir_op_get_src2(ir, q)));
          if (cid >= 0 && cid < max_call_id) call_idx_by_id[cid] = i;
        }
      }
      for (int i = 0; i < n; i++) {
        IRQuadCompact *q = &ir->compact_instructions[i];
        if (q->op != TCCIR_OP_FUNCPARAMVAL) continue;
        int cid = TCCIR_DECODE_CALL_ID(irop_get_imm64_ex(ir, tcc_ir_op_get_src2(ir, q)));
        if (cid < 0 || cid >= max_call_id) continue;
        int cidx = call_idx_by_id[cid];
        if (cidx < 0) continue;
        IROperand s1 = tcc_ir_op_get_src1(ir, q);
        int32_t vr = irop_get_vreg(s1);
        if (vr >= 0 && tcc_ir_vreg_is_valid(ir, vr)) {
          int idx = VREG_IDX(vr);
          if (idx < table_size) {
            if (ends[idx] < (uint32_t)cidx) ends[idx] = cidx;
            if (starts[idx] == INTERVAL_NOT_STARTED) starts[idx] = 0;
          }
        }
      }
      tcc_free(call_idx_by_id);
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
          uint32_t old_s_ = starts[idx_];                                       \
          starts[idx_] = (target);                                              \
          RA_DBG("    %s%d [%u,%u] -> [%u,%u] (loop-carried)",                 \
                 ra_vreg_type_char(type_), idx_ % max_vreg_pos, old_s_,         \
                 ends[idx_], starts[idx_], ends[idx_]);                          \
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
      iv->is_param = (type == TCCIR_VREG_TYPE_PARAM);

      IRLiveInterval *li = tcc_ir_vreg_live_interval(ir, vreg);
      iv->addrtaken = li->addrtaken;
      iv->reg_type = tcc_ir_vreg_type_get(ir, vreg);

      /* Static chain vreg */
      if (ir->has_static_chain && vreg == ir->static_chain_vreg) {
        iv->end = n;
        iv->crosses_call = 1;
        iv->precolored = 10;
      }

      /* Call crossing */
      if (!iv->crosses_call) {
        iv->crosses_call = ra_has_call_in_range(call_prefix, iv->start, iv->end, n);
        if (!iv->crosses_call && iv->end < (uint32_t)n) {
          TccIrOp eop = ir->compact_instructions[iv->end].op;
          if (eop == TCCIR_OP_FUNCCALLVAL || eop == TCCIR_OP_FUNCCALLVOID)
            iv->crosses_call = 1;
        }
      }

      /* Params: start at 0, precolor if in register */
      if (type == TCCIR_VREG_TYPE_PARAM) {
        iv->start = 0;
        if (iv->end == 0) iv->end = 1;
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
  return 0;
}

static void ra_linear_scan(SSAInterval *intervals, int count,
                           const RegAllocTarget *target, int spill_base,
                           uint64_t *out_dirty_int, uint64_t *out_dirty_fp,
                           int max_vreg_pos)
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

  uint64_t int_free = int_allowed;
  uint64_t fp_free = fp_allowed;
  uint64_t dirty_int = 0;
  uint64_t dirty_fp = 0;

  /* Active set sorted by end point */
  SSAInterval **active = tcc_malloc(sizeof(SSAInterval *) * count);
  int active_count = 0;

  int spill_loc = spill_base;

  for (int i = 0; i < count; i++) {
    SSAInterval *cur = &intervals[i];

    /* Expire old intervals */
    int w = 0;
    for (int j = 0; j < active_count; j++) {
      SSAInterval *a = active[j];
      if (a->end < cur->start) {
        /* Free register */
        if (a->r0 >= 0 && a->stack_location == 0) {
          if (a->reg_type == LS_REG_TYPE_FLOAT || a->reg_type == LS_REG_TYPE_DOUBLE) {
            fp_free |= (1ull << a->r0);
            if (a->r1 >= 0) fp_free |= (1ull << a->r1);
          } else {
            int_free |= (1ull << a->r0);
            if (a->r1 >= 0) int_free |= (1ull << a->r1);
          }
        }
      } else {
        active[w++] = a;
      }
    }
    active_count = w;

    /* Address-taken: force spill */
    if (cur->addrtaken) {
      spill_loc -= 4;
      cur->stack_location = spill_loc;
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
      if (cur->crosses_call && target->int_class.pair_align) {
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
    RA_DBG("  alloc T%d [%u,%u] xcall=%d int_free=0x%llx active=%d",
           TCCIR_DECODE_VREG_POSITION(cur->vreg), cur->start, cur->end,
           cur->crosses_call, (unsigned long long)int_free, active_count);

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
    if (cur->hint_vreg >= 0 && max_vreg_pos > 0) {
      int hint_idx = HINT_IDX(cur->hint_vreg);
      if (hint_idx >= 0 && hint_idx < hint_tbl_size) {
        SSAInterval *partner = vreg_to_iv[hint_idx];
        if (partner && partner->r0 >= 0 && partner->r1 < 0 &&
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
    if (reg < 0 && !cur->crosses_call) {
      /* Try caller-saved first, then callee-saved (only for non-call-crossing) */
      static const int alloc_order[] = {0, 1, 2, 3, 12, 4, 5, 6, 7, 8, 9, 10, 11};
      for (int oi = 0; oi < 13; oi++) {
        int r = alloc_order[oi];
        if (r >= tcc_state->registers_for_allocator) continue;
        if (int_free & (1ull << r)) { reg = r; break; }
      }
    }

    if (reg >= 0) {
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
   * any that turn out to land in the same physical register. */
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
                                         int *phi_spill_cursor)
{
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
      ra_emit_phi_copy(ir, new_instrs, wp, pool_wp, &save, records, record_count);
      copies[ci].src_vreg = tmp_vreg;
    }
  }
}

static void ra_build_live_regs_bitmap(TCCIRState *ir);

static void ra_resolve_phis(TCCIRState *ir, IRCFG *cfg, IRSSAState *ssa)
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
        int pred = phi->operands[pi].pred_block;
        if (pred < 0 || pred >= nb) continue;
        if (phi->operands[pi].vreg < 0) continue;
        if (!ra_phi_copy_needed(ir, phi, pi)) continue;
        copies_per_block[pred]++;
        total_copies++;
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
                                     copy_records, &copy_record_count, &phi_spill_cursor);
        tcc_free(copies);

        skip_dest = ir->iroperand_pool[skip_dest_pool_idx];
        skip_dest.u.imm32 = -(wp + 2);
        ir->iroperand_pool[skip_dest_pool_idx] = skip_dest;

        ir->iroperand_pool[pool_wp] = old_dest;
        new_instrs[wp].op = TCCIR_OP_JUMP;
        new_instrs[wp].operand_base = pool_wp;
        new_instrs[wp].line_num = term->line_num;
        new_instrs[wp].is_jump_target = 0;
        wp++;
        pool_wp++;
      } else {
        new_instrs[wp++] = *term;
      }

      if (fallthrough_count > 0) {
        RAPhiCopy *copies = tcc_malloc(sizeof(RAPhiCopy) * fallthrough_count);
        int copy_count = ra_collect_phi_copies_for_pred(ir, cfg, ssa, b, fallthrough_block, copies);
        ra_emit_scheduled_phi_copies(ir, new_instrs, &wp, &pool_wp, copies, copy_count, b,
                                     copy_records, &copy_record_count, &phi_spill_cursor);
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
                                   copy_records, &copy_record_count, &phi_spill_cursor);
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
    for (int b = 0; b < nb; b++)
      ssa->block_phis[b] = NULL;
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
 * Entry Point
 * ============================================================================ */

void tcc_ir_ssa_regalloc(TCCIRState *ir, const RegAllocTarget *target, int spill_base)
{
  if (!ir || !target) return;

  /* Build CFG + dominators */
  IRCFG *cfg = tcc_ir_cfg_build(ir);
  if (!cfg) {
    /* Fallback: no CFG means trivial function, use old allocator path */
    return;
  }
  tcc_ir_cfg_compute_dominators(cfg);
  tcc_ir_cfg_compute_dom_frontiers(cfg);

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
        ssa_opt_ctx.no_stack_fwd = 0;
        ssa_opt_var_const_fold(&ssa_opt_ctx);
        ssa_opt_var_forward(&ssa_opt_ctx);
        ssa_opt_sccp(&ssa_opt_ctx);
        ssa_opt_load_cse(&ssa_opt_ctx);
        ssa_opt_cprop(&ssa_opt_ctx);
        ssa_opt_fold(&ssa_opt_ctx);
        ssa_opt_branch(&ssa_opt_ctx);
        ssa_opt_reassoc(&ssa_opt_ctx);
        ssa_opt_strength(&ssa_opt_ctx);
        ssa_opt_narrow(&ssa_opt_ctx);
        ssa_opt_gvn(&ssa_opt_ctx);
        ssa_opt_phi_simplify(&ssa_opt_ctx);
        ssa_opt_dce(&ssa_opt_ctx);
      }
    } else {
      ssa_opt_cprop(&ssa_opt_ctx);
      ssa_opt_dce(&ssa_opt_ctx);
    }
    tcc_ir_ssa_opt_free(&ssa_opt_ctx);
  }

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
  ra_resolve_phis(ir, cfg, ssa);
  ra_phi_resolve_pre_ra_mode = 0;

  /* Fold CMP + JUMPIF where both operands resolve to constants within the
   * same basic block. Phi resolution often materializes the entry-path
   * constant of a loop counter right before its bound check; folding the
   * dead skip-loop block removes the carrier-vreg copies it contains and
   * cuts the carriers' live ranges, easing register pressure. */
  ra_fold_const_branches(ir);

  /* Build call prefix for call-crossing detection */
  int *call_prefix = ra_build_call_prefix(ir);

  /* Build SSA live intervals */
  SSAInterval *intervals = NULL;
  int interval_count = 0;
  int max_vreg_pos = 0;
  ra_build_intervals(ir, cfg, ssa, &intervals, &interval_count, call_prefix, &max_vreg_pos);

  /* Build phi register hints. block_phis is empty after pre-RA resolution,
   * so the phi-based pass is a no-op; the assign-based pass picks up
   * the explicit copies emitted at predecessor block ends. */
  ra_build_phi_hints(intervals, interval_count, ssa, cfg, max_vreg_pos);
  ra_build_assign_hints(intervals, interval_count, ir, max_vreg_pos);

  /* Run linear scan */
  uint64_t dirty_int = 0, dirty_fp = 0;
  ra_linear_scan(intervals, interval_count, target, spill_base, &dirty_int, &dirty_fp, max_vreg_pos);

  /* Write results to IR + LS state */
  ra_write_results(ir, intervals, interval_count);
  ir->ls.dirty_registers = dirty_int;
  ir->ls.dirty_float_registers = dirty_fp;

  /* Phi resolution already happened before ra_build_intervals (above).
   * The instruction stream now has explicit ASSIGN copies; ssa->block_phis
   * is cleared. We just need to build the live_regs bitmap from the
   * intervals the linear scan produced. */
  ra_build_live_regs_bitmap(ir);

  /* Cleanup */
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
    if (q->op != TCCIR_OP_ASSIGN)
      continue;

    const IROperand src1 = tcc_ir_op_get_src1(ir, q);
    const IROperand dest = tcc_ir_op_get_dest(ir, q);
    if (src1.is_lval || dest.is_lval) continue;
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
    if (src_iv->r0 < 0 || dst_iv->r0 < 0) continue;
    if (src_iv->stack_location != 0 || dst_iv->stack_location != 0) continue;
    if (src_iv->r0 == dst_iv->r0) continue;

    /* Forward direction: reassign dest to use src's register.
     * Requires src to die at this ASSIGN. */
    if (src_iv->end == (uint32_t)i) {
      int src_reg = src_iv->r0;
      if (dst_iv->crosses_call && !(src_reg >= 4 && src_reg <= 11))
        goto try_reverse;

      int conflict = 0;
      for (int k = i + 1; k <= (int)dst_iv->end && k < tbl_size; ++k)
      {
        if (ls->live_regs_by_instruction[k] & (1u << src_reg))
        { conflict = 1; break; }
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
          ls->live_regs_by_instruction[k] &= ~(1u << old_reg);
          ls->live_regs_by_instruction[k] |= (1u << src_reg);
        }
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
rev_check_done:
    if (conflict) continue;

    /* Check dest_reg not occupied by other intervals during src's range */
    for (int k = (int)src_iv->start; k <= (int)src_iv->end && k < tbl_size; ++k)
    {
      if (ls->live_regs_by_instruction[k] & (1u << dest_reg))
      {
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
      ls->live_regs_by_instruction[k] &= ~(1u << old_reg);
      ls->live_regs_by_instruction[k] |= (1u << dest_reg);
    }
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
