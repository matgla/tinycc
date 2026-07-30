/*
 *  TCC IR - Global Symbol Address CSE
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#ifndef TCC_OPT_FLAT_SYMADDR_CSE_H
#define TCC_OPT_FLAT_SYMADDR_CSE_H

#include "opt_engine.h"

extern const IROptGen symaddr_cse_gens[];
extern const int symaddr_cse_gens_count;

int tcc_ir_opt_symaddr_cse_ex(struct IROptCtx *ctx);
int tcc_ir_opt_symaddr_cse(struct TCCIRState *ir);
int tcc_ir_opt_symaddr_cse_late(struct TCCIRState *ir);

#endif
