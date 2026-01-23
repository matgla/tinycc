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

/* Define TCC_LS_DEBUG to enable printing of linear scan state */
/* #define TCC_LS_DEBUG */

#define LS_LIVE_INTERVAL_INIT_SIZE 64

/* NOTE:
 * The linear-scan allocator needs its own stack slot cursor for spills.
 * Do NOT reuse the global TCC frontend variable `loc` (declared in tcc.h),
 * otherwise spill offsets can become 0 (e.g. when `loc == 4`) and codegen
 * will emit loads/stores at [FP + 0], corrupting the frame (and breaking
 * indirect calls like function-pointer tables).
 */
static int ls_spill_loc;

void tcc_ls_initialize(LSLiveIntervalState *ls)
{
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

  /* Intervals changed; invalidate any precomputed liveness table. */
  if (ls->live_regs_by_instruction)
  {
    tcc_free(ls->live_regs_by_instruction);
    ls->live_regs_by_instruction = NULL;
    ls->live_regs_by_instruction_size = 0;
  }

  tcc_ls_reset_scratch_cache(ls);
}

static void tcc_ls_build_live_regs_by_instruction(LSLiveIntervalState *ls)
{
  if (!ls)
    return;

  if (ls->live_regs_by_instruction)
  {
    tcc_free(ls->live_regs_by_instruction);
    ls->live_regs_by_instruction = NULL;
    ls->live_regs_by_instruction_size = 0;
  }

  uint32_t max_end = 0;
  int has_any = 0;
  for (int i = 0; i < ls->next_interval_index; ++i)
  {
    const LSLiveInterval *interval = &ls->intervals[i];

    /* Only track integer register occupancy; skip spilled/stack-only intervals. */
    if (interval->reg_type != LS_REG_TYPE_INT && interval->reg_type != LS_REG_TYPE_LLONG &&
        interval->reg_type != LS_REG_TYPE_DOUBLE_SOFT)
      continue;
    if (interval->addrtaken || interval->stack_location != 0)
      continue;
    if (interval->r0 < 0)
      continue;

    has_any = 1;
    if (interval->end > max_end)
      max_end = interval->end;
  }

  if (!has_any)
    return;

  const int size = (int)max_end + 1;
  uint32_t *start_masks = (uint32_t *)tcc_mallocz(sizeof(uint32_t) * (size_t)size);
  uint32_t *end_masks = (uint32_t *)tcc_mallocz(sizeof(uint32_t) * (size_t)size);
  ls->live_regs_by_instruction = (uint32_t *)tcc_malloc(sizeof(uint32_t) * (size_t)size);
  ls->live_regs_by_instruction_size = size;

  for (int i = 0; i < ls->next_interval_index; ++i)
  {
    const LSLiveInterval *interval = &ls->intervals[i];

    if (interval->reg_type != LS_REG_TYPE_INT && interval->reg_type != LS_REG_TYPE_LLONG &&
        interval->reg_type != LS_REG_TYPE_DOUBLE_SOFT)
      continue;
    if (interval->addrtaken || interval->stack_location != 0)
      continue;
    if (interval->r0 < 0)
      continue;
    if ((int)interval->start < 0 || (int)interval->end < 0)
      continue;
    if ((int)interval->start >= size)
      continue;

    uint32_t mask = 0;
    if (interval->r0 >= 0 && interval->r0 < 16)
      mask |= (1u << interval->r0);
    if (interval->r1 >= 0 && interval->r1 < 16)
      mask |= (1u << interval->r1);

    /* Ignore anything outside the 0..15 integer register window. */
    if (!mask)
      continue;

    start_masks[interval->start] |= mask;
    if ((int)interval->end < size)
      end_masks[interval->end] |= mask;
    else
      end_masks[size - 1] |= mask;
  }

  uint32_t live = 0;
  for (int idx = 0; idx < size; ++idx)
  {
    live |= start_masks[idx];
    ls->live_regs_by_instruction[idx] = live;
    /* Inclusive end: remove after recording this instruction's occupancy. */
    live &= ~end_masks[idx];
  }

  tcc_free(start_masks);
  tcc_free(end_masks);
}

