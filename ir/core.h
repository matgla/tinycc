/*
 *  TCC IR - Core Operations
 *
 *  Copyright (c) 2025 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#ifndef TCC_IR_CORE_H
#define TCC_IR_CORE_H

/* operand.h is included via tcc.h as tccir_operand.h */

struct TCCIRState;
struct SValue;
struct CType;
struct Sym;

/* ============================================================================
 * IR Block Lifecycle
 * ============================================================================ */

/* Allocate new IR block */
struct TCCIRState *tcc_ir_alloc(void);

/* Free IR block and all associated memory */
void tcc_ir_free(struct TCCIRState *ir);

/* Reset IR block for reuse (keeps allocations) */
void tcc_ir_reset(struct TCCIRState *ir);

/* ============================================================================
 * Instruction Insertion
 * ============================================================================ */

/* Insert instruction with SValue operands */
int tcc_ir_put(struct TCCIRState *ir, TccIrOp op, 
                struct SValue *src1, struct SValue *src2, struct SValue *dest);

/* Insert instruction with IROperand operands */
int tcc_ir_put_op(struct TCCIRState *ir, TccIrOp op,
                   struct IROperand src1, struct IROperand src2, struct IROperand dest);

/* Insert instruction without operands */
int tcc_ir_put_no_op(struct TCCIRState *ir, TccIrOp op);

/* ============================================================================
 * Function Setup
 * ============================================================================ */

/* Add function parameters to IR */
void tcc_ir_params_add(struct TCCIRState *ir, struct CType *func_type);

/* Add local variable to IR */
int tcc_ir_local_add(struct TCCIRState *ir, struct Sym *sym, int stack_offset);

/* Parameter processing helpers */
void tcc_ir_params_process_single(struct TCCIRState *ir, struct Sym *sym, int arg_index, struct TCCAbiCallLayout *call_layout);
void tcc_ir_params_update_tracking(struct TCCIRState *ir, struct TCCAbiArgLoc loc_info, struct TCCAbiCallLayout *layout);
void tcc_ir_params_process_struct(struct TCCIRState *ir, struct Sym *sym, struct CType *type, int size, int align, struct TCCAbiArgLoc *loc_info, struct TCCAbiCallLayout *call_layout, int arg_index);
void tcc_ir_params_process_scalar(struct TCCIRState *ir, struct Sym *sym, struct CType *type, struct TCCAbiArgLoc *loc_info);

/* ============================================================================
 * Integer Operations
 * ============================================================================ */

/* Generate integer operation */
void tcc_ir_gen_i(struct TCCIRState *ir, int op);

/* Generate specific integer operation */
void tcc_ir_gen_add(struct TCCIRState *ir);
void tcc_ir_gen_sub(struct TCCIRState *ir);
void tcc_ir_gen_mul(struct TCCIRState *ir);
void tcc_ir_gen_div(struct TCCIRState *ir);
void tcc_ir_gen_mod(struct TCCIRState *ir);
void tcc_ir_gen_and(struct TCCIRState *ir);
void tcc_ir_gen_or(struct TCCIRState *ir);
void tcc_ir_gen_xor(struct TCCIRState *ir);
void tcc_ir_gen_shl(struct TCCIRState *ir);
void tcc_ir_gen_shr(struct TCCIRState *ir);
void tcc_ir_gen_sar(struct TCCIRState *ir);

/* ============================================================================
 * Floating Point Operations
 * ============================================================================ */

/* Generate floating point operation */
void tcc_ir_gen_f(struct TCCIRState *ir, int op);

/* Generate specific FP operation */
void tcc_ir_gen_fadd(struct TCCIRState *ir);
void tcc_ir_gen_fsub(struct TCCIRState *ir);
void tcc_ir_gen_fmul(struct TCCIRState *ir);
void tcc_ir_gen_fdiv(struct TCCIRState *ir);
void tcc_ir_gen_fneg(struct TCCIRState *ir);
void tcc_ir_gen_fcmp(struct TCCIRState *ir);

/* Generate FP conversion */
void tcc_ir_gen_cvt_ftof(struct TCCIRState *ir); /* float <-> double */
void tcc_ir_gen_cvt_itof(struct TCCIRState *ir); /* int -> float */
void tcc_ir_gen_cvt_ftoi(struct TCCIRState *ir); /* float -> int */

/* ============================================================================
 * Control Flow
 * ============================================================================ */

/* Generate test and branch */
int tcc_ir_gen_test(struct TCCIRState *ir, int invert, int t);

/* Generate unconditional jump */
int tcc_ir_gen_jmp(struct TCCIRState *ir);

