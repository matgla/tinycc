/*
 *  TCC IR - Variable-to-Temp Promotion & Forwarding
 *
 *  Copyright (c) 2025 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#define USING_GLOBALS

#include "ir.h"
#include "opt_engine.h"
#include "opt_du.h"
#include "opt_utils.h"
#include "opt_loop_utils.h"

int tcc_ir_opt_redundant_loop_check(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  int changes = 0;

  if (n < 4)
    return 0;

  IRLoops *loops = tcc_ir_detect_loops(ir);
  if (!loops || loops->num_loops == 0)
  {
    tcc_ir_free_loops(loops);
    return 0;
  }

  for (int li = 0; li < loops->num_loops; li++)
  {
    IRLoop *loop = &loops->loops[li];

    int32_t guard_vreg = -1;
    int64_t guard_const = 0;
    int guard_body_fact = -1;
    int guard_cmp_idx = -1;

    for (int i = loop->header_idx; i <= loop->header_idx + 4 && i <= loop->end_idx && i < n - 1; i++)
    {
      IRQuadCompact *cq = &ir->compact_instructions[i];
      if (cq->op == TCCIR_OP_NOP)
        continue;
      if (cq->op != TCCIR_OP_CMP)
        continue;

      IROperand s1 = tcc_ir_op_get_src1(ir, cq);
      IROperand s2 = tcc_ir_op_get_src2(ir, cq);
      if (!irop_is_immediate(s2) || s2.is_sym)
        continue;
      int32_t vr = irop_get_vreg(s1);
      if (vr < 0)
        continue;

      int j = i + 1;
      while (j < n && ir->compact_instructions[j].op == TCCIR_OP_NOP)
        j++;
      if (j >= n || ir->compact_instructions[j].op != TCCIR_OP_JUMPIF)
        continue;

      IRQuadCompact *jq = &ir->compact_instructions[j];
      IROperand cond_op = tcc_ir_op_get_src1(ir, jq);
      int cond = (int)irop_get_imm64_ex(ir, cond_op);
      IROperand jdest = tcc_ir_op_get_dest(ir, jq);
      int target = (int)jdest.u.imm32;

      if (target > loop->end_idx || target < loop->start_idx)
      {
        int neg = vrp_negate_cmp_tok(cond);
        if (neg >= 0)
        {
          guard_cmp_idx = i;
          guard_vreg = vr;
          guard_const = irop_get_imm64_ex(ir, s2);
          guard_body_fact = neg;
          break;
        }
      }
    }

    if (guard_body_fact < 0)
      continue;

    /* Find the scan range: all instructions in the loop body.
     * The guard fact holds between the header fall-through and the back-edge,
     * including body blocks that are after the back-edge in instruction order
     * but reachable from the header via a forward JMP.
     * Use the exit target as the upper bound — anything before the exit
     * target is in the loop body. */
    int exit_target = -1;
    {
      int gi = guard_cmp_idx + 1;
      while (gi < n && ir->compact_instructions[gi].op == TCCIR_OP_NOP)
        gi++;
      if (gi < n && ir->compact_instructions[gi].op == TCCIR_OP_JUMPIF)
      {
        IROperand gd = tcc_ir_op_get_dest(ir, &ir->compact_instructions[gi]);
        exit_target = (int)gd.u.imm32;
      }
    }
    int scan_end = (exit_target > 0) ? exit_target - 1 : loop->end_idx;

    for (int i = loop->start_idx; i <= scan_end && i < n - 1; i++)
    {
      if (i == guard_cmp_idx)
        continue;

      IRQuadCompact *cq = &ir->compact_instructions[i];
      if (cq->op != TCCIR_OP_CMP)
        continue;

      IROperand s1 = tcc_ir_op_get_src1(ir, cq);
      IROperand s2 = tcc_ir_op_get_src2(ir, cq);
      if (!irop_is_immediate(s2) || s2.is_sym)
        continue;
      if (irop_get_imm64_ex(ir, s2) != guard_const)
        continue;

      int32_t inner_vr = irop_get_vreg(s1);
      if (inner_vr < 0)
        continue;

      int vreg_match = (inner_vr == guard_vreg);
      if (!vreg_match)
      {
        int def_idx = tcc_ir_find_defining_instruction(ir, inner_vr, i);
        if (def_idx >= 0)
        {
          IRQuadCompact *dq = &ir->compact_instructions[def_idx];
          if (dq->op == TCCIR_OP_STORE || dq->op == TCCIR_OP_ASSIGN)
          {
            IROperand dsrc = tcc_ir_op_get_src1(ir, dq);
            if (irop_get_vreg(dsrc) == guard_vreg)
              vreg_match = 1;
          }
        }
      }
      if (!vreg_match)
        continue;

      int j = i + 1;
      while (j < n && ir->compact_instructions[j].op == TCCIR_OP_NOP)
        j++;
      if (j >= n || ir->compact_instructions[j].op != TCCIR_OP_JUMPIF)
        continue;

      IRQuadCompact *jq = &ir->compact_instructions[j];
      IROperand cond_op = tcc_ir_op_get_src1(ir, jq);
      int inner_cond = (int)irop_get_imm64_ex(ir, cond_op);
      IROperand jdest = tcc_ir_op_get_dest(ir, jq);

      if (vrp_cmp_implies(guard_body_fact, inner_cond))
      {
        cq->op = TCCIR_OP_NOP;
        jq->op = TCCIR_OP_JUMP;
        tcc_ir_set_dest(ir, j, jdest);
        changes++;
      }
      else
      {
        int neg_inner = vrp_negate_cmp_tok(inner_cond);
        if (neg_inner >= 0 && vrp_cmp_implies(guard_body_fact, neg_inner))
        {
          cq->op = TCCIR_OP_NOP;
          jq->op = TCCIR_OP_NOP;
          changes++;
        }
      }
    }
  }

  tcc_ir_free_loops(loops);
  return changes;
}

