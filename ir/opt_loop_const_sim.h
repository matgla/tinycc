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

int tcc_ir_opt_loop_const_sim(struct TCCIRState *ir);
int tcc_ir_opt_loop_const_sim_ex(struct IROptCtx *ctx);

#endif /* TCC_IR_OPT_LOOP_CONST_SIM_H */
