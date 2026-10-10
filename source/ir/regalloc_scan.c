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

/* SSA register allocator: linear-scan allocation, accurate per-vreg
 * liveness for the last-resort register share, and writing the results
 * back to the IR. */

#include "regalloc_priv.h"

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
   * vreg. */
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
    if (!tcc_ir_op_dest_has_vreg(ir, q) || tcc_ir_op_dest_vreg(ir, q) != cur_vreg) continue;

    int reads_partner = 0;
    if (irop_config[q->op].has_src1) {
      if (tcc_ir_op_src1_has_vreg(ir, q) && tcc_ir_op_src1_vreg(ir, q) == partner_vreg) reads_partner = 1;
    }
    if (!reads_partner && irop_config[q->op].has_src2) {
      if (tcc_ir_op_src2_has_vreg(ir, q) && tcc_ir_op_src2_vreg(ir, q) == partner_vreg) reads_partner = 1;
    }
    if (!reads_partner && tcc_ir_op_is_mac(q->op)) {
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
      if (tcc_ir_op_dest_has_vreg(ir, q) && tcc_ir_op_dest_vreg(ir, q) == cur_vreg)
        return 0;
    }

    int uses_partner_as_src = 0;
    if (irop_config[q->op].has_src1) {
      if (tcc_ir_op_src1_has_vreg(ir, q) && tcc_ir_op_src1_vreg(ir, q) == partner_vreg)
        uses_partner_as_src = 1;
    }
    if (!uses_partner_as_src && irop_config[q->op].has_src2) {
      if (tcc_ir_op_src2_has_vreg(ir, q) && tcc_ir_op_src2_vreg(ir, q) == partner_vreg)
        uses_partner_as_src = 1;
    }
    if (!uses_partner_as_src && tcc_ir_op_is_mac(q->op)) {
      IROperand s = tcc_ir_op_get_accum(ir, q);
      if (irop_has_vreg(s) && irop_get_vreg(s) == partner_vreg)
        uses_partner_as_src = 1;
    }
    if (uses_partner_as_src) {
      if (q->op != TCCIR_OP_ASSIGN) return 0;
      if (!tcc_ir_op_dest_has_vreg(ir, q) || tcc_ir_op_dest_vreg(ir, q) != partner_vreg) return 0;
      if (!tcc_ir_op_src1_has_vreg(ir, q) || tcc_ir_op_src1_vreg(ir, q) != cur_vreg) return 0;
      if (found_back_copy) return 0;
      found_back_copy = 1;
      continue;
    }

    if (irop_config[q->op].has_dest) {
      int dest_is_use = (q->op == TCCIR_OP_STORE || q->op == TCCIR_OP_STORE_INDEXED ||
                         q->op == TCCIR_OP_STORE_POSTINC);
      if (dest_is_use) {
        if (tcc_ir_op_dest_has_vreg(ir, q) && tcc_ir_op_dest_vreg(ir, q) == partner_vreg)
          return 0;
      } else {
        if (tcc_ir_op_dest_has_vreg(ir, q) && tcc_ir_op_dest_vreg(ir, q) == partner_vreg) {
          if (q->op != TCCIR_OP_ASSIGN) return 0;
          if (!tcc_ir_op_src1_has_vreg(ir, q) || tcc_ir_op_src1_vreg(ir, q) != cur_vreg) return 0;
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
    if (tcc_ir_op_src1_has_vreg(ir, q) && tcc_ir_op_src1_vreg(ir, q) == vreg) return 1;
  }
  if (irop_config[q->op].has_src2) {
    if (tcc_ir_op_src2_has_vreg(ir, q) && tcc_ir_op_src2_vreg(ir, q) == vreg) return 1;
  }
  if (tcc_ir_op_is_mac(q->op)) {
    IROperand s = tcc_ir_op_get_accum(ir, q);
    if (irop_has_vreg(s) && irop_get_vreg(s) == vreg) return 1;
  }
  if (irop_config[q->op].has_dest) {
    if (tcc_ir_op_dest_has_vreg(ir, q) && tcc_ir_op_dest_vreg(ir, q) == vreg) return 1;
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
    if (!tcc_ir_op_dest_has_vreg(ir, q) || tcc_ir_op_dest_vreg(ir, q) != cur_vreg) continue;
    if (q->op != TCCIR_OP_ASSIGN) return 0;
    if (!tcc_ir_op_src1_has_vreg(ir, q) || tcc_ir_op_src1_vreg(ir, q) != partner_vreg) return 0;
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
      RA_EXITPHI_PUSH((int)tcc_ir_op_dest_imm(ir, q));
      continue;
    }
    if (q->op == TCCIR_OP_JUMPIF) {
      RA_EXITPHI_PUSH((int)tcc_ir_op_dest_imm(ir, q));
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
      if (!tcc_ir_op_dest_has_vreg(ir, q) || tcc_ir_op_dest_vreg(ir, q) != cur_vreg) continue;
      if (visited[j]) continue; /* (b) forward continuation */
      /* (c) a parallel `cur <- partner` copy is fine; anything else rejects. */
      int parallel_copy = 0;
      if (q->op == TCCIR_OP_ASSIGN) {
        if (tcc_ir_op_src1_has_vreg(ir, q) && tcc_ir_op_src1_vreg(ir, q) == partner_vreg)
          parallel_copy = 1;
      }
      if (!parallel_copy) { dead = 0; break; }
    }
  }

  tcc_free(visited);
  tcc_free(stack);
  return dead;
}

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
void ra_alive_free(RaAliveInfo *info)
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
int ra_alive_worth_building(TCCIRState *ir, const SSAInterval *intervals, int count)
{
  if (TCC_OPT(tcc_state, optimize) < 1)
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
    int t = (int)tcc_ir_op_dest_imm(ir, q);
    if (t >= 0 && t <= i)
      return 0;
  }
  return 1;
}
/* For every CALL, the chain of FUNCPARAMVAL quads (by instruction index) whose
 * vreg source is read again AT that call: head[call] -> sibling[] -> ... -> -1.
 * Both arrays are n entries, caller frees. */
static void ra_param_use_lists(TCCIRState *ir, int n, int **out_head, int **out_sibling)
{
  int *call_param_head = tcc_malloc(sizeof(int) * n);
  int *param_sibling = tcc_malloc(sizeof(int) * n);
  for (int i = 0; i < n; i++) { call_param_head[i] = -1; param_sibling[i] = -1; }
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_FUNCPARAMVAL) continue;
    if (!tcc_ir_op_src1_has_vreg(ir, q) || tcc_ir_op_src1_is_imm(ir, q)) continue;
    int cid = TCCIR_DECODE_CALL_ID(tcc_ir_op_src2_imm(ir, q));
    if (cid < 0) continue;
    for (int j = i + 1; j < n; j++)
    {
      IRQuadCompact *qq = &ir->compact_instructions[j];
      if (qq->op != TCCIR_OP_FUNCCALLVOID && qq->op != TCCIR_OP_FUNCCALLVAL) continue;
      if (TCCIR_DECODE_CALL_ID(tcc_ir_op_src2_imm(ir, qq)) == cid)
      {
        param_sibling[i] = call_param_head[j];
        call_param_head[j] = i;
        break;
      }
    }
  }
  *out_head = call_param_head;
  *out_sibling = param_sibling;
}

