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

#include "thop_mrs.h"
#include "thumb.h"

/* ═══════════════════════════════════════════════════════════════════
 *  Move to/from special register (MRS, MSR)
 * ═══════════════════════════════════════════════════════════════════ */

/* ───── MRS (T32 only) ───── */

thumb_opcode mrs_emit(uint32_t base, const thop_args *a)
{
  uint32_t sysm = a->rm;
  uint32_t rd = a->rd;
  uint32_t op = base | (rd << 8) | sysm;
  return (thumb_opcode){.size = 4, .opcode = op};
}

TH_TABLE(TH_MRS, "mrs", {THOP_SHAPE_MRS, 0xf3ef8000, THOP_CUSTOM_mrs_emit});

/* ───── MSR (T32 only) ───── */

thumb_opcode msr_emit(uint32_t base, const thop_args *a)
{
  uint32_t sysm = a->rm;
  uint32_t rn = a->rd;
  uint32_t mask = a->imm;
  uint32_t op = base | (rn << 16) | (mask << 10) | sysm;
  return (thumb_opcode){.size = 4, .opcode = op};
}

TH_TABLE(TH_MSR, "msr", {THOP_SHAPE_MSR, 0xf3808000, THOP_CUSTOM_msr_emit});

/* ═══════════════════════════════════════════════════════════════════
 *  Public wrappers
 * ═══════════════════════════════════════════════════════════════════ */

thumb_opcode th_mrs(uint32_t rd, uint32_t sysm)
{
  return thop_emit_table(&TH_MRS, (thop_args){.rd = rd, .rm = sysm});
}

thumb_opcode th_msr(uint32_t specreg, uint32_t rn, uint32_t mask)
{
  return thop_emit_table(&TH_MSR, (thop_args){.rd = rn, .rm = specreg, .imm = mask});
}
