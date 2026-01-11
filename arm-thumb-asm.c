/*
 *  ARMvX-m assembly generator for TCC
 *  Uses thumb instruction set
 *
 * Based on
 *  ARM specific functions for TCC assembler

 *  Copyright (c) 2001, 2002 Fabrice Bellard
 *  Copyright (c) 2020 Danny Milosavljevic
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
#include <ctype.h>
#include <string.h>

#include "tcc.h"
#include "arm-thumb-opcodes.h"

enum
{
  OPT_REG32,
  OPT_REGSET32,
  OPT_IM8,
  OPT_IM8N,
  OPT_IM32,
  OPT_VREG32,
  OPT_VREG64,
};
#define OP_REG32 (1 << OPT_REG32)
#define OP_VREG32 (1 << OPT_VREG32)
#define OP_VREG64 (1 << OPT_VREG64)
#define OP_REG (OP_REG32 | OP_VREG32 | OP_VREG64)
#define OP_IM32 (1 << OPT_IM32)
#define OP_IM8 (1 << OPT_IM8)
#define OP_IM8N (1 << OPT_IM8N)
#define OP_REGSET32 (1 << OPT_REGSET32)
#define OP_VREGSETS32 (OP_VREG32 | OP_REGSET32)
#define OP_VREGSETD32 (OP_VREG64 | OP_REGSET32)

static bool thumb_operand_is_immediate(int type)
{
  if (type != OP_IM32 && type != OP_IM8 && type != OP_IM8N)
  {
    return false;
  }
  return true;
}

static bool thumb_operand_is_register(int type)
{
  if (type != OP_REG && type != OP_REG32)
  {
    return false;
  }
  return true;
}

static bool thumb_operand_is_registerset(int type)
{
  if (type != OP_REGSET32)
  {
    return false;
  }
  return true;
}

typedef struct Operand
{
  uint32_t type;
  union
  {
    uint8_t reg;
    uint32_t regset;
    ExprValue e;
  };
} Operand;

ST_FUNC void g(int c)
{
  int ind1;
  if (nocode_wanted)
    return;
  ind1 = ind + 1;
  if (ind1 > cur_text_section->data_allocated)
    section_realloc(cur_text_section, ind1);
  cur_text_section->data[ind] = c;
  ind = ind1;
}

ST_FUNC void gen_le16(int i)
{
  g(i);
  g(i >> 8);
}

ST_FUNC void gen_le32(int i)
{
  int ind1;
  if (nocode_wanted)
    return;
  ind1 = ind + 4;
  if (ind1 > cur_text_section->data_allocated)
    section_realloc(cur_text_section, ind1);
  cur_text_section->data[ind++] = i & 0xFF;
  cur_text_section->data[ind++] = (i >> 8) & 0xFF;
  cur_text_section->data[ind++] = (i >> 16) & 0xFF;
  cur_text_section->data[ind++] = (i >> 24) & 0xFF;
}

ST_FUNC void gen_expr32(ExprValue *pe)
{
  if (pe->sym)
  {
    /* Emit relocation for symbol reference */
    greloca(cur_text_section, pe->sym, ind, R_ARM_ABS32, pe->v);
    gen_le32(0); /* Placeholder, will be filled by relocation */
  }
  else
  {
    gen_le32(pe->v);
  }
}

int is_valid_opcode(thumb_opcode op);

static void thumb_emit_opcode(thumb_opcode op)
{
  if (!is_valid_opcode(op))
  {
    tcc_error("compiler_error: received invalid opcode: 0x%x\n", op.opcode);
  }
  if (op.size == 4)
  {
    gen_le16(op.opcode >> 16);
  }
  gen_le16(op.opcode & 0xffff);
}

ST_FUNC void subst_asm_operand(CString *add_str, SValue *sv, int modifier)
{
  int r, reg, size, val;

  r = sv->r;
  if ((r & VT_VALMASK) == VT_CONST)
  {
    if (!(r & VT_LVAL) && modifier != 'c' && modifier != 'n' && modifier != 'P')
      cstr_ccat(add_str, '#');
    if (r & VT_SYM)
    {
      const char *name = get_tok_str(sv->sym->v, NULL);
      if (sv->sym->v >= SYM_FIRST_ANOM)
      {
        /* In case of anonymous symbols ("L.42", used
           for static data labels) we can't find them
           in the C symbol table when later looking up
           this name.  So enter them now into the asm label
           list when we still know the symbol.  */
        get_asm_sym(tok_alloc(name, strlen(name))->tok, sv->sym);
      }
      if (tcc_state->leading_underscore)
        cstr_ccat(add_str, '_');
      cstr_cat(add_str, name, -1);
      if ((uint32_t)sv->c.i == 0)
        goto no_offset;
      cstr_ccat(add_str, '+');
    }
    val = sv->c.i;
    if (modifier == 'n')
      val = -val;
    cstr_printf(add_str, "%d", (int)sv->c.i);
  no_offset:;
  }
  else if ((r & VT_VALMASK) == VT_LOCAL)
  {
    cstr_printf(add_str, "[fp,#%d]", (int)sv->c.i);
  }
  else if (r & VT_LVAL)
  {
    reg = r & VT_VALMASK;
    if (reg >= VT_CONST)
      tcc_internal_error("");
    cstr_printf(add_str, "[%s]", get_tok_str(TOK_ASM_r0 + reg, NULL));
  }
  else
  {
    /* register case */
    reg = r & VT_VALMASK;
    if (reg >= VT_CONST)
      tcc_internal_error("");

    /* choose register operand size */
    if ((sv->type.t & VT_BTYPE) == VT_BYTE || (sv->type.t & VT_BTYPE) == VT_BOOL)
      size = 1;
    else if ((sv->type.t & VT_BTYPE) == VT_SHORT)
      size = 2;
    else
      size = 4;

    if (modifier == 'b')
    {
      size = 1;
    }
    else if (modifier == 'w')
    {
      size = 2;
    }
    else if (modifier == 'k')
    {
      size = 4;
    }

    switch (size)
    {
    default:
      reg = TOK_ASM_r0 + reg;
      break;
    }
    cstr_printf(add_str, "%s", get_tok_str(reg, NULL));
  }
}

/* generate prolog and epilog code for asm statement */
ST_FUNC void asm_gen_code(ASMOperand *operands, int nb_operands, int nb_outputs, int is_output, uint8_t *clobber_regs,
                          int out_reg)
{
  uint8_t regs_allocated[NB_ASM_REGS];
  ASMOperand *op;
  int i, reg;
  uint32_t saved_regset = 0;

  // TODO: Check non-E ABI.
  // Note: Technically, r13 (sp) is also callee-saved--but that does not matter
  // yet
  static const uint8_t reg_saved[] = {4, 5, 6, 7, 8, 9 /* Note: sometimes special reg "sb" */, 10, 11};

  /* mark all used registers */
  memcpy(regs_allocated, clobber_regs, sizeof(regs_allocated));
  for (i = 0; i < nb_operands; i++)
  {
    op = &operands[i];
    if (op->reg >= 0)
      regs_allocated[op->reg] = 1;
  }
  for (i = 0; i < sizeof(reg_saved) / sizeof(reg_saved[0]); i++)
  {
    reg = reg_saved[i];
    if (regs_allocated[reg])
      saved_regset |= 1 << reg;
  }

  if (!is_output)
  { // prolog
    /* generate reg save code */
    if (saved_regset)
      gen_le32(0xe92d0000 | saved_regset); // push {...}

    /* generate load code */
    for (i = 0; i < nb_operands; i++)
    {
      op = &operands[i];
      if (op->reg >= 0)
      {
        if ((op->vt->r & VT_VALMASK) == VT_LLOCAL && op->is_memory)
        {
          /* memory reference case (for both input and
             output cases) */
          SValue sv;
          sv = *op->vt;
          sv.r = (sv.r & ~VT_VALMASK) | VT_LOCAL | VT_LVAL;
          sv.type.t = VT_PTR;
          load(op->reg, &sv);
        }
        else if (i >= nb_outputs || op->is_rw)
        { // not write-only
          /* load value in register */
          load(op->reg, op->vt);
          if (op->is_llong)
            tcc_error("long long not implemented");
        }
      }
    }
  }
  else
  { // epilog
    /* generate save code */
    for (i = 0; i < nb_outputs; i++)
    {
      op = &operands[i];
      if (op->reg >= 0)
      {
        if ((op->vt->r & VT_VALMASK) == VT_LLOCAL)
        {
          if (!op->is_memory)
          {
            SValue sv;
            sv = *op->vt;
            sv.r = (sv.r & ~VT_VALMASK) | VT_LOCAL;
            sv.type.t = VT_PTR;
            load(out_reg, &sv);

            sv = *op->vt;
            sv.r = (sv.r & ~VT_VALMASK) | out_reg;
            store(op->reg, &sv);
          }
        }
        else
        {
          store(op->reg, op->vt);
          if (op->is_llong)
            tcc_error("long long not implemented");
        }
      }
    }

    /* generate reg restore code */
    if (saved_regset)
      gen_le32(0xe8bd0000 | saved_regset); // pop {...}
  }
}

/* return the constraint priority (we allocate first the lowest
   numbered constraints) */
static inline int constraint_priority(const char *str)
{
  int priority, c, pr;

  /* we take the lowest priority */
  priority = 0;
  for (;;)
  {
    c = *str;
    if (c == '\0')
      break;
    str++;
    switch (c)
    {
    case 'l': // in ARM mode, that's  an alias for 'r' [ARM].
    case 'r': // register [general]
    case 'p': // valid memory address for load,store [general]
      pr = 3;
      break;
    case 'M': // integer constant for shifts [ARM]
    case 'I': // integer valid for data processing instruction immediate
    case 'J': // integer in range -4095...4095

    case 'i': // immediate integer operand, including symbolic constants
              // [general]
    case 'm': // memory operand [general]
    case 'g': // general-purpose-register, memory, immediate integer [general]
      pr = 4;
      break;
    default:
      tcc_error("unknown constraint '%c'", c);
    }
    if (pr > priority)
      priority = pr;
  }
  return priority;
}

static const char *skip_constraint_modifiers(const char *p)
{
  /* Constraint modifier:
      =   Operand is written to by this instruction
      +   Operand is both read and written to by this instruction
      %   Instruction is commutative for this operand and the following operand.

     Per-alternative constraint modifier:
      &   Operand is clobbered before the instruction is done using the input
     operands
  */
  while (*p == '=' || *p == '&' || *p == '+' || *p == '%')
    p++;
  return p;
}

#define REG_OUT_MASK 0x01
#define REG_IN_MASK 0x02

#define is_reg_allocated(reg) (regs_allocated[reg] & reg_mask)

