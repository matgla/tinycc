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
  ls->live_sweep_order = NULL;
  ls->live_sweep_active = NULL;
  ls->live_sweep_valid = 0;
  ls->live_sweep_count = 0;
  ls->live_sweep_pos = 0;
  ls->live_sweep_active_count = 0;
  ls->live_sweep_last_idx = -1;
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

  tcc_free(ls->live_sweep_order);
  tcc_free(ls->live_sweep_active);
  ls->live_sweep_order = NULL;
  ls->live_sweep_active = NULL;
  ls->live_sweep_valid = 0;
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
  ls->live_sweep_valid = 0;

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
  interval->co_member = 0;
  {
    const int is_param = (TCCIR_DECODE_VREG_TYPE(vreg) == TCCIR_VREG_TYPE_PARAM);
    interval->sort_key = ((uint64_t)(!is_param) << 33) | ((uint64_t)(uint32_t)end << 1) | (lvalue ? 0u : 1u);
  }
  ls->next_interval_index++;
  ls->live_sweep_valid = 0;
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

  const int n = ls->next_interval_index;
  if (n == 0)
    return;

  /* Build a mapping from old stack_location -> new stack_location so that
   * multiple intervals sharing a slot (from regalloc slot reuse) continue
   * to share after compaction.  Without this mapping, each interval would
   * be assigned a fresh slot here, undoing addrtaken slot coalescing. */
  typedef struct
  {
    int old_offset;
    int size;
    int new_offset;
  } SlotMapEntry;

  SlotMapEntry *map = tcc_malloc(sizeof(SlotMapEntry) * n);
  int map_count = 0;

  unsigned ht_size = 16;
  while (ht_size < (unsigned)n * 2)
    ht_size <<= 1;
  int *ht = tcc_malloc(sizeof(int) * ht_size);
  for (unsigned i = 0; i < ht_size; ++i)
    ht[i] = -1;
#define SLOT_HASH(off) (((uint32_t)(off) * 2654435761u) & (ht_size - 1))

  /* Pass 1: collect distinct old offsets in first-encounter order (pass 2
   * assigns new offsets in that order) and track the max size required at
   * each (so a slot shared by a 4-byte and an 8-byte interval gets 8). */
  for (int i = 0; i < n; ++i)
  {
    LSLiveInterval *it = &ls->intervals[i];
    if (it->stack_location == 0)
      continue;

    const int size = tcc_ls_reg_type_stack_size(it->reg_type);
    unsigned h = SLOT_HASH(it->stack_location);
    while (ht[h] >= 0 && map[ht[h]].old_offset != (int)it->stack_location)
      h = (h + 1) & (ht_size - 1);
    if (ht[h] >= 0)
    {
      if (size > map[ht[h]].size)
        map[ht[h]].size = size;
    }
    else
    {
      map[map_count].old_offset = it->stack_location;
      map[map_count].size = size;
      map[map_count].new_offset = 0;
      ht[h] = map_count;
      map_count++;
    }
  }

  /* Pass 2: assign new offsets in the same order as old offsets were
   * encountered.  This preserves any relative ordering the codegen
   * relied on (e.g. adjacent spill slots for LDRD pairs). */
  int loc = spill_base;
  for (int j = 0; j < map_count; ++j)
  {
    const int size = map[j].size;
    loc = (loc - size) & -size;
    if (loc == 0)
      loc = -size;
    map[j].new_offset = loc;
  }

  /* Pass 3: rewrite each interval's stack_location through the map. */
  for (int i = 0; i < n; ++i)
  {
    LSLiveInterval *it = &ls->intervals[i];
    if (it->stack_location == 0)
      continue;

    unsigned h = SLOT_HASH(it->stack_location);
    while (ht[h] >= 0 && map[ht[h]].old_offset != (int)it->stack_location)
      h = (h + 1) & (ht_size - 1);
    if (ht[h] >= 0)
    {
      int j = ht[h];
      it->stack_location = map[j].new_offset;
      STACK_ALLOC_LOG("compact", it->vreg, map[j].new_offset, map[j].size);
    }
  }

#undef SLOT_HASH
  tcc_free(ht);
  tcc_free(map);
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

static uint32_t ls_interval_live_mask(const LSLiveInterval *interval, uint32_t idx)
{
  uint32_t mask = 0;
  if (interval->reg_type != LS_REG_TYPE_INT && interval->reg_type != LS_REG_TYPE_LLONG)
    return 0;
  if (interval->start <= idx && interval->end >= idx) {
    if (interval->r0 >= 0 && interval->r0 < 16)
      mask |= (1u << interval->r0);
    if (interval->r1 >= 0 && interval->r1 < 16)
      mask |= (1u << interval->r1);
  }
  return mask;
}

static const LSLiveInterval *ls_sweep_cmp_intervals;

static int ls_sweep_cmp_start(const void *a, const void *b)
{
  uint32_t sa = ls_sweep_cmp_intervals[*(const int *)a].start;
  uint32_t sb = ls_sweep_cmp_intervals[*(const int *)b].start;
  return sa < sb ? -1 : sa > sb;
}

/* Advance the sweep so live_sweep_active holds exactly the intervals with
 * start <= idx <= end (idx must be >= 0). */
