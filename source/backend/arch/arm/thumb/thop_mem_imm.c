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

#define USING_GLOBALS
#include "thop_mem_imm.h"
#include "tcc.h"

/* ═══════════════════════════════════════════════════════════════════
 *  Thumb load/store immediate-offset instructions
 * ═══════════════════════════════════════════════════════════════════ */

/* ───── T16 shapes ───── */

static const thop_variant_shape SHAPE_T16_MEM_IMM4 = {
    .size = THOP_VARIANT_T16,
    .rd_place = {0, 3}, .rn_place = {3, 3},
    .rd_con = REG_LOW_ONLY, .rn_con = REG_LOW_ONLY,
    .imm = {.kind = IMM_RAW, .width = 5, .scale_log2 = 2},
    .imm_place = {6, 5},
    .puw_fixed = 6,
    .feat = {.t16 = 1},
};

static const thop_variant_shape SHAPE_T16_MEM_IMM0 = {
    .size = THOP_VARIANT_T16,
    .rd_place = {0, 3}, .rn_place = {3, 3},
    .rd_con = REG_LOW_ONLY, .rn_con = REG_LOW_ONLY,
    .imm = {.kind = IMM_RAW, .width = 5, .scale_log2 = 0},
    .imm_place = {6, 5},
    .puw_fixed = 6,
    .feat = {.t16 = 1},
};

static const thop_variant_shape SHAPE_T16_MEM_IMM1 = {
    .size = THOP_VARIANT_T16,
    .rd_place = {0, 3}, .rn_place = {3, 3},
    .rd_con = REG_LOW_ONLY, .rn_con = REG_LOW_ONLY,
    .imm = {.kind = IMM_RAW, .width = 5, .scale_log2 = 1},
    .imm_place = {6, 5},
    .puw_fixed = 6,
    .feat = {.t16 = 1},
};

static const thop_variant_shape SHAPE_T16_MEM_SP_IMM4 = {
    .size = THOP_VARIANT_T16,
    .rd_place = {8, 3},
    .rd_con = REG_LOW_ONLY,
    .rn_con = REG_SP_ONLY, /* SP is implicit in encoding */
    .imm = {.kind = IMM_RAW, .width = 8, .scale_log2 = 2},
    .imm_place = {0, 8},
    .puw_fixed = 6,
    .feat = {.t16 = 1},
};

/* ───── T32 positive-offset shapes ───── */

static const thop_variant_shape SHAPE_T32_MEM_POS_ANY_NOTPC = {
    .size = THOP_VARIANT_T32,
    .rd_place = {12, 4}, .rn_place = {16, 4},
    .rd_con = REG_ANY, .rn_con = REG_NOT_PC,
    .imm = {.kind = IMM_RAW, .width = 12, .scale_log2 = 0},
    .imm_place = {0, 12},
    .puw_fixed = 6,
    .feat = {.t32 = 1},
};

static const thop_variant_shape SHAPE_T32_MEM_POS_NOSP_NOTPC = {
    .size = THOP_VARIANT_T32,
    .rd_place = {12, 4}, .rn_place = {16, 4},
    .rd_con = REG_NOT_SP, .rn_con = REG_NOT_PC,
    .imm = {.kind = IMM_RAW, .width = 12, .scale_log2 = 0},
    .imm_place = {0, 12},
    .puw_fixed = 6,
    .feat = {.t32 = 1},
};

static const thop_variant_shape SHAPE_T32_MEM_POS_NOSP_ANY = {
    .size = THOP_VARIANT_T32,
    .rd_place = {12, 4}, .rn_place = {16, 4},
    .rd_con = REG_NOT_SP, .rn_con = REG_ANY,
    .imm = {.kind = IMM_RAW, .width = 12, .scale_log2 = 0},
    .imm_place = {0, 12},
    .puw_fixed = 6,
    .feat = {.t32 = 1},
};

/* ───── T32 PC-relative shapes ───── */

static const thop_variant_shape SHAPE_T32_MEM_PC_POS = {
    .size = THOP_VARIANT_T32,
    .rd_place = {12, 4}, .rn_place = {16, 4},
    .rd_con = REG_ANY, .rn_con = REG_PC_ONLY,
    .imm = {.kind = IMM_RAW, .width = 12, .scale_log2 = 0},
    .imm_place = {0, 12},
    .puw_fixed = 6,
    .feat = {.t32 = 1},
};

