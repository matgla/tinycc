/*
 *  TCC IR - Shared optimization utilities (pre-SSA)
 *
 *  Copyright (c) 2025 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#ifndef TCC_IR_OPT_UTILS_H
#define TCC_IR_OPT_UTILS_H

#include <stdint.h>

struct TCCIRState;
struct IROperand;

/* ============================================================================
 * Constant evaluators
 * ============================================================================ */

int ir_opt_eval_const_u64(struct TCCIRState *ir, IROperand op, int use_idx,
                          uint64_t *out, int depth);

int ir_opt_eval_const_string(struct TCCIRState *ir, IROperand op, int use_idx,
                             const char **out, int depth);

/* Resolve op to the constant string it points at AND return the underlying
 * symref operand (with the resolved addend) in *out — needed to rebuild a
 * symref at a folded offset. */
int ir_opt_eval_const_string_operand(struct TCCIRState *ir, IROperand op,
                                     int use_idx, IROperand *out, int depth);

/* strlen() of a string materialized byte-by-byte into a stack buffer before
 * the call at call_idx.  Returns 1 and the length in *out_len on success. */
int ir_opt_eval_stack_strlen(struct TCCIRState *ir, IROperand arg,
                             int call_idx, int *out_len);

/* Constant string-comparison folders (byte semantics). */
int ir_opt_fold_strcmp_result(const char *s1, const char *s2);
int ir_opt_fold_strncmp_result(const char *s1, const char *s2, uint64_t n);
int ir_opt_fold_memcmp_result(const char *s1, const char *s2, uint64_t n);

int evaluate_compare_condition(int64_t val1, int64_t val2, int cond_token);

int is_power_of_2(int64_t n);

/* ============================================================================
 * Pass-disable helper (for debugging / bisection)
 * ============================================================================ */

int tcc_ir_opt_pass_disabled(const char *name);

/* ============================================================================
 * Condition token helpers
 * ============================================================================ */

int vrp_negate_cmp_tok(int tok);
int vrp_swap_cmp_tok(int tok);
int vrp_cmp_implies(int known_true, int check);
int fcmp_cmp_implies(int known_true, int check);
int invert_cond_token(int tok);
int invert_condition(int cond);
int ir_negate_condition(int cond);

/* ============================================================================
 * BB / CFG helpers
 * ============================================================================ */

uint8_t *ir_opt_build_merge_bitmap(struct TCCIRState *ir, int n);

void ir_opt_mark_block_starts(struct TCCIRState *ir, int *block_start_seen,
                              int gen, int n);

uint8_t *ir_opt_build_block_starts_bitmap(struct TCCIRState *ir, int n);

int ir_opt_next_non_nop(struct TCCIRState *ir, int start);

int ir_skip_nops_forward(struct TCCIRState *ir, int start, int n);

int ir_has_other_jump_to_fast(struct TCCIRState *ir, const int *jt_cnt,
                              int target, int exclude_idx);

/* ============================================================================
 * Purity tables
 * ============================================================================ */

int tcc_ir_is_pure_aeabi(const char *name);
int ir_opt_is_pure_helper_name(const char *name);
int ir_opt_is_readonly_str_helper_name(const char *name);
int ir_opt_is_flag_cmp_helper_name(const char *name);
int ir_opt_is_pure_fallthrough_instruction(struct TCCIRState *ir, int idx);

/* ============================================================================
 * Expression equality
 * ============================================================================ */

int ir_opt_nonvreg_expr_equal(struct TCCIRState *ir, IROperand a, IROperand b);
int ir_opt_pure_def_equal(struct TCCIRState *ir, int a_def_idx, int b_def_idx,
                          int depth);
int ir_opt_pure_expr_equal(struct TCCIRState *ir, IROperand a, int a_use_idx,
                           IROperand b, int b_use_idx, int depth);

/* ============================================================================
 * Call-param helpers
 * ============================================================================ */

int ir_opt_get_call_param_operand(struct TCCIRState *ir, int call_idx,
                                  int param_idx, IROperand *out);
/* Instruction index of the FUNCPARAMVAL/FUNCPARAMVOID marshalling `param_idx`
 * for the call at `call_idx`, or -1.  Use this as the reaching-def use-site for
 * a param's source: the call index is wrong because the source may be redefined
 * between param marshalling and the call. */
int ir_opt_get_call_param_index(struct TCCIRState *ir, int call_idx,
                                int param_idx);
void ir_opt_nop_call_params(struct TCCIRState *ir, int call_idx);
void ir_opt_nop_call_param(struct TCCIRState *ir, int call_idx, int param_idx);
void ir_opt_change_call_argc(struct TCCIRState *ir, int call_idx, int argc);

/* ============================================================================
 * Misc helpers (co-extracted dependencies)
 * ============================================================================ */

int ir_opt_vreg_address_taken_between(struct TCCIRState *ir, int32_t vreg,
                                      int start_idx, int end_idx);

const char *ir_opt_get_constant_string_from_symref(struct TCCIRState *ir,
                                                   IROperand op);

int tcc_ir_vreg_has_single_def(struct TCCIRState *ir, int32_t vreg);
int tcc_ir_vreg_has_multi_def(struct TCCIRState *ir, int32_t vreg);

/* ============================================================================
 * Callee symbol replacement helpers
 * ============================================================================ */

int change_callee_sym(struct TCCIRState *ir, int instr_idx, const char *new_name, int ret_btype);
int change_callee_sym_keep_type(struct TCCIRState *ir, int instr_idx, const char *new_name);

#endif /* TCC_IR_OPT_UTILS_H */
