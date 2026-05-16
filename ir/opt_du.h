/*
 *  TCC IR - Def-Use Table (shared pre-SSA optimization helper)
 *
 *  Copyright (c) 2025 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#ifndef TCC_IR_OPT_DU_H
#define TCC_IR_OPT_DU_H

#include <stdint.h>

struct TCCIRState;

/* ============================================================================
 * Def-Use Table: O(n) pre-computation enabling O(1) def/use queries
 * ============================================================================
 * Replaces tcc_ir_find_defining_instruction() (O(n) backward scan) and
 * inline O(n) use-count loops used in the fusion passes.
 *
 * Memory: (4+1) bytes x total_vregs.  For a typical embedded function with
 * ~90 total vregs that is ~450 bytes -- much less than existing pass allocs
 * such as sl_forward (32xn bytes).
 *
 * Layout in one allocation:
 *   int def[max_var + max_tmp + max_param]   (4 B each, -1 = no def)
 *   uint8_t use[max_var + max_tmp + max_param] (1 B each, saturates at 2)
 *
 * Vreg flat index:
 *   VAR   pos  ->  pos
 *   TMP   pos  ->  max_var + pos
 *   PARAM pos  ->  max_var + max_tmp + pos
 */
typedef struct IROptDU
{
  int *def;
  uint8_t *use;
  int max_var;
  int max_tmp;
  int total;
} IROptDU;

int ir_opt_du_idx(const IROptDU *du, int32_t vreg);

/* Build def and use tables in a single O(n) forward pass.
 * Call tcc_free(du.def) when done -- single allocation covers both arrays. */
void ir_opt_du_build(struct TCCIRState *ir, IROptDU *du);

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

#endif /* TCC_IR_OPT_DU_H */