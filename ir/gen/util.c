/*
 *  TCC IR - Small State Accessors
 *
 *  Copyright (c) 2025 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#define USING_GLOBALS
#include "ir.h"

int tcc_ir_count(TCCIRState *ir)
{
  return ir ? ir->next_instruction_index : 0;
}

int tcc_ir_current_idx(TCCIRState *ir)
{
  return ir ? ir->next_instruction_index - 1 : -1;
}

int tcc_ir_is_leaf(TCCIRState *ir)
{
  return ir ? ir->leaffunc : 0;
}

void tcc_ir_nonleaf_mark(TCCIRState *ir)
{
  if (ir)
    ir->leaffunc = 0;
}

int tcc_ir_call_id_next(TCCIRState *ir)
{
  if (!ir)
    return 0;
  return ir->next_call_id++;
}
