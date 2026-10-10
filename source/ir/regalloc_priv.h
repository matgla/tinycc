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

/* Declarations shared by the SSA register allocator's source files
 * (regalloc.c and regalloc_{scan,phi,entry,post}.c), which used to be one
 * 9.5k-line TU.  Each function or variable declared at the bottom is
 * defined in the file its comment names.  The allocator's public interface
 * is regalloc.h. */

#ifndef REGALLOC_PRIV_H
#define REGALLOC_PRIV_H

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

/* Extra per-decision lines for ONE function's linear scan: TCC_RA_TRACE_FUNC=<name>
 * matches the function being compiled (tcc_state->cur_func_sym, set by gen_function
 * before its body is generated), so a whole-TU compile stays readable.  The lines
 * it gates name each spill decision (evict-whom / spill-self) and print the
 * coalescing and loop-lock state at each allocation; without the filter they stay
 * off.  Needs the usual -DTCC_LOG_LS=1 build to print at all. */
TCC_DBG_ENV_STR(ra_trace_func, "TCC_RA_TRACE_FUNC")

static inline int ra_trace_on(void)
{
  const char *want = ra_trace_func();
  if (!want)
    return 0;
  const Sym *fs = tcc_state ? tcc_state->cur_func_sym : NULL;
  if (!fs)
    return 0;
  const char *name = get_tok_str(fs->v, NULL);
  return name && !strcmp(want, name);
}

/* Dense index for per-vreg tables: the VARs, then the TEMPs, then the PARAMs,
 * each [0, count).  The type * max_pos + pos layout left the type-0 quarter
 * unused and sized every type by the largest count -- 4x the entries of a
 * temp-heavy function (1457-instruction regcomp: 2908 slots for 778 vregs).
 * A vreg outside every range maps to `size`, which every table user already
 * rejects with its `idx < size` bound. */
typedef struct RaVregIdx {
  int base[4];
  int cnt[4];
  int size;
} RaVregIdx;

static inline void ra_vidx_init(RaVregIdx *x, int nvar, int ntemp, int nparam)
{
  x->base[0] = 0, x->cnt[0] = 0;
  x->base[TCCIR_VREG_TYPE_VAR] = 0, x->cnt[TCCIR_VREG_TYPE_VAR] = nvar;
  x->base[TCCIR_VREG_TYPE_TEMP] = nvar, x->cnt[TCCIR_VREG_TYPE_TEMP] = ntemp;
  x->base[TCCIR_VREG_TYPE_PARAM] = nvar + ntemp, x->cnt[TCCIR_VREG_TYPE_PARAM] = nparam;
  x->size = nvar + ntemp + nparam;
  if (x->size <= 0)
    x->size = 1; /* one never-addressed slot keeps the tables non-empty */
}

static inline int ra_vidx(const RaVregIdx *x, int32_t vr)
{
  if (vr < 0)
    return x->size;
  const unsigned t = (unsigned)TCCIR_DECODE_VREG_TYPE(vr);
  const int p = TCCIR_DECODE_VREG_POSITION(vr);
  if (t > TCCIR_VREG_TYPE_PARAM || p >= x->cnt[t])
    return x->size;
  return x->base[t] + p;
}

/* The vreg at dense index idx (0 <= idx < size, and not the empty filler). */
static inline int32_t ra_vidx_vreg(const RaVregIdx *x, int idx)
{
  const int t = idx >= x->base[TCCIR_VREG_TYPE_PARAM] ? TCCIR_VREG_TYPE_PARAM
                : idx >= x->base[TCCIR_VREG_TYPE_TEMP] ? TCCIR_VREG_TYPE_TEMP
                                                       : TCCIR_VREG_TYPE_VAR;
  return TCCIR_ENCODE_VREG(t, idx - x->base[t]);
}

/* ============================================================================
 * SSA Live Interval
 * ============================================================================ */

