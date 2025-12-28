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

#define USING_GLOBALS
#include "tcc.h"

#include <stdio.h>
#include <stdlib.h>

#define QUADRUPLE_INIT_SIZE 128

#define IR_LIVE_INTERVAL_INIT_SIZE 64
#define LOCAL_VARIABLES_INIT_SIZE 64

void tcc_print_quadruple(TACQuadruple *q, int pc);
void tcc_ir_print_vreg(int vreg);

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
    [TCCIR_OP_RETURNVALUE] = {0, 1, 0},
    [TCCIR_OP_JUMP] = {1, 0, 0},
    [TCCIR_OP_JUMPIF] = {1, 1, 0},
    [TCCIR_OP_SETIF] = {1, 1, 0},
    [TCCIR_OP_FUNCPARAMVOID] = {0, 0, 0},
    [TCCIR_OP_FUNCPARAMVAL] = {0, 1, 1},
    [TCCIR_OP_FUNCCALLVOID] = {0, 1, 0},
    [TCCIR_OP_FUNCCALLVAL] = {1, 1, 0},
    [TCCIR_OP_LOAD] = {1, 1, 0},
    [TCCIR_OP_STORE] = {1, 1, 0},
    [TCCIR_OP_ASSIGN] = {1, 1, 0},
    [TCCIR_OP_TEST_ZERO] = {0, 1, 0},
    /* Floating point operations */
    [TCCIR_OP_FADD] = {1, 1, 1},
    [TCCIR_OP_FSUB] = {1, 1, 1},
    [TCCIR_OP_FMUL] = {1, 1, 1},
    [TCCIR_OP_FDIV] = {1, 1, 1},
    [TCCIR_OP_FNEG] = {1, 1, 0},  /* unary: src1=input, dest */
    [TCCIR_OP_FCMP] = {0, 1, 1},
    /* Floating point conversion operations */
    [TCCIR_OP_CVT_FTOF] = {1, 1, 0},  /* dest=result, src1=input */
    [TCCIR_OP_CVT_ITOF] = {1, 1, 0},  /* dest=result, src1=input */
    [TCCIR_OP_CVT_FTOI] = {1, 1, 0},  /* dest=result, src1=input */
};
// clang-format on

#define IR_MAX_VARS 10000
#define IR_MAX_TEMPS 10000
#define IR_MAX_PARAMS 10000

static int tcc_is_vreg_valid(TCCIRState *ir, int vr) {
  const int type = TCCIR_DECODE_VREG_TYPE(vr);
  const int position = TCCIR_DECODE_VREG_POSITION(vr);
  switch (type) {
  case TCCIR_VREG_TYPE_VAR:
    return position < ir->variables_live_intervals_size;
  case TCCIR_VREG_TYPE_TEMP:
    return position < ir->temporary_variables_live_intervals_size;
  case TCCIR_VREG_TYPE_PARAM:
    return position < ir->parameters_live_intervals_size;
  default:
    return 0;
  }
  return 0;
}

static IRLiveInterval *tcc_ir_get_live_interval(TCCIRState *ir, int vreg) {
  if (vreg < 0) {
    fprintf(stderr, "tcc_ir_get_live_interval: invalid vreg: %d\n", vreg);
    exit(1);
  }
  int decoded_vreg_position = TCCIR_DECODE_VREG_POSITION(vreg);
  switch (TCCIR_DECODE_VREG_TYPE(vreg)) {
  case TCCIR_VREG_TYPE_VAR: {
    if (decoded_vreg_position >= ir->variables_live_intervals_size) {
      fprintf(stderr, "Getting out of bounds live interval for vreg %d\n",
              vreg);
      exit(1);
    }
    return &ir->variables_live_intervals[decoded_vreg_position];
  }
  case TCCIR_VREG_TYPE_TEMP: {
    if (decoded_vreg_position >= ir->temporary_variables_live_intervals_size) {
      fprintf(stderr, "Getting out of bounds live interval for vreg %d\n",
              vreg);
      exit(1);
    }
    return &ir->temporary_variables_live_intervals[decoded_vreg_position];
  }
  case TCCIR_VREG_TYPE_PARAM: {
    if (decoded_vreg_position >= ir->parameters_live_intervals_size) {
      fprintf(stderr, "Getting out of bounds live interval for vreg %d\n",
              vreg);
      exit(1);
    }
    return &ir->parameters_live_intervals[decoded_vreg_position];
  }
  default:
    fprintf(stderr,
            "tcc_ir_get_live_interval: unknown vreg type %d, for vreg: %d\n",
            TCCIR_DECODE_VREG_TYPE(vreg), vreg);
    exit(1);
  }
  return NULL;
}

static void tcc_ir_set_base_interval_end(TCCIRState *ir, int vreg) {
  IRLiveInterval *interval = tcc_ir_get_live_interval(ir, vreg);
  interval->end = ir->next_instruction_index;
}

/* Initialize all interval start fields to INTERVAL_NOT_STARTED and incoming_reg
 * to -1 */
static void tcc_ir_init_interval_starts(IRLiveInterval *intervals, int count) {
  for (int i = 0; i < count; ++i) {
    intervals[i].start = INTERVAL_NOT_STARTED;
    intervals[i].incoming_reg0 = -1;
    intervals[i].incoming_reg1 = -1;
  }
}

static void tcc_ir_clear_live_intervals(TCCIRState *ir) {
  ir->variables_live_intervals_size = IR_LIVE_INTERVAL_INIT_SIZE;
  if (ir->variables_live_intervals != NULL) {
    tcc_free(ir->variables_live_intervals);
  }
  ir->variables_live_intervals = (IRLiveInterval *)tcc_mallocz(
      sizeof(IRLiveInterval) * IR_LIVE_INTERVAL_INIT_SIZE);
  tcc_ir_init_interval_starts(ir->variables_live_intervals,
                              IR_LIVE_INTERVAL_INIT_SIZE);
  ir->next_local_variable = 0;

  ir->temporary_variables_live_intervals_size = IR_LIVE_INTERVAL_INIT_SIZE;
  if (ir->temporary_variables_live_intervals != NULL) {
    tcc_free(ir->temporary_variables_live_intervals);
  }
  ir->temporary_variables_live_intervals = (IRLiveInterval *)tcc_mallocz(
      sizeof(IRLiveInterval) * IR_LIVE_INTERVAL_INIT_SIZE);
  tcc_ir_init_interval_starts(ir->temporary_variables_live_intervals,
                              IR_LIVE_INTERVAL_INIT_SIZE);
  ir->next_temporary_variable = 0;

  ir->parameters_live_intervals_size = IR_LIVE_INTERVAL_INIT_SIZE;
  if (ir->parameters_live_intervals != NULL) {
    tcc_free(ir->parameters_live_intervals);
  }
  ir->parameters_live_intervals = (IRLiveInterval *)tcc_mallocz(
      sizeof(IRLiveInterval) * IR_LIVE_INTERVAL_INIT_SIZE);
  tcc_ir_init_interval_starts(ir->parameters_live_intervals,
                              IR_LIVE_INTERVAL_INIT_SIZE);
  ir->next_parameter = 0;
}

static int tcc_ir_operand_in_memory(SValue *sv) {
  const int svt = sv->r & VT_VALMASK;
  if (sv->pr0 == -1) {
    if (svt == VT_LOCAL) {
      return 1;
    } else if (svt == VT_CONST) {
      // VT_SYM is global variable, else is immediate
      return sv->r & VT_SYM;
    }
    // fprintf(
    //     stderr,
    //     "tcc_ir_operand_in_memory: unexpected operand type in memory
    //     check\n");
    return 0;
  }
  return sv->pr0 & PREG_SPILLED;
}

TCCIRState *tcc_ir_allocate_block() {
  TCCIRState *block = (TCCIRState *)tcc_mallocz(sizeof(TCCIRState));
  if (!block) {
    fprintf(stderr, "tcc_ir_allocate_block: out of memory\n");
    exit(1);
  }
  block->parameters_count = 0;
  block->active_set = (IRLiveInterval **)tcc_mallocz(
      sizeof(IRLiveInterval *) * tcc_gen_machine_number_of_registers());

  block->next_instruction_index = 0;

  block->leaffunc = 1;
  block->processing_if = 0;
  block->basic_block_start = 1;
  block->prevent_coalescing = 0;

  tcc_ir_clear_live_intervals(block);

  block->instructions_size = QUADRUPLE_INIT_SIZE;
  block->instructions =
      (TACQuadruple *)tcc_mallocz(sizeof(TACQuadruple) * QUADRUPLE_INIT_SIZE);
  if (!block->instructions) {
    fprintf(stderr, "tcc_ir_allocate_block: out of memory\n");
    exit(1);
  }

  tcc_ls_initialize(&block->ls);
  return block;
}