/* ============================================================================
 * Local Load CSE  (tcc_ir_opt_local_load_cse)
 * ============================================================================
 *
 * Within a basic block, when a VAR/PARAM is loaded twice into different TEMPs,
 * the second load is replaced with a copy of the first TEMP.
 *
 * Before:
 *   T9  <-- V1 [ASSIGN, lval]     # load V1 into T9
 *   T10 <-- V1 [ASSIGN, lval]     # load V1 again into T10
 *
 * After:
 *   T9  <-- V1 [ASSIGN, lval]     # load V1 into T9
 *   T10 <-- T9 [ASSIGN]           # copy from T9 (no reload)
 */

int tcc_ir_opt_var_to_tmp(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  int changes = 0;

  if (n < 2)
    return 0;

  /* Phase 1: single-pass collect def_count + any-non-lval-read for each VAR. */
  int max_var_pos = 0;
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;
    if (irop_config[q->op].has_dest)
    {
      IROperand d = tcc_ir_op_get_dest(ir, q);
      int32_t v = irop_get_vreg(d);
      if (TCCIR_DECODE_VREG_TYPE(v) == TCCIR_VREG_TYPE_VAR)
      {
        int p = TCCIR_DECODE_VREG_POSITION(v);
        if (p > max_var_pos)
          max_var_pos = p;
      }
    }
    if (irop_config[q->op].has_src1)
    {
      IROperand s = tcc_ir_op_get_src1(ir, q);
      int32_t v = irop_get_vreg(s);
      if (TCCIR_DECODE_VREG_TYPE(v) == TCCIR_VREG_TYPE_VAR)
      {
        int p = TCCIR_DECODE_VREG_POSITION(v);
        if (p > max_var_pos)
          max_var_pos = p;
      }
    }
    if (irop_config[q->op].has_src2)
    {
      IROperand s = tcc_ir_op_get_src2(ir, q);
      int32_t v = irop_get_vreg(s);
      if (TCCIR_DECODE_VREG_TYPE(v) == TCCIR_VREG_TYPE_VAR)
      {
        int p = TCCIR_DECODE_VREG_POSITION(v);
        if (p > max_var_pos)
          max_var_pos = p;
      }
    }
  }

  if (max_var_pos == 0)
    return 0;

  typedef struct
  {
    int def_idx;       /* -1 if no def yet, -2 if multiple defs seen */
    int bad_use;       /* 1 if V is ever used in a way we can't rewrite */
    int lval_read_cnt; /* number of rewritable lval ASSIGN reads */
  } VInfo;

  VInfo *info = tcc_mallocz(sizeof(VInfo) * (max_var_pos + 1));
  for (int p = 0; p <= max_var_pos; p++)
    info[p].def_idx = -1;

  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;

    /* Track VAR defs. In TCC's IR, writing a VAR is a STORE to its memory
     * slot; the dest carries is_lval=1 *because* a VAR is memory, not because
     * someone took its address. So any write where the dest vreg is a VAR
     * counts as a def regardless of is_lval. Address-taken VARs are filtered
     * out in Phase 2 via the live interval. */
    if (irop_config[q->op].has_dest)
    {
      IROperand d = tcc_ir_op_get_dest(ir, q);
      int32_t v = irop_get_vreg(d);
      if (TCCIR_DECODE_VREG_TYPE(v) == TCCIR_VREG_TYPE_VAR)
      {
        int p = TCCIR_DECODE_VREG_POSITION(v);
        if (info[p].def_idx == -1)
          info[p].def_idx = i;
        else
          info[p].def_idx = -2; /* multiple defs */
      }
    }

    /* Check src1 / src2 uses of any VAR. Rewritable uses are:
     *   - src1 of ASSIGN targeting a TEMP (the classic reload pattern)
     *   - src1 of FUNCPARAMVAL / FUNCPARAMVOID (passing V's value to a call)
     * Anything else disqualifies V. */
    if (irop_config[q->op].has_src1)
    {
      IROperand s = tcc_ir_op_get_src1(ir, q);
      int32_t v = irop_get_vreg(s);
      if (TCCIR_DECODE_VREG_TYPE(v) == TCCIR_VREG_TYPE_VAR)
      {
        int p = TCCIR_DECODE_VREG_POSITION(v);
        int ok = 0;
        if (q->op == TCCIR_OP_ASSIGN && s.is_lval &&
            TCCIR_DECODE_VREG_TYPE(irop_get_vreg(tcc_ir_op_get_dest(ir, q))) == TCCIR_VREG_TYPE_TEMP)
          ok = 1;
        else if ((q->op == TCCIR_OP_FUNCPARAMVAL || q->op == TCCIR_OP_FUNCPARAMVOID) && s.is_lval)
          ok = 1;
        if (ok)
          info[p].lval_read_cnt++;
        else
          info[p].bad_use = 1;
      }
    }
    if (irop_config[q->op].has_src2)
    {
      IROperand s = tcc_ir_op_get_src2(ir, q);
      int32_t v = irop_get_vreg(s);
      if (TCCIR_DECODE_VREG_TYPE(v) == TCCIR_VREG_TYPE_VAR)
      {
        /* VAR in src2 is never the "T <-- V [ASSIGN lval]" pattern — disqualify */
        int p = TCCIR_DECODE_VREG_POSITION(v);
        info[p].bad_use = 1;
      }
    }
  }

  /* Phase 2: for each viable VAR, verify single-def, in-BB, and rewrite. */
  for (int p = 0; p <= max_var_pos; p++)
  {
    LOG_COPY_PROP("var_to_tmp CAND V:%d def_idx=%d bad_use=%d lval_reads=%d", p, info[p].def_idx, info[p].bad_use,
                  info[p].lval_read_cnt);
    if (info[p].def_idx == -1)
    {
      LOG_COPY_PROP("var_to_tmp SKIP V:%d: no def", p);
      continue;
    }
    if (info[p].def_idx == -2)
    {
      LOG_COPY_PROP("var_to_tmp SKIP V:%d: multiple defs", p);
      continue;
    }
    if (info[p].bad_use)
    {
      LOG_COPY_PROP("var_to_tmp SKIP V:%d: bad_use", p);
      continue;
    }
    if (info[p].lval_read_cnt == 0)
    {
      LOG_COPY_PROP("var_to_tmp SKIP V:%d: no lval reads", p);
      continue;
    }

    int def_i = info[p].def_idx;
    IRQuadCompact *def_q = &ir->compact_instructions[def_i];
    IROperand def_dest = tcc_ir_op_get_dest(ir, def_q);
    int32_t dest_vr = irop_get_vreg(def_dest);
    int def_btype = irop_get_btype(def_dest);

    /* Only handle plain INT32 (pointer / regular int) scalars. Any wider or
     * multi-word type (INT64, FLOAT32/64, STRUCT, INT8/16) would require
     * preserving memory semantics we can't reproduce with a single TMP. */
    if (def_btype != IROP_BTYPE_INT32)
    {
      LOG_COPY_PROP("var_to_tmp SKIP V:%d: def btype=%d not INT32", p, def_btype);
      continue;
    }

    /* Skip VARs whose storage is observable or whose type wouldn't round-trip
     * cleanly through a TEMP: address-taken, complex, long long, float/double,
     * or explicit lvalue kinds. Keep the pass to plain 32-bit scalars and
     * let other passes tackle the wider cases. Missing live interval is
     * treated conservatively (skip) since we can't verify the type flags. */
    IRLiveInterval *intv = tcc_ir_get_live_interval(ir, dest_vr);
    if (!intv)
    {
      LOG_COPY_PROP("var_to_tmp SKIP V:%d: null interval", p);
      continue;
    }
    if (intv->addrtaken || intv->is_complex || intv->is_llong || intv->is_float || intv->is_double || intv->is_lvalue)
    {
      LOG_COPY_PROP("var_to_tmp SKIP V:%d: flags addr=%d cx=%d ll=%d f=%d d=%d lv=%d", p, intv->addrtaken,
                    intv->is_complex, intv->is_llong, intv->is_float, intv->is_double, intv->is_lvalue);
      continue;
    }

    /* Restrict the def to opcodes that produce a single scalar value and
     * whose operand shape is already a pure value-write (ASSIGN-like).
     * TCC's STORE is what you get for local-pointer VAR writes, but
     * converting STORE→ASSIGN has turned out to corrupt later passes
     * (observed wrong value in test_llong_load_signed). Hold it back
     * until we understand why. */
    int def_op = def_q->op;
    if (def_op != TCCIR_OP_ASSIGN && def_op != TCCIR_OP_LOAD && def_op != TCCIR_OP_ADD && def_op != TCCIR_OP_SUB &&
        def_op != TCCIR_OP_MUL && def_op != TCCIR_OP_AND && def_op != TCCIR_OP_OR && def_op != TCCIR_OP_XOR &&
        def_op != TCCIR_OP_SHL && def_op != TCCIR_OP_SHR && def_op != TCCIR_OP_LEA)
    {
      LOG_COPY_PROP("var_to_tmp SKIP V:%d: def op=%d not in allowlist", p, def_op);
      continue;
    }

    /* Walk forward within the same BB, collecting lval reads of V.
     * Abort on any control-flow boundary, call, or surprise use. */
    int uses[16];
    int num_uses = 0;
    int aborted = 0;

    for (int j = def_i + 1; j < n; j++)
    {
      IRQuadCompact *q = &ir->compact_instructions[j];
      if (q->op == TCCIR_OP_NOP)
        continue;

      /* Terminators end the BB without a use */
      if (q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_JUMPIF || q->op == TCCIR_OP_RETURNVALUE ||
          q->op == TCCIR_OP_RETURNVOID || q->op == TCCIR_OP_SWITCH_TABLE || q->op == TCCIR_OP_IJUMP)
        break;
      /* Calls clobber caller-saved regs and split the BB for our scan */
      if (q->op == TCCIR_OP_FUNCCALLVAL || q->op == TCCIR_OP_FUNCCALLVOID)
        break;

      /* Redef of V — stop (shouldn't happen since def_count==1, but defensive) */
      if (irop_config[q->op].has_dest)
      {
        IROperand d = tcc_ir_op_get_dest(ir, q);
        if (irop_get_vreg(d) == dest_vr)
        {
          aborted = 1;
          break;
        }
      }

      /* Collect ASSIGN-lval and FUNCPARAMVAL/VOID reads of V */
      int is_rewritable_read = 0;
      if (q->op == TCCIR_OP_ASSIGN && irop_config[q->op].has_src1)
      {
        IROperand s = tcc_ir_op_get_src1(ir, q);
        if (irop_get_vreg(s) == dest_vr && s.is_lval)
        {
          IROperand ud = tcc_ir_op_get_dest(ir, q);
          if (TCCIR_DECODE_VREG_TYPE(irop_get_vreg(ud)) != TCCIR_VREG_TYPE_TEMP)
          {
            aborted = 1;
            break;
          }
          is_rewritable_read = 1;
        }
      }
      else if ((q->op == TCCIR_OP_FUNCPARAMVAL || q->op == TCCIR_OP_FUNCPARAMVOID) && irop_config[q->op].has_src1)
      {
        IROperand s = tcc_ir_op_get_src1(ir, q);
        if (irop_get_vreg(s) == dest_vr && s.is_lval)
          is_rewritable_read = 1;
      }
      if (is_rewritable_read)
      {
        IROperand s = tcc_ir_get_src1(ir, j);
        /* Btype must match the def — narrowing/widening loads can't be
         * replaced by a direct register copy. */
        if (irop_get_btype(s) != def_btype)
        {
          aborted = 1;
          break;
        }
        if (num_uses >= (int)(sizeof(uses) / sizeof(uses[0])))
        {
          aborted = 1;
          break;
        }
        uses[num_uses++] = j;
      }
    }

    if (aborted || num_uses == 0)
    {
      LOG_COPY_PROP("var_to_tmp SKIP V:%d: aborted=%d num_uses=%d", p, aborted, num_uses);
      continue;
    }
    /* Guard against Phase-1 vs Phase-2 disagreement (uses outside this BB) */
    if (num_uses != info[p].lval_read_cnt)
    {
      LOG_COPY_PROP("var_to_tmp SKIP V:%d: cross-BB uses (BB=%d global=%d)", p, num_uses, info[p].lval_read_cnt);
      continue;
    }

    /* Transform: allocate a fresh TMP, redirect def, rewrite each use. */
    int32_t new_tmp = tcc_ir_vreg_alloc_temp(ir);
    if (new_tmp < 0)
      continue;

    /* Fresh TMP operand — clears is_local / is_llocal / u.*, preserves btype. */
    IROperand new_dest = irop_make_vreg(new_tmp, def_btype);
    new_dest.is_unsigned = def_dest.is_unsigned;
    tcc_ir_set_dest(ir, def_i, new_dest);

    /* If we ever re-enable STORE defs here, remember to flip the op to
     * ASSIGN so the backend doesn't misinterpret a TMP dest as a pointer
     * dereference. The earlier experiment produced wrong data — hold off. */

    for (int u = 0; u < num_uses; u++)
    {
      int j = uses[u];
      IROperand s = tcc_ir_get_src1(ir, j);
      IROperand new_src = irop_make_vreg(new_tmp, s.btype);
      new_src.is_unsigned = s.is_unsigned;
      /* is_lval cleared by irop_make_vreg — the read is now a register copy */
      tcc_ir_set_src1(ir, j, new_src);
      changes++;
      LOG_COPY_PROP("var_to_tmp: V:%d@def=%d -> T:%d; rewrite use at i=%d", p, def_i, new_tmp, j);
    }
  }

  tcc_free(info);
  return changes;
}

