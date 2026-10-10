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

#include "thop_shift_imm.h"

/* ═══════════════════════════════════════════════════════════════════
 *  Shift immediate — shared shapes
 * ═══════════════════════════════════════════════════════════════════ */

#define V_SHIFT_IMM16(b) {THOP_SHAPE_T16_SHIFT_IMM, (b)}
#define V_SHIFT_IMM32(b) {THOP_SHAPE_T32_SHIFT_IMM, (b)}

/* ═══════════════════════════════════════════════════════════════════
 *  Generic wrapper
 * ═══════════════════════════════════════════════════════════════════ */

static thumb_opcode thop_shift_imm(uint32_t rd, uint32_t rm, uint32_t imm, thumb_flags_behaviour flags,
                                   thumb_enforce_encoding enc, thumb_shift shift, const thop_table *table)
{
    return thop_emit_table(table, (thop_args){.rd = rd, .rm = rm, .imm = imm, .flags = flags, .shift = shift, .enc = enc});
}

#define THOP_SHIFT_IMM_FN(fn_name, table_id, shift_type)                                                               \
    thumb_opcode fn_name(uint32_t rd, uint32_t rm, uint32_t imm, thumb_flags_behaviour flags,                              \
                         thumb_enforce_encoding enc)                                                                     \
    {                                                                                                                    \
        thumb_shift shift = {.type = shift_type, .value = imm, .mode = THUMB_SHIFT_IMMEDIATE};                           \
        if (!th_shift_imm_normalize(&shift))                                                                             \
            return (thumb_opcode){.size = 0, .opcode = 0};                                                               \
        return thop_shift_imm(rd, rm, shift.value & 0x1f, flags, enc, shift, &table_id);                                 \
    }

/* ═══════════════════════════════════════════════════════════════════
 *  Instruction tables
 * ═══════════════════════════════════════════════════════════════════ */

TH_TABLE(TH_LSL_IMM, "lsl", V_SHIFT_IMM16(0x0000), V_SHIFT_IMM32(0xEA4F0000));
THOP_SHIFT_IMM_FN(th_lsl_imm, TH_LSL_IMM, THUMB_SHIFT_LSL)

TH_TABLE(TH_LSR_IMM, "lsr", V_SHIFT_IMM16(0x0000), V_SHIFT_IMM32(0xEA4F0000));
THOP_SHIFT_IMM_FN(th_lsr_imm, TH_LSR_IMM, THUMB_SHIFT_LSR)

TH_TABLE(TH_ASR_IMM, "asr", V_SHIFT_IMM16(0x0000), V_SHIFT_IMM32(0xEA4F0000));
THOP_SHIFT_IMM_FN(th_asr_imm, TH_ASR_IMM, THUMB_SHIFT_ASR)

TH_TABLE(TH_ROR_IMM, "ror", V_SHIFT_IMM32(0xEA4F0000));
THOP_SHIFT_IMM_FN(th_ror_imm, TH_ROR_IMM, THUMB_SHIFT_ROR)
