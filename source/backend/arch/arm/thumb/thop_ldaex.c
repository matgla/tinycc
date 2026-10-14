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

TH_TABLE(TH_LDAEX, "ldaex", {THOP_SHAPE_LDAEX, 0xe8d00fef, 0});
TH_TABLE(TH_LDAEXB, "ldaexb", {THOP_SHAPE_LDAEX, 0xe8d00fcf, 0});
TH_TABLE(TH_LDAEXH, "ldaexh", {THOP_SHAPE_LDAEX, 0xe8d00fdf, 0});

/* ───── STLEX / STLEXB / STLEXH (T32, ARMv8-M) ───── */

TH_TABLE(TH_STLEX, "stlex", {THOP_SHAPE_STLEX, 0xe8c00fe0, 0});
TH_TABLE(TH_STLEXB, "stlexb", {THOP_SHAPE_STLEX, 0xe8c00fc0, 0});
TH_TABLE(TH_STLEXH, "stlexh", {THOP_SHAPE_STLEX, 0xe8c00fd0, 0});

/* ═══════════════════════════════════════════════════════════════════
 *  Public wrappers
 * ═══════════════════════════════════════════════════════════════════ */

thumb_opcode th_ldaex(uint32_t rt, uint32_t rn)
{
  return thop_emit_table(&TH_LDAEX, (thop_args){.rd = rt, .rn = rn});
}

thumb_opcode th_stlex(uint32_t rd, uint32_t rt, uint32_t rn)
{
  return thop_emit_table(&TH_STLEX, (thop_args){.rd = rd, .rn = rn, .rm = rt});
}

thumb_opcode th_ldaexb(uint32_t rt, uint32_t rn)
{
  return thop_emit_table(&TH_LDAEXB, (thop_args){.rd = rt, .rn = rn});
}

thumb_opcode th_ldaexh(uint32_t rt, uint32_t rn)
{
  return thop_emit_table(&TH_LDAEXH, (thop_args){.rd = rt, .rn = rn});
}

thumb_opcode th_stlexb(uint32_t rd, uint32_t rt, uint32_t rn)
{
  return thop_emit_table(&TH_STLEXB, (thop_args){.rd = rd, .rn = rn, .rm = rt});
}

thumb_opcode th_stlexh(uint32_t rd, uint32_t rt, uint32_t rn)
{
  return thop_emit_table(&TH_STLEXH, (thop_args){.rd = rd, .rn = rn, .rm = rt});
}
