/*
 *  TCC - Tiny C Compiler
 *
 *  Copyright (c) 2026 Mateusz Stadnik
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

#include "source/backend/arch/fpu/arm/rp2350-dcp.h"
#include "tccir.h"

/* RP2350 (Raspberry Pi) floating-point capability description.
 *
 * The part has two independent FP resources:
 *
 *   - an FPv5-SP FPU (CP10/CP11) for single precision, and
 *   - the DCP, Raspberry Pi's own double coprocessor on CP4.  The DCP is not
 *     an FPU: it supplies primitives that a short instruction sequence
 *     composes into an IEEE double operation, and it operates on *GPR pairs*
 *     via mcrr/mrrc rather than on any FP register file.
 *
 * Because the DCP works on GPRs, doubles keep the ordinary soft-float ABI and
 * the existing register allocator -- there is no double register class to add.
 *
 * These has_* bits gate ir_put_soft_call_fpu_if_needed() (ir/gen/softfloat.c):
 * a set bit means "the backend emits this inline", a clear bit means "rewrite
 * it into an __aeabi_* call".  They are therefore a description of what the
 * *backend* currently implements, not of what the silicon can do, and get
 * turned on as the inline lowering lands.
 *
 * A bit must be flipped in the SAME commit as its emitter in the backend:
 * ir_op_is_implicit_call_ra() (ir/regalloc.c) reads these same bits to decide
 * whether an operation still clobbers r0-r3 like a call.  Setting a bit while
 * the backend keeps emitting a BL therefore does not merely miss an
 * optimisation, it un-models a real clobber.
 *
 * Landed inline so far: dadd, dsub (thumb_emit_dcp_addsub_mop).  Everything
 * else still lowers to a librp2350fp call -- which is itself DCP-backed and
 * already a large win over soft float, just paying call overhead.
 */
const FloatingPointConfig arm_rp2350_dcp_fpu_config = {
    /* FPv5-SP: 32 single-precision registers, viewable as 16 doubles. */
    .reg_size = 8,
    .reg_count = 16,
    .stack_align = 8,

    /* Single precision -- FPv5-SP hardware. */
    .has_fadd = 1,
    .has_fsub = 1,
    .has_fmul = 1,
    .has_fdiv = 1,
    .has_fcmp = 0,
    .has_fneg = 0,
    .has_itof = 0,
    .has_ftoi = 0,
    .has_ftof = 0,

    /* Double precision -- DCP coprocessor sequences. */
    .double_impl = FP_DOUBLE_IMPL_DCP,
    .has_dadd = 1,
    .has_dsub = 1,
    .has_dcmp = 1,
    .has_dmul = 0,
    .has_dneg = 0,
    .has_itod = 0,
    .has_dtoi = 0,
    .has_dtof = 0,
    .has_ftod = 0,

    /* Never lowered inline: the DCP division and square-root sequences run to
     * ~35 instructions and need five scratch registers, so they stay library
     * calls even once the rest is inline. */
    .has_ddiv = 0,

    /* 64-bit integer <-> float conversions have no short DCP sequence. */
    .has_ltod = 0,
    .has_ltof = 0,
    .has_dtol = 0,
    .has_ftol = 0,
};