void ra_alive_build(TCCIRState *ir, RaAliveInfo *info, int max_vreg_pos)
{
  memset(info, 0, sizeof *info);
  const int n = ir->next_instruction_index;
  if (n <= 0 || max_vreg_pos <= 0)
    return;
  if (TCC_OPT(tcc_state, optimize) < 1)
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
  int *call_param_head = NULL, *param_sibling = NULL;
  ra_param_use_lists(ir, n, &call_param_head, &param_sibling);
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

/* Widen the linear [start,end] intervals to cover every point where the value
 * is genuinely live.
 *
 * ra_build_intervals gives each vreg ONE contiguous range -- first to last
 * reference in linear order -- and widens it only at backward jumps whose
 * source or target the range already contains.  A value can leave that range
 * by a FORWARD jump to a block laid out after its last use and come back by a
 * BACKWARD jump to a block laid out before its def, falling or jumping into
 * the use from there.  Neither end of the excursion is inside the range, so
 * the blocks on it were treated as "value dead" and their temporaries got the
 * value's register (hand-written goto state machines, generated scanners and
 * the Zig C backend's label/goto output all have this layout).
 *
 * Real liveness (backward dataflow over the CFG, the model the coalescer and
 * ra_alive_build trust) says where the value is live-in/live-out of a block;
 * the range grows to the hull of those points.  Only "live AND possibly
 * defined" counts: a path that reaches a use without passing a def reads an
 * uninitialised value, and chasing it back to the function entry would only
 * inflate the range.  Growth only ever adds, so this can cost registers but
 * never remove a conflict the linear rules already saw.
 *
 * Only a function with a backward edge needs it: with forward edges alone
 * every path from a def to a use is monotone in linear order and stays inside
 * [first reference, last reference].
 *
 * `live_in_at_start[idx]` (zeroed by the caller) is set for a range that now
 * begins at a block's first instruction with the value live-in there: the
 * crossing tests in ra_build_intervals exclude the start, but a call, switch
 * dispatch or asm statement AT that instruction clobbers the value too.
 *
 * An IJUMP's successors cannot be enumerated: it is treated as reaching every
 * block (blocks entered only that way are not in the RPO, so such a function
 * is iterated over all blocks).  A function whose bit sets would be too large
 * to be worth their memory falls back to ra_widen_intervals_by_loops.  */
#define RA_WIDEN_MAX_WORDS (1 << 19) /* uint64 words over all five per-block matrices: 4 MiB */

/* Conservative stand-in for the liveness widening: a value can leave its linear
 * range forward and re-enter it backward only through a backward edge whose
 * loop encloses the range, so every enclosing loop is covered whole. */
static void ra_widen_intervals_by_loops(TCCIRState *ir, const RaVregIdx *vx, uint32_t *starts, uint32_t *ends,
                                        uint8_t *live_in_at_start)
{
  const int n = ir->next_instruction_index;
  const int vsize = vx->size;
  for (int changed = 1; changed;)
  {
    changed = 0;
    for (int i = 0; i < n; i++)
    {
      IRQuadCompact *q = &ir->compact_instructions[i];
      int lo = n, nt = 0, t[2];
      if (q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_JUMPIF)
        t[nt++] = (int)irop_get_imm64_ex(ir, tcc_ir_op_get_dest(ir, q));
      else if (q->op == TCCIR_OP_SWITCH_TABLE)
      {
        int table_id = (int)irop_get_imm64_ex(ir, tcc_ir_op_get_src2(ir, q));
        if (table_id >= 0 && table_id < ir->num_switch_tables)
        {
          const TCCIRSwitchTable *tb = &ir->switch_tables[table_id];
          for (int j = 0; j < tb->num_entries; j++)
            if (tb->targets[j] >= 0 && tb->targets[j] <= i && tb->targets[j] < lo)
              lo = tb->targets[j];
          if (tb->default_target >= 0 && tb->default_target <= i && tb->default_target < lo)
            lo = tb->default_target;
        }
      }
      for (int k = 0; k < nt; k++)
        if (t[k] >= 0 && t[k] <= i && t[k] < lo)
          lo = t[k];
      if (lo >= n)
        continue;
      for (int idx = 0; idx < vsize; idx++)
      {
        if (starts[idx] == INTERVAL_NOT_STARTED || ends[idx] <= starts[idx])
          continue;
        if ((int)starts[idx] > lo && (int)ends[idx] < i)
        {
          starts[idx] = (uint32_t)lo;
          ends[idx] = (uint32_t)i;
          live_in_at_start[idx] = 1;
          changed = 1;
        }
      }
    }
  }
}

void ra_widen_intervals_by_liveness(TCCIRState *ir, const RaVregIdx *vx, uint32_t *starts, uint32_t *ends,
                                    uint8_t *live_in_at_start)
{
  const int n = ir->next_instruction_index;
  if (n <= 1)
    return;

  int has_back = 0, has_ijump = 0;
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_IJUMP)
    {
      has_ijump = 1;
      has_back = 1;
    }
    else if (q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_JUMPIF)
    {
      int t = (int)irop_get_imm64_ex(ir, tcc_ir_op_get_dest(ir, q));
      if (t >= 0 && t <= i)
        has_back = 1;
    }
    else if (q->op == TCCIR_OP_SWITCH_TABLE)
    {
      int table_id = (int)irop_get_imm64_ex(ir, tcc_ir_op_get_src2(ir, q));
      if (table_id >= 0 && table_id < ir->num_switch_tables)
      {
        const TCCIRSwitchTable *tb = &ir->switch_tables[table_id];
        for (int j = 0; j < tb->num_entries; j++)
          if (tb->targets[j] >= 0 && tb->targets[j] <= i)
            has_back = 1;
        if (tb->default_target >= 0 && tb->default_target <= i)
          has_back = 1;
      }
    }
  }
  if (!has_back)
    return;

  /* Dense row per vreg that has a range worth checking. */
  const int vsize = vx->size;
  int *dense = tcc_malloc(sizeof(int) * (vsize + 1));
  int nd = 0;
  for (int idx = 0; idx < vsize; idx++)
  {
    dense[idx] = -1;
    if (starts[idx] != INTERVAL_NOT_STARTED && ends[idx] > starts[idx])
      dense[idx] = nd++;
  }
  dense[vsize] = -1;
  if (nd == 0)
  {
    tcc_free(dense);
    return;
  }

  IRCFG *cfg = tcc_ir_cfg_build(ir);
  if (!cfg)
  {
    tcc_free(dense);
    return;
  }
  tcc_ir_cfg_compute_rpo(cfg);
  const int nb = cfg->num_blocks;
  const int bw = (nd + 63) / 64;
  size_t max_words = RA_WIDEN_MAX_WORDS;
