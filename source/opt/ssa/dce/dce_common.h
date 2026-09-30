/*
 *  TCC IR - SSA DCE: helpers shared by more than one sub-pass
 *
 *  Copyright (c) 2025 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#pragma once

#include <stdint.h>

struct IRSSAOptCtx;

/* Every dead-store killer: does deleting this instruction delete a volatile
 * access?  A memory operand marked volatile, or a source naming a volatile
 * variable -- a fused `STORE dead <- lval(V)` embodies the read of V, and C
 * requires that read even when the stored-to slot is never read. */
struct TCCIRState;
struct IRQuadCompact;
int dce_instr_reads_volatile(struct TCCIRState *ir, struct IRQuadCompact *q);

/* dead_var_stores + stackloc_stores */
int sl_temp_has_live_uses(struct IRSSAOptCtx *ctx, int32_t vreg);

/* dead_overwrite_stores + dead_global_stores */
int sl_store_byte_width(int btype);
