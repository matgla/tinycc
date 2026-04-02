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

/* Legacy logging aliases — see log.h for scope switches:
 *   TCC_LOG_LS=1          linear scan allocator
 *   TCC_LOG_STACK_ALLOC=1 stack frame allocation */
#define LS_DBG(fmt, ...) LOG_LS(fmt, ##__VA_ARGS__)
#define LS_DBG_INDENT(indent, fmt, ...) LOG_LS_INDENT(indent, fmt, ##__VA_ARGS__)
#define STACK_ALLOC_LOG(reason, vreg, loc, size)                                                                       \
  LOG_STACK_ALLOC("%s vreg=0x%x loc=%d size=%d", (reason), (unsigned)(vreg), (int)(loc), (int)(size))

#define LS_LIVE_INTERVAL_INIT_SIZE 64

/* NOTE:
 * The linear-scan allocator needs its own stack slot cursor for spills.
 * Do NOT reuse the global TCC frontend variable `loc` (declared in tcc.h),
 * otherwise spill offsets can become 0 (e.g. when `loc == 4`) and codegen
 * will emit loads/stores at [FP + 0], corrupting the frame (and breaking
 * indirect calls like function-pointer tables).
 */
static int ls_spill_loc;

static uint16_t *ls_use_counts;
static int ls_use_count_size;

void tcc_ls_set_use_counts(uint16_t *counts, int count)
{
  ls_use_counts = counts;
  ls_use_count_size = count;
}

static uint16_t ls_get_use_count(LSLiveIntervalState *ls, LSLiveInterval *interval)
{
  if (!ls_use_counts || ls_use_count_size <= 0)
    return 1;
  int idx = (int)(interval - ls->intervals);
  if (idx < 0 || idx >= ls_use_count_size)
    return 1;
  return ls_use_counts[idx] ? ls_use_counts[idx] : 1;
}

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

  /* Intervals changed; invalidate any precomputed liveness table. */
  if (ls->live_regs_by_instruction)
  {
    tcc_free(ls->live_regs_by_instruction);
    ls->live_regs_by_instruction = NULL;
    ls->live_regs_by_instruction_size = 0;
  }

  tcc_ls_reset_scratch_cache(ls);

  if (ls_use_counts) { tcc_free(ls_use_counts); ls_use_counts = NULL; }
  ls_use_count_size = 0;
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
        interval->reg_type != LS_REG_TYPE_DOUBLE_SOFT && interval->reg_type != LS_REG_TYPE_COMPLEX_FLOAT)
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
        interval->reg_type != LS_REG_TYPE_DOUBLE_SOFT && interval->reg_type != LS_REG_TYPE_COMPLEX_FLOAT)
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
#ifdef TCC_LS_DEBUG
  const char *type_str;
  switch (reg_type)
  {
  case LS_REG_TYPE_INT:
    type_str = "INT";
    break;
  case LS_REG_TYPE_FLOAT:
    type_str = "FLOAT";
    break;
  case LS_REG_TYPE_DOUBLE:
    type_str = "DOUBLE";
    break;
  case LS_REG_TYPE_LLONG:
    type_str = "LLONG";
    break;
  case LS_REG_TYPE_DOUBLE_SOFT:
    type_str = "DOUBLE_SOFT";
    break;
  case LS_REG_TYPE_COMPLEX_FLOAT:
    type_str = "COMPLEX_FLOAT";
    break;
  case LS_REG_TYPE_COMPLEX_DOUBLE:
    type_str = "COMPLEX_DOUBLE";
    break;
  default:
    type_str = "UNKNOWN";
    break;
  }
  LS_DBG("Adding interval: vreg=%u range=[%d,%d] type=%s crosses_call=%d addrtaken=%d precolored=%d lvalue=%d", vreg,
         start, end, type_str, crosses_call, addrtaken, precolored_reg, lvalue);
#endif

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
  {
    const int is_param = (TCCIR_DECODE_VREG_TYPE(vreg) == TCCIR_VREG_TYPE_PARAM);
    interval->sort_key = ((uint64_t)(!is_param) << 33) | ((uint64_t)(uint32_t)end << 1) | (lvalue ? 0u : 1u);
  }
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