void tcc_ir_release_block(TCCIRState *ir) {
  if (!ir) {
    fprintf(stderr, "tcc_ir_release_block: NULL ir block\n");
    exit(1);
  }

  if (ir->active_set != NULL) {
    tcc_free(ir->active_set);
  }

  if (ir->instructions != NULL) {
    tcc_free(ir->instructions);
  }

  if (ir->variables_live_intervals != NULL) {
    tcc_free(ir->variables_live_intervals);
  }
  if (ir->temporary_variables_live_intervals != NULL) {
    tcc_free(ir->temporary_variables_live_intervals);
  }
  if (ir->parameters_live_intervals != NULL) {
    tcc_free(ir->parameters_live_intervals);
  }

  tcc_ls_deinitialize(&ir->ls);
  tcc_free(ir);
}

void tcc_ir_add_function_parameters(TCCIRState *ir, CType *func_type) {
  Sym *sym, *sym2;
  int n = 0, size = 0, align = 0, pn = 0,
      sn = 0; // pn = core registers, sn = stack
  sym = func_type->ref;
  func_vt = sym->type;
  tcc_state->need_frame_pointer = 0;

  for (sym2 = sym->next; sym2 && (n < architecture_config.parameter_registers);
       sym2 = sym2->next) {
    size = type_size(&sym2->type, &align);
    if (is_float(sym2->type.t)) {
      fprintf(stderr, "TODO: implement float parameter handling in IR\n");
    }
    n += CEIL_DIV(size, architecture_config.reg_size);
  }

  if (n > architecture_config.parameter_registers) {
    n = architecture_config.parameter_registers;
  }

  ir->parameters_count = n;
  // PC must be aligned to 8 bytes for ARM EABI
  n = ALIGN(n * architecture_config.reg_size, architecture_config.stack_align) /
      architecture_config.reg_size;

  while ((sym = sym->next)) {
    CType *type = &sym->type;
    int flags;
    int addr = 0;
    size = type_size(type, &align);
    size = CEIL_DIV(size, architecture_config.reg_size);
    align = ALIGN(align, architecture_config.reg_size);
    if (pn < architecture_config.parameter_registers) {
      pn = (pn + (align - 1) / 4) & -(align / 4); // ALIGN(pn, align);
      addr = pn * architecture_config.reg_size;
      pn += size;
      if (!sn && pn > architecture_config.parameter_registers) {
        sn = pn - architecture_config.parameter_registers;
      }
    } else {
      // take from stack
      sn = (sn + (align - 1) / 4) & -(align / 4);
      addr = (sn)*architecture_config.reg_size;
      sn += size;
      pn += size;
      tcc_state->need_frame_pointer = 1;
    }
    flags = VT_PARAM | VT_LVAL | VT_LOCAL;
    sym_push(sym->v & ~SYM_FIELD, type, flags, addr);
  }
  ir->leaffunc = 1;
  ir->loc = 0;
}

void tcc_ir_gen_opf(TCCIRState *ir, int op) {
  TccIrOp ir_op;
  SValue dest;
  int is_double;

  /* Determine the IR operation based on token */
  switch (op) {
  case '+':
    ir_op = TCCIR_OP_FADD;
    break;
  case '-':
    ir_op = TCCIR_OP_FSUB;
    break;
  case '*':
    ir_op = TCCIR_OP_FMUL;
    break;
  case '/':
    ir_op = TCCIR_OP_FDIV;
    break;
  case TOK_NEG:
    ir_op = TCCIR_OP_FNEG;
    break;
  default:
    /* Comparison operations */
    if (op >= TOK_ULT && op <= TOK_GT) {
      ir_op = TCCIR_OP_FCMP;
      tcc_ir_put(ir, ir_op, &vtop[-1], &vtop[0], NULL);
      --vtop;
      vtop->r = VT_CMP;
      vtop->cmp_op = op;
      vtop->jfalse = 0;
      vtop->jtrue = 0;
      return;
    }
    tcc_error("tcc_ir_gen_opf: unknown floating point operation: 0x%x", op);
    return;
  }

  /* Handle negation (unary) */
  if (ir_op == TCCIR_OP_FNEG) {
    memset(&dest, 0, sizeof(SValue));
    dest.vr = tcc_ir_get_vreg_temp(ir);
    dest.r = 0;
    dest.type = vtop->type;
    /* Mark temp as float/double */
    is_double = (vtop->type.t & VT_BTYPE) == VT_DOUBLE ||
                (vtop->type.t & VT_BTYPE) == VT_LDOUBLE;
    tcc_ir_set_float_type(ir, dest.vr, 1, is_double);
    tcc_ir_put(ir, ir_op, &vtop[0], NULL, &dest);
    vtop->vr = dest.vr;
    vtop->r = 0;
    return;
  }

  /* Binary FP operations */
  memset(&dest, 0, sizeof(SValue));
  dest.vr = tcc_ir_get_vreg_temp(ir);
  dest.r = 0;
  dest.type = vtop[-1].type;
  /* Mark temp as float/double */
  is_double = (vtop[-1].type.t & VT_BTYPE) == VT_DOUBLE ||
              (vtop[-1].type.t & VT_BTYPE) == VT_LDOUBLE;
  tcc_ir_set_float_type(ir, dest.vr, 1, is_double);
  tcc_ir_put(ir, ir_op, &vtop[-1], &vtop[0], &dest);
  vtop[-1].vr = dest.vr;
  vtop[-1].r = 0;
  --vtop;
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
    vtop->r = VT_CMP;
    vtop->cmp_op = op;
    vtop->jfalse = 0;
    vtop->jtrue = 0;
    return;
  }

  memset(&dest, 0, sizeof(SValue));
  dest.vr = tcc_ir_get_vreg_temp(ir);
  dest.r = 0;
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
  case TCCIR_OP_RETURNVALUE:
    return "RETURNVALUE";
  case TCCIR_OP_JUMP:
    return "JUMP";
  case TCCIR_OP_JUMPIF:
    return "JUMPIF";
  case TCCIR_OP_SETIF:
    return "SETIF";
  case TCCIR_OP_FUNCPARAMVOID:
    return "FUNCPARAMVOID";
  case TCCIR_OP_FUNCPARAMVAL:
    return "PARAM";
  case TCCIR_OP_FUNCCALLVAL:
  case TCCIR_OP_FUNCCALLVOID:
    return "CALL";
  case TCCIR_OP_LOAD:
    return "LOAD";
  case TCCIR_OP_STORE:
    return "STORE";
  case TCCIR_OP_ASSIGN:
    return "ASSIGN";
  case TCCIR_OP_TEST_ZERO:
    return "TEST_ZERO";
  case TCCIR_OP_FADD:
    return "FADD";
  case TCCIR_OP_FSUB:
    return "FSUB";
  case TCCIR_OP_FMUL:
    return "FMUL";
  case TCCIR_OP_FDIV:
    return "FDIV";
  case TCCIR_OP_FNEG:
    return "FNEG";
  case TCCIR_OP_FCMP:
    return "FCMP";
  case TCCIR_OP_CVT_FTOF:
    return "CVT_FTOF";
  case TCCIR_OP_CVT_ITOF:
    return "CVT_ITOF";
  case TCCIR_OP_CVT_FTOI:
    return "CVT_FTOI";
  default:
    return "UNKNOWN_OP";
  }
}

