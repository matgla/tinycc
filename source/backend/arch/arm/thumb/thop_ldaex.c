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

#include "thop_ldaex.h"
#include "thumb.h"

/* ═══════════════════════════════════════════════════════════════════
 *  Load-Acquire / Store-Release exclusive (ARMv8-M)
 * ═══════════════════════════════════════════════════════════════════ */

/* ───── LDAEX / LDAEXB / LDAEXH (T32, ARMv8-M) ───── */

static const thop_variant_shape SHAPE_LDAEX = {
    .size = THOP_VARIANT_T32,
    .rd_place = {12, 4},
    .rn_place = {16, 4},
    .feat = {.t32 = 1, .ldaex = 1},
};

TH_TABLE(TH_LDAEX, "ldaex", {&SHAPE_LDAEX, 0xe8d00fef, NULL});
TH_TABLE(TH_LDAEXB, "ldaexb", {&SHAPE_LDAEX, 0xe8d00fcf, NULL});
TH_TABLE(TH_LDAEXH, "ldaexh", {&SHAPE_LDAEX, 0xe8d00fdf, NULL});

/* ───── STLEX / STLEXB / STLEXH (T32, ARMv8-M) ───── */

static const thop_variant_shape SHAPE_STLEX = {
    .size = THOP_VARIANT_T32,
    .rd_place = {0, 4},
    .rn_place = {16, 4},
    .rm_place = {12, 4},
    .feat = {.t32 = 1, .ldaex = 1},
};

TH_TABLE(TH_STLEX, "stlex", {&SHAPE_STLEX, 0xe8c00fe0, NULL});
TH_TABLE(TH_STLEXB, "stlexb", {&SHAPE_STLEX, 0xe8c00fc0, NULL});
TH_TABLE(TH_STLEXH, "stlexh", {&SHAPE_STLEX, 0xe8c00fd0, NULL});

/* ═══════════════════════════════════════════════════════════════════
 *  Public wrappers
 * ═══════════════════════════════════════════════════════════════════ */

thumb_opcode th_ldaex(uint32_t rt, uint32_t rn)
{
  return thop_emit(TH_LDAEX.name, TH_LDAEX.variants, TH_LDAEX.variant_count,
                   (thop_args){.rd = rt, .rn = rn});
}

thumb_opcode th_stlex(uint32_t rd, uint32_t rt, uint32_t rn)
{
  return thop_emit(TH_STLEX.name, TH_STLEX.variants, TH_STLEX.variant_count,
                   (thop_args){.rd = rd, .rn = rn, .rm = rt});
}

thumb_opcode th_ldaexb(uint32_t rt, uint32_t rn)
{
  return thop_emit(TH_LDAEXB.name, TH_LDAEXB.variants, TH_LDAEXB.variant_count,
                   (thop_args){.rd = rt, .rn = rn});
}

thumb_opcode th_ldaexh(uint32_t rt, uint32_t rn)
{
  return thop_emit(TH_LDAEXH.name, TH_LDAEXH.variants, TH_LDAEXH.variant_count,
                   (thop_args){.rd = rt, .rn = rn});
}

thumb_opcode th_stlexb(uint32_t rd, uint32_t rt, uint32_t rn)
{
  return thop_emit(TH_STLEXB.name, TH_STLEXB.variants, TH_STLEXB.variant_count,
                   (thop_args){.rd = rd, .rn = rn, .rm = rt});
}

thumb_opcode th_stlexh(uint32_t rd, uint32_t rt, uint32_t rn)
{
  return thop_emit(TH_STLEXH.name, TH_STLEXH.variants, TH_STLEXH.variant_count,
                   (thop_args){.rd = rd, .rn = rn, .rm = rt});
}
