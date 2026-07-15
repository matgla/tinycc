/*
 *  TCC IR - Fold a chained ADD constant into an indexed load/store displacement (pre-SSA engine)
 *
 *  Copyright (c) 2025 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#pragma once

#include "opt_engine.h"

extern const IROptGen fusion_chain_gens[];
extern const int fusion_chain_gens_count;

int tcc_ir_opt_gens_chain_ex(struct IROptCtx *ctx);

