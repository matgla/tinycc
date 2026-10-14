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

#include "thop_cmp.h"

/* ═══════════════════════════════════════════════════════════════════
 *  Compare/Test immediate — shared shapes
 * ═══════════════════════════════════════════════════════════════════ */

/* ═══════════════════════════════════════════════════════════════════
 *  Compare/Test register — shared shapes
 * ═══════════════════════════════════════════════════════════════════ */

thumb_opcode cmp_reg_t2_custom_emit(uint32_t base, const thop_args *a)
{
  const uint16_t N = (a->rn >> 3) & 0x1;
  return (thumb_opcode){
      .size = 2,
      .opcode = base | (N << 7) | (a->rm << 3) | (a->rn & 0x7),
  };
}

/* ═══════════════════════════════════════════════════════════════════
 *  Generic wrappers
 * ═══════════════════════════════════════════════════════════════════ */

static thumb_opcode thop_cmp_imm(uint32_t rn, uint32_t imm, thumb_flags_behaviour flags, thumb_enforce_encoding enc,
                                 const thop_table *table)
{
  return thop_emit_table(table, (thop_args){.rd = 0xf, .rn = rn, .imm = imm, .flags = flags, .enc = enc});
}

static thumb_opcode thop_cmp_reg(uint32_t rn, uint32_t rm, thumb_flags_behaviour flags, thumb_shift shift,
                                 thumb_enforce_encoding enc, const thop_table *table)
{
  return thop_emit_table(table, (thop_args){.rd = 0xf, .rn = rn, .rm = rm, .flags = flags, .shift = shift, .enc = enc});
}

/* ═══════════════════════════════════════════════════════════════════
 *  Function-generating macros
 * ═══════════════════════════════════════════════════════════════════ */

#define THOP_CMP_IMM_FN(fn_name, table_id)                                                                             \
  thumb_opcode fn_name(uint32_t rn, uint32_t imm, thumb_flags_behaviour flags, thumb_enforce_encoding enc)             \
  {                                                                                                                    \
    return thop_cmp_imm(rn, imm, flags, enc, &table_id);                                                               \
  }

#define THOP_CMP_REG_FN(fn_name, table_id)                                                                             \
  thumb_opcode fn_name(uint32_t rn, uint32_t rm, thumb_flags_behaviour flags, thumb_shift shift,                       \
                       thumb_enforce_encoding enc)                                                                     \
  {                                                                                                                    \
    return thop_cmp_reg(rn, rm, flags, shift, enc, &table_id);                                                         \
  }

/* Shorthand variant initializers */
#define V_CMP_IMM8(b) {THOP_SHAPE_T16_CMP_IMM, (b)}
#define V_CMP_IMM(b) {THOP_SHAPE_T32_CMP_IMM, (b)}
#define V_CMP_REG(b) {THOP_SHAPE_T16_CMP_REG, (b)}
#define V_CMP_REG_T2(b) {THOP_SHAPE_T16_CMP_REG_T2, (b), THOP_CUSTOM_cmp_reg_t2_custom_emit}
#define V_CMP_REGS(b) {THOP_SHAPE_T32_CMP_REG, (b)}

/* ═══════════════════════════════════════════════════════════════════
 *  Instruction tables
 * ═══════════════════════════════════════════════════════════════════ */

TH_TABLE(TH_CMP_IMM, "cmp", V_CMP_IMM8(0x2800), V_CMP_IMM(0xF1B00F00));
THOP_CMP_IMM_FN(th_cmp_imm, TH_CMP_IMM)

TH_TABLE(TH_CMN_IMM, "cmn", V_CMP_IMM(0xF1100F00));
THOP_CMP_IMM_FN(th_cmn_imm, TH_CMN_IMM)

TH_TABLE(TH_TST_IMM, "tst", V_CMP_IMM(0xF0100F00));
THOP_CMP_IMM_FN(th_tst_imm, TH_TST_IMM)

TH_TABLE(TH_TEQ_IMM, "teq", V_CMP_IMM(0xF0900F00));
THOP_CMP_IMM_FN(th_teq_imm, TH_TEQ_IMM)

TH_TABLE(TH_CMP_REG, "cmp", V_CMP_REG(0x4280), V_CMP_REG_T2(0x4500), V_CMP_REGS(0xEBB00F00));

thumb_opcode th_cmp_reg(uint32_t rd, uint32_t rn, uint32_t rm, thumb_flags_behaviour flags, thumb_shift shift,
                        thumb_enforce_encoding encoding)
{
  (void)rd;
  return thop_cmp_reg(rn, rm, flags, shift, encoding, &TH_CMP_REG);
}

TH_TABLE(TH_CMN_REG, "cmn", V_CMP_REG(0x42C0), V_CMP_REGS(0xEB100F00));
THOP_CMP_REG_FN(th_cmn_reg, TH_CMN_REG)

TH_TABLE(TH_TST_REG, "tst", V_CMP_REG(0x4200), V_CMP_REGS(0xEA100F00));
THOP_CMP_REG_FN(th_tst_reg, TH_TST_REG)

TH_TABLE(TH_TEQ_REG, "teq", V_CMP_REGS(0xEA900F00));
THOP_CMP_REG_FN(th_teq_reg, TH_TEQ_REG)
