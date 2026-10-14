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

#include "thop_system.h"
#include "thumb.h"

/* ═══════════════════════════════════════════════════════════════════
 *  System hints, barriers, exceptions, status
 * ═══════════════════════════════════════════════════════════════════ */

/* ───── Hint instructions (NOP, SEV, WFE, WFI, YIELD) ───── */

TH_TABLE(TH_NOP_T16, "nop", {THOP_SHAPE_HINT_T16, 0xbf00, 0});
TH_TABLE(TH_NOP_T32, "nop.w", {THOP_SHAPE_HINT_T32, 0xf3af8000, 0});

TH_TABLE(TH_SEV_T16, "sev", {THOP_SHAPE_HINT_T16, 0xbf40, 0});
TH_TABLE(TH_SEV_T32, "sev.w", {THOP_SHAPE_HINT_T32, 0xf3af8004, 0});

TH_TABLE(TH_WFE_T16, "wfe", {THOP_SHAPE_HINT_T16, 0xbf20, 0});
TH_TABLE(TH_WFE_T32, "wfe.w", {THOP_SHAPE_HINT_T32, 0xf3af8002, 0});

TH_TABLE(TH_WFI_T16, "wfi", {THOP_SHAPE_HINT_T16, 0xbf30, 0});
TH_TABLE(TH_WFI_T32, "wfi.w", {THOP_SHAPE_HINT_T32, 0xf3af8003, 0});

TH_TABLE(TH_YIELD_T16, "yield", {THOP_SHAPE_HINT_T16, 0xbf10, 0});
TH_TABLE(TH_YIELD_T32, "yield.w", {THOP_SHAPE_HINT_T32, 0xf3af8001, 0});

/* ───── SVC / BKPT (T16 only, imm8) ───── */

TH_TABLE(TH_SVC, "svc", {THOP_SHAPE_IMM8_T16, 0xdf00, 0});
TH_TABLE(TH_BKPT, "bkpt", {THOP_SHAPE_IMM8_T16, 0xbe00, 0});

/* ───── UDF (T16 imm8, T32 imm12+imm4) ───── */

TH_TABLE(TH_UDF_T16, "udf", {THOP_SHAPE_UDF_T16, 0xde00, 0});
TH_TABLE(TH_UDF_T32, "udf.w", {THOP_SHAPE_UDF_T32, 0xf7f0a000, 0});

/* ───── CPS (T16 only) ───── */

TH_TABLE(TH_CPS, "cps", {THOP_SHAPE_CPS, 0xb660, 0});

/* ───── CLREX, CSDB, DMB, DSB, ISB, SSBB (T32 only) ───── */

TH_TABLE(TH_CLREX, "clrex", {THOP_SHAPE_NOARG_T32, 0xf3bf8f2f, 0});
TH_TABLE(TH_CSDB, "csdb", {THOP_SHAPE_NOARG_T32, 0xf3af8014, 0});
TH_TABLE(TH_DMB, "dmb", {THOP_SHAPE_BARRIER, 0xf3bf8f50, 0});
TH_TABLE(TH_DSB, "dsb", {THOP_SHAPE_BARRIER, 0xf3bf8f40, 0});
TH_TABLE(TH_ISB, "isb", {THOP_SHAPE_BARRIER, 0xf3bf8f60, 0});
TH_TABLE(TH_SSBB, "ssbb", {THOP_SHAPE_NOARG_T32, 0xf3bf8f40, 0});

/* ───── IT (T16 only) ───── */

TH_TABLE(TH_IT, "it", {THOP_SHAPE_IT, 0xbf00, 0});

/* ───── CLZ (T32 only) ───── */

thumb_opcode clz_emit(uint32_t base, const thop_args *a)
{
  uint32_t op = base | (a->rm << 16) | (a->rd << 8) | a->rm;
  return (thumb_opcode){.size = 4, .opcode = op};
}

TH_TABLE(TH_CLZ, "clz", {THOP_SHAPE_CLZ, 0xfab0f080, THOP_CUSTOM_clz_emit});

/* ═══════════════════════════════════════════════════════════════════
 *  Public wrappers
 * ═══════════════════════════════════════════════════════════════════ */

thumb_opcode th_nop(thumb_enforce_encoding encoding)
{
  if (encoding == ENFORCE_ENCODING_32BIT)
    return thop_emit_table(&TH_NOP_T32, (thop_args){});
  return thop_emit_table(&TH_NOP_T16, (thop_args){});
}

thumb_opcode th_sev(thumb_enforce_encoding encoding)
{
  if (encoding == ENFORCE_ENCODING_32BIT)
    return thop_emit_table(&TH_SEV_T32, (thop_args){});
  return thop_emit_table(&TH_SEV_T16, (thop_args){});
}

thumb_opcode th_wfe(thumb_enforce_encoding encoding)
{
  if (encoding == ENFORCE_ENCODING_32BIT)
    return thop_emit_table(&TH_WFE_T32, (thop_args){});
  return thop_emit_table(&TH_WFE_T16, (thop_args){});
}

thumb_opcode th_wfi(thumb_enforce_encoding encoding)
{
  if (encoding == ENFORCE_ENCODING_32BIT)
    return thop_emit_table(&TH_WFI_T32, (thop_args){});
  return thop_emit_table(&TH_WFI_T16, (thop_args){});
}

thumb_opcode th_yield(thumb_enforce_encoding encoding)
{
  if (encoding == ENFORCE_ENCODING_32BIT)
    return thop_emit_table(&TH_YIELD_T32, (thop_args){});
  return thop_emit_table(&TH_YIELD_T16, (thop_args){});
}

thumb_opcode th_svc(uint32_t imm)
{
  return thop_emit_table(&TH_SVC, (thop_args){.imm = imm});
}

thumb_opcode th_bkpt(uint32_t imm)
{
  return thop_emit_table(&TH_BKPT, (thop_args){.imm = imm});
}

thumb_opcode th_udf(uint32_t imm, thumb_enforce_encoding encoding)
{
  if (encoding != ENFORCE_ENCODING_32BIT && imm <= 0xff)
    return thop_emit_table(&TH_UDF_T16, (thop_args){.imm = imm});
  return thop_emit_table(&TH_UDF_T32, (thop_args){.imm = imm & 0xfff, .imm2 = (imm >> 12) & 0xf});
}

thumb_opcode th_cps(uint32_t enable, uint32_t i, uint32_t f)
{
  return thop_emit_table(&TH_CPS, (thop_args){.rd = enable, .imm = f, .imm2 = i});
}

thumb_opcode th_clrex()
{
  return thop_emit_table(&TH_CLREX, (thop_args){});
}

thumb_opcode th_csdb()
{
  return thop_emit_table(&TH_CSDB, (thop_args){});
}

thumb_opcode th_dmb(uint32_t option)
{
  return thop_emit_table(&TH_DMB, (thop_args){.imm = option});
}

thumb_opcode th_dsb(uint32_t option)
{
  return thop_emit_table(&TH_DSB, (thop_args){.imm = option});
}

thumb_opcode th_isb(uint32_t option)
{
  return thop_emit_table(&TH_ISB, (thop_args){.imm = option});
}

thumb_opcode th_ssbb()
{
  return thop_emit_table(&TH_SSBB, (thop_args){});
}

thumb_opcode th_it(uint16_t cond, uint16_t mask)
{
  return thop_emit_table(&TH_IT, (thop_args){.rd = cond, .imm = mask});
}

thumb_opcode th_clz(uint32_t rd, uint32_t rm)
{
  return thop_emit_table(&TH_CLZ, (thop_args){.rd = rd, .rm = rm});
}