void tcc_ls_add_live_interval(LSLiveIntervalState *ls, int vreg, int start, int end, int crosses_call, int addrtaken,
                              int reg_type, int lvalue, int precolored_reg)
{
  LSLiveInterval *interval;

  if (ls->next_interval_index >= ls->intervals_size)
  {
    ls->intervals_size <<= 1;
    ls->intervals = (LSLiveInterval *)tcc_realloc(ls->intervals, sizeof(LSLiveInterval) * ls->intervals_size);
    /* active_set must be able to hold as many entries as intervals */
    ls->active_set = (LSLiveInterval **)tcc_realloc(ls->active_set, sizeof(LSLiveInterval *) * ls->intervals_size);
  }

  interval = &ls->intervals[ls->next_interval_index];
  interval->vreg = vreg;
  interval->start = start;
  interval->end = end;
  interval->r0 = precolored_reg; /* -1 means no preference, >= 0 is ABI register hint */
  interval->r1 = -1;
  interval->stack_location = 0;
  interval->crosses_call = crosses_call;
  interval->addrtaken = addrtaken;
  interval->reg_type = reg_type;
  interval->lvalue = lvalue;
  ls->next_interval_index++;
}

static int sort_startpoints(const void *a, const void *b)
{
  LSLiveInterval *ia = (LSLiveInterval *)a;
  LSLiveInterval *ib = (LSLiveInterval *)b;
  if (TCCIR_DECODE_VREG_TYPE(ia->vreg) == TCCIR_VREG_TYPE_PARAM &&
      TCCIR_DECODE_VREG_TYPE(ib->vreg) != TCCIR_VREG_TYPE_PARAM)
  {
    return -1;
  }
  else if (TCCIR_DECODE_VREG_TYPE(ia->vreg) != TCCIR_VREG_TYPE_PARAM &&
           TCCIR_DECODE_VREG_TYPE(ib->vreg) == TCCIR_VREG_TYPE_PARAM)
  {
    return 1;
  }

  if (ia->start == 0 && ib->start == 0)
  {
    if (TCCIR_DECODE_VREG_TYPE(ia->vreg) == TCCIR_VREG_TYPE_PARAM)
    {
      return -1;
    }
  }
  if (ia->start < ib->start)
    return -1;
  else if (ia->start > ib->start)
    return 1;

  if (ia->start == ib->start && ia->end == ib->end)
  {
    if (TCCIR_DECODE_VREG_TYPE(ia->vreg) == TCCIR_VREG_TYPE_PARAM)
    {
      return -1;
    }
    else if (TCCIR_DECODE_VREG_TYPE(ib->vreg) == TCCIR_VREG_TYPE_PARAM)
    {
      return 1;
    }
  }
  return 0;
}

static int sort_endpoints(const void *a, const void *b)
{
  LSLiveInterval *ia = *(LSLiveInterval **)a;
  LSLiveInterval *ib = *(LSLiveInterval **)b;
  /* Keep PARAMs first to ensure correct parameter register handling */
  if (TCCIR_DECODE_VREG_TYPE(ia->vreg) == TCCIR_VREG_TYPE_PARAM &&
      TCCIR_DECODE_VREG_TYPE(ib->vreg) != TCCIR_VREG_TYPE_PARAM)
  {
    return -1;
  }
  else if (TCCIR_DECODE_VREG_TYPE(ia->vreg) != TCCIR_VREG_TYPE_PARAM &&
           TCCIR_DECODE_VREG_TYPE(ib->vreg) == TCCIR_VREG_TYPE_PARAM)
  {
    return 1;
  }

  if (ia->end < ib->end)
    return -1;
  else if (ia->end > ib->end)
    return 1;
  else if (ia->end == ib->end)
  {
    if (ia->lvalue && !ib->lvalue)
    {
      return -1;
    }
    else if (!ia->lvalue && ib->lvalue)
    {
      return 1;
    }
  }
  return 0;
}

void tcc_ls_release_register(LSLiveIntervalState *ls, int reg)
{
  if (reg < 0)
    return;
  if (tcc_state->registers_map_for_allocator & ((uint64_t)1 << reg))
  {
    ls->registers_map |= ((uint64_t)1 << reg);
    return;
  }
}

void tcc_ls_release_float_register(LSLiveIntervalState *ls, int reg)
{
  if (reg < 0)
    return;
  if (tcc_state->float_registers_map_for_allocator & ((uint64_t)1 << reg))
  {
    ls->float_registers_map |= ((uint64_t)1 << reg);
    return;
  }
}

