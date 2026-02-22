/*
 *  TCC IR - Debug Dumping Implementation
 *
 *  Copyright (c) 2025 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#define USING_GLOBALS
#include "ir.h"

/* ============================================================================
 * Operation Name Mapping
 * ============================================================================ */

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
  case TCCIR_OP_LOAD_INDEXED:
    return "LOAD_INDEXED";
  case TCCIR_OP_STORE_INDEXED:
    return "STORE_INDEXED";
  case TCCIR_OP_LOAD_POSTINC:
    return "LOAD_POSTINC";
  case TCCIR_OP_STORE_POSTINC:
    return "STORE_POSTINC";
  case TCCIR_OP_ASSIGN:
    return "ASSIGN";
  case TCCIR_OP_LEA:
    return "LEA";
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
  case TCCIR_OP_SET_CHAIN:
    return "SET_CHAIN";
  case TCCIR_OP_INIT_CHAIN_SLOT:
    return "INIT_CHAIN_SLOT";
  case TCCIR_OP_MLA:
    return "MLA";
  case TCCIR_OP_SWITCH_TABLE:
    return "SWITCH_TABLE";
  default:
    return "UNKNOWN_OP";
  }
}

/* ============================================================================
 * Dump Implementation
 * ============================================================================ */

void tcc_ir_dump(TCCIRState *ir, FILE *out)
{
  /* TODO: Move implementation from tccir.c */
  (void)ir;
  (void)out;
}

void tcc_ir_dump_stdout(TCCIRState *ir)
{
  tcc_ir_dump(ir, stdout);
}

void tcc_ir_dump_instr(TCCIRState *ir, int idx, FILE *out)
{
  /* TODO: Move implementation from tccir.c */
  (void)ir;
  (void)idx;
  (void)out;
}

void tcc_ir_dump_range(TCCIRState *ir, int start, int end, FILE *out)
{
  /* TODO: Move implementation from tccir.c */
  (void)ir;
  (void)start;
  (void)end;
  (void)out;
}

void tcc_ir_dump_svalue(TCCIRState *ir, const SValue *sv, FILE *out)
{
  /* TODO: Move implementation from tccir.c */
  (void)ir;
  (void)sv;
  (void)out;
}

void tcc_ir_dump_svalue_short(TCCIRState *ir, const SValue *sv, FILE *out)
{
  /* TODO: Move implementation from tccir.c */
  (void)ir;
  (void)sv;
  (void)out;
}

void tcc_ir_dump_op(TCCIRState *ir, IROperand op, FILE *out)
{
  /* TODO: Move implementation from tccir.c */
  (void)ir;
  (void)op;
  (void)out;
}

void tcc_ir_dump_op_short(TCCIRState *ir, IROperand op, FILE *out)
{
  /* TODO: Move implementation from tccir.c */
  (void)ir;
  (void)op;
  (void)out;
}

void tcc_ir_dump_quad(TCCIRState *ir, TACQuadruple *q, int pc, FILE *out)
{
  /* TODO: Move implementation from tccir.c */
  (void)ir;
  (void)q;
  (void)pc;
  (void)out;
}

void tcc_ir_dump_compact(TCCIRState *ir, IRQuadCompact *q, int pc, FILE *out)
{
  /* TODO: Move implementation from tccir.c */
  (void)ir;
  (void)q;
  (void)pc;
  (void)out;
}

void tcc_ir_dump_vreg(TCCIRState *ir, int vreg, FILE *out)
{
  /* TODO: Move implementation from tccir.c */
  (void)ir;
  (void)vreg;
  (void)out;
}

/* ============================================================================
 * Legacy Dump Functions (from tccir.c)
 * ============================================================================
 * These functions are used when TCC_DUMP_THUMB_GEN is enabled for debugging
 * the code generation process.
 * ============================================================================ */

#if TCC_DUMP_THUMB_GEN

#include "../tccmachine.h"

