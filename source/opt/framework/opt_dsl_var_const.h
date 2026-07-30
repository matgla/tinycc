/*
 *  TCC IR — Optimization DSL: single-def VAR immediate forwarding
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation.
 */

#pragma once

/* SSA companion to opt_dsl_ssa.h for memory-backed locals.  A non-addrtaken,
 * non-volatile VAR whose lone def is `V <- #imm [ASSIGN|STORE]` is a constant
 * in all but name; VAR_IMM() matches a source operand carrying such a VAR whose
 * def dominates the use and yields the immediate to forward.  Unlike PAIR (which
 * resolves single-def TEMP producers by SSA use-def), VARs are memory slots, so
 * this needs a whole-function precompute (opt_dsl_var_imm_state_build) and a real
 * dominator check.  Include ir.h and ssa_opt.h first; do NOT include from flat
 * (pre-SSA) passes. */

#include "ssa_opt.h"
#include "opt_dsl_types.h"

typedef struct {
  int32_t *def_idx;   /* per-VAR lone def instruction, -1 = none */
  uint8_t *def_cnt;   /* def count, saturating at 2 */
  uint8_t *blocked;   /* addrtaken / volatile / LEA-escaped VAR */
  uint8_t *use_cnt;   /* source-operand use count, saturating at 255 */
  int      nv;
} OptDslVarImmState;

typedef struct {
  int32_t   vr;       /* the VAR vreg carried at the use site */
  int       pos;
  IROperand def_imm;  /* raw immediate from the VAR's defining ASSIGN/STORE */
  IROperand use_op;   /* the operand at the use site (for width matching) */
} OptDslVarImm;

/* Whole-function precompute for VAR_IMM: single-def index/count, escape mask,
 * and per-VAR source-operand use counts.  Returns 1 with the arrays populated,
 * or 0 (arrays left NULL) when the function shape forbids the analysis — no
 * VARs, or an op whose control flow / aliasing the dominator tree can't model
 * (IJUMP/SWITCH_TABLE lack the CFG edges dominance needs; SETJMP re-entry
 * bypasses the dominator tree; ASM may touch VARs invisibly). */
