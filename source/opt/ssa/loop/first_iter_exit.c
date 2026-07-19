/*
 *  TCC IR - SSA loop: first-iteration exit elimination
 *
 *  Copyright (c) 2025 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#include "ir.h"
#include "ssa_opt.h"
#include "opt_utils.h"      /* evaluate_compare_condition */
#include "loop_cand.h"

#define LD_MAX_VARS 256
#define LD_MAX_TMPS 512

typedef enum {
  LD_UNKNOWN = 0,
  LD_CONST,
  LD_LEA_VAR,
} LdKind;

typedef struct {
  LdKind  kind;
  int64_t value;     /* LD_CONST */
  int     target;    /* LD_LEA_VAR: VAR position */
} LdInfo;

typedef struct {
  LdInfo  var_state[LD_MAX_VARS];
  LdInfo  tmp_state[LD_MAX_TMPS];
  uint8_t var_addrtaken[(LD_MAX_VARS + 7) / 8];
} LdState;

static void ld_clear_all_addrtaken(LdState *st)
{
  for (int v = 0; v < LD_MAX_VARS; v++) {
    if (st->var_addrtaken[v / 8] & (1u << (v % 8)))
      st->var_state[v] = (LdInfo){0};
  }
}

static int ld_decode_vreg(IROperand op, int *out_kind, int *out_pos)
{
  int32_t vr = irop_get_vreg(op);
  if (vr < 0)
    return 0;
  int kind = TCCIR_DECODE_VREG_TYPE(vr);
  int pos  = TCCIR_DECODE_VREG_POSITION(vr);
  if (kind != TCCIR_VREG_TYPE_VAR && kind != TCCIR_VREG_TYPE_TEMP)
    return 0;
  *out_kind = kind;
  *out_pos  = pos;
  return 1;
}

static LdInfo *ld_slot(LdState *st, int kind, int pos)
{
  if (kind == TCCIR_VREG_TYPE_VAR && pos < LD_MAX_VARS)
    return &st->var_state[pos];
  if (kind == TCCIR_VREG_TYPE_TEMP && pos < LD_MAX_TMPS)
    return &st->tmp_state[pos];
  return NULL;
}

/* Returns 0 when the operand's value is unknown. */
static int ld_resolve(TCCIRState *ir, LdState *st, IROperand op, LdInfo *out)
{
  if (irop_is_immediate(op)) {
    *out = (LdInfo){.kind = LD_CONST, .value = irop_get_imm64_ex(ir, op)};
    return 1;
  }

  int tag = irop_get_tag(op);

  /* STACKOFF: is_lval=0 is the address "&V", is_lval=1 the value in V's slot. */
  if (tag == IROP_TAG_STACKOFF) {
    int kind, pos;
    if (!ld_decode_vreg(op, &kind, &pos))
      return 0;
    if (kind != TCCIR_VREG_TYPE_VAR || pos >= LD_MAX_VARS)
      return 0;
    if (!op.is_lval) {
      *out = (LdInfo){.kind = LD_LEA_VAR, .target = pos};
      return 1;
    }
    *out = st->var_state[pos];
    return out->kind != LD_UNKNOWN;
  }

  /* VREG-tagged operand: is_lval=1 means "load via this TEMP/VAR address". */
  int kind, pos;
  if (!ld_decode_vreg(op, &kind, &pos))
    return 0;
  LdInfo *slot = ld_slot(st, kind, pos);
  if (!slot)
    return 0;

  if (!op.is_lval) {
    *out = *slot;
    return out->kind != LD_UNKNOWN;
  }

  if (slot->kind != LD_LEA_VAR || slot->target >= LD_MAX_VARS)
    return 0;
  *out = st->var_state[slot->target];
  return out->kind != LD_UNKNOWN;
}