static inline int active_interval_compare(LSLiveInterval *ia, LSLiveInterval *ib)
{
  if (ia->sort_key < ib->sort_key)
    return -1;
  if (ia->sort_key > ib->sort_key)
    return 1;
  return 0;
}

static void tcc_ls_active_set_reposition_at(LSLiveIntervalState *ls, int pos)
{
  while (pos > 0 && active_interval_compare(ls->active_set[pos], ls->active_set[pos - 1]) < 0)
  {
    LSLiveInterval *tmp = ls->active_set[pos - 1];
    ls->active_set[pos - 1] = ls->active_set[pos];
    ls->active_set[pos] = tmp;
    --pos;
  }
  while (pos < ls->next_active_index - 1 && active_interval_compare(ls->active_set[pos], ls->active_set[pos + 1]) > 0)
  {
    LSLiveInterval *tmp = ls->active_set[pos + 1];
    ls->active_set[pos + 1] = ls->active_set[pos];
    ls->active_set[pos] = tmp;
    ++pos;
  }
}

static void tcc_ls_active_set_insert(LSLiveIntervalState *ls, LSLiveInterval *interval)
{
  int pos = ls->next_active_index++;
  ls->active_set[pos] = interval;

  while (pos > 0 && active_interval_compare(ls->active_set[pos], ls->active_set[pos - 1]) < 0)
  {
    LSLiveInterval *tmp = ls->active_set[pos - 1];
    ls->active_set[pos - 1] = ls->active_set[pos];
    ls->active_set[pos] = tmp;
    --pos;
  }
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
  /* Prefer caller-saved (r0-r3, r12) over callee-saved (r4-r11)
   * to minimize push/pop in prologue/epilogue. */
  static const int alloc_order[] = {0, 1, 2, 3, 12, 4, 5, 6, 7, 8, 9, 10, 11};
  for (int idx = 0; idx < 13; ++idx)
  {
    int reg = alloc_order[idx];
    if (reg >= tcc_state->registers_for_allocator)
      continue;
    int assigned_reg = tcc_ls_assign_register(ls, reg);
    if (assigned_reg != -1)
      return assigned_reg;
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
  /* Callee-saved registers: R4-R11. R12 is caller-saved (AAPCS). */
  for (int reg = 4; reg <= 11; ++reg)
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
  /* Callee-saved register pairs: R4-R11. R12 excluded (caller-saved). */
  for (int reg = 4; reg <= 10; reg += 2)
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

static void tcc_ls_expire_release(LSLiveIntervalState *ls, LSLiveInterval *active)
{
  if (active->reg_type == LS_REG_TYPE_FLOAT)
  {
    LS_DBG("    Releasing float register S%d (vreg=%u ended at %d)", LS_VFP_REG_NUM(active->r0), active->vreg,
           active->end);
    tcc_ls_release_float_register(ls, active->r0);
  }
  else if (active->reg_type == LS_REG_TYPE_DOUBLE)
  {
    LS_DBG("    Releasing double registers S%d:S%d (vreg=%u ended at %d)", LS_VFP_REG_NUM(active->r0),
           LS_VFP_REG_NUM(active->r1), active->vreg, active->end);
    tcc_ls_release_float_register(ls, active->r0);
    if (active->r1 >= 0)
      tcc_ls_release_float_register(ls, active->r1);
  }
  else
  {
    if (active->r1 >= 0 && (active->reg_type == LS_REG_TYPE_LLONG || active->reg_type == LS_REG_TYPE_DOUBLE_SOFT ||
                            active->reg_type == LS_REG_TYPE_COMPLEX_FLOAT))
    {
      LS_DBG("    Releasing register pair R%d:R%d (vreg=%u ended at %d)", active->r0, active->r1, active->vreg,
             active->end);
    }
    else
    {
      LS_DBG("    Releasing register R%d (vreg=%u ended at %d)", active->r0, active->vreg, active->end);
    }
    tcc_ls_release_register(ls, active->r0);
    if (active->r1 >= 0 && (active->reg_type == LS_REG_TYPE_LLONG || active->reg_type == LS_REG_TYPE_DOUBLE_SOFT ||
                            active->reg_type == LS_REG_TYPE_COMPLEX_FLOAT))
    {
      tcc_ls_release_register(ls, active->r1);
    }
  }
}

void tcc_ls_expire_old_intervals(LSLiveIntervalState *ls, int current_index)
{
  LSLiveInterval *current = &ls->intervals[current_index];
  LS_DBG("  Expiring intervals ending before %d (current active=%d)", current->start, ls->next_active_index);

  /* The active set is partitioned by sort_key bit 33: params first (bit=0),
   * non-params after (bit=1). Within each partition, intervals are sorted by
   * end ascending. This guarantees the layout:
   *   [expired params | alive params | expired non-params | alive non-params]
   * So we can break early within each partition instead of scanning all. */

  int write_index = 0;
  int n = ls->next_active_index;

  /* The active set has two contiguous groups: [params sorted by end | non-params sorted by end].
   * Within each group the expired entries form a prefix and alive entries a suffix, so we can
   * break early. The same expire-prefix + memmove-alive pattern applies to both groups. */
  for (int i = 0; i < n;)
  {
    /* Find where this group ends */
    int is_param = TCCIR_DECODE_VREG_TYPE(ls->active_set[i]->vreg) == TCCIR_VREG_TYPE_PARAM;
    int g = i;
    while (g < n && (TCCIR_DECODE_VREG_TYPE(ls->active_set[g]->vreg) == TCCIR_VREG_TYPE_PARAM) == is_param)
      g++;

    /* Expire the expired prefix of this group */
    while (i < g && ls->active_set[i]->end < current->start)
      tcc_ls_expire_release(ls, ls->active_set[i++]);

    /* Bulk-move the alive suffix of this group */
    int alive = g - i;
    if (alive > 0)
    {
      if (write_index != i)
        memmove(&ls->active_set[write_index], &ls->active_set[i], alive * sizeof(LSLiveInterval *));
      write_index += alive;
    }
    i = g;
  }

  if (write_index != n)
  {
    LS_DBG("  Expired %d intervals, %d remain active", n - write_index, write_index);
  }
  ls->next_active_index = write_index;
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
    STACK_ALLOC_LOG("compact", it->vreg, loc, size);
  }
}

/* Spill interval to stack. For doubles, allocates 8 bytes. */
void tcc_ls_spill_interval_sized(LSLiveIntervalState *ls, int interval_index, int size)
{
  LSLiveInterval *interval = &ls->intervals[interval_index];
  LS_DBG("  Spilling interval vreg=%u: trying to find register by spilling another", interval->vreg);

  /* 128-bit complex doubles cannot fit in any register (pair).
   * Always spill to stack without trying to steal a register. */
  if (size > 8)
  {
    interval->stack_location = tcc_ls_next_stack_location_sized(size);
    STACK_ALLOC_LOG("oversize-type", interval->vreg, interval->stack_location, size);
    LS_DBG("    %d-bit type: spilled directly to stack at %d", size * 8, (int)interval->stack_location);
    return;
  }

  /* If no active intervals, just spill to stack */
  if (ls->next_active_index == 0)
  {
    interval->stack_location = tcc_ls_next_stack_location_sized(size);
    STACK_ALLOC_LOG("no-active-intervals", interval->vreg, interval->stack_location, size);
    LS_DBG("    No active intervals, spilled to stack at %d", (int)interval->stack_location);
    return;
  }
  int needs_pair = (size == 8);

  /* Find the best eviction candidate: prefer the active interval with the
   * fewest remaining uses (lowest spill cost). Among candidates with equal
   * use count, prefer the longest-living (original heuristic). */
  LSLiveInterval *best = NULL;
  int best_idx = -1;
  uint16_t best_uses = UINT16_MAX;
  uint32_t best_end = 0;

  for (int j = ls->next_active_index - 1; j >= 0; --j)
  {
    LSLiveInterval *cand = ls->active_set[j];
    if (cand->end < interval->end)
      break;
    if (cand->r0 < 0 || cand->stack_location != 0)
      continue;
    if (needs_pair && cand->r1 < 0)
      continue;
    uint16_t cand_uses = ls_get_use_count(ls, cand);
    if (cand_uses < best_uses || (cand_uses == best_uses && cand->end > best_end))
    {
      best_uses = cand_uses;
      best_end = cand->end;
      best = cand;
      best_idx = j;
    }
  }

  /* Only steal if the candidate's spill cost is lower than the current
   * interval's cost — otherwise just spill the current interval. */
  uint16_t cur_uses = ls_get_use_count(ls, interval);
  if (best && best_uses <= cur_uses)
  {
    LS_DBG("    Stealing register%s from vreg=%u (uses=%u <= cur=%u)", needs_pair ? " pair" : "",
           best->vreg, best_uses, cur_uses);
    interval->r0 = best->r0;
    interval->r1 = best->r1;
    best->r0 = -1;
    best->r1 = -1;
    {
      int _spill_sz = tcc_ls_reg_type_stack_size(best->reg_type);
      best->stack_location = tcc_ls_next_stack_location_sized(_spill_sz);
      STACK_ALLOC_LOG("stolen-by-cost", best->vreg, best->stack_location, _spill_sz);
    }
    if (needs_pair)
    {
      LS_DBG("    Got register pair R%d:R%d", interval->r0, interval->r1);
    }
    else if (interval->reg_type == LS_REG_TYPE_FLOAT || interval->reg_type == LS_REG_TYPE_DOUBLE)
    {
      LS_DBG("    Got float register S%d", LS_VFP_REG_NUM(interval->r0));
    }
    else
    {
      LS_DBG("    Got register R%d", interval->r0);
    }
    ls->active_set[best_idx] = interval;
    tcc_ls_active_set_reposition_at(ls, best_idx);
  }
  else
  {
    interval->stack_location = tcc_ls_next_stack_location_sized(size);
    STACK_ALLOC_LOG("no-reg-available", interval->vreg, interval->stack_location, size);
    LS_DBG("    Spilled to stack at %d", (int)interval->stack_location);
  }
}

void tcc_ls_spill_interval(LSLiveIntervalState *ls, int interval_index)
{
  tcc_ls_spill_interval_sized(ls, interval_index, 4);
}

#ifdef TCC_LS_DEBUG
static void tcc_ls_print_intervals(LSLiveIntervalState *ls);
#endif

void tcc_ls_allocate_registers(LSLiveIntervalState *ls, int used_parameters_registers,
                               int used_float_parameters_registers, int spill_base)
{
  LS_DBG("=== Starting register allocation ===");
  LS_DBG("Parameters: used_param_regs=%d used_float_param_regs=%d spill_base=%d", used_parameters_registers,
         used_float_parameters_registers, spill_base);
  LS_DBG("Available integer registers: 0x%llx", (unsigned long long)tcc_state->registers_map_for_allocator);
  LS_DBG("Available float registers: 0x%llx", (unsigned long long)tcc_state->float_registers_map_for_allocator);

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
  LS_DBG("Initial integer register map: 0x%llx", (unsigned long long)ls->registers_map);
  LS_DBG("Initial float register map: 0x%llx", (unsigned long long)ls->float_registers_map);

  /* If this function has a static chain (nested function with captured variables),
   * reserve R10 for the static chain pointer. */
  if (tcc_state->ir && tcc_state->ir->has_static_chain)
  {
    int chain_reg = architecture_config.static_chain_reg;
    ls->registers_map &= ~((uint64_t)1 << chain_reg);
    LS_DBG("Reserved static chain register R%d", chain_reg);
  }

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
    LS_DBG("Marking float parameter register S%d as used", i);
    tcc_ls_mark_float_register_as_used(ls, i);
  }
  qsort(ls->intervals, ls->next_interval_index, sizeof(LSLiveInterval), sort_startpoints);
  LS_DBG("Sorted %d intervals by start point", ls->next_interval_index);
  for (int i = 0; i < ls->next_interval_index; ++i)
  {
    LS_DBG("--- Processing interval %d/%d: vreg=%u range=[%d,%d] ---", i, ls->next_interval_index,
           ls->intervals[i].vreg, ls->intervals[i].start, ls->intervals[i].end);
    tcc_ls_expire_old_intervals(ls, i);
    LS_DBG("After expire: active_set size=%d, available int regs=0x%llx, available float regs=0x%llx",
           ls->next_active_index, (unsigned long long)ls->registers_map, (unsigned long long)ls->float_registers_map);

    /* Variables whose address is taken must be on the stack */
    if (ls->intervals[i].addrtaken)
    {
      int _sz = tcc_ls_reg_type_stack_size(ls->intervals[i].reg_type);
      ls->intervals[i].stack_location = tcc_ls_next_stack_location_sized(_sz);
      STACK_ALLOC_LOG("addrtaken", ls->intervals[i].vreg, ls->intervals[i].stack_location, _sz);
      LS_DBG("  Address-taken variable -> spilled to stack at %d", (int)ls->intervals[i].stack_location);
      /* Clear any precolored register hint: the variable lives on the stack,
       * the register was never taken from registers_map, so we must not
       * release it when this interval expires. */
      ls->intervals[i].r0 = -1;
      ls->intervals[i].r1 = -1;
      tcc_ls_active_set_insert(ls, &ls->intervals[i]);
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
        tcc_ls_active_set_insert(ls, &ls->intervals[i]);
        continue;
      }
      if (ls->intervals[i].r0 == -1)
      {
        /* For floats crossing calls, all S0-S15 are caller-saved anyway */
        ls->intervals[i].r0 = tcc_ls_assign_any_float_register(ls);
        LS_DBG("  Assigned float register S%d (any)", LS_VFP_REG_NUM(ls->intervals[i].r0));
      }
      else
      {
        /* r0 already contains the VFP register index - extract it, assign,
         * and re-add marker */
        int vfp_idx = LS_IS_VFP_REG(ls->intervals[i].r0) ? LS_VFP_REG_NUM(ls->intervals[i].r0) : ls->intervals[i].r0;
        int assigned = tcc_ls_assign_float_register(ls, vfp_idx);
        ls->intervals[i].r0 = (assigned >= 0) ? LS_VFP_REG_BASE + assigned : -1;
        LS_DBG("  Assigned precolored float register S%d (requested S%d)", assigned, vfp_idx);
      }
      if (ls->intervals[i].r0 == -1)
      {
        /* Spill to stack */
        LS_DBG("  No float register available, spilling to stack");
        tcc_ls_spill_interval(ls, i);
      }
    }
    else if (ls->intervals[i].reg_type == LS_REG_TYPE_LLONG || ls->intervals[i].reg_type == LS_REG_TYPE_DOUBLE_SOFT ||
             ls->intervals[i].reg_type == LS_REG_TYPE_COMPLEX_FLOAT)
    {
      /* 64-bit integer type or complex float - needs two integer registers */
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
        LS_DBG("  Could not allocate register pair, spilling to stack");
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
      else
      {
        LS_DBG("  Assigned register pair R%d:R%d%s", ls->intervals[i].r0, ls->intervals[i].r1,
               ls->intervals[i].crosses_call ? " (callee-saved)" : "");
      }
    }
    else if (ls->intervals[i].reg_type == LS_REG_TYPE_COMPLEX_DOUBLE)
    {
      /* 128-bit complex double: always spill (cannot fit in a register pair) */
      LS_DBG("  Complex double (128-bit): force-spilling to stack");
      tcc_ls_spill_interval_sized(ls, i, 16); /* 128-bit = 16 bytes */
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
          if (ls->intervals[i].r0 != -1)
          {
            LS_DBG("  Assigned callee-saved register R%d", ls->intervals[i].r0);
          }
        }
        else
        {
          ls->intervals[i].r0 = tcc_ls_assign_any_register(ls);
          if (ls->intervals[i].r0 != -1)
          {
            LS_DBG("  Assigned register R%d", ls->intervals[i].r0);
          }
        }
      }
      else
      {
        int precolored = ls->intervals[i].r0;
        ls->intervals[i].r0 = tcc_ls_assign_register(ls, precolored);
        if (ls->intervals[i].r0 != -1)
        {
          LS_DBG("  Assigned precolored register R%d", ls->intervals[i].r0);
        }
        else
        {
          LS_DBG("  Precolored register R%d unavailable, trying fallback", precolored);
          if (ls->intervals[i].crosses_call)
            ls->intervals[i].r0 = tcc_ls_assign_callee_saved_register(ls);
          else
            ls->intervals[i].r0 = tcc_ls_assign_any_register(ls);
          if (ls->intervals[i].r0 != -1)
            LS_DBG("  Fallback assigned register R%d", ls->intervals[i].r0);
        }
      }

      if (ls->intervals[i].r0 == -1)
      {
        // add spilling
        LS_DBG("  No register available, spilling to stack");
        tcc_ls_spill_interval(ls, i);
      }
    }
    tcc_ls_active_set_insert(ls, &ls->intervals[i]);
  }

