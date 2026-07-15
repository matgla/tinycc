/*
 *  ARM Thumb Call Site Management
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#pragma once

#include "memory/small_sequence.h"

enum
{
  THUMB_CALL_INLINE_ARGS = 16
};

TCC_SMALL_SEQUENCE_DEFINE(ThumbIROperandSequence, IROperand, THUMB_CALL_INLINE_ARGS)
TCC_SMALL_SEQUENCE_DEFINE(ThumbMachineOperandSequence, MachineOperand, THUMB_CALL_INLINE_ARGS)

ST_FUNC int thumb_build_call_layout_from_ir(TCCIRState *ir, int call_idx, int call_id, int argc_hint,
                                            TCCAbiCallLayout *layout, ThumbIROperandSequence *out_args,
                                            ThumbMachineOperandSequence *out_mops);

