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

/* T1: MVN <Rd>, <Rm>  —  rd==rn, low regs, implicit S */
static const thop_variant_shape SHAPE_MVN_REG_T1 = {
    .size = THOP_VARIANT_T16,
    .rd_place = {0, 3},
    .rm_place = {3, 3},
    .rd_con = REG_LOW_ONLY | REG_EQ_RN,
    .rn_con = REG_LOW_ONLY,
    .rm_con = REG_LOW_ONLY,
    .implicit_s = true,
    .feat = {.t16 = 1},
};

/* T3: MVN{S}.W <Rd>, <Rm>{,shift} */
static const thop_variant_shape SHAPE_MVN_REG_T3 = {
    .size = THOP_VARIANT_T32,
    .rd_place = {8, 4},
    .rn_place = {16, 4},
    .rm_place = {0, 4},
    .rd_con = REG_NOT_PC,
    .rn_con = REG_NOT_PC,
    .rm_con = REG_NOT_SP | REG_NOT_PC,
    .has_s_bit = 1,
    .shift_type_bits = {4, 2},
    .shift_imm2_bits = {6, 2},
    .shift_imm3_bits = {12, 3},
    .feat = {.t32 = 1},
};

TH_TABLE(TH_MVN_REG, "mvn",
         {&SHAPE_MVN_REG_T1, 0x43c0, NULL},
         {&SHAPE_MVN_REG_T3, 0xea6f0000, NULL});

static thumb_opcode thop_mvn_reg(uint32_t rd, uint32_t rn, uint32_t rm, thumb_flags_behaviour flags, thumb_shift shift,
                                 thumb_enforce_encoding enc)
{
  return thop_emit(TH_MVN_REG.name, TH_MVN_REG.variants, TH_MVN_REG.variant_count,
                   (thop_args){.rd = rd, .rn = rn, .rm = rm, .flags = flags, .shift = shift, .enc = enc});
}

thumb_opcode th_mvn_reg(uint32_t rd, uint32_t rn, uint32_t rm, thumb_flags_behaviour flags, thumb_shift shift,
                        thumb_enforce_encoding encoding)
{
  return thop_mvn_reg(rd, rn, rm, flags, shift, encoding);
}

/* ───── MVN immediate ───── */

/* T3: MVN <Rd>, #<const>  —  modified immediate only, always 32-bit */
static thumb_opcode mvn_imm_emit(uint32_t base, const thop_args *a)
{
  uint32_t S = (a->flags == FLAGS_BEHAVIOUR_SET) ? 1 : 0;
  uint32_t packed = th_pack_const(a->imm);
  if (packed == 0 && a->imm != 0)
    return (thumb_opcode){.size = 0, .opcode = 0};
  return (thumb_opcode){.size = 4, .opcode = base | (S << 20) | (a->rd << 8) | packed};
}

static const thop_variant_shape SHAPE_MVN_IMM_T3 = {
    .size = THOP_VARIANT_T32,
    .rd_place = {8, 4},
    .has_s_bit = 1,
    .feat = {.t32 = 1},
};

TH_TABLE(TH_MVN_IMM, "mvn", {&SHAPE_MVN_IMM_T3, 0xf06f0000, mvn_imm_emit});

thumb_opcode th_mvn_imm(uint32_t rd, uint32_t rm, uint32_t imm, thumb_flags_behaviour flags,
                        thumb_enforce_encoding encoding)
{
  (void)rm;
  return thop_emit(TH_MVN_IMM.name, TH_MVN_IMM.variants, TH_MVN_IMM.variant_count,
                   (thop_args){.rd = rd, .imm = imm, .flags = flags, .enc = encoding});
}