ST_FUNC void asm_compute_constraints(ASMOperand *operands, int nb_operands, int nb_outputs, const uint8_t *clobber_regs,
                                     int *pout_reg)
{
  /* overall format: modifier, then ,-seperated list of alternatives; all
   * operands for a single instruction must have the same number of alternatives
   */
  /* TODO: Simple constraints
      whitespace  ignored
      o  memory operand that is offsetable
      V  memory but not offsetable
      <  memory operand with autodecrement addressing is allowed.  Restrictions
     apply. >  memory operand with autoincrement addressing is allowed.
     Restrictions apply. n  immediate integer operand with a known numeric value
      E  immediate floating operand (const_double) is allowed, but only if
     target=host F  immediate floating operand (const_double or const_vector) is
     allowed s  immediate integer operand whose value is not an explicit integer
      X  any operand whatsoever
      0...9 (postfix); (can also be more than 1 digit number);  an operand that
     matches the specified operand number is allowed
  */

  /* TODO: ARM constraints:
      k the stack pointer register
      G the floating-point constant 0.0
      Q memory reference where the exact address is in a single register ("m" is
preferable for asm statements) R an item in the constant pool S symbol in the
text segment of the current file [       Uv memory reference suitable for VFP
load/store insns (reg+constant offset)] [       Uy memory reference suitable for
iWMMXt load/store instructions] Uq memory reference suitable for the ARMv4 ldrsb
instruction
  */
  ASMOperand *op;
  int sorted_op[MAX_ASM_OPERANDS];
  int i, j, k, p1, p2, tmp, reg, c, reg_mask;
  const char *str;
  uint8_t regs_allocated[NB_ASM_REGS];

  /* init fields */
  for (i = 0; i < nb_operands; i++)
  {
    op = &operands[i];
    op->input_index = -1;
    op->ref_index = -1;
    op->reg = -1;
    op->is_memory = 0;
    op->is_rw = 0;
  }
  /* compute constraint priority and evaluate references to output
     constraints if input constraints */
  for (i = 0; i < nb_operands; i++)
  {
    op = &operands[i];
    str = op->constraint;
    str = skip_constraint_modifiers(str);
    if (isnum(*str) || *str == '[')
    {
      /* this is a reference to another constraint */
      k = find_constraint(operands, nb_operands, str, NULL);
      if ((unsigned)k >= i || i < nb_outputs)
        tcc_error("invalid reference in constraint %d ('%s')", i, str);
      op->ref_index = k;
      if (operands[k].input_index >= 0)
        tcc_error("cannot reference twice the same operand");
      operands[k].input_index = i;
      op->priority = 5;
    }
    else if ((op->vt->r & VT_VALMASK) == VT_LOCAL && op->vt->sym && (reg = op->vt->sym->r & VT_VALMASK) < VT_CONST)
    {
      op->priority = 1;
      op->reg = reg;
    }
    else
    {
      op->priority = constraint_priority(str);
    }
  }

  /* sort operands according to their priority */
  for (i = 0; i < nb_operands; i++)
    sorted_op[i] = i;
  for (i = 0; i < nb_operands - 1; i++)
  {
    for (j = i + 1; j < nb_operands; j++)
    {
      p1 = operands[sorted_op[i]].priority;
      p2 = operands[sorted_op[j]].priority;
      if (p2 < p1)
      {
        tmp = sorted_op[i];
        sorted_op[i] = sorted_op[j];
        sorted_op[j] = tmp;
      }
    }
  }

  for (i = 0; i < NB_ASM_REGS; i++)
  {
    if (clobber_regs[i])
      regs_allocated[i] = REG_IN_MASK | REG_OUT_MASK;
    else
      regs_allocated[i] = 0;
  }
  /* sp cannot be used */
  regs_allocated[13] = REG_IN_MASK | REG_OUT_MASK;
  /* fp cannot be used yet */
  regs_allocated[11] = REG_IN_MASK | REG_OUT_MASK;

  /* allocate registers and generate corresponding asm moves */
  for (i = 0; i < nb_operands; i++)
  {
    j = sorted_op[i];
    op = &operands[j];
    str = op->constraint;
    /* no need to allocate references */
    if (op->ref_index >= 0)
      continue;
    /* select if register is used for output, input or both */
    if (op->input_index >= 0)
    {
      reg_mask = REG_IN_MASK | REG_OUT_MASK;
    }
    else if (j < nb_outputs)
    {
      reg_mask = REG_OUT_MASK;
    }
    else
    {
      reg_mask = REG_IN_MASK;
    }
    if (op->reg >= 0)
    {
      if (is_reg_allocated(op->reg))
        tcc_error("asm regvar requests register that's taken already");
      reg = op->reg;
    }
  try_next:
    c = *str++;
    switch (c)
    {
    case '=': // Operand is written-to
      goto try_next;
    case '+': // Operand is both READ and written-to
      op->is_rw = 1;
      /* FALL THRU */
    case '&': // Operand is clobbered before the instruction is done using the
              // input operands
      if (j >= nb_outputs)
        tcc_error("'%c' modifier can only be applied to outputs", c);
      reg_mask = REG_IN_MASK | REG_OUT_MASK;
      goto try_next;
    case 'l': // In non-thumb mode, alias for 'r'--otherwise r0-r7 [ARM]
    case 'r': // general-purpose register
    case 'p': // loadable/storable address
      /* any general register */
      if ((reg = op->reg) >= 0)
        goto reg_found;
      else
        for (reg = 0; reg <= 8; reg++)
        {
          if (!is_reg_allocated(reg))
            goto reg_found;
        }
      goto try_next;
    reg_found:
      /* now we can reload in the register */
      op->is_llong = 0;
      op->reg = reg;
      regs_allocated[reg] |= reg_mask;
      break;
    case 'I': // integer that is valid as an data processing instruction
              // immediate (0...255, rotated by a multiple of two)
    case 'J': // integer in the range -4095 to 4095 [ARM]
    case 'K': // integer that satisfies constraint I when inverted (one's
              // complement)
    case 'L': // integer that satisfies constraint I when inverted (two's
              // complement)
    case 'i': // immediate integer operand, including symbolic constants
      if (!((op->vt->r & (VT_VALMASK | VT_LVAL)) == VT_CONST))
        goto try_next;
      break;
    case 'M': // integer in the range 0 to 32
      if (!((op->vt->r & (VT_VALMASK | VT_LVAL | VT_SYM)) == VT_CONST))
        goto try_next;
      break;
    case 'm': // memory operand
    case 'g':
      /* nothing special to do because the operand is already in
         memory, except if the pointer itself is stored in a
         memory variable (VT_LLOCAL case) */
      /* XXX: fix constant case */
      /* if it is a reference to a memory zone, it must lie
         in a register, so we reserve the register in the
         input registers and a load will be generated
         later */
      if (j < nb_outputs || c == 'm')
      {
        if ((op->vt->r & VT_VALMASK) == VT_LLOCAL)
        {
          /* any general register */
          for (reg = 0; reg <= 8; reg++)
          {
            if (!(regs_allocated[reg] & REG_IN_MASK))
              goto reg_found1;
          }
          goto try_next;
        reg_found1:
          /* now we can reload in the register */
          regs_allocated[reg] |= REG_IN_MASK;
          op->reg = reg;
          op->is_memory = 1;
        }
      }
      break;
    default:
      tcc_error("asm constraint %d ('%s') could not be satisfied", j, op->constraint);
      break;
    }
    /* if a reference is present for that operand, we assign it too */
    if (op->input_index >= 0)
    {
      operands[op->input_index].reg = op->reg;
      operands[op->input_index].is_llong = op->is_llong;
    }
  }

  /* compute out_reg. It is used to store outputs registers to memory
     locations references by pointers (VT_LLOCAL case) */
  *pout_reg = -1;
  for (i = 0; i < nb_operands; i++)
  {
    op = &operands[i];
    if (op->reg >= 0 && (op->vt->r & VT_VALMASK) == VT_LLOCAL && !op->is_memory)
    {
      for (reg = 0; reg <= 8; reg++)
      {
        if (!(regs_allocated[reg] & REG_OUT_MASK))
          goto reg_found2;
      }
      tcc_error("could not find free output register for reloading");
    reg_found2:
      *pout_reg = reg;
      break;
    }
  }

  /* print sorted constraints */
#ifdef ASM_DEBUG
  for (i = 0; i < nb_operands; i++)
  {
    j = sorted_op[i];
    op = &operands[j];
    printf("%%%d [%s]: \"%s\" r=0x%04x reg=%d\n", j, op->id ? get_tok_str(op->id, NULL) : "", op->constraint, op->vt->r,
           op->reg);
  }
  if (*pout_reg >= 0)
    printf("out_reg=%d\n", *pout_reg);
#endif
}

ST_FUNC void asm_clobber(uint8_t *clobber_regs, const char *str)
{
  int reg;
  TokenSym *ts;

  if (!strcmp(str, "memory") || !strcmp(str, "cc") || !strcmp(str, "flags"))
    return;
  ts = tok_alloc(str, strlen(str));
  reg = asm_parse_regvar(ts->tok);
  if (reg == -1)
  {
    tcc_error("invalid clobber register '%s'", str);
  }
  clobber_regs[reg] = 1;
}

static int asm_parse_vfp_regvar(int t, int double_precision)
{
  if (double_precision)
  {
    if (t >= TOK_ASM_d0 && t <= TOK_ASM_d15)
      return t - TOK_ASM_d0;
  }
  else
  {
    if (t >= TOK_ASM_s0 && t <= TOK_ASM_s31)
      return t - TOK_ASM_s0;
  }
  return -1;
}

/* If T refers to a register then return the register number and type.
   Otherwise return -1.  */
ST_FUNC int asm_parse_regvar(int t)
{
  if (t >= TOK_ASM_r0 && t <= TOK_ASM_pc)
  { /* register name */
    switch (t)
    {
    case TOK_ASM_fp:
      return TOK_ASM_r11 - TOK_ASM_r0;
    case TOK_ASM_ip:
      return TOK_ASM_r12 - TOK_ASM_r0;
    case TOK_ASM_sp:
      return TOK_ASM_r13 - TOK_ASM_r0;
    case TOK_ASM_lr:
      return TOK_ASM_r14 - TOK_ASM_r0;
    case TOK_ASM_pc:
      return TOK_ASM_r15 - TOK_ASM_r0;
    default:
      return t - TOK_ASM_r0;
    }
  }
  else if (t >= TOK_ASM_s0 && t <= TOK_ASM_s31)
  {
    return t - TOK_ASM_s0;
  }
  else if (t >= TOK_ASM_d0 && t <= TOK_ASM_d15)
  {
    return t - TOK_ASM_d0;
  }
  return -1;
}

/* Parse a text containing operand and store the result in OP */
static bool parse_operand(TCCState *s1, Operand *op)
{
  ExprValue e;
  int reg;
  uint64_t regset = 0;
  int reg_start = -1;

  op->type = 0;

  if (tok == TOK_ASM_rrx || tok == TOK_ASM_asl || tok == TOK_ASM_lsl || tok == TOK_ASM_asr || tok == TOK_ASM_lsr ||
      tok == TOK_ASM_ror)
  {
    return false;
  }

  if (tok == '{')
  { // regset literal
    int regset_type = 0;
    next(); // skip '{'
    while (tok != '}' && tok != TOK_EOF)
    {
      int new_regset = 0;

      if (tok >= TOK_ASM_s0 && tok <= TOK_ASM_s31)
      {
        new_regset = OP_VREGSETS32;
      }
      else if (tok >= TOK_ASM_d0 && tok <= TOK_ASM_d15)
      {
        new_regset = OP_VREGSETD32;
      }
      else
      {
        new_regset = OP_REGSET32;
      }

      reg = asm_parse_regvar(tok);
      if (reg == -1)
      {
        expect("register");
      }
      else
        next(); // skip register name

      if (regset_type == 0)
      {
        regset_type = new_regset;
      }
      else if (regset_type != new_regset)
      {
        tcc_error("mixed register types in register set");
      }

      if ((1 << reg) < regset)
        tcc_warning("registers will be processed in ascending order by "
                    "hardware--but are not specified in ascending order here");

      if (reg_start != -1)
      {
        for (int r = reg_start; r <= reg; r++)
        {
          regset |= 1 << r;
        }
        reg_start = -1;
      }
      else
      {
        regset |= 1 << reg;
      }

      if (tok == '-')
      {
        reg_start = reg;
        next();
      }
      if (tok == ',')
        next(); // skip ','
    }
    skip('}');
    if (regset == 0)
    {
      // ARM instructions don't support empty regset.
      tcc_error("empty register list is not supported");
    }
    else
    {
      op->type = regset_type;
      op->regset = regset;
    }
    return true;
  }
  else if ((reg = asm_parse_vfp_regvar(tok, 0)) != -1)
  {
    next(); // skip register name
    op->type = OP_VREG32;
    op->reg = (uint8_t)reg;
    return true;
  }
  else if ((reg = asm_parse_vfp_regvar(tok, 1)) != -1)
  {
    next(); // skip register name
    op->type = OP_VREG64;
    op->reg = (uint8_t)reg;
    return true;
  }
  else if ((reg = asm_parse_regvar(tok)) != -1)
  {
    next(); // skip register name
    op->type = OP_REG32;
    op->reg = (uint8_t)reg;
    return true;
  }
  else if (tok == '#' || tok == '$')
  {
    /* constant value */
    next(); // skip '#' or '$'
  }
  asm_expr(s1, &e);
  op->type = OP_IM32;
  op->e = e;
  if (!op->e.sym)
  {
    if ((int)op->e.v < 0 && (int)op->e.v >= -255)
      op->type = OP_IM8N;
    else if (op->e.v == (uint8_t)op->e.v)
      op->type = OP_IM8;
  }
  else
    return false;
  return true;
}

static uint8_t thumb_build_it_mask(const char *pattern, uint16_t condition)
{
  uint8_t mask = 0x0;
  for (size_t i = 2; i < 6; ++i)
  {
    if (pattern[i] == 0)
    {
      mask |= (1 << (5 - i));
      return mask;
    }

    if (tolower(pattern[i] == 't'))
    {
      mask |= (condition << (5 - i));
    }
    else
    {
      mask |= ((!condition) << (5 - i));
    }
  }
  return mask;
}

static int thumb_conditional_scope = 0;

