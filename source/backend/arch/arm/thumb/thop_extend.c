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

#include "thop_extend.h"

/* ═══════════════════════════════════════════════════════════════════
 *  Extend instructions — shared shapes
 * ═══════════════════════════════════════════════════════════════════ */

/* T1: <OP> <Rd>, <Rm>  —  16-bit, rd/rm low, no rotation */
static const thop_variant_shape SHAPE_T16_EXTEND = {
    .size = THOP_VARIANT_T16,
    .rd_place = {0, 3},
    .rm_place = {3, 3},
    .rd_con = REG_LOW_ONLY,
    .rm_con = REG_LOW_ONLY,
    .imm = {.kind = IMM_RAW, .width = 0}, /* rotate must be 0 for T1 */
    .shift_allowed = (1u << THUMB_SHIFT_ROR),
    .feat = {.t16 = 1},
};

/* T2: <OP> <Rd>, <Rm>{, ROR #<rotate>}  —  32-bit, rotate in bits [5:4] */
static const thop_variant_shape SHAPE_T32_EXTEND = {
    .size = THOP_VARIANT_T32,
    .rd_place = {8, 4},
    .rm_place = {0, 4},
    .ra_place = {16, 4}, /* rm duplicated at bits [19:16] */
    .rd_con = REG_NOT_PC,
    .imm = {.kind = IMM_RAW, .width = 2, .scale_log2 = 3},
    .imm_place = {4, 2},
    .shift_allowed = (1u << THUMB_SHIFT_ROR),
    .feat = {.t32 = 1},
};

#define V_EXTEND_T16(b) {&SHAPE_T16_EXTEND, (b)}
#define V_EXTEND_T32(b) {&SHAPE_T32_EXTEND, (b)}

/* ═══════════════════════════════════════════════════════════════════
 *  Generic wrapper
 * ═══════════════════════════════════════════════════════════════════ */

static thumb_opcode thop_extend(uint32_t rd, uint32_t rm, thumb_shift shift, thumb_enforce_encoding enc,
                                const thop_table *table)
{
  return thop_emit(table->name, table->variants, table->variant_count,
                   (thop_args){.rd = rd,
                               .rm = rm,
                               .ra = rm,
                               .imm = shift.value,
                               .shift = shift,
                               .enc = enc});
}

#define THOP_EXTEND_FN(fn_name, table_id)                                                                               \
  thumb_opcode fn_name(uint32_t rd, uint32_t rm, thumb_shift shift, thumb_enforce_encoding enc)                           \
  {                                                                                                                       \
    return thop_extend(rd, rm, shift, enc, &table_id);                                                                    \
  }

/* ═══════════════════════════════════════════════════════════════════
 *  Instruction tables
 * ═══════════════════════════════════════════════════════════════════ */

TH_TABLE(TH_SXTB, "sxtb", V_EXTEND_T16(0xb240), V_EXTEND_T32(0xfa4ff080));
THOP_EXTEND_FN(th_sxtb, TH_SXTB)

TH_TABLE(TH_SXTH, "sxth", V_EXTEND_T16(0xb200), V_EXTEND_T32(0xfa0ff080));
THOP_EXTEND_FN(th_sxth, TH_SXTH)

TH_TABLE(TH_UXTB, "uxtb", V_EXTEND_T16(0xb2c0), V_EXTEND_T32(0xfa5ff080));
THOP_EXTEND_FN(th_uxtb, TH_UXTB)

TH_TABLE(TH_UXTH, "uxth", V_EXTEND_T16(0xb280), V_EXTEND_T32(0xfa1ff080));
THOP_EXTEND_FN(th_uxth, TH_UXTH)
