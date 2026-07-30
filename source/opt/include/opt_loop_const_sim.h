/*
 *  TCC IR - Loop Constant Simulation
 *
 *  Copyright (c) 2025 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#pragma once

#include "ir.h"
#include "opt_engine.h"

/* allow_extension enables the rotated-range tail extension (SSA callers pass 0); 1 if folded. */
int lcs_fold_region(struct TCCIRState *ir, int start_idx, int end_idx,
                    int header_idx, int preheader_idx, int allow_extension);
