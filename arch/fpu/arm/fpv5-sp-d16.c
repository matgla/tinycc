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

#include "arch/fpu/arm/fpv5-sp-d16.h"
#include "tccir.h"

const FloatingPointConfig arm_fpv5_sp_d16_fpu_config = {
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
    .has_ftod = 0,
    .has_ftoi = 1,
    .has_dadd = 0,
    .has_dsub = 0,
    .has_dmul = 0,
    .has_ddiv = 0,
    .has_dcmp = 0,
    .has_dtof = 0,
    .has_itod = 0,
    .has_dtoi = 0,
    .has_ltod = 0,
    .has_ltof = 0,
    .has_dtol = 0,
    .has_ftol = 0,
    .has_fneg = 1,
    .has_dneg = 0,
};