#ifdef TCC_LS_DEBUG
  tcc_ls_print_intervals(ls);
  LS_DBG("Final dirty registers: int=0x%llx float=0x%llx", (unsigned long long)ls->dirty_registers,
         (unsigned long long)ls->dirty_float_registers);
  LS_DBG("=== Register allocation complete ===");
#endif

  /* --- Register pressure audit ---
   * Check which callee-saved registers (R4-R11) in dirty_registers are
   * actually assigned to a live interval.  A register that is dirty but
   * not assigned to any interval is a "ghost" — it was allocated and then
   * the interval was expired/spilled, but dirty_registers was never cleared.
   */
  {
    uint64_t callee_dirty = 0;
    uint64_t callee_actually_used = 0;
    for (int r = 4; r <= 11; ++r)
    {
      if (ls->dirty_registers & (1ULL << r))
        callee_dirty |= (1ULL << r);
    }
    for (int j = 0; j < ls->next_interval_index; ++j)
    {
      int r = ls->intervals[j].r0;
      if (r >= 4 && r <= 11)
        callee_actually_used |= (1ULL << r);
      r = ls->intervals[j].r1;
      if (r >= 4 && r <= 11)
        callee_actually_used |= (1ULL << r);
    }
  }

  /* Build O(1) scratch-reg liveness table for codegen. */
  tcc_ls_build_live_regs_by_instruction(ls);
}

