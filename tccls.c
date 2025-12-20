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

void tcc_ls_initialize(LSLiveIntervalState *ls) {
  ls->intervals_size = LS_LIVE_INTERVAL_INIT_SIZE;
  ls->intervals =
      (LSLiveInterval *)tcc_malloc(sizeof(LSLiveInterval) * ls->intervals_size);
  ls->next_interval_index = 0;

  ls->active_set = (LSLiveInterval **)tcc_malloc(sizeof(LSLiveInterval *) *
                                                 LS_LIVE_INTERVAL_INIT_SIZE);
  ls->next_active_index = 0;
  ls->dirty_registers = 0;
}

void tcc_ls_deinitialize(LSLiveIntervalState *ls) {
  tcc_free(ls->intervals);
  tcc_free(ls->active_set);
}

void tcc_ls_clear_live_intervals(LSLiveIntervalState *ls) {
  ls->next_interval_index = 0;
  ls->next_active_index = 0;
}

void tcc_ls_add_live_interval(LSLiveIntervalState *ls, int vreg, int start,
                              int end, int crosses_call) {
  LSLiveInterval *interval;

  if (ls->next_interval_index >= ls->intervals_size) {
    ls->intervals_size <<= 1;
    ls->intervals = (LSLiveInterval *)tcc_realloc(
        ls->intervals, sizeof(LSLiveInterval) * ls->intervals_size);
  }

  interval = &ls->intervals[ls->next_interval_index];
  interval->vreg = vreg;
  interval->start = start;
  printf("Adding live interval for ");
  tcc_ir_print_vreg(vreg);
  printf(", start=%d end=%d crosses_call=%d\n", start, end, crosses_call);
  interval->end = end;
  interval->r0 = -1;
  interval->r1 = -1;
  interval->stack_location = 0;
  interval->crosses_call = crosses_call;
  ls->next_interval_index++;
}

static int sort_startpoints(const void *a, const void *b) {
  LSLiveInterval *ia = (LSLiveInterval *)a;
  LSLiveInterval *ib = (LSLiveInterval *)b;
  if (ia->start == 0 && ib->start == 0) {
    if (TCCIR_DECODE_VREG_TYPE(ia->vreg) == TCCIR_VREG_TYPE_PARAM) {
      return -1;
    }
    return 0;
  }
  if (ia->start < ib->start)
    return -1;
  else if (ia->start > ib->start)
    return 1;
  return 0;
}

static int sort_endpoints(const void *a, const void *b) {
  LSLiveInterval *ia = *(LSLiveInterval **)a;
  LSLiveInterval *ib = *(LSLiveInterval **)b;
  if (ia->end < ib->end)
    return -1;
  else if (ia->end > ib->end)
    return 1;
  return 0;
}

void tcc_ls_release_register(LSLiveIntervalState *ls, int reg) {
  if (tcc_state->registers_map_for_allocator & ((uint64_t)1 << reg)) {
    ls->registers_map |= ((uint64_t)1 << reg);
    return;
  }
  // fprintf(stderr, "Error: trying to release unallocatable register %d\n",
  // reg); exit(1);
}

int tcc_ls_assign_register(LSLiveIntervalState *ls, int reg) {
  if (tcc_state->registers_map_for_allocator & ((uint64_t)1 << reg)) {
    if (ls->registers_map & ((uint64_t)1 << reg)) {
      ls->registers_map &= ~((uint64_t)1 << reg);
      ls->dirty_registers |= ((uint64_t)1 << reg);
      return reg;
    }
  }
  return -1;
}

int tcc_ls_assign_any_register(LSLiveIntervalState *ls) {
  for (int reg = 0; reg < tcc_state->registers_for_allocator; ++reg) {
    int assigned_reg = tcc_ls_assign_register(ls, reg);
    if (assigned_reg != -1) {
      return assigned_reg;
    }
  }
  return -1;
}

/* Assign a callee-saved register (R4-R12) for intervals that cross calls */
int tcc_ls_assign_callee_saved_register(LSLiveIntervalState *ls) {
  /* Callee-saved registers start at R4 */
  for (int reg = 4; reg < tcc_state->registers_for_allocator; ++reg) {
    int assigned_reg = tcc_ls_assign_register(ls, reg);
    if (assigned_reg != -1) {
      return assigned_reg;
    }
  }
  return -1;
}

