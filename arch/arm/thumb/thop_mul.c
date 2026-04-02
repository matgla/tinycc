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

#include "thop_mul.h"
#include "thumb.h"

/* ═══════════════════════════════════════════════════════════════════
 *  Multiply, divide, and long multiply
 * ═══════════════════════════════════════════════════════════════════ */

/* ───── MUL (T16: lo regs only, N == D; T32: any regs) ───── */

static thumb_opcode mul_t16_emit(uint32_t base, const thop_args *a)
{
  uint32_t op = base | ((a->rd & 7) << 0) | ((a->rm & 7) << 3);
  return (thumb_opcode){.size = 2, .opcode = op};
}

static const thop_variant_shape SHAPE_MUL_T16 = {
    .size = THOP_VARIANT_T16,
    .rd_place = {0, 3},
    .rm_place = {3, 3},
    .feat = {.t16 = 1},
};

static const thop_variant_shape SHAPE_MUL_T32 = {
    .size = THOP_VARIANT_T32,
    .rd_place = {8, 4},
    .rn_place = {16, 4},
    .rm_place = {0, 4},
    .feat = {.t32 = 1},
};

TH_TABLE(TH_MUL_T16, "muls", {&SHAPE_MUL_T16, 0x4340, mul_t16_emit});
TH_TABLE(TH_MUL_T32, "mul", {&SHAPE_MUL_T32, 0xfb00f000, NULL});

/* ───── MLA / MLS (T32 only) ───── */

static const thop_variant_shape SHAPE_MLA = {
    .size = THOP_VARIANT_T32,
    .rd_place = {8, 4},
    .rn_place = {16, 4},
    .rm_place = {0, 4},
    .ra_place = {12, 4},
    .feat = {.t32 = 1},
};

TH_TABLE(TH_MLA, "mla", {&SHAPE_MLA, 0xfb000000, NULL});
TH_TABLE(TH_MLS, "mls", {&SHAPE_MLA, 0xfb000010, NULL});

/* ───── UMULL / UMLAL / SMULL / SMLAL (T32 only) ───── */

static thumb_opcode long_mul_emit(uint32_t base, const thop_args *a)
{
  uint32_t op = base | (a->rd << 8) | (a->rn << 16) | (a->rm << 0) | (a->ra << 12);
  return (thumb_opcode){.size = 4, .opcode = op};
}

static const thop_variant_shape SHAPE_LONG_MUL = {
    .size = THOP_VARIANT_T32,
    .rd_place = {8, 4},
    .rn_place = {0, 4},
    .rm_place = {16, 4},
    .ra_place = {12, 4},
    .feat = {.t32 = 1},
};

TH_TABLE(TH_UMULL, "umull", {&SHAPE_LONG_MUL, 0xfba00000, long_mul_emit});
TH_TABLE(TH_UMLAL, "umlal", {&SHAPE_LONG_MUL, 0xfbe00000, long_mul_emit});
TH_TABLE(TH_SMULL, "smull", {&SHAPE_LONG_MUL, 0xfb800000, long_mul_emit});
TH_TABLE(TH_SMLAL, "smlal", {&SHAPE_LONG_MUL, 0xfbc00000, long_mul_emit});

/* ───── SDIV / UDIV (T32 only) ───── */

static const thop_variant_shape SHAPE_DIV = {
    .size = THOP_VARIANT_T32,
    .rd_place = {8, 4},
    .rn_place = {16, 4},
    .rm_place = {0, 4},
    .feat = {.t32 = 1, .div = 1},
};

TH_TABLE(TH_UDIV, "udiv", {&SHAPE_DIV, 0xfbb0f0f0, NULL});
TH_TABLE(TH_SDIV, "sdiv", {&SHAPE_DIV, 0xfb90f0f0, NULL});

/* ═══════════════════════════════════════════════════════════════════
 *  Public wrappers
 * ═══════════════════════════════════════════════════════════════════ */

thumb_opcode th_mul(uint32_t rd, uint32_t rn, uint32_t rm, thumb_flags_behaviour flags,
                        thumb_enforce_encoding encoding)
{
  (void)flags;
  if (encoding == ENFORCE_ENCODING_32BIT || rd > 7 || rm > 7 || rn > 7 || rd != rm)
    return thop_emit(TH_MUL_T32.name, TH_MUL_T32.variants, TH_MUL_T32.variant_count,
                     (thop_args){.rd = rd, .rm = rm, .rn = rn});
  return thop_emit(TH_MUL_T16.name, TH_MUL_T16.variants, TH_MUL_T16.variant_count,
                   (thop_args){.rd = rd, .rm = rn});
}

thumb_opcode th_mla(uint32_t rd, uint32_t rn, uint32_t rm, uint32_t ra)
{
  return thop_emit(TH_MLA.name, TH_MLA.variants, TH_MLA.variant_count,
                   (thop_args){.rd = rd, .rn = rn, .rm = rm, .ra = ra});
}

thumb_opcode th_mls(uint32_t rd, uint32_t rn, uint32_t rm, uint32_t ra)
{
  return thop_emit(TH_MLS.name, TH_MLS.variants, TH_MLS.variant_count,
                   (thop_args){.rd = rd, .rn = rn, .rm = rm, .ra = ra});
}

thumb_opcode th_umull(uint32_t rdlo, uint32_t rdhi, uint32_t rn, uint32_t rm)
{
  return thop_emit(TH_UMULL.name, TH_UMULL.variants, TH_UMULL.variant_count,
                   (thop_args){.rd = rdhi, .rn = rn, .rm = rm, .ra = rdlo});
}

thumb_opcode th_umlal(uint32_t rdlo, uint32_t rdhi, uint32_t rn, uint32_t rm)
{
  return thop_emit(TH_UMLAL.name, TH_UMLAL.variants, TH_UMLAL.variant_count,
                   (thop_args){.rd = rdhi, .rn = rn, .rm = rm, .ra = rdlo});
}

thumb_opcode th_smull(uint32_t rdlo, uint32_t rdhi, uint32_t rn, uint32_t rm)
{
  return thop_emit(TH_SMULL.name, TH_SMULL.variants, TH_SMULL.variant_count,
                   (thop_args){.rd = rdhi, .rn = rn, .rm = rm, .ra = rdlo});
}

thumb_opcode th_smlal(uint32_t rdlo, uint32_t rdhi, uint32_t rn, uint32_t rm)
{
  return thop_emit(TH_SMLAL.name, TH_SMLAL.variants, TH_SMLAL.variant_count,
                   (thop_args){.rd = rdhi, .rn = rn, .rm = rm, .ra = rdlo});
}

thumb_opcode th_udiv(uint16_t rd, uint16_t rn, uint16_t rm)
{
  return thop_emit(TH_UDIV.name, TH_UDIV.variants, TH_UDIV.variant_count,
                   (thop_args){.rd = rd, .rn = rn, .rm = rm});
}

thumb_opcode th_sdiv(uint16_t rd, uint16_t rn, uint16_t rm)
{
  return thop_emit(TH_SDIV.name, TH_SDIV.variants, TH_SDIV.variant_count,
                   (thop_args){.rd = rd, .rn = rn, .rm = rm});
}