/* Returns 0 when the instruction breaks the linear walk and it must bail. */
static int ld_step(TCCIRState *ir, LdState *st, IRQuadCompact *q)
{
  int op = q->op;

  switch (op) {
    case TCCIR_OP_NOP:
      return 1;

    /* Any branch or hard control flow ends the linear walk's validity. */
    case TCCIR_OP_JUMP:
    case TCCIR_OP_JUMPIF:
    case TCCIR_OP_IJUMP:
    case TCCIR_OP_SWITCH_TABLE:
    case TCCIR_OP_RETURNVOID:
    case TCCIR_OP_RETURNVALUE:
      return 0;

    /* Calls may write address-taken locals through previously stored pointers. */
    case TCCIR_OP_FUNCCALLVAL:
    case TCCIR_OP_FUNCCALLVOID:
    case TCCIR_OP_VLA_ALLOC:
    case TCCIR_OP_VLA_SP_SAVE:
    case TCCIR_OP_VLA_SP_RESTORE:
    case TCCIR_OP_INLINE_ASM:
    case TCCIR_OP_SETJMP:
    case TCCIR_OP_LONGJMP:
    case TCCIR_OP_NL_SETJMP:
    case TCCIR_OP_NL_LONGJMP:
    case TCCIR_OP_BUILTIN_APPLY:
    case TCCIR_OP_BUILTIN_APPLY_ARGS:
    case TCCIR_OP_BUILTIN_RETURN:
      ld_clear_all_addrtaken(st);
      if (irop_config[op].has_dest) {
        IROperand dest = tcc_ir_op_get_dest(ir, q);
        int kind, pos;
        if (ld_decode_vreg(dest, &kind, &pos)) {
          LdInfo *s = ld_slot(st, kind, pos);
          if (s) *s = (LdInfo){0};
        }
      }
      return 1;

    case TCCIR_OP_LEA: {
      IROperand dest = tcc_ir_op_get_dest(ir, q);
      IROperand src1 = tcc_ir_op_get_src1(ir, q);
      int d_kind, d_pos, s_kind, s_pos;
      if (!ld_decode_vreg(dest, &d_kind, &d_pos))
        return 1;
      LdInfo *dslot = ld_slot(st, d_kind, d_pos);
      if (!dslot)
        return 1;
      if (ld_decode_vreg(src1, &s_kind, &s_pos) &&
          s_kind == TCCIR_VREG_TYPE_VAR && s_pos < LD_MAX_VARS) {
        st->var_addrtaken[s_pos / 8] |= (uint8_t)(1u << (s_pos % 8));
        *dslot = (LdInfo){.kind = LD_LEA_VAR, .target = s_pos};
      } else {
        *dslot = (LdInfo){0};
      }
      return 1;
    }

    case TCCIR_OP_ASSIGN: {
      IROperand dest = tcc_ir_op_get_dest(ir, q);
      IROperand src1 = tcc_ir_op_get_src1(ir, q);
      int d_kind, d_pos;
      if (!ld_decode_vreg(dest, &d_kind, &d_pos))
        return 1;
      /* A TEMP dest with is_lval=1 is a store-through-pointer. */
      if (d_kind == TCCIR_VREG_TYPE_TEMP && dest.is_lval) {
        ld_clear_all_addrtaken(st);
        return 1;
      }
      LdInfo *dslot = ld_slot(st, d_kind, d_pos);
      if (!dslot)
        return 1;
      LdInfo v;
      if (ld_resolve(ir, st, src1, &v))
        *dslot = v;
      else
        *dslot = (LdInfo){0};
      return 1;
    }

    case TCCIR_OP_LOAD: {
      IROperand dest = tcc_ir_op_get_dest(ir, q);
      IROperand src1 = tcc_ir_op_get_src1(ir, q);
      int d_kind, d_pos;
      if (!ld_decode_vreg(dest, &d_kind, &d_pos))
        return 1;
      LdInfo *dslot = ld_slot(st, d_kind, d_pos);
      if (!dslot)
        return 1;
      LdInfo v;
      if (ld_resolve(ir, st, src1, &v))
        *dslot = v;
      else
        *dslot = (LdInfo){0};
      return 1;
    }

    case TCCIR_OP_STORE: {
      IROperand dest = tcc_ir_op_get_dest(ir, q);
      IROperand src1 = tcc_ir_op_get_src1(ir, q);

      int d_tag = irop_get_tag(dest);

      if (d_tag == IROP_TAG_STACKOFF && dest.is_lval) {
        int d_kind, d_pos;
        if (ld_decode_vreg(dest, &d_kind, &d_pos) &&
            d_kind == TCCIR_VREG_TYPE_VAR && d_pos < LD_MAX_VARS) {
          LdInfo v;
          if (ld_resolve(ir, st, src1, &v))
            st->var_state[d_pos] = v;
          else
            st->var_state[d_pos] = (LdInfo){0};
          return 1;
        }
      }

      /* Store through a TEMP pointer: only a known LEA(&V) keeps V tracked. */
      if (d_tag == IROP_TAG_VREG && dest.is_lval) {
        int d_kind, d_pos;
        if (ld_decode_vreg(dest, &d_kind, &d_pos)) {
          LdInfo *aslot = ld_slot(st, d_kind, d_pos);
          if (aslot && aslot->kind == LD_LEA_VAR && aslot->target < LD_MAX_VARS) {
            LdInfo v;
            if (ld_resolve(ir, st, src1, &v))
              st->var_state[aslot->target] = v;
            else
              st->var_state[aslot->target] = (LdInfo){0};
            return 1;
          }
        }
      }

      ld_clear_all_addrtaken(st);
      return 1;
    }

    case TCCIR_OP_TEST_ZERO:
    case TCCIR_OP_CMP:
      return 1;

    default: {
      if (irop_config[op].has_dest) {
        IROperand dest = tcc_ir_op_get_dest(ir, q);
        if (dest.is_lval) {
          /* Unmodelled store-like form: pessimize. */
          ld_clear_all_addrtaken(st);
        } else {
          int d_kind, d_pos;
          if (ld_decode_vreg(dest, &d_kind, &d_pos)) {
            LdInfo *dslot = ld_slot(st, d_kind, d_pos);
            if (dslot) *dslot = (LdInfo){0};
          }
        }
      }
      return 1;
    }
  }
}

