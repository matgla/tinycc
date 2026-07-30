/*
 *  TCC IR - Param/local addr-of constant folding (flat pass)
 *
 *  Copyright (c) 2025 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */
/* Relocated flat pass, Branch A [A*]; see docs/plan_legacy_flat_ir_ssa_retire.md */

#define USING_GLOBALS

#include "ir.h"
#include "opt_engine.h"
#include "opt_utils.h"
#include "opt_du.h"

int tcc_ir_opt_param_addrof_const_fold(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  int max_par = ir->next_parameter;
  int max_tmp = ir->next_temporary_variable;
  int max_var = ir->next_local_variable;
  int changes = 0;

  if (max_par <= 0 || n == 0)
    return 0;

  /* Single-BB restriction: no dominance analysis. */
  for (int i = 0; i < n; i++)
  {
    int op = ir->compact_instructions[i].op;
    if (op == TCCIR_OP_JUMP || op == TCCIR_OP_JUMPIF || op == TCCIR_OP_IJUMP || op == TCCIR_OP_SWITCH_TABLE)
      return 0;
    if (ir->compact_instructions[i].is_jump_target && i > 0)
      return 0;
  }

  typedef struct
  {
    int lea_idx;
    int lea_tmp_pos;
    int store_idx;
    IROperand store_val;
    int disqualified;
  } ParamInfo;

  ParamInfo *pi = tcc_mallocz(sizeof(ParamInfo) * (max_par + 1));
  /* TEMP/VAR position -> PARAM position it aliases (&P), or -1. */
  int *tmp_lea_param = tcc_malloc(sizeof(int) * (max_tmp + 1));
  int *var_lea_param = tcc_malloc(sizeof(int) * (max_var + 1));
  /* Single-def VAR tracking (safe to chase as aliases). */
  uint8_t *var_def_count = tcc_mallocz(max_var + 1);
  /* Mark instrs in the LEA->VAR->TEMP chain so Phase 2 ignores them. */
  uint8_t *chain_instr = tcc_mallocz((n + 7) / 8);

  for (int p = 0; p <= max_par; p++)
  {
    pi[p].lea_idx = -1;
    pi[p].store_idx = -1;
  }
  for (int t = 0; t <= max_tmp; t++)
    tmp_lea_param[t] = -1;
  for (int v = 0; v <= max_var; v++)
    var_lea_param[v] = -1;

  /* Count VAR defs (cap at 2). */
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP || !irop_config[q->op].has_dest)
      continue;
    IROperand dest = tcc_ir_op_get_dest(ir, q);
    int32_t dvr = irop_get_vreg(dest);
    if (dvr < 0 || TCCIR_DECODE_VREG_TYPE(dvr) != TCCIR_VREG_TYPE_VAR)
      continue;
    int vp = TCCIR_DECODE_VREG_POSITION(dvr);
    if (vp <= max_var && var_def_count[vp] < 2)
      var_def_count[vp]++;
  }

  /* Phase 1: LEAs T = &P. */
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_LEA)
      continue;

    IROperand dest = tcc_ir_op_get_dest(ir, q);
    IROperand src1 = tcc_ir_op_get_src1(ir, q);
    int32_t dvr = irop_get_vreg(dest);
    int32_t svr = irop_get_vreg(src1);

    if (!src1.is_local || src1.is_lval)
      continue;
    if (svr < 0 || TCCIR_DECODE_VREG_TYPE(svr) != TCCIR_VREG_TYPE_PARAM)
      continue;
    if (dvr < 0 || TCCIR_DECODE_VREG_TYPE(dvr) != TCCIR_VREG_TYPE_TEMP)
      continue;

    int p = TCCIR_DECODE_VREG_POSITION(svr);
    int t = TCCIR_DECODE_VREG_POSITION(dvr);
    if (p > max_par || t > max_tmp)
      continue;

    if (pi[p].lea_idx == -1 && tmp_lea_param[t] == -1)
    {
      pi[p].lea_idx = i;
      pi[p].lea_tmp_pos = t;
      tmp_lea_param[t] = p;
    }
    else
    {
      if (pi[p].lea_idx >= 0)
        pi[p].disqualified = 1;
      if (tmp_lea_param[t] >= 0)
        pi[tmp_lea_param[t]].disqualified = 1;
    }
  }

  /* Phase 1b: propagate aliasing through STORE/ASSIGN chains (single-def V). */
  int chain_changed;
  do
  {
    chain_changed = 0;
    for (int i = 0; i < n; i++)
    {
      IRQuadCompact *q = &ir->compact_instructions[i];
      if (q->op == TCCIR_OP_NOP)
        continue;

      IROperand dest = tcc_ir_op_get_dest(ir, q);
      IROperand src1 = tcc_ir_op_get_src1(ir, q);
      int32_t dvr = irop_get_vreg(dest);
      int32_t svr = irop_get_vreg(src1);

      /* STORE V <-- T (dest VAR, src1 TEMP holding LEA result). */
      if (q->op == TCCIR_OP_STORE && dvr >= 0 && TCCIR_DECODE_VREG_TYPE(dvr) == TCCIR_VREG_TYPE_VAR &&
          svr >= 0 && TCCIR_DECODE_VREG_TYPE(svr) == TCCIR_VREG_TYPE_TEMP && !src1.is_lval)
      {
        int vp = TCCIR_DECODE_VREG_POSITION(dvr);
        int tp = TCCIR_DECODE_VREG_POSITION(svr);
        if (vp <= max_var && tp <= max_tmp && var_def_count[vp] == 1 && tmp_lea_param[tp] >= 0 &&
            var_lea_param[vp] == -1)
        {
          var_lea_param[vp] = tmp_lea_param[tp];
          chain_instr[i / 8] |= (1 << (i % 8));
          chain_changed = 1;
        }
      }
      /* ASSIGN T' <-- V (dest TEMP, src1 VAR with lea alias). */
      else if (q->op == TCCIR_OP_ASSIGN && !dest.is_lval && dvr >= 0 &&
               TCCIR_DECODE_VREG_TYPE(dvr) == TCCIR_VREG_TYPE_TEMP && svr >= 0 &&
               TCCIR_DECODE_VREG_TYPE(svr) == TCCIR_VREG_TYPE_VAR)
      {
        int tp = TCCIR_DECODE_VREG_POSITION(dvr);
        int vp = TCCIR_DECODE_VREG_POSITION(svr);
        if (tp <= max_tmp && vp <= max_var && var_lea_param[vp] >= 0 && tmp_lea_param[tp] == -1)
        {
          tmp_lea_param[tp] = var_lea_param[vp];
          chain_instr[i / 8] |= (1 << (i % 8));
          chain_changed = 1;
        }
      }
    }
  } while (chain_changed);

  /* Phase 2: find the unique const STORE through each T; detect disqualifying uses. */
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP || q->op == TCCIR_OP_LEA)
      continue;
    /* Skip LEA->VAR->TEMP chain instructions. */
    if (chain_instr[i / 8] & (1 << (i % 8)))
      continue;

    IROperand dest = tcc_ir_op_get_dest(ir, q);
    IROperand src1 = tcc_ir_op_get_src1(ir, q);
    IROperand src2 = tcc_ir_op_get_src2(ir, q);

    int store_p = -1;
    if (q->op == TCCIR_OP_STORE && dest.is_lval)
    {
      int32_t dvr = irop_get_vreg(dest);
      if (dvr >= 0 && TCCIR_DECODE_VREG_TYPE(dvr) == TCCIR_VREG_TYPE_TEMP)
      {
        int t = TCCIR_DECODE_VREG_POSITION(dvr);
        if (t <= max_tmp && tmp_lea_param[t] >= 0)
        {
          int p = tmp_lea_param[t];
          if (!pi[p].disqualified)
          {
            /* Accept IMM32/SYMREF const addresses; reject 64-bit values. */
            IROperand effective_val = src1;
            int sv_tag = irop_get_tag(src1);

            /* If src1 is a TEMP, look back through one TEMP->VAR->IMM indirection. */
            if (sv_tag == IROP_TAG_VREG)
            {
              int32_t s1vr = irop_get_vreg(src1);
              if (s1vr >= 0 && TCCIR_DECODE_VREG_TYPE(s1vr) == TCCIR_VREG_TYPE_TEMP)
              {
                int t1 = TCCIR_DECODE_VREG_POSITION(s1vr);
                int def_idx = -1;
                int def_count = 0;
                for (int j = 0; j < i && def_count <= 1; j++)
                {
                  IRQuadCompact *r = &ir->compact_instructions[j];
                  if (r->op == TCCIR_OP_NOP || !irop_config[r->op].has_dest)
                    continue;
                  IROperand rdest = tcc_ir_op_get_dest(ir, r);
                  int32_t rdvr = irop_get_vreg(rdest);
                  if (rdvr >= 0 && TCCIR_DECODE_VREG_TYPE(rdvr) == TCCIR_VREG_TYPE_TEMP &&
                      TCCIR_DECODE_VREG_POSITION(rdvr) == t1)
                  {
                    def_idx = j;
                    def_count++;
                  }
                }
                if (def_count == 1)
                {
                  IRQuadCompact *def_q = &ir->compact_instructions[def_idx];
                  if (def_q->op == TCCIR_OP_ASSIGN || def_q->op == TCCIR_OP_LOAD)
                  {
                    IROperand def_src = tcc_ir_op_get_src1(ir, def_q);
                    int def_tag = irop_get_tag(def_src);
                    if ((def_tag == IROP_TAG_IMM32 || def_tag == IROP_TAG_SYMREF) &&
                        !irop_is_64bit(def_src))
                    {
                      effective_val = def_src;
                      effective_val.btype = src1.btype;
                      sv_tag = def_tag;
                    }
                    /* Second hop: TEMP loaded from a single-def VAR holding an IMM/SYMREF. */
                    else
                    {
                      int32_t def_svr = irop_get_vreg(def_src);
                      if (def_svr >= 0 && TCCIR_DECODE_VREG_TYPE(def_svr) == TCCIR_VREG_TYPE_VAR)
                      {
                        int v1 = TCCIR_DECODE_VREG_POSITION(def_svr);
                        if (v1 <= max_var && var_def_count[v1] == 1)
                        {
                          for (int j = 0; j < def_idx; j++)
                          {
                            IRQuadCompact *r = &ir->compact_instructions[j];
                            if (r->op != TCCIR_OP_STORE && r->op != TCCIR_OP_ASSIGN)
                              continue;
                            IROperand rdest = tcc_ir_op_get_dest(ir, r);
                            int32_t rdvr = irop_get_vreg(rdest);
                            if (rdvr < 0 || TCCIR_DECODE_VREG_TYPE(rdvr) != TCCIR_VREG_TYPE_VAR ||
                                TCCIR_DECODE_VREG_POSITION(rdvr) != v1)
                              continue;
                            IROperand rsrc = tcc_ir_op_get_src1(ir, r);
                            int rtag = irop_get_tag(rsrc);
                            if ((rtag == IROP_TAG_IMM32 || rtag == IROP_TAG_SYMREF) &&
                                !irop_is_64bit(rsrc))
                            {
                              effective_val = rsrc;
                              effective_val.btype = src1.btype;
                              sv_tag = rtag;
                            }
                            break;
                          }
                        }
                      }
                    }
                  }
                }
              }
            }

            int sv_ok = (sv_tag == IROP_TAG_IMM32 || sv_tag == IROP_TAG_SYMREF) && !irop_is_64bit(effective_val);
            if (pi[p].store_idx == -1 && sv_ok)
            {
              pi[p].store_idx = i;
              pi[p].store_val = effective_val;
              store_p = p;
            }
            else
            {
              pi[p].disqualified = 1;
            }
          }
        }
      }
    }

    IROperand ops_arr[3];
    ops_arr[0] = dest;
    ops_arr[1] = src1;
    ops_arr[2] = src2;
    for (int oi = 0; oi < 3; oi++)
    {
      int32_t vr = irop_get_vreg(ops_arr[oi]);
      if (vr < 0)
        continue;
      int vt = TCCIR_DECODE_VREG_TYPE(vr);
      int vp = TCCIR_DECODE_VREG_POSITION(vr);

      if (vt == TCCIR_VREG_TYPE_TEMP && vp <= max_tmp && tmp_lea_param[vp] >= 0)
      {
        int p = tmp_lea_param[vp];
        if (p == store_p && oi == 0)
          continue;
        pi[p].disqualified = 1;
      }
      if (vt == TCCIR_VREG_TYPE_VAR && vp <= max_var && var_lea_param[vp] >= 0)
      {
        pi[var_lea_param[vp]].disqualified = 1;
      }

      if (vt == TCCIR_VREG_TYPE_PARAM && vp <= max_par && pi[vp].lea_idx >= 0)
      {
        if (pi[vp].store_idx == -1)
          pi[vp].disqualified = 1;
      }
    }
  }

  /* Phase 3: rewrite reads of P past the STORE, then NOP the LEA + STORE + chain. */
  for (int p = 0; p <= max_par; p++)
  {
    if (pi[p].disqualified || pi[p].lea_idx < 0 || pi[p].store_idx < 0)
      continue;

    int rewrote = 0;
    int store_idx = pi[p].store_idx;
    IROperand store_val = pi[p].store_val;

    for (int i = store_idx + 1; i < n; i++)
    {
      IRQuadCompact *q = &ir->compact_instructions[i];
      if (q->op == TCCIR_OP_NOP)
        continue;

      IROperand src1 = tcc_ir_op_get_src1(ir, q);
      IROperand src2 = tcc_ir_op_get_src2(ir, q);

      /* Replace post-STORE refs to P (spill-slot LOAD or direct PARAM ref) with stored value. */
      int32_t s1vr = irop_get_vreg(src1);
      if (s1vr >= 0 && TCCIR_DECODE_VREG_TYPE(s1vr) == TCCIR_VREG_TYPE_PARAM &&
          TCCIR_DECODE_VREG_POSITION(s1vr) == p && !(src1.is_lval && !src1.is_local))
      {
        IROperand newop = store_val;
        newop.btype = src1.btype;
        newop.is_lval = 0;
        tcc_ir_op_set_src1(ir, q, newop);
        if (q->op == TCCIR_OP_LOAD)
          q->op = TCCIR_OP_ASSIGN;
        rewrote++;
      }

      int32_t s2vr = irop_get_vreg(src2);
      if (s2vr >= 0 && TCCIR_DECODE_VREG_TYPE(s2vr) == TCCIR_VREG_TYPE_PARAM &&
          TCCIR_DECODE_VREG_POSITION(s2vr) == p && !(src2.is_lval && !src2.is_local))
      {
        IROperand newop = store_val;
        newop.btype = src2.btype;
        newop.is_lval = 0;
        tcc_ir_op_set_src2(ir, q, newop);
        rewrote++;
      }
    }

    if (rewrote > 0)
    {
      ir->compact_instructions[pi[p].lea_idx].op = TCCIR_OP_NOP;
      ir->compact_instructions[store_idx].op = TCCIR_OP_NOP;
      changes += rewrote + 2;

      /* NOP chain instructions so codegen drops their results. */
      for (int i = 0; i < n; i++)
      {
        if (!(chain_instr[i / 8] & (1 << (i % 8))))
          continue;
        IRQuadCompact *q = &ir->compact_instructions[i];
        /* Verify this chain instruction belongs to this P. */
        IROperand dest = tcc_ir_op_get_dest(ir, q);
        IROperand src1 = tcc_ir_op_get_src1(ir, q);
        int32_t dvr = irop_get_vreg(dest);
        int32_t svr = irop_get_vreg(src1);
        int belongs = 0;
        if (q->op == TCCIR_OP_STORE && dvr >= 0 && TCCIR_DECODE_VREG_TYPE(dvr) == TCCIR_VREG_TYPE_VAR)
        {
          int vp = TCCIR_DECODE_VREG_POSITION(dvr);
          if (vp <= max_var && var_lea_param[vp] == p)
            belongs = 1;
        }
        else if (q->op == TCCIR_OP_ASSIGN && svr >= 0 && TCCIR_DECODE_VREG_TYPE(svr) == TCCIR_VREG_TYPE_VAR)
        {
          int vp = TCCIR_DECODE_VREG_POSITION(svr);
          if (vp <= max_var && var_lea_param[vp] == p)
            belongs = 1;
        }
        if (belongs)
        {
          q->op = TCCIR_OP_NOP;
          changes++;
        }
      }

      int32_t pvr = TCCIR_ENCODE_VREG(TCCIR_VREG_TYPE_PARAM, p);
      IRLiveInterval *interval = tcc_ir_get_live_interval(ir, pvr);
      if (interval)
        interval->addrtaken = 0;
    }
  }

  tcc_free(pi);
  tcc_free(tmp_lea_param);
  tcc_free(var_lea_param);
  tcc_free(var_def_count);
  tcc_free(chain_instr);
  return changes;
}