typedef struct SSAInterval {
  /* Widest fields first: interleaving int8 and int32 fields padded the
   * struct to 40 bytes; grouped by size it is 36, on the device as well. */
  int32_t vreg;
  uint32_t start;
  uint32_t end;
  int32_t stack_location;
  int32_t hint_vreg;
  int32_t coalesce_to; /* graph coalescing: vreg of the representative this one merged into (-1 = rep / not merged) */
  uint16_t use_count;
  uint16_t narrow_uses; /* static count of references from ops with 16-bit encodings (want r0-r7) */
  int8_t r0;
  int8_t r1;
  int8_t precolored;
  int8_t pref_reg; /* soft hint: prefer this physical reg if available (e.g. r0 for RETURNVALUE feeders) */
  uint8_t crosses_call : 1;
  uint8_t crosses_real_call : 1; /* crosses a real FUNCCALL (VFP caller-saved clobber) — excludes native FP-op implicit calls */
  uint8_t addrtaken : 1;
  uint8_t is_volatile : 1; /* volatile-qualified local: force to a stack slot so every access is a real ldr/str */
  uint8_t is_param : 1;
  uint8_t reg_shared : 1; /* cur shares hr with another active interval (return-block tail); skip expire-free and active push */
  uint8_t alive_shared : 1; /* holds a pair BORROWED from a still-live owner (ra_alive_reg_shareable proved the owner dead only across THIS interval's range) — must not be passed on */
  uint8_t loop_phi_locked : 1; /* absorbed a loop-phi partner (carries a loop-carried value across the whole loop body); must not be evicted — spilling it mid-loop would not reload the partner's uses and corrupts the IV */
  uint8_t reg_type;
  uint8_t co_member;   /* 1 if part of a graph-coalesced class (rep or member) — the in-scan transfer must leave it alone */
  uint8_t cs_ok;       /* crosses only plain calls: may live in a caller-saved register saved around them */
  uint8_t caller_save; /* got one: the call lowering saves/reloads it around every call strictly inside [start,end] */
  uint16_t cs_cost;    /* loop-weighted saves + reloads that costs (2 * 4^depth per crossed call) */
} SSAInterval;

/* Decoded coalesce-class partner (graph coalescing), or -1 when the interval
 * is not part of a class.  For the trace lines only. */
