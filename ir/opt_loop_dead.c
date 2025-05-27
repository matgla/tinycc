/*
 *  TCC IR - Loop Peeling: prove first-iteration exit
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 *
 * When a top-tested loop's header exit-test (TEST_ZERO/CMP + JUMPIF) is
 * statically true on entry from the preheader, the loop never executes.
 * This pass detects that pattern and rewrites the conditional JUMPIF into
 * an unconditional JUMP to the exit target; DCE later removes the now-
 * unreachable body and back-edge.
 *
 * The first-iteration values are computed by a small linear walk from
 * function entry through the header up to the exit test, tracking VAR
 * and TEMP constants plus LEA-of-VAR addresses.  The walk bails on any
 * intervening JUMP/JUMPIF so the values genuinely reflect program-entry
 * flow.  Stores through unknown pointers and impure calls invalidate
 * address-taken VARs.
 *
 * Targets cases like the PR-tree-optimization torture test 20070824-1.c,
 * where the loop `for (p = &s; *p; p = &(*p)->a);` provably exits on
 * iter 1 because s == 0.  Pre-SSA, before unroll/LCS, so subsequent
 * passes see the simplified IR.
 */

#define USING_GLOBALS

#include <string.h>

#include "ir.h"
#include "opt.h"
#include "opt_loop_utils.h"
#include "opt_utils.h"
#include "licm.h"
#include "log.h"

#define LD_MAX_VARS 256
#define LD_MAX_TMPS 512

typedef enum {
  LD_UNKNOWN = 0,
  LD_CONST,
  LD_LEA_VAR, /* &VAR — address of a tracked VAR position */
} LdKind;

typedef struct {
  LdKind  kind;
  int64_t value;     /* LD_CONST */
  int     target;    /* LD_LEA_VAR: VAR position */
} LdInfo;

typedef struct {
  LdInfo  var_state[LD_MAX_VARS];
  LdInfo  tmp_state[LD_MAX_TMPS];
  /* address-taken VARs: set when an LEA producing &V is seen, used to
   * invalidate V's tracked value on impure operations (calls, unknown stores). */
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

/* Resolve an operand's value at the current walk position.
 * Handles:
 *   - immediates
 *   - VAR value (is_lval=1 on STACKOFF tag, or VREG tag with is_lval=1)
 *   - VAR address (is_lval=0 on STACKOFF tag — "&V")
 *   - TEMP value (VREG tag, is_lval=0)
 *   - TEMP deref (VREG tag, is_lval=1 — "*T")
 * Stores result in *out and returns 1 on success, 0 if unknown.
 */
static int ld_resolve(TCCIRState *ir, LdState *st, IROperand op, LdInfo *out)
{
  if (irop_is_immediate(op)) {
    *out = (LdInfo){.kind = LD_CONST, .value = irop_get_imm64_ex(ir, op)};
    return 1;
  }

  int tag = irop_get_tag(op);

  /* STACKOFF tag refers to a stack slot. With is_lval=0 it's the address
   * ("&V"); with is_lval=1 it's the value read from V's slot. The vreg
   * encodes the VAR (or sometimes raw stack-offset without vreg). */
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
    /* is_lval: read VAR's current value */
    *out = st->var_state[pos];
    return out->kind != LD_UNKNOWN;
  }

  /* VREG-tagged operand. is_lval=1 means "load via this TEMP/VAR address". */
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

  /* Deref: slot must hold an LEA_VAR to know the load target. */
  if (slot->kind != LD_LEA_VAR || slot->target >= LD_MAX_VARS)
    return 0;
  *out = st->var_state[slot->target];
  return out->kind != LD_UNKNOWN;
}

/* Process one instruction in the linear walk. Returns 1 on success, 0 if
 * we encountered something that breaks the walk and we should bail. */
