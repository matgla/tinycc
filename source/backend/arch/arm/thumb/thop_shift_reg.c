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

#include "thop_shift_reg.h"

/* ═══════════════════════════════════════════════════════════════════
 *  Shift register — shared shapes
 * ═══════════════════════════════════════════════════════════════════ */

/* T1: <OP> <Rdn>, <Rm>  —  16-bit, all low, rd==rn, no shift field */
static const thop_variant_shape SHAPE_T16_SHIFT_REG = {
    .size = THOP_VARIANT_T16,
    .rd_place = {0, 3},
    .rm_place = {3, 3},
    .rd_con = REG_LOW_ONLY | REG_EQ_RN,
    .rn_con = REG_LOW_ONLY,
    .rm_con = REG_LOW_ONLY,
    .implicit_s = true,
    .feat = {.t16 = 1},
};

/* T2/T3 (32-bit): <OP>{S}.W <Rd>, <Rn>, <Rm>  —  no shift field, rd/rn/rm any (not PC/SP) */
static const thop_variant_shape SHAPE_T32_SHIFT_REG = {
    .size = THOP_VARIANT_T32,
    .rd_place = {8, 4},
    .rn_place = {16, 4},
    .rm_place = {0, 4},
    .rd_con = REG_NOT_PC,
    .rn_con = REG_NOT_PC,
    .rm_con = REG_NOT_SP | REG_NOT_PC,
    .has_s_bit = 1,
    .feat = {.t32 = 1},
};

static thumb_opcode shift_reg_t1_emit(uint32_t base, const thop_args *a)
{
    return (thumb_opcode){
        .size = 2,
        .opcode = base | (a->rm << 3) | (a->rd & 0x7),
    };
}

#define V_LSL_REG_T1(b) {&SHAPE_T16_SHIFT_REG, (b), shift_reg_t1_emit}
#define V_LSR_REG_T1(b) {&SHAPE_T16_SHIFT_REG, (b), shift_reg_t1_emit}
#define V_ASR_REG_T1(b) {&SHAPE_T16_SHIFT_REG, (b), shift_reg_t1_emit}
#define V_SHIFT_REG32(b) {&SHAPE_T32_SHIFT_REG, (b)}

/* ═══════════════════════════════════════════════════════════════════
 *  Generic wrapper
 * ═══════════════════════════════════════════════════════════════════ */

static thumb_opcode thop_shift_reg(uint32_t rd, uint32_t rn, uint32_t rm, thumb_flags_behaviour flags,
                                   thumb_enforce_encoding enc, const thop_table *table)
{
    return thop_emit(table->name, table->variants, table->variant_count,
                     (thop_args){.rd = rd, .rn = rn, .rm = rm, .flags = flags, .enc = enc});
}

#define THOP_SHIFT_REG_FN(fn_name, table_id)                                                                           \
    thumb_opcode fn_name(uint32_t rd, uint32_t rn, uint32_t rm, thumb_flags_behaviour flags, thumb_shift shift,          \
                         thumb_enforce_encoding enc)                                                                     \
    {                                                                                                                    \
        (void)shift;                                                                                                     \
        return thop_shift_reg(rd, rn, rm, flags, enc, &table_id);                                                        \
    }

/* ═══════════════════════════════════════════════════════════════════
 *  Instruction tables
 * ═══════════════════════════════════════════════════════════════════ */

TH_TABLE(TH_LSL_REG, "lsl", V_LSL_REG_T1(0x4080), V_SHIFT_REG32(0xFA00F000));
THOP_SHIFT_REG_FN(th_lsl_reg, TH_LSL_REG)

TH_TABLE(TH_LSR_REG, "lsr", V_LSR_REG_T1(0x40C0), V_SHIFT_REG32(0xFA20F000));
THOP_SHIFT_REG_FN(th_lsr_reg, TH_LSR_REG)

TH_TABLE(TH_ASR_REG, "asr", V_ASR_REG_T1(0x4100), V_SHIFT_REG32(0xFA40F000));
THOP_SHIFT_REG_FN(th_asr_reg, TH_ASR_REG)

TH_TABLE(TH_ROR_REG, "ror", V_SHIFT_REG32(0xFA60F000));
THOP_SHIFT_REG_FN(th_ror_reg, TH_ROR_REG)
