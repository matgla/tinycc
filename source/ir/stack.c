/*
 *  TCC IR - Stack Layout Implementation
 *
 *  Copyright (c) 2025 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#define USING_GLOBALS
#include "ir.h"

/* Fallback: the bootstrapping armv8m-tcc may not predefine __INT32_MAX__,
   causing <stdint.h>/<limits.h> to omit INT32_MIN.  Provide it here so
   the build can proceed until the fixed compiler is in place. */
#ifndef INT32_MIN
#define INT32_MIN (-2147483647 - 1)
#endif

#ifndef TCC_STACK_LAYOUT_INIT_CAPACITY
#define TCC_STACK_LAYOUT_INIT_CAPACITY 16
#endif

/* ============================================================================
 * Internal Hash Table for Offset Lookup
 * ============================================================================ */

static inline uint32_t tcc_ir_hash_u32(uint32_t x)
{
  /* A small integer hash suitable for hash tables.
   * (Public-domain style mix; good enough for our offsets.)
   */
  x ^= x >> 16;
  x *= 0x7feb352dU;
  x ^= x >> 15;
  x *= 0x846ca68bU;
  x ^= x >> 16;
  return x;
}

static int tcc_ir_stack_layout_offset_hash_lookup_index(const TCCStackLayout *layout, int offset)
{
  if (!layout || !layout->offset_hash_keys || layout->offset_hash_size <= 0)
    return -1;

  const int size = layout->offset_hash_size;
  const int mask = size - 1;
  uint32_t h = tcc_ir_hash_u32((uint32_t)offset);
  int pos = (int)(h & (uint32_t)mask);

  for (int probe = 0; probe < size; ++probe)
  {
    const int key = layout->offset_hash_keys[pos];
    if (key == INT32_MIN)
      return -1;
    if (key == offset)
      return layout->offset_hash_values[pos];
    pos = (pos + 1) & mask;
  }
  return -1;
}

static void tcc_ir_stack_layout_offset_hash_rebuild(TCCStackLayout *layout, int new_size)
{
  if (!layout)
    return;
  if (new_size < 0)
    return;
  if (new_size == 0)
  {
    if (layout->offset_hash_keys)
      tcc_free(layout->offset_hash_keys);
    if (layout->offset_hash_values)
      tcc_free(layout->offset_hash_values);
    layout->offset_hash_keys = NULL;
    layout->offset_hash_values = NULL;
    layout->offset_hash_size = 0;
    return;
  }

  int *new_keys = (int *)tcc_malloc(sizeof(int) * (size_t)new_size);
  int *new_vals = (int *)tcc_malloc(sizeof(int) * (size_t)new_size);
  for (int i = 0; i < new_size; ++i)
    new_keys[i] = INT32_MIN;

  const int mask = new_size - 1;
  for (int slot_index = 0; slot_index < layout->slot_count; ++slot_index)
  {
    const int offset = layout->slots[slot_index].offset;
    uint32_t h = tcc_ir_hash_u32((uint32_t)offset);
    int pos = (int)(h & (uint32_t)mask);
    while (new_keys[pos] != INT32_MIN)
      pos = (pos + 1) & mask;
    new_keys[pos] = offset;
    new_vals[pos] = slot_index;
  }

  if (layout->offset_hash_keys)
    tcc_free(layout->offset_hash_keys);
  if (layout->offset_hash_values)
    tcc_free(layout->offset_hash_values);
  layout->offset_hash_keys = new_keys;
  layout->offset_hash_values = new_vals;
  layout->offset_hash_size = new_size;
}

static void tcc_ir_stack_layout_offset_hash_ensure_capacity(TCCStackLayout *layout, int needed_slots)
{
  if (!layout)
    return;
  if (needed_slots <= 0)
    return;

  /* Keep load factor <= 0.5 for fast probes. */
  int target = 16;
  while (target < needed_slots * 2)
    target <<= 1;

  if (layout->offset_hash_size >= target)
    return;
  tcc_ir_stack_layout_offset_hash_rebuild(layout, target);
}

