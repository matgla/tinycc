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

/* SSA register allocator: live intervals, phi register hints and
 * outgoing-parameter affinity.  The rest of the allocator is in
 * regalloc_{scan,phi,entry,post}.c; see regalloc_priv.h. */

#include "regalloc_priv.h"

/* ra_alive_share deliberately leaves a SECOND live holder on a register the
 * allocator still shows as owned by the first, so every site that frees a
 * register or hands it on has to ask whether anyone else is still living
 * there.  The expire path asks it with survivor_int; eviction learned to ask
 * it in 030e6fa7.  True when some active interval OTHER than `skip` holds hr. */
int ra_reg_has_other_holder(SSAInterval **active, int active_count,
                                   const SSAInterval *skip, int hr)
{
  if (hr < 0)
    return 0;
  for (int k = 0; k < active_count; k++)
  {
    const SSAInterval *a = active[k];
    if (a == skip)
      continue;
    if (a->r0 == hr || a->r1 == hr)
      return 1;
  }
  return 0;
}

/* ============================================================================
 * Call-site prefix sum (reused from ir/live.c pattern)
 * ============================================================================ */

int ir_op_is_implicit_call_ra(TccIrOp op)
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
int *ra_build_call_prefix(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  if (n <= 0)
    return NULL;
  int *prefix = tcc_malloc(sizeof(int) * (n + 1));
  prefix[0] = 0;
  for (int i = 0; i < n; i++) {
    IRQuadCompact *q = &ir->compact_instructions[i];
    TccIrOp op = q->op;
    int is_call = ((op == TCCIR_OP_FUNCCALLVOID || op == TCCIR_OP_FUNCCALLVAL) && !tcc_ir_call_clobbers_nothing(ir, q)) ||
                  op == TCCIR_OP_BUILTIN_APPLY || ir_op_is_implicit_call_ra(op);
    /* A large BLOCK_COPY lowers to a memcpy() call in the backend, clobbering
     * the caller-saved registers.  The (small) lowering saves/restores
     * everything it touches, so only the memcpy-sized copies count as calls. */
    if (!is_call && op == TCCIR_OP_BLOCK_COPY) {
      int bc_size = (int)tcc_ir_op_src2_imm(ir, q);
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
    int is_call = ((op == TCCIR_OP_FUNCCALLVOID || op == TCCIR_OP_FUNCCALLVAL) && !tcc_ir_call_clobbers_nothing(ir, q)) ||
                  op == TCCIR_OP_BUILTIN_APPLY;
    if (!is_call && op == TCCIR_OP_BLOCK_COPY) {
      int bc_size = (int)tcc_ir_op_src2_imm(ir, q);
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
  /* No dispatch, no table: a NULL prefix answers "none in range" (see
   * ra_has_switch_in_range), and saves n+1 ints in the common case. */
  int any = 0;
  for (int i = 0; i < n && !any; i++)
    any = ir->compact_instructions[i].op == TCCIR_OP_SWITCH_TABLE ||
          ir->compact_instructions[i].op == TCCIR_OP_SWITCH_LOAD;
  if (!any)
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

/* Prefix sum of INLINE_ASM statements.  The allocator does not see an asm
 * statement's register effects: its clobber list, and the registers the
 * constraint solver picks for operands (r0-r8, or the one a `register ...
 * __asm("rN")` variable names), are only known when the backend emits it.
 * asm_gen_code preserves callee-saved r4-r11 around the statement but not
 * r0-r3/r12, so a value live across an asm must not sit in a caller-saved
 * register -- the same constraint a call imposes. */
static int *ra_build_asm_prefix(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  /* As ra_build_switch_prefix: NULL when the function has no asm. */
  int any = 0;
  for (int i = 0; i < n && !any; i++)
    any = ir->compact_instructions[i].op == TCCIR_OP_INLINE_ASM;
  if (!any)
    return NULL;
  int *prefix = tcc_malloc(sizeof(int) * (n + 1));
  prefix[0] = 0;
  for (int i = 0; i < n; i++)
    prefix[i + 1] = prefix[i] + (ir->compact_instructions[i].op == TCCIR_OP_INLINE_ASM);
  return prefix;
}
const char *ra_vreg_type_char(int type)
{
  switch (type) {
  case TCCIR_VREG_TYPE_VAR: return "V";
  case TCCIR_VREG_TYPE_TEMP: return "T";
  case TCCIR_VREG_TYPE_PARAM: return "P";
  default: return "?";
  }
}
void ra_build_intervals(TCCIRState *ir, IRCFG *cfg, IRSSAState *ssa,
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
  int *asm_prefix = ra_build_asm_prefix(ir);
  int *real_call_prefix = ra_build_real_call_prefix(ir);

  /* Allocate per-vreg start/end tracking indexed by encoded vreg, in the
   * dense VAR/TEMP/PARAM layout of RaVregIdx. */
  RaVregIdx vx;
  ra_vidx_init(&vx, local_count, temp_count, param_count);
  int table_size = vx.size;
  uint32_t *starts = tcc_malloc(sizeof(uint32_t) * table_size);
  uint32_t *ends = tcc_malloc(sizeof(uint32_t) * table_size);
  uint16_t *uses = tcc_mallocz(sizeof(uint16_t) * table_size);
  for (int i = 0; i < table_size; i++) {
    starts[i] = INTERVAL_NOT_STARTED;
    ends[i] = 0;
  }

  #define VREG_IDX(vr) ra_vidx(&vx, (vr))

  /* Conditional loop blocks get one quarter of the latch-path weight; see docs/ra_branch_cost.md. */
  uint8_t *instr_depth = tcc_mallocz(n);
  if (TCC_OPT(tcc_state, optimize) > 0) {
    IRLoops *loops = tcc_ir_detect_loops(ir);
    if (loops) {
      IRCFG *rebuilt = NULL;
      IRCFG *cost_cfg = cfg;
      int branch_cost = !tcc_ir_opt_pass_disabled("ra:branch_cost");
      if (loops->num_loops && branch_cost && cfg->num_instrs != n) {
        rebuilt = tcc_ir_cfg_build(ir);
        tcc_ir_cfg_compute_dominators(rebuilt);
        cost_cfg = rebuilt;
      }
      for (int li = 0; li < loops->num_loops; li++) {
        IRLoop *lp = &loops->loops[li];
        int latch = -1, discount = 0;
        if (branch_cost && cost_cfg->dom_tin) {
          int header = cost_cfg->instr_to_block[lp->header_idx];
          latch = cost_cfg->instr_to_block[lp->end_idx];
          discount = tcc_ir_cfg_dominates(cost_cfg, header, latch);
        }
        for (int bi = 0; bi < lp->num_body_instrs; bi++) {
          int idx = lp->body_instrs[bi];
          if (idx >= 0 && idx < n) {
            int depth = lp->depth;
            if (discount && !tcc_ir_cfg_dominates(cost_cfg, cost_cfg->instr_to_block[idx], latch))
              depth--;
            if (depth > instr_depth[idx])
              instr_depth[idx] = (uint8_t)depth;
          }
        }
      }
      tcc_ir_cfg_free(rebuilt);
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
    if (tcc_ir_op_is_mac(q->op))       srcs[nsrcs++] = tcc_ir_op_get_accum(ir, q);
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

    /* Saturate the same branch-aware 4^depth weight for uses and call saves. */
    uint16_t w = 1;
    if (instr_depth[i] > 0) {
      w = 1 << (2 * (instr_depth[i] < 7 ? instr_depth[i] : 7));
    }

    /* Uses: src1, src2 */
    if (irop_config[q->op].has_src1) {
      int32_t vr = tcc_ir_op_src1_vreg(ir, q);
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
      int32_t vr = tcc_ir_op_src2_vreg(ir, q);
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
    if (tcc_ir_op_is_mac(q->op)) {
      int32_t vr = tcc_ir_op_accum_vreg(ir, q);
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
       * detonates pressure.  A narrow write is a whole def too: into a
       * register it is a MOV of the source register, and into the temp's
       * spill slot a word store of the value extended (9be4d176), so no byte
       * of the old value survives either way.  Treating it as a partial def
       * made every byte temp an inlined Zig helper leaves behind live from
       * entry, pinning a callee-saved register across the whole function --
       * six of them starved std.HashMap's probe loop into stack slots. */
      else if (q->op == TCCIR_OP_STORE && vr >= 0 && tcc_ir_vreg_is_valid(ir, vr) &&
               TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_TEMP &&
               !d.is_lval && !d.is_local &&
               (d.btype == IROP_BTYPE_INT8 || d.btype == IROP_BTYPE_INT16 ||
                d.btype == IROP_BTYPE_INT32 || d.btype == IROP_BTYPE_FLOAT32 ||
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
      int idx = vx.base[type] + pos;
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
      int type = TCCIR_DECODE_VREG_TYPE(ra_vidx_vreg(&vx, idx));
      int pos = TCCIR_DECODE_VREG_POSITION(ra_vidx_vreg(&vx, idx));
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
              int target = (int)tcc_ir_op_dest_u_imm32(ir, term);
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
      int type = TCCIR_DECODE_VREG_TYPE(ra_vidx_vreg(&vx, idx));
      int pos = TCCIR_DECODE_VREG_POSITION(ra_vidx_vreg(&vx, idx));
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
      int ccid = TCCIR_DECODE_CALL_ID(tcc_ir_op_src2_imm(ir, q));
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
        int ccid = TCCIR_DECODE_CALL_ID(tcc_ir_op_src2_imm(ir, q));
        if (ccid >= 0 && ccid <= max_cid) next_call_by_cid[ccid] = i;
      } else if (q->op == TCCIR_OP_FUNCPARAMVAL) {
        int cid = TCCIR_DECODE_CALL_ID(tcc_ir_op_src2_imm(ir, q));
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

    /* Pass 2: escapes.  The address passed to a call (which may return it,
     * as a `mem.asBytes(&x)` wrapper does, or keep it) or stored to memory
     * comes back through a value the flow above cannot follow, so the root
     * stays live to the function end.  Without this, `t = as_bytes(&b);
     * ...; memcpy(dst, t, 1)` gave b's slot to a later address-taken VAR
     * while t still pointed into it (Zig's autoHash of a u8 hashed the low
     * byte of a pointer -- `std.lang is corrupt` from the -O0 compiler). */
    for (int i = 0; i < n; i++) {
      IRQuadCompact *q = &ir->compact_instructions[i];
      IROperand v;
      if (q->op == TCCIR_OP_FUNCPARAMVAL || q->op == TCCIR_OP_STORE || q->op == TCCIR_OP_RETURNVALUE)
        v = tcc_ir_op_get_src1(ir, q);
      else
        continue;
      if (v.is_lval || v.is_llocal)
        continue; /* the value read through it, not the address */
      int32_t v_vr = irop_get_vreg(v);
      if (v_vr < 0 || !tcc_ir_vreg_is_valid(ir, v_vr))
        continue;
      int vidx = VREG_IDX(v_vr);
      if (vidx >= table_size)
        continue;
      int root = -1;
      if (irop_get_tag(v) == IROP_TAG_STACKOFF && v.is_local) {
        IRLiveInterval *li = tcc_ir_vreg_live_interval(ir, v_vr);
        if (li && li->addrtaken)
          root = vidx;
      } else if (taint_root[vidx] >= 0)
        root = taint_root[vidx];
      if (root >= 0 && ends[root] < (uint32_t)(n - 1))
        ends[root] = (uint32_t)(n - 1);
    }

    /* Pass 3: a local captured by a nested function of this one is read and
     * written through the static chain by whatever call runs a child, from
     * any point of the body, and no instruction here names that access.  Its
     * slot is the whole function's: with only its own references, -O0 gave
     * `int p = 4, q = 5, r = 6;` (never read by the parent) ONE slot. */
    for (int ni = 0; ni < tcc_state->nb_nested_funcs; ni++) {
      const NestedFunc *nf = tcc_state->nested_funcs[ni];
      for (int j = 0; j < nf->nb_captured; j++) {
        int32_t v_vr = nf->captured_vregs[j];
        if (v_vr < 0 || TCCIR_DECODE_VREG_TYPE(v_vr) != TCCIR_VREG_TYPE_VAR ||
            !tcc_ir_vreg_is_valid(ir, v_vr) ||
            !nested_capture_owned_by(nf, j, tcc_state->current_nested_func))
          continue;
        int vidx = VREG_IDX(v_vr);
        if (vidx >= table_size || starts[vidx] == INTERVAL_NOT_STARTED)
          continue;
        starts[vidx] = 0;
        if (ends[vidx] < (uint32_t)(n - 1))
          ends[vidx] = (uint32_t)(n - 1);
      }
    }

    tcc_free(taint_root);
  }

  if (TCC_LOG_LS) {
    RA_DBG("SSA ra_build_intervals: after addrtaken pointer-flow extension");
    for (int idx = 0; idx < table_size; idx++) {
      if (starts[idx] == INTERVAL_NOT_STARTED) continue;
      int type = TCCIR_DECODE_VREG_TYPE(ra_vidx_vreg(&vx, idx));
      int pos = TCCIR_DECODE_VREG_POSITION(ra_vidx_vreg(&vx, idx));
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
        int32_t dv = tcc_ir_op_dest_vreg(ir, q);
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
        int32_t needle_ = ra_vidx_vreg(&vx, (vidx));                           \
        for (int s_ = (lo); s_ < (hi); s_++) {                                 \
          IRQuadCompact *qa_ = &ir->compact_instructions[s_];                  \
          if (qa_->op != TCCIR_OP_ASSIGN) continue;                            \
          int32_t adv_ = tcc_ir_op_dest_vreg(ir, qa_);           \
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
        int32_t vr_ = ra_vidx_vreg(&vx, idx_);                                  \
        int type_ = TCCIR_DECODE_VREG_TYPE(vr_);                                \
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
                   ra_vreg_type_char(type_), TCCIR_DECODE_VREG_POSITION(vr_), old_s_, \
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
            int32_t nv_ = vr_;                                                  \
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
                   TCCIR_DECODE_VREG_POSITION(vr_), starts[idx_], old_e_, starts[idx_], \
                   ends[idx_]);                                                  \
          }                                                                     \
        }                                                                       \
      }                                                                         \
    } while (0)

  for (int i = 0; i < n; i++) {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_JUMPIF) {
      int target = (int)tcc_ir_op_dest_u_imm32(ir, q);
      if (target >= 0 && target < n && target < i)
        RA_EXTEND_BACKEDGE(i, target, 0);
    } else if (q->op == TCCIR_OP_IJUMP) {
      if (i > 0)
        RA_EXTEND_BACKEDGE(i, 0, 1);
    } else if (q->op == TCCIR_OP_SWITCH_TABLE) {
      int table_id = (int)tcc_ir_op_src2_imm(ir, q);
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

  /* The linear rules above cannot see a value that leaves its range by a
   * forward jump and returns by a backward one; real liveness can. */
  uint8_t *live_in_at_start = tcc_mallocz(table_size > 0 ? table_size : 1);
  ra_widen_intervals_by_liveness(ir, &vx, starts, ends, live_in_at_start);

  if (TCC_LOG_LS) {
    RA_DBG("SSA ra_build_intervals: after backward jump extension");
    for (int idx = 0; idx < table_size; idx++) {
      if (starts[idx] == INTERVAL_NOT_STARTED) continue;
      int type = TCCIR_DECODE_VREG_TYPE(ra_vidx_vreg(&vx, idx));
      int pos = TCCIR_DECODE_VREG_POSITION(ra_vidx_vreg(&vx, idx));
      RA_DBG("  %s%d range=[%u,%u]", ra_vreg_type_char(type), pos,
             starts[idx], ends[idx]);
    }
  }

  /* Caller-save candidates: plain calls cost a loop-weighted store+reload each;
   * anything else that clobbers caller-saved registers outside the call
   * lowering's save/restore (switch dispatch, asm, native FP helpers, memcpy
   * block copies, setjmp) rules the interval out. */
  uint32_t *cs_cost_prefix = tcc_malloc(sizeof(uint32_t) * (n + 1));
  int *cs_bad_prefix = tcc_malloc(sizeof(int) * (n + 1));
  int cs_func_ok = TCC_OPT(tcc_state, optimize) >= 1 && !ir->has_static_chain &&
                   !tcc_ir_opt_pass_disabled("ra:caller_save");
  cs_cost_prefix[0] = 0;
  cs_bad_prefix[0] = 0;
  for (int i = 0; i < n; i++) {
    IRQuadCompact *q = &ir->compact_instructions[i];
    TccIrOp op = q->op;
    uint32_t c = 0;
    int bad = 0;
    if ((op == TCCIR_OP_FUNCCALLVAL || op == TCCIR_OP_FUNCCALLVOID) && !tcc_ir_call_clobbers_nothing(ir, q)) {
      /* Match the branch-aware use weight. */
      int d = instr_depth[i] < 6 ? instr_depth[i] : 6;
      c = 2u << (2 * d);
    } else if (op == TCCIR_OP_BUILTIN_APPLY || ir_op_is_implicit_call_ra(op) ||
               op == TCCIR_OP_SWITCH_TABLE || op == TCCIR_OP_SWITCH_LOAD ||
               op == TCCIR_OP_INLINE_ASM || op == TCCIR_OP_ASM_INPUT || op == TCCIR_OP_ASM_OUTPUT ||
               op == TCCIR_OP_VLA_ALLOC || op == TCCIR_OP_VLA_SP_SAVE || op == TCCIR_OP_VLA_SP_RESTORE) {
      bad = 1;
    } else if (op == TCCIR_OP_SETJMP || op == TCCIR_OP_NL_SETJMP || op == TCCIR_OP_LONGJMP ||
               op == TCCIR_OP_NL_LONGJMP) {
      cs_func_ok = 0;
    } else if (op == TCCIR_OP_BLOCK_COPY && (int)tcc_ir_op_src2_imm(ir, q) >= TCCIR_BLOCK_COPY_MEMCPY_MIN_BYTES) {
      bad = 1;
    }
    cs_cost_prefix[i + 1] = cs_cost_prefix[i] + c;
    cs_bad_prefix[i + 1] = cs_bad_prefix[i] + bad;
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
      int idx = vx.base[type] + pos;
      if (starts[idx] == INTERVAL_NOT_STARTED) continue;
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
      /* A parameter is defined before instruction 0, so a call AT 0 is inside
       * its range too: a leaf whose body opens with a memcpy-sized BLOCK_COPY
       * (a local's rodata initialiser) had its first parameter precolored in
       * r0 across the copy. */
      const int xstart = type == TCCIR_VREG_TYPE_PARAM ? -1
                         : (int)iv->start - (live_in_at_start[idx] ? 1 : 0);
      iv->crosses_real_call = ra_has_call_in_range(real_call_prefix, xstart, iv->end, n) ? 1 : 0;
      if (!iv->crosses_call) {
        iv->crosses_call = ra_has_call_in_range(call_prefix, xstart, iv->end, n);
        if (!iv->crosses_call && iv->end < (uint32_t)n) {
          TccIrOp eop = ir->compact_instructions[iv->end].op;
          if ((eop == TCCIR_OP_FUNCCALLVAL || eop == TCCIR_OP_FUNCCALLVOID) &&
              !tcc_ir_call_clobbers_nothing(ir, &ir->compact_instructions[iv->end])) {
            int idx = VREG_IDX(vreg);
            int is_param_use = (param_extended &&
                                ((param_extended[idx >> 3] >> (idx & 7)) & 1));
            if (!is_param_use)
              iv->crosses_call = 1;
          }
        }
      }

      /* Caller-save eligibility over (xstart, end): see cs_cost_prefix. */
      iv->cs_ok = 0;
      iv->caller_save = 0;
      iv->cs_cost = 0;
      if (cs_func_ok && iv->crosses_call && iv->reg_type == LS_REG_TYPE_INT && !iv->addrtaken &&
          !iv->is_volatile && iv->precolored < 0 && iv->end < (uint32_t)n) {
        const int lo = xstart + 1 < 0 ? 0 : xstart + 1;
        const int hi = (int)iv->end; /* inclusive for clobbers */
        int ok = (cs_bad_prefix[hi + 1] - cs_bad_prefix[lo]) == 0;
        TccIrOp eop = ir->compact_instructions[iv->end].op;
        if (ok && (eop == TCCIR_OP_FUNCCALLVAL || eop == TCCIR_OP_FUNCCALLVOID)) {
          /* The value dies at this call: fine as an argument, but not as an
           * indirect target (argument setup may clobber it first). */
          int idx2 = VREG_IDX(vreg);
          ok = param_extended && ((param_extended[idx2 >> 3] >> (idx2 & 7)) & 1);
        }
        if (ok) {
          uint32_t cost = cs_cost_prefix[iv->end] - cs_cost_prefix[lo];
          if (cost > 0) {
            iv->cs_ok = 1;
            iv->cs_cost = cost > 65535 ? 65535 : (uint16_t)cost;
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
        iv->crosses_call = ra_has_switch_in_range(switch_prefix, xstart, iv->end, n);

      /* Asm crossing: see ra_build_asm_prefix.  Same inclusive-end range test
       * as the switch case, since a back-edge extension can end an interval
       * exactly at the asm statement it is live across. */
      if (!iv->crosses_call)
        iv->crosses_call = ra_has_switch_in_range(asm_prefix, xstart, iv->end, n);

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

  tcc_free(live_in_at_start);
  tcc_free(starts);
  tcc_free(ends);
  tcc_free(uses);
  tcc_free(instr_depth);
  tcc_free(param_extended);
  tcc_free(cs_cost_prefix);
  tcc_free(cs_bad_prefix);
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
  if (asm_prefix) tcc_free(asm_prefix);
  if (real_call_prefix) tcc_free(real_call_prefix);
  #undef VREG_IDX
}

/* ============================================================================
 * Phi Register Hints
 * ============================================================================ */

void ra_build_phi_hints(SSAInterval *intervals, int count,
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
void ra_build_assign_hints(SSAInterval *intervals, int count,
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
void ra_build_load_param_hints(SSAInterval *intervals, int count,
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
    IROperand s = tcc_ir_op_get_src1(ir, q);
    if (s.is_lval) continue;                /* real memory load, not a copy */
    int32_t dest_vr = tcc_ir_op_dest_vreg(ir, q);
    int32_t src_vr = irop_get_vreg(s);
    if (dest_vr < 0 || src_vr < 0) continue;
    /* Source must be a PARAM (the only LOAD-as-copy pattern we trust). */
    if (TCCIR_DECODE_VREG_TYPE(src_vr) != TCCIR_VREG_TYPE_PARAM) continue;
    if (TCCIR_DECODE_VREG_TYPE(dest_vr) == TCCIR_VREG_TYPE_PARAM) continue;
    /* Width gate: only full-word INT32.  Sub-word carries AAPCS narrowing,
     * INT64 needs a pair, FP types use a different reg class. */
    int sbtype = irop_get_btype(s);
    if (sbtype != IROP_BTYPE_INT32) continue;
    int dbtype = tcc_ir_op_dest_btype(ir, q);
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
void ra_build_bfi_hints(SSAInterval *intervals, int count,
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
    int32_t dest_vr = tcc_ir_op_dest_vreg(ir, q);
    int32_t src_vr = tcc_ir_op_src1_vreg(ir, q);
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
void ra_build_outgoing_param_hints(SSAInterval *intervals, int count,
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
