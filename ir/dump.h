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
struct IRQuadCompact;

void tcc_ir_dump(struct TCCIRState *ir, FILE *out);
void tcc_ir_dump_stdout(struct TCCIRState *ir);
void tcc_ir_dump_instr(struct TCCIRState *ir, int idx, FILE *out);
void tcc_ir_dump_range(struct TCCIRState *ir, int start, int end, FILE *out);
void tcc_ir_dump_compact(struct TCCIRState *ir, struct IRQuadCompact *q, int pc, FILE *out);
void tcc_ir_dump_op(struct TCCIRState *ir, struct IROperand op, FILE *out);
void tcc_ir_dump_svalue_short(struct SValue *sv, FILE *out);
void tcc_ir_dump_vreg(int vreg, FILE *out);
const char *tcc_ir_dump_op_name(int op);
void tcc_ir_print_vreg(int vreg);

#endif /* TCC_IR_DUMP_H */
