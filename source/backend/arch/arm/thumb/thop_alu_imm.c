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

#include "thop_alu_imm.h"

/* ═══════════════════════════════════════════════════════════════════
 *  ALU immediate — shared shapes (defined once in .rodata)
 *
 *  Each shape describes field layout, constraints, immediate encoding,
 *  and feature requirements.  Per-instruction variants only add the
 *  base opcode.
 * ═══════════════════════════════════════════════════════════════════ */

/* Shorthand for variant initializers */
#define V_IMM8(b) {THOP_SHAPE_T16_IMM8, (b)}
#define V_IMM3(b) {THOP_SHAPE_T16_IMM3, (b)}
#define V_MOD_IMM(b) {THOP_SHAPE_T32_MOD_IMM, (b)}
#define V_IMM12(b) {THOP_SHAPE_T32_IMM12, (b)}

/* ═══════════════════════════════════════════════════════════════════
 *  Function-generating macros
 * ═══════════════════════════════════════════════════════════════════ */

#define THOP_ALU_IMM_FN(fn_name, table_id)                                                                             \
  thumb_opcode fn_name(uint32_t rd, uint32_t rn, uint32_t imm, thumb_flags_behaviour flags,                            \
                       thumb_enforce_encoding enc)                                                                     \
  {                                                                                                                    \
    return thop_emit_table(&table_id, \
                     (thop_args){.rd = rd, .rn = rn, .imm = imm, .flags = flags, .enc = enc});                         \
  }

#define THOP_ALU_WIDE_FN(fn_name, base32)                                                                              \
  thumb_opcode fn_name(uint32_t rd, uint32_t rn, uint32_t imm)                                                         \
  {                                                                                                                    \
    static const thop_table _t = {#fn_name, {V_IMM12(base32), {THOP_SHAPE_END}}};                                      \
    return thop_emit_table(&_t, \
                     (thop_args){.rd = rd, .rn = rn, .imm = imm, .enc = ENFORCE_ENCODING_32BIT});                      \
  }

/* ═══════════════════════════════════════════════════════════════════
 *  ADD/SUB — all four forms (T16 narrow + T32 wide)
 * ═══════════════════════════════════════════════════════════════════ */

TH_TABLE(TH_ADD_IMM, "add", V_IMM8(0x3000), V_IMM3(0x1C00), {THOP_SHAPE_T16_ADD_SP_IMM, 0xb000},
         {THOP_SHAPE_T16_ADD_SP_IMM8, 0xa800}, V_MOD_IMM(0xF1000000), V_IMM12(0xF2000000));
THOP_ALU_IMM_FN(th_add_imm, TH_ADD_IMM)
THOP_ALU_WIDE_FN(th_addw, 0xF2000000)

TH_TABLE(TH_SUB_IMM, "sub", V_IMM8(0x3800), V_IMM3(0x1E00), {THOP_SHAPE_T16_SUB_SP_IMM, 0xb080}, V_MOD_IMM(0xF1A00000),
         V_IMM12(0xF2A00000));
THOP_ALU_IMM_FN(th_sub_imm, TH_SUB_IMM)
THOP_ALU_WIDE_FN(th_subw, 0xF2A00000)

/* ═══════════════════════════════════════════════════════════════════
 *  T32-only ALU immediate (modified immediate only)
 * ═══════════════════════════════════════════════════════════════════ */

TH_TABLE(TH_RSB_IMM, "rsb", V_MOD_IMM(0xF1C00000));
THOP_ALU_IMM_FN(th_rsb_imm, TH_RSB_IMM)

TH_TABLE(TH_ADC_IMM, "adc", V_MOD_IMM(0xF1400000));
THOP_ALU_IMM_FN(th_adc_imm, TH_ADC_IMM)

TH_TABLE(TH_SBC_IMM, "sbc", V_MOD_IMM(0xF1600000));
THOP_ALU_IMM_FN(th_sbc_imm, TH_SBC_IMM)

TH_TABLE(TH_AND_IMM, "and", V_MOD_IMM(0xF0000000));
THOP_ALU_IMM_FN(th_and_imm, TH_AND_IMM)

TH_TABLE(TH_BIC_IMM, "bic", V_MOD_IMM(0xF0200000));
THOP_ALU_IMM_FN(th_bic_imm, TH_BIC_IMM)

TH_TABLE(TH_ORR_IMM, "orr", V_MOD_IMM(0xF0400000));
THOP_ALU_IMM_FN(th_orr_imm, TH_ORR_IMM)

TH_TABLE(TH_ORN_IMM, "orn", V_MOD_IMM(0xF0600000));
THOP_ALU_IMM_FN(th_orn_imm, TH_ORN_IMM)

TH_TABLE(TH_EOR_IMM, "eor", V_MOD_IMM(0xF0800000));
THOP_ALU_IMM_FN(th_eor_imm, TH_EOR_IMM)
