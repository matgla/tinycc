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

static const thop_variant_shape SHAPE_HINT_T16 = {
    .size = THOP_VARIANT_T16,
    .imm = {.kind = IMM_RAW, .width = 4},
    .imm_place = {4, 4},
    .feat = {.t16 = 1},
};

static const thop_variant_shape SHAPE_HINT_T32 = {
    .size = THOP_VARIANT_T32,
    .imm = {.kind = IMM_RAW, .width = 4},
    .imm_place = {0, 4},
    .feat = {.t32 = 1},
};

TH_TABLE(TH_NOP_T16, "nop", {&SHAPE_HINT_T16, 0xbf00, NULL});
TH_TABLE(TH_NOP_T32, "nop.w", {&SHAPE_HINT_T32, 0xf3af8000, NULL});

TH_TABLE(TH_SEV_T16, "sev", {&SHAPE_HINT_T16, 0xbf40, NULL});
TH_TABLE(TH_SEV_T32, "sev.w", {&SHAPE_HINT_T32, 0xf3af8004, NULL});

TH_TABLE(TH_WFE_T16, "wfe", {&SHAPE_HINT_T16, 0xbf20, NULL});
TH_TABLE(TH_WFE_T32, "wfe.w", {&SHAPE_HINT_T32, 0xf3af8002, NULL});

TH_TABLE(TH_WFI_T16, "wfi", {&SHAPE_HINT_T16, 0xbf30, NULL});
TH_TABLE(TH_WFI_T32, "wfi.w", {&SHAPE_HINT_T32, 0xf3af8003, NULL});

TH_TABLE(TH_YIELD_T16, "yield", {&SHAPE_HINT_T16, 0xbf10, NULL});
TH_TABLE(TH_YIELD_T32, "yield.w", {&SHAPE_HINT_T32, 0xf3af8001, NULL});

/* ───── SVC / BKPT (T16 only, imm8) ───── */

static const thop_variant_shape SHAPE_IMM8_T16 = {
    .size = THOP_VARIANT_T16,
    .imm = {.kind = IMM_RAW, .width = 8},
    .imm_place = {0, 8},
    .feat = {.t16 = 1},
};

TH_TABLE(TH_SVC, "svc", {&SHAPE_IMM8_T16, 0xdf00, NULL});
TH_TABLE(TH_BKPT, "bkpt", {&SHAPE_IMM8_T16, 0xbe00, NULL});

/* ───── UDF (T16 imm8, T32 imm12+imm4) ───── */

static const thop_variant_shape SHAPE_UDF_T16 = {
    .size = THOP_VARIANT_T16,
    .imm = {.kind = IMM_RAW, .width = 8},
    .imm_place = {0, 8},
    .feat = {.t16 = 1},
};

static const thop_variant_shape SHAPE_UDF_T32 = {
    .size = THOP_VARIANT_T32,
    .imm = {.kind = IMM_RAW, .width = 12},
    .imm_place = {0, 12},
    .imm2_place = {16, 4},
    .feat = {.t32 = 1},
};

TH_TABLE(TH_UDF_T16, "udf", {&SHAPE_UDF_T16, 0xde00, NULL});
TH_TABLE(TH_UDF_T32, "udf.w", {&SHAPE_UDF_T32, 0xf7f0a000, NULL});

/* ───── CPS (T16 only) ───── */

static const thop_variant_shape SHAPE_CPS = {
    .size = THOP_VARIANT_T16,
    .rd_place = {4, 1},
    .imm = {.kind = IMM_RAW, .width = 1},
    .imm_place = {0, 1},
    .imm2_place = {1, 1},
    .feat = {.t16 = 1},
};

TH_TABLE(TH_CPS, "cps", {&SHAPE_CPS, 0xb660, NULL});

/* ───── CLREX, CSDB, DMB, DSB, ISB, SSBB (T32 only) ───── */

static const thop_variant_shape SHAPE_BARRIER = {
    .size = THOP_VARIANT_T32,
    .imm = {.kind = IMM_RAW, .width = 4},
    .imm_place = {0, 4},
    .feat = {.t32 = 1},
};

static const thop_variant_shape SHAPE_NOARG_T32 = {
    .size = THOP_VARIANT_T32,
    .feat = {.t32 = 1},
};

TH_TABLE(TH_CLREX, "clrex", {&SHAPE_NOARG_T32, 0xf3bf8f2f, NULL});
TH_TABLE(TH_CSDB, "csdb", {&SHAPE_NOARG_T32, 0xf3af8014, NULL});
TH_TABLE(TH_DMB, "dmb", {&SHAPE_BARRIER, 0xf3bf8f50, NULL});
TH_TABLE(TH_DSB, "dsb", {&SHAPE_BARRIER, 0xf3bf8f40, NULL});
TH_TABLE(TH_ISB, "isb", {&SHAPE_BARRIER, 0xf3bf8f60, NULL});
TH_TABLE(TH_SSBB, "ssbb", {&SHAPE_NOARG_T32, 0xf3bf8f40, NULL});

/* ───── IT (T16 only) ───── */

static const thop_variant_shape SHAPE_IT = {
    .size = THOP_VARIANT_T16,
    .rd_place = {4, 4},
    .imm = {.kind = IMM_RAW, .width = 4},
    .imm_place = {0, 4},
    .feat = {.t16 = 1},
};

TH_TABLE(TH_IT, "it", {&SHAPE_IT, 0xbf00, NULL});

/* ───── CLZ (T32 only) ───── */

