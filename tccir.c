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

static int tcc_ir_is_fpu_operation(TccIrOp op);
static bool tcc_ir_put_soft_call_fpu_if_needed(TCCIRState *ir, TccIrOp op, SValue *src1, SValue *src2, SValue *dest);
static inline int tcc_ir_is_float_type(int t)
{
  int bt = t & VT_BTYPE;
  return bt == VT_FLOAT || bt == VT_DOUBLE || bt == VT_LDOUBLE || bt == VT_QFLOAT;
}

static inline int tcc_ir_is_double_type(int t)
{
  int bt = t & VT_BTYPE;
  return bt == VT_DOUBLE || bt == VT_LDOUBLE;
}

/* Returns true if type is 64-bit (double, ldouble, or long long) */
static inline int tcc_ir_is_64bit_type(int t)
{
  int bt = t & VT_BTYPE;
  return bt == VT_DOUBLE || bt == VT_LDOUBLE || bt == VT_LLONG;
}

void tcc_print_quadruple(TACQuadruple *q, int pc);
void tcc_ir_print_vreg(int vreg);

typedef struct IRRegistersConfig
{
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
    /* Logical boolean operations */
    [TCCIR_OP_BOOL_OR] = {1, 1, 1},   /* dest = (src1 || src2) */
    [TCCIR_OP_BOOL_AND] = {1, 1, 1},  /* dest = (src1 && src2) */
};
// clang-format on

#define IR_MAX_VARS 10000
#define IR_MAX_TEMPS 10000
#define IR_MAX_PARAMS 10000

