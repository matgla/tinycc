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

#include "thop_mvn.h"
#include "thumb.h"

/* ═══════════════════════════════════════════════════════════════════
 *  MVN — move NOT
 * ═══════════════════════════════════════════════════════════════════ */

/* ───── MVN register ───── */

TH_TABLE(TH_MVN_REG, "mvn",
         {THOP_SHAPE_MVN_REG_T1, 0x43c0, 0},
         {THOP_SHAPE_MVN_REG_T3, 0xea6f0000, 0});

static thumb_opcode thop_mvn_reg(uint32_t rd, uint32_t rn, uint32_t rm, thumb_flags_behaviour flags, thumb_shift shift,
                                 thumb_enforce_encoding enc)
{
  return thop_emit_table(&TH_MVN_REG, (thop_args){.rd = rd, .rn = rn, .rm = rm, .flags = flags, .shift = shift, .enc = enc});
}

thumb_opcode th_mvn_reg(uint32_t rd, uint32_t rn, uint32_t rm, thumb_flags_behaviour flags, thumb_shift shift,
                        thumb_enforce_encoding encoding)
{
  return thop_mvn_reg(rd, rn, rm, flags, shift, encoding);
}

/* ───── MVN immediate ───── */

/* T3: MVN <Rd>, #<const>  —  modified immediate only, always 32-bit */
thumb_opcode mvn_imm_emit(uint32_t base, const thop_args *a)
{
  uint32_t S = (a->flags == FLAGS_BEHAVIOUR_SET) ? 1 : 0;
  uint32_t packed = th_pack_const(a->imm);
  if (packed == 0 && a->imm != 0)
    return (thumb_opcode){.size = 0, .opcode = 0};
  return (thumb_opcode){.size = 4, .opcode = base | (S << 20) | (a->rd << 8) | packed};
}

TH_TABLE(TH_MVN_IMM, "mvn", {THOP_SHAPE_MVN_IMM_T3, 0xf06f0000, THOP_CUSTOM_mvn_imm_emit});

thumb_opcode th_mvn_imm(uint32_t rd, uint32_t rm, uint32_t imm, thumb_flags_behaviour flags,
                        thumb_enforce_encoding encoding)
{
  (void)rm;
  return thop_emit_table(&TH_MVN_IMM, (thop_args){.rd = rd, .imm = imm, .flags = flags, .enc = encoding});
}
