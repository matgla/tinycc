/*
 *  TCC IR - SSA Construction and Destruction
 *
 *  Copyright (c) 2025 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#define USING_GLOBALS
#include "ir.h"
#include "ssa.h"
#include "memory/dynamic_bitset.h"
#include "memory/vector.h"

TCC_DYNAMIC_BITSET_DEFINE(SSABitset, 256)

static inline int raw_bitset_test(const uint8_t *bits, int pos)
{
  return bits[pos / 8] & (1 << (pos % 8));
}

static inline void raw_bitset_set(uint8_t *bits, int pos)
{
  bits[pos / 8] |= (1 << (pos % 8));
}

/* ============================================================================
 * SSA Construction
 * ============================================================================ */

static IRPhiNode *ssa_alloc_phi(int32_t orig_vreg, int32_t dest_vreg, int num_preds, int btype)
{
  IRPhiNode *phi = tcc_mallocz(sizeof(IRPhiNode));
  phi->orig_vreg = orig_vreg;
  phi->dest_vreg = dest_vreg;
  phi->num_operands = num_preds;
  phi->cap_operands = num_preds;
  phi->btype = btype;
  phi->operands = tcc_mallocz(num_preds * sizeof(IRPhiOperand));
  for (int i = 0; i < num_preds; i++) {
    phi->operands[i].vreg = -1;
    phi->operands[i].pred_block = -1;
  }
  return phi;
}

typedef struct {
  uint8_t *def_blocks;
  SSABitset *addrtaken;
  SSABitset *multi_block_def;
  SSABitset *global; /* upward-exposed somewhere: a read may observe a cross-block value */
  SSABitset *nonstore_def; /* has at least one def that is not a slot STORE */
  int *var_btype;
  const uint8_t *store_def_ok; /* per VAR: an INT32 slot STORE covers every read */
  int block_bitset_bytes;
  int num_vars;
} SSAVarInfo;

/* TCC_NO_STORE2ASSIGN: bisection hook that falls the STORE-def path back to the
 * in-place rename (multi-def temp).  Read once — the callers sit in inner loops. */
static int ssa_no_store2assign(void)
{
  static int no_s2a = -1;
  if (no_s2a < 0)
    no_s2a = getenv("TCC_NO_STORE2ASSIGN") != NULL;
  return no_s2a;
}

/* A full-width var-SLOT STORE of a plain VALUE completely redefines the
 * variable, so the renamer turns it into an ASSIGN with a FRESH SSA name.
 * Phi placement must agree, or a fresh name defined on one arm is lost at the
 * join (wrong code) — hence this single predicate shared by both.  Returns the
 * VAR position, or -1 when the store must stay a real memory write.
 *
 * The source must be a plain value: a STORE whose src is StackLoc/SYMREF/deref
 * is a memory LOAD into the var, and as an ASSIGN it would vanish from the
 * stack/global store-liveness scans and let DSE drop the slot's init stores. */
static int ssa_store_slot_def_pos(TCCIRState *ir, IRQuadCompact *q,
                                  const uint8_t *store_def_ok, int num_vars)
{
  if (q->op != TCCIR_OP_STORE || !store_def_ok || ssa_no_store2assign())
    return -1;
  IROperand d = tcc_ir_op_get_dest(ir, q);
  int32_t dvr = irop_get_vreg(d);
  if (dvr < 0 || TCCIR_DECODE_VREG_TYPE(dvr) != TCCIR_VREG_TYPE_VAR)
    return -1;
  if (!d.is_local || d.btype != IROP_BTYPE_INT32)
    return -1;
  int pos = TCCIR_DECODE_VREG_POSITION(dvr);
  if (pos >= num_vars || !store_def_ok[pos])
    return -1;
  IROperand s = tcc_ir_op_get_src1(ir, q);
  int src_is_value = (irop_get_tag(s) == IROP_TAG_VREG && !s.is_lval) ||
                     irop_get_tag(s) == IROP_TAG_IMM32;
  return src_is_value ? pos : -1;
}

/* LEA/ASM_INPUT/ASM_OUTPUT all prevent SSA promotion of the referenced VAR.
 * ASM: the codegen stores SValues with the original vreg at IR emission time;
 * SSA rename would split those into different temps, leaving stale SValues. */
static int ssa_mark_addrtaken(TCCIRState *ir, IRQuadCompact *q, SSABitset *addrtaken, int num_vars)
{
  int32_t vr = -1;
  if (q->op == TCCIR_OP_LEA || q->op == TCCIR_OP_ASM_INPUT) {
    IROperand src1 = tcc_ir_op_get_src1(ir, q);
    vr = irop_get_vreg(src1);
  } else if (q->op == TCCIR_OP_ASM_OUTPUT) {
    IROperand dest = tcc_ir_op_get_dest(ir, q);
    vr = irop_get_vreg(dest);
  } else {
    return 0;
  }
  if (vr >= 0 && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_VAR) {
    int pos = TCCIR_DECODE_VREG_POSITION(vr);
    if (pos < num_vars)
      SSABitset_set(addrtaken, pos);
  }
  return 1;
}

/* IJUMP: CFG cannot represent computed-goto edges, phi placement incomplete.
 * SETJMP: longjmp restores registers to setjmp-time values, losing
 * modifications made between setjmp and longjmp if locals are in regs. */
static int ssa_has_unsupported_ops(TCCIRState *ir)
{
  for (int i = 0; i < ir->next_instruction_index; i++) {
    TccIrOp op = ir->compact_instructions[i].op;
    if (op == TCCIR_OP_IJUMP || op == TCCIR_OP_SETJMP || op == TCCIR_OP_NL_SETJMP)
      return 1;
  }
  return 0;
}

