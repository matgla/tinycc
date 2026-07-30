/*
 *  TCC IR - memory-initializer pass group driver (flat, pre-SSA)
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



int ssa_opt_mem_init(TCCIRState *ir)
{
  int changes = 0;
  changes += tcc_ir_opt_block_copy_init(ir);
  tcc_ir_dump_after_pass(ir, "block_copy_init");
  changes += tcc_ir_opt_small_memset_to_store(ir);
  tcc_ir_dump_after_pass(ir, "small_memset_to_store");
  changes += tcc_ir_opt_small_global_memset_to_store(ir);
  tcc_ir_dump_after_pass(ir, "small_global_memset_to_store");
  return changes;
}