void tcc_ls_expire_old_intervals(LSLiveIntervalState *ls, int current_index) {
  int removed_intervals = 0;
  LSLiveInterval *current = &ls->intervals[current_index];
  static LSLiveInterval dirty = {
      .r0 = 0,
      .r1 = 0,
      .vreg = 0,
      .stack_location = 0,
      .start = 0,
      .end = ~0,
  };
  for (int i = 0; i < ls->next_active_index; ++i) {
    if (ls->active_set[i]->end >= current->start) {
      break;
    }
    tcc_ls_release_register(ls, ls->active_set[i]->r0);
    if (ls->active_set[i]->r1 != 0) {
      tcc_ls_release_register(ls, ls->active_set[i]->r1);
    }
    ls->active_set[i] = &dirty; // mark as removed
  }
  qsort(ls->active_set, ls->next_active_index, sizeof(LSLiveInterval *),
        sort_endpoints);
  ls->next_active_index -= removed_intervals;
}

void tcc_ls_mark_register_as_used(LSLiveIntervalState *ls, int reg) {
  if (tcc_state->registers_map_for_allocator & ((uint64_t)1 << reg)) {
    ls->registers_map &= ~((uint64_t)1 << reg);
    ls->dirty_registers |= ((uint64_t)1 << reg);
    return;
  }
  fprintf(stderr, "Error: trying to mark unallocatable register %d as used\n",
          reg);
  exit(1);
}

int tcc_ls_next_stack_location() {
  tcc_state->stack_location = (loc - 4) & -4;
  return loc;
}

void tcc_ls_spill_interval(LSLiveIntervalState *ls, int interval_index) {
  static uint32_t next_stack_location = 0;
  LSLiveInterval *interval = &ls->intervals[interval_index];
  LSLiveInterval *spill = ls->active_set[ls->next_active_index - 1];
  if (spill->end > interval->end) {
    interval->r0 = spill->r0;
    interval->r1 = spill->r1;
    spill->stack_location = tcc_ls_next_stack_location();
    ls->active_set[ls->next_active_index - 1] = interval;
    qsort(ls->active_set, ls->next_active_index, sizeof(LSLiveInterval *),
          sort_endpoints);
  } else {
    interval->stack_location = tcc_ls_next_stack_location();
  }
}

void tcc_ls_allocate_registers(LSLiveIntervalState *ls,
                               int used_parameters_registers) {
  // make all registers available at start
  ls->dirty_registers = 0;
  ls->registers_map = tcc_state->registers_map_for_allocator;
  for (int i = 0; i < used_parameters_registers; ++i) {
    tcc_ls_mark_register_as_used(ls, i);
  }
  qsort(ls->intervals, ls->next_interval_index, sizeof(LSLiveInterval),
        sort_startpoints);
  for (int i = 0; i < ls->next_interval_index; ++i) {
    tcc_ls_expire_old_intervals(ls, i);

    if (ls->intervals[i].r0 == -1) {
      /* If interval crosses a function call, use callee-saved registers only */
      if (ls->intervals[i].crosses_call) {
        ls->intervals[i].r0 = tcc_ls_assign_callee_saved_register(ls);
      } else {
        ls->intervals[i].r0 = tcc_ls_assign_any_register(ls);
      }
    } else {
      ls->intervals[i].r0 = tcc_ls_assign_register(ls, ls->intervals[i].r0);
    }

    if (ls->intervals[i].r0 == -1) {
      // add spilling
      tcc_ls_spill_interval(ls, i);
    }
    ls->active_set[ls->next_active_index++] = &ls->intervals[i];
    qsort(ls->active_set, ls->next_active_index, sizeof(LSLiveInterval *),
          sort_endpoints);
  }

  for (int i = 0; i < ls->next_interval_index; ++i) {
    printf("Interval %d (%d,%d), ", i, ls->intervals[i].start,
           ls->intervals[i].end);
    tcc_ir_print_vreg(ls->intervals[i].vreg);
    printf(" --> ");
    if (ls->intervals[i].stack_location) {
      printf("spilled to stack at %d\n", ls->intervals[i].stack_location);
    } else {
      printf("R0%d", ls->intervals[i].r0);
      if (ls->intervals[i].r1 >= 0) {
        printf(", R1%d", ls->intervals[i].r1);
      }
      printf("\n");
    }
  }
}