static int ssa_scan_var_defs(TCCIRState *ir, IRCFG *cfg, SSAVarInfo *info)
{
  int n = ir->next_instruction_index;
  int num_vars = info->num_vars;
  int bitset_bytes = info->block_bitset_bytes;
  dynamic_bitset(SSABitset) has_def = {0};

  if (SSABitset_init(&has_def, num_vars) != 0)
    return -1;

  for (int i = 0; i < n; i++) {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;

    if (ssa_mark_addrtaken(ir, q, info->addrtaken, num_vars))
      continue;

    if (!irop_config[q->op].has_dest)
      continue;
    if (q->op == TCCIR_OP_STORE_INDEXED || q->op == TCCIR_OP_STORE_POSTINC ||
        q->op == TCCIR_OP_FUNCPARAMVAL || q->op == TCCIR_OP_FUNCPARAMVOID)
      continue;
    /* A covering slot STORE is renamed to a fresh name (see
     * ssa_store_slot_def_pos); every other STORE updates the current name in
     * place and must not place a phi. */
    if (q->op == TCCIR_OP_STORE &&
        ssa_store_slot_def_pos(ir, q, info->store_def_ok, num_vars) < 0)
      continue;

    IROperand dest = tcc_ir_op_get_dest(ir, q);
    int32_t vr = irop_get_vreg(dest);
    if (vr < 0 || TCCIR_DECODE_VREG_TYPE(vr) != TCCIR_VREG_TYPE_VAR)
      continue;

    int pos = TCCIR_DECODE_VREG_POSITION(vr);
    if (pos >= num_vars)
      continue;

    int blk = cfg->instr_to_block[i];
    uint8_t *def_bits = &info->def_blocks[pos * bitset_bytes];
    if (SSABitset_test(&has_def, pos)) {
      if (!raw_bitset_test(def_bits, blk))
        SSABitset_set(info->multi_block_def, pos);
    }
    SSABitset_set(&has_def, pos);
    raw_bitset_set(def_bits, blk);
    if (q->op != TCCIR_OP_STORE)
      SSABitset_set(info->nonstore_def, pos);
    if (dest.btype != IROP_BTYPE_INT32)
      info->var_btype[pos] = dest.btype;
  }

  for (int v = 0; v < num_vars; v++) {
    if (SSABitset_test(info->addrtaken, v))
      continue;
    /* A volatile VAR must not be promoted to a TEMP: promotion turns its
     * memory loads/stores into value copies, eliding the mandated accesses.
     * The addrtaken bitset gates promotion, so barring it here keeps a
     * volatile VAR memory-resident. */
    if (v < ir->variables_live_intervals_size &&
        (ir->variables_live_intervals[v].addrtaken ||
         ir->variables_live_intervals[v].is_volatile))
      SSABitset_set(info->addrtaken, v);
  }

  return 0;
}

/* Briggs semi-pruned SSA: classify each VAR as "global" (upward-exposed —
 * some read may observe a value defined in another block) or block-local
 * (every read is preceded, in its own block, by a definition that fully
 * covers it).  A block-local var can never carry a value across a CFG edge,
 * so every phi placed for it is dead by construction.  Such dead phis are far
 * from free: SSA destruction materializes them as pass-through copies in
 * every predecessor, and a loop-header + join pair turns into a loop-carried
 * web the allocator must keep live everywhere (an inlined helper's scratch
 * var per switch case each got such a pair — six webs threaded through every
 * case as copies = spill storm).  Block-local vars are also always safely
 * renameable, including the single-block-def-in-multi-block-CFG shape
 * ssa_var_promotable used to reject as "no phi needed => keep as VAR"
 * (post-loop inlined-helper scratch vars stayed memory-resident RMW).
 *
 * The "global" bit is a whole-function verdict, so it still admits dead phis
 * for a var that is upward-exposed in one block yet dead at the join a phi
 * lands on.  ssa_compute_live_in below prunes those per block; this scan stays
 * as the cheap early-out that skips phi placement for a var entirely.
 *
 * Def/read semantics mirror tcc_ir_ssa_rename exactly:
 *  - a non-STORE-family dest is a fresh full definition (kill);
 *  - a var-SLOT STORE (is_local) kills only when its width covers every READ
 *    width seen for the var (a narrow store leaves prior bytes observable to a
 *    wider read).  Whether such a covering store then goes on to take a FRESH
 *    name or updates the current one in place (ssa_store_slot_def_pos) does not
 *    change the kill, so this scan runs before — and feeds — the def scan;
 *  - every src1/src2/accum var operand is a read, as is a STORE-family dest
 *    that dereferences the var's value (!is_local).
 * Any access outside a CFG block or with STRUCT/FUNC btype marks the var
 * global (conservative: phis placed exactly as before). */

static int ssa_btype_read_bytes(int btype)
{
  switch (btype) {
  case IROP_BTYPE_INT8: return 1;
  case IROP_BTYPE_INT16: return 2;
  case IROP_BTYPE_INT32: case IROP_BTYPE_FLOAT32: return 4;
  case IROP_BTYPE_INT64: case IROP_BTYPE_FLOAT64: return 8;
  default: return 0; /* STRUCT/FUNC: force-global sentinel */
  }
}

typedef struct {
  SSABitset *global;
  uint8_t *max_read; /* per-var: widest read in bytes */
  int *killed;       /* per-var: block id + 1 of a covering def, 0 = none */
  int num_vars;
} SSAGlobalScan;

