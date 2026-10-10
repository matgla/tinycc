/*
 *  TCC IR - Virtual Register Management Implementation
 *
 *  Copyright (c) 2025 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#define USING_GLOBALS
#include "ir.h"

/* ============================================================================
 * Virtual Register Validation
 * ============================================================================ */

/* Check if vreg is valid */
int tcc_ir_vreg_is_valid(TCCIRState *ir, int vr)
{
  const int type = TCCIR_DECODE_VREG_TYPE(vr);
  const int position = TCCIR_DECODE_VREG_POSITION(vr);
  switch (type)
  {
  case TCCIR_VREG_TYPE_VAR:
    return position < ir->variables_live_intervals_size;
  case TCCIR_VREG_TYPE_TEMP:
    return position < ir->temporary_variables_live_intervals_size;
  case TCCIR_VREG_TYPE_PARAM:
    return position < ir->parameters_live_intervals_size;
  default:
    return 0;
  }
}

/* Check if vreg should be ignored for spilling */
int tcc_ir_vreg_is_ignored(TCCIRState *ir, int vreg)
{
#define IGNORED_VREG_BITS_PER_ENTRY 3
#define IGNORED_VREG_LOCAL_VAR_BIT 0
#define IGNORED_VREG_TEMP_BIT 1
#define IGNORED_VREG_PARAM_BIT 2

  const int position = TCCIR_DECODE_VREG_POSITION(vreg);
  const int type = TCCIR_DECODE_VREG_TYPE(vreg);

  int type_bit;
  switch (type)
  {
  case TCCIR_VREG_TYPE_VAR:
    type_bit = IGNORED_VREG_LOCAL_VAR_BIT;
    break;
  case TCCIR_VREG_TYPE_TEMP:
    type_bit = IGNORED_VREG_TEMP_BIT;
    break;
  case TCCIR_VREG_TYPE_PARAM:
    type_bit = IGNORED_VREG_PARAM_BIT;
    break;
  default:
    return 0;
  }

  const int bit_offset = position * IGNORED_VREG_BITS_PER_ENTRY + type_bit;
  const int index = bit_offset / 32;
  const int bit = bit_offset % 32;

  if (ir->ignored_vregs == NULL || index >= ir->ignored_vregs_size)
    return 0;

  return (ir->ignored_vregs[index] & (1 << bit)) != 0;
}

/* ============================================================================
 * Virtual Register Allocation
 * ============================================================================ */

/* Forward declaration for interval initialization */
static void ir_vreg_intervals_init(IRLiveInterval *intervals, int count);

/* Resize one interval array to exactly `cap` entries; a new tail is zeroed and
 * initialised.  The array moves: no caller may keep an IRLiveInterval pointer
 * across an allocation. */
static void ir_vreg_intervals_resize(IRLiveInterval **intervals, int *size, int cap)
{
  const int used = *size;
  *intervals = (IRLiveInterval *)tcc_realloc(*intervals, sizeof(IRLiveInterval) * cap);
  if (cap > used)
  {
    memset(&(*intervals)[used], 0, sizeof(IRLiveInterval) * (cap - used));
    ir_vreg_intervals_init(&(*intervals)[used], cap - used);
  }
  *size = cap;
}

/* Grow one interval array to hold at least `need` entries.  The arrays are
 * freed after every function, so the slack left in the largest one is what
 * reaches the peak.  CONFIG_TCC_LOW_MEM grows by 1.5x instead of doubling: a
 * few more reallocs on a large function for about a third less slack.  (The
 * instruction array and operand pool keep doubling: ra_resolve_phis replaces
 * the one and doubles the other itself, so a 1.5x step there only moved which
 * files got lucky -- measured worse over the corpus.) */
static void ir_vreg_intervals_grow(IRLiveInterval **intervals, int *size, int need)
{
  int cap = *size > 0 ? *size : 1;
  while (cap < need)
#ifdef CONFIG_TCC_LOW_MEM
    cap += cap > 1 ? cap >> 1 : 1;
#else
    cap <<= 1;
#endif
  ir_vreg_intervals_resize(intervals, size, cap);
}

