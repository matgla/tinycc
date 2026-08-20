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
#include "mem_ssa.h"
#include "ssa_opt.h"
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
#include "opt/ssa/vrp.h"
#include "opt/ssa/setif_or_taut.h"
#include "opt/ssa/bool_norm.h"
#include "opt/ssa/cmp_offset_fold.h"
#include "opt_pipeline.h"
#include "opt.h"
#include "licm.h"
#include "opt_loop_utils.h"

#include "memory/bitspan.h"
#include "memory/bit_matrix.h"


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
  uint8_t crosses_real_call : 1; /* crosses a real FUNCCALL (VFP caller-saved clobber) — excludes native FP-op implicit calls */
  uint8_t addrtaken : 1;
  uint8_t is_volatile : 1; /* volatile-qualified local: force to a stack slot so every access is a real ldr/str */
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

/* Prefix of REAL calls only — user function calls, __aeabi helpers (all
 * FUNCCALLVAL/VOID), BUILTIN_APPLY, and memcpy-sized block copies.  Unlike
 * ra_build_call_prefix it excludes native FP-op implicit "calls" (a single-
 * precision vadd.f32 clobbers nothing), so it answers "does this value cross a
 * call that clobbers the caller-saved VFP registers?". */
static int *ra_build_real_call_prefix(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  if (n <= 0)
    return NULL;
  int *prefix = tcc_malloc(sizeof(int) * (n + 1));
  prefix[0] = 0;
  for (int i = 0; i < n; i++) {
    IRQuadCompact *q = &ir->compact_instructions[i];
    TccIrOp op = q->op;
    int is_call = (op == TCCIR_OP_FUNCCALLVOID || op == TCCIR_OP_FUNCCALLVAL || op == TCCIR_OP_BUILTIN_APPLY);
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
 * SSA Live Interval Building
 * ============================================================================ */

/* Defined with the graph coalescer below; used by the back-edge extension to
 * classify the instruction at a jump target. */
static void ra_co_ops(TCCIRState *ir, IRQuadCompact *q,
                      int32_t *out_def, int *has_def, int32_t uses[4], int *nuse);

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
  int *real_call_prefix = ra_build_real_call_prefix(ir);

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
        if (li && !li->addrtaken && !li->is_volatile && !li->is_lvalue && read_as_src)
          dest_is_use = 0;
      }
      /* TEMP-dest plain STORE with is_lval clear: SSA renaming rewrites a
       * promoted var's in-place slot store to its current temp name and
       * CLEARS the lval/local flags (a pointer-deref store keeps is_lval).
       * That is a register write, not a memory write — without a def here
       * the temp reads as live-from-entry, and a block-local scratch (an
       * inlined helper's per-case var) balloons across the whole loop and
       * detonates pressure.  Full-width writes only: a narrow store leaves
       * the temp's upper bytes live-before (partial def). */
      else if (q->op == TCCIR_OP_STORE && vr >= 0 && tcc_ir_vreg_is_valid(ir, vr) &&
               TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_TEMP &&
               !d.is_lval && !d.is_local &&
               (d.btype == IROP_BTYPE_INT32 || d.btype == IROP_BTYPE_FLOAT32 ||
                d.btype == IROP_BTYPE_INT64 || d.btype == IROP_BTYPE_FLOAT64)) {
        int didx = VREG_IDX(vr);
        int read_as_src = didx < table_size &&
                          ((vreg_read_as_src[didx >> 3] >> (didx & 7)) & 1);
        if (read_as_src)
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
  /* Resolve each FUNCPARAMVAL's paired CALL in one backward sweep instead of
   * a forward scan per PARAM: the scan was O(params x distance-to-call), and
   * limits-fnargs (thousands of arguments per call) spent 17.6 s in it.
   * Walking backward, next_call_by_cid[cid] always holds the nearest CALL
   * after the current instruction, which is exactly what the forward scan
   * found. The consuming loop below stays forward: the param_extended check
   * reads ends[idx] mid-iteration, so processing order is semantics. */
  int *param_cidx = tcc_malloc(sizeof(int) * (n > 0 ? n : 1));
  {
    int max_cid = -1;
    for (int i = 0; i < n; i++) {
      IRQuadCompact *q = &ir->compact_instructions[i];
      if (q->op != TCCIR_OP_FUNCCALLVOID && q->op != TCCIR_OP_FUNCCALLVAL) continue;
      int ccid = TCCIR_DECODE_CALL_ID(irop_get_imm64_ex(ir, tcc_ir_op_get_src2(ir, q)));
      if (ccid > max_cid) max_cid = ccid;
    }
    int *next_call_by_cid = NULL;
    if (max_cid >= 0) {
      next_call_by_cid = tcc_malloc(sizeof(int) * (max_cid + 1));
      for (int k = 0; k <= max_cid; k++) next_call_by_cid[k] = -1;
    }
    for (int i = n - 1; i >= 0; i--) {
      IRQuadCompact *q = &ir->compact_instructions[i];
      param_cidx[i] = -1;
      if (q->op == TCCIR_OP_FUNCCALLVOID || q->op == TCCIR_OP_FUNCCALLVAL) {
        int ccid = TCCIR_DECODE_CALL_ID(irop_get_imm64_ex(ir, tcc_ir_op_get_src2(ir, q)));
        if (ccid >= 0 && ccid <= max_cid) next_call_by_cid[ccid] = i;
      } else if (q->op == TCCIR_OP_FUNCPARAMVAL) {
        int cid = TCCIR_DECODE_CALL_ID(irop_get_imm64_ex(ir, tcc_ir_op_get_src2(ir, q)));
        if (cid >= 0 && cid <= max_cid && next_call_by_cid)
          param_cidx[i] = next_call_by_cid[cid];
      }
    }
    if (next_call_by_cid) tcc_free(next_call_by_cid);
  }

  for (int i = 0; i < n; i++) {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_FUNCPARAMVAL) continue;
    int cidx = param_cidx[i];
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
          if (li && !li->addrtaken && !li->is_volatile && !li->is_lvalue)
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

  tcc_free(param_cidx);

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
          /* A single-def TEMP whose interval STARTS at the jump target is    \
           * defined fresh on every arrival — nothing is carried across the  \
           * edge, so extending it to the jump only manufactures overlap.    \
           * (A switch dispatching to linearly-earlier case blocks ballooned \
           * every case temp to the dispatch: 8 pairs live at once, spill    \
           * storm — ashrdi-1 constant_shift's 488-byte frame.)  Keep        \
           * extending when the def at the target also READS the vreg (RMW   \
           * carry), or a phi-resolution ASSIGN writes it inside [target,i)  \
           * (loop-carried phi dest), or for broad/IJUMP edges.  VARs keep   \
           * the old conservative behavior (multi-def cross-block flow).  */ \
          int fresh_def_ = 0;                                                   \
          if (!(broad) && (int)starts[idx_] == (target) &&                      \
              type_ == TCCIR_VREG_TYPE_TEMP) {                                  \
            int32_t nv_ = TCCIR_ENCODE_VREG(type_, idx_ % max_vreg_pos);        \
            int32_t d_ = -1, u_[4]; int hd_ = 0, nu_ = 0;                       \
            ra_co_ops(ir, &ir->compact_instructions[(target)], &d_, &hd_,       \
                      u_, &nu_);                                                \
            if (hd_ && d_ == nv_) {                                             \
              int reads_self_ = 0;                                              \
              for (int k_ = 0; k_ < nu_; k_++)                                  \
                if (u_[k_] == nv_) reads_self_ = 1;                             \
              if (!reads_self_ &&                                               \
                  !RA_VREG_HAS_ASSIGN_IN_RANGE(idx_, (target), (i)))            \
                fresh_def_ = 1;                                                 \
            }                                                                   \
          }                                                                     \
          if (!fresh_def_) {                                                    \
            uint32_t old_e_ = ends[idx_];                                       \
            ends[idx_] = (i);                                                   \
            RA_DBG("    %s%d [%u,%u] -> [%u,%u]", ra_vreg_type_char(type_),    \
                   idx_ % max_vreg_pos, starts[idx_], old_e_, starts[idx_],     \
                   ends[idx_]);                                                  \
          }                                                                     \
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
      iv->is_volatile = li->is_volatile;
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
      iv->crosses_real_call = ra_has_call_in_range(real_call_prefix, iv->start, iv->end, n) ? 1 : 0;
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
        if (!iv->crosses_call && li->incoming_reg0 >= 0) {
          /* GPR params: up to 4 argument registers (r0-r3).  Hard-float float
           * params arrive in VFP argument registers (s0-s15) — precolor those
           * too so they land in their incoming register. */
          if (LS_IS_VFP_REG(li->incoming_reg0)) {
            if (LS_VFP_REG_NUM(li->incoming_reg0) < 16)
              iv->precolored = li->incoming_reg0;
          } else if (pos < 4) {
            iv->precolored = li->incoming_reg0;
          }
        }
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
  if (real_call_prefix) tcc_free(real_call_prefix);
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
      if (!li || li->addrtaken || li->is_volatile || li->is_lvalue) continue;
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

/* Pre-allocation frame-pointer prediction.  The real decision is made after
 * allocation — tcc_ir_codegen_generate forces FP for VLA, soft-float ops and
 * static chain; the Thumb prologue for variadic functions and force_lr_save —
 * so freeing the FP register for allocation is safe only when none of those
 * can fire.  This must remain a SUPERSET of every post-allocation forcing
 * site: add the condition HERE FIRST when adding one there.  Inline asm does
 * not force a frame pointer, but its text can name r7 outright, which the
 * allocator cannot see — keep the register reserved in such functions. */
/* ============================================================================
 * Accurate per-vreg liveness, for the allocator's last-resort register share
 *
 * ra_build_intervals models every value as ONE contiguous [start,end] range.
 * That is a strict over-approximation across a diamond: a value defined above
 * an if/else and read only in ONE arm looks live in the other arm too, so the
 * linear scan sees a false conflict and spills a value that could have had a
 * register for free.  `__aeabi_dadd`'s alignment block is the shape: `exp_diff`
 * is read in the `exp_diff > 0` arm and again in the `exp_diff < 0` arm, so its
 * register looks busy in BOTH -- and the 64-bit temp defined there was written
 * to the frame and read back at the very next instruction.
 *
 * This computes real liveness (backward dataflow over the CFG, the same
 * def/use model the graph coalescer trusts, deferred PARAM sources included)
 * and records, per vreg, every instruction index at which it is live-in or
 * live-out.  ra_linear_scan consults it ONLY when it is about to spill: if an
 * active interval is provably dead across the whole of the new interval's
 * range, the two cannot conflict on any path and can share one register.
 *
 * Unknown answers are reported BUSY, so a failure to build (un-enumerated CFG
 * edges, a vreg outside the table, an oversized function) can only cost a
 * sharing opportunity, never create a clobber.
 * ========================================================================== */
typedef struct RaAliveInfo
{
  int valid;
  int n;      /* instruction indices covered: [0, n) */
  int tbl;    /* vreg slot table size */
  int maxpos;
  int words;  /* uint64 words per vreg row */
  int ndense;
  int *dense;     /* tbl entries: slot -> row index, or -1 */
  uint64_t *rows; /* ndense rows of `words` words */
} RaAliveInfo;

#define RA_ALIVE_VIDX(info, vr) \
  ((TCCIR_DECODE_VREG_TYPE(vr) * (info)->maxpos) + TCCIR_DECODE_VREG_POSITION(vr))

/* Cap: a row per vreg over the whole instruction range.  Beyond this the
 * bitmap is not worth the compile time or the memory, and the allocator just
 * keeps its previous behaviour. */
#define RA_ALIVE_MAX_WORDS (1 << 18) /* 2 MiB */

/* Row of instruction indices at which `vreg` is live (live-in or live-out),
 * or NULL when the answer is not known. */
static const uint64_t *ra_alive_row(const RaAliveInfo *info, int32_t vreg)
{
  if (!info || !info->valid)
    return NULL;
  int vi = RA_ALIVE_VIDX(info, vreg);
  if (vi < 0 || vi >= info->tbl)
    return NULL;
  int d = info->dense[vi];
  if (d < 0)
    return NULL;
  return info->rows + (size_t)d * (size_t)info->words;
}

/* 1 if `vreg` may be live anywhere in [lo,hi].  Conservative: unknown -> 1.
 *
 * The range asked about is the NEW interval's [start,end] -- the window over
 * which the allocator guarantees it the register -- and NOT its accurate live
 * set.  Those differ: the interval over-approximates, and a donor live inside
 * one of its holes has still had its value destroyed by the def at `start`.
 * Testing the two live SETS against each other instead looks stronger and is
 * wrong; it miscompiles bench_double.c's Mandelbrot kernel. */
static int ra_alive_busy(const RaAliveInfo *info, int32_t vreg, uint32_t lo, uint32_t hi)
{
  const uint64_t *row = ra_alive_row(info, vreg);
  if (!row)
    return 1;
  if (hi >= (uint32_t)info->n)
    hi = (uint32_t)info->n - 1;
  if (lo > hi)
    return 1;
  for (uint32_t k = lo; k <= hi; k++)
  {
    uint32_t w = k >> 6;
    if ((k & 63) == 0 && row[w] == 0 && k + 63 <= hi)
    {
      k += 63;
      continue;
    }
    if (row[w] & (1ull << (k & 63)))
      return 1;
  }
  return 0;
}

static void ra_alive_free(RaAliveInfo *info)
{
  if (!info)
    return;
  tcc_free(info->dense);
  tcc_free(info->rows);
  memset(info, 0, sizeof *info);
}

/* Cheap pre-check so the dataflow is not built for a function that could never
 * use it.  A graph-coalesced class is disqualifying by construction: the
 * representative's register carries the whole class, which its own vreg's
 * liveness does not describe.  The return-block share is per-interval and is
 * handled at the donor (`reg_shared`) plus an interlock in the scan. */
static int ra_alive_worth_building(TCCIRState *ir, const SSAInterval *intervals, int count)
{
  if (tcc_state->optimize < 1)
    return 0;
  if (tcc_ir_opt_pass_disabled("ra:alive_share"))
    return 0;
  for (int i = 0; i < count; i++)
    if (intervals[i].coalesce_to >= 0 || intervals[i].co_member)
      return 0;
  /* A back-edge is disqualifying, and MEASURED so: with loops admitted
   * double_add improves a further 1.9% and bench_double.c's Mandelbrot kernel
   * MISCOMPILES.  A loop puts values on a register that no single vreg's
   * liveness describes -- the loop-phi absorb is only the visible half -- so
   * the donor filters are not sufficient there. */
  const int n = ir->next_instruction_index;
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_JUMP && q->op != TCCIR_OP_JUMPIF)
      continue;
    int t = (int)irop_get_imm64_ex(ir, tcc_ir_op_get_dest(ir, q));
    if (t >= 0 && t <= i)
      return 0;
  }
  return 1;
}

