/*
 *  TCC IR - Narrow a plain STORE's value btype to its access width (pre-SSA)
 *
 *  Copyright (c) 2025 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#pragma once

#include "opt_engine.h"

extern const IROptGen narrow_store_gens[];
extern const int narrow_store_gens_count;

int tcc_ir_opt_narrow_store_value_btype(struct TCCIRState *ir);

