/*
 *  TCC IR - Stack-slot aliasing helpers (pre-SSA optimization)
 *
 *  Copyright (c) 2025 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#ifndef TCC_IR_OPT_ALIAS_H
#define TCC_IR_OPT_ALIAS_H

#include <stdint.h>

struct TCCIRState;
struct IROperand;

int ir_opt_store_btype_size_bytes(int btype);

int ir_opt_stack_slot_range_for_offset(const struct TCCIRState *ir,
                                       int64_t frame_offset,
                                       int64_t *base_out, int64_t *end_out);

int stackoff_same_slot(IROperand a, IROperand b);

int operand_references_slot(IROperand op, IROperand slot);

int is_stack_address_operand(IROperand op);

int find_deref_use_operand(struct TCCIRState *ir, int consumer_idx,
                           int32_t vreg, int *which_out);

#endif /* TCC_IR_OPT_ALIAS_H */