int tcc_ls_assign_register(LSLiveIntervalState *ls, int reg)
{
  if (tcc_state->registers_map_for_allocator & ((uint64_t)1 << reg))
  {
    if (ls->registers_map & ((uint64_t)1 << reg))
    {
      ls->registers_map &= ~((uint64_t)1 << reg);
      ls->dirty_registers |= ((uint64_t)1 << reg);
      return reg;
    }
  }
  return -1;
}

int tcc_ls_assign_float_register(LSLiveIntervalState *ls, int reg)
{
  if (tcc_state->float_registers_map_for_allocator & ((uint64_t)1 << reg))
  {
    if (ls->float_registers_map & ((uint64_t)1 << reg))
    {
      ls->float_registers_map &= ~((uint64_t)1 << reg);
      ls->dirty_float_registers |= ((uint64_t)1 << reg);
      return reg;
    }
  }
  return -1;
}

int tcc_ls_assign_any_register(LSLiveIntervalState *ls)
{
  for (int reg = 0; reg < tcc_state->registers_for_allocator; ++reg)
  {
    int assigned_reg = tcc_ls_assign_register(ls, reg);
    if (assigned_reg != -1)
    {
      return assigned_reg;
    }
  }
  return -1;
}

int tcc_ls_assign_any_float_register(LSLiveIntervalState *ls)
{
  for (int reg = 0; reg < tcc_state->float_registers_for_allocator; ++reg)
  {
    int assigned_reg = tcc_ls_assign_float_register(ls, reg);
    if (assigned_reg != -1)
    {
      /* Return VFP register with marker so it's distinguishable from int regs
       */
      return LS_VFP_REG_BASE + reg;
    }
  }
  return -1;
}

/* Assign a callee-saved register (R4-R12) for intervals that cross calls */
int tcc_ls_assign_callee_saved_register(LSLiveIntervalState *ls)
{
  /* Callee-saved registers start at R4 */
  for (int reg = 4; reg < tcc_state->registers_for_allocator; ++reg)
  {
    int assigned_reg = tcc_ls_assign_register(ls, reg);
    if (assigned_reg != -1)
    {
      return assigned_reg;
    }
  }
  return -1;
}

/* Assign a pair of consecutive registers for 64-bit values (long long, double
 * soft-float). Returns first register of pair, or -1 if no pair available.
 * The pair is (reg, reg+1), so we need to find an even register where both
 * are free. ARM EABI requires doubleword values in R0:R1 or R2:R3 for
 * argument passing, so we try even-aligned pairs first (R0:R1, R2:R3, R4:R5,
 * etc.) */
int tcc_ls_assign_register_pair(LSLiveIntervalState *ls, int *r0_out, int *r1_out)
{
  /* Try even-aligned pairs first for best EABI compliance */
  for (int reg = 0; reg < tcc_state->registers_for_allocator - 1; reg += 2)
  {
    /* Check if both registers in pair are available */
    if ((tcc_state->registers_map_for_allocator & ((uint64_t)1 << reg)) &&
        (tcc_state->registers_map_for_allocator & ((uint64_t)1 << (reg + 1))) &&
        (ls->registers_map & ((uint64_t)1 << reg)) && (ls->registers_map & ((uint64_t)1 << (reg + 1))))
    {
      /* Skip any pair touching SP (R13) or PC (R15). */
      if (reg == 13 || reg == 15 || (reg + 1) == 13 || (reg + 1) == 15)
        continue;
      /* Allocate both */
      ls->registers_map &= ~((uint64_t)1 << reg);
      ls->registers_map &= ~((uint64_t)1 << (reg + 1));
      ls->dirty_registers |= ((uint64_t)1 << reg);
      ls->dirty_registers |= ((uint64_t)1 << (reg + 1));
      *r0_out = reg;
      *r1_out = reg + 1;
      return reg;
    }
  }
  /* Fallback: try any two available registers (not necessarily consecutive)
   */
  int first_reg = -1;
  for (int reg = 0; reg < tcc_state->registers_for_allocator; ++reg)
  {
    if (reg == 13 || reg == 15)
      continue; /* Skip SP and PC */
    if ((tcc_state->registers_map_for_allocator & ((uint64_t)1 << reg)) && (ls->registers_map & ((uint64_t)1 << reg)))
    {
      if (first_reg == -1)
      {
        first_reg = reg;
      }
      else
      {
        /* Found two registers */
        ls->registers_map &= ~((uint64_t)1 << first_reg);
        ls->registers_map &= ~((uint64_t)1 << reg);
        ls->dirty_registers |= ((uint64_t)1 << first_reg);
        ls->dirty_registers |= ((uint64_t)1 << reg);
        *r0_out = first_reg;
        *r1_out = reg;
        return first_reg;
      }
    }
  }
  return -1;
}

