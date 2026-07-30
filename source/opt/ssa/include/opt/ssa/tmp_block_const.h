/*
 *  TCC IR - SSA block-local TEMP reaching-constant forwarding (ssa:tmp_block_const)
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#pragma once

struct IRSSAOptCtx;

/* Substitute a multi-def TEMP's block-local reaching `T <-- #C` constant into
 * its INT32 value reads (the shape every def_count==1-gated SSA pass skips). */
int ssa_opt_tmp_block_const(struct IRSSAOptCtx *ctx);
