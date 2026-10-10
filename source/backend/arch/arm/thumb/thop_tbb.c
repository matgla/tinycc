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

#include "thop_tbb.h"
#include "thumb.h"

/* ═══════════════════════════════════════════════════════════════════
 *  Table branch (TBB, TBH) and TT instructions
 * ═══════════════════════════════════════════════════════════════════ */

/* ───── TBB / TBH (T32 only, ARMv7-M / v8-M) ───── */

TH_TABLE(TH_TBB, "tbb", {THOP_SHAPE_TBB, 0xe8d0f000, 0});
TH_TABLE(TH_TBH, "tbh", {THOP_SHAPE_TBB, 0xe8d0f010, 0});

/* ───── TT / TTT / TTA / TTAT (T32 only, ARMv8-M) ───── */

thumb_opcode tt_emit(uint32_t base, const thop_args *a)
{
  uint32_t op = base | (a->rn << 16) | (a->rd << 8);
  if (a->imm) {
    op |= 0x0080; /* A bit (bit 7) */
  }
  if (a->imm2) {
    op |= 0x0040; /* T bit (bit 6) */
  }
  return (thumb_opcode){.size = 4, .opcode = op};
}

TH_TABLE(TH_TT, "tt", {THOP_SHAPE_TT, 0xe840f000, THOP_CUSTOM_tt_emit});

/* ═══════════════════════════════════════════════════════════════════
 *  Public wrappers
 * ═══════════════════════════════════════════════════════════════════ */

thumb_opcode th_tbb(uint32_t rn, uint32_t rm, uint32_t h)
{
  if (h)
    return thop_emit_table(&TH_TBH, (thop_args){.rn = rn, .rm = rm});
  return thop_emit_table(&TH_TBB, (thop_args){.rn = rn, .rm = rm});
}

thumb_opcode th_tt(uint32_t rd, uint32_t rn, uint32_t a, uint32_t t)
{
  return thop_emit_table(&TH_TT, (thop_args){.rd = rd, .rn = rn, .imm = a, .imm2 = t});
}
