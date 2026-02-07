/*
 *  TCC IR - Debug Dumping
 *
 *  Copyright (c) 2025 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#ifndef TCC_IR_DUMP_H
#define TCC_IR_DUMP_H

struct TCCIRState;
struct SValue;
struct IROperand;
struct TACQuadruple;
struct IRQuadCompact;

/* ============================================================================
 * IR Dumping
 * ============================================================================ */

/* Dump entire IR block to file */
void tcc_ir_dump(struct TCCIRState *ir, FILE *out);

/* Dump IR block to stdout */
void tcc_ir_dump_stdout(struct TCCIRState *ir);

/* Dump single instruction to file */
void tcc_ir_dump_instr(struct TCCIRState *ir, int idx, FILE *out);

/* Dump instruction range to file */
void tcc_ir_dump_range(struct TCCIRState *ir, int start, int end, FILE *out);

/* ============================================================================
 * Value Dumping
 * ============================================================================ */

/* Dump SValue to file */
void tcc_ir_dump_svalue(struct TCCIRState *ir, const struct SValue *sv, FILE *out);

/* Dump SValue short form to file */
void tcc_ir_dump_svalue_short(struct TCCIRState *ir, const struct SValue *sv, FILE *out);

/* Dump IROperand to file */
void tcc_ir_dump_op(struct TCCIRState *ir, struct IROperand op, FILE *out);

/* Dump IROperand short form to file */
void tcc_ir_dump_op_short(struct TCCIRState *ir, struct IROperand op, FILE *out);

/* ============================================================================
 * Instruction Dumping
 * ============================================================================ */

/* Dump quadruple to file */
void tcc_ir_dump_quad(struct TCCIRState *ir, struct TACQuadruple *q, int pc, FILE *out);

/* Dump compact instruction to file */
void tcc_ir_dump_compact(struct TCCIRState *ir, struct IRQuadCompact *q, int pc, FILE *out);

/* Dump vreg info */
void tcc_ir_dump_vreg(struct TCCIRState *ir, int vreg, FILE *out);

/* ============================================================================
 * Live Interval Dumping
 * ============================================================================ */

/* Dump live intervals to file */
void tcc_ir_dump_live(struct TCCIRState *ir, FILE *out);

/* Dump live interval for specific vreg */
void tcc_ir_dump_live_vreg(struct TCCIRState *ir, int vreg, FILE *out);

/* ============================================================================
 * Stack Layout Dumping
 * ============================================================================ */

/* Dump stack layout to file */
void tcc_ir_dump_stack(struct TCCIRState *ir, FILE *out);

/* ============================================================================
 * Helper Functions
 * ============================================================================ */

/* Get operation name as string */
const char *tcc_ir_dump_op_name(int op);

/* Get vreg type string */
const char *tcc_ir_dump_vreg_type(int vreg_type);

/* Print vreg to stdout (for debugging) */
void tcc_ir_print_vreg(int vreg);

/* ============================================================================
 * Legacy Dump Functions (used when TCC_DUMP_THUMB_GEN is enabled)
 * ============================================================================ */

/* Dump SValue short form to file (legacy implementation) */
void tcc_dump_svalue_short_to(FILE *out, const struct SValue *sv);

/* Dump quadruple to file (legacy implementation) */
void tcc_dump_quadruple_to(FILE *out, const struct TACQuadruple *q, int pc);

#endif /* TCC_IR_DUMP_H */
