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
#define USING_GLOBALS
#include "tcc.h"

#include "tccdebug.h"

#ifndef TCC_DUMP_THUMB_GEN_SPAN
#define TCC_DUMP_THUMB_GEN_SPAN 1
#endif

#include <ctype.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__linux__) || defined(__APPLE__)
#include <fcntl.h>
#include <unistd.h>
#endif

#ifndef TCC_DUMP_THUMB_GEN
#define TCC_DUMP_THUMB_GEN 1
#endif

#ifndef TCC_DUMP_THUMB_GEN_MNEMONICS
#define TCC_DUMP_THUMB_GEN_MNEMONICS 0
#endif

#if TCC_DUMP_THUMB_GEN
#define THGEN_DUMP(...) fprintf(stderr, __VA_ARGS__)
#else
#define THGEN_DUMP(...)                                                                                                \
  do                                                                                                                   \
  {                                                                                                                    \
  } while (0)
#endif

/* Terminal color codes for spill locations in debug output */
#define SPILL_MARK_BEGIN "\033[41m"
#define SPILL_MARK_END "\033[0m"

#if TCC_DUMP_THUMB_GEN
/* Forward declarations for debug functions */
typedef struct IRRegistersConfig
{
  uint8_t has_dest : 1;
  uint8_t has_src1 : 1;
  uint8_t has_src2 : 1;
} IRRegistersConfig;

extern const IRRegistersConfig irop_config[];
const char *tcc_ir_get_vreg_type_string(int vreg);
static bool tcc_ir_operand_needs_dereference(SValue *sv);
#endif

static inline int is_thumb2_32bit_prefix(uint16_t h1)
{
  /* Thumb-2 32-bit instructions have a first halfword with top 5 bits:
   *   11101 (0xE800..0xEFFF)
   *   11110 (0xF000..0xF7FF)
   *   11111 (0xF800..0xFFFF)
   */
  uint16_t top5 = h1 & 0xF800;
  return top5 == 0xE800 || top5 == 0xF000 || top5 == 0xF800;
}

#if TCC_DUMP_THUMB_GEN && (defined(__linux__) || defined(__APPLE__))
static int tcc_try_dump_thumb_with_objdump(const unsigned char *bytes, size_t len, uint32_t start_vma);
#endif

static void tcc_dump_thumb_generated_span(uint32_t start, uint32_t end)
{
#if TCC_DUMP_THUMB_GEN
  if (!cur_text_section || !cur_text_section->data)
    return;
  if (end <= start)
    return;

  unsigned char *data = cur_text_section->data;

  if (TCC_DUMP_THUMB_GEN_MNEMONICS)
  {
    if (tcc_try_dump_thumb_with_objdump(data + start, (size_t)(end - start), start))
      return;
  }

  uint32_t pc = start;

  while (pc < end)
  {
    if (pc + 2 > end)
    {
      THGEN_DUMP("  %08x: <truncated %u byte>\n", pc, (unsigned)(end - pc));
      break;
    }

    uint16_t h1 = (uint16_t)(data[pc] | (data[pc + 1] << 8));

    if (is_thumb2_32bit_prefix(h1) && pc + 4 <= end)
    {
      uint16_t h2 = (uint16_t)(data[pc + 2] | (data[pc + 3] << 8));
      uint32_t op32 = ((uint32_t)h1 << 16) | (uint32_t)h2;
      THGEN_DUMP("  %08x: %04x %04x    ; thumb32 0x%08x\n", pc, h1, h2, op32);
      pc += 4;
    }
    else
    {
      THGEN_DUMP("  %08x: %04x         ; thumb16\n", pc, h1);
      pc += 2;
    }
  }
#endif
}

#if TCC_DUMP_THUMB_GEN
static void tcc_dump_svalue_short_to(FILE *out, const SValue *sv)
{
  if (!sv)
  {
    fprintf(out, "<null>");
    return;
  }

  const int r = sv->r;
  const int val_loc = r & VT_VALMASK;
  switch (val_loc)
  {
  case VT_CONST:
    if (r & VT_SYM)
    {
      fprintf(out, "%s", get_tok_str(sv->sym ? sv->sym->v : 0, NULL));
      if (sv->c.i)
        fprintf(out, "+%d", (int)sv->c.i);
    }
    else
    {
      if (!(r & VT_LVAL))
        fprintf(out, "#%d", (int)sv->c.i);
      else
        fprintf(out, "#%d***DEREF***", (int)sv->c.i);
    }
    break;
  case VT_LLOCAL:
    /* VT_LLOCAL with VT_LVAL: spilled pointer needing double dereference */
    if (sv->pr0 != PREG_NONE && (sv->pr0 & PREG_SPILLED))
      fprintf(out, SPILL_MARK_BEGIN "SpillLoc[%d]***DEREF***" SPILL_MARK_END, (int)sv->c.i);
    else
      fprintf(out, "VT_LLOCAL(cval=%d)", (int)sv->c.i);
    break;
  case VT_LOCAL:
    if (sv->pr0 != PREG_NONE)
    {
      if (sv->pr0 & PREG_SPILLED)
        fprintf(out, SPILL_MARK_BEGIN "SpillLoc[%d]" SPILL_MARK_END, (int)sv->c.i);
      else
      {
        if (!(r & VT_LVAL))
          fprintf(out, "&");
        fprintf(out, "R%d", sv->pr0);
      }
    }
    else if (sv->vr != -1)
    {
      if (!(r & VT_LVAL))
        fprintf(out, "&");
      /* Match tcc_ir_print_vreg() formatting */
      fprintf(out, "VReg %s:%d", tcc_ir_get_vreg_type_string(sv->vr), TCCIR_DECODE_VREG_POSITION(sv->vr));
    }
    else if (!(r & VT_LVAL))
    {
      fprintf(out, "Addr[StackLoc[%d]]", (int)sv->c.i);
    }
    else
    {
      fprintf(out, "StackLoc[%d]", (int)sv->c.i);
    }
    break;
  case VT_CMP:
    fprintf(out, "VT_CMP");
    break;
  case VT_JMP:
    fprintf(out, "VT_JMP");
    break;
  case VT_JMPI:
    fprintf(out, "VT_JMPI");
    break;
  default:
    if (sv->pr0 == PREG_NONE)
    {
      fprintf(out, "VReg %s:%d", tcc_ir_get_vreg_type_string(sv->vr), TCCIR_DECODE_VREG_POSITION(sv->vr));
      if (tcc_ir_operand_needs_dereference(sv))
        fprintf(out, "***DEREF***");
    }
    else
    {
      if (sv->pr0 & PREG_SPILLED)
        fprintf(out, SPILL_MARK_BEGIN "SpillLoc[%d]" SPILL_MARK_END, (int)sv->c.i);
      else
        fprintf(out, "R%d", sv->pr0);
      if (tcc_ir_operand_needs_dereference(sv))
        fprintf(out, "***DEREF***");
    }
    break;
  }
}

static void tcc_dump_quadruple_to(FILE *out, const TACQuadruple *q, int pc)
{
  if (!q)
  {
    fprintf(out, "%04d: <null>\n", pc);
    return;
  }

  const int op = q->op;
  fprintf(out, "%04d: ", pc);
  switch (op)
  {
  case TCCIR_OP_RETURNVALUE:
  case TCCIR_OP_RETURNVOID:
  case TCCIR_OP_FUNCCALLVOID:
  case TCCIR_OP_FUNCCALLVAL:
  case TCCIR_OP_FUNCPARAMVOID:
  case TCCIR_OP_TEST_ZERO:
  case TCCIR_OP_CMP:
    fprintf(out, "%s ", tcc_ir_get_op_name(op));
    break;
  case TCCIR_OP_FUNCPARAMVAL:
    fprintf(out, "%s%d[call_%d] ", tcc_ir_get_op_name(op), TCCIR_DECODE_PARAM_IDX(q->src2.c.i),
            TCCIR_DECODE_CALL_ID(q->src2.c.i));
    break;
  case TCCIR_OP_JUMP:
  case TCCIR_OP_JUMPIF:
    fprintf(out, "JMP to %d ", (int)q->dest.c.i);
    break;
  case TCCIR_OP_IJUMP:
    fprintf(out, "IJMP ");
    tcc_dump_svalue_short_to(out, &q->src1);
    fprintf(out, " ");
    break;
  default:
    tcc_dump_svalue_short_to(out, &q->dest);
    fprintf(out, " <-- ");
    break;
  }

  if (irop_config[op].has_src1)
  {
    if (op == TCCIR_OP_SETIF)
      fprintf(out, "(cond=0x%x)", (unsigned)q->src1.c.i);
    else if (op != TCCIR_OP_JUMPIF)
      tcc_dump_svalue_short_to(out, &q->src1);
  }

  if (irop_config[op].has_src2)
  {
    switch (op)
    {
    case TCCIR_OP_CMP:
      fprintf(out, ",");
      tcc_dump_svalue_short_to(out, &q->src2);
      break;
    case TCCIR_OP_FUNCPARAMVAL:
    case TCCIR_OP_FUNCCALLVAL:
      break;
    default:
      fprintf(out, " %s ", tcc_ir_get_op_name(op));
      tcc_dump_svalue_short_to(out, &q->src2);
      break;
    }
  }

  if (op == TCCIR_OP_STORE)
    fprintf(out, " [STORE]");
  else if (op == TCCIR_OP_LOAD)
    fprintf(out, " [LOAD]");
  else if (op == TCCIR_OP_ASSIGN)
    fprintf(out, " [ASSIGN]");
  else if (op == TCCIR_OP_FUNCCALLVAL)
  {
    fprintf(out, " --> ");
    tcc_dump_svalue_short_to(out, &q->dest);
  }
  else if (op == TCCIR_OP_JUMPIF)
  {
    fprintf(out, " if \"");
    switch (q->src1.c.i)
    {
    case TOK_EQ:
      fprintf(out, "==");
      break;
    case TOK_NE:
      fprintf(out, "!=");
      break;
    case TOK_LT:
      fprintf(out, "<S");
      break;
    case TOK_GT:
      fprintf(out, ">S");
      break;
    case TOK_LE:
      fprintf(out, "<=S");
      break;
    case TOK_GE:
      fprintf(out, ">=S");
      break;
    case TOK_ULT:
      fprintf(out, "<U");
      break;
    case TOK_UGT:
      fprintf(out, ">U");
      break;
    case TOK_ULE:
      fprintf(out, "<=U");
      break;
    case TOK_UGE:
      fprintf(out, ">=U");
      break;
    default:
      fprintf(out, "cc=0x%x", (unsigned)q->src1.c.i);
      break;
    }
    fprintf(out, "\"");
  }
  else if (op == TCCIR_OP_SETIF)
  {
    fprintf(out, "1 if \"");
    switch (q->src1.c.i)
    {
    case TOK_EQ:
      fprintf(out, "==");
      break;
    case TOK_NE:
      fprintf(out, "!=");
      break;
    case TOK_LT:
      fprintf(out, "<S");
      break;
    case TOK_GT:
      fprintf(out, ">S");
      break;
    case TOK_LE:
      fprintf(out, "<=S");
      break;
    case TOK_GE:
      fprintf(out, ">=S");
      break;
    case TOK_ULT:
      fprintf(out, "<U");
      break;
    case TOK_UGT:
      fprintf(out, ">U");
      break;
    case TOK_ULE:
      fprintf(out, "<=U");
      break;
    case TOK_UGE:
      fprintf(out, ">=U");
      break;
    default:
      fprintf(out, "cc=0x%x", (unsigned)q->src1.c.i);
      break;
    }
    fprintf(out, "\"");
  }

  fprintf(out, "\n");
}
#endif

#define TCC_STACK_LAYOUT_INIT_CAPACITY 16

#define QUADRUPLE_INIT_SIZE 128

#define IR_LIVE_INTERVAL_INIT_SIZE 64
#define LOCAL_VARIABLES_INIT_SIZE 64

static int tcc_ir_is_fpu_operation(TccIrOp op);
static bool tcc_ir_put_soft_call_fpu_if_needed(TCCIRState *ir, TccIrOp op, SValue *src1, SValue *src2, SValue *dest);
static bool tcc_ir_operand_needs_dereference(SValue *sv);

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

/* Check if an SValue operand is spilled (in memory) */
int tcc_ir_is_spilled(SValue *sv)
{
  return (sv->pr0 == PREG_NONE) || (sv->pr0 & PREG_SPILLED);
}

/* Returns true if type is 64-bit (double, ldouble, or long long) - exported for machine code */
int tcc_ir_is_64bit(int t)
{
  return tcc_ir_is_64bit_type(t);
}

static void tcc_abi_call_layout_ensure_capacity(TCCAbiCallLayout *layout, int needed)
{
  if (!layout)
    return;
  if (needed <= 0)
    return;

  if (layout->capacity >= needed && layout->locs && layout->args_effective && layout->args_original &&
      layout->arg_flags)
    return;

  int new_capacity = layout->capacity ? layout->capacity : 8;
  while (new_capacity < needed)
    new_capacity *= 2;

  layout->locs = (TCCAbiArgLoc *)tcc_realloc(layout->locs, sizeof(TCCAbiArgLoc) * (size_t)new_capacity);
  layout->args_original =
      (TCCAbiArgDesc *)tcc_realloc(layout->args_original, sizeof(TCCAbiArgDesc) * (size_t)new_capacity);
  layout->args_effective =
      (TCCAbiArgDesc *)tcc_realloc(layout->args_effective, sizeof(TCCAbiArgDesc) * (size_t)new_capacity);
  layout->arg_flags = (uint8_t *)tcc_realloc(layout->arg_flags, (size_t)new_capacity);

  /* Zero-init the newly added tail. */
  if (new_capacity > layout->capacity)
  {
    const int old = layout->capacity;
    memset(&layout->locs[old], 0, sizeof(TCCAbiArgLoc) * (size_t)(new_capacity - old));
    memset(&layout->args_original[old], 0, sizeof(TCCAbiArgDesc) * (size_t)(new_capacity - old));
    memset(&layout->args_effective[old], 0, sizeof(TCCAbiArgDesc) * (size_t)(new_capacity - old));
    memset(&layout->arg_flags[old], 0, (size_t)(new_capacity - old));
  }

  layout->capacity = new_capacity;
}

static void tcc_abi_call_layout_deinit(TCCAbiCallLayout *layout)
{
  if (!layout)
    return;
  if (layout->locs)
    tcc_free(layout->locs);
  if (layout->args_original)
    tcc_free(layout->args_original);
  if (layout->args_effective)
    tcc_free(layout->args_effective);
  if (layout->arg_flags)
    tcc_free(layout->arg_flags);
  memset(layout, 0, sizeof(*layout));
}

void tcc_print_quadruple(TACQuadruple *q, int pc);
void tcc_ir_print_vreg(int vreg);

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
    [TCCIR_OP_IJUMP] = {0, 1, 0},
    [TCCIR_OP_SETIF] = {1, 1, 0},
    /* FUNCPARAMVOID carries call_id in src2.c.i (encoded like FUNCPARAMVAL). */
    [TCCIR_OP_FUNCPARAMVOID] = {0, 0, 1},
    [TCCIR_OP_FUNCPARAMVAL] = {0, 1, 1},
    /* FUNCCALL* carries call_id in src2.c.i so backends can match parameters. */
    [TCCIR_OP_FUNCCALLVOID] = {0, 1, 1},
    [TCCIR_OP_FUNCCALLVAL] = {1, 1, 1},
    [TCCIR_OP_LOAD] = {1, 1, 0},
    [TCCIR_OP_STORE] = {1, 1, 0},
    [TCCIR_OP_ASSIGN] = {1, 1, 0},
    [TCCIR_OP_LEA] = {1, 1, 0},    /* dest = &src1 */
    [TCCIR_OP_TEST_ZERO] = {0, 1, 0},
    /* Floating point operations */
    [TCCIR_OP_FADD] = {1, 1, 1}, [TCCIR_OP_FSUB] = {1, 1, 1}, [TCCIR_OP_FMUL] = {1, 1, 1}, [TCCIR_OP_FDIV] = {1, 1, 1},
    [TCCIR_OP_FNEG] = {1, 1, 0}, /* unary: src1=input, dest */
    [TCCIR_OP_FCMP] = {0, 1, 1},
    /* Floating point conversion operations */
    [TCCIR_OP_CVT_FTOF] = {1, 1, 0}, /* dest=result, src1=input */
    [TCCIR_OP_CVT_ITOF] = {1, 1, 0}, /* dest=result, src1=input */
    [TCCIR_OP_CVT_FTOI] = {1, 1, 0}, /* dest=result, src1=input */
    /* Logical boolean operations */
    [TCCIR_OP_BOOL_OR] = {1, 1, 1},  /* dest = (src1 || src2) */
    [TCCIR_OP_BOOL_AND] = {1, 1, 1}, /* dest = (src1 && src2) */

    /* VLA / dynamic stack ops */
    [TCCIR_OP_VLA_ALLOC] = {0, 1, 1},      /* src1=size(bytes), src2=align(bytes) */
    [TCCIR_OP_VLA_SP_SAVE] = {1, 0, 0},    /* dest=stack slot to store SP */
    [TCCIR_OP_VLA_SP_RESTORE] = {0, 1, 0}, /* src1=stack slot holding saved SP */

    /* Inline asm markers/barrier.
     * INLINE_ASM carries inline_asm_id in src1.c.i. */
    [TCCIR_OP_ASM_INPUT] = {0, 1, 0}, [TCCIR_OP_INLINE_ASM] = {0, 1, 0}, [TCCIR_OP_ASM_OUTPUT] = {1, 0, 0},
    /* Explicit call sequence ops (Option A scaffold)
     * - CALLSEQ_BEGIN: src1=stack_size (bytes), src2=pad (bytes)
     * - CALLARG_REG: src1=value, src2=reg_index (immediate)
     * - CALLARG_STACK: src1=value, src2=stack_off (immediate)
     * - CALLSEQ_END: src1=stack_size (bytes), src2=pad (bytes)
     */
    [TCCIR_OP_CALLSEQ_BEGIN] = {0, 1, 1}, [TCCIR_OP_CALLARG_REG] = {0, 1, 1}, [TCCIR_OP_CALLARG_STACK] = {0, 1, 1},
    [TCCIR_OP_CALLSEQ_END] = {0, 1, 1},

    /* No-operation */
    [TCCIR_OP_NOP] = {0, 0, 0},
}
;
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

IRLiveInterval *tcc_ir_get_live_interval(TCCIRState *ir, int vreg)
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
    intervals[i].stack_slot_index = -1;
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
  if (sv->pr0 == PREG_NONE)
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
  block->ir_to_code_mapping = NULL;
  block->ir_to_code_mapping_size = 0;
  block->orig_ir_to_code_mapping = NULL;
  block->orig_ir_to_code_mapping_size = 0;

  block->next_instruction_index = 0;
  block->next_call_id = 1;

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
  block->stack_layout.slots = NULL;
  block->stack_layout.slot_capacity = 0;
  block->stack_layout.slot_count = 0;

#ifdef CONFIG_TCC_ASM
  block->inline_asms = NULL;
  block->inline_asm_count = 0;
  block->inline_asm_capacity = 0;
#endif
  return block;
}

#ifdef CONFIG_TCC_ASM
static void tcc_ir_inline_asms_ensure_capacity(TCCIRState *ir, int needed)
{
  if (!ir)
    return;
  if (ir->inline_asm_capacity >= needed)
    return;
  int new_cap = ir->inline_asm_capacity ? ir->inline_asm_capacity : 8;
  while (new_cap < needed)
    new_cap <<= 1;
  ir->inline_asms = tcc_realloc(ir->inline_asms, sizeof(TCCIRInlineAsm) * new_cap);
  memset(ir->inline_asms + ir->inline_asm_capacity, 0, sizeof(TCCIRInlineAsm) * (new_cap - ir->inline_asm_capacity));
  ir->inline_asm_capacity = new_cap;
}

int tcc_ir_add_inline_asm(TCCIRState *ir, const char *asm_str, int asm_len, int must_subst, ASMOperand *operands,
                          int nb_operands, int nb_outputs, int nb_labels, const uint8_t *clobber_regs)
{
  if (!ir)
    return -1;
  if (!asm_str || asm_len < 0)
    tcc_error("IR: invalid inline asm string");
  if (nb_operands < 0 || nb_operands > MAX_ASM_OPERANDS)
    tcc_error("IR: invalid asm operand count");
  if (nb_labels < 0 || nb_operands + nb_labels > MAX_ASM_OPERANDS)
    tcc_error("IR: invalid asm label count");
  if (nb_outputs < 0 || nb_outputs > nb_operands)
    tcc_error("IR: invalid asm output count");

  tcc_ir_inline_asms_ensure_capacity(ir, ir->inline_asm_count + 1);
  const int id = ir->inline_asm_count++;
  TCCIRInlineAsm *ia = &ir->inline_asms[id];

  ia->asm_len = asm_len;
  ia->asm_str = tcc_mallocz((size_t)asm_len + 1);
  memcpy(ia->asm_str, asm_str, (size_t)asm_len);
  ia->must_subst = must_subst;
  ia->nb_operands = nb_operands;
  ia->nb_outputs = nb_outputs;
  ia->nb_labels = nb_labels;
  if (clobber_regs)
    memcpy(ia->clobber_regs, clobber_regs, NB_ASM_REGS);
  else
    memset(ia->clobber_regs, 0, NB_ASM_REGS);

  ia->operands = tcc_mallocz(sizeof(ASMOperand) * (nb_operands + nb_labels));
  memcpy(ia->operands, operands, sizeof(ASMOperand) * (nb_operands + nb_labels));

  ia->values = tcc_mallocz(sizeof(SValue) * nb_operands);
  for (int i = 0; i < nb_operands; ++i)
  {
    if (!operands[i].vt)
      tcc_error("IR: asm operand missing value");
    ia->values[i] = *operands[i].vt;
    ia->operands[i].vt = &ia->values[i];
  }
  for (int i = nb_operands; i < nb_operands + nb_labels; ++i)
  {
    ia->operands[i].vt = NULL;
  }

  /* Conservative: inline asm is call-like for leaf analysis. */
  ir->leaffunc = 0;

  return id;
}

void tcc_ir_put_inline_asm(TCCIRState *ir, int inline_asm_id)
{
  if (!ir)
    return;
  SValue id_sv = tcc_svalue_const_i64(inline_asm_id);
  (void)tcc_ir_put(ir, TCCIR_OP_INLINE_ASM, &id_sv, NULL, NULL);
  ir->leaffunc = 0;
}
#endif

/* Peephole helpers for callsite argument folding (see tcc_ir_build_callsites). */
static int tcc_ir_is_stack_addr_operand_novreg(const SValue *sv)
{
  if (!sv)
    return 0;
  int val_kind = sv->r & VT_VALMASK;
  if ((val_kind == VT_LOCAL || val_kind == VT_LLOCAL) && !(sv->r & VT_LVAL) && sv->vr == -1)
    return 1;
  return 0;
}