/* ============================================================================
 * Conditional Select (ITE) Optimization
 *
 * Detects "diamond" if/else patterns where both branches produce a single
 * value and replaces them with a SELECT instruction, enabling ITE generation.
 *
 * Pattern: CMP + JUMPIF + PARAM+CALL(then) + JUMP + PARAM+CALL(else)
 * where both sides call the same function with only arg0 differing.
 * Result: CMP + SELECT arg0 + PARAM+CALL
 *
 * Also handles: CMP + JUMPIF + ASSIGN(then) + JUMP + ASSIGN(else)
 * where both sides assign to the same vreg.
 * Result: CMP + SELECT vreg
 * ============================================================================ */

/* ir_skip_nops_forward, ir_negate_condition, ir_has_other_jump_to_fast
 * moved to opt_utils.c */

/* ============================================================================
 * Redundant Initialization Elimination
 * ============================================================================
 *
 * Eliminates function-entry VAR initializations that are always killed
 * (redefined) before any use.  Common pattern:
 *
 *   int sum = 0;           ← dead init (eliminated)
 *   for (...) { ... }      ← loop doesn't use sum
 *   for (...) {
 *     sum = 0;             ← kills sum
 *     for (...) sum += x;  ← first use after kill
 *   }
 *
 * Uses forward dataflow: from the init, follow all control flow paths.
 * If every path reaches a redef of V before any use of V, the init is dead.
 */

