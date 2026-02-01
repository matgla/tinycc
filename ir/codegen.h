/*
 *  TCC IR - Code Generation Helpers
 *
 *  Copyright (c) 2025 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#ifndef TCC_IR_CODEGEN_H
#define TCC_IR_CODEGEN_H

struct TCCIRState;
struct SValue;
struct IROperand;
struct IRQuadCompact;

/* ============================================================================
 * Operand Access
 * ============================================================================ */

/* Read operand from instruction, expand to SValue with register allocation */
int tcc_ir_codegen_operand_get(struct TCCIRState *ir, const struct IRQuadCompact *q, 
                                int slot, struct SValue *out);

/* Get destination operand from instruction */
struct IROperand tcc_ir_codegen_dest_get(struct TCCIRState *ir, const struct IRQuadCompact *q);

/* Get source 1 operand from instruction */
struct IROperand tcc_ir_codegen_src1_get(struct TCCIRState *ir, const struct IRQuadCompact *q);

/* Get source 2 operand from instruction */
struct IROperand tcc_ir_codegen_src2_get(struct TCCIRState *ir, const struct IRQuadCompact *q);

/* Set destination operand in instruction */
void tcc_ir_codegen_dest_set(struct TCCIRState *ir, const struct IRQuadCompact *q, 
                              struct IROperand irop);

/* ============================================================================
 * Register Filling
 * ============================================================================ */

/* Fill physical registers into SValue from allocation */
void tcc_ir_codegen_reg_fill(struct TCCIRState *ir, struct SValue *sv);

/* Fill physical registers into IROperand from allocation */
void tcc_ir_codegen_reg_fill_op(struct TCCIRState *ir, struct IROperand *op);

/* Get physical register for vreg (or PREG_REG_NONE) */
int tcc_ir_codegen_reg_get(struct TCCIRState *ir, int vreg);

/* Set physical register for vreg */
void tcc_ir_codegen_reg_set(struct TCCIRState *ir, int vreg, int preg);

/* ============================================================================
 * Parameter Handling
 * ============================================================================ */

/* Setup register allocation for function parameters */
void tcc_ir_codegen_params_setup(struct TCCIRState *ir);

/* ============================================================================
 * Code Generation Entry Point
 * ============================================================================ */

/* Generate machine code from IR */
void tcc_ir_codegen_generate(struct TCCIRState *ir);

/* Main code generation entry point (legacy wrapper) */
void tcc_ir_generate_code(struct TCCIRState *ir);

/* Generate code for comparison and jump/set */
void tcc_ir_codegen_cmp_jmp_set(struct TCCIRState *ir);

/* ============================================================================
 * Jump Handling
 * ============================================================================ */

/* Backpatch jump target */
void tcc_ir_codegen_backpatch(struct TCCIRState *ir, int jump_idx, int target_address);

/* Backpatch jump to current position */
void tcc_ir_codegen_backpatch_here(struct TCCIRState *ir, int jump_idx);

/* Backpatch first jump in chain */
void tcc_ir_codegen_backpatch_first(struct TCCIRState *ir, int jump_idx, int target_address);

/* Append jump to chain, return new chain head */
int tcc_ir_codegen_jump_append(struct TCCIRState *ir, int chain, int jump);

/* Generate test and jump */
int tcc_ir_codegen_test_gen(struct TCCIRState *ir, int invert, int test);

/* Drop unused return value from function call */
void tcc_ir_codegen_drop_return(struct TCCIRState *ir);

/* ============================================================================
 * Basic Blocks
 * ============================================================================ */

/* Mark start of basic block */
void tcc_ir_codegen_bb_start(struct TCCIRState *ir);

#endif /* TCC_IR_CODEGEN_H */
