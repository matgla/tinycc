/*
 *  TCC IR - FP materialization cache shims
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

extern void tcc_opt_fp_mat_cache_init(TCCIRState *ir);
extern void tcc_opt_fp_mat_cache_clear(TCCIRState *ir);
extern void tcc_opt_fp_mat_cache_free(TCCIRState *ir);
extern int tcc_opt_fp_mat_cache_lookup(TCCIRState *ir, int offset, int *phys_reg);
extern void tcc_opt_fp_mat_cache_record(TCCIRState *ir, int offset, int phys_reg);
extern void tcc_opt_fp_mat_cache_invalidate_reg(TCCIRState *ir, int phys_reg);

void tcc_ir_opt_fp_cache_init(TCCIRState *ir)
{
  tcc_opt_fp_mat_cache_init(ir);
}

void tcc_ir_opt_fp_cache_clear(TCCIRState *ir)
{
  tcc_opt_fp_mat_cache_clear(ir);
}

void tcc_ir_opt_fp_cache_free(TCCIRState *ir)
{
  tcc_opt_fp_mat_cache_free(ir);
}

int tcc_ir_opt_fp_cache_lookup(TCCIRState *ir, int offset, int *phys_reg)
{
  return tcc_opt_fp_mat_cache_lookup(ir, offset, phys_reg);
}

void tcc_ir_opt_fp_cache_record(TCCIRState *ir, int offset, int phys_reg)
{
  tcc_opt_fp_mat_cache_record(ir, offset, phys_reg);
}

void tcc_ir_opt_fp_cache_invalidate_reg(TCCIRState *ir, int phys_reg)
{
  tcc_opt_fp_mat_cache_invalidate_reg(ir, phys_reg);
}

