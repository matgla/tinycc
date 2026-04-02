/*
 *  TCC - Tiny C Compiler
 *
 *  Copyright (c) 2025 Mateusz Stadnik
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

#ifndef _TCC_TARGET_H
#define _TCC_TARGET_H

#include <stdbool.h>
#include <stdint.h>

typedef struct FloatingPointConfig
{
  int8_t reg_size;
  int8_t reg_count;
  int8_t stack_align;
  int32_t has_fadd : 1;
  int32_t has_fsub : 1;
  int32_t has_fmul : 1;
  int32_t has_fdiv : 1;
  int32_t has_fcmp : 1;
  int32_t has_ftof : 1;
  int32_t has_itof : 1;
  int32_t has_ftod : 1;
  int32_t has_ftoi : 1;
  int32_t has_dadd : 1;
  int32_t has_dsub : 1;
  int32_t has_dmul : 1;
  int32_t has_ddiv : 1;
  int32_t has_dcmp : 1;
  int32_t has_dtof : 1;
  int32_t has_itod : 1;
  int32_t has_dtoi : 1;
  int32_t has_ltod : 1;
  int32_t has_ltof : 1;
  int32_t has_dtol : 1;
  int32_t has_ftol : 1;
  int32_t has_fneg : 1;
  int32_t has_dneg : 1;
  uint64_t fpu_feat;
} FloatingPointConfig;

/* Forward-declared; full definition lives in the active backend header
 * (e.g. arch/arm/arm.h).  Generic code never dereferences this pointer — it
 * is opaque outside the backend. */
struct target_dependent_config;

typedef struct ArchitectureConfig
{
  const FloatingPointConfig *fpu;
  const char *march_name;
  struct target_dependent_config *target_dependent;

  uint8_t pointer_size : 4; /* 4 or 8 */
  uint8_t stack_align : 4;  /* 4 or 8 */
  uint8_t reg_size : 4;     /* 4 or 8 */
  uint8_t parameter_registers : 4;
  uint8_t default_align : 4;    /* 1/2/4/8 */
  uint8_t static_chain_reg : 5; /* register index 0-31 */
  uint8_t int_reg_count : 5;    /* 0-31 */
  uint8_t fp_reg_count : 7;     /* 0-127 */
  uint8_t has_fpu : 1;
  uint8_t big_endian : 1;
} ArchitectureConfig;

extern ArchitectureConfig architecture_config;

/* ───── Generic target capability query (§10.2 / §11.2) ─────
 *
 * Tiny generic wrapper, no ARM knowledge.  Included by tccgen.c,
 * ir/, tccls.c, tccelf.c, etc.  The active backend provides the
 * implementation (currently in arch/arm/arm.c).
 */

typedef enum
{
  TCC_CAP_HW_DIVIDE,
  TCC_CAP_HW_FP_SP,
  TCC_CAP_HW_FP_DP,
  TCC_CAP_HW_FP_HP,
  TCC_CAP_DSP_SIMD,
  TCC_CAP_SATURATING_ARITH,
  TCC_CAP_BITFIELD_INSTRS,
  TCC_CAP_COND_EXEC,     /* IT blocks / conditional moves */
  TCC_CAP_MOVE_IMM_WIDE, /* movw/movt */
  TCC_CAP_VECTOR,        /* MVE / NEON-like */
  TCC_CAP_SECURITY,      /* TrustZone-M / CMSE */
  TCC_CAP_POINTER_AUTH,  /* PACBTI */
  TCC_CAP_LOW_OVERHEAD_LOOP,
  /* extended as generic code grows new branches */
} tcc_target_cap;

bool tcc_target_has(tcc_target_cap cap);

/* ───── Inline getters ─────
 *
 * These read from architecture_config and are inlined here so
 * generic code pays zero call overhead for frequent queries.
 * Requires architecture_config to be declared before this header
 * is included (tcc.h arranges this).
 */

static inline int tcc_target_ptr_size(void)
{
  return architecture_config.pointer_size;
}

static inline int tcc_target_int_reg_count(void)
{
  return architecture_config.int_reg_count;
}

static inline int tcc_target_fp_reg_count(void)
{
  return architecture_config.fp_reg_count;
}

static inline int tcc_target_stack_align(void)
{
  return architecture_config.stack_align;
}

static inline int tcc_target_default_align(void)
{
  return architecture_config.default_align;
}

static inline bool tcc_target_big_endian(void)
{
  return architecture_config.big_endian != 0;
}

static inline const char *tcc_target_arch_name(void)
{
  return architecture_config.march_name;
}

#endif /* _TCC_TARGET_H */