static const thop_variant_shape SHAPE_T32_MEM_PC_NEG = {
    .size = THOP_VARIANT_T32,
    .rd_place = {12, 4}, .rn_place = {16, 4},
    .rd_con = REG_ANY, .rn_con = REG_PC_ONLY,
    .imm = {.kind = IMM_RAW, .width = 12, .scale_log2 = 0},
    .imm_place = {0, 12},
    .puw_fixed = 4,
    .feat = {.t32 = 1},
};

/* ───── T32 indexed shapes (PUW in bits [10:8]) ───── */

static const thop_variant_shape SHAPE_T32_MEM_IDX_ANY = {
    .size = THOP_VARIANT_T32,
    .rd_place = {12, 4}, .rn_place = {16, 4},
    .rd_con = REG_ANY, .rn_con = REG_ANY,
    .imm = {.kind = IMM_RAW, .width = 8, .scale_log2 = 0},
    .imm_place = {0, 8},
    .puw_bits = {8, 3},
    .feat = {.t32 = 1},
};

static const thop_variant_shape SHAPE_T32_MEM_IDX_NOSP = {
    .size = THOP_VARIANT_T32,
    .rd_place = {12, 4}, .rn_place = {16, 4},
    .rd_con = REG_NOT_SP, .rn_con = REG_ANY,
    .imm = {.kind = IMM_RAW, .width = 8, .scale_log2 = 0},
    .imm_place = {0, 8},
    .puw_bits = {8, 3},
    .feat = {.t32 = 1},
};

/* ───── Tables ───── */

TH_TABLE(TH_LDR_IMM, "ldr",
    {&SHAPE_T16_MEM_IMM4, 0x6800, NULL},
    {&SHAPE_T16_MEM_SP_IMM4, 0x9800, NULL},
    {&SHAPE_T32_MEM_POS_ANY_NOTPC, 0xf8d00000, NULL},
    {&SHAPE_T32_MEM_PC_POS, 0xf8df0000, NULL},
    {&SHAPE_T32_MEM_PC_NEG, 0xf85f0000, NULL},
    {&SHAPE_T32_MEM_IDX_ANY, 0xf8500800, NULL});

TH_TABLE(TH_LDRB_IMM, "ldrb",
    {&SHAPE_T16_MEM_IMM0, 0x7800, NULL},
    {&SHAPE_T32_MEM_POS_NOSP_NOTPC, 0xf8900000, NULL},
    {&SHAPE_T32_MEM_PC_POS, 0xf89f0000, NULL},
    {&SHAPE_T32_MEM_PC_NEG, 0xf81f0000, NULL},
    {&SHAPE_T32_MEM_IDX_NOSP, 0xf8100800, NULL});

TH_TABLE(TH_LDRH_IMM, "ldrh",
    {&SHAPE_T16_MEM_IMM1, 0x8800, NULL},
    {&SHAPE_T32_MEM_POS_NOSP_NOTPC, 0xf8b00000, NULL},
    {&SHAPE_T32_MEM_PC_POS, 0xf8bf0000, NULL},
    {&SHAPE_T32_MEM_PC_NEG, 0xf83f0000, NULL},
    {&SHAPE_T32_MEM_IDX_NOSP, 0xf8300800, NULL});

TH_TABLE(TH_LDRSB_IMM, "ldrsb",
    {&SHAPE_T32_MEM_POS_NOSP_NOTPC, 0xf9900000, NULL},
    {&SHAPE_T32_MEM_PC_POS, 0xf99f0000, NULL},
    {&SHAPE_T32_MEM_PC_NEG, 0xf91f0000, NULL},
    {&SHAPE_T32_MEM_IDX_NOSP, 0xf9100800, NULL});

TH_TABLE(TH_LDRSH_IMM, "ldrsh",
    {&SHAPE_T32_MEM_POS_NOSP_NOTPC, 0xf9b00000, NULL},
    {&SHAPE_T32_MEM_PC_POS, 0xf9bf0000, NULL},
    {&SHAPE_T32_MEM_PC_NEG, 0xf93f0000, NULL},
    {&SHAPE_T32_MEM_IDX_NOSP, 0xf9300800, NULL});

TH_TABLE(TH_STR_IMM, "str",
    {&SHAPE_T16_MEM_IMM4, 0x6000, NULL},
    {&SHAPE_T16_MEM_SP_IMM4, 0x9000, NULL},
    {&SHAPE_T32_MEM_POS_ANY_NOTPC, 0xf8c00000, NULL},
    {&SHAPE_T32_MEM_PC_POS, 0xf8df0000, NULL},
    {&SHAPE_T32_MEM_PC_NEG, 0xf85f0000, NULL},
    {&SHAPE_T32_MEM_IDX_ANY, 0xf8400800, NULL});