static int thumb_parse_condition_str(const char *condition_str)
{
  if (strncmp(condition_str, "eq", 2) == 0)
  {
    return 0;
  }
  else if (strncmp(condition_str, "ne", 2) == 0)
  {
    return 1;
  }
  else if (strncmp(condition_str, "cs", 2) == 0)
  {
    return 2;
  }
  else if (strncmp(condition_str, "cc", 2) == 0)
  {
    return 3;
  }
  else if (strncmp(condition_str, "mi", 2) == 0)
  {
    return 4;
  }
  else if (strncmp(condition_str, "pl", 2) == 0)
  {
    return 5;
  }
  else if (strncmp(condition_str, "vs", 2) == 0)
  {
    return 6;
  }
  else if (strncmp(condition_str, "vc", 2) == 0)
  {
    return 7;
  }
  else if (strncmp(condition_str, "hi", 2) == 0)
  {
    return 8;
  }
  else if (strncmp(condition_str, "ls", 2) == 0)
  {
    return 9;
  }
  else if (strncmp(condition_str, "ge", 2) == 0)
  {
    return 0xa;
  }
  else if (strncmp(condition_str, "lt", 2) == 0)
  {
    return 0xb;
  }
  else if (strncmp(condition_str, "gt", 2) == 0)
  {
    return 0xc;
  }
  else if (strncmp(condition_str, "le", 2) == 0)
  {
    return 0xd;
  }
  return 0xe;
}

static thumb_shift asm_parse_optional_shift(TCCState *s1)
{
  Operand op;
  thumb_shift shift = {0, 0};
  if (tok == TOK_ASM_rrx)
  {
    next();
    return (thumb_shift){
        .type = THUMB_SHIFT_RRX,
        .value = 0,
    };
  }

  switch (tok)
  {
  case TOK_ASM_asl:
  case TOK_ASM_lsl:
    shift.type = THUMB_SHIFT_LSL;
    break;
  case TOK_ASM_asr:
    shift.type = THUMB_SHIFT_ASR;
    break;
  case TOK_ASM_lsr:
    shift.type = THUMB_SHIFT_LSR;
    break;
  case TOK_ASM_ror:
    shift.type = THUMB_SHIFT_ROR;
    break;
  default:
    return shift;
  }

  next();
  parse_operand(s1, &op);
  if (thumb_operand_is_immediate(op.type))
  {
    shift.mode = THUMB_SHIFT_IMMEDIATE;
    shift.value = op.e.v;
  }
  else if (thumb_operand_is_register(op.type))
  {
    shift.mode = THUMB_SHIFT_REGISTER;
    shift.value = op.reg;
  }
  return shift;
}

static void thumb_conditional_opcode(TCCState *s1, int token)
{
  int condition = 0;
  int mask = 0;
  const char *token_str = get_tok_str(token, NULL);
  char it_str[6] = {0};
  thumb_conditional_scope = strlen(token_str);
  strcpy(it_str, token_str);

  token_str = get_tok_str(tok, NULL);
  if (strlen(token_str) < 2)
  {
    tcc_error("thumb_conditional_opcode: condition too short: %s\n", token_str);
  }

  condition = thumb_parse_condition_str(token_str);
  mask = thumb_build_it_mask(it_str, condition & 1);
  thumb_emit_opcode(th_it(condition, mask));
  next();
}

static int process_operands(TCCState *s1, int max_operands, Operand *ops)
{
  int nb_ops = 0;
  for (nb_ops = 0; nb_ops < max_operands;)
  {
    if (!parse_operand(s1, &ops[nb_ops]))
    {
      break;
    }
    ++nb_ops;
    if (tok != ',')
      break;
    next(); // skip ','
  }
  if (tok == ',')
    next();
  return nb_ops;
}

static thumb_flags_behaviour thumb_determine_flags_behaviour(int token, int token_svariant, bool allow_in_it)
{
  if (THUMB_INSTRUCTION_GROUP(token) == token_svariant)
  {
    if (thumb_conditional_scope > 0 && !allow_in_it)
    {
      tcc_error("cannot use '%s' in IT block", get_tok_str(token, NULL));
    }
    return FLAGS_BEHAVIOUR_SET;
  }
  if (thumb_conditional_scope > 0)
  {
    return FLAGS_BEHAVIOUR_NOT_IMPORTANT;
  }
  return FLAGS_BEHAVIOUR_BLOCK;
}

typedef thumb_opcode (*thumb_generate_generic_imm_opcode)(uint32_t rd, uint32_t rn, uint32_t imm,
                                                          thumb_flags_behaviour flags, thumb_enforce_encoding encoding);

typedef thumb_opcode (*thumb_generate_generic_reg_opcode)(uint32_t rd, uint32_t rn, uint32_t rm,
                                                          thumb_flags_behaviour flags, thumb_shift shift,
                                                          thumb_enforce_encoding encoding);

typedef struct th_generic_op_data
{
  thumb_generate_generic_imm_opcode generate_imm_opcode;
  thumb_generate_generic_reg_opcode generate_reg_opcode;
  int regular_variant_token;
  int flags_variant_token;
} th_generic_op_data;

thumb_opcode thumb_process_generic_data_op(th_generic_op_data data, int token, thumb_shift shift, Operand *ops)
{
  thumb_flags_behaviour setflags = thumb_determine_flags_behaviour(token, data.flags_variant_token, true);
  thumb_enforce_encoding encoding = ENFORCE_ENCODING_NONE;
  if (thumb_operand_is_immediate(ops[2].type))
    return data.generate_imm_opcode(ops[0].reg, ops[1].reg, ops[2].e.v, setflags, encoding);

  if (thumb_operand_is_register(ops[2].type))
  {
    if ((THUMB_INSTRUCTION_GROUP(token) == data.regular_variant_token && thumb_conditional_scope == 0) ||
        THUMB_HAS_WIDE_QUALIFIER(token))
    {
      encoding = ENFORCE_ENCODING_32BIT;
    }
    return data.generate_reg_opcode(ops[0].reg, ops[1].reg, ops[2].reg, setflags, shift, encoding);
  }
  return (thumb_opcode){0, 0};
}

static void thumb_cps_opcode(int enable)
{
  int faultmask = 0;
  int interruptmask = 0;
  const char *target = get_tok_str(tok, NULL);
  if (strchr(target, 'i'))
  {
    interruptmask = 1;
  }
  if (strchr(target, 'f'))
  {
    faultmask = 1;
  }

  if (interruptmask == 1 || faultmask == 1)
  {
    next();
  }
  thumb_emit_opcode(th_cps(!enable, interruptmask, faultmask));
}

static void thumb_synchronization_barrier_opcode(int token)
{
  uint32_t fullsystem = 0xf;
  thumb_opcode op;
  const char *target = get_tok_str(tok, NULL);
  if (strcmp(target, "sy") == 0 || strcmp(target, "SY") == 0)
  {
    next();
  }
  switch (THUMB_INSTRUCTION_GROUP(token))
  {
  case TOK_ASM_dmbeq:
    op = th_dmb(fullsystem);
    break;
  case TOK_ASM_isbeq:
    op = th_isb(fullsystem);
    break;
  }
  thumb_emit_opcode(op);
}

static void thumb_dsb_opcode()
{
  uint32_t fullsystem = 0xf;
  const char *target = get_tok_str(tok, NULL);
  if (strcmp(target, "sy") == 0 || strcmp(target, "SY") == 0)
  {
    next();
  }
  thumb_emit_opcode(th_dsb(fullsystem));
}

static void thumb_adr_opcode(TCCState *s1, int token)
{
  int jump_addr = 0;
  Operand op;
  ExprValue e;
  ElfSym *esym;
  thumb_enforce_encoding encoding = ENFORCE_ENCODING_NONE;
  if (THUMB_HAS_WIDE_QUALIFIER(token))
  {
    encoding = ENFORCE_ENCODING_32BIT;
  }

  process_operands(s1, 1, &op);
  if (!thumb_operand_is_register(op.type))
  {
    expect("first operand must be a register");
  }

  asm_expr(s1, &e);
  if (e.sym)
  {
    esym = elfsym(e.sym);
    if (esym && esym->st_shndx == cur_text_section->sh_num)
    {
      int aligned_ind = ind & -4;
      jump_addr = esym->st_value - aligned_ind - 4;
    }
    else
    {
      greloca(cur_text_section, e.sym, ind, R_ARM_THM_ALU_PREL_11_0, 0);
      jump_addr = e.v;
      encoding = ENFORCE_ENCODING_32BIT;
    }
  }

  return thumb_emit_opcode(th_adr_imm(op.reg, jump_addr, encoding));
}

