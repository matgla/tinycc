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

#include "thop_bitfield.h"

#define USING_GLOBALS
#include "tcc.h"

/* ═══════════════════════════════════════════════════════════════════
 *  Bitfield / saturation — shared 32-bit shapes
 *
 *  BFX instructions (bfc, bfi, sbfx) share a common skeleton:
 *    - rd in bits [11:8], rn in bits [19:16]
 *    - lsb is split into imm3[14:12] and imm2[7:6] by the engine
 *    - the 5-bit payload (msb / width-1 / sat_imm) is passed as imm2
 *
 *  SAT instructions (ssat, usat) reuse the shift_imm2/imm3 fields for
 *  the shift amount and have two variants (LSL / ASR) differing only
 *  in the base opcode (sh bit at position 21).
 * ═══════════════════════════════════════════════════════════════════ */

static const thop_variant_shape SHAPE_T32_BFX = {
    .size = THOP_VARIANT_T32,
    .rd_place = {8, 4},
    .rn_place = {16, 4},
    .rd_con = REG_NOT_PC,
    .rn_con = REG_ANY,
    .imm = {.kind = IMM_RAW, .width = 5},
    .split_imm2_place = {6, 2},
    .split_imm3_place = {12, 3},
    .imm2_place = {0, 5},
    .feat = {.t32 = 1, .bfx = 1},
};

/* SSAT with LSL (or no shift) — base has sh=0 */
static const thop_variant_shape SHAPE_T32_SSAT_LSL = {
    .size = THOP_VARIANT_T32,
    .rd_place = {8, 4},
    .rn_place = {16, 4},
    .rd_con = REG_NOT_PC,
    .shift_imm3_bits = {12, 3},
    .shift_imm2_bits = {6, 2},
    .shift_allowed = (1u << THUMB_SHIFT_NONE) | (1u << THUMB_SHIFT_LSL),
    .imm2_place = {0, 5},
    .feat = {.t32 = 1, .sat = 1},
};

/* SSAT with ASR — base has sh=1 */
static const thop_variant_shape SHAPE_T32_SSAT_ASR = {
    .size = THOP_VARIANT_T32,
    .rd_place = {8, 4},
    .rn_place = {16, 4},
    .rd_con = REG_NOT_PC,
    .shift_imm3_bits = {12, 3},
    .shift_imm2_bits = {6, 2},
    .shift_allowed = (1u << THUMB_SHIFT_ASR),
    .imm2_place = {0, 5},
    .feat = {.t32 = 1, .sat = 1},
};

/* USAT with LSL (or no shift) — base has sh=0 */
static const thop_variant_shape SHAPE_T32_USAT_LSL = {
    .size = THOP_VARIANT_T32,
    .rd_place = {8, 4},
    .rn_place = {16, 4},
    .rd_con = REG_NOT_PC,
    .shift_imm3_bits = {12, 3},
    .shift_imm2_bits = {6, 2},
    .shift_allowed = (1u << THUMB_SHIFT_NONE) | (1u << THUMB_SHIFT_LSL),
    .imm2_place = {0, 5},
    .feat = {.t32 = 1, .sat = 1},
};

/* USAT with ASR — base has sh=1 */
static const thop_variant_shape SHAPE_T32_USAT_ASR = {
    .size = THOP_VARIANT_T32,
    .rd_place = {8, 4},
    .rn_place = {16, 4},
    .rd_con = REG_NOT_PC,
    .shift_imm3_bits = {12, 3},
    .shift_imm2_bits = {6, 2},
    .shift_allowed = (1u << THUMB_SHIFT_ASR),
    .imm2_place = {0, 5},
    .feat = {.t32 = 1, .sat = 1},
};

#define V_BFX(b) {&SHAPE_T32_BFX, (b)}
#define V_SSAT_LSL(b) {&SHAPE_T32_SSAT_LSL, (b)}
#define V_SSAT_ASR(b) {&SHAPE_T32_SSAT_ASR, (b)}
#define V_USAT_LSL(b) {&SHAPE_T32_USAT_LSL, (b)}
#define V_USAT_ASR(b) {&SHAPE_T32_USAT_ASR, (b)}

/* ═══════════════════════════════════════════════════════════════════
 *  Instruction tables
 * ═══════════════════════════════════════════════════════════════════ */

TH_TABLE(TH_BFC, "bfc", V_BFX(0xf36f0000));
TH_TABLE(TH_BFI, "bfi", V_BFX(0xf3600000));
TH_TABLE(TH_SBFX, "sbfx", V_BFX(0xf3400000));
TH_TABLE(TH_SSAT, "ssat", V_SSAT_LSL(0xf3000000), V_SSAT_ASR(0xf3200000));
TH_TABLE(TH_USAT, "usat", V_USAT_LSL(0xf3800000), V_USAT_ASR(0xf3a00000));

/* ═══════════════════════════════════════════════════════════════════
 *  Emit wrappers
 * ═══════════════════════════════════════════════════════════════════ */

thumb_opcode th_bfc(uint32_t rd, uint32_t lsb, uint32_t width)
{
  return thop_emit(TH_BFC.name, TH_BFC.variants, TH_BFC.variant_count,
                   (thop_args){.rd = rd, .rn = 0, .imm = lsb, .imm2 = lsb + width - 1});
}

thumb_opcode th_bfi(uint32_t rd, uint32_t rn, uint32_t lsb, uint32_t width)
{
  return thop_emit(TH_BFI.name, TH_BFI.variants, TH_BFI.variant_count,
                   (thop_args){.rd = rd, .rn = rn, .imm = lsb, .imm2 = lsb + width - 1});
}

thumb_opcode th_sbfx(uint32_t rd, uint32_t rn, uint32_t lsb, uint32_t width)
{
  return thop_emit(TH_SBFX.name, TH_SBFX.variants, TH_SBFX.variant_count,
                   (thop_args){.rd = rd, .rn = rn, .imm = lsb, .imm2 = width - 1});
}

thumb_opcode th_ssat(uint32_t rd, uint32_t imm, uint32_t rn, thumb_shift shift)
{
  return thop_emit(TH_SSAT.name, TH_SSAT.variants, TH_SSAT.variant_count,
                   (thop_args){.rd = rd, .rn = rn, .imm2 = imm - 1, .shift = shift});
}

thumb_opcode th_usat(uint32_t rd, uint32_t imm, uint32_t rn, thumb_shift shift)
{
  return thop_emit(TH_USAT.name, TH_USAT.variants, TH_USAT.variant_count,
                   (thop_args){.rd = rd, .rn = rn, .imm2 = imm, .shift = shift});
}
