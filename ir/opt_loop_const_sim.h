/*
 *  TCC IR - Loop Constant Simulation
 *
 *  Copyright (c) 2025 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#ifndef TCC_IR_OPT_LOOP_CONST_SIM_H
#define TCC_IR_OPT_LOOP_CONST_SIM_H

#include "ir.h"
#include "opt_engine.h"

/* Shared loop-fold engine over an explicit flat region, driven by
 * ssa_opt_loop_const_sim (ir/opt/ssa_opt_loop.c).  allow_extension enables the
 * rotated-range tail extension; SSA callers pass exact membership and disable
 * it.  Returns 1 if folded. */
int lcs_fold_region(struct TCCIRState *ir, int start_idx, int end_idx,
                    int header_idx, int preheader_idx, int allow_extension);

#endif /* TCC_IR_OPT_LOOP_CONST_SIM_H */
