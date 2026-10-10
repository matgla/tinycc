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

#include "thop_ldrex.h"
#include "thumb.h"

/* ═══════════════════════════════════════════════════════════════════
 *  Load/store exclusive
 * ═══════════════════════════════════════════════════════════════════ */

/* ───── LDREX (T32) ───── */

TH_TABLE(TH_LDREX, "ldrex", {THOP_SHAPE_LDREX, 0xe8500f00, 0});

/* ───── STREX (T32) ───── */

TH_TABLE(TH_STREX, "strex", {THOP_SHAPE_STREX, 0xe8400000, 0});

/* ───── LDREXB / LDREXH (T32) ───── */

TH_TABLE(TH_LDREXB, "ldrexb", {THOP_SHAPE_LDREXB, 0xe8d00f4f, 0});
TH_TABLE(TH_LDREXH, "ldrexh", {THOP_SHAPE_LDREXB, 0xe8d00f5f, 0});

/* ───── STREXB / STREXH (T32) ───── */

TH_TABLE(TH_STREXB, "strexb", {THOP_SHAPE_STREXB, 0xe8c00f40, 0});
TH_TABLE(TH_STREXH, "strexh", {THOP_SHAPE_STREXB, 0xe8c00f50, 0});

/* ═══════════════════════════════════════════════════════════════════
 *  Public wrappers
 * ═══════════════════════════════════════════════════════════════════ */

thumb_opcode th_ldrex(uint32_t rt, uint32_t rn, int imm)
{
  return thop_emit_table(&TH_LDREX, (thop_args){.rd = rt, .rn = rn, .imm = (uint32_t)(imm >> 2)});
}

thumb_opcode th_strex(uint32_t rd, uint32_t rt, uint32_t rn, int imm)
{
  return thop_emit_table(&TH_STREX, (thop_args){.rd = rd, .rn = rt, .rm = rn, .imm = (uint32_t)(imm >> 2)});
}

thumb_opcode th_ldrexb(uint32_t rt, uint32_t rn)
{
  return thop_emit_table(&TH_LDREXB, (thop_args){.rd = rt, .rn = rn});
}

thumb_opcode th_ldrexh(uint32_t rt, uint32_t rn)
{
  return thop_emit_table(&TH_LDREXH, (thop_args){.rd = rt, .rn = rn});
}

thumb_opcode th_strexb(uint32_t rd, uint32_t rt, uint32_t rn)
{
  return thop_emit_table(&TH_STREXB, (thop_args){.rd = rd, .rn = rt, .rm = rn});
}

thumb_opcode th_strexh(uint32_t rd, uint32_t rt, uint32_t rn)
{
  return thop_emit_table(&TH_STREXH, (thop_args){.rd = rd, .rn = rt, .rm = rn});
}
