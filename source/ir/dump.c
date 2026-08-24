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
  case TCCIR_OP_SMULL:
    return "SMULL";
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
  case TCCIR_OP_ROR:
    return "ROR";
  case TCCIR_OP_CLZ:
    return "CLZ";
  case TCCIR_OP_RBIT:
    return "RBIT";
  case TCCIR_OP_REV:
    return "REV";
  case TCCIR_OP_REV16:
    return "REV16";
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
  case TCCIR_OP_UBFX:
    return "UBFX";
  case TCCIR_OP_SBFX:
    return "SBFX";
  case TCCIR_OP_BFI:
    return "BFI";
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
  case TCCIR_OP_ZEXT:
    return "ZEXT";
  case TCCIR_OP_PACK64:
    return "PACK64";
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
  case TCCIR_OP_PREFETCH:
    return "PREFETCH";
  case TCCIR_OP_TRAP:
    return "TRAP";
  case TCCIR_OP_SET_CHAIN:
    return "SET_CHAIN";
  case TCCIR_OP_INIT_CHAIN_SLOT:
    return "INIT_CHAIN_SLOT";
  case TCCIR_OP_MLA:
    return "MLA";
  case TCCIR_OP_SWITCH_TABLE:
    return "SWITCH_TABLE";
  case TCCIR_OP_SWITCH_LOAD:
    return "SWITCH_LOAD";
  case TCCIR_OP_BUILTIN_APPLY_ARGS:
    return "BUILTIN_APPLY_ARGS";
  case TCCIR_OP_BUILTIN_APPLY:
    return "BUILTIN_APPLY";
  case TCCIR_OP_BUILTIN_RETURN:
    return "BUILTIN_RETURN";
  case TCCIR_OP_SETJMP:
    return "SETJMP";
  case TCCIR_OP_LONGJMP:
    return "LONGJMP";
  case TCCIR_OP_NL_SETJMP:
    return "NL_SETJMP";
  case TCCIR_OP_NL_LONGJMP:
    return "NL_LONGJMP";
  case TCCIR_OP_BLOCK_COPY:
    return "BLOCK_COPY";
  case TCCIR_OP_SELECT:
    return "SELECT";
  default:
    return "UNKNOWN_OP";
  }
}

const char *tcc_ir_dump_op_name(int op)
{
  return tcc_ir_get_op_name((TccIrOp)op);
}

void tcc_ir_dump_vreg(int vreg, FILE *out)
{
  fprintf(out, "VReg %s:%d", tcc_ir_vreg_type_string(vreg), TCCIR_DECODE_VREG_POSITION(vreg));
}

void tcc_ir_print_vreg(int vreg)
{
  tcc_ir_dump_vreg(vreg, stdout);
}

/* 0 before register allocation (vregs only), 1 after (physical regs shown) */
static int show_physical_regs = 0;

void tcc_ir_dump_set_show_physical_regs(int show)
{
  show_physical_regs = show;
}

/* Matches pass_name against the comma-separated -dump-ir-passes= list ("all" selects every pass) */
int tcc_ir_dump_passes_match(TCCState *s, const char *pass_name)
{
  if (!s || !s->dump_ir_passes || !pass_name)
    return 0;
  const char *p = s->dump_ir_passes;
  size_t name_len = strlen(pass_name);
  while (*p)
  {
    const char *comma = strchr(p, ',');
    size_t tok_len = comma ? (size_t)(comma - p) : strlen(p);
    if (tok_len == 3 && !memcmp(p, "all", 3))
      return 1;
    if (tok_len == name_len && !memcmp(p, pass_name, name_len))
      return 1;
    if (!comma)
      break;
    p = comma + 1;
  }
  return 0;
}

