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
  int16_t r0;
  int16_t r1;
  uint32_t vreg;
  uint32_t stack_location;
  uint32_t start;
  uint32_t end;
  uint8_t crosses_call;
  uint8_t addrtaken;
  uint8_t reg_type;
  uint8_t lvalue;
  uint64_t sort_key;
} LSLiveInterval;

typedef struct LSLiveIntervalState
{
  LSLiveInterval *intervals;
  int intervals_size;
  int next_interval_index;
  LSLiveInterval **active_set;
  int next_active_index;
  uint64_t registers_map;
  uint64_t dirty_registers;
  uint64_t float_registers_map;
  uint64_t dirty_float_registers;

  uint32_t *live_regs_by_instruction;
  int live_regs_by_instruction_size;

  int cached_instruction_idx;
  uint32_t cached_live_regs;
} LSLiveIntervalState;

void tcc_ls_initialize(LSLiveIntervalState *ls);
void tcc_ls_deinitialize(LSLiveIntervalState *ls);

void tcc_ls_clear_live_intervals(LSLiveIntervalState *ls);

void tcc_ls_add_live_interval(LSLiveIntervalState *ls, int vreg, int start, int end, int crosses_call, int addrtaken,
                              int reg_type, int lvalue, int precolored_reg);

void tcc_ls_compact_stack_locations(LSLiveIntervalState *ls, int spill_base);

void tcc_ls_reset_scratch_cache(LSLiveIntervalState *ls);

int tcc_ls_find_free_scratch_reg(LSLiveIntervalState *ls, int instruction_idx, uint32_t exclude_regs, int is_leaf);

void tcc_ls_recompute_dirty_registers(LSLiveIntervalState *ls);
