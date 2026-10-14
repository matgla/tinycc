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

/* ───── T32 positive-offset shapes ───── */

/* ───── T32 PC-relative shapes ───── */

/* ───── T32 indexed shapes (PUW in bits [10:8]) ───── */

/* ───── Tables ───── */

TH_TABLE(TH_LDR_IMM, "ldr",
    {THOP_SHAPE_T16_MEM_IMM4, 0x6800, 0},
    {THOP_SHAPE_T16_MEM_SP_IMM4, 0x9800, 0},
    {THOP_SHAPE_T32_MEM_POS_ANY_NOTPC, 0xf8d00000, 0},
    {THOP_SHAPE_T32_MEM_PC_POS, 0xf8df0000, 0},
    {THOP_SHAPE_T32_MEM_PC_NEG, 0xf85f0000, 0},
    {THOP_SHAPE_T32_MEM_IDX_ANY, 0xf8500800, 0});

TH_TABLE(TH_LDRB_IMM, "ldrb",
    {THOP_SHAPE_T16_MEM_IMM0, 0x7800, 0},
    {THOP_SHAPE_T32_MEM_POS_NOSP_NOTPC, 0xf8900000, 0},
    {THOP_SHAPE_T32_MEM_PC_POS, 0xf89f0000, 0},
    {THOP_SHAPE_T32_MEM_PC_NEG, 0xf81f0000, 0},
    {THOP_SHAPE_T32_MEM_IDX_NOSP, 0xf8100800, 0});

TH_TABLE(TH_LDRH_IMM, "ldrh",
    {THOP_SHAPE_T16_MEM_IMM1, 0x8800, 0},
    {THOP_SHAPE_T32_MEM_POS_NOSP_NOTPC, 0xf8b00000, 0},
    {THOP_SHAPE_T32_MEM_PC_POS, 0xf8bf0000, 0},
    {THOP_SHAPE_T32_MEM_PC_NEG, 0xf83f0000, 0},
    {THOP_SHAPE_T32_MEM_IDX_NOSP, 0xf8300800, 0});

TH_TABLE(TH_LDRSB_IMM, "ldrsb",
    {THOP_SHAPE_T32_MEM_POS_NOSP_NOTPC, 0xf9900000, 0},
    {THOP_SHAPE_T32_MEM_PC_POS, 0xf99f0000, 0},
    {THOP_SHAPE_T32_MEM_PC_NEG, 0xf91f0000, 0},
    {THOP_SHAPE_T32_MEM_IDX_NOSP, 0xf9100800, 0});

TH_TABLE(TH_LDRSH_IMM, "ldrsh",
    {THOP_SHAPE_T32_MEM_POS_NOSP_NOTPC, 0xf9b00000, 0},
    {THOP_SHAPE_T32_MEM_PC_POS, 0xf9bf0000, 0},
    {THOP_SHAPE_T32_MEM_PC_NEG, 0xf93f0000, 0},
    {THOP_SHAPE_T32_MEM_IDX_NOSP, 0xf9300800, 0});

TH_TABLE(TH_STR_IMM, "str",
    {THOP_SHAPE_T16_MEM_IMM4, 0x6000, 0},
    {THOP_SHAPE_T16_MEM_SP_IMM4, 0x9000, 0},
    {THOP_SHAPE_T32_MEM_POS_ANY_NOTPC, 0xf8c00000, 0},
    {THOP_SHAPE_T32_MEM_IDX_ANY_NOTPC, 0xf8400800, 0});

TH_TABLE(TH_STRB_IMM, "strb",
    {THOP_SHAPE_T16_MEM_IMM0, 0x7000, 0},
    {THOP_SHAPE_T32_MEM_POS_NOSP_NOTPC, 0xf8800000, 0},
    {THOP_SHAPE_T32_MEM_IDX_NOSP_NOTPC, 0xf8000800, 0});

TH_TABLE(TH_STRH_IMM, "strh",
    {THOP_SHAPE_T16_MEM_IMM1, 0x8000, 0},
    {THOP_SHAPE_T32_MEM_POS_NOSP_NOTPC, 0xf8a00000, 0},
    {THOP_SHAPE_T32_MEM_IDX_NOSP_NOTPC, 0xf8200800, 0});

/* ───── Emit wrappers ───── */

thumb_opcode th_ldr_imm(uint32_t rt, uint32_t rn, int imm, uint32_t puw, thumb_enforce_encoding enc)
{
    return thop_emit_table(&TH_LDR_IMM, (thop_args){.rd = rt, .rn = rn, .imm = (uint32_t)imm, .puw = (uint8_t)puw, .enc = enc});
}

thumb_opcode th_ldrb_imm(uint32_t rt, uint32_t rn, int imm, uint32_t puw, thumb_enforce_encoding enc)
{
    return thop_emit_table(&TH_LDRB_IMM, (thop_args){.rd = rt, .rn = rn, .imm = (uint32_t)imm, .puw = (uint8_t)puw, .enc = enc});
}

thumb_opcode th_ldrh_imm(uint32_t rt, uint32_t rn, int imm, uint32_t puw, thumb_enforce_encoding enc)
{
    return thop_emit_table(&TH_LDRH_IMM, (thop_args){.rd = rt, .rn = rn, .imm = (uint32_t)imm, .puw = (uint8_t)puw, .enc = enc});
}

thumb_opcode th_ldrsb_imm(uint32_t rt, uint32_t rn, int imm, uint32_t puw, thumb_enforce_encoding enc)
{
    return thop_emit_table(&TH_LDRSB_IMM, (thop_args){.rd = rt, .rn = rn, .imm = (uint32_t)imm, .puw = (uint8_t)puw, .enc = enc});
}

thumb_opcode th_ldrsh_imm(uint32_t rt, uint32_t rn, int imm, uint32_t puw, thumb_enforce_encoding enc)
{
    return thop_emit_table(&TH_LDRSH_IMM, (thop_args){.rd = rt, .rn = rn, .imm = (uint32_t)imm, .puw = (uint8_t)puw, .enc = enc});
}

thumb_opcode th_str_imm(uint32_t rt, uint32_t rn, int imm, uint32_t puw, thumb_enforce_encoding enc)
{
    return thop_emit_table(&TH_STR_IMM, (thop_args){.rd = rt, .rn = rn, .imm = (uint32_t)imm, .puw = (uint8_t)puw, .enc = enc});
}

thumb_opcode th_strb_imm(uint32_t rt, uint32_t rn, int imm, uint32_t puw, thumb_enforce_encoding enc)
{
    return thop_emit_table(&TH_STRB_IMM, (thop_args){.rd = rt, .rn = rn, .imm = (uint32_t)imm, .puw = (uint8_t)puw, .enc = enc});
}

thumb_opcode th_strh_imm(uint32_t rt, uint32_t rn, int imm, uint32_t puw, thumb_enforce_encoding enc)
{
    return thop_emit_table(&TH_STRH_IMM, (thop_args){.rd = rt, .rn = rn, .imm = (uint32_t)imm, .puw = (uint8_t)puw, .enc = enc});
}