thumb_opcode thumb_generate_opcode_for_data_processing(int token, thumb_shift shift, Operand *ops)
{
  thumb_enforce_encoding encoding = ENFORCE_ENCODING_NONE;
  if (THUMB_HAS_WIDE_QUALIFIER(token))
  {
    encoding = ENFORCE_ENCODING_32BIT;
  }

  switch (THUMB_INSTRUCTION_GROUP(token))
  {
  case TOK_ASM_adcseq:
  case TOK_ASM_adceq:
  {
    return thumb_process_generic_data_op(
        (th_generic_op_data){
            .generate_imm_opcode = th_adc_imm,
            .generate_reg_opcode = th_adc_reg,
            .regular_variant_token = TOK_ASM_adceq,
            .flags_variant_token = TOK_ASM_adcseq,
        },
        token, shift, ops);
  }
  case TOK_ASM_andseq:
  case TOK_ASM_andeq:
    return thumb_process_generic_data_op(
        (th_generic_op_data){
            .generate_imm_opcode = th_and_imm,
            .generate_reg_opcode = th_and_reg,
            .regular_variant_token = TOK_ASM_andeq,
            .flags_variant_token = TOK_ASM_andseq,
        },
        token, shift, ops);
  case TOK_ASM_ornseq:
  case TOK_ASM_orneq:
    return thumb_process_generic_data_op(
        (th_generic_op_data){
            .generate_imm_opcode = th_orn_imm,
            .generate_reg_opcode = th_orn_reg,
            .regular_variant_token = TOK_ASM_orneq,
            .flags_variant_token = TOK_ASM_ornseq,
        },
        token, shift, ops);
  case TOK_ASM_orrseq:
  case TOK_ASM_orreq:
    return thumb_process_generic_data_op(
        (th_generic_op_data){
            .generate_imm_opcode = th_orr_imm,
            .generate_reg_opcode = th_orr_reg,
            .regular_variant_token = TOK_ASM_orreq,
            .flags_variant_token = TOK_ASM_orrseq,
        },
        token, shift, ops);
  case TOK_ASM_addseq:
  case TOK_ASM_addeq:
  case TOK_ASM_addweq:
  {
    thumb_flags_behaviour setflags = thumb_determine_flags_behaviour(token, TOK_ASM_addseq, true);

    if (thumb_operand_is_immediate(ops[2].type))
    {
      if (ops[1].reg == R_SP)
      {
        if (THUMB_INSTRUCTION_GROUP(token) == TOK_ASM_addweq)
        {
          return th_add_sp_imm_t4(ops[0].reg, ops[2].e.v, setflags, encoding);
        }
        return th_add_sp_imm(ops[0].reg, ops[2].e.v, setflags, encoding);
      }
      if (THUMB_INSTRUCTION_GROUP(token) == TOK_ASM_addweq)
      {
        return th_add_imm_t4(ops[0].reg, ops[1].reg, ops[2].e.v);
      }

      if (THUMB_INSTRUCTION_GROUP(token) == TOK_ASM_addeq && thumb_conditional_scope == 0)
      {
        encoding = ENFORCE_ENCODING_32BIT;
      }
      if (THUMB_INSTRUCTION_GROUP(token) == TOK_ASM_addseq && thumb_conditional_scope > 0)
        encoding = ENFORCE_ENCODING_32BIT;
      return th_add_imm(ops[0].reg, ops[1].reg, ops[2].e.v, setflags, encoding);
      break;
    }

    if (thumb_operand_is_register(ops[2].type))
    {
      if (ops[1].reg == R_SP)
      {
        return th_add_sp_reg(ops[0].reg, ops[2].reg, setflags, encoding, shift);
      }
      return th_add_reg(ops[0].reg, ops[1].reg, ops[2].reg, setflags, shift, encoding);
    }
  }
  case TOK_ASM_bicseq:
  case TOK_ASM_biceq:
  {
    return thumb_process_generic_data_op(
        (th_generic_op_data){
            .generate_imm_opcode = th_bic_imm,
            .generate_reg_opcode = th_bic_reg,
            .regular_variant_token = TOK_ASM_biceq,
            .flags_variant_token = TOK_ASM_bicseq,
        },
        token, shift, ops);
  }
  case TOK_ASM_clzeq:
  {
    if (!thumb_operand_is_register(ops[1].type) || !(thumb_operand_is_register(ops[0].type)))
    {
      expect("operands must be registers");
    }
    return th_clz(ops[1].reg, ops[2].reg);
  }
  case TOK_ASM_cmpeq:
  {
    if (thumb_operand_is_immediate(ops[2].type))
    {
      return th_cmp_imm(0, ops[1].reg, ops[2].e.v, FLAGS_BEHAVIOUR_SET, encoding);
    }
    return th_cmp_reg(0, ops[1].reg, ops[2].reg, FLAGS_BEHAVIOUR_SET, shift, encoding);
  }
  case TOK_ASM_cmneq:
  {
    thumb_enforce_encoding encoding = ENFORCE_ENCODING_NONE;

    if (thumb_operand_is_immediate(ops[2].type))
    {
      return th_cmn_imm(ops[1].reg, ops[2].e.v);
    }

    if (thumb_operand_is_register(ops[2].type))
    {
      if (THUMB_HAS_WIDE_QUALIFIER(token))
      {
        encoding = ENFORCE_ENCODING_32BIT;
      }
      return th_cmn_reg(ops[1].reg, ops[2].reg, shift, encoding);
    }
  }
  case TOK_ASM_eorseq:
  case TOK_ASM_eoreq:
  {
    return thumb_process_generic_data_op(
        (th_generic_op_data){
            .generate_imm_opcode = th_eor_imm,
            .generate_reg_opcode = th_eor_reg,
            .regular_variant_token = TOK_ASM_eoreq,
            .flags_variant_token = TOK_ASM_eorseq,
        },
        token, shift, ops);
  }
  case TOK_ASM_rsbseq:
  case TOK_ASM_rsbeq:
  {
    return thumb_process_generic_data_op(
        (th_generic_op_data){
            .generate_imm_opcode = th_rsb_imm,
            .generate_reg_opcode = th_rsb_reg,
            .regular_variant_token = TOK_ASM_rsbeq,
            .flags_variant_token = TOK_ASM_rsbseq,
        },
        token, shift, ops);
  }
  case TOK_ASM_mvnseq:
  case TOK_ASM_mvneq:
  {
    return thumb_process_generic_data_op(
        (th_generic_op_data){
            .generate_imm_opcode = th_mvn_imm,
            .generate_reg_opcode = th_mvn_reg,
            .regular_variant_token = TOK_ASM_mvneq,
            .flags_variant_token = TOK_ASM_mvnseq,
        },
        token, shift, ops);
  }
  case TOK_ASM_movseq:
  case TOK_ASM_movweq:
  case TOK_ASM_moveq:
  {
    thumb_flags_behaviour setflags = thumb_determine_flags_behaviour(token, TOK_ASM_movseq, false);
    if (THUMB_INSTRUCTION_GROUP(token) == TOK_ASM_movweq)
      encoding = ENFORCE_ENCODING_32BIT;

    if (thumb_operand_is_immediate(ops[2].type))
    {
      return th_mov_imm(ops[1].reg, ops[2].e.v, setflags, encoding);
    }
    return th_mov_reg(ops[1].reg, ops[2].reg, setflags, shift, encoding, thumb_conditional_scope > 0);
  }
  case TOK_ASM_bfceq:
  {
    if (!thumb_operand_is_immediate(ops[1].type) && !thumb_operand_is_immediate(ops[2].type))
    {
      expect("second/third operand must be an immediate");
    }
    return th_bfc(ops[0].reg, ops[1].e.v, ops[2].e.v);
  }
  case TOK_ASM_mulseq:
  case TOK_ASM_muleq:
  {
    thumb_flags_behaviour setflags = thumb_determine_flags_behaviour(token, TOK_ASM_mulseq, false);
    uint32_t rm = ops[2].reg;
    uint32_t rn = ops[1].reg;
    if (ops[0].reg == ops[1].reg)
    {
      rm = ops[0].reg;
      rn = ops[2].reg;
    }
    return th_mul(ops[0].reg, rn, rm, setflags, encoding);
  }
  case TOK_ASM_sdiveq:
    return th_sdiv(ops[0].reg, ops[1].reg, ops[2].reg);
  case TOK_ASM_rbiteq:
    return th_rbit(ops[1].reg, ops[2].reg);
  case TOK_ASM_reveq:
    return th_rev(ops[1].reg, ops[2].reg, encoding);
  case TOK_ASM_rev16eq:
    return th_rev16(ops[1].reg, ops[2].reg, encoding);
  case TOK_ASM_revsheq:
    return th_revsh(ops[1].reg, ops[2].reg, encoding);
  case TOK_ASM_sbcseq:
  case TOK_ASM_sbceq:
  {
    return thumb_process_generic_data_op(
        (th_generic_op_data){
            .generate_imm_opcode = th_sbc_imm,
            .generate_reg_opcode = th_sbc_reg,
            .regular_variant_token = TOK_ASM_sbceq,
            .flags_variant_token = TOK_ASM_sbcseq,
        },
        token, shift, ops);
  }
  case TOK_ASM_subseq:
  case TOK_ASM_subeq:
  case TOK_ASM_subweq:
  {
    thumb_flags_behaviour setflags = thumb_determine_flags_behaviour(token, TOK_ASM_subseq, true);

    if (thumb_operand_is_immediate(ops[2].type))
    {
      if (ops[1].reg == R_SP)
      {
        if (THUMB_INSTRUCTION_GROUP(token) == TOK_ASM_subweq)
        {
          return th_sub_sp_imm_t3(ops[0].reg, ops[2].e.v, setflags, encoding);
        }
        return th_sub_sp_imm(ops[0].reg, ops[2].e.v, setflags, encoding);
      }
      if (THUMB_INSTRUCTION_GROUP(token) == TOK_ASM_subweq)
      {
        return th_sub_imm_t4(ops[0].reg, ops[1].reg, ops[2].e.v);
      }

      if (THUMB_INSTRUCTION_GROUP(token) == TOK_ASM_subeq && thumb_conditional_scope == 0)
      {
        encoding = ENFORCE_ENCODING_32BIT;
      }
      if (THUMB_INSTRUCTION_GROUP(token) == TOK_ASM_subseq && thumb_conditional_scope > 0)
        encoding = ENFORCE_ENCODING_32BIT;
      return th_sub_imm(ops[0].reg, ops[1].reg, ops[2].e.v, setflags, encoding);
      break;
    }

    if (thumb_operand_is_register(ops[2].type))
    {
      if (ops[1].reg == R_SP)
      {
        return th_sub_sp_reg(ops[0].reg, ops[2].reg, setflags, shift, encoding);
      }
      return th_sub_reg(ops[0].reg, ops[1].reg, ops[2].reg, setflags, shift, encoding);
    }
  }
  case TOK_ASM_sxtbeq:
    return th_sxtb(ops[1].reg, ops[2].reg, shift, encoding);
  case TOK_ASM_sxtheq:
    return th_sxth(ops[1].reg, ops[2].reg, shift, encoding);
  case TOK_ASM_teqeq:
    return th_teq(ops[1].reg, ops[2].e.v);
  case TOK_ASM_tsteq:
    if (thumb_operand_is_register(ops[2].type))
      return th_tst_reg(ops[1].reg, ops[2].reg, shift, encoding);
    return th_tst_imm(ops[1].reg, ops[2].e.v);
  case TOK_ASM_udiveq:
    return th_udiv(ops[0].reg, ops[1].reg, ops[2].reg);
  case TOK_ASM_uxtbeq:
    return th_uxtb(ops[1].reg, ops[2].reg, shift, encoding);
  case TOK_ASM_uxtheq:
    return th_uxth(ops[1].reg, ops[2].reg, shift, encoding);
  }
  return (thumb_opcode){0, 0};
}

static thumb_opcode thumb_single_memory_transfer_literal_opcode(TCCState *s1, int token, Operand op0, Operand op1)
{
  ExprValue e;
  ElfSym *esym;
  int jump_addr = 0;
  int puw = 0x6;
  thumb_enforce_encoding encoding = ENFORCE_ENCODING_NONE;
  if (THUMB_HAS_WIDE_QUALIFIER(token))
  {
    encoding = ENFORCE_ENCODING_32BIT;
  }

  asm_expr(s1, &e);
  if (e.sym)
  {
    esym = elfsym(e.sym);
    if (esym && esym->st_shndx == cur_text_section->sh_num)
    {
      int aligned_ind = ind & -4;
      jump_addr = esym->st_value - aligned_ind - 4;
    }
    else
    {
      if (THUMB_INSTRUCTION_GROUP(token) == TOK_ASM_ldrdeq)
      {
        greloca(cur_text_section, e.sym, ind, R_ARM_THM_PC8, 0);
      }
      else
      {
        greloca(cur_text_section, e.sym, ind, R_ARM_THM_PC12, 0);
      }
      jump_addr = e.v;
      encoding = ENFORCE_ENCODING_32BIT;
    }
  }
  if (jump_addr < 0)
  {
    puw &= ~(0x2);
    jump_addr = -jump_addr;
  }
  switch (THUMB_INSTRUCTION_GROUP(token))
  {
  case TOK_ASM_ldreq:
    return th_ldr_imm(op0.reg, R_PC, jump_addr, puw, encoding);

  case TOK_ASM_ldrbeq:
    return th_ldrb_imm(op0.reg, R_PC, jump_addr, puw, encoding);
  case TOK_ASM_ldrdeq:
    return th_ldrd_imm(op0.reg, op1.reg, R_PC, jump_addr, puw, encoding);
  case TOK_ASM_ldrheq:
    return th_ldrh_imm(op0.reg, R_PC, jump_addr, puw, encoding);
  case TOK_ASM_ldrsbeq:
    return th_ldrsb_imm(op0.reg, R_PC, jump_addr, puw, encoding);
  case TOK_ASM_ldrsheq:
    return th_ldrsh_imm(op0.reg, R_PC, jump_addr, puw, encoding);
  case TOK_ASM_strdeq:
    return th_strd_imm(op0.reg, op1.reg, R_PC, jump_addr, puw, encoding);
  };
  return (thumb_opcode){0, 0};
}

static thumb_opcode thumb_cache_preload_opcode(TCCState *s1, int token)
{
  ExprValue e;
  ElfSym *esym;
  bool is_literal = true;
  thumb_shift shift = {0, 0};
  Operand ops[2] = {};
  int jump_addr = 0;
  uint32_t h = 0;

  if (tok == '[')
  {
    is_literal = false;
    skip('[');
    parse_operand(s1, &ops[0]);
    if (tok == ',')
    {
      skip(',');
      parse_operand(s1, &ops[1]);
    }
    if (tok == ',')
    {
      skip(',');
      if (thumb_operand_is_register(ops[1].type))
      {
        shift = asm_parse_optional_shift(s1);
      }
    }
    skip(']');
  }
  else
  {
    asm_expr(s1, &e);
    if (e.sym)
    {
      esym = elfsym(e.sym);
      if (esym && esym->st_shndx == cur_text_section->sh_num)
      {
        int aligned_ind = ind & -4;
        jump_addr = esym->st_value - aligned_ind - 4;
      }
      else
      {
        greloca(cur_text_section, e.sym, ind, R_ARM_THM_PC12, 0);
        jump_addr = e.v;
      }
    }
  }
  h = 0;
  switch (THUMB_INSTRUCTION_GROUP(token))
  {
  case TOK_ASM_pldeq:
  {
    if (is_literal)
      return th_pld_literal(jump_addr);
    if (thumb_operand_is_register(ops[1].type))
    {
      return th_pld_reg(ops[0].reg, ops[1].reg, 0, shift);
    }
    return th_pld_imm(ops[0].reg, 0, ops[1].e.v);
  }
  case TOK_ASM_plieq:
  {
    if (is_literal)
      return th_pli_literal(jump_addr);
    if (thumb_operand_is_register(ops[1].type))
    {
      return th_pli_reg(ops[0].reg, ops[1].reg, 0, shift);
    }
    return th_pli_imm(ops[0].reg, 0, ops[1].e.v);
  }
  case TOK_ASM_tbheq:
    h = 1;
  case TOK_ASM_tbbeq:
    return th_tbb(ops[0].reg, ops[1].reg, h);
  }
  return (thumb_opcode){0, 0};
}