/* ============================================================================
 * Dead Loop Elimination
 * ============================================================================
 *
 * Eliminate loops whose body has no observable side effects.
 * When all stores inside the loop are to local VARs with constant values,
 * and there are no calls, memory stores, or other side effects, the entire
 * loop can be replaced by its final constant assignments.
 */
/* RETURNVALUE merge: when a function has multiple RETURNVALUE instructions
 * that return the same immediate value, keep the first one as-is and convert
 * the others into JUMPs to the first.  The codegen for RETURNVALUE emits
 * `mov r0, imm; b epilogue` (two instructions), whereas JUMP emits just `b`,
 * so each conversion saves one instruction.  Common in functions with multiple
 * `return 0;` or `return 1;` sites — e.g. test bodies that bail with `return 1`
 * on each failed check. */
int tcc_ir_opt_returnvalue_merge(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  int changes = 0;

  /* Linear list keyed by value; functions with many distinct return
   * constants are rare so we cap at 32 to keep this bounded. */
  struct { int64_t value; int idx; } first_ret[32];
  int num_first_ret = 0;

  for (int i = 0; i < n; i++) {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_RETURNVALUE) continue;

    IROperand src = tcc_ir_op_get_src1(ir, q);
    if (!irop_is_immediate(src) || src.is_lval) continue;

    /* Skip 64-bit and float sources — their materialization sequence is
     * longer than 1 instruction, so a single branch may not save anything,
     * and the codegen for them is more involved. */
    int btype = irop_get_btype(src);
    if (btype == IROP_BTYPE_INT64 || btype == IROP_BTYPE_FLOAT32 ||
        btype == IROP_BTYPE_FLOAT64)
      continue;

    int64_t val = irop_get_imm64_ex(ir, src);

    int canonical = -1;
    for (int j = 0; j < num_first_ret; j++) {
      if (first_ret[j].value == val) {
        canonical = first_ret[j].idx;
        break;
      }
    }

    if (canonical >= 0) {
      /* Convert to JUMP target=canonical IR index. */
      q->op = TCCIR_OP_JUMP;
      tcc_ir_op_set_dest(ir, q, irop_make_imm32(-1, canonical, IROP_BTYPE_INT32));
      tcc_ir_op_set_src1(ir, q, IROP_NONE);
      tcc_ir_op_set_src2(ir, q, IROP_NONE);
      changes++;
    } else if (num_first_ret < (int)(sizeof(first_ret) / sizeof(first_ret[0]))) {
      first_ret[num_first_ret].value = val;
      first_ret[num_first_ret].idx = i;
      num_first_ret++;
    }
  }

  return changes;
}