/* Assign callee-saved register pair for intervals crossing calls */
int tcc_ls_assign_callee_saved_register_pair(LSLiveIntervalState *ls, int *r0_out, int *r1_out)
{
  /* Callee-saved registers start at R4, try even-aligned pairs */
  for (int reg = 4; reg < tcc_state->registers_for_allocator - 1; reg += 2)
  {
    if ((tcc_state->registers_map_for_allocator & ((uint64_t)1 << reg)) &&
        (tcc_state->registers_map_for_allocator & ((uint64_t)1 << (reg + 1))) &&
        (ls->registers_map & ((uint64_t)1 << reg)) && (ls->registers_map & ((uint64_t)1 << (reg + 1))))
    {
      /* Skip any pair touching SP (R13) or PC (R15). */
      if (reg == 13 || reg == 15 || (reg + 1) == 13 || (reg + 1) == 15)
        continue;
      ls->registers_map &= ~((uint64_t)1 << reg);
      ls->registers_map &= ~((uint64_t)1 << (reg + 1));
      ls->dirty_registers |= ((uint64_t)1 << reg);
      ls->dirty_registers |= ((uint64_t)1 << (reg + 1));
      *r0_out = reg;
      *r1_out = reg + 1;
      return reg;
    }
  }
  return -1;
}

/* For VFP single precision, S16-S31 are callee-saved on ARM EABI */
int tcc_ls_assign_callee_saved_float_register(LSLiveIntervalState *ls)
{
  /* S16-S31 are callee-saved, but for fpv5-sp-d16 we only have S0-S15 */
  /* So all float registers are caller-saved in our case - just assign any */
  return tcc_ls_assign_any_float_register(ls);
}

void tcc_ls_expire_old_intervals(LSLiveIntervalState *ls, int current_index)
{
  int removed_intervals = 0;
  LSLiveInterval *current = &ls->intervals[current_index];
  static LSLiveInterval dirty = {
      .r0 = 0,
      .r1 = 0,
      .vreg = 0,
      .stack_location = 0,
      .start = 0,
      .end = ~0,
      .reg_type = LS_REG_TYPE_INT,
  };
  /* Iterate through ALL active intervals - cannot break early because
   * the active set is sorted with PARAMs first (for correct parameter
   * register assignment), which means a long-lived PARAM might come
   * before a short-lived TMP that should be expired. */
  for (int i = 0; i < ls->next_active_index; ++i)
  {
    if (ls->active_set[i]->end >= current->start)
    {
      continue; /* Still active, skip */
    }
    /* Release registers based on type */
    if (ls->active_set[i]->reg_type == LS_REG_TYPE_FLOAT)
    {
      tcc_ls_release_float_register(ls, ls->active_set[i]->r0);
    }
    else if (ls->active_set[i]->reg_type == LS_REG_TYPE_DOUBLE)
    {
      /* VFP double - release both S registers */
      tcc_ls_release_float_register(ls, ls->active_set[i]->r0);
      if (ls->active_set[i]->r1 >= 0)
      {
        tcc_ls_release_float_register(ls, ls->active_set[i]->r1);
      }
    }
    else
    {
      /* Integer types (INT, LLONG, DOUBLE_SOFT) */
      tcc_ls_release_register(ls, ls->active_set[i]->r0);
      /* Release second register for 64-bit types */
      if (ls->active_set[i]->r1 >= 0 &&
          (ls->active_set[i]->reg_type == LS_REG_TYPE_LLONG || ls->active_set[i]->reg_type == LS_REG_TYPE_DOUBLE_SOFT))
      {
        tcc_ls_release_register(ls, ls->active_set[i]->r1);
      }
    }
    ls->active_set[i] = &dirty; // mark as removed
    removed_intervals++;        // count removed intervals
  }
  qsort(ls->active_set, ls->next_active_index, sizeof(LSLiveInterval *), sort_endpoints);
  ls->next_active_index -= removed_intervals;
}