static thumb_opcode thumb_single_memory_transfer_opcode(TCCState *s1, int token)
{
  Operand ops[3];
  Operand op2reg;
  bool closed_bracket = false;
  bool op2_minus = false;
  int excalm = 0;
  thumb_shift shift = {0, 0};
  thumb_enforce_encoding encoding = ENFORCE_ENCODING_NONE;

  ops[2] = (Operand){
      .type = OP_IM32,
      .e =
          {
              .v = 0,
              .sym = NULL,
          },
  };
  if (THUMB_HAS_WIDE_QUALIFIER(token))
  {
    encoding = ENFORCE_ENCODING_32BIT;
  }
  parse_operand(s1, &ops[0]);
  if (!thumb_operand_is_register(ops[0].type))
  {
    expect("destination operand must be a register");
  }
  if (tok != ',')
  {
    expect("at least two operands");
  }
  next();
  if (THUMB_INSTRUCTION_GROUP(token) == TOK_ASM_ldrdeq || THUMB_INSTRUCTION_GROUP(token) == TOK_ASM_stlexeq ||
      THUMB_INSTRUCTION_GROUP(token) == TOK_ASM_stlexbeq || THUMB_INSTRUCTION_GROUP(token) == TOK_ASM_stlexheq ||
      THUMB_INSTRUCTION_GROUP(token) == TOK_ASM_strdeq || THUMB_INSTRUCTION_GROUP(token) == TOK_ASM_strexeq ||
      THUMB_INSTRUCTION_GROUP(token) == TOK_ASM_strexbeq || THUMB_INSTRUCTION_GROUP(token) == TOK_ASM_strexheq)
  {
    parse_operand(s1, &op2reg);
    next();
  }
  if (tok != '[')
  {
    // we have literal addressing mode
    return thumb_single_memory_transfer_literal_opcode(s1, token, ops[0], op2reg);
  }
  skip('[');
  parse_operand(s1, &ops[1]);
  if (!thumb_operand_is_register(ops[1].type))
  {
    expect("first source operand must be a register");
  }

  if (tok == ']')
  {
    next();
    closed_bracket = true;
  }

  if (tok == ',')
  {
    next();
    if (tok == '-')
    {
      op2_minus = true;
      next();
    }
    parse_operand(s1, &ops[2]);
    if (thumb_operand_is_register(ops[2].type))
    {
      if (ops[2].reg == R_PC)
      {
        expect("PC cannot be used as offset register");
      }
      if (tok == ',')
      {
        next();
        shift = asm_parse_optional_shift(s1);
      }
    }
  }
  if (!closed_bracket)
  {
    skip(']');
    if (tok == '!')
    {
      excalm = 1;
      next();
    }
  }

  if (THUMB_INSTRUCTION_GROUP(token) == TOK_ASM_ldrdeq)
  {
    if (tok == '!')
    {
      excalm = 1;
      next();
    }
  }
  switch (THUMB_INSTRUCTION_GROUP(token))
  {
  case TOK_ASM_ldaeq:
    return th_lda(ops[0].reg, ops[1].reg);
  case TOK_ASM_ldabeq:
    return th_ldab(ops[0].reg, ops[1].reg);
  case TOK_ASM_ldaexeq:
    return th_ldaex(ops[0].reg, ops[1].reg);
  case TOK_ASM_ldaexbeq:
    return th_ldaexb(ops[0].reg, ops[1].reg);
  case TOK_ASM_ldaexheq:
    return th_ldaexh(ops[0].reg, ops[1].reg);
  case TOK_ASM_ldaheq:
    return th_ldah(ops[0].reg, ops[1].reg);
  case TOK_ASM_ldreq:
  case TOK_ASM_ldrbeq:
  case TOK_ASM_ldrdeq:
  case TOK_ASM_ldrexeq:
  case TOK_ASM_ldrexbeq:
  case TOK_ASM_ldrexheq:
  case TOK_ASM_ldrheq:
  case TOK_ASM_ldrsbeq:
  case TOK_ASM_ldrsheq:
  case TOK_ASM_streq:
  case TOK_ASM_strbeq:
  case TOK_ASM_strdeq:
  case TOK_ASM_strexeq:
  case TOK_ASM_strexbeq:
  case TOK_ASM_strexheq:
  case TOK_ASM_strheq:
    if (thumb_operand_is_immediate(ops[2].type))
    {
      uint32_t puw = 0x6;
      int imm = ops[2].e.v;
      if (excalm)
      {
        puw = 0x7;
      }

      if (closed_bracket && imm != 0)
      {
        puw = 0x3;
      }

      if (op2_minus || imm < 0)
      {
        puw &= ~(0x2);
        imm = -imm;
      }

      switch (THUMB_INSTRUCTION_GROUP(token))
      {
      case TOK_ASM_ldreq:
        return th_ldr_imm(ops[0].reg, ops[1].reg, imm, puw, encoding);
      case TOK_ASM_ldrbeq:
        return th_ldrb_imm(ops[0].reg, ops[1].reg, imm, puw, encoding);
      case TOK_ASM_ldrdeq:
        return th_ldrd_imm(ops[0].reg, op2reg.reg, ops[1].reg, imm, puw, encoding);
      case TOK_ASM_ldrexeq:
        return th_ldrex(ops[0].reg, ops[1].reg, imm);
      case TOK_ASM_ldrexbeq:
        return th_ldrexb(ops[0].reg, ops[1].reg);
      case TOK_ASM_ldrexheq:
        return th_ldrexh(ops[0].reg, ops[1].reg);
      case TOK_ASM_ldrheq:
        return th_ldrh_imm(ops[0].reg, ops[1].reg, imm, puw, encoding);
      case TOK_ASM_ldrsbeq:
        return th_ldrsb_imm(ops[0].reg, ops[1].reg, imm, puw, encoding);
      case TOK_ASM_ldrsheq:
        return th_ldrsh_imm(ops[0].reg, ops[1].reg, imm, puw, encoding);
      case TOK_ASM_streq:
        return th_str_imm(ops[0].reg, ops[1].reg, imm, puw, encoding);
      case TOK_ASM_strbeq:
        return th_strb_imm(ops[0].reg, ops[1].reg, imm, puw, encoding);
      case TOK_ASM_strdeq:
        return th_strd_imm(ops[0].reg, op2reg.reg, ops[1].reg, imm, puw, encoding);
      case TOK_ASM_strexeq:
        return th_strex(ops[0].reg, op2reg.reg, ops[1].reg, imm);
      case TOK_ASM_strexbeq:
        return th_strexb(ops[0].reg, op2reg.reg, ops[1].reg);
      case TOK_ASM_strexheq:
        return th_strexh(ops[0].reg, op2reg.reg, ops[1].reg);
      case TOK_ASM_strheq:
        return th_strh_imm(ops[0].reg, ops[1].reg, imm, puw, encoding);
      };
    }
    else
    {
      switch (THUMB_INSTRUCTION_GROUP(token))
      {
      case TOK_ASM_ldreq:
        return th_ldr_reg(ops[0].reg, ops[1].reg, ops[2].reg, shift, encoding);
      case TOK_ASM_ldrbeq:
        return th_ldrb_reg(ops[0].reg, ops[1].reg, ops[2].reg, shift, encoding);
      case TOK_ASM_ldrheq:
        return th_ldrh_reg(ops[0].reg, ops[1].reg, ops[2].reg, shift, encoding);
      case TOK_ASM_ldrsbeq:
        return th_ldrsb_reg(ops[0].reg, ops[1].reg, ops[2].reg, shift, encoding);
      case TOK_ASM_ldrsheq:
        return th_ldrsh_reg(ops[0].reg, ops[1].reg, ops[2].reg, shift, encoding);
      case TOK_ASM_streq:
        return th_str_reg(ops[0].reg, ops[1].reg, ops[2].reg, shift, encoding);
      case TOK_ASM_strbeq:
        return th_strb_reg(ops[0].reg, ops[1].reg, ops[2].reg, shift, encoding);
      case TOK_ASM_strheq:
        return th_strh_reg(ops[0].reg, ops[1].reg, ops[2].reg, shift, encoding);
      }
    }
  case TOK_ASM_ldrbteq:
    if (!thumb_operand_is_immediate(ops[2].type))
    {
      expect("third operand must be an immediate");
    }
    return th_ldrbt(ops[0].reg, ops[1].reg, ops[2].e.v);
  case TOK_ASM_ldrhteq:
    if (!thumb_operand_is_immediate(ops[2].type))
    {
      expect("third operand must be an immediate");
    }
    return th_ldrht(ops[0].reg, ops[1].reg, ops[2].e.v);
  case TOK_ASM_ldrsbteq:
    if (!thumb_operand_is_immediate(ops[2].type))
    {
      expect("third operand must be an immediate");
    }
    return th_ldrsbt(ops[0].reg, ops[1].reg, ops[2].e.v);
  case TOK_ASM_ldrshteq:
    if (!thumb_operand_is_immediate(ops[2].type))
    {
      expect("third operand must be an immediate");
    }
    return th_ldrsht(ops[0].reg, ops[1].reg, ops[2].e.v);
  case TOK_ASM_ldrteq:
    if (!thumb_operand_is_immediate(ops[2].type))
    {
      expect("third operand must be an immediate");
    }
    return th_ldrt(ops[0].reg, ops[1].reg, ops[2].e.v);
  case TOK_ASM_stleq:
    return th_stl(ops[0].reg, ops[1].reg);
  case TOK_ASM_stlbeq:
    return th_stlb(ops[0].reg, ops[1].reg);
  case TOK_ASM_stlexeq:
    return th_stlex(ops[0].reg, op2reg.reg, ops[1].reg);
  case TOK_ASM_stlexbeq:
    return th_stlexb(ops[0].reg, op2reg.reg, ops[1].reg);
  case TOK_ASM_stlexheq:
    return th_stlexh(ops[0].reg, op2reg.reg, ops[1].reg);
  case TOK_ASM_stlheq:
    return th_stlh(ops[0].reg, ops[1].reg);
  case TOK_ASM_strbteq:
    if (!thumb_operand_is_immediate(ops[2].type))
    {
      expect("third operand must be an immediate");
    }
    return th_strbt(ops[0].reg, ops[1].reg, ops[2].e.v);
  case TOK_ASM_strhteq:
    if (!thumb_operand_is_immediate(ops[2].type))
    {
      expect("third operand must be an immediate");
    }
    return th_strht(ops[0].reg, ops[1].reg, ops[2].e.v);
  case TOK_ASM_strteq:
    if (!thumb_operand_is_immediate(ops[2].type))
    {
      expect("third operand must be an immediate");
    }
    return th_strt(ops[0].reg, ops[1].reg, ops[2].e.v);
  };
  return (thumb_opcode){0, 0};
}

static void thumb_block_memory_transfer_opcode(TCCState *s1, int token)
{
  bool op0_exclam = false;
  Operand ops[2];
  thumb_enforce_encoding encoding = ENFORCE_ENCODING_NONE;
  parse_operand(s1, &ops[0]);

  if (tok == '!')
  {
    op0_exclam = 1;
    next();
  }

  if (tok == ',')
  {
    next();
    parse_operand(s1, &ops[1]);
  }

  if (!thumb_operand_is_register(ops[0].type))
  {
    expect("destination must be registers");
  }

  if (!thumb_operand_is_registerset(ops[1].type))
  {
    expect("second operand must be a register set");
  }

  if (THUMB_HAS_WIDE_QUALIFIER(token))
  {
    encoding = ENFORCE_ENCODING_32BIT;
  }

  switch (THUMB_INSTRUCTION_GROUP(token))
  {
  case TOK_ASM_ldmeq:
  case TOK_ASM_ldmfdeq:
  case TOK_ASM_ldmiaeq:
    thumb_emit_opcode(th_ldm(ops[0].reg, ops[1].regset, op0_exclam, encoding));
    break;
  case TOK_ASM_ldmdbeq:
  case TOK_ASM_ldmeaeq:
    thumb_emit_opcode(th_ldmdb(ops[0].reg, ops[1].regset, op0_exclam));
    break;
  case TOK_ASM_stmeq:
  case TOK_ASM_stmiaeq:
  case TOK_ASM_stmeaeq:
    thumb_emit_opcode(th_stm(ops[0].reg, ops[1].regset, op0_exclam, encoding));
    break;
  case TOK_ASM_stmdbeq:
  case TOK_ASM_stmfdeq:
    thumb_emit_opcode(th_stmdb(ops[0].reg, ops[1].regset, op0_exclam, encoding));
  };
}

static thumb_opcode thumb_pushpop_opcode(TCCState *s1, int token)
{
  Operand op = {};
  parse_operand(s1, &op);

  switch (THUMB_INSTRUCTION_GROUP(token))
  {
  case TOK_ASM_popeq:
    return th_pop(op.regset);
  case TOK_ASM_pusheq:
    return th_push(op.regset);
  }
  return (thumb_opcode){0, 0};
}

static thumb_opcode thumb_vpushvpop_opcode(TCCState *s1, int token)
{
  int is_doubleword = 0;
  Operand op = {};
  parse_operand(s1, &op);
  is_doubleword = op.type == OP_VREGSETD32;

  switch (THUMB_INSTRUCTION_GROUP(token))
  {
  case TOK_ASM_vpopeq:
    return th_vpop(op.regset, is_doubleword);
  case TOK_ASM_vpusheq:
    return th_vpush(op.regset, is_doubleword);
  }
  return (thumb_opcode){0, 0};
}

static uint32_t thumb_vfp_size_from_token(int token)
{
  const char *token_str = get_tok_str(token, NULL);
  return strstr(token_str, ".f64") ? 1 : 0;
}

static void thumb_vfp_expect_operand(const Operand *op, uint32_t sz, const char *what)
{
  const bool is_double = sz != 0;
  const bool matches = (is_double && op->type == OP_VREG64) || (!is_double && op->type == OP_VREG32);
  if (!matches)
  {
    tcc_error("expected %s VFP %s register", what, is_double ? "d" : "s");
  }
}

