/*
 *  TCC IR - SSA straight-line global store DSE
 *
 *  Copyright (c) 2025 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#ifndef TCC_OPT_SSA_MEMORY_GLOBAL_STORE_DSE_H
#define TCC_OPT_SSA_MEMORY_GLOBAL_STORE_DSE_H

int ssa_opt_global_store_dse(IRSSAOptCtx *ctx);

#endif
