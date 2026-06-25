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

#include "thop_mem_reg.h"

#define USING_GLOBALS
#include "tcc.h"

/* ═══════════════════════════════════════════════════════════════════
 *  Load/Store register — shared shapes
 * ═══════════════════════════════════════════════════════════════════ */

/* T1: <OP> <Rt>, [<Rn>, <Rm>]  —  16-bit, all low, no shift */
static const thop_variant_shape SHAPE_T16_MEM_REG = {
    .size = THOP_VARIANT_T16,
    .rd_place = {0, 3},
    .rn_place = {3, 3},
    .rm_place = {6, 3},
    .rd_con = REG_LOW_ONLY,
    .rn_con = REG_LOW_ONLY,
    .rm_con = REG_LOW_ONLY,
    .feat = {.t16 = 1},
};

/* T2/T3/T4 (32-bit): <OP> <Rt>, [<Rn>, <Rm>{, LSL #<imm>}]  —  shift amount in imm2 bits [5:4] */
static const thop_variant_shape SHAPE_T32_MEM_REG = {
    .size = THOP_VARIANT_T32,
    .rd_place = {12, 4},
    .rn_place = {16, 4},
    .rm_place = {0, 4},
    .rd_con = REG_NOT_SP,
    .rn_con = REG_NOT_PC,
    .rm_con = REG_NOT_SP | REG_NOT_PC,
    .shift_imm2_bits = {4, 2},
    .shift_allowed = (1u << THUMB_SHIFT_LSL),
    .feat = {.t32 = 1},
};

#define V_MEM_REG_T1(b) {&SHAPE_T16_MEM_REG, (b)}
#define V_MEM_REG_T32(b) {&SHAPE_T32_MEM_REG, (b)}

/* ═══════════════════════════════════════════════════════════════════
 *  Generic wrapper
 * ═══════════════════════════════════════════════════════════════════ */

#define THOP_MEM_REG_FN(fn_name, table_id)                                                                             \
    thumb_opcode fn_name(uint32_t rt, uint32_t rn, uint32_t rm, thumb_shift shift, thumb_enforce_encoding enc)           \
    {                                                                                                                    \
        return thop_emit(table_id.name, table_id.variants, table_id.variant_count,                                      \
                         (thop_args){.rd = rt, .rn = rn, .rm = rm, .flags = FLAGS_BEHAVIOUR_NOT_IMPORTANT,              \
                                  .shift = shift, .enc = enc});                                                          \
    }

/* ═══════════════════════════════════════════════════════════════════
 *  Instruction tables
 * ═══════════════════════════════════════════════════════════════════ */

TH_TABLE(TH_LDR_REG, "ldr", V_MEM_REG_T1(0x5800), V_MEM_REG_T32(0xF8500000));
THOP_MEM_REG_FN(th_ldr_reg, TH_LDR_REG)

TH_TABLE(TH_LDRB_REG, "ldrb", V_MEM_REG_T1(0x5C00), V_MEM_REG_T32(0xF8100000));
THOP_MEM_REG_FN(th_ldrb_reg, TH_LDRB_REG)

TH_TABLE(TH_LDRH_REG, "ldrh", V_MEM_REG_T1(0x5A00), V_MEM_REG_T32(0xF8300000));
THOP_MEM_REG_FN(th_ldrh_reg, TH_LDRH_REG)

TH_TABLE(TH_LDRSB_REG, "ldrsb", V_MEM_REG_T1(0x5600), V_MEM_REG_T32(0xF9100000));
THOP_MEM_REG_FN(th_ldrsb_reg, TH_LDRSB_REG)

TH_TABLE(TH_LDRSH_REG, "ldrsh", V_MEM_REG_T1(0x5E00), V_MEM_REG_T32(0xF9300000));
THOP_MEM_REG_FN(th_ldrsh_reg, TH_LDRSH_REG)

TH_TABLE(TH_STR_REG, "str", V_MEM_REG_T1(0x5000), V_MEM_REG_T32(0xF8400000));
THOP_MEM_REG_FN(th_str_reg, TH_STR_REG)

TH_TABLE(TH_STRB_REG, "strb", V_MEM_REG_T1(0x5400), V_MEM_REG_T32(0xF8000000));
THOP_MEM_REG_FN(th_strb_reg, TH_STRB_REG)

TH_TABLE(TH_STRH_REG, "strh", V_MEM_REG_T1(0x5200), V_MEM_REG_T32(0xF8200000));
THOP_MEM_REG_FN(th_strh_reg, TH_STRH_REG)