int tcc_ir_opt_backedge_phi_hoist(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  if (n < 4) return 0;

  int changes = 0;

  for (int i = 0; i < n - 2; i++) {
    IRQuadCompact *jif = &ir->compact_instructions[i];
    if (jif->op != TCCIR_OP_JUMPIF)
      continue;

    int exit_target = (int)irop_get_imm32(tcc_ir_op_get_dest(ir, jif));
    int cond = (int)tcc_ir_op_get_src1(ir, jif).u.imm32;

    /* Exit target must be forward */
    if (exit_target <= i)
      continue;

    /* Count consecutive ASSIGNs after JUMPIF */
    int num_assigns = 0;
    for (int j = i + 1; j < n; j++) {
      IRQuadCompact *q = &ir->compact_instructions[j];
      if (q->op == TCCIR_OP_ASSIGN)
        num_assigns++;
      else
        break;
    }
    if (num_assigns == 0 || num_assigns > 8)
      continue;

    /* Next after ASSIGNs must be an unconditional backward JUMP */
    int jump_idx = i + 1 + num_assigns;
    if (jump_idx >= n)
      continue;
    IRQuadCompact *jmp = &ir->compact_instructions[jump_idx];
    if (jmp->op != TCCIR_OP_JUMP)
      continue;

    int body_target = (int)irop_get_imm32(tcc_ir_op_get_dest(ir, jmp));
    if (body_target >= i)
      continue;

    /* Exit target must be past the JUMP (fall-through reachable) */
    if (exit_target < jump_idx)
      continue;

    /* Verify CMP precedes JUMPIF */
    if (i == 0)
      continue;
    IRQuadCompact *cmp_q = &ir->compact_instructions[i - 1];
    if (cmp_q->op != TCCIR_OP_CMP)
      continue;

    /* Safety: all ASSIGN operands must be in physical registers (not spilled).
     * Stack-spilled copies generate load/store sequences that can interact
     * badly with pending condition flags. */
    int safe = 1;
    for (int j = 0; j < num_assigns && safe; j++) {
      IRQuadCompact *aq = &ir->compact_instructions[i + 1 + j];
      IROperand adst = tcc_ir_op_get_dest(ir, aq);
      IROperand asrc = tcc_ir_op_get_src1(ir, aq);
      int32_t adst_vr = irop_get_vreg(adst);
      int32_t asrc_vr = irop_get_vreg(asrc);

      /* Check dest is in a register */
      if (adst_vr >= 0) {
        int spilled = 0;
        for (int k = 0; k < ir->ls.next_interval_index; k++) {
          if (ir->ls.intervals[k].vreg == (uint32_t)adst_vr) {
            if (ir->ls.intervals[k].stack_location != 0 || ir->ls.intervals[k].r0 < 0)
              spilled = 1;
            break;
          }
        }
        if (spilled) safe = 0;
      }
      /* Check src is in a register */
      if (safe && asrc_vr >= 0) {
        int spilled = 0;
        for (int k = 0; k < ir->ls.next_interval_index; k++) {
          if (ir->ls.intervals[k].vreg == (uint32_t)asrc_vr) {
            if (ir->ls.intervals[k].stack_location != 0 || ir->ls.intervals[k].r0 < 0)
              spilled = 1;
            break;
          }
        }
        if (spilled) safe = 0;
      }
    }

    /* Verify no ASSIGN destination is a source of the CMP */
    IROperand cmp_src1 = tcc_ir_op_get_src1(ir, cmp_q);
    IROperand cmp_src2 = tcc_ir_op_get_src2(ir, cmp_q);
    int32_t cmp_vr1 = irop_get_vreg(cmp_src1);
    int32_t cmp_vr2 = irop_get_vreg(cmp_src2);
    for (int j = 0; j < num_assigns && safe; j++) {
      IRQuadCompact *aq = &ir->compact_instructions[i + 1 + j];
      IROperand adst = tcc_ir_op_get_dest(ir, aq);
      int32_t adst_vr = irop_get_vreg(adst);
      if (adst_vr >= 0 && (adst_vr == cmp_vr1 || adst_vr == cmp_vr2))
        safe = 0;
    }

    /* Verify no ASSIGN destination is live on the exit path.
     * If a dest vreg appears as a source operand after exit_target
     * before being redefined, the exit path reads the pre-ASSIGN value
     * and hoisting would clobber it. */
    for (int j = 0; j < num_assigns && safe; j++) {
      IROperand adst = tcc_ir_op_get_dest(ir, &ir->compact_instructions[i + 1 + j]);
      int32_t adst_vr = irop_get_vreg(adst);
      if (adst_vr < 0) continue;
      for (int k = exit_target; k < n && safe; k++) {
        IRQuadCompact *eq = &ir->compact_instructions[k];
        if (eq->op == TCCIR_OP_NOP) continue;
        if (irop_config[eq->op].has_src1) {
          if (irop_get_vreg(tcc_ir_op_get_src1(ir, eq)) == adst_vr)
            safe = 0;
        }
        if (safe && irop_config[eq->op].has_src2) {
          if (irop_get_vreg(tcc_ir_op_get_src2(ir, eq)) == adst_vr)
            safe = 0;
        }
        if (safe && eq->op == TCCIR_OP_MLA) {
          if (irop_get_vreg(tcc_ir_op_get_accum(ir, eq)) == adst_vr)
            safe = 0;
        }
        if (safe && irop_config[eq->op].has_dest &&
            irop_get_vreg(tcc_ir_op_get_dest(ir, eq)) == adst_vr) {
          /* STORE-family ops carry the store ADDRESS in their dest slot, so a
           * matching dest is a USE of the pointer vreg (the exit path stores
           * through it), not a redefinition.  Hoisting the ASSIGN over the
           * branch would clobber the pointer it stores through, so this is a
           * live use — not safe.  Same for an is_lval (deref) dest.  Only a
           * genuine value def of the vreg makes the prior value dead. */
          if (eq->op == TCCIR_OP_STORE || eq->op == TCCIR_OP_STORE_INDEXED ||
              eq->op == TCCIR_OP_STORE_POSTINC ||
              tcc_ir_op_get_dest(ir, eq).is_lval)
            safe = 0;
          else
            break; /* genuine redefinition before use — safe */
        }
      }
    }

    if (!safe)
      continue;

    int inv_cond = invert_condition(cond);
    if (inv_cond < 0)
      continue;

    /* Save the JUMPIF's operand pool base — we need to rewrite its operands */
    uint32_t jif_opbase = jif->operand_base;

    /* Save ASSIGN operands */
    uint32_t assign_opbases[8];
    for (int j = 0; j < num_assigns; j++)
      assign_opbases[j] = ir->compact_instructions[i + 1 + j].operand_base;

    /* Rewrite in place:
     * [i..i+num_assigns-1] become the ASSIGNs
     * [i+num_assigns] becomes the inverted JUMPIF */

    for (int j = 0; j < num_assigns; j++) {
      IRQuadCompact *q = &ir->compact_instructions[i + j];
      q->op = TCCIR_OP_ASSIGN;
      q->operand_base = assign_opbases[j];
      q->is_jump_target = (j == 0) ? jif->is_jump_target : 0;
    }

    /* Write inverted JUMPIF using the original JUMPIF's operand pool slot */
    {
      int jif_pos = i + num_assigns;
      IRQuadCompact *q = &ir->compact_instructions[jif_pos];
      q->op = TCCIR_OP_JUMPIF;
      q->operand_base = jif_opbase;
      q->is_jump_target = 0;
      IROperand dest_op = {0};
      dest_op.tag = IROP_TAG_IMM32;
      dest_op.u.imm32 = body_target;
      tcc_ir_op_set_dest(ir, q, dest_op);
      IROperand cond_op = {0};
      cond_op.tag = IROP_TAG_IMM32;
      cond_op.u.imm32 = inv_cond;
      tcc_ir_op_set_src1(ir, q, cond_op);
    }

    /* Redirect any OTHER jump that targeted the original fall-through path
     * [i+1 .. jump_idx] straight to body_target before we rewrite those slots.
     * That whole region was "(coalesced no-op ASSIGNs); JUMP body_target", so
     * entering it anywhere meant "go to body_target".  The rewrite repurposes
     * those slots (ASSIGNs shifted up, inverted JUMPIF at jif_pos, JUMP→NOP),
     * so a stale target pointing into the region would land on the inverted
     * JUMPIF and re-use its comparison flags — the `if (A || B)` short-circuit
     * bug where A's equality branch (which jumped to this continue/merge path)
     * ends up on B's relational branch.  Skip the pattern's own slots. */
    for (int k = 0; k < n; k++) {
      if (k >= i && k <= jump_idx)
        continue;
      IRQuadCompact *kq = &ir->compact_instructions[k];
      if (kq->op != TCCIR_OP_JUMP && kq->op != TCCIR_OP_JUMPIF)
        continue;
      int kt = (int)irop_get_imm32(tcc_ir_op_get_dest(ir, kq));
      if (kt < i + 1 || kt > jump_idx)
        continue;
      IROperand kd = {0};
      kd.tag = IROP_TAG_IMM32;
      kd.u.imm32 = body_target;
      tcc_ir_op_set_dest(ir, kq, kd);
    }

    /* NOP the old unconditional JUMP */
    ir->compact_instructions[jump_idx].op = TCCIR_OP_NOP;

    /* Update is_jump_target: body_target is now targeted by the new JUMPIF */
    if (body_target >= 0 && body_target < n)
      ir->compact_instructions[body_target].is_jump_target = 1;

    changes++;
  }

  return changes;
}