/* Shared by tccgen.c RUN_PASS and ir/opt/ssa_opt.c so every pass is observable the same way */
void tcc_ir_dump_after_pass(TCCIRState *ir, const char *pass_name)
{
#ifdef CONFIG_TCC_DEBUG
  if (!tcc_ir_dump_passes_match(tcc_state, pass_name))
    return;
  tcc_ir_dump_set_show_physical_regs(0);
  printf("=== AFTER %s ===\n", pass_name);
  tcc_ir_dump(ir, stdout);
  /* Switch side tables consume absolute indices; print them so a stale target is visible */
  for (int t = 0; t < ir->num_switch_tables; t++) {
    TCCIRSwitchTable *tbl = &ir->switch_tables[t];
    printf("SWTAB %d: min=%lld max=%lld default=%d targets=[", t,
           (long long)tbl->min_val, (long long)tbl->max_val, tbl->default_target);
    for (int j = 0; j < tbl->num_entries; j++)
      printf("%s%d", j ? "," : "", tbl->targets[j]);
    printf("]\n");
  }
  printf("=== END AFTER %s ===\n", pass_name);
#else
  (void)ir;
  (void)pass_name;
#endif
}

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

static void dump_vreg_short(int vreg, FILE *out)
{
  fprintf(out, "%c%d", vreg_type_prefix(vreg), TCCIR_DECODE_VREG_POSITION(vreg));
}

#define SPILL_MARK_BEGIN "\033[41m"
#define SPILL_MARK_END "\033[0m"

static int get_vreg_physical_reg(TCCIRState *ir, int32_t vreg, int *spilled, int *offset)
{
  if (vreg < 0 || !ir)
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

void tcc_ir_dump_op(TCCIRState *ir, IROperand op, FILE *out)
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
      fprintf(out, "GlobalSym(%d)", sym->v);
      if (addend != 0)
        fprintf(out, "+%d", (int)addend);
      if (op.is_lval)
        fprintf(out, "***DEREF***");
    }
    else
    {
      fprintf(out, "GlobalSym(?)");
    }
  }
  break;
  case IROP_TAG_IMM32:
  case IROP_TAG_F32:
  case IROP_TAG_I64:
  case IROP_TAG_F64:
  {
    if (op.btype == IROP_BTYPE_INT64)
      fprintf(out, "#%lld", (long long)irop_get_imm64_ex(ir, op));
    else
      fprintf(out, "#%d", (int)irop_get_imm64_ex(ir, op));
    /* An lvalue immediate is an absolute address read through, not a value. */
    if (op.is_lval)
      fprintf(out, "***DEREF***");
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
        fprintf(out, SPILL_MARK_BEGIN "SpillLoc[%d]***DEREF***" SPILL_MARK_END, offset);
      else
        fprintf(out, "VT_LLOCAL (cval=%ld)", (long)irop_get_stack_offset(op));
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
          fprintf(out, SPILL_MARK_BEGIN "SpillLoc[%d]" SPILL_MARK_END, offset);
        else
        {
          if (!op.is_lval)
            fprintf(out, "&");
          fprintf(out, "R%d", preg);
          if (vreg != -1)
          {
            fprintf(out, "(");
            dump_vreg_short(vreg, out);
            fprintf(out, ")");
          }
        }
      }
      else if (irop_get_vreg(op) != -1)
      {
        if (!op.is_lval)
          fprintf(out, "&");
        dump_vreg_short(irop_get_vreg(op), out);
      }
      else if (!op.is_lval)
      {
        fprintf(out, "Addr[StackLoc[%ld]]", (long)irop_get_stack_offset(op));
      }
      else
      {
        fprintf(out, "StackLoc[%ld]", (long)irop_get_stack_offset(op));
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
        dump_vreg_short(vreg, out);
      else
        fprintf(out, "VReg?");
      if (irop_op_is_lval(op))
        fprintf(out, "***DEREF***");
    }
    else
    {
      if (spilled)
      {
        fprintf(out, SPILL_MARK_BEGIN "SpillLoc[%d]" SPILL_MARK_END, offset);
      }
      else
      {
        fprintf(out, "R%d", preg);
        if (vreg != -1)
        {
          fprintf(out, "(");
          dump_vreg_short(vreg, out);
          fprintf(out, ")");
        }
      }
      if (irop_op_is_lval(op))
        fprintf(out, "***DEREF***");
    }
    break;
  }
  }
}

void print_iroperand_short(TCCIRState *ir, IROperand op)
{
  tcc_ir_dump_op(ir, op, stdout);
}

