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

#include "thop_pld.h"
#include "thumb.h"

/* ═══════════════════════════════════════════════════════════════════
 *  Preload instructions (PLD, PLI)
 *
 *  PLD/PLI have two distinct T32 encodings for positive vs negative
 *  immediates (T1 vs T2) with different opcode bases and immediate
 *  widths, so the wrappers encode directly rather than going through
 *  thop_emit.
 * ═══════════════════════════════════════════════════════════════════ */

thumb_opcode th_pld_literal(int imm)
{
  int u = 1;
  if (imm < 0) {
    u = 0;
    imm = -imm;
  }
  return (thumb_opcode){
      .size = 4,
      .opcode = 0xf81ff000 | u << 23 | imm,
  };
}

thumb_opcode th_pld_imm(uint32_t rn, uint32_t w, int imm)
{
  if (imm >= 0) {
    return (thumb_opcode){
        .size = 4,
        .opcode = 0xf890f000 | w << 22 | rn << 16 | imm,
    };
  }
  imm = -imm;
  return (thumb_opcode){
      .size = 4,
      .opcode = 0xf810fc00 | w << 22 | rn << 16 | imm,
  };
}

thumb_opcode th_pld_reg(uint32_t rn, uint32_t rm, uint32_t w, thumb_shift shift)
{
  if (shift.type == THUMB_SHIFT_NONE)
    shift.type = THUMB_SHIFT_LSL;
  return (thumb_opcode){
      .size = 4,
      .opcode = 0xf810f000 | w << 22 | rn << 16 | rm | shift.value << 4,
  };
}

thumb_opcode th_pli_literal(int imm)
{
  int u = 1;
  if (imm < 0) {
    u = 0;
    imm = -imm;
  }
  return (thumb_opcode){
      .size = 4,
      .opcode = 0xf91ff000 | u << 23 | imm,
  };
}

thumb_opcode th_pli_imm(uint32_t rn, uint32_t w, int imm)
{
  if (imm >= 0) {
    return (thumb_opcode){
        .size = 4,
        .opcode = 0xf990f000 | w << 22 | rn << 16 | imm,
    };
  }
  imm = -imm;
  return (thumb_opcode){
      .size = 4,
      .opcode = 0xf910fc00 | w << 22 | rn << 16 | imm,
  };
}

thumb_opcode th_pli_reg(uint32_t rn, uint32_t rm, uint32_t w, thumb_shift shift)
{
  if (shift.type == THUMB_SHIFT_NONE)
    shift.type = THUMB_SHIFT_LSL;
  return (thumb_opcode){
      .size = 4,
      .opcode = 0xf910f000 | w << 22 | rn << 16 | rm | shift.value << 4,
  };
}