static int ssa_global_var_pos(int32_t vr, int num_vars)
{
  if (vr < 0 || TCCIR_DECODE_VREG_TYPE(vr) != TCCIR_VREG_TYPE_VAR)
    return -1;
  int pos = TCCIR_DECODE_VREG_POSITION(vr);
  return pos < num_vars ? pos : -1;
}

static void ssa_global_read_width(SSAGlobalScan *gs, IROperand op)
{
  int pos = ssa_global_var_pos(irop_get_vreg(op), gs->num_vars);
  if (pos < 0)
    return;
  int bytes = ssa_btype_read_bytes(op.btype);
  if (bytes == 0) {
    SSABitset_set(gs->global, pos);
    gs->max_read[pos] = 0xFF; /* non-scalar access: sticky (0xFF > any width) */
  } else if (bytes > gs->max_read[pos])
    gs->max_read[pos] = (uint8_t)bytes;
}

static void ssa_global_mark_read(SSAGlobalScan *gs, int blk, IROperand op)
{
  int pos = ssa_global_var_pos(irop_get_vreg(op), gs->num_vars);
  if (pos < 0)
    return;
  if (blk < 0 || gs->killed[pos] != blk + 1)
    SSABitset_set(gs->global, pos);
}

static void ssa_scan_var_global(TCCIRState *ir, IRCFG *cfg, SSAVarInfo *info,
                                uint8_t *max_read_out)
{
  int n = ir->next_instruction_index;
  int num_vars = info->num_vars;
  SSAGlobalScan gs = {
    .global = info->global,
    .max_read = max_read_out,
    .killed = tcc_mallocz((size_t)num_vars * sizeof(int)),
    .num_vars = num_vars,
  };

  /* Pass 1: widest read per var (so partial stores never count as kills). */
  for (int i = 0; i < n; i++) {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;
    int is_store = (q->op == TCCIR_OP_STORE || q->op == TCCIR_OP_STORE_INDEXED ||
                    q->op == TCCIR_OP_STORE_POSTINC);
    if (irop_config[q->op].has_src1)
      ssa_global_read_width(&gs, tcc_ir_op_get_src1(ir, q));
    if (irop_config[q->op].has_src2)
      ssa_global_read_width(&gs, tcc_ir_op_get_src2(ir, q));
    if (q->op == TCCIR_OP_MLA)
      ssa_global_read_width(&gs, tcc_ir_op_get_accum(ir, q));
    if (is_store) {
      IROperand d = tcc_ir_op_get_dest(ir, q);
      if (q->op != TCCIR_OP_STORE || !d.is_local) {
        ssa_global_read_width(&gs, d); /* deref/postinc base: reads the var's value */
      } else if (ssa_btype_read_bytes(d.btype) == 0) {
        int pos = ssa_global_var_pos(irop_get_vreg(d), num_vars);
        if (pos >= 0) {
          SSABitset_set(gs.global, pos); /* STRUCT-width slot store: force-global */
          gs.max_read[pos] = 0xFF;
        }
      }
    }
  }

  /* Pass 2: upward exposure, block-ordered (instructions of a block are the
   * contiguous range [start_idx, end_idx), so linear order suffices). */
  for (int i = 0; i < n; i++) {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;
    int blk = cfg->instr_to_block ? cfg->instr_to_block[i] : -1;
    int is_store = (q->op == TCCIR_OP_STORE || q->op == TCCIR_OP_STORE_INDEXED ||
                    q->op == TCCIR_OP_STORE_POSTINC);

    if (irop_config[q->op].has_src1)
      ssa_global_mark_read(&gs, blk, tcc_ir_op_get_src1(ir, q));
    if (irop_config[q->op].has_src2)
      ssa_global_mark_read(&gs, blk, tcc_ir_op_get_src2(ir, q));
    if (q->op == TCCIR_OP_MLA)
      ssa_global_mark_read(&gs, blk, tcc_ir_op_get_accum(ir, q));

    if (is_store) {
      IROperand d = tcc_ir_op_get_dest(ir, q);
      int pos = ssa_global_var_pos(irop_get_vreg(d), num_vars);
      if (pos < 0)
        continue;
      if (q->op != TCCIR_OP_STORE || !d.is_local) {
        /* Deref through the var's value / postinc pointer update: a read. */
        if (blk < 0 || gs.killed[pos] != blk + 1)
          SSABitset_set(gs.global, pos);
        continue;
      }
      /* Var-slot store: kills only if it covers every read of the var
       * (0xFF non-scalar sentinel in max_read blocks this). */
      int bytes = ssa_btype_read_bytes(d.btype);
      if (blk >= 0 && bytes != 0 && bytes >= gs.max_read[pos])
        gs.killed[pos] = blk + 1;
      continue;
    }

    if (!irop_config[q->op].has_dest || q->op == TCCIR_OP_FUNCPARAMVAL ||
        q->op == TCCIR_OP_FUNCPARAMVOID)
      continue;
    {
      IROperand d = tcc_ir_op_get_dest(ir, q);
      int pos = ssa_global_var_pos(irop_get_vreg(d), num_vars);
      if (pos >= 0 && blk >= 0)
        gs.killed[pos] = blk + 1; /* fresh full definition */
    }
  }

  tcc_free(gs.killed);
}

/* ============================================================================
 * Pruned SSA: per-block live-in over local VARs
 * ==========================================================================*/