static int tcc_ir_find_def_for_vreg_before(const TCCIRState *ir, int vreg_encoded, int start_idx)
{
  if (!ir)
    return -1;
  if (start_idx > ir->next_instruction_index)
    start_idx = ir->next_instruction_index;
  for (int i = start_idx - 1; i >= 0; --i)
  {
    const TACQuadruple *q = &ir->instructions[i];
    if (q->dest.vr == vreg_encoded)
      return i;
  }
  return -1;
}

/* Resolve a vreg that represents a stack address into (kind, offset).
 * Returns 1 on success, 0 on failure.
 * This is conservative and intended for simple TEMP address chains.
 */
static int tcc_ir_try_resolve_stack_addr(const TCCIRState *ir, int vreg_encoded, int start_idx, int depth,
                                         int *out_kind, int *out_offset)
{
  if (!ir || !out_kind || !out_offset)
    return 0;
  if (depth <= 0)
    return 0;

  const int def_idx = tcc_ir_find_def_for_vreg_before(ir, vreg_encoded, start_idx);
  if (def_idx < 0)
    return 0;

  const TACQuadruple *def = &ir->instructions[def_idx];

  /* Base case: vreg = Addr[StackLoc[off]] (no VT_LVAL and no vreg on the address operand). */
  if (def->op == TCCIR_OP_ASSIGN && tcc_ir_is_stack_addr_operand_novreg(&def->src1))
  {
    *out_kind = def->src1.r & VT_VALMASK;
    *out_offset = def->src1.c.i;
    return 1;
  }

  /* Copy chain: vreg = other_vreg (address value). */
  if (def->op == TCCIR_OP_ASSIGN && tcc_is_vreg_valid((TCCIRState *)ir, def->src1.vr) && !(def->src1.r & VT_LVAL))
  {
    return tcc_ir_try_resolve_stack_addr(ir, def->src1.vr, def_idx, depth - 1, out_kind, out_offset);
  }

  /* Simple address arithmetic: vreg = base_vreg +/- const. */
  if ((def->op == TCCIR_OP_ADD || def->op == TCCIR_OP_SUB) && !(def->src1.r & VT_LVAL) && !(def->src2.r & VT_LVAL))
  {
    const int src1_is_vreg = tcc_is_vreg_valid((TCCIRState *)ir, def->src1.vr);
    const int src2_is_vreg = tcc_is_vreg_valid((TCCIRState *)ir, def->src2.vr);
    const int src1_is_const = (def->src1.r & VT_VALMASK) == VT_CONST && !(def->src1.r & VT_SYM);
    const int src2_is_const = (def->src2.r & VT_VALMASK) == VT_CONST && !(def->src2.r & VT_SYM);

    (void)src2_is_vreg;

    int base_kind = 0;
    int base_off = 0;

    if (src1_is_vreg && src2_is_const)
    {
      if (!tcc_ir_try_resolve_stack_addr(ir, def->src1.vr, def_idx, depth - 1, &base_kind, &base_off))
        return 0;
      *out_kind = base_kind;
      *out_offset = (def->op == TCCIR_OP_ADD) ? (base_off + def->src2.c.i) : (base_off - def->src2.c.i);
      return 1;
    }
    if (src1_is_const && src2_is_vreg && def->op == TCCIR_OP_ADD)
    {
      /* const + base */
      if (!tcc_ir_try_resolve_stack_addr(ir, def->src2.vr, def_idx, depth - 1, &base_kind, &base_off))
        return 0;
      *out_kind = base_kind;
      *out_offset = base_off + def->src1.c.i;
      return 1;
    }
  }

  return 0;
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

  if (ir->ir_to_code_mapping)
  {
    tcc_free(ir->ir_to_code_mapping);
    ir->ir_to_code_mapping = NULL;
    ir->ir_to_code_mapping_size = 0;
  }

  if (ir->orig_ir_to_code_mapping)
  {
    tcc_free(ir->orig_ir_to_code_mapping);
    ir->orig_ir_to_code_mapping = NULL;
    ir->orig_ir_to_code_mapping_size = 0;
  }

  if (ir->instructions != NULL)
  {
    tcc_free(ir->instructions);
  }

#ifdef CONFIG_TCC_ASM
  if (ir->inline_asms)
  {
    for (int i = 0; i < ir->inline_asm_count; ++i)
    {
      TCCIRInlineAsm *ia = &ir->inline_asms[i];
      if (ia->asm_str)
        tcc_free(ia->asm_str);
      ia->asm_str = NULL;
      if (ia->operands)
        tcc_free(ia->operands);
      ia->operands = NULL;
      if (ia->values)
        tcc_free(ia->values);
      ia->values = NULL;
    }
    tcc_free(ir->inline_asms);
  }
  ir->inline_asms = NULL;
  ir->inline_asm_count = 0;
  ir->inline_asm_capacity = 0;
#endif

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

  if (ir->stack_layout.slots != NULL)
  {
    tcc_free(ir->stack_layout.slots);
    ir->stack_layout.slots = NULL;
    ir->stack_layout.slot_capacity = 0;
    ir->stack_layout.slot_count = 0;
  }

  tcc_ls_deinitialize(&ir->ls);
  tcc_free(ir);
}

void tcc_ir_add_function_parameters(TCCIRState *ir, CType *func_type)
{
  Sym *sym = NULL;
  int size = 0, align = 0;
  SValue dst;

  TCCAbiCallLayout call_layout;
  memset(&call_layout, 0, sizeof(call_layout));

  sym = func_type->ref;
  func_vt = sym->type;
  tcc_state->need_frame_pointer = 0;

  loc = 0;

  /* Count arguments to pre-allocate layout arrays */
  int arg_count = 0;
  for (Sym *s = sym->next; s; s = s->next)
    arg_count++;
  if (arg_count > 0)
    tcc_abi_call_layout_ensure_capacity(&call_layout, arg_count);

  int arg_index = 0;
  for (sym = sym->next; sym; sym = sym->next, ++arg_index)
  {
    CType *type = &sym->type;
    int flags = 0;
    int addr = 0;

    size = type_size(type, &align);
    if (align < 1)
      align = 1;

    TCCAbiArgDesc desc;
    memset(&desc, 0, sizeof(desc));
    memset(&dst, 0, sizeof(dst));

    if ((type->t & VT_BTYPE) == VT_STRUCT)
    {
      desc.kind = TCC_ABI_ARG_STRUCT_BYVAL;
      desc.size = (uint16_t)size;
      desc.alignment = (uint8_t)align;
    }
    else if (tcc_ir_is_64bit_type(type->t))
    {
      desc.kind = TCC_ABI_ARG_SCALAR64;
      desc.size = 8;
      desc.alignment = (uint8_t)align;
    }
    else
    {
      desc.kind = TCC_ABI_ARG_SCALAR32;
      desc.size = 4;
      desc.alignment = (uint8_t)align;
    }

    TCCAbiArgLoc loc_info = tcc_abi_classify_argument(&call_layout, arg_index, &desc);

    /* Any stack-passed argument means we must keep a stable frame pointer
     * for addressing the caller argument area.
     */
    if (loc_info.kind == TCC_ABI_LOC_STACK)
      tcc_state->need_frame_pointer = 1;

    if ((type->t & VT_BTYPE) == VT_STRUCT)
    {
      const int invisible_ref =
          (call_layout.arg_flags && (call_layout.arg_flags[arg_index] & TCC_ABI_ARG_FLAG_INVISIBLE_REF));
      const int actual_size = (call_layout.args_original ? (int)call_layout.args_original[arg_index].size : size);
      const int actual_align =
          (call_layout.args_original ? (int)call_layout.args_original[arg_index].alignment : align);
      int slot_align = actual_align;
      if (slot_align < 4)
        slot_align = 4;

      if (invisible_ref)
      {
        /* ABI decided: large struct passed as hidden pointer.
         * Materialize: read pointer param into a callee-local slot,
         * expose C-visible struct param as an lvalue at that slot.
         */
        loc = (loc - PTR_SIZE) & -PTR_SIZE;
        const int ptr_slot = loc;
        const int ptr_param_vr = tcc_ir_get_vreg_param(ir);

        SValue src;
        memset(&src, 0, sizeof(src));
        memset(&dst, 0, sizeof(dst));
        src.type.t = VT_PTR;
        src.r = 0;
        src.vr = ptr_param_vr;
        dst.type.t = VT_PTR;
        dst.r = VT_LOCAL | VT_LVAL;
        dst.vr = -1;
        dst.c.i = ptr_slot;
        tcc_ir_put(ir, TCCIR_OP_STORE, &src, NULL, &dst);

        flags = VT_LVAL | VT_LLOCAL;
        addr = ptr_slot;
        sym_push(sym->v & ~SYM_FIELD, type, flags, addr);
        continue;
      }

      if (loc_info.kind == TCC_ABI_LOC_REG)
      {
        /* Struct passed in registers: spill incoming words into a callee-local
         * home and model the C-visible param as an lvalue on that home.
         */
        int slot_size = tcc_abi_align_up_int(actual_size, 4);
        loc = (loc - slot_size) & -slot_align;
        const int struct_slot = loc;

        const int word_count = (slot_size + 3) / 4;
        for (int w = 0; w < word_count; ++w)
        {
          const int word_param_vr = tcc_ir_get_vreg_param(ir);
          SValue src;
          SValue dst;
          memset(&src, 0, sizeof(src));
          memset(&dst, 0, sizeof(dst));
          src.type.t = VT_INT;
          src.r = 0;
          src.vr = word_param_vr;
          dst.type.t = VT_INT;
          dst.r = VT_LOCAL | VT_LVAL;
          dst.vr = -1;
          dst.c.i = struct_slot + w * 4;
          tcc_ir_put(ir, TCCIR_OP_STORE, &src, NULL, &dst);
        }

        flags = VT_LVAL | VT_LLOCAL;
        addr = struct_slot;
        sym_push(sym->v & ~SYM_FIELD, type, flags, addr);
        continue;
      }

      /* Struct passed on stack: keep it as a VT_PARAM lvalue at incoming stack offset. */
      flags = VT_PARAM | VT_LVAL | VT_LOCAL;
      addr = loc_info.stack_off;
      sym_push(sym->v & ~SYM_FIELD, type, flags, addr);
      continue;
    }

    /* Scalar params are always represented as PARAM vregs.
     * Do not encode ABI offsets here; prolog/incoming-reg tracking computes
     * that later from PARAM vreg ordering.
     */
    if (loc_info.kind == TCC_ABI_LOC_REG)
    {
      /* In-register param */
      flags = VT_PARAM | VT_LVAL;
      // argument is materialized in register, not local stack
      addr = 0;
    }
    else
    {
      flags = VT_PARAM | VT_LVAL | VT_LOCAL;
      addr = loc_info.stack_off;
      /* On-stack param */
    }

    sym->r |= ~(VT_LVAL | VT_LLOCAL);
    tcc_debug_print_sym(sym_push(sym->v & ~SYM_FIELD, type, flags, addr));
  }

  tcc_abi_call_layout_deinit(&call_layout);
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
      vtop->jfalse = -1; /* -1 = no chain */
      vtop->jtrue = -1;  /* -1 = no chain */
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
    vtop->jfalse = -1; /* -1 = no chain */
    vtop->jtrue = -1;  /* -1 = no chain */
    return;
  }

  memset(&dest, 0, sizeof(SValue));
  dest.vr = tcc_ir_get_vreg_temp(ir);
  dest.r = 0;
  /* Most integer ops preserve the operand type, but UMULL produces a 64-bit result. */
  if (ir_op == TCCIR_OP_UMULL)
  {
    dest.type.t = VT_LLONG | VT_UNSIGNED;
    tcc_ir_set_llong_type(ir, dest.vr);
  }
  else
  {
    dest.type.t = vtop[-1].type.t;
  }
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
  case TCCIR_OP_IJUMP:
    return "IJUMP";
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
  case TCCIR_OP_FDIV:
    return "FDIV";
  case TCCIR_OP_FNEG:
    return "CVT_FTOF";
  case TCCIR_OP_CVT_ITOF:
    return "CVT_ITOF";
  case TCCIR_OP_CVT_FTOI:
    return "CVT_FTOI";
  case TCCIR_OP_BOOL_OR:
    return "BOOL_OR";
  case TCCIR_OP_BOOL_AND:
    return "BOOL_AND";
  case TCCIR_OP_VLA_ALLOC:
    return "VLA_ALLOC";
  case TCCIR_OP_VLA_SP_SAVE:
    return "VLA_SP_SAVE";
  case TCCIR_OP_VLA_SP_RESTORE:
    return "VLA_SP_RESTORE";
  case TCCIR_OP_ASM_INPUT:
    return "ASM_INPUT";
  case TCCIR_OP_INLINE_ASM:
    return "INLINE_ASM";
  case TCCIR_OP_ASM_OUTPUT:
    return "ASM_OUTPUT";
  case TCCIR_OP_CALLSEQ_BEGIN:
    return "CALLSEQ_BEGIN";
  case TCCIR_OP_CALLARG_REG:
    return "CALLARG_REG";
  case TCCIR_OP_CALLARG_STACK:
    return "CALLARG_STACK";
  case TCCIR_OP_CALLSEQ_END:
    return "CALLSEQ_END";
  case TCCIR_OP_NOP:
    return "NOP";
  default:
    return "UNKNOWN_OP";
  }
}

/* Ensure that anonymous symbols referenced by SValues are registered in the ELF
 * symbol table before being stored in IR instructions. This prevents use-after-free
 * when the local scope is popped (sym_pop) before the IR is processed.
 *
 * Anonymous symbols (v >= SYM_FIRST_ANOM) with c == 0 are local to the current scope
 * and will be freed when the scope ends. By registering them (put_extern_sym), we
 * set c > 0 which prevents them from being freed in sym_pop.
 */
static void tcc_ir_ensure_sym_registered(SValue *sv)
{
  if (sv && (sv->r & VT_SYM) && sv->sym)
  {
    Sym *sym = sv->sym;
    /* Check if this is an anonymous symbol that hasn't been registered yet */
    if ((sym->v & ~0x0FFFFFFF) == SYM_FIRST_ANOM && sym->c == 0)
    {
      /* Use put_extern_sym2 directly to bypass nocode_wanted check.
       * We need the symbol registered in ELF even if we're in a "nocode" section
       * because the IR instruction we're about to create will reference it later. */
      put_extern_sym2(sym, SHN_UNDEF, 0, 0, 1);
    }
  }
}

static int tcc_ir_operand_is_stack_addr(const SValue *sv)
{
  if (!sv)
    return 0;
  int val_kind = sv->r & VT_VALMASK;
  if ((val_kind == VT_LOCAL || val_kind == VT_LLOCAL) && !(sv->r & VT_LVAL) && sv->vr == -1)
    return 1;
  return 0;
}

