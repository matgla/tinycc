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

/* SSA register allocator: the entry point (tcc_ir_ssa_regalloc) and
 * spill-driven live-range splitting. */

#include "regalloc_priv.h"

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
      if (tcc_ir_op_is_mac(q->op)) uses[nu++] = irop_get_vreg(tcc_ir_op_get_accum(ir, q));
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
    if (tcc_ir_op_is_mac(q->op)) REMAP(tcc_ir_op_get_accum, tcc_ir_op_set_accum);
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
      else { if (!tcc_ir_op_is_mac(q->op)) continue; s = tcc_ir_op_get_accum(ir, q); }
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

static int ra_split_flag_reader(TccIrOp op)
{
  return op == TCCIR_OP_JUMPIF || op == TCCIR_OP_SETIF || op == TCCIR_OP_SELECT || op == TCCIR_OP_ADC_USE ||
         op == TCCIR_OP_SUBC_USE;
}

static int ra_split_flag_setter(TccIrOp op)
{
  return op == TCCIR_OP_CMP || op == TCCIR_OP_TEST_ZERO || op == TCCIR_OP_FCMP || op == TCCIR_OP_ADC_GEN ||
         op == TCCIR_OP_SUBC_GEN;
}

/* A copy lowers to MOV/LDR/STR; a 16-bit MOVS would clobber the flags, so no
 * copy goes where a later flag reader still needs them. */
static int ra_split_flags_live_before(TCCIRState *ir, int i)
{
  int n = ir->next_instruction_index;
  for (int k = i; k < n; k++) {
    IRQuadCompact *q = &ir->compact_instructions[k];
    if (k > i && q->is_jump_target)
      return 0;
    if (ra_split_flag_reader(q->op))
      return 1;
    if (ra_split_flag_setter(q->op))
      return 0;
    if (q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_RETURNVALUE || q->op == TCCIR_OP_RETURNVOID)
      return 0;
  }
  return 0;
}

/* Ops a split value must not appear in: operands beyond dest/src1/src2/accum,
 * operands the op also writes, register pairs, or positions the backend pins. */
static int ra_split_op_excluded(TccIrOp op)
{
  switch (op) {
  case TCCIR_OP_LOAD_POSTINC: case TCCIR_OP_STORE_POSTINC: case TCCIR_OP_INLINE_ASM:
  case TCCIR_OP_ASM_INPUT: case TCCIR_OP_ASM_OUTPUT: case TCCIR_OP_CALLSEQ_BEGIN:
  case TCCIR_OP_CALLARG_REG: case TCCIR_OP_CALLARG_STACK: case TCCIR_OP_CALLSEQ_END:
  case TCCIR_OP_SWITCH_TABLE: case TCCIR_OP_SWITCH_LOAD: case TCCIR_OP_IJUMP:
  case TCCIR_OP_BLOCK_COPY: case TCCIR_OP_VLA_ALLOC: case TCCIR_OP_VLA_SP_SAVE:
  case TCCIR_OP_VLA_SP_RESTORE: case TCCIR_OP_SETJMP: case TCCIR_OP_LONGJMP:
  case TCCIR_OP_NL_SETJMP: case TCCIR_OP_NL_LONGJMP: case TCCIR_OP_BUILTIN_APPLY_ARGS:
  case TCCIR_OP_BUILTIN_APPLY: case TCCIR_OP_BUILTIN_RETURN: case TCCIR_OP_SET_CHAIN:
  case TCCIR_OP_INIT_CHAIN_SLOT: case TCCIR_OP_UMAAL: case TCCIR_OP_UMULL: case TCCIR_OP_SMULL:
  case TCCIR_OP_PACK64: case TCCIR_OP_ZEXT: case TCCIR_OP_BFI: case TCCIR_OP_ADC_USE:
  case TCCIR_OP_ADC_GEN: case TCCIR_OP_SUBC_USE: case TCCIR_OP_SUBC_GEN: case TCCIR_OP_RETURN_ADDRESS:
    return 1;
  default:
    return 0;
  }
}

static int ra_split_is_call(TCCIRState *ir, IRQuadCompact *q)
{
  TccIrOp op = q->op;
  if (op == TCCIR_OP_FUNCCALLVOID || op == TCCIR_OP_FUNCCALLVAL || op == TCCIR_OP_BUILTIN_APPLY ||
      ir_op_is_implicit_call_ra(op))
    return 1;
  if (op == TCCIR_OP_BLOCK_COPY)
    return (int)irop_get_imm64_ex(ir, tcc_ir_op_get_src2(ir, q)) >= TCCIR_BLOCK_COPY_MEMCPY_MIN_BYTES;
  return 0;
}

/* How an operand naming a split candidate reads it, and what the fresh temp's
 * operand becomes.  A promoted VAR is referenced either as a plain vreg or
 * through its spill encoding (STACKOFF, is_local + is_lval = its VALUE); a
 * plain vreg with is_lval dereferences the value.  Returns 0 for shapes a
 * temp cannot stand in for (the var's address, double indirection). */