#ifdef CONFIG_TCC_DEBUG
  if (getenv("TCC_RA_WIDEN_MAX_WORDS"))
    max_words = (size_t)strtoul(getenv("TCC_RA_WIDEN_MAX_WORDS"), NULL, 10);
#endif
  if (nb <= 0 || (size_t)5 * (size_t)nb * (size_t)bw > max_words)
  {
    tcc_ir_cfg_free(cfg);
    tcc_free(dense);
    ra_widen_intervals_by_loops(ir, vx, starts, ends, live_in_at_start);
    return;
  }

  /* Visiting order: the RPO, or every block when an IJUMP can enter any of them. */
  int *order = tcc_malloc(sizeof(int) * nb);
  int norder = cfg->rpo_count;
  if (has_ijump)
  {
    norder = nb;
    for (int b = 0; b < nb; b++)
      order[b] = b;
  }
  else
    for (int b = 0; b < norder; b++)
      order[b] = cfg->rpo_order[b];

  /* A SWITCH_TABLE's default may be outside its table (the cfg only enumerates
   * the entries): one extra successor per such block. */
  int *xsucc = tcc_malloc(sizeof(int) * nb);
  uint8_t *is_ijump = tcc_mallocz(nb);
  for (int b = 0; b < nb; b++)
  {
    xsucc[b] = -1;
    int last = cfg->blocks[b].end_idx - 1;
    if (last < cfg->blocks[b].start_idx)
      continue;
    IRQuadCompact *q = &ir->compact_instructions[last];
    if (q->op == TCCIR_OP_IJUMP)
      is_ijump[b] = 1;
    if (q->op != TCCIR_OP_SWITCH_TABLE)
      continue;
    int table_id = (int)irop_get_imm64_ex(ir, tcc_ir_op_get_src2(ir, q));
    if (table_id >= 0 && table_id < ir->num_switch_tables)
    {
      int dt = ir->switch_tables[table_id].default_target;
      if (dt >= 0 && dt < n)
        xsucc[b] = cfg->instr_to_block[dt];
    }
  }

  int *call_param_head = NULL, *param_sibling = NULL;
  ra_param_use_lists(ir, n, &call_param_head, &param_sibling);

  const size_t mat = (size_t)nb * bw;
  uint64_t *use_b = tcc_mallocz(sizeof(uint64_t) * mat);
  uint64_t *def_b = tcc_mallocz(sizeof(uint64_t) * mat);
  uint64_t *in_b = tcc_mallocz(sizeof(uint64_t) * mat);
  uint64_t *out_b = tcc_mallocz(sizeof(uint64_t) * mat);
  uint64_t *reach_b = tcc_mallocz(sizeof(uint64_t) * mat); /* possibly-defined at block exit */
  uint64_t *row = tcc_malloc(sizeof(uint64_t) * bw);
  uint64_t *seed = tcc_mallocz(sizeof(uint64_t) * bw);     /* defined on entry: the parameters */

  #define W_ROW(vr) (tcc_ir_vreg_is_valid(ir, (vr)) ? dense[ra_vidx(vx, (vr))] : -1)

  for (int idx = vx->base[TCCIR_VREG_TYPE_PARAM]; idx < vsize; idx++)
    if (dense[idx] >= 0)
      tcc_bitspan_set(seed, dense[idx]);

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
        int r = W_ROW(uses[k]);
        if (r >= 0 && !tcc_bitspan_test(db, r)) tcc_bitspan_set(ub, r);
      }
      for (int p = call_param_head[i]; p >= 0; p = param_sibling[p])
      {
        int r = W_ROW(irop_get_vreg(tcc_ir_op_get_src1(ir, &ir->compact_instructions[p])));
        if (r >= 0 && !tcc_bitspan_test(db, r)) tcc_bitspan_set(ub, r);
      }
      if (hd)
      {
        int r = W_ROW(def);
        if (r >= 0) tcc_bitspan_set(db, r);
      }
    }
  }

  /* Live-in / live-out, backward. */
  for (int changed = 1, guard = 0; changed && (has_ijump || guard++ < nb + 4);)
  {
    changed = 0;
    for (int ri = norder - 1; ri >= 0; ri--)
    {
      int b = order[ri];
      uint64_t *lo = out_b + (size_t)b * bw, *li = in_b + (size_t)b * bw;
      tcc_bitspan_zero(lo, bw);
      if (is_ijump[b])
        for (int sb = 0; sb < nb; sb++)
          tcc_bitspan_or(lo, in_b + (size_t)sb * bw, bw);
      for (int si = 0; si < cfg->blocks[b].num_succs; si++)
      {
        int sb = cfg->blocks[b].succs[si];
        if (sb >= 0 && sb < nb) tcc_bitspan_or(lo, in_b + (size_t)sb * bw, bw);
      }
      if (xsucc[b] >= 0)
        tcc_bitspan_or(lo, in_b + (size_t)xsucc[b] * bw, bw);
      changed |= tcc_bitspan_or_andnot(li, use_b + (size_t)b * bw, lo, def_b + (size_t)b * bw, bw);
    }
  }

  /* Possibly-defined, forward: reach_b[b] = (entry seed or any pred's reach) | defs(b). */
  for (int changed = 1, guard = 0; changed && (has_ijump || guard++ < nb + 4);)
  {
    changed = 0;
    for (int ri = 0; ri < norder; ri++)
    {
      int b = order[ri];
      tcc_bitspan_zero(row, bw);
      if (b == 0)
        tcc_bitspan_or(row, seed, bw);
      for (int ib = 0; has_ijump && ib < nb; ib++)
        if (is_ijump[ib])
          tcc_bitspan_or(row, reach_b + (size_t)ib * bw, bw);
      for (int pi = 0; pi < cfg->blocks[b].num_preds; pi++)
      {
        int pb = cfg->blocks[b].preds[pi];
        if (pb >= 0 && pb < nb) tcc_bitspan_or(row, reach_b + (size_t)pb * bw, bw);
      }
      uint64_t *rb = reach_b + (size_t)b * bw, *db = def_b + (size_t)b * bw;
      for (int w = 0; w < bw; w++)
      {
        uint64_t v = row[w] | db[w];
        if (v != rb[w]) { rb[w] = v; changed = 1; }
      }
    }
  }

  /* Widen: live-in (and possibly defined) at a block covers its first
   * instruction, live-out covers its last.  Live-in without a use or live-out
   * in the block is impossible, so the points between are covered by the
   * range's own references. */
  int *idx_of = tcc_malloc(sizeof(int) * nd);
  for (int idx = 0; idx < vsize; idx++)
    if (dense[idx] >= 0) idx_of[dense[idx]] = idx;
  int widened = 0;
  for (int ri = 0; ri < norder; ri++)
  {
    int b = order[ri];
    const uint32_t first = (uint32_t)cfg->blocks[b].start_idx;
    const uint32_t last = (uint32_t)(cfg->blocks[b].end_idx - 1);
    if (cfg->blocks[b].end_idx <= cfg->blocks[b].start_idx || last >= (uint32_t)n)
      continue;
    tcc_bitspan_zero(row, bw);
    if (b == 0)
      tcc_bitspan_or(row, seed, bw);
    for (int ib = 0; has_ijump && ib < nb; ib++)
      if (is_ijump[ib])
        tcc_bitspan_or(row, reach_b + (size_t)ib * bw, bw);
    for (int pi = 0; pi < cfg->blocks[b].num_preds; pi++)
    {
      int pb = cfg->blocks[b].preds[pi];
      if (pb >= 0 && pb < nb) tcc_bitspan_or(row, reach_b + (size_t)pb * bw, bw);
    }
    const uint64_t *li = in_b + (size_t)b * bw, *lo = out_b + (size_t)b * bw, *rb = reach_b + (size_t)b * bw;
    for (int w = 0; w < bw; w++)
    {
      for (int pass = 0; pass < 2; pass++)
      {
        uint64_t bits = pass == 0 ? (li[w] & row[w]) : (lo[w] & rb[w]);
        const uint32_t at = pass == 0 ? first : last;
        for (; bits; bits &= bits - 1)
        {
          int k = w * 64 + __builtin_ctzll(bits);
          if (k >= nd) break;
          int idx = idx_of[k];
          if (starts[idx] > at) { starts[idx] = at; widened++; }
          if (ends[idx] < at) { ends[idx] = at; widened++; }
          /* Live INTO the instruction at `at` (pass 0): the value is already
           * in its register when that instruction runs, so a call / dispatch /
           * asm sitting exactly at the range start still clobbers it.  The
           * crossing tests are exclusive of the start; tell the caller. */
          if (pass == 0 && starts[idx] == at) live_in_at_start[idx] = 1;
        }
      }
    }
  }
  if (widened)
    RA_DBG("ra_widen_intervals_by_liveness: %d range ends moved", widened);

  #undef W_ROW
  tcc_free(idx_of);
  tcc_free(seed);
  tcc_free(row);
  tcc_free(use_b); tcc_free(def_b); tcc_free(in_b); tcc_free(out_b); tcc_free(reach_b);
  tcc_free(call_param_head); tcc_free(param_sibling);
  tcc_free(xsucc);
  tcc_free(is_ijump);
  tcc_free(order);
  tcc_ir_cfg_free(cfg);
  tcc_free(dense);
}

