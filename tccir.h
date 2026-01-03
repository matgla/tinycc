/*
 *  TCC - Tiny C Compiler
 *
 *  Copyright (c) 2025 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#pragma once

#include <stdint.h>

#include "tccls.h"

#define PREG_SPILLED 0x80

typedef enum TccIrOp
{
  TCCIR_OP_ADD,
  TCCIR_OP_ADC_USE,
  TCCIR_OP_ADC_GEN,
  TCCIR_OP_SUB,
  TCCIR_OP_SUBC_USE,
  TCCIR_OP_SUBC_GEN,
  TCCIR_OP_MUL,
  TCCIR_OP_UMULL,
  TCCIR_OP_DIV,
  TCCIR_OP_UMOD,
  TCCIR_OP_IMOD,
  TCCIR_OP_AND,
  TCCIR_OP_OR,
  TCCIR_OP_XOR,
  TCCIR_OP_SHL,
  TCCIR_OP_SAR,
  TCCIR_OP_SHR,
  TCCIR_OP_PDIV,
  TCCIR_OP_UDIV,
  TCCIR_OP_CMP,
  TCCIR_OP_RETURNVOID,
  TCCIR_OP_RETURNVALUE,
  TCCIR_OP_JUMP,
  TCCIR_OP_JUMPIF,
  TCCIR_OP_SETIF,
  TCCIR_OP_TEST_ZERO,
  TCCIR_OP_FUNCPARAMVOID,
  TCCIR_OP_FUNCPARAMVAL,
  TCCIR_OP_FUNCCALLVOID,
  TCCIR_OP_FUNCCALLVAL,
  TCCIR_OP_LOAD,
  TCCIR_OP_STORE,
  TCCIR_OP_ASSIGN,
  /* Floating point operations */
  TCCIR_OP_FADD, /* float/double addition */
  TCCIR_OP_FSUB, /* float/double subtraction */
  TCCIR_OP_FMUL, /* float/double multiplication */
  TCCIR_OP_FDIV, /* float/double division */
  TCCIR_OP_FNEG, /* float/double negation */
  TCCIR_OP_FCMP, /* float/double comparison */
  /* Floating point conversion operations */
  TCCIR_OP_CVT_FTOF, /* float to double or double to float */
  TCCIR_OP_CVT_ITOF, /* int to float/double */
  TCCIR_OP_CVT_FTOI, /* float/double to int */
  /* Logical boolean operations - produce 0/1 result */
  TCCIR_OP_BOOL_OR,  /* (src1 != 0) || (src2 != 0) -> 0/1 */
  TCCIR_OP_BOOL_AND, /* (src1 != 0) && (src2 != 0) -> 0/1 */
} TccIrOp;

typedef struct CType CType;
typedef struct SValue SValue;

typedef struct TACQuadruple TACQuadruple;
typedef struct Sym Sym;

/* Sentinel value indicating an interval hasn't started yet.
 * Using 0xFFFFFFFF since instruction indices are non-negative. */
#define INTERVAL_NOT_STARTED 0xFFFFFFFF

typedef struct IRVregReplacement
{
  uint16_t r0; // first physical register
  uint16_t r1; // second physical register (for long long)
  int offset;  // stack offset if spilled
} IRVregReplacement;

typedef struct IRLiveInterval
{
  uint8_t start_within_if : 1; // whether the interval starts within an if block
  uint8_t addrtaken : 1;       // whether the variable's address is taken
  uint8_t is_float : 1;        // whether this is a float/double variable
  uint8_t is_double : 1;       // whether this is a double (vs float)
  uint8_t is_llong : 1;        // whether this is a long long (64-bit int)
  uint8_t use_vfp : 1;         // whether to use VFP registers (hard float)
  uint8_t is_lvalue : 1;
  uint32_t start; // start instruction index
  uint32_t end;   // end instruction index
  IRVregReplacement allocation;
  int8_t incoming_reg0;    // for params: which register arg arrives in (-1 if stack)
  int8_t incoming_reg1;    // for doubles: second register (-1 if not double or stack)
  int16_t original_offset; // for params: original offset from function entry point
} IRLiveInterval;

/* SpillContext: Tracks spilled register loading/storing for IR operations
 * Used by generate_code to centralize spill handling before/after machine ops
 */
typedef struct SpillContext
{
  int8_t orig_src1_pr0, orig_src2_pr0, orig_dest_pr0; // Original register allocations
  int src1_offset, src2_offset, dest_offset;          // Stack offsets
  uint8_t src1_spilled : 1;                           // Whether src1 was in memory
  uint8_t src2_spilled : 1;                           // Whether src2 was in memory
  uint8_t dest_spilled : 1;                           // Whether dest was in memory
  uint8_t is_64bit : 1;                               // Whether operation is 64-bit
} SpillContext;