void tcc_ir_dump_svalue_short(SValue *sv, FILE *out)
{
  int val_loc = sv->r & VT_VALMASK;

  switch (val_loc)
  {
  case VT_CONST:
    if (sv->r & VT_SYM)
    {
      fprintf(out, "GlobalSym(%d)", sv->sym->v);
      if (sv->c.i != 0)
        fprintf(out, "+%d", (int)sv->c.i);
      if (sv->r & VT_LVAL)
        fprintf(out, "***DEREF***");
    }
    else
    {
      if ((sv->type.t & VT_BTYPE) == VT_LLONG)
        fprintf(out, "#%lld", (long long)sv->c.i);
      else
        fprintf(out, "#%d", (int)sv->c.i);
    }
    break;
  case VT_LLOCAL:
    if (sv->pr0_reg != PREG_REG_NONE && sv->pr0_spilled)
      fprintf(out, SPILL_MARK_BEGIN "SpillLoc[%ld]***DEREF***" SPILL_MARK_END, (long)sv->c.i);
    else
      fprintf(out, "VT_LLOCAL (cval=%ld)", (long)sv->c.i);
    break;
  case VT_LOCAL:
    if (show_physical_regs && sv->pr0_reg != PREG_REG_NONE)
    {
      if (sv->pr0_spilled)
        fprintf(out, SPILL_MARK_BEGIN "SpillLoc[%ld]" SPILL_MARK_END, (long)sv->c.i);
      else
      {
        if (!(sv->r & VT_LVAL))
          fprintf(out, "&");
        fprintf(out, "R%d", sv->pr0_reg);
        if (sv->vr != -1)
        {
          fprintf(out, "(");
          dump_vreg_short(sv->vr, out);
          fprintf(out, ")");
        }
      }
    }
    else if (sv->vr != -1)
    {
      if (!(sv->r & VT_LVAL))
        fprintf(out, "&");
      dump_vreg_short(sv->vr, out);
    }
    else if (!(sv->r & VT_LVAL))
    {
      fprintf(out, "Addr[StackLoc[%ld]]", (long)sv->c.i);
    }
    else
    {
      fprintf(out, "StackLoc[%ld]", (long)sv->c.i);
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
    if (!show_physical_regs || sv->pr0_reg == PREG_REG_NONE)
    {
      dump_vreg_short(sv->vr, out);
      if (tcc_ir_operand_needs_dereference(sv))
        fprintf(out, "***DEREF***");
    }
    else
    {
      if (sv->pr0_spilled)
        fprintf(out, SPILL_MARK_BEGIN "SpillLoc[%ld]" SPILL_MARK_END, (long)sv->c.i);
      else
      {
        fprintf(out, "R%d", sv->pr0_reg);
        if (sv->vr != -1)
        {
          fprintf(out, "(");
          dump_vreg_short(sv->vr, out);
          fprintf(out, ")");
        }
      }
      if (tcc_ir_operand_needs_dereference(sv))
        fprintf(out, "***DEREF***");
    }
    break;
  }
}

void print_svalue_short(SValue *sv)
{
  tcc_ir_dump_svalue_short(sv, stdout);
}

void tcc_ir_dump_compact(TCCIRState *ir, IRQuadCompact *q, int pc, FILE *out)
{
  int op = q->op;
  IROperand src1 = tcc_ir_op_get_src1(ir, q);
  IROperand src2 = tcc_ir_op_get_src2(ir, q);
  IROperand dest = tcc_ir_op_get_dest(ir, q);

  fprintf(out, "%04d: ", pc);
  switch (op)
  {
  case TCCIR_OP_NOP:
  case TCCIR_OP_PREFETCH:
  case TCCIR_OP_TRAP:
  case TCCIR_OP_RETURNVALUE:
  case TCCIR_OP_RETURNVOID:
  case TCCIR_OP_FUNCCALLVOID:
  case TCCIR_OP_FUNCCALLVAL:
  case TCCIR_OP_FUNCPARAMVOID:
  case TCCIR_OP_TEST_ZERO:
  case TCCIR_OP_CMP:
    fprintf(out, "%s ", tcc_ir_get_op_name((TccIrOp)op));
    break;
  case TCCIR_OP_SET_CHAIN:
    fprintf(out, "%s /* R10 <- FP */ ", tcc_ir_get_op_name((TccIrOp)op));
    break;
  case TCCIR_OP_FUNCPARAMVAL:
    fprintf(out, "%s%d[call_%d] ", tcc_ir_get_op_name((TccIrOp)op), TCCIR_DECODE_PARAM_IDX(irop_get_imm64_ex(ir, src2)),
            TCCIR_DECODE_CALL_ID(irop_get_imm64_ex(ir, src2)));
    break;
  case TCCIR_OP_JUMP:
  case TCCIR_OP_JUMPIF:
    fprintf(out, "JMP to %ld ", (long)irop_get_imm64_ex(ir, dest));
    break;
  case TCCIR_OP_IJUMP:
    /* Mnemonic only; the generic has_src1 block below prints src1 once (docs/bugs.md #5) */
    fprintf(out, "IJMP ");
    break;
  case TCCIR_OP_MLA:
    tcc_ir_dump_op(ir, dest, out);
    fprintf(out, " <-- ");
    tcc_ir_dump_op(ir, src1, out);
    fprintf(out, " MLA ");
    tcc_ir_dump_op(ir, src2, out);
    fprintf(out, " + ");
    break;
  default:
    tcc_ir_dump_op(ir, dest, out);
    fprintf(out, " <-- ");
  }

  if (irop_config[op].has_src1)
  {
    if (op == TCCIR_OP_SETIF)
    {
      fprintf(out, "(cond=0x%lx)", (unsigned long)irop_get_imm64_ex(ir, src1));
    }
    else if (op != TCCIR_OP_JUMPIF && op != TCCIR_OP_MLA)
    {
      tcc_ir_dump_op(ir, src1, out);
    }
  }

  if (irop_config[op].has_src2)
  {
    switch (op)
    {
    case TCCIR_OP_CMP:
      fprintf(out, ",");
      tcc_ir_dump_op(ir, src2, out);
      break;
    case TCCIR_OP_FUNCPARAMVAL:
    case TCCIR_OP_FUNCCALLVAL:
    case TCCIR_OP_MLA:
      break;
    default:
      fprintf(out, " %s ", tcc_ir_get_op_name((TccIrOp)op));
      tcc_ir_dump_op(ir, src2, out);
    }
  }

  if (op == TCCIR_OP_BLOCK_COPY)
    fprintf(out, " [BLOCK_COPY]");
  else if (op == TCCIR_OP_SELECT)
    fprintf(out, " [SELECT cond=0x%lx]",
            (unsigned long)irop_get_imm64_ex(ir, tcc_ir_op_get_cond(ir, q)));
  else if (op == TCCIR_OP_STORE)
    fprintf(out, " [STORE]");
  else if (op == TCCIR_OP_LOAD)
    fprintf(out, " [LOAD]");
  else if (op == TCCIR_OP_ASSIGN)
    fprintf(out, " [ASSIGN]");
  else if (op == TCCIR_OP_FUNCCALLVAL)
  {
    fprintf(out, " --> ");
    tcc_ir_dump_op(ir, dest, out);
  }
  else if (op == TCCIR_OP_JUMPIF)
  {
    fprintf(out, " if \"");
    switch ((int)irop_get_imm64_ex(ir, src1))
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
      fprintf(out, "?");
      break;
    }
    fprintf(out, "\"");
  }
  else if (op == TCCIR_OP_MLA)
  {
    IROperand accum = tcc_ir_op_get_accum(ir, q);
    tcc_ir_dump_op(ir, accum, out);
  }
  fprintf(out, "\n");
}