int tcc_ir_put(TCCIRState *ir, TccIrOp op, SValue *src1, SValue *src2, SValue *dest)
{
  /* Respect front-end code suppression.
   *
   * The parser uses `nocode_wanted` to parse expressions/statements without
   * generating code (dead `?:` arms, sizeof/typeof, const-eval, etc.). In IR
   * mode, many front-end paths still call into `tcc_ir_put()` unconditionally;
   * without a guard, IR for suppressed regions can be emitted and later run,
   * causing hangs (see tests/tests2/87_dead_code.c).
   *
   * However `nocode_wanted` also carries the internal CODE_OFF bit (set after
   * unconditional jumps/returns to suppress fallthrough until a label). That
   * state must NOT suppress IR globally, or reachable code paths can lose IR
   * emission (e.g. the else-arm of an if whose then-arm ends with return),
   * breaking programs like tests/tests2/15_recursion.c.
   */
  {
    /* Must match CODE_OFF_BIT in tccgen.c */
    const int IR_CODE_OFF_BIT = 0x20000000;
    if (nocode_wanted & ~IR_CODE_OFF_BIT)
      return -1;
  }

  // resize array if needed
  const int pos = ir->next_instruction_index;
  TACQuadruple *q;
  /* Ensure any anonymous symbols in the operands are registered before
   * storing them in the IR instruction. This prevents use-after-free when
   * local scopes are popped before the IR is processed. */
  tcc_ir_ensure_sym_registered(src1);
  tcc_ir_ensure_sym_registered(src2);
  tcc_ir_ensure_sym_registered(dest);

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
  memset(q, 0, sizeof(TACQuadruple)); /* Zero-initialize to avoid garbage in unused fields */
  q->orig_index = pos;
  q->op = op;

  if (irop_config[op].has_src1 == 1)
  {
    if (src1 == NULL)
    {
      fprintf(stderr, "tcc_ir_put: src1 is NULL for op %s\n", tcc_ir_get_op_name(op));
      exit(1);
    }
    q->src1 = *src1;
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
      /* Most operations produce VALUES, not addresses, so is_lvalue should be 0.
       * The only case where is_lvalue should be 1 is when we're assigning an ADDRESS
       * to a variable (like ASSIGN of &var, without VT_LVAL dereference).
       * - LOAD: produces a VALUE (loaded from memory) → is_lvalue=0
       * - FUNCCALLVAL: produces a VALUE (return value) → is_lvalue=0
       * - Arithmetic ops (ADD, SUB, etc.): produce VALUES → is_lvalue=0
       * - ASSIGN with VT_LVAL source: produces a VALUE (dereferenced) → is_lvalue=0
       * - ASSIGN without VT_LVAL source: produces an ADDRESS → is_lvalue=1 */
      int old_is_lvalue = dest_interval->is_lvalue;
      int new_is_lvalue;
      int src_is_stack_addr = tcc_ir_operand_is_stack_addr(src1);
      if (op == TCCIR_OP_ASSIGN && src1 && !(src1->r & VT_LVAL) && !src_is_stack_addr)
      {
        /* ASSIGN of a non-stack address (no VT_LVAL) produces an lvalue that must be
         * reloaded if spilled. True stack addresses (Addr[StackLoc]) stay as raw
         * addresses so they can be recomputed instead of reloaded. */
        new_is_lvalue = 1;
      }
      else
      {
        /* All other operations (LOAD, arithmetic, function calls, etc.) produce values */
        new_is_lvalue = 0;
      }
      dest_interval->is_lvalue = new_is_lvalue;
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
  q->src1.pr0 = PREG_NONE;
  q->src1.pr1 = PREG_NONE;
  q->src2.pr0 = PREG_NONE;
  q->src2.pr1 = PREG_NONE;
  q->dest.pr0 = PREG_NONE;
  q->dest.pr1 = PREG_NONE;

  // store current source line number for debug info
  q->line_num = file ? file->line_num : 0;

  if (ir->basic_block_start)
  {
    ir->basic_block_start = 0;
  }
  else if (op == TCCIR_OP_ASSIGN && pos > 0)
  {
    /* Try to coalesce: if assigning from a TEMP that was the dest of the previous instruction,
     * redirect that instruction's dest to our dest and skip this ASSIGN. */
    const int can_coalesce = (!ir->prevent_coalescing) && (TCCIR_DECODE_VREG_TYPE(src1->vr) == TCCIR_VREG_TYPE_TEMP) &&
                             ((src1->r & VT_LVAL) == 0) && (src1->vr == ir->instructions[pos - 1].dest.vr);
    if (can_coalesce)
    {
      /* When coalescing, preserve the original c.i offset for global symbols.
       * For STORE operations, the dest.c.i contains the offset into the global symbol
       * and should not be overwritten by the local variable's stack offset. */
      int prev_c_i = ir->instructions[pos - 1].dest.c.i;
      int preserve_offset = ((ir->instructions[pos - 1].dest.r & (VT_VALMASK | VT_SYM)) == (VT_CONST | VT_SYM)) &&
                            (ir->instructions[pos - 1].op == TCCIR_OP_STORE);

      /* Preserve the original type - important for 64-bit operations where the result
       * is VT_LLONG but may be assigned to a VT_INT variable (extracting low 32 bits) */
      CType prev_type = ir->instructions[pos - 1].dest.type;

      /* Copy type information (like is_llong) from the old dest to the new dest before coalescing */
      int old_dest_vr = ir->instructions[pos - 1].dest.vr;
      int new_dest_vr = ir->instructions[pos].dest.vr;
      if (tcc_is_vreg_valid(ir, old_dest_vr) && tcc_is_vreg_valid(ir, new_dest_vr))
      {
        IRLiveInterval *old_interval = tcc_ir_get_live_interval(ir, old_dest_vr);
        IRLiveInterval *new_interval = tcc_ir_get_live_interval(ir, new_dest_vr);
        if (old_interval && new_interval && old_interval->is_llong)
          new_interval->is_llong = 1;
      }

      ir->instructions[pos - 1].dest = ir->instructions[pos].dest;

      /* Restore the original type - the coalesced instruction should keep its original result type */
      ir->instructions[pos - 1].dest.type = prev_type;

      if (preserve_offset)
      {
        /* Restore the original offset for global symbols */
        ir->instructions[pos - 1].dest.c.i = prev_c_i;
      }

      /* Don't increment - the ASSIGN at pos should be overwritten by the next instruction */
      return pos - 1; /* Return the coalesced instruction's position */
    }
  }

  ir->next_instruction_index++;

  return pos;
}

int tcc_ir_get_vreg_temp(TCCIRState *ir)
{
  if (ir == NULL)
  {
    return -1;
  }
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
  /* Check for invalid vreg: -1 is sentinel, and type 0 is invalid */
  if (vreg < 0 || TCCIR_DECODE_VREG_TYPE(vreg) == 0)
    return;
  IRLiveInterval *interval = tcc_ir_get_live_interval(ir, vreg);
  if (interval)
  {
    interval->addrtaken = 1;
  }
}

void tcc_ir_set_float_type(TCCIRState *ir, int vreg, int is_float, int is_double)
{
  /* Check for invalid vreg: -1 is sentinel, and type 0 is invalid */
  if (vreg < 0 || TCCIR_DECODE_VREG_TYPE(vreg) == 0)
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
  /* Check for invalid vreg: -1 is sentinel, and type 0 is invalid */
  if (vreg < 0 || TCCIR_DECODE_VREG_TYPE(vreg) == 0)
    return;
  IRLiveInterval *interval = tcc_ir_get_live_interval(ir, vreg);
  if (interval)
  {
    interval->is_llong = 1;
  }
}

void tcc_ir_set_original_offset(TCCIRState *ir, int vreg, int offset)
{
  /* Check for invalid vreg: -1 is sentinel, and type 0 is invalid */
  if (vreg < 0 || TCCIR_DECODE_VREG_TYPE(vreg) == 0)
    return;
  IRLiveInterval *interval = tcc_ir_get_live_interval(ir, vreg);
  if (interval)
  {
    interval->original_offset = offset;
  }
}

int tcc_ir_get_reg_type(TCCIRState *ir, int vreg)
{
  /* Check for invalid vreg: -1 is sentinel, and type 0 is invalid */
  if (vreg < 0 || TCCIR_DECODE_VREG_TYPE(vreg) == 0)
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
  if (!ir)
    return;

  /* FUNCCALL* does not list its arguments explicitly; instead arguments are
   * represented by preceding FUNCPARAMVAL markers tagged with the same call_id.
   *
   * For register allocation correctness, any vreg used in FUNCPARAMVAL must be
   * considered live until the owning FUNCCALL instruction (not just until the
   * FUNCPARAMVAL marker). Otherwise values can be allocated in caller-saved
   * registers and clobbered by intervening calls.
   */
  for (int call_idx = 0; call_idx < ir->next_instruction_index; ++call_idx)
  {
    const TACQuadruple *callq = &ir->instructions[call_idx];
    if (callq->op != TCCIR_OP_FUNCCALLVOID && callq->op != TCCIR_OP_FUNCCALLVAL)
      continue;

    /* FUNCCALL* stores call_id in src2.c.i encoded like FUNCPARAMVAL. */
    const int call_id = TCCIR_DECODE_CALL_ID(callq->src2.c.i);
    for (int j = call_idx - 1; j >= 0; --j)
    {
      const TACQuadruple *p = &ir->instructions[j];
      if (p->op != TCCIR_OP_FUNCPARAMVAL)
        continue;

      const int param_call_id = TCCIR_DECODE_CALL_ID(p->src2.c.i);
      if (param_call_id != call_id)
        continue;

      if (tcc_is_vreg_valid(ir, p->src1.vr))
      {
        IRLiveInterval *interval = tcc_ir_get_live_interval(ir, p->src1.vr);
        if (interval && interval->end < (uint32_t)call_idx)
          interval->end = (uint32_t)call_idx;
        if (interval && interval->start == INTERVAL_NOT_STARTED)
          interval->start = 0;
      }
    }
  }
}

/* Compute live intervals by scanning the IR after optimizations.
 * This replaces the incremental tracking done during tcc_ir_put(),
 * ensuring intervals are always accurate. */
static void tcc_ir_compute_live_intervals(TCCIRState *ir)
{
  /* Reset only start/end positions, preserve other flags like is_lvalue, addrtaken, etc. */
  for (int i = 0; i < ir->next_local_variable; ++i)
  {
    ir->variables_live_intervals[i].start = INTERVAL_NOT_STARTED;
    ir->variables_live_intervals[i].end = 0;
  }
  for (int i = 0; i < ir->next_temporary_variable; ++i)
  {
    ir->temporary_variables_live_intervals[i].start = INTERVAL_NOT_STARTED;
    ir->temporary_variables_live_intervals[i].end = 0;
  }
  for (int i = 0; i < ir->next_parameter; ++i)
  {
    ir->parameters_live_intervals[i].start = INTERVAL_NOT_STARTED;
    ir->parameters_live_intervals[i].end = 0;
  }

  /* Single forward pass over IR to find def/use ranges */
  for (int i = 0; i < ir->next_instruction_index; ++i)
  {
    TACQuadruple *q = &ir->instructions[i];

    /* Skip NOP instructions */
    if (q->op == TCCIR_OP_NOP)
      continue;

    /* Process source operands (uses) */
    if (irop_config[q->op].has_src1 == 1 && tcc_is_vreg_valid(ir, q->src1.vr))
    {
      IRLiveInterval *interval = tcc_ir_get_live_interval(ir, q->src1.vr);
      if (interval->start == INTERVAL_NOT_STARTED)
      {
        /* Use before def - this is a parameter or input */
        interval->start = 0;
      }
      interval->end = i;
    }

    if (irop_config[q->op].has_src2 == 1 && tcc_is_vreg_valid(ir, q->src2.vr))
    {
      IRLiveInterval *interval = tcc_ir_get_live_interval(ir, q->src2.vr);
      if (interval->start == INTERVAL_NOT_STARTED)
      {
        /* Use before def - this is a parameter or input */
        interval->start = 0;
      }
      interval->end = i;
    }

    /* Process destination operand (definition) */
    if (irop_config[q->op].has_dest == 1 && tcc_is_vreg_valid(ir, q->dest.vr))
    {
      IRLiveInterval *interval = tcc_ir_get_live_interval(ir, q->dest.vr);
      if (interval->start == INTERVAL_NOT_STARTED)
      {
        /* First time seeing this vreg - it's defined here */
        interval->start = i;
      }
      interval->end = i;
    }
  }

  /* Handle backward jumps - extend intervals for loop variables */
  for (int i = 0; i < ir->next_instruction_index; ++i)
  {
    TACQuadruple *q = &ir->instructions[i];
    if (q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_JUMPIF)
    {
      int jump_target = q->dest.c.i;
      /* Backward jump: target is before the jump instruction */
      if (jump_target < i)
      {
        /* Any vreg live at the jump target must extend to the jump */
        for (int vreg_type = 0; vreg_type < 3; ++vreg_type)
        {
          int max_vreg = (vreg_type == 0)   ? ir->next_local_variable
                         : (vreg_type == 1) ? ir->next_temporary_variable
                                            : ir->next_parameter;
          for (int vreg_idx = 0; vreg_idx < max_vreg; ++vreg_idx)
          {
            IRLiveInterval *interval = (vreg_type == 0)   ? &ir->variables_live_intervals[vreg_idx]
                                       : (vreg_type == 1) ? &ir->temporary_variables_live_intervals[vreg_idx]
                                                          : &ir->parameters_live_intervals[vreg_idx];

            if (interval->start != INTERVAL_NOT_STARTED && interval->start <= jump_target &&
                interval->end >= jump_target)
            {
              /* Variable is live at jump target, extend to jump */
              if (i > interval->end)
                interval->end = i;
            }
          }
        }
      }
    }
  }

  /* Extend intervals for vregs used as function parameters */
  tcc_ir_extend_param_intervals(ir);
}

void tcc_ir_liveness_analysis(TCCIRState *ir)
{
  int start, end;
  int crosses_call;
  int addrtaken;
  int reg_type;
  IRLiveInterval *interval;
  tcc_ls_clear_live_intervals(&ir->ls);

  /* Compute live intervals from the IR after optimizations */
  tcc_ir_compute_live_intervals(ir);

  /* Now populate the linear scan allocator with the computed intervals */
  for (int vreg = 0; vreg < ir->next_local_variable; ++vreg)
  {
    const int encoded_vreg = (TCCIR_VREG_TYPE_VAR << 28) | vreg;
    if (tcc_is_vreg_ignored(ir, vreg))
    {
      continue;
    }
    interval = tcc_ir_get_live_interval(ir, encoded_vreg);
    if (interval->start != INTERVAL_NOT_STARTED)
    {
      start = interval->start;
      end = interval->end;
      crosses_call = tcc_ir_has_call_in_range(ir, start, end);
      addrtaken = interval->addrtaken;
      reg_type = tcc_ir_get_reg_type(ir, encoded_vreg);
      /* If the interval ends at a CALL instruction, this vreg is a parameter
       * to that call (interval was extended by tcc_ir_extend_param_intervals).
       * It must be in a callee-saved register because the call's argument
       * setup phase may clobber caller-saved registers. */
      if (end < ir->next_instruction_index &&
          (ir->instructions[end].op == TCCIR_OP_FUNCCALLVAL || ir->instructions[end].op == TCCIR_OP_FUNCCALLVOID))
      {
        crosses_call = 1;
      }
      tcc_ls_add_live_interval(&ir->ls, encoded_vreg, start, end, crosses_call, addrtaken, reg_type,
                               interval->is_lvalue, -1);
    }
  }
  for (int vreg = 0; vreg < ir->next_temporary_variable; ++vreg)
  {
    const int vreg_encoded = (TCCIR_VREG_TYPE_TEMP << 28) | vreg;
    if (tcc_is_vreg_ignored(ir, vreg))
    {
      continue;
    }
    interval = tcc_ir_get_live_interval(ir, vreg_encoded);
    if (interval->start != INTERVAL_NOT_STARTED)
    {
      start = interval->start;
      end = interval->end;
      crosses_call = tcc_ir_has_call_in_range(ir, start, end);
      addrtaken = interval->addrtaken;
      reg_type = tcc_ir_get_reg_type(ir, vreg_encoded);
      /* If the interval ends at a CALL instruction, this vreg is a parameter
       * to that call (interval was extended by tcc_ir_extend_param_intervals).
       * It must be in a callee-saved register because the call's argument
       * setup phase may clobber caller-saved registers. */
      if (end < ir->next_instruction_index &&
          (ir->instructions[end].op == TCCIR_OP_FUNCCALLVAL || ir->instructions[end].op == TCCIR_OP_FUNCCALLVOID))
      {
        crosses_call = 1;
      }
      tcc_ls_add_live_interval(&ir->ls, vreg_encoded, start, end, crosses_call, addrtaken, reg_type,
                               interval->is_lvalue, -1);
    }
  }

  for (int vreg = 0; vreg < ir->next_parameter; ++vreg)
  {
    const int vreg_encoded = (TCCIR_VREG_TYPE_PARAM << 28) | vreg;
    interval = tcc_ir_get_live_interval(ir, vreg_encoded);
    /* Parameters start at instruction 0 and end at their last use.
     * If end==0 and param is used at instruction 0, that's valid.
     * If end==0 and param is unused, we still allocate a slot for it. */
    start = 0;
    end = interval->end;
    /* If param never used (end would be 0 from memset), set minimal end */
    if (end == 0)
      end = 1; /* Ensure at least one instruction range for allocation */
    // if (end < ir->next_instruction_index &&
    //     (ir->instructions[end].op == TCCIR_OP_FUNCCALLVAL || ir->instructions[end].op == TCCIR_OP_FUNCCALLVOID))
    // {
    //   /* Don't decrement if this vreg is an argument to the call */
    //   if (!tcc_ir_vreg_is_call_argument(ir, vreg_encoded, end))
    //     end--; /* Do not include call instruction itself */
    // }
    /* Parameters are live at function entry *before* IR instruction 0.
     * So a call at IR[0] must be treated as crossing for parameters, unlike
     * temporaries/locals where start marks a definition point.
     *
     * We intentionally scan calls in [0, end) (excluding end, which may be a
     * last-use-at-call position).
     */
    crosses_call = 0;
    for (int i = 0; i < end && i < ir->next_instruction_index; ++i)
    {
      const TccIrOp op = ir->instructions[i].op;
      if (op == TCCIR_OP_FUNCCALLVOID || op == TCCIR_OP_FUNCCALLVAL)
      {
        crosses_call = 1;
        break;
      }
    }
    addrtaken = interval->addrtaken;
    reg_type = tcc_ir_get_reg_type(ir, vreg_encoded);
    /* Pre-color parameters to their ABI registers (R0-R3 for first 4 params)
     * only if they do NOT cross a call.
     *
     * If a parameter is live across a call, it must not remain in caller-saved
     * R0-R3. Leave it un-precolored so the allocator can place it in a
     * callee-saved register (or spill). The prolog still knows the incoming
     * ABI register via incoming_reg0/1 and will copy it accordingly.
     */
    int precolored = (vreg < 4 && !crosses_call) ? vreg : -1;
    tcc_ls_add_live_interval(&ir->ls, vreg_encoded, start, end, crosses_call, addrtaken, reg_type, interval->is_lvalue,
                             precolored);
  }
}

void tcc_ir_patch_live_intervals_registers(TCCIRState *ir)
{
  for (int i = 0; i < ir->ls.next_interval_index; ++i)
  {
    LSLiveInterval *interval = &ir->ls.intervals[i];
    tcc_ir_assign_physical_register(ir, interval->vreg, interval->stack_location, interval->r0, interval->r1);
  }
}

void tcc_ir_assign_physical_register(TCCIRState *ir, int vreg, int offset, int r0, int r1)
{
  IRLiveInterval *interval = tcc_ir_get_live_interval(ir, vreg);
  /* If variable is spilled (offset != 0), mark r0 with PREG_SPILLED flag */
  if (offset != 0)
  {
    interval->allocation.r0 = PREG_SPILLED;
  }
  else
  {
    interval->allocation.r0 = r0;
    interval->allocation.r1 = r1;
  }
  interval->allocation.offset = offset;
}

static int tcc_ir_stack_slot_size_from_interval(const IRLiveInterval *interval)
{
  if (!interval)
    return 0;
  if (interval->is_double || interval->is_llong)
    return 8;
  return 4;
}

static void tcc_ir_stack_layout_reset(TCCStackLayout *layout)
{
  if (!layout)
    return;
  layout->slot_count = 0;
}

static void tcc_ir_stack_layout_ensure_capacity(TCCStackLayout *layout, int needed_slots)
{
  if (!layout)
    return;
  if (layout->slot_capacity >= needed_slots)
    return;
  int new_capacity = layout->slot_capacity ? layout->slot_capacity : TCC_STACK_LAYOUT_INIT_CAPACITY;
  while (new_capacity < needed_slots)
    new_capacity *= 2;
  layout->slots = (TCCStackSlot *)tcc_realloc(layout->slots, sizeof(TCCStackSlot) * new_capacity);
  layout->slot_capacity = new_capacity;
}

static TCCStackSlot *tcc_ir_stack_layout_find_by_offset(TCCStackLayout *layout, int offset)
{
  if (!layout)
    return NULL;
  for (int i = 0; i < layout->slot_count; ++i)
  {
    if (layout->slots[i].offset == offset)
      return &layout->slots[i];
  }
  return NULL;
}

static int tcc_ir_stack_layout_interval_crosses_call(const TCCIRState *ir, int vreg)
{
  if (!ir)
    return 0;
  for (int i = 0; i < ir->ls.next_interval_index; ++i)
  {
    if ((int)ir->ls.intervals[i].vreg == vreg)
      return ir->ls.intervals[i].crosses_call ? 1 : 0;
  }
  return 0;
}

static TCCStackSlotKind tcc_ir_stack_slot_kind_for_type(TCCIR_VREG_TYPE type)
{
  switch (type)
  {
  case TCCIR_VREG_TYPE_PARAM:
    return TCC_STACK_SLOT_PARAM_SPILL;
  case TCCIR_VREG_TYPE_VAR:
    return TCC_STACK_SLOT_LOCAL;
  default:
    return TCC_STACK_SLOT_SPILL;
  }
}

static void tcc_ir_stack_layout_note_interval(TCCIRState *ir, int vreg, IRLiveInterval *interval, TCCStackSlotKind kind)
{
  if (!interval || interval->allocation.offset == 0)
  {
    if (interval)
      interval->stack_slot_index = -1;
    return;
  }

  TCCStackLayout *layout = &ir->stack_layout;
  TCCStackSlot *slot = tcc_ir_stack_layout_find_by_offset(layout, interval->allocation.offset);
  if (!slot)
  {
    tcc_ir_stack_layout_ensure_capacity(layout, layout->slot_count + 1);
    slot = &layout->slots[layout->slot_count++];
    slot->offset = interval->allocation.offset;
    slot->size = tcc_ir_stack_slot_size_from_interval(interval);
    slot->alignment = (slot->size >= 8) ? 8 : 4;
    slot->kind = kind;
    slot->vreg = vreg;
    slot->live_across_calls = tcc_ir_stack_layout_interval_crosses_call(ir, vreg);
    slot->addressable = interval->addrtaken ? 1 : 0;
  }
  else if (slot->vreg == -1)
  {
    slot->vreg = vreg;
  }

  interval->stack_slot_index = (int)(slot - layout->slots);
}

static void tcc_ir_stack_layout_collect(TCCIRState *ir, IRLiveInterval *intervals, int count, TCCIR_VREG_TYPE type)
{
  if (!intervals || count <= 0)
    return;

  for (int idx = 0; idx < count; ++idx)
  {
    IRLiveInterval *interval = &intervals[idx];
    /* Skip unused intervals (never started and no allocation). */
    if (interval->start == INTERVAL_NOT_STARTED && interval->allocation.offset == 0 && interval->allocation.r0 == 0)
    {
      interval->stack_slot_index = -1;
      continue;
    }

    const int encoded_vreg = TCCIR_ENCODE_VREG(type, idx);
    tcc_ir_stack_layout_note_interval(ir, encoded_vreg, interval, tcc_ir_stack_slot_kind_for_type(type));
  }
}

void tcc_ir_build_stack_layout(TCCIRState *ir)
{
  if (!ir)
    return;

  tcc_ir_stack_layout_reset(&ir->stack_layout);
  tcc_ir_stack_layout_collect(ir, ir->variables_live_intervals, ir->next_local_variable, TCCIR_VREG_TYPE_VAR);
  tcc_ir_stack_layout_collect(ir, ir->temporary_variables_live_intervals, ir->next_temporary_variable,
                              TCCIR_VREG_TYPE_TEMP);
  tcc_ir_stack_layout_collect(ir, ir->parameters_live_intervals, ir->next_parameter, TCCIR_VREG_TYPE_PARAM);
}

const TCCStackSlot *tcc_ir_stack_slot_by_vreg(const TCCIRState *ir, int vreg)
{
  if (!ir || !tcc_is_vreg_valid((TCCIRState *)ir, vreg))
    return NULL;
  IRLiveInterval *interval = tcc_ir_get_live_interval((TCCIRState *)ir, vreg);
  if (!interval || interval->stack_slot_index < 0)
    return NULL;
  if (interval->stack_slot_index >= ir->stack_layout.slot_count)
    return NULL;
  return &ir->stack_layout.slots[interval->stack_slot_index];
}

const TCCStackSlot *tcc_ir_stack_slot_by_offset(const TCCIRState *ir, int frame_offset)
{
  if (!ir)
    return NULL;
  for (int i = 0; i < ir->stack_layout.slot_count; ++i)
  {
    if (ir->stack_layout.slots[i].offset == frame_offset)
      return &ir->stack_layout.slots[i];
  }
  return NULL;
}

static const TCCStackSlot *tcc_ir_materialization_slot(const TCCIRState *ir, const SValue *sv)
{
  if (!ir || !sv)
    return NULL;
  if (!tcc_is_vreg_valid((TCCIRState *)ir, sv->vr))
    return NULL;
  return tcc_ir_stack_slot_by_vreg(ir, sv->vr);
}

static int tcc_ir_materialization_offset(const TCCIRState *ir, const SValue *sv)
{
  const TCCStackSlot *slot = tcc_ir_materialization_slot(ir, sv);
  if (slot)
    return slot->offset;
  return sv ? sv->c.i : 0;
}

static void tcc_ir_require_materialization_result(void *ptr, const char *what)
{
  if (!ptr)
    tcc_error("compiler_error: %s requires a non-null result carrier", what);
}

void tcc_ir_materialize_value(TCCIRState *ir, SValue *sv, TCCMaterializedValue *result)
{
  if (result)
    memset(result, 0, sizeof(*result));

  if (!ir || !sv)
    return;

  if ((sv->r & VT_PARAM) && ((sv->r & VT_VALMASK) == VT_LOCAL))
  {
    /* Stack-passed parameters live in the caller frame. Leave them as VT_PARAM
     * lvalues so the backend can read directly from the caller stack. */
    sv->pr0 = PREG_NONE;
    sv->pr1 = PREG_NONE;
    return;
  }

  /* Register parameters (VT_PARAM with vreg, not on stack) have VT_LVAL set
   * to allow taking their address. But when materializing the VALUE, we need to
   * clear VT_LVAL since the register already holds the value, not a pointer. */
  if ((sv->r & VT_PARAM) && (sv->r & VT_LVAL))
  {
    const int val_kind = sv->r & VT_VALMASK;
    if (val_kind != VT_LOCAL && val_kind != VT_LLOCAL)
    {
      /* Register parameter - clear VT_LVAL since it's already a value */
      sv->r &= ~VT_LVAL;
    }
  }

  const int val_kind = sv->r & VT_VALMASK;
  const int is_64bit = tcc_ir_is_64bit_type(sv->type.t);
  const unsigned scratch_flags =
      (is_64bit ? TCC_MACHINE_SCRATCH_NEEDS_PAIR : 0) | (ir ? ir->codegen_materialize_scratch_flags : 0);

  /* Note: VT_CONST values are NOT automatically materialized here because many operations
   * (shifts, bitwise ops) can use immediate operands directly. Operations that need
   * constants in registers should use the explicit tcc_ir_materialize_const_to_reg() helper.
   *
   * Similarly, VT_CMP and VT_JMP/VT_JMPI are typically handled by branch/conditional ops
   * directly. Only materialize them when explicitly needed via tcc_ir_materialize_const_to_reg(). */

  /* Check for spilled values - this is the original materialization path */
  if (!(sv->pr0 & PREG_SPILLED))
  {
    return;
  }
  if (!tcc_is_vreg_valid(ir, sv->vr))
  {
    return;
  }

  if (!(sv->r & VT_LVAL) && (val_kind == VT_LOCAL || val_kind == VT_LLOCAL))
  {
    /* VT_LOCAL without VT_LVAL represents "address of stack location".
     * This is an address computation (fp + offset), not a value to be loaded.
     * Skip materialization - the backend will compute the address directly. */
    return;
  }

  tcc_ir_require_materialization_result(result, "materialize_value(spill)");

  const int frame_offset = tcc_ir_materialization_offset(ir, sv);
  unsigned short original_r = sv->r;

  result->original_pr0 = sv->pr0;
  result->original_pr1 = sv->pr1;
  result->original_c_i = sv->c.i;

  TCCMachineScratchRegs scratch = {0};
  tcc_machine_acquire_scratch(&scratch, scratch_flags);
  if (scratch.reg_count == 0)
    tcc_error("compiler_error: unable to allocate scratch register for spill load");

  tcc_machine_load_spill_slot(scratch.regs[0], frame_offset);
  if (is_64bit)
  {
    if (scratch.reg_count < 2)
      tcc_error("compiler_error: missing register pair for 64-bit spill load");
    tcc_machine_load_spill_slot(scratch.regs[1], frame_offset + 4);
  }

  int preserved_flags = sv->r & ~VT_VALMASK;
  /* The spill slot stores the vreg's VALUE.
   *
   * Important distinction:
   * - VT_LVAL on a normal (non-VT_LOCAL) operand means "load through pointer" and
   *   must be preserved.
   * - VT_LVAL on VT_LOCAL/VT_LLOCAL means "load from stack slot". Once we've
   *   loaded the spill slot into a register, that flag must be cleared, otherwise
   *   downstream code will incorrectly dereference the loaded value as an address
   *   (double-deref), e.g. treating an int loop index as int*.
   */
  {
    const int orig_kind = original_r & VT_VALMASK;
    if (orig_kind == VT_LOCAL || orig_kind == VT_LLOCAL)
      preserved_flags &= ~VT_LVAL;
  }

  sv->pr0 = scratch.regs[0];
  sv->pr1 = is_64bit ? scratch.regs[1] : PREG_NONE;
  /* sv->r should only contain the register number and semantic flags (VT_LVAL, VT_PARAM, etc.),
   * not PREG_SPILLED which is only for sv->pr0 */
  sv->r = (unsigned short)(scratch.regs[0] | preserved_flags);
  sv->c.i = 0;

  result->used_scratch = 1;
  result->is_64bit = is_64bit;
  result->original_r = original_r;
  result->scratch = scratch;
}

/* Explicit helper to materialize constants, comparisons, or jump results into registers.
 * Unlike tcc_ir_materialize_value() which only handles spills, this function explicitly
 * loads VT_CONST, VT_CMP, VT_JMP, VT_JMPI values into scratch registers.
 * Use this when an operation requires its operand to be in a register (e.g., reg-reg binops). */
void tcc_ir_materialize_const_to_reg(TCCIRState *ir, SValue *sv, TCCMaterializedValue *result)
{
  if (result)
    memset(result, 0, sizeof(*result));

  if (!ir || !sv)
    return;

  const int val_kind = sv->r & VT_VALMASK;

  /* Only handle values that aren't already in a register */
  if (sv->pr0 != PREG_NONE && !(sv->pr0 & PREG_SPILLED))
    return;

  /* Only handle constants, comparisons, and jump conditions */
  if (val_kind != VT_CONST && val_kind != VT_CMP && val_kind != VT_JMP && val_kind != VT_JMPI)
    return;

  /* Skip VT_CONST with VT_SYM (symbol references) - those need special handling */
  if (val_kind == VT_CONST && (sv->r & VT_SYM))
    return;

  /* Skip VT_CONST with VT_LVAL (memory loads) - those need load_to_dest */
  if (val_kind == VT_CONST && (sv->r & VT_LVAL))
    return;

  tcc_ir_require_materialization_result(result, "materialize_const_to_reg");

  const int is_64bit = tcc_ir_is_64bit_type(sv->type.t);
  const unsigned scratch_flags =
      (is_64bit ? TCC_MACHINE_SCRATCH_NEEDS_PAIR : 0) | (ir ? ir->codegen_materialize_scratch_flags : 0);

  result->original_pr0 = sv->pr0;
  result->original_pr1 = sv->pr1;
  result->original_c_i = sv->c.i;
  result->original_r = sv->r;

  TCCMachineScratchRegs scratch = {0};
  tcc_machine_acquire_scratch(&scratch, scratch_flags);
  if (scratch.reg_count == 0)
    tcc_error("compiler_error: unable to allocate scratch register for const-to-reg");

  if (val_kind == VT_CONST)
  {
    tcc_machine_load_constant(scratch.regs[0], is_64bit ? scratch.regs[1] : PREG_NONE, sv->c.i, is_64bit);
  }
  else if (val_kind == VT_CMP)
  {
    tcc_machine_load_cmp_result(scratch.regs[0], sv->c.i);
  }
  else /* VT_JMP or VT_JMPI */
  {
    const int invert = (val_kind == VT_JMPI) ? 1 : 0;
    tcc_machine_load_jmp_result(scratch.regs[0], sv->c.i, invert);
  }

  sv->pr0 = scratch.regs[0];
  sv->pr1 = is_64bit ? scratch.regs[1] : PREG_NONE;
  sv->r = (unsigned short)(scratch.regs[0]);
  sv->c.i = 0;

  result->used_scratch = 1;
  result->is_64bit = is_64bit;
  result->scratch = scratch;
}

void tcc_ir_materialize_addr(TCCIRState *ir, SValue *sv, TCCMaterializedAddr *result, int dest_reg)
{
  if (result)
    memset(result, 0, sizeof(*result));

  if (!ir || !sv)
    return;

  const int val_kind = sv->r & VT_VALMASK;
  const int wants_stack_address = (val_kind == VT_LOCAL || val_kind == VT_LLOCAL) && !(sv->r & VT_LVAL);
  /* Check for spilled pointer: pr0 must be PREG_SPILLED (0x80), NOT PREG_NONE (0xFF).
   * PREG_NONE has the PREG_SPILLED bit set, so we must explicitly exclude it. */
  const int spilled_pointer = (sv->pr0 != PREG_NONE) && (sv->pr0 & PREG_SPILLED);

  if (!wants_stack_address && !spilled_pointer)
    return;

  /* Optimization: For VT_LOCAL with encodable offsets, skip materialization.
   * Let the backend handle it directly with [base, #offset] addressing mode
   * instead of wasting a scratch register to compute the address. */
  if (wants_stack_address)
  {
    const int frame_offset = tcc_ir_materialization_offset(ir, sv);
    const int is_param = (sv->r & VT_PARAM) ? 1 : 0;
    /* Use the actual destination register for the encoding test.
     * If dest_reg is invalid (PREG_NONE), fall back to r12 (typical scratch). */
    const int test_reg = (dest_reg != PREG_NONE && dest_reg < 16) ? dest_reg : 12;
    if (tcc_machine_can_encode_stack_offset_with_param_adj(frame_offset, is_param, test_reg))
      return; /* Backend can encode this offset directly, no scratch needed */
  }

  tcc_ir_require_materialization_result(result, "materialize_addr");

  result->original_r = sv->r;
  result->original_pr0 = sv->pr0;
  result->original_pr1 = sv->pr1;
  result->original_c_i = sv->c.i;

  TCCMachineScratchRegs scratch = {0};
  tcc_machine_acquire_scratch(&scratch, (ir ? ir->codegen_materialize_scratch_flags : 0));
  if (scratch.reg_count == 0)
    tcc_error("compiler_error: unable to allocate scratch register for address materialization");

  const int target_reg = scratch.regs[0];
  const int frame_offset = tcc_ir_materialization_offset(ir, sv);

  if (wants_stack_address)
  {
    tcc_machine_addr_of_stack_slot(target_reg, frame_offset);
    int flags = (sv->r & ~VT_VALMASK) | VT_LVAL;
    sv->pr0 = target_reg;
    sv->pr1 = PREG_NONE;
    sv->r = (unsigned short)(target_reg | flags);
    sv->c.i = 0;
  }
  else if (spilled_pointer)
  {
    tcc_machine_load_spill_slot(target_reg, frame_offset);
    sv->pr0 = target_reg;
    sv->pr1 = PREG_NONE;
    sv->r = (unsigned short)((sv->r & ~VT_VALMASK) | target_reg);
    sv->c.i = 0;
  }

  result->used_scratch = 1;
  result->scratch = scratch;
}

void tcc_ir_materialize_dest(TCCIRState *ir, SValue *dest, TCCMaterializedDest *result)
{
  if (result)
    memset(result, 0, sizeof(*result));

  if (!ir || !dest)
    return;
  if (!(dest->pr0 & PREG_SPILLED))
    return;
  if (!tcc_is_vreg_valid(ir, dest->vr))
    return;

  tcc_ir_require_materialization_result(result, "materialize_dest");

  const int frame_offset = tcc_ir_materialization_offset(ir, dest);
  const int is_64bit = tcc_ir_is_64bit_type(dest->type.t);
  const unsigned scratch_flags =
      (is_64bit ? TCC_MACHINE_SCRATCH_NEEDS_PAIR : 0) | (ir ? ir->codegen_materialize_scratch_flags : 0);
  TCCMachineScratchRegs scratch = {0};
  tcc_machine_acquire_scratch(&scratch, scratch_flags);
  if (scratch.reg_count == 0)
    tcc_error("compiler_error: unable to allocate scratch register for spill destination");
  if (is_64bit && scratch.reg_count < 2)
    tcc_error("compiler_error: missing register pair for 64-bit spill destination");

  result->needs_storeback = 1;
  result->is_64bit = is_64bit;
  result->frame_offset = frame_offset;
  result->original_pr0 = dest->pr0;
  result->original_pr1 = dest->pr1;
  result->original_r = dest->r;
  result->scratch = scratch;

  dest->pr0 = scratch.regs[0];
  dest->pr1 = is_64bit ? scratch.regs[1] : PREG_NONE;
  int flags = dest->r & ~VT_VALMASK;
  flags &= ~VT_LVAL;
  dest->r = (unsigned short)(dest->pr0 | flags);
  dest->c.i = 0;
}

static void tcc_ir_storeback_materialized_dest(TACQuadruple *q, TCCMaterializedDest *mat)
{
  if (!mat || !mat->needs_storeback)
    return;

  tcc_machine_store_spill_slot(q->dest.pr0, mat->frame_offset);
  if (mat->is_64bit)
    tcc_machine_store_spill_slot(q->dest.pr1, mat->frame_offset + 4);

  tcc_machine_release_scratch(&mat->scratch);

  q->dest.pr0 = mat->original_pr0;
  q->dest.pr1 = mat->original_pr1;
  q->dest.r = mat->original_r;
  q->dest.c.i = mat->frame_offset;
}

static void tcc_ir_release_materialized_value(SValue *sv, TCCMaterializedValue *mat)
{
  if (!mat || !mat->used_scratch)
    return;

  tcc_machine_release_scratch(&mat->scratch);
  if (sv)
  {
    sv->pr0 = mat->original_pr0;
    sv->pr1 = mat->original_pr1;
    sv->r = mat->original_r;
    sv->c.i = mat->original_c_i;
  }
}

static void tcc_ir_release_materialized_addr(SValue *sv, TCCMaterializedAddr *mat)
{
  if (!mat || !mat->used_scratch)
    return;

  tcc_machine_release_scratch(&mat->scratch);
  if (sv)
  {
    sv->pr0 = mat->original_pr0;
    sv->pr1 = mat->original_pr1;
    sv->r = mat->original_r;
    sv->c.i = mat->original_c_i;
  }
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
        /* NOTE: For leaf functions, the linear scanner has already assigned registers.
         * Don't overwrite interval->allocation here - it would clobber the correct allocation
         * with argno (parameter index), which is NOT the same as the physical register number.
         * The prolog will use incoming_reg0/1 to know which registers the parameter arrives in. */
        /* REMOVED BUGGY CODE:
        if (ir->leaffunc && !already_spilled)
        {
          tcc_ir_assign_physical_register(ir, encoded_vreg, 0, argno, argno + 1);
        }
        */
        /* If already_spilled or non-leaf: keep the spill location from
         * linear scan, prolog will store incoming registers to stack */
      }
      else
      {
        /* Spilled to caller's stack frame - parameter passed on stack */
        interval->incoming_reg0 = -1;
        interval->incoming_reg1 = -1;
        /* Record where the parameter arrives on the caller's stack frame.
         * This is relative to SP after prolog (positive offset above saved regs).
         * The prolog needs to load this into the allocated register. */
        interval->original_offset = (argno - 4) * 4;
        /* IMPORTANT: If linear scan already spilled this parameter, keep the
         * allocator-provided spill location (a negative FP-relative offset).
         * Overwriting allocation.offset with the caller-stack offset would make
         * the prolog store into the saved-register block (FP+0..), corrupting
         * callee-saved restores (e.g. saved r7).
         *
         * The caller-stack location is tracked in original_offset and used by
         * the prolog to load from the incoming argument area.
         */
        interval->allocation.r0 = PREG_NONE;
        interval->allocation.r1 = PREG_NONE;
        interval->allocation.offset = 0;
      }
      argno += 2;
    }
    else
    {
      if (argno <= 3)
      {
        interval->incoming_reg0 = argno;
        interval->incoming_reg1 = -1;
        /* NOTE: For leaf functions, the linear scanner has already assigned registers.
         * Don't overwrite interval->allocation here - it would clobber the correct allocation
         * with argno (parameter index), which is NOT the same as the physical register number.
         * The prolog will use incoming_reg0 to know which register the parameter arrives in. */
        /* REMOVED BUGGY CODE:
        if (ir->leaffunc && !already_spilled)
        {
          tcc_ir_assign_physical_register(ir, encoded_vreg, 0, argno, -1);
        }
        */
      }
      else
      {
        /* Spilled to caller's stack frame - parameter passed on stack */
        interval->incoming_reg0 = -1;
        interval->incoming_reg1 = -1;
        /* Record where the parameter arrives on the caller's stack frame.
         * This is relative to SP after prolog (positive offset above saved regs).
         * The prolog needs to load this into the allocated register. */
        interval->original_offset = (argno - 4) * 4;
        /* See 64-bit case above: do not overwrite allocator spill slots with
         * caller-stack offsets.
         */
        interval->allocation.r0 = PREG_NONE;
        interval->allocation.r1 = PREG_NONE;
        interval->allocation.offset = 0;
      }
      argno++;
    }
  }
}