static int ra_may_need_frame_pointer(const TCCIRState *ir)
{
  if (tcc_state->force_frame_pointer)
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

int tcc_ir_callee_saved_capacity(const TCCIRState *ir, const RegAllocTarget *target)
{
  uint64_t allowed = tcc_state->registers_map_for_allocator;
  if (target->frame_pointer_reg >= 0 && !ra_may_need_frame_pointer(ir))
    allowed |= 1ull << target->frame_pointer_reg;
  if (target->static_chain_reg >= 0 && (ir->has_static_chain || tcc_state->nb_nested_funcs > 0))
    allowed &= ~(1ull << target->static_chain_reg);
  if (target->rodata_anchor_reg >= 0 && target->rodata_anchor_sites &&
      target->rodata_anchor_sites(ir, RA_RODATA_ANCHOR_MIN_SITES) >= RA_RODATA_ANCHOR_MIN_SITES)
    allowed &= ~(1ull << target->rodata_anchor_reg);
  int count = 0;
  for (int i = 0; i < target->int_class.num_callee_saved; ++i)
    count += !!(allowed & (1ull << target->int_class.callee_saved[i]));
  return count;
}

/* The return-block share (below, `reg_shared`) lets cur overwrite a register
 * another live interval still owns, on the argument that control goes from
 * cur's definition straight to the return at cur->end.  That holds only when
 * no instruction in [start, end) can send control anywhere outside the range:
 * a branch out of it reaches code that may still read the partner, after cur's
 * definition already clobbered the partner's register.  The partner-touch scan
 * only sees instructions INSIDE the range, so it cannot catch that.
 *
 * Zig's error-union shape `p = r.payload; if (r.error) return r.error;` built
 * it: the error value's coalesced class starts at its load, before the test,
 * so the branch to the code that stores through `p` lay inside cur's range, and
 * the shared r0 replaced `p` by the error code (zig.c ast-check segfaults with
 * ext_elim, which turned the error's re-extension into copies the coalescer
 * merged).  Jumps within the range are fine: every path stays on the way to
 * the return.  Anything that leaves it -- a jump out, an indirect or table
 * jump, asm goto, setjmp/longjmp -- keeps the share off.  Returns and traps
 * inside the range leave the function and are fine.
 * TCC_DISABLE_PASS=ra:ret_share_exit drops this guard (A/B only: unsound). */
static int ra_ret_tail_closed(TCCIRState *ir, int start, int end)
{
  if (tcc_ir_opt_pass_disabled("ra:ret_share_exit"))
    return 1;
  for (int p = start; p < end; p++)
  {
    IRQuadCompact *q = &ir->compact_instructions[p];
    if (q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_JUMPIF)
    {
      int t = (int)tcc_ir_op_dest_imm(ir, q);
      if (t < start || t > end)
        return 0;
      continue;
    }
    if (ir_op_has(q->op, IR_HZ_BRANCH | IR_HZ_NONLOCAL | IROP_A_MAY_BRANCH | IROP_A_RETURNS_TWICE))
      return 0;
  }
  return 1;
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
    if (a->precolored >= 0 || a->caller_save || a->co_member || a->loop_phi_locked || a->reg_shared || a->addrtaken)
      return 0;
    if (a->reg_type == LS_REG_TYPE_FLOAT || a->reg_type == LS_REG_TYPE_DOUBLE)
      return 0;
    if (ra_alive_busy(alive, (int32_t)a->vreg, cur->start, cur->end))
      return 0;
  }
  return holders > 0;
}

/* Spill-slot reuse.  A spilled interval only touches its slot inside
 * [start, end], so once it has expired the slot can hold a later interval of
 * the same size.  Without this every spill took a fresh slot for the whole
 * function, and a frame grew with the TOTAL number of spills rather than the
 * most that are live at once -- Zig's generated C, one long function body of
 * short-lived temporaries after another, had median frames of 104 bytes where
 * gcc needs 36, with 123k stack accesses past the 16-bit LDR/STR [sp] reach.
 *
 * A slot is reused only if its previous owner ended before the new owner
 * STARTS.  That is automatic for an interval spilled when the scan reaches it,
 * but not for an eviction victim: it had a register since its start and is
 * spilled for the whole interval, so a slot freed after that start is still
 * shared.  TCC_NO_SPILL_REUSE=1 restores the fresh-slot behaviour. */
TCC_DBG_ENV_FLAG(ra_no_spill_reuse, "TCC_NO_SPILL_REUSE")