/* Detect a guarded noreturn-call site whose guard JUMPIF is at index `i`:
 *
 *   i:        JUMPIF cond -> CONT      ; cond is the "good" test; jumps OVER
 *   i+1..j-1: FUNCPARAMVOID*           ;   the call (0+ void params; abort()
 *   j:        FUNCCALL* callee         ;   has none real, argc == 0, noreturn;
 *                                          void or value form (dead return))
 *   CONT=j+1: <continue>
 *
 * On a match, fills *call_idx (= j), *cond, *callee and *is_zero (the inline
 * form fuses into cbz/cbnz: the preceding flag-setter is a compare-against-zero
 * and the guard is EQ/NE), and returns the guard's entry index (i+1, i.e. the
 * first param, or the call when there is no param).  Returns -1 on no match.
 *
 * The CONT == j+1 check makes this a clean guarded diamond and excludes loop
 * latch / entry-guard JUMPIFs (not followed by a noreturn call).  The region
 * must have no other predecessor so redirecting/NOPing it loses no edge. */
static int ir_abort_guard_site(TCCIRState *ir, int i, int n, int *call_idx, int *cond_out,
                               Sym **callee_out, int *is_zero)
{
  IRQuadCompact *jif = &ir->compact_instructions[i];
  if (jif->op != TCCIR_OP_JUMPIF)
    return -1;

  int cont = (int)irop_get_imm32(tcc_ir_op_get_dest(ir, jif));
  int cond = (int)tcc_ir_op_get_src1(ir, jif).u.imm32;

  int j = i + 1;
  while (j < n && ir->compact_instructions[j].op == TCCIR_OP_FUNCPARAMVOID)
    j++;
  if (j >= n)
    return -1;
  /* Accept both call forms: a void call, or a value call whose return is dead
   * (a noreturn callee never returns, so its FUNCCALLVAL dest vreg is dead —
   * __builtin_abort lowers to FUNCCALLVAL).  The operand accessors are
   * config-aware (they offset src1/src2 past the dest), so callee/argc read
   * correctly for either form. */
  TccIrOp callop = ir->compact_instructions[j].op;
  if (callop != TCCIR_OP_FUNCCALLVOID && callop != TCCIR_OP_FUNCCALLVAL)
    return -1;

  IRQuadCompact *call = &ir->compact_instructions[j];
  Sym *callee = irop_get_sym_ex(ir, tcc_ir_op_get_src1(ir, call));
  if (!tcc_ir_callee_is_noreturn(callee))
    return -1;
  if (TCCIR_DECODE_CALL_ARGC((uint32_t)irop_get_imm64_ex(ir, tcc_ir_op_get_src2(ir, call))) != 0)
    return -1;
  if (cont != j + 1)
    return -1;
  /* A jump landing INSIDE the param/call sequence (i+2..j) can't be redirected
   * safely, so reject it.  The abort ENTRY (i+1) itself MAY be a jump target:
   * in `if (A || B) abort()` the A-branch jumps straight to the abort.  The
   * merge retargets such predecessors to the shared sink — every edge into an
   * argc==0 noreturn site is interchangeable with an edge into another site for
   * the same callee (both just execute `<no args>; call noreturn`). */
  for (int k = i + 2; k <= j; k++)
    if (ir->compact_instructions[k].is_jump_target)
      return -1;

  /* cbz/cbnz-eligible inline form: preceding flag-setter compares against 0
   * and the guard is EQ/NE.  Such a site is cheap inline (one fused cbz) but
   * loses the fusion if inverted to a (often backward) branch — so it makes the
   * best shared sink to keep inline. */
  int zero = 0;
  if (i >= 1 && (cond == TOK_EQ || cond == TOK_NE))
  {
    IRQuadCompact *prev = &ir->compact_instructions[i - 1];
    if (prev->op == TCCIR_OP_TEST_ZERO)
      zero = 1;
    else if (prev->op == TCCIR_OP_CMP)
    {
      IROperand s2 = tcc_ir_op_get_src2(ir, prev);
      if (s2.tag == IROP_TAG_IMM32 && s2.u.imm32 == 0)
        zero = 1;
    }
  }

  *call_idx = j;
  *cond_out = cond;
  *callee_out = callee;
  *is_zero = zero;
  return i + 1;
}