static thumb_opcode clz_emit(uint32_t base, const thop_args *a)
{
  uint32_t op = base | (a->rm << 16) | (a->rd << 8) | a->rm;
  return (thumb_opcode){.size = 4, .opcode = op};
}

static const thop_variant_shape SHAPE_CLZ = {
    .size = THOP_VARIANT_T32,
    .rd_place = {8, 4},
    .rm_place = {16, 4},
    .feat = {.t32 = 1, .clz_rbit = 1},
};

TH_TABLE(TH_CLZ, "clz", {&SHAPE_CLZ, 0xfab0f080, clz_emit});

/* ═══════════════════════════════════════════════════════════════════
 *  Public wrappers
 * ═══════════════════════════════════════════════════════════════════ */

thumb_opcode th_nop(thumb_enforce_encoding encoding)
{
  if (encoding == ENFORCE_ENCODING_32BIT)
    return thop_emit(TH_NOP_T32.name, TH_NOP_T32.variants, TH_NOP_T32.variant_count, (thop_args){});
  return thop_emit(TH_NOP_T16.name, TH_NOP_T16.variants, TH_NOP_T16.variant_count, (thop_args){});
}

thumb_opcode th_sev(thumb_enforce_encoding encoding)
{
  if (encoding == ENFORCE_ENCODING_32BIT)
    return thop_emit(TH_SEV_T32.name, TH_SEV_T32.variants, TH_SEV_T32.variant_count, (thop_args){});
  return thop_emit(TH_SEV_T16.name, TH_SEV_T16.variants, TH_SEV_T16.variant_count, (thop_args){});
}

thumb_opcode th_wfe(thumb_enforce_encoding encoding)
{
  if (encoding == ENFORCE_ENCODING_32BIT)
    return thop_emit(TH_WFE_T32.name, TH_WFE_T32.variants, TH_WFE_T32.variant_count, (thop_args){});
  return thop_emit(TH_WFE_T16.name, TH_WFE_T16.variants, TH_WFE_T16.variant_count, (thop_args){});
}

thumb_opcode th_wfi(thumb_enforce_encoding encoding)
{
  if (encoding == ENFORCE_ENCODING_32BIT)
    return thop_emit(TH_WFI_T32.name, TH_WFI_T32.variants, TH_WFI_T32.variant_count, (thop_args){});
  return thop_emit(TH_WFI_T16.name, TH_WFI_T16.variants, TH_WFI_T16.variant_count, (thop_args){});
}

thumb_opcode th_yield(thumb_enforce_encoding encoding)
{
  if (encoding == ENFORCE_ENCODING_32BIT)
    return thop_emit(TH_YIELD_T32.name, TH_YIELD_T32.variants, TH_YIELD_T32.variant_count, (thop_args){});
  return thop_emit(TH_YIELD_T16.name, TH_YIELD_T16.variants, TH_YIELD_T16.variant_count, (thop_args){});
}

thumb_opcode th_svc(uint32_t imm)
{
  return thop_emit(TH_SVC.name, TH_SVC.variants, TH_SVC.variant_count, (thop_args){.imm = imm});
}

thumb_opcode th_bkpt(uint32_t imm)
{
  return thop_emit(TH_BKPT.name, TH_BKPT.variants, TH_BKPT.variant_count, (thop_args){.imm = imm});
}

thumb_opcode th_udf(uint32_t imm, thumb_enforce_encoding encoding)
{
  if (encoding != ENFORCE_ENCODING_32BIT && imm <= 0xff)
    return thop_emit(TH_UDF_T16.name, TH_UDF_T16.variants, TH_UDF_T16.variant_count, (thop_args){.imm = imm});
  return thop_emit(TH_UDF_T32.name, TH_UDF_T32.variants, TH_UDF_T32.variant_count,
                   (thop_args){.imm = imm & 0xfff, .imm2 = (imm >> 12) & 0xf});
}

thumb_opcode th_cps(uint32_t enable, uint32_t i, uint32_t f)
{
  return thop_emit(TH_CPS.name, TH_CPS.variants, TH_CPS.variant_count,
                   (thop_args){.rd = enable, .imm = f, .imm2 = i});
}

thumb_opcode th_clrex()
{
  return thop_emit(TH_CLREX.name, TH_CLREX.variants, TH_CLREX.variant_count, (thop_args){});
}

thumb_opcode th_csdb()
{
  return thop_emit(TH_CSDB.name, TH_CSDB.variants, TH_CSDB.variant_count, (thop_args){});
}

thumb_opcode th_dmb(uint32_t option)
{
  return thop_emit(TH_DMB.name, TH_DMB.variants, TH_DMB.variant_count, (thop_args){.imm = option});
}

thumb_opcode th_dsb(uint32_t option)
{
  return thop_emit(TH_DSB.name, TH_DSB.variants, TH_DSB.variant_count, (thop_args){.imm = option});
}

thumb_opcode th_isb(uint32_t option)
{
  return thop_emit(TH_ISB.name, TH_ISB.variants, TH_ISB.variant_count, (thop_args){.imm = option});
}

thumb_opcode th_ssbb()
{
  return thop_emit(TH_SSBB.name, TH_SSBB.variants, TH_SSBB.variant_count, (thop_args){});
}

thumb_opcode th_it(uint16_t cond, uint16_t mask)
{
  return thop_emit(TH_IT.name, TH_IT.variants, TH_IT.variant_count,
                   (thop_args){.rd = cond, .imm = mask});
}

thumb_opcode th_clz(uint32_t rd, uint32_t rm)
{
  return thop_emit(TH_CLZ.name, TH_CLZ.variants, TH_CLZ.variant_count, (thop_args){.rd = rd, .rm = rm});
}
