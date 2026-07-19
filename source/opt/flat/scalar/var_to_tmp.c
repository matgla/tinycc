/*
 *  TCC IR - Single-def local VAR to TEMP promotion
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

int tcc_ir_opt_var_to_tmp(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  int changes = 0;

  if (n < 2)
    return 0;

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

    /* A VAR dest carries is_lval=1 because a VAR is memory, not because its address was taken: count it regardless. */
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

    /* Only lval src1 reloads into a TEMP and FUNCPARAM value passes are rewritable; any other use disqualifies V. */
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
        /* VAR in src2 is never the "T <-- V [ASSIGN lval]" pattern */
        int p = TCCIR_DECODE_VREG_POSITION(v);
        info[p].bad_use = 1;
      }
    }
  }

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

    /* Wider/multi-word types need memory semantics a single TMP can't reproduce. */
    if (def_btype != IROP_BTYPE_INT32)
    {
      LOG_COPY_PROP("var_to_tmp SKIP V:%d: def btype=%d not INT32", p, def_btype);
      continue;
    }

    /* No interval means the type flags below can't be verified, so skip conservatively. */
    IRLiveInterval *intv = tcc_ir_get_live_interval(ir, dest_vr);
    if (!intv)
    {
      LOG_COPY_PROP("var_to_tmp SKIP V:%d: null interval", p);
      continue;
    }
    if (intv->addrtaken || intv->is_volatile || intv->is_complex || intv->is_llong || intv->is_float || intv->is_double || intv->is_lvalue)
    {
      LOG_COPY_PROP("var_to_tmp SKIP V:%d: flags addr=%d vol=%d cx=%d ll=%d f=%d d=%d lv=%d", p, intv->addrtaken,
                    intv->is_volatile, intv->is_complex, intv->is_llong, intv->is_float, intv->is_double, intv->is_lvalue);
      continue;
    }

    /* Allowlist = pure value-writes only; STORE is excluded because rewriting it to ASSIGN miscompiles later passes. */
    /* The def op is left untouched below, so a STORE def would also need flipping to ASSIGN or the backend reads the TMP dest as a pointer deref. */
    int def_op = def_q->op;
    if (def_op != TCCIR_OP_ASSIGN && def_op != TCCIR_OP_LOAD && def_op != TCCIR_OP_ADD && def_op != TCCIR_OP_SUB &&
        def_op != TCCIR_OP_MUL && def_op != TCCIR_OP_AND && def_op != TCCIR_OP_OR && def_op != TCCIR_OP_XOR &&
        def_op != TCCIR_OP_SHL && def_op != TCCIR_OP_SHR && def_op != TCCIR_OP_LEA)
    {
      LOG_COPY_PROP("var_to_tmp SKIP V:%d: def op=%d not in allowlist", p, def_op);
      continue;
    }

    int uses[16];
    int num_uses = 0;
    int aborted = 0;

    for (int j = def_i + 1; j < n; j++)
    {
      IRQuadCompact *q = &ir->compact_instructions[j];
      if (q->op == TCCIR_OP_NOP)
        continue;

      if (q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_JUMPIF || q->op == TCCIR_OP_RETURNVALUE ||
          q->op == TCCIR_OP_RETURNVOID || q->op == TCCIR_OP_SWITCH_TABLE || q->op == TCCIR_OP_IJUMP)
        break;
      /* Calls clobber caller-saved regs, so they end the scan region */
      if (q->op == TCCIR_OP_FUNCCALLVAL || q->op == TCCIR_OP_FUNCCALLVOID)
        break;

      /* Defensive: single-def was already established, so a redef here shouldn't occur */
      if (irop_config[q->op].has_dest)
      {
        IROperand d = tcc_ir_op_get_dest(ir, q);
        if (irop_get_vreg(d) == dest_vr)
        {
          aborted = 1;
          break;
        }
      }

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
        /* Narrowing/widening loads can't be replaced by a direct register copy */
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
    /* A count mismatch means V is also read outside this BB */
    if (num_uses != info[p].lval_read_cnt)
    {
      LOG_COPY_PROP("var_to_tmp SKIP V:%d: cross-BB uses (BB=%d global=%d)", p, num_uses, info[p].lval_read_cnt);
      continue;
    }

    int32_t new_tmp = tcc_ir_vreg_alloc_temp(ir);
    if (new_tmp < 0)
      continue;

    /* irop_make_vreg clears is_local / is_llocal / u.*, preserving only btype */
    IROperand new_dest = irop_make_vreg(new_tmp, def_btype);
    new_dest.is_unsigned = def_dest.is_unsigned;
    tcc_ir_set_dest(ir, def_i, new_dest);

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

int tcc_ir_opt_var_to_tmp_ex(IROptCtx *ctx) { return tcc_ir_opt_var_to_tmp(ctx->ir); }
