/*
 *  TCC IR - Shared optimization utilities (pre-SSA)
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
struct IROperand;

int ir_opt_eval_const_u64(struct TCCIRState *ir, IROperand op, int use_idx,
                          uint64_t *out, int depth);

int ir_opt_eval_const_string(struct TCCIRState *ir, IROperand op, int use_idx,
                             const char **out, int depth);

/* *out receives the underlying symref (with resolved addend), to rebuild at a folded offset. */
int ir_opt_eval_const_string_operand(struct TCCIRState *ir, IROperand op,
                                     int use_idx, IROperand *out, int depth);

/* strlen of a string built byte-by-byte on the stack before call_idx; returns 1 + *out_len. */
int ir_opt_eval_stack_strlen(struct TCCIRState *ir, IROperand arg,
                             int call_idx, int *out_len);

/* Byte semantics. */
int ir_opt_fold_strcmp_result(const char *s1, const char *s2);
int ir_opt_fold_strncmp_result(const char *s1, const char *s2, uint64_t n);
int ir_opt_fold_memcmp_result(const char *s1, const char *s2, uint64_t n);
/* *out_offset = index of c within the first n bytes of s, or -1 when absent. */
int ir_opt_fold_memchr_offset(const char *s, unsigned char c, uint64_t n, int *out_offset);

int evaluate_compare_condition(int64_t val1, int64_t val2, int cond_token);

/* Applies signed/unsigned + width semantics derived from the operand types. */
int evaluate_compare_condition_cmp_operands(int64_t val1, int64_t val2, int cond,
                                            IROperand src1, IROperand src2);
/* IEEE NaN branch result for a soft-FP compare condition token: 0/1/-1. */
int nan_compare_branch_result(int cond_token);

struct IRQuadCompact;
int32_t ir_opt_mla_accum_vreg(const struct TCCIRState *ir, const struct IRQuadCompact *q);

int is_power_of_2(int64_t n);

int tcc_ir_opt_pass_disabled(const char *name);

int vrp_negate_cmp_tok(int tok);
int vrp_swap_cmp_tok(int tok);
int vrp_cmp_implies(int known_true, int check);
int fcmp_cmp_implies(int known_true, int check);
int invert_cond_token(int tok);
int invert_condition(int cond);
int ir_negate_condition(int cond);

uint8_t *ir_opt_build_merge_bitmap(struct TCCIRState *ir, int n);

void ir_opt_mark_block_starts(struct TCCIRState *ir, int *block_start_seen,
                              int gen, int n);

uint8_t *ir_opt_build_block_starts_bitmap(struct TCCIRState *ir, int n);

int ir_opt_next_non_nop(struct TCCIRState *ir, int start);

int ir_skip_nops_forward(struct TCCIRState *ir, int start, int n);

int ir_has_other_jump_to_fast(struct TCCIRState *ir, const int *jt_cnt,
                              int target, int exclude_idx);

int tcc_ir_is_pure_aeabi(const char *name);
int ir_opt_is_pure_helper_name(const char *name);
int ir_opt_is_readonly_str_helper_name(const char *name);
int ir_opt_is_flag_cmp_helper_name(const char *name);
int ir_opt_is_pure_fallthrough_instruction(struct TCCIRState *ir, int idx);

int ir_opt_nonvreg_expr_equal(struct TCCIRState *ir, IROperand a, IROperand b);
int ir_opt_pure_def_equal(struct TCCIRState *ir, int a_def_idx, int b_def_idx,
                          int depth);
int ir_opt_pure_expr_equal(struct TCCIRState *ir, IROperand a, int a_use_idx,
                           IROperand b, int b_use_idx, int depth);

int ir_opt_get_call_param_operand(struct TCCIRState *ir, int call_idx,
                                  int param_idx, IROperand *out);
/* FUNCPARAM* index (or -1) — the correct reaching-def use-site for a param source, not call_idx. */
int ir_opt_get_call_param_index(struct TCCIRState *ir, int call_idx,
                                int param_idx);
void ir_opt_nop_call_params(struct TCCIRState *ir, int call_idx);
void ir_opt_nop_call_param(struct TCCIRState *ir, int call_idx, int param_idx);
void ir_opt_change_call_argc(struct TCCIRState *ir, int call_idx, int argc);

int ir_opt_vreg_address_taken_between(struct TCCIRState *ir, int32_t vreg,
                                      int start_idx, int end_idx);

const char *ir_opt_get_constant_string_from_symref(struct TCCIRState *ir,
                                                   IROperand op);

int tcc_ir_vreg_has_single_def(struct TCCIRState *ir, int32_t vreg);
int tcc_ir_vreg_has_multi_def(struct TCCIRState *ir, int32_t vreg);

/* memcpy/memmove + __aeabi_mem{cpy,move}{,4,8}; the __tcc_memmove alias is NOT included. */
int ir_opt_is_memcpy_or_memmove_name(const char *name);

int change_callee_sym(struct TCCIRState *ir, int instr_idx, const char *new_name, int ret_btype);
int change_callee_sym_keep_type(struct TCCIRState *ir, int instr_idx, const char *new_name);

static inline float ir_bits_to_f(int64_t bits)
{
  union { uint32_t u; float f; } c;
  c.u = (uint32_t)bits;
  return c.f;
}

static inline double ir_bits_to_d(int64_t bits)
{
  union { uint64_t u; double d; } c;
  c.u = (uint64_t)bits;
  return c.d;
}

/* float result occupies the low 32 bits, sign-extended into the int64 carrier */
static inline int64_t ir_f_to_bits(float f)
{
  union { uint32_t u; float f; } c;
  c.f = f;
  return (int64_t)(int32_t)c.u;
}

static inline int64_t ir_d_to_bits(double d)
{
  union { uint64_t u; double d; } c;
  c.d = d;
  return (int64_t)c.u;
}

/* 3-way compare (-1/0/+1) of soft-float bit patterns; *is_nan flags IEEE-unordered. */
static inline int ir_softfp_cmp3(int is_double, int64_t a0, int64_t a1, int *is_nan)
{
  if (is_double)
  {
    double a = ir_bits_to_d(a0), b = ir_bits_to_d(a1);
    *is_nan = (a != a) || (b != b);
    return (a > b) - (a < b);
  }
  float a = ir_bits_to_f(a0), b = ir_bits_to_f(a1);
  *is_nan = (a != a) || (b != b);
  return (a > b) - (a < b);
}

