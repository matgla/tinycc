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

#include "thop_mem_unpriv.h"

/* ═══════════════════════════════════════════════════════════════════
 *  Unprivileged load/store — shared shape (T32 only)
 * ═══════════════════════════════════════════════════════════════════ */

static const thop_variant_shape SHAPE_T32_MEM_UNPRIV = {
    .size = THOP_VARIANT_T32,
    .rd_place = {12, 4},
    .rn_place = {16, 4},
    .imm = {.kind = IMM_RAW, .width = 8},
    .imm_place = {0, 8},
    .feat = {.t32 = 1},
};

#define V_MEM_UNPRIV(b) {&SHAPE_T32_MEM_UNPRIV, (b)}

#define THOP_MEM_UNPRIV_FN(fn_name, table_id)                                                                          \
    thumb_opcode fn_name(uint32_t rt, uint32_t rn, int imm)                                                              \
    {                                                                                                                    \
        return thop_emit((table_id).name, (table_id).variants, (table_id).variant_count,                                 \
                         (thop_args){.rd = rt, .rn = rn, .imm = (uint32_t)imm});                                         \
    }

/* ═══════════════════════════════════════════════════════════════════
 *  Instruction tables
 * ═══════════════════════════════════════════════════════════════════ */

TH_TABLE(TH_LDRT, "ldrt", V_MEM_UNPRIV(0xF8500E00));
THOP_MEM_UNPRIV_FN(th_ldrt, TH_LDRT)

TH_TABLE(TH_LDRBT, "ldrbt", V_MEM_UNPRIV(0xF8100E00));
THOP_MEM_UNPRIV_FN(th_ldrbt, TH_LDRBT)

TH_TABLE(TH_LDRHT, "ldrht", V_MEM_UNPRIV(0xF8300E00));
THOP_MEM_UNPRIV_FN(th_ldrht, TH_LDRHT)

TH_TABLE(TH_LDRSBT, "ldrsbt", V_MEM_UNPRIV(0xF9100E00));
THOP_MEM_UNPRIV_FN(th_ldrsbt, TH_LDRSBT)

TH_TABLE(TH_LDRSHT, "ldrsht", V_MEM_UNPRIV(0xF9300E00));
THOP_MEM_UNPRIV_FN(th_ldrsht, TH_LDRSHT)

TH_TABLE(TH_STRT, "strt", V_MEM_UNPRIV(0xF8400E00));
THOP_MEM_UNPRIV_FN(th_strt, TH_STRT)

TH_TABLE(TH_STRBT, "strbt", V_MEM_UNPRIV(0xF8000E00));
THOP_MEM_UNPRIV_FN(th_strbt, TH_STRBT)

TH_TABLE(TH_STRHT, "strht", V_MEM_UNPRIV(0xF8200E00));
THOP_MEM_UNPRIV_FN(th_strht, TH_STRHT)
