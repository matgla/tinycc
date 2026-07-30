/*
 *  TCC IR - Dead Trailing Addr-Var Store Elimination
 *
 *  Copyright (c) 2025 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#define USING_GLOBALS

#include "ir.h"
#include "opt.h"
#include "opt_engine.h"
#include "opt_xform.h"
#include "opt_alias.h"
#include "opt_utils.h"
#include "opt_du.h"
#include "opt_loop_utils.h"
#include "cfg.h"
#include "licm.h"

static void mark_var_escaped(uint8_t *m, int v)
{
  m[v / 8] |= (1 << (v % 8));
}

static int var_is_escaped(const uint8_t *m, int v)
{
  return m[v / 8] & (1 << (v % 8));
}

int tcc_ir_opt_dead_trailing_addrvar_store_elim(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  if (n == 0)
    return 0;

  for (int i = 0; i < n; i++)
  {
    int op = ir->compact_instructions[i].op;
    if (op == TCCIR_OP_FUNCCALLVAL || op == TCCIR_OP_FUNCCALLVOID ||
        op == TCCIR_OP_FUNCPARAMVAL || op == TCCIR_OP_FUNCPARAMVOID ||
        op == TCCIR_OP_IJUMP || op == TCCIR_OP_SETJMP || op == TCCIR_OP_LONGJMP ||
        op == TCCIR_OP_NL_SETJMP || op == TCCIR_OP_NL_LONGJMP ||
        op == TCCIR_OP_INLINE_ASM || op == TCCIR_OP_SET_CHAIN ||
        op == TCCIR_OP_INIT_CHAIN_SLOT || op == TCCIR_OP_SWITCH_TABLE)
      return 0;
  }

  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_JUMP && q->op != TCCIR_OP_JUMPIF)
      continue;
    IROperand dest = tcc_ir_op_get_dest(ir, q);
    int target = (int)irop_get_imm64_ex(ir, dest);
    if (target <= i)
      return 0;
  }

  int max_var = 0, max_tmp = 0;
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;
    IROperand ops[3];
    ops[0] = tcc_ir_op_get_dest(ir, q);
    ops[1] = tcc_ir_op_get_src1(ir, q);
    ops[2] = tcc_ir_op_get_src2(ir, q);
    for (int k = 0; k < 3; k++)
    {
      int32_t vr = irop_get_vreg(ops[k]);
      if (vr < 0)
        continue;
      int t = TCCIR_DECODE_VREG_TYPE(vr);
      int p = TCCIR_DECODE_VREG_POSITION(vr);
      if (t == TCCIR_VREG_TYPE_VAR && p > max_var) max_var = p;
      else if (t == TCCIR_VREG_TYPE_TEMP && p > max_tmp) max_tmp = p;
    }
  }
  if (max_var == 0)
    return 0;

  int *lea_map = tcc_malloc(sizeof(int) * (max_tmp + 1));
  for (int i = 0; i <= max_tmp; i++) lea_map[i] = -1;
  int *var_lea = tcc_malloc(sizeof(int) * (max_var + 1));
  for (int i = 0; i <= max_var; i++) var_lea[i] = -1;
  uint8_t *var_escaped = tcc_mallocz((max_var + 8) / 8);
  int *var_last_read = tcc_malloc(sizeof(int) * (max_var + 1));
  for (int i = 0; i <= max_var; i++) var_last_read[i] = -1;

  /* Pass 1: build lea_map (T=&V) and var_lea (V'=&V), propagate through TEMP<->VAR copies. */
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;

    if (q->op == TCCIR_OP_LEA)
    {
      IROperand src1 = tcc_ir_op_get_src1(ir, q);
      IROperand dest = tcc_ir_op_get_dest(ir, q);
      int32_t s_vr = irop_get_vreg(src1);
      int32_t d_vr = irop_get_vreg(dest);
      if (s_vr >= 0 && TCCIR_DECODE_VREG_TYPE(s_vr) == TCCIR_VREG_TYPE_VAR && d_vr >= 0)
      {
        int v = TCCIR_DECODE_VREG_POSITION(s_vr);
        if (v <= max_var)
        {
          if (TCCIR_DECODE_VREG_TYPE(d_vr) == TCCIR_VREG_TYPE_TEMP)
          {
            int t = TCCIR_DECODE_VREG_POSITION(d_vr);
            if (t <= max_tmp)
            {
              if (lea_map[t] >= 0 && lea_map[t] != v)
                mark_var_escaped(var_escaped, v);
              lea_map[t] = v;
            }
          }
          else if (TCCIR_DECODE_VREG_TYPE(d_vr) == TCCIR_VREG_TYPE_VAR)
          {
            int d_pos = TCCIR_DECODE_VREG_POSITION(d_vr);
            if (d_pos <= max_var)
            {
              if (var_lea[d_pos] >= 0 && var_lea[d_pos] != v)
                mark_var_escaped(var_escaped, v);
              var_lea[d_pos] = v;
            }
          }
        }
      }
      continue;
    }

    /* STORE V=T where T holds a LEA result -> propagate lea_map to var_lea. */
    if (q->op == TCCIR_OP_STORE)
    {
      IROperand dest = tcc_ir_op_get_dest(ir, q);
      IROperand src1 = tcc_ir_op_get_src1(ir, q);
      int32_t d_vr = irop_get_vreg(dest);
      int32_t s_vr = irop_get_vreg(src1);
      if (d_vr >= 0 && TCCIR_DECODE_VREG_TYPE(d_vr) == TCCIR_VREG_TYPE_VAR &&
          s_vr >= 0 && TCCIR_DECODE_VREG_TYPE(s_vr) == TCCIR_VREG_TYPE_TEMP)
      {
        int d_pos = TCCIR_DECODE_VREG_POSITION(d_vr);
        int s_tmp = TCCIR_DECODE_VREG_POSITION(s_vr);
        if (d_pos <= max_var && s_tmp <= max_tmp && lea_map[s_tmp] >= 0)
        {
          if (var_lea[d_pos] >= 0 && var_lea[d_pos] != lea_map[s_tmp])
            mark_var_escaped(var_escaped, lea_map[s_tmp]);
          var_lea[d_pos] = lea_map[s_tmp];
        }
      }
    }

    /* ASSIGN/LOAD T=V where V holds a LEA result -> propagate var_lea to lea_map. */
    if (q->op == TCCIR_OP_ASSIGN || q->op == TCCIR_OP_LOAD)
    {
      IROperand dest = tcc_ir_op_get_dest(ir, q);
      IROperand src1 = tcc_ir_op_get_src1(ir, q);
      int32_t d_vr = irop_get_vreg(dest);
      int32_t s_vr = irop_get_vreg(src1);
      if (d_vr >= 0 && TCCIR_DECODE_VREG_TYPE(d_vr) == TCCIR_VREG_TYPE_TEMP &&
          s_vr >= 0 && TCCIR_DECODE_VREG_TYPE(s_vr) == TCCIR_VREG_TYPE_VAR)
      {
        int d_tmp = TCCIR_DECODE_VREG_POSITION(d_vr);
        int s_pos = TCCIR_DECODE_VREG_POSITION(s_vr);
        if (d_tmp <= max_tmp && s_pos <= max_var && var_lea[s_pos] >= 0)
        {
          if (lea_map[d_tmp] >= 0 && lea_map[d_tmp] != var_lea[s_pos])
            mark_var_escaped(var_escaped, var_lea[s_pos]);
          lea_map[d_tmp] = var_lea[s_pos];
        }
      }
    }
  }

  /* Pass 2: scan LEA-TEMP uses for untracked escapes. */
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;

    /* STORE src1=T (value, non-lval) where T is a LEA temp -> pointer escapes. */
    if ((q->op == TCCIR_OP_STORE || q->op == TCCIR_OP_STORE_INDEXED ||
         q->op == TCCIR_OP_STORE_POSTINC) && irop_config[q->op].has_src1)
    {
      IROperand src1 = tcc_ir_op_get_src1(ir, q);
      if (!src1.is_lval)
      {
        int32_t vr = irop_get_vreg(src1);
        if (vr >= 0 && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_TEMP)
        {
          int t = TCCIR_DECODE_VREG_POSITION(vr);
          if (t <= max_tmp && lea_map[t] >= 0)
            mark_var_escaped(var_escaped, lea_map[t]);
        }
      }
    }
    /* RETURNVALUE of a LEA temp -> pointer to local escapes. */
    if (q->op == TCCIR_OP_RETURNVALUE)
    {
      IROperand src1 = tcc_ir_op_get_src1(ir, q);
      int32_t vr = irop_get_vreg(src1);
      if (vr >= 0 && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_TEMP)
      {
        int t = TCCIR_DECODE_VREG_POSITION(vr);
        if (t <= max_tmp && lea_map[t] >= 0)
          mark_var_escaped(var_escaped, lea_map[t]);
      }
    }
  }

  /* Pass 3: record var_last_read[V] from direct VAR reads and LEA-TEMP derefs. */
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;

    for (int k = 0; k < 2; k++)
    {
      int has = (k == 0) ? irop_config[q->op].has_src1 : irop_config[q->op].has_src2;
      if (!has)
        continue;
      IROperand s = (k == 0) ? tcc_ir_op_get_src1(ir, q) : tcc_ir_op_get_src2(ir, q);
      int32_t vr = irop_get_vreg(s);
      if (vr < 0)
        continue;
      int vt = TCCIR_DECODE_VREG_TYPE(vr);
      int vp = TCCIR_DECODE_VREG_POSITION(vr);
      if (vt == TCCIR_VREG_TYPE_VAR && vp <= max_var)
      {
        /* Direct VAR read: even STORE src1 of `STORE dest <- V` reads V. */
        if (i > var_last_read[vp])
          var_last_read[vp] = i;
      }
      else if (vt == TCCIR_VREG_TYPE_TEMP && vp <= max_tmp && lea_map[vp] >= 0 && s.is_lval)
      {
        /* lval deref of LEA TEMP -> read of V's memory. */
        int v = lea_map[vp];
        if (v <= max_var && i > var_last_read[v])
          var_last_read[v] = i;
      }
    }
  }

  /* Pass 4: NOP writes to V after last_read[V] (direct, or via a LEA TEMP). */
  int changes = 0;
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;

    int written_var = -1;
    if (irop_config[q->op].has_dest)
    {
      IROperand dest = tcc_ir_op_get_dest(ir, q);
      int32_t vr = irop_get_vreg(dest);
      if (vr >= 0 && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_VAR && !dest.is_lval)
      {
        /* Direct write: conservative, only side-effect-free ASSIGN/STORE shapes. */
        if (q->op == TCCIR_OP_ASSIGN || q->op == TCCIR_OP_LEA || q->op == TCCIR_OP_STORE ||
            q->op == TCCIR_OP_STORE_INDEXED || q->op == TCCIR_OP_LOAD || q->op == TCCIR_OP_ADD ||
            q->op == TCCIR_OP_SUB || q->op == TCCIR_OP_AND || q->op == TCCIR_OP_OR ||
            q->op == TCCIR_OP_XOR || q->op == TCCIR_OP_MUL || q->op == TCCIR_OP_SHL ||
            q->op == TCCIR_OP_SHR || q->op == TCCIR_OP_SAR || q->op == TCCIR_OP_ZEXT)
          written_var = TCCIR_DECODE_VREG_POSITION(vr);
      }
      else if ((q->op == TCCIR_OP_STORE || q->op == TCCIR_OP_STORE_INDEXED ||
                q->op == TCCIR_OP_STORE_POSTINC) &&
               vr >= 0 && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_TEMP)
      {
        /* STORE/INDEXED/POSTINC imply a memory write regardless of is_lval. */
        int t = TCCIR_DECODE_VREG_POSITION(vr);
        if (t <= max_tmp && lea_map[t] >= 0)
          written_var = lea_map[t];
      }
    }

    if (written_var < 0 || written_var > max_var)
      continue;
    if (var_is_escaped(var_escaped, written_var))
      continue;
    int last_read = var_last_read[written_var];
    if (last_read < 0)
      continue; /* never read -- dead_addrvar handles full elimination */
    if (i <= last_read)
      continue;

    LOG_IR_GEN("=== DEAD TRAILING ADDRVAR STORE: NOP i=%d (V=%d, last_read=%d) ===", i,
               written_var, last_read);
    q->op = TCCIR_OP_NOP;
    changes++;
  }

  tcc_free(var_last_read);
  tcc_free(var_escaped);
  tcc_free(var_lea);
  tcc_free(lea_map);
  return changes;
}

int tcc_ir_opt_dead_trailing_addrvar_store_elim_ex(IROptCtx *ctx)
{
  return tcc_ir_opt_dead_trailing_addrvar_store_elim(ctx->ir);
}