/* Mark function return value vregs with incoming_reg0 to indicate they arrive in r0.
 * This allows the register allocator to decide whether to keep them in r0 or move them
 * if r0 is needed for something else (like the next call's argument). */
void tcc_ir_mark_return_value_incoming_regs(TCCIRState *ir)
{
  if (!ir)
    return;

  /* Scan all instructions to find FUNCCALLVAL that produce return values */
  for (int i = 0; i < ir->next_instruction_index; ++i)
  {
    TACQuadruple *q = &ir->instructions[i];
    if (q->op != TCCIR_OP_FUNCCALLVAL)
      continue;

    /* dest is the vreg that receives the return value */
    if (q->dest.vr < 0 || !tcc_is_vreg_valid(ir, q->dest.vr))
      continue;

    IRLiveInterval *interval = tcc_ir_get_live_interval(ir, q->dest.vr);
    if (!interval)
      continue;

    /* Mark that this vreg arrives in r0 (or r0+r1 for 64-bit returns) */
    interval->incoming_reg0 = 0; /* r0 */
    if (interval->is_llong || interval->is_double)
      interval->incoming_reg1 = 1; /* r1 */
    else
      interval->incoming_reg1 = -1;
  }
}

void tcc_ir_avoid_spilling_stack_passed_params(TCCIRState *ir)
{
  if (!ir)
    return;

  /* Compute which PARAM vregs are stack-passed under AAPCS.
   * We intentionally do this before patching IRLiveInterval allocations,
   * operating on the linear-scan table so we can also shrink `loc`/frame size.
   */
  const int param_count = ir->next_parameter;
  if (param_count <= 0)
    return;

  uint8_t *is_stack_passed = tcc_mallocz((size_t)param_count);
  int argno = 0;
  for (int vreg = 0; vreg < param_count; ++vreg)
  {
    const int encoded_vreg = (TCCIR_VREG_TYPE_PARAM << 28) | vreg;
    IRLiveInterval *interval = tcc_ir_get_live_interval(ir, encoded_vreg);
    if (!interval)
      continue;

    const int is_64bit = interval->is_double || interval->is_llong;
    if (is_64bit && (argno & 1))
      argno++; /* align 64-bit to even reg pair */

    const int in_regs = is_64bit ? (argno <= 2) : (argno <= 3);
    if (!in_regs)
      is_stack_passed[vreg] = 1;

    argno += is_64bit ? 2 : 1;
  }

  /* Rewrite linear-scan results: stack-passed params already have an incoming
   * memory home (caller arg area), so if the allocator spilled them, drop the
   * local spill slot. Also force address-taken stack params to remain in
   * memory (we can use the incoming slot as their addressable home).
   */
  for (int i = 0; i < ir->ls.next_interval_index; ++i)
  {
    LSLiveInterval *ls = &ir->ls.intervals[i];
    if (TCCIR_DECODE_VREG_TYPE((int)ls->vreg) != TCCIR_VREG_TYPE_PARAM)
      continue;
    const int pidx = TCCIR_DECODE_VREG_POSITION((int)ls->vreg);
    if (pidx < 0 || pidx >= param_count)
      continue;
    if (!is_stack_passed[pidx])
      continue;

    /* Stack-passed params live in the caller's argument area. If linear-scan
     * assigned them a register (without spilling), the prolog won't load them
     * into that register, causing incorrect code. Always reset r0/r1 to force
     * them to use the incoming stack location via VT_PARAM path. */
    ls->r0 = PREG_NONE;
    ls->r1 = PREG_NONE;
    ls->stack_location = 0;
  }

  tcc_free(is_stack_passed);
}

