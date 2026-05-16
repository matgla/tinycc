/*
 *  TCC IR - Transform Primitives (shared pre-SSA optimization helpers)
 *
 *  Copyright (c) 2025 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#define USING_GLOBALS

#include "ir.h"
#include "opt_xform.h"

int ir_xform_same_block(TCCIRState *ir, int from_idx, int to_idx)
{
  for (int j = from_idx + 1; j < to_idx; j++)
  {
    TccIrOp bop = ir->compact_instructions[j].op;
    if (bop == TCCIR_OP_JUMP || bop == TCCIR_OP_JUMPIF)
      return 0;
  }
  return 1;
}