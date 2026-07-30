/*
 *  TCC IR - SSA Target-Specific Optimization Generators (ARM Thumb-2)
 *
 *  Copyright (c) 2025 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#pragma once

#include "ssa_opt.h"

/* ============================================================================
 * ARM Thumb-2 Generators
 *
 * Each generator rewrites one target-specific instruction pattern.
 * Named explicitly for the pattern they match, like thop_* builders.
 * ============================================================================ */

/* MUL + ADD → MLA: fuse single-use multiply into multiply-accumulate */
int ssa_gen_arm_fuse_mul_add_to_mla(IRSSAOptCtx *ctx, int instr_idx);

/* SHL + ADD + LOAD → LOAD_INDEXED: fuse array index computation */
int ssa_gen_arm_fuse_shl_add_to_load_indexed(IRSSAOptCtx *ctx, int instr_idx);

/* SHL + ADD + STORE → STORE_INDEXED: fuse array store computation */
int ssa_gen_arm_fuse_shl_add_to_store_indexed(IRSSAOptCtx *ctx, int instr_idx);

/* MUL → SHL: strength-reduce power-of-2 multiply to shift */
int ssa_gen_arm_reduce_mul_to_shift(IRSSAOptCtx *ctx, int instr_idx);

/* ADD(base, #imm) + LOAD → LOAD_INDEXED(base, #imm, scale=0):
 * fuse base + small constant offset addressing into a single load. */
int ssa_gen_arm_fuse_load_through_add_imm(IRSSAOptCtx *ctx, int instr_idx);

/* ADD(base, #imm) + STORE → STORE_INDEXED(base, val, #imm, scale=0). */
int ssa_gen_arm_fuse_store_through_add_imm(IRSSAOptCtx *ctx, int instr_idx);

/* Register ARM generators with the SSA optimization engine */
void tcc_ir_ssa_opt_arm_register(void);

