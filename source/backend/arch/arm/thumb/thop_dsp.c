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

#include "thop_dsp.h"
#include "thumb.h"

/* ═══════════════════════════════════════════════════════════════════
 *  DSP and SIMD instructions
 * ═══════════════════════════════════════════════════════════════════ */

/* ───── UADD8 / USUB8 / SEL (T32 only, ARMv7E-M / v8-M) ───── */

static const thop_variant_shape SHAPE_DSP_REG3 = {
    .size = THOP_VARIANT_T32,
    .rd_place = {8, 4},
    .rn_place = {16, 4},
    .rm_place = {0, 4},
    .feat = {.t32 = 1, .dsp = 1},
};

TH_TABLE(TH_UADD8, "uadd8", {&SHAPE_DSP_REG3, 0xfa80f040, NULL});
TH_TABLE(TH_USUB8, "usub8", {&SHAPE_DSP_REG3, 0xfac0f040, NULL});
TH_TABLE(TH_SEL, "sel", {&SHAPE_DSP_REG3, 0xfaa0f080, NULL});

/* ───── PKHBT (T32 only) ───── */

static thumb_opcode pkhbt_emit(uint32_t base, const thop_args *a)
{
  uint32_t shift_n = a->shift.value;
  uint32_t tb = (a->shift.type == THUMB_SHIFT_ASR) ? 1 : 0;
  uint32_t op = base | (a->rd << 8) | (a->rn << 16) | (a->rm << 0) |
                ((shift_n & 3) << 6) | (((shift_n >> 2) & 7) << 12) | (tb << 5);
  return (thumb_opcode){.size = 4, .opcode = op};
}

static const thop_variant_shape SHAPE_PKH = {
    .size = THOP_VARIANT_T32,
    .rd_place = {8, 4},
    .rn_place = {16, 4},
    .rm_place = {0, 4},
    .shift_allowed = (1u << THUMB_SHIFT_LSL) | (1u << THUMB_SHIFT_ASR),
    .feat = {.t32 = 1, .dsp = 1},
};

TH_TABLE(TH_PKHBT, "pkhbt", {&SHAPE_PKH, 0xeac00000, pkhbt_emit});

/* ═══════════════════════════════════════════════════════════════════
 *  Public wrappers
 * ═══════════════════════════════════════════════════════════════════ */

thumb_opcode th_uadd8(uint16_t rd, uint16_t rn, uint16_t rm)
{
  return thop_emit(TH_UADD8.name, TH_UADD8.variants, TH_UADD8.variant_count,
                   (thop_args){.rd = rd, .rn = rn, .rm = rm});
}

thumb_opcode th_usub8(uint16_t rd, uint16_t rn, uint16_t rm)
{
  return thop_emit(TH_USUB8.name, TH_USUB8.variants, TH_USUB8.variant_count,
                   (thop_args){.rd = rd, .rn = rn, .rm = rm});
}

thumb_opcode th_sel(uint16_t rd, uint16_t rn, uint16_t rm)
{
  return thop_emit(TH_SEL.name, TH_SEL.variants, TH_SEL.variant_count,
                   (thop_args){.rd = rd, .rn = rn, .rm = rm});
}

thumb_opcode th_pkhbt(uint32_t rd, uint32_t rn, uint32_t rm, thumb_shift shift)
{
  return thop_emit(TH_PKHBT.name, TH_PKHBT.variants, TH_PKHBT.variant_count,
                   (thop_args){.rd = rd, .rn = rn, .rm = rm, .shift = shift});
}
