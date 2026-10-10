/*
 *  TCC - Tiny C Compiler
 *
 *  Copyright (c) 2025 Mateusz Stadnik
 *
 *  Inspired by: https://bitbucket.org/theStack/tccls_poc.git
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#pragma once

#include <stdint.h>

/* Register type for allocation */
#define LS_REG_TYPE_INT 0
#define LS_REG_TYPE_FLOAT 1
#define LS_REG_TYPE_DOUBLE 2
#define LS_REG_TYPE_LLONG 3
#define LS_REG_TYPE_DOUBLE_SOFT 4
#define LS_REG_TYPE_COMPLEX_FLOAT 5
#define LS_REG_TYPE_COMPLEX_DOUBLE 6

/* VFP register marker */
#define LS_VFP_REG_BASE 0x40
#define LS_IS_VFP_REG(r) ((r) >= LS_VFP_REG_BASE && (r) < LS_VFP_REG_BASE + 32)
#define LS_VFP_REG_NUM(r) ((r) - LS_VFP_REG_BASE)

typedef struct LSLiveInterval
{
  /* 24 bytes: the registers fit int8_t (they come from SSAInterval's int8_t
   * r0/r1/precolored), and the uint64_t sort_key nobody read padded it to 40. */
  uint32_t vreg;
  uint32_t stack_location;
  uint32_t start;
  uint32_t end;
  int8_t r0;
  int8_t r1;
  uint8_t crosses_call;
  uint8_t addrtaken;
  uint8_t reg_type;
  uint8_t lvalue;
  uint8_t co_member; /* part of a graph-coalesced class — post-RA move coalescing must not reassign it */
  uint8_t caller_save; /* crosses calls in a caller-saved register the call lowering saves around them */
} LSLiveInterval;

typedef struct LSLiveIntervalState
{
  LSLiveInterval *intervals;
  int intervals_size;
  int next_interval_index;
  uint64_t registers_map;
  uint64_t dirty_registers;
  /* Callee-saved registers no interval uses, reserved by codegen's phase-3
   * fixup for scratch where every other register is taken: free everywhere,
   * saved by the prologue (dirty_registers keeps them). */
  uint32_t spare_scratch_regs;
  int caller_save_count; /* intervals with caller_save set (most functions: none) */
  uint64_t float_registers_map;
  uint64_t dirty_float_registers;

  uint32_t *live_regs_by_instruction;
  int live_regs_by_instruction_size;

  int cached_instruction_idx;
  uint32_t cached_live_regs;

  /* compute_live_regs sweep index; assumes start/end frozen post-RA, adds/clears invalidate,
   * and so must giving a register-less interval a register (the sweep skips those) */
  int *live_sweep_order;
  int *live_sweep_active;
  int live_sweep_valid;
  int live_sweep_count;
  int live_sweep_pos;
  int live_sweep_active_count;
  int live_sweep_last_idx;
} LSLiveIntervalState;

void tcc_ls_initialize(LSLiveIntervalState *ls);
void tcc_ls_deinitialize(LSLiveIntervalState *ls);

void tcc_ls_clear_live_intervals(LSLiveIntervalState *ls);

void tcc_ls_add_live_interval(LSLiveIntervalState *ls, int vreg, int start, int end, int crosses_call, int addrtaken,
                              int reg_type, int lvalue, int precolored_reg);
/* Grow intervals[] to hold at least `count` entries.  Like an add, it may move
 * the array: re-derive any LSLiveInterval* afterwards. */
void tcc_ls_reserve(LSLiveIntervalState *ls, int count);

void tcc_ls_compact_stack_locations(LSLiveIntervalState *ls, int spill_base);
/* The same, with the slots ordered by weights[i] (per interval, summed over
 * the intervals sharing a slot) per byte: the heaviest nearest SP. */
void tcc_ls_compact_stack_locations_weighted(LSLiveIntervalState *ls, int spill_base, const uint32_t *weights);

void tcc_ls_reset_scratch_cache(LSLiveIntervalState *ls);

uint32_t tcc_ls_compute_live_regs(LSLiveIntervalState *ls, int instruction_idx);

/* Lowest-index single-register INT interval (non-addrtaken, non-spilled) holding r at instruction_idx, or -1. */
int tcc_ls_find_int_reg_holder(LSLiveIntervalState *ls, int r, int instruction_idx);

int tcc_ls_reg_held_by_other(const LSLiveIntervalState *ls, int reg, int pos, const LSLiveInterval *skip);
uint8_t *tcc_ls_reg_held_by_other_range(const LSLiveIntervalState *ls, int reg, int start, int end,
                                        const LSLiveInterval *skip);

int tcc_ls_find_free_scratch_reg(LSLiveIntervalState *ls, int instruction_idx, uint32_t exclude_regs, int is_leaf);

void tcc_ls_recompute_dirty_registers(LSLiveIntervalState *ls);
