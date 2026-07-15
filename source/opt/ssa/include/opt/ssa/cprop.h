/*
 *  TCC SSA opt - copy/constant propagation
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#pragma once

struct IRSSAOptCtx;

int ssa_opt_cprop(struct IRSSAOptCtx *ctx);
int ssa_opt_symref_operand_cse(struct IRSSAOptCtx *ctx);
int ssa_opt_var_forward(struct IRSSAOptCtx *ctx);
int ssa_opt_var_to_param_forward(struct IRSSAOptCtx *ctx);
int ssa_opt_var_const_fold(struct IRSSAOptCtx *ctx);
int ssa_opt_const_prop_tmp(struct IRSSAOptCtx *ctx);

