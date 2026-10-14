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

#include "thop_ldrd.h"
#include "thumb.h"

/* ═══════════════════════════════════════════════════════════════════
 *  LDRD / STRD (dual-word load/store)
 * ═══════════════════════════════════════════════════════════════════ */

/* ───── LDRD_imm (T32) ───── */

thumb_opcode ldrd_imm_emit(uint32_t base, const thop_args *a)
{
  uint32_t imm = a->imm;
  uint32_t index = (a->puw & 0x4) ? 1 : 0;
  uint32_t add = (a->puw & 0x2) ? 1 : 0;
  uint32_t wback = (a->puw & 0x1) ? 1 : 0;
  uint32_t rn = a->rn;
  uint32_t rt = a->rd;
  uint32_t rt2 = a->rm;
  uint32_t op = base | (add << 23) | (index << 24) | (wback << 21) |
                (rn << 16) | (rt << 12) | (rt2 << 8) | (imm & 0xff);
  return (thumb_opcode){.size = 4, .opcode = op};
}

TH_TABLE(TH_LDRD_IMM, "ldrd", {THOP_SHAPE_LDRD, 0xe8500000, THOP_CUSTOM_ldrd_imm_emit});
TH_TABLE(TH_STRD_IMM, "strd", {THOP_SHAPE_LDRD, 0xe8400000, THOP_CUSTOM_ldrd_imm_emit});

static bool ldrd_args_ok(uint32_t rt, uint32_t rt2, int imm, uint32_t puw, bool is_load)
{
  if (imm < 0 || (imm & 3) || imm > 1020)
    return false;
  if ((puw & 0x5) == 0) /* P=0,W=0 is another encoding space */
    return false;
  if (is_load && rt == rt2)
    return false;
  return true;
}

/* ═══════════════════════════════════════════════════════════════════
 *  Public wrappers
 * ═══════════════════════════════════════════════════════════════════ */

thumb_opcode th_ldrd_imm(uint32_t rt, uint32_t rt2, uint32_t rn, int imm, uint32_t puw)
{
  if (!ldrd_args_ok(rt, rt2, imm, puw, true))
    return (thumb_opcode){.size = 0};
  return thop_emit_table(&TH_LDRD_IMM, (thop_args){.rd = rt, .rm = rt2, .rn = rn,
                               .imm = (uint32_t)(imm >> 2), .puw = (uint8_t)puw});
}

thumb_opcode th_strd_imm(uint32_t rt, uint32_t rt2, uint32_t rn, int imm, uint32_t puw)
{
  if (!ldrd_args_ok(rt, rt2, imm, puw, false))
    return (thumb_opcode){.size = 0};
  return thop_emit_table(&TH_STRD_IMM, (thop_args){.rd = rt, .rm = rt2, .rn = rn,
                               .imm = (uint32_t)(imm >> 2), .puw = (uint8_t)puw});
}
