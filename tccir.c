/*
 *  TCC - Tiny C Compiler
 *
 *  Copyright (c) 2025 Mateusz Stadnik
 *
 *  Inspired by: https://bitbucket.org/theStack/tccls_poc.git
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

#include "tccir.h"

#include "tcc.h"

#include <stdio.h>
#include <stdlib.h>

#define QUADRUPLE_INIT_SIZE 128

#define IR_LIVE_INTERVAL_INIT_SIZE 64

typedef struct IRRegistersConfig {
  uint8_t has_dest : 1;
  uint8_t has_src1 : 1;
  uint8_t has_src2 : 1;
} IRRegistersConfig;

// clang-format off
const IRRegistersConfig irop_config[] = {
    [TCCIR_OP_ADD] = {1, 1, 1},
    [TCCIR_OP_ADC_USE] = {1, 1, 1},
    [TCCIR_OP_ADC_GEN] = {1, 1, 1},
    [TCCIR_OP_SUB] = {1, 1, 1},
    [TCCIR_OP_SUBC_GEN] = {1, 1, 1},
    [TCCIR_OP_SUBC_USE] = {1, 1, 1},
    [TCCIR_OP_MUL] = {1, 1, 1},
    [TCCIR_OP_UMULL] = {1, 1, 1},
    [TCCIR_OP_DIV] = {1, 1, 1},
    [TCCIR_OP_UMOD] = {1, 1, 1},
    [TCCIR_OP_IMOD] = {1, 1, 1},
    [TCCIR_OP_AND] = {1, 1, 1},
    [TCCIR_OP_OR] = {1, 1, 1},
    [TCCIR_OP_XOR] = {1, 1, 1},
    [TCCIR_OP_SHL] = {1, 1, 1},
    [TCCIR_OP_SAR] = {1, 1, 1},
    [TCCIR_OP_SHR] = {1, 1, 1},
    [TCCIR_OP_PDIV] = {1, 1, 1},
    [TCCIR_OP_UDIV] = {1, 1, 1},
    [TCCIR_OP_CMP] = {0, 1, 1},
    [TCCIR_OP_RETURNVOID] = {0, 0, 0},
    [TCCIR_OP_JUMP] = {0, 0, 0},
    [TCCIR_OP_FUNCPARAMVOID] = {0, 0, 0},
    [TCCIR_OP_FUNCCALLVOID] = {0, 1, 0},
    [TCCIR_OP_FUNCCALLVAL] = {1, 1, 0},
    [TCCIR_OP_LOAD] = {1, 1, 0},
    [TCCIR_OP_STORE] = {1, 1, 0},
};
// clang-format on

#define IR_MAX_VARS 10000
#define IR_MAX_TEMPS 10000
#define IR_MAX_PARAMS 10000

#define IS_VREG_VALID(vr)                                                      \
  (vr >= 0 && vr < (IR_MAX_VARS + IR_MAX_TEMPS + IR_MAX_PARAMS))

TCCIRState *tcc_ir_allocate_block() {
  TCCIRState *block = (TCCIRState *)tcc_malloc(sizeof(TCCIRState));
  if (!block) {
    fprintf(stderr, "tcc_ir_allocate_block: out of memory\n");
    exit(1);
  }
  block->parameters_count = 0;
  block->instructions_size = QUADRUPLE_INIT_SIZE;
  block->instructions =
      (TACQuadruple *)tcc_mallocz(sizeof(TACQuadruple) * QUADRUPLE_INIT_SIZE);

  block->active_set = (IRLiveInterval **)tcc_mallocz(
      sizeof(IRLiveInterval *) * tcc_gen_machine_number_of_registers());
  block->next_instruction_index = 0;
  block->next_temp_vr = IR_MAX_VARS;
  if (!block->instructions) {
    fprintf(stderr, "tcc_ir_allocate_block: out of memory\n");
    exit(1);
  }
  return block;
}

static void tcc_ir_clear_live_intervals(TCCIRState *ir) {
  ir->live_intervals = (IRLiveInterval *)tcc_mallocz(
      sizeof(IRLiveInterval) * IR_LIVE_INTERVAL_INIT_SIZE);
  ir->next_live_interval_index = 0;
}

void tcc_ir_release_block(TCCIRState *ir) {
  if (!ir) {
    fprintf(stderr, "tcc_ir_release_block: NULL ir block\n");
    exit(1);
  }

  if (ir->instructions != NULL) {
    tcc_free(ir->instructions);
  }
  tcc_free(ir);
}

void tcc_ir_add_function_parameters(TCCIRState *ir, CType *func_type) {
  Sym *sym, *sym2;
  int n = 0, size = 0, align = 0, pn = 0,
      sn = 0; // pn = core registers, sn = stack
  sym = func_type->ref;
  func_vt = sym->type;

  for (sym2 = sym->next; sym2 && (n < architecture_config.parameter_registers);
       sym2 = sym2->next) {
    size = type_size(&sym2->type, &align);
    if (is_float(sym2->type.t)) {
      fprintf(stderr, "TODO: implement float parameter handling in IR\n");
    }
    printf("Got parameter of size %d bytes\n", size);
    n += CEIL_DIV(size, architecture_config.reg_size);
  }

  if (n > architecture_config.parameter_registers) {
    n = architecture_config.parameter_registers;
  }

  // PC must be aligned to 8 bytes for ARM EABI
  n = ALIGN(n * architecture_config.reg_size, architecture_config.stack_align) /
      architecture_config.reg_size;
  ir->parameters_count = n;

  while ((sym = sym->next)) {
    CType *type = &sym->type;
    int flags;
    int addr = 0;
    size = type_size(type, &align);
    size = CEIL_DIV(size, architecture_config.reg_size);
    align = ALIGN(align, architecture_config.reg_size);
    if (pn < architecture_config.parameter_registers) {
      pn = ALIGN(pn, align);
      addr = pn * architecture_config.reg_size;
      pn += size;
      if (!sn && pn > architecture_config.parameter_registers) {
        sn = pn - architecture_config.parameter_registers;
      }
    } else {
      // take from stack
      sn = ALIGN(sn, align);
      addr = (n + sn) * architecture_config.reg_size;
      sn += size;
    }
    sym_push(sym->v & ~SYM_FIELD, type, VT_LOCAL | VT_LVAL, addr + 12);
  }
  ir->leaffunc = 1;
  ir->loc = 0;
}

void tcc_ir_gen_opf(TCCIRState *ir, int op) {
  fprintf(stderr,
          "tcc_ir_gen_opf: floating point operations not implemented\n");
  exit(1);
}

TccIrOp tcc_irop_from_token(int token) {
  switch (token) {
  case '+':
    return TCCIR_OP_ADD;
  case TOK_ADDC1:
    return TCCIR_OP_ADC_GEN;
  case TOK_ADDC2:
    return TCCIR_OP_ADC_USE;
  case '-':
    return TCCIR_OP_SUB;
  case TOK_SUBC1:
    return TCCIR_OP_SUBC_GEN;
  case TOK_SUBC2:
    return TCCIR_OP_SUBC_USE;
  case '&':
    return TCCIR_OP_AND;
  case '^':
    return TCCIR_OP_XOR;
  case '|':
    return TCCIR_OP_OR;
  case '*':
    return TCCIR_OP_MUL;
  case TOK_UMULL:
    return TCCIR_OP_UMULL;
  case TOK_SHL:
    return TCCIR_OP_SHL;
  case TOK_SAR:
    return TCCIR_OP_SAR;
  case TOK_SHR:
    return TCCIR_OP_SHR;
  case '/':
    return TCCIR_OP_DIV;
  case TOK_PDIV:
    return TCCIR_OP_DIV;
  case TOK_UDIV:
    return TCCIR_OP_UDIV;
  case '%':
    return TCCIR_OP_IMOD;
  case TOK_UMOD:
    return TCCIR_OP_UMOD;
  case TOK_EQ:
  case TOK_NE:
  case TOK_LT:
  case TOK_GT:
  case TOK_LE:
  case TOK_GE:
  case TOK_ULT:
  case TOK_UGT:
  case TOK_ULE:
  case TOK_UGE:
    return TCCIR_OP_CMP;
  };
  fprintf(stderr, "tcc_irop_from_token: unknown token %d(0x%x)\n", token,
          token);
  exit(1);
}

void tcc_ir_gen_opi(TCCIRState *ir, int op) {
  const TccIrOp ir_op = tcc_irop_from_token(op);
  SValue dest;
  if (ir_op == TCCIR_OP_CMP) {
    tcc_ir_put(ir, ir_op, &vtop[-1], &vtop[0], NULL);
    --vtop;
    // vtop->r = VT_CMP;
    vtop->c.i = op;
    return;
  }

  memset(&dest, 0, sizeof(SValue));
  dest.vr = tcc_ir_get_vreg_temp(ir);
  dest.type.t = vtop[-1].type.t;
  tcc_ir_put(ir, ir_op, &vtop[-1], &vtop[0], &dest);
  vtop[-1].vr = dest.vr;
  vtop[-1].r = 0;
  --vtop;
}

const char *tcc_ir_get_op_name(TccIrOp op) {
  switch (op) {
  case TCCIR_OP_ADD:
    return "ADD";
  case TCCIR_OP_ADC_GEN:
    return "ADC_GEN";
  case TCCIR_OP_ADC_USE:
    return "ADC_USE";
  case TCCIR_OP_SUB:
    return "SUB";
  case TCCIR_OP_SUBC_GEN:
    return "SUBC_GEN";
  case TCCIR_OP_SUBC_USE:
    return "SUBC_USE";
  case TCCIR_OP_MUL:
    return "MUL";
  case TCCIR_OP_UMULL:
    return "UMULL";
  case TCCIR_OP_DIV:
    return "DIV";
  case TCCIR_OP_UMOD:
    return "UMOD";
  case TCCIR_OP_IMOD:
    return "IMOD";
  case TCCIR_OP_AND:
    return "AND";
  case TCCIR_OP_OR:
    return "OR";
  case TCCIR_OP_XOR:
    return "XOR";
  case TCCIR_OP_SHL:
    return "SHL";
  case TCCIR_OP_SAR:
    return "SAR";
  case TCCIR_OP_SHR:
    return "SHR";
  case TCCIR_OP_PDIV:
    return "PDIV";
  case TCCIR_OP_UDIV:
    return "UDIV";
  case TCCIR_OP_CMP:
    return "CMP";
  case TCCIR_OP_RETURNVOID:
    return "RETURNVOID";
  case TCCIR_OP_JUMP:
    return "JUMP";
  case TCCIR_OP_FUNCPARAMVOID:
    return "FUNCPARAMVOID";
  case TCCIR_OP_FUNCCALLVAL:
    return "FUNCCALLVAL";
  case TCCIR_OP_FUNCCALLVOID:
    return "FUNCCALLVOID";
  default:
    return "UNKNOWN_OP";
  }
}

void tcc_ir_put(TCCIRState *ir, TccIrOp op, SValue *src1, SValue *src2,
                SValue *dest) {
  // resize array if needed
  const int pos = ir->next_instruction_index;
  TACQuadruple *q;
  if (ir->next_instruction_index >= ir->instructions_size) {
    ir->instructions_size <<= 1;
    ir->instructions = (TACQuadruple *)tcc_realloc(
        ir->instructions, sizeof(TACQuadruple) * ir->instructions_size);
    if (!ir->instructions) {
      fprintf(stderr, "tcc_ir_put: out of memory\n");
      exit(1);
    }
  }
  printf("IR: Adding instruction %s at pos %d\n", tcc_ir_get_op_name(op), pos);
  q = &ir->instructions[pos];
  q->op = op;
  if (irop_config[op].has_src1 == 1) {
    if (src1 == NULL) {
      fprintf(stderr, "tcc_ir_put: src1 is NULL for op %s\n",
              tcc_ir_get_op_name(op));
      exit(1);
    }
    q->src1 = *src1;
  } else {
    q->src1.vr = -1;
  }

  if (irop_config[op].has_src2 == 1) {
    if (src2 == NULL) {
      fprintf(stderr, "tcc_ir_put: src2 is NULL for op %s\n",
              tcc_ir_get_op_name(op));
      exit(1);
    }
    q->src2 = *src2;
  } else {
    q->src2.vr = -1;
  }

  if (irop_config[op].has_dest == 1) {
    if (dest == NULL) {
      fprintf(stderr, "tcc_ir_put: dest is NULL for op %s\n",
              tcc_ir_get_op_name(op));
      exit(1);
    }
    q->dest = *dest;
  } else {
    q->dest.vr = -1;
  }

  if ((op == TCCIR_OP_FUNCCALLVOID) || (op == TCCIR_OP_FUNCCALLVAL)) {
    ir->leaffunc = 0;
  }

  q->src1.r = -1;
  q->src1.r2 = -1;
  q->src2.r = -1;
  q->src2.r2 = -1;
  q->dest.r = -1;
  q->dest.r2 = -1;
  ++ir->next_instruction_index;
}

uint16_t tcc_ir_get_vreg_temp(TCCIRState *ir) {
  const uint16_t next_temp_vr = ir->next_temp_vr;
  if (next_temp_vr >= (IR_MAX_VARS + IR_MAX_TEMPS)) {
    fprintf(stderr,
            "tcc_ir_get_vreg_temp: out of temporary virtual registers\n");
    exit(1);
  }
  ++ir->next_temp_vr;
  return next_temp_vr;
}

void tcc_ir_liveness_analysis(TCCIRState *ir) {
  tcc_ir_clear_live_intervals(ir);
}
void tcc_ir_register_allocation(TCCIRState *ir) {}
void tcc_ir_register_allocation_params(TCCIRState *ir) {}

void tcc_ir_generate_code(TCCIRState *ir) {
  SValue *src1, *src2, *dest;
  TACQuadruple *q;

  for (int i = 0; i < ir->next_instruction_index; i++) {
    q = &ir->instructions[i];
    printf("Generating code for IR op %s at index %d\n",
           tcc_ir_get_op_name(q->op), i);
    switch (q->op) {
    case TCCIR_OP_MUL:
    case TCCIR_OP_ADD:
      tcc_gen_machine_data_processing_op(q);
      break;
    default: {
      printf("Unsupported operation in tcc_generate_code: %s\n",
             tcc_ir_get_op_name(q->op));
    }
    };
  }
}