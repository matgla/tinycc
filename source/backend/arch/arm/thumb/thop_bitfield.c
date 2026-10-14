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

#define V_BFX(b) {THOP_SHAPE_T32_BFX, (b)}
#define V_SSAT_LSL(b) {THOP_SHAPE_T32_SSAT_LSL, (b)}
#define V_SSAT_ASR(b) {THOP_SHAPE_T32_SSAT_ASR, (b)}
#define V_USAT_LSL(b) {THOP_SHAPE_T32_USAT_LSL, (b)}
#define V_USAT_ASR(b) {THOP_SHAPE_T32_USAT_ASR, (b)}

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
  return thop_emit_table(&TH_BFC, (thop_args){.rd = rd, .rn = 0, .imm = lsb, .imm2 = lsb + width - 1});
}

thumb_opcode th_bfi(uint32_t rd, uint32_t rn, uint32_t lsb, uint32_t width)
{
  return thop_emit_table(&TH_BFI, (thop_args){.rd = rd, .rn = rn, .imm = lsb, .imm2 = lsb + width - 1});
}

thumb_opcode th_sbfx(uint32_t rd, uint32_t rn, uint32_t lsb, uint32_t width)
{
  return thop_emit_table(&TH_SBFX, (thop_args){.rd = rd, .rn = rn, .imm = lsb, .imm2 = width - 1});
}

thumb_opcode th_ssat(uint32_t rd, uint32_t imm, uint32_t rn, thumb_shift shift)
{
  return thop_emit_table(&TH_SSAT, (thop_args){.rd = rd, .rn = rn, .imm2 = imm - 1, .shift = shift});
}

thumb_opcode th_usat(uint32_t rd, uint32_t imm, uint32_t rn, thumb_shift shift)
{
  return thop_emit_table(&TH_USAT, (thop_args){.rd = rd, .rn = rn, .imm2 = imm, .shift = shift});
}
