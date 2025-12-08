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
  TCCIR_OP_JUMP,
  TCCIR_OP_FUNCPARAMVOID,
  TCCIR_OP_FUNCCALLVOID,
  TCCIR_OP_FUNCCALLVAL,
  TCCIR_OP_LOAD,
  TCCIR_OP_STORE,
} TccIrOp;

typedef struct CType CType;
typedef struct SValue SValue;

typedef struct TACQuadruple TACQuadruple;

typedef struct IRLiveInterval {
  uint8_t r;                 // first register
  uint8_t r2;                // second register (for pairs)
  uint16_t virtual_register; // virtual register number
  uint32_t stack_location;   // stack location if spilled
  uint32_t start;            // start instruction index
  uint32_t end;              // end instruction index
  struct IRLiveInterval *next;
} IRLiveInterval;

typedef struct TCCIRState {
  // number of function parameters
  int8_t parameters_count;

  int8_t leaffunc : 1;
  int32_t loc;

  TACQuadruple *instructions;
  IRLiveInterval *live_intervals;
  int next_live_interval_index;
  IRLiveInterval **active_set;
  int instructions_size;
  int next_instruction_index;
  uint16_t next_temp_vr;
} TCCIRState;

TCCIRState *tcc_ir_allocate_block();
void tcc_ir_release_block(TCCIRState *ir);

void tcc_ir_add_function_parameters(TCCIRState *ir, CType *func_type);

int tcc_ir_gvtst(TCCIRState *ir, int inv, int t);

void tcc_ir_gen_opi(TCCIRState *ir, int op);
void tcc_ir_gen_opf(TCCIRState *ir, int op);
void tcc_ir_put(TCCIRState *ir, TccIrOp op, SValue *src1, SValue *src2,
                SValue *dest);

uint16_t tcc_ir_get_vreg_temp(TCCIRState *ir);

void tcc_ir_liveness_analysis(TCCIRState *ir);
void tcc_ir_register_allocation(TCCIRState *ir);
void tcc_ir_register_allocation_params(TCCIRState *ir);
void tcc_ir_generate_code(TCCIRState *ir);

const char *tcc_ir_get_op_name(TccIrOp op);