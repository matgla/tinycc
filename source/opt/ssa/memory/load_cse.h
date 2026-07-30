/*
 *  TCC IR - SSA Global Load CSE + Stack Store-Load Forwarding
 *
 *  Copyright (c) 2025 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#pragma once

struct IRSSAOptCtx;

/* Global-load CSE + stack store-load forwarding on SSA-form IR: canonicalizes
 * addresses (LEA/stack-slot resolution) and folds redundant loads / forwards
 * dominating stores to their loads.  See ir/opt/ssa_opt.h for the driver. */
int ssa_opt_load_cse(struct IRSSAOptCtx *ctx);

