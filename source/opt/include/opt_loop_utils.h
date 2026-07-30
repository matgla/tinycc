/*
 *  TCC IR - Loop optimization utilities (pre-SSA)
 *
 *  Copyright (c) 2025 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#pragma once

#include "ir.h"
#include "licm.h"

#define MAX_IV 8
#define MAX_DIV 16
#define UNROLL_MAX_TRIP_COUNT 16
#define UNROLL_MAX_BODY_INSNS 32
#define UNROLL_MAX_TOTAL_INSNS 128

typedef struct InductionVar
{
  int vreg;
  int init_val;
  int step;
  int def_idx;
  int init_idx;
} InductionVar;

typedef struct DerivedIV
{
  int iv_idx;
  int base_vreg;
  IROperand base_op;
  int stride;
  int use_idx;
  int shl_idx;
  int share_with;
} DerivedIV;

/* IV analysis */
int find_induction_vars_ex(struct TCCIRState *ir, struct IRLoop *loop,
                           InductionVar *ivs, int max_ivs, int allow_copy_through);

int find_derived_ivs(struct TCCIRState *ir, struct IRLoop *loop,
                     InductionVar *ivs, int num_ivs,
                     DerivedIV *divs, int max_divs);

int transform_derived_iv(struct TCCIRState *ir, struct IRLoop *loop,
                         InductionVar *iv, DerivedIV *div,
                         int *out_ptr_vreg, int *out_idx_shift,
                         int *out_postnop_origpos, int *out_stride_pos,
                         int shared_ptr_vreg);

int iv_strength_reduction_core(struct TCCIRState *ir, struct IRLoops *loops);

int try_eliminate_iv_counter(struct TCCIRState *ir, struct IRLoop *loop,
                             InductionVar *iv, DerivedIV *div,
                             int ptr_vreg, int idx_shift);

/* Instruction insertion */
int insert_instr_at(struct TCCIRState *ir, int pos, TccIrOp op,
                    IROperand dest, IROperand src1, IROperand src2);

/* Loop exit analysis */
int find_loop_exit_condition(struct TCCIRState *ir, struct IRLoop *loop,
                             int iv_vreg, int *out_cmp_idx, int *out_jmpif_idx,
                             int *out_limit, int *out_cond, int *out_exit_target);

int find_loop_exit_condition_op(struct TCCIRState *ir, struct IRLoop *loop,
                                int iv_vreg, int *out_cmp_idx, int *out_jmpif_idx,
                                IROperand *out_limit_op, int *out_cond,
                                int *out_exit_target);

int compute_trip_count(int init_val, int limit, int step, int cond_token);

int collect_body_instructions(struct TCCIRState *ir, struct IRLoop *loop,
                              int iv_vreg, int cmp_idx, int jmpif_idx,
                              int iv_def_idx, int *body_indices, int max_body);

/* NOP-slot writers */
void write_instr_at_nop(struct TCCIRState *ir, int pos, TccIrOp op,
                        IROperand dest, IROperand src1, IROperand src2);

void write_select_at_nop(struct TCCIRState *ir, int pos, IROperand dest,
                         IROperand then_val, IROperand else_val,
                         int cond_tok);

/* Loop transforms */
int try_eliminate_loop(struct TCCIRState *ir, struct IRLoop *loop);
int try_eliminate_loop_symbolic(struct TCCIRState *ir, struct IRLoop *loop);
int try_unroll_loop_ex(struct TCCIRState *ir, struct IRLoop *loop,
                       struct IRLoops *loops, int loop_idx);
int try_rotate_loop(struct TCCIRState *ir, struct IRLoop *loop);
/* Placement-only sibling of rotation: moves a `for` body ahead of its
 * increment so the two bridging jumps become fall-throughs. */
int try_relayout_loop(struct TCCIRState *ir, struct IRLoop *loop);

/* Zero-trip entry-guard elimination for chains of sequential counted loops
 * (source/opt/flat/loop/seq_guard_elim.c).  Carries the IV constant forward in
 * program order (exit value = init + trip*step) so a rotated loop whose IV
 * enters as the previous loop's exit value can drop its `CMP/JUMPIF` guard.
 * Returns the number of guards removed. */
int tcc_ir_opt_loop_guard_elim(struct TCCIRState *ir);

/* Query side of the same walker: constant value of `vreg` on entry to
 * instruction `at_idx`, carried across preceding counted loops.  1 on success.
 * Rotation uses it to decide whether its guard will fold before committing. */
int tcc_ir_loop_seq_entry_const(struct TCCIRState *ir, int at_idx, int32_t vreg,
                                int64_t *out_val);

/* Rewrites a count-up pure-counter loop in [start,end] to count-down-to-zero; 1 if rewritten. */
int dtz_try_region(struct TCCIRState *ir, int start, int end, int header_idx,
                   int preheader_idx);

/* Misc helpers */
int signed_to_unsigned_cond(int cond_token);
int loop_size_cmp(const void *a, const void *b);