static thumb_opcode thumb_vfp_arith_opcode(TCCState *s1, int token)
{
  // Skip suffix tokens if present (e.g., "vadd.f32" splits into "vadd", ".", "f32")
  if (tok == '.')
  {
    next(); // skip the dot
    next(); // skip the suffix (f32 or f64)
  }

  Operand ops[3] = {};
  const char *tokstr = get_tok_str(token, NULL);
  const int nb_ops = process_operands(s1, sizeof(ops) / sizeof(ops[0]), ops);
  const uint32_t sz = thumb_vfp_size_from_token(token);
  const bool is_unary = strncmp(tokstr, "vneg", 4) == 0;
  const int needed = is_unary ? 2 : 3;

  if (nb_ops != needed)
  {
    expect(is_unary ? "two operands" : "three operands");
  }

  thumb_vfp_expect_operand(&ops[0], sz, "destination");
  thumb_vfp_expect_operand(&ops[1], sz, is_unary ? "source" : "operand");
  if (!is_unary)
  {
    thumb_vfp_expect_operand(&ops[2], sz, "operand");
  }

  if (strncmp(tokstr, "vadd", 4) == 0)
    return th_vadd_f(ops[0].reg, ops[1].reg, ops[2].reg, sz);
  if (strncmp(tokstr, "vsub", 4) == 0)
    return th_vsub_f(ops[0].reg, ops[1].reg, ops[2].reg, sz);
  if (strncmp(tokstr, "vmul", 4) == 0)
    return th_vmul_f(ops[0].reg, ops[1].reg, ops[2].reg, sz);
  if (strncmp(tokstr, "vdiv", 4) == 0)
    return th_vdiv_f(ops[0].reg, ops[1].reg, ops[2].reg, sz);
  if (strncmp(tokstr, "vneg", 4) == 0)
    return th_vneg_f(ops[0].reg, ops[1].reg, sz);

  tcc_error("unsupported VFP instruction '%s'", tokstr);
  return (thumb_opcode){0, 0};
}

static thumb_opcode thumb_vmov_opcode(TCCState *s1, int token)
{
  // Skip suffix tokens if present
  if (tok == '.')
  {
    next(); // skip the dot
    next(); // skip the suffix
  }

  Operand ops[3] = {};
  const int nb_ops = process_operands(s1, sizeof(ops) / sizeof(ops[0]), ops);

  if (nb_ops < 2 || nb_ops > 3)
  {
    expect("two or three operands");
  }

  // Three operands: vmov d0, r0, r1 or vmov r0, r1, d0
  if (nb_ops == 3)
  {
    // vmov d0, r0, r1 - Move two GP registers to double-precision register
    if (ops[0].type == OP_VREG64 && thumb_operand_is_register(ops[1].type) && thumb_operand_is_register(ops[2].type))
    {
      return th_vmov_2gp_dp(ops[1].reg, ops[2].reg, ops[0].reg, 0 /* to VFP register */);
    }
    // vmov r0, r1, d0 - Move double-precision register to two GP registers
    if (thumb_operand_is_register(ops[0].type) && thumb_operand_is_register(ops[1].type) && ops[2].type == OP_VREG64)
    {
      return th_vmov_2gp_dp(ops[0].reg, ops[1].reg, ops[2].reg, 1 /* to ARM registers */);
    }
    tcc_error("unsupported three-operand combination for vmov");
    return (thumb_opcode){0, 0};
  }

  // VFP register to VFP register moves
  if (ops[0].type == OP_VREG32 && ops[1].type == OP_VREG32)
  {
    return th_vmov_register(ops[0].reg, ops[1].reg, 0);
  }
  if (ops[0].type == OP_VREG64 && ops[1].type == OP_VREG64)
  {
    return th_vmov_register(ops[0].reg, ops[1].reg, 1);
  }

  // General-purpose register <-> single-precision register moves
  if (thumb_operand_is_register(ops[0].type) && ops[1].type == OP_VREG32)
  {
    return th_vmov_gp_sp(ops[0].reg, ops[1].reg, 1 /* to ARM register */);
  }
  if (ops[0].type == OP_VREG32 && thumb_operand_is_register(ops[1].type))
  {
    return th_vmov_gp_sp(ops[1].reg, ops[0].reg, 0 /* to VFP register */);
  }

  tcc_error("unsupported operand combination for vmov");
  return (thumb_opcode){0, 0};
}

static thumb_opcode thumb_vcmp_opcode(TCCState *s1, int token)
{
  // Skip suffix tokens if present (e.g., "vcmp.f32" splits into "vcmp", ".", "f32")
  if (tok == '.')
  {
    next(); // skip the dot
    next(); // skip the suffix (f32 or f64)
  }

  Operand ops[2] = {};
  const char *tokstr = get_tok_str(token, NULL);
  const int nb_ops = process_operands(s1, sizeof(ops) / sizeof(ops[0]), ops);
  const uint32_t sz = thumb_vfp_size_from_token(token);

  if (nb_ops != 2)
  {
    expect("two operands");
  }

  thumb_vfp_expect_operand(&ops[0], sz, "destination");
  thumb_vfp_expect_operand(&ops[1], sz, "source");

  return th_vcmp_f(ops[0].reg, ops[1].reg, sz);
}

static thumb_opcode thumb_vmrs_opcode(TCCState *s1, int token)
{
  // Skip suffix tokens if present
  if (tok == '.')
  {
    next(); // skip the dot
    next(); // skip the suffix
  }

  Operand op1 = {};
  parse_operand(s1, &op1);

  if (tok != ',')
  {
    expect("comma");
  }
  next(); // skip ','

  // VMRS rt, fpscr: move FP status register to ARM register
  if (!thumb_operand_is_register(op1.type))
  {
    tcc_error("vmrs: first operand must be a general-purpose register");
  }

  // Check for fpscr as second operand
  const char *second_operand = get_tok_str(tok, NULL);
  if (strcmp(second_operand, "fpscr") != 0)
  {
    tcc_error("vmrs: second operand must be fpscr");
  }
  next(); // skip 'fpscr'

  // Use the opcode helper function
  const uint32_t rt = op1.reg;
  return th_vmrs(rt);
}

static thumb_opcode thumb_vcvt_opcode(TCCState *s1, int token)
{
  // VCVT instruction for floating-point conversions
  // Syntax: vcvt.<dest_type>.<src_type> dest, src
  // Examples: vcvt.s32.f32 (float to signed int), vcvt.f32.s32 (signed int to float)

  // Parse the conversion type suffix from the token string
  // The token contains the full instruction like "vcvt.s32.f32"
  char dest_type[16] = {0};
  char src_type[16] = {0};

  const char *token_str = get_tok_str(token, NULL);

  // Find the first dot
  const char *dot1 = strchr(token_str, '.');
  if (dot1)
  {
    dot1++; // skip the dot
    const char *dot2 = strchr(dot1, '.');
    if (dot2)
    {
      // Extract dest_type (between first and second dot)
      int len = dot2 - dot1;
      if (len > 0 && len < (int)sizeof(dest_type))
      {
        strncpy(dest_type, dot1, len);
        dest_type[len] = '\0';
      }
      dot2++; // skip the second dot
      // Extract src_type (after second dot)
      strncpy(src_type, dot2, sizeof(src_type) - 1);
      src_type[sizeof(src_type) - 1] = '\0';
    }
  }

  // Skip any tokenized suffix (the suffix has already been parsed above)
  if (tok == '.')
  {
    next(); // skip the dot
    next(); // skip the suffix
  }

  Operand ops[2] = {};
  const int nb_ops = process_operands(s1, sizeof(ops) / sizeof(ops[0]), ops);

  if (nb_ops != 2)
  {
    expect("two operands");
  }

  // Extract source and destination register numbers
  const uint32_t vd = ops[0].reg; // destination
  const uint32_t vm = ops[1].reg; // source

  // Use the centralized helper function for vcvt conversions
  thumb_opcode result = th_vcvt_convert(vd, vm, dest_type, src_type);

  if (result.size == 0)
  {
    tcc_error("vcvt: unsupported conversion from %s to %s", src_type, dest_type);
  }

  return result;
}

static thumb_opcode thumb_ssat_opcode(TCCState *s1, int token)
{
  Operand ops[3];
  thumb_shift shift = {0, 0};
  process_operands(s1, sizeof(ops) / sizeof(ops[0]), ops);

  shift = asm_parse_optional_shift(s1);
  if (shift.type == THUMB_SHIFT_NONE)
  {
    shift.type = THUMB_SHIFT_LSL;
    shift.value = 0;
  }
  switch (THUMB_INSTRUCTION_GROUP(token))
  {
  case TOK_ASM_ssateq:
    return th_ssat(ops[0].reg, ops[1].e.v, ops[2].reg, shift);
  case TOK_ASM_usateq:
    return th_usat(ops[0].reg, ops[1].e.v, ops[2].reg, shift);
  }
  return (thumb_opcode){0, 0};
}

static thumb_opcode thumb_tt(TCCState *s1, int token)
{
  Operand ops[2];
  int nb_ops;
  uint32_t a = 0;
  uint32_t t = 0;

  nb_ops = process_operands(s1, sizeof(ops) / sizeof(ops[0]), ops);

  if (nb_ops < 2)
  {
    expect("two operands");
    return (thumb_opcode){0, 0};
  }

  switch (THUMB_INSTRUCTION_GROUP(token))
  {
  case TOK_ASM_ttaeq:
    a = 1;
    break;
  case TOK_ASM_ttateq:
    a = 1;
    t = 1;
    break;
  case TOK_ASM_ttteq:
    t = 1;
    break;
  }
  return th_tt(ops[0].reg, ops[1].reg, a, t);
}

static thumb_opcode thumb_bitmanipulation_opcode(TCCState *s1, int token)
{
  Operand ops[4];
  int nb_ops;
  nb_ops = process_operands(s1, sizeof(ops) / sizeof(ops[0]), ops);

  if (nb_ops < 4)
  {
    expect("four operands");
    return (thumb_opcode){0, 0};
  }

  if (!thumb_operand_is_register(ops[0].type) || !thumb_operand_is_register(ops[1].type))
  {
    expect("first two operands must be registers");
  }

  if (!thumb_operand_is_immediate(ops[2].type) || !thumb_operand_is_immediate(ops[3].type))
  {
    expect("last two operands must be immediates");
  }

  switch (THUMB_INSTRUCTION_GROUP(token))
  {
  case TOK_ASM_bfieq:
    return th_bfi(ops[0].reg, ops[1].reg, ops[2].e.v, ops[3].e.v);
  case TOK_ASM_sbfxeq:
    return th_sbfx(ops[0].reg, ops[1].reg, ops[2].e.v, ops[3].e.v);
  }
  return (thumb_opcode){0, 0};
}

static thumb_opcode thumb_pkhbt_opcode(TCCState *s1, int token)
{
  Operand ops[3];
  thumb_shift shift = {0, 0};
  process_operands(s1, sizeof(ops) / sizeof(ops[0]), ops);
  shift = asm_parse_optional_shift(s1);
  switch (THUMB_INSTRUCTION_GROUP(token))
  {
  case TOK_ASM_pkhbteq:
    if (shift.type == THUMB_SHIFT_NONE)
    {
      shift.type = THUMB_SHIFT_LSL;
      shift.value = 0;
      break;
    }
    if (shift.type != THUMB_SHIFT_LSL)
    {
      expect("shift must be LSL");
    }
    break;
  case TOK_ASM_pkhtbeq:
    if (shift.type == THUMB_SHIFT_NONE)
    {
      shift.type = THUMB_SHIFT_ASR;
      shift.value = 0;
      break;
    }
    if (shift.type != THUMB_SHIFT_ASR)
    {
      expect("shift must be ASR");
    }
    break;
  };
  return th_pkhbt(ops[0].reg, ops[1].reg, ops[2].reg, shift);
}

static thumb_opcode thumb_math_opcode(TCCState *s1, int token)
{
  Operand ops[4];
  int nb_ops;
  nb_ops = process_operands(s1, sizeof(ops) / sizeof(ops[0]), ops);

  if (nb_ops < 4)
  {
    expect("four operands");
    return (thumb_opcode){0, 0};
  }

  switch (THUMB_INSTRUCTION_GROUP(token))
  {
  case TOK_ASM_mlaeq:
    return th_mla(ops[0].reg, ops[1].reg, ops[2].reg, ops[3].reg);
  case TOK_ASM_mlseq:
    return th_mls(ops[0].reg, ops[1].reg, ops[2].reg, ops[3].reg);
  case TOK_ASM_smlaleq:
    return th_smlal(ops[0].reg, ops[1].reg, ops[2].reg, ops[3].reg);
  case TOK_ASM_smulleq:
    return th_smull(ops[0].reg, ops[1].reg, ops[2].reg, ops[3].reg);
  case TOK_ASM_umlaleq:
    return th_umlal(ops[0].reg, ops[1].reg, ops[2].reg, ops[3].reg);
  case TOK_ASM_umulleq:
    return th_umull(ops[0].reg, ops[1].reg, ops[2].reg, ops[3].reg);
  }
  return (thumb_opcode){0, 0};
}