/* Briggs' "global" bit above says a phi is needed *somewhere*; it says nothing
 * about *where*, so placement falls back to the whole iterated dominance
 * frontier of the var's defs.  For a block-scoped
 * `{ unsigned xx = x, yy = y; ... }` inside a loop that is far too much: the
 * reads sit in the block after the init, so the var is upward-exposed and
 * counts as global, and it collects a loop-header + latch phi pair whose only
 * readers are each other.  SSA destruction materializes that dead web as a
 * pass-through copy on every edge into the latch, and dce_dead_phi_cycles
 * cannot clear it (each phi's dest is "read" by the other).  gcc-torture
 * arith-rand-ll has 46 such webs: 845 of main's 1273 instructions were
 * slot-to-slot phi copies.
 *
 * Full pruning drops them at the source -- place a phi only where the var is
 * live-in.  The def/use notions must mirror tcc_ir_ssa_rename exactly:
 *  - USE is every operand the renamer resolves through the name stack, which
 *    includes a STORE dest that is *not* a covering slot def (the renamer
 *    rewrites such a store in place, so the incoming value is read);
 *  - KILL is exactly info->def_blocks, the writes that take a fresh name.  An
 *    in-place write is therefore not a kill and keeps both the value and the
 *    phi alive.
 * Over-approximating USE or under-approximating KILL only places extra phis,
 * so both sets err towards the previous behaviour. */
static void ssa_live_mark_use(uint8_t *ue_b, const uint8_t *kill_b, IROperand op,
                              int num_vars)
{
  int pos = ssa_global_var_pos(irop_get_vreg(op), num_vars);
  if (pos < 0 || raw_bitset_test(kill_b, pos))
    return;
  raw_bitset_set(ue_b, pos);
}

/* Returns an nb x var_bytes block-major bitmap of live-in vars, or NULL when
 * liveness is unavailable (callers then keep semi-pruned placement). */
static uint8_t *ssa_compute_live_in(TCCIRState *ir, IRCFG *cfg,
                                    const SSAVarInfo *info, int var_bytes)
{
  int nb = cfg->num_blocks;
  int num_vars = info->num_vars;

  if (!cfg->instr_to_block || nb <= 0 || var_bytes <= 0)
    return NULL;

  size_t sz = (size_t)nb * var_bytes;
  uint8_t *ue = tcc_mallocz(sz);
  uint8_t *kill = tcc_mallocz(sz);
  uint8_t *live = tcc_mallocz(sz);

  int n = ir->next_instruction_index;
  for (int i = 0; i < n; i++) {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;
    int blk = cfg->instr_to_block[i];
    if (blk < 0 || blk >= nb)
      continue;
    uint8_t *ue_b = &ue[(size_t)blk * var_bytes];
    uint8_t *kill_b = &kill[(size_t)blk * var_bytes];

    if (irop_config[q->op].has_src1)
      ssa_live_mark_use(ue_b, kill_b, tcc_ir_op_get_src1(ir, q), num_vars);
    if (irop_config[q->op].has_src2)
      ssa_live_mark_use(ue_b, kill_b, tcc_ir_op_get_src2(ir, q), num_vars);
    if (q->op == TCCIR_OP_MLA)
      ssa_live_mark_use(ue_b, kill_b, tcc_ir_op_get_accum(ir, q), num_vars);

    if (!irop_config[q->op].has_dest)
      continue;
    /* Mirrors the skips in ssa_scan_var_defs: these never define a fresh
     * name, and LEA/ASM_* mark the var addrtaken (so unpromotable) anyway. */
    if (q->op == TCCIR_OP_FUNCPARAMVAL || q->op == TCCIR_OP_FUNCPARAMVOID ||
        q->op == TCCIR_OP_LEA || q->op == TCCIR_OP_ASM_INPUT ||
        q->op == TCCIR_OP_ASM_OUTPUT)
      continue;

    IROperand d = tcc_ir_op_get_dest(ir, q);
    if (q->op == TCCIR_OP_STORE_INDEXED || q->op == TCCIR_OP_STORE_POSTINC ||
        (q->op == TCCIR_OP_STORE &&
         ssa_store_slot_def_pos(ir, q, info->store_def_ok, num_vars) < 0)) {
      ssa_live_mark_use(ue_b, kill_b, d, num_vars);
      continue;
    }
    int pos = ssa_global_var_pos(irop_get_vreg(d), num_vars);
    if (pos >= 0)
      raw_bitset_set(kill_b, pos);
  }

  memcpy(live, ue, sz);
  tcc_free(ue);

  /* Backward dataflow: live_in(b) = ue(b) | (U live_in(succ) & ~kill(b)).
   * Blocks are visited in reverse RPO so a loop settles in ~2 sweeps; when the
   * RPO does not cover every block (unreachable code) fall back to reverse
   * index order, which is program order here. */
  const int *order = (cfg->rpo_order && cfg->rpo_count == nb) ? cfg->rpo_order : NULL;
  int changed = 1;
  while (changed) {
    changed = 0;
    for (int oi = nb - 1; oi >= 0; oi--) {
      int b = order ? order[oi] : oi;
      if (b < 0 || b >= nb)
        continue;
      IRBasicBlock *bb = &cfg->blocks[b];
      uint8_t *live_b = &live[(size_t)b * var_bytes];
      const uint8_t *kill_b = &kill[(size_t)b * var_bytes];
      for (int si = 0; si < bb->num_succs; si++) {
        int s = bb->succs[si];
        if (s < 0 || s >= nb || s == b)
          continue;
        const uint8_t *live_s = &live[(size_t)s * var_bytes];
        for (int k = 0; k < var_bytes; k++) {
          uint8_t nv = (uint8_t)(live_b[k] | (live_s[k] & ~kill_b[k]));
          if (nv != live_b[k]) {
            live_b[k] = nv;
            changed = 1;
          }
        }
      }
    }
  }

  tcc_free(kill);
  return live;
}

