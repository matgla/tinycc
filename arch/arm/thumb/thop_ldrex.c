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

#include "thop_ldrex.h"
#include "thumb.h"

/* ═══════════════════════════════════════════════════════════════════
 *  Load/store exclusive
 * ═══════════════════════════════════════════════════════════════════ */

/* ───── LDREX (T32) ───── */

static const thop_variant_shape SHAPE_LDREX = {
    .size = THOP_VARIANT_T32,
    .rd_place = {12, 4},
    .rn_place = {16, 4},
    .imm = {.kind = IMM_RAW, .width = 8},
    .imm_place = {0, 8},
    .feat = {.t32 = 1},
};

TH_TABLE(TH_LDREX, "ldrex", {&SHAPE_LDREX, 0xe8500f00, NULL});

/* ───── STREX (T32) ───── */

static const thop_variant_shape SHAPE_STREX = {
    .size = THOP_VARIANT_T32,
    .rd_place = {8, 4},
    .rn_place = {12, 4},
    .rm_place = {16, 4},
    .imm = {.kind = IMM_RAW, .width = 8},
    .imm_place = {0, 8},
    .feat = {.t32 = 1},
};

TH_TABLE(TH_STREX, "strex", {&SHAPE_STREX, 0xe8400000, NULL});

/* ───── LDREXB / LDREXH (T32) ───── */

static const thop_variant_shape SHAPE_LDREXB = {
    .size = THOP_VARIANT_T32,
    .rd_place = {12, 4},
    .rn_place = {16, 4},
    .feat = {.t32 = 1},
};

TH_TABLE(TH_LDREXB, "ldrexb", {&SHAPE_LDREXB, 0xe8d00f4f, NULL});
TH_TABLE(TH_LDREXH, "ldrexh", {&SHAPE_LDREXB, 0xe8d00f5f, NULL});

/* ───── STREXB / STREXH (T32) ───── */

static const thop_variant_shape SHAPE_STREXB = {
    .size = THOP_VARIANT_T32,
    .rd_place = {0, 4},
    .rn_place = {12, 4},
    .rm_place = {16, 4},
    .feat = {.t32 = 1},
};

TH_TABLE(TH_STREXB, "strexb", {&SHAPE_STREXB, 0xe8c00f40, NULL});
TH_TABLE(TH_STREXH, "strexh", {&SHAPE_STREXB, 0xe8c00f50, NULL});

/* ═══════════════════════════════════════════════════════════════════
 *  Public wrappers
 * ═══════════════════════════════════════════════════════════════════ */

thumb_opcode th_ldrex(uint32_t rt, uint32_t rn, int imm)
{
  return thop_emit(TH_LDREX.name, TH_LDREX.variants, TH_LDREX.variant_count,
                   (thop_args){.rd = rt, .rn = rn, .imm = (uint32_t)(imm >> 2)});
}

thumb_opcode th_strex(uint32_t rd, uint32_t rt, uint32_t rn, int imm)
{
  return thop_emit(TH_STREX.name, TH_STREX.variants, TH_STREX.variant_count,
                   (thop_args){.rd = rd, .rn = rt, .rm = rn, .imm = (uint32_t)(imm >> 2)});
}

thumb_opcode th_ldrexb(uint32_t rt, uint32_t rn)
{
  return thop_emit(TH_LDREXB.name, TH_LDREXB.variants, TH_LDREXB.variant_count,
                   (thop_args){.rd = rt, .rn = rn});
}

thumb_opcode th_ldrexh(uint32_t rt, uint32_t rn)
{
  return thop_emit(TH_LDREXH.name, TH_LDREXH.variants, TH_LDREXH.variant_count,
                   (thop_args){.rd = rt, .rn = rn});
}

thumb_opcode th_strexb(uint32_t rd, uint32_t rt, uint32_t rn)
{
  return thop_emit(TH_STREXB.name, TH_STREXB.variants, TH_STREXB.variant_count,
                   (thop_args){.rd = rd, .rn = rt, .rm = rn});
}

thumb_opcode th_strexh(uint32_t rd, uint32_t rt, uint32_t rn)
{
  return thop_emit(TH_STREXH.name, TH_STREXH.variants, TH_STREXH.variant_count,
                   (thop_args){.rd = rd, .rn = rt, .rm = rn});
}