static thumb_opcode thumb_movt_opcode(TCCState *s1, int token)
{
  Operand ops[2];
  int nb_ops;
  nb_ops = process_operands(s1, sizeof(ops) / sizeof(ops[0]), ops);

  if (nb_ops < 2)
  {
    expect("two operands");
    return (thumb_opcode){0, 0};
  }

  return th_movt(ops[0].reg, ops[1].e.v);
}

uint32_t thumb_parse_special_register(int token)
{
  char buffer[64] = {0};
  const char *regstr = get_tok_str(token, NULL);
  const uint32_t reglen = strlen(regstr);
  for (int i = 0; i < reglen && i < sizeof(buffer) - 1; i++)
  {
    buffer[i] = tolower(regstr[i]);
  }
  if (strstr(buffer, "iapsr") != NULL)
  {
    return 0x01;
  }
  else if (strstr(buffer, "eapsr") != NULL)
  {
    return 0x02;
  }
  else if (strstr(buffer, "xpsr") != NULL)
  {
    return 0x03;
  }
  else if (strstr(buffer, "ipsr") != NULL)
  {
    return 0x05;
  }
  else if (strstr(buffer, "iepsr") != NULL)
  {
    return 0x07;
  }
  else if (strstr(buffer, "epsr") != NULL)
  {
    return 0x06;
  }
  else if (strstr(buffer, "apsr") != NULL)
  {
    return 0x00;
  }
  else if (strstr(buffer, "msplim_ns") != NULL)
  {
    return 0x8a;
  }
  else if (strstr(buffer, "psplim_ns") != NULL)
  {
    return 0x8b;
  }
  else if (strstr(buffer, "msplim") != NULL)
  {
    return 0x0a;
  }
  else if (strstr(buffer, "psplim") != NULL)
  {
    return 0x0b;
  }
  else if (strstr(buffer, "msp_ns") != NULL)
  {
    return 0x88;
  }
  else if (strstr(buffer, "psp_ns") != NULL)
  {
    return 0x89;
  }
  else if (strstr(buffer, "msp") != NULL)
  {
    return 0x08;
  }
  else if (strstr(buffer, "psp") != NULL)
  {
    return 0x09;
  }
  else if (strstr(buffer, "primask_ns") != NULL)
  {
    return 0x90;
  }
  else if (strstr(buffer, "basepri_ns") != NULL)
  {
    return 0x91;
  }
  else if (strstr(buffer, "faultmask_ns") != NULL)
  {
    return 0x93;
  }
  else if (strstr(buffer, "control_ns") != NULL)
  {
    return 0x94;
  }
  else if (strstr(buffer, "sp_ns") != NULL)
  {
    return 0x98;
  }
  else if (strstr(buffer, "primask") != NULL)
  {
    return 0x10;
  }
  else if (strstr(buffer, "basepri") != NULL)
  {
    return 0x11;
  }
  else if (strstr(buffer, "basepri_max") != NULL)
  {
    return 0x12;
  }
  else if (strstr(buffer, "faultmask") != NULL)
  {
    return 0x13;
  }
  else if (strstr(buffer, "control") != NULL)
  {
    return 0x14;
  }
  return 0xff;
}

uint32_t thumb_parse_special_register_mask(int token)
{
  char buffer[64] = {0};
  const char *regstr = get_tok_str(token, NULL);
  const uint32_t reglen = strlen(regstr);
  for (int i = 0; i < reglen && i < sizeof(buffer) - 1; i++)
  {
    buffer[i] = tolower(regstr[i]);
  }

  if (strstr(buffer, "_nzcvqg") != NULL)
  {
    return 0x3;
  }
  else if (strstr(buffer, "_nzcvq") != NULL)
  {
    return 0x2;
  }
  else if (strstr(buffer, "_g") != NULL)
  {
    return 0x1;
  }
  return 0x2;
}
static thumb_opcode thumb_mrs_opcode(TCCState *s1, int token)
{
  Operand op;
  uint32_t specreg = 0;
  parse_operand(s1, &op);
  skip(',');
  specreg = thumb_parse_special_register(tok);
  next();

  return th_mrs(op.reg, specreg);
}

static thumb_opcode thumb_msr_opcode(TCCState *s1, int token)
{
  Operand op;
  uint32_t specreg = 0;
  uint32_t mask = 0;
  specreg = thumb_parse_special_register(tok);
  mask = thumb_parse_special_register_mask(tok);
  next();
  skip(',');
  parse_operand(s1, &op);
  return th_msr(specreg, op.reg, mask);
}

static thumb_opcode thumb_control_opcode(TCCState *s1, int token)
{
  thumb_enforce_encoding encoding = ENFORCE_ENCODING_NONE;
  if (THUMB_HAS_WIDE_QUALIFIER(token))
  {
    encoding = ENFORCE_ENCODING_32BIT;
  }
  switch (THUMB_INSTRUCTION_GROUP(token))
  {
  case TOK_ASM_nopeq:
    return th_nop(encoding);
  case TOK_ASM_seveq:
    return th_sev(encoding);
  case TOK_ASM_wfeeq:
    return th_wfe(encoding);
  case TOK_ASM_wfieq:
    return th_wfi(encoding);
  case TOK_ASM_yieldeq:
    return th_yield(encoding);
  };
  return (thumb_opcode){0, 0};
}

static void thumb_data_processing_opcode(TCCState *s1, int token)
{
  Operand ops[3];
  int nb_ops;
  thumb_shift shift = {0, 0};
  thumb_opcode opcode;

  nb_ops = process_operands(s1, sizeof(ops) / sizeof(ops[0]), ops);
  if (nb_ops < 2)
  {
    expect("at least two operands");
    return;
  }
  else if (nb_ops == 2)
  {
    memcpy(&ops[2], &ops[1], sizeof(ops[1]));
    memcpy(&ops[1], &ops[0],
           sizeof(ops[0])); // most instructions may have implicit destination
                            // register
    nb_ops = 3;
  }
  shift = asm_parse_optional_shift(s1);

  if (ops[0].type != OP_REG32)
  {
    expect("first operand must be a register");
  }

  // alias for adr
  if (THUMB_INSTRUCTION_GROUP(token) == TOK_ASM_addeq && ops[1].reg == R_PC)
  {
    thumb_enforce_encoding encoding = ENFORCE_ENCODING_NONE;
    if (!thumb_operand_is_immediate(ops[2].type))
    {
      expect("second operand must be an immediate for adr");
    }
    if (THUMB_HAS_WIDE_QUALIFIER(token))
    {
      encoding = ENFORCE_ENCODING_32BIT;
    }
    return thumb_emit_opcode(th_adr_imm(ops[0].reg, ops[2].e.v, encoding));
  };

  opcode = thumb_generate_opcode_for_data_processing(token, shift, ops);
  thumb_emit_opcode(opcode);
}

static thumb_opcode thumb_data_shift_opcode(TCCState *s1, int token)
{
  Operand ops[3];
  int nb_ops;
  thumb_flags_behaviour flags = FLAGS_BEHAVIOUR_BLOCK;
  thumb_enforce_encoding encoding = ENFORCE_ENCODING_NONE;
  const bool in_it_block = thumb_conditional_scope > 0;
  thumb_shift shift = {0, 0, 0};
  bool token_svariant = false;

  nb_ops = process_operands(s1, sizeof(ops) / sizeof(ops[0]), ops);

  if (nb_ops == 1)
  {
    memcpy(&ops[2], &ops[0], sizeof(ops[1]));
    memcpy(&ops[1], &ops[0],
           sizeof(ops[0])); // most instructions may have implicit destination
                            // register
    nb_ops = 3;
  }
  else if (nb_ops == 2)
  {
    memcpy(&ops[2], &ops[1], sizeof(ops[1]));
    memcpy(&ops[1], &ops[0],
           sizeof(ops[0])); // most instructions may have implicit destination
                            // register
    nb_ops = 3;
  }

  if (!thumb_operand_is_register(ops[0].type) || !thumb_operand_is_register(ops[1].type))
  {
    expect("First two operands must be registers for shift instructions");
  }

  if (THUMB_HAS_WIDE_QUALIFIER(token))
  {
    encoding = ENFORCE_ENCODING_32BIT;
  }

  switch (THUMB_INSTRUCTION_GROUP(token))
  {
  case TOK_ASM_asrseq:
  case TOK_ASM_rorseq:
  case TOK_ASM_lslseq:
  case TOK_ASM_lsrseq:
  case TOK_ASM_rrxseq:
    token_svariant = true;
  };

  switch (THUMB_INSTRUCTION_GROUP(token))
  {
  case TOK_ASM_asrseq:
  case TOK_ASM_asreq:
  {
    shift.type = THUMB_SHIFT_ASR;
  }
  break;
  case TOK_ASM_lslseq:
  case TOK_ASM_lsleq:
  {
    shift.type = THUMB_SHIFT_LSL;
  }
  break;
  case TOK_ASM_lsrseq:
  case TOK_ASM_lsreq:
  {
    shift.type = THUMB_SHIFT_LSR;
  }
  break;
  case TOK_ASM_rorseq:
  case TOK_ASM_roreq:
  {
    shift.type = THUMB_SHIFT_ROR;
  }
  break;
  case TOK_ASM_rrxseq:
  case TOK_ASM_rrxeq:
  {
    shift.type = THUMB_SHIFT_RRX;
    shift.value = 0;
  }
  break;
  }

  if (token_svariant)
  {
    if (thumb_conditional_scope > 0)
    {
      tcc_error("cannot use '%s' in IT block", get_tok_str(token, NULL));
    }
    flags = FLAGS_BEHAVIOUR_SET;
  }
  else if (thumb_conditional_scope > 0)
  {
    flags = FLAGS_BEHAVIOUR_NOT_IMPORTANT;
  }

  if (shift.type == THUMB_SHIFT_RRX)
  {
    shift.value = 0;
    shift.mode = THUMB_SHIFT_IMMEDIATE;
    ops[1].reg = ops[2].reg;
  }
  else if (thumb_operand_is_immediate(ops[2].type))
  {
    shift.value = ops[2].e.v;
    shift.mode = THUMB_SHIFT_IMMEDIATE;
  }
  else
  {
    shift.value = ops[2].reg;
    shift.mode = THUMB_SHIFT_REGISTER;
  }

  if (!token_svariant && thumb_conditional_scope == 0)
  {
    encoding = ENFORCE_ENCODING_32BIT;
  }

  return th_mov_reg(ops[0].reg, ops[1].reg, flags, shift, encoding, in_it_block);
}

static void thumb_process_control(TCCState *s1, int token)
{
  Operand op;
  thumb_opcode opcode;
  thumb_enforce_encoding encoding = ENFORCE_ENCODING_NONE;
  int nb_ops = process_operands(s1, 1, &op);
  if (nb_ops > 1 || nb_ops == 0)
  {
    expect("one operand");
    return;
  }
  if (op.type != OP_IM8 && op.type != OP_IM32 && op.type != OP_IM8N)
  {
    expect("operand must be an immediate");
    return;
  }
  if (THUMB_HAS_WIDE_QUALIFIER(token))
  {
    encoding = ENFORCE_ENCODING_32BIT;
  }

  switch (THUMB_INSTRUCTION_GROUP(token))
  {
  case TOK_ASM_svceq:
    opcode = th_svc(op.e.v);
    break;
  case TOK_ASM_bkpteq:
    opcode = th_bkpt(op.e.v);
    break;
  case TOK_ASM_udfeq:
    opcode = th_udf(op.e.v, encoding);
    break;
  }

  thumb_emit_opcode(opcode);
}

