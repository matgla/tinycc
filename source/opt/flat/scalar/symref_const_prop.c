/*
 *  TCC IR - Symref constant propagation (flat pass)
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
#include "opt.h"
#include "opt_engine.h"
#include "opt_utils.h"
#include "opt_du.h"

/* Propagate ASSIGN T <- &S+addend into later TMP uses within a basic block. */
int tcc_ir_opt_symref_const_prop(TCCIRState *ir)
{
  if (!ir || ir->next_instruction_index == 0)
    return 0;

  const int n = ir->next_instruction_index;
  int changes = 0;

  int max_tmp_pos = 0;
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;
    if (!irop_config[q->op].has_dest)
      continue;
    IROperand dest = tcc_ir_op_get_dest(ir, q);
    int32_t dvr = irop_get_vreg(dest);
    if (TCCIR_DECODE_VREG_TYPE(dvr) != TCCIR_VREG_TYPE_TEMP)
      continue;
    const int pos = TCCIR_DECODE_VREG_POSITION(dvr);
    if (pos > max_tmp_pos)
      max_tmp_pos = pos;
  }
  if (max_tmp_pos == 0)
    return 0;

  /* Per-tmp tracked symref. gen 0 means invalid; bumps on block boundaries. */
  typedef struct
  {
    int gen;
    uint32_t pool_idx;
    int btype;
    uint8_t is_local;
    uint8_t is_const;
    uint8_t is_unsigned;
  } SymrefTmp;

  SymrefTmp *map = tcc_mallocz(sizeof(SymrefTmp) * (max_tmp_pos + 1));
  int current_gen = 1;
  int *block_start_seen = tcc_mallocz(sizeof(int) * n);
  int block_start_gen = 1;
  ir_opt_mark_block_starts(ir, block_start_seen, block_start_gen, n);

  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];

    if (i != 0 && block_start_seen[i] == block_start_gen)
      current_gen++;

    if (q->op == TCCIR_OP_NOP)
      continue;

    /* Control-flow ops clear the map; do not rewrite their operands. */
    if (q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_JUMPIF || q->op == TCCIR_OP_IJUMP ||
        q->op == TCCIR_OP_RETURNVALUE || q->op == TCCIR_OP_RETURNVOID)
    {
      current_gen++;
      continue;
    }
    if (q->op == TCCIR_OP_FUNCCALLVAL || q->op == TCCIR_OP_FUNCCALLVOID)
    {
      current_gen++;
      /* fall through to substitute call argument operands */
    }

    /* Substitute symref into operand uses (src1, src2).  Skip the dest. */
    for (int slot = 0; slot < 2; slot++)
    {
      int has = (slot == 0) ? irop_config[q->op].has_src1 : irop_config[q->op].has_src2;
      if (!has)
        continue;
      IROperand opnd = (slot == 0) ? tcc_ir_op_get_src1(ir, q) : tcc_ir_op_get_src2(ir, q);
      /* Operand must be a plain vreg (not already a sym/imm operand). */
      if (opnd.is_sym)
        continue;
      int32_t opnd_vr = irop_get_vreg(opnd);
      if (TCCIR_DECODE_VREG_TYPE(opnd_vr) != TCCIR_VREG_TYPE_TEMP)
        continue;
      int pos = TCCIR_DECODE_VREG_POSITION(opnd_vr);
      if (pos > max_tmp_pos || map[pos].gen != current_gen)
        continue;

      /* Replace with a fresh symref operand carrying use-site flags. */
      IROperand new_opnd = irop_make_symref(-1, map[pos].pool_idx, opnd.is_lval, map[pos].is_local,
                                            map[pos].is_const, irop_get_btype(opnd));
      new_opnd.is_unsigned = opnd.is_unsigned;

      if (slot == 0)
        tcc_ir_set_src1(ir, i, new_opnd);
      else
        tcc_ir_set_src2(ir, i, new_opnd);
      changes++;
    }

    /* Record fresh ASSIGN(symref) defs; any other write to the tmp kills it. */
    if (irop_config[q->op].has_dest)
    {
      IROperand dest = tcc_ir_op_get_dest(ir, q);
      int32_t dvr = irop_get_vreg(dest);
      if (TCCIR_DECODE_VREG_TYPE(dvr) == TCCIR_VREG_TYPE_TEMP)
      {
        int pos = TCCIR_DECODE_VREG_POSITION(dvr);
        int recorded = 0;
        if (q->op == TCCIR_OP_ASSIGN)
        {
          IROperand src1 = tcc_ir_op_get_src1(ir, q);
          if (src1.is_sym && !src1.is_lval && pos <= max_tmp_pos)
          {
            map[pos].gen = current_gen;
            map[pos].pool_idx = (uint32_t)src1.u.pool_idx;
            map[pos].btype = irop_get_btype(src1);
            map[pos].is_local = src1.is_local;
            map[pos].is_const = src1.is_const;
            map[pos].is_unsigned = src1.is_unsigned;
            recorded = 1;
          }
        }
        /* Not a fresh copy record → this write kills any tracked symref. */
        if (!recorded && pos <= max_tmp_pos && map[pos].gen == current_gen)
          map[pos].gen = 0;
      }
    }
  }

  tcc_free(map);
  tcc_free(block_start_seen);
  return changes;
}