void tcc_ls_mark_register_as_used(LSLiveIntervalState *ls, int reg)
{
  if (tcc_state->registers_map_for_allocator & ((uint64_t)1 << reg))
  {
    ls->registers_map &= ~((uint64_t)1 << reg);
    ls->dirty_registers |= ((uint64_t)1 << reg);
    return;
  }
  fprintf(stderr, "Error: trying to mark unallocatable register %d as used\n", reg);
  exit(1);
}

void tcc_ls_mark_float_register_as_used(LSLiveIntervalState *ls, int reg)
{
  if (tcc_state->float_registers_map_for_allocator & ((uint64_t)1 << reg))
  {
    ls->float_registers_map &= ~((uint64_t)1 << reg);
    ls->dirty_float_registers |= ((uint64_t)1 << reg);
    return;
  }
  fprintf(stderr, "Error: trying to mark unallocatable float register %d as used\n", reg);
  exit(1);
}

int tcc_ls_next_stack_location_sized(int size)
{
  /* Align to size and allocate */
  ls_spill_loc = (ls_spill_loc - size) & -size;
  /* Offset 0 is not a valid spill slot: codegen treats FP+0 as part of the
   * saved-register area (e.g. saved R4 at [FP]). If we ever return 0 here,
   * spilled values will alias the frame header and break indirect calls.
   */
  if (ls_spill_loc == 0)
    ls_spill_loc = -size;
  return ls_spill_loc;
}

int tcc_ls_next_stack_location()
{
  return tcc_ls_next_stack_location_sized(4);
}

static int tcc_ls_reg_type_stack_size(int reg_type)
{
  switch (reg_type)
  {
  case LS_REG_TYPE_LLONG:
  case LS_REG_TYPE_DOUBLE:
  case LS_REG_TYPE_DOUBLE_SOFT:
    return 8;
  default:
    return 4;
  }
}

void tcc_ls_compact_stack_locations(LSLiveIntervalState *ls, int spill_base)
{
  if (!ls)
    return;

  /* Mirror allocator behavior: spill_base is FP-relative (typically <= 0). */
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
  }
}

/* Spill interval to stack. For doubles, allocates 8 bytes. */
void tcc_ls_spill_interval_sized(LSLiveIntervalState *ls, int interval_index, int size)
{
  LSLiveInterval *interval = &ls->intervals[interval_index];
  /* If no active intervals, just spill to stack */
  if (ls->next_active_index == 0)
  {
    interval->stack_location = tcc_ls_next_stack_location_sized(size);
    return;
  }
  LSLiveInterval *spill = ls->active_set[ls->next_active_index - 1];
  /* Only steal register from spill if:
   * 1. spill lives longer than interval (worth spilling)
   * 2. spill actually has a valid register (r0 >= 0 and not already spilled)
   * 3. For 64-bit intervals (size==8), spill must also have a valid r1 (register pair) */
  int spill_has_pair = (spill->r1 >= 0);
  int needs_pair = (size == 8);
  if (spill->end > interval->end && spill->r0 >= 0 && spill->stack_location == 0 && (!needs_pair || spill_has_pair))
  {
    interval->r0 = spill->r0;
    interval->r1 = spill->r1;
    spill->r0 = -1; /* Clear register from spilled interval */
    spill->r1 = -1;
    spill->stack_location = tcc_ls_next_stack_location_sized(tcc_ls_reg_type_stack_size(spill->reg_type));
    ls->active_set[ls->next_active_index - 1] = interval;
    qsort(ls->active_set, ls->next_active_index, sizeof(LSLiveInterval *), sort_endpoints);
  }
  else
  {
    interval->stack_location = tcc_ls_next_stack_location_sized(size);
  }
}

void tcc_ls_spill_interval(LSLiveIntervalState *ls, int interval_index)
{
  tcc_ls_spill_interval_sized(ls, interval_index, 4);
}

