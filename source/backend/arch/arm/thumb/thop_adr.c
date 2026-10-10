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

#include "thop_adr.h"
#include "thumb.h"

/* ═══════════════════════════════════════════════════════════════════
 *  ADR — address to register
 * ═══════════════════════════════════════════════════════════════════ */

TH_TABLE(TH_ADR_IMM, "adr", {THOP_SHAPE_ADR_T1, 0xa000, 0}, {THOP_SHAPE_ADR_T3, 0xf20f0000, 0},
         {THOP_SHAPE_ADR_T4, 0xf2af0000, 0});

/* ═══════════════════════════════════════════════════════════════════
 *  Public wrappers
 * ═══════════════════════════════════════════════════════════════════ */

thumb_opcode th_adr_imm(uint32_t rd, int imm, thumb_enforce_encoding encoding)
{
  return thop_emit_table(&TH_ADR_IMM, (thop_args){.rd = rd, .imm = (uint32_t)imm, .enc = encoding});
}
