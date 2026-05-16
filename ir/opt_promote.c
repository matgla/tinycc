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

/* TMP Constant Propagation
 * After constant folding may create TMP <- #const instructions,
 * propagate these constants to uses of the TMP within the same basic block.
 *
 * Performance: Uses generation counters for O(1) block clears instead of memset.
 * Stack buffers avoid malloc for small functions.
 */

int tcc_ir_opt_var_tmp_fwd(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  int changes = 0;

  if (n < 2)
    return 0;

  LOG_IR_GEN("=== VAR→TMP FWD START ===");

  /* Pre-compute a fresh jump-target bitmap instead of relying on stale
   * IRQuadCompact::is_jump_target flags that can desync between passes.
   * A back-edge into the scan window — including into a NOP that later
   * falls through to a real use — must terminate forwarding, because on
   * the next loop iteration V may hold a different value. */
  uint8_t *is_target = tcc_mallocz((size_t)((n + 7) / 8));
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *jq = &ir->compact_instructions[i];
    if (jq->op == TCCIR_OP_JUMP || jq->op == TCCIR_OP_JUMPIF)
    {
      IROperand d = tcc_ir_op_get_dest(ir, jq);
      int t = (int)d.u.imm32;
      if (t >= 0 && t < n)
        is_target[t >> 3] |= (uint8_t)(1u << (t & 7));
    }
  }
  for (int tbl = 0; tbl < ir->num_switch_tables; tbl++)
  {
    TCCIRSwitchTable *st = &ir->switch_tables[tbl];
    for (int k = 0; k < st->num_entries; k++)
    {
      int t = st->targets[k];
      if (t >= 0 && t < n)
        is_target[t >> 3] |= (uint8_t)(1u << (t & 7));
    }
    if (st->default_target >= 0 && st->default_target < n)
      is_target[st->default_target >> 3] |= (uint8_t)(1u << (st->default_target & 7));
  }

  /* Pre-compute VAR use counts: when the STORE source is a DEREF
   * (is_lval=1), forwarding V → *T duplicates the memory read at every
   * remaining use site.  Only safe when V has a single use total (the
   * one being rewritten — V then becomes dead and the STORE is DCE'd).
   * Counts uses of any VAR as a src operand across the whole IR. */
  int max_var_for_use = ir->next_local_variable;
  int *var_use_count = NULL;
  if (max_var_for_use > 0)
    var_use_count = tcc_mallocz(max_var_for_use * sizeof(int));
  for (int i = 0; i < n && var_use_count; i++)
  {
    IRQuadCompact *vq = &ir->compact_instructions[i];
    if (vq->op == TCCIR_OP_NOP)
      continue;
    int nops = irop_config[vq->op].has_src1 + irop_config[vq->op].has_src2;
    for (int oi = 0; oi < nops; oi++)
    {
      IROperand s = oi == 0 ? tcc_ir_op_get_src1(ir, vq) : tcc_ir_op_get_src2(ir, vq);
      int32_t vr = irop_get_vreg(s);
      if (vr < 0 || TCCIR_DECODE_VREG_TYPE(vr) != TCCIR_VREG_TYPE_VAR)
        continue;
      int pos = TCCIR_DECODE_VREG_POSITION(vr);
      if (pos < max_var_for_use)
        var_use_count[pos]++;
    }
  }

  /* Pre-compute aliasing info for the addrtaken refinement:
   * - has_nested_or_chain: this function contains a SET_CHAIN op (i.e.
   *   it actually calls a nested function that may read its locals via
   *   the static chain).  Mere existence of nested functions in the
   *   program is not enough — gen_function clears per-VAR addrtaken for
   *   captures whose nested callees were all inlined.
   * - var_has_lea[pos]: per-VAR LEA bitmap */
  int has_nested_or_chain = 0;
  int max_var_for_lea = 0;
  uint8_t *var_has_lea = NULL;
  if (!has_nested_or_chain)
  {
    for (int i = 0; i < n; i++)
    {
      IRQuadCompact *sq = &ir->compact_instructions[i];
      if (sq->op == TCCIR_OP_SET_CHAIN || sq->op == TCCIR_OP_INIT_CHAIN_SLOT)
      {
        has_nested_or_chain = 1;
        break;
      }
      if (sq->op == TCCIR_OP_LEA)
      {
        IROperand ls = tcc_ir_op_get_src1(ir, sq);
        int32_t vr = irop_get_vreg(ls);
        if (vr >= 0 && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_VAR)
        {
          int pos = TCCIR_DECODE_VREG_POSITION(vr);
          if (pos > max_var_for_lea)
            max_var_for_lea = pos;
        }
      }
    }
    if (max_var_for_lea > 0 && !has_nested_or_chain)
    {
      var_has_lea = tcc_mallocz((max_var_for_lea + 8) / 8);
      for (int i = 0; i < n; i++)
      {
        IRQuadCompact *sq = &ir->compact_instructions[i];
        if (sq->op == TCCIR_OP_LEA)
        {
          IROperand ls = tcc_ir_op_get_src1(ir, sq);
          int32_t vr = irop_get_vreg(ls);
          if (vr >= 0 && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_VAR)
          {
            int pos = TCCIR_DECODE_VREG_POSITION(vr);
            if (pos <= max_var_for_lea)
              var_has_lea[pos / 8] |= (1 << (pos % 8));
          }
        }
      }
    }
  }

  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *store_q = &ir->compact_instructions[i];
    if (store_q->op != TCCIR_OP_STORE && store_q->op != TCCIR_OP_ASSIGN)
      continue;
    if (!irop_config[store_q->op].has_dest || !irop_config[store_q->op].has_src1)
      continue;

    IROperand dest = tcc_ir_op_get_dest(ir, store_q);
    int32_t dest_vr = irop_get_vreg(dest);
    if (TCCIR_DECODE_VREG_TYPE(dest_vr) != TCCIR_VREG_TYPE_VAR)
      continue;

    /* Skip address-taken VARs if there is an actual aliasing path:
     * LEA in the IR, nested function definitions, or SET_CHAIN.
     * Without these, addrtaken is a stale frontend annotation. */
    {
      IRLiveInterval *interval = tcc_ir_get_live_interval(ir, dest_vr);
      if (interval && interval->addrtaken)
      {
        int dpos = TCCIR_DECODE_VREG_POSITION(dest_vr);
        if (has_nested_or_chain ||
            (var_has_lea && dpos <= max_var_for_lea && (var_has_lea[dpos / 8] & (1 << (dpos % 8)))))
          continue;
      }
    }

    IROperand src1 = tcc_ir_op_get_src1(ir, store_q);
    int32_t src_vr = irop_get_vreg(src1);
    if (TCCIR_DECODE_VREG_TYPE(src_vr) != TCCIR_VREG_TYPE_TEMP)
      continue;

    /* DEREF source guard: forwarding V → *T duplicates the load at every
     * use site.  Only beneficial when V has exactly one use (which we're
     * about to rewrite), making V dead and DCE'ing the STORE.  Multiple
     * uses → substituting one reintroduces the load there without removing
     * the STORE the other uses still need.  Pattern from inlined check1:
     *   V <- *T [STORE]        \
     *   CMP got, V              -- if both rewritten, V dies. But if
     *   PARAM3 V (outside BB)  /   PARAM3 stays, the substitution at CMP
     *                              adds a redundant ldr without payoff. */
    if (src1.is_lval && var_use_count)
    {
      int dpos = TCCIR_DECODE_VREG_POSITION(dest_vr);
      if (dpos >= 0 && dpos < max_var_for_use && var_use_count[dpos] > 1)
        continue;
    }

    /* Don't forward TEMPs that hold a computed stack/symbol ADDRESS (from
     * LEA / Addr[...]).  Even when V is single-use, removing the VAR that
     * held the address breaks downstream DSE / store_redundant analyses —
     * they rely on the VAR-holding-address pattern to know which stack slot
     * is live, and without V they can drop the stores the slot requires.
     * The few VAR→TMP wins we'd get from LEA-sourced chains aren't worth
     * the risk of silently corrupting programs that take the address of a
     * local variable. */
    {
      int t_def = tcc_ir_find_defining_instruction(ir, src_vr, i);
      if (t_def >= 0 && ir->compact_instructions[t_def].op == TCCIR_OP_LEA)
        continue;
    }

    int src_btype = irop_get_btype(src1);

    for (int j = i + 1; j < n; j++)
    {
      IRQuadCompact *q = &ir->compact_instructions[j];

      /* BB boundary check first: a jump-target instruction may be a NOP
       * whose next real instruction is NOT itself a jump target, so we
       * cannot defer this check until after NOP-skipping. */
      if (is_target[j >> 3] & (1u << (j & 7)))
        break;

      if (q->op == TCCIR_OP_NOP)
        continue;
      /* Skip unconditional JMPs that target the next non-NOP instruction
       * (fallthroughs left by earlier branch elimination + DCE). */
      if (q->op == TCCIR_OP_JUMP)
      {
        IROperand jd = tcc_ir_op_get_dest(ir, q);
        int jt = (int)jd.u.imm32;
        while (jt < n && ir->compact_instructions[jt].op == TCCIR_OP_NOP)
          jt++;
        int next_real = j + 1;
        while (next_real < n && ir->compact_instructions[next_real].op == TCCIR_OP_NOP)
          next_real++;
        if (jt == next_real)
          continue; /* fallthrough JMP — skip it */
      }
      if (q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_JUMPIF || q->op == TCCIR_OP_RETURNVALUE ||
          q->op == TCCIR_OP_RETURNVOID || q->op == TCCIR_OP_IJUMP || q->op == TCCIR_OP_SWITCH_TABLE)
        break;
      /* Calls clobber caller-saved TEMP registers — extending T across a call
       * would force a spill, defeating the purpose. */
      if (q->op == TCCIR_OP_FUNCCALLVAL || q->op == TCCIR_OP_FUNCCALLVOID)
        break;

      /* Detect a redefinition of T (rare for TEMPs, but guard). */
      if (irop_config[q->op].has_dest)
      {
        IROperand d = tcc_ir_op_get_dest(ir, q);
        int32_t d_vr = irop_get_vreg(d);
        if (d_vr == src_vr)
          break;
      }

      /* Substitute V → T in readable src positions.  Skip operand slots that
       * the op uses to encode non-value metadata (callee sym, call id). */
      int can_rewrite_src1 = (q->op != TCCIR_OP_FUNCCALLVAL && q->op != TCCIR_OP_FUNCCALLVOID);
      int can_rewrite_src2 = (q->op != TCCIR_OP_FUNCPARAMVAL && q->op != TCCIR_OP_FUNCPARAMVOID &&
                              q->op != TCCIR_OP_FUNCCALLVAL && q->op != TCCIR_OP_FUNCCALLVOID);

      /* Ops where src1 represents an *address* (not a value to read).  For
       * these we must not rewrite an is_lval=1 VAR source to an is_lval=0
       * TEMP, because the TEMP holds the value, not an address.  Pure value
       * ops (FUNCPARAMVAL, CMP, arithmetic, etc.) safely accept the TEMP —
       * the VAR read was just a deref of V's slot, which now holds T. */
      int src1_is_address = (q->op == TCCIR_OP_LOAD || q->op == TCCIR_OP_LOAD_POSTINC || q->op == TCCIR_OP_LEA ||
                             q->op == TCCIR_OP_LOAD_INDEXED);

      if (!src1_is_address && can_rewrite_src1 && irop_config[q->op].has_src1)
      {
        IROperand s = tcc_ir_op_get_src1(ir, q);
        if (irop_get_vreg(s) == dest_vr && irop_get_btype(s) == src_btype && !s.is_llocal)
        {
          tcc_ir_set_src1(ir, j, src1);
          changes++;
          LOG_IR_GEN("VAR→TMP FWD: V=%d -> T at j=%d (src1) after store at i=%d", dest_vr, j, i);
        }
      }
      /* src2 is a value for every op in this table (indices for LOAD_INDEXED
       * and STORE_INDEXED are values, not addresses). */
      if (can_rewrite_src2 && irop_config[q->op].has_src2)
      {
        IROperand s = tcc_ir_op_get_src2(ir, q);
        if (irop_get_vreg(s) == dest_vr && irop_get_btype(s) == src_btype && !s.is_llocal)
        {
          tcc_ir_set_src2(ir, j, src1);
          changes++;
          LOG_IR_GEN("VAR→TMP FWD: V=%d -> T at j=%d (src2) after store at i=%d", dest_vr, j, i);
        }
      }

      /* If this op wrote V (dest == V), subsequent reads see the new value —
       * stop forwarding the old one. */
      if (irop_config[q->op].has_dest)
      {
        IROperand d = tcc_ir_op_get_dest(ir, q);
        int32_t d_vr = irop_get_vreg(d);
        if (d_vr == dest_vr)
          break;
      }
    }
  }

  tcc_free(var_has_lea);
  tcc_free(var_use_count);
  tcc_free(is_target);
  LOG_IR_GEN("=== VAR→TMP FWD END: %d substitutions ===", changes);
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
int tcc_ir_opt_select(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  int changes = 0;

  /* Smallest pattern is the RETURN diamond: JUMPIF + RETURN + RETURN = 3 instr.
   * Other patterns (ASSIGN/CALL diamond) need more, but their inner checks
   * already bail out when the remaining suffix is too short. */
  if (n < 3)
    return 0;

  /* Precompute jump target counts: jt_cnt[target] = number of JUMP/JUMPIFs
   * targeting `target`. Lets ir_has_other_jump_to_fast (below) answer in O(1)
   * what would otherwise be an O(n) scan per query — opt_select calls the
   * predicate four times per JUMPIF, which on a function with thousands of
   * branches drives the pass into O(n^2). Decrement the count whenever we
   * NOP a JUMP/JUMPIF below to keep it consistent. */
  int *jt_cnt = tcc_mallocz(sizeof(int) * n);
  for (int j = 0; j < n; j++) {
    IRQuadCompact *q = &ir->compact_instructions[j];
    if (q->op != TCCIR_OP_JUMP && q->op != TCCIR_OP_JUMPIF)
      continue;
    IROperand d = tcc_ir_op_get_dest(ir, q);
    int t = (int)irop_get_imm64_ex(ir, d);
    if (t >= 0 && t < n)
      jt_cnt[t]++;
  }
  #define JT_HAS_OTHER(target, exclude_idx) \
    ir_has_other_jump_to_fast(ir, jt_cnt, (target), (exclude_idx))
  #define JT_NOP_JUMP(idx) do { \
    IRQuadCompact *_jq = &ir->compact_instructions[idx]; \
    if (_jq->op == TCCIR_OP_JUMP || _jq->op == TCCIR_OP_JUMPIF) { \
      IROperand _jd = tcc_ir_op_get_dest(ir, _jq); \
      int _jt = (int)irop_get_imm64_ex(ir, _jd); \
      if (_jt >= 0 && _jt < n && jt_cnt[_jt] > 0) jt_cnt[_jt]--; \
    } \
    _jq->op = TCCIR_OP_NOP; \
  } while (0)

  for (int i = 0; i < n - 2; i++)
  {
    IRQuadCompact *jumpif_q = &ir->compact_instructions[i];
    if (jumpif_q->op != TCCIR_OP_JUMPIF)
      continue;

    /* Get JUMPIF operands: dest=else_target, src1=condition */
    IROperand jumpif_dest = tcc_ir_op_get_dest(ir, jumpif_q);
    IROperand jumpif_cond = tcc_ir_op_get_src1(ir, jumpif_q);
    int branch_cond = (int)irop_get_imm64_ex(ir, jumpif_cond);
    int else_target = (int)irop_get_imm64_ex(ir, jumpif_dest);

    /* Normalize `JUMPIF C → A; JUMP → B` into `JUMPIF !C → B` when A is the
     * instruction immediately following the JUMP.  Without this, a ternary
     * lowering whose true-path falls through and false-path is an
     * unconditional JUMP keeps an extra branch that defeats the diamond
     * patterns below.  Conditions: the JUMP must be unconditional, nothing
     * else targets its position, and the original else_target (A) must be
     * the next real instruction after the JUMP. */
    if (else_target >= 0 && else_target < n) {
      int next_nop = ir_skip_nops_forward(ir, i + 1, n);
      if (next_nop < n) {
        IRQuadCompact *next_q = &ir->compact_instructions[next_nop];
        if (next_q->op == TCCIR_OP_JUMP &&
            !JT_HAS_OTHER(next_nop, -1) &&
            ir_skip_nops_forward(ir, next_nop + 1, n) == else_target) {
          IROperand new_target = tcc_ir_op_get_dest(ir, next_q);
          int new_else_target = (int)irop_get_imm64_ex(ir, new_target);
          if (new_else_target >= 0 && new_else_target < n) {
            jt_cnt[else_target]--;
            jt_cnt[new_else_target]++;
            tcc_ir_op_set_dest(ir, jumpif_q, new_target);
            IROperand new_cond = irop_make_imm32(-1, ir_negate_condition(branch_cond), VT_INT);
            tcc_ir_op_set_src1(ir, jumpif_q, new_cond);
            JT_NOP_JUMP(next_nop);
            if (!JT_HAS_OTHER(else_target, -1))
              ir->compact_instructions[else_target].is_jump_target = 0;
            branch_cond = ir_negate_condition(branch_cond);
            else_target = new_else_target;
            changes++;
          }
        }
      }
    }

    /* The "then" condition is the negation of the branch condition
     * (branch jumps to else when cond is true, so then runs when !cond) */
    int then_cond = ir_negate_condition(branch_cond);

    /* Scan forward past NOPs to find the then-block start */
    int then_start = ir_skip_nops_forward(ir, i + 1, n);
    if (then_start >= n)
      continue;

    /* Safety: the then-block (fall-through) must not be a jump target from
     * elsewhere, and the else-block must only be targeted by this JUMPIF.
     * Otherwise NOP'ing the blocks would break other control flow. */
    if (JT_HAS_OTHER(then_start, i))
      continue;
    if (JT_HAS_OTHER(else_target, i))
      continue;

    /* ----------------------------------------------------------------
     * Pattern: Call diamond (PARAM+CALL in both branches)
     * ----------------------------------------------------------------
     * then: PARAM0[call_A] val1, CALL func
     * JUMP to merge
     * else: PARAM0[call_B] val2, CALL func
     * merge: ...
     * ---------------------------------------------------------------- */
    IRQuadCompact *then_q1 = &ir->compact_instructions[then_start];
    if (then_q1->op == TCCIR_OP_FUNCPARAMVAL || then_q1->op == TCCIR_OP_FUNCPARAMVOID)
    {
      /* Check if next non-NOP is a FUNCCALLVOID */
      int then_call_idx = ir_skip_nops_forward(ir, then_start + 1, n);
      if (then_call_idx >= n)
        continue;
      IRQuadCompact *then_call_q = &ir->compact_instructions[then_call_idx];
      if (then_call_q->op != TCCIR_OP_FUNCCALLVOID)
        continue;

      /* Next should be unconditional JUMP to merge */
      int jump_idx = ir_skip_nops_forward(ir, then_call_idx + 1, n);
      if (jump_idx >= n)
        continue;
      IRQuadCompact *jump_q = &ir->compact_instructions[jump_idx];
      if (jump_q->op != TCCIR_OP_JUMP)
        continue;
      int merge_target = (int)irop_get_imm64_ex(ir, tcc_ir_op_get_dest(ir, jump_q));

      /* else_target should point to the else block */
      if (else_target < 0 || else_target >= n)
        continue;

      /* Find else block start (skip NOPs) */
      int else_start = ir_skip_nops_forward(ir, else_target, n);
      if (else_start >= n)
        continue;

      /* Else block: PARAM + CALL same function */
      IRQuadCompact *else_q1 = &ir->compact_instructions[else_start];
      if (else_q1->op != TCCIR_OP_FUNCPARAMVAL && else_q1->op != TCCIR_OP_FUNCPARAMVOID)
        continue;

      int else_call_idx = ir_skip_nops_forward(ir, else_start + 1, n);
      if (else_call_idx >= n)
        continue;
      IRQuadCompact *else_call_q = &ir->compact_instructions[else_call_idx];
      if (else_call_q->op != TCCIR_OP_FUNCCALLVOID)
        continue;

      /* Verify both calls target the same function */
      Sym *then_callee = irop_get_sym_ex(ir, tcc_ir_op_get_src1(ir, then_call_q));
      Sym *else_callee = irop_get_sym_ex(ir, tcc_ir_op_get_src1(ir, else_call_q));
      if (!then_callee || !else_callee || then_callee != else_callee)
        continue;

      /* Verify both params are param index 0 for their respective calls */
      IROperand then_param_enc = tcc_ir_op_get_src2(ir, then_q1);
      IROperand else_param_enc = tcc_ir_op_get_src2(ir, else_q1);
      int then_param_idx = TCCIR_DECODE_PARAM_IDX((uint32_t)irop_get_imm64_ex(ir, then_param_enc));
      int else_param_idx = TCCIR_DECODE_PARAM_IDX((uint32_t)irop_get_imm64_ex(ir, else_param_enc));
      if (then_param_idx != 0 || else_param_idx != 0)
        continue;

      /* Both calls have 1 argument (CALL #N where argc from encoded src2) */
      IROperand then_call_meta = tcc_ir_op_get_src2(ir, then_call_q);
      IROperand else_call_meta = tcc_ir_op_get_src2(ir, else_call_q);
      int then_argc = TCCIR_DECODE_CALL_ARGC((uint32_t)irop_get_imm64_ex(ir, then_call_meta));
      int else_argc = TCCIR_DECODE_CALL_ARGC((uint32_t)irop_get_imm64_ex(ir, else_call_meta));
      if (then_argc != 1 || else_argc != 1)
        continue;

      /* Verify merge_target is right after the else CALL (or after NOPs) */
      int after_else_call = ir_skip_nops_forward(ir, else_call_idx + 1, n);
      if (after_else_call != merge_target && else_call_idx + 1 != merge_target)
      {
        /* Also accept if merge_target equals the instruction after else_call directly */
        int next_real = ir_skip_nops_forward(ir, else_call_idx + 1, n);
        if (next_real != merge_target)
          continue;
      }

      /* Get the differing parameter values */
      IROperand then_val = tcc_ir_op_get_src1(ir, then_q1);
      IROperand else_val = tcc_ir_op_get_src1(ir, else_q1);

      /* Both must be compile-time constants (SYMREF or IMM32) — no vreg uses
       * since the vreg lifetimes would be disrupted by removing the branches */
      int then_tag = irop_get_tag(then_val);
      int else_tag = irop_get_tag(else_val);
      if (then_tag != IROP_TAG_SYMREF && then_tag != IROP_TAG_IMM32)
        continue;
      if (else_tag != IROP_TAG_SYMREF && else_tag != IROP_TAG_IMM32)
        continue;

      /* ---- Transform ---- */

      /* Create a new temporary vreg for the SELECT result */
      int32_t select_vreg = tcc_ir_get_vreg_temp(ir);

      /* Allocate 4 pool entries for SELECT: dest, src1(then), src2(else), cond */
      IROperand sel_dest = irop_make_vreg(select_vreg, IROP_BTYPE_INT32);

      IROperand sel_cond = irop_make_imm32(-1, then_cond, VT_INT);

      int pool_base = tcc_ir_iroperand_pool_add(ir, sel_dest);
      tcc_ir_iroperand_pool_add(ir, then_val);
      tcc_ir_iroperand_pool_add(ir, else_val);
      tcc_ir_iroperand_pool_add(ir, sel_cond);

      /* Rewrite the JUMPIF as SELECT (drops one jump to else_target) */
      if (else_target >= 0 && else_target < n && jt_cnt[else_target] > 0)
        jt_cnt[else_target]--;
      jumpif_q->op = TCCIR_OP_SELECT;
      jumpif_q->operand_base = pool_base;

      /* Rewrite the then PARAM to use the SELECT result vreg,
       * and keep it pointing to the else call (which we'll keep) */
      /* Actually, we need to rewrite the else_q1 (PARAM) to use the
       * SELECT vreg as its value, then NOP the then-block entirely */

      /* Rewrite else PARAM0 to use the SELECT result */
      IROperand new_param_val = irop_make_vreg(select_vreg, IROP_BTYPE_INT32);
      tcc_ir_op_set_src1(ir, else_q1, new_param_val);

      /* NOP the then-block: then_param, then_call, unconditional jump */
      ir->compact_instructions[then_start].op = TCCIR_OP_NOP;
      ir->compact_instructions[then_call_idx].op = TCCIR_OP_NOP;
      JT_NOP_JUMP(jump_idx);

      /* Clear stale is_jump_target on positions no longer targeted
       * (the NOPed JUMP no longer jumps, so re-check with no exclusion) */
      if (!JT_HAS_OTHER(else_target, -1))
        ir->compact_instructions[else_target].is_jump_target = 0;
      if (merge_target >= 0 && merge_target < n && !JT_HAS_OTHER(merge_target, -1))
        ir->compact_instructions[merge_target].is_jump_target = 0;

      changes++;
      continue;
    }

    /* ----------------------------------------------------------------
     * Pattern: Simple ASSIGN diamond
     * ----------------------------------------------------------------
     * then: dest <-- val1 [ASSIGN, or LOAD of an immediate]
     * JUMP to merge
     * else: dest <-- val2 [same op]
     * merge: ...
     *
     * The frontend emits `T <-- #c [LOAD]` for `?:` ternary arms that are
     * integer constants — it's a register materialization, not a memory
     * load.  Accepting that form lets us fold `r = (c) ? 0 : 1` into a
     * SELECT/IT-block instead of a jump diamond. */
    int then_is_load_imm = (then_q1->op == TCCIR_OP_LOAD &&
                            irop_is_immediate(tcc_ir_op_get_src1(ir, then_q1)));
    if (then_q1->op == TCCIR_OP_ASSIGN || then_is_load_imm)
    {
      IROperand then_dest = tcc_ir_op_get_dest(ir, then_q1);
      IROperand then_val = tcc_ir_op_get_src1(ir, then_q1);
      int32_t dest_vreg = irop_get_vreg(then_dest);

      /* Next should be unconditional JUMP to merge */
      int jump_idx = ir_skip_nops_forward(ir, then_start + 1, n);
      if (jump_idx >= n)
        continue;
      IRQuadCompact *jump_q = &ir->compact_instructions[jump_idx];
      if (jump_q->op != TCCIR_OP_JUMP)
        continue;

      /* Find else block */
      int else_start = ir_skip_nops_forward(ir, else_target, n);
      if (else_start >= n)
        continue;
      IRQuadCompact *else_q = &ir->compact_instructions[else_start];
      if (else_q->op != then_q1->op)
        continue;
      if (then_is_load_imm && !irop_is_immediate(tcc_ir_op_get_src1(ir, else_q)))
        continue;

      /* Same destination vreg */
      IROperand else_dest = tcc_ir_op_get_dest(ir, else_q);
      IROperand else_val = tcc_ir_op_get_src1(ir, else_q);
      if (irop_get_vreg(else_dest) != dest_vreg)
        continue;

      /* The else block must be exactly one ASSIGN.  After it, the next
       * instruction must be the merge point (the JMP target from then).
       * Otherwise the else block has more instructions and it's not a
       * simple diamond — NOP'ing the else ASSIGN would break the rest. */
      int merge_target = (int)irop_get_imm64_ex(ir, tcc_ir_op_get_dest(ir, jump_q));
      int after_else = ir_skip_nops_forward(ir, else_start + 1, n);
      if (after_else != merge_target)
        continue;

      /* Allocate 4 pool entries for SELECT */
      IROperand sel_cond = irop_make_imm32(-1, then_cond, VT_INT);
      int pool_base = tcc_ir_iroperand_pool_add(ir, then_dest);
      tcc_ir_iroperand_pool_add(ir, then_val);
      tcc_ir_iroperand_pool_add(ir, else_val);
      tcc_ir_iroperand_pool_add(ir, sel_cond);

      /* Rewrite JUMPIF as SELECT (drops one jump to else_target) */
      if (else_target >= 0 && else_target < n && jt_cnt[else_target] > 0)
        jt_cnt[else_target]--;
      jumpif_q->op = TCCIR_OP_SELECT;
      jumpif_q->operand_base = pool_base;

      /* NOP the then-assign, jump, and else-assign */
      ir->compact_instructions[then_start].op = TCCIR_OP_NOP;
      JT_NOP_JUMP(jump_idx);
      ir->compact_instructions[else_start].op = TCCIR_OP_NOP;

      /* Clear stale is_jump_target on positions no longer targeted
       * (the NOPed JUMP no longer jumps, so re-check with no exclusion) */
      if (!JT_HAS_OTHER(else_target, -1))
        ir->compact_instructions[else_target].is_jump_target = 0;
      if (merge_target >= 0 && merge_target < n && !JT_HAS_OTHER(merge_target, -1))
        ir->compact_instructions[merge_target].is_jump_target = 0;

      changes++;
      continue;
    }

    /* ----------------------------------------------------------------
     * Pattern: Return diamond
     * ----------------------------------------------------------------
     * then: RETURNVALUE val_then [const]
     * else_target: RETURNVALUE val_else [const]
     *
     * Both branches are terminal (no merge), so no JUMP between them.
     * The else_target falls immediately after the then RETURNVALUE.
     * ---------------------------------------------------------------- */
    if (then_q1->op == TCCIR_OP_RETURNVALUE)
    {
      IROperand then_val = tcc_ir_op_get_src1(ir, then_q1);

      /* Then-value must be a compile-time constant — vreg uses would
       * be disrupted by removing the branches. */
      int then_tag = irop_get_tag(then_val);
      if (then_tag != IROP_TAG_IMM32 && then_tag != IROP_TAG_SYMREF)
        continue;

      /* Find else block (skip NOPs starting at else_target) */
      int else_start = ir_skip_nops_forward(ir, else_target, n);
      if (else_start >= n)
        continue;
      IRQuadCompact *else_q = &ir->compact_instructions[else_start];
      if (else_q->op != TCCIR_OP_RETURNVALUE)
        continue;

      IROperand else_val = tcc_ir_op_get_src1(ir, else_q);
      int else_tag = irop_get_tag(else_val);
      if (else_tag != IROP_TAG_IMM32 && else_tag != IROP_TAG_SYMREF)
        continue;

      /* Else block must immediately follow the then RETURNVALUE
       * (otherwise NOP'ing the else RETURNVALUE could break adjacent code). */
      int after_then = ir_skip_nops_forward(ir, then_start + 1, n);
      if (after_then != else_start)
        continue;

      /* Allocate 4 pool entries for SELECT */
      int32_t select_vreg = tcc_ir_get_vreg_temp(ir);
      IROperand sel_dest = irop_make_vreg(select_vreg, IROP_BTYPE_INT32);
      IROperand sel_cond = irop_make_imm32(-1, then_cond, VT_INT);

      int pool_base = tcc_ir_iroperand_pool_add(ir, sel_dest);
      tcc_ir_iroperand_pool_add(ir, then_val);
      tcc_ir_iroperand_pool_add(ir, else_val);
      tcc_ir_iroperand_pool_add(ir, sel_cond);

      /* Rewrite JUMPIF as SELECT (drops one jump to else_target) */
      if (else_target >= 0 && else_target < n && jt_cnt[else_target] > 0)
        jt_cnt[else_target]--;
      jumpif_q->op = TCCIR_OP_SELECT;
      jumpif_q->operand_base = pool_base;

      /* Rewrite the then RETURNVALUE to consume the SELECT result */
      IROperand new_ret_val = irop_make_vreg(select_vreg, IROP_BTYPE_INT32);
      tcc_ir_op_set_src1(ir, then_q1, new_ret_val);

      /* NOP the else RETURNVALUE (else_start) — unreachable now */
      ir->compact_instructions[else_start].op = TCCIR_OP_NOP;

      /* Clear stale is_jump_target on else_target if no longer targeted */
      if (!JT_HAS_OTHER(else_target, -1))
        ir->compact_instructions[else_target].is_jump_target = 0;

      changes++;
      continue;
    }
  }

  tcc_free(jt_cnt);
  #undef JT_HAS_OTHER
  #undef JT_NOP_JUMP
  return changes;
}