static inline int opt_dsl_var_imm_state_build(IRSSAOptCtx *ctx,
                                              OptDslVarImmState *st)
{
  TCCIRState *ir = ctx->ir;
  st->def_idx = NULL; st->def_cnt = NULL;
  st->blocked = NULL; st->use_cnt = NULL; st->nv = 0;
  int n = ir->next_instruction_index;
  int nv = ir->next_local_variable;
  if (nv <= 0)
    return 0;

  for (int i = 0; i < n; i++) {
    int op = ir->compact_instructions[i].op;
    if (op == TCCIR_OP_IJUMP || op == TCCIR_OP_SWITCH_TABLE ||
        op == TCCIR_OP_SETJMP || op == TCCIR_OP_NL_SETJMP ||
        op == TCCIR_OP_INLINE_ASM || op == TCCIR_OP_ASM_INPUT ||
        op == TCCIR_OP_ASM_OUTPUT)
      return 0;
  }

  int32_t *def_idx = tcc_malloc(nv * sizeof(int32_t));
  uint8_t *def_cnt = tcc_mallocz(nv);
  uint8_t *blocked = tcc_mallocz(nv);
  uint8_t *use_cnt = tcc_mallocz(nv);
  for (int v = 0; v < nv; v++)
    def_idx[v] = -1;
  for (int v = 0; v < nv && v < ir->variables_live_intervals_size; v++)
    if (ir->variables_live_intervals[v].addrtaken ||
        ir->variables_live_intervals[v].is_volatile)
      blocked[v] = 1;

  for (int i = 0; i < n; i++) {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;
    if (q->op == TCCIR_OP_LEA) {
      IROperand s1 = tcc_ir_op_get_src1(ir, q);
      int32_t lv = irop_get_vreg(s1);
      if (lv >= 0 && TCCIR_DECODE_VREG_TYPE(lv) == TCCIR_VREG_TYPE_VAR &&
          TCCIR_DECODE_VREG_POSITION(lv) < (uint32_t)nv)
        blocked[TCCIR_DECODE_VREG_POSITION(lv)] = 1;
    }
    if (!irop_config[q->op].has_dest || q->op == TCCIR_OP_FUNCPARAMVAL ||
        q->op == TCCIR_OP_FUNCPARAMVOID)
      continue;
    IROperand d = tcc_ir_op_get_dest(ir, q);
    int32_t dv = irop_get_vreg(d);
    if (dv < 0 || TCCIR_DECODE_VREG_TYPE(dv) != TCCIR_VREG_TYPE_VAR)
      continue;
    int pos = TCCIR_DECODE_VREG_POSITION(dv);
    if (pos >= nv)
      continue;
    /* STORE_INDEXED reads its dest as base address; deref STORE writes the
     * pointee; anything else with a VAR dest (slot-write STORE, POSTINC pointer
     * update, ALU/CALL/ASSIGN dest) redefines the VAR. */
    if (q->op == TCCIR_OP_STORE_INDEXED)
      continue;
    if (q->op == TCCIR_OP_STORE && d.is_lval && !d.is_local)
      continue;
    def_idx[pos] = i;
    if (def_cnt[pos] < 2)
      def_cnt[pos]++;
  }

  for (int i = 0; i < n; i++) {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;
    for (int slot = 0; slot < 3; slot++) {
      IROperand s;
      if (slot == 0) {
        if (!irop_config[q->op].has_src1) continue;
        s = tcc_ir_op_get_src1(ir, q);
      } else if (slot == 1) {
        if (!irop_config[q->op].has_src2) continue;
        s = tcc_ir_op_get_src2(ir, q);
      } else {
        if (q->op != TCCIR_OP_MLA) continue;
        s = tcc_ir_op_get_accum(ir, q);
      }
      if (s.tag != IROP_TAG_VREG)
        continue;
      int32_t vr = irop_get_vreg(s);
      if (vr < 0 || TCCIR_DECODE_VREG_TYPE(vr) != TCCIR_VREG_TYPE_VAR)
        continue;
      int pos = TCCIR_DECODE_VREG_POSITION(vr);
      if (pos < nv && use_cnt[pos] < 255)
        use_cnt[pos]++;
    }
  }

  st->def_idx = def_idx; st->def_cnt = def_cnt;
  st->blocked = blocked; st->use_cnt = use_cnt; st->nv = nv;
  return 1;
}

static inline void opt_dsl_var_imm_state_free(OptDslVarImmState *st)
{
  tcc_free(st->def_idx); tcc_free(st->def_cnt);
  tcc_free(st->blocked); tcc_free(st->use_cnt);
  st->def_idx = NULL; st->def_cnt = NULL;
  st->blocked = NULL; st->use_cnt = NULL; st->nv = 0;
}

/* def dominates use: same block => program order; else walk the use's idom
 * chain up to the def's block. */
static inline int opt_dsl_var_imm_dominates(IRCFG *cfg, int def_idx, int use_idx)
{
  if (def_idx >= cfg->num_instrs || use_idx >= cfg->num_instrs)
    return 0;
  int db = cfg->instr_to_block[def_idx];
  int ub = cfg->instr_to_block[use_idx];
  if (db < 0 || ub < 0)
    return 0;
  if (db == ub)
    return def_idx < use_idx;
  int b = ub, steps = 0;
  while (b >= 0 && b != db && steps++ < cfg->num_blocks) {
    int id = cfg->blocks[b].idom;
    if (id == b)
      break;
    b = id;
  }
  return b == db;
}

/* Resolve slot (0=src1, 1=src2, 2=accum) of instruction i to a forwardable VAR
 * immediate; fills *out and returns 1, or returns 0 when the operand is not a
 * single-def, non-escaped, dominating VAR whose def is a width-matching inline
 * immediate.  Accepts a bare (non-lval) VAR VREG value or the lval-STACKOFF
 * slot-read the frontend emits when a VAR's value is read into an arithmetic
 * op — both decode to the VAR vreg; a symref or pointer-deref lval is not a
 * VAR-slot value and is rejected. */