void tcc_ls_recompute_dirty_registers(LSLiveIntervalState *ls)
{
  if (!ls->live_regs_by_instruction || ls->live_regs_by_instruction_size <= 0)
    return;

  /* Compute the union of all registers that appear live at any instruction. */
  uint64_t actually_used = 0;
  for (int i = 0; i < ls->live_regs_by_instruction_size; ++i)
    actually_used |= (uint64_t)ls->live_regs_by_instruction[i];

  /* Only keep callee-saved registers (R4-R11) that are actually live
   * somewhere.  Caller-saved registers (R0-R3, R12) stay as-is. */
  uint64_t callee_mask = 0;
  for (int r = 4; r <= 11; ++r)
    callee_mask |= (1ULL << r);

  uint64_t old_dirty = ls->dirty_registers;
  uint64_t non_callee = old_dirty & ~callee_mask;
  uint64_t callee_dirty = old_dirty & callee_mask;
  uint64_t callee_used = actually_used & callee_mask;
  ls->dirty_registers = non_callee | (callee_dirty & callee_used);
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

  LS_DBG("  Finding scratch register at instruction %d (is_leaf=%d)", instruction_idx, is_leaf);
  LS_DBG("    Exclude regs: 0x%x", exclude_regs);

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
    LS_DBG("    Using precomputed liveness: 0x%x", live_regs);
  }
  else
  {
    /* Use cached live registers if same instruction, otherwise compute and cache */
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

  /* Prefer caller-saved registers only.
   * Scratch allocation happens after the function prolog has been emitted.
   * Returning a callee-saved register (R4-R11) here can violate the ABI unless
   * the prolog already saved it.
   */
  /* First try R0-R3 (caller-saved, often free for scratch) */
  {
    const uint32_t avail_low = (~live_regs) & 0xFu;
    if (avail_low)
    {
      int reg = (int)__builtin_ctz(avail_low);
      LS_DBG("    Found scratch register R%d (from R0-R3)", reg);
      return reg;
    }
  }

  /* Then try R12 (IP - inter-procedure scratch) */
  if (!(live_regs & (1u << 12)))
  {
    LS_DBG("    Found scratch register R12 (IP)");
    return 12;
  }

  /* IMPORTANT: Do NOT return R11 or any callee-saved register (R4-R10) here!
   * These registers can only be used as scratch if they were already saved
   * in the function prolog. If we return them as "free", the caller won't
   * save them (since they appear "free"), but the prolog also didn't save
   * them (since they weren't in dirty_registers), leading to ABI violations.
   *
   * The caller (get_scratch_reg_with_save) will fall through to push/pop
   * these registers if no caller-saved registers are available.
   */

  /* Finally try LR if not a leaf function */
  if (!is_leaf && !(live_regs & (1u << 14)))
  {
    LS_DBG("    Found scratch register R14 (LR)");
    return 14;
  }

  /* No register available */
  LS_DBG("    No scratch register available");
  return PREG_NONE;
}
