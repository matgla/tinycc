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

#include "thop_alu_reg.h"

/* ═══════════════════════════════════════════════════════════════════
 *  ALU register — shared shapes
 *
 *  T1: OP <Rd>, <Rn>, <Rm>        — 16-bit, all low, no shift
 *  T3: OP{S}.W <Rd>, <Rn>, <Rm>{,shift} — 32-bit, with shift+S
 *
 *  T2 (ADD <Rdn>, <Rm>) uses a split DN:Rd encoding that doesn't
 *  fit the generic bitfield model — handled via custom emit.
 * ═══════════════════════════════════════════════════════════════════ */

/* 16-bit: OP <Rd>, <Rn>, <Rm>  —  all low regs, no shift */
static const thop_variant_shape SHAPE_T16_REG3 = {
    .size = THOP_VARIANT_T16,
    .rd_place = {0, 3},
    .rn_place = {3, 3},
    .rm_place = {6, 3},
    .rd_con = REG_LOW_ONLY,
    .rn_con = REG_LOW_ONLY,
    .rm_con = REG_LOW_ONLY,
    .implicit_s = true,
    .feat = {.t16 = 1},
};

/* 32-bit: OP{S}.W <Rd>, <Rn>, <Rm>{,shift}  —  with S bit and shift */
static const thop_variant_shape SHAPE_T32_REG_SHIFT = {
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

#define V_REG3(b) {&SHAPE_T16_REG3, (b)}
#define V_REGS(b) {&SHAPE_T32_REG_SHIFT, (b)}

/* ═══════════════════════════════════════════════════════════════════
 *  Generic wrapper
 * ═══════════════════════════════════════════════════════════════════ */

static thumb_opcode thop_alu_reg(uint32_t rd, uint32_t rn, uint32_t rm, thumb_flags_behaviour flags, thumb_shift shift,
                                 thumb_enforce_encoding enc, const thop_table *table)
{
  return thop_emit(table->name, table->variants, table->variant_count,
                   (thop_args){.rd = rd, .rn = rn, .rm = rm, .flags = flags, .shift = shift, .enc = enc});
}

/* ═══════════════════════════════════════════════════════════════════
 *  Function-generating macros
 * ═══════════════════════════════════════════════════════════════════ */

#define THOP_ALU_REG_FN(fn_name, table_id)                                                                             \
  thumb_opcode fn_name(uint32_t rd, uint32_t rn, uint32_t rm, thumb_flags_behaviour flags, thumb_shift shift,          \
                       thumb_enforce_encoding enc)                                                                     \
  {                                                                                                                    \
    return thop_alu_reg(rd, rn, rm, flags, shift, enc, &table_id);                                                     \
  }

/* ═══════════════════════════════════════════════════════════════════
 *  ADD register — T1 + ADD-SP-T1 + T2 + T3
 *
 *  ADD-SP-T1 (ADD <Rdm>, SP, <Rdm>) and T2 (ADD <Rdn>, <Rm>) share
 *  the same 0x4400 base — SP goes in the Rm encoding field via
 *  rn_place, Rdm via the DN:Rd split.
 * ═══════════════════════════════════════════════════════════════════ */

/* ADD <Rdm>, SP, <Rdm>  —  rd==rm, rn==SP, DN:Rd split */
static const thop_variant_shape SHAPE_T16_ADD_SP_REG = {
    .size = THOP_VARIANT_T16,
    .rd_place = {0, 3},
    .has_rd_hi = 1,
    .rn_place = {3, 4},
    .rd_con = REG_EQ_RM,
    .rn_con = REG_SP_ONLY,
    .feat = {.t16 = 1},
};

/* ADD <Rdn>, <Rm>  —  rd==rn, any reg, no shift, no S, DN:Rd split */
static const thop_variant_shape SHAPE_T16_ADD_T2 = {
    .size = THOP_VARIANT_T16,
    .rd_place = {0, 3},
    .has_rd_hi = 1,
    .rn_place = {0, 3},
    .rm_place = {3, 4},
    .rd_con = REG_EQ_RN,
    .feat = {.t16 = 1},
};

#define V_ADD_SP_REG(b) {&SHAPE_T16_ADD_SP_REG, (b)}
#define V_ADD_T2(b) {&SHAPE_T16_ADD_T2, (b)}

TH_TABLE(TH_ADD_REG, "add", V_REG3(0x1800), V_ADD_SP_REG(0x4400), V_ADD_T2(0x4400), V_REGS(0xEB000000));
THOP_ALU_REG_FN(th_add_reg, TH_ADD_REG)

TH_TABLE(TH_SUB_REG, "sub", V_REG3(0x1a00), V_REGS(0xEBA00000));
THOP_ALU_REG_FN(th_sub_reg, TH_SUB_REG)

TH_TABLE(TH_RSB_REG, "rsb", V_REGS(0xEBC00000));
THOP_ALU_REG_FN(th_rsb_reg, TH_RSB_REG)

/* 16-bit: OP <Rdn>, <Rm>  —  rd==rn, all low, no shift (ADC, SBC, AND, ORR, EOR, BIC, etc.) */
static const thop_variant_shape SHAPE_T16_REG_RDN_RM = {
    .size = THOP_VARIANT_T16,
    .rd_place = {0, 3},
    .rm_place = {3, 3},
    .rd_con = REG_LOW_ONLY | REG_EQ_RN,
    .rn_con = REG_LOW_ONLY,
    .rm_con = REG_LOW_ONLY,
    .implicit_s = true,
    .feat = {.t16 = 1},
};

#define V_REG_RDN_RM(b) {&SHAPE_T16_REG_RDN_RM, (b)}

TH_TABLE(TH_ADC_REG, "adc", V_REG_RDN_RM(0x4140), V_REGS(0xEB400000));
THOP_ALU_REG_FN(th_adc_reg, TH_ADC_REG)

TH_TABLE(TH_SBC_REG, "sbc", V_REG_RDN_RM(0x4180), V_REGS(0xEB600000));
THOP_ALU_REG_FN(th_sbc_reg, TH_SBC_REG)

TH_TABLE(TH_AND_REG, "and", V_REG_RDN_RM(0x4000), V_REGS(0xEA000000));
THOP_ALU_REG_FN(th_and_reg, TH_AND_REG)

TH_TABLE(TH_BIC_REG, "bic", V_REG_RDN_RM(0x4380), V_REGS(0xEA200000));
THOP_ALU_REG_FN(th_bic_reg, TH_BIC_REG)

TH_TABLE(TH_ORR_REG, "orr", V_REG_RDN_RM(0x4300), V_REGS(0xEA400000));
THOP_ALU_REG_FN(th_orr_reg, TH_ORR_REG)

TH_TABLE(TH_ORN_REG, "orn", V_REGS(0xEA600000));
THOP_ALU_REG_FN(th_orn_reg, TH_ORN_REG)

TH_TABLE(TH_EOR_REG, "eor", V_REG_RDN_RM(0x4040), V_REGS(0xEA800000));
THOP_ALU_REG_FN(th_eor_reg, TH_EOR_REG)
