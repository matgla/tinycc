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

/* Flat index: VAR pos -> pos; TMP -> max_var + pos; PARAM -> max_var + max_tmp + pos. */
int ir_opt_du_idx(const IROptDU *du, int32_t vreg);

/* O(n) build (IR_DU_MODE_FULL); tcc_free(du.def) frees all three arrays (one allocation). */
void ir_opt_du_build(struct TCCIRState *ir, IROptDU *du);

/* TMP_ONLY skips VAR/PARAM tracking for lower memory usage. */
void ir_opt_du_build_mode(struct TCCIRState *ir, IROptDU *du, uint8_t mode);

/* Def index strictly before before_idx; -1 when undefined or defined later. */
/* (These four are out of line in analysis/du_chains.c.) */
int ir_opt_du_def(const IROptDU *du, int32_t vreg, int before_idx);

/* Saturating: 0, 1, or 2 meaning "2 or more". */
int ir_opt_du_uses(const IROptDU *du, int32_t vreg);

/* Saturating: 0, 1, or 2 meaning "2 or more". */
int ir_opt_du_def_count(const IROptDU *du, int32_t vreg);

int ir_opt_du_is_single_def(const IROptDU *du, int32_t vreg);

/* Indexed [vreg_type * stride + vreg_position], stride via *out_stride; caller tcc_free()s. */
uint8_t *ir_opt_build_def_count(struct TCCIRState *ir, int n, int *out_stride);

#define DC_IS_SINGLE_DEF(dc, stride, vr)                                                                               \
  ((vr) >= 0 && (dc)[TCCIR_DECODE_VREG_TYPE(vr) * (stride) + TCCIR_DECODE_VREG_POSITION(vr)] == 1)