int tcc_ir_put(TCCIRState *ir, TccIrOp op, SValue *src1, SValue *src2,
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
  q = &ir->instructions[pos];
  q->op = op;

  if (irop_config[op].has_src1 == 1) {
    if (src1 == NULL) {
      fprintf(stderr, "tcc_ir_put: src1 is NULL for op %s\n",
              tcc_ir_get_op_name(op));
      exit(1);
    }
    q->src1 = *src1;
    if (tcc_is_vreg_valid(ir, src1->vr)) {
      tcc_ir_set_base_interval_end(ir, src1->vr);
    }
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
    if (tcc_is_vreg_valid(ir, src2->vr)) {
      tcc_ir_set_base_interval_end(ir, src2->vr);
    }
  } else {
    q->src2.vr = -1;
  }

  if (irop_config[op].has_dest == 1) {
    IRLiveInterval *dest_interval = NULL;
    if (dest == NULL) {
      fprintf(stderr, "tcc_ir_put: dest is NULL for op %s\n",
              tcc_ir_get_op_name(op));
      exit(1);
    }
    q->dest = *dest;
    if (tcc_is_vreg_valid(ir, dest->vr)) {
      dest_interval = tcc_ir_get_live_interval(ir, dest->vr);
      if (dest_interval->start == INTERVAL_NOT_STARTED) {
        dest_interval->start = ir->next_instruction_index;
        if (ir->processing_if &&
            TCCIR_DECODE_VREG_TYPE(dest->vr) == TCCIR_VREG_TYPE_VAR) {
          dest_interval->start_within_if = 1;
        }
      }
      dest_interval->end = ir->next_instruction_index;
    }
  } else {
    q->dest.vr = -1;
  }

  if ((op == TCCIR_OP_FUNCCALLVOID) || (op == TCCIR_OP_FUNCCALLVAL)) {
    ir->leaffunc = 0;
  }

  // physical registers were not assigned yet
  q->src1.pr0 = -1;
  q->src1.pr1 = -1;
  q->src2.pr0 = -1;
  q->src2.pr1 = -1;
  q->dest.pr0 = -1;
  q->dest.pr1 = -1;

  // store current source line number for debug info
  q->line_num = file ? file->line_num : 0;

  if (ir->basic_block_start) {
    ir->basic_block_start = 0;
  } else if ((!ir->prevent_coalescing) && (op == TCCIR_OP_ASSIGN) &&
             (TCCIR_DECODE_VREG_TYPE(src1->vr) == TCCIR_VREG_TYPE_TEMP) &&
             ((src1->r & VT_LVAL) == 0) &&
             (src1->vr == ir->instructions[pos - 1].dest.vr) &&
             (ir->instructions[pos - 1].op != TCCIR_OP_FUNCCALLVAL)) {
    ir->instructions[pos - 1].dest = ir->instructions[pos].dest;
    printf("[PATCHED] ");
    tcc_print_quadruple(&ir->instructions[pos - 1], pos - 1);
    IRLiveInterval *interval = tcc_ir_get_live_interval(ir, src1->vr);
    interval->start = INTERVAL_NOT_STARTED;
    interval->end = 0;
    printf("Setting interval of vreg %d to invalid, %p\n", src1->vr, interval);
    if (tcc_is_vreg_valid(ir, ir->instructions[pos].dest.vr)) {
      IRLiveInterval *dest_interval =
          tcc_ir_get_live_interval(ir, ir->instructions[pos].dest.vr);
      if (dest_interval->start == pos) {
        dest_interval->start = pos - 1;
      }
    }
    // Mark the src1 vreg as ignored
    return ir->next_instruction_index;
  }

  return ir->next_instruction_index++;
}

int tcc_ir_get_vreg_temp(TCCIRState *ir) {
  if (ir->next_temporary_variable >=
      ir->temporary_variables_live_intervals_size) {
    const int used = ir->temporary_variables_live_intervals_size;
    ir->temporary_variables_live_intervals_size <<= 1;
    ir->temporary_variables_live_intervals = (IRLiveInterval *)tcc_realloc(
        ir->temporary_variables_live_intervals,
        sizeof(IRLiveInterval) * ir->temporary_variables_live_intervals_size);
    memset(&ir->temporary_variables_live_intervals[used], 0,
           sizeof(IRLiveInterval) *
               (ir->temporary_variables_live_intervals_size - used));
    tcc_ir_init_interval_starts(&ir->temporary_variables_live_intervals[used],
                                ir->temporary_variables_live_intervals_size -
                                    used);
  }
  const int next_temp_vr = ir->next_temporary_variable;
  ++ir->next_temporary_variable;
  return (TCCIR_VREG_TYPE_TEMP << 28) | next_temp_vr;
}

int tcc_ir_get_vreg_var(TCCIRState *ir) {
  if (ir == NULL) {
    return -1;
  }
  if (ir->next_local_variable >= ir->variables_live_intervals_size) {
    const int used = ir->variables_live_intervals_size;
    ir->variables_live_intervals_size <<= 1;
    ir->variables_live_intervals = (IRLiveInterval *)tcc_realloc(
        ir->variables_live_intervals,
        sizeof(IRLiveInterval) * ir->variables_live_intervals_size);
    memset(&ir->variables_live_intervals[used], 0,
           sizeof(IRLiveInterval) * (ir->variables_live_intervals_size - used));
    tcc_ir_init_interval_starts(&ir->variables_live_intervals[used],
                                ir->variables_live_intervals_size - used);
  }
  const int next_var_vr = ir->next_local_variable;
  ++ir->next_local_variable;
  return (TCCIR_VREG_TYPE_VAR << 28) | next_var_vr;
}

int tcc_ir_get_vreg_param(TCCIRState *ir) {
  if (ir->next_parameter >= ir->parameters_live_intervals_size) {
    const int used = ir->parameters_live_intervals_size;
    ir->parameters_live_intervals_size <<= 1;
    ir->parameters_live_intervals = (IRLiveInterval *)tcc_realloc(
        ir->parameters_live_intervals,
        sizeof(IRLiveInterval) * ir->parameters_live_intervals_size);
    memset(&ir->parameters_live_intervals[used], 0,
           sizeof(IRLiveInterval) *
               (ir->parameters_live_intervals_size - used));
    tcc_ir_init_interval_starts(&ir->parameters_live_intervals[used],
                                ir->parameters_live_intervals_size - used);
  }
  const int next_param_vr = ir->next_parameter;
  ++ir->next_parameter;
  return (TCCIR_VREG_TYPE_PARAM << 28) | next_param_vr;
}

void tcc_ir_set_addrtaken(TCCIRState *ir, int vreg) {
  if (vreg < 0)
    return;
  IRLiveInterval *interval = tcc_ir_get_live_interval(ir, vreg);
  if (interval) {
    interval->addrtaken = 1;
  }
}

void tcc_ir_set_float_type(TCCIRState *ir, int vreg, int is_float,
                           int is_double) {
  if (vreg < 0)
    return;
  IRLiveInterval *interval = tcc_ir_get_live_interval(ir, vreg);
  if (interval) {
    interval->is_float = is_float;
    interval->is_double = is_double;
    /* For now, assume soft-float for ARM Thumb (no VFP for doubles) */
    /* TODO: make this configurable based on target */
    interval->use_vfp = 0;
  }
}

void tcc_ir_set_llong_type(TCCIRState *ir, int vreg) {
  if (vreg < 0)
    return;
  IRLiveInterval *interval = tcc_ir_get_live_interval(ir, vreg);
  if (interval) {
    interval->is_llong = 1;
  }
}

int tcc_ir_get_reg_type(TCCIRState *ir, int vreg) {
  if (vreg < 0)
    return LS_REG_TYPE_INT;
  IRLiveInterval *interval = tcc_ir_get_live_interval(ir, vreg);
  if (interval) {
    if (interval->is_llong) {
      return LS_REG_TYPE_LLONG;
    }
    if (interval->is_float) {
      if (interval->is_double) {
        /* For soft-float, doubles use two integer registers */
        return interval->use_vfp ? LS_REG_TYPE_DOUBLE : LS_REG_TYPE_DOUBLE_SOFT;
      }
      return LS_REG_TYPE_FLOAT;
    }
  }
  return LS_REG_TYPE_INT;
}

// 3 bits per vreg position: bit 0 = local_variable, bit 1 = temp, bit 2 =
// parameter
#define IGNORED_VREG_BITS_PER_ENTRY 3
#define IGNORED_VREG_LOCAL_VAR_BIT 0
#define IGNORED_VREG_TEMP_BIT 1
#define IGNORED_VREG_PARAM_BIT 2

static int tcc_get_vreg_type_bit(int vreg_type) {
  switch (vreg_type) {
  case TCCIR_VREG_TYPE_VAR:
    return IGNORED_VREG_LOCAL_VAR_BIT;
  case TCCIR_VREG_TYPE_TEMP:
    return IGNORED_VREG_TEMP_BIT;
  case TCCIR_VREG_TYPE_PARAM:
    return IGNORED_VREG_PARAM_BIT;
  default:
    return -1;
  }
}

static int tcc_is_vreg_ignored(TCCIRState *ir, int vreg) {
  const int position = TCCIR_DECODE_VREG_POSITION(vreg);
  const int type = TCCIR_DECODE_VREG_TYPE(vreg);
  const int type_bit = tcc_get_vreg_type_bit(type);
  const int bit_offset = position * IGNORED_VREG_BITS_PER_ENTRY + type_bit;
  const int index = bit_offset / 32;
  const int bit = bit_offset % 32;
  if (ir->ignored_vregs == NULL || type_bit < 0 ||
      index >= ir->ignored_vregs_size) {
    return 0;
  }
  return (ir->ignored_vregs[index] & (1 << bit)) != 0;
}

#define IGNORED_VREGS_INIT_SIZE 64

