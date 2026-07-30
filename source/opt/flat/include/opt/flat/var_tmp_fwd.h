/*
 *  TCC IR - VAR→TMP local forwarding (flat DSL pass)
 *
 *  Copyright (c) 2025 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#pragma once

#include "opt_engine.h"

extern const IROptGen var_tmp_fwd_gens[];
extern const int var_tmp_fwd_gens_count;

int tcc_ir_opt_var_tmp_fwd(struct TCCIRState *ir);
int tcc_ir_opt_var_tmp_fwd_ex(struct IROptCtx *ctx);