/* ============================================================================
 * Block Copy Initialization Optimization
 *
 * Detects pattern: memset(stack_area, 0, N) followed by consecutive STORE
 * instructions writing constant values (symbol refs) into the same stack area.
 * Replaces with a single BLOCK_COPY from a pre-built rodata block.
 *
 * Before: memset(sp[-20], 0, 20) + 5x STORE sp[-20..-4] <- GlobalSym(...)
 * After:  BLOCK_COPY sp[-20] <- rodata_sym, 20
 * ============================================================================ */


int tcc_ir_opt_postinc_assign_fold(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  int changes = 0;

  if (n < 2)
    return 0;

  for (int i = 0; i < n - 1; i++)
  {
    IRQuadCompact *q_assign = &ir->compact_instructions[i];

    /* Instruction A must be ASSIGN */
    if (q_assign->op != TCCIR_OP_ASSIGN)
      continue;
    if (!irop_config[q_assign->op].has_dest)
      continue;

    IROperand assign_dest = tcc_ir_op_get_dest(ir, q_assign);
    IROperand assign_src = tcc_ir_op_get_src1(ir, q_assign);

    /* Dest must be a TEMP vreg */
    int32_t temp_vr = irop_get_vreg(assign_dest);
    if (temp_vr < 0)
      continue;
    if (TCCIR_DECODE_VREG_TYPE(temp_vr) != TCCIR_VREG_TYPE_TEMP)
      continue;

    /* Source must be a VAR or PARAM with is_lval (stack variable) */
    int32_t var_vr = irop_get_vreg(assign_src);
    if (var_vr < 0)
      continue;
    int var_type = TCCIR_DECODE_VREG_TYPE(var_vr);
    if (var_type != TCCIR_VREG_TYPE_VAR && var_type != TCCIR_VREG_TYPE_PARAM)
      continue;
    if (!assign_src.is_lval)
      continue;

    /* Find next non-NOP instruction */
    int next_idx = -1;
    for (int j = i + 1; j < n; j++)
    {
      IRQuadCompact *q_next = &ir->compact_instructions[j];
      if (q_next->op == TCCIR_OP_NOP)
        continue;
      /* Bail if we hit a jump target — control flow might skip A */
      if (q_next->is_jump_target)
        break;
      next_idx = j;
      break;
    }
    if (next_idx < 0)
      continue;

    IRQuadCompact *q_arith = &ir->compact_instructions[next_idx];

    /* Instruction B must have dest, src1, src2 (arithmetic/binary op) */
    if (!irop_config[q_arith->op].has_dest || !irop_config[q_arith->op].has_src1 || !irop_config[q_arith->op].has_src2)
      continue;

    /* B's dest must be the same VAR/PARAM as A's source */
    IROperand arith_dest = tcc_ir_op_get_dest(ir, q_arith);
    int32_t arith_dest_vr = irop_get_vreg(arith_dest);
    if (arith_dest_vr != var_vr)
      continue;

    /* B's src1 must be the TEMP from A */
    IROperand arith_src1 = tcc_ir_op_get_src1(ir, q_arith);
    int32_t arith_src1_vr = irop_get_vreg(arith_src1);
    if (arith_src1_vr != temp_vr)
      continue;

    /* T must have exactly one use across the entire function (instruction B).
     * We cannot use tcc_ir_vreg_has_single_use() because it only checks
     * src1/src2 operands.  For STORE instructions, the "dest" operand is
     * actually a USE (the memory address being written to), e.g.:
     *   T7***DEREF*** <-- T9 [STORE]   -- T7 provides the address
     * If T appears as a STORE dest, it has an additional use that would
     * make our folding unsafe (the *q++ = t pattern). */
    {
      int use_count = 0;
      int safe = 1;
      for (int k = 0; k < n && safe; k++)
      {
        if (k == i) /* skip the ASSIGN we're considering */
          continue;
        IRQuadCompact *qk = &ir->compact_instructions[k];
        if (qk->op == TCCIR_OP_NOP)
          continue;

        /* Check src1 and src2 */
        if (irop_config[qk->op].has_src1 && irop_get_vreg(tcc_ir_op_get_src1(ir, qk)) == temp_vr)
          use_count++;
        if (irop_config[qk->op].has_src2 && irop_get_vreg(tcc_ir_op_get_src2(ir, qk)) == temp_vr)
          use_count++;

        /* Check dest operand — for STORE/STORE_INDEXED/STORE_POSTINC the
         * dest provides the memory address (a USE, not a definition).
         * For PARAM ops the dest provides the value being passed.
         * Conservatively count any dest reference as a use. */
        if (irop_config[qk->op].has_dest)
        {
          IROperand dest_op = tcc_ir_op_get_dest(ir, qk);
          if (irop_get_vreg(dest_op) == temp_vr)
            use_count++;
        }

        if (use_count > 1)
          safe = 0;
      }
      if (!safe || use_count != 1)
        continue;
    }

    /* Safe to fold: replace T with V(is_lval) in B's src1, NOP A */

    /* Build replacement: use the original assign_src (V with is_lval)
     * to preserve load semantics, btype, and signedness */
    tcc_ir_set_src1(ir, next_idx, assign_src);

    /* NOP the ASSIGN */
    q_assign->op = TCCIR_OP_NOP;

    changes++;
  }

  return changes;
}

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
        if (safe && irop_config[eq->op].has_dest) {
          if (irop_get_vreg(tcc_ir_op_get_dest(ir, eq)) == adst_vr)
            break; /* redefined before use — safe */
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

    /* NOP the old unconditional JUMP */
    ir->compact_instructions[jump_idx].op = TCCIR_OP_NOP;

    /* Update is_jump_target: body_target is now targeted by the new JUMPIF */
    if (body_target >= 0 && body_target < n)
      ir->compact_instructions[body_target].is_jump_target = 1;

    changes++;
  }

  return changes;
}

int tcc_ir_opt_var_to_tmp_ex(IROptCtx *ctx) { return tcc_ir_opt_var_to_tmp(ctx->ir); }
int tcc_ir_opt_var_tmp_fwd_ex(IROptCtx *ctx) { return tcc_ir_opt_var_tmp_fwd(ctx->ir); }
int tcc_ir_opt_redundant_loop_check_ex(IROptCtx *ctx) { return tcc_ir_opt_redundant_loop_check(ctx->ir); }