void tcc_print_quadruple_irop(TCCIRState *ir, IRQuadCompact *q, int pc)
{
  tcc_ir_dump_compact(ir, q, pc, stdout);
}

void tcc_ir_dump_instr(TCCIRState *ir, int idx, FILE *out)
{
  if (idx < 0 || idx >= ir->next_instruction_index)
    return;
  tcc_ir_dump_compact(ir, &ir->compact_instructions[idx], idx, out);
}

void tcc_ir_dump_range(TCCIRState *ir, int start, int end, FILE *out)
{
  if (start < 0)
    start = 0;
  if (end > ir->next_instruction_index)
    end = ir->next_instruction_index;
  for (int i = start; i < end; i++)
    tcc_ir_dump_compact(ir, &ir->compact_instructions[i], i, out);
}

void tcc_ir_dump(TCCIRState *ir, FILE *out)
{
  tcc_ir_dump_range(ir, 0, ir->next_instruction_index, out);
}

void tcc_ir_dump_stdout(TCCIRState *ir)
{
  tcc_ir_dump(ir, stdout);
}

void tcc_ir_show(TCCIRState *ir)
{
  tcc_ir_dump(ir, stdout);
}

/* Print the function the way SSA is written down: one section per basic block,
 * the phi nodes at the head of it, then that block's instructions.
 *
 * `tcc_ir_dump_after_pass` cannot do this and never could: a phi is not an
 * instruction here, it is an IRPhiNode hanging off IRSSAState.block_phis, so
 * the flat listing after `ssa_rename` shows uses of names that nothing in it
 * defines -- the phi destinations.  This is the same IR with the joins put
 * back, which is what makes a rename dump readable.
 *
 *     B1  [0002..0004]  preds: B0 B3
 *           T4 <-- PHI [B0: T8, B3: T10]   ; V0
 *     0002: CMP T5,P0
 *
 * The `; V0` is IRPhiNode.orig_vreg -- the variable slot the value had before
 * promotion, which is the name the source used.  Debug-only, like every other
 * dump in this file. */