static void ra_alive_build(TCCIRState *ir, RaAliveInfo *info, int max_vreg_pos)
{
  memset(info, 0, sizeof *info);
  const int n = ir->next_instruction_index;
  if (n <= 0 || max_vreg_pos <= 0)
    return;
  if (tcc_state->optimize < 1)
    return;
  /* Un-enumerated edges make the dataflow unsound; same guard the coalescer
   * and the scratch-liveness refinement use. */
  for (int i = 0; i < n; i++)
  {
    int op = ir->compact_instructions[i].op;
    if (op == TCCIR_OP_IJUMP || op == TCCIR_OP_SWITCH_TABLE || op == TCCIR_OP_SWITCH_LOAD)
      return;
  }

  IRCFG *cfg = tcc_ir_cfg_build(ir);
  if (!cfg)
    return;
  tcc_ir_cfg_compute_rpo(cfg);
  const int nb = cfg->num_blocks;
  if (nb <= 0)
  {
    tcc_ir_cfg_free(cfg);
    return;
  }

  const int maxpos = max_vreg_pos;
  const int tbl = 4 * maxpos;
  #define AVIDX(vr) ((TCCIR_DECODE_VREG_TYPE(vr) * maxpos) + TCCIR_DECODE_VREG_POSITION(vr))

  int *dense = tcc_malloc(sizeof(int) * tbl);
  for (int i = 0; i < tbl; i++)
    dense[i] = -1;
  int ndense = 0;
  for (int i = 0; i < n; i++)
  {
    int32_t def = -1, hd = 0, uses[4], nu = 0;
    ra_co_ops(ir, &ir->compact_instructions[i], &def, &hd, uses, &nu);
    for (int k = 0; k < nu; k++)
    {
      if (!tcc_ir_vreg_is_valid(ir, uses[k])) continue;
      int u = AVIDX(uses[k]);
      if (u >= 0 && u < tbl && dense[u] < 0) dense[u] = ndense++;
    }
    if (hd && tcc_ir_vreg_is_valid(ir, def))
    {
      int d = AVIDX(def);
      if (d >= 0 && d < tbl && dense[d] < 0) dense[d] = ndense++;
    }
  }
  if (ndense == 0)
  {
    tcc_free(dense);
    tcc_ir_cfg_free(cfg);
    return;
  }

  const int words = (n + 63) / 64;
  if ((size_t)ndense * (size_t)words > RA_ALIVE_MAX_WORDS)
  {
    tcc_free(dense);
    tcc_ir_cfg_free(cfg);
    return;
  }

  /* Deferred PARAM sources are read again at their CALL, after anything
   * between the PARAM quad and the call; ra_build_intervals extends ends for
   * exactly this, and the liveness has to model it or a share would clobber an
   * argument already marshalled. */
  int *call_param_head = tcc_malloc(sizeof(int) * n);
  int *param_sibling = tcc_malloc(sizeof(int) * n);
  for (int i = 0; i < n; i++) { call_param_head[i] = -1; param_sibling[i] = -1; }
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_FUNCPARAMVAL) continue;
    IROperand s1 = tcc_ir_op_get_src1(ir, q);
    if (!irop_has_vreg(s1) || irop_is_immediate(s1)) continue;
    int cid = TCCIR_DECODE_CALL_ID(irop_get_imm64_ex(ir, tcc_ir_op_get_src2(ir, q)));
    if (cid < 0) continue;
    for (int j = i + 1; j < n; j++)
    {
      IRQuadCompact *qq = &ir->compact_instructions[j];
      if (qq->op != TCCIR_OP_FUNCCALLVOID && qq->op != TCCIR_OP_FUNCCALLVAL) continue;
      if (TCCIR_DECODE_CALL_ID(irop_get_imm64_ex(ir, tcc_ir_op_get_src2(ir, qq))) == cid)
      {
        param_sibling[i] = call_param_head[j];
        call_param_head[j] = i;
        break;
      }
    }
  }
  #define AV_FOR_PARAM_USES(call_idx, vr_, body)                                    \
    do {                                                                            \
      for (int p_ = call_param_head[(call_idx)]; p_ >= 0; p_ = param_sibling[p_]) {  \
        IROperand ps_ = tcc_ir_op_get_src1(ir, &ir->compact_instructions[p_]);       \
        int32_t vr_ = irop_get_vreg(ps_);                                           \
        if (!tcc_ir_vreg_is_valid(ir, vr_)) continue;                               \
        body                                                                        \
      }                                                                             \
    } while (0)

  const int bw = (ndense + 63) / 64;
  uint64_t *use_b = tcc_mallocz(sizeof(uint64_t) * (size_t)nb * bw);
  uint64_t *def_b = tcc_mallocz(sizeof(uint64_t) * (size_t)nb * bw);
  uint64_t *in_b = tcc_mallocz(sizeof(uint64_t) * (size_t)nb * bw);
  uint64_t *out_b = tcc_mallocz(sizeof(uint64_t) * (size_t)nb * bw);

  for (int b = 0; b < nb; b++)
  {
    uint64_t *ub = use_b + (size_t)b * bw, *db = def_b + (size_t)b * bw;
    int s = cfg->blocks[b].start_idx, e = cfg->blocks[b].end_idx;
    for (int i = s; i < e && i < n; i++)
    {
      int32_t def = -1, hd = 0, uses[4], nu = 0;
      ra_co_ops(ir, &ir->compact_instructions[i], &def, &hd, uses, &nu);
      for (int k = 0; k < nu; k++)
      {
        if (!tcc_ir_vreg_is_valid(ir, uses[k])) continue;
        int u = AVIDX(uses[k]); if (u < 0 || u >= tbl || dense[u] < 0) continue;
        if (!tcc_bitspan_test(db, dense[u])) tcc_bitspan_set(ub, dense[u]);
      }
      AV_FOR_PARAM_USES(i, vr, {
        int u = AVIDX(vr); if (u < 0 || u >= tbl || dense[u] < 0) continue;
        if (!tcc_bitspan_test(db, dense[u])) tcc_bitspan_set(ub, dense[u]);
      });
      if (hd && tcc_ir_vreg_is_valid(ir, def))
      {
        int d = AVIDX(def); if (d >= 0 && d < tbl && dense[d] >= 0) tcc_bitspan_set(db, dense[d]);
      }
    }
  }

  int changed = 1, guard = 0;
  while (changed && guard++ < nb + 4)
  {
    changed = 0;
    for (int ri = cfg->rpo_count - 1; ri >= 0; ri--)
    {
      int b = cfg->rpo_order ? cfg->rpo_order[ri] : ri;
      if (b < 0 || b >= nb) continue;
      uint64_t *lo = out_b + (size_t)b * bw, *li = in_b + (size_t)b * bw;
      uint64_t *ub = use_b + (size_t)b * bw, *db = def_b + (size_t)b * bw;
      tcc_bitspan_zero(lo, bw);
      for (int si = 0; si < cfg->blocks[b].num_succs; si++)
      {
        int sb = cfg->blocks[b].succs[si];
        if (sb < 0 || sb >= nb) continue;
        tcc_bitspan_or(lo, in_b + (size_t)sb * bw, bw);
      }
      changed |= tcc_bitspan_or_andnot(li, ub, lo, db, bw);
    }
  }

  uint64_t *rows = tcc_mallocz(sizeof(uint64_t) * (size_t)ndense * words);
  uint64_t *live = tcc_malloc(sizeof(uint64_t) * bw);
  for (int b = 0; b < nb; b++)
  {
    int s = cfg->blocks[b].start_idx, e = cfg->blocks[b].end_idx;
    tcc_bitspan_copy(live, out_b + (size_t)b * bw, bw);
    for (int i = (e < n ? e : n) - 1; i >= s; i--)
    {
      /* Mark live-OUT of i, then live-IN of i: a vreg counted at either end
       * must keep its register across i. */
      tcc_bitspan_for_each_set(live, bw, ndense, di)
      {
        rows[(size_t)di * words + (i >> 6)] |= (1ull << (i & 63));
      }
      int32_t def = -1, hd = 0, uses[4], nu = 0;
      ra_co_ops(ir, &ir->compact_instructions[i], &def, &hd, uses, &nu);
      if (hd && tcc_ir_vreg_is_valid(ir, def))
      {
        int d = AVIDX(def);
        if (d >= 0 && d < tbl && dense[d] >= 0) tcc_bitspan_reset(live, dense[d]);
      }
      for (int k = 0; k < nu; k++)
      {
        if (!tcc_ir_vreg_is_valid(ir, uses[k])) continue;
        int u = AVIDX(uses[k]); if (u < 0 || u >= tbl || dense[u] < 0) continue;
        tcc_bitspan_set(live, dense[u]);
      }
      AV_FOR_PARAM_USES(i, vr, {
        int u = AVIDX(vr); if (u < 0 || u >= tbl || dense[u] < 0) continue;
        tcc_bitspan_set(live, dense[u]);
      });
      tcc_bitspan_for_each_set(live, bw, ndense, di)
      {
        rows[(size_t)di * words + (i >> 6)] |= (1ull << (i & 63));
      }
    }
  }

  tcc_free(live);
  tcc_free(use_b); tcc_free(def_b); tcc_free(in_b); tcc_free(out_b);
  tcc_free(call_param_head); tcc_free(param_sibling);
  tcc_ir_cfg_free(cfg);
  #undef AV_FOR_PARAM_USES
  #undef AVIDX

  info->valid = 1;
  info->n = n;
  info->tbl = tbl;
  info->maxpos = maxpos;
  info->words = words;
  info->ndense = ndense;
  info->dense = dense;
  info->rows = rows;
}

static int ra_may_need_frame_pointer(const TCCIRState *ir)
{
  if (tcc_state->force_frame_pointer || tcc_state->force_lr_save)
    return 1;
  if (func_var)
    return 1;
  if (ir->has_static_chain || tcc_state->nb_nested_funcs > 0)
    return 1;
  for (int i = 0; i < ir->next_instruction_index; i++)
  {
    const int op = ir->compact_instructions[i].op;
    if (op == TCCIR_OP_VLA_ALLOC)
      return 1;
    if (op >= TCCIR_OP_FADD && op <= TCCIR_OP_CVT_FTOI)
      return 1;
    if (op == TCCIR_OP_ASM_INPUT || op == TCCIR_OP_INLINE_ASM || op == TCCIR_OP_ASM_OUTPUT)
      return 1;
  }
  return 0;
}

/* Can physical register `r` be handed to `cur` even though an active interval
 * still owns it?  Yes exactly when EVERY active interval holding it is provably
 * dead across the whole of cur's range: then no execution reads that register's
 * old value from inside the range, and on the paths where the old value IS read
 * later, cur was never defined and never wrote it.
 *
 * Every donor property that would make its vreg's own liveness an incomplete
 * description of what the register holds -- a coalesced class, a loop-phi
 * absorb, a return-block share, an address-taken home -- disqualifies it. */
static int ra_alive_reg_shareable(const RaAliveInfo *alive, SSAInterval *cur,
                                  SSAInterval **active, int active_count, int r,
                                  const RegAllocTarget *target)
{
  if (!alive || !alive->valid)
    return 0;
  if (r < 0 || r >= tcc_state->registers_for_allocator)
    return 0;
  if (cur->crosses_call)
  {
    int ok = 0;
    for (int ci = 0; ci < target->int_class.num_callee_saved; ci++)
      if (target->int_class.callee_saved[ci] == r) { ok = 1; break; }
    if (!ok)
      return 0;
  }
  int holders = 0;
  for (int j = 0; j < active_count; j++)
  {
    SSAInterval *a = active[j];
    if (a == cur || a->stack_location != 0)
      continue;
    if (a->r0 != r && a->r1 != r)
      continue;
    holders++;
    if (a->precolored >= 0 || a->co_member || a->loop_phi_locked || a->reg_shared || a->addrtaken)
      return 0;
    if (a->reg_type == LS_REG_TYPE_FLOAT || a->reg_type == LS_REG_TYPE_DOUBLE)
      return 0;
    if (ra_alive_busy(alive, (int32_t)a->vreg, cur->start, cur->end))
      return 0;
  }
  return holders > 0;
}

