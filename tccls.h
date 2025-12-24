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

typedef struct LSLiveInterval {
  int16_t r0;              // physical register assigned
  int16_t r1;              // second physical register assigned (for long long)
  uint32_t vreg;           // virtual register number
  uint32_t stack_location; // stack location if spilled
  uint32_t start;          // start instruction index
  uint32_t end;            // end instruction index
  uint8_t crosses_call;    // 1 if interval spans a function call
  uint8_t addrtaken; // 1 if variable's address is taken (must be on stack)
} LSLiveInterval;

typedef struct LSLiveIntervalState {
  LSLiveInterval *intervals;
  int intervals_size;
  int next_interval_index;
  LSLiveInterval **active_set;
  int next_active_index;
  uint64_t registers_map;
  uint64_t dirty_registers;
} LSLiveIntervalState;

void tcc_ls_initialize(LSLiveIntervalState *ls);
void tcc_ls_deinitialize(LSLiveIntervalState *ls);

void tcc_ls_clear_live_intervals(LSLiveIntervalState *ls);

void tcc_ls_add_live_interval(LSLiveIntervalState *ls, int vreg, int start,
                              int end, int crosses_call, int addrtaken);
void tcc_ls_allocate_registers(LSLiveIntervalState *ls,
                               int used_parameters_registers);
