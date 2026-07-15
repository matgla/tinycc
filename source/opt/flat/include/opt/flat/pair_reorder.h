/*
 *  TCC IR - Reorder adjacent word-strided indexed load/store pairs into program order (pre-SSA engine)
 *
 *  Copyright (c) 2025 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#pragma once

#include "opt_engine.h"

extern const IROptGen fusion_pair_reorder_gens[];
extern const int fusion_pair_reorder_gens_count;

int tcc_ir_opt_gens_pair_reorder_ex(struct IROptCtx *ctx);

