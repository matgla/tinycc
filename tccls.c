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

#include "tccls.h"

#include "tcc.h"

#define LS_DBG(fmt, ...) LOG_LS(fmt, ##__VA_ARGS__)
#define STACK_ALLOC_LOG(reason, vreg, loc, size)                                                                       \
  LOG_STACK_ALLOC("%s vreg=0x%x loc=%d size=%d", (reason), (unsigned)(vreg), (int)(loc), (int)(size))

#define LS_LIVE_INTERVAL_INIT_SIZE 64

void tcc_ls_initialize(LSLiveIntervalState *ls)
{
  LS_DBG("Initializing linear scan allocator");
  ls->intervals_size = LS_LIVE_INTERVAL_INIT_SIZE;
  ls->intervals = (LSLiveInterval *)tcc_malloc(sizeof(LSLiveInterval) * ls->intervals_size);
  ls->next_interval_index = 0;

  ls->active_set = (LSLiveInterval **)tcc_malloc(sizeof(LSLiveInterval *) * LS_LIVE_INTERVAL_INIT_SIZE);
  ls->next_active_index = 0;
  ls->dirty_registers = 0;
  ls->dirty_float_registers = 0;
  ls->live_regs_by_instruction = NULL;
  ls->live_regs_by_instruction_size = 0;
  ls->cached_instruction_idx = -1;
  ls->cached_live_regs = 0;
}

void tcc_ls_deinitialize(LSLiveIntervalState *ls)
{
  tcc_free(ls->intervals);
  tcc_free(ls->active_set);

  if (ls->live_regs_by_instruction)
  {
    tcc_free(ls->live_regs_by_instruction);
    ls->live_regs_by_instruction = NULL;
    ls->live_regs_by_instruction_size = 0;
  }
}

void tcc_ls_reset_scratch_cache(LSLiveIntervalState *ls)
{
  ls->cached_instruction_idx = -1;
  ls->cached_live_regs = 0;
}

void tcc_ls_clear_live_intervals(LSLiveIntervalState *ls)
{
  ls->next_interval_index = 0;
  ls->next_active_index = 0;

  if (ls->live_regs_by_instruction)
  {
    tcc_free(ls->live_regs_by_instruction);
    ls->live_regs_by_instruction = NULL;
    ls->live_regs_by_instruction_size = 0;
  }

  tcc_ls_reset_scratch_cache(ls);
}

void tcc_ls_add_live_interval(LSLiveIntervalState *ls, int vreg, int start, int end, int crosses_call, int addrtaken,
                              int reg_type, int lvalue, int precolored_reg)
{
  LSLiveInterval *interval;

  if (ls->next_interval_index >= ls->intervals_size)
  {
    ls->intervals_size <<= 1;
    ls->intervals = (LSLiveInterval *)tcc_realloc(ls->intervals, sizeof(LSLiveInterval) * ls->intervals_size);
    ls->active_set = (LSLiveInterval **)tcc_realloc(ls->active_set, sizeof(LSLiveInterval *) * ls->intervals_size);
  }

  interval = &ls->intervals[ls->next_interval_index];
  interval->vreg = vreg;
  interval->start = start;
  interval->end = end;
  interval->r0 = precolored_reg;
  interval->r1 = -1;
  interval->stack_location = 0;
  interval->crosses_call = crosses_call;
  interval->addrtaken = addrtaken;
  interval->reg_type = reg_type;
  interval->lvalue = lvalue;
  {
    const int is_param = (TCCIR_DECODE_VREG_TYPE(vreg) == TCCIR_VREG_TYPE_PARAM);
    interval->sort_key = ((uint64_t)(!is_param) << 33) | ((uint64_t)(uint32_t)end << 1) | (lvalue ? 0u : 1u);
  }
  ls->next_interval_index++;
}

static int tcc_ls_reg_type_stack_size(int reg_type)
{
  switch (reg_type)
  {
  case LS_REG_TYPE_LLONG:
  case LS_REG_TYPE_DOUBLE:
  case LS_REG_TYPE_DOUBLE_SOFT:
  case LS_REG_TYPE_COMPLEX_FLOAT:
    return 8;
  case LS_REG_TYPE_COMPLEX_DOUBLE:
    return 16;
  default:
    return 4;
  }
}

