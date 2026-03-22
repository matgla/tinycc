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

// linear scan implementation for register allocation

/* Register type for allocation */
#define LS_REG_TYPE_INT 0
#define LS_REG_TYPE_FLOAT 1
#define LS_REG_TYPE_DOUBLE 2
#define LS_REG_TYPE_LLONG                                                                                              \
  3 /* 64-bit integer (long long) - needs 2 int regs                                                                   \
     */
#define LS_REG_TYPE_DOUBLE_SOFT                                                                                        \
  4 /* double in soft-float - needs 2 int regs                                                                         \
     */
#define LS_REG_TYPE_COMPLEX_FLOAT 5  /* Phase 3: complex float - needs 2 int regs for real+imag */
#define LS_REG_TYPE_COMPLEX_DOUBLE 6 /* complex double - always spilled (128-bit = 16 bytes) */

/* VFP register marker - add to VFP register number to distinguish from integer
 * registers */
#define LS_VFP_REG_BASE 0x40 /* VFP registers are encoded as 0x40 + Sn */
#define LS_IS_VFP_REG(r) ((r) >= LS_VFP_REG_BASE && (r) < LS_VFP_REG_BASE + 32)
#define LS_VFP_REG_NUM(r) ((r) - LS_VFP_REG_BASE) /* Extract Sn number */

typedef struct LSLiveInterval
{
  int16_t r0;              // physical register assigned
  int16_t r1;              // second physical register assigned (for long long)
  uint32_t vreg;           // virtual register number
  uint32_t stack_location; // stack location if spilled
  uint32_t start;          // start instruction index
  uint32_t end;            // end instruction index
  uint8_t crosses_call;    // 1 if interval spans a function call
  uint8_t addrtaken;       // 1 if variable's address is taken (must be on stack)
  uint8_t reg_type;        // LS_REG_TYPE_INT, LS_REG_TYPE_FLOAT, or LS_REG_TYPE_DOUBLE
  uint8_t lvalue;          // 1 if interval represents an lvalue
} LSLiveInterval;

typedef struct LSLiveIntervalState
{
  LSLiveInterval *intervals;
  int intervals_size;
  int next_interval_index;
  LSLiveInterval **active_set;
  int next_active_index;
  uint64_t registers_map;         // integer registers
  uint64_t dirty_registers;       // integer registers that were used
  uint64_t float_registers_map;   // VFP registers (s0-s31 mapped to bits 0-31)
  uint64_t dirty_float_registers; // VFP registers that were used

  /* Optional precomputed table: live integer registers bitmap at each IR instruction.
   * If present, scratch register lookup can be O(1).
   */
  uint32_t *live_regs_by_instruction;
  int live_regs_by_instruction_size;

  /* Cache for scratch register lookup - avoid recomputing for same instruction */
  int cached_instruction_idx;
  uint32_t cached_live_regs;
} LSLiveIntervalState;

void tcc_ls_initialize(LSLiveIntervalState *ls);
void tcc_ls_deinitialize(LSLiveIntervalState *ls);

void tcc_ls_clear_live_intervals(LSLiveIntervalState *ls);

void tcc_ls_add_live_interval(LSLiveIntervalState *ls, int vreg, int start, int end, int crosses_call, int addrtaken,
                              int reg_type, int lvalue, int precolored_reg);
void tcc_ls_allocate_registers(LSLiveIntervalState *ls, int used_parameters_registers,
                               int used_float_parameters_registers, int spill_base);

/* Reassign stack spill slots densely starting from spill_base.
 * Useful after rewriting intervals (e.g. dropping some spills) so the frame
 * size and remaining spill offsets shrink accordingly.
 */
void tcc_ls_compact_stack_locations(LSLiveIntervalState *ls, int spill_base);

/* Reset scratch register cache - call before codegen starts */
void tcc_ls_reset_scratch_cache(LSLiveIntervalState *ls);

/* Find a free scratch register at the given instruction index.
 * Returns -1 if no register is available.
 * Uses per-instruction caching for efficiency.
 *   ls - the live interval state
 *   instruction_idx - current instruction index
 *   exclude_regs - bitmap of registers to exclude (e.g., already used as scratch)
 *   is_leaf - 1 if this is a leaf function (LR holds return address)
 */
int tcc_ls_find_free_scratch_reg(LSLiveIntervalState *ls, int instruction_idx, uint32_t exclude_regs, int is_leaf);
