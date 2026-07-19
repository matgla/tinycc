/*
 *  TCC IR - Address-of-VAR forwarding (pre-SSA)
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
#include "opt_utils.h"

/* Within one block: V=#N; T=&V; deref of T becomes #N, if &V never escapes. */
int tcc_ir_opt_addrof_var_fwd(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  int changes = 0;

  if (n < 3)
    return 0;

  /* Called directly from function_pipeline.c, not through the pass table, so the
   * bisection knob has to be checked here (docs/addrof_var_fwd_ssa_migration.md). */
  if (tcc_ir_opt_pass_disabled("addrof_var_fwd"))
    return 0;

  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    /* A constant write to a local VAR is ASSIGN or STORE depending on frontend path. */
    if (q->op != TCCIR_OP_ASSIGN && q->op != TCCIR_OP_STORE)
      continue;

    IROperand dest = tcc_ir_op_get_dest(ir, q);
    int32_t v_dest_vr = irop_get_vreg(dest);
    if (v_dest_vr < 0 || TCCIR_DECODE_VREG_TYPE(v_dest_vr) != TCCIR_VREG_TYPE_VAR)
      continue;

    IROperand src = tcc_ir_op_get_src1(ir, q);
    if (irop_get_tag(src) != IROP_TAG_IMM32)
      continue;

    int v_btype = irop_get_btype(dest);
    if (v_btype != IROP_BTYPE_INT32)
      continue;

    int32_t imm_val = src.u.imm32;

#define ADDROF_VAR_MAX_REWRITES 32
#define ADDROF_VAR_MAX_ALIASES 16
    int rewrites_idx[ADDROF_VAR_MAX_REWRITES];
    int rewrites_slot[ADDROF_VAR_MAX_REWRITES]; /* 0 = src1, 1 = src2 */
    int rewrite_count = 0;
    /* alias[]: vregs that currently hold &V_p (TEMP or VAR). */
    int32_t alias[ADDROF_VAR_MAX_ALIASES];
    int alias_count = 0;
    int aborted = 0;

    for (int j = i + 1; j < n && !aborted; j++)
    {
      IRQuadCompact *jq = &ir->compact_instructions[j];
      if (jq->op == TCCIR_OP_NOP)
        continue;

      if (jq->is_jump_target)
        break;
      if (jq->op == TCCIR_OP_JUMP || jq->op == TCCIR_OP_JUMPIF || jq->op == TCCIR_OP_IJUMP ||
          jq->op == TCCIR_OP_RETURNVALUE || jq->op == TCCIR_OP_RETURNVOID ||
          jq->op == TCCIR_OP_SWITCH_TABLE)
        break;
      if (jq->op == TCCIR_OP_FUNCCALLVAL || jq->op == TCCIR_OP_FUNCCALLVOID)
        break;

      int handled = 0;

      /* LEA T = &V_p creates the first alias. */
      if (jq->op == TCCIR_OP_LEA)
      {
        IROperand ldest = tcc_ir_op_get_dest(ir, jq);
        IROperand lsrc = tcc_ir_op_get_src1(ir, jq);
        int32_t lsrc_vr = irop_get_vreg(lsrc);
        int32_t ldest_vr = irop_get_vreg(ldest);
        if (lsrc_vr == v_dest_vr && !lsrc.is_lval &&
            TCCIR_DECODE_VREG_TYPE(ldest_vr) == TCCIR_VREG_TYPE_TEMP)
        {
          if (alias_count >= ADDROF_VAR_MAX_ALIASES)
          {
            aborted = 1;
            break;
          }
          alias[alias_count++] = ldest_vr;
          handled = 1;
        }
      }

      /* STORE V_a <-- T_alias: src has is_lval=0, it stores a TEMP's pointer value. */
      if (!handled && jq->op == TCCIR_OP_STORE)
      {
        IROperand sdest = tcc_ir_op_get_dest(ir, jq);
        IROperand ssrc = tcc_ir_op_get_src1(ir, jq);
        int32_t sdest_vr = irop_get_vreg(sdest);
        int32_t ssrc_vr = irop_get_vreg(ssrc);
        if (!ssrc.is_lval && sdest_vr >= 0 && ssrc_vr >= 0 &&
            TCCIR_DECODE_VREG_TYPE(sdest_vr) == TCCIR_VREG_TYPE_VAR &&
            TCCIR_DECODE_VREG_TYPE(ssrc_vr) == TCCIR_VREG_TYPE_TEMP)
        {
          for (int k = 0; k < alias_count; k++)
          {
            if (alias[k] == ssrc_vr)
            {
              if (alias_count >= ADDROF_VAR_MAX_ALIASES)
              {
                aborted = 1;
              }
              else
              {
                alias[alias_count++] = sdest_vr;
                handled = 1;
              }
              break;
            }
          }
        }
      }

      /* ASSIGN T_b <-- V_alias is a slot load of the pointer, not a deref: propagate the alias. */
      if (!handled && jq->op == TCCIR_OP_ASSIGN)
      {
        IROperand adest = tcc_ir_op_get_dest(ir, jq);
        IROperand asrc = tcc_ir_op_get_src1(ir, jq);
        int32_t adest_vr = irop_get_vreg(adest);
        int32_t asrc_vr = irop_get_vreg(asrc);
        if (adest_vr >= 0 && asrc_vr >= 0 &&
            TCCIR_DECODE_VREG_TYPE(adest_vr) == TCCIR_VREG_TYPE_TEMP &&
            TCCIR_DECODE_VREG_TYPE(asrc_vr) == TCCIR_VREG_TYPE_VAR &&
            asrc.is_lval)
        {
          for (int k = 0; k < alias_count; k++)
          {
            if (alias[k] == asrc_vr)
            {
              if (alias_count >= ADDROF_VAR_MAX_ALIASES)
              {
                aborted = 1;
              }
              else
              {
                alias[alias_count++] = adest_vr;
                handled = 1;
              }
              break;
            }
          }
        }
      }

      if (handled)
        continue;
      if (aborted)
        break;

      /* Redefinition of V_p or a tracked alias invalidates; VAR dest always has is_lval=1. */
      if (irop_config[jq->op].has_dest)
      {
        IROperand d = tcc_ir_op_get_dest(ir, jq);
        int32_t d_vr = irop_get_vreg(d);
        if (d_vr == v_dest_vr)
        {
          aborted = 1;
          break;
        }
        for (int k = 0; k < alias_count; k++)
        {
          if (d_vr == alias[k])
          {
            aborted = 1;
            break;
          }
        }
        if (aborted)
          break;
      }

      for (int s = 0; s < 2 && !aborted; s++)
      {
        if (s == 0 && !irop_config[jq->op].has_src1)
          continue;
        if (s == 1 && !irop_config[jq->op].has_src2)
          continue;
        IROperand u = (s == 0) ? tcc_ir_op_get_src1(ir, jq) : tcc_ir_op_get_src2(ir, jq);
        int32_t u_vr = irop_get_vreg(u);
        if (u_vr < 0)
          continue;

        for (int k = 0; k < alias_count; k++)
        {
          if (u_vr != alias[k])
            continue;
          /* Only a TEMP alias read as a deref is rewritable; anything else escapes. */
          if (TCCIR_DECODE_VREG_TYPE(u_vr) != TCCIR_VREG_TYPE_TEMP)
          {
            aborted = 1;
            break;
          }
          if (!u.is_lval)
          {
            aborted = 1;
            break;
          }
          if (irop_get_btype(u) != v_btype)
          {
            aborted = 1;
            break;
          }
          if (rewrite_count >= ADDROF_VAR_MAX_REWRITES)
          {
            aborted = 1;
            break;
          }
          rewrites_idx[rewrite_count] = j;
          rewrites_slot[rewrite_count] = s;
          rewrite_count++;
        }
      }
    }

    if (aborted || rewrite_count == 0)
      continue;

    for (int r = 0; r < rewrite_count; r++)
    {
      int idx = rewrites_idx[r];
      IROperand orig = (rewrites_slot[r] == 1) ? tcc_ir_get_src2(ir, idx) : tcc_ir_get_src1(ir, idx);
      IROperand newop = irop_make_imm32(-1, imm_val, irop_get_btype(orig));
      newop.is_unsigned = orig.is_unsigned;
      if (rewrites_slot[r] == 1)
        tcc_ir_set_src2(ir, idx, newop);
      else
        tcc_ir_set_src1(ir, idx, newop);
      changes++;
    }
#undef ADDROF_VAR_MAX_REWRITES
#undef ADDROF_VAR_MAX_ALIASES
  }

  return changes;
}

int tcc_ir_opt_addrof_var_fwd_ex(IROptCtx *ctx) { return tcc_ir_opt_addrof_var_fwd(ctx->ir); }
