/*
 *  TCC IR - SSA DCE: sub-pass entry points driven by ssa_opt_dce()
 *
 *  Copyright (c) 2025 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#pragma once

struct IRSSAOptCtx;

int dce_temp_worklist(struct IRSSAOptCtx *ctx);
int dce_unreachable(struct IRSSAOptCtx *ctx);
int dce_dead_var_stores(struct IRSSAOptCtx *ctx);
int dce_orphan_params(struct IRSSAOptCtx *ctx);
int dce_dead_stackloc_stores(struct IRSSAOptCtx *ctx);
int dce_ret_path_frame_store(struct IRSSAOptCtx *ctx);
int dce_dead_phi_cycles(struct IRSSAOptCtx *ctx);
int dce_dead_overwrite_stores(struct IRSSAOptCtx *ctx);
int dce_dead_global_stores(struct IRSSAOptCtx *ctx);
int dce_var_liveness(struct IRSSAOptCtx *ctx);