void tcc_ls_compact_stack_locations(LSLiveIntervalState *ls, int spill_base)
{
  if (!ls)
    return;

  if (spill_base > 0)
    spill_base = 0;

  int loc = spill_base;

  for (int i = 0; i < ls->next_interval_index; ++i)
  {
    LSLiveInterval *it = &ls->intervals[i];
    if (it->stack_location == 0)
      continue;

    const int size = tcc_ls_reg_type_stack_size(it->reg_type);
    loc = (loc - size) & -size;
    if (loc == 0)
      loc = -size;
    it->stack_location = loc;
    STACK_ALLOC_LOG("compact", it->vreg, loc, size);
  }
}

void tcc_ls_recompute_dirty_registers(LSLiveIntervalState *ls)
{
  if (!ls->live_regs_by_instruction || ls->live_regs_by_instruction_size <= 0)
    return;

  uint64_t actually_used = 0;
  for (int i = 0; i < ls->live_regs_by_instruction_size; ++i)
    actually_used |= (uint64_t)ls->live_regs_by_instruction[i];

  uint64_t callee_mask = 0;
  for (int r = 4; r <= 11; ++r)
    callee_mask |= (1ULL << r);

  uint64_t old_dirty = ls->dirty_registers;
  uint64_t non_callee = old_dirty & ~callee_mask;
  uint64_t callee_dirty = old_dirty & callee_mask;
  uint64_t callee_used = actually_used & callee_mask;
  ls->dirty_registers = non_callee | (callee_dirty & callee_used);
}

static uint32_t tcc_ls_compute_live_regs(LSLiveIntervalState *ls, int instruction_idx)
{
  uint32_t live_regs = 0;
  for (int i = 0; i < ls->next_interval_index; ++i)
  {
    LSLiveInterval *interval = &ls->intervals[i];

    if (interval->reg_type != LS_REG_TYPE_INT && interval->reg_type != LS_REG_TYPE_LLONG)
      continue;

    if (interval->start <= (uint32_t)instruction_idx && interval->end >= (uint32_t)instruction_idx)
    {
      if (interval->r0 >= 0 && interval->r0 < 16)
      {
        live_regs |= (1 << interval->r0);
      }
      if (interval->r1 >= 0 && interval->r1 < 16)
      {
        live_regs |= (1 << interval->r1);
      }
    }
  }
  return live_regs;
}

int tcc_ls_find_free_scratch_reg(LSLiveIntervalState *ls, int instruction_idx, uint32_t exclude_regs, int is_leaf)
{
  uint32_t live_regs = exclude_regs;

  LS_DBG("  Finding scratch register at instruction %d (is_leaf=%d)", instruction_idx, is_leaf);
  LS_DBG("    Exclude regs: 0x%x", exclude_regs);

  live_regs |= (1 << 13);

  if (is_leaf)
  {
    live_regs |= (1 << 14);
  }

  live_regs |= (1 << 15);

  if (ls->live_regs_by_instruction && instruction_idx >= 0 && instruction_idx < ls->live_regs_by_instruction_size)
  {
    live_regs |= ls->live_regs_by_instruction[instruction_idx];
    LS_DBG("    Using precomputed liveness: 0x%x", live_regs);
  }
  else
  {
    if (ls->cached_instruction_idx == instruction_idx)
    {
      live_regs |= ls->cached_live_regs;
      LS_DBG("    Using cached liveness: 0x%x", live_regs);
    }
    else
    {
      uint32_t computed = tcc_ls_compute_live_regs(ls, instruction_idx);
      ls->cached_instruction_idx = instruction_idx;
      ls->cached_live_regs = computed;
      live_regs |= computed;
      LS_DBG("    Computed live registers: 0x%x", live_regs);
    }
  }

  {
    const uint32_t avail_low = (~live_regs) & 0xFu;
    if (avail_low)
    {
      int reg = (int)__builtin_ctz(avail_low);
      LS_DBG("    Found scratch register R%d (from R0-R3)", reg);
      return reg;
    }
  }

  if (!(live_regs & (1u << 12)))
  {
    LS_DBG("    Found scratch register R12 (IP)");
    return 12;
  }

  if (!is_leaf && !(live_regs & (1u << 14)))
  {
    LS_DBG("    Found scratch register R14 (LR)");
    return 14;
  }

  LS_DBG("    No scratch register available");
  return PREG_NONE;
}
