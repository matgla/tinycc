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

#define V_MEM_REG_T1(b) {THOP_SHAPE_T16_MEM_REG, (b)}
#define V_MEM_REG_T32(b) {THOP_SHAPE_T32_MEM_REG, (b)}

/* ═══════════════════════════════════════════════════════════════════
 *  Generic wrapper
 * ═══════════════════════════════════════════════════════════════════ */

#define THOP_MEM_REG_FN(fn_name, table_id)                                                                             \
    thumb_opcode fn_name(uint32_t rt, uint32_t rn, uint32_t rm, thumb_shift shift, thumb_enforce_encoding enc)           \
    {                                                                                                                    \
        return thop_emit_table(&table_id, \
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
