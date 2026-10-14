/*
 *  TCC SSA opt - duplicate diamond reuse
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#pragma once

struct IRSSAOptCtx;

/* A diamond that repeats the test and arms of the diamond just before it takes
 * that diamond's result.  Returns the number of diamonds removed. */
int ssa_opt_dup_diamond(struct IRSSAOptCtx *ctx);