/* Allocate a temporary virtual register */
int tcc_ir_vreg_alloc_temp(TCCIRState *ir)
{
  if (ir == NULL)
    return -1;

  if (ir->next_temporary_variable >= ir->temporary_variables_live_intervals_size)
    ir_vreg_intervals_grow(&ir->temporary_variables_live_intervals, &ir->temporary_variables_live_intervals_size,
                           ir->next_temporary_variable + 1);

  const int next_temp_vr = ir->next_temporary_variable;
  ++ir->next_temporary_variable;
  return TCCIR_ENCODE_VREG(TCCIR_VREG_TYPE_TEMP, next_temp_vr);
}

/* Allocate a variable virtual register */
int tcc_ir_vreg_alloc_var(TCCIRState *ir)
{
  if (ir == NULL)
    return -1;

  if (ir->next_local_variable >= ir->variables_live_intervals_size)
    ir_vreg_intervals_grow(&ir->variables_live_intervals, &ir->variables_live_intervals_size,
                           ir->next_local_variable + 1);

  const int next_var_vr = ir->next_local_variable;
  ++ir->next_local_variable;
  return TCCIR_ENCODE_VREG(TCCIR_VREG_TYPE_VAR, next_var_vr);
}

/* Allocate a parameter virtual register */
int tcc_ir_vreg_alloc_param(TCCIRState *ir)
{
  if (ir->next_parameter >= ir->parameters_live_intervals_size)
    ir_vreg_intervals_grow(&ir->parameters_live_intervals, &ir->parameters_live_intervals_size,
                           ir->next_parameter + 1);

  const int next_param_vr = ir->next_parameter;
  ++ir->next_parameter;
  return TCCIR_ENCODE_VREG(TCCIR_VREG_TYPE_PARAM, next_param_vr);
}

/* Allocate a static chain virtual register for nested functions.
 * This allocates a variable vreg (not a parameter) to model the static chain
 * register (R10 on ARM). The chain vreg is used for liveness tracking and
 * ensuring R10 is preserved, but it doesn't consume a parameter slot.
 *
 * The static chain is passed in R10 by the parent function (via SET_CHAIN),
 * and the nested function uses R10 directly when accessing captured variables.
 * The chain vreg ensures R10 is treated as live-in and preserved if modified.
 */
int tcc_ir_vreg_alloc_static_chain(TCCIRState *ir)
{
  /* Allocate as a variable vreg (not parameter) to avoid shifting parameter indices */
  int vreg = tcc_ir_vreg_alloc_var(ir);

  /* Set the incoming register to the static chain register (R10) */
  IRLiveInterval *interval = tcc_ir_vreg_live_interval(ir, vreg);
  if (interval)
  {
    /* R10 is the static chain register on ARM */
    interval->incoming_reg0 = 10; /* R10 */
    interval->incoming_reg1 = -1; /* Not a 64-bit value */
    /* Mark as live from instruction 0 */
    interval->start = 0;
    /* End will be set to last instruction during liveness analysis */
  }

  return vreg;
}

/* Initialize interval start fields */
static void ir_vreg_intervals_init(IRLiveInterval *intervals, int count)
{
  for (int i = 0; i < count; ++i)
  {
    intervals[i].start = INTERVAL_NOT_STARTED;
    intervals[i].incoming_reg0 = -1;
    intervals[i].incoming_reg1 = -1;
    intervals[i].stack_slot_index = -1;
    intervals[i].allocation.r0 = PREG_NONE;
    intervals[i].allocation.r1 = PREG_NONE;
    intervals[i].allocation.offset = 0;
  }
}

/* Resize an interval array holding `count` vregs when it is short of them or
 * carries more slack than `headroom`. */
static void ir_vreg_intervals_fit(IRLiveInterval **intervals, int *size, int count, int headroom)
{
  if (*size < count || *size > count + headroom)
    ir_vreg_intervals_resize(intervals, size, count + headroom > 0 ? count + headroom : 1);
}

/* Size the temp and var interval arrays once SSA renaming has numbered every
 * temp of the function (`temp_count`).  Besides growing the temp array, this
 * trims the slack that doubling during IR generation left in both (the
 * 262k-instruction function of 101_cleanup.c kept room for 131072 vars and
 * used 65543; 95_bitfields.c room for 4096 temps): they stay alive through
 * register allocation and code generation, where a function's memory peaks.  No var is allocated
 * after this point, and temps only by the phi resolver's cycle breakers and a
 * few post-RA rewrites (at most 32 in a function of the test corpus, none at
 * -O0), so the arrays keep that much headroom; CONFIG_TCC_LOW_MEM keeps just
 * enough for a few cycle breakers.
 * Trimming moves an array like growing does: this runs at the end of
 * tcc_ir_ssa_rename, where no IRLiveInterval pointer is held. */