static void tcc_set_vreg_ignored(TCCIRState *ir, int vreg) {
  const int position = TCCIR_DECODE_VREG_POSITION(vreg);
  const int type = TCCIR_DECODE_VREG_TYPE(vreg);
  const int type_bit = tcc_get_vreg_type_bit(type);
  const int bit_offset = position * IGNORED_VREG_BITS_PER_ENTRY + type_bit;
  const int index = bit_offset / 32;
  const int bit = bit_offset % 32;
  if (type_bit < 0) {
    return;
  }

  if (ir->ignored_vregs == NULL) {
    ir->ignored_vregs_size = IGNORED_VREGS_INIT_SIZE;
    ir->ignored_vregs =
        (uint32_t *)tcc_mallocz(sizeof(uint32_t) * ir->ignored_vregs_size);
  }

  // Resize if needed
  while (index >= ir->ignored_vregs_size) {
    const int new_size = ir->ignored_vregs_size << 1;
    ir->ignored_vregs =
        (uint32_t *)tcc_realloc(ir->ignored_vregs, sizeof(uint32_t) * new_size);
    memset(ir->ignored_vregs + ir->ignored_vregs_size, 0,
           sizeof(uint32_t) * (new_size - ir->ignored_vregs_size));
    ir->ignored_vregs_size = new_size;
  }

  ir->ignored_vregs[index] |= (1 << bit);
}

static int tcc_ir_find_live_interval(TCCIRState *ir, int vreg, int *start,
                                     int *end, int check_for_backwards_jumps) {
  int retval = 0;
  IRLiveInterval *interval = tcc_ir_get_live_interval(ir, vreg);

  *start = interval->start;
  *end = interval->end;

  if (interval->start != INTERVAL_NOT_STARTED) {
    retval = 1;
  }

  if (!check_for_backwards_jumps) {
    return retval;
  }

  /* Check for backward jumps that would extend the live interval.
   * If a variable is live at a backward jump target, it must stay live
   * until the jump instruction. */
  for (int i = 0; i < ir->next_instruction_index; ++i) {
    TACQuadruple *q = &ir->instructions[i];
    if (q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_JUMPIF) {
      int jump_target = q->dest.c.i;
      /* Backward jump: target is before the jump instruction */
      if (jump_target < i) {
        /* If variable is live at jump target (start <= target),
         * extend end to include the jump instruction */
        if (*start != INTERVAL_NOT_STARTED && *start <= jump_target &&
            *end >= jump_target) {
          if (i > *end) {
            *end = i;
          }
        }
      }
    }
  }

  return retval;
}

/* Check if there's a function call between start and end instruction indices
 * A call at the start position is where the value is defined, so it doesn't
 * count. A call at the end position is where the value is last used, so it
 * doesn't count. We only care about calls strictly between start and end. */
static int tcc_ir_has_call_in_range(TCCIRState *ir, int start, int end) {
  for (int i = start + 1; i < end && i < ir->next_instruction_index; ++i) {
    TccIrOp op = ir->instructions[i].op;
    if (op == TCCIR_OP_FUNCCALLVOID || op == TCCIR_OP_FUNCCALLVAL ||
        op == TCCIR_OP_FUNCPARAMVOID || op == TCCIR_OP_FUNCPARAMVAL) {
      return 1;
    }
  }
  return 0;
}

/* Extend live intervals for vregs used as function parameters.
 * When a vreg is passed to FUNCPARAMVAL, it must stay live until the
 * corresponding FUNCCALL instruction. */
static void tcc_ir_extend_param_intervals(TCCIRState *ir) {
  int in_call = 0;
  int call_index = -1;

  /* Scan forward to find PARAM instructions and their corresponding CALL */
  for (int i = 0; i < ir->next_instruction_index; ++i) {
    TACQuadruple *q = &ir->instructions[i];
    if (q->op == TCCIR_OP_FUNCPARAMVAL) {
      /* Find the next CALL instruction */
      if (!in_call) {
        for (int j = i + 1; j < ir->next_instruction_index; ++j) {
          TACQuadruple *q2 = &ir->instructions[j];
          if (q2->op == TCCIR_OP_FUNCCALLVAL ||
              q2->op == TCCIR_OP_FUNCCALLVOID) {
            call_index = j;
            in_call = 1;
            break;
          }
        }
      }
      /* Extend the live interval of the source vreg to the call */
      if (call_index >= 0 && tcc_is_vreg_valid(ir, q->src1.vr)) {
        IRLiveInterval *interval = tcc_ir_get_live_interval(ir, q->src1.vr);
        if (interval && interval->end < call_index) {
          interval->end = call_index;
        }
      }
    } else if (q->op == TCCIR_OP_FUNCCALLVAL ||
               q->op == TCCIR_OP_FUNCCALLVOID) {
      in_call = 0;
      call_index = -1;
    }
  }
}

void tcc_ir_liveness_analysis(TCCIRState *ir) {
  int start, end;
  int crosses_call;
  int addrtaken;
  int reg_type;
  IRLiveInterval *interval;
  tcc_ls_clear_live_intervals(&ir->ls);

  /* Extend intervals for vregs used as function parameters */
  tcc_ir_extend_param_intervals(ir);

  for (int vreg = 0; vreg < ir->next_local_variable; ++vreg) {
    const int encoded_vreg = (TCCIR_VREG_TYPE_VAR << 28) | vreg;
    if (tcc_is_vreg_ignored(ir, vreg)) {
      continue;
    }
    start = 0;
    end = ~0;
    if (tcc_ir_find_live_interval(ir, encoded_vreg, &start, &end, 1)) {
      crosses_call = tcc_ir_has_call_in_range(ir, start, end);
      interval = tcc_ir_get_live_interval(ir, encoded_vreg);
      addrtaken = interval ? interval->addrtaken : 0;
      reg_type = tcc_ir_get_reg_type(ir, encoded_vreg);
      tcc_ls_add_live_interval(&ir->ls, encoded_vreg, start, end, crosses_call,
                               addrtaken, reg_type);
    }
  }

  for (int vreg = 0; vreg < ir->next_temporary_variable; ++vreg) {
    const int vreg_encoded = (TCCIR_VREG_TYPE_TEMP << 28) | vreg;
    if (tcc_is_vreg_ignored(ir, vreg)) {
      continue;
    }
    start = 0;
    end = ~0;
    if (tcc_ir_find_live_interval(ir, vreg_encoded, &start, &end, 1)) {
      crosses_call = tcc_ir_has_call_in_range(ir, start, end);
      interval = tcc_ir_get_live_interval(ir, vreg_encoded);
      addrtaken = interval ? interval->addrtaken : 0;
      reg_type = tcc_ir_get_reg_type(ir, vreg_encoded);
      tcc_ls_add_live_interval(&ir->ls, vreg_encoded, start, end, crosses_call,
                               addrtaken, reg_type);
    }
  }

  /* For non-leaf functions, include parameters in liveness analysis
   * so they get allocated to callee-saved registers if they cross calls.
   * Parameters are live from instruction 0 (function entry).
   * Include ALL parameters that arrived in registers - they need stack slots
   * even if unused, because we save them in the prolog. */
  if (!ir->leaffunc) {
    for (int vreg = 0; vreg < ir->next_parameter; ++vreg) {
      const int vreg_encoded = (TCCIR_VREG_TYPE_PARAM << 28) | vreg;
      IRLiveInterval *interval = tcc_ir_get_live_interval(ir, vreg_encoded);
      /* Parameters start at instruction 0 and end at their last use.
       * If end==0 and param is used at instruction 0, that's valid.
       * If end==0 and param is unused, we still allocate a slot for it. */
      start = 0;
      end = interval->end;
      /* If param never used (end would be 0 from memset), set minimal end */
      if (end == 0)
        end = 1; /* Ensure at least one instruction range for allocation */
      /* Check for backward jumps that would extend end */
      for (int i = 0; i < ir->next_instruction_index; ++i) {
        TACQuadruple *q = &ir->instructions[i];
        if (q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_JUMPIF) {
          int jump_target = q->dest.c.i;
          if (jump_target < i && start <= jump_target && end >= jump_target) {
            if (i > end)
              end = i;
          }
        }
      }
      crosses_call = tcc_ir_has_call_in_range(ir, start, end);
      addrtaken = interval->addrtaken;
      reg_type = tcc_ir_get_reg_type(ir, vreg_encoded);
      printf("DEBUG liveness: vreg=%d is_float=%d is_double=%d is_llong=%d -> "
             "reg_type=%d\n",
             vreg_encoded, interval->is_float, interval->is_double,
             interval->is_llong, reg_type);
      tcc_ls_add_live_interval(&ir->ls, vreg_encoded, start, end, crosses_call,
                               addrtaken, reg_type);
    }
  }
}

void tcc_ir_patch_live_intervals_registers(TCCIRState *ir) {
  for (int i = 0; i < ir->ls.next_interval_index; ++i) {
    LSLiveInterval *interval = &ir->ls.intervals[i];
    printf("DEBUG tcc_ir_patch: vreg=%d stack_loc=%d r0=%d r1=%d\n",
           interval->vreg, (int)interval->stack_location, interval->r0,
           interval->r1);
    tcc_ir_assign_physical_register(ir, interval->vreg,
                                    interval->stack_location, interval->r0,
                                    interval->r1);
  }
}