/* Generate indirect jump */
void tcc_ir_gen_ijmp(struct TCCIRState *ir, struct SValue *target);

/* Generate return */
void tcc_ir_gen_return_void(struct TCCIRState *ir);
void tcc_ir_gen_return_value(struct TCCIRState *ir, struct SValue *val);

/* ============================================================================
 * Comparison
 * ============================================================================ */

/* Generate comparison */
void tcc_ir_gen_cmp(struct TCCIRState *ir, int op);

/* Generate set if condition */
void tcc_ir_gen_setif(struct TCCIRState *ir, int condition);

/* ============================================================================
 * Memory Operations
 * ============================================================================ */

/* Generate load */
void tcc_ir_gen_load(struct TCCIRState *ir, struct CType *type);

/* Generate store */
void tcc_ir_gen_store(struct TCCIRState *ir, struct CType *type);

/* Generate load effective address */
void tcc_ir_gen_lea(struct TCCIRState *ir);

/* ============================================================================
 * Function Calls
 * ============================================================================ */

/* Generate function call (void return) */
void tcc_ir_gen_call_void(struct TCCIRState *ir, struct SValue *func);

/* Generate function call (value return) */
void tcc_ir_gen_call_value(struct TCCIRState *ir, struct SValue *func, struct CType *ret_type);

/* Generate function parameter */
void tcc_ir_gen_param_void(struct TCCIRState *ir, struct SValue *val);
void tcc_ir_gen_param_value(struct TCCIRState *ir, struct SValue *val);

/* Generate soft float call if needed (returns 1 if generated) */
int tcc_ir_gen_soft_call_fpu(struct TCCIRState *ir, TccIrOp op, 
                              struct SValue *src1, struct SValue *src2, struct SValue *dest);

/* Generate soft call */
void tcc_ir_gen_soft_call(struct TCCIRState *ir, TccIrOp op, 
                           struct SValue *src1, struct SValue *src2, struct SValue *dest);

/* Drop return value */
void tcc_ir_return_drop(struct TCCIRState *ir);

/* ============================================================================
 * Boolean Operations
 * ============================================================================ */

/* Generate boolean OR */
void tcc_ir_gen_bool_or(struct TCCIRState *ir);

/* Generate boolean AND */
void tcc_ir_gen_bool_and(struct TCCIRState *ir);

/* Test if value is zero */
void tcc_ir_gen_test_zero(struct TCCIRState *ir, struct SValue *val);

/* ============================================================================
 * VLA (Variable Length Array) Support
 * ============================================================================ */

/* Allocate VLA on stack */
void tcc_ir_gen_vla_alloc(struct TCCIRState *ir, struct SValue *size);

/* Save SP before VLA allocation */
void tcc_ir_gen_vla_sp_save(struct TCCIRState *ir, int slot);

/* Restore SP from saved VLA slot */
void tcc_ir_gen_vla_sp_restore(struct TCCIRState *ir, int slot);

/* ============================================================================
 * Inline Assembly
 * ============================================================================ */

#ifdef CONFIG_TCC_ASM

/* Add inline assembly block, return ID */
int tcc_ir_asm_add(struct TCCIRState *ir, const char *asm_str, int asm_len, 
                    int must_subst, struct ASMOperand *operands,
                    int nb_operands, int nb_outputs, int nb_labels,
                    const uint8_t *clobber_regs);

/* Put inline assembly instruction */
void tcc_ir_asm_put(struct TCCIRState *ir, int asm_id);

#endif /* CONFIG_TCC_ASM */

/* ============================================================================
 * Jump Chain Management
 * ============================================================================ */

/* Backpatch jump chain to target address */
void tcc_ir_backpatch(struct TCCIRState *ir, int t, int target_address);

/* Backpatch jump chain to current instruction position */
void tcc_ir_backpatch_to_here(struct TCCIRState *ir, int t);

/* Backpatch first jump in chain to target address */
void tcc_ir_backpatch_first(struct TCCIRState *ir, int t, int target_address);

/* Append target to end of jump chain, return head */
int tcc_ir_gjmp_append(struct TCCIRState *ir, int n, int t);

/* ============================================================================
 * Utility Functions
 * ============================================================================ */

/* Get number of instructions */
int tcc_ir_count(struct TCCIRState *ir);

/* Get current instruction index */
int tcc_ir_current_idx(struct TCCIRState *ir);

/* Check if leaf function (no calls) */
int tcc_ir_is_leaf(struct TCCIRState *ir);

/* Mark function as non-leaf */
void tcc_ir_nonleaf_mark(struct TCCIRState *ir);

/* Get next call ID */
int tcc_ir_call_id_next(struct TCCIRState *ir);

#endif /* TCC_IR_CORE_H */