/* JUMPIF condition tokens; must match the arm-thumb token values. */
#define LD_TOK_EQ 0x94
#define LD_TOK_NE 0x95

/* Returns 1 if the branch is proven taken, 0 if proven not-taken, -1 unknown. */
static int ld_eval_branch(TCCIRState *ir, LdState *st, int test_idx,
                          int jumpif_idx)
{
  IRQuadCompact *test_q = &ir->compact_instructions[test_idx];
  IRQuadCompact *jump_q = &ir->compact_instructions[jumpif_idx];
  IROperand jcond = tcc_ir_op_get_src1(ir, jump_q);
  int tok = (int)irop_get_imm64_ex(ir, jcond);

  if (test_q->op == TCCIR_OP_TEST_ZERO) {
    IROperand src1 = tcc_ir_op_get_src1(ir, test_q);
    LdInfo v;
    if (!ld_resolve(ir, st, src1, &v) || v.kind != LD_CONST)
      return -1;
    if (tok == LD_TOK_EQ) return v.value == 0;
    if (tok == LD_TOK_NE) return v.value != 0;
    return -1;
  }

  if (test_q->op == TCCIR_OP_CMP) {
    IROperand s1 = tcc_ir_op_get_src1(ir, test_q);
    IROperand s2 = tcc_ir_op_get_src2(ir, test_q);
    LdInfo a, b;
    if (!ld_resolve(ir, st, s1, &a) || a.kind != LD_CONST)
      return -1;
    if (!ld_resolve(ir, st, s2, &b) || b.kind != LD_CONST)
      return -1;
    int r = evaluate_compare_condition(a.value, b.value, tok);
    if (r < 0) return -1;
    return r;
  }

  return -1;
}

/* Returns 0 if the entry path to stop_at is not straight-line. */
static int ld_walk_linear_to(TCCIRState *ir, LdState *st, int stop_at)
{
  for (int i = 0; i < stop_at; i++) {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (!ld_step(ir, st, q))
      return 0;
  }
  return 1;
}

/* Returns 1 only when the exit branch is provably TAKEN on first entry. */
static int ld_first_iter_prove(TCCIRState *ir, int test_idx, int jumpif_idx)
{
  if (!ir || test_idx < 0 || jumpif_idx <= test_idx ||
      jumpif_idx >= ir->next_instruction_index)
    return 0;

  /* ~18 KB: a stack local here overflows the 32 KB target process stack. */
  LdState *st = tcc_malloc(sizeof(*st));
  memset(st, 0, sizeof(*st));
  int ok = ld_walk_linear_to(ir, st, jumpif_idx);
  int taken = ok ? ld_eval_branch(ir, st, test_idx, jumpif_idx) : -1;
  tcc_free(st);

  if (!ok) {
    LOG_LOOP_OPT("first_iter_exit: bail (non-straight-line path before jumpif)");
    return 0;
  }
  if (taken != 1) {
    LOG_LOOP_OPT("first_iter_exit: branch outcome=%d (need 1=taken)", taken);
    return 0;
  }
  return 1;
}

/* NOP any unconditional JUMP whose target is its own next non-NOP successor. */
static void ld_nop_fallthrough_jumps(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  for (int i = 0; i < n; i++) {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_JUMP)
      continue;
    IROperand dest = tcc_ir_op_get_dest(ir, q);
    int target = (int)irop_get_imm64_ex(ir, dest);
    int next_live = i + 1;
    while (next_live < n && ir->compact_instructions[next_live].op == TCCIR_OP_NOP)
      next_live++;
    if (target == next_live)
      q->op = TCCIR_OP_NOP;
  }
}

/* One elimination per CFG build: the rewrite makes the CFG stale. */
#define SSA_FIRST_ITER_EXIT_MAX_PASSES 4

/* The header must test-and-exit within this many instructions of its start. */
#define FIE_EXIT_TEST_LOOKAHEAD 6