static int ld_step(TCCIRState *ir, LdState *st, IRQuadCompact *q)
{
  int op = q->op;

  switch (op) {
    case TCCIR_OP_NOP:
      return 1;

    /* Any branch / hard control flow ends the linear-walk's validity. */
    case TCCIR_OP_JUMP:
    case TCCIR_OP_JUMPIF:
    case TCCIR_OP_IJUMP:
    case TCCIR_OP_SWITCH_TABLE:
    case TCCIR_OP_RETURNVOID:
    case TCCIR_OP_RETURNVALUE:
      return 0;

    /* Calls may write to address-taken locals through stored pointers,
     * and have arbitrary side effects. Invalidate all addrtaken VARs and
     * any tracked dest TEMP/VAR. */
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
      /* TEMP destination with is_lval=1 would be a store-through-pointer.
       * ASSIGN normally doesn't use that form (STORE does), but be safe. */
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

      /* STORE to a stack-slot via STACKOFF dest: V.is_lval=1 means
       * "store into V's slot". */
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

      /* STORE through a TEMP pointer: *T = src.  If T is a known LEA(&V),
       * update V; otherwise invalidate all addrtaken VARs. */
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

      /* Unknown store: pessimize all addrtaken VARs. */
      ld_clear_all_addrtaken(st);
      return 1;
    }

    case TCCIR_OP_TEST_ZERO:
    case TCCIR_OP_CMP:
      /* Test-only ops produce no value; ignore. */
      return 1;

    default: {
      /* Generic op: if it has a dest TEMP/VAR, mark it unknown. */
      if (irop_config[op].has_dest) {
        IROperand dest = tcc_ir_op_get_dest(ir, q);
        if (dest.is_lval) {
          /* Some "store-like" form we don't model; pessimize. */
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

/* JUMPIF condition tokens (from arm-thumb tokens; matches branch-fold). */
#define LD_TOK_EQ 0x94
#define LD_TOK_NE 0x95

/* Locate the first exit JUMPIF in [start_idx .. limit] whose target is
 * outside the loop range.  out_test_idx is the matching TEST_ZERO or CMP
 * immediately preceding it.  Returns 1 on success. */
static int ld_find_exit_branch(TCCIRState *ir, IRLoop *loop, int *out_test_idx,
                               int *out_jumpif_idx, int *out_exit_target)
{
  int n = ir->next_instruction_index;
  int max_lookahead = 6; /* header should test within a handful of insns */

  int test_idx = -1;
  for (int i = loop->start_idx; i < n && i <= loop->start_idx + max_lookahead; i++) {
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
      /* Exit target must be outside the loop range. */
      if (target < loop->start_idx || target > loop->end_idx) {
        *out_test_idx     = test_idx;
        *out_jumpif_idx   = i;
        *out_exit_target  = target;
        return 1;
      }
      return 0;
    }
    /* Other instruction kinds between header and the exit branch are
     * allowed (e.g. ASSIGN, LEA, LOAD); they participate in the linear
     * value walk. */
  }
  return 0;
}

/* Evaluate the static result of the exit branch using the walked state.
 * Returns 1 if proven taken, 0 if proven not-taken, -1 if unknown. */
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
    /* Reuse the engine's comparator. */
    int r = evaluate_compare_condition(a.value, b.value, tok);
    if (r < 0) return -1;
    return r;
  }

  return -1;
}

/* Walk linearly through [0..stop_at-1], updating the state. Bails (returns 0)
 * on the first JUMP/JUMPIF encountered before stop_at, which indicates the
 * function entry path is not straight-line into the loop. */
static int ld_walk_linear_to(TCCIRState *ir, LdState *st, int stop_at)
{
  for (int i = 0; i < stop_at; i++) {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (!ld_step(ir, st, q))
      return 0;
  }
  return 1;
}

static int try_first_iter_exit(TCCIRState *ir, IRLoop *loop)
{
  if (!ir || !loop)
    return 0;
  if (loop->start_idx < 0 || loop->end_idx < 0)
    return 0;
  if (loop->preheader_idx < 0)
    return 0;
  if (loop->start_idx >= ir->next_instruction_index)
    return 0;

  int test_idx, jumpif_idx, exit_target;
  if (!ld_find_exit_branch(ir, loop, &test_idx, &jumpif_idx, &exit_target))
    return 0;

  LOG_LOOP_OPT("first_iter_exit: header=%d test@%d jumpif@%d exit=%d",
               loop->header_idx, test_idx, jumpif_idx, exit_target);

  LdState st;
  memset(&st, 0, sizeof(st));
  if (!ld_walk_linear_to(ir, &st, jumpif_idx)) {
    LOG_LOOP_OPT("first_iter_exit: bail (non-straight-line path before jumpif)");
    return 0;
  }

  int taken = ld_eval_branch(ir, &st, test_idx, jumpif_idx);
  if (taken != 1) {
    LOG_LOOP_OPT("first_iter_exit: branch outcome=%d (need 1=taken)", taken);
    return 0;
  }

  /* Rewrite JUMPIF to unconditional JUMP. The dest already holds the
   * exit target; we just switch the op and remove the condition src1. */
  IRQuadCompact *jump_q = &ir->compact_instructions[jumpif_idx];
  IROperand exit_dest = tcc_ir_op_get_dest(ir, jump_q);
  jump_q->op = TCCIR_OP_JUMP;
  tcc_ir_set_dest(ir, jumpif_idx, exit_dest);

  /* NOP every other instruction in the loop range — the body never runs and
   * the header's pre-JUMPIF defs (e.g. `T = V`) are dead since the test that
   * read them is itself dead.  Leaving them creates stale `is_jump_target`
   * tracking that confuses downstream non-null analyses. */
  for (int i = loop->start_idx; i <= loop->end_idx; i++) {
    if (i == jumpif_idx)
      continue;
    ir->compact_instructions[i].op = TCCIR_OP_NOP;
  }

  LOG_IR_GEN("[LOOP-DEAD-1ST-ITER] Eliminated loop header=%d (proven exit at iter 1, target=%d)",
             loop->header_idx, exit_target);
  return 1;
}

/* NOP any unconditional JUMP whose target is its own next non-NOP successor.
 * After loop elimination + compact_nops, the redirect JUMP often becomes a
 * no-op that nevertheless leaves an `is_jump_target` flag on its target,
 * which is_jump_target-sensitive analyses (e.g. stack_addr_nonnull_fold)
 * use as a tracking-reset signal — needlessly losing precision. */
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

int tcc_ir_opt_loop_dead_first_iter(TCCIRState *ir)
{
  if (!ir || ir->next_instruction_index == 0)
    return 0;

  IRLoops *loops = tcc_ir_detect_loops(ir);
  if (!loops || loops->num_loops == 0) {
    tcc_ir_free_loops(loops);
    return 0;
  }

  /* Sort smallest-first so inner loops are tried before outer ones. */
  qsort(loops->loops, loops->num_loops, sizeof(IRLoop), loop_size_cmp);

  int eliminated = 0;
  for (int i = 0; i < loops->num_loops; i++) {
    IRLoop *loop = &loops->loops[i];
    if (loop->start_idx < 0)
      continue;
    eliminated += try_first_iter_exit(ir, loop);
  }

  if (eliminated > 0)
    ld_nop_fallthrough_jumps(ir);

  tcc_ir_free_loops(loops);
  return eliminated;
}
