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

#include "thop_adr.h"
#include "thumb.h"

/* ═══════════════════════════════════════════════════════════════════
 *  ADR — address to register
 * ═══════════════════════════════════════════════════════════════════ */

/* T1: ADR <Rd>, #<imm8*4>  —  rd low reg, imm scaled by 4, positive */
static const thop_variant_shape SHAPE_ADR_T1 = {
    .size = THOP_VARIANT_T16,
    .rd_place = {8, 3},
    .rd_con = REG_LOW_ONLY,
    .imm = {.kind = IMM_RAW, .width = 8, .scale_log2 = 2},
    .imm_place = {0, 8},
    .feat = {.t16 = 1},
};

/* T3: ADR <Rd>, #<imm12>  —  positive, plain 12-bit */
static const thop_variant_shape SHAPE_ADR_T3 = {
    .size = THOP_VARIANT_T32,
    .rd_place = {8, 4},
    .rd_con = REG_NOT_PC,
    .imm = {.kind = IMM_PACK_3_8_1, .width = 12},
    .feat = {.t32 = 1},
};

/* T4: ADR <Rd>, #-<imm12>  —  negative offset */
static const thop_variant_shape SHAPE_ADR_T4 = {
    .size = THOP_VARIANT_T32,
    .rd_place = {8, 4},
    .rd_con = REG_NOT_PC,
    .imm = {.kind = IMM_PACK_3_8_1, .width = 12, .is_signed = true},
    .feat = {.t32 = 1},
};

TH_TABLE(TH_ADR_IMM, "adr", {&SHAPE_ADR_T1, 0xa000, NULL}, {&SHAPE_ADR_T3, 0xf20f0000, NULL},
         {&SHAPE_ADR_T4, 0xf2af0000, NULL});

/* ═══════════════════════════════════════════════════════════════════
 *  Public wrappers
 * ═══════════════════════════════════════════════════════════════════ */

thumb_opcode th_adr_imm(uint32_t rd, int imm, thumb_enforce_encoding encoding)
{
  return thop_emit(TH_ADR_IMM.name, TH_ADR_IMM.variants, TH_ADR_IMM.variant_count,
                   (thop_args){.rd = rd, .imm = (uint32_t)imm, .enc = encoding});
}