void tcc_ir_assign_physical_register(TCCIRState *ir, int vreg, int offset,
                                     int r0, int r1) {
  IRLiveInterval *interval = tcc_ir_get_live_interval(ir, vreg);
  /* If variable is spilled (offset != 0), mark r0 with PREG_SPILLED flag */
  if (offset != 0) {
    printf("DEBUG assign_phys_reg: vreg=%d offset=%d -> setting PREG_SPILLED\n",
           vreg, offset);
    interval->allocation.r0 = PREG_SPILLED;
  } else {
    interval->allocation.r0 = r0;
    interval->allocation.r1 = r1;
  }
  interval->allocation.offset = offset;
}

const char *tcc_ir_get_vreg_type_string(int vreg) {
  switch (TCCIR_DECODE_VREG_TYPE(vreg)) {
  case TCCIR_VREG_TYPE_VAR:
    return "VAR";
  case TCCIR_VREG_TYPE_TEMP:
    return "TMP";
  case TCCIR_VREG_TYPE_PARAM:
    return "PAR";
  default:
    return "UNK";
  }
}

void tcc_ir_register_allocation_params(TCCIRState *ir) {
  /* For leaf functions: parameters can stay in registers r0-r3.
   * For non-leaf functions: parameters arrive in registers but must be
   * stored to stack since r0-r3 are caller-saved.
   * In both cases, we need to track which register each parameter arrives in.
   */
  int argno = 0; // current register number (r0-r3)
  for (int vreg = 0; vreg < ir->next_parameter; ++vreg) {
    const int encoded_vreg = (TCCIR_VREG_TYPE_PARAM << 28) | vreg;
    IRLiveInterval *interval = tcc_ir_get_live_interval(ir, encoded_vreg);
    int is_double = interval && interval->is_double;

    /* AAPCS: doubles must be aligned to even register pairs */
    if (is_double && (argno & 1)) {
      argno++; /* skip odd register to align to even */
    }

    if (is_double) {
      /* Double takes r0+r1 or r2+r3 */
      if (argno <= 2) {
        /* For leaf functions: keep in registers.
         * For non-leaf: also set incoming_reg so prolog can save to stack */
        interval->incoming_reg0 = argno;
        interval->incoming_reg1 = argno + 1;
        if (ir->leaffunc) {
          tcc_ir_assign_physical_register(ir, encoded_vreg, 0, argno,
                                          argno + 1);
        }
        /* For non-leaf, allocation was already set by liveness analysis
         * (spilled to stack), but we need to mark incoming registers
         * so prolog knows to store r0:r1 or r2:r3 to stack location */
      } else {
        /* Spilled to caller's stack frame - parameter passed on stack */
        interval->incoming_reg0 = -1;
        interval->incoming_reg1 = -1;
        if (ir->leaffunc) {
          tcc_ir_assign_physical_register(ir, encoded_vreg, (argno - 4) * 4 + 8,
                                          -1, -1);
        }
      }
      argno += 2;
    } else {
      if (argno <= 3) {
        interval->incoming_reg0 = argno;
        interval->incoming_reg1 = -1;
        if (ir->leaffunc) {
          tcc_ir_assign_physical_register(ir, encoded_vreg, 0, argno, -1);
        }
      } else {
        /* Spilled to caller's stack frame - parameter passed on stack */
        interval->incoming_reg0 = -1;
        interval->incoming_reg1 = -1;
        if (ir->leaffunc) {
          tcc_ir_assign_physical_register(ir, encoded_vreg, (argno - 4) * 4 + 8,
                                          -1, -1);
        }
      }
      argno++;
    }
  }
}

void tcc_ir_fill_registers(TCCIRState *ir, SValue *sv) {
  if (tcc_is_vreg_valid(ir, sv->vr)) {
    IRLiveInterval *interval = tcc_ir_get_live_interval(ir, sv->vr);
    printf("DEBUG fill_registers: vr=%d allocation.r0=%d, allocation.r1=%d "
           "allocation.offset=%d\n",
           sv->vr, interval->allocation.r0, interval->allocation.r1,
           (int)interval->allocation.offset);
    sv->pr0 = interval->allocation.r0;
    sv->pr1 = interval->allocation.r1;
    sv->c.i = interval->allocation.offset;
  }
}

/* Dead Code Elimination pass
 * Removes unreachable instructions by following control flow from entry.
 * Returns 1 if any instructions were eliminated, 0 otherwise.
 */
int tcc_ir_dead_code_elimination(TCCIRState *ir) {
  int n = ir->next_instruction_index;
  if (n == 0)
    return 0;

  /* Allocate reachability bitmap and old->new index mapping */
  uint8_t *reachable = tcc_mallocz((n + 7) / 8);
  int *new_index = tcc_malloc(sizeof(int) * n);

  /* Mark reachable instructions using a worklist algorithm */
  int *worklist = tcc_malloc(sizeof(int) * n);
  int worklist_head = 0, worklist_tail = 0;

#define MARK_REACHABLE(idx)                                                    \
  do {                                                                         \
    if ((idx) >= 0 && (idx) < n &&                                             \
        !(reachable[(idx) / 8] & (1 << ((idx) % 8)))) {                        \
      reachable[(idx) / 8] |= (1 << ((idx) % 8));                              \
      worklist[worklist_tail++] = (idx);                                       \
    }                                                                          \
  } while (0)

  /* Start from instruction 0 */
  MARK_REACHABLE(0);

  while (worklist_head < worklist_tail) {
    int i = worklist[worklist_head++];
    TACQuadruple *q = &ir->instructions[i];

    switch (q->op) {
    case TCCIR_OP_JUMP:
      /* Unconditional jump - only the target is reachable */
      MARK_REACHABLE(q->dest.c.i);
      break;
    case TCCIR_OP_JUMPIF:
      /* Conditional jump - both target and fall-through are reachable */
      MARK_REACHABLE(q->dest.c.i);
      MARK_REACHABLE(i + 1);
      break;
    case TCCIR_OP_RETURNVALUE:
    case TCCIR_OP_RETURNVOID:
      /* Return - no successor (epilogue is implicit) */
      break;
    default:
      /* All other instructions fall through to the next */
      MARK_REACHABLE(i + 1);
      break;
    }
  }

#undef MARK_REACHABLE

  /* Count reachable instructions and build index mapping */
  int new_count = 0;
  for (int i = 0; i < n; i++) {
    if (reachable[i / 8] & (1 << (i % 8))) {
      new_index[i] = new_count++;
    } else {
      new_index[i] = -1; /* Dead instruction */
    }
  }

  /* If nothing was eliminated, clean up and return */
  if (new_count == n) {
    tcc_free(reachable);
    tcc_free(new_index);
    tcc_free(worklist);
    return 0;
  }

  /* Compact instructions and update jump targets */
  int write_pos = 0;
  for (int i = 0; i < n; i++) {
    if (new_index[i] >= 0) {
      TACQuadruple *q = &ir->instructions[i];

      /* Update jump targets */
      if (q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_JUMPIF) {
        int old_target = q->dest.c.i;
        if (old_target < n) {
          q->dest.c.i = new_index[old_target];
        } else {
          /* Target is past the end (epilogue) - adjust for removed instructions
           */
          q->dest.c.i = new_count;
        }
      }

      /* Move instruction to new position if needed */
      if (write_pos != i) {
        ir->instructions[write_pos] = *q;
      }
      write_pos++;
    }
  }

  ir->next_instruction_index = new_count;

  tcc_free(reachable);
  tcc_free(new_index);
  tcc_free(worklist);

  return 1;
}

static void tcc_ir_backpatch_jumps(TCCIRState *ir,
                                   uint32_t *ir_to_code_mapping) {
  TACQuadruple *q;
  for (int i = 0; i < ir->next_instruction_index; i++) {
    q = &ir->instructions[i];
    if (q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_JUMPIF) {
      const int instruction_address = ir_to_code_mapping[i];
      const int target_address = ir_to_code_mapping[q->dest.c.i];
      tcc_gen_machine_backpatch_jump(instruction_address, target_address);
    }
  }
}

