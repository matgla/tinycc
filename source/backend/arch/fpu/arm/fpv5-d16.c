/*
 *  TCC - Tiny C Compiler
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

#include "tcc.h"

#include "source/backend/arch/fpu/arm/fpv5-d16.h"
#include "tccir.h"

const FloatingPointConfig arm_fpv5_d16_fpu_config = {
    .reg_size = 8,
    .reg_count = 16,
    .stack_align = 8,
    .has_fadd = 1,
    .has_fsub = 1,
    .has_fmul = 1,
    .has_fdiv = 1,
    .has_fcmp = 1,
    .has_ftof = 1,
    .has_itof = 1,
    .has_ftod = 1,
    .has_ftoi = 1,
    .has_dadd = 1,
    .has_dsub = 1,
    .has_dmul = 1,
    .has_ddiv = 1,
    .has_dcmp = 1,
    .has_dtof = 1,
    .has_itod = 1,
    .has_dtoi = 1,
    .has_ltod = 0,
    .has_ltof = 0,
    .has_dtol = 0,
    .has_ftol = 0,
    .has_fneg = 1,
    .has_dneg = 1,
};