static int ra_split_use_operand(IROperand s, IROperand *out, int32_t to)
{
  if (s.is_llocal || s.btype == IROP_BTYPE_STRUCT || s.is_complex)
    return 0;
  IROperand t = s;
  if (s.is_local) {
    if (!s.is_lval)
      return 0; /* address of the var's home */
    t.is_local = 0;
    t.is_lval = 0;
  } else if (s.tag != IROP_TAG_VREG)
    return 0;
  t.tag = IROP_TAG_VREG;
  t.is_param = 0;
  t.u.imm32 = 0;
  irop_set_vreg(&t, to);
  if (out)
    *out = t;
  return 1;
}

/* Rename every USE of `from` in q (the slots ra_co_ops reads) to `to`. */
static void ra_split_rename_uses(TCCIRState *ir, IRQuadCompact *q, int32_t from, int32_t to)
{
  IROperand s, t;
  if (irop_config[q->op].has_src1) {
    s = tcc_ir_op_get_src1(ir, q);
    if (irop_has_vreg(s) && !irop_is_immediate(s) && irop_get_vreg(s) == from && ra_split_use_operand(s, &t, to))
      tcc_ir_op_set_src1(ir, q, t);
  }
  if (irop_config[q->op].has_src2) {
    s = tcc_ir_op_get_src2(ir, q);
    if (irop_has_vreg(s) && !irop_is_immediate(s) && irop_get_vreg(s) == from && ra_split_use_operand(s, &t, to))
      tcc_ir_op_set_src2(ir, q, t);
  }
  if (tcc_ir_op_is_mac(q->op)) {
    s = tcc_ir_op_get_accum(ir, q);
    if (irop_has_vreg(s) && !irop_is_immediate(s) && irop_get_vreg(s) == from && ra_split_use_operand(s, &t, to))
      tcc_ir_op_set_accum(ir, q, t);
  }
  if (q->op == TCCIR_OP_STORE || q->op == TCCIR_OP_STORE_INDEXED) {
    s = tcc_ir_op_get_dest(ir, q);
    if (irop_has_vreg(s) && irop_get_vreg(s) == from && ra_split_use_operand(s, &t, to))
      tcc_ir_op_set_dest(ir, q, t);
  }
}

/* Make q, the single-register def of a candidate, write `to` instead.  A
 * STORE-form def (a promoted VAR's spill-encoded store, or a temp's in-place
 * store) becomes the ASSIGN it stands for: same dest/src1 layout. */
static void ra_split_rename_def(TCCIRState *ir, IRQuadCompact *q, int32_t to)
{
  IROperand d = tcc_ir_op_get_dest(ir, q);
  if (q->op == TCCIR_OP_STORE) {
    IROperand t;
    memset(&t, 0, sizeof t);
    t.tag = IROP_TAG_VREG;
    t.btype = IROP_BTYPE_INT32;
    irop_set_vreg(&t, to);
    q->op = TCCIR_OP_ASSIGN;
    tcc_ir_op_set_dest(ir, q, t);
    return;
  }
  irop_set_vreg(&d, to);
  tcc_ir_op_set_dest(ir, q, d);
}

/* Insert the planned copies (sorted by `before`, skip-copies first) into the
 * instruction stream, remapping branch and switch targets. */
static void ra_split_insert(TCCIRState *ir, RaSplitCopy *cp, int ncp)
{
  int old_n = ir->next_instruction_index;
  int new_cap = old_n + ncp + 16;
  IRQuadCompact *ni = tcc_mallocz(new_cap * sizeof(IRQuadCompact));
  int *old_to_new = tcc_malloc((old_n + 1) * sizeof(int));
  while (ir->iroperand_pool_count + 2 * ncp > ir->iroperand_pool_capacity) {
    int nc = ir->iroperand_pool_capacity ? ir->iroperand_pool_capacity * 2 : 256;
    ir->iroperand_pool = tcc_realloc(ir->iroperand_pool, nc * sizeof(IROperand));
    ir->iroperand_pool_capacity = nc;
  }
  int wp = 0, c = 0;
  for (int i = 0; i <= old_n; i++) {
    int landing = -1;
    int line = i < old_n ? ir->compact_instructions[i].line_num : 0;
    for (; c < ncp && cp[c].before == i; c++) {
      IROperand d, s;
      memset(&d, 0, sizeof d);
      memset(&s, 0, sizeof s);
      irop_set_vreg(&d, cp[c].dest);
      d.tag = IROP_TAG_VREG;
      d.btype = cp[c].btype;
      d.is_unsigned = cp[c].is_unsigned;
      irop_set_vreg(&s, cp[c].src);
      s.tag = IROP_TAG_VREG;
      s.btype = cp[c].btype;
      s.is_unsigned = cp[c].is_unsigned;
      int pb = ir->iroperand_pool_count;
      ir->iroperand_pool[pb] = d;
      ir->iroperand_pool[pb + 1] = s;
      ir->iroperand_pool_count += 2;
      if (cp[c].land && landing < 0)
        landing = wp;
      ni[wp].op = TCCIR_OP_ASSIGN;
      ni[wp].operand_base = pb;
      ni[wp].line_num = line;
      ni[wp].orig_index = ++ir->max_orig_index;
      wp++;
    }
    if (i == old_n) {
      old_to_new[i] = landing >= 0 ? landing : wp;
      break;
    }
    ni[wp] = ir->compact_instructions[i];
    old_to_new[i] = landing >= 0 ? landing : wp;
    if (landing >= 0 && ni[wp].is_jump_target) {
      ni[wp].is_jump_target = 0;
      ni[landing].is_jump_target = 1;
    }
    wp++;
  }
  for (int i = 0; i < wp; i++) {
    IRQuadCompact *q = &ni[i];
    if (q->op != TCCIR_OP_JUMP && q->op != TCCIR_OP_JUMPIF)
      continue;
    IROperand dest = ir->iroperand_pool[q->operand_base];
    int t = (int)irop_get_imm64_ex(ir, dest);
    if (t >= 0 && t <= old_n) {
      dest.u.imm32 = old_to_new[t];
      ir->iroperand_pool[q->operand_base] = dest;
    }
  }
  for (int t = 0; t < ir->num_switch_tables; t++) {
    TCCIRSwitchTable *table = &ir->switch_tables[t];
    for (int ti = 0; ti < table->num_entries; ti++)
      if (table->targets[ti] >= 0 && table->targets[ti] <= old_n)
        table->targets[ti] = old_to_new[table->targets[ti]];
    if (table->default_target >= 0 && table->default_target <= old_n)
      table->default_target = old_to_new[table->default_target];
  }
  tcc_free(ir->compact_instructions);
  ir->compact_instructions = ni;
  ir->compact_instructions_size = new_cap;
  ir->next_instruction_index = wp;
  tcc_free(old_to_new);
}