void tcc_ir_fill_registers(TCCIRState *ir, SValue *sv)
{
  int old_r = sv->r;
  int old_v = old_r & VT_VALMASK;

  /* VT_LOCAL/VT_LLOCAL operands can mean either:
   * - a concrete stack slot (vr == -1), e.g. VLA save slots, or
   * - a logical local tracked as a vreg by the IR (vr != -1).
   *
   * For concrete stack slots, do not rewrite them into registers here; doing
   * so can create uninitialized register reads at runtime.
   *
   * For locals that do carry a vreg, they must participate in register
   * allocation so that defs/uses stay consistent.
   */
  if ((old_v == VT_LOCAL || old_v == VT_LLOCAL) && sv->vr == -1)
  {
    sv->pr0 = PREG_NONE;
    sv->pr1 = PREG_NONE;
    return;
  }
  if (tcc_is_vreg_valid(ir, sv->vr))
  {
    IRLiveInterval *interval = tcc_ir_get_live_interval(ir, sv->vr);

    /* Stack-passed parameters: if not allocated to a register, treat them as
     * residing in the incoming argument area (VT_PARAM) rather than forcing a
     * separate local spill slot.
     *
     * This is safe under AAPCS: the caller's argument stack area remains valid
     * for the duration of the call, and it also provides a correct addressable
     * home for '&param' semantics.
     */
    if (TCCIR_DECODE_VREG_TYPE(sv->vr) == TCCIR_VREG_TYPE_PARAM && interval && interval->incoming_reg0 < 0 &&
        interval->allocation.r0 == PREG_NONE && interval->allocation.offset == 0)
    {
      sv->pr0 = PREG_NONE;
      sv->pr1 = PREG_NONE;
      sv->c.i = interval->original_offset;

      int need_lval = (old_r & VT_LVAL);
      if (old_v < VT_CONST && old_v != VT_LOCAL && old_v != VT_LLOCAL && interval->is_lvalue)
        need_lval = VT_LVAL;

      sv->r = VT_LOCAL | need_lval | VT_PARAM;
      return;
    }

    /* Register-passed parameters: if allocated to a register (not spilled),
     * clear VT_LVAL. The value is already in the register, no dereference needed.
     * VT_LVAL is only used on parameters for address-of operations (&param) or
     * when they're on the stack (VT_LOCAL).
     */
    int is_register_param =
        (TCCIR_DECODE_VREG_TYPE(sv->vr) == TCCIR_VREG_TYPE_PARAM && interval && interval->incoming_reg0 >= 0);

    sv->pr0 = interval->allocation.r0;
    sv->pr1 = interval->allocation.r1;
    sv->c.i = interval->allocation.offset;

    /* Determine if we should preserve VT_LVAL:
     * - If old_r was VT_LOCAL|VT_LVAL (local variable on stack), and now
     *   it's allocated to a register, we should NOT preserve VT_LVAL because
     *   the value is already in the register, no load needed.
     * - If old_r has VT_LVAL but (old_r & VT_VALMASK) < VT_CONST, it means
     *   the vreg holds a pointer that needs dereferencing - preserve VT_LVAL.
     * - Register parameters: do NOT preserve VT_LVAL when allocated to a register.
     *   VT_LVAL on parameters is only needed for stack params (VT_LOCAL) or for
     *   address-of operations.
     * - If old_r does NOT have VT_LVAL, this is an address-of operation
     *   (we want the address, not the value). Do NOT add VT_LVAL. */
    int preserve_flags = old_r & VT_PARAM; /* Always preserve VT_PARAM */
    if ((old_r & VT_LVAL) && old_v < VT_CONST && old_v != VT_LOCAL && old_v != VT_LLOCAL && !is_register_param)
    {
      /* The vreg holds a pointer that needs dereferencing.
       * Note: VT_LOCAL/VT_LLOCAL use VT_LVAL to mean "load from stack slot".
       * When such a local/param is promoted to a register, we must NOT
       * preserve VT_LVAL, otherwise we turn a plain value into a pointer
       * dereference (double-indirection bugs).
       */
      preserve_flags |= VT_LVAL;
    }

    if (interval->allocation.r0 == PREG_SPILLED || interval->allocation.offset != 0)
    {
      /* Spilled to stack - treat as local.
       * For computed values (old_r was 0 or a register), add VT_LVAL to load the value.
       * For address-of expressions (old_r == VT_LOCAL without VT_LVAL), don't add VT_LVAL.
       * If original had VT_LVAL (pointer dereference), preserve it.
       *
       * DOUBLE INDIRECTION CASE: If old_r has VT_LVAL AND the original was NOT
       * already a local variable (VT_LOCAL), then the code wants to DEREFERENCE
       * the value held in this vreg. If that value is spilled:
       *   - Spill slot contains a POINTER value (e.g., result of ADD on address)
       *   - Need to: (1) load pointer from spill, (2) dereference it
       * Use VT_LLOCAL to encode this double-indirection requirement.
       *
       * But if old_v == VT_LOCAL, the VT_LVAL means "load/store from/to this stack slot"
       * which is standard local variable access - do NOT use VT_LLOCAL.
       *
       * ADDRESS-OF CASE: If old_v == VT_LOCAL and old_r does NOT have VT_LVAL,
       * this is an address-of operation (&var). We want the ADDRESS of the spill
       * slot, not its contents. Do NOT add VT_LVAL in this case.
       *
       * COMPUTED VALUE CASE: If old_v was a register (computed value that got
       * spilled), we ALWAYS need VT_LVAL to load the value from the spill slot. */
      int need_lval;
      if (old_v == VT_LOCAL || old_v == VT_LLOCAL)
      {
        /* Local variable: preserve VT_LVAL to distinguish load vs address-of */
        need_lval = (old_r & VT_LVAL);
      }
      else
      {
        /* Computed value (was in register): always need VT_LVAL to load from spill */
        need_lval = VT_LVAL;
      }
      int base_kind = VT_LOCAL;
      if ((old_r & VT_LVAL) && old_v != VT_LOCAL && old_v != VT_LLOCAL)
      {
        /* The original use wants to dereference the value in this vreg.
         * Since the value is spilled, we need double indirection:
         * load pointer from spill slot, then dereference it.
         * Note: We exclude VT_LOCAL/VT_LLOCAL because their VT_LVAL means
         * "access this stack slot" not "dereference pointer in vreg". */
        base_kind = VT_LLOCAL;
      }
      sv->r = base_kind | need_lval | (old_r & VT_PARAM);
    }
    else if (interval->allocation.r0 != PREG_NONE)
    {
      /* In a register - set r to the register number, preserving VT_LVAL only for pointer derefs */
      sv->r = interval->allocation.r0 | preserve_flags;
    }
  }
  else if ((sv->vr == -1 || sv->vr == 0 || TCCIR_DECODE_VREG_TYPE(sv->vr) == 0) &&
           (sv->r == -1 || sv->r == (int)0xffff || (sv->r & VT_VALMASK) == 0x3f))
  {
    /* No valid vreg and invalid .r - this is likely a constant that wasn't
       properly marked. Treat as VT_CONST while preserving important flags. */
    int flags = sv->r & (VT_LVAL | VT_SYM);
    sv->r = VT_CONST | flags;
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

  uint8_t *reachable = tcc_mallocz((n + 7) / 8);
  int *new_index = tcc_malloc(n * sizeof(int));
  int *worklist = tcc_malloc(n * sizeof(int));
  int worklist_head = 0, worklist_tail = 0;

/* Mark instruction as reachable if not already marked */
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
    case TCCIR_OP_IJUMP:
      /* Indirect jump (computed goto).
         The successor set is not statically known, but in typical patterns
         (like GCC's labels-as-values jump tables) targets are within the same
         function and code continues at/after those labels.
         Conservatively keep fall-through reachable to avoid deleting label
         blocks and subsequent code. */
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
        if (old_target >= 0 && old_target < n)
        {
          q->dest.c.i = new_index[old_target];
        }
        else if (old_target >= n)
        {
          /* Target is past the end (epilogue) - adjust for removed instructions
           */
          q->dest.c.i = new_count;
        }
        /* else: old_target < 0 means unpatched jump, leave as -1 */
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

    /* For STORE operations, the dest field is used as a pointer (address to store to),
     * not as a destination being written. If dest has VT_LVAL, the vreg is being
     * dereferenced, so it's a USE not a DEF. Mark it as used. */
    if (q->op == TCCIR_OP_STORE && TCCIR_DECODE_VREG_TYPE(q->dest.vr) == TCCIR_VREG_TYPE_TEMP)
    {
      int pos = TCCIR_DECODE_VREG_POSITION(q->dest.vr);
      if (pos <= max_tmp_pos)
        used[pos / 8] |= (1 << (pos % 8));
    }
  }

  /* Remove ASSIGN instructions where dest is an unused TMP vreg, and NOP instructions */
  int changes = 0;
  int write_pos = 0;
  int *new_index = tcc_malloc(sizeof(int) * n);

#ifdef DEBUG_IR_GEN
  printf("=== DEAD STORE ELIMINATION START ===\n");
#endif

  for (int i = 0; i < n; i++)
  {
    TACQuadruple *q = &ir->instructions[i];
    int keep = 1;

    /* Remove NOP instructions */
    if (q->op == TCCIR_OP_NOP)
    {
      keep = 0;
      changes++;
    }
    /* Remove ASSIGN instructions where dest is an unused TMP vreg */
    else if (q->op == TCCIR_OP_ASSIGN && TCCIR_DECODE_VREG_TYPE(q->dest.vr) == TCCIR_VREG_TYPE_TEMP)
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

#ifdef DEBUG_IR_GEN
  printf("=== DEAD STORE ELIMINATION END (removed %d, n=%d -> %d) ===\n", changes, n, write_pos);
#endif

  /* Update jump targets */
  for (int i = 0; i < write_pos; i++)
  {
    TACQuadruple *q = &ir->instructions[i];
    if (q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_JUMPIF)
    {
      int old_target = q->dest.c.i;
      if (old_target >= 0 && old_target < n)
      {
        if (new_index[old_target] >= 0)
        {
          q->dest.c.i = new_index[old_target];
        }
        else
        {
          /* Target instruction was removed - find next valid instruction */
          int next = old_target + 1;
          while (next < n && new_index[next] < 0)
            next++;
          if (next < n)
            q->dest.c.i = new_index[next];
          else
            q->dest.c.i = write_pos; /* Past end */
        }
      }
      else if (old_target >= n)
        q->dest.c.i = write_pos; /* Past end */
      /* else: old_target < 0 means unpatched, leave as -1 */
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
    /* Skip if vr1 or vr2 is -1 (invalid, e.g., from constants) */
    if (vr1 >= 0 && vr1 == vr2)
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

/* Return value optimization - fold LOAD -> RETURNVALUE patterns
 * When a temp vreg is loaded and only used by RETURNVALUE,
 * propagate the load source directly to RETURNVALUE.
 * This avoids allocating an intermediate register.
 *
 * Pattern: temp = LOAD [addr]; RETURNVALUE temp
 * Becomes: RETURNVALUE [addr]
 *
 * Only applies when the source is a memory location that has been stored to.
 * Does not apply to register-only local variables.
 *
 * NOTE: Currently disabled - requires more complex analysis to ensure
 * the source variable is actually in memory (not just in registers from
 * constant initialization).
 */
int tcc_ir_return_value_optimization(TCCIRState *ir)
{
  /* Disabled for now - the optimization incorrectly handles cases where
   * a local variable is initialized from a constant (loaded to registers)
   * but never stored to memory. The RETURNVALUE then tries to load from
   * a memory location that doesn't contain the value.
   *
   * TODO: Enable this optimization only when we can prove the source
   * variable has been stored to memory (has a STORE instruction to it).
   */
  return 0;

#if 0 /* Original implementation - kept for reference */
  int n = ir->next_instruction_index;
  int changes = 0;

  if (n < 2)
    return 0;

  /* For each RETURNVALUE, check if its source is a temp vreg
   * that was just loaded and used only by this RETURNVALUE */
  for (int i = 0; i < n; i++)
  {
    TACQuadruple *ret = &ir->instructions[i];
    if (ret->op != TCCIR_OP_RETURNVALUE)
      continue;

    /* src1 must be a temp vreg */
    if (TCCIR_DECODE_VREG_TYPE(ret->src1.vr) != TCCIR_VREG_TYPE_TEMP)
      continue;

    int ret_vreg = ret->src1.vr;

    /* Find the instruction that defines this temp vreg */
    int def_idx = -1;
    for (int j = i - 1; j >= 0; j--)
    {
      TACQuadruple *q = &ir->instructions[j];
      if (irop_config[q->op].has_dest && q->dest.vr == ret_vreg)
      {
        def_idx = j;
        break;
      }
    }

    if (def_idx < 0)
      continue;

    TACQuadruple *def = &ir->instructions[def_idx];

    /* Must be a LOAD instruction */
    if (def->op != TCCIR_OP_LOAD)
      continue;

    /* The LOAD source must be the same type as LOAD destination (no cast involved).
     * If there's a type mismatch (e.g., loading int from long long local),
     * we can't optimize because we'd need to do a proper truncating load. */
    int src_btype = def->src1.type.t & VT_BTYPE;
    int dst_btype = def->dest.type.t & VT_BTYPE;
#ifdef DEBUG_IR_GEN
#endif
    if (src_btype != dst_btype)
      continue; /* Type cast involved, don't optimize */

    /* Check that the temp vreg is only used by this RETURNVALUE */
    int use_count = 0;
    for (int j = def_idx + 1; j < n; j++)
    {
      TACQuadruple *q = &ir->instructions[j];
      if (irop_config[q->op].has_src1 && q->src1.vr == ret_vreg)
        use_count++;
      if (irop_config[q->op].has_src2 && q->src2.vr == ret_vreg)
        use_count++;
    }

    if (use_count != 1)
      continue;

    /* Optimization: propagate LOAD source to RETURNVALUE, but keep the return type */
#ifdef DEBUG_IR_GEN
    printf("OPTIMIZE: LOAD vr%d -> RETURNVALUE vr%d => RETURNVALUE from mem\n", ret_vreg, ret_vreg);
#endif

    /* Save the return type (from the LOAD destination, which has the cast type) */
    CType ret_type = def->dest.type;

    /* Copy the LOAD source to RETURNVALUE */
    ret->src1 = def->src1;
    /* Preserve the return type (may be different from source type due to cast) */
    ret->src1.type = ret_type;

    /* Mark the LOAD for elimination by converting to no-op
     * (DCE will clean it up) */
    def->op = TCCIR_OP_ASSIGN;
    def->src1 = def->dest; /* Self-assign becomes no-op */

    changes++;
  }

  return changes;
#endif
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

/* Constant Propagation with Algebraic Simplification
 * Phase 1: Track constant variables, propagate them, and apply algebraic simplifications
 * Patterns:
 *   - Replace uses of constant VARs with immediate values
 *   - X + 0 = X, X - 0 = X, X * 1 = X, X * 0 = 0
 *   - X & 0 = 0, X & -1 = X, X | 0 = X, X | -1 = -1
 *   - X << 0 = X, X >> 0 = X, 0 << X = 0
 *   - C1 OP C2 = result (full constant folding)
 */
int tcc_ir_constant_propagation(TCCIRState *ir)
{
  /* VarConstInfo: track constant variables */
  typedef struct
  {
    uint8_t is_constant : 1;
    uint8_t def_count : 7;
    int64_t value;
  } VarConstInfo;

  int n = ir->next_instruction_index;
  int changes = 0;
  int max_var_pos = 0;
  int i;
  TACQuadruple *q;
  VarConstInfo *var_info;

  if (n == 0)
    return 0;

  /* Track which VAR vregs are constant (assigned exactly once with a constant value) */
  for (i = 0; i < n; i++)
  {
    q = &ir->instructions[i];
    if (irop_config[q->op].has_dest && TCCIR_DECODE_VREG_TYPE(q->dest.vr) == TCCIR_VREG_TYPE_VAR)
    {
      int pos = TCCIR_DECODE_VREG_POSITION(q->dest.vr);
      if (pos > max_var_pos)
        max_var_pos = pos;
    }
  }

  if (max_var_pos == 0)
    return 0;

  var_info = tcc_mallocz(sizeof(VarConstInfo) * (max_var_pos + 1));

  /* First pass: identify constant variables */
  for (i = 0; i < n; i++)
  {
    TACQuadruple *q = &ir->instructions[i];

    /* Track definitions of VAR vregs */
    if (irop_config[q->op].has_dest && TCCIR_DECODE_VREG_TYPE(q->dest.vr) == TCCIR_VREG_TYPE_VAR)
    {
      int pos = TCCIR_DECODE_VREG_POSITION(q->dest.vr);
      if (pos <= max_var_pos)
      {
        var_info[pos].def_count++;

        /* Check if this is a constant assignment */
        if (q->op == TCCIR_OP_ASSIGN && (q->src1.r & VT_VALMASK) == VT_CONST && !(q->src1.r & VT_SYM))
        {
          if (var_info[pos].def_count == 1)
          {
            var_info[pos].is_constant = 1;
            var_info[pos].value = q->src1.c.i;
          }
        }
        else
        {
          /* Non-constant assignment - mark as non-constant */
          var_info[pos].is_constant = 0;
        }
      }
    }
  }

  /* Mark variables with multiple definitions as non-constant */
  for (i = 0; i <= max_var_pos; i++)
  {
    if (var_info[i].def_count > 1)
      var_info[i].is_constant = 0;
  }

  /* Second pass: propagate constants and apply algebraic simplifications */
  for (i = 0; i < n; i++)
  {
    int src1_is_const, src2_is_const;
    int64_t result;
    int can_fold;
    int skip_bool_prop;

    q = &ir->instructions[i];

    /* For BOOL_AND/BOOL_OR, don't propagate constants unless both become constants.
     * The code generator can't handle mixed const/reg operands for these ops. */
    skip_bool_prop = 0;
    if (q->op == TCCIR_OP_BOOL_AND || q->op == TCCIR_OP_BOOL_OR)
    {
      int src1_can_be_const = 0, src2_can_be_const = 0;
      /* Check if both would become constants */
      if (TCCIR_DECODE_VREG_TYPE(q->src1.vr) == TCCIR_VREG_TYPE_VAR)
      {
        int pos = TCCIR_DECODE_VREG_POSITION(q->src1.vr);
        if (pos <= max_var_pos && var_info[pos].is_constant)
          src1_can_be_const = 1;
      }
      else if ((q->src1.r & VT_VALMASK) == VT_CONST && !(q->src1.r & VT_SYM))
        src1_can_be_const = 1;

      if (TCCIR_DECODE_VREG_TYPE(q->src2.vr) == TCCIR_VREG_TYPE_VAR)
      {
        int pos = TCCIR_DECODE_VREG_POSITION(q->src2.vr);
        if (pos <= max_var_pos && var_info[pos].is_constant)
          src2_can_be_const = 1;
      }
      else if ((q->src2.r & VT_VALMASK) == VT_CONST && !(q->src2.r & VT_SYM))
        src2_can_be_const = 1;

      /* Skip propagation if only ONE would become constant (can't generate code) */
      if (src1_can_be_const != src2_can_be_const)
        skip_bool_prop = 1;
    }

    /* Propagate constant VAR vregs to immediate values.
     * IMPORTANT: Don't propagate if src1 is VT_LOCAL without VT_LVAL - that means
     * "address of local variable", not its value. The address must be computed at runtime. */
    if (!skip_bool_prop && irop_config[q->op].has_src1 && TCCIR_DECODE_VREG_TYPE(q->src1.vr) == TCCIR_VREG_TYPE_VAR &&
        !((q->src1.r & VT_VALMASK) == VT_LOCAL && !(q->src1.r & VT_LVAL)))
    {
      int pos = TCCIR_DECODE_VREG_POSITION(q->src1.vr);
      if (pos <= max_var_pos && var_info[pos].is_constant)
      {
        q->src1.r = VT_CONST;
        q->src1.c.i = var_info[pos].value;
        q->src1.vr = -1;
        changes++;
      }
    }

    if (!skip_bool_prop && irop_config[q->op].has_src2 && TCCIR_DECODE_VREG_TYPE(q->src2.vr) == TCCIR_VREG_TYPE_VAR &&
        !((q->src2.r & VT_VALMASK) == VT_LOCAL && !(q->src2.r & VT_LVAL)))
    {
      int pos = TCCIR_DECODE_VREG_POSITION(q->src2.vr);
      if (pos <= max_var_pos && var_info[pos].is_constant)
      {
        q->src2.r = VT_CONST;
        q->src2.c.i = var_info[pos].value;
        q->src2.vr = -1;
        changes++;
      }
    }

    /* Algebraic simplifications */
    src1_is_const = (q->src1.r & VT_VALMASK) == VT_CONST && !(q->src1.r & VT_SYM);
    src2_is_const = (q->src2.r & VT_VALMASK) == VT_CONST && !(q->src2.r & VT_SYM);

    /* For commutative operations, if src1 is const and src2 is not, swap them.
     * This ensures constants end up in src2 where the code generator expects them.
     * Note: BOOL_AND/BOOL_OR are not included because the code generator doesn't
     * handle constants in either operand - they require both to be registers. */
    if (irop_config[q->op].has_src1 && irop_config[q->op].has_src2 && src1_is_const && !src2_is_const)
    {
      int is_commutative = 0;
      switch (q->op)
      {
      case TCCIR_OP_ADD:
      case TCCIR_OP_MUL:
      case TCCIR_OP_AND:
      case TCCIR_OP_OR:
      case TCCIR_OP_XOR:
        is_commutative = 1;
        break;
      default:
        break;
      }
      if (is_commutative)
      {
        SValue tmp;
#ifdef DEBUG_IR_GEN
        printf("OPTIMIZE: Swap operands for commutative %s (const in src1) at i=%d\n", tcc_ir_get_op_name(q->op), i);
#endif
        tmp = q->src1;
        q->src1 = q->src2;
        q->src2 = tmp;
        /* Update flags after swap */
        src1_is_const = 0;
        src2_is_const = 1;
      }
    }

    /* Full constant folding: C1 OP C2 = result */
    result = 0;
    can_fold = 1;

    if (irop_config[q->op].has_src1 && irop_config[q->op].has_src2 && src1_is_const && src2_is_const)
    {
      switch (q->op)
      {
      case TCCIR_OP_ADD:
        result = q->src1.c.i + q->src2.c.i;
        break;
      case TCCIR_OP_SUB:
        result = q->src1.c.i - q->src2.c.i;
        break;
      case TCCIR_OP_MUL:
        result = q->src1.c.i * q->src2.c.i;
        break;
      case TCCIR_OP_AND:
        result = q->src1.c.i & q->src2.c.i;
        break;
      case TCCIR_OP_OR:
        result = q->src1.c.i | q->src2.c.i;
        break;
      case TCCIR_OP_XOR:
        result = q->src1.c.i ^ q->src2.c.i;
        break;
      case TCCIR_OP_SHL:
        result = q->src1.c.i << q->src2.c.i;
        break;
      case TCCIR_OP_SHR:
        result = (uint64_t)q->src1.c.i >> q->src2.c.i;
        break;
      case TCCIR_OP_SAR:
        result = q->src1.c.i >> q->src2.c.i;
        break;
      case TCCIR_OP_BOOL_AND:
        result = (q->src1.c.i != 0) && (q->src2.c.i != 0) ? 1 : 0;
        break;
      case TCCIR_OP_BOOL_OR:
        result = (q->src1.c.i != 0) || (q->src2.c.i != 0) ? 1 : 0;
        break;
      default:
        can_fold = 0;
        break;
      }

      if (can_fold)
      {
#ifdef DEBUG_IR_GEN
        printf("OPTIMIZE: Constant fold %s(%lld, %lld) = %lld at i=%d\n", tcc_ir_get_op_name(q->op),
               (long long)q->src1.c.i, (long long)q->src2.c.i, (long long)result, i);
#endif
        q->op = TCCIR_OP_ASSIGN;
        q->src1.r = VT_CONST;
        q->src1.c.i = result;
        q->src1.vr = -1;
        memset(&q->src2, 0, sizeof(q->src2));
        q->src2.vr = -1;
        changes++;
        continue;
      }
    }

    /* Algebraic simplifications with one constant operand */
    if (irop_config[q->op].has_src2 && src2_is_const)
    {
      int64_t c;
      int simplify;
      int replace_with_zero;
      int replace_with_const;
      int64_t const_value;

      c = q->src2.c.i;
      simplify = 0;
      replace_with_zero = 0;
      replace_with_const = 0;
      const_value = 0;

      switch (q->op)
      {
      case TCCIR_OP_ADD:
      case TCCIR_OP_SUB:
        if (c == 0)
          simplify = 1; /* X + 0 = X, X - 0 = X */
        break;
      case TCCIR_OP_OR:
        if (c == 0)
          simplify = 1; /* X | 0 = X */
        else if (c == -1 || c == 0xFFFFFFFF)
        {
          replace_with_const = 1; /* X | -1 = -1 */
          const_value = -1;
        }
        break;
      case TCCIR_OP_SHL:
      case TCCIR_OP_SHR:
      case TCCIR_OP_SAR:
        if (c == 0)
          simplify = 1; /* X << 0 = X, X >> 0 = X */
        break;
      case TCCIR_OP_MUL:
        if (c == 1)
          simplify = 1; /* X * 1 = X */
        else if (c == 0)
          replace_with_zero = 1; /* X * 0 = 0 */
        break;
      case TCCIR_OP_DIV:
      case TCCIR_OP_UDIV:
        if (c == 1)
          simplify = 1; /* X / 1 = X */
        break;
      case TCCIR_OP_AND:
        if (c == 0)
          replace_with_zero = 1; /* X & 0 = 0 */
        else if (c == -1 || c == 0xFFFFFFFF)
          simplify = 1; /* X & -1 = X */
        break;
      }

      if (simplify)
      {
#ifdef DEBUG_IR_GEN
        printf("OPTIMIZE: Algebraic simplify %s(x, %lld) = x at i=%d\n", tcc_ir_get_op_name(q->op), (long long)c, i);
#endif
        q->op = TCCIR_OP_ASSIGN;
        /* src1 stays as-is, clear src2 */
        memset(&q->src2, 0, sizeof(q->src2));
        q->src2.vr = -1;
        changes++;
      }
      else if (replace_with_zero)
      {
#ifdef DEBUG_IR_GEN
        printf("OPTIMIZE: Algebraic simplify %s(x, %lld) = 0 at i=%d\n", tcc_ir_get_op_name(q->op), (long long)c, i);
#endif
        q->op = TCCIR_OP_ASSIGN;
        q->src1.r = VT_CONST;
        q->src1.c.i = 0;
        q->src1.vr = -1;
        memset(&q->src2, 0, sizeof(q->src2));
        q->src2.vr = -1;
        changes++;
      }
      else if (replace_with_const)
      {
#ifdef DEBUG_IR_GEN
        printf("OPTIMIZE: Algebraic simplify %s(x, %lld) = %lld at i=%d\n", tcc_ir_get_op_name(q->op), (long long)c,
               (long long)const_value, i);
#endif
        q->op = TCCIR_OP_ASSIGN;
        q->src1.r = VT_CONST;
        q->src1.c.i = const_value;
        q->src1.vr = -1;
        memset(&q->src2, 0, sizeof(q->src2));
        q->src2.vr = -1;
        changes++;
      }
    }

    /* Handle commutative operations: 0 + X = X, 0 << X = 0 */
    if (irop_config[q->op].has_src1 && src1_is_const)
    {
      int64_t c;

      c = q->src1.c.i;

      switch (q->op)
      {
      case TCCIR_OP_ADD:
      case TCCIR_OP_OR:
        if (c == 0)
        {
          /* 0 + X = X, 0 | X = X (commutative, swap operands) */
#ifdef DEBUG_IR_GEN
          printf("OPTIMIZE: Algebraic simplify %s(0, x) = x at i=%d\n", tcc_ir_get_op_name(q->op), i);
#endif
          q->op = TCCIR_OP_ASSIGN;
          q->src1 = q->src2;
          memset(&q->src2, 0, sizeof(q->src2));
          q->src2.vr = -1;
          changes++;
        }
        break;
      case TCCIR_OP_MUL:
        if (c == 0)
        {
          /* 0 * X = 0 */
#ifdef DEBUG_IR_GEN
          printf("OPTIMIZE: Algebraic simplify %s(0, x) = 0 at i=%d\n", tcc_ir_get_op_name(q->op), i);
#endif
          q->op = TCCIR_OP_ASSIGN;
          /* src1 is already 0 */
          memset(&q->src2, 0, sizeof(q->src2));
          q->src2.vr = -1;
          changes++;
        }
        break;
      case TCCIR_OP_SHL:
      case TCCIR_OP_SHR:
      case TCCIR_OP_SAR:
        if (c == 0)
        {
          /* 0 << X = 0, 0 >> X = 0 */
#ifdef DEBUG_IR_GEN
          printf("OPTIMIZE: Algebraic simplify %s(0, x) = 0 at i=%d\n", tcc_ir_get_op_name(q->op), i);
#endif
          q->op = TCCIR_OP_ASSIGN;
          /* src1 is already 0 */
          memset(&q->src2, 0, sizeof(q->src2));
          q->src2.vr = -1;
          changes++;
        }
        break;
      }
    }
  }

  /* Third pass: Fold CMP+SETIF patterns when CMP has constant operands */
  for (i = 0; i < n - 1; i++)
  {
    TACQuadruple *cmp_q = &ir->instructions[i];
    TACQuadruple *setif_q = &ir->instructions[i + 1];
    int cmp_src1_const, cmp_src2_const;
    int64_t val1, val2;
    int cond, result;

    if (cmp_q->op != TCCIR_OP_CMP)
      continue;
    if (setif_q->op != TCCIR_OP_SETIF)
      continue;

    cmp_src1_const = (cmp_q->src1.r & VT_VALMASK) == VT_CONST && !(cmp_q->src1.r & VT_SYM);
    cmp_src2_const = (cmp_q->src2.r & VT_VALMASK) == VT_CONST && !(cmp_q->src2.r & VT_SYM);

    if (!cmp_src1_const || !cmp_src2_const)
      continue;

    val1 = cmp_q->src1.c.i;
    val2 = cmp_q->src2.c.i;
    cond = setif_q->src1.c.i; /* Condition code stored in src1.c.i (TCC token) */

    /* Evaluate the comparison based on TCC token values */
    result = 0;
    switch (cond)
    {
    case 0x94: /* TOK_EQ */
      result = (val1 == val2) ? 1 : 0;
      break;
    case 0x95: /* TOK_NE */
      result = (val1 != val2) ? 1 : 0;
      break;
    case 0x9c: /* TOK_LT */
      result = (val1 < val2) ? 1 : 0;
      break;
    case 0x9d: /* TOK_GE */
      result = (val1 >= val2) ? 1 : 0;
      break;
    case 0x9e: /* TOK_LE */
      result = (val1 <= val2) ? 1 : 0;
      break;
    case 0x9f: /* TOK_GT */
      result = (val1 > val2) ? 1 : 0;
      break;
    case 0x96: /* TOK_ULT (unsigned <) */
      result = ((uint64_t)val1 < (uint64_t)val2) ? 1 : 0;
      break;
    case 0x97: /* TOK_UGE (unsigned >=) */
      result = ((uint64_t)val1 >= (uint64_t)val2) ? 1 : 0;
      break;
    case 0x98: /* TOK_ULE (unsigned <=) */
      result = ((uint64_t)val1 <= (uint64_t)val2) ? 1 : 0;
      break;
    case 0x99: /* TOK_UGT (unsigned >) */
      result = ((uint64_t)val1 > (uint64_t)val2) ? 1 : 0;
      break;
    default:
      /* Unknown condition, don't fold */
      continue;
    }

#ifdef DEBUG_IR_GEN
    printf("OPTIMIZE: Fold CMP+SETIF const (%lld cmp %lld, cond=0x%x) = %d at i=%d\n", (long long)val1, (long long)val2,
           cond, result, i);
#endif

    /* Convert CMP to NOP and SETIF to ASSIGN with constant result.
     * Dead store elimination will remove the NOP. */
    cmp_q->op = TCCIR_OP_NOP;
    setif_q->op = TCCIR_OP_ASSIGN;
    setif_q->src1.r = VT_CONST;
    setif_q->src1.c.i = result;
    setif_q->src1.vr = -1;
    memset(&setif_q->src2, 0, sizeof(setif_q->src2));
    setif_q->src2.vr = -1;
    changes++;
  }

  tcc_free(var_info);

  return changes;
}

/* TMP Constant Propagation
 * After constant folding may create TMP <- #const instructions,
 * propagate these constants to uses of the TMP within the same basic block.
 */
int tcc_ir_tmp_constant_propagation(TCCIRState *ir)
{
  typedef struct
  {
    int valid;
    int64_t value;
  } TmpConstInfo;

  int n = ir->next_instruction_index;
  int changes = 0;
  int max_tmp_pos = 0;
  int i;
  TACQuadruple *q;
  TmpConstInfo *tmp_info;
  uint8_t *block_start;

  if (n == 0)
    return 0;

  /* Find max TMP position */
  for (i = 0; i < n; i++)
  {
    q = &ir->instructions[i];
    if (irop_config[q->op].has_dest && TCCIR_DECODE_VREG_TYPE(q->dest.vr) == TCCIR_VREG_TYPE_TEMP)
    {
      int pos = TCCIR_DECODE_VREG_POSITION(q->dest.vr);
      if (pos > max_tmp_pos)
        max_tmp_pos = pos;
    }
  }

  if (max_tmp_pos == 0)
    return 0;

  tmp_info = tcc_mallocz(sizeof(TmpConstInfo) * (max_tmp_pos + 1));

  /* Basic-block-local propagation must not cross join points.
   * Treat any jump target as a basic block start and clear state there.
   * Otherwise we can incorrectly propagate values from one predecessor
   * into a join block (e.g. short-circuit boolean lowering). */
  block_start = tcc_mallocz(n);
  block_start[0] = 1;
  for (i = 0; i < n; i++)
  {
    q = &ir->instructions[i];
    if (q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_JUMPIF)
    {
      int tgt = q->dest.c.i;
      if (tgt >= 0 && tgt < n)
        block_start[tgt] = 1;
    }
  }

  /* Single pass: track TMP constants and propagate */
  for (i = 0; i < n; i++)
  {
    q = &ir->instructions[i];

    /* Clear at basic block entry (jump targets) to avoid cross-predecessor propagation. */
    if (i != 0 && block_start[i])
    {
      memset(tmp_info, 0, sizeof(TmpConstInfo) * (max_tmp_pos + 1));
    }

    /* Propagate TMP constants to src1 */
    if (irop_config[q->op].has_src1 && TCCIR_DECODE_VREG_TYPE(q->src1.vr) == TCCIR_VREG_TYPE_TEMP)
    {
      int pos = TCCIR_DECODE_VREG_POSITION(q->src1.vr);
      if (pos <= max_tmp_pos && tmp_info[pos].valid)
      {
#ifdef DEBUG_IR_GEN
        printf("OPTIMIZE: TMP const propagate TMP:%d = %lld to src1 at i=%d\n", pos, (long long)tmp_info[pos].value, i);
#endif
        q->src1.r = VT_CONST;
        q->src1.c.i = tmp_info[pos].value;
        q->src1.vr = -1;
        changes++;
      }
    }

    /* Propagate TMP constants to src2 */
    if (irop_config[q->op].has_src2 && TCCIR_DECODE_VREG_TYPE(q->src2.vr) == TCCIR_VREG_TYPE_TEMP)
    {
      int pos = TCCIR_DECODE_VREG_POSITION(q->src2.vr);
      if (pos <= max_tmp_pos && tmp_info[pos].valid)
      {
#ifdef DEBUG_IR_GEN
        printf("OPTIMIZE: TMP const propagate TMP:%d = %lld to src2 at i=%d\n", pos, (long long)tmp_info[pos].value, i);
#endif
        q->src2.r = VT_CONST;
        q->src2.c.i = tmp_info[pos].value;
        q->src2.vr = -1;
        changes++;
      }
    }

    /* Clear all at basic block boundaries */
    if (q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_JUMPIF || q->op == TCCIR_OP_FUNCCALLVOID ||
        q->op == TCCIR_OP_FUNCCALLVAL)
    {
      memset(tmp_info, 0, sizeof(TmpConstInfo) * (max_tmp_pos + 1));
    }

    /* Track TMP <- constant assignments */
    if (q->op == TCCIR_OP_ASSIGN && irop_config[q->op].has_dest &&
        TCCIR_DECODE_VREG_TYPE(q->dest.vr) == TCCIR_VREG_TYPE_TEMP)
    {
      int pos = TCCIR_DECODE_VREG_POSITION(q->dest.vr);
      if (pos <= max_tmp_pos)
      {
        int src_is_const = (q->src1.r & VT_VALMASK) == VT_CONST && !(q->src1.r & VT_SYM);
        if (src_is_const)
        {
          tmp_info[pos].valid = 1;
          tmp_info[pos].value = q->src1.c.i;
#ifdef DEBUG_IR_GEN
          printf("TMP_CONST: Record TMP:%d = %lld at i=%d\n", pos, (long long)q->src1.c.i, i);
#endif
        }
        else
        {
          tmp_info[pos].valid = 0;
        }
      }
    }
    else if (irop_config[q->op].has_dest && TCCIR_DECODE_VREG_TYPE(q->dest.vr) == TCCIR_VREG_TYPE_TEMP)
    {
      /* TMP is defined by non-ASSIGN instruction */
      int pos = TCCIR_DECODE_VREG_POSITION(q->dest.vr);
      if (pos <= max_tmp_pos)
        tmp_info[pos].valid = 0;
    }
  }

  tcc_free(block_start);
  tcc_free(tmp_info);
  return changes;
}

/* Copy Propagation
 * Phase 2: Eliminate redundant copy temporaries
 * Patterns:
 *   - TMP:X <- SRC; ... TMP:X used -> replace uses with SRC
 *   - Eliminate copy chains
 */
int tcc_ir_copy_propagation(TCCIRState *ir)
{
  /* Track ASSIGN sources for TMP vregs.
   * A copy is: TMP:X <- VAR:Y or TMP:X <- PAR:Y (not TMP, not constant)
   * We can replace uses of TMP:X with the source, as long as the source
   * hasn't been redefined between the copy and the use.
   */
  typedef struct
  {
    int valid;     /* Whether this copy is still valid */
    int source_vr; /* Source vreg (-1 if not a copy) */
    SValue source; /* Source of the ASSIGN */
  } CopyInfo;

  int n = ir->next_instruction_index;
  int changes = 0;
  int max_tmp_pos = 0;
  int i, j;
  TACQuadruple *q;
  CopyInfo *copy_info;
  uint8_t *block_start;

  if (n == 0)
    return 0;

  /* Find max TMP position */
  for (i = 0; i < n; i++)
  {
    q = &ir->instructions[i];
    if (irop_config[q->op].has_dest && TCCIR_DECODE_VREG_TYPE(q->dest.vr) == TCCIR_VREG_TYPE_TEMP)
    {
      int pos = TCCIR_DECODE_VREG_POSITION(q->dest.vr);
      if (pos > max_tmp_pos)
        max_tmp_pos = pos;
    }
  }

  if (max_tmp_pos == 0)
    return 0;

  copy_info = tcc_mallocz(sizeof(CopyInfo) * (max_tmp_pos + 1));

  /* Like TMP constant propagation, copy propagation is basic-block-local.
   * Clear at jump targets to avoid propagating copies across join points. */
  block_start = tcc_mallocz(n);
  block_start[0] = 1;
  for (i = 0; i < n; i++)
  {
    q = &ir->instructions[i];
    if (q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_JUMPIF)
    {
      int tgt = q->dest.c.i;
      if (tgt >= 0 && tgt < n)
        block_start[tgt] = 1;
    }
  }

  /* Single pass: process instructions in order, tracking and propagating copies */
  for (i = 0; i < n; i++)
  {
    q = &ir->instructions[i];

    if (i != 0 && block_start[i])
    {
      memset(copy_info, 0, sizeof(CopyInfo) * (max_tmp_pos + 1));
    }

    /* First, propagate copies to uses in this instruction.
     * Important: We DON'T propagate if the use has VT_LVAL because:
     *   - TMP:X <- VAR:Y (copy of pointer value)
     *   - ... TMP:X***DEREF*** (load through the pointer)
     * If we replace TMP:X with VAR:Y (which may have LVAL=load the pointer),
     * then adding another LVAL would mean double-dereference, which is wrong.
     * Only propagate to non-LVAL uses where we just need the pointer value.
     */
    if (irop_config[q->op].has_src1 && TCCIR_DECODE_VREG_TYPE(q->src1.vr) == TCCIR_VREG_TYPE_TEMP)
    {
      int pos = TCCIR_DECODE_VREG_POSITION(q->src1.vr);
      int has_lval = q->src1.r & VT_LVAL;
      if (pos <= max_tmp_pos && copy_info[pos].valid && !has_lval)
      {
#ifdef DEBUG_IR_GEN
        printf("OPTIMIZE: Copy propagate TMP:%d -> vreg:%d at i=%d\n", pos,
               TCCIR_DECODE_VREG_POSITION(copy_info[pos].source_vr), i);
#endif
        q->src1 = copy_info[pos].source;
        changes++;
      }
    }

    if (irop_config[q->op].has_src2 && TCCIR_DECODE_VREG_TYPE(q->src2.vr) == TCCIR_VREG_TYPE_TEMP)
    {
      int pos = TCCIR_DECODE_VREG_POSITION(q->src2.vr);
      int has_lval = q->src2.r & VT_LVAL;
      if (pos <= max_tmp_pos && copy_info[pos].valid && !has_lval)
      {
#ifdef DEBUG_IR_GEN
        printf("OPTIMIZE: Copy propagate TMP:%d -> vreg:%d at i=%d\n", pos,
               TCCIR_DECODE_VREG_POSITION(copy_info[pos].source_vr), i);
#endif
        q->src2 = copy_info[pos].source;
        changes++;
      }
    }

    /* If this instruction defines a VAR/PAR, invalidate any copies from that vreg */
    if (irop_config[q->op].has_dest)
    {
      int dest_type = TCCIR_DECODE_VREG_TYPE(q->dest.vr);
      if (dest_type == TCCIR_VREG_TYPE_VAR || dest_type == TCCIR_VREG_TYPE_PARAM)
      {
        int dest_vr = q->dest.vr;
        for (j = 0; j <= max_tmp_pos; j++)
        {
          if (copy_info[j].valid && copy_info[j].source_vr == dest_vr)
          {
#ifdef DEBUG_IR_GEN
            printf("COPY_PROP: Invalidate TMP:%d (source VAR/PAR:%d redefined) at i=%d\n", j,
                   TCCIR_DECODE_VREG_POSITION(dest_vr), i);
#endif
            copy_info[j].valid = 0;
          }
        }
      }
    }

    /* Clear all copies at basic block boundaries */
    if (q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_JUMPIF || q->op == TCCIR_OP_FUNCCALLVOID ||
        q->op == TCCIR_OP_FUNCCALLVAL)
    {
      memset(copy_info, 0, sizeof(CopyInfo) * (max_tmp_pos + 1));
    }

    /* If this is a copy (ASSIGN TMP <- VAR/PAR), record it */
    if (q->op == TCCIR_OP_ASSIGN && irop_config[q->op].has_dest &&
        TCCIR_DECODE_VREG_TYPE(q->dest.vr) == TCCIR_VREG_TYPE_TEMP)
    {
      int pos = TCCIR_DECODE_VREG_POSITION(q->dest.vr);
      if (pos <= max_tmp_pos)
      {
        int src_valmask = q->src1.r & VT_VALMASK;
        int src_is_const = src_valmask == VT_CONST;
        int src_vreg_type = TCCIR_DECODE_VREG_TYPE(q->src1.vr);

        /* Only allow propagation if source is VAR or PAR (not TMP, not constant) */
        if (!src_is_const && q->src1.vr >= 0 &&
            (src_vreg_type == TCCIR_VREG_TYPE_VAR || src_vreg_type == TCCIR_VREG_TYPE_PARAM))
        {
          copy_info[pos].valid = 1;
          copy_info[pos].source_vr = q->src1.vr;
          copy_info[pos].source = q->src1;
#ifdef DEBUG_IR_GEN
          printf("COPY_PROP: Record TMP:%d <- vreg:%d (type=%d) at i=%d\n", pos, TCCIR_DECODE_VREG_POSITION(q->src1.vr),
                 src_vreg_type, i);
#endif
        }
        else
        {
          /* TMP is assigned something other than a simple VAR/PAR copy */
          copy_info[pos].valid = 0;
        }
      }
    }
    else if (irop_config[q->op].has_dest && TCCIR_DECODE_VREG_TYPE(q->dest.vr) == TCCIR_VREG_TYPE_TEMP)
    {
      /* TMP is defined by a non-ASSIGN instruction - invalidate any copy for it */
      int pos = TCCIR_DECODE_VREG_POSITION(q->dest.vr);
      if (pos <= max_tmp_pos)
        copy_info[pos].valid = 0;
    }
  }

  tcc_free(block_start);
  tcc_free(copy_info);

  return changes;
}

/* Store-Load Forwarding
 * Phase 4: Replace loads from addresses that were just stored to with the stored value
 * Uses conservative basic-block-local alias analysis:
 *   - Stack locals (VT_LOCAL) never alias pointer derefs
 *   - Track base vreg + offset for array accesses
 *   - Clear all pointer-based stores at unknown stores
 *   - Clear all stores at basic block boundaries and function calls
 */
int tcc_ir_store_load_forwarding(TCCIRState *ir)
{
  typedef struct StoreEntry
  {
    int valid;
    int addr_vr;          /* vreg of the address (for pointer stores) */
    int addr_is_local;    /* 1 if this is VT_LOCAL (stack variable) */
    int addr_addrtaken;   /* 1 if address of this local is taken */
    int64_t local_offset; /* offset for VT_LOCAL or base+offset for arrays */
    Sym *local_sym;       /* symbol for VT_LOCAL */
    int stored_value_vr;  /* vreg of the stored value */
    SValue stored_value;  /* full SValue of what was stored */
    int instruction_idx;  /* where the store happened */
    struct StoreEntry *next;
  } StoreEntry;

  int n = ir->next_instruction_index;
  int changes = 0;
  int i;
  TACQuadruple *q;
  StoreEntry *hash_table[128];
  StoreEntry *entries;
  int entry_count;

  if (n == 0)
    return 0;

  memset(hash_table, 0, sizeof(hash_table));
  entries = tcc_malloc(sizeof(StoreEntry) * n);
  entry_count = 0;

#ifdef DEBUG_IR_GEN
  printf("=== STORE-LOAD FORWARDING START ===\n");
#endif

  for (i = 0; i < n; i++)
  {
    q = &ir->instructions[i];

    /* Clear all stores at basic block boundaries and function calls */
    if (q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_JUMPIF || q->op == TCCIR_OP_FUNCCALLVOID ||
        q->op == TCCIR_OP_FUNCCALLVAL || q->op == TCCIR_OP_RETURNVALUE || q->op == TCCIR_OP_RETURNVOID)
    {
      memset(hash_table, 0, sizeof(hash_table));
      entry_count = 0;
      continue;
    }

    /* Process LOAD instructions: check if we can forward from a previous store */
    if (q->op == TCCIR_OP_LOAD)
    {
      /* LOAD: dest <- src1***DEREF***
       * src1 is the address to load from */
      int addr_valmask = q->src1.r & VT_VALMASK;
      int addr_is_local = (addr_valmask == VT_LOCAL);
      int64_t addr_offset = q->src1.c.i;
      Sym *addr_sym = q->src1.sym;
      int addr_vr = q->src1.vr;
      uint32_t h;
      StoreEntry *e;

      /* CONSERVATIVE: Only forward for stack locals */
      if (!addr_is_local)
        continue;

      /* Check if address is taken - if so, skip forwarding (may alias through pointer) */
      if (addr_vr >= 0)
      {
        IRLiveInterval *interval = tcc_ir_get_live_interval(ir, addr_vr);
        if (interval && interval->addrtaken)
          continue;
      }

      /* For VT_LOCAL, hash on symbol pointer and offset */
      h = ((uintptr_t)addr_sym * 31 + (uint32_t)addr_offset * 17) % 128;

      /* Search for matching store */
      for (e = hash_table[h]; e != NULL; e = e->next)
      {
        if (!e->valid || !e->addr_is_local || e->addr_addrtaken)
          continue;

        /* Both are stack locals - match on symbol and offset */
        if (e->local_sym == addr_sym && e->local_offset == addr_offset)
        {
#ifdef DEBUG_IR_GEN
          printf("OPTIMIZE: Store-load forwarding at i=%d from store at i=%d\n", i, e->instruction_idx);
#endif
          /* Replace LOAD with ASSIGN from the stored value */
          q->op = TCCIR_OP_ASSIGN;
          q->src1 = e->stored_value;
          memset(&q->src2, 0, sizeof(q->src2));
          q->src2.vr = -1;
          changes++;
          break;
        }
      }
    }
    /* Process STORE instructions: track them for later forwarding */
    else if (q->op == TCCIR_OP_STORE)
    {
      /* STORE: dest***DEREF*** <- src1
       * dest is the address, src1 is the value to store */
      int addr_valmask = q->dest.r & VT_VALMASK;
      int addr_is_local = (addr_valmask == VT_LOCAL);
      int64_t addr_offset = q->dest.c.i;
      Sym *addr_sym = q->dest.sym;
      int addr_vr = q->dest.vr;
      int addr_addrtaken = 0;
      uint32_t h;
      StoreEntry *new_entry;
      int j;

      /* CONSERVATIVE: Only track stack locals for forwarding */
      if (!addr_is_local)
      {
        /* Non-local store - must invalidate ALL tracked stores since it could alias */
        for (j = 0; j < entry_count; j++)
        {
          if (entries[j].valid && entries[j].addr_addrtaken)
          {
#ifdef DEBUG_IR_GEN
            printf("STORE-LOAD: Invalidate addr-taken local at i=%d due to pointer store at i=%d\n",
                   entries[j].instruction_idx, i);
#endif
            entries[j].valid = 0;
          }
        }
        continue;
      }

      /* Check if address of this local is taken */
      if (addr_vr >= 0)
      {
        IRLiveInterval *interval = tcc_ir_get_live_interval(ir, addr_vr);
        if (interval && interval->addrtaken)
          addr_addrtaken = 1;
      }

      /* For VT_LOCAL, hash on symbol pointer and offset */
      h = ((uintptr_t)addr_sym * 31 + (uint32_t)addr_offset * 17) % 128;

      /* Check if we already have a store to this exact location - if so, invalidate it
       * (the new store overwrites the old one) */
      /* Check if we already have a store to this exact location - if so, invalidate it
       * (the new store overwrites the old one) */
      for (new_entry = hash_table[h]; new_entry != NULL; new_entry = new_entry->next)
      {
        if (new_entry->addr_is_local)
        {
          if (new_entry->local_sym == addr_sym && new_entry->local_offset == addr_offset)
            new_entry->valid = 0;
        }
      }

      /* Record the new store */
      new_entry = &entries[entry_count++];
      new_entry->valid = 1;
      new_entry->addr_is_local = addr_is_local;
      new_entry->addr_addrtaken = addr_addrtaken;
      new_entry->addr_vr = addr_vr;
      new_entry->local_offset = addr_offset;
      new_entry->local_sym = addr_sym;
      new_entry->stored_value = q->src1;
      new_entry->stored_value_vr = q->src1.vr;
      new_entry->instruction_idx = i;
      new_entry->next = hash_table[h];
      hash_table[h] = new_entry;

#ifdef DEBUG_IR_GEN
      printf("STORE-LOAD: Track store at i=%d (local=%d, addrtaken=%d, offset=%lld)\n", i, addr_is_local,
             addr_addrtaken, (long long)addr_offset);
#endif
    }

    /* If this instruction modifies a vreg that's used as a stored value,
     * invalidate those store entries */
    if (irop_config[q->op].has_dest && q->op != TCCIR_OP_STORE && q->op != TCCIR_OP_LOAD)
    {
      int dest_vr = q->dest.vr;
      int j;

      for (j = 0; j < entry_count; j++)
      {
        if (entries[j].valid)
        {
          /* If the stored value vreg is redefined, invalidate */
          if (entries[j].stored_value_vr == dest_vr)
          {
#ifdef DEBUG_IR_GEN
            printf("STORE-LOAD: Invalidate store at i=%d (stored value redefined at i=%d)\n",
                   entries[j].instruction_idx, i);
#endif
            entries[j].valid = 0;
          }
        }
      }
    }
  }

  tcc_free(entries);

#ifdef DEBUG_IR_GEN
  printf("=== STORE-LOAD FORWARDING END: %d changes ===\n", changes);
#endif

  return changes;
}

/* Redundant Store Elimination
 * Phase 4: Remove stores to memory locations that are overwritten before being read
 * (dead stores to memory)
 * CONSERVATIVE: Only handles stack locals whose address is not taken
 */
int tcc_ir_redundant_store_elimination(TCCIRState *ir)
{
  typedef struct StoreInfo
  {
    int addr_vr;
    int addr_is_local;
    int addr_addrtaken;
    int64_t local_offset;
    Sym *local_sym;
    int store_idx;
    int is_dead;
  } StoreInfo;

  int n = ir->next_instruction_index;
  int changes = 0;
  int i, j;
  TACQuadruple *q;
  StoreInfo *stores;
  int store_count;

  if (n == 0)
    return 0;

  stores = tcc_malloc(sizeof(StoreInfo) * n);
  store_count = 0;

#ifdef DEBUG_IR_GEN
  printf("=== REDUNDANT STORE ELIMINATION START ===\n");
#endif

  /* Collect only VT_LOCAL STORE instructions (whose address is not taken) */
  for (i = 0; i < n; i++)
  {
    q = &ir->instructions[i];
    if (q->op == TCCIR_OP_STORE)
    {
      int addr_valmask = q->dest.r & VT_VALMASK;
      int addr_is_local = (addr_valmask == VT_LOCAL);
      int addr_addrtaken = 0;
      int addr_vr = q->dest.vr;

      /* CONSERVATIVE: Only track stack locals */
      if (!addr_is_local)
        continue;

      /* Check if address is taken */
      if (addr_vr >= 0)
      {
        IRLiveInterval *interval = tcc_ir_get_live_interval(ir, addr_vr);
        if (interval && interval->addrtaken)
          addr_addrtaken = 1;
      }

      stores[store_count].addr_is_local = 1;
      stores[store_count].addr_addrtaken = addr_addrtaken;
      stores[store_count].addr_vr = addr_vr;
      stores[store_count].local_offset = q->dest.c.i;
      stores[store_count].local_sym = q->dest.sym;
      stores[store_count].store_idx = i;
      stores[store_count].is_dead = 0;
      store_count++;
    }
  }

  /* For each store, check if it's overwritten before being read */
  for (i = 0; i < store_count; i++)
  {
    int store_idx = stores[i].store_idx;
    int found_read = 0;
    int found_overwrite = 0;

    /* Skip stores to addresses that are taken (could be read through pointer) */
    if (stores[i].addr_addrtaken)
      continue;

    /* Scan forward from this store */
    for (j = store_idx + 1; j < n && !found_read && !found_overwrite; j++)
    {
      q = &ir->instructions[j];

      /* Stop at basic block boundaries - can't track across blocks conservatively */
      if (q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_JUMPIF || q->op == TCCIR_OP_FUNCCALLVOID ||
          q->op == TCCIR_OP_FUNCCALLVAL || q->op == TCCIR_OP_RETURNVALUE || q->op == TCCIR_OP_RETURNVOID)
      {
        break;
      }

      /* Check for LOAD from the same address */
      if (q->op == TCCIR_OP_LOAD)
      {
        int addr_valmask = q->src1.r & VT_VALMASK;
        int addr_is_local = (addr_valmask == VT_LOCAL);

        if (addr_is_local)
        {
          if (stores[i].local_sym == q->src1.sym && stores[i].local_offset == q->src1.c.i)
            found_read = 1;
        }
        /* Non-local load could potentially alias with addr-taken locals
         * but we already skip addr-taken stores above */
      }

      /* Check for STORE to the same address (overwrite) */
      if (q->op == TCCIR_OP_STORE && j != store_idx)
      {
        int addr_valmask = q->dest.r & VT_VALMASK;
        int addr_is_local = (addr_valmask == VT_LOCAL);

        if (addr_is_local)
        {
          if (stores[i].local_sym == q->dest.sym && stores[i].local_offset == q->dest.c.i)
            found_overwrite = 1;
        }
      }
    }

    /* If we found an overwrite without a read in between, the store is dead */
    if (found_overwrite && !found_read)
    {
#ifdef DEBUG_IR_GEN
      printf("OPTIMIZE: Redundant store at i=%d (overwritten without read)\n", store_idx);
#endif
      stores[i].is_dead = 1;
      ir->instructions[store_idx].op = TCCIR_OP_NOP;
      changes++;
    }
  }

  tcc_free(stores);

#ifdef DEBUG_IR_GEN
  printf("=== REDUNDANT STORE ELIMINATION END: %d changes ===\n", changes);
#endif

  return changes;
}

/* Arithmetic Common Subexpression Elimination
 * Phase 3: Eliminate redundant arithmetic computations within basic blocks
 * Handles ADD, SUB, MUL, AND, OR, XOR, SHL, SHR, SAR operations
 */
int tcc_ir_arithmetic_cse(TCCIRState *ir)
{
  typedef struct ArithCSEEntry
  {
    TccIrOp op;
    int src1_vr;
    int src2_vr;
    int64_t src1_const;
    int64_t src2_const;
    uint8_t src1_is_const : 1;
    uint8_t src2_is_const : 1;
    int result_vr;
    int instruction_idx;
    struct ArithCSEEntry *next;
  } ArithCSEEntry;

  int n;
  int changes;
  int i, j;
  TACQuadruple *q;
  ArithCSEEntry *hash_table[256];
  ArithCSEEntry *entries;
  int entry_count;

  n = ir->next_instruction_index;
  changes = 0;

  if (n == 0)
    return 0;

  memset(hash_table, 0, sizeof(hash_table));
  entries = tcc_malloc(sizeof(ArithCSEEntry) * n);
  entry_count = 0;

  for (i = 0; i < n; i++)
  {
    int src1_is_const, src2_is_const;
    int64_t src1_const, src2_const;
    int src1_vr, src2_vr;
    uint32_t h;
    int found;
    ArithCSEEntry *e;

    q = &ir->instructions[i];

    if (q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_JUMPIF || q->op == TCCIR_OP_FUNCCALLVOID ||
        q->op == TCCIR_OP_FUNCCALLVAL || q->op == TCCIR_OP_RETURNVALUE || q->op == TCCIR_OP_RETURNVOID)
    {
      memset(hash_table, 0, sizeof(hash_table));
      entry_count = 0;
      continue;
    }

    if (q->op != TCCIR_OP_ADD && q->op != TCCIR_OP_SUB && q->op != TCCIR_OP_MUL && q->op != TCCIR_OP_AND &&
        q->op != TCCIR_OP_OR && q->op != TCCIR_OP_XOR && q->op != TCCIR_OP_SHL && q->op != TCCIR_OP_SHR &&
        q->op != TCCIR_OP_SAR)
      continue;

    src1_is_const = (q->src1.r & VT_VALMASK) == VT_CONST && !(q->src1.r & VT_SYM);
    src2_is_const = (q->src2.r & VT_VALMASK) == VT_CONST && !(q->src2.r & VT_SYM);
    src1_const = src1_is_const ? q->src1.c.i : 0;
    src2_const = src2_is_const ? q->src2.c.i : 0;
    src1_vr = q->src1.vr;
    src2_vr = q->src2.vr;

    h = (uint32_t)q->op * 31;
    if (src1_is_const)
      h += (uint32_t)src1_const * 17;
    else
      h += (uint32_t)src1_vr * 17;
    if (src2_is_const)
      h += (uint32_t)src2_const * 13;
    else
      h += (uint32_t)src2_vr * 13;
    h = h % 256;

    found = 0;
    for (e = hash_table[h]; e != NULL; e = e->next)
    {
      int is_commutative;
      int match1, match2;

      if (e->op != q->op)
        continue;

      if (e->src1_is_const == src1_is_const && e->src2_is_const == src2_is_const)
      {
        match1 = e->src1_is_const ? (e->src1_const == src1_const) : (e->src1_vr == src1_vr);
        match2 = e->src2_is_const ? (e->src2_const == src2_const) : (e->src2_vr == src2_vr);
        if (match1 && match2)
        {
#ifdef DEBUG_IR_GEN
          printf("OPTIMIZE: Arithmetic CSE %s at %d same as %d -> ASSIGN\n", tcc_ir_get_op_name(q->op), i,
                 e->instruction_idx);
#endif
          q->op = TCCIR_OP_ASSIGN;
          /* Create a reference to the previous instruction's dest vreg.
           * IMPORTANT: Only copy vr and type - do NOT copy VT_LVAL or other flags
           * that might cause incorrect dereferencing. The dest vreg holds a VALUE,
           * not an address to be dereferenced. */
          q->src1.vr = ir->instructions[e->instruction_idx].dest.vr;
          q->src1.type = ir->instructions[e->instruction_idx].dest.type;
          q->src1.r = 0; /* No flags - this is a simple vreg read */
          q->src1.c.i = 0;
          memset(&q->src2, 0, sizeof(q->src2));
          q->src2.vr = -1;
          changes++;
          found = 1;
          break;
        }
      }

      is_commutative = (q->op == TCCIR_OP_ADD || q->op == TCCIR_OP_MUL || q->op == TCCIR_OP_AND ||
                        q->op == TCCIR_OP_OR || q->op == TCCIR_OP_XOR);

      if (is_commutative && e->src1_is_const == src2_is_const && e->src2_is_const == src1_is_const)
      {
        match1 = e->src1_is_const ? (e->src1_const == src2_const) : (e->src1_vr == src2_vr);
        match2 = e->src2_is_const ? (e->src2_const == src1_const) : (e->src2_vr == src1_vr);
        if (match1 && match2)
        {
#ifdef DEBUG_IR_GEN
          printf("OPTIMIZE: Arithmetic CSE %s at %d same as %d (commutative) -> ASSIGN\n", tcc_ir_get_op_name(q->op), i,
                 e->instruction_idx);
#endif
          q->op = TCCIR_OP_ASSIGN;
          /* Create a reference to the previous instruction's dest vreg.
           * IMPORTANT: Only copy vr and type - do NOT copy VT_LVAL or other flags
           * that might cause incorrect dereferencing. The dest vreg holds a VALUE,
           * not an address to be dereferenced. */
          q->src1.vr = ir->instructions[e->instruction_idx].dest.vr;
          q->src1.type = ir->instructions[e->instruction_idx].dest.type;
          q->src1.r = 0; /* No flags - this is a simple vreg read */
          q->src1.c.i = 0;
          memset(&q->src2, 0, sizeof(q->src2));
          q->src2.vr = -1;
          changes++;
          found = 1;
          break;
        }
      }
    }

    if (!found && entry_count < n)
    {
      ArithCSEEntry *new_entry;
      new_entry = &entries[entry_count++];
      new_entry->op = q->op;
      new_entry->src1_vr = src1_vr;
      new_entry->src2_vr = src2_vr;
      new_entry->src1_const = src1_const;
      new_entry->src2_const = src2_const;
      new_entry->src1_is_const = src1_is_const;
      new_entry->src2_is_const = src2_is_const;
      new_entry->result_vr = q->dest.vr;
      new_entry->instruction_idx = i;
      new_entry->next = hash_table[h];
      hash_table[h] = new_entry;
    }

    if (irop_config[q->op].has_dest)
    {
      int dest_vr;
      dest_vr = q->dest.vr;
      for (j = 0; j < 256; j++)
      {
        ArithCSEEntry **ep;
        ep = &hash_table[j];
        while (*ep)
        {
          e = *ep;
          if ((!e->src1_is_const && e->src1_vr == dest_vr) || (!e->src2_is_const && e->src2_vr == dest_vr))
            *ep = e->next;
          else
            ep = &e->next;
        }
      }
    }
  }

  tcc_free(entries);
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
      int target_ir = q->dest.c.i;
      /* Skip unpatched jumps (target is -1 or truly out of range)
       * Note: target_ir == ir->next_instruction_index is valid (epilogue) */
      if (target_ir < 0 || target_ir > ir->next_instruction_index)
        continue;
      const int instruction_address = ir_to_code_mapping[i];
      const int target_address = ir_to_code_mapping[target_ir];
      tcc_gen_machine_backpatch_jump(instruction_address, target_address);
    }
  }
}

void tcc_ir_put_soft_call(TCCIRState *ir, TccIrOp op, SValue *src1, SValue *src2, SValue *dest)
{
  SValue param;
  Sym *sym;
  const int call_id = ir ? ir->next_call_id++ : 0;
  TACQuadruple q = {
      .op = op,
  };
  const char *func_name = NULL;

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
  memset(&param, 0, sizeof(SValue));
  if (irop_config[q.op].has_src1)
  {
    param.c.i = TCCIR_ENCODE_PARAM(call_id, 0);
    tcc_ir_put(ir, TCCIR_OP_FUNCPARAMVAL, src1, &param, NULL);
  }
  if (irop_config[q.op].has_src2)
  {
    param.c.i = TCCIR_ENCODE_PARAM(call_id, 1);
    tcc_ir_put(ir, TCCIR_OP_FUNCPARAMVAL, src2, &param, NULL);
  }
  sym = external_global_sym(tok_alloc_const(func_name), &func_old_type);
  param.r = VT_CONST | VT_SYM;
  param.sym = sym;
  param.c.i = 0;

  if (irop_config[q.op].has_dest)
  {
    SValue call_id_sv = tcc_ir_svalue_call_id(call_id);
    tcc_ir_put(ir, TCCIR_OP_FUNCCALLVAL, &param, &call_id_sv, dest);
  }
  else
  {
    SValue call_id_sv = tcc_ir_svalue_call_id(call_id);
    tcc_ir_put(ir, TCCIR_OP_FUNCCALLVOID, &param, &call_id_sv, NULL);
  }
}

static bool tcc_ir_put_soft_call_fpu_if_needed(TCCIRState *ir, TccIrOp op, SValue *src1, SValue *src2, SValue *dest)
{
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

#ifdef CONFIG_TCC_ASM
static void tcc_ir_codegen_inline_asm(TCCIRState *ir, const TACQuadruple *q)
{
  if (!ir || !q)
    return;
  const int id = (int)q->src1.c.i;
  if (id < 0 || id >= ir->inline_asm_count)
    tcc_error("IR: invalid inline asm id");

  TCCIRInlineAsm *ia = &ir->inline_asms[id];
  if (!ia->asm_str)
    tcc_error("IR: inline asm payload missing");

  const int nb_operands = ia->nb_operands;
  const int nb_labels = ia->nb_labels;
  if (nb_operands < 0 || nb_operands > MAX_ASM_OPERANDS || nb_operands + nb_labels > MAX_ASM_OPERANDS)
    tcc_error("IR: invalid asm operand count");

  ASMOperand ops[MAX_ASM_OPERANDS];
  SValue vals[MAX_ASM_OPERANDS];
  memset(ops, 0, sizeof(ops));
  memset(vals, 0, sizeof(vals));

  memcpy(ops, ia->operands, sizeof(ASMOperand) * (nb_operands + nb_labels));
  for (int i = 0; i < nb_operands; ++i)
  {
    vals[i] = ia->values[i];
    tcc_ir_fill_registers(ir, &vals[i]);
    ops[i].vt = &vals[i];
  }
  for (int i = nb_operands; i < nb_operands + nb_labels; ++i)
    ops[i].vt = NULL;

  uint8_t clobber_regs[NB_ASM_REGS];
  memcpy(clobber_regs, ia->clobber_regs, sizeof(clobber_regs));

  tcc_asm_emit_inline(ops, nb_operands, ia->nb_outputs, nb_labels, clobber_regs, ia->asm_str, ia->asm_len,
                      ia->must_subst);
}
#endif

void tcc_ir_generate_code(TCCIRState *ir)
{
  TACQuadruple *q;
  int drop_return_value = 0;

  /* `&&label` stores label positions as IR indices BEFORE DCE/compaction.
   * Build a mapping for original indices, not just the compacted array indices.
   */
  int max_orig_index = -1;
  for (int i = 0; i < ir->next_instruction_index; i++)
  {
    if (ir->instructions[i].orig_index > max_orig_index)
      max_orig_index = ir->instructions[i].orig_index;
  }
  if (max_orig_index < 0)
    max_orig_index = 0;

  /* +1 to include epilogue when needed.
   * Keep this mapping available after codegen (e.g. for &&label). */
  if (ir->ir_to_code_mapping)
  {
    tcc_free(ir->ir_to_code_mapping);
    ir->ir_to_code_mapping = NULL;
    ir->ir_to_code_mapping_size = 0;
  }
  ir->ir_to_code_mapping_size = ir->next_instruction_index + 1;
  ir->ir_to_code_mapping = tcc_mallocz(sizeof(uint32_t) * ir->ir_to_code_mapping_size);
  uint32_t *ir_to_code_mapping = ir->ir_to_code_mapping;

  if (ir->orig_ir_to_code_mapping)
  {
    tcc_free(ir->orig_ir_to_code_mapping);
    ir->orig_ir_to_code_mapping = NULL;
    ir->orig_ir_to_code_mapping_size = 0;
  }
  /* +1 extra slot for a synthetic epilogue mapping.
   * Use 0xFFFFFFFF sentinel to distinguish "unmapped" from offset 0. */
  ir->orig_ir_to_code_mapping_size = max_orig_index + 2;
  ir->orig_ir_to_code_mapping = tcc_malloc(sizeof(uint32_t) * ir->orig_ir_to_code_mapping_size);
  uint32_t *orig_ir_to_code_mapping = ir->orig_ir_to_code_mapping;
  memset(orig_ir_to_code_mapping, 0xFF, sizeof(uint32_t) * ir->orig_ir_to_code_mapping_size);
  /* Track addresses of return jumps for later backpatching to epilogue */
  int *return_jump_addrs = tcc_malloc(sizeof(int) * ir->next_instruction_index);
  int num_return_jumps = 0;

  /* Clear spill cache at function start */
  tcc_ir_spill_cache_clear(&ir->spill_cache);

  /* Some peephole optimizations (LOAD/ASSIGN -> RETURNVALUE in R0, and skipping
   * RETURNVALUE moves) are only valid when RETURNVALUE is reached by straight-line
   * fallthrough from the immediately preceding instruction.
   *
   * If RETURNVALUE is a jump target (a control-flow merge), those peepholes can
   * become incorrect: the preceding instruction might not execute on all paths,
   * leaving the return value in a non-return register.
   *
   * Track which IR instruction indices are jump targets to guard these peepholes.
   */
  uint8_t *has_incoming_jump = tcc_mallocz(ir->next_instruction_index ? ir->next_instruction_index : 1);
  for (int i = 0; i < ir->next_instruction_index; ++i)
  {
    TACQuadruple *p = &ir->instructions[i];
    if (p->op == TCCIR_OP_JUMP || p->op == TCCIR_OP_JUMPIF)
    {
      int target = p->dest.c.i;
      if (target >= 0 && target < ir->next_instruction_index)
        has_incoming_jump[target] = 1;
    }
  }

  /* Reserve outgoing call stack args area at the very bottom of the frame.
   * This ensures prepared-call stack args are at call-time SP.
   */
  if (ir->call_outgoing_size > 0)
  {
    loc -= ir->call_outgoing_size;
    ir->call_outgoing_base = loc;
  }

  // generate prolog
  int stack_size = (-loc + 7) & ~7; // align to 8 bytes
  THGEN_DUMP("DEBUG prolog: loc=%d stack_size=%d\n", loc, stack_size);
  int ind_before_prolog = ind;
  tcc_gen_machine_prolog(ir->leaffunc, ir->ls.dirty_registers, stack_size);

  for (int i = 0; i < ir->next_instruction_index; i++)
  {
    drop_return_value = 0;
    q = &ir->instructions[i];

    /* Default: no extra scratch constraints for this instruction. */
    ir->codegen_materialize_scratch_flags = 0;

    /* Track current instruction for scratch register allocation */
    ir->codegen_instruction_idx = i;

    int ind_before = ind;

    ir_to_code_mapping[i] = ind;

    if (q->orig_index >= 0 && q->orig_index < ir->orig_ir_to_code_mapping_size)
      orig_ir_to_code_mapping[q->orig_index] = ind;

    // emit debug line info for this IR instruction AFTER recording ind
    tcc_debug_line_num(tcc_state, q->line_num);

    /* Fill in register allocations before deciding on materialization */
    if (irop_config[q->op].has_src1 == 1)
    {
      tcc_ir_fill_registers(ir, &q->src1);
    }
    if (irop_config[q->op].has_src2 == 1)
    {
      tcc_ir_fill_registers(ir, &q->src2);
    }
    if (irop_config[q->op].has_dest == 1)
    {
      tcc_ir_fill_registers(ir, &q->dest);
    }

    bool need_src1_value = false;
    bool need_src2_value = false;
    bool need_dest_value = false;
    bool need_src1_addr = false;
    bool need_src2_addr = false;
    bool need_dest_addr = false;
    bool need_src1_in_reg = false; /* Operand must be in register, not immediate */
    bool need_src2_in_reg = false;

    switch (q->op)
    {
    case TCCIR_OP_MUL:
    case TCCIR_OP_DIV:
    case TCCIR_OP_UDIV:
    case TCCIR_OP_IMOD:
    case TCCIR_OP_UMOD:
    case TCCIR_OP_UMULL:
      /* These operations require register-only operands (no immediate forms) */
      need_src1_value = true;
      need_src2_value = true;
      need_dest_value = true;
      need_src1_in_reg = true;
      need_src2_in_reg = true;
      break;
    case TCCIR_OP_ADD:
    case TCCIR_OP_SUB:
    case TCCIR_OP_AND:
    case TCCIR_OP_OR:
    case TCCIR_OP_XOR:
    case TCCIR_OP_SHL:
    case TCCIR_OP_SHR:
    case TCCIR_OP_SAR:
    case TCCIR_OP_ADC_GEN:
    case TCCIR_OP_ADC_USE:
    case TCCIR_OP_BOOL_OR:
    case TCCIR_OP_BOOL_AND:
      need_src1_value = true;
      need_src2_value = true;
      need_dest_value = true;
      break;
    case TCCIR_OP_CMP:
      need_src1_value = true;
      need_src2_value = true;
      break;
    case TCCIR_OP_TEST_ZERO:
      need_src1_value = true;
      break;
    case TCCIR_OP_FADD:
    case TCCIR_OP_FSUB:
    case TCCIR_OP_FMUL:
    case TCCIR_OP_FDIV:
      need_src1_value = true;
      need_src2_value = true;
      need_dest_value = true;
      break;
    case TCCIR_OP_FNEG:
    case TCCIR_OP_CVT_FTOF:
    case TCCIR_OP_CVT_ITOF:
    case TCCIR_OP_CVT_FTOI:
      need_src1_value = true;
      need_dest_value = true;
      break;
    case TCCIR_OP_LOAD:
      need_src1_addr = true;
      need_dest_value = true;
      break;
    case TCCIR_OP_STORE:
      need_src1_value = true;
      need_dest_addr = true;
      break;
    case TCCIR_OP_ASSIGN:
      need_src1_value = true;
      need_dest_value = true;
      break;
    case TCCIR_OP_LEA:
      need_src1_addr = true; /* We need the address of src1, not its value */
      need_dest_value = true;
      break;
    case TCCIR_OP_IJUMP:
      need_src1_value = true;
      break;
    case TCCIR_OP_SETIF:
      need_dest_value = true;
      break;
    case TCCIR_OP_RETURNVALUE:
      need_src1_value = true;
      break;
    case TCCIR_OP_FUNCPARAMVAL:
      /* FUNCPARAMVAL is a marker op only.
       * Argument placement is handled when we reach the owning FUNCCALL*,
       * so do not materialize anything here (would just emit dead loads).
       */
      break;
    case TCCIR_OP_FUNCCALLVAL:
      need_dest_value = true;
      /* fall through */
    case TCCIR_OP_FUNCCALLVOID:
    {
      need_src1_value = true;
      break;
    }
    case TCCIR_OP_VLA_ALLOC:
      need_src1_value = true;
      break;
    default:
      break;
    }

    TCCMaterializedValue mat_src1 = {0};
    TCCMaterializedValue mat_src2 = {0};
    TCCMaterializedAddr mat_src1_addr = {0};
    TCCMaterializedAddr mat_src2_addr = {0};
    TCCMaterializedAddr mat_dest_addr = {0};
    TCCMaterializedDest mat_dest = {0};
#if TCC_DUMP_THUMB_GEN
    THGEN_DUMP("IR[%d] orig=%d line=%d ind=0x%x ", i, q->orig_index, q->line_num, ind);
    tcc_dump_quadruple_to(stderr, q, i);
#endif
    if (need_src1_value)
      tcc_ir_materialize_value(ir, &q->src1, &mat_src1);
    if (need_src1_addr)
      tcc_ir_materialize_addr(ir, &q->src1, &mat_src1_addr, q->dest.pr0);
    if (need_src2_value)
      tcc_ir_materialize_value(ir, &q->src2, &mat_src2);
    if (need_src2_addr)
      tcc_ir_materialize_addr(ir, &q->src2, &mat_src2_addr, q->dest.pr0);
    if (need_dest_value)
      tcc_ir_materialize_dest(ir, &q->dest, &mat_dest);
    if (need_dest_addr)
      tcc_ir_materialize_addr(ir, &q->dest, &mat_dest_addr, q->dest.pr0);

    /* For operations that require register-only operands (MUL, DIV, MOD),
     * ensure constants/comparisons are loaded into registers. This replaces
     * backend-level thumb_materialize_binop32_sources() with IR-level handling. */
    TCCMaterializedValue mat_src1_reg = {0};
    TCCMaterializedValue mat_src2_reg = {0};
    if (need_src1_in_reg)
      tcc_ir_materialize_const_to_reg(ir, &q->src1, &mat_src1_reg);
    if (need_src2_in_reg)
      tcc_ir_materialize_const_to_reg(ir, &q->src2, &mat_src2_reg);

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
    case TCCIR_OP_IMOD:
    case TCCIR_OP_UMOD:
    case TCCIR_OP_SAR:
    case TCCIR_OP_UMULL:
    case TCCIR_OP_ADC_GEN:
    case TCCIR_OP_ADC_USE:
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
    {
      /* Peephole: if next instruction is RETURNVALUE using this LOAD's result,
       * load directly to R0 instead of the allocated register */
      const TACQuadruple *ir_next = (i + 1 < ir->next_instruction_index) ? &ir->instructions[i + 1] : NULL;
      if (ir_next && ir_next->op == TCCIR_OP_RETURNVALUE && ir_next->src1.vr == q->dest.vr && !has_incoming_jump[i + 1])
      {
        q->dest.pr0 = REG_IRET; /* R0 */
        if (tcc_ir_is_64bit_type(q->dest.type.t))
        {
          q->dest.pr1 = REG_IRE2; /* R1 */
        }
      }
      tcc_gen_machine_load_op(q);
      break;
    }
    case TCCIR_OP_STORE:
      tcc_gen_machine_store_op(q);
      break;
    case TCCIR_OP_RETURNVALUE:
    {
      /* Peephole: if previous instruction was LOAD/ASSIGN that already loaded to R0,
       * skip the return value copy */
      const TACQuadruple *ir_prev = (i > 0) ? &ir->instructions[i - 1] : NULL;
      THGEN_DUMP("DEBUG codegen RETURNVALUE: i=%d src1.r=0x%x src1.vr=%d src1.c.i=%lld src1.pr0=%d prev.op=%d "
                 "prev.dest.vr=%d prev.dest.pr0=%d\n",
                 i, q->src1.r, q->src1.vr, (long long)q->src1.c.i, q->src1.pr0, ir_prev ? ir_prev->op : -1,
                 ir_prev ? ir_prev->dest.vr : -2, ir_prev ? ir_prev->dest.pr0 : -2);
      if (!has_incoming_jump[i] && ir_prev && (ir_prev->op == TCCIR_OP_LOAD || ir_prev->op == TCCIR_OP_ASSIGN) &&
          ir_prev->dest.vr == q->src1.vr && ir_prev->dest.pr0 == REG_IRET /* R0 */)
      {
        THGEN_DUMP("DEBUG codegen RETURNVALUE: SKIP due to peephole\n");
        /* Value is already in R0, no need to generate return value op */
        /* Just fall through to RETURNVOID which handles the jump */
      }
      else
      {
        tcc_gen_machine_return_value_op(q);
      }
    }
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
    {
      /* Peephole: if next instruction is RETURNVALUE using this ASSIGN's dest,
       * assign directly to R0 to avoid an extra move */
      const TACQuadruple *ir_next = (i + 1 < ir->next_instruction_index) ? &ir->instructions[i + 1] : NULL;
      if (ir_next && ir_next->op == TCCIR_OP_RETURNVALUE && ir_next->src1.vr == q->dest.vr && !has_incoming_jump[i + 1])
      {
        q->dest.pr0 = REG_IRET; /* R0 */
        if (tcc_ir_is_64bit_type(q->dest.type.t))
          q->dest.pr1 = REG_IRE2; /* R1 */
      }
      tcc_gen_machine_assign_op(q);
      break;
    }
    case TCCIR_OP_LEA:
      /* Load Effective Address: compute address of src1 into dest */
      tcc_gen_machine_lea_op(q);
      break;
    case TCCIR_OP_FUNCPARAMVAL:
    {
      tcc_gen_machine_func_parameter_op(q);
      break;
    }
    case TCCIR_OP_JUMP:
      tcc_gen_machine_jump_op(q);
      /* Clear spill cache at branch - value may come from different path */
      tcc_ir_spill_cache_clear(&ir->spill_cache);
      break;
    case TCCIR_OP_JUMPIF:
      tcc_gen_machine_conditional_jump_op(q);
      /* Clear spill cache at conditional branch - target may have different values */
      tcc_ir_spill_cache_clear(&ir->spill_cache);
      break;
    case TCCIR_OP_IJUMP:
      tcc_gen_machine_indirect_jump_op(q);
      tcc_ir_spill_cache_clear(&ir->spill_cache);
      break;
    case TCCIR_OP_SETIF:
      tcc_gen_machine_setif_op(q);
      break;
    case TCCIR_OP_BOOL_OR:
    case TCCIR_OP_BOOL_AND:
      tcc_gen_machine_bool_op(q);
      break;
    case TCCIR_OP_FUNCPARAMVOID:
      /* Create call site for void calls (no parameters) */
      tcc_gen_machine_func_parameter_op(q);
      break;
    case TCCIR_OP_VLA_ALLOC:
    case TCCIR_OP_VLA_SP_SAVE:
    case TCCIR_OP_VLA_SP_RESTORE:
      tcc_gen_machine_vla_op(q);
      break;
    case TCCIR_OP_FUNCCALLVOID:
      drop_return_value = 1;
      /* fall through */
    case TCCIR_OP_FUNCCALLVAL:
    {
      tcc_gen_machine_func_call_op(q, drop_return_value, ir, i);
      /* Clear spill cache after function call - callee may have modified memory */
      tcc_ir_spill_cache_clear(&ir->spill_cache);
      break;
    }
    case TCCIR_OP_NOP:
      /* No operation - skip silently */
      break;
    case TCCIR_OP_ASM_INPUT:
    case TCCIR_OP_ASM_OUTPUT:
      /* Marker ops only: regalloc/liveness uses them, codegen emits nothing. */
      break;
    case TCCIR_OP_INLINE_ASM:
    {
#ifdef CONFIG_TCC_ASM
      tcc_ir_codegen_inline_asm(ir, q);
      /* Inline asm may clobber registers/memory: treat as a full barrier. */
      tcc_ir_spill_cache_clear(&ir->spill_cache);
#else
      tcc_error("inline asm not supported");
#endif
      break;
    }
    default:
    {
      printf("Unsupported operation in tcc_generate_code: %s\n", tcc_ir_get_op_name(q->op));
      if (ir->ir_to_code_mapping)
      {
        tcc_free(ir->ir_to_code_mapping);
        ir->ir_to_code_mapping = NULL;
        ir->ir_to_code_mapping_size = 0;
      }
      tcc_free(return_jump_addrs);
      exit(1);
    }
    };

    /* Clean up scratch register state at end of each IR instruction.
     * This restores any pushed scratch registers and resets the global exclude mask. */
    tcc_gen_machine_end_instruction();

    tcc_ir_release_materialized_addr(&q->dest, &mat_dest_addr);
    tcc_ir_storeback_materialized_dest(q, &mat_dest);
    tcc_ir_release_materialized_addr(&q->src2, &mat_src2_addr);
    tcc_ir_release_materialized_value(&q->src2, &mat_src2_reg);
    tcc_ir_release_materialized_value(&q->src2, &mat_src2);
    tcc_ir_release_materialized_value(&q->src1, &mat_src1_reg);
    tcc_ir_release_materialized_addr(&q->src1, &mat_src1_addr);
    tcc_ir_release_materialized_value(&q->src1, &mat_src1);

    /* Disabled: hex dump of emitted bytes
    if (TCC_DUMP_THUMB_GEN && TCC_DUMP_THUMB_GEN_SPAN)
    {
      uint32_t ind_after = ind;
      THGEN_DUMP("  ; emitted %u bytes (0x%x -> 0x%x)\n", (unsigned)(ind_after - ind_before), ind_before, ind_after);
      tcc_dump_thumb_generated_span((uint32_t)ind_before, (uint32_t)ind_after);
    }
    */
  }

  ir_to_code_mapping[ir->next_instruction_index] = ind;
  orig_ir_to_code_mapping[ir->orig_ir_to_code_mapping_size - 1] = ind;

  /* Fill gaps for removed original indices: map them to the next reachable
   * emitted code address (or epilogue). This keeps &&label stable even if the
   * instruction at the exact original index was optimized away. */
  {
    uint32_t last = orig_ir_to_code_mapping[ir->orig_ir_to_code_mapping_size - 1];
    for (int k = ir->orig_ir_to_code_mapping_size - 2; k >= 0; --k)
    {
      if (orig_ir_to_code_mapping[k] == 0xFFFFFFFFu)
        orig_ir_to_code_mapping[k] = last;
      else
        last = orig_ir_to_code_mapping[k];
    }
  }

  tcc_gen_machine_epilog(ir->leaffunc);
  tcc_ir_backpatch_jumps(ir, ir_to_code_mapping);

  /* Backpatch return jumps to point to epilogue */
  int epilogue_addr = ir_to_code_mapping[ir->next_instruction_index];
  for (int i = 0; i < num_return_jumps; i++)
  {
    tcc_gen_machine_backpatch_jump(return_jump_addrs[i], epilogue_addr);
  }

  tcc_free(return_jump_addrs);
  tcc_free(has_incoming_jump);
}

void tcc_ir_print_vreg(int vreg)
{
  printf("VReg %s:%d", tcc_ir_get_vreg_type_string(vreg), TCCIR_DECODE_VREG_POSITION(vreg));
}

void print_svalue_short(SValue *sv)
{
  int val_loc = sv->r & VT_VALMASK;

  /* XXX: probably show ignored vregs in a special way */
  switch (val_loc)
  {
  case VT_CONST:
    if (sv->r & VT_SYM)
    {
      printf("GlobalSym(%d)", sv->sym->v);
      if (sv->c.i != 0)
        printf("+%d", (int)sv->c.i);
      if (sv->r & VT_LVAL)
        printf("***DEREF***");
    }
    else
    {
      /* Check if this is a long long constant */
      if ((sv->type.t & VT_BTYPE) == VT_LLONG)
        printf("#%lld", (long long)sv->c.i);
      else
        printf("#%d", (int)sv->c.i);
    }
    break;
  case VT_LLOCAL:
    /* VT_LLOCAL with VT_LVAL: spilled pointer needing double dereference */
    if (sv->pr0 != PREG_NONE && (sv->pr0 & PREG_SPILLED))
      printf(SPILL_MARK_BEGIN "SpillLoc[%d]***DEREF***" SPILL_MARK_END, sv->c.i);
    else
      printf("VT_LLOCAL (cval=%d)", sv->c.i);
    break;
  // case VT_LOCAL: printf("VReg%d[stack_offset=%d]", sv->vreg, sv->c.i); break;
  case VT_LOCAL:
    if (sv->pr0 != PREG_NONE)
    { /* already register-allocated? */
      if (sv->pr0 & PREG_SPILLED)
        printf(SPILL_MARK_BEGIN "SpillLoc[%d]" SPILL_MARK_END, sv->c.i);
      else
      {
        if (!(sv->r & VT_LVAL))
          printf("&"); /* address-of */
        printf("R%d", sv->pr0);
      }
    }
    else if (sv->vr != -1)
    { /* not reg-alloced, but vreg'ed? */
      if (!(sv->r & VT_LVAL))
        printf("&"); /* address-of: we want the address, not the value */
      tcc_ir_print_vreg(sv->vr);
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
    if (sv->pr0 == PREG_NONE)
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
      if (tcc_ir_operand_needs_dereference(sv))
        printf("***DEREF***");
    }
    else
    {
      if (sv->pr0 & PREG_SPILLED)
        printf(SPILL_MARK_BEGIN "SpillLoc[%d]" SPILL_MARK_END, sv->c.i);
      else
        printf("R%d", sv->pr0);
      if (tcc_ir_operand_needs_dereference(sv))
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
    printf("%s%d[call_%d] ", tcc_ir_get_op_name(op), TCCIR_DECODE_PARAM_IDX(q->src2.c.i),
           TCCIR_DECODE_CALL_ID(q->src2.c.i));
    break;
  case TCCIR_OP_JUMP:
  case TCCIR_OP_JUMPIF:
    printf("JMP to %d ", q->dest.c.i);
    break;
  case TCCIR_OP_IJUMP:
    printf("IJMP ");
    print_svalue_short(&q->src1);
    printf(" ");
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
  else if (op == TCCIR_OP_LOAD)
    printf(" [LOAD]");
  else if (op == TCCIR_OP_ASSIGN)
    printf(" [ASSIGN]");
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
    /* Only drop return values that are assigned to temporaries.
     * If coalescing redirected the dest to a VAR, the value IS used
     * and should not be dropped. */
    if (TCCIR_DECODE_VREG_TYPE(last_instr->dest.vr) == TCCIR_VREG_TYPE_TEMP)
    {
      if (tcc_is_vreg_valid(ir, last_instr->dest.vr))
      {
        IRLiveInterval *interval = tcc_ir_get_live_interval(ir, last_instr->dest.vr);
        interval->start = INTERVAL_NOT_STARTED;
        interval->end = 0;
      }
      last_instr->op = TCCIR_OP_FUNCCALLVOID;
      last_instr->dest.vr = -1;
      /* NOTE: Do NOT clear src1.vr - it contains the function address to call! */
    }
  }
}

void tcc_ir_backpatch(TCCIRState *ir, int t, int target_address)
{
  SValue *cur;
  int next;
  if (t < 0)
    return; /* -1 means no chain */

  printf("\n=== BACKPATCH CALLED: t=%d target=%d ===\n", t, target_address);

  while (t >= 0 && t < ir->next_instruction_index)
  {
    TccIrOp op = ir->instructions[t].op;
    printf("  Patching instr[%d]: op=%d", t, op);

    /* Check if this instruction is actually a jump */
    if (op != TCCIR_OP_JUMP && op != TCCIR_OP_JUMPIF)
    {
      printf(" ERROR: Not a jump instruction!\n");
      printf("  Dumping first 10 instructions:\n");
      for (int i = 0; i < 10 && i < ir->next_instruction_index; i++)
      {
        printf("    [%d] op=%d dest.c.i=%d\n", i, ir->instructions[i].op, (int)ir->instructions[i].dest.c.i);
      }
      break; /* Don't corrupt non-jump instructions */
    }

    cur = &ir->instructions[t].dest;
    next = cur->c.i;
    printf(" (next=%d) -> setting to %d\n", next, target_address);
    cur->c.i = target_address;

    /* Chain ends when next is -1 (sentinel), out of range, or already patched */
    if (next < 0 || next >= ir->next_instruction_index || next == target_address)
      break;
    t = next;
  }
  printf("=== BACKPATCH DONE ===\n\n");
}

void tcc_ir_backpatch_to_here(TCCIRState *ir, int t)
{
  if (!ir)
    return;
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
    /* Validate condition is a valid comparison token */
    src.c.i = cond;
    dest.vr = -1;
    dest.c.i = t;
    t = tcc_ir_put(ir, TCCIR_OP_JUMPIF, &src, NULL, &dest);

    /* Handle pending jump chains - merge with the appropriate chain */
    if (inv)
    {
      /* inv=1: we want to jump when condition is false */
      /* Merge any existing "jump-on-false" chain with the new jump.
       * Patch the opposite chain (jump-on-true) to fall through here. */
      if (jfalse >= 0)
      {
        tcc_ir_backpatch_first(ir, jfalse, t);
        t = jfalse;
      }
      if (jtrue >= 0)
      {
        tcc_ir_backpatch_to_here(ir, jtrue);
      }
    }
    else
    {
      /* inv=0: we want to jump when condition is true */
      /* Merge any existing "jump-on-true" chain with the new jump.
       * Patch the opposite chain (jump-on-false) to fall through here. */
      if (jtrue >= 0)
      {
        tcc_ir_backpatch_first(ir, jtrue, t);
        t = jtrue;
      }
      if (jfalse >= 0)
      {
        tcc_ir_backpatch_to_here(ir, jfalse);
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
      /* If we're testing a memory lvalue (e.g. tabl[i]), load the value first.
       * Otherwise we end up testing the address, which is almost always non-zero
       * and can lead to invalid indirect calls.
       */
      tcc_ir_put(ir, TCCIR_OP_TEST_ZERO, &vtop[0], NULL, NULL);
      vtop->r = VT_CMP;
      vtop->cmp_op = TOK_NE;
      vtop->jtrue = -1;  /* -1 = no chain */
      vtop->jfalse = -1; /* -1 = no chain */
      return tcc_ir_generate_test(ir, inv, t);
    }
  }
  --vtop;
  return t;
}

void tcc_ir_backpatch_first(TCCIRState *ir, int t, int target_address)
{
  int lp, next;
  if (t < 0)
    return; /* -1 means no chain */
  do
  {
    lp = t;
    next = ir->instructions[t].dest.c.i;
    /* Stop if we hit end of chain or go out of bounds */
    if (next < 0 || next >= ir->next_instruction_index)
      break;
    t = next;
  } while (1);
  ir->instructions[lp].dest.c.i = target_address;
}

/* Append target t to end of jump chain n, return head of chain */
int tcc_ir_gjmp_append(TCCIRState *ir, int n, int t)
{
  if (n >= 0 && n < ir->next_instruction_index)
  {
    tcc_ir_backpatch_first(ir, n, t);
    return n;
  }
  return t;
}

void tcc_ir_generate_cmp_jmp_set(TCCIRState *ir)
{
  if (ir == NULL)
    return;
  /* Guard against invalid vtop - can happen with empty structs */
  extern SValue _vstack[];
  if (vtop < _vstack + 1) /* vstack is defined as (_vstack + 1) */
    return;
  int v = vtop->r & VT_VALMASK;
  if (v == VT_CMP)
  {
    SValue src, dest;
    int jtrue = vtop->jtrue;
    int jfalse = vtop->jfalse;
    memset(&src, 0, sizeof(SValue));
    memset(&dest, 0, sizeof(SValue));
    dest.vr = tcc_ir_get_vreg_temp(ir);
    dest.type.t = VT_INT;
    dest.pr0 = PREG_NONE;
    dest.pr1 = PREG_NONE;

    if (jtrue >= 0 || jfalse >= 0)
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
      jump_dest.c.i = -1; /* will be patched */
      int end_jump = tcc_ir_put(ir, TCCIR_OP_JUMP, NULL, NULL, &jump_dest);

      /* Patch jtrue chain to here - set dest = 1 */
      if (jtrue >= 0)
      {
        tcc_ir_backpatch_to_here(ir, jtrue);
        src.r = VT_CONST;
        src.c.i = 1;
        src.pr0 = PREG_NONE;
        src.pr1 = PREG_NONE;
        tcc_ir_put(ir, TCCIR_OP_ASSIGN, &src, NULL, &dest);
        if (jfalse >= 0)
        {
          /* Jump over the jfalse handler */
          jump_dest.c.i = -1; /* will be patched */
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
      else if (jfalse >= 0)
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
    SValue jump_dest;
    int t;
    memset(&src1, 0, sizeof(SValue));
    memset(&dest, 0, sizeof(SValue));
    memset(&jump_dest, 0, sizeof(SValue));
    dest.vr = tcc_ir_get_vreg_temp(ir);
    dest.type.t = VT_INT;
    src1.vr = -1;
    src1.r = VT_CONST;
    t = v & 1;
    src1.c.i = t;
    tcc_ir_put(ir, TCCIR_OP_ASSIGN, &src1, NULL, &dest);

    /* Default path: result already set to `t`. Skip the alternate assignment.
       If the jump chain is taken, execution lands at the alternate assignment
       which flips the result to `t ^ 1`. */
    jump_dest.vr = -1;
    jump_dest.c.i = -1; /* patched to end */
    int end_jump = tcc_ir_put(ir, TCCIR_OP_JUMP, NULL, NULL, &jump_dest);

    tcc_ir_backpatch_to_here(ir, vtop->c.i);
    src1.c.i = t ^ 1;
    tcc_ir_put(ir, TCCIR_OP_ASSIGN, &src1, NULL, &dest);

    ir->instructions[end_jump].dest.c.i = ir->next_instruction_index;
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

static bool tcc_ir_operand_needs_dereference(SValue *sv)
{
  const int val_loc = sv->r & VT_VALMASK;
  switch (val_loc)
  {
  case VT_CONST:
  case VT_LOCAL:
    /* VT_CONST with VT_LVAL means we're loading through a global symbol address.
     * For example: a.x where 'a' is a static struct - the address is a constant
     * (global symbol) but we need to dereference it to get the value. */
    return (sv->r & VT_LVAL) != 0;
  case VT_LLOCAL:
  case VT_CMP:
  case VT_JMP:
  case VT_JMPI:
    return false;
  default: /* must be temporary vreg */
    /* Register parameters (VT_PARAM without VT_LOCAL) have VT_LVAL set to allow
     * taking their address (&param), but the register holds the VALUE directly,
     * not a pointer. So VT_LVAL does NOT mean dereference for these. */
    if ((sv->r & VT_PARAM) && !(sv->r & VT_LOCAL))
      return false;
    return (sv->r & VT_LVAL) != 0;
  }
}
