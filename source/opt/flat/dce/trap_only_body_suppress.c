/*
 *  TCC IR - Trap-Only Body Suppression
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

int tcc_ir_opt_trap_only_body_suppress(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  if (n == 0)
    return 0;
  if (!tcc_state || TCC_OPT(tcc_state, optimize) < 2)
    return 0;
  if (ir->naked)
    return 0;

  int trap_idx = -1;
  for (int i = 0; i < n; i++)
  {
    int op = ir->compact_instructions[i].op;
    if (op == TCCIR_OP_NOP)
      continue;
    if (op == TCCIR_OP_TRAP && trap_idx < 0)
    {
      trap_idx = i;
      continue;
    }
    /* Any other live op (or a second TRAP) — not a pure trap-only body. */
    return 0;
  }
  if (trap_idx < 0)
    return 0;

  LOG_IR_GEN("TRAP-ONLY-BODY: body collapsed to a single TRAP at i=%d — "
             "suppressing prologue/epilogue", trap_idx);
  ir_opt_reset_body_regs(ir);
  ir->noreturn = 1;
  tcc_state->need_frame_pointer = 0;
  tcc_state->force_frame_pointer = 0;
  return 1;
}