static void thumb_branch(TCCState *s1, int token)
{
  int jump_addr = 0;
  Operand op;
  ExprValue e;
  ElfSym *esym;
  int condition = 0xe;
  bool must_use_t4 = false;
  bool must_use_32bit = false;
  int sign = 0;
  if (THUMB_HAS_WIDE_QUALIFIER(token))
  {
    must_use_32bit = true;
  }

  if (THUMB_INSTRUCTION_GROUP(token) == TOK_ASM_cbzeq || THUMB_INSTRUCTION_GROUP(token) == TOK_ASM_cbnzeq)
  {
    process_operands(s1, 1, &op);
  }

  if (THUMB_INSTRUCTION_GROUP(token) == TOK_ASM_beq || THUMB_INSTRUCTION_GROUP(token) == TOK_ASM_bleq ||
      THUMB_INSTRUCTION_GROUP(token) == TOK_ASM_cbzeq || THUMB_INSTRUCTION_GROUP(token) == TOK_ASM_cbnzeq)
  {
    asm_expr(s1, &e);
    if (e.sym)
    {
      esym = elfsym(e.sym);
      if (esym && esym->st_shndx == cur_text_section->sh_num)
      {
        /* Strip thumb bit from the fully computed target (GAS does this for B/BL).
           Otherwise we can end up with an odd offset and the short encoding rejects it. */
        int target = (e.v + esym->st_value) & ~1;
        jump_addr = th_encbranch(ind, target);
      }
      else
      {
        if (THUMB_INSTRUCTION_GROUP(token) == TOK_ASM_cbzeq || THUMB_INSTRUCTION_GROUP(token) == TOK_ASM_cbnzeq)
        {
          greloca(cur_text_section, e.sym, ind, R_ARM_THM_JUMP6, 0);
        }
        else
        {
          greloca(cur_text_section, e.sym, ind, R_ARM_THM_PC22, 0);
        }
        must_use_t4 = true;
        jump_addr = th_encbranch(ind, (ind + e.v) & ~1);
      }
    }
  }
  else
  {
    process_operands(s1, 1, &op);
  }

  condition = THUMB_GET_CONDITION(token);

  switch (THUMB_INSTRUCTION_GROUP(token))
  {
  case TOK_ASM_bxeq:
  {
    if (!thumb_operand_is_register(op.type))
    {
      expect("first operand must be a register");
    }
    return thumb_emit_opcode(th_bx_reg(op.reg));
  }
  case TOK_ASM_blxeq:
  {
    if (!thumb_operand_is_register(op.type))
    {
      expect("first operand must be a register");
    }
    return thumb_emit_opcode(th_blx_reg(op.reg));
  }
  case TOK_ASM_bleq:
    return thumb_emit_opcode(th_bl_t1(jump_addr));
  case TOK_ASM_beq:
  {
    if (must_use_t4)
    {
      return thumb_emit_opcode(th_b_t4(jump_addr));
    }

    if (jump_addr >= -2048 && jump_addr <= 2046 && !(jump_addr & 1) && !must_use_32bit &&
        (condition == 0xe || thumb_conditional_scope > 0))
    {
      thumb_opcode opcode = th_b_t2(jump_addr);
      if (opcode.size)
        return thumb_emit_opcode(opcode);
      /* If the short encoding can't be formed (e.g. odd offset), fall back. */
      return thumb_emit_opcode(th_b_t4(jump_addr & ~1));
    }
    else if (jump_addr >= -256 && jump_addr <= 254 && !(jump_addr & 1) && thumb_conditional_scope == 0 &&
             !must_use_32bit)
    {
      return thumb_emit_opcode(th_b_t1(condition, jump_addr >> 1));
    }
    else if (jump_addr >= -16777216 && jump_addr <= 16777214 && (condition == 0xe || thumb_conditional_scope > 0))
    {
      return thumb_emit_opcode(th_b_t4(jump_addr));
    }
    else if (jump_addr >= -1048576 && jump_addr <= 1048574 && thumb_conditional_scope == 0)
    {
      return thumb_emit_opcode(th_b_t3(condition, jump_addr >> 1));
    }
    else
    {
      tcc_error("branch target out of range: %d", jump_addr);
    }
  }
  case TOK_ASM_cbnzeq:
    sign = 1;
  case TOK_ASM_cbzeq:
  {
    if (!thumb_operand_is_register(op.type))
    {
      expect("first operand must be a register");
    }
    return thumb_emit_opcode(th_cbz(op.reg, 0, sign));
  }
  default:
    tcc_error("unknown branch instruction: %s", get_tok_str(token, NULL));
  }
}

ST_FUNC void asm_opcode(TCCState *s1, int token)
{
  while (token == TOK_LINEFEED)
  {
    next();
    token = tok;
  }
  if (token == TOK_EOF)
    return;

  if (token >= TOK_ASM_it && token <= TOK_ASM_iteee)
  {
    thumb_conditional_opcode(s1, token);
    return;
  }

  if (thumb_conditional_scope > 0)
    --thumb_conditional_scope;

  const char *token_str = get_tok_str(token, NULL);
  if (strncmp(token_str, "vmov", 4) == 0)
  {
    thumb_emit_opcode(thumb_vmov_opcode(s1, token));
    return;
  }
  if (strncmp(token_str, "vadd", 4) == 0 || strncmp(token_str, "vsub", 4) == 0 || strncmp(token_str, "vmul", 4) == 0 ||
      strncmp(token_str, "vdiv", 4) == 0 || strncmp(token_str, "vneg", 4) == 0)
  {
    thumb_emit_opcode(thumb_vfp_arith_opcode(s1, token));
    return;
  }
  if (strncmp(token_str, "vcmp", 4) == 0)
  {
    thumb_emit_opcode(thumb_vcmp_opcode(s1, token));
    return;
  }
  if (strncmp(token_str, "vmrs", 4) == 0)
  {
    thumb_emit_opcode(thumb_vmrs_opcode(s1, token));
    return;
  }
  if (strncmp(token_str, "vcvt", 4) == 0)
  {
    thumb_emit_opcode(thumb_vcvt_opcode(s1, token));
    return;
  }
  switch (THUMB_INSTRUCTION_GROUP(token))
  {
  case TOK_ASM_bxeq:
  case TOK_ASM_bleq:
  case TOK_ASM_blxeq:
  case TOK_ASM_beq:
  case TOK_ASM_cbzeq:
  case TOK_ASM_cbnzeq:
    return thumb_branch(s1, token);
  case TOK_ASM_adceq:
  case TOK_ASM_adcseq:
  case TOK_ASM_addeq:
  case TOK_ASM_addseq:
  case TOK_ASM_addweq:
  case TOK_ASM_andeq:
  case TOK_ASM_andseq:
  case TOK_ASM_movseq:
  case TOK_ASM_movweq:
  case TOK_ASM_moveq:
  case TOK_ASM_cmpeq:
  case TOK_ASM_bfceq:
  case TOK_ASM_biceq:
  case TOK_ASM_bicseq:
  case TOK_ASM_clzeq:
  case TOK_ASM_cmneq:
  case TOK_ASM_eoreq:
  case TOK_ASM_eorseq:
  case TOK_ASM_muleq:
  case TOK_ASM_mulseq:
  case TOK_ASM_mvneq:
  case TOK_ASM_mvnseq:
  case TOK_ASM_orneq:
  case TOK_ASM_ornseq:
  case TOK_ASM_orreq:
  case TOK_ASM_orrseq:
  case TOK_ASM_rbiteq:
  case TOK_ASM_reveq:
  case TOK_ASM_rev16eq:
  case TOK_ASM_revsheq:
  case TOK_ASM_rsbeq:
  case TOK_ASM_rsbseq:
  case TOK_ASM_sbceq:
  case TOK_ASM_sbcseq:
  case TOK_ASM_sdiveq:
  case TOK_ASM_subeq:
  case TOK_ASM_subseq:
  case TOK_ASM_subweq:
  case TOK_ASM_sxtbeq:
  case TOK_ASM_sxtheq:
  case TOK_ASM_teqeq:
  case TOK_ASM_tsteq:
  case TOK_ASM_udiveq:
  case TOK_ASM_uxtbeq:
  case TOK_ASM_uxtheq:
    return thumb_data_processing_opcode(s1, token);
  case TOK_ASM_adreq:
    return thumb_adr_opcode(s1, token);
  case TOK_ASM_svceq:
  case TOK_ASM_bkpteq:
  case TOK_ASM_udfeq:
    return thumb_process_control(s1, token);
  case TOK_ASM_asreq:
  case TOK_ASM_asrseq:
  case TOK_ASM_lsleq:
  case TOK_ASM_lslseq:
  case TOK_ASM_lsreq:
  case TOK_ASM_lsrseq:
  case TOK_ASM_roreq:
  case TOK_ASM_rorseq:
  case TOK_ASM_rrxeq:
  case TOK_ASM_rrxseq:
    return thumb_emit_opcode(thumb_data_shift_opcode(s1, token));
  case TOK_ASM_bfieq:
  case TOK_ASM_sbfxeq:
    return thumb_emit_opcode(thumb_bitmanipulation_opcode(s1, token));
  case TOK_ASM_clrexeq:
    return thumb_emit_opcode(th_clrex());
  case TOK_ASM_cpsideq:
    return thumb_cps_opcode(0);
  case TOK_ASM_cpsieeq:
    return thumb_cps_opcode(1);
  case TOK_ASM_csdbeq:
    return thumb_emit_opcode(th_csdb());
  case TOK_ASM_dmbeq:
  case TOK_ASM_isbeq:
    return thumb_synchronization_barrier_opcode(token);
  case TOK_ASM_dsbeq:
    return thumb_dsb_opcode();
  case TOK_ASM_ldaeq:
  case TOK_ASM_ldabeq:
  case TOK_ASM_ldaexeq:
  case TOK_ASM_ldaexbeq:
  case TOK_ASM_ldaexheq:
  case TOK_ASM_ldaheq:
  case TOK_ASM_ldreq:
  case TOK_ASM_ldrbeq:
  case TOK_ASM_ldrbteq:
  case TOK_ASM_ldrdeq:
  case TOK_ASM_ldrexeq:
  case TOK_ASM_ldrexbeq:
  case TOK_ASM_ldrexheq:
  case TOK_ASM_ldrheq:
  case TOK_ASM_ldrhteq:
  case TOK_ASM_ldrsbeq:
  case TOK_ASM_ldrsbteq:
  case TOK_ASM_ldrsheq:
  case TOK_ASM_ldrshteq:
  case TOK_ASM_ldrteq:
  case TOK_ASM_stleq:
  case TOK_ASM_stlbeq:
  case TOK_ASM_stlexeq:
  case TOK_ASM_stlexbeq:
  case TOK_ASM_stlexheq:
  case TOK_ASM_stlheq:
  case TOK_ASM_streq:
  case TOK_ASM_strbeq:
  case TOK_ASM_strbteq:
  case TOK_ASM_strdeq:
  case TOK_ASM_strexeq:
  case TOK_ASM_strexbeq:
  case TOK_ASM_strexheq:
  case TOK_ASM_strheq:
  case TOK_ASM_strhteq:
  case TOK_ASM_strteq:
    return thumb_emit_opcode(thumb_single_memory_transfer_opcode(s1, token));
  case TOK_ASM_pldeq:
  case TOK_ASM_pldweq:
  case TOK_ASM_plieq:
  case TOK_ASM_pliweq:
  case TOK_ASM_tbbeq:
  case TOK_ASM_tbheq:
    return thumb_emit_opcode(thumb_cache_preload_opcode(s1, token));
  case TOK_ASM_ldmeq:
  case TOK_ASM_ldmfdeq:
  case TOK_ASM_ldmiaeq:
  case TOK_ASM_ldmdbeq:
  case TOK_ASM_ldmeaeq:
  case TOK_ASM_stmeq:
  case TOK_ASM_stmiaeq:
  case TOK_ASM_stmeaeq:
  case TOK_ASM_stmdbeq:
  case TOK_ASM_stmfdeq:
    return thumb_block_memory_transfer_opcode(s1, token);
  case TOK_ASM_mlaeq:
  case TOK_ASM_smlaleq:
  case TOK_ASM_smulleq:
  case TOK_ASM_umlaleq:
  case TOK_ASM_mlseq:
  case TOK_ASM_umulleq:
    return thumb_emit_opcode(thumb_math_opcode(s1, token));
  case TOK_ASM_movteq:
    return thumb_emit_opcode(thumb_movt_opcode(s1, token));
  case TOK_ASM_mrseq:
    return thumb_emit_opcode(thumb_mrs_opcode(s1, token));
  case TOK_ASM_msreq:
    return thumb_emit_opcode(thumb_msr_opcode(s1, token));
  case TOK_ASM_nopeq:
  case TOK_ASM_seveq:
  case TOK_ASM_wfeeq:
  case TOK_ASM_wfieq:
  case TOK_ASM_yieldeq:
    return thumb_emit_opcode(thumb_control_opcode(s1, token));
  case TOK_ASM_pkhbteq:
  case TOK_ASM_pkhtbeq:
    return thumb_emit_opcode(thumb_pkhbt_opcode(s1, token));
  case TOK_ASM_popeq:
  case TOK_ASM_pusheq:
    return thumb_emit_opcode(thumb_pushpop_opcode(s1, token));
  case TOK_ASM_ssateq:
  case TOK_ASM_usateq:
    return thumb_emit_opcode(thumb_ssat_opcode(s1, token));
  case TOK_ASM_ssbbeq:
    return thumb_emit_opcode(th_ssbb());
  case TOK_ASM_tteq:
  case TOK_ASM_ttteq:
  case TOK_ASM_ttaeq:
  case TOK_ASM_ttateq:
    return thumb_emit_opcode(thumb_tt(s1, token));
  case TOK_ASM_vpusheq:
  case TOK_ASM_vpopeq:
    return thumb_emit_opcode(thumb_vpushvpop_opcode(s1, token));
  default:
    printf("asm_opcode: unknown token %s\n", get_tok_str(token, NULL));
    expect("known instruction");
  }
}

/*************************************************************/