void tcc_ir_generate_code(TCCIRState *ir) {
  TACQuadruple *q;
  int drop_return_value = 0;
  // +1 to include epilogue when needed
  uint32_t *ir_to_code_mapping =
      tcc_mallocz(sizeof(uint32_t) * (ir->next_instruction_index + 1));

  /* Track addresses of return jumps for later backpatching to epilogue */
  int *return_jump_addrs = tcc_malloc(sizeof(int) * ir->next_instruction_index);
  int num_return_jumps = 0;

  // generate prolog
  int stack_size = (-loc + 7) & ~7; // align to 8 bytes
  tcc_gen_machine_prolog(ir->leaffunc, ir->ls.dirty_registers, stack_size);

  /* For non-leaf functions, save parameters from incoming registers (r0-r3)
   * to their allocated stack locations. Parameters are spilled because
   * r0-r3 are caller-saved and get clobbered by calls.
   * For leaf functions, parameters stay in r0-r3 via register allocation. */
  printf("DEBUG tcc_ir_generate_code: leaffunc=%d next_parameter=%d\n",
         ir->leaffunc, ir->next_parameter);
  if (!ir->leaffunc) {
    for (int vreg = 0; vreg < ir->next_parameter; ++vreg) {
      const int encoded_vreg = (TCCIR_VREG_TYPE_PARAM << 28) | vreg;
      IRLiveInterval *interval = tcc_ir_get_live_interval(ir, encoded_vreg);

      int incoming_r0 = interval->incoming_reg0;
      int incoming_r1 = interval->incoming_reg1;

      printf("DEBUG prolog param %d: incoming_r0=%d incoming_r1=%d "
             "alloc.offset=%d is_double=%d\n",
             vreg, incoming_r0, incoming_r1, (int)interval->allocation.offset,
             interval->is_double);

      /* If parameter arrived in registers (not on caller's stack) */
      if (incoming_r0 >= 0) {
        /* Parameter needs to be saved to stack (spilled by register allocator)
         */
        int stack_offset = interval->allocation.offset;
        if (interval->is_double && incoming_r1 >= 0) {
          /* Double parameter: save both registers */
          /* Store r0/r2 (low word) to [FP + offset] */
          tcc_gen_machine_store_to_stack(incoming_r0, stack_offset);
          /* Store r1/r3 (high word) to [FP + offset + 4] */
          tcc_gen_machine_store_to_stack(incoming_r1, stack_offset + 4);
        } else {
          /* Single 32-bit parameter */
          tcc_gen_machine_store_to_stack(incoming_r0, stack_offset);
        }
      }
      /* Parameters that arrived on stack stay where they are (accessed via
       * offset_to_args) */
    }
  }

  for (int i = 0; i < ir->next_instruction_index; i++) {
    drop_return_value = 0;
    q = &ir->instructions[i];

    ir_to_code_mapping[i] = ind;

    // emit debug line info for this IR instruction AFTER recording ind
    tcc_debug_line_num(tcc_state, q->line_num);

    if (irop_config[q->op].has_src1 == 1) {
      tcc_ir_fill_registers(ir, &q->src1);
      if (q->op != TCCIR_OP_FUNCCALLVAL && q->op != TCCIR_OP_FUNCCALLVOID &&
          q->op != TCCIR_OP_FUNCPARAMVAL && q->op != TCCIR_OP_FUNCPARAMVOID &&
          q->op != TCCIR_OP_ASSIGN) {
        if (tcc_ir_operand_in_memory(&q->src1)) {
          q->src1.pr0 = architecture_config.scratch_register;
          tcc_gen_machine_load_register(&q->src1);
        }
      }
    }

    if (irop_config[q->op].has_src2 == 1) {
      tcc_ir_fill_registers(ir, &q->src2);
      if (tcc_ir_operand_in_memory(&q->src2)) {
        q->src2.pr0 = architecture_config.scratch_register;
        tcc_gen_machine_load_register(&q->src2);
      }
    }

    if (irop_config[q->op].has_dest == 1) {
      tcc_ir_fill_registers(ir, &q->dest);
      /* Don't pre-load destination to scratch register for operations that
       * handle their own destination storage (e.g., float conversions that
       * may need to store to spilled stack locations) */
      if (tcc_ir_operand_in_memory(&q->dest) && (q->op != TCCIR_OP_ASSIGN) &&
          (q->op != TCCIR_OP_STORE) && (q->op != TCCIR_OP_CVT_FTOF) &&
          (q->op != TCCIR_OP_CVT_ITOF) && (q->op != TCCIR_OP_CVT_FTOI)) {
        q->dest.pr0 = architecture_config.scratch_register;
        tcc_gen_machine_load_register(&q->dest);
      }
    }

    switch (q->op) {
    case TCCIR_OP_MUL:
    case TCCIR_OP_ADD:
    case TCCIR_OP_SUB:
    case TCCIR_OP_CMP:
    case TCCIR_OP_TEST_ZERO:
    case TCCIR_OP_SHL:
    case TCCIR_OP_SHR:
    case TCCIR_OP_OR:
    case TCCIR_OP_AND:
    case TCCIR_OP_XOR:
    case TCCIR_OP_DIV:
    case TCCIR_OP_UDIV:
    case TCCIR_OP_SAR:
      tcc_gen_machine_data_processing_op(q);
      break;
    case TCCIR_OP_FADD:
    case TCCIR_OP_FSUB:
    case TCCIR_OP_FMUL:
    case TCCIR_OP_FDIV:
    case TCCIR_OP_FNEG:
    case TCCIR_OP_FCMP:
    case TCCIR_OP_CVT_FTOF:
    case TCCIR_OP_CVT_ITOF:
    case TCCIR_OP_CVT_FTOI:
      tcc_gen_machine_fp_op(q);
      break;
    case TCCIR_OP_LOAD:
      tcc_gen_machine_load_op(q);
      break;
    case TCCIR_OP_STORE:
      tcc_gen_machine_store_op(q);
      break;
    case TCCIR_OP_RETURNVALUE:
      tcc_gen_machine_return_value_op(q);
      /* Emit jump to epilogue (will be backpatched later) */
      return_jump_addrs[num_return_jumps++] = ind;
      tcc_gen_machine_jump_op(q); /* Emits a placeholder jump */
      break;
    case TCCIR_OP_RETURNVOID:
      /* Emit jump to epilogue (will be backpatched later) */
      return_jump_addrs[num_return_jumps++] = ind;
      tcc_gen_machine_jump_op(q); /* Emits a placeholder jump */
      break;
    case TCCIR_OP_ASSIGN:
      tcc_gen_machine_assign_op(q);
      break;
    case TCCIR_OP_FUNCPARAMVAL: {
      /* Detect nested function call: if this param number is less than or equal
       * to the number of params we've already collected, we're starting a new
       * inner call. Save the outer call's context. */
      int param_num = q->src2.c.i; /* 1-based param number */
      tcc_gen_machine_func_param_op(q, param_num);
      break;
    }
    case TCCIR_OP_JUMP:
      tcc_gen_machine_jump_op(q);
      break;
    case TCCIR_OP_JUMPIF:
      tcc_gen_machine_conditional_jump_op(q);
      break;
    case TCCIR_OP_SETIF:
      tcc_gen_machine_setif_op(q);
      break;
    case TCCIR_OP_FUNCPARAMVOID:
      break;
    case TCCIR_OP_FUNCCALLVOID:
      drop_return_value = 1;
      /* fall through */
    case TCCIR_OP_FUNCCALLVAL: {
      // if return follows call then we can optimize away move
      const TACQuadruple *ir_next = (i + 1 < ir->next_instruction_index)
                                        ? &ir->instructions[i + 1]
                                        : NULL;
      if (ir_next && ir_next->op == TCCIR_OP_RETURNVALUE &&
          ir_next->src1.vr == q->dest.vr && q->src1.vr != -1) {
        q->dest.pr0 = REG_IRET;
        ++i; // skip next instruction
      }

      tcc_gen_machine_func_call_op(q, drop_return_value);
      /* Restore outer call's arguments if this was a nested call */
      tcc_gen_machine_restore_call_context();
      ir_to_code_mapping[i] = ind;
      break;
    }
    default: {
      printf("Unsupported operation in tcc_generate_code: %s\n",
             tcc_ir_get_op_name(q->op));
      tcc_free(ir_to_code_mapping);
      tcc_free(return_jump_addrs);
      exit(1);
    }
    };
  }

  ir_to_code_mapping[ir->next_instruction_index] = ind;
  tcc_gen_machine_epilog(ir->leaffunc);
  tcc_ir_backpatch_jumps(ir, ir_to_code_mapping);

  /* Backpatch return jumps to point to epilogue */
  int epilogue_addr = ir_to_code_mapping[ir->next_instruction_index];
  for (int i = 0; i < num_return_jumps; i++) {
    tcc_gen_machine_backpatch_jump(return_jump_addrs[i], epilogue_addr);
  }

  tcc_free(ir_to_code_mapping);
  tcc_free(return_jump_addrs);
}

void tcc_ir_print_vreg(int vreg) {
  printf("VReg %s:%d", tcc_ir_get_vreg_type_string(vreg),
         TCCIR_DECODE_VREG_POSITION(vreg));
}