static void ra_linear_scan(TCCIRState *ir, SSAInterval *intervals, int count,
                           const RegAllocTarget *target, int spill_base,
                           uint64_t *out_dirty_int, uint64_t *out_dirty_fp,
                           int max_vreg_pos, int has_call, const RaAliveInfo *alive)
{
  if (count <= 0) return;

  qsort(intervals, count, sizeof(SSAInterval), sort_by_start);

  /* Accurate-liveness register sharing; ra_alive_worth_building did the
   * gating, and a failed build leaves `valid` clear.
   *
   * The return-block share below (`reg_shared`) is the one other mechanism
   * that puts a second owner on a register, and it leaves that owner OUT of
   * `active` -- so neither mechanism can see the other's extra holder.  The
   * two are interlocked: whichever fires first in a function locks the other
   * out for the rest of it. */
  int share_ok = (alive && alive->valid);
  int alive_shared_used = 0;
  int reg_shared_used = 0;

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

  /* The frame-pointer register (R7 on Thumb) is held out of the target's
   * allocator map because the backend needs it as the frame base — but only
   * in the minority of functions that actually get a frame pointer.  The FP
   * decision is made AFTER allocation (tcc_ir_codegen_generate and the
   * prologue), so free the register here only when no forcing condition can
   * fire.  ra_may_need_frame_pointer must stay a SUPERSET of those
   * conditions: predicting FP when none materializes just leaves the
   * register unused, but the reverse would hand out the frame base.  The
   * prologue hard-fails if they ever disagree.  A low register matters
   * doubly on Thumb: it takes 16-bit encodings that R8/R10/R11 cannot, and
   * every value it holds is one fewer spill. */
  if (target->frame_pointer_reg >= 0 && !ra_may_need_frame_pointer(ir))
    int_allowed |= int_avail & (1ull << (uint64_t)target->frame_pointer_reg);

  /* Withhold one register in functions that address shared .rodata often.
   * Each such reference otherwise reloads the segment's runtime base from the
   * GOT through a scratch register it has to push and pop; parking the base in
   * a callee-saved register for the whole body turns every one of them into a
   * bare ADD.  The backend claims the register after allocation, from whatever
   * this pass left free (tcc_gen_machine_rodata_anchor_claim), so all this has
   * to do is leave one unallocated — and only where the references pay for it,
   * since a withheld register is one fewer for values.  Purely a size trade:
   * if the backend declines to claim, the register is merely unused. */
  if (target->rodata_anchor_reg >= 0 && target->rodata_anchor_sites &&
      target->rodata_anchor_sites(ir, RA_RODATA_ANCHOR_MIN_SITES) >= RA_RODATA_ANCHOR_MIN_SITES)
    int_allowed &= ~(1ull << (uint64_t)target->rodata_anchor_reg);

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
     * list when cur expires.
     * A volatile local is forced to memory the same way: every access must
     * be a real ldr/str, so it must never occupy a register. */
    if (cur->addrtaken || cur->is_volatile) {
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
      if (LS_IS_VFP_REG(reg)) {
        /* Hard-float float param precolored to its incoming VFP register. */
        fp_free &= ~(1ull << (uint64_t)LS_VFP_REG_NUM(reg));
        dirty_fp |= (1ull << (uint64_t)LS_VFP_REG_NUM(reg));
      } else {
        int_free &= ~(1ull << reg);
        dirty_int |= (1ull << reg);
      }
      /* Insert into active set */
      active[active_count++] = cur;
      continue;
    }

    /* Float/double: use FP class */
    if (cur->reg_type == LS_REG_TYPE_FLOAT) {
      int reg = -1;
      /* Every allocatable single-precision VFP register (s0-s13) is caller-saved
       * and there are no callee-saved VFP registers in the allocation map, so a
       * float that is live across a REAL call cannot stay in a VFP register —
       * force it to a stack slot.  Native FP-op "implicit calls" (a single-
       * precision vadd.f32) clobber nothing, so they do not force a spill. */
      if (!cur->crosses_real_call && fp_free) {
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
      /* The mirror case: a pair the function RETURNS.  AAPCS requires it in
       * r0:r1 at the return, so computing it there directly avoids
       * `mov r0,r4 / mov r1,r5` AND the push/pop of the callee-saved pair it
       * would otherwise occupy — `long long add64(a,b){return a+b;}` goes from
       * 7 instructions to 3, matching GCC.  Same !crosses_call requirement: a
       * call in between would clobber r0:r1. */
      int wants_return_pair = 0;
      if (!cur->crosses_call && 1 < tcc_state->registers_for_allocator) {
        if ((int)cur->start < ir->next_instruction_index &&
            ir->compact_instructions[cur->start].op == TCCIR_OP_FUNCCALLVAL) {
          wants_return_pair = 1; /* incoming: this pair IS a call's result */
        } else if ((int)cur->end < ir->next_instruction_index) {
          IRQuadCompact *eq = &ir->compact_instructions[cur->end];
          if (eq->op == TCCIR_OP_RETURNVALUE &&
              irop_get_vreg(tcc_ir_op_get_src1(ir, eq)) == cur->vreg)
            wants_return_pair = 1; /* outgoing: this pair is what we return */
        }
      }
      if (wants_return_pair) {
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
        /* Boundary reuse for 64-bit pairs.
         *
         * An interval whose end == cur->start is read (at the latest) by the
         * very instruction that defines cur, so its pair is free the moment
         * that instruction retires.  The strict `<` expiration above still
         * counts it as active, which is why the pair paths saw 0-1 free
         * registers and spilled temps that live for a single instruction.
         * Handing cur exactly that pair makes dest == that source, and the
         * dest-equals-source case is safe for the ops below:
         *
         *   ADD/SUB/AND/OR/XOR : thumb_emit_data_processing_mop64 writes the
         *     low half then reads the high half, so an EXACT pair match is
         *     fine (partial overlap is not — we never create one, since we
         *     take both registers of one interval).
         *   SHL/SHR/SAR        : thumb_emit_shift64_mop is already alias
         *     aware (it computes the cross-shift into a temp first and
         *     reorders the halves when dst aliases src).
         *   ASSIGN             : a copy onto itself; both movs become
         *     mov rX, rX and are elided.
         *
         * MUL/UMULL/MLA are excluded: UMULL requires its destination pair not
         * to overlap the sources at all.  Calls are excluded because the
         * result lands in fixed registers.  This is the pair analogue of the
         * single-INT phi-hint boundary case further below, whose comment
         * blanket-excluded pairs; the shift emitter turned out to be safe. */
        int def_op = (cur->start < ir->next_instruction_index)
                         ? ir->compact_instructions[cur->start].op : -1;
        int op_ok = (def_op == TCCIR_OP_ADD || def_op == TCCIR_OP_SUB || def_op == TCCIR_OP_AND ||
                     def_op == TCCIR_OP_OR || def_op == TCCIR_OP_XOR || def_op == TCCIR_OP_SHL ||
                     def_op == TCCIR_OP_SHR || def_op == TCCIR_OP_SAR || def_op == TCCIR_OP_ASSIGN);
        int reused = 0;
        if (op_ok && cur->reg_type == LS_REG_TYPE_LLONG) {
          for (int k = 0; k < active_count; k++) {
            SSAInterval *p = active[k];
            if (p->end != cur->start || p->r0 < 0 || p->r1 < 0 || p->stack_location != 0)
              continue;
            if (p->co_member || cur->co_member || p->loop_phi_locked)
              continue;
            if (p->r0 >= tcc_state->registers_for_allocator ||
                p->r1 >= tcc_state->registers_for_allocator)
              continue;
            if (cur->crosses_call) {
              int c0 = 0, c1 = 0;
              for (int ci = 0; ci < target->int_class.num_callee_saved; ci++) {
                if (target->int_class.callee_saved[ci] == p->r0) c0 = 1;
                if (target->int_class.callee_saved[ci] == p->r1) c1 = 1;
              }
              if (!c0 || !c1)
                continue;
            }
            cur->r0 = p->r0;
            cur->r1 = p->r1;
            ir->ls.dirty_registers |= (1ull << p->r0) | (1ull << p->r1);
            active[k] = active[--active_count];
            active[active_count++] = cur;
            reused = 1;
            break;
          }
        }
        if (reused)
          continue;

        /* Share two registers whose current owners are dead across cur's whole
         * range, rather than writing a 64-bit temp to the frame and reading it
         * straight back. */
        if (share_ok && !reg_shared_used)
        {
          int p0 = -1, p1 = -1;
          for (int r = 0; r < 13 && p1 < 0; r++)
          {
            if (int_free & (1ull << r))
              continue; /* a free register would have been taken above */
            if (!ra_alive_reg_shareable(alive, cur, active, active_count, r, target))
              continue;
            if (p0 < 0) p0 = r; else p1 = r;
          }
          if (p1 >= 0)
          {
            dirty_int |= ((1ull << p0) | (1ull << p1));
            cur->r0 = p0;
            cur->r1 = p1;
            alive_shared_used = 1;
            active[active_count++] = cur;
            RA_DBG("  alive_share pair T%d [%u,%u] -> R%d:R%d",
                   TCCIR_DECODE_VREG_POSITION(cur->vreg), cur->start, cur->end, p0, p1);
            continue;
          }
        }

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
          if (reg < 0 && !alive_shared_used && cur->end > cur->start &&
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
                  reg_shared_used = 1;
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
      /* Before spilling, look for a register whose current owner is provably
       * dead across cur's whole range: sharing it costs nothing, where a spill
       * costs a store plus a reload at every use. */
      if (share_ok && !reg_shared_used) {
        int sh = -1;
        for (int r = 0; r < 13; r++) {
          if (int_free & (1ull << r)) continue; /* a free one was taken above */
          if (!ra_alive_reg_shareable(alive, cur, active, active_count, r, target)) continue;
          sh = r;
          break;
        }
        if (sh >= 0) {
          dirty_int |= (1ull << sh);
          cur->r0 = sh;
          alive_shared_used = 1;
          active[active_count++] = cur;
          RA_DBG("  alive_share T%d [%u,%u] -> R%d",
                 TCCIR_DECODE_VREG_POSITION(cur->vreg), cur->start, cur->end, sh);
          continue;
        }
      }
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

/* Candidate (phi, operand) pairs bucketed by predecessor block, in exactly the
 * order a block-major scan would visit them.
 *
 * ra_collect_phi_copies_for_pred() with no successor filter wants every phi
 * operand naming one predecessor, and scanning all blocks' phis to find them
 * is O(blocks x phi operands) -- once per predecessor, so quadratic.  pr34093's
 * 1000-case switch still spent ~25% of its compile there after the filtered
 * scans were fixed.  The buckets hold CANDIDATES, not decisions:
 * ra_phi_copy_needed() is still asked about each one, in the same order as
 * before, so its phi_pinned side effect is unchanged. */
typedef struct RAPhiPredIndex
{
  int *off;        /* num_blocks + 1 bucket bounds */
  IRPhiNode **phi; /* off[nb] entries */
  int *opidx;
} RAPhiPredIndex;

static int ra_phi_pred_candidate(const IRPhiNode *phi, int pi, int nb)
{
  int pred = phi->operands[pi].pred_block;
  if (pred < 0 || pred >= nb || phi->operands[pi].vreg < 0)
    return -1;
  return pred;
}

static void ra_phi_pred_index_build(IRCFG *cfg, IRSSAState *ssa, RAPhiPredIndex *idx)
{
  const int nb = cfg->num_blocks;

  idx->off = tcc_mallocz(sizeof(int) * (nb + 1));
  idx->phi = NULL;
  idx->opidx = NULL;

  for (int sb = 0; sb < nb; sb++)
    for (IRPhiNode *phi = ssa->block_phis[sb]; phi; phi = phi->next)
      for (int pi = 0; pi < phi->num_operands; pi++) {
        int pred = ra_phi_pred_candidate(phi, pi, nb);
        if (pred >= 0)
          idx->off[pred + 1]++;
      }
  for (int b = 0; b < nb; b++)
    idx->off[b + 1] += idx->off[b];

  const int total = idx->off[nb];
  if (!total)
    return;

  idx->phi = tcc_malloc(sizeof(IRPhiNode *) * total);
  idx->opidx = tcc_malloc(sizeof(int) * total);

  int *cursor = tcc_malloc(sizeof(int) * nb);
  memcpy(cursor, idx->off, sizeof(int) * nb);
  for (int sb = 0; sb < nb; sb++)
    for (IRPhiNode *phi = ssa->block_phis[sb]; phi; phi = phi->next)
      for (int pi = 0; pi < phi->num_operands; pi++) {
        int pred = ra_phi_pred_candidate(phi, pi, nb);
        if (pred < 0)
          continue;
        idx->phi[cursor[pred]] = phi;
        idx->opidx[cursor[pred]] = pi;
        cursor[pred]++;
      }
  tcc_free(cursor);
}

static void ra_phi_pred_index_free(RAPhiPredIndex *idx)
{
  tcc_free(idx->off);
  tcc_free(idx->phi);
  tcc_free(idx->opidx);
  idx->off = NULL;
  idx->phi = NULL;
  idx->opidx = NULL;
}

/* Resolve a successor filter to the block range the scanners below must walk.
 *
 * Both took the filter as a per-iteration equality test, so a call that wanted
 * ONE block still walked all of them -- and ra_resolve_phis calls them once per
 * predecessor, which is O(blocks^2).  pr34093's 1000-case switch spent 32% of
 * its entire compile in ra_collect_phi_copies_for_pred that way.  Same fix, and
 * same reason, as the copies_per_block aggregation in ra_resolve_phis. */
static void ra_phi_scan_range(const IRCFG *cfg, int succ_filter, int *first, int *last)
{
  if (succ_filter < 0) {
    *first = 0;
    *last = cfg->num_blocks - 1;
  } else if (succ_filter < cfg->num_blocks) {
    *first = *last = succ_filter;
  } else {
    /* Names no block: scan nothing, as the equality test used to. */
    *first = 0;
    *last = -1;
  }
}

static int ra_count_phi_copies_for_pred(TCCIRState *ir, IRCFG *cfg, IRSSAState *ssa,
                                        int pred_block, int succ_filter)
{
  int count = 0;
  int first, last;
  ra_phi_scan_range(cfg, succ_filter, &first, &last);
  for (int sb = first; sb <= last; sb++) {
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
                                          const RAPhiPredIndex *idx, RAPhiCopy *copies)
{
  int copy_count = 0;
  int first, last;

  if (succ_filter < 0 && idx->phi && pred_block >= 0 && pred_block < cfg->num_blocks) {
    for (int e = idx->off[pred_block]; e < idx->off[pred_block + 1]; e++) {
      IRPhiNode *phi = idx->phi[e];
      int pi = idx->opidx[e];
      if (!ra_phi_copy_needed(ir, phi, pi))
        continue;
      copies[copy_count].dest_vreg = phi->dest_vreg;
      copies[copy_count].src_vreg = phi->operands[pi].vreg;
      copies[copy_count].btype = phi->btype;
      copies[copy_count].emitted = 0;
      copy_count++;
    }
    return copy_count;
  }

  ra_phi_scan_range(cfg, succ_filter, &first, &last);
  for (int sb = first; sb <= last; sb++) {
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

  RAPhiPredIndex phi_pred_idx;
  ra_phi_pred_index_build(cfg, ssa, &phi_pred_idx);

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
      /* Every copy counted for this pred must land on the target or the
       * fall-through edge.  A shortfall means the CFG no longer matches
       * ssa->block_phis (a pass retargeted a jump while phis were live) and
       * a phi arm would be SILENTLY dropped — a guaranteed miscompile.
       * Surface it loudly instead. */
      if (target_count + fallthrough_count < copies_per_block[b])
        fprintf(stderr,
                "tcc: internal warning: %d phi cop%s dropped at block %d "
                "(JUMPIF target %d -> block %d, fallthrough block %d) — "
                "CFG/phi desync, expect wrong code\n",
                copies_per_block[b] - target_count - fallthrough_count,
                (copies_per_block[b] - target_count - fallthrough_count) == 1 ? "y" : "ies",
                b, old_target, target_block, fallthrough_block);

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
        int copy_count = ra_collect_phi_copies_for_pred(ir, cfg, ssa, b, target_block, &phi_pred_idx, copies);
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
        int copy_count = ra_collect_phi_copies_for_pred(ir, cfg, ssa, b, fallthrough_block, &phi_pred_idx, copies);
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
      int copy_count = ra_collect_phi_copies_for_pred(ir, cfg, ssa, b, -1, &phi_pred_idx, copies);
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

  ra_phi_pred_index_free(&phi_pred_idx);

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

TCC_DBG_ENV_FLAG(ra_no_coalesce, "TCC_NO_COALESCE")
TCC_DBG_ENV_INT(ra_coalesce_env_level, "TCC_COALESCE", 2) /* default: apply, pure copies */
TCC_DBG_ENV_FLAG(ra_dump_ir_co, "DUMP_IR_CO")
TCC_DBG_ENV_FLAG(ra_no_coalesce_llong, "TCC_NO_COALESCE_LLONG")
/* Memory-SSA is opt-in; see the comment at its use site in tcc_ir_ssa_regalloc. */
TCC_DBG_ENV_FLAG(ra_mem_ssa_fwd, "TCC_MEM_SSA_FWD")
TCC_DBG_ENV_FLAG(ra_mem_ssa_dump, "TCC_MEM_SSA_DUMP")
TCC_DBG_ENV_FLAG(ra_mem_ssa_verify_on, "TCC_MEM_SSA")

static int ra_coalesce_level(void)
{
  return ra_no_coalesce() ? 0 : ra_coalesce_env_level();
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
      /* A plain STORE whose TEMP dest has is_lval clear is SSA renaming's
       * in-place register write of a promoted var (a pointer-deref store
       * keeps is_lval) — a DEF, matching ra_build_intervals.  Full-width
       * only: a narrow store leaves the old upper bytes live-before. */
      int inplace_def = (q->op == TCCIR_OP_STORE && !d.is_lval && !d.is_local &&
                         TCCIR_DECODE_VREG_TYPE(irop_get_vreg(d)) == TCCIR_VREG_TYPE_TEMP &&
                         (d.btype == IROP_BTYPE_INT32 || d.btype == IROP_BTYPE_FLOAT32 ||
                          d.btype == IROP_BTYPE_INT64 || d.btype == IROP_BTYPE_FLOAT64));
      if (store_class && !inplace_def) uses[(*nuse)++] = irop_get_vreg(d);
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
  tcc_ir_cfg_compute_rpo(cfg); /* the dataflow needs only the RPO ordering */
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
  TCC_DBG_BLOCK(ra_dump_ir_co) { printf("==== IR AT GRAPH-COALESCE ====\n"); tcc_ir_show(ir); fflush(stdout); }

  /* ---- Stage 1: fresh CFG (entry cfg is stale after phi resolution). ---- */
  IRCFG *cfg = tcc_ir_cfg_build(ir);
  if (!cfg) return;
  tcc_ir_cfg_compute_dominators(cfg); /* populates rpo_order/rpo_count for the dataflow */
  if (cfg->rpo_count <= 0) { tcc_ir_cfg_free(cfg); return; }
  for (int i = 0; i < n; i++) {
    int op = ir->compact_instructions[i].op;
    /* IJUMP (computed goto) has un-enumerable successors — live-out is
     * untrustworthy, bail.  SWITCH_TABLE is fine: tcc_ir_cfg_build models
     * every dispatch edge (targets[] is pre-filled with the default target
     * for gap values, and out-of-range never reaches the dispatch — the
     * frontend guards it with an explicit JUMPIF).  SWITCH_LOAD is a plain
     * table-value LOAD, not a jump.  Bailing on switches was the single
     * blocker for the 64-bit switch-merge family: every case temp kept its
     * own spill slot + str/str/ldr/ldr round-trip (ashrdi-1 constant_shift:
     * 488-byte frame vs GCC's zero). */
    if (op == TCCIR_OP_IJUMP) {
      tcc_ir_cfg_free(cfg);
      return;
    }
  }
  int nb = cfg->num_blocks;
  int tbl = 4 * max_vreg_pos;
  int nw = (tbl + 63) / 64;
  /* Guard pathologically large functions (compile-time / memory). */
  if (nb <= 0 || (long)nb * nw > (4L << 20)) { tcc_ir_cfg_free(cfg); return; }

  /* Historical note: this used to bail on any function containing a 64-bit
   * (LLONG) interval, blaming the UMULL half-operand spill path for a
   * bug_ull_mul miscompile under coalescing.  The actual culprits were the
   * deferred-PARAM liveness hole and the backedge_phi_hoist side-entry
   * retarget (both since fixed); with the pair-aware pressure gate below,
   * LLONG functions coalesce safely — including LLONG-LLONG pair copies
   * (admitted in the union gate).  TCC_NO_COALESCE_LLONG restores the old
   * bail for attribution. */
  if (ra_no_coalesce_llong())
    for (int i = 0; i < count; i++)
      if (intervals[i].reg_type == LS_REG_TYPE_LLONG) { tcc_ir_cfg_free(cfg); return; }

  #define VIDX(vr) ((TCCIR_DECODE_VREG_TYPE(vr) * max_vreg_pos) + TCCIR_DECODE_VREG_POSITION(vr))

  int *iv_of = tcc_malloc(sizeof(int) * tbl);
  for (int i = 0; i < tbl; i++) iv_of[i] = -1;
  for (int i = 0; i < count; i++) {
    int idx = VIDX(intervals[i].vreg);
    if (idx >= 0 && idx < tbl) iv_of[idx] = i;
  }

  /* ---- Deferred-PARAM uses: model FUNCPARAMVAL sources at their CALL. ----
   * Argument marshaling happens at the call site, so a FUNCPARAMVAL source
   * is read again at the matching FUNCCALL — after any instructions between
   * the PARAM quad and the CALL.  ra_build_intervals extends interval ends
   * accordingly; this liveness must do the same or the interference graph
   * misses the overlap and a coalesced def between PARAM and CALL clobbers
   * the argument before the call reads it (bug_stride_minimal: the CSE'd
   * `i+1` loop increment merged with `i` while `printf`'s deferred PARAM
   * still needed the old `i`).  cid matching mirrors ra_build_intervals. */
  int *call_param_head = tcc_malloc(sizeof(int) * n);
  int *param_sibling = tcc_malloc(sizeof(int) * n);
  for (int i = 0; i < n; i++) { call_param_head[i] = -1; param_sibling[i] = -1; }
  for (int i = 0; i < n; i++) {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_FUNCPARAMVAL) continue;
    IROperand s1 = tcc_ir_op_get_src1(ir, q);
    if (!irop_has_vreg(s1) || irop_is_immediate(s1)) continue;
    int cid = TCCIR_DECODE_CALL_ID(irop_get_imm64_ex(ir, tcc_ir_op_get_src2(ir, q)));
    if (cid < 0) continue;
    for (int j = i + 1; j < n; j++) {
      IRQuadCompact *qq = &ir->compact_instructions[j];
      if (qq->op != TCCIR_OP_FUNCCALLVOID && qq->op != TCCIR_OP_FUNCCALLVAL) continue;
      int ccid = TCCIR_DECODE_CALL_ID(irop_get_imm64_ex(ir, tcc_ir_op_get_src2(ir, qq)));
      if (ccid == cid) {
        param_sibling[i] = call_param_head[j];
        call_param_head[j] = i;
        break;
      }
    }
  }
  /* Iterate a CALL's deferred param-source vregs: body receives vreg in `vr_`. */
  #define RA_CO_FOR_CALL_PARAM_USES(call_idx, vr_, body)                        \
    do {                                                                        \
      for (int p_ = call_param_head[(call_idx)]; p_ >= 0; p_ = param_sibling[p_]) { \
        IROperand ps_ = tcc_ir_op_get_src1(ir, &ir->compact_instructions[p_]);  \
        int32_t vr_ = irop_get_vreg(ps_);                                       \
        if (!tcc_ir_vreg_is_valid(ir, vr_)) continue;                           \
        body                                                                    \
      }                                                                         \
    } while (0)

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
      RA_CO_FOR_CALL_PARAM_USES(i, vr, {
        int u = VIDX(vr);
        if (u >= 0 && u < tbl && !tcc_bitspan_test(db, u)) tcc_bitspan_set(ub, u);
      });
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
    /* Weight per value: how many INT registers it occupies while live.
     * LLONG / soft-double / complex-float take a pair; complex-double takes
     * four.  Counting pairs as 1 (the old behavior) under-approximates
     * pressure in 64-bit-heavy functions and lets coalescing push them into
     * spill territory. */
    uint8_t *iw = tcc_mallocz(tbl);
    for (int i = 0; i < count; i++) {
      int idx = VIDX(intervals[i].vreg);
      if (idx < 0 || idx >= tbl) continue;
      switch (intervals[i].reg_type) {
      case LS_REG_TYPE_INT:
        if (intervals[i].r1 < 0) iw[idx] = 1;
        break;
      case LS_REG_TYPE_LLONG:
      case LS_REG_TYPE_DOUBLE_SOFT:
      case LS_REG_TYPE_COMPLEX_FLOAT:
        iw[idx] = 2;
        break;
      case LS_REG_TYPE_COMPLEX_DOUBLE:
        iw[idx] = 4;
        break;
      default:
        break; /* VFP-resident: no INT pressure */
      }
    }
    uint64_t *live = tcc_malloc(sizeof(uint64_t) * nw);
    int maxp = 0;
    for (int b = 0; b < nb && maxp < K; b++) {
      uint64_t *lo = RaLiveMatrix_row(&liveout, b);
      int p = 0;
      tcc_bitspan_copy(live, lo, nw);
      tcc_bitspan_for_each_set(lo, nw, tbl, vi) {
        p += iw[vi];
      }
      int s = cfg->blocks[b].start_idx, e = cfg->blocks[b].end_idx;
      for (int i = e - 1; i >= s && i < n; i--) {
        if (p > maxp) maxp = p;
        IRQuadCompact *q = &ir->compact_instructions[i];
        int32_t def = -1, hd = 0, uses[4], nu = 0;
        ra_co_ops(ir, q, &def, &hd, uses, &nu);
        if (hd && tcc_ir_vreg_is_valid(ir, def)) {
          int d = VIDX(def);
          if (d >= 0 && d < tbl && tcc_bitspan_test(live, d)) { p -= iw[d]; tcc_bitspan_reset(live, d); }
        }
        for (int k = 0; k < nu; k++) {
          if (!tcc_ir_vreg_is_valid(ir, uses[k])) continue;
          int u = VIDX(uses[k]);
          if (u >= 0 && u < tbl && !tcc_bitspan_test(live, u)) { p += iw[u]; tcc_bitspan_set(live, u); }
        }
        RA_CO_FOR_CALL_PARAM_USES(i, vr, {
          int u = VIDX(vr);
          if (u >= 0 && u < tbl && !tcc_bitspan_test(live, u)) { p += iw[u]; tcc_bitspan_set(live, u); }
        });
      }
    }
    tcc_free(iw); tcc_free(live);
    if (maxp >= K) {
      RA_DBG("coalesce: skip — peak INT pressure %d >= K=%d", maxp, K);
      tcc_free(iv_of);
      tcc_free(call_param_head); tcc_free(param_sibling);
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
  /* Per-value linked list of def positions (head/next), so per-edge admission
   * checks walk only that value's defs instead of rescanning all n
   * instructions (quadratic on big functions — 101_cleanup 0.3s -> 19s). */
  int *def_head = tcc_malloc(sizeof(int) * tbl);
  int *def_next = tcc_malloc(sizeof(int) * n);
  for (int i = 0; i < tbl; i++) def_head[i] = -1;
  for (int i = 0; i < n; i++) def_next[i] = -1;
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
  /* Def list covers ALL instructions (not just CFG-block ranges) so the
   * admission walks see exactly the defs the previous full scans saw —
   * a def outside any block (instr_block -1) must keep rejecting. */
  for (int i = 0; i < n; i++) {
    IRQuadCompact *q = &ir->compact_instructions[i];
    int32_t def = -1, hd = 0, uses[4], nu = 0;
    ra_co_ops(ir, q, &def, &hd, uses, &nu);
    if (hd && tcc_ir_vreg_is_valid(ir, def)) {
      int d = VIDX(def);
      if (d >= 0 && d < tbl) {
        def_next[i] = def_head[d];
        def_head[d] = i;
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
    /* A volatile VAR must stay memory-resident (every access a real ldr/str);
     * coalescing it into a register would elide its mandated loads/stores. */
    {
      IRLiveInterval *dvi = tcc_ir_vreg_live_interval(ir, dv);
      IRLiveInterval *svi = tcc_ir_vreg_live_interval(ir, sv);
      if ((dvi && dvi->is_volatile) || (svi && svi->is_volatile))
        continue;
    }
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
        /* Allow phi-copy coalescing only for loop phis.  Conditional-merge
         * phis can have a source equivalent to a variable live across the
         * merge; coalescing them lets the sibling arm overwrite that variable
         * (seed 860).  Loop phis are safe because the latch source is computed
         * fresh each iteration and dies at the copy.  Two placements qualify:
         *   - the copy sits in the loop header itself (a predecessor is a back
         *     edge: the copy's block dominates that predecessor), or
         *   - the copy sits in a latch (a successor is reached by a back edge:
         *     that successor dominates the copy's block).  This is where phi
         *     resolution places the `IV <- IV+1` copy of a rotated or
         *     bottom-tested loop, previously rejected here — the single
         *     largest source of residual `adds rX,rY,#i; mov rY,rX` pairs. */
        int bi = instr_block[i];
        int is_loop_phi = 0;
        if (bi >= 0 && bi < nb) {
          for (int pi = 0; pi < cfg->blocks[bi].num_preds; pi++) {
            int pb = cfg->blocks[bi].preds[pi];
            if (pb >= 0 && pb < nb && tcc_ir_cfg_dominates(cfg, bi, pb)) {
              is_loop_phi = 1;
              break;
            }
          }
          for (int sx = 0; sx < cfg->blocks[bi].num_succs && !is_loop_phi; sx++) {
            int sb = cfg->blocks[bi].succs[sx];
            if (sb >= 0 && sb < nb && sb != bi && tcc_ir_cfg_dominates(cfg, sb, bi))
              is_loop_phi = 1;
          }
          /* Loop-ENTRY copy (split critical edge into a loop header): the
           * copy block has a unique successor that dominates every OTHER
           * def block of the dest — i.e. all sibling defs (the latch
           * copies) execute strictly after this copy, so they can only
           * overwrite a value the copy already delivered.  The parallel
           * arms of a conditional merge (seed 860) fail this test: a
           * sibling arm's def block is never dominated by the merge. */
          if (!is_loop_phi && cfg->blocks[bi].num_succs == 1) {
            int sb = cfg->blocks[bi].succs[0];
            if (sb >= 0 && sb < nb && sb != bi) {
              int all_dominated = 1;
              for (int k = def_head[di]; k >= 0 && all_dominated; k = def_next[k]) {
                if (k == i) continue;
                int kb = instr_block[k];
                if (kb == bi) continue; /* same-block sibling: executes after us */
                if (kb < 0 || kb >= nb || !tcc_ir_cfg_dominates(cfg, sb, kb))
                  all_dominated = 0;
              }
              if (all_dominated)
                is_loop_phi = 1;
            }
          }
          /* Diamond-arm copy whose source is dead at every sibling def of
           * the dest: no OTHER def of the dest falls inside the source's
           * live interval [start, end), so overwriting the class register
           * on a parallel arm can never clobber a value that arm (or any
           * path from it) still reads.  Interval ranges are back-edge
           * extended by ra_build_intervals, so linear containment is a
           * conservative test under loops.  Catches the SETIF-temp ->
           * arm-copy -> phi-dest chain (960402-1: class inherits the r0
           * RETURNVALUE preference) and BOTH arms of a value-select diamond
           * (arith-1::sat_add — admitting only one arm is worse than none:
           * co_member disables the in-scan hint transfer and the surviving
           * real copy blocks post_ra_forward_diamond).  The seed-860 hazard
           * (source equivalent to a value live ACROSS the merge) fails this
           * test at the sibling def; its transitive form (source already
           * coalesced with a live-through value) is refused by the
           * class-level interference check at union time. */
          if (!is_loop_phi && bi >= 0 && iv_of[si] >= 0) {
            uint32_t s_start = intervals[iv_of[si]].start;
            uint32_t s_end = intervals[iv_of[si]].end;
            int safe = 1;
            for (int k = def_head[di]; k >= 0 && safe; k = def_next[k]) {
              if (k == i) continue;
              if ((uint32_t)k >= s_start && (uint32_t)k < s_end)
                safe = 0;
            }
            if (safe)
              is_loop_phi = 1;
          }
        }
        if (!is_loop_phi) {
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
  tcc_free(def_head);
  tcc_free(def_next);

  if (ncand < 2 || ne == 0) {
    tcc_free(iv_of); tcc_free(instr_block);
    tcc_free(cand_id); tcc_free(edge_d); tcc_free(edge_s);
    tcc_free(call_param_head); tcc_free(param_sibling);
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
        RA_CO_FOR_CALL_PARAM_USES(i, vr, {
          int u = VIDX(vr); if (u >= 0 && u < tbl) tcc_bitspan_set(live, u);
        });
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
        /* gate: same-type classes only — INT single-reg with INT, LLONG pair
         * with LLONG pair.  A mixed edge is a truncating/widening copy (e.g.
         * `T9 <- T8` extracting the low half of a 64-bit value): the interval
         * model cannot express half-pair aliasing, so those never merge.
         * DOUBLE_SOFT pairs were tried and measured NET WORSE (+220 corpus,
         * cdivchk* +11): merged classes extend across __aeabi_d* calls and
         * defeat the in-scan return-pair r0:r1 preference (the pr58574
         * lever), which handles soft-float chains better than a class can.
         * Complex types stay out too (rare; pair-sym marshaling subtleties).
         * Not precolored/addrtaken/param/spill-fixed. */
        if (ia->reg_type != ib->reg_type) continue;
        if (ia->reg_type != LS_REG_TYPE_INT && ia->reg_type != LS_REG_TYPE_LLONG) continue;
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
          /* Neighbor classes are weighted by register footprint (INT = 1,
           * LLONG pair = 2), and a pair-class merge needs a free adjacent
           * pair, so its headroom threshold drops by one.  DOUBLE_SOFT
           * neighbors deliberately stay weight 1: weighting them 2 measured
           * +264 corpus — soft-double values rarely stay live across the
           * merged range (they cycle through __aeabi_d* r0:r1 returns), so
           * counting them as resident pairs over-blocks INT/LLONG merges. */
          int limit = (ia->reg_type == LS_REG_TYPE_LLONG) ? K - 1 : K;
          int over = 0, distinct = 0;
          gen++;
          for (int side = 0; side < 2 && !over; side++) {
            int r = side ? rb : ra;
            for (int m = r; m >= 0 && !over; m = mnext[m]) {
              for (int a = adj_start[m]; a < adj_start[m + 1]; a++) {
                int nr = UF_FIND(adj[a]);
                if (nr == ra || nr == rb) continue;
                if (seen[nr] != gen) {
                  seen[nr] = gen;
                  SSAInterval *niv = &intervals[iv_of[cand_vidx[nr]]];
                  distinct += (niv->reg_type == LS_REG_TYPE_LLONG) ? 2 : 1;
                  if (distinct >= limit) { over = 1; break; }
                }
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
          int pref = -1; uint32_t pref_end = 0;
          for (int m = c; m >= 0; m = mnext[m]) {
            int ivi = iv_of[cand_vidx[m]];
            SSAInterval *iv = &intervals[ivi];
            if (iv->start < lo_s) lo_s = iv->start;
            if (iv->end > hi_e) hi_e = iv->end;
            xcall |= iv->crosses_call;
            uc += iv->use_count;
            nuc += iv->narrow_uses;
            sum_len += iv->end - iv->start + 1;
            /* Keep the LATEST-ending member's soft register preference: the
             * r0-for-RETURNVALUE hint matters at the class's end boundary,
             * and the member feeding the return is the one that ends last.
             * Without this the merged class loses the hint (co_member skips
             * the in-scan hint transfer) and both diamond arms compute into
             * a scratch reg plus a final `mov r0, rX` (960402-1 +2). */
            if (iv->pref_reg >= 0 && (pref < 0 || iv->end >= pref_end)) {
              pref = iv->pref_reg; pref_end = iv->end;
            }
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
          if (pref >= 0 && intervals[rep_iv].pref_reg < 0)
            intervals[rep_iv].pref_reg = (int8_t)pref;
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
  tcc_free(call_param_head); tcc_free(param_sibling);
  #undef RA_CO_FOR_CALL_PARAM_USES
  tcc_ir_cfg_free(cfg);
}

/* ============================================================================
 * Entry Point
 * ============================================================================ */

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
/* The def-side operand filter shared by the passes below: returns the TEMP
 * index this instruction defines, or -1 when it defines no plain TEMP.  STORE
 * dests are addresses (a use), FUNCPARAM* dests are not TEMP defs, and an
 * is_lval dest is a deref store target rather than a plain TEMP def. */
static int ra_def_temp_of(TCCIRState *ir, IRQuadCompact *q, int ntmp)
{
  if (q->op == TCCIR_OP_NOP || !irop_config[q->op].has_dest)
    return -1;
  if (q->op == TCCIR_OP_STORE || q->op == TCCIR_OP_STORE_INDEXED ||
      q->op == TCCIR_OP_STORE_POSTINC || q->op == TCCIR_OP_FUNCPARAMVAL ||
      q->op == TCCIR_OP_FUNCPARAMVOID)
    return -1;
  IROperand d = tcc_ir_op_get_dest(ir, q);
  int32_t vr = irop_get_vreg(d);
  if (vr < 0 || d.is_lval || TCCIR_DECODE_VREG_TYPE(vr) != TCCIR_VREG_TYPE_TEMP)
    return -1;
  int t = TCCIR_DECODE_VREG_POSITION(vr);
  return (t >= 0 && t < ntmp) ? t : -1;
}

/* Membership set over (TEMP, block) pairs, used below to answer "does block b
 * define TEMP t?" without a per-TEMP rescan.  Open addressing, power-of-two
 * capacity, linear probe; slot 0 means empty, so keys are stored biased by one.
 * The caller sizes the table at >= 2x the number of insertions, which bounds
 * the load factor at 0.5 and guarantees the probe terminates. */
static inline uint64_t ra_blockset_mix(uint64_t k)
{
  k *= 0x9E3779B97F4A7C15ull;
  return k ^ (k >> 29);
}

static void ra_blockset_add(uint64_t *set, int cap, uint64_t key)
{
  const uint64_t k = key + 1;
  int i = (int)(ra_blockset_mix(k) & (uint64_t)(cap - 1));
  while (set[i] && set[i] != k)
    i = (i + 1) & (cap - 1);
  set[i] = k;
}

static int ra_blockset_has(const uint64_t *set, int cap, uint64_t key)
{
  const uint64_t k = key + 1;
  int i = (int)(ra_blockset_mix(k) & (uint64_t)(cap - 1));
  while (set[i]) {
    if (set[i] == k)
      return 1;
    i = (i + 1) & (cap - 1);
  }
  return 0;
}

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
    int t = ra_def_temp_of(ir, &ir->compact_instructions[i], ntmp);
    if (t < 0) continue;
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
    /* Rescanning every instruction per multi-block TEMP -- once to collect its
     * def-blocks, once more to look for a use outside them -- is
     * O(multi-block TEMPs x instructions), with an operand decode at every step
     * and an isdef[] clear per candidate on top.  strlen-5 spent 24.7% of its
     * whole compile here.  Two passes over the instructions answer the same
     * question: the first records every (TEMP, block) pair that is a def, the
     * second asks that set about every use. */
    const uint64_t nblk = (uint64_t)cfg->num_blocks;
    /* Nothing multi-block means nothing to decide -- and no table to pay for.
     * Size it from the number of defs that actually go in, not from n: most
     * functions here have a handful, and a table sized for every instruction
     * cost more to allocate and zero than it saved (20040709-2). */
    int any_multi = 0;
    for (int t = 0; t < ntmp && !any_multi; t++)
      any_multi = (def_block[t] == -2);
    if (!any_multi)
      goto promote_decided; /* nothing to decide: don't walk the instructions */

    int ndefs = 0;
    for (int i = 0; i < n; i++) {
      int t = ra_def_temp_of(ir, &ir->compact_instructions[i], ntmp);
      if (t >= 0 && def_block[t] == -2) ndefs++;
    }
    if (!ndefs)
      goto promote_decided;

    int dcap = 16;
    while (dcap < ndefs * 2) dcap <<= 1;
    uint64_t *defset = tcc_mallocz(sizeof(uint64_t) * dcap);

    for (int i = 0; i < n; i++) {
      int t = ra_def_temp_of(ir, &ir->compact_instructions[i], ntmp);
      if (t < 0 || def_block[t] != -2) continue;
      ra_blockset_add(defset, dcap, (uint64_t)t * nblk + (uint64_t)cfg->instr_to_block[i]);
    }

    /* a use in a non-def block => needs a phi */
    for (int i = 0; i < n; i++) {
      IRQuadCompact *q = &ir->compact_instructions[i];
      if (q->op == TCCIR_OP_NOP) continue;
      int32_t uses[5]; int nu = 0;
      if (irop_config[q->op].has_src1) uses[nu++] = irop_get_vreg(tcc_ir_op_get_src1(ir, q));
      if (irop_config[q->op].has_src2) uses[nu++] = irop_get_vreg(tcc_ir_op_get_src2(ir, q));
      if (q->op == TCCIR_OP_MLA) uses[nu++] = irop_get_vreg(tcc_ir_op_get_accum(ir, q));
      if (q->op == TCCIR_OP_STORE || q->op == TCCIR_OP_STORE_INDEXED || q->op == TCCIR_OP_STORE_POSTINC)
        uses[nu++] = irop_get_vreg(tcc_ir_op_get_dest(ir, q));
      if (!nu) continue;
      const uint64_t blk = (uint64_t)cfg->instr_to_block[i];
      for (int u = 0; u < nu; u++) {
        if (uses[u] < 0 || TCCIR_DECODE_VREG_TYPE(uses[u]) != TCCIR_VREG_TYPE_TEMP) continue;
        int t = TCCIR_DECODE_VREG_POSITION(uses[u]);
        if (t < 0 || t >= ntmp || def_block[t] != -2 || needs_phi[t]) continue;
        if (!ra_blockset_has(defset, dcap, (uint64_t)t * nblk + blk))
          needs_phi[t] = 1;
      }
    }
    tcc_free(defset);
  promote_decided:;
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

    /* Do NOT drop the def: rematerialization only happens for the one
     * machine_op operand shape (spilled + need_lval value read, see
     * machine_op.c) — a PARAM src or a plain binop src2 of the spilled temp
     * takes the slot/frame path instead and would read garbage once the
     * def's mov+store is gone (fuzz seed longlong:6393; the LLONG
     * pair classes raised pressure enough to spill the const).  The
     * redundant mov+store for a remat'd spilled constant is noise compared
     * to wrong code. */
    (void)needs_slot;
  }

  tcc_free(needs_slot);
  tcc_free(def_count);
  tcc_free(def_idx);
}

/* Gate + time + dump one flat-region pass, the shape every entry in the
 * pre-CFG cluster below shares.  `changed` receives the pass's return value so
 * the timing dump can report productivity (Phase 0.4 of
 * docs/plans/opt_pass_dedup_and_perf.md). */
#define RA_FLAT_PASS(changed, name, cond, call)                                                                        \
  do                                                                                                                   \
  {                                                                                                                    \
    (changed) = 0;                                                                                                     \
    if (tcc_state && (cond) && !tcc_ir_opt_pass_disabled(name))                                                        \
      TCC_PASS_TIMED(changed, name, (call));                                                                           \
    tcc_ir_dump_after_pass(ir, name);                                                                                  \
  } while (0)

/* ssa:cfg_cleanup body — see the call site for why it runs where it does. */
static int ra_cfg_cleanup(TCCIRState *ir)
{
  const IRPassGroup *groups;
  int group_count;
  int total = 0;
  tcc_ir_opt_get_pipeline(IR_OPT_LEVEL_2, &groups, &group_count);
  const IRPassGroup *cleanup_group = &groups[group_count - 1];
  for (int outer = 0; outer < 3; outer++) {
    int ch = 0;
    for (int i = 0; i < 8; i++) {
      int c = tcc_ir_opt_jump_threading(ir);
      /* Inside the cascade, not beside it: retargeting the constant arm's edge
       * turns the true arm's trailing JUMP into a fall-through and orphans the
       * merge label, and only eliminate_fallthrough + dce below can collect
       * that -- which is what exposes `T <-- (cond)` to setif fusion. */
      c += tcc_ir_opt_bool_diamond_branch(ir);
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
    total += ch;
    if (!ch)
      break;
  }
  return total;
}

void tcc_ir_ssa_regalloc(TCCIRState *ir, const RegAllocTarget *target, int spill_base)
{
  if (!ir || !target) return;
  int ra_ch;
  RAPhiStats phi_stats = {0};
  dbg_scan_overlap(ir, "ssa_regalloc_entry");

  /* ssa:mem_init — frontend memory-init lowering (memset(0)+const stores →
   * rodata BLOCK_COPY; small stack/global zero-memset → direct STORE #0).
   * Runs first on raw flat IR, before cfg_cleanup and the loop transforms
   * below, matching its former position at the head of the tccgen.c pipeline.
   * Ungated by -O (the lowering also fires at -O0); knob
   * TCC_DISABLE_PASS=ssa:mem_init.  See docs/plan_legacy_flat_ir_ssa_retire.md. */
  RA_FLAT_PASS(ra_ch, "ssa:mem_init", 1, ssa_opt_mem_init(ir));

  /* ssa:cfg_cleanup — GCC-style cleanup_cfg at SSA-pipeline entry: thread jump
   * chains / drop fall-through jumps / NOP orphan flag-setters left by the flat
   * pipeline, so the flat-region loop transforms below (reroll/licm/loop_rotate)
   * see normalized control flow instead of trampoline chains they would rotate
   * around and duplicate tests for.  When the cascade fired, re-run call-result
   * demotion + the flat late_cleanup group (self-gated passes): collapsed
   * diamonds expose dead pure calls / VLA allocs / stores those passes could
   * not see on their earlier run. */
  RA_FLAT_PASS(ra_ch, "ssa:cfg_cleanup",
               tcc_state->optimize >= 1 && tcc_state->opt_jump_threading,
               ra_cfg_cleanup(ir));

  /* ssa:struct_copy_roundtrip — drop the memmove(B,A);memmove(A,B) pair left
   * by an inlined identity `y = retme(y)` helper.  The matcher needs the two
   * copies in one straight line; the inline expansion leaves a fall-through
   * JMP + target marker between them that only cfg_cleanup removes, so the
   * tccgen-time call misses the pattern (20040709-2 fn1* family). */
  RA_FLAT_PASS(ra_ch, "ssa:struct_copy_roundtrip", tcc_state->opt_redundant_store,
               tcc_ir_opt_struct_copy_roundtrip_elim(ir));

  /* ssa:or_bool_diamond — fold `acc |= (cond ? 1 : 0)` stack-slot
   * materialization into per-arm ORs.  Needs the STORE-slot/OR adjacency that
   * cfg_cleanup's eliminate_fallthrough just created.  Gate matches the legacy
   * tccgen.c call (opt_const_prop); knob TCC_DISABLE_PASS=ssa:or_bool_diamond. */
  RA_FLAT_PASS(ra_ch, "ssa:or_bool_diamond", tcc_state->opt_const_prop,
               ssa_opt_or_bool_diamond(ir));

  /* ssa:stack_addr_simplify — deref-of-known-stack-addr → direct StackLoc; legacy gate; knob TCC_DISABLE_PASS=ssa:stack_addr_simplify */
  RA_FLAT_PASS(ra_ch, "ssa:stack_addr_simplify", tcc_state->opt_const_prop,
               tcc_ir_opt_stack_addr_simplify(ir));

  /* ssa:reroll — re-roll runs of identical macro-unrolled blocks into a counted
   * loop.  Runs first in the flat region (preserving reroll's legacy "earliest
   * loop transform" order) but now post-propagation: foldable runs are already
   * collapsed by downstream const-prop, so only non-foldable repetition survives
   * to re-roll, which fixes the legacy pre-propagation counterproductivity.
   * Gated opt_reroll (-O2); knob TCC_DISABLE_PASS=ssa:reroll.
   * See docs/plan_legacy_loop_reroll_ssa.md. */
  /* compact the (N-1)*P NOPs a successful re-roll leaves so CFG/SSA below don't iterate them */
  RA_FLAT_PASS(ra_ch, "ssa:reroll", tcc_state->opt_reroll,
               ssa_opt_reroll(ir) ? (tcc_ir_opt_compact_nops(ir), 1) : 0);

  /* ssa:licm — hoist loop invariants (arithmetic + pure/const calls) to the
   * preheader before the other loop transforms and ssa:iv_strength_reduction,
   * preserving the legacy "LICM before IV-SR" order now that both left tccgen.
   * Reuses the proven licm.c engine; gated opt_licm (-O2); knob
   * TCC_DISABLE_PASS=ssa:licm. */
  RA_FLAT_PASS(ra_ch, "ssa:licm", tcc_state->opt_licm, ssa_opt_licm(ir));

  /* Loop rotation (ssa:loop_rotate): convert safe top-tested loops to
   * bottom-tested on flat IR, before the CFG/SSA below are built, so the
   * downstream SSA passes and regalloc see the rotated shape.  CFG/dominator
   * based natural-loop detection driving the proven flat-IR rewrite; gated to
   * -O1+ (matching the legacy pass and the SSA opt tier) and disableable via
   * TCC_DISABLE_PASS=ssa:loop_rotate. */
  RA_FLAT_PASS(ra_ch, "ssa:loop_rotate", tcc_state->optimize >= 1,
               ssa_opt_loop_rotate(ir));

  /* ssa:licm ran BEFORE rotation and so saw the un-rotated header/latch/body
   * shape, whose preheader is not the header's immediate predecessor — the
   * invariant-global-load hoist's insertion safety demands exactly that, so it
   * declined every ordinary `for` loop.  Rotation provides the shape; give the
   * hoist a second chance on it. */
  RA_FLAT_PASS(ra_ch, "ssa:licm_global_load", tcc_state->opt_licm,
               ssa_opt_licm_global_load(ir));

  /* Zero-trip guard elimination (ssa:loop_guard_elim): drop the pre-loop
   * `CMP iv,#lim / JUMPIF` that rotation leaves in front of a bottom-tested
   * loop when the IV's entry value is a constant carried from the exit of a
   * preceding counted loop over the same variable.  Runs immediately after
   * ssa:loop_rotate — rotation is the shape provider, and everything below
   * must already see the single-exit form.  Const prop covers the literal-init
   * case on its own; only the carried case needs this pass (SSA sees a phi at
   * the header and cannot conclude i == A).  Gated -O1+ to match
   * ssa:loop_rotate; disableable via TCC_DISABLE_PASS=ssa:loop_guard_elim,
   * which also makes rotation stop admitting the shapes that depend on it. */
  RA_FLAT_PASS(ra_ch, "ssa:loop_guard_elim", tcc_state->optimize >= 1,
               tcc_ir_opt_loop_guard_elim(ir));

  /* First-iteration-exit peeling (ssa:first_iter_exit): eliminate top-tested
   * loops whose header exit test is provably true on first entry, on flat IR
   * after rotation (rotation declines these shapes; a rotated loop's guard is
   * outside the loop so this pass declines rotated shapes — no overlap).
   * Gate matches the legacy tccgen.c pass exactly (-O1+ with const-prop, so
   * -fno-const-prop keeps its meaning for bisection); disableable via
   * TCC_DISABLE_PASS=ssa:first_iter_exit. */
  RA_FLAT_PASS(ra_ch, "ssa:first_iter_exit",
               tcc_state->optimize >= 1 && tcc_state->opt_const_prop,
               ssa_opt_first_iter_exit(ir));

  /* Pointer-IV exit-value substitution (ssa:ptr_iv_exit_subst): rewrite
   * post-loop pointer-IV reads to the closed-form exit address and fold the
   * consuming `p != &a[N]` compares (pass-owned; nothing downstream folds
   * them).  Gate matches the legacy tccgen.c pass (-O1+ with const-prop). */
  RA_FLAT_PASS(ra_ch, "ssa:ptr_iv_exit_subst",
               tcc_state->optimize >= 1 && tcc_state->opt_const_prop,
               ssa_opt_ptr_iv_exit_subst(ir));

  /* Loop constant simulation (ssa:loop_const_sim): collapse register-only
   * bounded-trip loops to residual final values on flat IR.  Gate matches the
   * legacy tccgen.c Phase 4e pass exactly (opt_loop_unroll, -O2 default;
   * -floop-unroll reaches it at lower levels; -fno-loop-unroll disables both
   * it and the unroller, keeping the shared bisection knob).  Disableable via
   * TCC_DISABLE_PASS=ssa:loop_const_sim. */
  RA_FLAT_PASS(ra_ch, "ssa:loop_const_sim", tcc_state->opt_loop_unroll,
               ssa_opt_loop_const_sim(ir));

  /* Loop unrolling / constant-trip elimination (ssa:loop_unroll): fully unroll
   * or close-form-eliminate small constant/symbolic-trip register-only loops on
   * flat IR after const_sim has starved it of its own candidates.  Same gate as
   * the legacy tccgen.c Phase 5a pass (opt_loop_unroll); the SSA/regalloc
   * pipeline folds the residual arithmetic (no post-unroll cascade replicated).
   * Disableable via TCC_DISABLE_PASS=ssa:loop_unroll. */
  RA_FLAT_PASS(ra_ch, "ssa:loop_unroll", tcc_state->opt_loop_unroll,
               ssa_opt_loop_unroll(ir));

  /* Induction-variable strength reduction (ssa:iv_strength_reduction): transform
   * array-indexing recurrences base + i*stride into a maintained stride pointer
   * (enabling post-increment addressing) and optionally eliminate the counter IV
   * against a hoisted end pointer, on flat IR after rotation/const_sim/unroll
   * have normalized and starved the loops.  Runs before ssa:decrement_to_zero,
   * preserving the legacy "decrement_to_zero after IV-SR" order.  Gate matches
   * the legacy tccgen.c Phase 6 pass (opt_iv_strength_red, -O1+); disableable via
   * TCC_DISABLE_PASS=ssa:iv_strength_reduction. */
  RA_FLAT_PASS(ra_ch, "ssa:iv_strength_reduction", tcc_state->opt_iv_strength_red,
               ssa_opt_iv_strength_reduction(ir));

  /* Decrement-to-zero (ssa:decrement_to_zero): rewrite count-up pure-counter
   * loops that ssa:loop_rotate turned bottom-tested (and const_sim/unroll left
   * as side-effecting survivors) into count-down-to-zero so the backend fuses
   * the latch SUB+CMP#0 into a flag-setting SUBS.  Flat IR before the CFG build
   * below, so the NOPed guard's control-flow change is reflected downstream.
   * Gated -O1+ (matches ssa:loop_rotate, the shape provider); disableable via
   * TCC_DISABLE_PASS=ssa:decrement_to_zero. */
  RA_FLAT_PASS(ra_ch, "ssa:decrement_to_zero", tcc_state->optimize >= 1,
               ssa_opt_decrement_to_zero(ir));

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

  /* Sweep NOPs (and their stale jump-target marks) left by the flat pipeline
   * and the flat transforms above before building the CFG: a NOP that was a
   * jump target introduces a spurious block boundary that blocks var->temp
   * promotion in ssa_rename (cmp_offset_common_base).  The flat groups only
   * compact when a pass reported changes, so a clean run can still arrive
   * here with NOPs in the stream. */
  tcc_ir_opt_compact_nops(ir);

  /* Build CFG + dominators */
  TCCPassTimer ra2_pt;
  tcc_pass_timing_begin(&ra2_pt, "ra2:cfg_ssa");
  IRCFG *cfg = tcc_ir_cfg_build(ir);
  if (!cfg) {
    /* Fallback: no CFG means trivial function, use old allocator path */
    tcc_pass_timing_end(&ra2_pt, -1);
    return;
  }
  tcc_ir_cfg_compute_dominators(cfg);
  tcc_ir_cfg_compute_dom_frontiers(cfg);

  ra_promote_multidef_temps_to_vars(ir, cfg);
  tcc_ir_dump_after_pass(ir, "ssa_promote");

  /* Construct SSA.
   *
   * NOTE (docs/plans/o0_compile_perf.md §9): this is NOT skippable at -O0,
   * even though DCE-on-phis is the only -O0 *optimization* downstream of it.
   * SSA renaming is what gives every definition its own vreg, which is what
   * makes the allocator's one-interval-per-vreg model sound; a value defined
   * on two paths otherwise gets a single interval spanning both.  Forcing the
   * no-promotable fallback at -O0 was measured at -20% compile time and -210 B
   * of code, and miscompiled 4 gcc-torture execute tests (990404-1, pr125291,
   * pr34415, pending-4) -- with dominators and promotion still running, so it
   * is the renaming itself that is load-bearing.  Making -O0 cheaper here
   * needs a different interval model, not a skip. */
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
  tcc_pass_timing_end(&ra2_pt, -1);
  tcc_pass_timing_begin(&ra2_pt, "ra2:ssaopt");

  /* Phase 2 (memory-SSA): store->load forwarding over the reaching-def walk,
   * surviving joins (MemoryPhi) and provably-non-aliasing intervening stores
   * that the dom-spine load_cse cannot.  Currently OPT-IN (TCC_MEM_SSA_FWD):
   * only full-word (4/8-byte) stores forward soundly; the high-value sub-word
   * bitfield idiom needs a consumer-mask demand analysis before it can be
   * forwarded safely (a raw sub-word forward truncates — see pr78477), so this
   * is not yet on by default.  TCC_MEM_SSA / TCC_MEM_SSA_DUMP verify / dump the
   * constructed memory-SSA. */
  {
    const int do_fwd = ra_mem_ssa_fwd();
    const int do_dump = ra_mem_ssa_dump();
    const int do_validate = ra_mem_ssa_verify_on() || do_dump;
    if (do_fwd || do_validate) {
      MemSSAState *msa = tcc_ir_mem_ssa_build(ir, cfg);
      if (msa) {
        if (do_validate && !tcc_ir_mem_ssa_verify(ir, msa))
          fprintf(stderr, "[mem-ssa] INVARIANT VIOLATION (%d instrs)\n",
                  ir->next_instruction_index);
        if (do_dump)
          tcc_ir_mem_ssa_dump(ir, msa);
        if (do_fwd)
          tcc_ir_mem_ssa_load_fwd(ir, msa);
        tcc_ir_mem_ssa_free(msa);
      }
    }
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
        /* Run a pass, then make it observable to -dump-ir-passes=<name>
         * golden snapshots (same names as the tcc_ir_ssa_opt_run driver). */
#define RUN_SSA(name, call)                                                                                            \
  do                                                                                                                   \
  {                                                                                                                    \
    if (!tcc_ir_opt_pass_disabled(name))                                                                               \
    {                                                                                                                  \
      int _c;                                                                                                          \
      TCC_PASS_TIMED(_c, name, (call));                                                                                \
      np_changes += _c;                                                                                                \
    }                                                                                                                  \
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
          RUN_SSA("ssa:vrp", (tcc_state && tcc_state->opt_vrp) ? ssa_opt_vrp(&ssa_opt_ctx) : 0);
          RUN_SSA("ssa:setif_or_taut",
                  (tcc_state && tcc_state->opt_const_prop) ? ssa_opt_setif_or_taut(&ssa_opt_ctx) : 0);
          RUN_SSA("ssa:setif_mask_fold",
                  (tcc_state && tcc_state->opt_const_prop) ? ssa_opt_setif_mask_fold(&ssa_opt_ctx) : 0);
          RUN_SSA("ssa:cmp_offset_fold",
                  (tcc_state && tcc_state->opt_const_prop) ? ssa_opt_cmp_offset_fold(&ssa_opt_ctx) : 0);
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
        /* After the loop, never inside it: bool_norm deletes the `CMP b,#0`
         * that setif_mask_fold matches on (see the SSA driver's tail). */
        if (tcc_state && tcc_state->opt_const_prop &&
            !tcc_ir_opt_pass_disabled("ssa:bool_norm")) {
          int _c;
          TCC_PASS_TIMED(_c, "ssa:bool_norm", ssa_opt_bool_norm(&ssa_opt_ctx));
          tcc_ir_dump_after_pass(ir, "ssa:bool_norm");
          if (_c)
            ssa_opt_dce(&ssa_opt_ctx);
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
         * worklist can't cascade the now-dead dest-LEA/ADD address temps.
         * Phi operand uses must be re-added too — dropping them lets DCE gut
         * every loop-carried def (33_ternary_op loop body). */
        for (int p = 0; p < ssa_opt_ctx.vinfo_cap; p++)
          ssa_opt_ctx.vinfo[p].use_count = 0;
        for (int i = 0; i < ir->next_instruction_index; i++) {
          IRQuadCompact *q = &ir->compact_instructions[i];
          if (q->op != TCCIR_OP_NOP)
            ssa_opt_scan_instr_uses(&ssa_opt_ctx, i, q);
        }
        if (ssa->block_phis) {
          for (int b = 0; b < cfg->num_blocks; b++) {
            for (IRPhiNode *phi = ssa->block_phis[b]; phi; phi = phi->next) {
              for (int pi = 0; pi < phi->num_operands; pi++) {
                IRSSAVregInfo *pvi = ssa_opt_vinfo(&ssa_opt_ctx, phi->operands[pi].vreg);
                if (pvi)
                  ssa_opt_add_use_phi(pvi, b, pi);
              }
            }
          }
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
  tcc_pass_timing_end(&ra2_pt, -1);
  tcc_pass_timing_begin(&ra2_pt, "ra2:phis_folds");
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
  if (tcc_state && tcc_state->optimize >= 2) {
    int addr_changes = 0;
    /* Before the address hoists, so it matches operands the frontend built
     * (which carry IROP_AUX_NONVOLATILE) rather than bases synthesized by a
     * pass.  Here rather than in the flat pipeline because the two reads it
     * collapses only name the same address temp once GVN has run — see the
     * header of source/opt/flat/memory/deref_operand_cse.c. */
    addr_changes += tcc_ir_opt_deref_operand_cse(ir);
    if (!tcc_ir_opt_pass_disabled("ssa:global_addr_hoist"))
      addr_changes += tcc_ir_ssa_opt_global_addr_hoist(ir);
    if (!tcc_ir_opt_pass_disabled("ssa:loop_addr_hoist"))
      addr_changes += tcc_ir_ssa_opt_loop_addr_hoist(ir);
    if (!tcc_ir_opt_pass_disabled("ssa:local_addr_cse"))
      addr_changes += tcc_ir_ssa_opt_local_addr_cse(ir);
    /* Same reason these three run here: an indexed access's STACKOFF base is a
     * DIRECT frame reference that slot-based alias analysis reads precisely, so
     * parking it in a register may only happen once every alias-sensitive pass
     * is done (early it miscompiles gcc.c-torture pr51466). */
    if (!tcc_ir_opt_pass_disabled("ssa:stackoff_indexed_base_cse"))
      addr_changes += tcc_ir_opt_stackoff_indexed_base_cse(ir);
    if (addr_changes > 0) {
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
  }
  tcc_ir_dump_after_pass(ir, "ssa:global_addr_hoist");

  /* Build call prefix for call-crossing detection */
  int *call_prefix = ra_build_call_prefix(ir);

  /* Build SSA live intervals */
  SSAInterval *intervals = NULL;
  int interval_count = 0;
  int max_vreg_pos = 0;
  tcc_pass_timing_end(&ra2_pt, -1);
  tcc_pass_timing_begin(&ra2_pt, "ra2:intervals");
  ra_build_intervals(ir, cfg, ssa, &intervals, &interval_count, call_prefix, &max_vreg_pos);
  tcc_pass_timing_end(&ra2_pt, -1);
  tcc_pass_timing_begin(&ra2_pt, "ra2:hints");
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
  tcc_pass_timing_end(&ra2_pt, -1);
  tcc_pass_timing_begin(&ra2_pt, "ra2:coalesce_g");
  ra_coalesce_graph(ir, intervals, interval_count, max_vreg_pos);
  tcc_pass_timing_end(&ra2_pt, -1);
  tcc_pass_timing_begin(&ra2_pt, "ra2:scan");

  /* Run linear scan */
  uint64_t dirty_int = 0, dirty_fp = 0;
  int n_instr = ir->next_instruction_index;
  int has_call = call_prefix && n_instr > 0 && call_prefix[n_instr] > 0;
  RaAliveInfo alive;
  memset(&alive, 0, sizeof alive);
  if (ra_alive_worth_building(ir, intervals, interval_count))
    ra_alive_build(ir, &alive, max_vreg_pos);
  ra_linear_scan(ir, intervals, interval_count, target, spill_base, &dirty_int, &dirty_fp,
                 max_vreg_pos, has_call, &alive);
  ra_alive_free(&alive);
  tcc_pass_timing_end(&ra2_pt, -1);
  tcc_pass_timing_begin(&ra2_pt, "ra2:finish");

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

  tcc_pass_timing_end(&ra2_pt, -1);

  /* Cleanup */
  tcc_free(phi_stats.participants);
  tcc_free(intervals);
  if (call_prefix) tcc_free(call_prefix);
  tcc_ir_ssa_free(ssa);
  tcc_ir_cfg_free(cfg);
}

/* ============================================================================
 * Post-Allocation Redundant Reload Elimination
 *
 * Every soft-float routine opens by punning its arguments through a union:
 *
 *   StackLoc[-8] <-- R0(P0) [STORE]     ; ua.d = a
 *   R0(V0)      <-- StackLoc[-8] [LOAD] ; a_bits = ua.u
 *
 * which lowers to `strd r0,r1,[sp,#N]` immediately followed by
 * `ldrd r0,r1,[sp,#N]` -- a reload of registers that already hold the value.
 * sl_forward refuses to forward it because the two ends have different types
 * (FLOAT64 -> INT64), and forwarding it BEFORE allocation was measured and
 * reverted: keeping the punned value in a register stretched a 64-bit live
 * range and the linear-scan allocator cascaded into spills (dadd +13.4%).
 *
 * After allocation that trade no longer exists.  The registers are already
 * chosen, so dropping a load that rewrites a register with the value it
 * already holds cannot lengthen anything -- it only removes a memory access.
 *
 * The analysis is a straight-line available-value table: remember which
 * register pair was last stored to each frame slot, drop a later load of that
 * slot into exactly those registers, and invalidate on anything that could
 * disturb either side.
 * ==========================================================================*/

/* An allocation half carries its physical register in the low five bits and a
 * spill flag at 0x20, but an UNALLOCATED half is the all-ones sentinel -- and
 * that sentinel has the spill bit set.  So "is this half spilled" is only a
 * question once the half names a register; asked unconditionally it answers
 * yes for the absent high half of every 32-bit value.  Codegen already reads
 * it this way (ir/codegen.c takes pr1_reg and pr1_spilled apart separately).
 */
static int ra_alloc_half_unset(uint16_t half)
{
  return (half & PREG_REG_NONE) == PREG_REG_NONE;
}

static int ra_alloc_half_spilled(uint16_t half)
{
  return !ra_alloc_half_unset(half) && (half & PREG_SPILLED) != 0;
}

/* A direct frame slot: `StackLoc[off]` as a memory operand (an `Addr[...]`
 * form has is_lval clear and is an address, not a location). */
static int ra_rl_slot_offset(IROperand op, int *off)
{
  if (op.tag != IROP_TAG_STACKOFF || op.is_llocal || op.is_sym)
    return 0;
  if (irop_get_vreg(op) != -1 || !op.is_lval)
    return 0;
  *off = (int)irop_get_stack_offset(op);
  return 1;
}

/* Physical registers behind a plain register operand, as codegen resolves it. */
static int ra_rl_operand_regs(TCCIRState *ir, IROperand op, int *r0, int *r1)
{
  int32_t vr = irop_get_vreg(op);
  if (vr < 0 || !tcc_ir_vreg_is_valid(ir, vr))
    return 0;
  IRLiveInterval *li = tcc_ir_vreg_live_interval(ir, vr);
  if (!li || li->allocation.offset != 0)
    return 0;
  if (ra_alloc_half_spilled(li->allocation.r0) || ra_alloc_half_spilled(li->allocation.r1))
    return 0;
  int a0 = li->allocation.r0 & PREG_REG_NONE;
  int a1 = li->allocation.r1 & PREG_REG_NONE;
  if (a0 == PREG_REG_NONE)
    return 0;
  *r0 = a0;
  *r1 = (a1 == PREG_REG_NONE) ? -1 : a1;
  return 1;
}

/* Bytes an access moves when the register ends up holding EXACTLY the slot's
 * contents.  A sub-word access does not: `strb r2,[sp,#N]` writes only r2's
 * low byte and `ldrb r2,[sp,#N]` brings it back zero-extended, so neither end
 * of such a pair may be matched against the other or against a word access.
 * FLOAT64/INT64 (and FLOAT32/INT32) share a width because a 64-bit soft-float
 * value lives in the same GPR pair as the integer it was punned from -- that
 * equivalence is the whole point of the pass. */
static int ra_rl_exact_width(int btype)
{
  switch (btype)
  {
  case IROP_BTYPE_INT32:
  case IROP_BTYPE_FLOAT32:
    return 4;
  case IROP_BTYPE_INT64:
  case IROP_BTYPE_FLOAT64:
    return 8;
  default:
    return 0;
  }
}

typedef struct RARelSlot
{
  int off;      /* frame offset of the slot */
  int size;     /* bytes the access covered (4 or 8; exact, see above) */
  int r0, r1;   /* registers whose value it holds (r1 < 0 when 32-bit) */
  int def_idx;  /* the store that put it there */
  int valid;
} RARelSlot;

#define RA_REL_MAX_TRACKED 16

/* Keep `regs` occupied over [lo,hi] so the encoder cannot hand them out.
 *
 * A register outside every live interval holds nothing the allocator is
 * accounting for, and the encoder helps itself to exactly those whenever it
 * has to materialise an operand: `R0 <- StackLoc[-12] ADD StackLoc[-36]` is
 * three instructions, two of them loads of the operands into whatever happens
 * to be free.  Neither load exists at IR level, so nothing in this pass sees
 * them -- it would go on believing a slot's value is in a register the
 * encoder had already reused (nested_struct_return printed dx + dy where it
 * meant p.y + dy).
 *
 * Refusing whenever the register is dead somewhere in between is correct but
 * throws away most of what this pass is for.  Reserving it instead makes the
 * assumption true: both scratch pickers -- tcc_ls_find_free_scratch_reg and
 * scratch_pushed_dead_reg -- union live_regs_by_instruction[] into what they
 * consider taken, and codegen has not run yet.  Nothing else owns the
 * register over this window, or it would have been live and there would be
 * nothing to reserve. */
static void ra_rl_reserve_regs(LSLiveIntervalState *ls, int r0, int r1, int lo, int hi)
{
  if (!ls->live_regs_by_instruction)
    return;
  for (int k = lo; k <= hi && k < ls->live_regs_by_instruction_size; ++k)
  {
    if (k < 0)
      continue;
    if (r0 >= 0)
      ls->live_regs_by_instruction[k] |= (1u << r0);
    if (r1 >= 0)
      ls->live_regs_by_instruction[k] |= (1u << r1);
  }
}

static int ra_redundant_reload_elim(TCCIRState *ir)
{
  LSLiveIntervalState *ls = &ir->ls;
  const int n = ir->next_instruction_index;
  int removed = 0;

  if (tcc_state->optimize < 1)
    return 0;
  if (tcc_ir_opt_pass_disabled("ra:reload_elim"))
    return 0;

  /* A slot whose address is taken anywhere can be written through that
   * pointer, so it is never tracked. */
  int escaped[RA_REL_MAX_TRACKED];
  int nescaped = 0;
  int escape_overflow = 0;
  for (int i = 0; i < n; ++i)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;
    for (int k = 0; k < 4; ++k)
    {
      IROperand op = (k == 0)   ? (irop_config[q->op].has_dest ? tcc_ir_op_get_dest(ir, q) : IROP_NONE)
                     : (k == 1) ? (irop_config[q->op].has_src1 ? tcc_ir_op_get_src1(ir, q) : IROP_NONE)
                     : (k == 2) ? (irop_config[q->op].has_src2 ? tcc_ir_op_get_src2(ir, q) : IROP_NONE)
                                : (q->op == TCCIR_OP_MLA ? tcc_ir_op_get_accum(ir, q) : IROP_NONE);
      if (op.tag != IROP_TAG_STACKOFF || op.is_llocal || op.is_lval)
        continue;
      if (irop_get_vreg(op) != -1)
        continue;
      int off = (int)irop_get_stack_offset(op); /* Addr[StackLoc[off]] */
      if (nescaped < RA_REL_MAX_TRACKED)
        escaped[nescaped++] = off;
      else
        escape_overflow = 1;
    }
  }

  RARelSlot tracked[RA_REL_MAX_TRACKED];
  int ntracked = 0;

  for (int i = 0; i < n; ++i)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];

    /* Control can arrive here from anywhere else; nothing carries over. */
    if (q->is_jump_target)
      ntracked = 0;
    if (q->op == TCCIR_OP_NOP)
      continue;

    switch (q->op)
    {
    /* A call, a write through a pointer or an indexed write can land on any
     * slot, and inline asm can do anything at all. */
    case TCCIR_OP_FUNCCALLVAL:
    case TCCIR_OP_FUNCCALLVOID:
    case TCCIR_OP_STORE_INDEXED:
    case TCCIR_OP_STORE_POSTINC:
    case TCCIR_OP_INLINE_ASM:
    case TCCIR_OP_ASM_INPUT:
    case TCCIR_OP_ASM_OUTPUT:
    case TCCIR_OP_SETJMP:
    case TCCIR_OP_NL_SETJMP:
    case TCCIR_OP_VLA_ALLOC:
    case TCCIR_OP_VLA_SP_RESTORE:
      ntracked = 0;
      continue;
    default:
      break;
    }

    int slot_off = 0;

    /* Drop a load that rewrites the very registers already holding the slot. */
    if (q->op == TCCIR_OP_LOAD && ntracked &&
        ra_rl_slot_offset(tcc_ir_op_get_src1(ir, q), &slot_off))
    {
      int d0 = 0, d1 = 0;
      IROperand dest = tcc_ir_op_get_dest(ir, q);
      int width = ra_rl_exact_width(irop_get_btype(tcc_ir_op_get_src1(ir, q)));
      if (width && !dest.is_lval && ra_rl_operand_regs(ir, dest, &d0, &d1) &&
          ((d1 >= 0) == (width == 8)))
      {
        for (int t = 0; t < ntracked; ++t)
        {
          if (!tracked[t].valid || tracked[t].off != slot_off ||
              tracked[t].size != width)
            continue;
          if (tracked[t].r0 != d0 || tracked[t].r1 != d1)
            break;
          ra_rl_reserve_regs(ls, tracked[t].r0, tracked[t].r1, tracked[t].def_idx, i);
          q->op = TCCIR_OP_NOP;
          removed++;
          RA_DBG("reload_elim @%d: slot %d already in R%d/R%d", i, slot_off, d0, d1);
          break;
        }
      }
      if (q->op == TCCIR_OP_NOP)
        continue;
    }

    /* Record a store, after invalidating whatever it overlaps. */
    if (q->op == TCCIR_OP_STORE &&
        ra_rl_slot_offset(tcc_ir_op_get_dest(ir, q), &slot_off))
    {
      IROperand val = tcc_ir_op_get_src1(ir, q);
      int s0 = 0, s1 = 0;
      int dst_bt = irop_get_btype(tcc_ir_op_get_dest(ir, q));
      int size = ra_rl_exact_width(dst_bt);
      int inval_size = size;
      if (!size)
      {
        /* A sub-word store writes at most two bytes, so invalidating a whole
         * word over-covers it; it leaves no register holding the slot, so it
         * records nothing.  Anything else (a struct copy) has a width this
         * code cannot see -- assume it reached everything. */
        if (dst_bt != IROP_BTYPE_INT8 && dst_bt != IROP_BTYPE_INT16)
        {
          ntracked = 0;
          continue;
        }
        inval_size = 4;
      }

      for (int t = 0; t < ntracked; ++t)
        if (tracked[t].valid && slot_off < tracked[t].off + tracked[t].size &&
            tracked[t].off < slot_off + inval_size)
          tracked[t].valid = 0;

      int is_escaped = escape_overflow;
      for (int e = 0; e < nescaped && !is_escaped; ++e)
        if (escaped[e] == slot_off)
          is_escaped = 1;

      if (size && !is_escaped && !val.is_lval && ra_rl_operand_regs(ir, val, &s0, &s1) &&
          ((s1 >= 0) == (size == 8)) && ra_rl_exact_width(irop_get_btype(val)) == size &&
          ntracked < RA_REL_MAX_TRACKED)
      {
        tracked[ntracked].off = slot_off;
        tracked[ntracked].size = size;
        tracked[ntracked].r0 = s0;
        tracked[ntracked].r1 = s1;
        tracked[ntracked].def_idx = i;
        tracked[ntracked].valid = 1;
        ntracked++;
      }
      continue;
    }

    /* A store to a slot we could not decode, or through a vreg, may hit
     * anything we are holding. */
    if (q->op == TCCIR_OP_STORE)
    {
      ntracked = 0;
      continue;
    }

    /* Any other write to a register drops the entries that named it. */
    if (irop_config[q->op].has_dest)
    {
      IROperand dest = tcc_ir_op_get_dest(ir, q);
      int d0 = 0, d1 = 0;
      if (!dest.is_lval && ra_rl_operand_regs(ir, dest, &d0, &d1))
      {
        for (int t = 0; t < ntracked; ++t)
          if (tracked[t].valid &&
              (tracked[t].r0 == d0 || tracked[t].r1 == d0 ||
               (d1 >= 0 && (tracked[t].r0 == d1 || tracked[t].r1 == d1))))
            tracked[t].valid = 0;
      }
      else if (!dest.is_lval && irop_get_vreg(dest) >= 0)
      {
        /* Destination register unknown -- assume it hit everything. */
        ntracked = 0;
      }
    }
  }

  return removed;
}

/* ============================================================================
 * Post-Allocation Dead Frame-Store Elimination
 *
 * The reload elimination above is what leaves these behind.  Every soft-float
 * routine opens by punning its arguments through a union:
 *
 *   StackLoc[-8] <-- R0(P0) [STORE]      ; ua.d = a
 *   R0(V0)       <-- StackLoc[-8] [LOAD] ; a_bits = ua.u
 *
 * Once the reload is gone nothing in the function reads that slot again, and
 * the store is `strd r0,r1,[sp,#N]` writing two words no one will ever look at.
 * __aeabi_dadd opens with two such slots, __aeabi_dmul with four.  The IR-level
 * dead-store pass (dce_dead_stackloc_stores) cannot reach them: it runs before
 * allocation, while the reload is still a reader.
 *
 * The rule is deliberately blunt -- a store dies only when NO instruction
 * anywhere in the function names an overlapping byte of its slot -- so it needs
 * no ordering or dominance reasoning and stays correct however control flows.
 * Anything that could reach a frame slot without naming it rules the whole
 * function out: an address taken of any slot, an indexed access based on one
 * (which reaches past the operand's own width), inline asm, setjmp, a VLA
 * moving sp, or a nested function's static chain.
 * ==========================================================================*/

/* Bytes an access to a frame slot covers, or 0 when the width is not
 * statically known -- a struct copy or a complex pair reaches further than its
 * component btype says. */
static int ra_dfs_width(IROperand op)
{
  if (op.is_complex)
    return 0;
  switch (irop_get_btype(op))
  {
  case IROP_BTYPE_INT8:
    return 1;
  case IROP_BTYPE_INT16:
    return 2;
  case IROP_BTYPE_INT32:
  case IROP_BTYPE_FLOAT32:
    return 4;
  case IROP_BTYPE_INT64:
  case IROP_BTYPE_FLOAT64:
    return 8;
  default:
    return 0;
  }
}

/* A plain frame slot operand: `StackLoc[off]` naming memory directly. */
static int ra_dfs_is_slot(IROperand op)
{
  return op.tag == IROP_TAG_STACKOFF && !op.is_sym && irop_get_vreg(op) == -1;
}

#define RA_DFS_MAX_READS 256

static int ra_dead_frame_store_elim(TCCIRState *ir)
{
  const int n = ir->next_instruction_index;
  int removed = 0;

  if (tcc_state->optimize < 1)
    return 0;
  if (tcc_ir_opt_pass_disabled("ra:dead_frame_store"))
    return 0;
  if (ir->has_static_chain)
    return 0;

  struct
  {
    int off;
    int size;
  } reads[RA_DFS_MAX_READS];
  int nreads = 0;

  for (int i = 0; i < n; ++i)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;

    switch (q->op)
    {
    case TCCIR_OP_INLINE_ASM:
    case TCCIR_OP_ASM_INPUT:
    case TCCIR_OP_ASM_OUTPUT:
    case TCCIR_OP_SETJMP:
    case TCCIR_OP_NL_SETJMP:
    case TCCIR_OP_VLA_ALLOC:
    case TCCIR_OP_VLA_SP_RESTORE:
    case TCCIR_OP_SET_CHAIN:
    case TCCIR_OP_INIT_CHAIN_SLOT:
      return 0;
    default:
      break;
    }

    /* An indexed or post-incrementing access based on a slot runs off the end
     * of the operand's own btype, so its width says nothing about what it
     * touched. */
    const int reaches_past_width =
        (q->op == TCCIR_OP_LOAD_INDEXED || q->op == TCCIR_OP_STORE_INDEXED ||
         q->op == TCCIR_OP_LOAD_POSTINC || q->op == TCCIR_OP_STORE_POSTINC);

    for (int k = 0; k < 4; ++k)
    {
      IROperand op = (k == 0)   ? (irop_config[q->op].has_dest ? tcc_ir_op_get_dest(ir, q) : IROP_NONE)
                     : (k == 1) ? (irop_config[q->op].has_src1 ? tcc_ir_op_get_src1(ir, q) : IROP_NONE)
                     : (k == 2) ? (irop_config[q->op].has_src2 ? tcc_ir_op_get_src2(ir, q) : IROP_NONE)
                                : (q->op == TCCIR_OP_MLA ? tcc_ir_op_get_accum(ir, q) : IROP_NONE);
      if (!ra_dfs_is_slot(op))
        continue;
      /* `Addr[StackLoc[..]]` hands the address to something this scan cannot
       * follow; is_llocal reaches memory through a pointer held in the slot. */
      if (!op.is_lval || op.is_llocal || reaches_past_width)
        return 0;
      /* The destination of a plain STORE is the write under consideration, not
       * a read.  Every other appearance keeps the slot alive. */
      if (k == 0 && q->op == TCCIR_OP_STORE)
        continue;
      int w = ra_dfs_width(op);
      if (w == 0 || nreads >= RA_DFS_MAX_READS)
        return 0;
      reads[nreads].off = (int)irop_get_stack_offset(op);
      reads[nreads].size = w;
      nreads++;
    }
  }

  for (int i = 0; i < n; ++i)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_STORE)
      continue;
    IROperand dest = tcc_ir_op_get_dest(ir, q);
    if (!ra_dfs_is_slot(dest) || !dest.is_lval || dest.is_llocal)
      continue;
    if (tcc_ir_access_is_volatile(ir, dest))
      continue;
    const int w = ra_dfs_width(dest);
    if (w == 0)
      continue;
    const int off = (int)irop_get_stack_offset(dest);
    int is_read = 0;
    for (int r = 0; r < nreads && !is_read; ++r)
      if (off < reads[r].off + reads[r].size && reads[r].off < off + w)
        is_read = 1;
    if (is_read)
      continue;
    q->op = TCCIR_OP_NOP;
    removed++;
    RA_DBG("dead_frame_store @%d: slot %d (%d bytes) is never read", i, off, w);
  }

  return removed;
}

/* ============================================================================
 * Post-Allocation Copy Propagation
 *
 * Move coalescing (below) removes a register copy by REASSIGNING one of its
 * endpoints so the move degenerates to `mov rX,rX`.  Both of its directions
 * need one endpoint's register to be free across the other's live range, so
 * neither can fire when the source OUTLIVES the copy: the source is still
 * sitting in its own register, which is precisely why that register is not
 * free.
 *
 * That is the dominant shape in soft float, where one union-punned 64-bit
 * value feeds a dozen inlined accessors:
 *
 *   R4(T8)  <-- ...                  ; a_bits, live to the end of the function
 *   R8(T18) <-- &R4(T8) [ASSIGN]     ; mov r8,r4 / mov r9,r5
 *   R8(T19) <-- &R8(T18) SHR #52     ; reads only the high half
 *
 * The complementary transform needs no free register at all: rewrite T18's
 * reads back to T8 and drop the copy.  That is legal exactly when T8's
 * register still holds T8 everywhere T18 is live, and the allocator's own
 * invariant -- one live interval per physical register at a time -- makes
 * that checkable by scanning the interval list for another claimant.
 *
 * Nothing here changes an allocation, so unlike every hint- or
 * forwarding-based attempt on these same copies, it cannot stretch a live
 * range and trade a move for a spill.  Each removed quad is a removed
 * instruction.
 * ==========================================================================*/

/* Ops whose operands are pinned to particular physical registers (ABI slots,
 * asm constraints, the static chain) or whose destination must not alias a
 * source: a read inside one of these cannot be redirected to another
 * register. */
static int ra_cp_use_is_redirectable(int op)
{
  switch (op)
  {
  case TCCIR_OP_RETURNVALUE:
  case TCCIR_OP_FUNCPARAMVAL:
  case TCCIR_OP_FUNCPARAMVOID:
  case TCCIR_OP_FUNCCALLVAL:
  case TCCIR_OP_FUNCCALLVOID:
  case TCCIR_OP_SET_CHAIN:
  case TCCIR_OP_ASM_INPUT:
  case TCCIR_OP_ASM_OUTPUT:
  case TCCIR_OP_INLINE_ASM:
  case TCCIR_OP_IJUMP:
  case TCCIR_OP_SWITCH_TABLE:
  case TCCIR_OP_SWITCH_LOAD:
  case TCCIR_OP_SETJMP:
  case TCCIR_OP_NL_SETJMP:
  case TCCIR_OP_UMULL:
  case TCCIR_OP_MLA:
  case TCCIR_OP_VLA_ALLOC:
  case TCCIR_OP_VLA_SP_SAVE:
  case TCCIR_OP_VLA_SP_RESTORE:
    return 0;
  default:
    return 1;
  }
}

/* Does any interval other than `keep` occupy one of `keep`'s registers at some
 * point in [lo,hi]?  If not, `keep`'s registers still hold `keep`'s value
 * across the whole window and reads of a copy of it can be redirected there. */
static int ra_cp_src_regs_clobbered(const LSLiveIntervalState *ls,
                                    const LSLiveInterval *keep,
                                    uint32_t lo, uint32_t hi)
{
  int regs[2];
  int nregs = 0;
  regs[nregs++] = keep->r0;
  if (keep->r1 >= 0 && keep->r1 < PREG_NONE && keep->r1 != keep->r0)
    regs[nregs++] = keep->r1;

  for (int j = 0; j < ls->next_interval_index; ++j)
  {
    const LSLiveInterval *x = &ls->intervals[j];
    if (x == keep || x->stack_location != 0)
      continue;
    if (x->start > hi || x->end < lo)
      continue;
    for (int r = 0; r < nregs; ++r)
      if (x->r0 == regs[r] || x->r1 == regs[r])
      {
        return 1;
      }
  }
  return 0;
}

/* Rewrite the reads of a copy's destination back to its source and drop the
 * copy.  Returns the number of copies removed. */
static int ra_copy_propagate(TCCIRState *ir)
{
  LSLiveIntervalState *ls = &ir->ls;
  const int n = ir->next_instruction_index;
  int removed = 0;

  if (tcc_state->optimize < 1)
    return 0;
  if (!ls->live_regs_by_instruction || ls->live_regs_by_instruction_size <= 0)
    return 0;
  if (tcc_ir_opt_pass_disabled("ra:copy_prop"))
    return 0;

  for (int i = 0; i < n; ++i)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_ASSIGN && q->op != TCCIR_OP_LOAD)
      continue;

    IROperand dest = tcc_ir_op_get_dest(ir, q);
    IROperand src = tcc_ir_op_get_src1(ir, q);
    /* A LOAD off a vreg whose interval ended up in a register performs no
     * memory access -- it is a register copy at codegen, and the IR generator
     * emits the inlined `temp = var` binding in exactly that form.  Skip
     * is_llocal (a real dereference through a pointer) and is_sym (a global).
     * Sub-word LOADs emit a UXTB/SXTH alongside the move to narrow the value,
     * so only full-width copies qualify. */
    int is_copy_load = 0;
    if (q->op == TCCIR_OP_LOAD)
    {
      int dbt = irop_get_btype(dest), sbt = irop_get_btype(src);
      /* is_lval on a STACKOFF source is the VAR's own slot being read, which
       * for a register-resident VAR is a register copy.  On a VREG source it
       * is a DEREFERENCE OF THAT REGISTER -- `R4 <- R3***DEREF***` reads the
       * memory R3 points at, and rewriting its readers to R3 hands them the
       * address instead of the value (pr90311: `b -= c < (unsigned char) a`
       * subtracted from &b).  is_llocal is not the flag that separates them;
       * a plain pointer dereference does not set it. */
      int deref_through_reg = (src.tag == IROP_TAG_VREG && src.is_lval);
      if ((src.tag == IROP_TAG_VREG || (src.tag == IROP_TAG_STACKOFF && src.is_local)) &&
          !deref_through_reg && !src.is_llocal && !src.is_sym && dbt == sbt &&
          (dbt == IROP_BTYPE_INT32 || dbt == IROP_BTYPE_INT64 || dbt == IROP_BTYPE_FUNC))
        is_copy_load = 1;
      if (!is_copy_load)
        continue;
    }
    /* Plain register-to-register only; an lval on either side is memory --
     * except a copy-LOAD, whose source carries is_lval for the VAR read. */
    if (dest.is_lval || (src.is_lval && !is_copy_load))
      continue;
    if (irop_get_btype(dest) != irop_get_btype(src))
      continue;

    int32_t dv = irop_get_vreg(dest);
    int32_t sv = irop_get_vreg(src);
    if (dv < 0 || sv < 0 || dv == sv)
      continue;
    if (!tcc_ir_vreg_is_valid(ir, dv) || !tcc_ir_vreg_is_valid(ir, sv))
      continue;
    /* The destination must be a temp: a VAR has a home beyond its register.
     * The source may be a VAR -- a register-resident one is just a register,
     * and it is the punned `a_bits` that every inlined accessor re-reads --
     * but then every write to it counts as a redefinition (below). */
    int src_is_var = (TCCIR_DECODE_VREG_TYPE(sv) == TCCIR_VREG_TYPE_VAR);
    if (TCCIR_DECODE_VREG_TYPE(dv) != TCCIR_VREG_TYPE_TEMP)
      continue;
    if (TCCIR_DECODE_VREG_TYPE(sv) != TCCIR_VREG_TYPE_TEMP && !src_is_var)
      continue;

    /* One interval per endpoint.  A vreg split across several intervals can
     * hold a DIFFERENT register in each, and the range reasoning below reads
     * a single interval's bounds -- picking one of several would let a read
     * be redirected into the register the other half was living in. */
    LSLiveInterval *src_iv = NULL, *dst_iv = NULL;
    int src_ivs = 0, dst_ivs = 0;
    for (int j = 0; j < ls->next_interval_index; ++j)
    {
      if (ls->intervals[j].vreg == (uint32_t)sv) { src_iv = &ls->intervals[j]; src_ivs++; }
      if (ls->intervals[j].vreg == (uint32_t)dv) { dst_iv = &ls->intervals[j]; dst_ivs++; }
    }
    if (!src_iv || !dst_iv || src_ivs != 1 || dst_ivs != 1)
      continue;
    /* PREG_NONE/PREG_SPILLED are >= 0 but are not registers. */
    if (src_iv->r0 < 0 || src_iv->r0 >= PREG_NONE)
      continue;
    if (src_iv->stack_location != 0)
      continue;
    /* A SPILLED destination is the better case, not a disqualifying one: the
     * copy the allocator could not keep in a register costs a store and a
     * reload at every read, and redirecting those reads to the source removes
     * all of it.  __aeabi_dadd's `b_bits` is exactly this -- one 64-bit ASSIGN
     * off the parameter, spilled, then loaded back four bytes at a time.  The
     * legality argument below never mentions the destination's register, only
     * that the SOURCE's registers still hold the source across the reads, so
     * it carries over unchanged. */
    const int dst_spilled = (dst_iv->r0 < 0 || dst_iv->r0 >= PREG_NONE || dst_iv->stack_location != 0);
    /* Both ends must sit in the same register class, and a 64-bit class means
     * a register PAIR: redirecting a paired read to an interval that only owns
     * r0 hands the backend a half-formed operand (it asserts on r1 == 31).  A
     * register-resident VAR can be exactly that. */
    if (src_iv->reg_type != dst_iv->reg_type)
      continue;
    if (!dst_spilled)
    {
      int src_pair = (src_iv->r1 >= 0 && src_iv->r1 < PREG_NONE);
      int dst_pair = (dst_iv->r1 >= 0 && dst_iv->r1 < PREG_NONE);
      if (src_pair != dst_pair)
        continue;
    }
    if (src_iv->addrtaken || dst_iv->addrtaken)
      continue;
    /* dst's register is shared with a graph-coalesced class that expects this
     * write to land; src is only read, so its class membership is harmless. */
    if (dst_iv->co_member)
      continue;
    /* The LS intervals above drive the allocator; codegen resolves an operand
     * through the IR-level interval's `allocation` instead, and the two are
     * not interchangeable -- a VAR can be register-resident there with no high
     * half, which the backend rejects the moment a 64-bit read names it.
     * Validate against the structure codegen will actually consult. */
    {
      IRLiveInterval *src_li = tcc_ir_vreg_live_interval(ir, sv);
      IRLiveInterval *dst_li = tcc_ir_vreg_live_interval(ir, dv);
      if (!src_li || !dst_li)
        continue;
      if (src_li->allocation.offset != 0)
        continue;
      if (ra_alloc_half_spilled(src_li->allocation.r0) ||
          ra_alloc_half_spilled(src_li->allocation.r1))
        continue;
      if (ra_alloc_half_unset(src_li->allocation.r0))
        continue;
      /* The source must carry the full width the reads expect.  With a
       * register-resident destination that is "both are pairs or neither is";
       * with a spilled one there is no destination pair to compare against, so
       * the operand btype -- already required equal above -- decides. */
      const int src_paired = ((src_li->allocation.r1 & PREG_REG_NONE) != PREG_REG_NONE);
      if (!dst_spilled)
      {
        if (dst_li->allocation.offset != 0)
          continue;
        if (ra_alloc_half_spilled(dst_li->allocation.r0) ||
            ra_alloc_half_spilled(dst_li->allocation.r1))
          continue;
        if (ra_alloc_half_unset(dst_li->allocation.r0))
          continue;
        if (src_paired != ((dst_li->allocation.r1 & PREG_REG_NONE) != PREG_REG_NONE))
          continue;
      }
      else if (src_paired != (irop_get_btype(dest) == IROP_BTYPE_INT64 ||
                              irop_get_btype(dest) == IROP_BTYPE_FLOAT64))
        continue;
    }

    /* Already the same register: codegen elides it, nothing to remove. */
    if (src_iv->r0 == dst_iv->r0 && src_iv->r1 == dst_iv->r1)
      continue;
    /* src must already be live at the copy. */
    if (src_iv->start > (uint32_t)i)
      continue;

    /* Collect dst's reads.  Require exactly one definition (this copy), every
     * read after it, and every read in an op whose operands are not pinned. */
    int ok = 1;
    int nuses = 0;
    int last_use = i;
    for (int k = 0; k < n; ++k)
    {
      IRQuadCompact *qk = &ir->compact_instructions[k];
      if (k == i || qk->op == TCCIR_OP_NOP)
        continue;
      if (irop_config[qk->op].has_dest &&
          irop_get_vreg(tcc_ir_op_get_dest(ir, qk)) == dv)
      { ok = 0; break; } /* redefined, or written through as an address */

      int reads = 0;
      if (irop_config[qk->op].has_src1 &&
          irop_get_vreg(tcc_ir_op_get_src1(ir, qk)) == dv)
        reads = 1;
      if (!reads && irop_config[qk->op].has_src2 &&
          irop_get_vreg(tcc_ir_op_get_src2(ir, qk)) == dv)
        reads = 1;
      if (!reads && qk->op == TCCIR_OP_MLA &&
          irop_get_vreg(tcc_ir_op_get_accum(ir, qk)) == dv)
        reads = 1;
      if (!reads)
        continue;

      if (k < i || !ra_cp_use_is_redirectable(qk->op))
      { ok = 0; break; }
      nuses++;
      if (k > last_use)
        last_use = k;
    }
    /* nuses == 0 is a dead copy; ra_eliminate_dead_reg_copies owns that. */
    if (!ok || nuses == 0)
      continue;

    /* src's value must survive to the last redirected read. */
    if (src_iv->end < (uint32_t)last_use)
      continue;
    /* ...and no other interval may claim src's registers over that window. */
    if (ra_cp_src_regs_clobbered(ls, src_iv, (uint32_t)i, (uint32_t)last_use))
      continue;
    /* ...and src itself must not be redefined under the reads. */
    for (int k = i + 1; k <= last_use; ++k)
    {
      IRQuadCompact *qk = &ir->compact_instructions[k];
      if (qk->op == TCCIR_OP_NOP || !irop_config[qk->op].has_dest)
        continue;
      IROperand dk = tcc_ir_op_get_dest(ir, qk);
      /* For a temp, an lval destination is a store THROUGH it (an address
       * use), not a new value.  For a VAR it is a write to the variable, so
       * there is no exemption. */
      if (dk.is_lval && !src_is_var)
        continue;
      if (irop_get_vreg(dk) == sv)
      { ok = 0; break; }
    }
    if (!ok)
      continue;

    for (int k = i + 1; k <= last_use; ++k)
    {
      IRQuadCompact *qk = &ir->compact_instructions[k];
      if (qk->op == TCCIR_OP_NOP)
        continue;
      if (irop_config[qk->op].has_src1)
      {
        IROperand o = tcc_ir_op_get_src1(ir, qk);
        if (irop_get_vreg(o) == dv)
        { irop_set_vreg(&o, sv); tcc_ir_set_src1(ir, k, o); }
      }
      if (irop_config[qk->op].has_src2)
      {
        IROperand o = tcc_ir_op_get_src2(ir, qk);
        if (irop_get_vreg(o) == dv)
        { irop_set_vreg(&o, sv); tcc_ir_set_src2(ir, k, o); }
      }
    }
    q->op = TCCIR_OP_NOP;
    removed++;
    RA_DBG("copy_prop @%d: T%d -> T%d (R%d), %d use(s) to %d", i,
           (int)(dv & 0xffffff), (int)(sv & 0xffffff), src_iv->r0, nuses, last_use);
  }

  return removed;
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

/* Is `vr` mentioned by any instruction at all? */
static int ra_rp_vreg_mentioned(TCCIRState *ir, uint32_t vr)
{
  const int n = ir->next_instruction_index;
  for (int k = 0; k < n; ++k)
  {
    IRQuadCompact *qk = &ir->compact_instructions[k];
    if (qk->op == TCCIR_OP_NOP)
      continue;
    if (irop_config[qk->op].has_dest &&
        (uint32_t)irop_get_vreg(tcc_ir_op_get_dest(ir, qk)) == vr)
      return 1;
    if (irop_config[qk->op].has_src1 &&
        (uint32_t)irop_get_vreg(tcc_ir_op_get_src1(ir, qk)) == vr)
      return 1;
    if (irop_config[qk->op].has_src2 &&
        (uint32_t)irop_get_vreg(tcc_ir_op_get_src2(ir, qk)) == vr)
      return 1;
    if (qk->op == TCCIR_OP_MLA &&
        (uint32_t)irop_get_vreg(tcc_ir_op_get_accum(ir, qk)) == vr)
      return 1;
  }
  return 0;
}

/* Is any of `keep`'s registers claimed by another interval over [lo,hi]?
 *
 * ra_cp_src_regs_clobbered answers the same question, but every pass that
 * deletes an instruction leaves the interval of the value it computed behind:
 * the vreg is then mentioned nowhere, yet its interval still claims its
 * registers over the range it used to occupy.  Such a phantom holds no value
 * and cannot clobber anything -- and because it sits exactly where the deleted
 * instruction was, it lands on the one-instruction window this pass asks
 * about far more often than its share.  So a claim is only believed once its
 * vreg is shown to still exist in the instruction stream.
 */
static int ra_rp_dst_regs_busy(TCCIRState *ir, const LSLiveIntervalState *ls,
                               const LSLiveInterval *keep, uint32_t lo, uint32_t hi)
{
  int regs[2];
  int nregs = 0;
  regs[nregs++] = keep->r0;
  if (keep->r1 >= 0 && keep->r1 < PREG_NONE && keep->r1 != keep->r0)
    regs[nregs++] = keep->r1;

  for (int j = 0; j < ls->next_interval_index; ++j)
  {
    const LSLiveInterval *x = &ls->intervals[j];
    if (x == keep || x->stack_location != 0)
      continue;
    if (x->start > hi || x->end < lo)
      continue;
    int hits = 0;
    for (int r = 0; r < nregs; ++r)
      if (x->r0 == regs[r] || x->r1 == regs[r])
        hits = 1;
    if (!hits)
      continue;
    if (ra_rp_vreg_mentioned(ir, x->vreg))
      return 1;
  }
  return 0;
}

/* Which producers may have their destination register changed.  This is an
 * ALLOW list, not a deny list: an op earns a place only when its destination
 * is a plain register write the encoder is free to direct anywhere.  A fixed
 * destination (a call's r0, UMULL's pair, the division helpers), a
 * two-address one (BFI presets its dest to the host word) and one the encoder
 * also reads (the postinc forms write back through their base, LDRD against
 * its own base is unpredictable) all stay off it. */
static int ra_rp_def_is_retargetable(int op)
{
  switch (op)
  {
  case TCCIR_OP_ADD:
  case TCCIR_OP_SUB:
  case TCCIR_OP_MUL:
  case TCCIR_OP_AND:
  case TCCIR_OP_OR:
  case TCCIR_OP_XOR:
  case TCCIR_OP_SHL:
  case TCCIR_OP_SAR:
  case TCCIR_OP_SHR:
  case TCCIR_OP_UBFX:
  case TCCIR_OP_SBFX:
  case TCCIR_OP_ZEXT:
  case TCCIR_OP_SETIF:
  case TCCIR_OP_ASSIGN:
    return 1;
  default:
    return 0;
  }
}

/* Retarget the producer of a dying copy source, and drop the copy.
 *
 *     ubfx  r9, r1, #20, #11        ubfx  sl, r1, #20, #11
 *     mov   sl, r9              ->
 *
 * This is the backward sibling of ra_copy_propagate.  Copy propagation
 * rewrites the reads of a copy's DESTINATION back to its source, so it needs
 * the source to still be the occupant of its register across every one of
 * them -- a long-lived destination therefore rules it out.  Retargeting goes
 * the other way: the producer writes the destination's register directly, the
 * copy disappears, and whatever reads of the source remain are rewritten to
 * the destination, which holds the identical value from the producer onwards.
 *
 * Nothing here depends on how long the destination lives.  What it costs is
 * that the destination is born one instruction earlier, so its register has
 * to be free over that one instruction -- and, when the source outlives the
 * copy, that the source's last read still falls inside the destination's own
 * live range, so no range grows at the far end either.
 *
 * The move coalescer's forward direction covers the same shape by giving the
 * destination the SOURCE's register, but only when that register is free
 * across the destination's whole live range, which for a value computed early
 * and used late it usually is not.  Retargeting asks for far less: the
 * destination's register must be free over the single instruction the value's
 * definition moves back by.
 *
 * Every candidate is checked against the whole function rather than against
 * the live intervals alone, so an interval that over-approximates cannot make
 * a read of the source disappear.
 */
static int ra_retarget_producer(TCCIRState *ir)
{
  LSLiveIntervalState *ls = &ir->ls;
  const int n = ir->next_instruction_index;
  int retargeted = 0;

  if (tcc_state->optimize < 1)
    return 0;
  if (!ls->live_regs_by_instruction || ls->live_regs_by_instruction_size <= 0)
    return 0;
  if (tcc_ir_opt_pass_disabled("ra:retarget_producer"))
    return 0;
  const int tbl_size = ls->live_regs_by_instruction_size;

  for (int i = 0; i < n; ++i)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_ASSIGN)
      continue;
    /* Control may reach the copy without passing the producer, in which case
     * the copy is the only thing establishing the destination on that path. */
    if (q->is_jump_target)
      continue;

    IROperand dest = tcc_ir_op_get_dest(ir, q);
    IROperand src = tcc_ir_op_get_src1(ir, q);
    if (dest.is_lval || src.is_lval)
      continue;
    if (irop_get_btype(dest) != irop_get_btype(src))
      continue;

    int32_t dv = irop_get_vreg(dest);
    int32_t sv = irop_get_vreg(src);
    if (dv < 0 || sv < 0 || dv == sv)
      continue;
    if (!tcc_ir_vreg_is_valid(ir, dv) || !tcc_ir_vreg_is_valid(ir, sv))
      continue;
    /* Both ends must be temps.  A VAR has a home beyond its register, and the
     * source's disappears entirely here. */
    if (TCCIR_DECODE_VREG_TYPE(dv) != TCCIR_VREG_TYPE_TEMP ||
        TCCIR_DECODE_VREG_TYPE(sv) != TCCIR_VREG_TYPE_TEMP)
      continue;

    /* The producer is the instruction before the copy; NOPs left by earlier
     * passes do not count as instructions, but a NOP that is a jump target
     * still lets control in between the two. */
    int d = -1;
    for (int k = i - 1; k >= 0; --k)
    {
      IRQuadCompact *qk = &ir->compact_instructions[k];
      if (qk->op != TCCIR_OP_NOP) { d = k; break; }
      if (qk->is_jump_target) break;
    }
    if (d < 0)
      continue;
    IRQuadCompact *pq = &ir->compact_instructions[d];
    if (!ra_rp_def_is_retargetable(pq->op) || !irop_config[pq->op].has_dest)
      continue;
    IROperand pd = tcc_ir_op_get_dest(ir, pq);
    if (pd.is_lval || irop_get_vreg(pd) != sv)
      continue;
    /* The width the encoder lowers by is the DESTINATION's, and passes that
     * narrow a value retype operands in place -- so the producer's own view of
     * its destination must be the width the copy moves. */
    if (irop_get_btype(pd) != irop_get_btype(dest))
      continue;

    /* Exactly one interval per endpoint: a split vreg holds a different
     * register in each half, and the reasoning below reads one pair of
     * bounds. */
    LSLiveInterval *src_iv = NULL, *dst_iv = NULL;
    int src_ivs = 0, dst_ivs = 0;
    for (int j = 0; j < ls->next_interval_index; ++j)
    {
      if (ls->intervals[j].vreg == (uint32_t)sv) { src_iv = &ls->intervals[j]; src_ivs++; }
      if (ls->intervals[j].vreg == (uint32_t)dv) { dst_iv = &ls->intervals[j]; dst_ivs++; }
    }
    if (!src_iv || !dst_iv || src_ivs != 1 || dst_ivs != 1)
      continue;
    /* PREG_NONE/PREG_SPILLED are >= 0 but are not registers.  Both ends must
     * be register-resident: a spilled destination would turn the producer's
     * write into a spill store, which is not what this is measuring. */
    if (src_iv->r0 < 0 || src_iv->r0 >= PREG_NONE || src_iv->stack_location != 0)
      continue;
    if (dst_iv->r0 < 0 || dst_iv->r0 >= PREG_NONE || dst_iv->stack_location != 0)
      continue;
    if (src_iv->reg_type != dst_iv->reg_type)
      continue;
    /* A 64-bit class means a register PAIR; handing the encoder a destination
     * that owns only its low half aborts it (r1 == 31). */
    {
      int src_pair = (src_iv->r1 >= 0 && src_iv->r1 < PREG_NONE);
      int dst_pair = (dst_iv->r1 >= 0 && dst_iv->r1 < PREG_NONE);
      if (src_pair != dst_pair)
        continue;
    }
    if (src_iv->addrtaken || dst_iv->addrtaken)
      continue;
    /* A graph-coalesced interval shares one register with its whole class. */
    if (src_iv->co_member || dst_iv->co_member)
      continue;
    /* Already the same register: codegen elides the move, nothing to remove. */
    if (src_iv->r0 == dst_iv->r0 && src_iv->r1 == dst_iv->r1)
      continue;

    /* Codegen resolves an operand through the IR-level interval's allocation,
     * not through the LS one, and the two are not interchangeable. */
    {
      IRLiveInterval *src_li = tcc_ir_vreg_live_interval(ir, sv);
      IRLiveInterval *dst_li = tcc_ir_vreg_live_interval(ir, dv);
      if (!src_li || !dst_li)
        continue;
      if (dst_li->allocation.offset != 0)
        continue;
      if (ra_alloc_half_spilled(dst_li->allocation.r0) ||
          ra_alloc_half_spilled(dst_li->allocation.r1))
        continue;
      if (ra_alloc_half_unset(dst_li->allocation.r0))
        continue;
      if (ra_alloc_half_unset(src_li->allocation.r1) !=
          ra_alloc_half_unset(dst_li->allocation.r1))
        continue;
    }

    /* The source is born at the producer, the destination at the copy, and
     * the source does not outlive the destination -- the value ends up in the
     * destination's register, which is guaranteed to be the destination's own
     * only up to where the allocator said it ends. */
    if (src_iv->start != (uint32_t)d)
      continue;
    if (dst_iv->start != (uint32_t)i)
      continue;
    if (src_iv->end > dst_iv->end)
      continue;

    /* The destination's registers must be free from the producer to the last
     * read that will be redirected onto them.  Below the copy that window is
     * inside the destination's own range and the check is a formality; above
     * it, it is what makes moving the definition back safe -- and it is also
     * what keeps the producer's operands out of the way, since a source of
     * the producer is live at the producer, and a multi-instruction lowering
     * need not read all of its operands before writing its first half. */
    {
      uint32_t hi = src_iv->end > (uint32_t)i ? src_iv->end : (uint32_t)i;
      if (ra_rp_dst_regs_busy(ir, ls, dst_iv, (uint32_t)d, hi))
        continue;
    }

    /* Now against the instruction stream rather than the intervals, so that
     * an interval which over-approximates cannot hide a mention.  Outside the
     * producer and the copy, the source may only be READ, only below the copy
     * and inside the destination's range, and only by an op whose operands are
     * not pinned.  A mention in a DEST operand is refused whatever it means --
     * a redefinition invalidates the rewrite, and a store THROUGH the source
     * is a read this pass does not rewrite.  The destination, in turn, must
     * not be written anywhere but the copy. */
    int ok = 1;
    int last_use = i;
    for (int k = 0; k < n; ++k)
    {
      if (k == d || k == i)
        continue;
      IRQuadCompact *qk = &ir->compact_instructions[k];
      if (qk->op == TCCIR_OP_NOP)
        continue;
      /* Control must not be able to enter at the copy: on such an edge the
       * copy is the only thing that establishes the destination, and the
       * producer that would replace it never ran.  `is_jump_target` says this
       * already and is checked above, but it is a bit many passes maintain by
       * hand, so the branches are also read directly.  A computed target is
       * refused outright because it names no index to compare against. */
      if (qk->op == TCCIR_OP_IJUMP || qk->op == TCCIR_OP_SWITCH_TABLE ||
          qk->op == TCCIR_OP_SWITCH_LOAD || qk->op == TCCIR_OP_SETJMP ||
          qk->op == TCCIR_OP_NL_SETJMP)
      { ok = 0; break; }
      if ((qk->op == TCCIR_OP_JUMP || qk->op == TCCIR_OP_JUMPIF) &&
          (int)irop_get_imm64_ex(ir, tcc_ir_op_get_dest(ir, qk)) == i)
      { ok = 0; break; }
      if (irop_config[qk->op].has_dest &&
          irop_get_vreg(tcc_ir_op_get_dest(ir, qk)) == sv)
      { ok = 0; break; }
      int reads = 0;
      if (irop_config[qk->op].has_src1 &&
          irop_get_vreg(tcc_ir_op_get_src1(ir, qk)) == sv)
        reads = 1;
      if (!reads && irop_config[qk->op].has_src2 &&
          irop_get_vreg(tcc_ir_op_get_src2(ir, qk)) == sv)
        reads = 1;
      if (!reads && qk->op == TCCIR_OP_MLA &&
          irop_get_vreg(tcc_ir_op_get_accum(ir, qk)) == sv)
        reads = 1;
      if (!reads)
        continue;
      if (k < i || (uint32_t)k > dst_iv->end || !ra_cp_use_is_redirectable(qk->op))
      { ok = 0; break; }
      if (k > last_use)
        last_use = k;
    }
    if (!ok)
      continue;
    /* The redirected reads must all see the value the producer wrote, so the
     * destination may not be given a new one underneath them.  Only the span
     * that actually carries a redirected read matters: a temp reassigned
     * beyond the last of them is none of this transform's business. */
    for (int k = i + 1; k <= last_use; ++k)
    {
      IRQuadCompact *qk = &ir->compact_instructions[k];
      if (qk->op == TCCIR_OP_NOP)
        continue;
      if (irop_config[qk->op].has_dest &&
          irop_get_vreg(tcc_ir_op_get_dest(ir, qk)) == dv)
      { ok = 0; break; }
    }
    if (!ok)
      continue;

    irop_set_vreg(&pd, dv);
    tcc_ir_set_dest(ir, d, pd);
    q->op = TCCIR_OP_NOP;
    for (int k = i + 1; k <= last_use; ++k)
    {
      IRQuadCompact *qk = &ir->compact_instructions[k];
      if (qk->op == TCCIR_OP_NOP)
        continue;
      if (irop_config[qk->op].has_src1)
      {
        IROperand o = tcc_ir_op_get_src1(ir, qk);
        if (irop_get_vreg(o) == sv)
        { irop_set_vreg(&o, dv); tcc_ir_set_src1(ir, k, o); }
      }
      if (irop_config[qk->op].has_src2)
      {
        IROperand o = tcc_ir_op_get_src2(ir, qk);
        if (irop_get_vreg(o) == sv)
        { irop_set_vreg(&o, dv); tcc_ir_set_src2(ir, k, o); }
      }
    }

    /* The destination is now born at the producer.  The source's interval is
     * left claiming its own registers over [d,i]; nothing writes them any
     * more, but pretending they are still busy only costs later passes an
     * opportunity, where clearing them wrongly would cost correctness. */
    dst_iv->start = (uint32_t)d;
    {
      IRLiveInterval *dst_li = tcc_ir_vreg_live_interval(ir, dv);
      if (dst_li && dst_li->start > (uint32_t)d)
        dst_li->start = (uint32_t)d;
    }
    for (int k = d; k <= i && k < tbl_size; ++k)
    {
      ls->live_regs_by_instruction[k] |= (1u << dst_iv->r0);
      if (dst_iv->r1 >= 0 && dst_iv->r1 < PREG_NONE)
        ls->live_regs_by_instruction[k] |= (1u << dst_iv->r1);
    }
    retargeted++;
    RA_DBG("retarget_producer @%d: %s dest T%d -> T%d (R%d), reads to %d", d,
           tcc_ir_get_op_name((TccIrOp)pq->op), (int)(sv & 0xffffff),
           (int)(dv & 0xffffff), dst_iv->r0, last_use);
  }

  return retargeted;
}

/* ── ra:load_postinc / ra:store_postinc ──
 *
 * `D <- P***DEREF***` immediately followed (in the same straight-line region)
 * by `P <- P + #k` is ARM's post-indexed load, `ldr D,[P],#k`; the mirrored
 * `P***DEREF*** <- V; P <- P + #k` is the post-indexed store, `str V,[P],#k`.
 * The backend has emitted both forms since forever
 * (tcc_gen_machine_load/store_postinc_mop), but until now NOTHING in the
 * pipeline ever produced the ops, so the whole addressing mode was dead code.
 * Every IV-strength-reduced pointer walk paid a separate `adds P,#k`; on the
 * M33 the post-increment is also a cycle cheaper than the separate add (docs,
 * and tcc-m33-addressing-mode-cycle-costs).
 *
 * The store arm only sees IV-strength-reduced walks (`a[i] = v` loops turned
 * into a pointer walk).  A source-level `*p++ = v` does NOT lower to this
 * shape but to a copy of the pointer followed by the increment
 * (`T <- P; P <- T + k; T***DEREF*** <- V`); that needs its own matcher plus
 * a liveness proof that T is dead, and is still absent.
 *
 * This runs POST-allocation deliberately.  Pre-RA the fused op writes P without
 * the allocator being told (irop_config says the POSTINC ops read P as a USE
 * only), and P is exactly the loop-carried value the allocator most needs to
 * model.  After allocation both registers are already fixed and folding the
 * add changes no allocation at all.
 *
 * The operand layout is the indexed-op one: two value slots, unused, increment
 * at operand_base+3 (codegen reads it via tcc_ir_op_get_scale), so the fused
 * instruction takes four fresh pool slots rather than reusing the original
 * op's two.
 */
static int ra_load_postinc_fuse(TCCIRState *ir)
{
  const int n = ir->next_instruction_index;
  int fused = 0;

  if (tcc_state->optimize < 1)
    return 0;
  int load_disabled = tcc_ir_opt_pass_disabled("ra:load_postinc");
  int store_disabled = tcc_ir_opt_pass_disabled("ra:store_postinc");
  if (load_disabled && store_disabled)
    return 0;

  for (int i = 0; i < n; ++i)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    int is_load = (q->op == TCCIR_OP_LOAD);
    int is_store = (q->op == TCCIR_OP_STORE);
    if (!is_load && !is_store)
      continue;
    if (is_load ? load_disabled : store_disabled)
      continue;

    /* LOAD: `value <- ptr***DEREF***` — ptr is src1, an lvalue.
     * STORE: `ptr***DEREF*** <- value` — ptr is the dest, an lvalue. */
    IROperand ptr = is_load ? tcc_ir_op_get_src1(ir, q) : tcc_ir_op_get_dest(ir, q);
    IROperand value = is_load ? tcc_ir_op_get_dest(ir, q) : tcc_ir_op_get_src1(ir, q);

    /* A dereference THROUGH a register: `R4 <- R3***DEREF***`.  is_local /
     * is_llocal / is_sym are the frame-slot and global forms, which name memory
     * some other way and have no pointer register to write back. */
    if (irop_get_tag(ptr) != IROP_TAG_VREG || !ptr.is_lval)
      continue;
    if (ptr.is_local || ptr.is_llocal || ptr.is_sym || ptr.is_complex)
      continue;
    /* No volatility guard is needed: the fusion neither removes, duplicates nor
     * reorders the access.  `ldr D,[P],#k` performs exactly the access the
     * original did, in the same place; only the address arithmetic moves. */
    if (value.is_lval)
      continue;
    /* load_postinc_mop takes the access width from the DEST operand (and
     * store_postinc_mop from the VALUE operand), while a plain LOAD/STORE
     * takes it from the lvalue.  Requiring the two to agree makes the forms
     * equivalent; a widening load (`int c = *bytep`, dest INT32 over an INT8
     * access) would otherwise silently become LDR.  Signedness only matters
     * for loads — a store of width N writes the low N bytes either way.
     * 64-bit is LDRD/STRD-with-writeback and stays out until it has its own
     * test. */
    {
      int vbt = irop_get_btype(value), pbt = irop_get_btype(ptr);
      if (vbt != pbt || (is_load && value.is_unsigned != ptr.is_unsigned))
        continue;
      if (vbt != IROP_BTYPE_INT32 && vbt != IROP_BTYPE_INT16 && vbt != IROP_BTYPE_INT8)
        continue;
    }

    int32_t pv = irop_get_vreg(ptr);
    int32_t dv = irop_get_vreg(value);
    if (pv < 0 || dv < 0 || pv == dv)
      continue;

    /* The pointer must live in a real register: load_postinc_mop writes the
     * incremented address back to the register it found the base in, and never
     * to a spill slot, so a spilled P would silently lose the increment. */
    {
      IRLiveInterval *pli = tcc_ir_vreg_live_interval(ir, pv);
      if (!pli || pli->allocation.offset != 0)
        continue;
      if (ra_alloc_half_unset(pli->allocation.r0) ||
          ra_alloc_half_spilled(pli->allocation.r0))
        continue;
    }

    /* Scan forward for `P <- P + #k`, bailing on anything that would change
     * what the fusion means.  The jump-target check comes BEFORE the NOP
     * skip: a NOPed quad that is still a jump target means a path enters the
     * window that never executed the access, and once the add is folded away
     * that path would leave P un-incremented. */
    int add_idx = -1;
    int assign_idx = -1; /* the `P <- T` half of a renamed split, if any */
    for (int j = i + 1; j < n; ++j)
    {
      IRQuadCompact *jq = &ir->compact_instructions[j];
      /* Leaving the region: an instruction reached from elsewhere may arrive
       * with P un-incremented, and a branch may skip the add entirely. */
      if (jq->is_jump_target)
        break;
      if (jq->op == TCCIR_OP_NOP)
        continue;
      if (jq->op == TCCIR_OP_JUMP || jq->op == TCCIR_OP_JUMPIF ||
          jq->op == TCCIR_OP_IJUMP || jq->op == TCCIR_OP_SWITCH_TABLE ||
          jq->op == TCCIR_OP_RETURNVALUE || jq->op == TCCIR_OP_RETURNVOID)
        break;

      if (jq->op == TCCIR_OP_ADD)
      {
        IROperand ad = tcc_ir_op_get_dest(ir, jq);
        IROperand a1 = tcc_ir_op_get_src1(ir, jq);
        IROperand a2 = tcc_ir_op_get_src2(ir, jq);
        if (!ad.is_lval && irop_get_vreg(a1) == pv && irop_is_immediate(a2))
        {
          int64_t k = irop_get_imm64_ex(ir, a2);
          int32_t av = irop_get_vreg(ad);
          if (k >= 1 && k <= 255 && av == pv)
          {
            add_idx = j; /* in-place: P <- P + #k */
          }
          else if (k >= 1 && k <= 255 && av >= 0)
          {
            /* SSA rename splits the in-place bump into `T <- P + #k` followed
             * by `P <- T [ASSIGN]`.  That is still the same increment when T
             * is born at the ADD and dies at the copy — prove it with the
             * allocator's own intervals, then fold and NOP both halves.
             * Between the two halves only non-jump-target NOPs may sit. */
            for (int m = j + 1; m < n; ++m)
            {
              IRQuadCompact *mq = &ir->compact_instructions[m];
              if (mq->is_jump_target)
                break;
              if (mq->op == TCCIR_OP_NOP)
                continue;
              if (mq->op == TCCIR_OP_ASSIGN)
              {
                IROperand cd = tcc_ir_op_get_dest(ir, mq);
                IROperand cs = tcc_ir_op_get_src1(ir, mq);
                if (!cd.is_lval && !cs.is_lval && irop_get_vreg(cd) == pv &&
                    irop_get_vreg(cs) == av)
                {
                  IRLiveInterval *tli = tcc_ir_vreg_live_interval(ir, av);
                  if (tli && tli->start == (uint32_t)j && tli->end == (uint32_t)m)
                  {
                    add_idx = j;
                    assign_idx = m;
                  }
                }
              }
              break; /* first non-NOP decides either way */
            }
          }
        }
        if (add_idx >= 0)
          break;
      }

      /* Any other read or write of P in between would see the wrong value
       * once the increment moves up to the load. */
      {
        int touches = 0;
        for (int k = 0; k < 3 && !touches; ++k)
        {
          IROperand o;
          if (k == 0)
          {
            if (!irop_config[jq->op].has_dest)
              continue;
            o = tcc_ir_op_get_dest(ir, jq);
          }
          else if (k == 1)
          {
            if (!irop_config[jq->op].has_src1)
              continue;
            o = tcc_ir_op_get_src1(ir, jq);
          }
          else
          {
            if (!irop_config[jq->op].has_src2)
              continue;
            o = tcc_ir_op_get_src2(ir, jq);
          }
          if (irop_get_vreg(o) == pv)
            touches = 1;
        }
        if (jq->op == TCCIR_OP_MLA && irop_get_vreg(tcc_ir_op_get_accum(ir, jq)) == pv)
          touches = 1;
        if (touches)
          break;
      }
    }
    if (add_idx < 0)
      continue;

    IRQuadCompact *aq = &ir->compact_instructions[add_idx];
    IROperand incr = tcc_ir_op_get_src2(ir, aq);

    /* Four fresh pool slots: dest, ptr, unused, increment (see the header). */
    int nb = ir->iroperand_pool_count;
    tcc_ir_pool_add(ir, IROP_NONE);
    tcc_ir_pool_add(ir, IROP_NONE);
    tcc_ir_pool_add(ir, IROP_NONE);
    tcc_ir_pool_add(ir, IROP_NONE);
    if (nb + 3 >= ir->iroperand_pool_capacity)
      continue;

    q->op = is_load ? TCCIR_OP_LOAD_POSTINC : TCCIR_OP_STORE_POSTINC;
    q->operand_base = (uint32_t)nb;
    /* The backend wants the POINTER VALUE here, not the lvalue it dereferences:
     * mach_ensure_in_reg on an lval operand emits a load OF the pointer from
     * the memory it names, and the writeback then lands in a scratch. */
    IROperand base = ptr;
    base.is_lval = 0;
    base.aux = 0;
    /* LOAD_POSTINC decodes {dest=value, src1=base}; STORE_POSTINC decodes
     * {dest=base, src1=value} — the same slots as the plain ops they replace. */
    ir->iroperand_pool[nb + 0] = is_load ? value : base;
    ir->iroperand_pool[nb + 1] = is_load ? base : value;
    ir->iroperand_pool[nb + 2] = IROP_NONE;
    ir->iroperand_pool[nb + 3] = incr;
    aq->op = TCCIR_OP_NOP;
    if (assign_idx >= 0)
      ir->compact_instructions[assign_idx].op = TCCIR_OP_NOP; /* the `P <- T` half */
    fused++;
    RA_DBG("%s_postinc @%d: folded +%d from @%d", is_load ? "load" : "store", i,
           (int)irop_get_imm64_ex(ir, incr), add_idx);
  }

  return fused;
}

int tcc_ir_move_coalescing(TCCIRState *ir)
{
  LSLiveIntervalState *ls = &ir->ls;

  /* Copies whose source outlives them cannot be coalesced by reassignment;
   * propagate those away first so the loop below only sees the rest. */
  int propagated = ra_copy_propagate(ir);
  /* Then the copies whose source is born one instruction earlier, which have a
   * producer to retarget instead of reads to redirect. */
  int retargeted = ra_retarget_producer(ir);
  /* Drop reloads of a slot into the registers that already hold it -- the
   * union punning every soft-float routine opens with. */
  int reloads = ra_redundant_reload_elim(ir);
  /* And then the stores those reloads were the only reader of. */
  reloads += ra_dead_frame_store_elim(ir);

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

  /* Last: the IR is in its final shape, so an increment folded into a load
   * cannot be moved apart again. */
  int postinc = ra_load_postinc_fuse(ir);

  if (TCC_LOG_LS)
    LOG_LS("copy_stats propagated=%d reloads=%d coalesced=%d retargeted=%d reaching_codegen=%d",
           propagated, reloads, coalesced, retargeted,
           ra_count_copies_reaching_codegen(ir));

  return coalesced + propagated + reloads + retargeted + postinc;
}
