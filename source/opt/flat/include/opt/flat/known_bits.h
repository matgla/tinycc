/*
 *  TCC IR - Known-Bits Propagation (flat DSL pass)
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#pragma once

#include "opt_engine.h"

extern const IROptGen known_bits_gens[];
extern const int known_bits_gens_count;

int tcc_ir_opt_known_bits(struct TCCIRState *ir);
int tcc_ir_opt_known_bits_ex(struct IROptCtx *ctx);