TH_TABLE(TH_STRB_IMM, "strb",
    {&SHAPE_T16_MEM_IMM0, 0x7000, NULL},
    {&SHAPE_T32_MEM_POS_NOSP_ANY, 0xf8800000, NULL},
    {&SHAPE_T32_MEM_IDX_NOSP, 0xf8000800, NULL});

TH_TABLE(TH_STRH_IMM, "strh",
    {&SHAPE_T16_MEM_IMM1, 0x8000, NULL},
    {&SHAPE_T32_MEM_POS_NOSP_ANY, 0xf8a00000, NULL},
    {&SHAPE_T32_MEM_IDX_NOSP, 0xf8200800, NULL});

/* ───── Emit wrappers ───── */

thumb_opcode th_ldr_imm(uint32_t rt, uint32_t rn, int imm, uint32_t puw, thumb_enforce_encoding enc)
{
    return thop_emit(TH_LDR_IMM.name, TH_LDR_IMM.variants, TH_LDR_IMM.variant_count,
                     (thop_args){.rd = rt, .rn = rn, .imm = (uint32_t)imm, .puw = (uint8_t)puw, .enc = enc});
}

thumb_opcode th_ldrb_imm(uint32_t rt, uint32_t rn, int imm, uint32_t puw, thumb_enforce_encoding enc)
{
    return thop_emit(TH_LDRB_IMM.name, TH_LDRB_IMM.variants, TH_LDRB_IMM.variant_count,
                     (thop_args){.rd = rt, .rn = rn, .imm = (uint32_t)imm, .puw = (uint8_t)puw, .enc = enc});
}

thumb_opcode th_ldrh_imm(uint32_t rt, uint32_t rn, int imm, uint32_t puw, thumb_enforce_encoding enc)
{
    return thop_emit(TH_LDRH_IMM.name, TH_LDRH_IMM.variants, TH_LDRH_IMM.variant_count,
                     (thop_args){.rd = rt, .rn = rn, .imm = (uint32_t)imm, .puw = (uint8_t)puw, .enc = enc});
}

thumb_opcode th_ldrsb_imm(uint32_t rt, uint32_t rn, int imm, uint32_t puw, thumb_enforce_encoding enc)
{
    return thop_emit(TH_LDRSB_IMM.name, TH_LDRSB_IMM.variants, TH_LDRSB_IMM.variant_count,
                     (thop_args){.rd = rt, .rn = rn, .imm = (uint32_t)imm, .puw = (uint8_t)puw, .enc = enc});
}

thumb_opcode th_ldrsh_imm(uint32_t rt, uint32_t rn, int imm, uint32_t puw, thumb_enforce_encoding enc)
{
    return thop_emit(TH_LDRSH_IMM.name, TH_LDRSH_IMM.variants, TH_LDRSH_IMM.variant_count,
                     (thop_args){.rd = rt, .rn = rn, .imm = (uint32_t)imm, .puw = (uint8_t)puw, .enc = enc});
}

thumb_opcode th_str_imm(uint32_t rt, uint32_t rn, int imm, uint32_t puw, thumb_enforce_encoding enc)
{
    return thop_emit(TH_STR_IMM.name, TH_STR_IMM.variants, TH_STR_IMM.variant_count,
                     (thop_args){.rd = rt, .rn = rn, .imm = (uint32_t)imm, .puw = (uint8_t)puw, .enc = enc});
}

thumb_opcode th_strb_imm(uint32_t rt, uint32_t rn, int imm, uint32_t puw, thumb_enforce_encoding enc)
{
    return thop_emit(TH_STRB_IMM.name, TH_STRB_IMM.variants, TH_STRB_IMM.variant_count,
                     (thop_args){.rd = rt, .rn = rn, .imm = (uint32_t)imm, .puw = (uint8_t)puw, .enc = enc});
}

thumb_opcode th_strh_imm(uint32_t rt, uint32_t rn, int imm, uint32_t puw, thumb_enforce_encoding enc)
{
    return thop_emit(TH_STRH_IMM.name, TH_STRH_IMM.variants, TH_STRH_IMM.variant_count,
                     (thop_args){.rd = rt, .rn = rn, .imm = (uint32_t)imm, .puw = (uint8_t)puw, .enc = enc});
}