void tcc_dump_svalue_short_to(FILE *out, const SValue *sv)
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
    if (sv->pr0_reg != PREG_REG_NONE && sv->pr0_spilled)
      fprintf(out, SPILL_MARK_BEGIN "SpillLoc[%d]***DEREF***" SPILL_MARK_END, (int)sv->c.i);
    else
      fprintf(out, "VT_LLOCAL(cval=%d)", (int)sv->c.i);
    break;
  case VT_LOCAL:
    if (sv->pr0_reg != PREG_REG_NONE)
    {
      if (sv->pr0_spilled)
        fprintf(out, SPILL_MARK_BEGIN "SpillLoc[%d]" SPILL_MARK_END, (int)sv->c.i);
      else
      {
        if (!(r & VT_LVAL))
          fprintf(out, "&");
        fprintf(out, "R%d", sv->pr0_reg);
        /* Also show virtual register info if available */
        if (sv->vr != -1)
        {
          fprintf(out, "(");
          fprintf(out, "VReg %s:%d", tcc_ir_get_vreg_type_string(sv->vr), TCCIR_DECODE_VREG_POSITION(sv->vr));
          fprintf(out, ")");
        }
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
    if (sv->pr0_reg == PREG_REG_NONE)
    {
      fprintf(out, "VReg %s:%d", tcc_ir_get_vreg_type_string(sv->vr), TCCIR_DECODE_VREG_POSITION(sv->vr));
      if (tcc_ir_operand_needs_dereference(sv))
        fprintf(out, "***DEREF***");
    }
    else
    {
      if (sv->pr0_spilled)
        fprintf(out, SPILL_MARK_BEGIN "SpillLoc[%d]" SPILL_MARK_END, (int)sv->c.i);
      else
      {
        fprintf(out, "R%d", sv->pr0_reg);
        /* Also show virtual register info if available */
        if (sv->vr != -1)
        {
          fprintf(out, "(");
          fprintf(out, "VReg %s:%d", tcc_ir_get_vreg_type_string(sv->vr), TCCIR_DECODE_VREG_POSITION(sv->vr));
          fprintf(out, ")");
        }
      }
      if (tcc_ir_operand_needs_dereference(sv))
        fprintf(out, "***DEREF***");
    }
    break;
  }
}

void tcc_dump_quadruple_to(FILE *out, const TACQuadruple *q, int pc)
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
  case TCCIR_OP_NOP:
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

#endif /* TCC_DUMP_THUMB_GEN */

void tcc_ir_dump_live(TCCIRState *ir, FILE *out)
{
  /* TODO: Move implementation from tccir.c */
  (void)ir;
  (void)out;
}

void tcc_ir_dump_live_vreg(TCCIRState *ir, int vreg, FILE *out)
{
  /* TODO: Move implementation from tccir.c */
  (void)ir;
  (void)vreg;
  (void)out;
}

void tcc_ir_dump_stack(TCCIRState *ir, FILE *out)
{
  /* TODO: Move implementation from tccir.c */
  (void)ir;
  (void)out;
}

const char *tcc_ir_dump_op_name(int op)
{
  return tcc_ir_get_op_name((TccIrOp)op);
}

const char *tcc_ir_dump_vreg_type(int vreg_type)
{
  /* TODO: Move implementation from tccir.c */
  (void)vreg_type;
  return "unknown";
}

/* Print vreg to stdout - legacy function used throughout codebase */
void tcc_ir_print_vreg(int vreg)
{
  printf("VReg %s:%d", tcc_ir_vreg_type_string(vreg), TCCIR_DECODE_VREG_POSITION(vreg));
}

/* Flag to control whether to show physical registers in IR dump.
 * This is set to 1 after register allocation, so the second dump shows
 * physical registers, while the first dump (before optimizations) shows
 * only virtual registers. */
static int show_physical_regs = 0;

/* Set whether to show physical registers in IR dump */
void tcc_ir_dump_set_show_physical_regs(int show)
{
  show_physical_regs = show;
}

/* Get the short prefix for a vreg type: V, T, or P */
static char vreg_type_prefix(int vreg)
{
  switch (TCCIR_DECODE_VREG_TYPE(vreg))
  {
  case TCCIR_VREG_TYPE_VAR:
    return 'V';
  case TCCIR_VREG_TYPE_TEMP:
    return 'T';
  case TCCIR_VREG_TYPE_PARAM:
    return 'P';
  default:
    return '?';
  }
}

