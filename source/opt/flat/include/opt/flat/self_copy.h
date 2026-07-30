/*
 *  TCC IR - Self-copy (memcpy/memmove dst==src) elimination (pre-SSA engine)
 *
 *  Copyright (c) 2025 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#pragma once

#include "opt_engine.h"

extern const IROptGen self_copy_gens[];
extern const int self_copy_gens_count;

int tcc_ir_opt_self_copy_elim(struct TCCIRState *ir);
int tcc_ir_opt_self_copy_elim_ex(struct IROptCtx *ctx);