static void ra_spill_pool_expire(RaSpillPool *p, uint32_t pos)
{
  if (p->active_count == 0 || p->active_min_end >= pos)
    return;
  int w = 0;
  p->active_min_end = UINT32_MAX;
  for (int j = 0; j < p->active_count; j++)
  {
    SSAInterval *a = p->active[j].iv;
    if (a->end < pos)
    {
      RaFreeSpillSlot *f = &p->free_slots[p->free_count++];
      f->off = a->stack_location;
      f->size = p->active[j].size;
      f->released = a->end;
    }
    else
    {
      p->active[w++] = p->active[j];
      if (a->end < p->active_min_end)
        p->active_min_end = a->end;
    }
  }
  p->active_count = w;
}

/* Give `iv` a `size`-byte spill slot: a free one released before iv->start,
 * else a fresh one below *spill_loc. */
static void ra_spill_pool_assign(RaSpillPool *p, SSAInterval *iv, int size, int *spill_loc)
{
  int off = 0, found = 0;
  if (!p->no_reuse)
    for (int k = p->free_count - 1; k >= 0; k--)
      if (p->free_slots[k].size == size && p->free_slots[k].released < iv->start)
      {
        off = p->free_slots[k].off;
        p->free_slots[k] = p->free_slots[--p->free_count];
        found = 1;
        break;
      }
  if (!found)
  {
    *spill_loc -= size;
    off = *spill_loc;
  }
  iv->stack_location = off;
  /* Grown on demand rather than sized by the interval count: most intervals
   * get a register.  Expiry only moves entries from active to free, so the
   * sum of the two never passes the number of assignments. */
  if (p->active_count + p->free_count >= p->cap)
  {
    p->cap = p->cap ? 2 * p->cap : 16;
    p->free_slots = tcc_realloc(p->free_slots, sizeof(RaFreeSpillSlot) * p->cap);
    p->active = tcc_realloc(p->active, sizeof(RaActiveSpill) * p->cap);
  }
  p->active[p->active_count].iv = iv;
  p->active[p->active_count++].size = size;
  if (iv->end < p->active_min_end)
    p->active_min_end = iv->end;
}

/* What spilling `iv` for its whole range costs: a reload per (loop-weighted)
 * use plus the store of its definition. */
static uint32_t ra_spill_cost(const SSAInterval *iv)
{
  return (uint32_t)iv->use_count + 1;
}

static int ra_pressure_ev_cmp(const void *a, const void *b)
{
  const RaPressureEv *x = a, *y = b;
  if (x->pos != y->pos)
    return x->pos < y->pos ? -1 : 1;
  return y->delta - x->delta; /* starts before ends at one position: the upper bound */
}
/* ra:evict_pair: a 64-bit value in a register pair may be the victim a single
 * INT interval evicts -- both registers come free, cur takes the low one.  A
 * cold pair held across a whole function (the template-replay timestamp read
 * once before and once after its loop) otherwise kept two callee-saved
 * registers while the loop's hot values spilled. */
static int ra_pair_victim_ok(const SSAInterval *a, const SSAInterval *cur, const RegAllocTarget *target)
{
  if (a->reg_type != LS_REG_TYPE_LLONG || a->r0 < 0 || a->r1 < 0 || a->stack_location != 0 ||
      a->alive_shared || a->reg_shared)
    return 0;
  if (cur->crosses_call) {
    /* cur takes the low register: it must survive cur's calls */
    for (int ci = 0; ci < target->int_class.num_callee_saved; ci++)
      if (target->int_class.callee_saved[ci] == a->r0)
        return 1;
    return 0;
  }
  return 1;
}