static void tcc_ir_stack_layout_offset_hash_insert(TCCStackLayout *layout, int offset, int slot_index)
{
  if (!layout)
    return;
  if (slot_index < 0)
    return;

  if (!layout->offset_hash_keys || layout->offset_hash_size <= 0)
    tcc_ir_stack_layout_offset_hash_ensure_capacity(layout, layout->slot_count + 1);

  if (!layout->offset_hash_keys || layout->offset_hash_size <= 0)
    return;

  const int size = layout->offset_hash_size;
  const int mask = size - 1;
  uint32_t h = tcc_ir_hash_u32((uint32_t)offset);
  int pos = (int)(h & (uint32_t)mask);
  for (int probe = 0; probe < size; ++probe)
  {
    const int key = layout->offset_hash_keys[pos];
    if (key == INT32_MIN || key == offset)
    {
      layout->offset_hash_keys[pos] = offset;
      layout->offset_hash_values[pos] = slot_index;
      return;
    }
    pos = (pos + 1) & mask;
  }

  /* Table unexpectedly full: grow and retry once. */
  tcc_ir_stack_layout_offset_hash_rebuild(layout, size ? (size << 1) : 16);
  if (!layout->offset_hash_keys || layout->offset_hash_size <= 0)
    return;

  /* Retry insert after rebuild. */
  const int new_size = layout->offset_hash_size;
  const int new_mask = new_size - 1;
  h = tcc_ir_hash_u32((uint32_t)offset);
  pos = (int)(h & (uint32_t)new_mask);
  for (int probe = 0; probe < new_size; ++probe)
  {
    const int key = layout->offset_hash_keys[pos];
    if (key == INT32_MIN || key == offset)
    {
      layout->offset_hash_keys[pos] = offset;
      layout->offset_hash_values[pos] = slot_index;
      return;
    }
    pos = (pos + 1) & new_mask;
  }
}

static void tcc_ir_stack_layout_ensure_capacity(TCCStackLayout *layout, int needed_slots)
{
  if (!layout)
    return;
  if (layout->slot_capacity >= needed_slots)
    return;
  int new_capacity = layout->slot_capacity ? layout->slot_capacity : TCC_STACK_LAYOUT_INIT_CAPACITY;
  while (new_capacity < needed_slots)
    new_capacity *= 2;
  layout->slots = (TCCStackSlot *)tcc_realloc(layout->slots, sizeof(TCCStackSlot) * new_capacity);
  layout->slot_capacity = new_capacity;
}

typedef struct TCCStackVregSlot
{
  int vreg;
  int slot;
  int seq; /* build order: a later entry for the same vreg wins */
} TCCStackVregSlot;

static int stack_vreg_slot_cmp(const void *a, const void *b)
{
  const TCCStackVregSlot *x = (const TCCStackVregSlot *)a, *y = (const TCCStackVregSlot *)b;
  if (x->vreg != y->vreg)
    return x->vreg < y->vreg ? -1 : 1;
  return x->seq < y->seq ? -1 : x->seq > y->seq;
}

/* Record that VREG lives in SLOT (only vregs with an interval, as the
 * back-link in the interval used to be). */
static void stack_vreg_slot_note(TCCIRState *ir, int vreg, int slot)
{
  TCCStackLayout *layout = &ir->stack_layout;
  if (!tcc_ir_get_live_interval(ir, vreg))
    return;
  TCCStackVregSlot *e = &layout->vreg_slots[layout->vreg_slot_count];
  e->vreg = vreg;
  e->slot = slot;
  e->seq = layout->vreg_slot_count++;
}

/* Sort the notes by vreg, keeping the last one of each vreg. */
static void stack_vreg_slots_finish(TCCStackLayout *layout)
{
  int n = layout->vreg_slot_count, w = 0;
  if (n <= 0)
    return;
  tcc_qsort(layout->vreg_slots, n, sizeof(TCCStackVregSlot), stack_vreg_slot_cmp);
  for (int i = 0; i < n; i++)
  {
    if (i + 1 < n && layout->vreg_slots[i + 1].vreg == layout->vreg_slots[i].vreg)
      continue;
    layout->vreg_slots[w++] = layout->vreg_slots[i];
  }
  layout->vreg_slot_count = w;
}

static void tcc_ir_stack_layout_reset(TCCStackLayout *layout)
{
  if (!layout)
    return;
  layout->slot_count = 0;
  layout->vreg_slot_count = 0;
  if (layout->offset_hash_keys && layout->offset_hash_size > 0)
  {
    for (int i = 0; i < layout->offset_hash_size; ++i)
      layout->offset_hash_keys[i] = INT32_MIN;
  }
}

/* ============================================================================
 * Stack Layout Build and Query
 * ============================================================================ */