static inline int opt_dsl_var_imm_match(IRSSAOptCtx *ctx,
                                        const OptDslVarImmState *st,
                                        int i, int slot, OptDslVarImm *out)
{
  TCCIRState *ir = ctx->ir;
  IRQuadCompact *q = &ir->compact_instructions[i];
  IROperand s = (slot == 0) ? tcc_ir_op_get_src1(ir, q)
              : (slot == 1) ? tcc_ir_op_get_src2(ir, q)
                            : tcc_ir_op_get_accum(ir, q);
  int is_bare_vreg = (s.tag == IROP_TAG_VREG && !s.is_lval && !s.is_local &&
                      !s.is_llocal && !s.is_sym);
  int is_slot_read = (s.tag == IROP_TAG_STACKOFF && s.is_lval && !s.is_sym);
  if (!is_bare_vreg && !is_slot_read)
    return 0;
  int use_bt = irop_get_btype(s);
  if (use_bt != IROP_BTYPE_INT32 && use_bt != IROP_BTYPE_INT64)
    return 0;
  int32_t vr = irop_get_vreg(s);
  if (vr < 0 || TCCIR_DECODE_VREG_TYPE(vr) != TCCIR_VREG_TYPE_VAR)
    return 0;
  int pos = TCCIR_DECODE_VREG_POSITION(vr);
  if (pos >= st->nv || st->blocked[pos] || st->def_cnt[pos] != 1)
    return 0;
  IRQuadCompact *dq = &ir->compact_instructions[st->def_idx[pos]];
  if (dq->op != TCCIR_OP_ASSIGN && dq->op != TCCIR_OP_STORE)
    return 0;
  IROperand dd = tcc_ir_op_get_dest(ir, dq);
  int def_bt = irop_get_btype(dd);
  /* An lval ASSIGN would be a deref, not a value def; the STORE slot form is
   * safe here because the VAR is non-addr-taken (blocked) and single-def. */
  if (dq->op == TCCIR_OP_ASSIGN && dd.is_lval)
    return 0;
  /* def and use must be the same width: forwarding a 64-bit const into a
   * 32-bit operand (or vice versa) changes the value the operand carries. */
  if (def_bt != use_bt ||
      (def_bt != IROP_BTYPE_INT32 && def_bt != IROP_BTYPE_INT64))
    return 0;
  IROperand ds = tcc_ir_op_get_src1(ir, dq);
  int ds_ok = (def_bt == IROP_BTYPE_INT64)
                  ? (ds.tag == IROP_TAG_IMM32 || ds.tag == IROP_TAG_I64)
                  : (ds.tag == IROP_TAG_IMM32);
  if (!ds_ok || ds.is_lval || ds.is_local || ds.is_sym)
    return 0;
  /* A 64-bit constant materialises via a multi-instruction / pool-load
   * sequence, so forwarding it to more than one use trades one slot load for
   * several materialisations — forward 64-bit only when single-use (the def
   * becomes dead and DCE removes it, a strict win). */
  if (def_bt == IROP_BTYPE_INT64 && st->use_cnt[pos] > 1)
    return 0;
  if (!opt_dsl_var_imm_dominates(ctx->cfg, st->def_idx[pos], i))
    return 0;
  out->vr      = vr;
  out->pos     = pos;
  out->def_imm = ds;
  out->use_op  = s;
  return 1;
}

/* VAR_IMM(state_ptr, slot_expr) — bind the forwardable VAR immediate at a
 * source slot and bail the dispatch on mismatch.  Requires ctx and i in scope
 * (as PAIR does).  Binds viv (the VAR vreg), videf (raw def immediate) and
 * viuse (the use operand); build the forwarded immediate with
 * ssa_cprop_imm_for_use(videf, viuse). */
#define VAR_IMM(state_ptr, slot_expr) \
  OptDslVarImm _var_imm; \
  if (!opt_dsl_var_imm_match(ctx, (state_ptr), i, (slot_expr), &_var_imm)) \
    return 0; \
  int32_t viv = _var_imm.vr; (void)viv; \
  IROperand videf = _var_imm.def_imm; (void)videf; \
  IROperand viuse = _var_imm.use_op; (void)viuse