/* Decide whether a local VAR should be promoted to SSA (and get phi nodes).
 *
 * Single-block CFG: no back-edges, so any non-addrtaken VAR is safely
 * promotable to a TEMP via straight-line renaming — no phi placement needed.
 * Enabling this lets GVN / cprop / DCE see local-variable defs in leaf
 * functions.
 *
 * Multi-block CFG: a VAR defined in >=2 blocks (multi_block_def) needs phis and
 * is promoted.  "Defined" includes a covering slot STORE, which the renamer
 * gives a fresh name (ssa_store_slot_def_pos) — the u3/u4 shape in
 * 304_fuzz_add_reassoc_addrtaken_alias, where the frontend writes the var in
 * STORE form and SSA used to see one multi-def temp that cprop/SCCP could not
 * thread anything through.  A VAR defined in only ONE block ALSO needs a phi
 * when that def does not dominate all later uses — i.e. its def-block has a
 * non-empty dominance frontier.  The classic case is a value defined only inside a loop
 * and read again on the next iteration through the back-edge (the loop header
 * is in the def-block's DF): without a phi it stays an unpromoted VAR with no
 * loop-header definition, and the register allocator can hand it a register
 * that is clobbered around the loop body (gcc-torture pr125291).  A value
 * defined on one arm of a branch and read after the merge is the same shape.
 * Promoting it is always safe: the phi resolver drops undef (vreg<0) operands,
 * so a path that leaves the var genuinely uninitialized is unchanged. */
static int ssa_var_promotable(const SSAVarInfo *info, IRCFG *cfg, int nb, int v,
                              int single_block)
{
  if (SSABitset_test(info->addrtaken, v))
    return 0;
  /* Block-local (non-global) var: no value ever crosses a CFG edge, so it is
   * renameable with no phis at all — including the single-block-def shape
   * rejected below, which otherwise stays a memory-resident VAR. */
  if (!SSABitset_test(info->global, v))
    return 1;
  if (single_block)
    return 1;
  /* An upward-exposed var whose ONLY defs are slot STOREs stays memory-
   * resident.  Its STORE defs now carry fresh SSA names (ssa_store_slot_def_pos)
   * so promoting it would be *correct*, but it buys nothing: with no other def
   * there is no value for cprop/SCCP to thread, while the phi web SSA
   * destruction materializes costs a copy in every predecessor (gcc-torture
   * loop-15 +12 insns / +74 cycles, bug_switch_in_loop +122 cycles).  Vars that
   * mix ASSIGN and STORE defs — the shape this whole path exists for — keep
   * being promoted. */
  if (!SSABitset_test(info->nonstore_def, v))
    return 0;
  if (SSABitset_test(info->multi_block_def, v))
    return 1;
  /* Single-block-def: promote iff a phi would actually be placed, i.e. some
   * def-block has a non-empty dominance frontier. */
  const uint8_t *def_bits = &info->def_blocks[v * info->block_bitset_bytes];
  for (int b = 0; b < nb; b++) {
    if (raw_bitset_test(def_bits, b) && cfg->blocks[b].num_df > 0)
      return 1;
  }
  return 0;
}

static uint8_t *ssa_build_promotable(const SSAVarInfo *info, IRCFG *cfg, int nb,
                                     int *out_count)
{
  int num_vars = info->num_vars;
  int single_block = (nb <= 1);
  int count = 0;
  for (int v = 0; v < num_vars; v++) {
    if (ssa_var_promotable(info, cfg, nb, v, single_block))
      count++;
  }
  *out_count = count;
  if (count == 0)
    return NULL;

  uint8_t *is_promotable = tcc_mallocz((num_vars + 7) / 8);
  for (int v = 0; v < num_vars; v++) {
    if (ssa_var_promotable(info, cfg, nb, v, single_block))
      raw_bitset_set(is_promotable, v);
  }
  return is_promotable;
}

static int ssa_place_phis_for_var(IRSSAState *ssa, TCCIRState *ir, IRCFG *cfg, int v, int var_btype,
                                  uint8_t *def_bits, SSABitset *has_phi,
                                  SSABitset *in_worklist, int *worklist,
                                  int phi_counter,
                                  const uint8_t *live_in, int var_bytes)
{
  int nb = cfg->num_blocks;
  int wl_count = 0;
  SSABitset_clear(has_phi);
  SSABitset_clear(in_worklist);

  for (int b = 0; b < nb; b++) {
    if (raw_bitset_test(def_bits, b)) {
      worklist[wl_count++] = b;
      SSABitset_set(in_worklist, b);
    }
  }

  int32_t orig_vreg = TCCIR_ENCODE_VREG(TCCIR_VREG_TYPE_VAR, v);

  for (int wi = 0; wi < wl_count; wi++) {
    int b = worklist[wi];
    IRBasicBlock *bb = &cfg->blocks[b];
    for (int di = 0; di < bb->num_df; di++) {
      int df = bb->dom_frontier[di];
      if (SSABitset_test(has_phi, df))
        continue;
      /* Pruned SSA: no phi where the var is dead.  Such a block is not a def
       * either, so it must not seed further frontier expansion -- every use
       * reachable from it is preceded by a real def whose block is already on
       * the worklist. */
      if (live_in && !raw_bitset_test(&live_in[(size_t)df * var_bytes], v))
        continue;
      SSABitset_set(has_phi, df);

      int num_preds = cfg->blocks[df].num_preds;
      int32_t phi_dest = TCCIR_ENCODE_VREG(TCCIR_VREG_TYPE_TEMP,
                                            ir->next_temporary_variable + phi_counter);
      phi_counter++;

      IRPhiNode *phi = ssa_alloc_phi(orig_vreg, phi_dest, num_preds, var_btype);
      for (int pi = 0; pi < num_preds; pi++)
        phi->operands[pi].pred_block = cfg->blocks[df].preds[pi];
      phi->next = ssa->block_phis[df];
      ssa->block_phis[df] = phi;

      if (!SSABitset_test(in_worklist, df)) {
        SSABitset_set(in_worklist, df);
        worklist[wl_count++] = df;
      }
    }
  }

  return phi_counter;
}