void tcc_ls_allocate_registers(LSLiveIntervalState *ls, int used_parameters_registers,
                               int used_float_parameters_registers, int spill_base)
{
  /* Reset spill cursor for this allocation run.
   * Start below the frontend-allocated locals so spill slots do not overlap
   * local variables (which would corrupt things like function-pointer tables
   * and computed-goto targets).
   */
  /* Spill base should be FP-relative and typically negative or 0.
   * If a positive value sneaks in, clamp to 0 so the first spill goes to -4.
   */
  if (spill_base > 0)
    spill_base = 0;
  ls_spill_loc = spill_base;

  // make all registers available at start
  ls->dirty_registers = 0;
  ls->dirty_float_registers = 0;
  ls->registers_map = tcc_state->registers_map_for_allocator;
  ls->float_registers_map = tcc_state->float_registers_map_for_allocator;

  /* R11 is available for normal allocation, but reserved during call argument processing.
   * R12 (IP) is the standard inter-procedure scratch register. */
  /* Note: We used to reserve R0-R3 here, but with parameter pre-coloring, the
   * PAR:n intervals get assigned R0-R3 directly. The intervals themselves will
   * prevent those registers from being reused by other intervals during their
   * live range. So we no longer pre-reserve parameter registers.
   *
   * The parameter pre-coloring (r0 = 0..3 for PAR:0..3) ensures that parameters
   * are allocated to their ABI-mandated registers, and the linear-scan algorithm
   * will prevent conflicts with other intervals.
   */
  for (int i = 0; i < used_float_parameters_registers; ++i)
  {
    tcc_ls_mark_float_register_as_used(ls, i);
  }
  qsort(ls->intervals, ls->next_interval_index, sizeof(LSLiveInterval), sort_startpoints);
  for (int i = 0; i < ls->next_interval_index; ++i)
  {
    tcc_ls_expire_old_intervals(ls, i);

    /* Variables whose address is taken must be on the stack */
    if (ls->intervals[i].addrtaken)
    {
      ls->intervals[i].stack_location =
          tcc_ls_next_stack_location_sized(tcc_ls_reg_type_stack_size(ls->intervals[i].reg_type));
      ls->active_set[ls->next_active_index++] = &ls->intervals[i];
      qsort(ls->active_set, ls->next_active_index, sizeof(LSLiveInterval *), sort_endpoints);
      continue;
    }

    /* Handle float/double registers separately */
    if (ls->intervals[i].reg_type == LS_REG_TYPE_FLOAT || ls->intervals[i].reg_type == LS_REG_TYPE_DOUBLE)
    {
      /* For VFP doubles, always spill to stack for now since the register
       * allocator doesn't properly handle D-register pairs (S0+S1, S2+S3,
       * etc.) and conversion operations use D0 as scratch */
      if (ls->intervals[i].reg_type == LS_REG_TYPE_DOUBLE)
      {
        tcc_ls_spill_interval_sized(ls, i, 8); /* doubles are 8 bytes */
        ls->active_set[ls->next_active_index++] = &ls->intervals[i];
        qsort(ls->active_set, ls->next_active_index, sizeof(LSLiveInterval *), sort_endpoints);
        continue;
      }
      if (ls->intervals[i].r0 == -1)
      {
        /* For floats crossing calls, all S0-S15 are caller-saved anyway */
        ls->intervals[i].r0 = tcc_ls_assign_any_float_register(ls);
      }
      else
      {
        /* r0 already contains the VFP register index - extract it, assign,
         * and re-add marker */
        int vfp_idx = LS_IS_VFP_REG(ls->intervals[i].r0) ? LS_VFP_REG_NUM(ls->intervals[i].r0) : ls->intervals[i].r0;
        int assigned = tcc_ls_assign_float_register(ls, vfp_idx);
        ls->intervals[i].r0 = (assigned >= 0) ? LS_VFP_REG_BASE + assigned : -1;
      }
      if (ls->intervals[i].r0 == -1)
      {
        /* Spill to stack */
        tcc_ls_spill_interval(ls, i);
      }
    }
    else if (ls->intervals[i].reg_type == LS_REG_TYPE_LLONG || ls->intervals[i].reg_type == LS_REG_TYPE_DOUBLE_SOFT)
    {
      /* 64-bit integer type - needs two integer registers */
      int r0 = -1, r1 = -1;
      if (ls->intervals[i].r0 == -1)
      {
        /* No pre-assigned registers - allocate a pair */
        if (ls->intervals[i].crosses_call)
        {
          tcc_ls_assign_callee_saved_register_pair(ls, &r0, &r1);
        }
        else
        {
          tcc_ls_assign_register_pair(ls, &r0, &r1);
        }
        ls->intervals[i].r0 = r0;
        ls->intervals[i].r1 = r1;
      }
      else
      {
        /* Pre-assigned r0 - try to get it and find r1 */
        int pre_r0 = ls->intervals[i].r0;
        ls->intervals[i].r0 = tcc_ls_assign_register(ls, pre_r0);
        if (ls->intervals[i].r0 >= 0)
        {
          /* Got r0, now find r1 (prefer r0+1 if available) */
          int preferred_r1 = ls->intervals[i].r0 + 1;
          if (preferred_r1 != 13 && preferred_r1 != 15)
          { /* Not SP or PC */
            ls->intervals[i].r1 = tcc_ls_assign_register(ls, preferred_r1);
          }
          if (ls->intervals[i].r1 < 0)
          {
            /* Try any available register */
            ls->intervals[i].r1 = tcc_ls_assign_any_register(ls);
          }
        }
        else
        {
          /* Pre-assigned register unavailable - fall back to allocating a fresh pair */
          if (ls->intervals[i].crosses_call)
          {
            tcc_ls_assign_callee_saved_register_pair(ls, &r0, &r1);
          }
          else
          {
            tcc_ls_assign_register_pair(ls, &r0, &r1);
          }
          ls->intervals[i].r0 = r0;
          ls->intervals[i].r1 = r1;
        }
      }

      if (ls->intervals[i].r0 == ls->intervals[i].r1)
      {
        /* Invalid register pair: force spill rather than clobbering. */
        if (ls->intervals[i].r0 >= 0)
          tcc_ls_release_register(ls, ls->intervals[i].r0);
        ls->intervals[i].r0 = -1;
        ls->intervals[i].r1 = -1;
      }

      if (ls->intervals[i].r0 == -1 || ls->intervals[i].r1 == -1)
      {
        /* Couldn't allocate pair - spill to stack */
        /* Release any partially allocated register */
        if (ls->intervals[i].r0 >= 0)
        {
          tcc_ls_release_register(ls, ls->intervals[i].r0);
          ls->intervals[i].r0 = -1;
        }
        if (ls->intervals[i].r1 >= 0)
        {
          tcc_ls_release_register(ls, ls->intervals[i].r1);
          ls->intervals[i].r1 = -1;
        }
        tcc_ls_spill_interval_sized(ls, i, 8); /* 64-bit = 8 bytes */
      }
    }
    else
    {
      /* Integer register allocation */
      if (ls->intervals[i].r0 == -1)
      {
        /* If interval crosses a function call, use callee-saved registers
         * only
         */
        if (ls->intervals[i].crosses_call)
        {
          ls->intervals[i].r0 = tcc_ls_assign_callee_saved_register(ls);
        }
        else
        {
          ls->intervals[i].r0 = tcc_ls_assign_any_register(ls);
        }
      }
      else
      {
        ls->intervals[i].r0 = tcc_ls_assign_register(ls, ls->intervals[i].r0);
      }

      if (ls->intervals[i].r0 == -1)
      {
        // add spilling
        tcc_ls_spill_interval(ls, i);
      }
    }
    ls->active_set[ls->next_active_index++] = &ls->intervals[i];
    qsort(ls->active_set, ls->next_active_index, sizeof(LSLiveInterval *), sort_endpoints);
  }

#ifdef TCC_LS_DEBUG
  tcc_ls_print_intervals(ls);
#endif

  /* Build O(1) scratch-reg liveness table for codegen. */
  tcc_ls_build_live_regs_by_instruction(ls);
}