void tcc_ir_dump_ssa_after_pass(TCCIRState *ir, struct IRSSAState *ssa, const char *pass_name)
{
#ifdef CONFIG_TCC_DEBUG
  if (!tcc_ir_dump_passes_match(tcc_state, pass_name))
    return;
  IRCFG *cfg = ssa ? ssa->cfg : NULL;
  if (!cfg)
    return;
  tcc_ir_dump_set_show_physical_regs(0);
  printf("=== AFTER %s ===\n", pass_name);

  int count = ir->next_instruction_index;
  unsigned char *printed = count > 0 ? tcc_mallocz(count) : NULL;

  for (int b = 0; b < cfg->num_blocks; b++)
  {
    IRBasicBlock *blk = &cfg->blocks[b];
    /* end_idx is exclusive (cfg.c builds it as the next leader's index). */
    printf("B%d  [%04d..%04d]  preds:", b, blk->start_idx, blk->end_idx - 1);
    if (!blk->num_preds)
      printf(" -");
    for (int p = 0; p < blk->num_preds; p++)
      printf(" B%d", blk->preds[p]);
    printf("\n");

    for (IRPhiNode *phi = ssa->block_phis ? ssa->block_phis[b] : NULL; phi; phi = phi->next)
    {
      printf("      ");
      dump_vreg_short(phi->dest_vreg, stdout);
      printf(" <-- PHI [");
      for (int i = 0; i < phi->num_operands; i++)
      {
        printf("%sB%d: ", i ? ", " : "", phi->operands[i].pred_block);
        if (phi->operands[i].vreg < 0)
          printf("undef");
        else
          dump_vreg_short(phi->operands[i].vreg, stdout);
      }
      printf("]");
      if (phi->orig_vreg >= 0)
      {
        printf("   ; ");
        dump_vreg_short(phi->orig_vreg, stdout);
      }
      printf("\n");
    }

    for (int i = blk->start_idx; i < blk->end_idx && i < count; i++)
    {
      if (i < 0)
        continue;
      tcc_ir_dump_compact(ir, &ir->compact_instructions[i], i, stdout);
      if (printed)
        printed[i] = 1;
    }
  }

  /* Anything the CFG does not own -- trailing NOPs, an unreachable tail left by
   * a fold -- is still part of the array the next pass sees, so say so rather
   * than dropping it silently. */
  if (printed)
  {
    int announced = 0;
    for (int i = 0; i < count; i++)
    {
      if (printed[i])
        continue;
      if (!announced++)
        printf("(in no block)\n");
      tcc_ir_dump_compact(ir, &ir->compact_instructions[i], i, stdout);
    }
    tcc_free(printed);
  }

  printf("=== END AFTER %s ===\n", pass_name);
#else
  (void)ir;
  (void)ssa;
  (void)pass_name;
#endif
}
