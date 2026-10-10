/*
 *  TCC IR - RETURNVALUE merge: duplicate constant-return sites become jumps to the first
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

/* RETURNVALUE codegen is `mov r0, imm; b epilogue`, JUMP is just `b`: one instruction saved per merge. */
int tcc_ir_opt_returnvalue_merge(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  int changes = 0;

  /* Linear scan keyed by value; capped at 32 to keep the search bounded. */
  struct { int64_t value; int idx; } first_ret[32];
  int num_first_ret = 0;

  for (int i = 0; i < n; i++) {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_RETURNVALUE) continue;

    if (!tcc_ir_op_src1_is_imm(ir, q) || tcc_ir_op_src1_is_lval(ir, q)) continue;

    /* 64-bit/float materialization exceeds one instruction, so a branch may save nothing. */
    int btype = tcc_ir_op_src1_btype(ir, q);
    if (btype == IROP_BTYPE_INT64 || btype == IROP_BTYPE_FLOAT32 ||
        btype == IROP_BTYPE_FLOAT64)
      continue;

    int64_t val = tcc_ir_op_src1_imm(ir, q);

    int canonical = -1;
    for (int j = 0; j < num_first_ret; j++) {
      if (first_ret[j].value == val) {
        canonical = first_ret[j].idx;
        break;
      }
    }

    if (canonical >= 0) {
      /* JUMP dest holds the target IR instruction index. */
      q->op = TCCIR_OP_JUMP;
      tcc_ir_op_set_dest_imm32(ir, q, canonical, IROP_BTYPE_INT32);
      tcc_ir_op_set_src1_none(ir, q);
      tcc_ir_op_set_src2_none(ir, q);
      changes++;
    } else if (num_first_ret < (int)(sizeof(first_ret) / sizeof(first_ret[0]))) {
      first_ret[num_first_ret].value = val;
      first_ret[num_first_ret].idx = i;
      num_first_ret++;
    }
  }

  return changes;
}