typedef struct TCCIRState
{
  // number of function parameters
  int8_t parameters_count;

  uint8_t leaffunc : 1;
  uint8_t processing_if : 1;
  uint8_t check_for_backwards_jumps : 1;
  uint8_t basic_block_start : 1;
  uint8_t prevent_coalescing;
  int32_t loc;

  TACQuadruple *instructions;
  IRLiveInterval **active_set;

  IRLiveInterval *variables_live_intervals;
  int variables_live_intervals_size;
  int next_local_variable;

  IRLiveInterval *temporary_variables_live_intervals;
  int temporary_variables_live_intervals_size;
  int next_temporary_variable;

  IRLiveInterval *parameters_live_intervals;
  int parameters_live_intervals_size;
  int next_parameter;

  int next_live_interval_index;
  int instructions_size;
  int next_instruction_index;

  uint32_t *ignored_vregs;
  int ignored_vregs_size;

  LSLiveIntervalState ls;
} TCCIRState;

TCCIRState *tcc_ir_allocate_block();
void tcc_ir_release_block(TCCIRState *ir);

void tcc_ir_add_function_parameters(TCCIRState *ir, CType *func_type);

int tcc_ir_gvtst(TCCIRState *ir, int inv, int t);

void tcc_ir_gen_opi(TCCIRState *ir, int op);
void tcc_ir_gen_opf(TCCIRState *ir, int op);
int tcc_ir_put(TCCIRState *ir, TccIrOp op, SValue *src1, SValue *src2, SValue *dest);

int tcc_ir_get_vreg_temp(TCCIRState *ir);
int tcc_ir_get_vreg_var(TCCIRState *ir);
int tcc_ir_get_vreg_param(TCCIRState *ir);

void tcc_ir_set_float_type(TCCIRState *ir, int vreg, int is_float, int is_double);
void tcc_ir_set_llong_type(TCCIRState *ir, int vreg);
void tcc_ir_set_original_offset(TCCIRState *ir, int vreg, int offset);
int tcc_ir_get_reg_type(TCCIRState *ir, int vreg);

void tcc_ir_liveness_analysis(TCCIRState *ir);
void tcc_ir_register_allocation_params(TCCIRState *ir);
void tcc_ir_generate_code(TCCIRState *ir);

int tcc_ir_add_local_variable(TCCIRState *ir, Sym *sym, int stack_offset);
void tcc_ir_assign_physical_register(TCCIRState *ir, int vreg, int offset, int r0, int r1);
const char *tcc_ir_get_op_name(TccIrOp op);
void tcc_ir_show(TCCIRState *ir);
void tcc_ir_drop_return_value(TCCIRState *ir);
void tcc_ir_set_addrtaken(TCCIRState *ir, int vreg);

void tcc_ir_patch_live_intervals_registers(TCCIRState *ir);
void tcc_ir_backpatch(TCCIRState *ir, int t, int target_address);
void tcc_ir_backpatch_to_here(TCCIRState *ir, int t);
void tcc_ir_backpatch_first(TCCIRState *ir, int t, int target_address);
int tcc_ir_gjmp_append(TCCIRState *ir, int n, int t);
int tcc_ir_generate_test(TCCIRState *ir, int inv, int t);
int tcc_ir_dead_code_elimination(TCCIRState *ir);
int tcc_ir_dead_store_elimination(TCCIRState *ir);
int tcc_ir_bool_cse(TCCIRState *ir);
int tcc_ir_bool_idempotent(TCCIRState *ir);
int tcc_ir_bool_simplification(TCCIRState *ir);
int tcc_ir_return_value_optimization(TCCIRState *ir);
void tcc_ir_print_vreg(int vreg);
void tcc_ir_generate_cmp_jmp_set(TCCIRState *ir);
void tcc_ir_start_basic_block(TCCIRState *ir);

/* Spill handling helpers - centralized in generate_code */
int tcc_ir_is_spilled(SValue *sv);
SpillContext tcc_ir_preload_spills(TACQuadruple *q, int preload_src1, int preload_src2, int setup_dest);
void tcc_ir_storeback_spill(TACQuadruple *q, SpillContext *ctx);

/* Check if FPU supports double precision (defined in arm-thumb-gen.c) */
int arm_fpu_supports_double(int fpu_type);

typedef enum TCCIR_VREG_TYPE
{
  TCCIR_VREG_TYPE_VAR = 1,
  TCCIR_VREG_TYPE_TEMP = 2,
  TCCIR_VREG_TYPE_PARAM = 3,
} TCCIR_VREG_TYPE;

#define TCCIR_DECODE_VREG_POSITION(vr) (vr & 0xFFFFFFF)
#define TCCIR_DECODE_VREG_TYPE(vr) (vr >> 28)
#define TCCIR_ENCODE_VREG(type, position) (((type) << 28) | (position))