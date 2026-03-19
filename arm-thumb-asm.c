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

#include "arm-thumb-opcodes.h"
#include "tcc.h"
#include "tccir.h"

/* Forward declarations for MOP-based load/store from arm-thumb-gen.c */
void tcc_gen_mach_load_to_reg(int dest_reg, const MachineOperand *op);
void tcc_gen_mach_store_from_reg(int src_reg, const MachineOperand *op);

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
  /* During dry-run, don't write to section data, just track position */
  if (tcc_gen_machine_dry_run_is_active())
  {
    ind++;
    return;
  }
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
  /* During dry-run, don't write to section data, just track position */
  if (tcc_gen_machine_dry_run_is_active())
  {
    ind += 4;
    return;
  }
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
    {
      gen_le16(0xe92d);       /* STMDB SP!, first halfword */
      gen_le16(saved_regset); /* register list second halfword */
    }

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
          /* Convert LLOCAL stack slot to a pointer in a LOCAL stack slot.
            This matches the old SValue rewrite to VT_LOCAL|VT_LVAL with VT_PTR type. */
          IROperand src = svalue_to_iroperand(tcc_state->ir, op->vt);
          src.is_llocal = 0;
          src.is_lval = 1;
          src.btype = IROP_BTYPE_INT32; /* pointers are 32-bit on ARMv8-M */
          MachineOperand mop = machine_op_from_ir(tcc_state->ir, &src);
          tcc_gen_mach_load_to_reg(op->reg, &mop);
        }
        else if (i >= nb_outputs || op->is_rw)
        { // not write-only
          /* load value in register */
          IROperand src = svalue_to_iroperand(tcc_state->ir, op->vt);
          MachineOperand mop = machine_op_from_ir(tcc_state->ir, &src);
          tcc_gen_mach_load_to_reg(op->reg, &mop);
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
            IROperand ir_op = svalue_to_iroperand(tcc_state->ir, op->vt);

            /* Load pointer from LOCAL stack slot into out_reg.
               Change LLOCAL->LOCAL and set btype to PTR (INT32). */
            IROperand addr = ir_op;
            addr.is_llocal = 0;
            addr.btype = IROP_BTYPE_INT32;
            MachineOperand addr_mop = machine_op_from_ir(tcc_state->ir, &addr);
            tcc_gen_mach_load_to_reg(out_reg, &addr_mop);

            /* Store op->reg through the pointer now in out_reg */
            MachineOperand store_mop;
            memset(&store_mop, 0, sizeof(store_mop));
            store_mop.kind = MACH_OP_REG;
            store_mop.btype = irop_get_btype(ir_op);
            store_mop.is_unsigned = ir_op.is_unsigned;
            store_mop.u.reg.r0 = out_reg;
            store_mop.u.reg.r1 = -1;
            store_mop.needs_deref = true;
            tcc_gen_mach_store_from_reg(op->reg, &store_mop);
          }
        }
        else
        {
          IROperand ir_op = svalue_to_iroperand(tcc_state->ir, op->vt);
          MachineOperand mop = machine_op_from_ir(tcc_state->ir, &ir_op);
          tcc_gen_mach_store_from_reg(op->reg, &mop);
          if (op->is_llong)
            tcc_error("long long not implemented");
        }
      }
    }

    /* generate reg restore code */
    if (saved_regset)
    {
      gen_le16(0xe8bd);       /* LDMIA SP!, first halfword */
      gen_le16(saved_regset); /* register list second halfword */
    }
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
    case ',':
      continue;
    case 'l': // in ARM mode, that's  an alias for 'r' [ARM].
    case 'r': // register [general]
    case 'p': // valid memory address for load,store [general]
      pr = 3;
      break;
    case 'M': // integer constant for shifts [ARM]
    case 'I': // integer valid for data processing instruction immediate
    case 'J': // integer in range -4095...4095
    case 'n': // immediate integer operand with a known numeric value

    case 'i': // immediate integer operand, including symbolic constants
    case 's': // immediate integer operand whose value is not an explicit integer
              // [general]
    case 'Q': // memory reference with a single base register [ARM]
    case 'm': // memory operand [general]
    case 'g': // general-purpose-register, memory, immediate integer [general]
    case 'X': // any operand whatsoever [general]
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
                                     const uint8_t *reserved_regs, int *pout_reg)
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
  /* Also mark registers reserved by the IR register allocator (live variables).
   * These are NOT clobbered (no save/restore in asm_gen_code), but should not be
   * picked by the constraint solver for "r" operand allocation. */
  if (reserved_regs)
  {
    for (i = 0; i < NB_ASM_REGS; i++)
    {
      if (reserved_regs[i])
        regs_allocated[i] |= REG_IN_MASK | REG_OUT_MASK;
    }
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
    case ',':
      goto try_next;
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
    case 'n': // immediate integer operand with a known numeric value
    case 'i': // immediate integer operand, including symbolic constants
    case 's': // immediate integer operand whose value is not an explicit integer
      if (!((op->vt->r & (VT_VALMASK | VT_LVAL)) == VT_CONST))
        goto try_next;
      break;
    case 'M': // integer in the range 0 to 32
      if (!((op->vt->r & (VT_VALMASK | VT_LVAL | VT_SYM)) == VT_CONST))
        goto try_next;
      break;
    case 'Q': // simple memory operand [ARM]
    case 'm': // memory operand
    case 'g':
    case 'X':
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

/* ========================================================================
 * Assembly Suffix Parsing - Global state for runtime suffix parsing
 * ======================================================================== */

/* Condition code name to enum mapping table - global definition */
/* Note: Must match extern declaration in arm-thumb-defs.h */
const cond_name_entry_t cond_names[] = {
    {"eq", 0},  /* COND_EQ */
    {"ne", 1},  /* COND_NE */
    {"cs", 2},  /* COND_CS */
    {"hs", 2},  /* Alias for carry set */
    {"cc", 3},  /* COND_CC */
    {"lo", 3},  /* Alias for carry clear */
    {"mi", 4},  /* COND_MI */
    {"pl", 5},  /* COND_PL */
    {"vs", 6},  /* COND_VS */
    {"vc", 7},  /* COND_VC */
    {"hi", 8},  /* COND_HI */
    {"ls", 9},  /* COND_LS */
    {"ge", 10}, /* COND_GE */
    {"lt", 11}, /* COND_LT */
    {"gt", 12}, /* COND_GT */
    {"le", 13}, /* COND_LE */
    {"al", 14}, /* COND_AL */
    {NULL, 14}, /* Default/unconditional terminator */
};

/* Global state for current assembly instruction suffix */
static thumb_asm_suffix current_asm_suffix __attribute__((unused)) = {
    .condition = COND_AL,
    .width = WIDTH_NONE,
    .has_suffix = 0,
};

/* ========================================================================
 * Helper macros to maintain compatibility during transition
 * ======================================================================== */
#define THUMB_GET_CONDITION_FROM_STATE() (current_asm_suffix.condition)
#define THUMB_HAS_WIDE_QUALIFIER_FROM_STATE() (current_asm_suffix.width == WIDTH_WIDE)
#define THUMB_HAS_NARROW_QUALIFIER_FROM_STATE() (current_asm_suffix.width == WIDTH_NARROW)

/* ========================================================================
 * Parse ARM assembly instruction suffix
 * Input:  token_str - full token string (e.g., "addeq.w")
 * Output: suffix - parsed condition and width qualifier
 * Returns: Length of suffix portion (0 if no suffix)
 * ======================================================================== */
static int __attribute__((unused)) parse_asm_suffix(const char *token_str, thumb_asm_suffix *suffix)
{
  const char *p = token_str;
  int suffix_len = 0;

  suffix->condition = COND_AL; /* Default: always */
  suffix->width = WIDTH_NONE;
  suffix->has_suffix = 0;

  /* Skip base instruction name (it's all letters until we hit something else) */
  while (*p && isalpha(*p))
    p++;

  /* Check for condition code suffix */
  if (*p == '\0')
  {
    /* No suffix at all */
    return 0;
  }

  /* Try to match condition code */
  for (int i = 0; i < COND_NAMES_COUNT; i++)
  {
    size_t cond_len = strlen(cond_names[i].name);
    if (strncmp(p, cond_names[i].name, cond_len) == 0)
    {
      suffix->condition = cond_names[i].code;
      suffix->has_suffix = 1;
      p += cond_len;
      suffix_len += cond_len;
      break;
    }
  }

  /* Check for width qualifier (.w, .n, ._) */
  if (*p == '.')
  {
    suffix->has_suffix = 1;
    p++; /* Skip dot */
    suffix_len++;

    if (strncmp(p, "w", 1) == 0 || strncmp(p, "W", 1) == 0)
    {
      suffix->width = WIDTH_WIDE;
      p++;
      suffix_len++;
    }
    else if (strncmp(p, "n", 1) == 0 || strncmp(p, "N", 1) == 0)
    {
      suffix->width = WIDTH_NARROW;
      p++;
      suffix_len++;
    }
    else if (*p == '_')
    {
      suffix->width = WIDTH_RESERVED;
      p++;
      suffix_len++;
    }
  }

  return suffix_len;
}

/* ========================================================================
 * Extract base instruction name from token
 * Input:  token_str - full token string (e.g., "addeq.w")
 * Output: base_buf - buffer to store base name
 *         base_buf_size - size of base_buf
 * Returns: Length of base name
 * ======================================================================== */
static int __attribute__((unused)) get_base_instruction_name(const char *token_str, char *base_buf, int base_buf_size)
{
  const char *p = token_str;
  int len = 0;
  int token_len = strlen(token_str);

  /* Check for width qualifier first (.w, .n, ._) */
  int width_pos = token_len;
  for (int i = 0; i < token_len; i++)
  {
    if (token_str[i] == '.')
    {
      width_pos = i;
      break;
    }
  }

  /* Check for condition code before width qualifier */
  /* Condition codes are always 2 characters (eq, ne, cs, etc.) */
  /* Important: Only strip condition codes if the base is long enough to be valid */
  /* Most ARM base instructions are at least 3 characters (add, mov, sub, etc.) */
  /* Valid 1-char bases: "b" (branch) */
  /* Valid 2-char bases: "bx" (branch and exchange), "cbz", "cbnz" */
  static const char *valid_2char_bases[] = {"bx", "bl", NULL};
  int condition_pos = width_pos;
  if (width_pos >= 3)
  { /* Need at least 1 char for base + 2 for condition code */
    /* Check if the last 2 alphabetic chars before width qualifier form a condition code */
    for (int i = 0; i < COND_NAMES_COUNT; i++)
    {
      size_t cond_len = strlen(cond_names[i].name);
      if (width_pos >= (int)cond_len && strncmp(token_str + width_pos - cond_len, cond_names[i].name, cond_len) == 0)
      {
        /* Found a condition code - check if stripping it leaves a valid base instruction */
        int candidate_len = width_pos - cond_len;
        /* Check if candidate base is valid */
        int valid_base = 0;
        if (candidate_len == 1 && token_str[0] == 'b')
        {
          valid_base = 1; /* "b" is the only valid 1-char base */
        }
        else if (candidate_len == 2)
        {
          /* Check if it's one of the known valid 2-char bases */
          for (int j = 0; valid_2char_bases[j] != NULL; j++)
          {
            if (strncmp(token_str, valid_2char_bases[j], 2) == 0)
            {
              valid_base = 1;
              break;
            }
          }
        }
        else if (candidate_len >= 3)
        {
          valid_base = 1; /* 3+ chars is valid (add, mov, etc.) */
        }
        if (valid_base)
        {
          condition_pos = candidate_len;
          break;
        }
      }
    }
  }

  /* Copy base instruction name (before condition code and width qualifier) */
  int max_len = condition_pos;

  while (*p && isalnum(*p) && len < max_len && len < base_buf_size - 1)
  {
    base_buf[len++] = *p++;
  }
  base_buf[len] = '\0';

  return len;
}

/* ========================================================================
 * Parse assembly instruction token to extract base token and condition code
 * Input:  token - the token ID to parse
 * Output: base_token - receives the base instruction token ID (e.g., TOK_ASM_add)
 * Returns: The condition code (0-14 for eq/al, or -1 for AL/no suffix)
 * ======================================================================== */
ST_FUNC int thumb_parse_token_suffix(int token, int *base_token)
{
  const char *token_str = get_tok_str(token, NULL);
  char base_buf[32];
  int base_len;
  int condition = COND_AL; /* Default: always (no suffix) */

  /* Reset width qualifier */
  current_asm_suffix.width = WIDTH_NONE;

  if (!token_str)
  {
    *base_token = token;
    return COND_AL;
  }

  /* Extract base instruction name */
  base_len = get_base_instruction_name(token_str, base_buf, sizeof(base_buf));

  /* Look for condition code suffix */
  const char *p = token_str + base_len;

  /* Try to match condition code */
  for (int i = 0; i < COND_NAMES_COUNT; i++)
  {
    size_t cond_len = strlen(cond_names[i].name);
    if (strncmp(p, cond_names[i].name, cond_len) == 0)
    {
      condition = cond_names[i].code;
      p += cond_len;
      break;
    }
  }

  /* Parse width qualifier (.w, .n) after condition code */
  if (*p == '.')
  {
    p++;
    if (*p == 'w' || *p == 'W')
    {
      current_asm_suffix.width = WIDTH_WIDE;
    }
    else if (*p == 'n' || *p == 'N')
    {
      current_asm_suffix.width = WIDTH_NARROW;
    }
  }

  /* Find base token by looking up the base instruction name */
  *base_token = tok_alloc_const(base_buf);

  return condition;
}

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
  if (token == token_svariant)
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
    if ((token == data.regular_variant_token && thumb_conditional_scope == 0) || THUMB_HAS_WIDE_QUALIFIER_FROM_STATE())
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
  switch (token)
  {
  case TOK_ASM_dmb:
    op = th_dmb(fullsystem);
    break;
  case TOK_ASM_isb:
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
  if (THUMB_HAS_WIDE_QUALIFIER_FROM_STATE())
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
  if (THUMB_HAS_WIDE_QUALIFIER_FROM_STATE())
  {
    encoding = ENFORCE_ENCODING_32BIT;
  }

  switch (token)
  {
  case TOK_ASM_adcs:
  case TOK_ASM_adc:
  {
    return thumb_process_generic_data_op(
        (th_generic_op_data){
            .generate_imm_opcode = th_adc_imm,
            .generate_reg_opcode = th_adc_reg,
            .regular_variant_token = TOK_ASM_adc,
            .flags_variant_token = TOK_ASM_adcs,
        },
        token, shift, ops);
  }
  case TOK_ASM_ands:
  case TOK_ASM_and:
    return thumb_process_generic_data_op(
        (th_generic_op_data){
            .generate_imm_opcode = th_and_imm,
            .generate_reg_opcode = th_and_reg,
            .regular_variant_token = TOK_ASM_and,
            .flags_variant_token = TOK_ASM_ands,
        },
        token, shift, ops);
  case TOK_ASM_orns:
  case TOK_ASM_orn:
    return thumb_process_generic_data_op(
        (th_generic_op_data){
            .generate_imm_opcode = th_orn_imm,
            .generate_reg_opcode = th_orn_reg,
            .regular_variant_token = TOK_ASM_orn,
            .flags_variant_token = TOK_ASM_orns,
        },
        token, shift, ops);
  case TOK_ASM_orrs:
  case TOK_ASM_orr:
    return thumb_process_generic_data_op(
        (th_generic_op_data){
            .generate_imm_opcode = th_orr_imm,
            .generate_reg_opcode = th_orr_reg,
            .regular_variant_token = TOK_ASM_orr,
            .flags_variant_token = TOK_ASM_orrs,
        },
        token, shift, ops);
  case TOK_ASM_adds:
  case TOK_ASM_add:
  case TOK_ASM_addw:
  {
    thumb_flags_behaviour setflags = thumb_determine_flags_behaviour(token, TOK_ASM_adds, true);

    if (thumb_operand_is_immediate(ops[2].type))
    {
      if (ops[1].reg == R_SP)
      {
        if (token == TOK_ASM_addw)
        {
          return th_add_sp_imm_t4(ops[0].reg, ops[2].e.v, setflags, encoding);
        }
        return th_add_sp_imm(ops[0].reg, ops[2].e.v, setflags, encoding);
      }
      if (token == TOK_ASM_addw)
      {
        return th_add_imm_t4(ops[0].reg, ops[1].reg, ops[2].e.v);
      }

      if (token == TOK_ASM_add && thumb_conditional_scope == 0)
      {
        encoding = ENFORCE_ENCODING_32BIT;
      }
      if (token == TOK_ASM_adds && thumb_conditional_scope > 0)
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
  case TOK_ASM_bics:
  case TOK_ASM_bic:
  {
    return thumb_process_generic_data_op(
        (th_generic_op_data){
            .generate_imm_opcode = th_bic_imm,
            .generate_reg_opcode = th_bic_reg,
            .regular_variant_token = TOK_ASM_bic,
            .flags_variant_token = TOK_ASM_bics,
        },
        token, shift, ops);
  }
  case TOK_ASM_clz:
  {
    if (!thumb_operand_is_register(ops[1].type) || !(thumb_operand_is_register(ops[0].type)))
    {
      expect("operands must be registers");
    }
    return th_clz(ops[1].reg, ops[2].reg);
  }
  case TOK_ASM_cmp:
  {
    if (thumb_operand_is_immediate(ops[2].type))
    {
      return th_cmp_imm(0, ops[1].reg, ops[2].e.v, FLAGS_BEHAVIOUR_SET, encoding);
    }
    return th_cmp_reg(0, ops[1].reg, ops[2].reg, FLAGS_BEHAVIOUR_SET, shift, encoding);
  }
  case TOK_ASM_cmn:
  {
    thumb_enforce_encoding encoding = ENFORCE_ENCODING_NONE;

    if (thumb_operand_is_immediate(ops[2].type))
    {
      return th_cmn_imm(ops[1].reg, ops[2].e.v);
    }

    if (thumb_operand_is_register(ops[2].type))
    {
      if (THUMB_HAS_WIDE_QUALIFIER_FROM_STATE())
      {
        encoding = ENFORCE_ENCODING_32BIT;
      }
      return th_cmn_reg(ops[1].reg, ops[2].reg, shift, encoding);
    }
  }
  case TOK_ASM_eors:
  case TOK_ASM_eor:
  {
    return thumb_process_generic_data_op(
        (th_generic_op_data){
            .generate_imm_opcode = th_eor_imm,
            .generate_reg_opcode = th_eor_reg,
            .regular_variant_token = TOK_ASM_eor,
            .flags_variant_token = TOK_ASM_eors,
        },
        token, shift, ops);
  }
  case TOK_ASM_rsbs:
  case TOK_ASM_rsb:
  {
    return thumb_process_generic_data_op(
        (th_generic_op_data){
            .generate_imm_opcode = th_rsb_imm,
            .generate_reg_opcode = th_rsb_reg,
            .regular_variant_token = TOK_ASM_rsb,
            .flags_variant_token = TOK_ASM_rsbs,
        },
        token, shift, ops);
  }
  case TOK_ASM_mvns:
  case TOK_ASM_mvn:
  {
    return thumb_process_generic_data_op(
        (th_generic_op_data){
            .generate_imm_opcode = th_mvn_imm,
            .generate_reg_opcode = th_mvn_reg,
            .regular_variant_token = TOK_ASM_mvn,
            .flags_variant_token = TOK_ASM_mvns,
        },
        token, shift, ops);
  }
  case TOK_ASM_movs:
  case TOK_ASM_movw:
  case TOK_ASM_mov:
  {
    thumb_flags_behaviour setflags = thumb_determine_flags_behaviour(token, TOK_ASM_movs, false);
    if (token == TOK_ASM_movw)
      encoding = ENFORCE_ENCODING_32BIT;

    if (thumb_operand_is_immediate(ops[2].type))
    {
      return th_mov_imm(ops[1].reg, ops[2].e.v, setflags, encoding);
    }
    return th_mov_reg(ops[1].reg, ops[2].reg, setflags, shift, encoding, thumb_conditional_scope > 0);
  }
  case TOK_ASM_bfc:
  {
    if (!thumb_operand_is_immediate(ops[1].type) && !thumb_operand_is_immediate(ops[2].type))
    {
      expect("second/third operand must be an immediate");
    }
    return th_bfc(ops[0].reg, ops[1].e.v, ops[2].e.v);
  }
  case TOK_ASM_muls:
  case TOK_ASM_mul:
  {
    thumb_flags_behaviour setflags = thumb_determine_flags_behaviour(token, TOK_ASM_muls, false);
    uint32_t rm = ops[2].reg;
    uint32_t rn = ops[1].reg;
    if (ops[0].reg == ops[1].reg)
    {
      rm = ops[0].reg;
      rn = ops[2].reg;
    }
    return th_mul(ops[0].reg, rn, rm, setflags, encoding);
  }
  case TOK_ASM_sdiv:
    return th_sdiv(ops[0].reg, ops[1].reg, ops[2].reg);
  case TOK_ASM_rbit:
    return th_rbit(ops[1].reg, ops[2].reg);
  case TOK_ASM_rev:
    return th_rev(ops[1].reg, ops[2].reg, encoding);
  case TOK_ASM_rev16:
    return th_rev16(ops[1].reg, ops[2].reg, encoding);
  case TOK_ASM_revsh:
    return th_revsh(ops[1].reg, ops[2].reg, encoding);
  case TOK_ASM_sbcs:
  case TOK_ASM_sbc:
  {
    return thumb_process_generic_data_op(
        (th_generic_op_data){
            .generate_imm_opcode = th_sbc_imm,
            .generate_reg_opcode = th_sbc_reg,
            .regular_variant_token = TOK_ASM_sbc,
            .flags_variant_token = TOK_ASM_sbcs,
        },
        token, shift, ops);
  }
  case TOK_ASM_subs:
  case TOK_ASM_sub:
  case TOK_ASM_subw:
  {
    thumb_flags_behaviour setflags = thumb_determine_flags_behaviour(token, TOK_ASM_subs, true);

    if (thumb_operand_is_immediate(ops[2].type))
    {
      if (ops[1].reg == R_SP)
      {
        if (token == TOK_ASM_subw)
        {
          return th_sub_sp_imm_t3(ops[0].reg, ops[2].e.v, setflags, encoding);
        }
        return th_sub_sp_imm(ops[0].reg, ops[2].e.v, setflags, encoding);
      }
      if (token == TOK_ASM_subw)
      {
        return th_sub_imm_t4(ops[0].reg, ops[1].reg, ops[2].e.v);
      }

      if (token == TOK_ASM_sub && thumb_conditional_scope == 0)
      {
        encoding = ENFORCE_ENCODING_32BIT;
      }
      if (token == TOK_ASM_subs && thumb_conditional_scope > 0)
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
  case TOK_ASM_sxtb:
    return th_sxtb(ops[1].reg, ops[2].reg, shift, encoding);
  case TOK_ASM_sxth:
    return th_sxth(ops[1].reg, ops[2].reg, shift, encoding);
  case TOK_ASM_teq:
    return th_teq(ops[1].reg, ops[2].e.v);
  case TOK_ASM_tst:
    if (thumb_operand_is_register(ops[2].type))
      return th_tst_reg(ops[1].reg, ops[2].reg, shift, encoding);
    return th_tst_imm(ops[1].reg, ops[2].e.v);
  case TOK_ASM_udiv:
    return th_udiv(ops[0].reg, ops[1].reg, ops[2].reg);
  case TOK_ASM_uxtb:
    return th_uxtb(ops[1].reg, ops[2].reg, shift, encoding);
  case TOK_ASM_uxth:
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
  if (THUMB_HAS_WIDE_QUALIFIER_FROM_STATE())
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
      if (token == TOK_ASM_ldrd)
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
  switch (token)
  {
  case TOK_ASM_ldr:
    return th_ldr_imm(op0.reg, R_PC, jump_addr, puw, encoding);

  case TOK_ASM_ldrb:
    return th_ldrb_imm(op0.reg, R_PC, jump_addr, puw, encoding);
  case TOK_ASM_ldrd:
    return th_ldrd_imm(op0.reg, op1.reg, R_PC, jump_addr, puw, encoding);
  case TOK_ASM_ldrh:
    return th_ldrh_imm(op0.reg, R_PC, jump_addr, puw, encoding);
  case TOK_ASM_ldrsb:
    return th_ldrsb_imm(op0.reg, R_PC, jump_addr, puw, encoding);
  case TOK_ASM_ldrsh:
    return th_ldrsh_imm(op0.reg, R_PC, jump_addr, puw, encoding);
  case TOK_ASM_strd:
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
  switch (token)
  {
  case TOK_ASM_pld:
  {
    if (is_literal)
      return th_pld_literal(jump_addr);
    if (thumb_operand_is_register(ops[1].type))
    {
      return th_pld_reg(ops[0].reg, ops[1].reg, 0, shift);
    }
    return th_pld_imm(ops[0].reg, 0, ops[1].e.v);
  }
  case TOK_ASM_pli:
  {
    if (is_literal)
      return th_pli_literal(jump_addr);
    if (thumb_operand_is_register(ops[1].type))
    {
      return th_pli_reg(ops[0].reg, ops[1].reg, 0, shift);
    }
    return th_pli_imm(ops[0].reg, 0, ops[1].e.v);
  }
  case TOK_ASM_tbh:
    h = 1;
  case TOK_ASM_tbb:
    return th_tbb(ops[0].reg, ops[1].reg, h);
  }
  return (thumb_opcode){0, 0};
}

static void thumb_single_memory_transfer_opcode(TCCState *s1, int token)
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
  if (THUMB_HAS_WIDE_QUALIFIER_FROM_STATE())
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
  if (token == TOK_ASM_ldrd || token == TOK_ASM_stlex || token == TOK_ASM_stlexb || token == TOK_ASM_stlexh ||
      token == TOK_ASM_strd || token == TOK_ASM_strex || token == TOK_ASM_strexb || token == TOK_ASM_strexh)
  {
    parse_operand(s1, &op2reg);
    next();
  }
  if (tok != '[')
  {
    /* Literal addressing mode.
       Also support GAS-style: ldr Rt, =expr
       which loads the *value* of expr via an inline literal word.
       This differs from `ldr Rt, label` which loads from memory at `label`.
    */
    if (tok == '=' && token == TOK_ASM_ldr)
    {
      ExprValue e;
      int insn_pos = ind;
      int literal_pos;
      int aligned_insn_pos;
      int jump_addr;
      int branch_pos;
      int literal_end;
      int branch_offset;
      int puw = 0x6;

      next();
      asm_expr(s1, &e);

      /* Emit a 32-bit LDR (literal) so it works for any Rt.
         Place the literal immediately after, aligned to 4 bytes.
       */
      aligned_insn_pos = insn_pos & ~3;
      branch_pos = insn_pos + 4;
      literal_pos = (branch_pos + 4 + 3) & ~3;
      jump_addr = literal_pos - aligned_insn_pos - 4;

      thumb_emit_opcode(th_ldr_imm(ops[0].reg, R_PC, jump_addr, puw, ENFORCE_ENCODING_32BIT));

      /* Emit branch to skip over the inline literal data. */
      literal_end = literal_pos + 4;
      branch_offset = literal_end - (branch_pos + 4);
      thumb_emit_opcode(th_b_t4(branch_offset));

      /* Pad to 4-byte alignment if needed. */
      while (ind < literal_pos)
        gen_le16(0);

      /* Inline literal (with relocation if e.sym is set). */
      gen_expr32(&e);
      return;
    }

    thumb_emit_opcode(thumb_single_memory_transfer_literal_opcode(s1, token, ops[0], op2reg));
    return;
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

  if (token == TOK_ASM_ldrd)
  {
    if (tok == '!')
    {
      excalm = 1;
      next();
    }
  }
  switch (token)
  {
  case TOK_ASM_lda:
    thumb_emit_opcode(th_lda(ops[0].reg, ops[1].reg));
    return;
  case TOK_ASM_ldab:
    thumb_emit_opcode(th_ldab(ops[0].reg, ops[1].reg));
    return;
  case TOK_ASM_ldaex:
    thumb_emit_opcode(th_ldaex(ops[0].reg, ops[1].reg));
    return;
  case TOK_ASM_ldaexb:
    thumb_emit_opcode(th_ldaexb(ops[0].reg, ops[1].reg));
    return;
  case TOK_ASM_ldaexh:
    thumb_emit_opcode(th_ldaexh(ops[0].reg, ops[1].reg));
    return;
  case TOK_ASM_ldah:
    thumb_emit_opcode(th_ldah(ops[0].reg, ops[1].reg));
    return;
  case TOK_ASM_ldr:
  case TOK_ASM_ldrb:
  case TOK_ASM_ldrd:
  case TOK_ASM_ldrex:
  case TOK_ASM_ldrexb:
  case TOK_ASM_ldrexh:
  case TOK_ASM_ldrh:
  case TOK_ASM_ldrsb:
  case TOK_ASM_ldrsh:
  case TOK_ASM_str:
  case TOK_ASM_strb:
  case TOK_ASM_strd:
  case TOK_ASM_strex:
  case TOK_ASM_strexb:
  case TOK_ASM_strexh:
  case TOK_ASM_strh:
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

      switch (token)
      {
      case TOK_ASM_ldr:
        thumb_emit_opcode(th_ldr_imm(ops[0].reg, ops[1].reg, imm, puw, encoding));
        return;
      case TOK_ASM_ldrb:
        thumb_emit_opcode(th_ldrb_imm(ops[0].reg, ops[1].reg, imm, puw, encoding));
        return;
      case TOK_ASM_ldrd:
        thumb_emit_opcode(th_ldrd_imm(ops[0].reg, op2reg.reg, ops[1].reg, imm, puw, encoding));
        return;
      case TOK_ASM_ldrex:
        thumb_emit_opcode(th_ldrex(ops[0].reg, ops[1].reg, imm));
        return;
      case TOK_ASM_ldrexb:
        thumb_emit_opcode(th_ldrexb(ops[0].reg, ops[1].reg));
        return;
      case TOK_ASM_ldrexh:
        thumb_emit_opcode(th_ldrexh(ops[0].reg, ops[1].reg));
        return;
      case TOK_ASM_ldrh:
        thumb_emit_opcode(th_ldrh_imm(ops[0].reg, ops[1].reg, imm, puw, encoding));
        return;
      case TOK_ASM_ldrsb:
        thumb_emit_opcode(th_ldrsb_imm(ops[0].reg, ops[1].reg, imm, puw, encoding));
        return;
      case TOK_ASM_ldrsh:
        thumb_emit_opcode(th_ldrsh_imm(ops[0].reg, ops[1].reg, imm, puw, encoding));
        return;
      case TOK_ASM_str:
        thumb_emit_opcode(th_str_imm(ops[0].reg, ops[1].reg, imm, puw, encoding));
        return;
      case TOK_ASM_strb:
        thumb_emit_opcode(th_strb_imm(ops[0].reg, ops[1].reg, imm, puw, encoding));
        return;
      case TOK_ASM_strd:
        thumb_emit_opcode(th_strd_imm(ops[0].reg, op2reg.reg, ops[1].reg, imm, puw, encoding));
        return;
      case TOK_ASM_strex:
        thumb_emit_opcode(th_strex(ops[0].reg, op2reg.reg, ops[1].reg, imm));
        return;
      case TOK_ASM_strexb:
        thumb_emit_opcode(th_strexb(ops[0].reg, op2reg.reg, ops[1].reg));
        return;
      case TOK_ASM_strexh:
        thumb_emit_opcode(th_strexh(ops[0].reg, op2reg.reg, ops[1].reg));
        return;
      case TOK_ASM_strh:
        thumb_emit_opcode(th_strh_imm(ops[0].reg, ops[1].reg, imm, puw, encoding));
        return;
      };
    }
    else
    {
      switch (token)
      {
      case TOK_ASM_ldr:
        thumb_emit_opcode(th_ldr_reg(ops[0].reg, ops[1].reg, ops[2].reg, shift, encoding));
        return;
      case TOK_ASM_ldrb:
        thumb_emit_opcode(th_ldrb_reg(ops[0].reg, ops[1].reg, ops[2].reg, shift, encoding));
        return;
      case TOK_ASM_ldrh:
        thumb_emit_opcode(th_ldrh_reg(ops[0].reg, ops[1].reg, ops[2].reg, shift, encoding));
        return;
      case TOK_ASM_ldrsb:
        thumb_emit_opcode(th_ldrsb_reg(ops[0].reg, ops[1].reg, ops[2].reg, shift, encoding));
        return;
      case TOK_ASM_ldrsh:
        thumb_emit_opcode(th_ldrsh_reg(ops[0].reg, ops[1].reg, ops[2].reg, shift, encoding));
        return;
      case TOK_ASM_str:
        thumb_emit_opcode(th_str_reg(ops[0].reg, ops[1].reg, ops[2].reg, shift, encoding));
        return;
      case TOK_ASM_strb:
        thumb_emit_opcode(th_strb_reg(ops[0].reg, ops[1].reg, ops[2].reg, shift, encoding));
        return;
      case TOK_ASM_strh:
        thumb_emit_opcode(th_strh_reg(ops[0].reg, ops[1].reg, ops[2].reg, shift, encoding));
        return;
      }
    }
  case TOK_ASM_ldrbt:
    if (!thumb_operand_is_immediate(ops[2].type))
    {
      expect("third operand must be an immediate");
    }
    thumb_emit_opcode(th_ldrbt(ops[0].reg, ops[1].reg, ops[2].e.v));
    return;
  case TOK_ASM_ldrht:
    if (!thumb_operand_is_immediate(ops[2].type))
    {
      expect("third operand must be an immediate");
    }
    thumb_emit_opcode(th_ldrht(ops[0].reg, ops[1].reg, ops[2].e.v));
    return;
  case TOK_ASM_ldrsbt:
    if (!thumb_operand_is_immediate(ops[2].type))
    {
      expect("third operand must be an immediate");
    }
    thumb_emit_opcode(th_ldrsbt(ops[0].reg, ops[1].reg, ops[2].e.v));
    return;
  case TOK_ASM_ldrsht:
    if (!thumb_operand_is_immediate(ops[2].type))
    {
      expect("third operand must be an immediate");
    }
    thumb_emit_opcode(th_ldrsht(ops[0].reg, ops[1].reg, ops[2].e.v));
    return;
  case TOK_ASM_ldrt:
    if (!thumb_operand_is_immediate(ops[2].type))
    {
      expect("third operand must be an immediate");
    }
    thumb_emit_opcode(th_ldrt(ops[0].reg, ops[1].reg, ops[2].e.v));
    return;
  case TOK_ASM_stl:
    thumb_emit_opcode(th_stl(ops[0].reg, ops[1].reg));
    return;
  case TOK_ASM_stlb:
    thumb_emit_opcode(th_stlb(ops[0].reg, ops[1].reg));
    return;
  case TOK_ASM_stlex:
    thumb_emit_opcode(th_stlex(ops[0].reg, op2reg.reg, ops[1].reg));
    return;
  case TOK_ASM_stlexb:
    thumb_emit_opcode(th_stlexb(ops[0].reg, op2reg.reg, ops[1].reg));
    return;
  case TOK_ASM_stlexh:
    thumb_emit_opcode(th_stlexh(ops[0].reg, op2reg.reg, ops[1].reg));
    return;
  case TOK_ASM_stlh:
    thumb_emit_opcode(th_stlh(ops[0].reg, ops[1].reg));
    return;
  case TOK_ASM_strbt:
    if (!thumb_operand_is_immediate(ops[2].type))
    {
      expect("third operand must be an immediate");
    }
    thumb_emit_opcode(th_strbt(ops[0].reg, ops[1].reg, ops[2].e.v));
    return;
  case TOK_ASM_strht:
    if (!thumb_operand_is_immediate(ops[2].type))
    {
      expect("third operand must be an immediate");
    }
    thumb_emit_opcode(th_strht(ops[0].reg, ops[1].reg, ops[2].e.v));
    return;
  case TOK_ASM_strt:
    if (!thumb_operand_is_immediate(ops[2].type))
    {
      expect("third operand must be an immediate");
    }
    thumb_emit_opcode(th_strt(ops[0].reg, ops[1].reg, ops[2].e.v));
    return;
  };
  return;
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

  if (THUMB_HAS_WIDE_QUALIFIER_FROM_STATE())
  {
    encoding = ENFORCE_ENCODING_32BIT;
  }

  switch (token)
  {
  case TOK_ASM_ldm:
  case TOK_ASM_ldmfd:
  case TOK_ASM_ldmia:
    thumb_emit_opcode(th_ldm(ops[0].reg, ops[1].regset, op0_exclam, encoding));
    break;
  case TOK_ASM_ldmdb:
  case TOK_ASM_ldmea:
    thumb_emit_opcode(th_ldmdb(ops[0].reg, ops[1].regset, op0_exclam));
    break;
  case TOK_ASM_stm:
  case TOK_ASM_stmia:
  case TOK_ASM_stmea:
    thumb_emit_opcode(th_stm(ops[0].reg, ops[1].regset, op0_exclam, encoding));
    break;
  case TOK_ASM_stmdb:
  case TOK_ASM_stmfd:
    thumb_emit_opcode(th_stmdb(ops[0].reg, ops[1].regset, op0_exclam, encoding));
  };
}

static thumb_opcode thumb_pushpop_opcode(TCCState *s1, int token)
{
  Operand op = {};
  parse_operand(s1, &op);

  switch (token)
  {
  case TOK_ASM_pop:
    return th_pop(op.regset);
  case TOK_ASM_push:
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

  switch (token)
  {
  case TOK_ASM_vpop:
    return th_vpop(op.regset, is_doubleword);
  case TOK_ASM_vpush:
    return th_vpush(op.regset, is_doubleword);
  }
  return (thumb_opcode){0, 0};
}

static uint32_t thumb_vfp_size_from_token_str(const char *token_str)
{
  return (token_str && strstr(token_str, ".f64")) ? 1 : 0;
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

static thumb_opcode thumb_vfp_arith_opcode(TCCState *s1, int token, const char *orig_token_str)
{
  // Skip suffix tokens if present (e.g., "vadd.f32" splits into "vadd", ".", "f32")
  if (tok == '.')
  {
    next(); // skip the dot
    next(); // skip the suffix (f32 or f64)
  }

  Operand ops[3] = {};
  const int nb_ops = process_operands(s1, sizeof(ops) / sizeof(ops[0]), ops);
  const uint32_t sz = thumb_vfp_size_from_token_str(orig_token_str);
  const bool is_unary = (orig_token_str && strncmp(orig_token_str, "vneg", 4) == 0);
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

  if (orig_token_str && strncmp(orig_token_str, "vadd", 4) == 0)
    return th_vadd_f(ops[0].reg, ops[1].reg, ops[2].reg, sz);
  if (orig_token_str && strncmp(orig_token_str, "vsub", 4) == 0)
    return th_vsub_f(ops[0].reg, ops[1].reg, ops[2].reg, sz);
  if (orig_token_str && strncmp(orig_token_str, "vmul", 4) == 0)
    return th_vmul_f(ops[0].reg, ops[1].reg, ops[2].reg, sz);
  if (orig_token_str && strncmp(orig_token_str, "vdiv", 4) == 0)
    return th_vdiv_f(ops[0].reg, ops[1].reg, ops[2].reg, sz);
  if (orig_token_str && strncmp(orig_token_str, "vneg", 4) == 0)
    return th_vneg_f(ops[0].reg, ops[1].reg, sz);

  tcc_error("unsupported VFP instruction '%s'", orig_token_str ? orig_token_str : "(null)");
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

static thumb_opcode thumb_vcmp_opcode(TCCState *s1, int token, const char *orig_token_str)
{
  // Skip suffix tokens if present (e.g., "vcmp.f32" splits into "vcmp", ".", "f32")
  if (tok == '.')
  {
    next(); // skip the dot
    next(); // skip the suffix (f32 or f64)
  }

  Operand ops[2] = {};
  const int nb_ops = process_operands(s1, sizeof(ops) / sizeof(ops[0]), ops);
  const uint32_t sz = thumb_vfp_size_from_token_str(orig_token_str);

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

static thumb_opcode thumb_vcvt_opcode(TCCState *s1, int token, const char *orig_token_str)
{
  // VCVT instruction for floating-point conversions
  // Syntax: vcvt.<dest_type>.<src_type> dest, src
  // Examples: vcvt.s32.f32 (float to signed int), vcvt.f32.s32 (signed int to float)

  char dest_type[16] = {0};
  char src_type[16] = {0};

  // Parse the conversion types from the original token string (e.g., "vcvt.s32.f32")
  // The suffix parsing has already stripped the suffix from 'token', so we use orig_token_str
  if (orig_token_str)
  {
    const char *dot1 = strchr(orig_token_str, '.');
    if (dot1)
    {
      dot1++; // skip the first dot
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
  switch (token)
  {
  case TOK_ASM_ssat:
    return th_ssat(ops[0].reg, ops[1].e.v, ops[2].reg, shift);
  case TOK_ASM_usat:
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

  switch (token)
  {
  case TOK_ASM_tta:
    a = 1;
    break;
  case TOK_ASM_ttat:
    a = 1;
    t = 1;
    break;
  case TOK_ASM_ttt:
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

  switch (token)
  {
  case TOK_ASM_bfi:
    return th_bfi(ops[0].reg, ops[1].reg, ops[2].e.v, ops[3].e.v);
  case TOK_ASM_sbfx:
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
  switch (token)
  {
  case TOK_ASM_pkhbt:
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
  case TOK_ASM_pkhtb:
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

  switch (token)
  {
  case TOK_ASM_mla:
    return th_mla(ops[0].reg, ops[1].reg, ops[2].reg, ops[3].reg);
  case TOK_ASM_mls:
    return th_mls(ops[0].reg, ops[1].reg, ops[2].reg, ops[3].reg);
  case TOK_ASM_smlal:
    return th_smlal(ops[0].reg, ops[1].reg, ops[2].reg, ops[3].reg);
  case TOK_ASM_smull:
    return th_smull(ops[0].reg, ops[1].reg, ops[2].reg, ops[3].reg);
  case TOK_ASM_umlal:
    return th_umlal(ops[0].reg, ops[1].reg, ops[2].reg, ops[3].reg);
  case TOK_ASM_umull:
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
  if (THUMB_HAS_WIDE_QUALIFIER_FROM_STATE())
  {
    encoding = ENFORCE_ENCODING_32BIT;
  }
  switch (token)
  {
  case TOK_ASM_nop:
    return th_nop(encoding);
  case TOK_ASM_sev:
    return th_sev(encoding);
  case TOK_ASM_wfe:
    return th_wfe(encoding);
  case TOK_ASM_wfi:
    return th_wfi(encoding);
  case TOK_ASM_yield:
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
  if (token == TOK_ASM_add && ops[1].reg == R_PC)
  {
    thumb_enforce_encoding encoding = ENFORCE_ENCODING_NONE;
    if (!thumb_operand_is_immediate(ops[2].type))
    {
      expect("second operand must be an immediate for adr");
    }
    if (THUMB_HAS_WIDE_QUALIFIER_FROM_STATE())
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

  if (THUMB_HAS_WIDE_QUALIFIER_FROM_STATE())
  {
    encoding = ENFORCE_ENCODING_32BIT;
  }

  switch (token)
  {
  case TOK_ASM_asrs:
  case TOK_ASM_rors:
  case TOK_ASM_lsls:
  case TOK_ASM_lsrs:
  case TOK_ASM_rrxs:
    token_svariant = true;
  };

  switch (token)
  {
  case TOK_ASM_asrs:
  case TOK_ASM_asr:
  {
    shift.type = THUMB_SHIFT_ASR;
  }
  break;
  case TOK_ASM_lsls:
  case TOK_ASM_lsl:
  {
    shift.type = THUMB_SHIFT_LSL;
  }
  break;
  case TOK_ASM_lsrs:
  case TOK_ASM_lsr:
  {
    shift.type = THUMB_SHIFT_LSR;
  }
  break;
  case TOK_ASM_rors:
  case TOK_ASM_ror:
  {
    shift.type = THUMB_SHIFT_ROR;
  }
  break;
  case TOK_ASM_rrxs:
  case TOK_ASM_rrx:
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
  if (THUMB_HAS_WIDE_QUALIFIER_FROM_STATE())
  {
    encoding = ENFORCE_ENCODING_32BIT;
  }

  switch (token)
  {
  case TOK_ASM_svc:
    opcode = th_svc(op.e.v);
    break;
  case TOK_ASM_bkpt:
    opcode = th_bkpt(op.e.v);
    break;
  case TOK_ASM_udf:
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
  bool must_use_t3 = false;
  bool must_use_32bit = false;
  int sign = 0;
  if (THUMB_HAS_WIDE_QUALIFIER_FROM_STATE())
  {
    must_use_32bit = true;
  }

  /* Read condition early so we can choose the right relocation type */
  condition = THUMB_GET_CONDITION_FROM_STATE();

  if (token == TOK_ASM_cbz || token == TOK_ASM_cbnz)
  {
    process_operands(s1, 1, &op);
  }

  if (token == TOK_ASM_b || token == TOK_ASM_bl || token == TOK_ASM_cbz || token == TOK_ASM_cbnz)
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
        if (token == TOK_ASM_cbz || token == TOK_ASM_cbnz)
        {
          greloca(cur_text_section, e.sym, ind, R_ARM_THM_JUMP6, 0);
        }
        else if (token == TOK_ASM_b && condition != 0xe && thumb_conditional_scope == 0)
        {
          /* Conditional branch forward reference: use T3 encoding with R_ARM_THM_JUMP19 */
          greloca(cur_text_section, e.sym, ind, R_ARM_THM_JUMP19, 0);
          must_use_t3 = true;
        }
        else
        {
          greloca(cur_text_section, e.sym, ind, R_ARM_THM_PC22, 0);
          must_use_t4 = true;
        }
        jump_addr = th_encbranch(ind, (ind + e.v) & ~1);
      }
    }
  }
  else
  {
    process_operands(s1, 1, &op);
  }

  switch (token)
  {
  case TOK_ASM_bx:
  {
    if (!thumb_operand_is_register(op.type))
    {
      expect("first operand must be a register");
    }
    return thumb_emit_opcode(th_bx_reg(op.reg));
  }
  case TOK_ASM_blx:
  {
    if (!thumb_operand_is_register(op.type))
    {
      expect("first operand must be a register");
    }
    return thumb_emit_opcode(th_blx_reg(op.reg));
  }
  case TOK_ASM_bl:
    return thumb_emit_opcode(th_bl_t1(jump_addr));
  case TOK_ASM_b:
  {
    if (must_use_t3)
    {
      /* Conditional forward reference: emit T3 (32-bit conditional) */
      return thumb_emit_opcode(th_b_t3(condition, jump_addr >> 1));
    }
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
  case TOK_ASM_cbnz:
    sign = 1;
  case TOK_ASM_cbz:
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

  const char *orig_token_str = get_tok_str(token, NULL);

  /* Parse token suffix to extract base token and condition code */
  int base_token;
  int condition = thumb_parse_token_suffix(token, &base_token);
  /* Use the base token for dispatch, but remember the condition code */
  token = base_token;
  current_asm_suffix.condition = condition;

  /* GAS-compatible aliases for conditional branches.
     (hs == cs, lo == cc)
     These mnemonics are common in upstream CMSIS startup code.
   */
  {
    const char *alias = get_tok_str(token, NULL);
    if (alias)
    {
      if (strcmp(alias, "bhs") == 0)
        token = TOK_ASM_b;
      else if (strcmp(alias, "blo") == 0)
        token = TOK_ASM_b;
      /* Note: Width qualifiers (.w, .n) are now parsed at runtime */
      else if (strcmp(alias, "bhs.w") == 0)
        token = TOK_ASM_b;
      else if (strcmp(alias, "blo.w") == 0)
        token = TOK_ASM_b;
    }
  }

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
  if (strncmp(orig_token_str, "vadd", 4) == 0 || strncmp(orig_token_str, "vsub", 4) == 0 ||
      strncmp(orig_token_str, "vmul", 4) == 0 || strncmp(orig_token_str, "vdiv", 4) == 0 ||
      strncmp(orig_token_str, "vneg", 4) == 0)
  {
    thumb_emit_opcode(thumb_vfp_arith_opcode(s1, token, orig_token_str));
    return;
  }
  if (strncmp(orig_token_str, "vcmp", 4) == 0)
  {
    thumb_emit_opcode(thumb_vcmp_opcode(s1, token, orig_token_str));
    return;
  }
  if (strncmp(token_str, "vmrs", 4) == 0)
  {
    thumb_emit_opcode(thumb_vmrs_opcode(s1, token));
    return;
  }
  if (strncmp(orig_token_str, "vcvt", 4) == 0)
  {
    thumb_emit_opcode(thumb_vcvt_opcode(s1, token, orig_token_str));
    return;
  }
  switch (token)
  {
  case TOK_ASM_bx:
  case TOK_ASM_bl:
  case TOK_ASM_blx:
  case TOK_ASM_b:
  case TOK_ASM_cbz:
  case TOK_ASM_cbnz:
    return thumb_branch(s1, token);
  case TOK_ASM_adc:
  case TOK_ASM_adcs:
  case TOK_ASM_add:
  case TOK_ASM_adds:
  case TOK_ASM_addw:
  case TOK_ASM_and:
  case TOK_ASM_ands:
  case TOK_ASM_movs:
  case TOK_ASM_movw:
  case TOK_ASM_mov:
  case TOK_ASM_cmp:
  case TOK_ASM_bfc:
  case TOK_ASM_bic:
  case TOK_ASM_bics:
  case TOK_ASM_clz:
  case TOK_ASM_cmn:
  case TOK_ASM_eor:
  case TOK_ASM_eors:
  case TOK_ASM_mul:
  case TOK_ASM_muls:
  case TOK_ASM_mvn:
  case TOK_ASM_mvns:
  case TOK_ASM_orn:
  case TOK_ASM_orns:
  case TOK_ASM_orr:
  case TOK_ASM_orrs:
  case TOK_ASM_rbit:
  case TOK_ASM_rev:
  case TOK_ASM_rev16:
  case TOK_ASM_revsh:
  case TOK_ASM_rsb:
  case TOK_ASM_rsbs:
  case TOK_ASM_sbc:
  case TOK_ASM_sbcs:
  case TOK_ASM_sdiv:
  case TOK_ASM_sub:
  case TOK_ASM_subs:
  case TOK_ASM_subw:
  case TOK_ASM_sxtb:
  case TOK_ASM_sxth:
  case TOK_ASM_teq:
  case TOK_ASM_tst:
  case TOK_ASM_udiv:
  case TOK_ASM_uxtb:
  case TOK_ASM_uxth:
    return thumb_data_processing_opcode(s1, token);
  case TOK_ASM_adr:
    return thumb_adr_opcode(s1, token);
  case TOK_ASM_svc:
  case TOK_ASM_bkpt:
  case TOK_ASM_udf:
    return thumb_process_control(s1, token);
  case TOK_ASM_asr:
  case TOK_ASM_asrs:
  case TOK_ASM_lsl:
  case TOK_ASM_lsls:
  case TOK_ASM_lsr:
  case TOK_ASM_lsrs:
  case TOK_ASM_ror:
  case TOK_ASM_rors:
  case TOK_ASM_rrx:
  case TOK_ASM_rrxs:
    return thumb_emit_opcode(thumb_data_shift_opcode(s1, token));
  case TOK_ASM_bfi:
  case TOK_ASM_sbfx:
    return thumb_emit_opcode(thumb_bitmanipulation_opcode(s1, token));
  case TOK_ASM_clrex:
    return thumb_emit_opcode(th_clrex());
  case TOK_ASM_cpsid:
    return thumb_cps_opcode(0);
  case TOK_ASM_cpsie:
    return thumb_cps_opcode(1);
  case TOK_ASM_csdb:
    return thumb_emit_opcode(th_csdb());
  case TOK_ASM_dmb:
  case TOK_ASM_isb:
    return thumb_synchronization_barrier_opcode(token);
  case TOK_ASM_dsb:
    return thumb_dsb_opcode();
  case TOK_ASM_lda:
  case TOK_ASM_ldab:
  case TOK_ASM_ldaex:
  case TOK_ASM_ldaexb:
  case TOK_ASM_ldaexh:
  case TOK_ASM_ldah:
  case TOK_ASM_ldr:
  case TOK_ASM_ldrb:
  case TOK_ASM_ldrbt:
  case TOK_ASM_ldrd:
  case TOK_ASM_ldrex:
  case TOK_ASM_ldrexb:
  case TOK_ASM_ldrexh:
  case TOK_ASM_ldrh:
  case TOK_ASM_ldrht:
  case TOK_ASM_ldrsb:
  case TOK_ASM_ldrsbt:
  case TOK_ASM_ldrsh:
  case TOK_ASM_ldrsht:
  case TOK_ASM_ldrt:
  case TOK_ASM_stl:
  case TOK_ASM_stlb:
  case TOK_ASM_stlex:
  case TOK_ASM_stlexb:
  case TOK_ASM_stlexh:
  case TOK_ASM_stlh:
  case TOK_ASM_str:
  case TOK_ASM_strb:
  case TOK_ASM_strbt:
  case TOK_ASM_strd:
  case TOK_ASM_strex:
  case TOK_ASM_strexb:
  case TOK_ASM_strexh:
  case TOK_ASM_strh:
  case TOK_ASM_strht:
  case TOK_ASM_strt:
    return thumb_single_memory_transfer_opcode(s1, token);
  case TOK_ASM_pld:
  case TOK_ASM_pldw:
  case TOK_ASM_pli:
  case TOK_ASM_pliw:
  case TOK_ASM_tbb:
  case TOK_ASM_tbh:
    return thumb_emit_opcode(thumb_cache_preload_opcode(s1, token));
  case TOK_ASM_ldm:
  case TOK_ASM_ldmfd:
  case TOK_ASM_ldmia:
  case TOK_ASM_ldmdb:
  case TOK_ASM_ldmea:
  case TOK_ASM_stm:
  case TOK_ASM_stmia:
  case TOK_ASM_stmea:
  case TOK_ASM_stmdb:
  case TOK_ASM_stmfd:
    return thumb_block_memory_transfer_opcode(s1, token);
  case TOK_ASM_mla:
  case TOK_ASM_smlal:
  case TOK_ASM_smull:
  case TOK_ASM_umlal:
  case TOK_ASM_mls:
  case TOK_ASM_umull:
    return thumb_emit_opcode(thumb_math_opcode(s1, token));
  case TOK_ASM_movt:
    return thumb_emit_opcode(thumb_movt_opcode(s1, token));
  case TOK_ASM_mrs:
    return thumb_emit_opcode(thumb_mrs_opcode(s1, token));
  case TOK_ASM_msr:
    return thumb_emit_opcode(thumb_msr_opcode(s1, token));
  case TOK_ASM_nop:
  case TOK_ASM_sev:
  case TOK_ASM_wfe:
  case TOK_ASM_wfi:
  case TOK_ASM_yield:
    return thumb_emit_opcode(thumb_control_opcode(s1, token));
  case TOK_ASM_pkhbt:
  case TOK_ASM_pkhtb:
    return thumb_emit_opcode(thumb_pkhbt_opcode(s1, token));
  case TOK_ASM_pop:
  case TOK_ASM_push:
    return thumb_emit_opcode(thumb_pushpop_opcode(s1, token));
  case TOK_ASM_ssat:
  case TOK_ASM_usat:
    return thumb_emit_opcode(thumb_ssat_opcode(s1, token));
  case TOK_ASM_ssbb:
    return thumb_emit_opcode(th_ssbb());
  case TOK_ASM_tt:
  case TOK_ASM_ttt:
  case TOK_ASM_tta:
  case TOK_ASM_ttat:
    return thumb_emit_opcode(thumb_tt(s1, token));
  case TOK_ASM_vpush:
  case TOK_ASM_vpop:
    return thumb_emit_opcode(thumb_vpushvpop_opcode(s1, token));
  default:
    printf("asm_opcode: unknown token %s\n", get_tok_str(token, NULL));
    expect("known instruction");
  }
}

/*************************************************************/
