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

#define LS_LIVE_INTERVAL_INIT_SIZE 64

void tcc_ls_initialize(LSLiveIntervalState *ls)
{
  ls->intervals_size = LS_LIVE_INTERVAL_INIT_SIZE;
  ls->intervals = (LSLiveInterval *)tcc_malloc(sizeof(LSLiveInterval) * ls->intervals_size);
  ls->next_interval_index = 0;

  ls->active_set = (LSLiveInterval **)tcc_malloc(sizeof(LSLiveInterval *) * LS_LIVE_INTERVAL_INIT_SIZE);
  ls->next_active_index = 0;
  ls->dirty_registers = 0;
  ls->dirty_float_registers = 0;
}

void tcc_ls_deinitialize(LSLiveIntervalState *ls)
{
  tcc_free(ls->intervals);
  tcc_free(ls->active_set);
}

void tcc_ls_clear_live_intervals(LSLiveIntervalState *ls)
{
  ls->next_interval_index = 0;
  ls->next_active_index = 0;
}

void tcc_ls_add_live_interval(LSLiveIntervalState *ls, int vreg, int start, int end, int crosses_call, int addrtaken,
                              int reg_type, int lvalue)
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
  printf("Adding live interval for ");
  tcc_ir_print_vreg(vreg);
  printf(", start=%d end=%d crosses_call=%d addrtaken=%d reg_type=%d, is_lvalue: %d\n", start, end, crosses_call,
         addrtaken, reg_type, lvalue);
  interval->end = end;
  interval->r0 = -1;
  interval->r1 = -1;
  interval->stack_location = 0;
  interval->crosses_call = crosses_call;
  interval->addrtaken = addrtaken;
  interval->reg_type = reg_type;
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
  if (tcc_state->registers_map_for_allocator & ((uint64_t)1 << reg))
  {
    ls->registers_map |= ((uint64_t)1 << reg);
    return;
  }
  // fprintf(stderr, "Error: trying to release unallocatable register %d\n",
  // reg); exit(1);
}

void tcc_ls_release_float_register(LSLiveIntervalState *ls, int reg)
{
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
      /* Skip R12:R13 - R13 is SP */
      if (reg + 1 == 13)
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
      if (reg + 1 == 13)
        continue; /* Skip R12:R13 */
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
  for (int i = 0; i < ls->next_active_index; ++i)
  {
    if (ls->active_set[i]->end >= current->start)
    {
      break;
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
  loc = (loc - size) & -size;
  return loc;
}

int tcc_ls_next_stack_location()
{
  return tcc_ls_next_stack_location_sized(4);
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
   * 2. spill actually has a valid register (r0 >= 0 and not already spilled) */
  if (spill->end > interval->end && spill->r0 >= 0 && spill->stack_location == 0)
  {
    interval->r0 = spill->r0;
    interval->r1 = spill->r1;
    spill->r0 = -1; /* Clear register from spilled interval */
    spill->r1 = -1;
    spill->stack_location = tcc_ls_next_stack_location_sized(size);
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
                               int used_float_parameters_registers)
{
  // make all registers available at start
  ls->dirty_registers = 0;
  ls->dirty_float_registers = 0;
  ls->registers_map = tcc_state->registers_map_for_allocator;
  ls->float_registers_map = tcc_state->float_registers_map_for_allocator;
  // for (int i = 0; i < used_parameters_registers; ++i) {
  //   tcc_ls_mark_register_as_used(ls, i);
  // }
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
      ls->intervals[i].stack_location = tcc_ls_next_stack_location();
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
        ls->intervals[i].r0 = tcc_ls_assign_register(ls, ls->intervals[i].r0);
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

  for (int i = 0; i < ls->next_interval_index; ++i)
  {
    /* Check for invalid state: r0 == -1 but not spilled to stack */
    if (ls->intervals[i].r0 == -1 && ls->intervals[i].stack_location == 0 && !ls->intervals[i].addrtaken)
    {
      printf("ERROR: Interval %d has r0=-1 but stack_location=0 (not spilled)! vreg=0x%x\n", i, ls->intervals[i].vreg);
    }
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

/* Find a free scratch register at the given instruction index.
 * Returns -1 if no register is available.
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

  /* Mark all registers that are live at this instruction */
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

  /* Prefer caller-saved registers R0-R3, then R12 (IP), then callee-saved R4-R11 */
  /* First try R0-R3 (caller-saved, often free for scratch) */
  for (int r = 0; r <= 3; ++r)
  {
    if (!(live_regs & (1 << r)))
      return r;
  }

  /* Then try R12 (IP - inter-procedure scratch) */
  if (!(live_regs & (1 << 12)))
    return 12;

  /* Then try callee-saved R4-R11 */
  for (int r = 4; r <= 11; ++r)
  {
    if (!(live_regs & (1 << r)))
      return r;
  }

  /* Finally try LR if not a leaf function */
  if (!is_leaf && !(live_regs & (1 << 14)))
    return 14;

  /* No register available */
  return PREG_NONE;
}