void tcc_ir_stack_build(TCCIRState *ir)
{
  if (!ir)
    return;

  tcc_ir_stack_layout_reset(&ir->stack_layout);

  /* Build stack slots only for intervals that actually ended up stack-backed.
   * We iterate over the allocator's interval list (ir->ls.intervals) which is
   * typically much smaller than the total number of vregs created. We extract
   * all slot metadata directly from LSLiveInterval to avoid expensive
   * tcc_ir_get_live_interval() lookups per interval.
   */
  const int n = ir->ls.next_interval_index;
  if (n <= 0)
    return;

  /* Count stack-backed intervals for pre-sizing. */
  int estimated_slots = 0;
  for (int i = 0; i < n; ++i)
  {
    if (ir->ls.intervals[i].stack_location != 0)
      estimated_slots++;
  }
  if (estimated_slots == 0)
    return;

  /* Pre-allocate slots array and hash table. */
  TCCStackLayout *layout = &ir->stack_layout;
  tcc_ir_stack_layout_ensure_capacity(layout, estimated_slots);
  tcc_ir_stack_layout_offset_hash_ensure_capacity(layout, estimated_slots + 8);
  tcc_free(layout->vreg_slots);
  layout->vreg_slots = (TCCStackVregSlot *)tcc_malloc(sizeof(TCCStackVregSlot) * estimated_slots);

  /* Build slots directly from LSLiveInterval data. */
  for (int i = 0; i < n; ++i)
  {
    const LSLiveInterval *ls_it = &ir->ls.intervals[i];
    const int offset = (int)ls_it->stack_location;
    if (offset == 0)
      continue;

    /* Check if we already have a slot at this offset (via hash). */
    const int existing_idx = tcc_ir_stack_layout_offset_hash_lookup_index(layout, offset);
    if (existing_idx >= 0)
    {
      /* Slot exists; just update vreg owner if needed. */
      TCCStackSlot *slot = &layout->slots[existing_idx];
      if (slot->vreg == -1)
        slot->vreg = (int)ls_it->vreg;
      stack_vreg_slot_note(ir, (int)ls_it->vreg, existing_idx);
      continue;
    }

    /* New slot: derive size from reg_type. */
    int size = 4;
    switch (ls_it->reg_type)
    {
    case LS_REG_TYPE_LLONG:
    case LS_REG_TYPE_DOUBLE:
    case LS_REG_TYPE_DOUBLE_SOFT:
      size = 8;
      break;
    default:
      size = 4;
      break;
    }

    /* Derive kind from vreg type. */
    const TCCIR_VREG_TYPE vtype = (TCCIR_VREG_TYPE)TCCIR_DECODE_VREG_TYPE((int)ls_it->vreg);
    TCCStackSlotKind kind;
    switch (vtype)
    {
    case TCCIR_VREG_TYPE_PARAM:
      kind = TCC_STACK_SLOT_PARAM_SPILL;
      break;
    case TCCIR_VREG_TYPE_VAR:
      kind = TCC_STACK_SLOT_LOCAL;
      break;
    default:
      kind = TCC_STACK_SLOT_SPILL;
      break;
    }

    /* Create slot directly. */
    const int slot_idx = layout->slot_count++;
    TCCStackSlot *slot = &layout->slots[slot_idx];
    slot->offset = offset;
    slot->size = size;
    slot->alignment = (size >= 8) ? 8 : 4;
    slot->kind = kind;
    slot->vreg = (int)ls_it->vreg;

    /* Insert into hash table for fast lookup. */
    tcc_ir_stack_layout_offset_hash_insert(layout, offset, slot_idx);

    stack_vreg_slot_note(ir, (int)ls_it->vreg, slot_idx);
  }
  stack_vreg_slots_finish(layout);
}

const TCCStackSlot *tcc_ir_stack_slot_by_vreg(const TCCIRState *ir, int vreg)
{
  if (!ir || !tcc_ir_vreg_is_valid((TCCIRState *)ir, vreg))
    return NULL;
  if (!tcc_ir_get_live_interval((TCCIRState *)ir, vreg))
    return NULL;
  const TCCStackLayout *layout = &ir->stack_layout;
  int lo = 0, hi = layout->vreg_slot_count - 1;
  while (lo <= hi)
  {
    const int mid = (lo + hi) >> 1;
    const TCCStackVregSlot *e = &layout->vreg_slots[mid];
    if (e->vreg == vreg)
      return e->slot < layout->slot_count ? &layout->slots[e->slot] : NULL;
    if (e->vreg < vreg)
      lo = mid + 1;
    else
      hi = mid - 1;
  }
  return NULL;
}

const TCCStackSlot *tcc_ir_stack_slot_by_offset(const TCCIRState *ir, int frame_offset)
{
  if (!ir)
    return NULL;

  const int idx = tcc_ir_stack_layout_offset_hash_lookup_index(&ir->stack_layout, frame_offset);
  if (idx >= 0 && idx < ir->stack_layout.slot_count)
    return &ir->stack_layout.slots[idx];

  for (int i = 0; i < ir->stack_layout.slot_count; ++i)
  {
    if (ir->stack_layout.slots[i].offset == frame_offset)
      return &ir->stack_layout.slots[i];
  }
  return NULL;
}

