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

#include "thop_branch.h"
#include "thumb.h"

/* ═══════════════════════════════════════════════════════════════════
 *  Branch instructions
 * ═══════════════════════════════════════════════════════════════════ */

/* ───── BX (T16 only) ───── */

static const thop_variant_shape SHAPE_BX = {
    .size = THOP_VARIANT_T16,
    .rm_place = {3, 4},
    .feat = {.t16 = 1},
};

TH_TABLE(TH_BX, "bx", {&SHAPE_BX, 0x4700, NULL});

/* ───── BL (T32 only) ───── */

static thumb_opcode bl_t1_emit(uint32_t base, const thop_args *a)
{
  uint32_t val = a->imm;
  uint32_t s = (val >> 24) & 1;
  uint32_t imm10 = (val >> 14) & 0x3ff;
  uint32_t j1 = (~((val >> 23) & 1) ^ s) & 1;
  uint32_t j2 = (~((val >> 22) & 1) ^ s) & 1;
  uint32_t imm11 = (val >> 1) & 0x7ff;
  uint32_t hi = 0xf000 | (s << 10) | imm10;
  uint32_t lo = 0xd000 | (j1 << 13) | (j2 << 11) | imm11;
  uint32_t op = (hi << 16) | lo;
  return (thumb_opcode){.size = 4, .opcode = op};
}

static const thop_variant_shape SHAPE_BL_T1 = {
    .size = THOP_VARIANT_T32,
    .feat = {.t32 = 1},
};

TH_TABLE(TH_BL_T1, "bl", {&SHAPE_BL_T1, 0, bl_t1_emit});

/* ───── BLX (T16 reg) ───── */

static const thop_variant_shape SHAPE_BLX_REG = {
    .size = THOP_VARIANT_T16,
    .rm_place = {3, 4},
    .feat = {.t16 = 1},
};

TH_TABLE(TH_BLX_REG, "blx", {&SHAPE_BLX_REG, 0x4780, NULL});

/* ───── B (conditional T16, T32, unconditional T32) ───── */

static const thop_variant_shape SHAPE_B_COND_T16 = {
    .size = THOP_VARIANT_T16,
    .rd_place = {8, 4},
    .imm = {.kind = IMM_RAW, .width = 8},
    .imm_place = {0, 8},
    .feat = {.t16 = 1},
};

TH_TABLE(TH_B_COND_T16, "b", {&SHAPE_B_COND_T16, 0xd000, NULL});

static thumb_opcode b_t3_emit(uint32_t base, const thop_args *a)
{
  uint32_t imm = a->imm;
  uint32_t s = (imm >> 19) & 1;
  uint32_t imm6 = (imm >> 11) & 0x3f;
  uint32_t imm11 = imm & 0x7ff;
  uint32_t j2 = (imm >> 18) & 1;
  uint32_t j1 = (imm >> 17) & 1;
  uint32_t a_field = (s << 10) | imm6;
  uint32_t b_field = (j1 << 13) | (j2 << 11) | imm11;
  uint32_t enc = (a_field << 16) | b_field;
  uint32_t op = 0xf0008000 | (a->rd << 22) | enc;
  return (thumb_opcode){.size = 4, .opcode = op};
}

static thumb_opcode b_t4_emit(uint32_t base, const thop_args *a)
{
  uint32_t val = a->imm;
  uint32_t s = (val >> 24) & 1;
  uint32_t imm10 = (val >> 14) & 0x3ff;
  uint32_t j1 = (~((val >> 23) & 1) ^ s) & 1;
  uint32_t j2 = (~((val >> 22) & 1) ^ s) & 1;
  uint32_t imm11 = (val >> 1) & 0x7ff;
  uint32_t hi = 0xf000 | (s << 10) | imm10;
  uint32_t lo = 0x9000 | (j1 << 13) | (j2 << 11) | imm11;
  uint32_t op = (hi << 16) | lo;
  return (thumb_opcode){.size = 4, .opcode = op};
}

static const thop_variant_shape SHAPE_B_T3 = {
    .size = THOP_VARIANT_T32,
    .rd_place = {22, 4},
    .feat = {.t32 = 1},
};

static const thop_variant_shape SHAPE_B_T4 = {
    .size = THOP_VARIANT_T32,
    .feat = {.t32 = 1},
};