IRSSAState *tcc_ir_ssa_construct(TCCIRState *ir, IRCFG *cfg)
{
  if (!ir || !cfg || cfg->num_blocks == 0)
    return NULL;

  int nb = cfg->num_blocks;
  int num_vars = ir->next_local_variable;

  if (num_vars == 0 || nb == 0)
    return NULL;

  if (ssa_has_unsupported_ops(ir))
    return NULL;

  int bitset_bytes = (nb + 7) / 8;
  scoped_vector(uint8_t) def_blocks_owner = {0};
  scoped_vector(int) var_btype_owner = {0};
  dynamic_bitset(SSABitset) addrtaken = {0};
  dynamic_bitset(SSABitset) multi_block_def = {0};
  dynamic_bitset(SSABitset) global = {0};
  dynamic_bitset(SSABitset) nonstore_def = {0};

  if (vector_resize(&def_blocks_owner, (size_t)num_vars * bitset_bytes) != 0 ||
      vector_resize(&var_btype_owner, num_vars) != 0 ||
      SSABitset_init(&addrtaken, num_vars) != 0 ||
      SSABitset_init(&multi_block_def, num_vars) != 0 ||
      SSABitset_init(&global, num_vars) != 0 ||
      SSABitset_init(&nonstore_def, num_vars) != 0)
    return NULL;

  SSAVarInfo info = {
    .def_blocks = vector_data(&def_blocks_owner),
    .addrtaken = &addrtaken,
    .multi_block_def = &multi_block_def,
    .global = &global,
    .nonstore_def = &nonstore_def,
    .var_btype = vector_data(&var_btype_owner),
    .block_bitset_bytes = bitset_bytes,
    .num_vars = num_vars,
  };
  /* Read widths first: whether a slot STORE counts as a fresh definition is a
   * per-var property the def scan below needs, and the upward-exposure scan
   * does not depend on def_blocks.  Fold the decision into one byte per var —
   * a 4-byte write must cover every read of the var (the 0xFF non-scalar
   * sentinel and 64-bit reads fail this). */
  uint8_t *var_max_read = tcc_mallocz(num_vars);
  ssa_scan_var_global(ir, cfg, &info, var_max_read);
  for (int v = 0; v < num_vars; v++)
    var_max_read[v] = (var_max_read[v] <= 4) ? 1 : 0;
  info.store_def_ok = var_max_read;

  if (ssa_scan_var_defs(ir, cfg, &info) != 0) {
    tcc_free(var_max_read);
    return NULL;
  }

  int promotable_count;
  uint8_t *is_promotable = ssa_build_promotable(&info, cfg, nb, &promotable_count);
  if (!is_promotable) {
    tcc_free(var_max_read);
    return NULL;
  }

  IRSSAState *ssa = tcc_mallocz(sizeof(IRSSAState));
  ssa->cfg = cfg;
  ssa->block_phis = tcc_mallocz(nb * sizeof(IRPhiNode *));
  ssa->next_ssa_vreg = TCCIR_ENCODE_VREG(TCCIR_VREG_TYPE_TEMP, ir->next_temporary_variable);
  ssa->is_promotable = is_promotable;
  ssa->var_store_def_ok = var_max_read;
  ssa->num_vars = num_vars;

  dynamic_bitset(SSABitset) has_phi = {0};
  dynamic_bitset(SSABitset) in_worklist = {0};
  scoped_vector(int) worklist_owner = {0};
  if (SSABitset_init(&has_phi, nb) != 0 ||
      SSABitset_init(&in_worklist, nb) != 0 ||
      vector_resize(&worklist_owner, nb) != 0) {
    tcc_ir_ssa_free(ssa);
    return NULL;
  }
  int *worklist = vector_data(&worklist_owner);
  int phi_counter = 0;
  int var_bytes = (num_vars + 7) / 8;
  uint8_t *live_in = ssa_compute_live_in(ir, cfg, &info, var_bytes);

  for (int v = 0; v < num_vars; v++) {
    /* Place phis for every promoted var (is_promotable already excludes
     * addrtaken). For single-block-def vars this now also covers the ones kept
     * as VARs before — loop-carried / branch-merge-live values that need a phi.
     * In a single-block CFG the def-block has an empty DF, so this places none. */
    if (!raw_bitset_test(is_promotable, v))
      continue;
    /* Semi-pruned: a block-local var never carries a value across an edge —
     * every phi for it would be dead, so place none. */
    if (!SSABitset_test(&global, v))
      continue;
    uint8_t *def_bits = &info.def_blocks[v * bitset_bytes];
    phi_counter = ssa_place_phis_for_var(ssa, ir, cfg, v, info.var_btype[v], def_bits,
                                         &has_phi, &in_worklist, worklist,
                                         phi_counter, live_in, var_bytes);
  }

  tcc_free(live_in);

  ssa->next_ssa_vreg = TCCIR_ENCODE_VREG(TCCIR_VREG_TYPE_TEMP,
                                          ir->next_temporary_variable + phi_counter);

  return ssa;
}

