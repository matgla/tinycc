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

/* SSA register allocator: phi resolution and graph-based register
 * coalescing. */

#include "regalloc_priv.h"

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
int ra_phi_stats_has_participant(RAPhiStats *stats, int32_t vreg)
{
  if (!TCC_LOG_LS || !stats)
    return 0;
  for (int i = 0; i < stats->participant_count; i++)
    if (stats->participants[i] == vreg)
      return 1;
  return 0;
}

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
int ra_phi_resolve_pre_ra_mode = 0;

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
void ra_eliminate_dead_reg_copies(TCCIRState *ir)
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
      if (tcc_ir_op_is_mac(q->op)) {
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
int ra_phi_copy_is_identity(IRLiveInterval *dest_li, IRLiveInterval *src_li)
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
    if (tcc_ir_op_is_mac(q->op))
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
void ra_resolve_phis(TCCIRState *ir, IRCFG *cfg, IRSSAState *ssa,
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
void ra_build_live_regs_bitmap(TCCIRState *ir)
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
TCC_DBG_ENV_FLAG(ra_no_coalesce_gapless, "TCC_NO_COALESCE_GAPLESS")

static int ra_coalesce_level(void)
{
  return ra_no_coalesce() ? 0 : ra_coalesce_env_level();
}

/* Collect an instruction's def and use vreg operands into out-params.
 * STORE-class dest is a USE (it is the address); MLA accumulator is a USE.
 * Matches ra_build_intervals' operand semantics exactly. */
void ra_co_ops(TCCIRState *ir, IRQuadCompact *q,
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
  if (tcc_ir_op_is_mac(q->op)) {
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
void ra_build_narrow_weights(TCCIRState *ir, const RegAllocTarget *target,
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
void ra_refine_live_regs_accurate(TCCIRState *ir)
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
  /* Only register-resident slots can contribute to the final mask, and only a
   * slot read before any def in some block (upward-exposed) can ever be live-in
   * at a block; liveness is per-vreg independent, so restricting the dataflow
   * universe to slots that are both is exact.  dense[] first holds each slot's
   * last defining block while the blocks are scanned for upward-exposed uses. */
  int *dense = tcc_malloc(sizeof(int) * tbl);
  uint8_t *upward = tcc_mallocz(tbl);
  for (int i = 0; i < tbl; i++) dense[i] = -1;
  for (int b = 0; b < nb; b++) {
    int s = cfg->blocks[b].start_idx, e = cfg->blocks[b].end_idx;
    for (int i = s; i < e && i < n; i++) {
      int32_t def=-1, hd=0, uses[4], nu=0;
      ra_co_ops(ir, &ir->compact_instructions[i], &def, &hd, uses, &nu);
      for (int k=0;k<nu;k++){ if(!tcc_ir_vreg_is_valid(ir,uses[k]))continue; int u=DVIDX(uses[k]); if(u>=0&&u<tbl&&dense[u]!=b) upward[u]=1;}
      if (hd && tcc_ir_vreg_is_valid(ir,def)){int d=DVIDX(def); if(d>=0&&d<tbl) dense[d]=b;}
    }
  }
  int ndense = 0;
  for (int i = 0; i < tbl; i++) {
    int tracked = (vr0[i] >= 0 && vr0[i] < 16) || (vr1[i] >= 0 && vr1[i] < 16);
    dense[i] = tracked && upward[i] ? ndense++ : -1;
  }
  tcc_free(upward);
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
  /* Only live_in is read from here on. */
  RaLiveMatrix_cleanup(&useb);
  RaLiveMatrix_cleanup(&defbk);
  RaLiveMatrix_cleanup(&liveout);
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

/* 1 when the member intervals of the class rooted at `root` cover their
 * union [min start, max end] without a gap. */
static int ra_co_class_gapless(const SSAInterval *intervals, const int *iv_of, const int *cand_vidx,
                               const int *mnext, int root)
{
  int nm = 0;
  for (int m = root; m >= 0; m = mnext[m])
    nm++;
  uint32_t *st = tcc_malloc(sizeof(uint32_t) * 2 * nm);
  int k = 0;
  for (int m = root; m >= 0; m = mnext[m], k++)
  {
    const SSAInterval *iv = &intervals[iv_of[cand_vidx[m]]];
    st[2 * k] = iv->start;
    st[2 * k + 1] = iv->end;
  }
  /* insertion sort by start: classes are small */
  for (int a = 1; a < nm; a++)
    for (int b = a; b > 0 && st[2 * b] < st[2 * (b - 1)]; b--)
    {
      uint32_t t0 = st[2 * b], t1 = st[2 * b + 1];
      st[2 * b] = st[2 * (b - 1)];
      st[2 * b + 1] = st[2 * (b - 1) + 1];
      st[2 * (b - 1)] = t0;
      st[2 * (b - 1) + 1] = t1;
    }
  int ok = 1;
  uint32_t reach = st[1];
  for (int a = 1; a < nm && ok; a++)
  {
    if (st[2 * a] > reach + 1)
      ok = 0;
    if (st[2 * a + 1] > reach)
      reach = st[2 * a + 1];
  }
  tcc_free(st);
  return ok;
}
void ra_coalesce_graph(TCCIRState *ir, SSAInterval *intervals, int count,
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

  /* ---- Stage 2: backward liveness dataflow (live_in/live_out per block). ----
   * Only a vreg with an upward-exposed use in some block (read there before
   * any def in that block) can be live across a block boundary: live_in(b) =
   * use(b) ∪ (live_out(b) − def(b)) and live_out is the union of successor
   * live_ins, so a vreg in no use(b) is in no live_in and no live_out.  The
   * dataflow runs over those "global" vregs only (dense index g, glob_vidx:
   * g -> VIDX), which is exact and keeps the four nb-row matrices a fraction
   * of nb x tbl bits -- over the whole VIDX space they were 4 x 7.8 MB for one
   * function of tinycc's own ir/codegen.c, the peak of that compile. */
  int *glob_of = tcc_malloc(sizeof(int) * tbl);
  int *def_stamp = tcc_malloc(sizeof(int) * tbl);
  for (int i = 0; i < tbl; i++) { glob_of[i] = -1; def_stamp[i] = -1; }
  int ng = 0;
  #define RA_CO_NOTE_USE(u)                                                   \
    do {                                                                      \
      if ((u) >= 0 && (u) < tbl && def_stamp[(u)] != b && glob_of[(u)] < 0)  \
        glob_of[(u)] = ng++;                                                  \
    } while (0)
  for (int b = 0; b < nb; b++) {
    int s = cfg->blocks[b].start_idx, e = cfg->blocks[b].end_idx;
    for (int i = s; i < e && i < n; i++) {
      int32_t def = -1, hd = 0, uses[4], nu = 0;
      ra_co_ops(ir, &ir->compact_instructions[i], &def, &hd, uses, &nu);
      for (int k = 0; k < nu; k++)
        if (tcc_ir_vreg_is_valid(ir, uses[k])) RA_CO_NOTE_USE(VIDX(uses[k]));
      RA_CO_FOR_CALL_PARAM_USES(i, vr, { RA_CO_NOTE_USE(VIDX(vr)); });
      if (hd && tcc_ir_vreg_is_valid(ir, def)) {
        int d = VIDX(def);
        if (d >= 0 && d < tbl) def_stamp[d] = b;
      }
    }
  }
  #undef RA_CO_NOTE_USE
  tcc_free(def_stamp);
  int *glob_vidx = tcc_malloc(sizeof(int) * (ng + 1));
  for (int i = 0; i < tbl; i++)
    if (glob_of[i] >= 0) glob_vidx[glob_of[i]] = i;
  int ngw = (ng + 63) / 64;

  /* use(b) / def(b) are a handful of globals per block, so they are kept as
   * per-block lists (CSR: blk_off[b] .. blk_off[b+1] into use_g / def_g), and
   * live_in(s) = use(s) ∪ (live_out(s) − def(s)) is rebuilt from them when a
   * predecessor reads it: live_out is the only nb-row matrix. */
  int *use_off = tcc_malloc(sizeof(int) * (nb + 1));
  int *def_off = tcc_malloc(sizeof(int) * (nb + 1));
  int use_cap = 64, def_cap = 64, nuse = 0, ndef = 0;
  int *use_g = tcc_malloc(sizeof(int) * use_cap);
  int *def_g = tcc_malloc(sizeof(int) * def_cap);
  int *g_def_blk = tcc_malloc(sizeof(int) * (ng + 1));
  int *g_use_blk = tcc_malloc(sizeof(int) * (ng + 1));
  for (int g = 0; g < ng; g++) { g_def_blk[g] = -1; g_use_blk[g] = -1; }
  /* upward-exposed use of global g in block b (read before any def in b) */
  #define RA_CO_ADD_USE(g_)                                                     \
    do {                                                                        \
      int gg_ = (g_);                                                           \
      if (gg_ >= 0 && g_def_blk[gg_] != b && g_use_blk[gg_] != b) {             \
        g_use_blk[gg_] = b;                                                     \
        if (nuse == use_cap) use_g = tcc_realloc(use_g, sizeof(int) * (use_cap *= 2)); \
        use_g[nuse++] = gg_;                                                    \
      }                                                                         \
    } while (0)
  for (int b = 0; b < nb; b++) {
    use_off[b] = nuse;
    def_off[b] = ndef;
    int s = cfg->blocks[b].start_idx, e = cfg->blocks[b].end_idx;
    for (int i = s; i < e && i < n; i++) {
      int32_t def = -1, hd = 0, uses[4], nu = 0;
      ra_co_ops(ir, &ir->compact_instructions[i], &def, &hd, uses, &nu);
      for (int k = 0; k < nu; k++) {
        if (!tcc_ir_vreg_is_valid(ir, uses[k])) continue;
        int u = VIDX(uses[k]);
        if (u >= 0 && u < tbl) RA_CO_ADD_USE(glob_of[u]);
      }
      RA_CO_FOR_CALL_PARAM_USES(i, vr, {
        int u = VIDX(vr);
        if (u >= 0 && u < tbl) RA_CO_ADD_USE(glob_of[u]);
      });
      if (hd && tcc_ir_vreg_is_valid(ir, def)) {
        int d = VIDX(def);
        int g = (d >= 0 && d < tbl) ? glob_of[d] : -1;
        if (g >= 0 && g_def_blk[g] != b) {
          g_def_blk[g] = b;
          if (ndef == def_cap) def_g = tcc_realloc(def_g, sizeof(int) * (def_cap *= 2));
          def_g[ndef++] = g;
        }
      }
    }
  }
  use_off[nb] = nuse;
  def_off[nb] = ndef;
  #undef RA_CO_ADD_USE
  tcc_free(g_def_blk);
  tcc_free(g_use_blk);
  tcc_free(glob_of);

  bit_matrix(RaLiveMatrix) liveout = {0};
  RaLiveMatrix_init(&liveout, nb, ng);
  uint64_t *in_row = tcc_malloc(sizeof(uint64_t) * (ngw + 1));
  uint64_t *out_row = tcc_malloc(sizeof(uint64_t) * (ngw + 1));

  /* Fixpoint over reverse-RPO order. */
  int changed = 1, guard = 0;
  while (changed && guard++ < nb + 4) {
    changed = 0;
    for (int ri = cfg->rpo_count - 1; ri >= 0; ri--) {
      int b = cfg->rpo_order ? cfg->rpo_order[ri] : ri;
      if (b < 0 || b >= nb) continue;
      /* live_out = union of successors' live_in */
      tcc_bitspan_zero(out_row, ngw);
      for (int si = 0; si < cfg->blocks[b].num_succs; si++) {
        int sb = cfg->blocks[b].succs[si];
        if (sb < 0 || sb >= nb) continue;
        /* live_in(sb) = use ∪ (live_out − def) */
        tcc_bitspan_copy(in_row, RaLiveMatrix_row(&liveout, sb), ngw);
        for (int k = def_off[sb]; k < def_off[sb + 1]; k++) tcc_bitspan_reset(in_row, def_g[k]);
        for (int k = use_off[sb]; k < use_off[sb + 1]; k++) tcc_bitspan_set(in_row, use_g[k]);
        tcc_bitspan_or(out_row, in_row, ngw);
      }
      uint64_t *lo = RaLiveMatrix_row(&liveout, b);
      if (ngw && memcmp(lo, out_row, sizeof(uint64_t) * ngw)) {
        tcc_bitspan_copy(lo, out_row, ngw);
        changed = 1;
      }
    }
  }
  tcc_free(in_row); tcc_free(out_row);
  tcc_free(use_off); tcc_free(def_off); tcc_free(use_g); tcc_free(def_g);
  #define RA_CO_LIVE_OUT(live_, b_)                                            \
    do {                                                                       \
      tcc_bitspan_zero((live_), nw);                                           \
      tcc_bitspan_for_each_set(RaLiveMatrix_row(&liveout, (b_)), ngw, ng, g_)  \
        tcc_bitspan_set((live_), glob_vidx[g_]);                               \
    } while (0)

  /* ---- Register-pressure gate. ----
   * Coalescing can only ever ADD instructions (vs the baseline) by forcing a
   * spill: merging two non-interfering values reduces distinct values at every
   * point EXCEPT a liveness hole, where the merged interval occupies the
   * register and raises pressure by one.  If the function's peak INT pressure
   * leaves headroom (< K allocatable int regs), no spill can result, so
   * coalescing is a pure win (it only removes copies).  When pressure already
   * reaches K (spilling territory), coalescing's longer intervals can perturb
   * the linear scan into worse spills (observed: large high-pressure functions
   * regress).  In that case only a class whose members' ranges leave no gap
   * is applied (high_pressure, checked at Stage 5): it occupies the register
   * exactly where one of its members already did, so no point of the scan
   * sees one more live interval.  That still takes a loop's `i <- i - 1` and
   * `acc <- acc << 8 | b` latch copies, which lie inside the phi's range --
   * the whole-function bail used to drop them in every function with one
   * high-pressure point (Zig's Wyhash.hash: a u128 multiply). */
  int high_pressure = 0;
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
      int p = 0;
      RA_CO_LIVE_OUT(live, b);
      tcc_bitspan_for_each_set(live, nw, tbl, vi) {
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
      RA_DBG("coalesce: peak INT pressure %d >= K=%d, gapless classes only", maxp, K);
      if (ra_no_coalesce_gapless()) {
        tcc_free(iv_of); tcc_free(glob_vidx);
        tcc_free(call_param_head); tcc_free(param_sibling);
        tcc_ir_cfg_free(cfg);
        return;
      }
      high_pressure = 1;
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
    tcc_free(iv_of); tcc_free(glob_vidx); tcc_free(instr_block);
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
      RA_CO_LIVE_OUT(live, b);
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
          if (high_pressure && !ra_co_class_gapless(intervals, iv_of, cand_vidx, mnext, c))
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
  #undef RA_CO_LIVE_OUT
  tcc_free(deg); tcc_free(live);
  tcc_free(iv_of); tcc_free(glob_vidx);
  tcc_free(instr_block);
  tcc_free(cand_id); tcc_free(cand_vidx); tcc_free(edge_d); tcc_free(edge_s);
  tcc_free(call_param_head); tcc_free(param_sibling);
  #undef RA_CO_FOR_CALL_PARAM_USES
  tcc_ir_cfg_free(cfg);
}
