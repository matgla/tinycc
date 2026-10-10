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

thumb_opcode mul_t16_emit(uint32_t base, const thop_args *a)
{
  uint32_t op = base | ((a->rd & 7) << 0) | ((a->rm & 7) << 3);
  return (thumb_opcode){.size = 2, .opcode = op};
}

TH_TABLE(TH_MUL_T16, "muls", {THOP_SHAPE_MUL_T16, 0x4340, THOP_CUSTOM_mul_t16_emit});
TH_TABLE(TH_MUL_T32, "mul", {THOP_SHAPE_MUL_T32, 0xfb00f000, 0});

/* ───── MLA / MLS (T32 only) ───── */

TH_TABLE(TH_MLA, "mla", {THOP_SHAPE_MLA, 0xfb000000, 0});
TH_TABLE(TH_MLS, "mls", {THOP_SHAPE_MLA, 0xfb000010, 0});

/* ───── UMULL / UMLAL / SMULL / SMLAL (T32 only) ───── */

thumb_opcode long_mul_emit(uint32_t base, const thop_args *a)
{
  uint32_t op = base | (a->rd << 8) | (a->rn << 16) | (a->rm << 0) | (a->ra << 12);
  return (thumb_opcode){.size = 4, .opcode = op};
}

TH_TABLE(TH_UMULL, "umull", {THOP_SHAPE_LONG_MUL, 0xfba00000, THOP_CUSTOM_long_mul_emit});
TH_TABLE(TH_UMLAL, "umlal", {THOP_SHAPE_LONG_MUL, 0xfbe00000, THOP_CUSTOM_long_mul_emit});
TH_TABLE(TH_SMULL, "smull", {THOP_SHAPE_LONG_MUL, 0xfb800000, THOP_CUSTOM_long_mul_emit});
TH_TABLE(TH_SMLAL, "smlal", {THOP_SHAPE_LONG_MUL, 0xfbc00000, THOP_CUSTOM_long_mul_emit});

TH_TABLE(TH_UMAAL, "umaal", {THOP_SHAPE_LONG_MUL_DSP, 0xfbe00060, THOP_CUSTOM_long_mul_emit});

/* ───── SDIV / UDIV (T32 only) ───── */

TH_TABLE(TH_UDIV, "udiv", {THOP_SHAPE_DIV, 0xfbb0f0f0, 0});
TH_TABLE(TH_SDIV, "sdiv", {THOP_SHAPE_DIV, 0xfb90f0f0, 0});

/* ═══════════════════════════════════════════════════════════════════
 *  Public wrappers
 * ═══════════════════════════════════════════════════════════════════ */

thumb_opcode th_mul(uint32_t rd, uint32_t rn, uint32_t rm, thumb_flags_behaviour flags,
                        thumb_enforce_encoding encoding)
{
  /* T16 MULS <Rdm>,<Rn>,<Rdm> computes Rdm = Rn * Rdm, so the destination must
     be one of the sources.  MUL is commutative, so either rd == rm or rd == rn
     is encodable - the latter simply swaps which operand plays Rn.  The T16
     form has an implicit S bit, so it is only usable when the caller does not
     need NZCV preserved. */
  bool t16_possible = encoding != ENFORCE_ENCODING_32BIT && rd <= 7 && rn <= 7 && rm <= 7 &&
                      flags != FLAGS_BEHAVIOUR_BLOCK;
  if (t16_possible && (rd == rm || rd == rn))
  {
    uint32_t other = (rd == rm) ? rn : rm;
    return thop_emit_table(&TH_MUL_T16, (thop_args){.rd = rd, .rm = other, .flags = flags, .enc = encoding});
  }
  return thop_emit_table(&TH_MUL_T32, (thop_args){.rd = rd, .rm = rm, .rn = rn, .flags = flags, .enc = encoding});
}

thumb_opcode th_mla(uint32_t rd, uint32_t rn, uint32_t rm, uint32_t ra)
{
  return thop_emit_table(&TH_MLA, (thop_args){.rd = rd, .rn = rn, .rm = rm, .ra = ra});
}

thumb_opcode th_mls(uint32_t rd, uint32_t rn, uint32_t rm, uint32_t ra)
{
  return thop_emit_table(&TH_MLS, (thop_args){.rd = rd, .rn = rn, .rm = rm, .ra = ra});
}

thumb_opcode th_umull(uint32_t rdlo, uint32_t rdhi, uint32_t rn, uint32_t rm)
{
  return thop_emit_table(&TH_UMULL, (thop_args){.rd = rdhi, .rn = rn, .rm = rm, .ra = rdlo});
}

thumb_opcode th_umlal(uint32_t rdlo, uint32_t rdhi, uint32_t rn, uint32_t rm)
{
  return thop_emit_table(&TH_UMLAL, (thop_args){.rd = rdhi, .rn = rn, .rm = rm, .ra = rdlo});
}

thumb_opcode th_umaal(uint32_t rdlo, uint32_t rdhi, uint32_t rn, uint32_t rm)
{
  return thop_emit_table(&TH_UMAAL, (thop_args){.rd = rdhi, .rn = rn, .rm = rm, .ra = rdlo});
}

thumb_opcode th_smull(uint32_t rdlo, uint32_t rdhi, uint32_t rn, uint32_t rm)
{
  return thop_emit_table(&TH_SMULL, (thop_args){.rd = rdhi, .rn = rn, .rm = rm, .ra = rdlo});
}

thumb_opcode th_smlal(uint32_t rdlo, uint32_t rdhi, uint32_t rn, uint32_t rm)
{
  return thop_emit_table(&TH_SMLAL, (thop_args){.rd = rdhi, .rn = rn, .rm = rm, .ra = rdlo});
}

thumb_opcode th_udiv(uint16_t rd, uint16_t rn, uint16_t rm)
{
  return thop_emit_table(&TH_UDIV, (thop_args){.rd = rd, .rn = rn, .rm = rm});
}

thumb_opcode th_sdiv(uint16_t rd, uint16_t rn, uint16_t rm)
{
  return thop_emit_table(&TH_SDIV, (thop_args){.rd = rd, .rn = rn, .rm = rm});
}