/* Print vreg in short format like V0, T1, P2 */
static void print_vreg_short(int vreg)
{
  printf("%c%d", vreg_type_prefix(vreg), TCCIR_DECODE_VREG_POSITION(vreg));
}

/* Spill mark macros for debugging output */
#define SPILL_MARK_BEGIN "\033[41m"
#define SPILL_MARK_END "\033[0m"

/* Helper to get physical register allocation for a vreg.
 * Returns the allocated physical register (0-15), or PREG_NONE if spilled/not allocated.
 * Also sets *spilled to 1 if the vreg is spilled to stack, *offset to spill location. */
static int get_vreg_physical_reg(TCCIRState *ir, int32_t vreg, int *spilled, int *offset)
{
  if (vreg == -1 || !ir)
  {
    if (spilled)
      *spilled = 0;
    if (offset)
      *offset = 0;
    return PREG_NONE;
  }
  IRLiveInterval *interval = tcc_ir_vreg_live_interval(ir, vreg);
  if (!interval)
  {
    if (spilled)
      *spilled = 0;
    if (offset)
      *offset = 0;
    return PREG_NONE;
  }
  int r0 = interval->allocation.r0;
  if (spilled)
    *spilled = (r0 & PREG_SPILLED) != 0;
  if (offset)
    *offset = interval->allocation.offset;
  return r0 & PREG_REG_NONE;
}