/* Returns 1 if the loop was eliminated. `member` is num_blocks caller scratch. */
static int fie_try_candidate(TCCIRState *ir, IRCFG *cfg, int header_b,
                             int latch_b, uint8_t *member)
{
  IRBasicBlock *hb = &cfg->blocks[header_b];

  fie_collect_members(cfg, header_b, latch_b, member);

  /* Header preds must be {function-entry, latch}: no jump target may sit
   * between the walked entry code and the header. */
  if (hb->num_preds != 2)
    return 0;
  int latch_seen = 0, entry_pred = -1;
  for (int i = 0; i < hb->num_preds; i++) {
    int p = hb->preds[i];
    if (p == latch_b && !latch_seen)
      latch_seen = 1;
    else
      entry_pred = p;
  }
  if (!latch_seen || entry_pred < 0 || member[entry_pred])
    return 0;
  if (cfg->blocks[entry_pred].start_idx != 0)
    return 0;

  /* No other member block may be entered from outside the loop. */
  for (int b = 0; b < cfg->num_blocks; b++) {
    if (!member[b] || b == header_b)
      continue;
    IRBasicBlock *bb = &cfg->blocks[b];
    for (int i = 0; i < bb->num_preds; i++)
      if (!member[bb->preds[i]])
        return 0;
  }

  /* Exit branch: TEST_ZERO/CMP then a JUMPIF targeting a non-member block. */
  int test_idx = -1, jumpif_idx = -1;
  for (int i = hb->start_idx;
       i < hb->end_idx && i <= hb->start_idx + FIE_EXIT_TEST_LOOKAHEAD; i++) {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;
    if (q->op == TCCIR_OP_TEST_ZERO || q->op == TCCIR_OP_CMP) {
      test_idx = i;
      continue;
    }
    if (q->op == TCCIR_OP_JUMPIF) {
      if (test_idx < 0)
        return 0;
      IROperand dest = tcc_ir_op_get_dest(ir, q);
      int target = (int)irop_get_imm64_ex(ir, dest);
      if (target < 0 || target >= cfg->num_instrs)
        return 0;
      if (member[cfg->instr_to_block[target]])
        return 0; /* not an exit branch */
      jumpif_idx = i;
      break;
    }
  }
  if (jumpif_idx < 0)
    return 0;

  if (!ld_first_iter_prove(ir, test_idx, jumpif_idx))
    return 0;

  /* The JUMPIF dest already holds the exit target. */
  IRQuadCompact *jump_q = &ir->compact_instructions[jumpif_idx];
  IROperand exit_dest = tcc_ir_op_get_dest(ir, jump_q);
  jump_q->op = TCCIR_OP_JUMP;
  tcc_ir_set_dest(ir, jumpif_idx, exit_dest);

  for (int b = 0; b < cfg->num_blocks; b++) {
    if (!member[b])
      continue;
    for (int i = cfg->blocks[b].start_idx; i < cfg->blocks[b].end_idx; i++) {
      if (i == jumpif_idx)
        continue;
      ir->compact_instructions[i].op = TCCIR_OP_NOP;
    }
  }
  return 1;
}

int ssa_opt_first_iter_exit(TCCIRState *ir)
{
  if (!ir || ir->next_instruction_index == 0)
    return 0;
  if (!tcc_ir_cfg_flat_has_backedge(ir))
    return 0;

  int total = 0;
  for (int pass = 0; pass < SSA_FIRST_ITER_EXIT_MAX_PASSES; pass++) {
    IRCFG *cfg = tcc_ir_cfg_build(ir);
    if (!cfg || cfg->num_blocks <= 1) {
      tcc_ir_cfg_free(cfg);
      break;
    }
    tcc_ir_cfg_compute_dominators(cfg);

    /* Dominance-verified back-edges latch->header, tried smallest-first. */
    int cap = cfg->num_blocks;
    FieCand *cands = tcc_mallocz(sizeof(FieCand) * (size_t)cap);
    int nc = 0;
    for (int b = 0; b < cfg->num_blocks && nc < cap; b++) {
      IRBasicBlock *bb = &cfg->blocks[b];
      for (int si = 0; si < bb->num_succs && nc < cap; si++) {
        int h = bb->succs[si];
        if (h < 0 || h >= cfg->num_blocks)
          continue;
        if (!tcc_ir_cfg_dominates(cfg, h, b))
          continue;
        cands[nc].header_b = h;
        cands[nc].latch_b = b;
        cands[nc].size = cfg->blocks[b].end_idx - cfg->blocks[h].start_idx;
        nc++;
      }
    }

    /* Stop at the first elimination: the rewrite invalidates this CFG. */
    int fired = 0;
    if (nc > 0) {
      qsort(cands, nc, sizeof(FieCand), fie_cand_cmp);
      uint8_t *member = tcc_malloc((size_t)cfg->num_blocks);
      for (int i = 0; i < nc && !fired; i++)
        fired = fie_try_candidate(ir, cfg, cands[i].header_b,
                                  cands[i].latch_b, member);
      tcc_free(member);
    }
    tcc_free(cands);
    tcc_ir_cfg_free(cfg);

    if (!fired)
      break;
    total++;
    ld_nop_fallthrough_jumps(ir);
  }
  return total;
}