/* Abort tail-merge + body-invert (post-regalloc).
 *
 * memclr-style check loops each guard a noreturn call (abort): good path
 * `beq.w CONT` jumps over an inline `bl abort`, bad path falls into it.  GCC
 * instead keeps ONE shared abort and makes each check `cmp; bne SHARED` with
 * the good path falling straight through.
 *
 * We reach static parity by choosing, per distinct noreturn callee, ONE site as
 * the shared sink and leaving it inline.  Every OTHER site for that callee is
 * inverted (cond -> !cond), retargeted to the sink's entry, and its local
 * param+call NOPed.  The good path then falls through the NOPs to CONT; the bad
 * path branches to the one shared `bl abort`.
 *
 * Sink choice: prefer a cbz/cbnz-eligible (compare-against-zero) site.  Inline
 * it costs one fused cbz; inverting it would cost cmp+bne (cbz/cbnz are
 * forward-only, so a backward branch to the sink cannot fuse) — net zero saving.
 * A non-zero site, by contrast, is cmp+beq+bl_abort inline vs cmp+bne inverted,
 * saving one `bl abort`.  So keeping a zero site inline and inverting the
 * non-zero ones maximises the merge (memclr's three loops are nonzero/zero/
 * nonzero: keep the middle, invert the outer two -> two `bl abort` removed).
 *
 * Why a kept-inline site is a safe sink: it retains its original
 * fall-through-into-call layout, so nothing else falls through *into* the sink;
 * other sites reach it solely via their inverted conditional branch.  abort is
 * argc==0, so no register/stack arg setup differs between sites — branching into
 * the sink's param->call sequence is sound regardless of the source edge.
 *
 * Runs post-regalloc (operands already physical; the sink references no vregs so
 * coalescing is irrelevant to it) and BEFORE the jump-threading / eliminate-
 * fallthrough / reachability-DCE cleanup that tidies the NOPs.  Like the
 * neighbouring post-RA peepholes we do NOT compact_nops — renumbering would
 * perturb downstream index-keyed peepholes.
 *
 * Disabled by TCC_NO_ABORT_MERGE.  Bails on IJUMP / SWITCH_TABLE / SWITCH_LOAD:
 * their edges are not statically enumerable and could reach into a guarded
 * region we assume is entered only via fall-through from its JUMPIF. */