#ifdef TCC_LS_DEBUG
static void tcc_ls_print_intervals(LSLiveIntervalState *ls)
{
  for (int i = 0; i < ls->next_interval_index; ++i)
  {
    printf("Interval %d (%d,%d), ", i, ls->intervals[i].start, ls->intervals[i].end);
    tcc_ir_print_vreg(ls->intervals[i].vreg);
    const char *type_str;
    switch (ls->intervals[i].reg_type)
    {
    case LS_REG_TYPE_INT:
      type_str = "int";
      break;
    case LS_REG_TYPE_FLOAT:
      type_str = "float";
      break;
    case LS_REG_TYPE_DOUBLE:
      type_str = "double(vfp)";
      break;
    case LS_REG_TYPE_LLONG:
      type_str = "llong";
      break;
    case LS_REG_TYPE_DOUBLE_SOFT:
      type_str = "double(soft)";
      break;
    default:
      type_str = "unknown";
      break;
    }
    printf(" [%s] --> ", type_str);
    if (ls->intervals[i].stack_location != 0 || ls->intervals[i].addrtaken)
    {
      printf("spilled to stack at %d\n", (int)ls->intervals[i].stack_location);
    }
    else
    {
      if (ls->intervals[i].reg_type == LS_REG_TYPE_FLOAT || ls->intervals[i].reg_type == LS_REG_TYPE_DOUBLE)
      {
        printf("S%d", LS_VFP_REG_NUM(ls->intervals[i].r0));
      }
      else
      {
        printf("R%d", ls->intervals[i].r0);
      }
      if (ls->intervals[i].r1 >= 0)
      {
        printf(":R%d", ls->intervals[i].r1);
      }
      printf("\n");
    }
  }
}
#endif