static int ra_split_copy_cmp(const void *a, const void *b)
{
  const RaSplitCopy *x = a, *y = b;
  if (x->before != y->before)
    return x->before < y->before ? -1 : 1;
  if (x->land != y->land)
    return x->land - y->land; /* skip-copies (end of the previous block) first */
  return 0;
}

/* The def of candidate `v` at q, or 0 when q does not define it.  2: a
 * STORE-form def (rewritten to ASSIGN when split).  -1: a def shape the split
 * cannot rename (narrow, address, pair, excluded op). */
static int ra_split_def_kind(TCCIRState *ir, IRQuadCompact *q, int32_t v)
{
  if (!irop_config[q->op].has_dest)
    return 0;
  IROperand d = tcc_ir_op_get_dest(ir, q);
  if (!irop_has_vreg(d) || irop_get_vreg(d) != v)
    return 0;
  int vt = TCCIR_DECODE_VREG_TYPE(v);
  if (q->op == TCCIR_OP_STORE) {
    if (irop_get_btype(d) != IROP_BTYPE_INT32 || d.is_complex)
      return -1;
    /* promoted VAR through its spill encoding: STORE StackLoc(V) = V's def */
    if (vt == TCCIR_VREG_TYPE_VAR && d.tag == IROP_TAG_STACKOFF && d.is_local && d.is_lval && !d.is_llocal)
      return 2;
    /* a temp's in-place def (SSA renaming of a promoted var's slot store) */
    if (vt == TCCIR_VREG_TYPE_TEMP && d.tag == IROP_TAG_VREG && !d.is_lval && !d.is_local && !d.is_llocal)
      return 2;
    return 0; /* a store THROUGH the value: a use, classified by the caller */
  }
  if (q->op == TCCIR_OP_STORE_INDEXED || q->op == TCCIR_OP_STORE_POSTINC)
    return 0;
  int bt = irop_get_btype(d);
  if (ra_split_op_excluded(q->op) || d.tag != IROP_TAG_VREG || d.is_lval || d.is_local || d.is_llocal ||
      d.is_complex || !(bt == IROP_BTYPE_INT32 || bt == IROP_BTYPE_INT8 || bt == IROP_BTYPE_INT16))
    return -1;
  return 1;
}