/* Print IROperand in short form (moved from tccir.c) */
void print_iroperand_short(TCCIRState *ir, IROperand op)
{
  int tag = irop_get_tag(op);

  switch (tag)
  {
  case IROP_TAG_SYMREF:
  {
    struct Sym *sym = irop_get_sym_ex(ir, op);
    if (sym)
    {
      int32_t addend = 0;
      IRPoolSymref *symref = irop_get_symref_ex(ir, op);
      if (symref)
        addend = symref->addend;
      printf("GlobalSym(%d)", sym->v);
      if (addend != 0)
        printf("+%d", (int)addend);
      if (op.is_lval)
        printf("***DEREF***");
    }
    else
    {
      printf("GlobalSym(?)");
    }
  }
  break;
  case IROP_TAG_IMM32:
  case IROP_TAG_F32:
  case IROP_TAG_I64:
  case IROP_TAG_F64:
  {
    if (op.btype == IROP_BTYPE_INT64)
      printf("#%lld", (long long)irop_get_imm64_ex(ir, op));
    else
      printf("#%d", (int)irop_get_imm64_ex(ir, op));
  }
  break;
  case IROP_TAG_STACKOFF:
    if (op.is_llocal)
    {
      int32_t vreg = irop_get_vreg(op);
      int spilled = 0;
      int offset = 0;
      int preg = PREG_NONE;
      if (show_physical_regs && vreg != -1)
        preg = get_vreg_physical_reg(ir, vreg, &spilled, &offset);
      if (preg != PREG_NONE && spilled)
        printf(SPILL_MARK_BEGIN "SpillLoc[%d]***DEREF***" SPILL_MARK_END, offset);
      else
        printf("VT_LLOCAL (cval=%ld)", (long)irop_get_stack_offset(op));
    }
    else
    {
      int32_t vreg = irop_get_vreg(op);
      int spilled = 0;
      int offset = 0;
      int preg = PREG_NONE;
      if (show_physical_regs && vreg != -1)
        preg = get_vreg_physical_reg(ir, vreg, &spilled, &offset);
      if (show_physical_regs && preg != PREG_NONE)
      {
        if (spilled)
          printf(SPILL_MARK_BEGIN "SpillLoc[%d]" SPILL_MARK_END, offset);
        else
        {
          if (!op.is_lval)
            printf("&");
          printf("R%d", preg);
          /* Also show virtual register info if available */
          if (vreg != -1)
          {
            printf("(");
            print_vreg_short(vreg);
            printf(")");
          }
        }
      }
      else if (irop_get_vreg(op) != -1)
      {
        if (!op.is_lval)
          printf("&");
        print_vreg_short(irop_get_vreg(op));
      }
      else if (!op.is_lval)
      {
        printf("Addr[StackLoc[%ld]]", (long)irop_get_stack_offset(op));
      }
      else
      {
        printf("StackLoc[%ld]", (long)irop_get_stack_offset(op));
      }
    }
    break;
  default:
  {
    int32_t vreg = irop_get_vreg(op);
    int spilled = 0;
    int offset = 0;
    int preg = PREG_NONE;

    if (show_physical_regs && vreg != -1)
      preg = get_vreg_physical_reg(ir, vreg, &spilled, &offset);

    if (!show_physical_regs || preg == PREG_NONE)
    {
      if (vreg != -1)
        print_vreg_short(vreg);
      else
        printf("VReg?");
      if (irop_op_is_lval(op))
        printf("***DEREF***");
    }
    else
    {
      if (spilled)
      {
        printf(SPILL_MARK_BEGIN "SpillLoc[%d]" SPILL_MARK_END, offset);
      }
      else
      {
        printf("R%d", preg);
        /* Also show virtual register info if available */
        if (vreg != -1)
        {
          printf("(");
          print_vreg_short(vreg);
          printf(")");
        }
      }
      if (irop_op_is_lval(op))
        printf("***DEREF***");
    }
    break;
  }
  }
} /* Print SValue in short form (moved from tccir.c) */
void print_svalue_short(SValue *sv)
{
  int val_loc = sv->r & VT_VALMASK;

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
      if ((sv->type.t & VT_BTYPE) == VT_LLONG)
        printf("#%lld", (long long)sv->c.i);
      else
        printf("#%d", (int)sv->c.i);
    }
    break;
  case VT_LLOCAL:
    if (sv->pr0_reg != PREG_REG_NONE && sv->pr0_spilled)
      printf(SPILL_MARK_BEGIN "SpillLoc[%ld]***DEREF***" SPILL_MARK_END, (long)sv->c.i);
    else
      printf("VT_LLOCAL (cval=%ld)", (long)sv->c.i);
    break;
  case VT_LOCAL:
    if (show_physical_regs && sv->pr0_reg != PREG_REG_NONE)
    {
      if (sv->pr0_spilled)
        printf(SPILL_MARK_BEGIN "SpillLoc[%ld]" SPILL_MARK_END, (long)sv->c.i);
      else
      {
        if (!(sv->r & VT_LVAL))
          printf("&");
        printf("R%d", sv->pr0_reg);
        /* Also show virtual register info if available */
        if (sv->vr != -1)
        {
          printf("(");
          print_vreg_short(sv->vr);
          printf(")");
        }
      }
    }
    else if (sv->vr != -1)
    {
      if (!(sv->r & VT_LVAL))
        printf("&");
      print_vreg_short(sv->vr);
    }
    else if (!(sv->r & VT_LVAL))
    {
      printf("Addr[StackLoc[%ld]]", (long)sv->c.i);
    }
    else
    {
      printf("StackLoc[%ld]", (long)sv->c.i);
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
  default:
    if (!show_physical_regs || sv->pr0_reg == PREG_REG_NONE)
    {
      print_vreg_short(sv->vr);
      if (tcc_ir_operand_needs_dereference(sv))
        printf("***DEREF***");
    }
    else
    {
      if (sv->pr0_spilled)
        printf(SPILL_MARK_BEGIN "SpillLoc[%ld]" SPILL_MARK_END, (long)sv->c.i);
      else
      {
        printf("R%d", sv->pr0_reg);
        /* Also show virtual register info if available */
        if (sv->vr != -1)
        {
          printf("(");
          print_vreg_short(sv->vr);
          printf(")");
        }
      }
      if (tcc_ir_operand_needs_dereference(sv))
        printf("***DEREF***");
    }
    break;
  }
}