static inline int ra_coalesce_pos(const SSAInterval *iv)
{
  return iv->coalesce_to >= 0 ? TCCIR_DECODE_VREG_POSITION(iv->coalesce_to) : -1;
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

typedef struct
{
  int off, size;
  uint32_t released; /* end of the interval that last held the slot */
} RaFreeSpillSlot;

typedef struct
{
  SSAInterval *iv;
  int size;
} RaActiveSpill;

typedef struct
{
  RaFreeSpillSlot *free_slots;
  int free_count;
  RaActiveSpill *active; /* spilled intervals still live */
  int active_count;
  int cap; /* entries in each array; free_count + active_count <= cap */
  uint32_t active_min_end;
  /* No reuse when the function calls setjmp, vfork, ...: a slot whose interval
   * ended on one return may still be read on the other. */
  int no_reuse;
} RaSpillPool;

/* A live-range edge for the callee-saved pressure sweep in ra_linear_scan. */
typedef struct {
  uint32_t pos;
  int delta;
} RaPressureEv;

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

#define RA_MAX_PHI_COPY_RECORDS 512

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
/* Memory-SSA is opt-in; see the comment at its use site in tcc_ir_ssa_regalloc. */
TCC_DBG_ENV_FLAG(ra_mem_ssa_fwd, "TCC_MEM_SSA_FWD")
TCC_DBG_ENV_FLAG(ra_mem_ssa_dump, "TCC_MEM_SSA_DUMP")
TCC_DBG_ENV_FLAG(ra_mem_ssa_verify_on, "TCC_MEM_SSA")

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

/* ============================================================================
 * Spill-driven live-range splitting
 *
 * Linear scan without splitting spills a value for its whole range: its def
 * stores it and EVERY use reloads it, even a use two instructions after the
 * def or right after another reload of the same slot.  Most spilled values in
 * call-heavy code (the Zig compiler: 88% of its spilled intervals cross a
 * real call) are forced to memory only by the calls; between two calls a
 * caller-saved register would hold them fine.
 *
 * After the first scan, each spilled 32-bit (or narrower) value V is split
 * where a register copy is provably enough:
 *   - def group: uses a def of V reaches within one call-free region read a
 *     fresh temp the def now writes; `V <- T` after the def is the store the
 *     spill needed anyway (dropped when V has one def and no other use);
 *   - reload groups: uses after an earlier use of V in one call-free region
 *     share `T <- V` (one reload) placed before that first use.
 * Then the allocator runs again on the rewritten IR.  The new temps are short
 * and cross no call, so they mostly land in caller-saved registers.
 * zig.c at -O2: -22 KB of .text; about half of the reloads it removes were
 * already elided by the encoder's slot cache, and in the giant functions
 * some temps spill again.  TCC_DISABLE_PASS=ra:spill_split turns it off.
 *
 * Reaching, for V with ONE definition d: dominance.  d dominates the leader
 * and the leader (or d) dominates each member, so a path from the copy to a
 * member that does not pass the copy again cannot pass d either, and V still
 * holds the copied value there.  For V with several definitions: straight
 * line only -- no jump target between the copy and the member, and no def of
 * V in between.
 *
 * "Call-free" is only a profitability filter (the second scan gives a temp
 * that crosses a call a callee-saved register or a slot, like any value): no
 * call in [start, member) on linear order, and no backward jump from beyond
 * the member into the range.
 * ==========================================================================*/

typedef struct
{
  int before;     /* old index of the instruction the copy goes in front of */
  int land;       /* 1: branches to `before` now land on the copy; 0: they skip it */
  int32_t dest, src;
  uint8_t btype, is_unsigned; /* the def's width for a def-group store copy */
  uint8_t entry;              /* 1: a loop group's entry copy; sorts last at a shared point */
} RaSplitCopy;

typedef struct
{
  int instr;
  int eff; /* where the value is consumed: the paired CALL for a FUNCPARAMVAL */
} RaSplitUse;

typedef struct RARelSlot
{
  int off;      /* frame offset of the slot */
  int size;     /* bytes the access covered (4 or 8; exact, see above) */
  int r0, r1;   /* registers whose value it holds (r1 < 0 when 32-bit) */
  int32_t vreg; /* the stored value */
  int def_idx;  /* the store that put it there */
  int valid;
} RARelSlot;

#define RA_REL_MAX_TRACKED 16

/* How far the straight-line walk below will look before giving up.  A store
 * this far from its return is not the shape being targeted. */
#define RA_DFS_TAIL_LIMIT 64

#define RA_DFS_MAX_READS 256

/* Defined in regalloc.c. */
int ra_reg_has_other_holder(SSAInterval **active, int active_count,
                                   const SSAInterval *skip, int hr);
int ir_op_is_implicit_call_ra(TccIrOp op);
int *ra_build_call_prefix(TCCIRState *ir);
const char *ra_vreg_type_char(int type);
void ra_build_intervals(TCCIRState *ir, IRCFG *cfg, IRSSAState *ssa,
                               SSAInterval **out_intervals, int *out_count,
                               const int *call_prefix, int *out_max_vreg_pos);
void ra_build_phi_hints(SSAInterval *intervals, int count,
                               IRSSAState *ssa, IRCFG *cfg, int max_vreg_pos);
void ra_build_assign_hints(SSAInterval *intervals, int count,
                                  TCCIRState *ir, int max_vreg_pos);
void ra_build_load_param_hints(SSAInterval *intervals, int count,
                                      TCCIRState *ir, int max_vreg_pos);
void ra_build_bfi_hints(SSAInterval *intervals, int count,
                               TCCIRState *ir, int max_vreg_pos);
void ra_build_outgoing_param_hints(SSAInterval *intervals, int count,
                                           TCCIRState *ir, int max_vreg_pos);

/* Defined in regalloc_scan.c. */
void ra_alive_free(RaAliveInfo *info);
int ra_alive_worth_building(TCCIRState *ir, const SSAInterval *intervals, int count);
void ra_alive_build(TCCIRState *ir, RaAliveInfo *info, int max_vreg_pos);
void ra_widen_intervals_by_liveness(TCCIRState *ir, const RaVregIdx *vx, uint32_t *starts, uint32_t *ends,
                                    uint8_t *live_in_at_start);
void ra_linear_scan(TCCIRState *ir, SSAInterval *intervals, int count,
                           const RegAllocTarget *target, int spill_base,
                           uint64_t *out_dirty_int, uint64_t *out_dirty_fp,
                           int max_vreg_pos, int has_call, const RaAliveInfo *alive);
void ra_write_results(TCCIRState *ir, SSAInterval *intervals, int count);

/* Defined in regalloc_phi.c. */
extern int ra_phi_resolve_pre_ra_mode;
int ra_phi_stats_has_participant(RAPhiStats *stats, int32_t vreg);
void ra_eliminate_dead_reg_copies(TCCIRState *ir);
int ra_phi_copy_is_identity(IRLiveInterval *dest_li, IRLiveInterval *src_li);
void ra_resolve_phis(TCCIRState *ir, IRCFG *cfg, IRSSAState *ssa,
                            RAPhiStats *stats);
void ra_build_live_regs_bitmap(TCCIRState *ir);
void ra_co_ops(TCCIRState *ir, IRQuadCompact *q,
                      int32_t *out_def, int *has_def, int32_t uses[4], int *nuse);
void ra_build_narrow_weights(TCCIRState *ir, const RegAllocTarget *target,
                                    SSAInterval *intervals, int count, int max_vreg_pos);
void ra_refine_live_regs_accurate(TCCIRState *ir);
void ra_coalesce_graph(TCCIRState *ir, SSAInterval *intervals, int count,
                              int max_vreg_pos);

#endif /* REGALLOC_PRIV_H */