/* Analogue of the PARAM pass for local VARs (single BB). */
int tcc_ir_opt_local_addrof_const_fold(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  int max_tmp = ir->next_temporary_variable;
  int max_var = ir->next_local_variable;
  int changes = 0;

  if (max_var <= 0 || n == 0)
    return 0;

  /* Single-BB restriction. */
  for (int i = 0; i < n; i++)
  {
    int op = ir->compact_instructions[i].op;
    if (op == TCCIR_OP_JUMP || op == TCCIR_OP_JUMPIF || op == TCCIR_OP_IJUMP || op == TCCIR_OP_SWITCH_TABLE)
      return 0;
    if (ir->compact_instructions[i].is_jump_target && i > 0)
      return 0;
  }

  typedef struct
  {
    int init_idx;
    IROperand init_val;
    int lea_idx;
    int lea_tmp_pos;
    int store_idx;
    IROperand store_val;
    int disqualified;
  } VarInfo;

  VarInfo *vi = tcc_mallocz(sizeof(VarInfo) * (max_var + 1));
  /* TEMP pos -> target VAR pos, or -1 (LEA result or chained alias). */
  int *tmp_lea_var = tcc_malloc(sizeof(int) * (max_tmp + 1));
  /* VAR pos -> aliased target VAR pos (chain), or -1. */
  int *var_lea_var = tcc_malloc(sizeof(int) * (max_var + 1));
  /* Single-def VAR tracking for safe chain traversal. */
  uint8_t *var_def_count = tcc_mallocz(max_var + 1);
  /* Bitmap of chain instructions (STORE V_a=T, ASSIGN T'=V_a). */
  uint8_t *chain_instr = tcc_mallocz((n + 7) / 8);

  for (int v = 0; v <= max_var; v++)
  {
    vi[v].init_idx = -1;
    vi[v].lea_idx = -1;
    vi[v].store_idx = -1;
    var_lea_var[v] = -1;
  }
  for (int t = 0; t <= max_tmp; t++)
    tmp_lea_var[t] = -1;

  /* Count VAR defs (cap at 2). */
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP || !irop_config[q->op].has_dest)
      continue;
    IROperand dest = tcc_ir_op_get_dest(ir, q);
    int32_t dvr = irop_get_vreg(dest);
    if (dvr < 0 || TCCIR_DECODE_VREG_TYPE(dvr) != TCCIR_VREG_TYPE_VAR)
      continue;
    int vp = TCCIR_DECODE_VREG_POSITION(dvr);
    if (vp <= max_var && var_def_count[vp] < 2)
      var_def_count[vp]++;
  }

  /* Phase 1: locate LEA T = &V for local V. */
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_LEA)
      continue;

    IROperand dest = tcc_ir_op_get_dest(ir, q);
    IROperand src1 = tcc_ir_op_get_src1(ir, q);
    int32_t dvr = irop_get_vreg(dest);
    int32_t svr = irop_get_vreg(src1);

    if (!src1.is_local || src1.is_lval)
      continue;
    if (svr < 0 || TCCIR_DECODE_VREG_TYPE(svr) != TCCIR_VREG_TYPE_VAR)
      continue;
    if (dvr < 0 || TCCIR_DECODE_VREG_TYPE(dvr) != TCCIR_VREG_TYPE_TEMP)
      continue;

    int v = TCCIR_DECODE_VREG_POSITION(svr);
    int t = TCCIR_DECODE_VREG_POSITION(dvr);
    if (v > max_var || t > max_tmp)
      continue;

    if (vi[v].lea_idx == -1 && tmp_lea_var[t] == -1)
    {
      vi[v].lea_idx = i;
      vi[v].lea_tmp_pos = t;
      tmp_lea_var[t] = v;
    }
    else
    {
      if (vi[v].lea_idx >= 0)
        vi[v].disqualified = 1;
      if (tmp_lea_var[t] >= 0)
        vi[tmp_lea_var[t]].disqualified = 1;
    }
  }

  /* Phase 1b: propagate alias through STORE/ASSIGN chains (single-def V_a). */
  int chain_changed;
  do
  {
    chain_changed = 0;
    for (int i = 0; i < n; i++)
    {
      IRQuadCompact *q = &ir->compact_instructions[i];
      if (q->op == TCCIR_OP_NOP)
        continue;

      IROperand dest = tcc_ir_op_get_dest(ir, q);
      IROperand src1 = tcc_ir_op_get_src1(ir, q);
      int32_t dvr = irop_get_vreg(dest);
      int32_t svr = irop_get_vreg(src1);

      if (q->op == TCCIR_OP_STORE && dvr >= 0 && TCCIR_DECODE_VREG_TYPE(dvr) == TCCIR_VREG_TYPE_VAR &&
          svr >= 0 && TCCIR_DECODE_VREG_TYPE(svr) == TCCIR_VREG_TYPE_TEMP && !src1.is_lval)
      {
        int vap = TCCIR_DECODE_VREG_POSITION(dvr);
        int tp = TCCIR_DECODE_VREG_POSITION(svr);
        if (vap <= max_var && tp <= max_tmp && var_def_count[vap] == 1 && tmp_lea_var[tp] >= 0 &&
            var_lea_var[vap] == -1 && vap != tmp_lea_var[tp])
        {
          var_lea_var[vap] = tmp_lea_var[tp];
          chain_instr[i / 8] |= (1 << (i % 8));
          chain_changed = 1;
        }
      }
      else if (q->op == TCCIR_OP_ASSIGN && !dest.is_lval && dvr >= 0 &&
               TCCIR_DECODE_VREG_TYPE(dvr) == TCCIR_VREG_TYPE_TEMP && svr >= 0 &&
               TCCIR_DECODE_VREG_TYPE(svr) == TCCIR_VREG_TYPE_VAR)
      {
        int tp = TCCIR_DECODE_VREG_POSITION(dvr);
        int vap = TCCIR_DECODE_VREG_POSITION(svr);
        if (tp <= max_tmp && vap <= max_var && var_lea_var[vap] >= 0 && tmp_lea_var[tp] == -1)
        {
          tmp_lea_var[tp] = var_lea_var[vap];
          chain_instr[i / 8] |= (1 << (i % 8));
          chain_changed = 1;
        }
      }
    }
  } while (chain_changed);

  /* Phase 2: locate per-V init STORE and modify STORE; detect out-of-pattern uses. */
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP || q->op == TCCIR_OP_LEA)
      continue;
    if (chain_instr[i / 8] & (1 << (i % 8)))
      continue;

    IROperand dest = tcc_ir_op_get_dest(ir, q);
    IROperand src1 = tcc_ir_op_get_src1(ir, q);
    IROperand src2 = tcc_ir_op_get_src2(ir, q);

    int store_v = -1;

    /* STORE through T (or chained T') with constant value. */
    if (q->op == TCCIR_OP_STORE && dest.is_lval)
    {
      int32_t dvr = irop_get_vreg(dest);
      if (dvr >= 0 && TCCIR_DECODE_VREG_TYPE(dvr) == TCCIR_VREG_TYPE_TEMP)
      {
        int t = TCCIR_DECODE_VREG_POSITION(dvr);
        if (t <= max_tmp && tmp_lea_var[t] >= 0)
        {
          int v = tmp_lea_var[t];
          if (!vi[v].disqualified)
          {
            IROperand effective_val = src1;
            int sv_tag = irop_get_tag(src1);

            /* Same TEMP->VAR->IMM look-through as the PARAM pass. */
            if (sv_tag == IROP_TAG_VREG)
            {
              int32_t s1vr = irop_get_vreg(src1);
              if (s1vr >= 0 && TCCIR_DECODE_VREG_TYPE(s1vr) == TCCIR_VREG_TYPE_TEMP)
              {
                int t1 = TCCIR_DECODE_VREG_POSITION(s1vr);
                int def_idx = -1;
                int def_count = 0;
                for (int j = 0; j < i && def_count <= 1; j++)
                {
                  IRQuadCompact *r = &ir->compact_instructions[j];
                  if (r->op == TCCIR_OP_NOP || !irop_config[r->op].has_dest)
                    continue;
                  IROperand rdest = tcc_ir_op_get_dest(ir, r);
                  int32_t rdvr = irop_get_vreg(rdest);
                  if (rdvr >= 0 && TCCIR_DECODE_VREG_TYPE(rdvr) == TCCIR_VREG_TYPE_TEMP &&
                      TCCIR_DECODE_VREG_POSITION(rdvr) == t1)
                  {
                    def_idx = j;
                    def_count++;
                  }
                }
                if (def_count == 1)
                {
                  IRQuadCompact *def_q = &ir->compact_instructions[def_idx];
                  if (def_q->op == TCCIR_OP_ASSIGN || def_q->op == TCCIR_OP_LOAD)
                  {
                    IROperand def_src = tcc_ir_op_get_src1(ir, def_q);
                    int def_tag = irop_get_tag(def_src);
                    if ((def_tag == IROP_TAG_IMM32 || def_tag == IROP_TAG_SYMREF) &&
                        !irop_is_64bit(def_src))
                    {
                      effective_val = def_src;
                      effective_val.btype = src1.btype;
                      sv_tag = def_tag;
                    }
                    else
                    {
                      int32_t def_svr = irop_get_vreg(def_src);
                      if (def_svr >= 0 && TCCIR_DECODE_VREG_TYPE(def_svr) == TCCIR_VREG_TYPE_VAR)
                      {
                        int va = TCCIR_DECODE_VREG_POSITION(def_svr);
                        if (va <= max_var && var_def_count[va] == 1)
                        {
                          for (int j = 0; j < def_idx; j++)
                          {
                            IRQuadCompact *r = &ir->compact_instructions[j];
                            if (r->op != TCCIR_OP_STORE && r->op != TCCIR_OP_ASSIGN)
                              continue;
                            IROperand rdest = tcc_ir_op_get_dest(ir, r);
                            int32_t rdvr = irop_get_vreg(rdest);
                            if (rdvr < 0 || TCCIR_DECODE_VREG_TYPE(rdvr) != TCCIR_VREG_TYPE_VAR ||
                                TCCIR_DECODE_VREG_POSITION(rdvr) != va)
                              continue;
                            IROperand rsrc = tcc_ir_op_get_src1(ir, r);
                            int rtag = irop_get_tag(rsrc);
                            if ((rtag == IROP_TAG_IMM32 || rtag == IROP_TAG_SYMREF) &&
                                !irop_is_64bit(rsrc))
                            {
                              effective_val = rsrc;
                              effective_val.btype = src1.btype;
                              sv_tag = rtag;
                            }
                            break;
                          }
                        }
                      }
                    }
                  }
                }
              }
            }

            int sv_ok = (sv_tag == IROP_TAG_IMM32 || sv_tag == IROP_TAG_SYMREF) && !irop_is_64bit(effective_val);
            if (vi[v].store_idx == -1 && sv_ok)
            {
              vi[v].store_idx = i;
              vi[v].store_val = effective_val;
              store_v = v;
            }
            else
            {
              vi[v].disqualified = 1;
            }
          }
        }
      }
    }

    /* Detect the init STORE V <-- C0 (must be pre-LEA, single, constant). */
    if (q->op == TCCIR_OP_STORE && !dest.is_lval)
    {
      /* Plain VAR write (dest is VAR with !is_lval). */
    }
    if (q->op == TCCIR_OP_STORE)
    {
      int32_t dvr = irop_get_vreg(dest);
      if (dvr >= 0 && TCCIR_DECODE_VREG_TYPE(dvr) == TCCIR_VREG_TYPE_VAR)
      {
        int v = TCCIR_DECODE_VREG_POSITION(dvr);
        if (v <= max_var && vi[v].lea_idx >= 0)
        {
          /* Only pre-LEA, constant, exactly-one init is allowed. */
          if (i < vi[v].lea_idx)
          {
            int it_tag = irop_get_tag(src1);
            int it_ok = (it_tag == IROP_TAG_IMM32 || it_tag == IROP_TAG_SYMREF) && !irop_is_64bit(src1);
            if (vi[v].init_idx == -1 && it_ok)
            {
              vi[v].init_idx = i;
              vi[v].init_val = src1;
            }
            else
            {
              vi[v].disqualified = 1;
            }
          }
          else
          {
            /* Post-LEA direct write of V (not via the tracked T) — disqualify. */
            vi[v].disqualified = 1;
          }
        }
      }
    }

    /* Out-of-pattern use detection on all three operands. */
    IROperand ops_arr[3];
    ops_arr[0] = dest;
    ops_arr[1] = src1;
    ops_arr[2] = src2;
    for (int oi = 0; oi < 3; oi++)
    {
      int32_t vr = irop_get_vreg(ops_arr[oi]);
      if (vr < 0)
        continue;
      int vt = TCCIR_DECODE_VREG_TYPE(vr);
      int vp = TCCIR_DECODE_VREG_POSITION(vr);

      /* TEMP holding &V: allowed only as STORE-through dest; else disqualify. */
      if (vt == TCCIR_VREG_TYPE_TEMP && vp <= max_tmp && tmp_lea_var[vp] >= 0)
      {
        int v = tmp_lea_var[vp];
        if (v == store_v && oi == 0)
          continue;
        vi[v].disqualified = 1;
      }
      /* VAR chain alias: any use outside the chain disqualifies the target. */
      if (vt == TCCIR_VREG_TYPE_VAR && vp <= max_var && var_lea_var[vp] >= 0)
      {
        vi[var_lea_var[vp]].disqualified = 1;
      }
      /* V itself: init STORE dest and post-STORE src allowed; pre-modify reads disqualify. */
      if (vt == TCCIR_VREG_TYPE_VAR && vp <= max_var && vi[vp].lea_idx >= 0)
      {
        int v = vp;
        /* Skip the init STORE dest position. */
        if (q->op == TCCIR_OP_STORE && oi == 0 && i == vi[v].init_idx)
          continue;
        /* Reads (src1/src2) of V before the modify STORE disqualify. */
        if (oi >= 1)
        {
          if (vi[v].store_idx == -1 || i <= vi[v].store_idx)
            vi[v].disqualified = 1;
        }
      }
    }
  }

  /* Phase 3: rewrite reads of V past the STORE, then NOP init + LEA + STORE + chain. */
  for (int v = 0; v <= max_var; v++)
  {
    if (vi[v].disqualified)
      continue;
    if (vi[v].lea_idx < 0 || vi[v].store_idx < 0 || vi[v].init_idx < 0)
      continue;

    int rewrote = 0;
    int store_idx = vi[v].store_idx;
    IROperand store_val = vi[v].store_val;

    for (int i = store_idx + 1; i < n; i++)
    {
      IRQuadCompact *q = &ir->compact_instructions[i];
      if (q->op == TCCIR_OP_NOP)
        continue;

      IROperand src1 = tcc_ir_op_get_src1(ir, q);
      IROperand src2 = tcc_ir_op_get_src2(ir, q);

      int32_t s1vr = irop_get_vreg(src1);
      if (s1vr >= 0 && TCCIR_DECODE_VREG_TYPE(s1vr) == TCCIR_VREG_TYPE_VAR &&
          TCCIR_DECODE_VREG_POSITION(s1vr) == v && !(src1.is_lval && !src1.is_local))
      {
        IROperand newop = store_val;
        newop.btype = src1.btype;
        newop.is_lval = 0;
        tcc_ir_op_set_src1(ir, q, newop);
        if (q->op == TCCIR_OP_LOAD)
          q->op = TCCIR_OP_ASSIGN;
        rewrote++;
      }

      int32_t s2vr = irop_get_vreg(src2);
      if (s2vr >= 0 && TCCIR_DECODE_VREG_TYPE(s2vr) == TCCIR_VREG_TYPE_VAR &&
          TCCIR_DECODE_VREG_POSITION(s2vr) == v && !(src2.is_lval && !src2.is_local))
      {
        IROperand newop = store_val;
        newop.btype = src2.btype;
        newop.is_lval = 0;
        tcc_ir_op_set_src2(ir, q, newop);
        rewrote++;
      }
    }

    if (rewrote > 0)
    {
      ir->compact_instructions[vi[v].init_idx].op = TCCIR_OP_NOP;
      ir->compact_instructions[vi[v].lea_idx].op = TCCIR_OP_NOP;
      ir->compact_instructions[store_idx].op = TCCIR_OP_NOP;
      changes += rewrote + 3;

      /* NOP chain instructions belonging to this V. */
      for (int i = 0; i < n; i++)
      {
        if (!(chain_instr[i / 8] & (1 << (i % 8))))
          continue;
        IRQuadCompact *q = &ir->compact_instructions[i];
        IROperand dest = tcc_ir_op_get_dest(ir, q);
        IROperand src1 = tcc_ir_op_get_src1(ir, q);
        int32_t dvr = irop_get_vreg(dest);
        int32_t svr = irop_get_vreg(src1);
        int belongs = 0;
        if (q->op == TCCIR_OP_STORE && dvr >= 0 && TCCIR_DECODE_VREG_TYPE(dvr) == TCCIR_VREG_TYPE_VAR)
        {
          int vap = TCCIR_DECODE_VREG_POSITION(dvr);
          if (vap <= max_var && var_lea_var[vap] == v)
            belongs = 1;
        }
        else if (q->op == TCCIR_OP_ASSIGN && svr >= 0 && TCCIR_DECODE_VREG_TYPE(svr) == TCCIR_VREG_TYPE_VAR)
        {
          int vap = TCCIR_DECODE_VREG_POSITION(svr);
          if (vap <= max_var && var_lea_var[vap] == v)
            belongs = 1;
        }
        if (belongs)
        {
          q->op = TCCIR_OP_NOP;
          changes++;
        }
      }

      int32_t vvr = TCCIR_ENCODE_VREG(TCCIR_VREG_TYPE_VAR, v);
      IRLiveInterval *interval = tcc_ir_get_live_interval(ir, vvr);
      if (interval)
        interval->addrtaken = 0;
    }
  }

  tcc_free(vi);
  tcc_free(tmp_lea_var);
  tcc_free(var_lea_var);
  tcc_free(var_def_count);
  tcc_free(chain_instr);
  return changes;
}