static int tcc_is_vreg_valid(TCCIRState *ir, int vr)
{
  const int type = TCCIR_DECODE_VREG_TYPE(vr);
  const int position = TCCIR_DECODE_VREG_POSITION(vr);
  switch (type)
  {
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

static IRLiveInterval *tcc_ir_get_live_interval(TCCIRState *ir, int vreg)
{
  if (vreg < 0)
  {
    fprintf(stderr, "tcc_ir_get_live_interval: invalid vreg: %d\n", vreg);
    exit(1);
  }
  int decoded_vreg_position = TCCIR_DECODE_VREG_POSITION(vreg);
  switch (TCCIR_DECODE_VREG_TYPE(vreg))
  {
  case TCCIR_VREG_TYPE_VAR:
  {
    if (decoded_vreg_position >= ir->variables_live_intervals_size)
    {
      fprintf(stderr, "Getting out of bounds live interval for vreg %d\n", vreg);
      exit(1);
    }
    return &ir->variables_live_intervals[decoded_vreg_position];
  }
  case TCCIR_VREG_TYPE_TEMP:
  {
    if (decoded_vreg_position >= ir->temporary_variables_live_intervals_size)
    {
      fprintf(stderr, "Getting out of bounds live interval for vreg %d\n", vreg);
      exit(1);
    }
    return &ir->temporary_variables_live_intervals[decoded_vreg_position];
  }
  case TCCIR_VREG_TYPE_PARAM:
  {
    if (decoded_vreg_position >= ir->parameters_live_intervals_size)
    {
      fprintf(stderr, "Getting out of bounds live interval for vreg %d\n", vreg);
      exit(1);
    }
    return &ir->parameters_live_intervals[decoded_vreg_position];
  }
  default:
    fprintf(stderr, "tcc_ir_get_live_interval: unknown vreg type %d, for vreg: %d\n", TCCIR_DECODE_VREG_TYPE(vreg),
            vreg);
    exit(1);
  }
  return NULL;
}

static void tcc_ir_set_base_interval_end(TCCIRState *ir, int vreg)
{
  IRLiveInterval *interval = tcc_ir_get_live_interval(ir, vreg);
  interval->end = ir->next_instruction_index;
}

/* Initialize all interval start fields to INTERVAL_NOT_STARTED and incoming_reg
 * to -1 */
static void tcc_ir_init_interval_starts(IRLiveInterval *intervals, int count)
{
  for (int i = 0; i < count; ++i)
  {
    intervals[i].start = INTERVAL_NOT_STARTED;
    intervals[i].incoming_reg0 = -1;
    intervals[i].incoming_reg1 = -1;
  }
}

static void tcc_ir_clear_live_intervals(TCCIRState *ir)
{
  ir->variables_live_intervals_size = IR_LIVE_INTERVAL_INIT_SIZE;
  if (ir->variables_live_intervals != NULL)
  {
    tcc_free(ir->variables_live_intervals);
  }
  ir->variables_live_intervals = (IRLiveInterval *)tcc_mallocz(sizeof(IRLiveInterval) * IR_LIVE_INTERVAL_INIT_SIZE);
  tcc_ir_init_interval_starts(ir->variables_live_intervals, IR_LIVE_INTERVAL_INIT_SIZE);
  ir->next_local_variable = 0;

  ir->temporary_variables_live_intervals_size = IR_LIVE_INTERVAL_INIT_SIZE;
  if (ir->temporary_variables_live_intervals != NULL)
  {
    tcc_free(ir->temporary_variables_live_intervals);
  }
  ir->temporary_variables_live_intervals =
      (IRLiveInterval *)tcc_mallocz(sizeof(IRLiveInterval) * IR_LIVE_INTERVAL_INIT_SIZE);
  tcc_ir_init_interval_starts(ir->temporary_variables_live_intervals, IR_LIVE_INTERVAL_INIT_SIZE);
  ir->next_temporary_variable = 0;

  ir->parameters_live_intervals_size = IR_LIVE_INTERVAL_INIT_SIZE;
  if (ir->parameters_live_intervals != NULL)
  {
    tcc_free(ir->parameters_live_intervals);
  }
  ir->parameters_live_intervals = (IRLiveInterval *)tcc_mallocz(sizeof(IRLiveInterval) * IR_LIVE_INTERVAL_INIT_SIZE);
  tcc_ir_init_interval_starts(ir->parameters_live_intervals, IR_LIVE_INTERVAL_INIT_SIZE);
  ir->next_parameter = 0;
}

static int tcc_ir_operand_in_memory(SValue *sv)
{
  const int svt = sv->r & VT_VALMASK;
  if (sv->pr0 == -1)
  {
    if (svt == VT_LOCAL)
    {
      return 1;
    }
    else if (svt == VT_CONST)
    {
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

TCCIRState *tcc_ir_allocate_block()
{
  TCCIRState *block = (TCCIRState *)tcc_mallocz(sizeof(TCCIRState));
  if (!block)
  {
    fprintf(stderr, "tcc_ir_allocate_block: out of memory\n");
    exit(1);
  }
  block->parameters_count = 0;
  block->active_set = (IRLiveInterval **)tcc_mallocz(sizeof(IRLiveInterval *) * tcc_gen_machine_number_of_registers());

  block->next_instruction_index = 0;

  block->leaffunc = 1;
  block->processing_if = 0;
  block->basic_block_start = 1;
  block->prevent_coalescing = 0;

  tcc_ir_clear_live_intervals(block);

  block->instructions_size = QUADRUPLE_INIT_SIZE;
  block->instructions = (TACQuadruple *)tcc_mallocz(sizeof(TACQuadruple) * QUADRUPLE_INIT_SIZE);
  if (!block->instructions)
  {
    fprintf(stderr, "tcc_ir_allocate_block: out of memory\n");
    exit(1);
  }

  tcc_ls_initialize(&block->ls);
  return block;
}

void tcc_ir_release_block(TCCIRState *ir)
{
  if (!ir)
  {
    fprintf(stderr, "tcc_ir_release_block: NULL ir block\n");
    exit(1);
  }

  if (ir->active_set != NULL)
  {
    tcc_free(ir->active_set);
  }

  if (ir->instructions != NULL)
  {
    tcc_free(ir->instructions);
  }

  if (ir->variables_live_intervals != NULL)
  {
    tcc_free(ir->variables_live_intervals);
  }
  if (ir->temporary_variables_live_intervals != NULL)
  {
    tcc_free(ir->temporary_variables_live_intervals);
  }
  if (ir->parameters_live_intervals != NULL)
  {
    tcc_free(ir->parameters_live_intervals);
  }

  tcc_ls_deinitialize(&ir->ls);
  tcc_free(ir);
}

void tcc_ir_add_function_parameters(TCCIRState *ir, CType *func_type)
{
  Sym *sym, *sym2;
  int n = 0, size = 0, align = 0, pn = 0,
      sn = 0; // pn = core registers, sn = stack
  sym = func_type->ref;
  func_vt = sym->type;
  tcc_state->need_frame_pointer = 0;

  for (sym2 = sym->next; sym2 && (n < architecture_config.parameter_registers); sym2 = sym2->next)
  {
    size = type_size(&sym2->type, &align);
    if (is_float(sym2->type.t))
    {
      /* Soft-float ABI: floats/doubles are passed in integer registers.
       * - float (4 bytes): 1 register, no special alignment
       * - double (8 bytes): 2 registers, must start at even register boundary
       * For hard-float (VFP), this would need separate FP register tracking. */
      if (tcc_ir_is_double_type(sym2->type.t))
      {
        /* Align to even register for double (8-byte alignment) */
        n = (n + 1) & ~1;
      }
    }
    n += CEIL_DIV(size, architecture_config.reg_size);
  }

  if (n > architecture_config.parameter_registers)
  {
    n = architecture_config.parameter_registers;
  }

  ir->parameters_count = n;
  // PC must be aligned to 8 bytes for ARM EABI
  n = ALIGN(n * architecture_config.reg_size, architecture_config.stack_align) / architecture_config.reg_size;

  while ((sym = sym->next))
  {
    CType *type = &sym->type;
    int flags;
    int addr = 0;
    size = type_size(type, &align);
    size = CEIL_DIV(size, architecture_config.reg_size);
    align = ALIGN(align, architecture_config.reg_size);
    if (pn < architecture_config.parameter_registers)
    {
      pn = (pn + (align - 1) / 4) & -(align / 4); // ALIGN(pn, align);
      addr = pn * architecture_config.reg_size;
      pn += size;
      if (!sn && pn > architecture_config.parameter_registers)
      {
        sn = pn - architecture_config.parameter_registers;
      }
    }
    else
    {
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

void tcc_ir_gen_opf(TCCIRState *ir, int op)
{
  TccIrOp ir_op;
  SValue dest;
  int is_double;

  /* Determine the IR operation based on token */
  switch (op)
  {
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
    if (op >= TOK_ULT && op <= TOK_GT)
    {
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
  if (ir_op == TCCIR_OP_FNEG)
  {
    memset(&dest, 0, sizeof(SValue));
    dest.vr = tcc_ir_get_vreg_temp(ir);
    dest.r = 0;
    dest.type = vtop->type;
    /* Mark temp as float/double */
    is_double = (vtop->type.t & VT_BTYPE) == VT_DOUBLE || (vtop->type.t & VT_BTYPE) == VT_LDOUBLE;
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
  is_double = (vtop[-1].type.t & VT_BTYPE) == VT_DOUBLE || (vtop[-1].type.t & VT_BTYPE) == VT_LDOUBLE;
  tcc_ir_set_float_type(ir, dest.vr, 1, is_double);
  tcc_ir_put(ir, ir_op, &vtop[-1], &vtop[0], &dest);
  vtop[-1].vr = dest.vr;
  vtop[-1].r = 0;
  --vtop;
}

TccIrOp tcc_irop_from_token(int token)
{
  switch (token)
  {
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
  fprintf(stderr, "tcc_irop_from_token: unknown token %d(0x%x)\n", token, token);
  exit(1);
}

void tcc_ir_gen_opi(TCCIRState *ir, int op)
{
  const TccIrOp ir_op = tcc_irop_from_token(op);
  SValue dest;
  if (ir_op == TCCIR_OP_CMP)
  {
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

const char *tcc_ir_get_op_name(TccIrOp op)
{
  switch (op)
  {
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
  case TCCIR_OP_BOOL_OR:
    return "BOOL_OR";
  case TCCIR_OP_BOOL_AND:
    return "BOOL_AND";
  default:
    return "UNKNOWN_OP";
  }
}

int tcc_ir_put(TCCIRState *ir, TccIrOp op, SValue *src1, SValue *src2, SValue *dest)
{
  // resize array if needed
  const int pos = ir->next_instruction_index;
  TACQuadruple *q;

  if (tcc_ir_is_fpu_operation(op))
  {
    if (tcc_ir_put_soft_call_fpu_if_needed(ir, op, src1, src2, dest))
    {
      return ir->next_instruction_index;
    }
  }

  if (ir->next_instruction_index >= ir->instructions_size)
  {
    ir->instructions_size <<= 1;
    ir->instructions = (TACQuadruple *)tcc_realloc(ir->instructions, sizeof(TACQuadruple) * ir->instructions_size);
    if (!ir->instructions)
    {
      fprintf(stderr, "tcc_ir_put: out of memory\n");
      exit(1);
    }
  }
  q = &ir->instructions[pos];
  q->op = op;

  if (irop_config[op].has_src1 == 1)
  {
    if (src1 == NULL)
    {
      fprintf(stderr, "tcc_ir_put: src1 is NULL for op %s\n", tcc_ir_get_op_name(op));
      exit(1);
    }
    q->src1 = *src1;
    if (tcc_is_vreg_valid(ir, src1->vr))
    {
      tcc_ir_set_base_interval_end(ir, src1->vr);
    }
  }
  else
  {
    q->src1.vr = -1;
  }

  if (irop_config[op].has_src2 == 1)
  {
    if (src2 == NULL)
    {
      fprintf(stderr, "tcc_ir_put: src2 is NULL for op %s\n", tcc_ir_get_op_name(op));
      exit(1);
    }
    q->src2 = *src2;
    if (tcc_is_vreg_valid(ir, src2->vr))
    {
      tcc_ir_set_base_interval_end(ir, src2->vr);
    }
  }
  else
  {
    q->src2.vr = -1;
  }

  if (irop_config[op].has_dest == 1)
  {
    IRLiveInterval *dest_interval = NULL;
    if (dest == NULL)
    {
      fprintf(stderr, "tcc_ir_put: dest is NULL for op %s\n", tcc_ir_get_op_name(op));
      exit(1);
    }
    q->dest = *dest;
    if (tcc_is_vreg_valid(ir, dest->vr))
    {
      /* Ensure the vreg is tagged with the correct type for register
       * allocation. This is important for 64-bit values (double/long long)
       * which require a register pair in soft-float. Some paths create temps
       * without an explicit type tag; deriving it here prevents pr1 from
       * staying -1. */
      if (tcc_ir_is_float_type(dest->type.t))
      {
        tcc_ir_set_float_type(ir, dest->vr, 1, tcc_ir_is_double_type(dest->type.t));
      }
      else if ((dest->type.t & VT_BTYPE) == VT_LLONG)
      {
        tcc_ir_set_llong_type(ir, dest->vr);
      }
      dest_interval = tcc_ir_get_live_interval(ir, dest->vr);
      dest_interval->is_lvalue = 1;
      if (dest_interval->start == INTERVAL_NOT_STARTED)
      {
        dest_interval->start = ir->next_instruction_index;
        if (ir->processing_if && TCCIR_DECODE_VREG_TYPE(dest->vr) == TCCIR_VREG_TYPE_VAR)
        {
          dest_interval->start_within_if = 1;
        }
      }
      dest_interval->end = ir->next_instruction_index;
    }
  }
  else
  {
    q->dest.vr = -1;
  }

  if ((op == TCCIR_OP_FUNCCALLVOID) || (op == TCCIR_OP_FUNCCALLVAL))
  {
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

  if (ir->basic_block_start)
  {
    ir->basic_block_start = 0;
  }
  else if ((!ir->prevent_coalescing) && (op == TCCIR_OP_ASSIGN) &&
           (TCCIR_DECODE_VREG_TYPE(src1->vr) == TCCIR_VREG_TYPE_TEMP) && ((src1->r & VT_LVAL) == 0) &&
           (src1->vr == ir->instructions[pos - 1].dest.vr) && (ir->instructions[pos - 1].op != TCCIR_OP_FUNCCALLVAL))
  {
    ir->instructions[pos - 1].dest = ir->instructions[pos].dest;
    printf("[PATCHED] ");
    tcc_print_quadruple(&ir->instructions[pos - 1], pos - 1);
    IRLiveInterval *interval = tcc_ir_get_live_interval(ir, src1->vr);
    interval->start = INTERVAL_NOT_STARTED;
    interval->end = 0;
    printf("Setting interval of vreg %d to invalid, %p\n", src1->vr, interval);
    if (tcc_is_vreg_valid(ir, ir->instructions[pos].dest.vr))
    {
      IRLiveInterval *dest_interval = tcc_ir_get_live_interval(ir, ir->instructions[pos].dest.vr);
      if (dest_interval->start == pos)
      {
        dest_interval->start = pos - 1;
      }
    }
    // Mark the src1 vreg as ignored
    return ir->next_instruction_index;
  }

  return ir->next_instruction_index++;
}

int tcc_ir_get_vreg_temp(TCCIRState *ir)
{
  if (ir->next_temporary_variable >= ir->temporary_variables_live_intervals_size)
  {
    const int used = ir->temporary_variables_live_intervals_size;
    ir->temporary_variables_live_intervals_size <<= 1;
    ir->temporary_variables_live_intervals = (IRLiveInterval *)tcc_realloc(
        ir->temporary_variables_live_intervals, sizeof(IRLiveInterval) * ir->temporary_variables_live_intervals_size);
    memset(&ir->temporary_variables_live_intervals[used], 0,
           sizeof(IRLiveInterval) * (ir->temporary_variables_live_intervals_size - used));
    tcc_ir_init_interval_starts(&ir->temporary_variables_live_intervals[used],
                                ir->temporary_variables_live_intervals_size - used);
  }
  const int next_temp_vr = ir->next_temporary_variable;
  ++ir->next_temporary_variable;
  return (TCCIR_VREG_TYPE_TEMP << 28) | next_temp_vr;
}

int tcc_ir_get_vreg_var(TCCIRState *ir)
{
  if (ir == NULL)
  {
    return -1;
  }
  if (ir->next_local_variable >= ir->variables_live_intervals_size)
  {
    const int used = ir->variables_live_intervals_size;
    ir->variables_live_intervals_size <<= 1;
    ir->variables_live_intervals = (IRLiveInterval *)tcc_realloc(
        ir->variables_live_intervals, sizeof(IRLiveInterval) * ir->variables_live_intervals_size);
    memset(&ir->variables_live_intervals[used], 0, sizeof(IRLiveInterval) * (ir->variables_live_intervals_size - used));
    tcc_ir_init_interval_starts(&ir->variables_live_intervals[used], ir->variables_live_intervals_size - used);
  }
  const int next_var_vr = ir->next_local_variable;
  ++ir->next_local_variable;
  return (TCCIR_VREG_TYPE_VAR << 28) | next_var_vr;
}

int tcc_ir_get_vreg_param(TCCIRState *ir)
{
  if (ir->next_parameter >= ir->parameters_live_intervals_size)
  {
    const int used = ir->parameters_live_intervals_size;
    ir->parameters_live_intervals_size <<= 1;
    ir->parameters_live_intervals = (IRLiveInterval *)tcc_realloc(
        ir->parameters_live_intervals, sizeof(IRLiveInterval) * ir->parameters_live_intervals_size);
    memset(&ir->parameters_live_intervals[used], 0,
           sizeof(IRLiveInterval) * (ir->parameters_live_intervals_size - used));
    tcc_ir_init_interval_starts(&ir->parameters_live_intervals[used], ir->parameters_live_intervals_size - used);
  }
  const int next_param_vr = ir->next_parameter;
  ++ir->next_parameter;
  return (TCCIR_VREG_TYPE_PARAM << 28) | next_param_vr;
}

void tcc_ir_set_addrtaken(TCCIRState *ir, int vreg)
{
  if (vreg < 0)
    return;
  IRLiveInterval *interval = tcc_ir_get_live_interval(ir, vreg);
  if (interval)
  {
    interval->addrtaken = 1;
  }
}

void tcc_ir_set_float_type(TCCIRState *ir, int vreg, int is_float, int is_double)
{
  if (vreg < 0)
    return;
  IRLiveInterval *interval = tcc_ir_get_live_interval(ir, vreg);
  if (interval)
  {
    interval->is_float = is_float;
    interval->is_double = is_double;
    /* For now, assume soft-float for ARM Thumb (no VFP for doubles) */
    /* TODO: make this configurable based on target */
    interval->use_vfp = 0;
  }
}

void tcc_ir_set_llong_type(TCCIRState *ir, int vreg)
{
  if (vreg < 0)
    return;
  IRLiveInterval *interval = tcc_ir_get_live_interval(ir, vreg);
  if (interval)
  {
    interval->is_llong = 1;
  }
}

void tcc_ir_set_original_offset(TCCIRState *ir, int vreg, int offset)
{
  if (vreg < 0)
    return;
  IRLiveInterval *interval = tcc_ir_get_live_interval(ir, vreg);
  if (interval)
  {
    interval->original_offset = offset;
  }
}

int tcc_ir_get_reg_type(TCCIRState *ir, int vreg)
{
  if (vreg < 0)
    return LS_REG_TYPE_INT;
  IRLiveInterval *interval = tcc_ir_get_live_interval(ir, vreg);
  if (interval)
  {
    if (interval->is_llong)
    {
      return LS_REG_TYPE_LLONG;
    }
    if (interval->is_float)
    {
      if (interval->is_double)
      {
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

static int tcc_get_vreg_type_bit(int vreg_type)
{
  switch (vreg_type)
  {
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

static int tcc_is_vreg_ignored(TCCIRState *ir, int vreg)
{
  const int position = TCCIR_DECODE_VREG_POSITION(vreg);
  const int type = TCCIR_DECODE_VREG_TYPE(vreg);
  const int type_bit = tcc_get_vreg_type_bit(type);
  const int bit_offset = position * IGNORED_VREG_BITS_PER_ENTRY + type_bit;
  const int index = bit_offset / 32;
  const int bit = bit_offset % 32;
  if (ir->ignored_vregs == NULL || type_bit < 0 || index >= ir->ignored_vregs_size)
  {
    return 0;
  }
  return (ir->ignored_vregs[index] & (1 << bit)) != 0;
}

#define IGNORED_VREGS_INIT_SIZE 64

static void tcc_set_vreg_ignored(TCCIRState *ir, int vreg)
{
  const int position = TCCIR_DECODE_VREG_POSITION(vreg);
  const int type = TCCIR_DECODE_VREG_TYPE(vreg);
  const int type_bit = tcc_get_vreg_type_bit(type);
  const int bit_offset = position * IGNORED_VREG_BITS_PER_ENTRY + type_bit;
  const int index = bit_offset / 32;
  const int bit = bit_offset % 32;
  if (type_bit < 0)
  {
    return;
  }

  if (ir->ignored_vregs == NULL)
  {
    ir->ignored_vregs_size = IGNORED_VREGS_INIT_SIZE;
    ir->ignored_vregs = (uint32_t *)tcc_mallocz(sizeof(uint32_t) * ir->ignored_vregs_size);
  }

  // Resize if needed
  while (index >= ir->ignored_vregs_size)
  {
    const int new_size = ir->ignored_vregs_size << 1;
    ir->ignored_vregs = (uint32_t *)tcc_realloc(ir->ignored_vregs, sizeof(uint32_t) * new_size);
    memset(ir->ignored_vregs + ir->ignored_vregs_size, 0, sizeof(uint32_t) * (new_size - ir->ignored_vregs_size));
    ir->ignored_vregs_size = new_size;
  }

  ir->ignored_vregs[index] |= (1 << bit);
}

static int tcc_ir_find_live_interval(TCCIRState *ir, int vreg, int *start, int *end, int check_for_backwards_jumps)
{
  int retval = 0;
  IRLiveInterval *interval = tcc_ir_get_live_interval(ir, vreg);

  *start = interval->start;
  *end = interval->end;

  if (interval->start != INTERVAL_NOT_STARTED)
  {
    retval = 1;
  }

  if (!check_for_backwards_jumps)
  {
    return retval;
  }

  /* Check for backward jumps that would extend the live interval.
   * If a variable is live at a backward jump target, it must stay live
   * until the jump instruction. */
  for (int i = 0; i < ir->next_instruction_index; ++i)
  {
    TACQuadruple *q = &ir->instructions[i];
    if (q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_JUMPIF)
    {
      int jump_target = q->dest.c.i;
      /* Backward jump: target is before the jump instruction */
      if (jump_target < i)
      {
        /* If variable is live at jump target (start <= target),
         * extend end to include the jump instruction */
        if (*start != INTERVAL_NOT_STARTED && *start <= jump_target && *end >= jump_target)
        {
          if (i > *end)
          {
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
static int tcc_ir_has_call_in_range(TCCIRState *ir, int start, int end)
{
  for (int i = start + 1; i < end && i < ir->next_instruction_index; ++i)
  {
    TccIrOp op = ir->instructions[i].op;
    if (i == end - 1)
    {
      if (op == TCCIR_OP_FUNCCALLVOID || op == TCCIR_OP_FUNCCALLVAL)
      {
        return 1;
      }
    }
    if (op == TCCIR_OP_FUNCCALLVOID || op == TCCIR_OP_FUNCCALLVAL)
    {
      return 1;
    }
  }
  return 0;
}

/* Extend live intervals for vregs used as function parameters.
 * When a vreg is passed to FUNCPARAMVAL, it must stay live until the
 * corresponding FUNCCALL instruction. */
static void tcc_ir_extend_param_intervals(TCCIRState *ir)
{
  int in_call = 0;
  int call_index = -1;

  /* Scan forward to find PARAM instructions and their corresponding CALL */
  for (int i = 0; i < ir->next_instruction_index; ++i)
  {
    TACQuadruple *q = &ir->instructions[i];
    if (q->op == TCCIR_OP_FUNCPARAMVAL)
    {
      /* Find the next CALL instruction */
      if (!in_call)
      {
        for (int j = i + 1; j < ir->next_instruction_index; ++j)
        {
          TACQuadruple *q2 = &ir->instructions[j];
          if (q2->op == TCCIR_OP_FUNCCALLVAL || q2->op == TCCIR_OP_FUNCCALLVOID)
          {
            call_index = j;
            in_call = 1;
            break;
          }
        }
      }
      /* Extend the live interval of the source vreg to the call */
      if (call_index >= 0 && tcc_is_vreg_valid(ir, q->src1.vr))
      {
        IRLiveInterval *interval = tcc_ir_get_live_interval(ir, q->src1.vr);
        if (interval && interval->end < call_index)
        {
          interval->end = call_index;
        }
      }
    }
    else if (q->op == TCCIR_OP_FUNCCALLVAL || q->op == TCCIR_OP_FUNCCALLVOID)
    {
      in_call = 0;
      call_index = -1;
    }
  }
}

void tcc_ir_liveness_analysis(TCCIRState *ir)
{
  int start, end;
  int crosses_call;
  int addrtaken;
  int reg_type;
  IRLiveInterval *interval;
  tcc_ls_clear_live_intervals(&ir->ls);

  /* Extend intervals for vregs used as function parameters */
  tcc_ir_extend_param_intervals(ir);

  for (int vreg = 0; vreg < ir->next_local_variable; ++vreg)
  {
    const int encoded_vreg = (TCCIR_VREG_TYPE_VAR << 28) | vreg;
    if (tcc_is_vreg_ignored(ir, vreg))
    {
      continue;
    }
    start = 0;
    end = ~0;
    if (tcc_ir_find_live_interval(ir, encoded_vreg, &start, &end, 1))
    {
      crosses_call = tcc_ir_has_call_in_range(ir, start, end);
      interval = tcc_ir_get_live_interval(ir, encoded_vreg);
      addrtaken = interval ? interval->addrtaken : 0;
      reg_type = tcc_ir_get_reg_type(ir, encoded_vreg);
      if (ir->instructions[end].op == TCCIR_OP_FUNCCALLVAL || ir->instructions[end].op == TCCIR_OP_FUNCCALLVOID)
      {
        end--; /* Do not include call instruction itself */
      }
      tcc_ls_add_live_interval(&ir->ls, encoded_vreg, start, end, crosses_call, addrtaken, reg_type,
                               interval->is_lvalue);
    }
  }

  for (int vreg = 0; vreg < ir->next_temporary_variable; ++vreg)
  {
    const int vreg_encoded = (TCCIR_VREG_TYPE_TEMP << 28) | vreg;
    if (tcc_is_vreg_ignored(ir, vreg))
    {
      continue;
    }
    start = 0;
    end = ~0;
    if (tcc_ir_find_live_interval(ir, vreg_encoded, &start, &end, 1))
    {
      crosses_call = tcc_ir_has_call_in_range(ir, start, end);
      interval = tcc_ir_get_live_interval(ir, vreg_encoded);
      addrtaken = interval ? interval->addrtaken : 0;
      reg_type = tcc_ir_get_reg_type(ir, vreg_encoded);
      if (ir->instructions[end].op == TCCIR_OP_FUNCCALLVAL || ir->instructions[end].op == TCCIR_OP_FUNCCALLVOID)
      {
        end--; /* Do not include call instruction itself */
      }
      tcc_ls_add_live_interval(&ir->ls, vreg_encoded, start, end, crosses_call, addrtaken, reg_type,
                               interval->is_lvalue);
    }
  }

  for (int vreg = 0; vreg < ir->next_parameter; ++vreg)
  {
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
    for (int i = 0; i < ir->next_instruction_index; ++i)
    {
      TACQuadruple *q = &ir->instructions[i];
      if (q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_JUMPIF)
      {
        int jump_target = q->dest.c.i;
        if (jump_target < i && start <= jump_target && end >= jump_target)
        {
          if (i > end)
            end = i;
        }
      }
    }
    if (ir->instructions[end].op == TCCIR_OP_FUNCCALLVAL || ir->instructions[end].op == TCCIR_OP_FUNCCALLVOID)
    {
      end--; /* Do not include call instruction itself */
    }
    crosses_call = tcc_ir_has_call_in_range(ir, start, end);
    addrtaken = interval->addrtaken;
    reg_type = tcc_ir_get_reg_type(ir, vreg_encoded);
    printf("DEBUG liveness: vreg=%d is_float=%d is_double=%d is_llong=%d -> "
           "reg_type=%d, lvalue: %d\n",
           vreg_encoded, interval->is_float, interval->is_double, interval->is_llong, reg_type, interval->is_lvalue);
    tcc_ls_add_live_interval(&ir->ls, vreg_encoded, start, end, crosses_call, addrtaken, reg_type, interval->is_lvalue);
  }
}

void tcc_ir_patch_live_intervals_registers(TCCIRState *ir)
{
  for (int i = 0; i < ir->ls.next_interval_index; ++i)
  {
    LSLiveInterval *interval = &ir->ls.intervals[i];
    printf("DEBUG tcc_ir_patch: vreg=%d stack_loc=%d r0=%d r1=%d\n", interval->vreg, (int)interval->stack_location,
           interval->r0, interval->r1);
    tcc_ir_assign_physical_register(ir, interval->vreg, interval->stack_location, interval->r0, interval->r1);
  }
}

void tcc_ir_assign_physical_register(TCCIRState *ir, int vreg, int offset, int r0, int r1)
{
  IRLiveInterval *interval = tcc_ir_get_live_interval(ir, vreg);
  /* If variable is spilled (offset != 0), mark r0 with PREG_SPILLED flag */
  if (offset != 0)
  {
    printf("DEBUG assign_phys_reg: vreg=%d offset=%d -> setting PREG_SPILLED\n", vreg, offset);
    interval->allocation.r0 = PREG_SPILLED;
  }
  else
  {
    interval->allocation.r0 = r0;
    interval->allocation.r1 = r1;
  }
  interval->allocation.offset = offset;
}

const char *tcc_ir_get_vreg_type_string(int vreg)
{
  switch (TCCIR_DECODE_VREG_TYPE(vreg))
  {
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

void tcc_ir_register_allocation_params(TCCIRState *ir)
{
  /* For leaf functions: parameters can stay in registers r0-r3, UNLESS
   * the linear scan allocator already spilled them due to register pressure.
   * For non-leaf functions: parameters arrive in registers but must be
   * stored to stack since r0-r3 are caller-saved.
   * In both cases, we need to track which register each parameter arrives in.
   */
  int argno = 0; // current register number (r0-r3)
  for (int vreg = 0; vreg < ir->next_parameter; ++vreg)
  {
    const int encoded_vreg = (TCCIR_VREG_TYPE_PARAM << 28) | vreg;
    IRLiveInterval *interval = tcc_ir_get_live_interval(ir, encoded_vreg);
    /* is_double for soft-float (LS_REG_TYPE_DOUBLE_SOFT) or is_llong for 64-bit
     */
    int is_64bit = interval && (interval->is_double || interval->is_llong);

    printf("DEBUG reg_alloc_params: vreg=%d is_double=%d is_llong=%d "
           "is_64bit=%d argno=%d\n",
           vreg, interval ? interval->is_double : -1, interval ? interval->is_llong : -1, is_64bit, argno);

    /* AAPCS: 64-bit values must be aligned to even register pairs */
    if (is_64bit && (argno & 1))
    {
      argno++; /* skip odd register to align to even */
    }

    /* Check if linear scan allocator already spilled this parameter */
    int already_spilled = (interval->allocation.r0 == PREG_SPILLED || interval->allocation.offset != 0);

    if (is_64bit)
    {
      /* 64-bit value (double or long long) takes r0+r1 or r2+r3 */
      if (argno <= 2)
      {
        /* Parameter arrives in registers */
        interval->incoming_reg0 = argno;
        interval->incoming_reg1 = argno + 1;
        if (ir->leaffunc && !already_spilled)
        {
          /* Leaf function and not spilled: keep in registers */
          tcc_ir_assign_physical_register(ir, encoded_vreg, 0, argno, argno + 1);
        }
        /* If already_spilled or non-leaf: keep the spill location from
         * linear scan, prolog will store incoming registers to stack */
      }
      else
      {
        /* Spilled to caller's stack frame - parameter passed on stack */
        interval->incoming_reg0 = -1;
        interval->incoming_reg1 = -1;
        if (ir->leaffunc && !already_spilled)
        {
          tcc_ir_assign_physical_register(ir, encoded_vreg, (argno - 4) * 4, -1, -1);
        }
      }
      argno += 2;
    }
    else
    {
      if (argno <= 3)
      {
        interval->incoming_reg0 = argno;
        interval->incoming_reg1 = -1;
        if (ir->leaffunc && !already_spilled)
        {
          /* Leaf function and not spilled: keep in register */
          tcc_ir_assign_physical_register(ir, encoded_vreg, 0, argno, -1);
        }
      }
      else
      {
        /* Spilled to caller's stack frame - parameter passed on stack */
        interval->incoming_reg0 = -1;
        interval->incoming_reg1 = -1;
        if (ir->leaffunc && !already_spilled)
        {
          tcc_ir_assign_physical_register(ir, encoded_vreg, (argno - 4) * 4, -1, -1);
        }
      }
      argno++;
    }
  }
}

void tcc_ir_fill_registers(TCCIRState *ir, SValue *sv)
{
  if (tcc_is_vreg_valid(ir, sv->vr))
  {
    IRLiveInterval *interval = tcc_ir_get_live_interval(ir, sv->vr);
    printf("DEBUG fill_registers: vr=%d allocation.r0=%d, allocation.r1=%d "
           "allocation.offset=%d\n",
           sv->vr, interval->allocation.r0, interval->allocation.r1, (int)interval->allocation.offset);
    sv->pr0 = interval->allocation.r0;
    sv->pr1 = interval->allocation.r1;
    sv->c.i = interval->allocation.offset;
  }
}

/* Dead Code Elimination pass
 * Removes unreachable instructions by following control flow from entry.
 * Returns 1 if any instructions were eliminated, 0 otherwise.
 */
int tcc_ir_dead_code_elimination(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  if (n == 0)
    return 0;

  /* Allocate reachability bitmap and old->new index mapping */
  uint8_t *reachable = tcc_mallocz((n + 7) / 8);
  int *new_index = tcc_malloc(sizeof(int) * n);

  /* Mark reachable instructions using a worklist algorithm */
  int *worklist = tcc_malloc(sizeof(int) * n);
  int worklist_head = 0, worklist_tail = 0;

#define MARK_REACHABLE(idx)                                                                                            \
  do                                                                                                                   \
  {                                                                                                                    \
    if ((idx) >= 0 && (idx) < n && !(reachable[(idx) / 8] & (1 << ((idx) % 8))))                                       \
    {                                                                                                                  \
      reachable[(idx) / 8] |= (1 << ((idx) % 8));                                                                      \
      worklist[worklist_tail++] = (idx);                                                                               \
    }                                                                                                                  \
  } while (0)

  /* Start from instruction 0 */
  MARK_REACHABLE(0);

  while (worklist_head < worklist_tail)
  {
    int i = worklist[worklist_head++];
    TACQuadruple *q = &ir->instructions[i];

    switch (q->op)
    {
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
  for (int i = 0; i < n; i++)
  {
    if (reachable[i / 8] & (1 << (i % 8)))
    {
      new_index[i] = new_count++;
    }
    else
    {
      new_index[i] = -1; /* Dead instruction */
    }
  }

  /* If nothing was eliminated, clean up and return */
  if (new_count == n)
  {
    tcc_free(reachable);
    tcc_free(new_index);
    tcc_free(worklist);
    return 0;
  }

  /* Compact instructions and update jump targets */
  int write_pos = 0;
  for (int i = 0; i < n; i++)
  {
    if (new_index[i] >= 0)
    {
      TACQuadruple *q = &ir->instructions[i];

      /* Update jump targets */
      if (q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_JUMPIF)
      {
        int old_target = q->dest.c.i;
        if (old_target < n)
        {
          q->dest.c.i = new_index[old_target];
        }
        else
        {
          /* Target is past the end (epilogue) - adjust for removed instructions
           */
          q->dest.c.i = new_count;
        }
      }

      /* Move instruction to new position if needed */
      if (write_pos != i)
      {
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

/* Dead Store Elimination - remove ASSIGN instructions where the destination
 * vreg is never used. This eliminates redundant copies after CSE/idempotent
 * optimizations.
 */
int tcc_ir_dead_store_elimination(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  if (n == 0)
    return 0;

  /* Track which TMP vregs are used as sources */
  int max_tmp_pos = 0;
  for (int i = 0; i < n; i++)
  {
    TACQuadruple *q = &ir->instructions[i];
    if (irop_config[q->op].has_dest && TCCIR_DECODE_VREG_TYPE(q->dest.vr) == TCCIR_VREG_TYPE_TEMP)
    {
      int pos = TCCIR_DECODE_VREG_POSITION(q->dest.vr);
      if (pos > max_tmp_pos)
        max_tmp_pos = pos;
    }
  }

  if (max_tmp_pos == 0)
    return 0;

  uint8_t *used = tcc_mallocz((max_tmp_pos + 8) / 8);

  /* Mark all TMP vregs that are used as sources */
  for (int i = 0; i < n; i++)
  {
    TACQuadruple *q = &ir->instructions[i];

    /* Check src1 */
    if (irop_config[q->op].has_src1 && TCCIR_DECODE_VREG_TYPE(q->src1.vr) == TCCIR_VREG_TYPE_TEMP)
    {
      int pos = TCCIR_DECODE_VREG_POSITION(q->src1.vr);
      if (pos <= max_tmp_pos)
        used[pos / 8] |= (1 << (pos % 8));
    }

    /* Check src2 */
    if (irop_config[q->op].has_src2 && TCCIR_DECODE_VREG_TYPE(q->src2.vr) == TCCIR_VREG_TYPE_TEMP)
    {
      int pos = TCCIR_DECODE_VREG_POSITION(q->src2.vr);
      if (pos <= max_tmp_pos)
        used[pos / 8] |= (1 << (pos % 8));
    }
  }

  /* Remove ASSIGN instructions where dest is an unused TMP vreg */
  int changes = 0;
  int write_pos = 0;
  int *new_index = tcc_malloc(sizeof(int) * n);

  for (int i = 0; i < n; i++)
  {
    TACQuadruple *q = &ir->instructions[i];
    int keep = 1;

    /* Only consider removing ASSIGN instructions */
    if (q->op == TCCIR_OP_ASSIGN && TCCIR_DECODE_VREG_TYPE(q->dest.vr) == TCCIR_VREG_TYPE_TEMP)
    {
      int pos = TCCIR_DECODE_VREG_POSITION(q->dest.vr);
      if (pos <= max_tmp_pos && !(used[pos / 8] & (1 << (pos % 8))))
      {
        /* This ASSIGN's destination is never used - remove it */
        keep = 0;
        changes++;
      }
    }

    new_index[i] = keep ? write_pos : -1;

    if (keep)
    {
      if (write_pos != i)
        ir->instructions[write_pos] = *q;
      write_pos++;
    }
  }

  /* Update jump targets */
  for (int i = 0; i < write_pos; i++)
  {
    TACQuadruple *q = &ir->instructions[i];
    if (q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_JUMPIF)
    {
      int old_target = q->dest.c.i;
      if (old_target < n && new_index[old_target] >= 0)
        q->dest.c.i = new_index[old_target];
      else if (old_target >= n)
        q->dest.c.i = write_pos; /* Past end */
    }
  }

  ir->next_instruction_index = write_pos;

  tcc_free(used);
  tcc_free(new_index);

  return changes;
}

/* Helper: check if two SValues refer to the same virtual register */
static int same_vreg(SValue *a, SValue *b)
{
  /* Both must be vregs (not constants) */
  if ((a->r & VT_VALMASK) == VT_CONST || (b->r & VT_VALMASK) == VT_CONST)
    return 0;
  return a->vr == b->vr;
}

/* Helper: check if two BOOL_OR/BOOL_AND ops have same operands (in any order) */
static int same_bool_operands(TACQuadruple *q1, TACQuadruple *q2)
{
  /* Same order: (a,b) == (a,b) */
  if (same_vreg(&q1->src1, &q2->src1) && same_vreg(&q1->src2, &q2->src2))
    return 1;
  /* Swapped order: (a,b) == (b,a) - only valid for commutative ops */
  if (same_vreg(&q1->src1, &q2->src2) && same_vreg(&q1->src2, &q2->src1))
    return 1;
  return 0;
}

/* Hash table entry for CSE */
typedef struct CSEHashEntry
{
  uint32_t key;        /* hash of (op, min(vr1,vr2), max(vr1,vr2)) */
  int instruction_idx; /* index of instruction that computes this */
  struct CSEHashEntry *next;
} CSEHashEntry;

#define CSE_HASH_SIZE 256

/* Compute hash for a commutative boolean op */
static uint32_t cse_hash(TccIrOp op, int vr1, int vr2)
{
  /* Normalize order for commutative ops */
  int min_vr = (vr1 < vr2) ? vr1 : vr2;
  int max_vr = (vr1 < vr2) ? vr2 : vr1;
  /* Simple hash combining op and both vregs */
  return ((uint32_t)op * 31 + (uint32_t)min_vr * 17 + (uint32_t)max_vr) % CSE_HASH_SIZE;
}

/* Common Subexpression Elimination for commutative boolean ops
 * Pattern: If we see BOOL_OR(a,b) followed by BOOL_OR(b,a),
 *          the second is redundant since OR is commutative.
 * Same applies to BOOL_AND.
 * Optimized with hash table for O(n) average case.
 */
int tcc_ir_bool_cse(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  int changes = 0;

  if (n == 0)
    return 0;

  /* Hash table for seen boolean ops */
  CSEHashEntry *hash_table[CSE_HASH_SIZE] = {0};
  CSEHashEntry *entries = tcc_malloc(sizeof(CSEHashEntry) * n); /* Pool for entries */
  int entry_count = 0;

  for (int i = 0; i < n; i++)
  {
    TACQuadruple *q = &ir->instructions[i];

    /* Only handle BOOL_OR and BOOL_AND - they are commutative */
    if (q->op != TCCIR_OP_BOOL_OR && q->op != TCCIR_OP_BOOL_AND)
      continue;

    uint32_t h = cse_hash(q->op, q->src1.vr, q->src2.vr);

    /* Search hash bucket for match */
    int found = 0;
    for (CSEHashEntry *e = hash_table[h]; e != NULL; e = e->next)
    {
      TACQuadruple *prev = &ir->instructions[e->instruction_idx];
      if (prev->op == q->op && same_bool_operands(prev, q))
      {
        /* Found duplicate! Replace with ASSIGN from previous result */
#ifdef DEBUG_IR_GEN
        printf("OPTIMIZE: CSE %s at %d same as %d -> ASSIGN\n", q->op == TCCIR_OP_BOOL_OR ? "BOOL_OR" : "BOOL_AND", i,
               e->instruction_idx);
#endif
        q->op = TCCIR_OP_ASSIGN;
        q->src1 = prev->dest;
        memset(&q->src2, 0, sizeof(q->src2));
        changes++;
        found = 1;
        break;
      }
    }

    /* If not found, add to hash table */
    if (!found)
    {
      CSEHashEntry *new_entry = &entries[entry_count++];
      new_entry->key = h;
      new_entry->instruction_idx = i;
      new_entry->next = hash_table[h];
      hash_table[h] = new_entry;
    }
  }

  tcc_free(entries);

  return changes;
}

/* Idempotent boolean simplification - eliminate redundant operations with same operands
 * Patterns optimized:
 *   BOOL_AND(x, x) -> x  (idempotent: x && x == x)
 *   BOOL_OR(x, x) -> x   (idempotent: x || x == x)
 * Also handles ASSIGN chains: BOOL_AND(x, y) where y = x -> x
 */
int tcc_ir_bool_idempotent(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  int changes = 0;

  if (n == 0)
    return 0;

  /* Build a map from TMP vreg position -> instruction index that defines it.
   * Only track TMP vregs since they have small indices.
   * VAR/PARAM vregs don't need tracking for ASSIGN chain resolution. */
  int max_tmp_pos = 0;
  for (int i = 0; i < n; i++)
  {
    TACQuadruple *q = &ir->instructions[i];
    if (irop_config[q->op].has_dest && TCCIR_DECODE_VREG_TYPE(q->dest.vr) == TCCIR_VREG_TYPE_TEMP)
    {
      int pos = TCCIR_DECODE_VREG_POSITION(q->dest.vr);
      if (pos > max_tmp_pos)
        max_tmp_pos = pos;
    }
  }

  int *vreg_def = tcc_mallocz(sizeof(int) * (max_tmp_pos + 1));
  for (int i = 0; i <= max_tmp_pos; i++)
    vreg_def[i] = -1;

  for (int i = 0; i < n; i++)
  {
    TACQuadruple *q = &ir->instructions[i];
    if (irop_config[q->op].has_dest && TCCIR_DECODE_VREG_TYPE(q->dest.vr) == TCCIR_VREG_TYPE_TEMP)
    {
      int pos = TCCIR_DECODE_VREG_POSITION(q->dest.vr);
      vreg_def[pos] = i;
    }
  }

  for (int i = 0; i < n; i++)
  {
    TACQuadruple *q = &ir->instructions[i];

    /* Only handle BOOL_OR and BOOL_AND */
    if (q->op != TCCIR_OP_BOOL_OR && q->op != TCCIR_OP_BOOL_AND)
      continue;

    /* Resolve both operands through ASSIGN chains (only for TMP vregs) */
    int vr1 = q->src1.vr;
    int vr2 = q->src2.vr;

    /* Follow ASSIGN chains for TMP vregs */
    while (TCCIR_DECODE_VREG_TYPE(vr1) == TCCIR_VREG_TYPE_TEMP)
    {
      int pos = TCCIR_DECODE_VREG_POSITION(vr1);
      if (pos > max_tmp_pos || vreg_def[pos] < 0)
        break;
      TACQuadruple *def = &ir->instructions[vreg_def[pos]];
      if (def->op != TCCIR_OP_ASSIGN)
        break;
      vr1 = def->src1.vr;
    }

    while (TCCIR_DECODE_VREG_TYPE(vr2) == TCCIR_VREG_TYPE_TEMP)
    {
      int pos = TCCIR_DECODE_VREG_POSITION(vr2);
      if (pos > max_tmp_pos || vreg_def[pos] < 0)
        break;
      TACQuadruple *def = &ir->instructions[vreg_def[pos]];
      if (def->op != TCCIR_OP_ASSIGN)
        break;
      vr2 = def->src1.vr;
    }

    /* Check if both operands resolve to the same vreg */
    if (vr1 == vr2)
    {
      /* Pattern matched: BOOL_OP(x, x) -> x */
#ifdef DEBUG_IR_GEN
      printf("OPTIMIZE: %s(x, x) -> ASSIGN at i=%d (idempotent, resolved vr=%d)\n",
             q->op == TCCIR_OP_BOOL_OR ? "BOOL_OR" : "BOOL_AND", i, vr1);
#endif
      q->op = TCCIR_OP_ASSIGN;
      /* src1 already has the right value, just clear src2 */
      memset(&q->src2, 0, sizeof(q->src2));
      changes++;
    }
  }

  tcc_free(vreg_def);

  return changes;
}

/* Boolean expression simplification - eliminate redundant BOOL_OR/BOOL_AND
 * Patterns optimized:
 *   BOOL_AND(BOOL_OR(a,b), BOOL_OR(b,a)) -> BOOL_OR(a,b)
 *   BOOL_OR(BOOL_AND(a,b), BOOL_AND(b,a)) -> BOOL_AND(a,b)
 */
int tcc_ir_bool_simplification(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  int changes = 0;

  if (n == 0)
    return 0;

  /* Build a map from TMP vreg position -> instruction index that defines it */
  int max_tmp_pos = 0;
  for (int i = 0; i < n; i++)
  {
    TACQuadruple *q = &ir->instructions[i];
    if (irop_config[q->op].has_dest && TCCIR_DECODE_VREG_TYPE(q->dest.vr) == TCCIR_VREG_TYPE_TEMP)
    {
      int pos = TCCIR_DECODE_VREG_POSITION(q->dest.vr);
      if (pos > max_tmp_pos)
        max_tmp_pos = pos;
    }
  }

  int *vreg_def = tcc_mallocz(sizeof(int) * (max_tmp_pos + 1));
  for (int i = 0; i <= max_tmp_pos; i++)
    vreg_def[i] = -1;

  for (int i = 0; i < n; i++)
  {
    TACQuadruple *q = &ir->instructions[i];
    if (irop_config[q->op].has_dest && TCCIR_DECODE_VREG_TYPE(q->dest.vr) == TCCIR_VREG_TYPE_TEMP)
    {
      int pos = TCCIR_DECODE_VREG_POSITION(q->dest.vr);
      vreg_def[pos] = i;
    }
  }

  /* Look for BOOL_AND(BOOL_OR(a,b), BOOL_OR(b,a)) patterns
   * and BOOL_OR(BOOL_AND(a,b), BOOL_AND(b,a)) patterns */
  for (int i = 0; i < n; i++)
  {
    TACQuadruple *q = &ir->instructions[i];

    /* Must be BOOL_AND or BOOL_OR */
    if (q->op != TCCIR_OP_BOOL_AND && q->op != TCCIR_OP_BOOL_OR)
      continue;

    /* Get the defining instructions for both operands (only TMP vregs) */
    int def1 = -1, def2 = -1;
    if (TCCIR_DECODE_VREG_TYPE(q->src1.vr) == TCCIR_VREG_TYPE_TEMP)
    {
      int pos = TCCIR_DECODE_VREG_POSITION(q->src1.vr);
      if (pos <= max_tmp_pos)
        def1 = vreg_def[pos];
    }
    if (TCCIR_DECODE_VREG_TYPE(q->src2.vr) == TCCIR_VREG_TYPE_TEMP)
    {
      int pos = TCCIR_DECODE_VREG_POSITION(q->src2.vr);
      if (pos <= max_tmp_pos)
        def2 = vreg_def[pos];
    }

    if (def1 < 0 || def2 < 0)
      continue;

    TACQuadruple *q1 = &ir->instructions[def1];
    TACQuadruple *q2 = &ir->instructions[def2];

    /* For BOOL_AND, both inputs should be BOOL_OR (and vice versa) */
    TccIrOp expected_inner = (q->op == TCCIR_OP_BOOL_AND) ? TCCIR_OP_BOOL_OR : TCCIR_OP_BOOL_AND;

    if (q1->op != expected_inner || q2->op != expected_inner)
      continue;

    if (!same_bool_operands(q1, q2))
      continue;

    /* Pattern matched!
     * BOOL_AND(BOOL_OR(a,b), BOOL_OR(b,a)) -> BOOL_OR(a,b)
     * BOOL_OR(BOOL_AND(a,b), BOOL_AND(b,a)) -> BOOL_AND(a,b)
     */
#ifdef DEBUG_IR_GEN
    printf("OPTIMIZE: %s(%s, %s) with same operands -> single %s at i=%d\n",
           q->op == TCCIR_OP_BOOL_AND ? "BOOL_AND" : "BOOL_OR",
           expected_inner == TCCIR_OP_BOOL_OR ? "BOOL_OR" : "BOOL_AND",
           expected_inner == TCCIR_OP_BOOL_OR ? "BOOL_OR" : "BOOL_AND",
           expected_inner == TCCIR_OP_BOOL_OR ? "BOOL_OR" : "BOOL_AND", i);
#endif
    /* Replace outer op with ASSIGN from first inner op result */
    q->op = TCCIR_OP_ASSIGN;
    q->src1 = q1->dest; /* Copy the result of first BOOL_OR/BOOL_AND */
    memset(&q->src2, 0, sizeof(q->src2));

    /* The second inner op will be eliminated by DCE if unused */
    changes++;
  }

  tcc_free(vreg_def);

  return changes;
}

static void tcc_ir_backpatch_jumps(TCCIRState *ir, uint32_t *ir_to_code_mapping)
{
  TACQuadruple *q;
  for (int i = 0; i < ir->next_instruction_index; i++)
  {
    q = &ir->instructions[i];
    if (q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_JUMPIF)
    {
      const int instruction_address = ir_to_code_mapping[i];
      const int target_address = ir_to_code_mapping[q->dest.c.i];
      tcc_gen_machine_backpatch_jump(instruction_address, target_address);
    }
  }
}

void tcc_ir_put_soft_call(TCCIRState *ir, TccIrOp op, SValue *src1, SValue *src2, SValue *dest)
{
  SValue param;
  Sym *sym;
  TACQuadruple q = {
      .op = op,
  };
  const char *func_name = NULL;

  printf("DEBUG tcc_ir_generate_soft_call: op=%s\n", tcc_ir_get_op_name(op));

  if (irop_config[q.op].has_src1)
  {
    q.src1 = *src1;
  }
  if (irop_config[q.op].has_src2)
  {
    q.src2 = *src2;
  }
  if (irop_config[q.op].has_dest)
  {
    q.dest = *dest;
  }
  func_name = tcc_get_abi_softcall_name(&q);
  if (func_name == NULL)
  {
    tcc_error("No soft-float ABI function for operation %s\n", tcc_ir_get_op_name(op));
    return;
  }
  printf("DEBUG tcc_ir_generate_soft_call: calling %s\n", func_name);
  memset(&param, 0, sizeof(SValue));
  if (irop_config[q.op].has_src1)
  {
    param.c.i = 0;
    tcc_ir_put(ir, TCCIR_OP_FUNCPARAMVAL, src1, &param, NULL);
  }
  if (irop_config[q.op].has_src2)
  {
    param.c.i = 1;
    tcc_ir_put(ir, TCCIR_OP_FUNCPARAMVAL, src2, &param, NULL);
  }
  sym = external_global_sym(tok_alloc_const(func_name), &func_old_type);
  param.r = VT_CONST | VT_SYM;
  param.sym = sym;
  param.c.i = 0;

  if (irop_config[q.op].has_dest)
  {
    tcc_ir_put(ir, TCCIR_OP_FUNCCALLVAL, &param, NULL, dest);
  }
  else
  {
    tcc_ir_put(ir, TCCIR_OP_FUNCCALLVOID, &param, NULL, NULL);
  }
}

static bool tcc_ir_put_soft_call_fpu_if_needed(TCCIRState *ir, TccIrOp op, SValue *src1, SValue *src2, SValue *dest)
{
  printf("DEBUG tcc_ir_generate_fpu_operation: op=%s\n", tcc_ir_get_op_name(op));
  const int is64bit = tcc_is_64bit_operand(src1) || tcc_is_64bit_operand(src2) || tcc_is_64bit_operand(dest);
  const FloatingPointConfig *fpu = architecture_config.fpu;

  switch (op)
  {
  case TCCIR_OP_FADD:
  {
    if (is64bit && fpu->has_dadd)
    {
      return false; // tcc_gen_machine_fp_op(q);
    }
    else if (!is64bit && fpu->has_fadd)
    {
      return false; // tcc_gen_machine_fp_op(q);
    }
  }
  break;
  case TCCIR_OP_FSUB:
  {
    if (is64bit && fpu->has_dsub)
    {
      return false; // tcc_gen_machine_fp_op(q);
    }
    else if (!is64bit && fpu->has_fsub)
    {
      return false; // tcc_gen_machine_fp_op(q);
    }
  }
  break;
  case TCCIR_OP_FMUL:
  {
    if (is64bit && fpu->has_dmul)
    {
      return false; // tcc_gen_machine_fp_op(q);
    }
    else if (!is64bit && fpu->has_fmul)
    {
      return false; // tcc_gen_machine_fp_op(q);
    }
  }
  break;
  case TCCIR_OP_FDIV:
  {
    if (is64bit && fpu->has_ddiv)
    {
      return false; // tcc_gen_machine_fp_op(q);
    }
    else if (!is64bit && fpu->has_fdiv)
    {
      return false; // tcc_gen_machine_fp_op(q);
    }
  }
  break;
  case TCCIR_OP_FNEG:
  {
    // Negation is always supported if any FP op is supported
    if (is64bit && fpu->has_dneg)
    {
      return false; // tcc_gen_machine_fp_op(q);
    }
    else if (!is64bit && fpu->has_fneg)
    {
      return false; // tcc_gen_machine_fp_op(q);
    }
  }
  break;
  case TCCIR_OP_FCMP:
  {
    if (is64bit && fpu->has_dcmp)
    {
      return false; // tcc_gen_machine_fp_op(q);
    }
    else if (!is64bit && fpu->has_fcmp)
    {
      return false; // tcc_gen_machine_fp_op(q);
    }
  }
  break;
  case TCCIR_OP_CVT_FTOF:
  {
    // float<->double conversion
    const int src1_is_64bit = tcc_is_64bit_operand(src1);
    const int dest_is_64bit = tcc_is_64bit_operand(dest);
    if (src1_is_64bit && !dest_is_64bit)
    {
      // double to float
      if (fpu->has_dtof)
      {
        return false; // tcc_gen_machine_fp_op(q);
      }
    }
    else if (!src1_is_64bit && dest_is_64bit)
    {
      // float to double
      if (fpu->has_ftod)
      {
        return false; // tcc_gen_machine_fp_op(q);
      }
    }
    else if (src1_is_64bit == dest_is_64bit)
    {
      // no conversion needed
      return true;
    }
    else if (!src1_is_64bit && !dest_is_64bit)
    {
      // float to float no conversion needed
      return true;
    }
  }
  break;
  case TCCIR_OP_CVT_ITOF:
  {
    // int/long->float/double conversion
    const int src1_is_64bit = tcc_is_64bit_operand(src1);
    const int dest_is_64bit = tcc_is_64bit_operand(dest);
    if (src1_is_64bit && !dest_is_64bit)
    {
      // double to int
      if (fpu->has_itod)
      {
        return false; // tcc_gen_machine_fp_op(q);
      }
    }
    else if (!src1_is_64bit && !dest_is_64bit)
    {
      // float to int
      if (fpu->has_itof)
      {
        return false; // tcc_gen_machine_fp_op(q);
      }
    }
    else if (src1_is_64bit && dest_is_64bit)
    {
      if (fpu->has_ltod)
      {
        return false; // tcc_gen_machine_fp_op(q);
      }
    }
    else if (!src1_is_64bit && dest_is_64bit)
    {
      // float to float no conversion needed
      if (fpu->has_ltof)
      {
        return false; // tcc_gen_machine_fp_op(q);
      }
    }
  }
  break;
  case TCCIR_OP_CVT_FTOI:
  {
    // int/long<-float/double conversion
    const int src1_is_64bit = tcc_is_64bit_operand(src1);
    const int dest_is_64bit = tcc_is_64bit_operand(dest);
    if (src1_is_64bit && !dest_is_64bit)
    {
      // double to int
      if (fpu->has_dtoi)
      {
        return false; // tcc_gen_machine_fp_op(q);
      }
    }
    else if (!src1_is_64bit && !dest_is_64bit)
    {
      // float to int
      if (fpu->has_ftoi)
      {
        return false; // tcc_gen_machine_fp_op(q);
      }
    }
    else if (src1_is_64bit && dest_is_64bit)
    {
      if (fpu->has_dtol)
      {
        return false; // tcc_gen_machine_fp_op(q);
      }
    }
    else if (!src1_is_64bit && dest_is_64bit)
    {
      // float to float no conversion needed
      if (fpu->has_ftol)
      {
        return false; // tcc_gen_machine_fp_op(q);
      }
    }
  }
  break;
  default:
  {
    tcc_error("tcc_ir_generate_fpu_operation: unsupported FP op %d", op);
  }
  }
  tcc_ir_put_soft_call(ir, op, src1, src2, dest);
  return true;
}

static int tcc_ir_is_fpu_operation(TccIrOp op)
{
  switch (op)
  {
  case TCCIR_OP_FADD:
  case TCCIR_OP_FSUB:
  case TCCIR_OP_FMUL:
  case TCCIR_OP_FDIV:
  case TCCIR_OP_FNEG:
  case TCCIR_OP_FCMP:
  case TCCIR_OP_CVT_FTOF:
  case TCCIR_OP_CVT_ITOF:
  case TCCIR_OP_CVT_FTOI:
    return 1;
  default:
    return 0;
  }
}

void tcc_ir_generate_code(TCCIRState *ir)
{
  TACQuadruple *q;
  int drop_return_value = 0;

  // +1 to include epilogue when needed
  uint32_t *ir_to_code_mapping = tcc_mallocz(sizeof(uint32_t) * (ir->next_instruction_index + 1));

  /* Track addresses of return jumps for later backpatching to epilogue */
  int *return_jump_addrs = tcc_malloc(sizeof(int) * ir->next_instruction_index);
  int num_return_jumps = 0;

  // generate prolog
  int stack_size = (-loc + 7) & ~7; // align to 8 bytes
  tcc_gen_machine_prolog(ir->leaffunc, ir->ls.dirty_registers, stack_size);

  // /* Save parameters from incoming registers (r0-r3) to their stack locations
  //  * when they are spilled. This is needed for:
  //  * - Non-leaf functions: r0-r3 are caller-saved and get clobbered by calls
  //  * - Leaf functions with spilled params: register pressure forced a spill
  //  * For leaf functions with register-allocated params, they stay in r0-r3.
  //  */
  // printf("DEBUG tcc_ir_generate_code: leaffunc=%d next_parameter=%d\n",
  //        ir->leaffunc, ir->next_parameter);
  // for (int vreg = 0; vreg < ir->next_parameter; ++vreg) {
  //   const int encoded_vreg = (TCCIR_VREG_TYPE_PARAM << 28) | vreg;
  //   IRLiveInterval *interval = tcc_ir_get_live_interval(ir, encoded_vreg);

  //   int incoming_r0 = interval->incoming_reg0;
  //   int incoming_r1 = interval->incoming_reg1;

  //   /* Check if parameter is spilled to stack */
  //   int is_spilled = (interval->allocation.r0 == PREG_SPILLED ||
  //                     interval->allocation.offset != 0);

  //   /* 64-bit parameters (double or long long) need both registers saved */
  //   int is_64bit = interval->is_double || interval->is_llong;

  //   /* Get allocated registers (if not spilled) */
  //   int alloc_r0 = interval->allocation.r0;
  //   int alloc_r1 = interval->allocation.r1;

  //   printf("DEBUG prolog param %d: incoming_r0=%d incoming_r1=%d "
  //          "alloc.r0=%d alloc.r1=%d alloc.offset=%d is_double=%d is_llong=%d
  //          " "is_64bit=%d is_spilled=%d\n", vreg, incoming_r0, incoming_r1,
  //          alloc_r0, alloc_r1, (int)interval->allocation.offset,
  //          interval->is_double, interval->is_llong, is_64bit, is_spilled);

  //   /* If parameter arrived in registers, we need to either:
  //    * 1. Copy to allocated callee-saved registers (non-leaf, not spilled),
  //    OR
  //    * 2. Store to stack (spilled) */
  //   if (incoming_r0 >= 0) {
  //     if (is_spilled) {
  //       /* Parameter needs to be saved to stack at its allocated offset.
  //        * Use allocation.offset which is where loads will read from. */
  //       int stack_offset = interval->allocation.offset;
  //       if (is_64bit && incoming_r1 >= 0) {
  //         /* 64-bit parameter: save both registers */
  //         tcc_gen_machine_store_to_stack(incoming_r0, stack_offset);
  //         tcc_gen_machine_store_to_stack(incoming_r1, stack_offset + 4);
  //       } else {
  //         /* Single 32-bit parameter */
  //         tcc_gen_machine_store_to_stack(incoming_r0, stack_offset);
  //       }
  //     } else if (!ir->leaffunc && alloc_r0 != incoming_r0) {
  //       /* Non-leaf function: copy from incoming to callee-saved register */
  //       /* For 64-bit: copy both registers */
  //       if (is_64bit && incoming_r1 >= 0 && alloc_r1 >= 0) {
  //         tcc_gen_machine_move_reg(alloc_r0, incoming_r0);
  //         tcc_gen_machine_move_reg(alloc_r1, incoming_r1);
  //       } else {
  //         tcc_gen_machine_move_reg(alloc_r0, incoming_r0);
  //       }
  //     }
  //     /* For leaf functions where alloc == incoming, no action needed */
  //   }
  //   /* Parameters that arrived on stack stay where they are (accessed via
  //    * offset_to_args) */
  // }

  for (int i = 0; i < ir->next_instruction_index; i++)
  {
    drop_return_value = 0;
    q = &ir->instructions[i];

    ir_to_code_mapping[i] = ind;

    // emit debug line info for this IR instruction AFTER recording ind
    tcc_debug_line_num(tcc_state, q->line_num);

    if (irop_config[q->op].has_src1 == 1)
    {
      tcc_ir_fill_registers(ir, &q->src1);
      if (q->op != TCCIR_OP_FUNCCALLVAL && q->op != TCCIR_OP_FUNCCALLVOID && q->op != TCCIR_OP_FUNCPARAMVAL &&
          q->op != TCCIR_OP_FUNCPARAMVOID && q->op != TCCIR_OP_ASSIGN)
      {
        if (tcc_ir_operand_in_memory(&q->src1))
        {
          /* Skip 64-bit types here - they need special handling with register
           * pairs, which individual operation handlers provide. Using R12 alone
           * doesn't work because R12+1 = R13 = SP. */
          if (!tcc_ir_is_64bit_type(q->src1.type.t))
          {
            q->src1.pr0 = architecture_config.scratch_register;
            tcc_gen_machine_load_register(&q->src1);
          }
        }
      }
    }

    if (irop_config[q->op].has_src2 == 1)
    {
      tcc_ir_fill_registers(ir, &q->src2);
      if (tcc_ir_operand_in_memory(&q->src2))
      {
        /* Skip 64-bit types - need special handling */
        if (!tcc_ir_is_64bit_type(q->src2.type.t))
        {
          q->src2.pr0 = architecture_config.scratch_register;
          tcc_gen_machine_load_register(&q->src2);
        }
      }
    }

    if (irop_config[q->op].has_dest == 1)
    {
      tcc_ir_fill_registers(ir, &q->dest);
      /* Don't pre-load destination to scratch register for operations that
       * handle their own destination storage (e.g., float conversions that
       * may need to store to spilled stack locations) */
      if (tcc_ir_operand_in_memory(&q->dest) && (q->op != TCCIR_OP_ASSIGN) && (q->op != TCCIR_OP_STORE) &&
          (q->op != TCCIR_OP_CVT_FTOF) && (q->op != TCCIR_OP_CVT_ITOF) && (q->op != TCCIR_OP_CVT_FTOI))
      {
        /* Skip 64-bit types - need special handling */
        if (!tcc_ir_is_64bit_type(q->dest.type.t))
        {
          q->dest.pr0 = architecture_config.scratch_register;
          tcc_gen_machine_load_register(&q->dest);
        }
      }
    }

    switch (q->op)
    {
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
    case TCCIR_OP_RETURNVOID:
      /* Emit jump to epilogue (will be backpatched later) */
      /* if return is last instruction, then jump is not needed */
      if (i != ir->next_instruction_index - 1)
      {
        return_jump_addrs[num_return_jumps++] = ind;
        tcc_gen_machine_jump_op(q);
      }
      break;
    case TCCIR_OP_ASSIGN:
      tcc_gen_machine_assign_op(q);
      break;
    case TCCIR_OP_FUNCPARAMVAL:
    {
      const int param_num = q->src2.c.i; /* 1-based param number */
      tcc_gen_machine_func_param_op(q, param_num, i);
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
    case TCCIR_OP_BOOL_OR:
    case TCCIR_OP_BOOL_AND:
      tcc_gen_machine_bool_op(q);
      break;
    case TCCIR_OP_FUNCPARAMVOID:
      break;
    case TCCIR_OP_FUNCCALLVOID:
      drop_return_value = 1;
      /* fall through */
    case TCCIR_OP_FUNCCALLVAL:
    {
      // Save the call instruction index before potentially incrementing i
      int call_idx = i;
      // if return follows call then we can optimize away move
      const TACQuadruple *ir_next = (i + 1 < ir->next_instruction_index) ? &ir->instructions[i + 1] : NULL;
      if (ir_next && ir_next->op == TCCIR_OP_RETURNVALUE && ir_next->src1.vr == q->dest.vr && q->src1.vr != -1)
      {
        q->dest.pr0 = REG_IRET;
        ++i; // skip next instruction
      }

      tcc_gen_machine_func_call_op(q, drop_return_value, ir, call_idx);
      /* Restore outer call's arguments if this was a nested call */
      ir_to_code_mapping[i] = ind;
      break;
    }
    default:
    {
      printf("Unsupported operation in tcc_generate_code: %s\n", tcc_ir_get_op_name(q->op));
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
  for (int i = 0; i < num_return_jumps; i++)
  {
    tcc_gen_machine_backpatch_jump(return_jump_addrs[i], epilogue_addr);
  }

  tcc_free(ir_to_code_mapping);
  tcc_free(return_jump_addrs);
}

void tcc_ir_print_vreg(int vreg)
{
  printf("VReg %s:%d", tcc_ir_get_vreg_type_string(vreg), TCCIR_DECODE_VREG_POSITION(vreg));
}

void print_svalue_short(SValue *sv)
{
  int val_loc = sv->r & VT_VALMASK;
#define SPILL_MARK_BEGIN "\033[41m"
#define SPILL_MARK_END "\033[0m"

  /* XXX: probably show ignored vregs in a special way */
  switch (val_loc)
  {
  case VT_CONST:
    if (sv->r & VT_SYM)
    {
      printf("GlobalSym(%d)", sv->sym->v);
      if (sv->c.i != 0)
        printf("+%d", sv->c.i);
    }
    else
      printf("#%d", sv->c.i);
    break;
  case VT_LLOCAL:
    printf("VT_LLOCAL (cval=%d)", sv->c.i);
    break;
  // case VT_LOCAL: printf("VReg%d[stack_offset=%d]", sv->vreg, sv->c.i); break;
  case VT_LOCAL:
    if (sv->pr0 != -1)
    { /* already register-allocated? */
      if (sv->pr0 & PREG_SPILLED)
        printf(SPILL_MARK_BEGIN "SpillLoc[%d]" SPILL_MARK_END, sv->c.i);
      else
        printf("R%d", sv->pr0);
    }
    else if (sv->vr != -1)
    { /* not reg-alloced, but vreg'ed? */
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
    }
    else if (!(sv->r & VT_LVAL))
    { /* no LVAL, is just an address */
      printf("Addr[StackLoc[%d]]", sv->c.i);
    }
    else
    { /* fixed location on stack */
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
    if (sv->pr0 == -1)
    {
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
    }
    else
    {
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

void tcc_print_quadruple(TACQuadruple *q, int pc)
{
  int op = q->op;
  printf("%04d: ", pc);
  switch (op)
  {
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

  if (irop_config[op].has_src1)
  {
    if (op == TCCIR_OP_SETIF)
    {
      /* Print condition code instead of vreg for SETIF */
      printf("(cond=0x%x)", q->src1.c.i);
    }
    else if (op != TCCIR_OP_JUMPIF)
    {
      print_svalue_short(&q->src1);
    }
  }

  if (irop_config[op].has_src2)
  {
    switch (op)
    {
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
  else if (op == TCCIR_OP_FUNCCALLVAL)
  {
    printf(" --> ");
    print_svalue_short(&q->dest);
  }
  else if (op == TCCIR_OP_JUMPIF)
  {
    printf(" if \"");
    switch (q->src1.c.i)
    {
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

  else if (op == TCCIR_OP_SETIF)
  {
    printf("1 if \"");
    switch (q->src1.c.i)
    {
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

void tcc_ir_show(TCCIRState *ir)
{
  for (int i = 0; i < ir->next_instruction_index; i++)
  {
    tcc_print_quadruple(&ir->instructions[i], i);
  }
}

void tcc_ir_drop_return_value(TCCIRState *ir)
{
  if (ir->next_instruction_index == 0)
  {
    return;
  }
  TACQuadruple *last_instr = &ir->instructions[ir->next_instruction_index - 1];
  if (last_instr->op == TCCIR_OP_FUNCCALLVAL)
  {
    if (tcc_is_vreg_valid(ir, last_instr->dest.vr))
    {
      IRLiveInterval *interval = tcc_ir_get_live_interval(ir, last_instr->dest.vr);
      interval->start = INTERVAL_NOT_STARTED;
      interval->end = 0;
    }
    last_instr->op = TCCIR_OP_FUNCCALLVOID;
    last_instr->dest.vr = -1;
    last_instr->src1.vr = -1;
  }
}

void tcc_ir_backpatch(TCCIRState *ir, int t, int target_address)
{
  SValue *cur;
  printf("Backpatching jump at %d to target %d\n", t, target_address);
  while (t)
  {
    cur = &ir->instructions[t].dest;
    t = cur->c.i;
    cur->c.i = target_address;
  }
}

void tcc_ir_backpatch_to_here(TCCIRState *ir, int t)
{
  tcc_ir_backpatch(ir, t, ir->next_instruction_index);
}

int tcc_ir_generate_test(TCCIRState *ir, int inv, int t)
{
  int v;
  v = vtop->r & VT_VALMASK;
  if (v == VT_CMP)
  {
    SValue src, dest;
    int jtrue = vtop->jtrue;
    int jfalse = vtop->jfalse;

    memset(&src, 0, sizeof(SValue));
    memset(&dest, 0, sizeof(SValue));
    src.vr = -1;
    /* Use cmp_op and invert if needed. In TCC, comparison tokens are designed
     * so that XORing with 1 inverts them (e.g., TOK_EQ ^ 1 = TOK_NE) */
    int cond = vtop->cmp_op ^ inv;
    printf("DEBUG: cmp_op=0x%x, inv=%d, result=0x%x, jtrue=%d, jfalse=%d\n", vtop->cmp_op, inv, cond, jtrue, jfalse);
    src.c.i = cond;
    dest.vr = -1;
    dest.c.i = t;
    t = tcc_ir_put(ir, TCCIR_OP_JUMPIF, &src, NULL, &dest);

    /* Handle pending jump chains - merge with the appropriate chain */
    if (inv)
    {
      /* inv=1: we want to jump when condition is false */
      /* jtrue chain should be merged with t (jump on false) */
      /* jfalse chain should be backpatched to here (they also represent false)
       */
      if (jtrue)
      {
        tcc_ir_backpatch_first(ir, jtrue, t);
        t = jtrue;
      }
      if (jfalse)
      {
        tcc_ir_backpatch_to_here(ir, jfalse);
      }
    }
    else
    {
      /* inv=0: we want to jump when condition is true */
      /* jfalse chain should be merged with t (jump on true - inverted sense) */
      /* jtrue chain should be backpatched to here */
      if (jfalse)
      {
        tcc_ir_backpatch_first(ir, jfalse, t);
        t = jfalse;
      }
      if (jtrue)
      {
        tcc_ir_backpatch_to_here(ir, jtrue);
      }
    }
  }
  else if (v == VT_JMP || v == VT_JMPI)
  {
    if ((v & 1) == inv)
    {
      if (vtop->c.i == -1)
      {
        vtop->c.i = t;
      }
      else
      {
        if (t != -1)
        {
          tcc_ir_backpatch_first(ir, vtop->c.i, t);
        }
        t = vtop->c.i;
      }
    }
    else
    {
      SValue dest;
      memset(&dest, 0, sizeof(SValue));
      dest.vr = -1;
      dest.c.i = t;
      t = tcc_ir_put(ir, TCCIR_OP_JUMP, NULL, NULL, &dest);
      tcc_ir_backpatch_to_here(ir, vtop->c.i);
    }
  }
  else
  {
    if ((vtop->r & (VT_VALMASK | VT_LVAL | VT_SYM)) == VT_CONST)
    {
      if ((vtop->c.i != 0) != inv)
      {
        SValue dest;
        memset(&dest, 0, sizeof(SValue));
        dest.vr = -1;
        dest.c.i = t;
        t = tcc_ir_put(ir, TCCIR_OP_JUMP, NULL, NULL, &dest);
      }
    }
    else
    {
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

void tcc_ir_backpatch_first(TCCIRState *ir, int t, int target_address)
{
  int lp;
  do
  {
    lp = t;
    t = ir->instructions[t].dest.c.i;
  } while (t);
  ir->instructions[lp].dest.c.i = target_address;
}

/* Append target t to end of jump chain n, return head of chain */
int tcc_ir_gjmp_append(TCCIRState *ir, int n, int t)
{
  if (n && n < ir->next_instruction_index)
  {
    tcc_ir_backpatch_first(ir, n, t);
    return n;
  }
  return t;
}

void tcc_ir_generate_cmp_jmp_set(TCCIRState *ir)
{
  int v = vtop->r & VT_VALMASK;
  printf("DEBUG cmp_jmp_set: v=%d, vtop->r=0x%x, cmp_op=0x%x, jtrue=%d, "
         "jfalse=%d\n",
         v, vtop->r, vtop->cmp_op, vtop->jtrue, vtop->jfalse);
  if (v == VT_CMP)
  {
    SValue src, dest;
    int jtrue = vtop->jtrue;
    int jfalse = vtop->jfalse;
    memset(&src, 0, sizeof(SValue));
    memset(&dest, 0, sizeof(SValue));
    dest.vr = tcc_ir_get_vreg_temp(ir);
    dest.type.t = VT_INT;
    dest.pr0 = -1;
    dest.pr1 = -1;

    if (jtrue || jfalse)
    {
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
      if (jtrue)
      {
        tcc_ir_backpatch_to_here(ir, jtrue);
        src.r = VT_CONST;
        src.c.i = 1;
        src.pr0 = -1;
        src.pr1 = -1;
        tcc_ir_put(ir, TCCIR_OP_ASSIGN, &src, NULL, &dest);
        if (jfalse)
        {
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
      }
      else if (jfalse)
      {
        tcc_ir_backpatch_to_here(ir, jfalse);
        src.r = VT_CONST;
        src.c.i = 0;
        tcc_ir_put(ir, TCCIR_OP_ASSIGN, &src, NULL, &dest);
      }

      /* Patch end_jump to here */
      ir->instructions[end_jump].dest.c.i = ir->next_instruction_index;
      tcc_ir_start_basic_block(ir);
    }
    else
    {
      /* Simple case - just SETIF */
      src.vr = -1;
      src.c.i = vtop->cmp_op;
      tcc_ir_put(ir, TCCIR_OP_SETIF, &src, NULL, &dest);
    }

    vtop->vr = dest.vr;
    vtop->r = 0;
  }
  else if ((v & ~1) == VT_JMP)
  {
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

void tcc_ir_start_basic_block(TCCIRState *ir)
{
  ir->basic_block_start = 1;
}

ST_FUNC int tcc_has_quadruple_64bit_operand(TACQuadruple *q)
{
  /* Check both operands for 64-bit (double/long long) types */
  if (tcc_is_64bit_operand(&q->src1))
  {
    return 1;
  }
  if (tcc_is_64bit_operand(&q->src2))
  {
    return 1;
  }
  return 0;
}