void print_svalue_short(SValue *sv) {
  int val_loc = sv->r & VT_VALMASK;
#define SPILL_MARK_BEGIN "\033[41m"
#define SPILL_MARK_END "\033[0m"

  /* XXX: probably show ignored vregs in a special way */
  switch (val_loc) {
  case VT_CONST:
    if (sv->r & VT_SYM) {
      printf("GlobalSym(%d)", sv->sym->v);
      if (sv->c.i != 0)
        printf("+%d", sv->c.i);
    } else
      printf("#%d", sv->c.i);
    break;
  case VT_LLOCAL:
    printf("VT_LLOCAL (cval=%d)", sv->c.i);
    break;
  // case VT_LOCAL: printf("VReg%d[stack_offset=%d]", sv->vreg, sv->c.i); break;
  case VT_LOCAL:
    if (sv->pr0 != -1) { /* already register-allocated? */
      if (sv->pr0 & PREG_SPILLED)
        printf(SPILL_MARK_BEGIN "SpillLoc[%d]" SPILL_MARK_END, sv->c.i);
      else
        printf("R%d", sv->pr0);
    } else if (sv->vr != -1) { /* not reg-alloced, but vreg'ed? */
      tcc_ir_print_vreg(sv->vr);
#if 0
      printf("VReg%d[", sv->vreg);
      int bt = sv->type.t & VT_BTYPE;
      switch (bt) {
      case VT_INT: printf("INT"); break;
      case VT_BYTE: printf("BYTE"); break;
      case VT_SHORT: printf("SHORT"); break;
      case VT_VOID: printf("VOID"); break;
      case VT_PTR: printf("PTR"); break;
      case VT_ENUM: printf("ENUM"); break;
      case VT_FUNC: printf("FUNC"); break;
      case VT_STRUCT: printf("STRUCT"); break;
      case VT_BOOL: printf("BOOL"); break;
      default:
         printf("OTHER=%d", bt);
      }
      if (sv->r & VT_LVAL) printf(",LVAL");
      if ((sv->r & VT_VALMASK) == VT_LOCAL) printf(",VT_LOCAL");
      printf("]");
#endif

    } else if (!(sv->r & VT_LVAL)) { /* no LVAL, is just an address */
      printf("Addr[StackLoc[%d]]", sv->c.i);
    } else { /* fixed location on stack */
      printf("StackLoc[%d]", sv->c.i);
    }
    break;
  case VT_CMP:
    printf("VT_CMP");
    break;
  case VT_JMP:
    printf("VT_JMP");
    break;
  case VT_JMPI:
    printf("VT_JMPI");
    break;
  default: /* must be temporary vreg */
    if (sv->pr0 == -1) {
      tcc_ir_print_vreg(sv->vr);
#if 0
      printf("VReg%d[", sv->vreg);
      int bt = sv->type.t & VT_BTYPE;
      switch (bt) {
      case VT_INT: printf("INT"); break;
      case VT_BYTE: printf("BYTE"); break;
      case VT_SHORT: printf("SHORT"); break;
      case VT_VOID: printf("VOID"); break;
      case VT_PTR: printf("PTR"); break;
      case VT_ENUM: printf("ENUM"); break;
      case VT_FUNC: printf("FUNC"); break;
      case VT_STRUCT: printf("STRUCT"); break;
      case VT_BOOL: printf("BOOL"); break;
      default:
         printf("OTHER=%d", bt);
      }
      if (sv->r & VT_LVAL) printf(",LVAL");
      printf("]");
#endif
      if (sv->r & VT_LVAL)
        printf("***DEREF***");
    } else {
      if (sv->pr0 & PREG_SPILLED)
        printf(SPILL_MARK_BEGIN "SpillLoc[%d]" SPILL_MARK_END, sv->c.i);
      else
        printf("R%d", sv->pr0);
      if (sv->r & VT_LVAL)
        printf("***DEREF***");
    }
    break;
  }
}

void tcc_print_quadruple(TACQuadruple *q, int pc) {
  int op = q->op;
  printf("%04d: ", pc);
  switch (op) {
  case TCCIR_OP_RETURNVALUE:
  case TCCIR_OP_RETURNVOID:
  case TCCIR_OP_FUNCCALLVOID:
  case TCCIR_OP_FUNCCALLVAL:
  case TCCIR_OP_FUNCPARAMVOID:
  case TCCIR_OP_TEST_ZERO:
  case TCCIR_OP_CMP:
    printf("%s ", tcc_ir_get_op_name(op));
    break;
  case TCCIR_OP_FUNCPARAMVAL:
    printf("%s%d ", tcc_ir_get_op_name(op), q->src2.c.i);
    break;
  case TCCIR_OP_JUMP:
  case TCCIR_OP_JUMPIF:
    printf("JMP to %d ", q->dest.c.i);
    break;
  default:
    print_svalue_short(&q->dest);
    printf(" <-- ");
  }

  if (irop_config[op].has_src1) {
    if (op == TCCIR_OP_SETIF) {
      /* Print condition code instead of vreg for SETIF */
      printf("(cond=0x%x)", q->src1.c.i);
    } else if (op != TCCIR_OP_JUMPIF) {
      print_svalue_short(&q->src1);
    }
  }

  if (irop_config[op].has_src2) {
    switch (op) {
    case TCCIR_OP_CMP:
      printf(",");
      print_svalue_short(&q->src2);
      break;
    case TCCIR_OP_FUNCPARAMVAL:
    case TCCIR_OP_FUNCCALLVAL:
      break;
    default:
      printf(" %s ", tcc_ir_get_op_name(op));
      print_svalue_short(&q->src2);
    }
  }
  /* additional information */
  if (op == TCCIR_OP_STORE)
    printf(" [STORE]");
  else if (op == TCCIR_OP_FUNCCALLVAL) {
    printf(" --> ");
    print_svalue_short(&q->dest);
  } else if (op == TCCIR_OP_JUMPIF) {
    printf(" if \"");
    switch (q->src1.c.i) {
    case TOK_EQ:
      printf("==");
      break;
    case TOK_NE:
      printf("!=");
      break;
    case TOK_LT:
      printf("<S");
      break;
    case TOK_GT:
      printf(">S");
      break;
    case TOK_LE:
      printf("<=S");
      break;
    case TOK_GE:
      printf(">=S");
      break;
    case TOK_ULT:
      printf("<U");
      break;
    case TOK_UGT:
      printf(">U");
      break;
    case TOK_ULE:
      printf("<=U");
      break;
    case TOK_UGE:
      printf(">=U");
      break;
    default:
      printf("cc=0x%x", q->src1.c.i);
      break;
    }
    printf("\"");
  }

  else if (op == TCCIR_OP_SETIF) {
    printf("1 if \"");
    switch (q->src1.c.i) {
    case TOK_EQ:
      printf("==");
      break;
    case TOK_NE:
      printf("!=");
      break;
    case TOK_LT:
      printf("<S");
      break;
    case TOK_GT:
      printf(">S");
      break;
    case TOK_LE:
      printf("<=S");
      break;
    case TOK_GE:
      printf(">=S");
      break;
    case TOK_ULT:
      printf("<U");
      break;
    case TOK_UGT:
      printf(">U");
      break;
    case TOK_ULE:
      printf("<=U");
      break;
    case TOK_UGE:
      printf(">=U");
      break;
    }
    printf("\"");
  } // else if (op == TCCIR_OP_) {
  // printf("if \"");
  // switch (quad->src1.c.i) {
  // case TOK_EQ:
  //   printf("==");
  //   break;
  // case TOK_NE:
  //   printf("!=");
  //   break;
  // case TOK_LT:
  //   printf("<S");
  //   break;
  // case TOK_GT:
  //   printf(">S");
  //   break;
  // case TOK_LE:
  //   printf("<=S");
  //   break;
  // case TOK_GE:
  //   printf(">=S");
  //   break;
  // case TOK_ULT:
  //   printf("<U");
  //   break;
  // case TOK_UGT:
  //   printf(">U");
  //   break;
  // case TOK_ULE:
  //   printf("<=U");
  //   break;
  // case TOK_UGE:
  //   printf(">=U");
  //   break;
  //  }
  // printf("\"");
  // }

  printf("\n");
}

void tcc_ir_show(TCCIRState *ir) {
  for (int i = 0; i < ir->next_instruction_index; i++) {
    tcc_print_quadruple(&ir->instructions[i], i);
  }
}

void tcc_ir_drop_return_value(TCCIRState *ir) {
  if (ir->next_instruction_index == 0) {
    return;
  }
  TACQuadruple *last_instr = &ir->instructions[ir->next_instruction_index - 1];
  if (last_instr->op == TCCIR_OP_FUNCCALLVAL) {
    if (tcc_is_vreg_valid(ir, last_instr->dest.vr)) {
      IRLiveInterval *interval =
          tcc_ir_get_live_interval(ir, last_instr->dest.vr);
      interval->start = INTERVAL_NOT_STARTED;
      interval->end = 0;
    }
    last_instr->op = TCCIR_OP_FUNCCALLVOID;
    last_instr->dest.vr = -1;
    last_instr->src1.vr = -1;
  }
}