/* Compute live registers bitmap for a given instruction index */
static uint32_t tcc_ls_compute_live_regs(LSLiveIntervalState *ls, int instruction_idx)
{
  uint32_t live_regs = 0;
  for (int i = 0; i < ls->next_interval_index; ++i)
  {
    LSLiveInterval *interval = &ls->intervals[i];

    /* Skip non-integer registers */
    if (interval->reg_type != LS_REG_TYPE_INT && interval->reg_type != LS_REG_TYPE_LLONG)
      continue;

    /* Check if interval is live at this instruction */
    if (interval->start <= instruction_idx && interval->end >= instruction_idx)
    {
      /* This vreg is live - mark its register(s) as unavailable */
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

/* Find a free scratch register at the given instruction index.
 * Returns -1 if no register is available.
 * Uses per-instruction caching for efficiency.
 *
 * Parameters:
 *   ls - the live interval state
 *   instruction_idx - current instruction index
 *   exclude_regs - bitmap of registers to exclude (e.g., already used as scratch)
 *   is_leaf - 1 if this is a leaf function (LR holds return address)
 */
int tcc_ls_find_free_scratch_reg(LSLiveIntervalState *ls, int instruction_idx, uint32_t exclude_regs, int is_leaf)
{
  uint32_t live_regs = exclude_regs;

  /* Always exclude SP (R13) */
  live_regs |= (1 << 13);

  /* Exclude LR (R14) in leaf functions - it holds return address */
  if (is_leaf)
  {
    live_regs |= (1 << 14);
  }

  /* Exclude PC (R15) */
  live_regs |= (1 << 15);

  /* Prefer precomputed liveness when available (fast path). */
  if (ls->live_regs_by_instruction && instruction_idx >= 0 && instruction_idx < ls->live_regs_by_instruction_size)
  {
    live_regs |= ls->live_regs_by_instruction[instruction_idx];
  }
  else
  {
    /* Use cached live registers if same instruction, otherwise compute and cache */
    if (ls->cached_instruction_idx == instruction_idx)
    {
      live_regs |= ls->cached_live_regs;
    }
    else
    {
      uint32_t computed = tcc_ls_compute_live_regs(ls, instruction_idx);
      ls->cached_instruction_idx = instruction_idx;
      ls->cached_live_regs = computed;
      live_regs |= computed;
    }
  }

  /* Prefer caller-saved registers only.
   * Scratch allocation happens after the function prolog has been emitted.
   * Returning a callee-saved register (R4-R11) here can violate the ABI unless
   * the prolog already saved it.
   */
  /* First try R0-R3 (caller-saved, often free for scratch) */
  {
    const uint32_t avail_low = (~live_regs) & 0xFu;
    if (avail_low)
      return (int)__builtin_ctz(avail_low);
  }

  /* Then try R12 (IP - inter-procedure scratch) */
  if (!(live_regs & (1u << 12)))
    return 12;

  /* Try R11 - reserved for call argument processing but available as scratch otherwise */
  if (!(live_regs & (1u << 11)))
    return 11;

  /* Finally try LR if not a leaf function */
  if (!is_leaf && !(live_regs & (1u << 14)))
    return 14;

  /* No register available */
  return PREG_NONE;
}