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

#include "thop_mem_exclusive.h"

/* ═══════════════════════════════════════════════════════════════════
 *  Exclusive/acquire-release — shared shape (T32 only)
 * ═══════════════════════════════════════════════════════════════════ */

static const thop_variant_shape SHAPE_T32_EXCLUSIVE = {
    .size = THOP_VARIANT_T32,
    .rd_place = {12, 4},
    .rn_place = {16, 4},
    .feat = {.t32 = 1},
};

#define V_EXCLUSIVE(b) {&SHAPE_T32_EXCLUSIVE, (b)}

static thumb_opcode thop_exclusive(uint32_t rt, uint32_t rn, const thop_table *table)
{
    return thop_emit(table->name, table->variants, table->variant_count, (thop_args){.rd = rt, .rn = rn});
}

#define THOP_EXCLUSIVE_FN(fn_name, table_id)                                                                           \
    thumb_opcode fn_name(uint32_t rt, uint32_t rn)                                                                       \
    {                                                                                                                    \
        return thop_exclusive(rt, rn, &table_id);                                                                        \
    }

/* ═══════════════════════════════════════════════════════════════════
 *  Instruction tables
 * ═══════════════════════════════════════════════════════════════════ */

TH_TABLE(TH_LDA, "lda", V_EXCLUSIVE(0xE8D00FAF));
THOP_EXCLUSIVE_FN(th_lda, TH_LDA)

TH_TABLE(TH_LDAB, "ldab", V_EXCLUSIVE(0xE8D00F8F));
THOP_EXCLUSIVE_FN(th_ldab, TH_LDAB)

TH_TABLE(TH_LDAH, "ldah", V_EXCLUSIVE(0xE8D00F9F));
THOP_EXCLUSIVE_FN(th_ldah, TH_LDAH)

TH_TABLE(TH_STL, "stl", V_EXCLUSIVE(0xE8C00FAF));
THOP_EXCLUSIVE_FN(th_stl, TH_STL)

TH_TABLE(TH_STLB, "stlb", V_EXCLUSIVE(0xE8C00F8F));
THOP_EXCLUSIVE_FN(th_stlb, TH_STLB)

TH_TABLE(TH_STLH, "stlh", V_EXCLUSIVE(0xE8C00F9F));
THOP_EXCLUSIVE_FN(th_stlh, TH_STLH)
