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

#include "thop_ldr_literal.h"
#include "thumb.h"

/* ═══════════════════════════════════════════════════════════════════
 *  LDR (literal) — load from PC-relative address
 * ═══════════════════════════════════════════════════════════════════ */

/* T1: LDR <Rt>, [PC, #<imm8*4>]  —  rt low reg, imm scaled by 4 */
static const thop_variant_shape SHAPE_LDR_LIT_T1 = {
    .size = THOP_VARIANT_T16,
    .rd_place = {8, 3},
    .rd_con = REG_LOW_ONLY,
    .imm = {.kind = IMM_RAW, .width = 8, .scale_log2 = 2},
    .imm_place = {0, 8},
    .feat = {.t16 = 1},
};

/* T3/T4: LDR <Rt>, [PC, #+/-<imm12>]  —  32-bit, rt != PC */
static thumb_opcode ldr_literal_emit(uint32_t base, const thop_args *a)
{
  uint32_t rt = a->rd;
  uint32_t imm = a->imm;
  uint32_t add = a->rn; /* re-use rn to pass add/sub flag */

  if (rt == R_PC)
    return (thumb_opcode){.size = 0, .opcode = 0};

  if (imm <= 0xfff) {
    uint32_t ins = (0xf85f | ((add & 1) << 7)) << 16;
    ins |= (rt & 0xf) << 12 | imm;
    return (thumb_opcode){.size = 4, .opcode = ins};
  }
  return (thumb_opcode){.size = 0, .opcode = 0};
}

static const thop_variant_shape SHAPE_LDR_LIT_T32 = {
    .size = THOP_VARIANT_T32,
    .rd_place = {12, 4},
    .rd_con = REG_NOT_PC,
    .feat = {.t32 = 1},
};

TH_TABLE(TH_LDR_LITERAL, "ldr",
         {&SHAPE_LDR_LIT_T1, 0x4800, NULL},
         {&SHAPE_LDR_LIT_T32, 0, ldr_literal_emit});

/* ═══════════════════════════════════════════════════════════════════
 *  Public wrappers
 * ═══════════════════════════════════════════════════════════════════ */

thumb_opcode th_ldr_literal(uint16_t rt, uint32_t imm, uint32_t add)
{
  return thop_emit(TH_LDR_LITERAL.name, TH_LDR_LITERAL.variants, TH_LDR_LITERAL.variant_count,
                   (thop_args){.rd = rt, .imm = imm, .rn = add});
}

