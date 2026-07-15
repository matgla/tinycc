/*
 *  TCC IR - Def-Use Table (shared pre-SSA optimization helper)
 *
 *  Copyright (c) 2025 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#pragma once

#include <stdint.h>

struct TCCIRState;

/* ============================================================================
 * Def-Use Table: O(n) pre-computation enabling O(1) def/use queries
 * ============================================================================
 * Replaces tcc_ir_find_defining_instruction() (O(n) backward scan) and
 * inline O(n) use-count loops used in the fusion passes.
 *
 * Memory: (4+1+1) bytes x total_vregs.  For a typical embedded function with
 * ~90 total vregs that is ~540 bytes -- much less than existing pass allocs
 * such as sl_forward (32xn bytes).
 *
 * Layout in one allocation:
 *   int def[total]           (4 B each, -1 = no def, stores last def idx)
 *   uint8_t use[total]       (1 B each, saturates at 2)
 *   uint8_t def_cnt[total]   (1 B each, saturates at 2)
 *
 * Vreg flat index:
 *   VAR   pos  ->  pos
 *   TMP   pos  ->  max_var + pos
 *   PARAM pos  ->  max_var + max_tmp + pos
 *
 * Build modes:
 *   IR_DU_MODE_FULL     — track all vreg types (VAR + TMP + PARAM)
 *   IR_DU_MODE_TMP_ONLY — track only TMP vregs (lighter allocation)
 */

#define IR_DU_MODE_FULL     0
#define IR_DU_MODE_TMP_ONLY 1

typedef struct IROptDU
{
  int *def;
  uint8_t *use;
  uint8_t *def_cnt;
  int max_var;
  int max_tmp;
  int total;
  uint8_t mode;
} IROptDU;

int ir_opt_du_idx(const IROptDU *du, int32_t vreg);

/* Build def, use, and def_cnt tables in a single O(n) forward pass.
 * Uses IR_DU_MODE_FULL by default.
 * Call tcc_free(du.def) when done -- single allocation covers all arrays. */
void ir_opt_du_build(struct TCCIRState *ir, IROptDU *du);

/* Build with explicit mode (IR_DU_MODE_FULL or IR_DU_MODE_TMP_ONLY).
 * TMP_ONLY skips VAR/PARAM tracking for lower memory usage. */
void ir_opt_du_build_mode(struct TCCIRState *ir, IROptDU *du, uint8_t mode);

/* Defining instruction index for vreg that is strictly before before_idx.
 * Returns -1 when the vreg has no definition or its def is not before before_idx. */
static inline int ir_opt_du_def(const IROptDU *du, int32_t vreg, int before_idx)
{
  int idx = ir_opt_du_idx(du, vreg);
  if (idx < 0)
    return -1;
  int d = du->def[idx];
  return (d >= 0 && d < before_idx) ? d : -1;
}

/* Use count for vreg (0, 1, or 2 meaning "2 or more"). */
static inline int ir_opt_du_uses(const IROptDU *du, int32_t vreg)
{
  int idx = ir_opt_du_idx(du, vreg);
  return (idx >= 0) ? (int)du->use[idx] : 0;
}

/* Definition count for vreg (0, 1, or 2 meaning "2 or more"). */
static inline int ir_opt_du_def_count(const IROptDU *du, int32_t vreg)
{
  int idx = ir_opt_du_idx(du, vreg);
  return (idx >= 0) ? (int)du->def_cnt[idx] : 0;
}

/* Check if vreg has exactly one definition. */
static inline int ir_opt_du_is_single_def(const IROptDU *du, int32_t vreg)
{
  return ir_opt_du_def_count(du, vreg) == 1;
}

/* ============================================================================
 * Flat def-count table (lightweight alternative to full IROptDU)
 * ============================================================================
 * Returns allocated array indexed by [vreg_type * stride + vreg_position].
 * stride = max_vreg_position + 1.  Caller must tcc_free() the result. */
uint8_t *ir_opt_build_def_count(struct TCCIRState *ir, int n, int *out_stride);

#define DC_IS_SINGLE_DEF(dc, stride, vr)                                                                               \
  ((vr) >= 0 && (dc)[TCCIR_DECODE_VREG_TYPE(vr) * (stride) + TCCIR_DECODE_VREG_POSITION(vr)] == 1)
