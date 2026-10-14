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

#include "thop_rev.h"

/* ═══════════════════════════════════════════════════════════════════
 *  Reverse / bit-reverse — shared shapes
 * ═══════════════════════════════════════════════════════════════════ */

#define V_REV_T16(b) {THOP_SHAPE_T16_REV, (b)}
#define V_REV_T32(b) {THOP_SHAPE_T32_REV, (b)}
#define V_RBIT_T32(b) {THOP_SHAPE_T32_RBIT, (b)}

/* ═══════════════════════════════════════════════════════════════════
 *  Generic wrapper
 * ═══════════════════════════════════════════════════════════════════ */

static thumb_opcode thop_rev(uint32_t rd, uint32_t rm, thumb_enforce_encoding enc, const thop_table *table)
{
  return thop_emit_table(table, (thop_args){.rd = rd, .rm = rm, .ra = rm, .enc = enc});
}

/* ═══════════════════════════════════════════════════════════════════
 *  Instruction tables
 * ═══════════════════════════════════════════════════════════════════ */

TH_TABLE(TH_REV, "rev", V_REV_T16(0xba00), V_REV_T32(0xfa90f080));

thumb_opcode th_rev(uint32_t rd, uint32_t rm, thumb_enforce_encoding enc)
{
  return thop_rev(rd, rm, enc, &TH_REV);
}

TH_TABLE(TH_REV16, "rev16", V_REV_T16(0xba40), V_REV_T32(0xfa90f090));

thumb_opcode th_rev16(uint32_t rd, uint32_t rm, thumb_enforce_encoding enc)
{
  return thop_rev(rd, rm, enc, &TH_REV16);
}

TH_TABLE(TH_REVSH, "revsh", V_REV_T16(0xbac0), V_REV_T32(0xfa90f0b0));

thumb_opcode th_revsh(uint32_t rd, uint32_t rm, thumb_enforce_encoding enc)
{
  return thop_rev(rd, rm, enc, &TH_REVSH);
}

TH_TABLE(TH_RBIT, "rbit", V_RBIT_T32(0xfa90f0a0));

thumb_opcode th_rbit(uint32_t rd, uint32_t rm)
{
  return thop_rev(rd, rm, ENFORCE_ENCODING_NONE, &TH_RBIT);
}