void tcc_ir_vreg_fit_intervals(TCCIRState *ir, int temp_count)
{
#ifdef CONFIG_TCC_LOW_MEM
  /* A few temps for the phi resolver's cycle breakers, so one swap does not
   * cost a 1.5x step of the whole array.  No var slack: a var allocated later
   * grows the array, and the only lookups past next_local_variable were the
   * nested-function passes resolving a PARENT's captured vregs in this IR,
   * which use the bounds-safe tcc_ir_try_get_live_interval. */
  const int temp_headroom = 8, var_headroom = 0;
#else
  const int temp_headroom = (temp_count >> 3) + 32;
  const int var_headroom = (ir->next_local_variable >> 3) + 16;
#endif
  ir_vreg_intervals_fit(&ir->temporary_variables_live_intervals, &ir->temporary_variables_live_intervals_size,
                        temp_count, temp_headroom);
  ir_vreg_intervals_fit(&ir->variables_live_intervals, &ir->variables_live_intervals_size, ir->next_local_variable,
                        var_headroom);
}

/* ============================================================================
 * Live Interval Access
 * ============================================================================ */

/* Get live interval for vreg */
IRLiveInterval *tcc_ir_vreg_live_interval(TCCIRState *ir, int vreg)
{
  if (vreg < 0)
  {
    fprintf(stderr, "tcc_ir_vreg_live_interval: invalid vreg: %d\n", vreg);
    exit(1);
  }

  int decoded_vreg_position = TCCIR_DECODE_VREG_POSITION(vreg);
  switch (TCCIR_DECODE_VREG_TYPE(vreg))
  {
  case TCCIR_VREG_TYPE_VAR:
  {
    /* Interval array not allocated yet — this happens only for hand-built IR
       in unit tests that skip liveness setup; a real compile always has these
       sized before any query.  Report "no interval" (NULL) rather than abort,
       so callers that merely probe a property (e.g. addrtaken) degrade
       gracefully. */
    if (!ir->variables_live_intervals)
      return NULL;
    if (decoded_vreg_position >= ir->variables_live_intervals_size)
    {
      fprintf(stderr, "Getting out of bounds live interval for vreg %d\n", vreg);
      exit(1);
    }
    return &ir->variables_live_intervals[decoded_vreg_position];
  }
  case TCCIR_VREG_TYPE_TEMP:
  {
    if (!ir->temporary_variables_live_intervals)
      return NULL;
    if (decoded_vreg_position >= ir->temporary_variables_live_intervals_size)
    {
      fprintf(stderr, "Getting out of bounds live interval for vreg %d\n", vreg);
      exit(1);
    }
    return &ir->temporary_variables_live_intervals[decoded_vreg_position];
  }
  case TCCIR_VREG_TYPE_PARAM:
  {
    if (!ir->parameters_live_intervals)
      return NULL;
    if (decoded_vreg_position >= ir->parameters_live_intervals_size)
    {
      fprintf(stderr, "Getting out of bounds live interval for vreg %d\n", vreg);
      exit(1);
    }
    return &ir->parameters_live_intervals[decoded_vreg_position];
  }
  default:
    fprintf(stderr, "tcc_ir_vreg_live_interval: unknown vreg type %d, for vreg: %d\n", TCCIR_DECODE_VREG_TYPE(vreg),
            vreg);
    exit(1);
  }
  return NULL;
}

/* ============================================================================
 * Type Setting
 * ============================================================================ */

/* Mark vreg as address-taken */
void tcc_ir_vreg_flag_addrtaken_set(TCCIRState *ir, int vreg)
{
  if (vreg < 0 || TCCIR_DECODE_VREG_TYPE(vreg) == 0)
    return;
  IRLiveInterval *interval = tcc_ir_vreg_live_interval(ir, vreg);
  if (interval)
    interval->addrtaken = 1;
}

/* 1 when vreg is flagged address-taken (by `&x`, or by a nested function
 * capturing it); 0 for no interval or a non-vreg. */
