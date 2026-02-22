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

/* Allocate a temporary virtual register */
int tcc_ir_vreg_alloc_temp(TCCIRState *ir)
{
  if (ir == NULL)
    return -1;
    
  if (ir->next_temporary_variable >= ir->temporary_variables_live_intervals_size)
  {
    const int used = ir->temporary_variables_live_intervals_size;
    ir->temporary_variables_live_intervals_size <<= 1;
    ir->temporary_variables_live_intervals = (IRLiveInterval *)tcc_realloc(
        ir->temporary_variables_live_intervals, 
        sizeof(IRLiveInterval) * ir->temporary_variables_live_intervals_size);
    memset(&ir->temporary_variables_live_intervals[used], 0,
           sizeof(IRLiveInterval) * (ir->temporary_variables_live_intervals_size - used));
    ir_vreg_intervals_init(&ir->temporary_variables_live_intervals[used],
                           ir->temporary_variables_live_intervals_size - used);
  }
  
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
  {
    const int used = ir->variables_live_intervals_size;
    ir->variables_live_intervals_size <<= 1;
    ir->variables_live_intervals = (IRLiveInterval *)tcc_realloc(
        ir->variables_live_intervals, 
        sizeof(IRLiveInterval) * ir->variables_live_intervals_size);
    memset(&ir->variables_live_intervals[used], 0, 
           sizeof(IRLiveInterval) * (ir->variables_live_intervals_size - used));
    ir_vreg_intervals_init(&ir->variables_live_intervals[used], 
                           ir->variables_live_intervals_size - used);
  }
  
  const int next_var_vr = ir->next_local_variable;
  ++ir->next_local_variable;
  return TCCIR_ENCODE_VREG(TCCIR_VREG_TYPE_VAR, next_var_vr);
}

/* Allocate a parameter virtual register */
int tcc_ir_vreg_alloc_param(TCCIRState *ir)
{
  if (ir->next_parameter >= ir->parameters_live_intervals_size)
  {
    const int used = ir->parameters_live_intervals_size;
    ir->parameters_live_intervals_size <<= 1;
    ir->parameters_live_intervals = (IRLiveInterval *)tcc_realloc(
        ir->parameters_live_intervals, 
        sizeof(IRLiveInterval) * ir->parameters_live_intervals_size);
    memset(&ir->parameters_live_intervals[used], 0,
           sizeof(IRLiveInterval) * (ir->parameters_live_intervals_size - used));
    ir_vreg_intervals_init(&ir->parameters_live_intervals[used], 
                           ir->parameters_live_intervals_size - used);
  }
  
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
    interval->incoming_reg0 = 10;  /* R10 */
    interval->incoming_reg1 = -1;  /* Not a 64-bit value */
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
      if (decoded_vreg_position >= ir->variables_live_intervals_size)
      {
        fprintf(stderr, "Getting out of bounds live interval for vreg %d\n", vreg);
        exit(1);
      }
      return &ir->variables_live_intervals[decoded_vreg_position];
    }
    case TCCIR_VREG_TYPE_TEMP:
    {
      if (decoded_vreg_position >= ir->temporary_variables_live_intervals_size)
      {
        fprintf(stderr, "Getting out of bounds live interval for vreg %d\n", vreg);
        exit(1);
      }
      return &ir->temporary_variables_live_intervals[decoded_vreg_position];
    }
    case TCCIR_VREG_TYPE_PARAM:
    {
      if (decoded_vreg_position >= ir->parameters_live_intervals_size)
      {
        fprintf(stderr, "Getting out of bounds live interval for vreg %d\n", vreg);
        exit(1);
      }
      return &ir->parameters_live_intervals[decoded_vreg_position];
    }
    default:
      fprintf(stderr, "tcc_ir_vreg_live_interval: unknown vreg type %d, for vreg: %d\n", 
              TCCIR_DECODE_VREG_TYPE(vreg), vreg);
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

/* Set original stack offset for vreg */
void tcc_ir_vreg_offset_set(TCCIRState *ir, int vreg, int offset)
{
  if (vreg < 0 || TCCIR_DECODE_VREG_TYPE(vreg) == 0)
    return;
  IRLiveInterval *interval = tcc_ir_vreg_live_interval(ir, vreg);
  if (interval)
    interval->original_offset = offset;
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
    if (interval->is_float)
    {
      if (interval->is_double)
        return interval->use_vfp ? LS_REG_TYPE_DOUBLE : LS_REG_TYPE_DOUBLE_SOFT;
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
