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

/* Resolve a frame buffer whose contents are known to a rodata string: *out_sym is
 * a symref operand usable as a copy source, *out_len its strlen. */
int ir_opt_eval_stack_const_string(struct TCCIRState *ir, IROperand arg, int use_idx,
                                   IROperand *out_sym, int *out_len);

/* Byte semantics. */
int ir_opt_fold_strcmp_result(const char *s1, const char *s2);
int ir_opt_fold_strncmp_result(const char *s1, const char *s2, uint64_t n);
int ir_opt_fold_memcmp_result(const char *s1, const char *s2, uint64_t n);
/* *out_offset = index of c within the first n bytes of s, or -1 when absent. */
int ir_opt_fold_memchr_offset(const char *s, unsigned char c, uint64_t n, int *out_offset);

int evaluate_compare_condition(int64_t val1, int64_t val2, int cond_token);

/* Applies signed/unsigned + width semantics derived from the operand types. */
int evaluate_compare_condition_cmp_annotated(const TCCIRState *ir, const IRQuadCompact *q,
                                             int64_t val1, int64_t val2, int cond,
                                             IROperand src1, IROperand src2);
int evaluate_compare_condition_cmp_operands(int64_t val1, int64_t val2, int cond,
                                            IROperand src1, IROperand src2);
/* IEEE NaN branch result for a soft-FP compare condition token: 0/1/-1. */
int nan_compare_branch_result(int cond_token);

struct IRQuadCompact;
int32_t ir_opt_mla_accum_vreg(const struct TCCIRState *ir, const struct IRQuadCompact *q);

/* 1 when this instruction performs a memory access that may be volatile, so a
 * pass must not delete, duplicate or reorder it against another such access.
 * Answers 0 for every instruction of a function that never touches volatile
 * memory — see TCCIRState.func_has_volatile_access. */
int tcc_ir_instr_access_is_volatile(const struct TCCIRState *ir, const struct IRQuadCompact *q);
int tcc_ir_operand_names_volatile_var(const struct TCCIRState *ir, IROperand op);

int is_power_of_2(int64_t n);

/* tcc_ir_opt_pass_disabled (TCC_DISABLE_PASS) is declared in tccir.h — its
 * release no-op has to reach TUs that never include this header. */

int vrp_negate_cmp_tok(int tok);
int vrp_swap_cmp_tok(int tok);
int vrp_cmp_implies(int known_true, int check);
int fcmp_cmp_implies(int known_true, int check);
int invert_cond_token(int tok);
int swap_cond_token(int tok);
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
/* Whether name is one of the NUL-separated names in list ("a\0b\0"; the
 * literal's own terminator ends the list). */
int ir_opt_name_in(const char *name, const char *list);
int ir_opt_is_memcpy_or_memmove_name(const char *name);
/* memset(dst, c, n) vs __aeabi_memset(dst, n, c): for either name, returns 1 and the
 * PARAM indices of the size and fill value; 0 for any other callee. */
int ir_opt_memset_params(const char *name, int *size_idx, int *fill_idx);

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

/* A forward-reachability worklist over instruction indices: `bits` marks the
 * reached ones, wl[head..tail) holds those still to visit; both sized n. */
typedef struct IrReachWorklist
{
  uint8_t *bits;
  int *wl;
  int head, tail, n;
} IrReachWorklist;

/* Reach instruction idx (ignored outside [0, n) or when already reached). */
void ir_opt_reach_mark(IrReachWorklist *w, int idx);

/* The same with one byte per element: seen[idx] set, idx pushed on list[top++]. */
typedef struct IrReachList
{
  uint8_t *seen;
  int *list;
  int top, n;
} IrReachList;

void ir_opt_reach_push(IrReachList *w, int idx);

/* Whole-body rewrites of the collapse passes: NOP instructions [0, n) and clear
 * their jump-target marks; make instruction 0 a `JUMP 0` self-loop (`b .`);
 * clear the post-RA dirty-register masks and live-register map and mark the
 * function a leaf, for a body that no longer touches registers. */
void ir_opt_nop_body(struct TCCIRState *ir, int n);
void ir_opt_set_self_jump0(struct TCCIRState *ir);
void ir_opt_reset_body_regs(struct TCCIRState *ir);

/* 3-way compare (-1/0/+1) of soft-float bit patterns; *is_nan flags IEEE-unordered. */
int ir_softfp_cmp3(int is_double, int64_t a0, int64_t a1, int *is_nan);