/* Print quadruple IR operation (moved from tccir.c) */
void tcc_print_quadruple_irop(TCCIRState *ir, IRQuadCompact *q, int pc)
{
  int op = q->op;
  IROperand src1 = tcc_ir_op_get_src1(ir, q);
  IROperand src2 = tcc_ir_op_get_src2(ir, q);
  IROperand dest = tcc_ir_op_get_dest(ir, q);

  printf("%04d: ", pc);
  switch (op)
  {
  case TCCIR_OP_NOP:
  case TCCIR_OP_RETURNVALUE:
  case TCCIR_OP_RETURNVOID:
  case TCCIR_OP_FUNCCALLVOID:
  case TCCIR_OP_FUNCCALLVAL:
  case TCCIR_OP_FUNCPARAMVOID:
  case TCCIR_OP_TEST_ZERO:
  case TCCIR_OP_CMP:
    printf("%s ", tcc_ir_get_op_name((TccIrOp)op));
    break;
  case TCCIR_OP_SET_CHAIN:
    printf("%s /* R10 <- FP */ ", tcc_ir_get_op_name((TccIrOp)op));
    break;
  case TCCIR_OP_FUNCPARAMVAL:
    printf("%s%d[call_%d] ", tcc_ir_get_op_name((TccIrOp)op), TCCIR_DECODE_PARAM_IDX(irop_get_imm64_ex(ir, src2)),
           TCCIR_DECODE_CALL_ID(irop_get_imm64_ex(ir, src2)));
    break;
  case TCCIR_OP_JUMP:
  case TCCIR_OP_JUMPIF:
    printf("JMP to %ld ", (long)irop_get_imm64_ex(ir, dest));
    break;
  case TCCIR_OP_IJUMP:
    printf("IJMP ");
    print_iroperand_short(ir, src1);
    printf(" ");
    break;
  case TCCIR_OP_MLA:
    /* MLA has 4 operands: dest = src1 * src2 + accum */
    print_iroperand_short(ir, dest);
    printf(" <-- ");
    print_iroperand_short(ir, src1);
    printf(" MLA ");
    print_iroperand_short(ir, src2);
    printf(" + ");
    break;
  default:
    print_iroperand_short(ir, dest);
    printf(" <-- ");
  }

  if (irop_config[op].has_src1)
  {
    if (op == TCCIR_OP_SETIF)
    {
      printf("(cond=0x%lx)", (unsigned long)irop_get_imm64_ex(ir, src1));
    }
    else if (op != TCCIR_OP_JUMPIF && op != TCCIR_OP_MLA)
    {
      print_iroperand_short(ir, src1);
    }
  }

  if (irop_config[op].has_src2)
  {
    switch (op)
    {
    case TCCIR_OP_CMP:
      printf(",");
      print_iroperand_short(ir, src2);
      break;
    case TCCIR_OP_FUNCPARAMVAL:
    case TCCIR_OP_FUNCCALLVAL:
    case TCCIR_OP_MLA:
      break;
    default:
      printf(" %s ", tcc_ir_get_op_name((TccIrOp)op));
      print_iroperand_short(ir, src2);
    }
  }

  if (op == TCCIR_OP_STORE)
    printf(" [STORE]");
  else if (op == TCCIR_OP_LOAD)
    printf(" [LOAD]");
  else if (op == TCCIR_OP_ASSIGN)
    printf(" [ASSIGN]");
  else if (op == TCCIR_OP_FUNCCALLVAL)
  {
    printf(" --> ");
    print_iroperand_short(ir, dest);
  }
  else if (op == TCCIR_OP_JUMPIF)
  {
    printf(" if \"");
    switch ((int)irop_get_imm64_ex(ir, src1))
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
      printf("?");
      break;
    }
    printf("\"");
  }
  else if (op == TCCIR_OP_MLA)
  {
    /* Print the 4th operand (accumulator) */
    IROperand accum = tcc_ir_op_get_accum(ir, q);
    print_iroperand_short(ir, accum);
  }
  printf("\n");
}

/* Show IR block (moved from tccir.c) */
void tcc_ir_show(TCCIRState *ir)
{
  for (int i = 0; i < ir->next_instruction_index; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    tcc_print_quadruple_irop(ir, q, i);
  }
}

int tcc_ir_dump_try_objdump(const unsigned char *bytes, size_t len, uint32_t start_vma)
{
  /* TODO: Move implementation from tccir.c */
  (void)bytes;
  (void)len;
  (void)start_vma;
  return 0;
}