const TCCStackSlot *tcc_ir_stack_slot_by_index(TCCIRState *ir, int idx)
{
  if (!ir || idx < 0 || idx >= ir->stack_layout.slot_count)
    return NULL;
  return &ir->stack_layout.slots[idx];
}

int tcc_ir_stack_slot_count(TCCIRState *ir)
{
  return ir ? ir->stack_layout.slot_count : 0;
}

/* ============================================================================
 * Physical Register Assignment
 * ============================================================================ */

void tcc_ir_stack_reg_assign(TCCIRState *ir, int vreg, int offset, int r0, int r1)
{
  IRLiveInterval *interval = tcc_ir_get_live_interval(ir, vreg);
  if (!interval)
    return;
  /* If variable is spilled (offset != 0), mark r0 with PREG_SPILLED flag */
  if (offset != 0)
  {
    const int is_64bit = interval->is_double || interval->is_llong;
    interval->allocation.r0 = PREG_SPILLED | PREG_REG_NONE;
    /* For 64-bit values, mark the high word as spilled too so codegen reloads it
     * instead of treating an uninitialized pr1 as a real register. */
    interval->allocation.r1 = is_64bit ? (PREG_SPILLED | PREG_REG_NONE) : PREG_NONE;
  }
  else
  {
    interval->allocation.r0 = r0;
    interval->allocation.r1 = r1;
  }
  interval->allocation.offset = offset;
}

void tcc_ir_stack_reg_get(TCCIRState *ir, int vreg, int *r0, int *r1)
{
  IRLiveInterval *interval = tcc_ir_try_get_live_interval(ir, vreg);
  if (!interval)
  {
    if (r0)
      *r0 = PREG_NONE;
    if (r1)
      *r1 = PREG_NONE;
    return;
  }
  if (r0)
    *r0 = interval->allocation.r0;
  if (r1)
    *r1 = interval->allocation.r1;
}

/* ============================================================================
 * Spill Cache Wrappers
 * ============================================================================
 * Note: The actual spill cache functions (tcc_ir_spill_cache_*)
 * are defined in arm-thumb-gen.c. These are IR-state wrappers.
 */

void tcc_ir_stack_spill_cache_clear(TCCIRState *ir)
{
  if (!ir)
    return;
  tcc_ir_spill_cache_clear(&ir->spill_cache);
}

void tcc_ir_stack_spill_cache_record(TCCIRState *ir, int reg, int offset)
{
  if (!ir)
    return;
  tcc_ir_spill_cache_record(&ir->spill_cache, reg, offset);
}

int tcc_ir_stack_spill_cache_lookup(TCCIRState *ir, int offset)
{
  if (!ir)
    return -1;
  return tcc_ir_spill_cache_lookup(&ir->spill_cache, offset);
}

void tcc_ir_stack_spill_cache_invalidate_reg(TCCIRState *ir, int reg)
{
  if (!ir)
    return;
  tcc_ir_spill_cache_invalidate_reg(&ir->spill_cache, reg);
}

void tcc_ir_stack_spill_cache_invalidate_offset(TCCIRState *ir, int offset)
{
  if (!ir)
    return;
  tcc_ir_spill_cache_invalidate_offset(&ir->spill_cache, offset);
}

/* ============================================================================
 * Stack Frame Information
 * ============================================================================ */

int tcc_ir_stack_frame_size(TCCIRState *ir)
{
  if (!ir)
    return 0;
  /* Calculate total frame size from slots */
  int max_offset = 0;
  for (int i = 0; i < ir->stack_layout.slot_count; ++i)
  {
    const int end = ir->stack_layout.slots[i].offset + ir->stack_layout.slots[i].size;
    if (end > max_offset)
      max_offset = end;
  }
  return max_offset;
}

int tcc_ir_stack_alignment(TCCIRState *ir)
{
  (void)ir;
  return 8;
}

int tcc_ir_stack_args_offset(TCCIRState *ir)
{
  return ir ? ir->call_outgoing_base : 0;
}

int tcc_ir_stack_args_size(TCCIRState *ir)
{
  return ir ? ir->call_outgoing_size : 0;
}

void tcc_ir_stack_reset(TCCIRState *ir)
{
  if (!ir)
    return;
  ir->stack_layout.slot_count = 0;
}

/* ============================================================================
 * Legacy API Wrappers
 * ============================================================================ */

/* Build stack layout - legacy name */
void tcc_ir_build_stack_layout(TCCIRState *ir)
{
  tcc_ir_stack_build(ir);
}

/* Assign physical registers to vreg - legacy name */
void tcc_ir_assign_physical_register(TCCIRState *ir, int vreg, int offset, int r0, int r1)
{
  tcc_ir_stack_reg_assign(ir, vreg, offset, r0, r1);
}