void tcc_ir_backpatch(TCCIRState *ir, int t, int target_address) {
  SValue *cur;
  printf("Backpatching jump at %d to target %d\n", t, target_address);
  while (t) {
    cur = &ir->instructions[t].dest;
    t = cur->c.i;
    cur->c.i = target_address;
  }
}

void tcc_ir_backpatch_to_here(TCCIRState *ir, int t) {
  tcc_ir_backpatch(ir, t, ir->next_instruction_index);
}

int tcc_ir_generate_test(TCCIRState *ir, int inv, int t) {
  int v;
  v = vtop->r & VT_VALMASK;
  if (v == VT_CMP) {
    SValue src, dest;
    int jtrue = vtop->jtrue;
    int jfalse = vtop->jfalse;

    memset(&src, 0, sizeof(SValue));
    memset(&dest, 0, sizeof(SValue));
    src.vr = -1;
    /* Use cmp_op and invert if needed. In TCC, comparison tokens are designed
     * so that XORing with 1 inverts them (e.g., TOK_EQ ^ 1 = TOK_NE) */
    int cond = vtop->cmp_op ^ inv;
    printf("DEBUG: cmp_op=0x%x, inv=%d, result=0x%x, jtrue=%d, jfalse=%d\n",
           vtop->cmp_op, inv, cond, jtrue, jfalse);
    src.c.i = cond;
    dest.vr = -1;
    dest.c.i = t;
    t = tcc_ir_put(ir, TCCIR_OP_JUMPIF, &src, NULL, &dest);

    /* Handle pending jump chains - merge with the appropriate chain */
    if (inv) {
      /* inv=1: we want to jump when condition is false */
      /* jtrue chain should be merged with t (jump on false) */
      /* jfalse chain should be backpatched to here (they also represent false)
       */
      if (jtrue) {
        tcc_ir_backpatch_first(ir, jtrue, t);
        t = jtrue;
      }
      if (jfalse) {
        tcc_ir_backpatch_to_here(ir, jfalse);
      }
    } else {
      /* inv=0: we want to jump when condition is true */
      /* jfalse chain should be merged with t (jump on true - inverted sense) */
      /* jtrue chain should be backpatched to here */
      if (jfalse) {
        tcc_ir_backpatch_first(ir, jfalse, t);
        t = jfalse;
      }
      if (jtrue) {
        tcc_ir_backpatch_to_here(ir, jtrue);
      }
    }
  } else if (v == VT_JMP || v == VT_JMPI) {
    if ((v & 1) == inv) {
      if (vtop->c.i == -1) {
        vtop->c.i = t;
      } else {
        if (t != -1) {
          tcc_ir_backpatch_first(ir, vtop->c.i, t);
        }
        t = vtop->c.i;
      }
    } else {
      SValue dest;
      memset(&dest, 0, sizeof(SValue));
      dest.vr = -1;
      dest.c.i = t;
      t = tcc_ir_put(ir, TCCIR_OP_JUMP, NULL, NULL, &dest);
      tcc_ir_backpatch_to_here(ir, vtop->c.i);
    }
  } else {
    if ((vtop->r & (VT_VALMASK | VT_LVAL | VT_SYM)) == VT_CONST) {
      if ((vtop->c.i != 0) != inv) {
        SValue dest;
        memset(&dest, 0, sizeof(SValue));
        dest.vr = -1;
        dest.c.i = t;
        t = tcc_ir_put(ir, TCCIR_OP_JUMP, NULL, NULL, &dest);
      }
    } else {
      tcc_ir_put(ir, TCCIR_OP_TEST_ZERO, &vtop[0], NULL, NULL);
      vtop->r = VT_CMP;
      vtop->cmp_op = TOK_NE;
      vtop->jtrue = 0;
      vtop->jfalse = 0;
      return tcc_ir_generate_test(ir, inv, t);
    }
  }
  --vtop;
  return t;
}

void tcc_ir_backpatch_first(TCCIRState *ir, int t, int target_address) {
  int lp;
  do {
    lp = t;
    t = ir->instructions[t].dest.c.i;
  } while (t);
  ir->instructions[lp].dest.c.i = target_address;
}

/* Append target t to end of jump chain n, return head of chain */
int tcc_ir_gjmp_append(TCCIRState *ir, int n, int t) {
  if (n && n < ir->next_instruction_index) {
    tcc_ir_backpatch_first(ir, n, t);
    return n;
  }
  return t;
}

void tcc_ir_generate_cmp_jmp_set(TCCIRState *ir) {
  int v = vtop->r & VT_VALMASK;
  printf("DEBUG cmp_jmp_set: v=%d, vtop->r=0x%x, cmp_op=0x%x, jtrue=%d, "
         "jfalse=%d\n",
         v, vtop->r, vtop->cmp_op, vtop->jtrue, vtop->jfalse);
  if (v == VT_CMP) {
    SValue src, dest;
    int jtrue = vtop->jtrue;
    int jfalse = vtop->jfalse;
    memset(&src, 0, sizeof(SValue));
    memset(&dest, 0, sizeof(SValue));
    dest.vr = tcc_ir_get_vreg_temp(ir);
    dest.type.t = VT_INT;

    if (jtrue || jfalse) {
      /* We have pending jump chains - need to merge them with the comparison */
      SValue jump_dest;
      memset(&jump_dest, 0, sizeof(SValue));
      jump_dest.vr = -1;

      /* Generate SETIF for the comparison part */
      src.vr = -1;
      src.c.i = vtop->cmp_op;
      tcc_ir_put(ir, TCCIR_OP_SETIF, &src, NULL, &dest);

      /* Jump to end */
      jump_dest.c.i = 0;
      int end_jump = tcc_ir_put(ir, TCCIR_OP_JUMP, NULL, NULL, &jump_dest);

      /* Patch jtrue chain to here - set dest = 1 */
      if (jtrue) {
        tcc_ir_backpatch_to_here(ir, jtrue);
        src.r = VT_CONST;
        src.c.i = 1;
        tcc_ir_put(ir, TCCIR_OP_ASSIGN, &src, NULL, &dest);
        if (jfalse) {
          /* Jump over the jfalse handler */
          jump_dest.c.i = 0;
          int skip_jump = tcc_ir_put(ir, TCCIR_OP_JUMP, NULL, NULL, &jump_dest);
          /* Patch jfalse chain to here - set dest = 0 */
          tcc_ir_backpatch_to_here(ir, jfalse);
          src.r = VT_CONST;
          src.c.i = 0;
          tcc_ir_put(ir, TCCIR_OP_ASSIGN, &src, NULL, &dest);
          /* Patch skip_jump to end */
          ir->instructions[skip_jump].dest.c.i = ir->next_instruction_index;
        }
      } else if (jfalse) {
        tcc_ir_backpatch_to_here(ir, jfalse);
        src.r = VT_CONST;
        src.c.i = 0;
        tcc_ir_put(ir, TCCIR_OP_ASSIGN, &src, NULL, &dest);
      }

      /* Patch end_jump to here */
      ir->instructions[end_jump].dest.c.i = ir->next_instruction_index;
      tcc_ir_start_basic_block(ir);
    } else {
      /* Simple case - just SETIF */
      src.vr = -1;
      src.c.i = vtop->cmp_op;
      tcc_ir_put(ir, TCCIR_OP_SETIF, &src, NULL, &dest);
    }

    vtop->vr = dest.vr;
    vtop->r = 0;
  } else if ((v & ~1) == VT_JMP) {
    SValue dest, src1;
    int t, addr;
    memset(&src1, 0, sizeof(SValue));
    memset(&dest, 0, sizeof(SValue));
    dest.vr = tcc_ir_get_vreg_temp(ir);
    dest.type.t = VT_INT;
    src1.vr = -1;
    src1.r = VT_CONST;
    t = v & 1;
    src1.c.i = t;
    addr = tcc_ir_put(ir, TCCIR_OP_ASSIGN, &src1, NULL, &dest);
    src1.c.i = addr + 3;
    tcc_ir_put(ir, TCCIR_OP_JUMP, NULL, NULL, &dest);
    tcc_ir_backpatch_to_here(ir, vtop->c.i);
    src1.c.i = t ^ 1;
    tcc_ir_put(ir, TCCIR_OP_ASSIGN, &src1, NULL, &dest);
    tcc_ir_start_basic_block(ir);
    vtop->vr = dest.vr;
    vtop->r = 0;
  }
}

void tcc_ir_start_basic_block(TCCIRState *ir) { ir->basic_block_start = 1; }