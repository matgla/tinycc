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

TH_TABLE(TH_UADD8, "uadd8", {THOP_SHAPE_DSP_REG3, 0xfa80f040, 0});
TH_TABLE(TH_USUB8, "usub8", {THOP_SHAPE_DSP_REG3, 0xfac0f040, 0});
TH_TABLE(TH_SEL, "sel", {THOP_SHAPE_DSP_REG3, 0xfaa0f080, 0});

/* ───── PKHBT (T32 only) ───── */

thumb_opcode pkhbt_emit(uint32_t base, const thop_args *a)
{
  uint32_t shift_n = a->shift.value;
  uint32_t tb = (a->shift.type == THUMB_SHIFT_ASR) ? 1 : 0;
  uint32_t op = base | (a->rd << 8) | (a->rn << 16) | (a->rm << 0) |
                ((shift_n & 3) << 6) | (((shift_n >> 2) & 7) << 12) | (tb << 5);
  return (thumb_opcode){.size = 4, .opcode = op};
}

TH_TABLE(TH_PKHBT, "pkhbt", {THOP_SHAPE_PKH, 0xeac00000, THOP_CUSTOM_pkhbt_emit});

/* ═══════════════════════════════════════════════════════════════════
 *  Public wrappers
 * ═══════════════════════════════════════════════════════════════════ */

thumb_opcode th_uadd8(uint16_t rd, uint16_t rn, uint16_t rm)
{
  return thop_emit_table(&TH_UADD8, (thop_args){.rd = rd, .rn = rn, .rm = rm});
}

thumb_opcode th_usub8(uint16_t rd, uint16_t rn, uint16_t rm)
{
  return thop_emit_table(&TH_USUB8, (thop_args){.rd = rd, .rn = rn, .rm = rm});
}

thumb_opcode th_sel(uint16_t rd, uint16_t rn, uint16_t rm)
{
  return thop_emit_table(&TH_SEL, (thop_args){.rd = rd, .rn = rn, .rm = rm});
}

thumb_opcode th_pkhbt(uint32_t rd, uint32_t rn, uint32_t rm, thumb_shift shift)
{
  return thop_emit_table(&TH_PKHBT, (thop_args){.rd = rd, .rn = rn, .rm = rm, .shift = shift});
}
