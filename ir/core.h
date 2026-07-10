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

/* Generate floating point operation */
void tcc_ir_gen_f(struct TCCIRState *ir, int op);

/* ============================================================================
 * Control Flow
 * ============================================================================ */

/* Generate return */
void tcc_ir_gen_return_value(struct TCCIRState *ir, struct SValue *val);

/* ============================================================================
 * VLA (Variable Length Array) Support
 * ============================================================================ */

/* Allocate VLA on stack; align is the runtime SP alignment in bytes */
void tcc_ir_gen_vla_alloc(struct TCCIRState *ir, struct SValue *size, int align);

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
