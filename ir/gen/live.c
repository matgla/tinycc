/*
 *  TCC IR - Live Interval Access
 *
 *  Copyright (c) 2025 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#define USING_GLOBALS
#include "ir.h"

IRLiveInterval *tcc_ir_get_live_interval(TCCIRState *ir, int vreg)
{
  if (vreg < 0)
  {
    fprintf(stderr, "tcc_ir_get_live_interval: invalid vreg: %d\n", vreg);
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
    fprintf(stderr, "Unknown vreg type %d for vreg %d\n", TCCIR_DECODE_VREG_TYPE(vreg), vreg);
    exit(1);
  }
  return NULL; /* unreachable, silences -Werror with old compiler */
}

/* Non-fatal sibling of tcc_ir_get_live_interval(): returns NULL instead of
 * aborting the process when vreg is negative, carries an unknown type, or
 * addresses a position past the allocated interval array.  Use this from
 * callers that must tolerate an unmapped vreg (e.g. tcc_ir_stack_reg_get). */
IRLiveInterval *tcc_ir_try_get_live_interval(TCCIRState *ir, int vreg)
{
  if (!ir || vreg < 0)
    return NULL;
  int decoded_vreg_position = TCCIR_DECODE_VREG_POSITION(vreg);
  switch (TCCIR_DECODE_VREG_TYPE(vreg))
  {
  case TCCIR_VREG_TYPE_VAR:
    if (decoded_vreg_position >= ir->variables_live_intervals_size)
      return NULL;
    return &ir->variables_live_intervals[decoded_vreg_position];
  case TCCIR_VREG_TYPE_TEMP:
    if (decoded_vreg_position >= ir->temporary_variables_live_intervals_size)
      return NULL;
    return &ir->temporary_variables_live_intervals[decoded_vreg_position];
  case TCCIR_VREG_TYPE_PARAM:
    if (decoded_vreg_position >= ir->parameters_live_intervals_size)
      return NULL;
    return &ir->parameters_live_intervals[decoded_vreg_position];
  default:
    return NULL;
  }
}
