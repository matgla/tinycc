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

/* NOTE ON THESE BITS.  They describe what the *backend emits inline*, not what
 * the silicon can do.  ir_op_is_implicit_call_ra() (ir/regalloc.c) reads the
 * same bits to decide whether an operation still clobbers r0-r3 like a call, so
 * a bit set without a matching emitter does not merely miss an optimisation --
 * ir_put_soft_call_fpu_if_needed() stops rewriting the op into a call, the
 * backend emits a BL anyway, and the allocator no longer models the clobber.
 *
 * This table used to claim every operation.  It was harmless only for as long
 * as the header declared `const FloatingPointConfig x;` without `extern`, which
 * made it a tentative definition every includer replaced with a zero-filled
 * copy; fixing that (Phase 2 of docs/plan_rp2350_dcp.md) made the claims live
 * and the clobbers un-modelled.  Only the four arithmetic ops have emitters
 * (thumb_emit_vfp_arith_mop), so only those are set.  Turn the rest back on as
 * their lowering lands -- in the same commit, never before.
 */
const FloatingPointConfig arm_fpv5_d16_fpu_config = {
    .reg_size = 8,
    .reg_count = 16,
    .stack_align = 8,
    .has_fadd = 1,
    .has_fsub = 1,
    .has_fmul = 1,
    .has_fdiv = 1,
    .has_fcmp = 0,
    .has_ftof = 0,
    .has_itof = 0,
    .has_ftod = 0,
    .has_ftoi = 0,
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
    .has_fneg = 0,
    .has_dneg = 0,
};