/* ============================================================================
 * SSA Renaming
 * ============================================================================ */

typedef vector(int32_t) VRegStack;

static void vstack_push(VRegStack *s, int32_t v)
{
  vector_push_back(s, v);
}

static int32_t vstack_top(VRegStack *s)
{
  return s->size > 0 ? s->data[s->size - 1] : -1;
}

static int ssa_rename_use(IROperand *op, int num_vars, const uint8_t *is_promotable,
                          VRegStack *stacks)
{
  int32_t vr = irop_get_vreg(*op);
  if (vr < 0 || TCCIR_DECODE_VREG_TYPE(vr) != TCCIR_VREG_TYPE_VAR)
    return 0;
  int pos = TCCIR_DECODE_VREG_POSITION(vr);
  if (pos >= num_vars || !raw_bitset_test(is_promotable, pos))
    return 0;
  int32_t cur = vstack_top(&stacks[pos]);
  if (cur < 0)
    return 0;
  /* A use that dereferences the variable's *value* (is_lval set, is_local
   * clear) is a pointer dereference — e.g. `*vv` / `vv->m` after the address
   * fold collapsed `&vv->m` (offset 0) to vv itself, leaving the var operand
   * as the pointer being stored/loaded through.  Promoting vv to an SSA
   * register must KEEP the dereference: the pointer now lives in `cur`, so
   * `*cur` still loads/stores through it.  Only a var-SLOT access (is_local)
   * collapses to a plain register value.  Without this, `(vv=call())->m0=c`
   * lowered `*vv=c` to `vv=c`, dropping the store and clobbering the pointer. */
  int deref_through_value = op->is_lval && !op->is_local;
  irop_set_vreg(op, cur);
  op->tag = IROP_TAG_VREG;
  if (!deref_through_value) {
    op->is_lval = 0;
    op->is_local = 0;
  }
  op->u.imm32 = 0;
  return deref_through_value ? 2 : 1;
}

static void ssa_rename_phi_defs(IRSSAState *ssa, int b, VRegStack *stacks, int num_vars)
{
  for (IRPhiNode *phi = ssa->block_phis[b]; phi; phi = phi->next) {
    int32_t orig = phi->orig_vreg;
    if (TCCIR_DECODE_VREG_TYPE(orig) != TCCIR_VREG_TYPE_VAR)
      continue;
    int pos = TCCIR_DECODE_VREG_POSITION(orig);
    if (pos < num_vars && raw_bitset_test(ssa->is_promotable, pos))
      vstack_push(&stacks[pos], phi->dest_vreg);
  }
}

static void ssa_rename_block_instrs(TCCIRState *ir, IRSSAState *ssa, IRBasicBlock *bb,
                                    VRegStack *stacks, int num_vars, int *next_temp_pos)
{
  for (int i = bb->start_idx; i < bb->end_idx; i++) {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;

    if (irop_config[q->op].has_src1) {
      IROperand s = tcc_ir_op_get_src1(ir, q);
      int r = ssa_rename_use(&s, num_vars, ssa->is_promotable, stacks);
      if (r) {
        tcc_ir_op_set_src1(ir, q, s);
        /* A var-slot LOAD becomes a register copy (ASSIGN); a LOAD that
         * dereferences the var's pointer value (r==2) stays a real LOAD. */
        if (r == 1 && q->op == TCCIR_OP_LOAD)
          q->op = TCCIR_OP_ASSIGN;
      }
    }

    if (irop_config[q->op].has_src2) {
      IROperand s = tcc_ir_op_get_src2(ir, q);
      if (ssa_rename_use(&s, num_vars, ssa->is_promotable, stacks))
        tcc_ir_op_set_src2(ir, q, s);
    }

    if (q->op == TCCIR_OP_MLA) {
      IROperand s = tcc_ir_op_get_accum(ir, q);
      if (ssa_rename_use(&s, num_vars, ssa->is_promotable, stacks))
        tcc_ir_op_set_accum(ir, q, s);
    }

    /* A plain STORE to a promoted var's SLOT (is_local) that covers every
     * read of the var is just an assignment: convert it to ASSIGN (same
     * {dest,src1} layout) so the generic dest handling below gives it a
     * FRESH SSA name.  The in-place "rename dest to the current name" path
     * below exists only for writes that may be partial (narrow stores,
     * 64-bit pairs) — routing full-width INT32 writes through it created
     * multi-def temps invisible to const/copy propagation and to the
     * allocator's def scan. */
    if (q->op == TCCIR_OP_STORE) {
      int pos = ssa_store_slot_def_pos(ir, q, ssa->var_store_def_ok, num_vars);
      if (pos >= 0 && raw_bitset_test(ssa->is_promotable, pos))
        q->op = TCCIR_OP_ASSIGN; /* falls through to fresh-def dest handling */
    }

    if (q->op == TCCIR_OP_STORE || q->op == TCCIR_OP_STORE_INDEXED ||
        q->op == TCCIR_OP_STORE_POSTINC) {
      IROperand d = tcc_ir_op_get_dest(ir, q);
      if (ssa_rename_use(&d, num_vars, ssa->is_promotable, stacks))
        tcc_ir_op_set_dest(ir, q, d);
      continue;
    }

    if (irop_config[q->op].has_dest &&
        q->op != TCCIR_OP_FUNCPARAMVAL && q->op != TCCIR_OP_FUNCPARAMVOID) {
      IROperand d = tcc_ir_op_get_dest(ir, q);
      int32_t vr = irop_get_vreg(d);
      if (vr >= 0 && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_VAR) {
        int pos = TCCIR_DECODE_VREG_POSITION(vr);
        if (pos < num_vars && raw_bitset_test(ssa->is_promotable, pos)) {
          int32_t new_name = TCCIR_ENCODE_VREG(TCCIR_VREG_TYPE_TEMP, (*next_temp_pos)++);
          vstack_push(&stacks[pos], new_name);
          irop_set_vreg(&d, new_name);
          d.tag = IROP_TAG_VREG;
          d.is_lval = 0;
          d.is_local = 0;
          d.u.imm32 = 0;
          tcc_ir_op_set_dest(ir, q, d);
        }
      }
    }
  }
}

