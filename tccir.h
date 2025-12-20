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

typedef enum TccIrOp {
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
  TCCIR_OP_TEST_ZERO,
  TCCIR_OP_FUNCPARAMVOID,
  TCCIR_OP_FUNCPARAMVAL,
  TCCIR_OP_FUNCCALLVOID,
  TCCIR_OP_FUNCCALLVAL,
  TCCIR_OP_LOAD,
  TCCIR_OP_STORE,
  TCCIR_OP_ASSIGN,
} TccIrOp;

typedef struct CType CType;
typedef struct SValue SValue;

typedef struct TACQuadruple TACQuadruple;
typedef struct Sym Sym;

typedef struct IRVregReplacement {
  uint16_t r0; // first physical register
  uint16_t r1; // second physical register (for long long)
  int offset;  // stack offset if spilled
} IRVregReplacement;

typedef struct IRLiveInterval {
  uint8_t start_within_if : 1; // whether the interval starts within an if block
  uint32_t start;              // start instruction index
  uint32_t end;                // end instruction index
  IRVregReplacement allocation;
} IRLiveInterval;

typedef struct TCCIRState {
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
int tcc_ir_put(TCCIRState *ir, TccIrOp op, SValue *src1, SValue *src2,
               SValue *dest);

int tcc_ir_get_vreg_temp(TCCIRState *ir);
int tcc_ir_get_vreg_var(TCCIRState *ir);
int tcc_ir_get_vreg_param(TCCIRState *ir);

void tcc_ir_liveness_analysis(TCCIRState *ir);
void tcc_ir_register_allocation_params(TCCIRState *ir);
void tcc_ir_generate_code(TCCIRState *ir);

int tcc_ir_add_local_variable(TCCIRState *ir, Sym *sym, int stack_offset);
void tcc_ir_assign_physical_register(TCCIRState *ir, int vreg, int offset,
                                     int r0, int r1);
const char *tcc_ir_get_op_name(TccIrOp op);
void tcc_ir_show(TCCIRState *ir);
void tcc_ir_drop_return_value(TCCIRState *ir);

void tcc_ir_patch_live_intervals_registers(TCCIRState *ir);
void tcc_ir_backpatch(TCCIRState *ir, int t, int target_address);
void tcc_ir_backpatch_to_here(TCCIRState *ir, int t);
int tcc_ir_generate_test(TCCIRState *ir, int inv, int t);
void tcc_ir_print_vreg(int vreg);

typedef enum TCCIR_VREG_TYPE {
  TCCIR_VREG_TYPE_VAR = 1,
  TCCIR_VREG_TYPE_TEMP = 2,
  TCCIR_VREG_TYPE_PARAM = 3,
} TCCIR_VREG_TYPE;

#define TCCIR_DECODE_VREG_POSITION(vr) (vr & 0xFFFFFFF)
#define TCCIR_DECODE_VREG_TYPE(vr) (vr >> 28)
#define TCCIR_ENCODE_VREG(type, position) (((type) << 28) | (position))