/* Returns the number of IR edits (0: IR untouched). */
static int ra_split_spilled(TCCIRState *ir, IRCFG *cfg, SSAInterval *intervals, int count, int max_vreg_pos,
                            const int *call_prefix)
{
  int n = ir->next_instruction_index;
  (void)cfg; /* may predate the flat edits after phi resolution; built fresh below */
  if (n <= 0 || !call_prefix)
    return 0;
  if (ir->func_has_label_addr || ir->inline_asm_count > 0 || tcc_ir_calls_returns_twice(ir))
    return 0;

  int tbl = 4 * (max_vreg_pos > 0 ? max_vreg_pos : 1);
  int *cand = tcc_malloc(sizeof(int) * tbl);
  for (int i = 0; i < tbl; i++)
    cand[i] = -1;
  int ncand = 0;
  int32_t *cvreg = tcc_malloc(sizeof(int32_t) * (count > 0 ? count : 1));
#define SPLIT_IDX(vr) (TCCIR_DECODE_VREG_TYPE(vr) * max_vreg_pos + TCCIR_DECODE_VREG_POSITION(vr))
#define SPLIT_CAND(vr) ((vr) >= 0 && SPLIT_IDX(vr) >= 0 && SPLIT_IDX(vr) < tbl ? cand[SPLIT_IDX(vr)] : -1)
  for (int i = 0; i < count; i++) {
    SSAInterval *iv = &intervals[i];
    if (iv->stack_location == 0 || iv->r0 >= 0 || iv->addrtaken || iv->is_volatile || iv->is_param ||
        iv->reg_type != LS_REG_TYPE_INT || iv->precolored >= 0 || iv->coalesce_to >= 0 || iv->co_member)
      continue;
    int t = TCCIR_DECODE_VREG_TYPE(iv->vreg);
    if (t != TCCIR_VREG_TYPE_TEMP && t != TCCIR_VREG_TYPE_VAR)
      continue;
    IRLiveInterval *li = tcc_ir_vreg_live_interval(ir, iv->vreg);
    if (!li || li->addrtaken || li->is_volatile || li->is_lvalue || tcc_ir_vreg_is_ignored(ir, iv->vreg))
      continue;
    int idx = SPLIT_IDX(iv->vreg);
    if (idx < 0 || idx >= tbl)
      continue;
    cand[idx] = ncand;
    cvreg[ncand++] = iv->vreg;
  }
  if (ncand == 0) {
    tcc_free(cand);
    tcc_free(cvreg);
    return 0;
  }
  cfg = tcc_ir_cfg_build(ir);
  if (!cfg || !cfg->instr_to_block || cfg->num_instrs < n) {
    if (cfg)
      tcc_ir_cfg_free(cfg);
    tcc_free(cand);
    tcc_free(cvreg);
    return 0;
  }
  tcc_ir_cfg_compute_dominators(cfg);

  uint8_t *bad = tcc_mallocz(ncand);
  int *nuse = tcc_mallocz(sizeof(int) * (ncand + 1));
  int *ndef = tcc_mallocz(sizeof(int) * (ncand + 1));

  /* Paired CALL of every FUNCPARAMVAL (see ra_build_intervals). */
  int *param_call = tcc_malloc(sizeof(int) * n);
  {
    int max_cid = -1;
    for (int i = 0; i < n; i++) {
      IRQuadCompact *q = &ir->compact_instructions[i];
      if (q->op == TCCIR_OP_FUNCCALLVOID || q->op == TCCIR_OP_FUNCCALLVAL) {
        int ccid = TCCIR_DECODE_CALL_ID(irop_get_imm64_ex(ir, tcc_ir_op_get_src2(ir, q)));
        if (ccid > max_cid)
          max_cid = ccid;
      }
    }
    int *next_call = max_cid >= 0 ? tcc_malloc(sizeof(int) * (max_cid + 1)) : NULL;
    for (int k = 0; k <= max_cid; k++)
      next_call[k] = -1;
    for (int i = n - 1; i >= 0; i--) {
      IRQuadCompact *q = &ir->compact_instructions[i];
      param_call[i] = -1;
      if (q->op == TCCIR_OP_FUNCCALLVOID || q->op == TCCIR_OP_FUNCCALLVAL) {
        int ccid = TCCIR_DECODE_CALL_ID(irop_get_imm64_ex(ir, tcc_ir_op_get_src2(ir, q)));
        if (ccid >= 0 && ccid <= max_cid)
          next_call[ccid] = i;
      } else if (q->op == TCCIR_OP_FUNCPARAMVAL && next_call) {
        int cid = TCCIR_DECODE_CALL_ID(irop_get_imm64_ex(ir, tcc_ir_op_get_src2(ir, q)));
        if (cid >= 0 && cid <= max_cid)
          param_call[i] = next_call[cid];
      }
    }
    if (next_call)
      tcc_free(next_call);
  }

  /* Pass 1: count uses and defs; disqualify shapes a temp can't replace. */
  for (int i = 0; i < n; i++) {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;
    int excluded = ra_split_op_excluded(q->op);
    IROperand ops[4];
    int no = 0;
    if (irop_config[q->op].has_src1) ops[no++] = tcc_ir_op_get_src1(ir, q);
    if (irop_config[q->op].has_src2) ops[no++] = tcc_ir_op_get_src2(ir, q);
    if (tcc_ir_op_is_mac(q->op)) ops[no++] = tcc_ir_op_get_accum(ir, q);
    for (int k = 0; k < no; k++) {
      if (!irop_has_vreg(ops[k]) || irop_is_immediate(ops[k]))
        continue;
      int c = SPLIT_CAND(irop_get_vreg(ops[k]));
      if (c < 0)
        continue;
      nuse[c]++;
      if (!bad[c] && (excluded || !ra_split_use_operand(ops[k], NULL, 0)))
        bad[c] = 1;
    }
    if (!irop_config[q->op].has_dest)
      continue;
    IROperand d = tcc_ir_op_get_dest(ir, q);
    if (!irop_has_vreg(d))
      continue;
    int32_t dv = irop_get_vreg(d);
    int c = SPLIT_CAND(dv);
    if (c < 0)
      continue;
    int dk = ra_split_def_kind(ir, q, dv);
    if (dk > 0) {
      ndef[c]++;
    } else if (dk < 0) {
      bad[c] = 1;
    } else {
      /* A store through the value.  STORE_INDEXED's base is a use for a
       * temp; for a VAR ra_build_intervals reads it as a def, so leave it. */
      int ok = (q->op == TCCIR_OP_STORE && TCCIR_DECODE_VREG_TYPE(dv) == TCCIR_VREG_TYPE_TEMP && d.is_lval &&
                !d.is_local && !d.is_llocal && d.tag == IROP_TAG_VREG) ||
               (q->op == TCCIR_OP_STORE_INDEXED && TCCIR_DECODE_VREG_TYPE(dv) == TCCIR_VREG_TYPE_TEMP &&
                d.tag == IROP_TAG_VREG && !d.is_local && !d.is_llocal);
      nuse[c]++;
      if (!ok)
        bad[c] = 1;
    }
  }

  /* Use and def lists in instruction order, bucketed by candidate. */
  int total = 0, dtotal = 0;
  for (int c = 0; c < ncand; c++) {
    int k = nuse[c];
    nuse[c] = total;
    total += k;
    k = ndef[c];
    ndef[c] = dtotal;
    dtotal += k;
  }
  nuse[ncand] = total;
  ndef[ncand] = dtotal;
  RaSplitUse *uses = tcc_malloc(sizeof(RaSplitUse) * (total > 0 ? total : 1));
  int *defs = tcc_malloc(sizeof(int) * (dtotal > 0 ? dtotal : 1));
  int *ufill = tcc_malloc(sizeof(int) * ncand);
  int *dfill = tcc_malloc(sizeof(int) * ncand);
  for (int c = 0; c < ncand; c++) {
    ufill[c] = nuse[c];
    dfill[c] = ndef[c];
  }
  for (int i = 0; i < n; i++) {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;
    int e = (q->op == TCCIR_OP_FUNCPARAMVAL && param_call[i] >= 0) ? param_call[i] : i;
    IROperand ops[4];
    int no = 0;
    if (irop_config[q->op].has_src1) ops[no++] = tcc_ir_op_get_src1(ir, q);
    if (irop_config[q->op].has_src2) ops[no++] = tcc_ir_op_get_src2(ir, q);
    if (tcc_ir_op_is_mac(q->op)) ops[no++] = tcc_ir_op_get_accum(ir, q);
    for (int k = 0; k < no; k++) {
      if (!irop_has_vreg(ops[k]) || irop_is_immediate(ops[k]))
        continue;
      int c = SPLIT_CAND(irop_get_vreg(ops[k]));
      if (c < 0)
        continue;
      uses[ufill[c]].instr = i;
      uses[ufill[c]++].eff = e;
    }
    if (!irop_config[q->op].has_dest)
      continue;
    IROperand d = tcc_ir_op_get_dest(ir, q);
    if (!irop_has_vreg(d))
      continue;
    int32_t dv = irop_get_vreg(d);
    int c = SPLIT_CAND(dv);
    if (c < 0)
      continue;
    int dk = ra_split_def_kind(ir, q, dv);
    if (dk > 0)
      defs[dfill[c]++] = i;
    else if (dk == 0) {
      uses[ufill[c]].instr = i;
      uses[ufill[c]++].eff = e;
    }
  }

  /* maxsrc[t]: furthest backward jump landing on t; jtp: jump-target prefix. */
  int *maxsrc = tcc_mallocz(sizeof(int) * (n + 1));
  int *jtp = tcc_malloc(sizeof(int) * (n + 2));
  jtp[0] = 0;
  for (int i = 0; i < n; i++) {
    IRQuadCompact *q = &ir->compact_instructions[i];
    jtp[i + 1] = jtp[i] + (q->is_jump_target ? 1 : 0);
    if (q->op != TCCIR_OP_JUMP && q->op != TCCIR_OP_JUMPIF)
      continue;
    int t = (int)irop_get_imm64_ex(ir, tcc_ir_op_get_dest(ir, q));
    if (t >= 0 && t <= i && i > maxsrc[t])
      maxsrc[t] = i;
  }
  jtp[n + 1] = jtp[n];

  RaSplitCopy *cps = NULL;
  int ncp = 0, cap = 0;
  uint8_t *grouped = tcc_mallocz(total > 0 ? total : 1);
  int dead_stores = 0;

  /* Call-free, and no loop that could leave the range past its end and come back. */
#define SPLIT_REGION_OK(start, e)                                                                                      \
  ({                                                                                                                   \
    int ok_ = (e) >= (start) && call_prefix[(e)] == call_prefix[(start)] && (e) - (start) < 4096;                      \
    for (int t_ = (start); ok_ && t_ <= (e); t_++)                                                                     \
      if (maxsrc[t_] > (e))                                                                                            \
        ok_ = 0;                                                                                                       \
    ok_;                                                                                                               \
  })
  /* No jump target in (a, b]: b is reached from a only by falling through. */
#define SPLIT_STRAIGHT(a, b) (jtp[(b) + 1] - jtp[(a) + 1] == 0)
#define SPLIT_PUSH(b, l, dv, sv, bt, uns)                                                                              \
  do {                                                                                                                 \
    if (ncp == cap) {                                                                                                  \
      cap = cap ? 2 * cap : 64;                                                                                        \
      cps = tcc_realloc(cps, sizeof(RaSplitCopy) * cap);                                                               \
    }                                                                                                                  \
    cps[ncp].before = (b);                                                                                             \
    cps[ncp].land = (l);                                                                                               \
    cps[ncp].dest = (dv);                                                                                              \
    cps[ncp].src = (sv);                                                                                               \
    cps[ncp].btype = (bt);                                                                                             \
    cps[ncp].is_unsigned = (uns);                                                                                      \
    ncp++;                                                                                                             \
  } while (0)

  for (int c = 0; c < ncand; c++) {
    int lo = nuse[c], hi = nuse[c + 1], dlo = ndef[c], dhi = ndef[c + 1];
    int nd = dhi - dlo;
    if (bad[c] || nd == 0 || hi - lo < 1)
      continue;
    int32_t V = cvreg[c];
    int single = nd == 1;

    /* A def of V strictly between a and b (linear)? */
#define SPLIT_DEF_BETWEEN(a, b)                                                                                        \
  ({                                                                                                                   \
    int f_ = 0;                                                                                                        \
    for (int x_ = dlo; x_ < dhi && !f_; x_++)                                                                          \
      if (defs[x_] > (a) && defs[x_] < (b))                                                                            \
        f_ = 1;                                                                                                        \
    f_;                                                                                                                \
  })

    /* Def groups: uses the def reaches in a call-free region read a temp the
     * def now writes. */
    for (int x = dlo; x < dhi; x++) {
      int d = defs[x], db = cfg->instr_to_block[d];
      if (db < 0 || d + 1 >= n || ra_split_flags_live_before(ir, d + 1))
        continue;
      int start = ra_split_is_call(ir, &ir->compact_instructions[d]) ? d + 1 : d;
      int members = 0, rest = 0;
      for (int k = lo; k < hi; k++) {
        if (grouped[k])
          continue;
        int ui = uses[k].instr, ub = cfg->instr_to_block[ui];
        int reach = ui > d && ub >= 0 &&
                    (single ? (ub == db || tcc_ir_cfg_dominates(cfg, db, ub))
                            : (SPLIT_STRAIGHT(d, ui) && !SPLIT_DEF_BETWEEN(d, ui)));
        if (reach && SPLIT_REGION_OK(start, uses[k].eff))
          members++;
        else
          rest++;
      }
      if (members == 0)
        continue;
      int32_t T = tcc_ir_vreg_alloc_temp(ir);
      if (T < 0)
        break;
      int last = -1;
      for (int k = lo; k < hi; k++) {
        if (grouped[k])
          continue;
        int ui = uses[k].instr, ub = cfg->instr_to_block[ui];
        int reach = ui > d && ub >= 0 &&
                    (single ? (ub == db || tcc_ir_cfg_dominates(cfg, db, ub))
                            : (SPLIT_STRAIGHT(d, ui) && !SPLIT_DEF_BETWEEN(d, ui)));
        if (!(reach && SPLIT_REGION_OK(start, uses[k].eff)))
          continue;
        grouped[k] = 1;
        if (ui != last) {
          ra_split_rename_uses(ir, &ir->compact_instructions[ui], V, T);
          last = ui;
        }
      }
      /* The spill store of a narrow def extends it into the slot; the copy
       * carries the def's width so it still does. */
      IROperand dop = tcc_ir_op_get_dest(ir, &ir->compact_instructions[d]);
      int dbt = ir->compact_instructions[d].op == TCCIR_OP_STORE ? IROP_BTYPE_INT32 : irop_get_btype(dop);
      int duns = dbt == IROP_BTYPE_INT32 ? 0 : dop.is_unsigned;
      ra_split_rename_def(ir, &ir->compact_instructions[d], T);
      if (!single || rest > 0)
        SPLIT_PUSH(d + 1, 0, V, T, dbt, duns);
      else
        dead_stores++;
    }

    /* Reload groups: uses after a first use of V in one call-free region share
     * its reload.  Single def: the def dominates the leader and the leader the
     * members.  Several defs: straight-line, no def of V in [leader, member). */
    for (int k = lo; k < hi; k++) {
      if (grouped[k])
        continue;
      int u0 = uses[k].instr, b0 = cfg->instr_to_block[u0];
      if (b0 < 0)
        continue;
      if (single) {
        int d = defs[dlo], db = cfg->instr_to_block[d];
        if (db < 0 || !(b0 == db ? d < u0 : tcc_ir_cfg_dominates(cfg, db, b0)))
          continue;
      }
      if (ra_split_flags_live_before(ir, u0))
        continue;
      int distinct = 0, last = -1;
#define SPLIT_MEMBER(j)                                                                                                \
  ({                                                                                                                   \
    int ui_ = uses[(j)].instr, ub_ = cfg->instr_to_block[ui_];                                                         \
    (j) == k || (ub_ >= 0 && ui_ >= u0 &&                                                                              \
                 (single ? (ub_ == b0 || tcc_ir_cfg_dominates(cfg, b0, ub_))                                           \
                         : (SPLIT_STRAIGHT(u0, ui_) && !SPLIT_DEF_BETWEEN(u0 - 1, ui_))) &&                            \
                 SPLIT_REGION_OK(u0, uses[(j)].eff));                                                                  \
  })
      for (int j = k; j < hi; j++) {
        if (grouped[j] || !SPLIT_MEMBER(j))
          continue;
        if (uses[j].instr != last) {
          distinct++;
          last = uses[j].instr;
        }
      }
      if (distinct < 2)
        continue;
      int32_t T = tcc_ir_vreg_alloc_temp(ir);
      if (T < 0)
        break;
      last = -1;
      for (int j = k; j < hi; j++) {
        if (grouped[j] || !SPLIT_MEMBER(j))
          continue;
        grouped[j] = 1;
        if (uses[j].instr != last) {
          ra_split_rename_uses(ir, &ir->compact_instructions[uses[j].instr], V, T);
          last = uses[j].instr;
        }
      }
#undef SPLIT_MEMBER
      SPLIT_PUSH(u0, 1, T, V, IROP_BTYPE_INT32, 0);
    }
#undef SPLIT_DEF_BETWEEN
  }
#undef SPLIT_REGION_OK
#undef SPLIT_STRAIGHT
#undef SPLIT_PUSH
#undef SPLIT_CAND
#undef SPLIT_IDX

  int changed = ncp + dead_stores;
  if (ncp > 0) {
    qsort(cps, ncp, sizeof(RaSplitCopy), ra_split_copy_cmp);
    ra_split_insert(ir, cps, ncp);
  }
  tcc_free(cps);
  tcc_free(grouped);
  tcc_free(maxsrc);
  tcc_free(jtp);
  tcc_free(uses);
  tcc_free(defs);
  tcc_free(ufill);
  tcc_free(dfill);
  tcc_free(param_call);
  tcc_free(bad);
  tcc_free(nuse);
  tcc_free(ndef);
  tcc_free(cand);
  tcc_free(cvreg);
  tcc_ir_cfg_free(cfg);
  return changed;
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
  /* sret_nrvo -- a struct returned through the hidden pointer is built in the
   * caller's buffer instead of a local copied out at the return.  Before
   * mem_init, which turns its initializer into a BLOCK_COPY that only writes
   * the frame. */
  RA_FLAT_PASS(ra_ch, "sret_nrvo", tcc_state->optimize > 0, tcc_ir_opt_sret_nrvo(ir));
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
  /* A table read `tab[i]` in an unrolled body is a load at a constant offset
   * now: the propagation group folds the addresses and the loads to the
   * table's values (symref_prop, global_init), which SSA does not.  Only for
   * a function whose unrolled loop read a const table: rerun on every
   * unrolled body, the group lost a packed-bitfield RMW through an address
   * alias (fuzz 231). */
  if (ra_ch && ir->unrolled_table_loads && !tcc_ir_opt_pass_disabled("ssa:unroll_cascade"))
  {
    {
      const IRPassGroup *groups;
      int group_count;
      tcc_ir_opt_get_pipeline(IR_OPT_LEVEL_2, &groups, &group_count);
      IROptCtx uc_ctx;
      tcc_ir_opt_ctx_init(&uc_ctx, ir);
      tcc_ir_opt_run_group(&uc_ctx, &groups[0]);
      tcc_ir_opt_ctx_free(&uc_ctx);
    }
    ir->unrolled_table_loads = 0;
    tcc_ir_dump_after_pass(ir, "ssa:unroll_cascade");
  }

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

  /* Bottom-test the walks IVSR just produced (ssa:loop_bottom_test): copy the
   * header CMP in front of the back-edge and make the back-edge the inverted
   * conditional branch, leaving the header pair as the zero-trip guard.  Runs
   * HERE, after ssa:iv_strength_reduction, because counter elimination is what
   * creates the `p != end` top test this rewrites; ssa:loop_rotate ran long
   * before and its matcher cannot see a body that falls through from the
   * header.  Flat IR before the CFG build, like the transforms above.  Gated
   * -O1+ (matches ssa:loop_rotate); disableable via
   * TCC_DISABLE_PASS=ssa:loop_bottom_test, level knob TCC_BOTTOM_TEST. */
  RA_FLAT_PASS(ra_ch, "ssa:loop_bottom_test", tcc_state->optimize >= 1,
               ssa_opt_loop_bottom_test(ir));

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

  /* Small frame objects touched only a word at a time become VARs, which the
   * SSA construction below renames like any other scalar. */
  ir->sra_var_lo = ir->sra_var_hi = -1;
  if (tcc_state && tcc_state->optimize > 0 && !tcc_ir_opt_pass_disabled("sra"))
  {
    tcc_ir_opt_sra(ir);
    tcc_ir_dump_after_pass(ir, "sra");
  }
  if (tcc_state && tcc_state->optimize > 0 && !tcc_ir_opt_pass_disabled("param_home_fwd"))
  {
    tcc_ir_opt_param_home_fwd(ir);
    tcc_ir_dump_after_pass(ir, "param_home_fwd");
  }

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
  /* The same state with the joins shown -- `ssa_rename`'s flat listing reads
   * phi destinations that nothing in it defines. */
  tcc_ir_dump_ssa_after_pass(ir, ssa, "ssa_phi");
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
      /* -O0.  `ssa_opt_dce` here is *not* an optimization the level is meant to
       * turn off: at optimize == 0 its body is `dce_temp_worklist` +
       * `dce_unreachable` (everything else in it is gated on optimize >= 1), so
       * what it removes is a dead compiler-generated temp and a block nothing
       * can reach.  Unreachable code is not code, and dropping it is
       * load-bearing at -O0 -- gcc-torture's `link_error` tests
       * (`medce-1`, `20030330-1`, `ieee/fp-cmp-7`) fold their condition in the
       * *frontend* and are left with a `JMP` over a call that must not reach
       * the linker.  gcc -O0 drops it too.
       *
       * `ssa_opt_cprop` used to run here and has been removed, because that one
       * is an optimization.  Its immediate dispatcher (`ssa_gen_cprop_imm`)
       * forwarded `T <- #k [ASSIGN]` into every use, so
       * `int k = 4 * 2; t = n * k;` reached the encoder at -O0 as a single
       * `MUL #8` -- which the backend then turned into a shifted add, making
       * -O0 output indistinguishable from -O2 on any function small enough to
       * read.  Propagating a value is exactly what -O0 promises not to do; with
       * it gone the same function keeps its `movs r1, #8` and a real `mul`.
       *
       * If something else ever *has* to run at -O0, it is lowering, and it
       * belongs in the lowering path rather than in the else arm of the
       * optimizer's gate. */
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
  /* And once the passes have had it: which phis survived them. */
  tcc_ir_dump_ssa_after_pass(ir, ssa, "ssa_phi_opt");
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
  /* The one edit that makes a phi disappear: every dump before this one can
   * have phis in it (with -dump-ir-passes=ssa_phi to see them) and no dump
   * after it can, because there is no phi left to show. */
  tcc_ir_dump_after_pass(ir, "ra_resolve_phis");
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

  /* `CMP; t <- SETIF; TEST_ZERO t; JUMPIF` -> `CMP; JUMPIF`, again.  The flat
   * pipeline's setif_fuse runs before SSA, when the Zig C backend's one
   * `bool t` per function is still a single VAR with copies and several
   * reads; SSA renaming gives each check its own single-use temp, and those
   * quartets used to reach codegen as ITE/MOV/MOV/CMP/branch.  Here phis are
   * explicit copies, so the use scan sees every read. */
  if (tcc_state->optimize >= 1 && !tcc_ir_opt_pass_disabled("ra:setif_fuse"))
    tcc_ir_opt_setif_branch_fuse(ir);
  tcc_ir_dump_after_pass(ir, "ra:setif_fuse");

  /* Product + zero-extended words -> UMAAL.  Last of the IR rewrites: every
   * pass before this point is unaware of TCCIR_OP_UMAAL; the allocator and
   * everything after it read its accumulator via tcc_ir_op_is_mac. */
  if (tcc_state->optimize >= 1 && !tcc_ir_opt_pass_disabled("ra:umaal"))
    ra_fuse_umaal(ir);
  tcc_ir_dump_after_pass(ir, "ra:umaal");

  /* TEMP definitions nothing reads, or overwritten before a read -- the SSA
   * passes above leave them (an entry-block VAR renamed to one TEMP keeps its
   * zero fill).  Here, before the intervals: graph coalescing below erases
   * the copies between vregs that share a register, after which a value can
   * be read under another vreg's name (dead_def.c). */
  if (tcc_state->optimize >= 1 && !tcc_ir_opt_pass_disabled("dead_def") && tcc_ir_dead_def(ir))
    tcc_ir_dump_after_pass(ir, "dead_def");

  int *call_prefix = NULL;
  SSAInterval *intervals = NULL;
  int interval_count = 0;
  int max_vreg_pos = 0;
  uint64_t dirty_int = 0, dirty_fp = 0;
  for (int round = 0;; round++) {
    /* Build call prefix for call-crossing detection */
    call_prefix = ra_build_call_prefix(ir);

    /* Build SSA live intervals */
    intervals = NULL;
    interval_count = 0;
    max_vreg_pos = 0;
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
    dirty_int = 0;
    dirty_fp = 0;
    int n_instr = ir->next_instruction_index;
    int has_call = call_prefix && n_instr > 0 && call_prefix[n_instr] > 0;
    RaAliveInfo alive;
    memset(&alive, 0, sizeof alive);
    if (ra_alive_worth_building(ir, intervals, interval_count))
      ra_alive_build(ir, &alive, max_vreg_pos);
    /* Drop the frame objects nothing references any more so spills go right
     * below the live ones (frame.c).  Here, after the SSA passes: some of them
     * add direct frame references (loop_const_sim's residual stores), and the
     * layout must see every reference the code will make. */
    if (round == 0 && tcc_state && tcc_state->optimize > 0 && !tcc_ir_opt_pass_disabled("frame_relayout"))
      tcc_ir_frame_relayout(ir, &spill_base);
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

    /* Spill-driven splitting: rewrite the spilled values' uses, then allocate
     * again from scratch (see ra_split_spilled). */
    if (round == 0 && tcc_state->optimize >= 1 && !tcc_ir_opt_pass_disabled("ra:spill_split") &&
        ra_split_spilled(ir, cfg, intervals, interval_count, max_vreg_pos, call_prefix) > 0) {
      tcc_free(intervals);
      intervals = NULL;
      tcc_free(call_prefix);
      call_prefix = NULL;
      tcc_ir_cfg_free(cfg);
      cfg = tcc_ir_cfg_build(ir);
      tcc_ir_cfg_compute_dominators(cfg);
      ssa->cfg = cfg;
      ssa->block_phis = tcc_realloc(ssa->block_phis, cfg->num_blocks * sizeof(IRPhiNode *));
      memset(ssa->block_phis, 0, cfg->num_blocks * sizeof(IRPhiNode *));
      continue;
    }
    break;
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