static void ssa_fill_successor_phis(IRSSAState *ssa, IRCFG *cfg, int b,
                                    VRegStack *stacks, int num_vars)
{
  IRBasicBlock *bb = &cfg->blocks[b];
  for (int si = 0; si < bb->num_succs; si++) {
    int succ = bb->succs[si];
    if (succ < 0)
      continue;
    IRBasicBlock *sbb = &cfg->blocks[succ];
    int pred_idx = -1;
    for (int pi = 0; pi < sbb->num_preds; pi++) {
      if (sbb->preds[pi] == b) { pred_idx = pi; break; }
    }
    if (pred_idx < 0)
      continue;
    for (IRPhiNode *phi = ssa->block_phis[succ]; phi; phi = phi->next) {
      int32_t orig = phi->orig_vreg;
      if (TCCIR_DECODE_VREG_TYPE(orig) != TCCIR_VREG_TYPE_VAR)
        continue;
      int pos = TCCIR_DECODE_VREG_POSITION(orig);
      if (pos < num_vars && raw_bitset_test(ssa->is_promotable, pos)) {
        if (pred_idx < phi->num_operands)
          phi->operands[pred_idx].vreg = vstack_top(&stacks[pos]);
      }
    }
  }
}

void tcc_ir_ssa_rename(TCCIRState *ir, IRSSAState *ssa)
{
  if (!ir || !ssa || !ssa->cfg || !ssa->is_promotable)
    return;

  IRCFG *cfg = ssa->cfg;
  int nb = cfg->num_blocks;
  int num_vars = ssa->num_vars;
  int next_temp_pos = TCCIR_DECODE_VREG_POSITION(ssa->next_ssa_vreg);

  scoped_vector(VRegStack) stacks_owner = {0};
  if (vector_resize(&stacks_owner, num_vars) != 0)
    return;
  VRegStack *stacks = vector_data(&stacks_owner);

  for (int v = 0; v < num_vars; v++) {
    if (raw_bitset_test(ssa->is_promotable, v)) {
      int32_t init_name = TCCIR_ENCODE_VREG(TCCIR_VREG_TYPE_TEMP, next_temp_pos++);
      vstack_push(&stacks[v], init_name);
    }
  }

  typedef struct { int block; int child_idx; } DomFrame;
  scoped_vector(DomFrame) dom_stack_owner = {0};
  scoped_vector(int) saved_depths_owner = {0};
  if (vector_resize(&dom_stack_owner, nb) != 0 ||
      vector_resize(&saved_depths_owner, (size_t)nb * num_vars) != 0) {
    for (int v = 0; v < num_vars; v++)
      vector_cleanup(&stacks[v]);
    return;
  }
  DomFrame *dom_stack = vector_data(&dom_stack_owner);
  int *saved_depths = vector_data(&saved_depths_owner);
  int dsp = 0;

  dom_stack[dsp++] = (DomFrame){0, 0};

  while (dsp > 0) {
    DomFrame *top = &dom_stack[dsp - 1];
    int b = top->block;

    if (top->child_idx == 0) {
      int *frame_depths = &saved_depths[(dsp - 1) * num_vars];
      for (int v = 0; v < num_vars; v++)
        frame_depths[v] = stacks[v].size;

      ssa_rename_phi_defs(ssa, b, stacks, num_vars);
      ssa_rename_block_instrs(ir, ssa, &cfg->blocks[b], stacks, num_vars, &next_temp_pos);
      ssa_fill_successor_phis(ssa, cfg, b, stacks, num_vars);
    }

    IRBasicBlock *bb = &cfg->blocks[b];
    if (top->child_idx < bb->num_dom_children) {
      int child = bb->dom_children[top->child_idx];
      top->child_idx++;
      dom_stack[dsp++] = (DomFrame){child, 0};
    }
    else {
      int *frame_depths = &saved_depths[(dsp - 1) * num_vars];
      for (int v = 0; v < num_vars; v++)
        stacks[v].size = frame_depths[v];
      dsp--;
    }
  }

  tcc_ir_vreg_ensure_temp_capacity(ir, next_temp_pos);
  ir->next_temporary_variable = next_temp_pos;

  for (int v = 0; v < num_vars; v++)
    vector_cleanup(&stacks[v]);
}

void tcc_ir_ssa_free(IRSSAState *ssa)
{
  if (!ssa)
    return;
  if (ssa->block_phis && ssa->cfg) {
    for (int b = 0; b < ssa->cfg->num_blocks; b++) {
      IRPhiNode *phi = ssa->block_phis[b];
      while (phi) {
        IRPhiNode *next = phi->next;
        tcc_free(phi->operands);
        tcc_free(phi);
        phi = next;
      }
    }
    tcc_free(ssa->block_phis);
  }
  tcc_free(ssa->is_promotable);
  tcc_free(ssa->var_store_def_ok);
  tcc_free(ssa);
}
