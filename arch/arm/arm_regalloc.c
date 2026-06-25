/*
 *  TCC - Tiny C Compiler
 *
 *  ARM register set definitions for SSA register allocator.
 *
 *  Copyright (c) 2025 Mateusz Stadnik
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

#include "arm_regalloc.h"

/* AAPCS: R0-R3 caller-saved, R4-R11 callee-saved, R12(IP) caller-saved */
static const int arm_caller_saved[] = {0, 1, 2, 3, 12};
static const int arm_callee_saved[] = {4, 5, 6, 7, 8, 9, 10, 11};

/* VFP: S0-S15 caller-saved, S16-S31 callee-saved */
static const int arm_fp_caller_saved[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
static const int arm_fp_callee_saved[] = {16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31};

static const RegAllocTarget arm_target = {
    .int_class =
        {
            .num_regs = 13, /* R0-R12 */
            .caller_saved = arm_caller_saved,
            .num_caller_saved = 5,
            .callee_saved = arm_callee_saved,
            .num_callee_saved = 8,
            .pair_align = 1, /* even-aligned pairs for 64-bit */
        },
    .fp_class =
        {
            .num_regs = 32, /* S0-S31 */
            .caller_saved = arm_fp_caller_saved,
            .num_caller_saved = 16,
            .callee_saved = arm_fp_callee_saved,
            .num_callee_saved = 16,
            .pair_align = 1, /* even-aligned for double */
        },
    .param_regs = 4,         /* R0-R3 */
    .static_chain_reg = 10,  /* R10 */
};

const RegAllocTarget *arm_get_regalloc_target(void)
{
  return &arm_target;
}