int tcc_ir_vreg_flag_addrtaken_get(TCCIRState *ir, int vreg)
{
  if (vreg < 0 || TCCIR_DECODE_VREG_TYPE(vreg) == 0)
    return 0;
  IRLiveInterval *interval = tcc_ir_vreg_live_interval(ir, vreg);
  return interval && interval->addrtaken;
}

/* Mark vreg as float/double type */
void tcc_ir_vreg_type_set_fp(TCCIRState *ir, int vreg, int is_float, int is_double)
{
  if (vreg < 0 || TCCIR_DECODE_VREG_TYPE(vreg) == 0)
    return;
  IRLiveInterval *interval = tcc_ir_vreg_live_interval(ir, vreg);
  if (interval)
  {
    interval->is_float = is_float;
    interval->is_double = is_double;
    interval->use_vfp = (tcc_state && tcc_state->float_abi == ARM_HARD_FLOAT);
  }
}

/* Mark vreg as 64-bit (long long) */
void tcc_ir_vreg_type_set_64bit(TCCIRState *ir, int vreg)
{
  if (vreg < 0 || TCCIR_DECODE_VREG_TYPE(vreg) == 0)
    return;
  IRLiveInterval *interval = tcc_ir_vreg_live_interval(ir, vreg);
  if (interval)
    interval->is_llong = 1;
}

/* Phase 3: Mark vreg as complex type */
void tcc_ir_vreg_type_set_complex(TCCIRState *ir, int vreg)
{
  if (vreg < 0 || TCCIR_DECODE_VREG_TYPE(vreg) == 0)
    return;
  IRLiveInterval *interval = tcc_ir_vreg_live_interval(ir, vreg);
  if (interval)
    interval->is_complex = 1;
}

/* Set original stack offset for vreg */
void tcc_ir_vreg_offset_set(TCCIRState *ir, int vreg, int offset)
{
  if (vreg < 0 || TCCIR_DECODE_VREG_TYPE(vreg) == 0)
    return;
  IRLiveInterval *interval = tcc_ir_vreg_live_interval(ir, vreg);
  if (interval)
  {
    interval->original_offset = offset;
  }
}

/* ============================================================================
 * Type Queries
 * ============================================================================ */

/* Get register type for vreg */
int tcc_ir_vreg_type_get(TCCIRState *ir, int vreg)
{
  if (vreg < 0 || TCCIR_DECODE_VREG_TYPE(vreg) == 0)
    return LS_REG_TYPE_INT;

  IRLiveInterval *interval = tcc_ir_vreg_live_interval(ir, vreg);
  if (interval)
  {
    if (interval->is_llong)
      return LS_REG_TYPE_LLONG;
    /* Phase 3: Complex types need register pairs like DOUBLE_SOFT */
    if (interval->is_complex)
      return interval->is_double ? LS_REG_TYPE_COMPLEX_DOUBLE : LS_REG_TYPE_COMPLEX_FLOAT;
    if (interval->is_float)
    {
      /* Only single-precision floats occupy the VFP register class.  Doubles
       * stay soft (GPR pairs + __aeabi_d*) even under hard-float — also required
       * on fpv5-sp-d16, whose FPU has no double-precision unit. */
      if (interval->is_double)
        return LS_REG_TYPE_DOUBLE_SOFT;
      return interval->use_vfp ? LS_REG_TYPE_FLOAT : LS_REG_TYPE_INT;
    }
  }
  return LS_REG_TYPE_INT;
}

/* Get string representation of vreg type */
const char *tcc_ir_vreg_type_string(int vreg)
{
  switch (TCCIR_DECODE_VREG_TYPE(vreg))
  {
  case TCCIR_VREG_TYPE_VAR:
    return "VAR";
  case TCCIR_VREG_TYPE_TEMP:
    return "TMP";
  case TCCIR_VREG_TYPE_PARAM:
    return "PAR";
  default:
    return "UNK";
  }
}

/* ============================================================================
 * Stack Slot Access
 * ============================================================================ */

/* Get stack slot index for vreg */
int tcc_ir_vreg_stack_slot_get(TCCIRState *ir, int vreg)
{
  if (!tcc_ir_vreg_is_valid(ir, vreg))
    return -1;
  IRLiveInterval *interval = tcc_ir_vreg_live_interval(ir, vreg);
  return interval ? interval->stack_slot_index : -1;
}

