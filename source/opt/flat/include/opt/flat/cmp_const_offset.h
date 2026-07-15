/*
 *  TCC IR - CMP constant-offset fold (pre-SSA engine)
 *
 *  Copyright (c) 2025 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#pragma once

#include "opt_engine.h"

extern const IROptGen cmp_const_offset_gens[];
extern const int cmp_const_offset_gens_count;

int tcc_ir_opt_cmp_const_offset_fold(struct TCCIRState *ir);
int tcc_ir_opt_cmp_const_offset_fold_ex(struct IROptCtx *ctx);

