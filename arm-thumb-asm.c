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

#ifdef TARGET_DEFS_ONLY

#define CONFIG_TCC_ASM
#define NB_ASM_REGS 16

ST_FUNC void g(int c);
ST_FUNC void gen_le16(int c);
ST_FUNC void gen_le32(int c);

#else

#define USING_GLOBALS
#include <ctype.h>

#include "arm-thumb-opcodes.h"
#include "tcc.h"

enum {
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

typedef struct Operand {
  uint32_t type;
  union {
    uint8_t reg;
    uint16_t regset;
    ExprValue e;
  };
} Operand;

ST_FUNC void g(int c) {
  int ind1;
  if (nocode_wanted)
    return;
  ind1 = ind + 1;
  if (ind1 > cur_text_section->data_allocated)
    section_realloc(cur_text_section, ind1);
  cur_text_section->data[ind] = c;
  ind = ind1;
}

ST_FUNC void gen_le16(int i) {
  g(i);
  g(i >> 8);
}

ST_FUNC void gen_le32(int i) {
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

ST_FUNC void gen_expr32(ExprValue *pe) { gen_le32(pe->v); }

int is_valid_opcode(thumb_opcode op);

static void thumb_emit_opcode(thumb_opcode op) {
  if (!is_valid_opcode(op)) {
    tcc_error("compiler_error: received invalid opcode: 0x%x\n", op.opcode);
  }
  if (op.size == 4) {
    gen_le16(op.opcode >> 16);
  }
  gen_le16(op.opcode & 0xffff);
}

ST_FUNC void subst_asm_operand(CString *add_str, SValue *sv, int modifier) {
  int r, reg, size, val;

  r = sv->r;
  if ((r & VT_VALMASK) == VT_CONST) {
    if (!(r & VT_LVAL) && modifier != 'c' && modifier != 'n' && modifier != 'P')
      cstr_ccat(add_str, '#');
    if (r & VT_SYM) {
      const char *name = get_tok_str(sv->sym->v, NULL);
      if (sv->sym->v >= SYM_FIRST_ANOM) {
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
  } else if ((r & VT_VALMASK) == VT_LOCAL) {
    cstr_printf(add_str, "[fp,#%d]", (int)sv->c.i);
  } else if (r & VT_LVAL) {
    reg = r & VT_VALMASK;
    if (reg >= VT_CONST)
      tcc_internal_error("");
    cstr_printf(add_str, "[%s]", get_tok_str(TOK_ASM_r0 + reg, NULL));
  } else {
    /* register case */
    reg = r & VT_VALMASK;
    if (reg >= VT_CONST)
      tcc_internal_error("");

    /* choose register operand size */
    if ((sv->type.t & VT_BTYPE) == VT_BYTE ||
        (sv->type.t & VT_BTYPE) == VT_BOOL)
      size = 1;
    else if ((sv->type.t & VT_BTYPE) == VT_SHORT)
      size = 2;
    else
      size = 4;

    if (modifier == 'b') {
      size = 1;
    } else if (modifier == 'w') {
      size = 2;
    } else if (modifier == 'k') {
      size = 4;
    }

    switch (size) {
    default:
      reg = TOK_ASM_r0 + reg;
      break;
    }
    cstr_printf(add_str, "%s", get_tok_str(reg, NULL));
  }
}

/* generate prolog and epilog code for asm statement */
ST_FUNC void asm_gen_code(ASMOperand *operands, int nb_operands, int nb_outputs,
                          int is_output, uint8_t *clobber_regs, int out_reg) {
  tcc_error("asm_gen_code not implemented");
}

ST_FUNC void asm_clobber(uint8_t *clobber_regs, const char *str) {
  tcc_error("asm clobber not implemented");
}

ST_FUNC void asm_compute_constraints(ASMOperand *operands, int nb_operands,
                                     int nb_outputs,
                                     const uint8_t *clobber_regs,
                                     int *pout_reg) {
  tcc_error("asm_compute_constraints not implemented");
}

static int asm_parse_vfp_regvar(int t, int double_precision) {
  if (double_precision) {
    if (t >= TOK_ASM_d0 && t <= TOK_ASM_d15)
      return t - TOK_ASM_d0;
  } else {
    if (t >= TOK_ASM_s0 && t <= TOK_ASM_s31)
      return t - TOK_ASM_s0;
  }
  return -1;
}

/* If T refers to a register then return the register number and type.
   Otherwise return -1.  */
ST_FUNC int asm_parse_regvar(int t) {
  if (t >= TOK_ASM_r0 && t <= TOK_ASM_pc) { /* register name */
    switch (t) {
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
  } else
    return -1;
}

/* Parse a text containing operand and store the result in OP */
static void parse_operand(TCCState *s1, Operand *op) {
  ExprValue e;
  int8_t reg;
  uint16_t regset = 0;

  op->type = 0;

  if (tok == '{') { // regset literal
    next();         // skip '{'
    while (tok != '}' && tok != TOK_EOF) {
      reg = asm_parse_regvar(tok);
      if (reg == -1) {
        expect("register");
      } else
        next(); // skip register name

      if ((1 << reg) < regset)
        tcc_warning("registers will be processed in ascending order by "
                    "hardware--but are not specified in ascending order here");
      regset |= 1 << reg;
      if (tok != ',')
        break;
      next(); // skip ','
    }
    skip('}');
    if (regset == 0) {
      // ARM instructions don't support empty regset.
      tcc_error("empty register list is not supported");
    } else {
      op->type = OP_REGSET32;
      op->regset = regset;
    }
    return;
  } else if ((reg = asm_parse_regvar(tok)) != -1) {
    next(); // skip register name
    op->type = OP_REG32;
    op->reg = (uint8_t)reg;
    return;
  } else if ((reg = asm_parse_vfp_regvar(tok, 0)) != -1) {
    next(); // skip register name
    op->type = OP_VREG32;
    op->reg = (uint8_t)reg;
    return;
  } else if ((reg = asm_parse_vfp_regvar(tok, 1)) != -1) {
    next(); // skip register name
    op->type = OP_VREG64;
    op->reg = (uint8_t)reg;
    return;
  } else if (tok == '#' || tok == '$') {
    /* constant value */
    next(); // skip '#' or '$'
  }
  asm_expr(s1, &e);
  op->type = OP_IM32;
  op->e = e;
  if (!op->e.sym) {
    if ((int)op->e.v < 0 && (int)op->e.v >= -255)
      op->type = OP_IM8N;
    else if (op->e.v == (uint8_t)op->e.v)
      op->type = OP_IM8;
  } else
    expect("operand");
}

static uint8_t thumb_build_it_mask(const char *pattern, uint16_t condition) {
  uint8_t mask = 0x0;
  for (size_t i = 2; i < 6; ++i) {
    if (pattern[i] == 0) {
      mask |= (1 << (5 - i));
      return mask;
    }

    if (tolower(pattern[i] == 't')) {
      mask |= (condition << (5 - i));
    } else {
      mask |= ((!condition) << (5 - i));
    }
  }
  return mask;
}

static int thumb_conditional_scope = 0;

static int thumb_parse_condition_str(const char *condition_str) {
  if (strncmp(condition_str, "eq", 2) == 0) {
    return 0;
  } else if (strncmp(condition_str, "ne", 2) == 0) {
    return 1;
  } else if (strncmp(condition_str, "cs", 2) == 0) {
    return 2;
  } else if (strncmp(condition_str, "cc", 2) == 0) {
    return 3;
  } else if (strncmp(condition_str, "mi", 2) == 0) {
    return 4;
  } else if (strncmp(condition_str, "pl", 2) == 0) {
    return 5;
  } else if (strncmp(condition_str, "vs", 2) == 0) {
    return 6;
  } else if (strncmp(condition_str, "vc", 2) == 0) {
    return 7;
  } else if (strncmp(condition_str, "hi", 2) == 0) {
    return 8;
  } else if (strncmp(condition_str, "ls", 2) == 0) {
    return 9;
  } else if (strncmp(condition_str, "ge", 2) == 0) {
    return 0xa;
  } else if (strncmp(condition_str, "lt", 2) == 0) {
    return 0xb;
  } else if (strncmp(condition_str, "gt", 2) == 0) {
    return 0xc;
  } else if (strncmp(condition_str, "le", 2) == 0) {
    return 0xd;
  }
  return 0xe;
}

static thumb_shift asm_parse_optional_shift(TCCState *s1) {
  Operand op;
  thumb_shift shift = {0, 0};
  if (tok == TOK_ASM_rrx) {
    next();
    return (thumb_shift){
        .type = THUMB_SHIFT_RRX,
        .value = 0,
    };
  }

  switch (tok) {
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
  if (op.type != OP_IM32 && op.type != OP_IM8 && op.type != OP_IM8N) {
    tcc_error("shift operand must be immediate value");
  }
  shift.value = op.e.v;
  return shift;
}

static void thumb_conditional_opcode(TCCState *s1, int token) {
  int condition = 0;
  int mask = 0;
  const char *token_str = get_tok_str(token, NULL);
  thumb_conditional_scope = strlen(token_str);
  char it_str[6] = {0};
  strcpy(it_str, token_str);

  token_str = get_tok_str(tok, NULL);
  if (strlen(token_str) < 2) {
    tcc_error("thumb_conditional_opcode: condition too short: %s\n", token_str);
  }

  condition = thumb_parse_condition_str(token_str);
  mask = thumb_build_it_mask(it_str, condition & 1);
  thumb_emit_opcode(th_it(condition, mask));
  next();
}

static int process_operands(TCCState *s1, int max_operands, Operand *ops) {
  int nb_ops = 0;
  for (nb_ops = 0; nb_ops < max_operands;) {
    parse_operand(s1, &ops[nb_ops]);
    ++nb_ops;
    if (tok != ',')
      break;
    next(); // skip ','
  }
  if (tok == ',')
    next();
  return nb_ops;
}

static flags_behaviour thumb_determine_flags_behaviour(int token,
                                                       int token_svariant,
                                                       bool allow_in_it) {
  if (THUMB_INSTRUCTION_GROUP(token) == token_svariant) {
    if (thumb_conditional_scope > 0 && !allow_in_it) {
      tcc_error("cannot use '%s' in IT block", get_tok_str(token, NULL));
    }
    return FLAGS_BEHAVIOUR_SET;
  }
  if (thumb_conditional_scope > 0) {
    return FLAGS_BEHAVIOUR_NOT_IMPORTANT;
  }
  return FLAGS_BEHAVIOUR_BLOCK;
}

static bool thumb_operand_is_immediate(int type) {
  if (type != OP_IM32 && type != OP_IM8 && type != OP_IM8N) {
    return false;
  }
  return true;
}

static bool thumb_operand_is_register(int type) {
  if (type != OP_REG && type != OP_REG32) {
    return false;
  }
  return true;
}

static void thumb_data_processing_opcode(TCCState *s1, int token) {
  Operand ops[3];
  int nb_ops;
  uint32_t operands = 0;
  thumb_shift shift_info = {0, 0};

  nb_ops = process_operands(s1, sizeof(ops) / sizeof(ops[0]), ops);

  if (nb_ops < 2) {
    expect("at least two operands");
    return;
  } else if (nb_ops == 2) {
    memcpy(&ops[2], &ops[1], sizeof(ops[1]));
    memcpy(&ops[1], &ops[0],
           sizeof(ops[0])); // most instructions may have implicit destination
                            // register
    nb_ops = 3;
  }
  shift_info = asm_parse_optional_shift(s1);

  if (ops[0].type != OP_REG32) {
    expect("first operand must be a register");
  }

  flags_behaviour setflags = FLAGS_BEHAVIOUR_NOT_IMPORTANT;
  enforce_encoding encoding = ENFORCE_ENCODING_NONE;
  if (THUMB_HAS_WIDE_QUALIFIER(token)) {
    encoding = ENFORCE_ENCODING_32BIT;
  }

  switch (THUMB_INSTRUCTION_GROUP(token)) {
  case TOK_ASM_adcseq:
  case TOK_ASM_adceq: {
    setflags = thumb_determine_flags_behaviour(token, TOK_ASM_adcseq, true);
    if (thumb_operand_is_immediate(ops[2].type))
      return thumb_emit_opcode(
          th_adc_imm(ops[0].reg, ops[1].reg, ops[2].e.v, setflags));

    if (thumb_operand_is_register(ops[2].type)) {
      if ((THUMB_INSTRUCTION_GROUP(token) == TOK_ASM_adceq &&
           thumb_conditional_scope == 0) ||
          THUMB_HAS_WIDE_QUALIFIER(token)) {
        encoding = ENFORCE_ENCODING_32BIT;
      }
      return thumb_emit_opcode(th_adc_reg(ops[0].reg, ops[1].reg, ops[2].reg,
                                          setflags, shift_info, encoding));
    }
  }
  case TOK_ASM_addseq:
  case TOK_ASM_addeq:
  case TOK_ASM_addweq: {
    setflags = thumb_determine_flags_behaviour(token, TOK_ASM_addseq, true);
    if (THUMB_HAS_WIDE_QUALIFIER(token)) {
      encoding = ENFORCE_ENCODING_32BIT;
    }

    if (thumb_operand_is_immediate(ops[2].type)) {
      if (ops[1].reg == R_SP) {
        if (THUMB_INSTRUCTION_GROUP(token) == TOK_ASM_addweq) {
          return thumb_emit_opcode(
              th_add_sp_imm_t4(ops[0].reg, ops[2].e.v, setflags, encoding));
        }
        return thumb_emit_opcode(
            th_add_sp_imm(ops[0].reg, ops[2].e.v, setflags, encoding));
      }
    }

    if (thumb_operand_is_register(ops[2].type)) {
    }
  }
  case TOK_ASM_cmpeq: {
    switch (ops[2].type) {
    case OP_IM8:
    case OP_IM32:
    case OP_IM8N:
      return thumb_emit_opcode(th_cmp_imm(ops[1].reg, ops[2].e.v, encoding));
    default:
      expect("second operand must be an immediate");
    }
  }
  case TOK_ASM_movseq:
  case TOK_ASM_movweq:
  case TOK_ASM_moveq: {
    setflags = thumb_determine_flags_behaviour(token, TOK_ASM_movseq, false);
    if (THUMB_INSTRUCTION_GROUP(token) == TOK_ASM_movweq)
      encoding = ENFORCE_ENCODING_32BIT;

    switch (ops[2].type) {
    case OP_IM8:
    case OP_IM32:
    case OP_IM8N:
      return thumb_emit_opcode(
          th_mov_imm(ops[1].reg, ops[2].e.v, setflags, encoding));
    case OP_REG32:
      return thumb_emit_opcode(th_mov_reg(ops[1].reg, ops[2].e.v));
    }
    break;
  }
  }
  tcc_error("Unhandled operation for: %s", get_tok_str(token, NULL));
}

static void thumb_process_svc(TCCState *s1, int token) {
  Operand op;
  int nb_ops = process_operands(s1, 1, &op);
  if (nb_ops > 1 || nb_ops == 0) {
    expect("SVC takes one operand");
    return;
  }
  if (op.type != OP_IM8 && op.type != OP_IM32 && op.type != OP_IM8N) {
    expect("SVC operand must be an immediate");
    return;
  }

  thumb_emit_opcode(th_svc(op.e.v));
}

static void thumb_bx(TCCState *s1, int token) {
  Operand op;
  int nb_ops = process_operands(s1, 1, &op);
  if (nb_ops > 1 || nb_ops == 0) {
    expect("BX takes one operand");
    return;
  }

  if (op.type != OP_REG32) {
    expect("BX operand must be a register");
    return;
  }

  thumb_emit_opcode(th_bx_reg(op.reg));
}

static void thumb_branch(TCCState *s1, int token) {
  int jump_addr = 0;
  Operand op;
  ExprValue e;
  ElfSym *esym;
  int condition = 0xe;
  bool must_use_t4 = false;
  bool must_use_32bit = false;
  if (THUMB_HAS_WIDE_QUALIFIER(token)) {
    must_use_32bit = true;
  }

  if (THUMB_INSTRUCTION_GROUP(token) == TOK_ASM_beq ||
      THUMB_INSTRUCTION_GROUP(token) == TOK_ASM_bleq) {
    asm_expr(s1, &e);
    if (e.sym) {
      esym = elfsym(e.sym);
      if (esym && esym->st_shndx == cur_text_section->sh_num) {
        jump_addr = th_encbranch(ind, e.v + esym->st_value);
      } else {
        greloca(cur_text_section, e.sym, ind, R_ARM_THM_PC22, 0);
        must_use_t4 = true;
        jump_addr = th_encbranch(ind, ind + e.v);
      }
    }
  } else {
    process_operands(s1, 1, &op);
  }

  printf("Token: %s\n", get_tok_str(token, NULL));
  condition = THUMB_GET_CONDITION(token);

  switch (THUMB_INSTRUCTION_GROUP(token)) {
  case TOK_ASM_bxeq: {
    if (op.type != OP_REG32) {
      expect("BX operand must be a register");
    }
    return thumb_emit_opcode(th_bx_reg(op.reg));
  }
  case TOK_ASM_blxeq: {
    if (op.type != OP_REG32) {
      expect("BLX operand must be a register");
    }
    return thumb_emit_opcode(th_blx_reg(op.reg));
  }
  case TOK_ASM_bleq:
    return thumb_emit_opcode(th_bl_t1(jump_addr));
  case TOK_ASM_beq: {
    if (must_use_t4) {
      return thumb_emit_opcode(th_b_t4(jump_addr));
    }

    if (jump_addr >= -2048 && jump_addr <= 2046 && !must_use_32bit &&
        (condition == 0xe || thumb_conditional_scope > 0)) {
      return thumb_emit_opcode(th_b_t2(jump_addr));
    } else if (jump_addr >= -256 && jump_addr <= 254 &&
               thumb_conditional_scope == 0 && !must_use_32bit) {
      return thumb_emit_opcode(th_b_t1(condition, jump_addr >> 1));
    } else if (jump_addr >= -16777216 && jump_addr <= 16777214 &&
               (condition == 0xe || thumb_conditional_scope > 0)) {
      return thumb_emit_opcode(th_b_t4(jump_addr));
    } else if (jump_addr >= -1048576 && jump_addr <= 1048574 &&
               thumb_conditional_scope == 0) {
      return thumb_emit_opcode(th_b_t3(condition, jump_addr >> 1));
    } else {
      tcc_error("branch target out of range: %d", jump_addr);
    }
  }
  default:
    tcc_error("unknown branch instruction: %s", get_tok_str(token, NULL));
  }
}

ST_FUNC void asm_opcode(TCCState *s1, int token) {
  while (token == TOK_LINEFEED) {
    next();
    token = tok;
  }
  if (token == TOK_EOF)
    return;

  if (token >= TOK_ASM_it && token <= TOK_ASM_iteee) {
    thumb_conditional_opcode(s1, token);
    return;
  }

  if (thumb_conditional_scope > 0)
    --thumb_conditional_scope;
  switch (THUMB_INSTRUCTION_GROUP(token)) {
  case TOK_ASM_bxeq:
  case TOK_ASM_bleq:
  case TOK_ASM_blxeq:
  case TOK_ASM_beq:
    return thumb_branch(s1, token);
  case TOK_ASM_adceq:
  case TOK_ASM_adcseq:
  case TOK_ASM_addeq:
  case TOK_ASM_addseq:
  case TOK_ASM_addweq:
  case TOK_ASM_movseq:
  case TOK_ASM_movweq:
  case TOK_ASM_moveq:
  case TOK_ASM_cmpeq:
    return thumb_data_processing_opcode(s1, token);
  case TOK_ASM_svceq:
    return thumb_process_svc(s1, token);
  default:
    printf("asm_opcode: unknown token %s\n", get_tok_str(token, NULL));
    expect("known instruction");
  }
}

/*************************************************************/
#endif /* ndef TARGET_DEFS_ONLY */