TH_TABLE(TH_B_T3, "b.w", {&SHAPE_B_T3, 0, b_t3_emit});
TH_TABLE(TH_B_T4, "b.w", {&SHAPE_B_T4, 0, b_t4_emit});

/* ───── B (T2 unconditional, 16-bit) ───── */

static thumb_opcode b_t2_emit(uint32_t base, const thop_args *a)
{
  int32_t imm = (int32_t)a->imm;
  int32_t i = imm >> 1;
  if (i < 1023 && i > -1024 && !(imm & 1))
  {
    return (thumb_opcode){.size = 2, .opcode = base | (i & 0x7ff)};
  }
  return (thumb_opcode){.size = 0, .opcode = 0};
}

static const thop_variant_shape SHAPE_B_T2 = {
    .size = THOP_VARIANT_T16,
    .feat = {.t16 = 1},
};

TH_TABLE(TH_B_T2, "b", {&SHAPE_B_T2, 0xe000, b_t2_emit});

/* ───── CBZ / CBNZ (T16 only) ───── */

static thumb_opcode cbz_emit(uint32_t base, const thop_args *a)
{
  uint32_t val = a->imm;
  uint32_t i = (val >> 5) & 1;
  uint32_t imm5 = (val >> 1) & 0x1f;
  uint32_t op = base | (i << 9) | (imm5 << 3) | a->rd;
  return (thumb_opcode){.size = 2, .opcode = op};
}

static const thop_variant_shape SHAPE_CBZ = {
    .size = THOP_VARIANT_T16,
    .rd_place = {0, 3},
    .feat = {.t16 = 1, .cbz = 1},
};

TH_TABLE(TH_CBZ, "cbz", {&SHAPE_CBZ, 0xb100, cbz_emit});
TH_TABLE(TH_CBNZ, "cbnz", {&SHAPE_CBZ, 0xb900, cbz_emit});

/* ═══════════════════════════════════════════════════════════════════
 *  Public wrappers
 * ═══════════════════════════════════════════════════════════════════ */

thumb_opcode th_bx_reg(uint16_t rm)
{
  return thop_emit(TH_BX.name, TH_BX.variants, TH_BX.variant_count, (thop_args){.rm = rm});
}

thumb_opcode th_bl_t1(uint32_t imm)
{
  return thop_emit(TH_BL_T1.name, TH_BL_T1.variants, TH_BL_T1.variant_count,
                   (thop_args){.imm = imm});
}

thumb_opcode th_b_t1(uint32_t cond, uint32_t imm)
{
  return thop_emit(TH_B_COND_T16.name, TH_B_COND_T16.variants, TH_B_COND_T16.variant_count,
                   (thop_args){.rd = cond, .imm = imm & 0xff});
}

thumb_opcode th_b_t3(uint32_t cond, uint32_t imm)
{
  return thop_emit(TH_B_T3.name, TH_B_T3.variants, TH_B_T3.variant_count,
                   (thop_args){.rd = cond, .imm = imm});
}

thumb_opcode th_b_t4(int32_t imm)
{
  return thop_emit(TH_B_T4.name, TH_B_T4.variants, TH_B_T4.variant_count, (thop_args){.imm = (uint32_t)imm});
}

thumb_opcode th_b_t2(int32_t imm11)
{
  return thop_emit(TH_B_T2.name, TH_B_T2.variants, TH_B_T2.variant_count,
                   (thop_args){.imm = (uint32_t)imm11});
}

thumb_opcode th_blx_reg(uint16_t rm)
{
  return thop_emit(TH_BLX_REG.name, TH_BLX_REG.variants, TH_BLX_REG.variant_count,
                   (thop_args){.rm = rm});
}

thumb_opcode th_cbz(uint16_t rn, uint32_t imm, uint32_t nonzero)
{
  if (nonzero)
    return thop_emit(TH_CBNZ.name, TH_CBNZ.variants, TH_CBNZ.variant_count,
                     (thop_args){.rd = rn, .imm = imm});
  return thop_emit(TH_CBZ.name, TH_CBZ.variants, TH_CBZ.variant_count,
                   (thop_args){.rd = rn, .imm = imm});
}