void ra_linear_scan(TCCIRState *ir, SSAInterval *intervals, int count,
                           const RegAllocTarget *target, int spill_base,
                           uint64_t *out_dirty_int, uint64_t *out_dirty_fp,
                           int max_vreg_pos, int has_call, const RaAliveInfo *alive)
{
  if (count <= 0) return;

  if (ra_trace_on())
    RA_DBG("SSA ra_linear_scan: decision trace for %s (%d intervals)", ra_trace_func(), count);

  tcc_qsort(intervals, count, sizeof(SSAInterval), sort_by_start);

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
  const int evict_density = TCC_OPT(tcc_state, optimize) >= 1 && !tcc_ir_opt_pass_disabled("ra:evict_density");
  const int evict_swap = TCC_OPT(tcc_state, optimize) >= 1 && !tcc_ir_opt_pass_disabled("ra:evict_swap");
  /* ra:caller_save: call-crossing values in free caller-saved registers,
   * saved around the calls they cross (see the eligibility in ra_build_intervals). */
  const int caller_save_on = TCC_OPT(tcc_state, optimize) >= 1 && !tcc_ir_opt_pass_disabled("ra:caller_save");
  const int evict_pair = TCC_OPT(tcc_state, optimize) >= 1 && !tcc_ir_opt_pass_disabled("ra:evict_pair");
  const int low_saved_pref = TCC_OPT(tcc_state, optimize) >= 1 && !tcc_ir_opt_pass_disabled("ra:low_saved_pref");

  /* Build vreg -> interval lookup for phi hint resolution (dense layout,
   * see RaVregIdx: every interval's vreg is below these counts). */
  RaVregIdx hx;
  ra_vidx_init(&hx, ir->next_local_variable, ir->next_temporary_variable, ir->next_parameter);
  int hint_tbl_size = hx.size;
  SSAInterval **vreg_to_iv = tcc_mallocz(sizeof(SSAInterval *) * hint_tbl_size);
  #define HINT_IDX(vr) ra_vidx(&hx, (vr))
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
  int n_mem = 0;
  for (int i = 0; i < count; i++)
    n_mem += intervals[i].addrtaken || intervals[i].is_volatile;
  /* Only memory-forced intervals enter it (and free_slots_4 below): size both
   * by their number, not by every interval. */
  SSAInterval **active_addrtaken = tcc_malloc(sizeof(SSAInterval *) * (n_mem > 0 ? n_mem : 1));
  int active_addrtaken_count = 0;
  uint32_t active_addrtaken_min_end = UINT32_MAX;

  /* Free list of expired 4-byte addrtaken stack slots, available for reuse
   * by later addrtaken intervals.  Only 4-byte slots are tracked here; other
   * sizes fall through to fresh allocation, matching the legacy behavior of
   * always assigning `spill_loc -= 4` regardless of value width. */
  int *free_slots_4 = tcc_malloc(sizeof(int) * (n_mem > 0 ? n_mem : 1));
  int free_slots_4_count = 0;

  /* When more call-crossing INT values are live at once than there are free
   * low callee-saved registers, r8-r11 get pushed whatever the order, so the
   * question is only which values take r4-r7.  First come first served gave
   * them to whatever started earliest -- a pointer passed from call to call
   * -- and the value compared with #0 after every call landed in r9: every
   * CMP #imm, CBZ, LDR/STR [Rn,#imm] and ADDS on it 32-bit (9,400 CMP.W on a
   * high register in zig.c against gcc's 3,400).  Then a call-crossing value
   * at most one 16-bit-capable op touches takes a free high register first,
   * leaving r4-r7 to the values that use them (ra:hi_callee_pref).  Measured
   * on zig.c -Os: narrow_uses <= 1 -11.5 KB, == 0 -1.4, <= 2 -4.6; density
   * gates (as the non-crossing preference uses) all lost. */
  int hi_callee_pref = 0;
  if (TCC_OPT(tcc_state, optimize) >= 1 && !tcc_ir_opt_pass_disabled("ra:hi_callee_pref")) {
    int low_cs = __builtin_popcountll(int_free & 0xf0ull);
    int nev = 0;
    RaPressureEv *ev = tcc_malloc(sizeof(RaPressureEv) * 2 * (count > 0 ? count : 1));
    for (int i = 0; i < count; i++) {
      SSAInterval *iv = &intervals[i];
      if (iv->coalesce_to >= 0 || !iv->crosses_call || iv->addrtaken)
        continue;
      const int w = iv->reg_type == LS_REG_TYPE_INT ? 1 : iv->reg_type == LS_REG_TYPE_LLONG ? 2 : 0;
      if (!w)
        continue;
      ev[nev].pos = iv->start, ev[nev].delta = w, nev++;
      ev[nev].pos = iv->end + 1, ev[nev].delta = -w, nev++;
    }
    tcc_qsort(ev, nev, sizeof(RaPressureEv), ra_pressure_ev_cmp);
    int live = 0, peak = 0;
    for (int k = 0; k < nev; k++) {
      live += ev[k].delta;
      if (live > peak)
        peak = live;
    }
    tcc_free(ev);
    hi_callee_pref = peak > low_cs;
  }

  int spill_loc = spill_base;
  RaSpillPool spool = {0};
  spool.active_min_end = UINT32_MAX;
  spool.no_reuse = ra_no_spill_reuse() || tcc_ir_calls_returns_twice(ir);

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

    ra_spill_pool_expire(&spool, cur->start);

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
      if (free_slots_4_count > 0 && !spool.no_reuse) {
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
        ra_spill_pool_assign(&spool, cur, 4, &spill_loc);
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
        ra_spill_pool_assign(&spool, cur, 8, &spill_loc);
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
              tcc_ir_op_src1_vreg(ir, eq) == cur->vreg)
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
          /* Same co-holder rule: an interval whose registers someone else
           * still holds is not a victim — freeing them frees nothing. */
          if (ra_reg_has_other_holder(active, active_count, a, a->r0) ||
              (a->r1 >= 0 && ra_reg_has_other_holder(active, active_count, a, a->r1)))
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

      /* In-place pair inheritance.  UMAAL accumulates into its destination
       * pair, and ZEXT / PACK64 build theirs from words that usually die at
       * that very instruction: taking the dying operands' registers makes the
       * moves the emitter would otherwise need disappear.  A donor qualifies
       * only when this instruction is its last use (end == cur->start), and
       * only on the terms the pair boundary reuse below sets for donors. */
      if (r0 < 0 && cur->reg_type == LS_REG_TYPE_LLONG && !cur->co_member &&
          TCC_OPT(tcc_state, optimize) >= 1 && max_vreg_pos > 0 &&
          (int)cur->start < ir->next_instruction_index) {
        IRQuadCompact *dq = &ir->compact_instructions[cur->start];
        SSAInterval *don[2] = {NULL, NULL};
        if (dq->op == TCCIR_OP_UMAAL) {
          int32_t av = tcc_ir_op_accum_vreg(ir, dq);
          int ai = av >= 0 ? HINT_IDX(av) : -1;
          SSAInterval *p = (ai >= 0 && ai < hint_tbl_size) ? vreg_to_iv[ai] : NULL;
          if (p && p->r1 >= 0)
            don[0] = don[1] = p;
        } else if (dq->op == TCCIR_OP_ZEXT || dq->op == TCCIR_OP_PACK64) {
          for (int w = 0; w < (dq->op == TCCIR_OP_PACK64 ? 2 : 1); w++) {
            IROperand so = tcc_ir_op_get_src1_or_2(ir, dq, w);
            int32_t sv = (!so.is_lval && irop_has_vreg(so)) ? irop_get_vreg(so) : -1;
            int si = sv >= 0 ? HINT_IDX(sv) : -1;
            SSAInterval *p = (si >= 0 && si < hint_tbl_size) ? vreg_to_iv[si] : NULL;
            /* A word-typed source naming a pair reads its low register. */
            if (p && (p->r1 < 0 || !irop_needs_pair(so)))
              don[w] = p;
          }
        }
        int want[2] = {-1, -1};
        for (int w = 0; w < 2; w++) {
          SSAInterval *p = don[w];
          if (!p || p->end != cur->start || p->r0 < 0 || p->stack_location != 0 ||
              p->co_member || p->loop_phi_locked || p->alive_shared ||
              (p->reg_type != LS_REG_TYPE_INT && p->reg_type != LS_REG_TYPE_LLONG))
            continue;
          /* UMAAL inherits the accumulator pair word for word; ZEXT / PACK64
           * read each source's low register. */
          int hr = (dq->op == TCCIR_OP_UMAAL && w == 1) ? p->r1 : p->r0;
          /* Retiring the donor frees both of its registers: neither may have
           * another holder (an alive_share borrower). */
          if (hr >= tcc_state->registers_for_allocator ||
              ra_reg_has_other_holder(active, active_count, p, p->r0) ||
              (p->r1 >= 0 && ra_reg_has_other_holder(active, active_count, p, p->r1)))
            continue;
          if (cur->crosses_call) {
            int cs = 0;
            for (int ci = 0; ci < target->int_class.num_callee_saved; ci++)
              if (target->int_class.callee_saved[ci] == hr) cs = 1;
            if (!cs)
              continue;
          }
          want[w] = hr;
        }
        if (want[0] >= 0 && want[0] == want[1])
          want[1] = -1;
        /* Every other path hands out pairs with r0 < r1, and code downstream
         * leans on it: the return-pair eviction takes r0:r1 from a boundary
         * donor, so a donor pair r4:r0 made a 64-bit ADD write r0 (its low
         * word) before reading r0 (the source's high word).  Keep the order;
         * a crossed pair of donors keeps just the low one. */
        if (want[0] >= 0 && want[1] >= 0 && want[0] > want[1])
          want[1] = -1;
        if (want[0] >= 0 || want[1] >= 0) {
          /* The other word (ZEXT's high half, or an operand that stays live)
           * takes any free register the pair rules allow. */
          for (int w = 0; w < 2; w++) {
            if (want[w] >= 0)
              continue;
            for (int r = 0; r < 13; r++) {
              if (!(int_free & (1ull << r)) || r == want[1 - w] ||
                  r >= tcc_state->registers_for_allocator)
                continue;
              if (want[1 - w] >= 0 && (w == 0 ? r > want[1] : r < want[0]))
                continue;
              if (cur->crosses_call) {
                int cs = 0;
                for (int ci = 0; ci < target->int_class.num_callee_saved; ci++)
                  if (target->int_class.callee_saved[ci] == r) cs = 1;
                if (!cs)
                  continue;
              }
              want[w] = r;
              break;
            }
          }
          if (want[0] >= 0 && want[1] >= 0 && want[0] < want[1]) {
            /* Commit: retire the donors that supplied a register (they end
             * here, and nothing else holds their registers), so the
             * registers are ours. */
            for (int w = 0; w < 2; w++) {
              SSAInterval *p = don[w];
              if (!p || (w == 1 && p == don[0]))
                continue;
              int took = 0;
              for (int x = 0; x < 2; x++)
                took |= want[x] == p->r0 || (p->r1 >= 0 && want[x] == p->r1);
              if (!took)
                continue;
              for (int k = 0; k < active_count; k++) {
                if (active[k] == p && p->end == cur->start) {
                  if (p->r0 >= 0) int_free |= (1ull << p->r0);
                  if (p->r1 >= 0) int_free |= (1ull << p->r1);
                  active[k] = active[--active_count];
                  break;
                }
              }
            }
            r0 = want[0];
            r1 = want[1];
          }
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
            if (a->precolored >= 0 || a->caller_save) continue;
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
            /* Its register has to be ITS ALONE.  alive_share leaves a second
             * live holder (a borrower, or the owner a borrower sits on) on a
             * register the allocator still shows as taken by one of them, so
             * spilling this victim does not free the register -- and putting it
             * in int_free hands it to the pair (or, when the pair cannot be
             * completed, to the next call-crossing value), overwriting the
             * holder that is still living there.  Same rule as the single-INT
             * victim scan below and the expire path. */
            if (ra_reg_has_other_holder(active, active_count, a, a->r0)) continue;
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
          ra_spill_pool_assign(&spool, victim, 4, &spill_loc);
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
            /* A pair the donor itself only BORROWED is not the donor's to pass
             * on.  ra_alive_reg_shareable proved the real owner dead across the
             * DONOR's range and no further, so inheriting the pair here extends
             * the loan over a range nothing has checked -- and the owner, still
             * live, finds its register rewritten under it.
             *
             * __aeabi_dadd built exactly that chain: alive_share lent r4:r5 to
             * a temp over [140,148] while the aligned-mantissa phi owned them
             * across [131,180], boundary reuse passed them to [148,149] and
             * then to the sticky-bit value over [149,160], and the phi's
             * mantissa came back as the other operand's.  1.0 + 12.0 returned
             * 24.0. */
            if (p->alive_shared)
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
            cur->alive_shared = 1;
            alive_shared_used = 1;
            active[active_count++] = cur;
            RA_DBG("  alive_share pair T%d [%u,%u] -> R%d:R%d",
                   TCCIR_DECODE_VREG_POSITION(cur->vreg), cur->start, cur->end, p0, p1);
            continue;
          }
        }

        ra_spill_pool_assign(&spool, cur, 8, &spill_loc);
      }
      continue;
    }

    /* Complex double: always spill (128-bit) */
    if (cur->reg_type == LS_REG_TYPE_COMPLEX_DOUBLE) {
      ra_spill_pool_assign(&spool, cur, 16, &spill_loc);
      continue;
    }

    /* Integer: single register */
    int reg = -1;
    RA_DBG("  alloc T%d [%u,%u] uses=%u xcall=%d co=%d co_to=%d lp=%d hint=%d narrow=%u int_free=0x%llx active=%d",
           TCCIR_DECODE_VREG_POSITION(cur->vreg), cur->start, cur->end,
           cur->use_count, cur->crosses_call, cur->co_member, ra_coalesce_pos(cur),
           cur->loop_phi_locked, cur->hint_vreg, cur->narrow_uses,
           (unsigned long long)int_free, active_count);

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
          /* Partner expiring at cur->start does NOT free hr while an
           * alive_share borrower's interval runs past it: taking hr here
           * would clobber the borrower, and marking it free would let the
           * next allocation clobber it again. */
          if (boundary && ra_reg_has_other_holder(active, active_count, partner, hr))
            boundary = 0;
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
        cur->reg_type == LS_REG_TYPE_INT && TCC_OPT(tcc_state, optimize) >= 1) {
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
                  a->stack_location == 0 && a->reg_type == LS_REG_TYPE_INT &&
                  !ra_reg_has_other_holder(active, active_count, a, hr)) {
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
              (int)cur->end < ir->next_instruction_index &&
              ra_ret_tail_closed(ir, (int)cur->start, (int)cur->end)) {
            IRQuadCompact *eq = &ir->compact_instructions[cur->end];
            if (eq->op == TCCIR_OP_RETURNVALUE || eq->op == TCCIR_OP_RETURNVOID) {
              for (int k = 0; k < active_count; k++) {
                SSAInterval *a = active[k];
                if (a->r0 != hr || a->r1 >= 0 || a->stack_location != 0 ||
                    a->reg_type != LS_REG_TYPE_INT || a->caller_save)
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

    if (reg < 0 && cur->crosses_call && hi_callee_pref && cur->narrow_uses <= 1) {
      for (int ci = 0; ci < target->int_class.num_callee_saved && reg < 0; ci++) {
        int r = target->int_class.callee_saved[ci];
        if (r >= 8 && r < tcc_state->registers_for_allocator && (int_free & (1ull << r)))
          reg = r;
      }
    }
    if (reg < 0 && cur->crosses_call) {
      /* Prefer callee-saved */
      for (int ci = 0; ci < target->int_class.num_callee_saved; ci++) {
        int r = target->int_class.callee_saved[ci];
        if (int_free & (1ull << r)) { reg = r; break; }
      }
    }
    /* No callee-saved register left: a value whose crossed calls are cheaper
     * to save around than its uses are to reload lives in a free caller-saved
     * register, which the call lowering stores before and reloads after each
     * call it crosses (the nested-call save area). */
    int cs_take = 0;
    if (reg < 0 && cur->crosses_call && cur->cs_ok && cur->precolored < 0 && caller_save_on &&
        2u * (uint32_t)cur->cs_cost <= ra_spill_cost(cur)) {
      /* ...and cheaper than the eviction the spill path would make: a cold
       * victim's reloads can cost less than these saves. */
      uint32_t best_victim = UINT32_MAX;
      for (int j = 0; j < active_count; j++) {
        SSAInterval *a = active[j];
        if (a->precolored >= 0 || a->caller_save || a->end <= cur->end || a->loop_phi_locked)
          continue;
        if (a->reg_type != LS_REG_TYPE_INT && !(evict_pair && ra_pair_victim_ok(a, cur, target)))
          continue;
        if (ra_spill_cost(a) < best_victim)
          best_victim = ra_spill_cost(a);
      }
      cs_take = (uint32_t)cur->cs_cost < best_victim;
    }
    if (cs_take) {
      /* Leave caller-saved registers for the short temporaries in its range:
       * taking the last ones moves the spills onto them.  Kernel warm launch
       * (instructions): keep 2 -> 264.5k, 3 -> 263.9k, 4 -> 264.6k. */
      const int cs_keep = 3;
      int free_cs = 0;
      static const int cs_regs[] = {0, 1, 2, 3, 12};
      for (int oi = 0; oi < 5; oi++)
        if (cs_regs[oi] < tcc_state->registers_for_allocator && (int_free & (1ull << cs_regs[oi])))
          free_cs++;
      if (free_cs - 1 < cs_keep)
        cs_take = 0;
    }
    if (cs_take) {
      static const int cs_order[] = {3, 2, 1, 12, 0};
      for (int oi = 0; oi < 5; oi++) {
        int r = cs_order[oi];
        if (r < tcc_state->registers_for_allocator && (int_free & (1ull << r))) {
          reg = r;
          cur->caller_save = 1;
          if (ra_trace_on())
            RA_DBG("  caller_save T%d [%u,%u] uses=%u cs_cost=%u -> R%d (saved around its calls)",
                   TCCIR_DECODE_VREG_POSITION(cur->vreg), cur->start, cur->end, cur->use_count,
                   cur->cs_cost, r);
          break;
        }
      }
    }
    if (reg < 0 && !cur->crosses_call &&
        (int)cur->start < ir->next_instruction_index) {
      IRQuadCompact *dq = &ir->compact_instructions[cur->start];
      if (tcc_ir_op_is_mac(dq->op) &&
          tcc_ir_op_dest_vreg(ir, dq) == cur->vreg) {
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
      /* A low callee-saved register the prologue already saves beats R12: every
       * access through R12 is a 32-bit encoding, and the push costs nothing
       * more.  zig.c -O2: -9 KB (TCC_DISABLE_PASS=ra:low_saved_pref). */
      if (low_saved_pref && order == alloc_order) {
        uint64_t lowdirty = int_free & dirty_int & 0xf0ull;
        for (int r = 4; r <= 7 && reg < 0; r++)
          if ((lowdirty & (1ull << r)) && r < tcc_state->registers_for_allocator)
            reg = r;
      }
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
      /* Outliving victims retain priority; see docs/bugs/ra-spill-self-despite-cheaper-short-actives.md. */
      SSAInterval *victim = NULL;
      int victim_idx = -1;
      uint16_t victim_uses = UINT16_MAX;
      SSAInterval *swap_victim = NULL;
      int swap_idx = -1;
      uint32_t swap_cost = ra_spill_cost(cur);
      for (int j = 0; j < active_count; j++) {
        SSAInterval *a = active[j];
        if (a->precolored >= 0 || a->caller_save) continue;
        if (a->reg_type != LS_REG_TYPE_INT && !(evict_pair && ra_pair_victim_ok(a, cur, target))) continue;
        int short_active = a->end <= cur->end;
        if (short_active && (!evict_swap || ra_spill_cost(a) >= swap_cost)) continue;
        if (short_active && cur->crosses_call) {
          int callee_saved = 0;
          for (int ci = 0; ci < target->int_class.num_callee_saved; ci++)
            callee_saved |= target->int_class.callee_saved[ci] == a->r0;
          if (!callee_saved) continue;
        }
        /* Absorbed loop-phi partners still depend on this register. */
        if (a->loop_phi_locked) continue;
        /* Evicting one co-holder does not free the register. */
        {
          int co_held = 0;
          for (int k = 0; k < active_count; k++) {
            SSAInterval *b = active[k];
            if (b != a && b != cur && b->stack_location == 0 &&
                (b->r0 == a->r0 || b->r1 == a->r0 ||
                 (a->r1 >= 0 && (b->r0 == a->r1 || b->r1 == a->r1)))) { co_held = 1; break; }
          }
          if (co_held) continue;
        }
        if (short_active) {
          swap_victim = a;
          swap_idx = j;
          swap_cost = ra_spill_cost(a);
          continue;
        }
        if (evict_density) {
          if (!victim ||
              (uint64_t)ra_spill_cost(a) * (victim->end - cur->start + 1) <
                  (uint64_t)ra_spill_cost(victim) * (a->end - cur->start + 1)) {
            victim_uses = a->use_count;
            victim = a;
            victim_idx = j;
          }
        } else if (a->use_count < victim_uses ||
            (a->use_count == victim_uses && victim && a->end > victim->end)) {
          victim_uses = a->use_count;
          victim = a;
          victim_idx = j;
        }
      }
      if (!victim && swap_victim) {
        victim = swap_victim;
        victim_idx = swap_idx;
      }
      if (victim) {
        if (ra_trace_on())
          RA_DBG("  %s T%d evicts T%d [%u,%u] uses=%u from R%d; cur takes that R%d",
                 victim->end <= cur->end ? "spill_swap" : "spill_evict",
                 TCCIR_DECODE_VREG_POSITION(cur->vreg),
                 TCCIR_DECODE_VREG_POSITION(victim->vreg), victim->start, victim->end,
                 victim->use_count, victim->r0, victim->r0);
        /* Evict victim, give its register to cur */
        reg = victim->r0;
        victim->r0 = -1;
        if (victim->r1 >= 0) {
          /* a pair (ra:evict_pair): its high register is free from here on */
          int_free |= (1ull << victim->r1);
          victim->r1 = -1;
          ra_spill_pool_assign(&spool, victim, 8, &spill_loc);
        } else {
          ra_spill_pool_assign(&spool, victim, 4, &spill_loc);
        }
        /* Remove victim from active */
        active[victim_idx] = active[--active_count];
        cur->r0 = reg;
        active[active_count++] = cur;
      } else {
        ra_spill_pool_assign(&spool, cur, 4, &spill_loc);
        if (ra_trace_on())
          RA_DBG("  spill_self T%d [%u,%u] uses=%u: no eligible outliving or cheaper short active -> spill(%d)",
                 TCCIR_DECODE_VREG_POSITION(cur->vreg), cur->start, cur->end,
                 cur->use_count, spill_loc);
      }
    }
  }

  tcc_free(spool.free_slots);
  tcc_free(spool.active);
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
        RA_DBG("  %s%d [%u,%u] uses=%u -> spill(%d)", ra_vreg_type_char(type), pos,
               iv->start, iv->end, iv->use_count, iv->stack_location);
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

void ra_write_results(TCCIRState *ir, SSAInterval *intervals, int count)
{
  /* Clear LS intervals and repopulate from SSA results */
  tcc_ls_clear_live_intervals(&ir->ls);
  tcc_ls_reserve(&ir->ls, count);

  for (int i = 0; i < count; i++) {
    SSAInterval *iv = &intervals[i];
    tcc_ls_add_live_interval(&ir->ls, iv->vreg, iv->start, iv->end,
                             iv->crosses_call, iv->addrtaken, iv->reg_type,
                             0, iv->precolored);
    LSLiveInterval *lsi = &ir->ls.intervals[ir->ls.next_interval_index - 1];
    lsi->caller_save = iv->caller_save && iv->stack_location == 0;
    ir->ls.caller_save_count += lsi->caller_save;
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
