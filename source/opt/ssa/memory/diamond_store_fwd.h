/*
 *  TCC IR - SSA Diamond Store Forwarding
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#ifndef TCC_OPT_SSA_DIAMOND_STORE_FWD_H
#define TCC_OPT_SSA_DIAMOND_STORE_FWD_H

struct IRSSAOptCtx;
struct TCCIRState;

/* forward the constant both diamond arms store to the post-merge LOAD_INDEXED */
int ssa_opt_diamond_store_fwd(struct IRSSAOptCtx *ctx);

/* ungated core over bare IR, shared with the unit tests */
int ssa_opt_diamond_store_fwd_core(struct TCCIRState *ir);

#endif