int tcc_ir_opt_abort_tail_merge(TCCIRState *ir)
{
  if (getenv("TCC_NO_ABORT_MERGE"))
    return 0;

  int n = ir->next_instruction_index;
  if (n < 3)
    return 0;

  /* Gate: un-enumerable control flow could branch into a guarded region. */
  for (int i = 0; i < n; i++)
  {
    TccIrOp op = ir->compact_instructions[i].op;
    if (op == TCCIR_OP_IJUMP || op == TCCIR_OP_SWITCH_TABLE || op == TCCIR_OP_SWITCH_LOAD)
      return 0;
  }

  /* Per-distinct-callee sink choice.  Few functions have more than one noreturn
   * callee; cap the table and just skip merging beyond it. */
  enum { MAX_SINKS = 8 };
  Sym *cal[MAX_SINKS];
  int first_entry[MAX_SINKS]; /* entry of the first site (fallback sink) */
  int zero_entry[MAX_SINKS];  /* entry of the first cbz-eligible site, or -1 */
  int num = 0;

  int j, cond, is_zero;
  Sym *callee;

  /* Pass 1: discover sites and, per callee, remember the first site and the
   * first cbz-eligible site. */
  for (int i = 0; i + 1 < n; i++)
  {
    if (ir_abort_guard_site(ir, i, n, &j, &cond, &callee, &is_zero) < 0)
      continue;
    int s = -1;
    for (int t = 0; t < num; t++)
      if (cal[t] == callee)
      {
        s = t;
        break;
      }
    if (s < 0)
    {
      if (num >= MAX_SINKS)
        continue;
      s = num++;
      cal[s] = callee;
      first_entry[s] = i + 1;
      zero_entry[s] = -1;
    }
    if (is_zero && zero_entry[s] < 0)
      zero_entry[s] = i + 1;
  }
  if (num == 0)
    return 0;

  /* Pass 2: invert + retarget every non-sink site to its callee's chosen sink,
   * NOPing the duplicate param+call.  Mark touched sinks for pass 3. */
  int sink_used[MAX_SINKS] = {0};
  int changes = 0;
  for (int i = 0; i + 1 < n; i++)
  {
    int entry = ir_abort_guard_site(ir, i, n, &j, &cond, &callee, &is_zero);
    if (entry < 0)
      continue;
    int s = -1;
    for (int t = 0; t < num; t++)
      if (cal[t] == callee)
      {
        s = t;
        break;
      }
    if (s < 0)
      continue; /* callee overflowed MAX_SINKS */

    int sink = (zero_entry[s] >= 0) ? zero_entry[s] : first_entry[s];
    if (entry == sink)
      continue; /* this IS the sink — leave it inline */

    int inv = invert_condition(cond);
    if (inv < 0)
      continue;

    /* `if (A || B) abort()`: the A-branch jumps straight to this site's abort
     * entry.  Redirect every such predecessor to the shared sink before NOPing
     * the local call (otherwise A-true would fall through the NOPs to CONT and
     * skip the abort).  Safe because both entries run the same argc==0 noreturn
     * callee.  Then clear the (now-unreferenced) entry's jump-target flag. */
    if (ir->compact_instructions[entry].is_jump_target)
    {
      for (int p = 0; p < n; p++)
      {
        IRQuadCompact *pq = &ir->compact_instructions[p];
        if (pq->op != TCCIR_OP_JUMP && pq->op != TCCIR_OP_JUMPIF)
          continue;
        IROperand pd = tcc_ir_op_get_dest(ir, pq);
        if (pd.tag != IROP_TAG_IMM32 || pd.u.imm32 != entry)
          continue;
        IROperand sink_dest = {0};
        sink_dest.tag = IROP_TAG_IMM32;
        sink_dest.u.imm32 = sink;
        tcc_ir_op_set_dest(ir, pq, sink_dest);
      }
      ir->compact_instructions[entry].is_jump_target = 0;
    }

    IRQuadCompact *jif = &ir->compact_instructions[i];
    IROperand new_cond = {0};
    new_cond.tag = IROP_TAG_IMM32;
    new_cond.u.imm32 = inv;
    tcc_ir_op_set_src1(ir, jif, new_cond);

    IROperand new_dest = {0};
    new_dest.tag = IROP_TAG_IMM32;
    new_dest.u.imm32 = sink;
    tcc_ir_op_set_dest(ir, jif, new_dest);

    for (int k = i + 1; k <= j; k++)
      ir->compact_instructions[k].op = TCCIR_OP_NOP;

    sink_used[s] = 1;
    changes++;
  }

  /* Pass 3: flag the entry of each sink that actually received a branch.  We
   * defer this so the pass-2 re-detection of sites stays clean (marking a sink
   * mid-scan would make ir_abort_guard_site reject it via its no-other-pred
   * check before the entry==sink test could leave it inline). */
  for (int s = 0; s < num; s++)
    if (sink_used[s])
    {
      int sink = (zero_entry[s] >= 0) ? zero_entry[s] : first_entry[s];
      ir->compact_instructions[sink].is_jump_target = 1;
    }

  return changes;
}

int tcc_ir_opt_var_to_tmp_ex(IROptCtx *ctx) { return tcc_ir_opt_var_to_tmp(ctx->ir); }
int tcc_ir_opt_redundant_loop_check_ex(IROptCtx *ctx) { return tcc_ir_opt_redundant_loop_check(ctx->ir); }