static void ls_sweep_advance(LSLiveIntervalState *ls, uint32_t idx)
{
  if (!ls->live_sweep_valid) {
    int n = ls->next_interval_index;
    ls->live_sweep_order = tcc_realloc(ls->live_sweep_order, sizeof(int) * (n > 0 ? n : 1));
    ls->live_sweep_active = tcc_realloc(ls->live_sweep_active, sizeof(int) * (n > 0 ? n : 1));
    for (int i = 0; i < n; ++i)
      ls->live_sweep_order[i] = i;
    ls_sweep_cmp_intervals = ls->intervals;
    qsort(ls->live_sweep_order, n, sizeof(int), ls_sweep_cmp_start);
    ls->live_sweep_count = n;
    ls->live_sweep_valid = 1;
    ls->live_sweep_pos = 0;
    ls->live_sweep_active_count = 0;
    ls->live_sweep_last_idx = -1;
  }

  if ((int)idx < ls->live_sweep_last_idx) {
    ls->live_sweep_pos = 0;
    ls->live_sweep_active_count = 0;
  }
  ls->live_sweep_last_idx = (int)idx;

  while (ls->live_sweep_pos < ls->live_sweep_count &&
         ls->intervals[ls->live_sweep_order[ls->live_sweep_pos]].start <= idx) {
    int ii = ls->live_sweep_order[ls->live_sweep_pos++];
    if (ls->intervals[ii].end >= idx)
      ls->live_sweep_active[ls->live_sweep_active_count++] = ii;
  }

  int kept = 0;
  for (int k = 0; k < ls->live_sweep_active_count; ++k) {
    int ii = ls->live_sweep_active[k];
    if (ls->intervals[ii].end < idx)
      continue;
    ls->live_sweep_active[kept++] = ii;
  }
  ls->live_sweep_active_count = kept;
}

uint32_t tcc_ls_compute_live_regs(LSLiveIntervalState *ls, int instruction_idx)
{
  if (instruction_idx < 0) {
    uint32_t live_regs = 0;
    for (int i = 0; i < ls->next_interval_index; ++i)
      live_regs |= ls_interval_live_mask(&ls->intervals[i], (uint32_t)instruction_idx);
    return live_regs;
  }

  ls_sweep_advance(ls, (uint32_t)instruction_idx);

  uint32_t live_regs = 0;
  for (int k = 0; k < ls->live_sweep_active_count; ++k)
    live_regs |= ls_interval_live_mask(&ls->intervals[ls->live_sweep_active[k]], (uint32_t)instruction_idx);
  return live_regs;
}

int tcc_ls_find_int_reg_holder(LSLiveIntervalState *ls, int r, int instruction_idx)
{
  int best = -1;
  if (instruction_idx >= 0) {
    ls_sweep_advance(ls, (uint32_t)instruction_idx);
    for (int k = 0; k < ls->live_sweep_active_count; ++k) {
      int ii = ls->live_sweep_active[k];
      const LSLiveInterval *iv = &ls->intervals[ii];
      if (iv->reg_type != LS_REG_TYPE_INT)
        continue;
      if (iv->addrtaken || iv->stack_location != 0)
        continue;
      if (iv->r1 >= 0 && iv->r1 < 16)
        continue;
      if (iv->r0 != r)
        continue;
      if ((int)iv->start > instruction_idx || (int)iv->end < instruction_idx)
        continue;
      if (best < 0 || ii < best)
        best = ii;
    }
  }
  return best;
}

/* True when physical register `reg` is claimed at instruction `pos` by any
 * live interval other than `skip`.  Post-RA register rewriters (move
 * coalescing, the phase-3 scratch-conflict fixup) deliberately make two
 * overlapping intervals share one register (in-place two-address ops), so a
 * single live_regs_by_instruction bit can carry two claims.  When a rewrite
 * moves one claimant away it must leave the bit set wherever another claimant
 * is still live, or the bitmap under-reports and a later rewrite allocates
 * the register on top of a live value. */
int tcc_ls_reg_held_by_other(const LSLiveIntervalState *ls, int reg, int pos, const LSLiveInterval *skip)
{
  for (int i = 0; i < ls->next_interval_index; ++i)
  {
    const LSLiveInterval *iv = &ls->intervals[i];
    if (iv == skip)
      continue;
    if (iv->stack_location != 0)
      continue;
    if (iv->r0 != reg && iv->r1 != reg)
      continue;
    if (iv->start <= (uint32_t)pos && iv->end >= (uint32_t)pos)
      return 1;
  }
  return 0;
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

  /* Union the precomputed per-instruction bitmap with a fresh interval scan.
   * ra_build_live_regs_bitmap deliberately OMITS any interval that carries a
   * stack_location (it assumes a spilled value does not hold a register across
   * its whole range).  That assumption is FALSE for a loop-carried value kept
   * live in a register across the loop body while also owning a spill slot
   * (r0 >= 0 AND stack_location != 0): the bitmap then under-reports that
   * register as free, and the scratch picker can hand it out, clobbering the
   * still-live value (random-C O2 wrong-code, Finding #15).  tcc_ls_compute_live_regs
   * scans the intervals directly (ignoring stack_location) and DOES report it,
   * so unioning the two is correct and strictly conservative: it can only mark
   * MORE registers live, never fewer, so it can never introduce a new clobber. */
  if (ls->live_regs_by_instruction && instruction_idx >= 0 && instruction_idx < ls->live_regs_by_instruction_size)
    live_regs |= ls->live_regs_by_instruction[instruction_idx];

  if (ls->cached_instruction_idx == instruction_idx)
    live_regs |= ls->cached_live_regs;
  else
  {
    uint32_t computed = tcc_ls_compute_live_regs(ls, instruction_idx);
    ls->cached_instruction_idx = instruction_idx;
    ls->cached_live_regs = computed;
    live_regs |= computed;
  }
  LS_DBG("    Liveness (bitmap ∪ interval-scan): 0x%x", live_regs);

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