/* Set stack slot index for vreg */
void tcc_ir_vreg_stack_slot_set(TCCIRState *ir, int vreg, int slot_idx)
{
  if (!tcc_ir_vreg_is_valid(ir, vreg))
    return;
  IRLiveInterval *interval = tcc_ir_vreg_live_interval(ir, vreg);
  if (interval)
    interval->stack_slot_index = slot_idx;
}

/* Get frame offset for vreg */
int tcc_ir_vreg_frame_offset_get(TCCIRState *ir, int vreg)
{
  if (!tcc_ir_vreg_is_valid(ir, vreg))
    return 0;
  IRLiveInterval *interval = tcc_ir_vreg_live_interval(ir, vreg);
  return interval ? interval->allocation.offset : 0;
}

/* ============================================================================
 * Physical Register Access
 * ============================================================================ */

/* Get physical register for vreg */
int tcc_ir_vreg_preg_get(TCCIRState *ir, int vreg)
{
  if (!tcc_ir_vreg_is_valid(ir, vreg))
    return PREG_NONE;
  IRLiveInterval *interval = tcc_ir_vreg_live_interval(ir, vreg);
  return interval ? interval->allocation.r0 : PREG_NONE;
}

/* Set physical register for vreg */
void tcc_ir_vreg_preg_set(TCCIRState *ir, int vreg, int preg)
{
  if (!tcc_ir_vreg_is_valid(ir, vreg))
    return;
  IRLiveInterval *interval = tcc_ir_vreg_live_interval(ir, vreg);
  if (interval)
    interval->allocation.r0 = preg;
}

/* Get high physical register for vreg */
int tcc_ir_vreg_preg_hi_get(TCCIRState *ir, int vreg)
{
  if (!tcc_ir_vreg_is_valid(ir, vreg))
    return PREG_NONE;
  IRLiveInterval *interval = tcc_ir_vreg_live_interval(ir, vreg);
  return interval ? interval->allocation.r1 : PREG_NONE;
}

/* Set high physical register for vreg */
void tcc_ir_vreg_preg_hi_set(TCCIRState *ir, int vreg, int preg)
{
  if (!tcc_ir_vreg_is_valid(ir, vreg))
    return;
  IRLiveInterval *interval = tcc_ir_vreg_live_interval(ir, vreg);
  if (interval)
    interval->allocation.r1 = preg;
}

/* ============================================================================
 * Legacy API Wrappers (for compatibility during migration)
 * ============================================================================ */

/* Allocate temporary vreg - legacy name */
int tcc_ir_get_vreg_temp(TCCIRState *ir)
{
  return tcc_ir_vreg_alloc_temp(ir);
}

/* Allocate variable vreg - legacy name */
int tcc_ir_get_vreg_var(TCCIRState *ir)
{
  return tcc_ir_vreg_alloc_var(ir);
}

/* Allocate parameter vreg - legacy name */
int tcc_ir_get_vreg_param(TCCIRState *ir)
{
  return tcc_ir_vreg_alloc_param(ir);
}

/* Allocate static chain vreg - legacy name */
int tcc_ir_get_vreg_static_chain(TCCIRState *ir)
{
  return tcc_ir_vreg_alloc_static_chain(ir);
}

/* Mark vreg as address-taken - legacy name */
void tcc_ir_set_addrtaken(TCCIRState *ir, int vreg)
{
  tcc_ir_vreg_flag_addrtaken_set(ir, vreg);
}

/* Set float/double type for vreg - legacy name */
void tcc_ir_set_float_type(TCCIRState *ir, int vreg, int is_float, int is_double)
{
  tcc_ir_vreg_type_set_fp(ir, vreg, is_float, is_double);
}

/* Set 64-bit type for vreg - legacy name */
void tcc_ir_set_llong_type(TCCIRState *ir, int vreg)
{
  tcc_ir_vreg_type_set_64bit(ir, vreg);
}

/* Set original stack offset for vreg - legacy name */
void tcc_ir_set_original_offset(TCCIRState *ir, int vreg, int offset)
{
  tcc_ir_vreg_offset_set(ir, vreg, offset);
}

/* Get register type for vreg - legacy name */
int tcc_ir_get_reg_type(TCCIRState *ir, int vreg)
{
  return tcc_ir_vreg_type_get(ir, vreg);
}
