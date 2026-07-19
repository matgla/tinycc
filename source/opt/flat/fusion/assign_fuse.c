/*
 *  TCC IR - Fusion & Addressing Mode Optimization
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
#include "opt_loop_utils.h"
#include "opt_alias.h"
#include "opt_utils.h"

extern int gsym_cse_insert_before(TCCIRState *ir, int before_idx, IRQuadCompact *new_q);



/*
 * Fuse an ASSIGN whose source is a single-use, single-def TEMP produced by the
 * immediately preceding instruction: rewrite the producer's dest to the
 * ASSIGN's dest and NOP the ASSIGN.
 */

int tcc_ir_opt_assign_fuse(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  int changes = 0;
  if (n < 2)
    return 0;

  IROptDU du;
  ir_opt_du_build_mode(ir, &du, IR_DU_MODE_TMP_ONLY);

  for (int i = 1; i < n; i++)
  {
    IRQuadCompact *q_asn = &ir->compact_instructions[i];
    if (q_asn->op != TCCIR_OP_ASSIGN)
      continue;
    if (q_asn->is_jump_target)
      continue;

    IROperand asn_src = tcc_ir_op_get_src1(ir, q_asn);
    int32_t src_vr = irop_get_vreg(asn_src);
    if (TCCIR_DECODE_VREG_TYPE(src_vr) != TCCIR_VREG_TYPE_TEMP)
      continue;
    if (ir_opt_du_uses(&du, src_vr) != 1 || !ir_opt_du_is_single_def(&du, src_vr))
      continue;
    if (asn_src.is_lval)
      continue;

    int def_i = ir_opt_du_def(&du, src_vr, n);
    if (def_i < 0 || def_i >= i)
      continue;

    /* The producer must be the immediately preceding non-NOP instruction
     * in the same basic block (no jump targets between them). */
    int between_ok = 1;
    for (int j = def_i + 1; j < i; j++)
    {
      IRQuadCompact *qj = &ir->compact_instructions[j];
      if (qj->op != TCCIR_OP_NOP) { between_ok = 0; break; }
      if (qj->is_jump_target) { between_ok = 0; break; }
    }
    if (!between_ok)
      continue;

    IRQuadCompact *q_def = &ir->compact_instructions[def_i];
    /* Only fuse defs whose dest semantics is a plain register write. */
    switch (q_def->op)
    {
    case TCCIR_OP_NOP:
    case TCCIR_OP_STORE:
    case TCCIR_OP_STORE_INDEXED:
    case TCCIR_OP_STORE_POSTINC:
    case TCCIR_OP_FUNCPARAMVAL:
    case TCCIR_OP_FUNCCALLVAL:  /* call result lands in a fixed register */
    case TCCIR_OP_CMP:
    case TCCIR_OP_TEST_ZERO:
    case TCCIR_OP_JUMP:
    case TCCIR_OP_JUMPIF:
    case TCCIR_OP_IJUMP:
    case TCCIR_OP_RETURNVOID:
    case TCCIR_OP_RETURNVALUE:
    case TCCIR_OP_VLA_ALLOC:
    case TCCIR_OP_VLA_SP_SAVE:
    case TCCIR_OP_VLA_SP_RESTORE:
      continue;
    default:
      break;
    }

    /* Skip if the ASSIGN's dest types differ from the source's: a
     * sub-word ASSIGN may truncate or widen, which the producer can't
     * faithfully reproduce by writing to a different dest. */
    IROperand asn_dest = tcc_ir_op_get_dest(ir, q_asn);
    IROperand def_dest = tcc_ir_op_get_dest(ir, q_def);
    if (irop_get_btype(asn_dest) != irop_get_btype(def_dest))
      continue;
    if (asn_dest.is_lval)
      continue;

    LOG_IR_GEN("OPTIMIZE: assign_fuse def_i=%d asn_i=%d (T%d → T%d)", def_i, i,
               TCCIR_DECODE_VREG_POSITION(src_vr), TCCIR_DECODE_VREG_POSITION(irop_get_vreg(asn_dest)));
    tcc_ir_set_dest(ir, def_i, asn_dest);
    q_asn->op = TCCIR_OP_NOP;
    changes++;
  }

  tcc_free(du.def);
  return changes;
}

int tcc_ir_opt_assign_fuse_ex(IROptCtx *ctx) { return tcc_ir_opt_assign_fuse(ctx->ir); }
