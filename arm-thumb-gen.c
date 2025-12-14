/*
 *  ARMvX-m code generator for TCC
 *  Uses thumb instruction set
 *
 *  Based on:
 *  ARM Thumb 2 instruction functions for TCC
 *  Copyright (c) 2020 Erlend J. Sveen
 *  from:
 * https://git.erlendjs.no/erlendjs/tinycc/-/blob/arm-thumb/arm-thumb-gen.c
 *        https://git.erlendjs.no/erlendjs/tinycc/-/blob/arm-thumb/arm-thumb-instructions.c
 *
 *  And
 *
 *  ARMv4 code generator for TCC
 *
 *  Copyright (c) 2003 Daniel Glöckner
 *  Copyright (c) 2012 Thomas Preud'homme
 *
 *  Based on i386-gen.c by Fabrice Bellard
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

#if defined(TCC_ARM_EABI) && !defined(TCC_ARM_VFP)
#error "Currently TinyCC only supports float computation with VFP instructions"
#endif

/* number of available registers */
#ifdef TCC_ARM_VFP
#define NB_REGS 13
#else
#define NB_REGS 9
#endif

#ifndef CONFIG_TCC_CPUVER
#define CONFIG_TCC_CPUVER 5
#endif

/* a register can belong to several classes. The classes must be
   sorted from more general to more precise (see gv2() code which does
   assumptions on it). */
#define RC_INT 0x0001   /* generic integer register */
#define RC_FLOAT 0x0002 /* generic float register */
#define RC_R0 0x0004
#define RC_R1 0x0008
#define RC_R2 0x0010
#define RC_R3 0x0020
#define RC_R12 0x0040
#define RC_F0 0x0080
#define RC_F1 0x0100
#define RC_F2 0x0200
#define RC_F3 0x0400
#ifdef TCC_ARM_VFP
#define RC_F4 0x0800
#define RC_F5 0x1000
#define RC_F6 0x2000
#define RC_F7 0x4000
#endif
#define RC_IRET RC_R0 /* function return: integer register */
#define RC_IRE2 RC_R1 /* function return: second integer register */
#define RC_FRET RC_F0 /* function return: float register */

enum Armv8mRegisters {
  ARM_R0 = 0,
  ARM_R1 = 1,
  ARM_R2 = 2,
  ARM_R3 = 3,
  ARM_R4 = 4,
  ARM_R5 = 5,
  ARM_R6 = 6,
  ARM_R7 = 7,
  ARM_R8 = 8,
  ARM_R9 = 9,
  ARM_R10 = 10,
  ARM_R11 = 11,
  ARM_R12 = 12,
  ARM_SP = 13,
  ARM_LR = 14,
  ARM_PC = 15
};

/* pretty names for the registers */
enum {
  TREG_R0 = 0,
  TREG_R1,
  TREG_R2,
  TREG_R3,
  TREG_R12,
  TREG_F0,
  TREG_F1,
  TREG_F2,
  TREG_F3,
#ifdef TCC_ARM_VFP
  TREG_F4,
  TREG_F5,
  TREG_F6,
  TREG_F7,
#endif
  TREG_SP = 13,
  TREG_LR,
};

#ifdef TCC_ARM_VFP
#define T2CPR(t) (((t) & VT_BTYPE) != VT_FLOAT ? 0x100 : 0)
#endif

/* return registers for function */
#define REG_IRET TREG_R0 /* single word int return register */
#define REG_IRE2 TREG_R1 /* second word return register (for long long) */
#define REG_FRET TREG_F0 /* float return register */

#ifdef TCC_ARM_EABI
#define TOK___divdi3 TOK___aeabi_ldivmod
#define TOK___moddi3 TOK___aeabi_ldivmod
#define TOK___udivdi3 TOK___aeabi_uldivmod
#define TOK___umoddi3 TOK___aeabi_uldivmod
#endif

/* defined if function parameters must be evaluated in reverse order */
#define INVERT_FUNC_PARAMS

/* defined if structures are passed as pointers. Otherwise structures
   are directly pushed on stack. */
/* #define FUNC_STRUCT_PARAM_AS_PTR */

/* pointer size, in bytes */
#define PTR_SIZE 4

/* long double size and alignment, in bytes */
#ifdef TCC_ARM_VFP
#define LDOUBLE_SIZE 8
#endif

#ifndef LDOUBLE_SIZE
#define LDOUBLE_SIZE 8
#endif

#ifdef TCC_ARM_EABI
#define LDOUBLE_ALIGN 8
#else
#define LDOUBLE_ALIGN 4
#endif

/* maximum alignment (for aligned attribute support) */
#define MAX_ALIGN 8

#define CHAR_IS_UNSIGNED

#ifdef TCC_ARM_HARDFLOAT
#define ARM_FLOAT_ABI ARM_HARD_FLOAT
#else
#define ARM_FLOAT_ABI ARM_SOFTFP_FLOAT
#endif

#else // TARGET_DEFS_ONLY

#define USING_GLOBALS
#include "tcc.h"

#include "arm-thumb-opcodes.h"

ST_DATA const char *const target_machine_defs = "__arm__\0"
                                                "__arm\0"
                                                "arm\0"
                                                "__arm_elf__\0"
                                                "__arm_elf\0"
                                                "arm_elf\0"
#if defined TCC_TARGET_ARM_ARCHV8M
                                                "__ARM_ARCH_8M__\0"
#endif // TCC_TARGET_ARM_ARCHV8M
                                                "__ARMEL__\0"
                                                "__APCS_32__\0"
#if defined TCC_ARM_EABI
                                                "__ARM_EABI__\0"
#endif
    ;

enum float_abi float_abi;
unsigned char text_and_data_separation;
unsigned char pic;

int offset_to_args = 0;

flags_behaviour g_setflags = FLAGS_BEHAVIOUR_SET;

uint32_t caller_saved_registers;
uint32_t pushed_registers;

TACQuadruple function_arguments[4];
int function_argument_count = 0;

ST_DATA const int reg_classes[NB_REGS] = {
    /* r0 */ RC_INT | RC_R0,
    /* r1 */ RC_INT | RC_R1,
    /* r2 */ RC_INT | RC_R2,
    /* r3 */ RC_INT | RC_R3,
    /* r12 */ RC_INT | RC_R12,
    /* f0 */ RC_FLOAT | RC_F0,
    /* f1 */ RC_FLOAT | RC_F1,
    /* f2 */ RC_FLOAT | RC_F2,
    /* f3 */ RC_FLOAT | RC_F3,
#ifdef TCC_ARM_VFP
    /* d4/s8 */ RC_FLOAT | RC_F4,
    /* d5/s10 */ RC_FLOAT | RC_F5,
    /* d6/s12 */ RC_FLOAT | RC_F6,
    /* d7/s14 */ RC_FLOAT | RC_F7,
#endif
};

#define CHECK_R(r) ((r) >= TREG_R0 && (r) <= TREG_LR)

int is_valid_opcode(thumb_opcode op);
int ot(thumb_opcode op);

int ot_check(thumb_opcode op) {
  if (!is_valid_opcode(op)) {
    tcc_error("compiler_error: received invalid opcode: 0x%x\n", op.opcode);
  }
  return ot(op);
}

static int two2mask(int a, int b) {
  if (!CHECK_R(a) || !CHECK_R(b))
    tcc_error("compiler error! registers %i,%i is not valid", a, b);
  return (reg_classes[a] | reg_classes[b]) & ~(RC_INT | RC_FLOAT);
}

static uint32_t mapcc(int cc) {
  switch (cc) {
  case TOK_ULT:
    return 0x3; /* CC/LO */
  case TOK_UGE:
    return 0x2; /* CS/HS */
  case TOK_EQ:
    return 0x0; /* EQ */
  case TOK_NE:
    return 0x1; /* NE */
  case TOK_ULE:
    return 0x9; /* LS */
  case TOK_UGT:
    return 0x8; /* HI */
  case TOK_Nset:
    return 0x4; /* MI */
  case TOK_Nclear:
    return 0x5; /* PL */
  case TOK_LT:
    return 0xB; /* LT */
  case TOK_GE:
    return 0xA; /* GE */
  case TOK_LE:
    return 0xD; /* LE */
  case TOK_GT:
    return 0xC; /* GT */
  }
  tcc_error("unexpected condition code");
  return 0xE; /* AL */
}

static int func_nregs = 0; // number of registers stored in function prologue
static int func_sub_sp_offset = 0;
static int leaffunc = 0; // function is leaf

#if defined(TCC_ARM_EABI) && !defined(CONFIG_TCC_ELFINTERP)
const char *default_elfinterp(struct TCCState *s) {
  // just for pass compilation, in the future add real loaders from yasos
  if (s->float_abi == ARM_HARD_FLOAT) {
    return "/lib/ld-linux-armhf.so";
  } else {
    return "/lib/ld-linux.so";
  }
}
#endif // TCC_ARM_EABI && !CONFIG_TCC_ELFINTERP

static CType float_type, double_type, func_float_type, func_double_type;

static int unalias_ldbl(int btype);
static int is_hgen_float_aggr(CType *type);
static uint32_t intr(int r);

#ifdef TCC_ARM_VFP
static uint32_t vfpr(int r);
#endif

struct avail_regs {
  signed char avail[3]; /* 3 holes max with only float and double alignments */
  int first_hole;       /* first available hole */
  int last_hole;        /* last available hole (none if equal to first_hole) */
  int first_free_reg;   /* next free register in the sequence, hole excluded */
};
#define AVAIL_REGS_INITIALIZER (struct avail_regs){{0, 0, 0}, 0, 0, 0}
/* Find suitable registers for a VFP Co-Processor Register Candidate (VFP CPRC
   param) according to the rules described in the procedure call standard for
   the ARM architecture (AAPCS). If found, the registers are assigned to this
   VFP CPRC parameter. Registers are allocated in sequence unless a hole exists
   and the parameter is a single float.

   avregs: opaque structure to keep track of available VFP co-processor regs
   align: alignment constraints for the param, as returned by type_size()
   size: size of the parameter, as returned by type_size() */
int assign_vfpreg(struct avail_regs *avregs, int align, int size) {
  int first_reg = 0;

  if (avregs->first_free_reg == -1)
    return -1;
  if (align >> 3) { /* double alignment */
    first_reg = avregs->first_free_reg;
    /* alignment constraint not respected so use next reg and record hole */
    if (first_reg & 1)
      avregs->avail[avregs->last_hole++] = first_reg++;
  } else { /* no special alignment (float or array of float) */
    /* if single float and a hole is available, assign the param to it */
    if (size == 4 && avregs->first_hole != avregs->last_hole)
      return avregs->avail[avregs->first_hole++];
    else
      first_reg = avregs->first_free_reg;
  }
  if (first_reg + size / 4 <= 16) {
    avregs->first_free_reg = first_reg + size / 4;
    return first_reg;
  }
  avregs->first_free_reg = -1;
  return -1;
}

/* Parameters are classified according to how they are copied to their final
   destination for the function call. Because the copying is performed class
   after class according to the order in the union below, it is important that
   some constraints about the order of the members of this union are respected:
   - CORE_STRUCT_CLASS must come after STACK_CLASS;
   - CORE_CLASS must come after STACK_CLASS, CORE_STRUCT_CLASS and
     VFP_STRUCT_CLASS;
   - VFP_STRUCT_CLASS must come after VFP_CLASS.
   See the comment for the main loop in copy_params() for the reason. */
enum reg_class {
  STACK_CLASS = 0,
  CORE_STRUCT_CLASS,
  VFP_CLASS,
  VFP_STRUCT_CLASS,
  CORE_CLASS,
  NB_CLASSES
};

struct param_plan {
  int start;    /* first reg or addr used depending on the class */
  int end;      /* last reg used or next free addr depending on the class */
  SValue *sval; /* pointer to SValue on the value stack */
  struct param_plan *prev; /*  previous element in this class */
};

struct plan {
  struct param_plan *pplans;               /* array of all the param plans */
  struct param_plan *clsplans[NB_CLASSES]; /* per class lists of param plans */
  int nb_plans;
};

static void add_param_plan(struct plan *plan, int cls, int start, int end,
                           SValue *v) {
  struct param_plan *p = &plan->pplans[plan->nb_plans++];
  p->prev = plan->clsplans[cls];
  plan->clsplans[cls] = p;
  p->start = start, p->end = end, p->sval = v;
}

/* Assign parameters to registers and stack with alignment according to the
   rules in the procedure call standard for the ARM architecture (AAPCS).
   The overall assignment is recorded in an array of per parameter structures
   called parameter plans. The parameter plans are also further organized in a
   number of linked lists, one per class of parameter (see the comment for the
   definition of union reg_class).

   nb_args: number of parameters of the function for which a call is generated
   float_abi: float ABI in use for this function call
   plan: the structure where the overall assignment is recorded
   todo: a bitmap that record which core registers hold a parameter

   Returns the amount of stack space needed for parameter passing

   Note: this function allocated an array in plan->pplans with tcc_malloc. It
   is the responsibility of the caller to free this array once used (ie not
   before copy_params). */
static int assign_regs(int nb_args, int float_abi, struct plan *plan,
                       int *todo) {
  int i, size, align;
  int ncrn /* next core register number */,
      nsaa /* next stacked argument address*/;
  struct avail_regs avregs = {{0}};

  ncrn = nsaa = 0;
  *todo = 0;

  for (i = nb_args; i--;) {
    int j, start_vfpreg = 0;
    CType type = vtop[-i].type;
    ElfSym *sym = NULL;
    type.t &= ~VT_ARRAY;
    size = type_size(&type, &align);
    size = (size + 3) & ~3;
    align = (align + 3) & ~3;
    // if argument is a function pointer, then symbol must be exported
    if (vtop[-i].r & VT_SYM) {
      if (((type.t & VT_BTYPE) == VT_FUNC) ||
          ((type.t & VT_BTYPE) == VT_PTR && type.ref &&
           (type.ref->type.t & VT_BTYPE) == VT_FUNC)) {
        sym = elfsym(vtop[-i].sym);
      }
    }
    if (sym != NULL) {
      sym->st_info |= (STB_GLOBAL << 4);
    }

    switch (vtop[-i].type.t & VT_BTYPE) {
    case VT_STRUCT:
    case VT_FLOAT:
    case VT_DOUBLE:
    case VT_LDOUBLE:
      if (float_abi == ARM_HARD_FLOAT) {
        int is_hfa = 0; /* Homogeneous float aggregate */

        if (is_float(vtop[-i].type.t) ||
            (is_hfa = is_hgen_float_aggr(&vtop[-i].type))) {
          int end_vfpreg;

          start_vfpreg = assign_vfpreg(&avregs, align, size);
          end_vfpreg = start_vfpreg + ((size - 1) >> 2);
          if (start_vfpreg >= 0) {
            add_param_plan(plan, is_hfa ? VFP_STRUCT_CLASS : VFP_CLASS,
                           start_vfpreg, end_vfpreg, &vtop[-i]);
            continue;
          } else
            break;
        }
      }
      ncrn = (ncrn + (align - 1) / 4) & ~((align / 4) - 1);
      if (ncrn + size / 4 <= 4 || (ncrn < 4 && start_vfpreg != -1)) {
        /* The parameter is allocated both in core register and on stack. As
         * such, it can be of either class: it would either be the last of
         * CORE_STRUCT_CLASS or the first of STACK_CLASS. */
        for (j = ncrn; j < 4 && j < ncrn + size / 4; j++)
          *todo |= (1 << j);
        add_param_plan(plan, CORE_STRUCT_CLASS, ncrn, j, &vtop[-i]);
        ncrn += size / 4;
        if (ncrn > 4)
          nsaa = (ncrn - 4) * 4;
      } else {
        ncrn = 4;
        break;
      }
      continue;
    default:
      if (ncrn < 4) {
        int is_long = (vtop[-i].type.t & VT_BTYPE) == VT_LLONG;
        if (is_long) {
          ncrn = (ncrn + 1) & -2;
          if (ncrn == 4)
            break;
        }
        add_param_plan(plan, CORE_CLASS, ncrn, ncrn + is_long, &vtop[-i]);
        ncrn += 1 + is_long;
        continue;
      }
    }
    nsaa = (nsaa + (align - 1)) & ~(align - 1);
    add_param_plan(plan, STACK_CLASS, nsaa, nsaa + size, &vtop[-i]);
    nsaa += size; /* size already rounded up before */
  }
  return nsaa;
}

ST_FUNC void arm_init(struct TCCState *s) {
  float_type.t = VT_FLOAT;
  double_type.t = VT_DOUBLE;
  func_float_type.t = VT_FUNC;
  func_float_type.ref = sym_push(SYM_FIELD, &float_type, FUNC_CDECL, FUNC_OLD);
  func_double_type.t = VT_FUNC;
  func_double_type.ref =
      sym_push(SYM_FIELD, &double_type, FUNC_CDECL, FUNC_OLD);
  float_abi = s->float_abi;
  text_and_data_separation = s->text_and_data_separation;
  pic = s->pic;
  s->parameters_registers = 4;
  s->registers_map_for_allocator =
      (1 << ARM_R0) | (1 << ARM_R1) | (1 << ARM_R2) | (1 << ARM_R3) |
      (1 << ARM_R4) | (1 << ARM_R5) | (1 << ARM_R6) | (1 << ARM_R8) |
      (1 << ARM_R10) | (1 << ARM_R11) | (1 << ARM_R12);

  s->registers_for_allocator = 11;
  caller_saved_registers =
      (1 << ARM_R0) | (1 << ARM_R1) | (1 << ARM_R2) | (1 << ARM_R3);

  if (!s->pic) {
    s->registers_map_for_allocator |= (1 << ARM_R9);
    s->registers_for_allocator += 1;
  }

  if (s->omit_frame_pointer) {
    s->registers_map_for_allocator |= (1 << ARM_R7);
    s->registers_for_allocator += 1;
  }
}

static int regmask(int r) { return reg_classes[r] & ~(RC_INT | RC_FLOAT); }

/*
 * Write 2 - byte Thumb instruction
 * current write position must be 16-bit aligned
 */
void o(unsigned int i) {
  const int ind1 = ind + 2;
  TRACE("  o: 0x%03x pc: 0x%x", i, ind);
  if (nocode_wanted) {
    return;
  }
  if (!cur_text_section) {
    tcc_error("compiler error! This happens f.ex. if the compiler\n"
              "can't evaluate constant expressions outside of a function.");
  }
  if (ind1 > cur_text_section->data_allocated) {
    section_realloc(cur_text_section, ind1);
  }
  cur_text_section->data[ind++] = i & 255;
  cur_text_section->data[ind++] = i >> 8;
}

int is_valid_opcode(thumb_opcode op) { return (op.size == 2 || op.size == 4); }

int ot(thumb_opcode op) {
  if (op.size == 0)
    return op.size;

  if (op.size == 4)
    o(op.opcode >> 16);
  o(op.opcode & 0xffff);
  return op.size;
}

static void load_full_const(int r, int32_t imm, struct Sym *sym);

// TODO: this is armv7-m code
int decbranch(int pos) {
  int xa = *(uint16_t *)(cur_text_section->data + pos);
  int xb = *(uint16_t *)(cur_text_section->data + pos + 2);

  TRACE("  decbranch ins at pos 0x%.8x, target inst 0x%x 0x%x", pos, xa, xb);

  if ((xa & 0xf000) == 0xd000) {
    // Branch encoding t1
    xa &= 0x00ff;
    if (xa & 0x0080)
      xa -= 0x100;
    xa = (xa * 2) + pos + 4;
  } else if ((xa & 0xf800) == 0xe000) {
    // Branch encoding t2
    xa &= 0x7ff;
    if (xa & 0x400)
      xa -= 0x800;
    xa = (xa * 2) + pos + 4;
  } else if ((xa & 0xf800) == 0xf000 && (xb & 0xd000) == 0x8000) {
    // Branch encoding t3
    uint32_t s = (xa >> 10) & 1;
    uint32_t imm6 = (xa & 0x3f);
    uint32_t j1 = (xb >> 13) & 1;
    uint32_t j2 = (xb >> 11) & 1;
    uint32_t imm11 = xb & 0x7ff;

    //      10 9876543210 9876543210 9876543210
    // IMM:             s 21bbbbbbaa aaaaaaaaa0
    // IMM:               s21bbbbbba aaaaaaaaaa
    uint32_t ret = (j2 << 19) | (j1 << 18) | (imm6 << 12) | (imm11 << 1);
    if (s)
      ret |= 0xfff00000;

    xa = ret + pos + 4;
  } else if ((xa & 0xf800) == 0xf000 && (xb & 0xd000) == 0x9000) {
    // Branch encoding t4
    uint32_t s = (xa >> 10) & 1;
    uint32_t imm10 = (xa & 0x3ff);
    uint32_t j1 = (xb >> 13) & 1;
    uint32_t j2 = (xb >> 11) & 1;
    uint32_t imm11 = xb & 0x7ff;

    uint32_t i1 = ~(j1 ^ s) & 1;
    uint32_t i2 = ~(j2 ^ s) & 1;

    //      10 9876543210 9876543210 9876543210
    // IMM:         s21bb bbbbbbbbaa aaaaaaaaa0
    uint32_t ret = (i2 << 23) | (i1 << 22) | (imm10 << 12) | (imm11 << 1);
    if (s)
      ret |= 0xff000000;

    xa = ret + pos + 4;
  } else {
    tcc_error(
        "internal error: decbranch unknown encoding pos 0x%x, inst: 0x%x\n",
        pos, xa);
    return 0;
  }

  return xa;
}

static thumb_opcode th_generic_mov_imm(uint32_t r, uint32_t imm) {
  return th_mov_imm(r, imm, FLAGS_BEHAVIOUR_NOT_IMPORTANT,
                    ENFORCE_ENCODING_NONE);
}
int th_offset_to_reg(int off, int sign) {
  // we will crash if there is no reg available
  // int rr = get_reg(RC_INT);
  int rr =
      R_LR; // can I use R_LR here? lr should be already saved in proluge right?

  // if mov is not possible then load from data
  if (!ot(th_generic_mov_imm(rr, off))) {
    load_full_const(rr, sign ? -off : off, NULL);
    return rr;
  }

  if (sign)
    ot_check(th_rsb_imm(rr, rr, 0, FLAGS_BEHAVIOUR_NOT_IMPORTANT));
  return rr;
}

int th_patch_call(int t, int a) {
  uint16_t *x = (uint16_t *)(cur_text_section->data + t);
  int lt = t;

  TRACE("'th_patch_call' t: %.8x, a: %.8x\n", t, a);

  t = decbranch(t);
  TRACE("t: %.8x\n", t);
  if (a == lt + 2)
    *x = 0xbf00;
  else if ((*x & 0xf000) == 0xd000) {
    *x &= 0xff00;
    *x |= th_encbranch_8(lt, a);
  } else if ((*x & 0xf800) == 0xe000) {
    *x &= 0xf800;
    *x |= th_encbranch_11(lt, a);
  } else if ((x[0] & 0xf800) == 0xf000 && (x[1] & 0xd000) == 0x8000) {
    uint32_t enc = 0;
    x[0] &= 0xfbc0;
    x[1] &= 0xd000;
    enc = th_encbranch_b_t3(th_encbranch_20(lt, a));
    x[0] |= enc >> 16;
    x[1] |= enc;
  } else if ((x[0] & 0xf800) == 0xf000 && (x[1] & 0xd000) == 0x9000) {
    uint32_t enc = 0;
    x[0] &= 0xf800;
    x[1] &= 0xd000;
    enc = th_packimm_10_11_0(th_encbranch_20(lt, a) << 1);
    x[0] |= enc >> 16;
    x[1] |= enc;
  } else
    tcc_error("compiler_error: unhandled branch type in th_patch_call for: t: "
              "0x%x, a: 0x%x, x: 0x%x 0x%x\n",
              t, a, x[0], x[1]);

  return t;
}

static void gadd_sp(int val) {
  if (val > 0) {
    ot_check(th_add_sp_imm(R_SP, val, FLAGS_BEHAVIOUR_NOT_IMPORTANT,
                           ENFORCE_ENCODING_NONE));
  } else if (val < 0) {
    ot_check(th_sub_sp_imm(R_SP, -val, FLAGS_BEHAVIOUR_NOT_IMPORTANT,
                           ENFORCE_ENCODING_NONE));
  }
}

static void gcall_or_jmp(int is_jmp) {
  if ((vtop->r & (VT_VALMASK | VT_LVAL)) == VT_CONST) {
    uint32_t x = th_encbranch(ind, ind + vtop->c.i);

    TRACE("gcall_or_jmp: %d, ind: 0x%x, 0x%x", is_jmp, ind, x);
    if (x) {
      if (vtop->r & VT_SYM)
        greloc(cur_text_section, vtop->sym, ind, R_ARM_THM_JUMP24);
      ot_check(th_bl_t1(x));
    }
  } else {
    int r = gv(RC_INT);
    TRACE("gcall_or_jmp indirect call");
    ot_check(th_orr_imm(r, r, 1, FLAGS_BEHAVIOUR_NOT_IMPORTANT));
    if (!is_jmp)
      ot_check(th_blx_reg(intr(r)));
    else
      ot_check(th_bx_reg(intr(r)));
  }
}

/* Copy parameters to their final destination (core reg, VFP reg or stack) for
   function call.

   nb_args: number of parameters the function take
   plan: the overall assignment plan for parameters
   todo: a bitmap indicating what core reg will hold a parameter

   Returns the number of SValue added by this function on the value stack */
static int copy_params(int nb_args, struct plan *plan, int todo) {
  int size, align, i, nb_extra_sval = 0;
  uint32_t r = 0;
  struct param_plan *pplan;
  int pass = 0;

  /* Several constraints require parameters to be copied in a specific order:
     - structures are copied to the stack before being loaded in a reg;
     - floats loaded to an odd numbered VFP reg are first copied to the
       preceding even numbered VFP reg and then moved to the next VFP reg.

     It is thus important that:
     - structures assigned to core regs must be copied after parameters
       assigned to the stack but before structures assigned to VFP regs because
       a structure can lie partly in core registers and partly on the stack;
     - parameters assigned to the stack and all structures be copied before
       parameters assigned to a core reg since copying a parameter to the stack
       require using a core reg;
     - parameters assigned to VFP regs be copied before structures assigned to
       VFP regs as the copy might use an even numbered VFP reg that already
       holds part of a structure. */
again:
  for (i = 0; i < NB_CLASSES; i++) {
    for (pplan = plan->clsplans[i]; pplan; pplan = pplan->prev) {

      if (pass && (i != CORE_CLASS || pplan->sval->r < VT_CONST))
        continue;

      vpushv(pplan->sval);
      pplan->sval->r = pplan->sval->r2 = VT_CONST; /* disable entry */
      switch (i) {
      case STACK_CLASS:
      case CORE_STRUCT_CLASS:
      case VFP_STRUCT_CLASS:
        if ((pplan->sval->type.t & VT_BTYPE) == VT_STRUCT) {
          int padding = 0;
          size = type_size(&pplan->sval->type, &align);
          /* align to stack align size */
          size = (size + 3) & ~3;
          if (i == STACK_CLASS && pplan->prev)
            padding = pplan->start - pplan->prev->end;
          size += padding; /* Add padding if any */
          /* allocate the necessary size on stack */
          gadd_sp(-size);
          /* generate structure store */
          r = get_reg(RC_INT);
          ot_check(th_add_sp_imm(intr(r), padding,
                                 FLAGS_BEHAVIOUR_NOT_IMPORTANT,
                                 ENFORCE_ENCODING_NONE));
          vset(&vtop->type, r | VT_LVAL, 0);
          vswap();
          /* XXX: optimize. Save all register because memcpy can use them */
          ot_check(th_vpush(0xffffffff, false));
          // wait haven't we just stored? in 746
          vstore(); /* memcpy to current sp + potential padding */
          ot_check(th_vpop(0xffffffff, false));

          /* Homogeneous float aggregate are loaded to VFP registers
             immediately since there is no way of loading data in multiple
             non consecutive VFP registers as what is done for other
             structures (see the use of todo). */
          if (i == VFP_STRUCT_CLASS) {
            int first = pplan->start, nb = pplan->end - first + 1;
            /* vpop.32 {pplan->start, ..., pplan->end} */
            int regs = 0;
            for (int j = 0; j < nb; j++)
              regs |= 1 << (first + j);
            ot_check(th_vpop(regs, false));
            /* No need to write the register used to a SValue since VFP regs
               cannot be used for gcall_or_jmp */
          }
        } else {
          if (is_float(pplan->sval->type.t)) {
#ifdef TCC_ARM_VFP
            int is_doubleword = 0;
            r = vfpr(gv(RC_FLOAT));
            if ((pplan->sval->type.t & VT_BTYPE) == VT_FLOAT)
              is_doubleword = 0;
            else {
              is_doubleword = 1;
            }
            ot_check(th_vpush(r, is_doubleword));
#else
            r = fpr(gv(RC_FLOAT)) << 12;
            if ((pplan->sval->type.t & VT_BTYPE) == VT_FLOAT)
              size = 4;
            else if ((pplan->sval->type.t & VT_BTYPE) == VT_DOUBLE)
              size = 8;
            else
              size = LDOUBLE_SIZE;

            if (size == 12)
              r |= 0x400000;
            else if (size == 8)
              r |= 0x8000;
            tcc_error("compiler_error: implement vpush for fpa\n");
            // o(0xED2D0100|r|(size>>2)); /* some kind of vpush for FPA */
#endif
          } else {
            /* simple type (currently always same size) */
            /* XXX: implicit cast ? */
            size = 4;
            if ((pplan->sval->type.t & VT_BTYPE) == VT_LLONG) {
              lexpand();
              size = 8;
              r = gv(RC_INT);
              ot_check(th_push(1 << intr(r)));
              vtop--;
              print_vstack("copy_params(1)");
            }
            r = gv(RC_INT);
            ot_check(th_push(1 << intr(r)));
          }
          if (i == STACK_CLASS && pplan->prev)
            gadd_sp(pplan->prev->end - pplan->start); /* Add padding if any */
        }
        break;

      case VFP_CLASS:
        gv(regmask(TREG_F0 + (pplan->start >> 1)));
        if (pplan->start & 1) { /* Must be in upper part of double register */
          ot_check(th_vmov_register(pplan->start, pplan->start - 1, 0));
          vtop->r =
              VT_CONST; /* avoid being saved on stack by gv for next float */
        }
        break;

      case CORE_CLASS:
        if ((pplan->sval->type.t & VT_BTYPE) == VT_LLONG) {
          lexpand();
          gv(regmask(pplan->end));
          pplan->sval->r2 = vtop->r;
          vtop--;
          print_vstack("copy_params(CORE_CLASS)");
        }
        gv(regmask(pplan->start));
        /* Mark register as used so that gcall_or_jmp use another one
           (regs >=4 are free as never used to pass parameters) */
        pplan->sval->r = vtop->r;
        break;
      }

      vtop--;
      print_vstack("copy_params(ALL)");
    }
  }

  /* second pass to restore registers that were saved on stack by accident.
     Maybe redundant after the "lvalue_save" patch in tccgen.c:gv() */
  if (++pass < 2)
    goto again;

  /* Manually free remaining registers since next parameters are loaded
   * manually, without the help of gv(int). */
  save_regs(nb_args);

  if (todo) {
    ot_check(th_pop(todo));
    for (pplan = plan->clsplans[CORE_STRUCT_CLASS]; pplan;
         pplan = pplan->prev) {
      int r;
      pplan->sval->r = pplan->start;
      /* An SValue can only pin 2 registers at best (r and r2) but a structure
         can occupy more than 2 registers. Thus, we need to push on the value
         stack some fake parameter to have on SValue for each registers used
         by a structure (r2 is not used). */
      for (r = pplan->start + 1; r <= pplan->end; r++) {
        if (todo & (1 << r)) {
          nb_extra_sval++;
          vpushi(0);
          vtop->r = r;
        }
      }
    }
  }
  return nb_extra_sval;
}

ST_FUNC void gen_fill_nops(int bytes) {
  TRACE("'gen_fill_nops'");

  if (bytes & 1) {
    tcc_error(
        "compiler_error: 'gen_fill_nops' bytes are not aligned to: 2-bytes\n");
    return;
  }
  while (bytes > 0) {
    ot_check(th_nop(ENFORCE_ENCODING_16BIT));
    bytes -= 2;
  }
}

// generate function prolog
void gfunc_prolog(Sym *func_sym) {
  CType *func_type = &func_sym->type;
  Sym *sym, *sym2;
  int n, nf, size, align, rs, struct_ret = 0;
  int addr, pn, sn; /* pn=core, sn=stack */
  CType ret_type;
  int est;

  struct avail_regs avregs = {{0}}; // AVAIL_REGS_INITIALIZER;

  TRACE("########## gfunc_prolog ########## func_vt.t %d, name: %s",
        func_vt.t & VT_BTYPE, get_tok_str(func_sym->v, NULL));

  sym = func_type->ref;
  func_vt = sym->type;
  func_var = (func_type->ref->f.func_type == FUNC_ELLIPSIS);
  n = 0;
  nf = 0;
  if ((func_vt.t & VT_BTYPE) == VT_STRUCT &&
      !gfunc_sret(&func_vt, func_var, &ret_type, &align, &rs)) {
    n++;
    struct_ret = 1;
    func_vc = 12; /* Offset from fp of the place to store the result */
  }
  for (sym2 = sym->next; sym2 && (n < 4 || nf < 16); sym2 = sym2->next) {
    size = type_size(&sym2->type, &align);
    if (float_abi == ARM_HARD_FLOAT && !func_var &&
        (is_float(sym2->type.t) || is_hgen_float_aggr(&sym2->type))) {
      int tmpnf = assign_vfpreg(&avregs, align, size);
      tmpnf += (size + 3) / 4;
      nf = (tmpnf > nf) ? tmpnf : nf;
    } else if (n < 4)
      n += (size + 3) / 4;
  }
  th_sym_t();
  if (func_var)
    n = 4;

  if (n) {
    if (n > 4)
      n = 4;
    n = (n + 1) & -2;
    func_nregs = n;
    TRACE("  save r0-r4 on stack, n %i", n);
    ot_check(th_push((1 << n) - 1));
  } else
    func_nregs = 0;

  if (nf) {
    int regs = 0;
    if (nf > 16)
      nf = 16;
    nf = (nf + 1) & -2; /* nf => HARDFLOAT => EABI */
    for (int i = 0; i < nf; i++)
      regs |= 1 << i;
    TRACE("  save s0-s15 on stack if needed");
    ot_check(th_vpush(regs, false));
    func_nregs += nf;
  }

  ot_check(th_push(0x5800)); // push {fp, ip, lr} (r11, r12, r14)
  ot_check(th_mov_reg(11, 13, FLAGS_BEHAVIOUR_NOT_IMPORTANT,
                      THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE,
                      false)); // mov fp, sp
  // nop has 2 bytes
  // I need 4 bytes for address and 4 bytes for instruction in the worst case
  // scenario

  // nooo there must be a better way to do this
  // maybe in case of full loading use branch to epilogue code?
  // ind + branch instruction + ldr is 4 bytes
  est = th_ldr_literal_estimate(R_LR, 4);
  est += 2; // 2 bytes for the branch instruction
  est += 2; // 2 bytes for the sub instruction
  est += ind;
  // align to 4 bytes for memory access
  if (est & 3) {
    ot_check(th_nop(ENFORCE_ENCODING_16BIT));
  }
  ot_check(th_ldr_literal(R_LR, 4, 1));
  ot_check(th_add_sp_reg(R_SP, R_LR, FLAGS_BEHAVIOUR_NOT_IMPORTANT,
                         ENFORCE_ENCODING_NONE, THUMB_SHIFT_DEFAULT));
  ot_check(th_b_t2(2));

  func_sub_sp_offset = ind;
  // ot_check(th_nop()); /* leave space for stack adjustment in epilog */
  // ot_check(th_nop());
  ot((thumb_opcode){
      .size = 4,
      .opcode = 0x00000000,
  });

  if (float_abi == ARM_HARD_FLOAT) {
    func_vc += nf * 4;
    memset(&avregs, 0, sizeof(avregs));
    // avregs = AVAIL_REGS_INITIALIZER;
  }

  pn = struct_ret, sn = 0;
  while ((sym = sym->next)) {
    CType *type;
    type = &sym->type;
    size = type_size(type, &align);
    size = (size + 3) >> 2;
    align = (align + 3) & ~3;

    if (float_abi == ARM_HARD_FLOAT && !func_var &&
        (is_float(sym->type.t) || is_hgen_float_aggr(&sym->type))) {
      int fpn = assign_vfpreg(&avregs, align, size << 2);
      if (fpn >= 0)
        addr = fpn * 4;
      else
        goto from_stack;
    } else if (pn < 4) {
      pn = (pn + (align - 1) / 4) & -(align / 4);
      addr = (nf + pn) * 4;
      pn += size;
      if (!sn && pn > 4)
        sn = (pn - 4);
    } else {
    from_stack:
      sn = (sn + (align - 1) / 4) & -(align / 4);
      addr = (n + nf + sn) * 4;
      sn += size;
    }
    sym_push(sym->v & ~SYM_FIELD, type, VT_LOCAL | VT_LVAL, addr + 12);
  }
  leaffunc = 1;
  loc = 0;
}

// all params needs to be passed in core registers or not
static int floats_in_core_regs(const SValue *sval) {
  if (!sval->sym) {
    return 0;
  }

  switch (sval->sym->v) {
  case TOK___floatundidf:
  case TOK___floatundisf:
  case TOK___fixunsdfdi:
  case TOK___fixunssfdi:
  case TOK___floatdisf:
  case TOK___floatdidf:
  case TOK___fixsfdi:
  case TOK___fixdfdi:
    return 1;
  default:
    return 0;
  }
}

void gfunc_call(int nb_args) {
  int r;
  int args_size;
  int def_float_abi = float_abi;
  int todo;
  struct plan plan;
  int variadic;
  int x;

  TRACE("'gfunc_call: nb_args: %d, float_abi: %d'", nb_args, float_abi);
  // we will be calling a function, R9 must be saved for Yasos.zig
  ot_check(th_push(1 << R9 | 1 << R_IP));

  if (float_abi == ARM_HARD_FLOAT) {
    variadic = (vtop[-nb_args].type.ref->f.func_type == FUNC_ELLIPSIS);
    if (variadic || floats_in_core_regs(&vtop[-nb_args]))
      float_abi = ARM_SOFTFP_FLOAT;
  }
  r = vtop->r & VT_VALMASK;
  if (r == VT_CMP || (r & ~1) == VT_JMP)
    gv(RC_INT);

  memset(&plan, 0, sizeof(plan));
  if (nb_args)
    plan.pplans = tcc_malloc(nb_args * sizeof(*plan.pplans));
  args_size = assign_regs(nb_args, float_abi, &plan, &todo);

  if (args_size & 7) // stack must be 8-byte aligned according to AAPCS for EABI
  {
    args_size = (args_size + 7) & ~7;
    ot_check(th_sub_sp_imm(R_SP, 4, FLAGS_BEHAVIOUR_NOT_IMPORTANT,
                           ENFORCE_ENCODING_NONE));
  }
  x = copy_params(nb_args, &plan, todo);
  nb_args += x;
  tcc_free(plan.pplans);

  vrotb(nb_args + 1);
  gcall_or_jmp(0);

  if (args_size)
    gadd_sp(args_size);
  if (float_abi == ARM_SOFTFP_FLOAT && is_float(vtop->type.ref->type.t)) {
    if ((vtop->type.ref->type.t & VT_BTYPE) == VT_FLOAT)
      ot_check(th_vmov_gp_sp(0, 0, 0));
    else
      ot_check(th_vmov_2gp_dp(0, 1, 0, 0));
  }
  vtop -= nb_args + 1; // +1 is function address
  print_vstack("gfunc_call(0)");
  leaffunc = 0;
  ot_check(th_pop(1 << R9 | 1 << R_IP));
  TRACE("gfunc_call finished");
  float_abi = def_float_abi;
}

void gfunc_epilog(void) {
  int diff = 0;
  TRACE("'gfunc_epilog'");
  // copy float return value to core register if base standard is used
  // and float computation is made with VFP
  if ((float_abi == ARM_SOFTFP_FLOAT || func_var) && is_float(func_vt.t)) {
    if ((func_vt.t & VT_BTYPE) == VT_FLOAT) {
      ot_check(th_vmov_gp_sp(R0, 0, 1));
    } else // double
    {
      ot_check(th_vmov_2gp_dp(R0, R1, 0, 1));
    }
  }
  // align stack
  diff = (-loc + 3) & -4;
  if (!leaffunc)
    diff = ((diff + 11) & -8) - 4;
  if (diff > 0) {
    if (!ot(th_add_sp_imm(R_SP, diff, FLAGS_BEHAVIOUR_NOT_IMPORTANT,
                          ENFORCE_ENCODING_NONE))) {
      int rr = th_offset_to_reg(diff, 0);
      ot_check(th_add_sp_reg(rr, rr, FLAGS_BEHAVIOUR_NOT_IMPORTANT,
                             ENFORCE_ENCODING_NONE, THUMB_SHIFT_DEFAULT));
      ot_check(th_mov_reg(R_SP, rr, FLAGS_BEHAVIOUR_NOT_IMPORTANT,
                          THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE, false));
    }
  }

  ot_check(th_pop((1 << R_FP) | (1 << R_IP) | (1 << R_LR)));

  // what if diff is too far for sub sp imm?
  if (diff > 0) {
    *(uint32_t *)(cur_text_section->data + func_sub_sp_offset) = -diff;
  }

  if (func_nregs) {
    ot_check(th_add_sp_imm(R_SP, func_nregs << 2, FLAGS_BEHAVIOUR_NOT_IMPORTANT,
                           ENFORCE_ENCODING_NONE));
  }
  ot_check(th_bx_reg(R_LR));

  if (ind & 3)
    ot_check(th_nop(ENFORCE_ENCODING_16BIT));
}

void ggoto(void) {
  TRACE("'ggoto'");
  gcall_or_jmp(1);

  vtop--;
  print_vstack("ggoto");
}

ST_FUNC int gjmp(int t) {
  int r = ind;
  int val = ((t - r) >> 1) - 2;
  TRACE("gjump t: 0x%x, r: %d, val: %d", t, r, val);
  if (nocode_wanted)
    return t;

  // disable T16 instruction until root cause is found
  // if (val < -1024 || val > 1023)
  ot_check(th_b_t4(val << 1));
  // else
  // ot_check(th_b_t2(val << 1));
  return r;
}

ST_FUNC void gjmp_addr(int a) {
  TRACE("'gjump_addr'");
  gjmp(a);
}

ST_FUNC int gjmp_append(int n, int t) {
  int p, lp;
  TRACE("gjmp_append n: 0x%x, t: 0x%x", n, t);
  if (n) {
    p = n;
    do {
      p = decbranch(lp = p);
    } while (p);
    th_patch_call(lp, t);
    t = n;
  }
  return t;
}

ST_FUNC int gjmp_cond(int op, int t) {
  int r = ind;

  TRACE("'gjmp_cond' op: 0x%x, target 0x%x", op, t);

  if (nocode_wanted)
    return t;

  op = mapcc(op);

  ot_check(th_b_t3(op, th_encbranch_20(r, t)));
  return r;
}

void gsym_addr(int t, int a) {
  TRACE("'gsym_addr' %.8x branch target: %.8x\n", t, a);

  while (t)
    t = th_patch_call(t, a);
}

ST_FUNC void gen_vla_alloc(CType *type, int align) {
  // int r = intr(gv(RC_INT));
  // th_sub_reg(r, 13, r, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT,
  //            ENFORCE_ENCODING_NONE);
  // if (align < 8)
  //   align = 8;
  // if (align & (align - 1))
  //   tcc_error("alignment is not a power of 2: %i", align);
  // /* bic sp, r, #align-1 */
  // ot_check(th_bic_imm(r, r, align - 1, FLAGS_BEHAVIOUR_NOT_IMPORTANT));
  // ot_check(th_mov_reg(13, r, FLAGS_BEHAVIOUR_NOT_IMPORTANT,
  // THUMB_SHIFT_DEFAULT,
  //                     ENFORCE_ENCODING_NONE, false));
  // vpop();
  tcc_error("gen_vla_alloc not implemented yet");
}

ST_FUNC void gen_vla_sp_save(int addr) {
  tcc_error("gen_vla_sp_save not implemented yet");
  // SValue v;
  // v.type.t = VT_PTR;
  // v.r = VT_LOCAL | VT_LVAL;
  // v.c.i = addr;
  // store(TREG_SP, &v);
}

ST_FUNC void gen_vla_sp_restore(int addr) {
  tcc_error("gen_vla_sp_restore not implemented yet");
  // SValue v;
  // v.type.t = VT_PTR;
  // v.r = VT_LOCAL | VT_LVAL;
  // v.c.i = addr;
  // load(TREG_SP, &v);
}

static int unalias_ldbl(int btype) {
#if LDOUBLE_SIZE == 8
  if (btype == VT_LDOUBLE)
    btype = VT_DOUBLE;
#endif
  return btype;
}

/* Return whether a structure is an homogeneous float aggregate or not.
   The answer is true if all the elements of the structure are of the same
   primitive float type and there is less than 4 elements.

   type: the type corresponding to the structure to be tested */
static int is_hgen_float_aggr(CType *type) {
  if ((type->t & VT_BTYPE) == VT_STRUCT) {
    struct Sym *ref;
    int btype, nb_fields = 0;

    ref = type->ref->next;
    if (ref) {
      btype = unalias_ldbl(ref->type.t & VT_BTYPE);
      if (btype == VT_FLOAT || btype == VT_DOUBLE) {
        for (; ref && btype == unalias_ldbl(ref->type.t & VT_BTYPE);
             ref = ref->next, nb_fields++)
          ;
        return !ref && nb_fields <= 4;
      }
    }
  }
  return 0;
}

// How many registers are necessary to return struct via registers
// if not possible, then 0 means return via struct pointer
ST_FUNC int gfunc_sret(CType *vt, int variadic, CType *ret, int *ret_align,
                       int *regsize) {
  int align;
  const int size = type_size(vt, &align);

  TRACE("'gfunc_sret'");
  if (float_abi == ARM_HARD_FLOAT && !variadic &&
      (is_float(vt->t) || is_hgen_float_aggr(vt))) {
    *ret_align = 8;
    *regsize = 8;
    ret->ref = NULL;
    ret->t = VT_DOUBLE;
    return ceil_div(size, 8);
  } else if (size > 0 && size <= 4) {
    *ret_align = 4;
    *regsize = 4;
    ret->ref = NULL;
    ret->t = VT_INT;
    return 1;
  }
  return 0;
}

#ifdef TCC_ARM_VFP
static uint32_t vfpr(int r) {
  if (r < TREG_F0 || r > TREG_F7) {
    tcc_error("compiler_error: register: %d is not vfp register\n", r);
  }
  return r - TREG_F0;
}
#else
static uint32_t fpr(int r) {
  if (r < TREG_F0 || r > TREG_F3) {
    tcc_error("compiler_error: register: %d is not fp register\n", r);
  }
  return r - TREF_F0;
}
#endif
// are those offsets to allow TREG_R0 start from other register than r0?
// not sure
static uint32_t intr(int r) {
  if (r == TREG_R12) {
    return r;
  }
  if (r >= TREG_R0 && r <= TREG_R3) {
    return r - TREG_R0;
  }
  return r + (13 - TREG_SP);
}

void store(int r, SValue *sv) {
  int v, fc, ft, fr, sign;
  TRACE("'store' reg: %d", r);

  fr = sv->r;
  ft = sv->type.t;
  fc = sv->c.i;

  if (fc >= 0)
    sign = 0;
  else {
    sign = 1;
    fc = -fc;
  }

  v = fr & VT_VALMASK;

  if (fr & VT_LVAL || fr == VT_LOCAL) {
    uint32_t base = 11;
    if (v < VT_CONST) {
      base = intr(v);
      v = VT_LOCAL;
      fc = sign = 0;
    } else if (v == VT_CONST) {
      SValue v1;
      v1.type.t = ft;
      v1.r = fr & ~VT_LVAL;
      v1.c.i = sv->c.i;
      v1.sym = sv->sym;
      load(base = 14, &v1);
      fc = sign = 0;
      v = VT_LOCAL;
    }
    if (v == VT_LOCAL) {
      if (is_float(ft)) {
        if ((ft & VT_BTYPE) != VT_FLOAT)
          ot_check(th_vstr(base, vfpr(r), !sign, 1, fc));
        else
          ot_check(th_vstr(base, vfpr(r), !sign, 0, fc));
      } else if ((ft & VT_BTYPE) == VT_SHORT) {
        if (!ot(th_strh_imm(r, base, fc, sign ? 4 : 6,
                            ENFORCE_ENCODING_NONE))) {
          int rr = th_offset_to_reg(fc, sign);
          ot_check(th_strh_reg(r, base, rr, THUMB_SHIFT_DEFAULT,
                               ENFORCE_ENCODING_NONE));
        }
      } else if ((ft & VT_BTYPE) == VT_BYTE) {
        if (!ot(th_strb_imm(r, base, fc, sign ? 4 : 6,
                            ENFORCE_ENCODING_NONE))) {
          int rr = th_offset_to_reg(fc, sign);
          ot_check(th_strb_reg(r, base, rr, THUMB_SHIFT_DEFAULT,
                               ENFORCE_ENCODING_NONE));
        }
      } else {
        TRACE("store: sign: %x, r: %x, base: %x, fc: %x", sign, r, base, fc);
        if (!ot(th_str_imm(r, base, fc, sign ? 4 : 6, ENFORCE_ENCODING_NONE))) {
          int rr = th_offset_to_reg(fc, sign);
          ot_check(th_str_reg(r, base, rr, THUMB_SHIFT_DEFAULT,
                              ENFORCE_ENCODING_NONE));
        }
        TRACE("done");
      }
    }
  }
}

static void load_vt_lval_vt_local_float(int r, SValue *sv, int ft, int fc,
                                        int sign, uint32_t base) {
  if ((ft & VT_BTYPE) != VT_FLOAT) {
    // load double
    ot_check(th_vldr(base, vfpr(r), !sign, 1, fc));
  } else {
    ot_check(th_vldr(base, vfpr(r), !sign, 0, fc));
  }
}

static void load_full_const(int r, int32_t imm, struct Sym *sym) {
  int est = 0;
  ElfSym *esym = elfsym(sym);
  int sym_off = 0;
  TRACE("'load_full_const' to register: %d, with imm: %d\n", r, imm);
  est = th_ldr_literal_estimate(r, 4);
  est += 4; // branch instruction size
  est += ind;
  // 4-byte alignment
  if (est & 3)
    ot_check(th_nop(ENFORCE_ENCODING_16BIT));
  ot_check(th_ldr_literal(r, 4, 1));
  ot_check(th_b_t4(4));

  if (esym) {
    sym_off = esym->st_shndx;
  }

  if (!pic) {
    if (sym)
      greloc(cur_text_section, sym, ind, R_ARM_ABS32);
  } else {
    if (sym) {
      if (text_and_data_separation) {
        // all data except constants in .ro section can be addressed relative to
        // .got, how can I distinguish that situation?
        //

        if (sym->type.t & VT_STATIC && sym_off != cur_text_section->sh_num) {
          greloc(cur_text_section, sym, ind, R_ARM_GOTOFF);
        } else {
          greloc(cur_text_section, sym, ind, R_ARM_GOT32);
        }

      } else {
        if (sym->type.t & VT_STATIC) {
          greloc(cur_text_section, sym, ind, R_ARM_REL32);
        } else {
          greloc(cur_text_section, sym, ind, R_ARM_GOT_PREL);
        }
      }
    }
  }
  th_sym_d();
  // this immediate value will be relocated by the linker
  o(imm & 0xffff);
  o(imm >> 16);
  th_sym_t();

  if (pic) {
    if (sym) {
      if (text_and_data_separation) {
        if (sym->type.t & VT_STATIC && sym_off != cur_text_section->sh_num) {
          ot_check(th_add_reg(r, r, R9, FLAGS_BEHAVIOUR_NOT_IMPORTANT,
                              THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
        } else {
          thumb_opcode ot;
          ot_check(th_add_reg(r, r, R9, FLAGS_BEHAVIOUR_NOT_IMPORTANT,
                              THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));

          ot_check(th_ldr_imm(r, r, 0, 6, ENFORCE_ENCODING_NONE));
          ot = th_add_imm(r, r, imm, FLAGS_BEHAVIOUR_NOT_IMPORTANT,
                          ENFORCE_ENCODING_NONE);
          if (ot.size != 0) {
            ot_check(ot);
          } else {
            // size += o.size;
            // ot_check(o);
            ot_check(th_b_t4(4));
            th_sym_d();
            // thus that immediate value must be preserved without linker touch
            o(imm & 0xffff);
            o(imm >> 16);
            th_sym_t();
            ot_check(th_ldr_imm(R_LR, R_PC, 8, 4, ENFORCE_ENCODING_NONE));
            ot_check(th_add_reg(r, r, R_LR, FLAGS_BEHAVIOUR_NOT_IMPORTANT,
                                THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));

            // ot_check(th_bkpt(1));
          }
        }
      } else {
        if (sym->type.t & VT_STATIC) {
          ot_check(th_add_reg(r, r, R_PC, FLAGS_BEHAVIOUR_NOT_IMPORTANT,
                              THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
          ot_check(th_sub_imm(r, r, 8, FLAGS_BEHAVIOUR_NOT_IMPORTANT,
                              ENFORCE_ENCODING_NONE));
        } else {
          thumb_opcode ot;
          ot_check(th_add_reg(r, r, R_PC, FLAGS_BEHAVIOUR_NOT_IMPORTANT,
                              THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
          ot_check(th_ldr_imm(r, r, 4, 6, ENFORCE_ENCODING_NONE));
          ot = th_add_imm(r, r, imm, FLAGS_BEHAVIOUR_NOT_IMPORTANT,
                          ENFORCE_ENCODING_NONE);
          if (ot.size != 0) {
            ot_check(ot);
          } else {
            ot_check(th_b_t4(4));
            th_sym_d();
            o(imm & 0xffff);
            o(imm >> 16);
            th_sym_t();
            ot_check(th_ldr_imm(R_LR, R_PC, 8, 4, ENFORCE_ENCODING_NONE));
            ot_check(th_add_reg(r, r, R_LR, FLAGS_BEHAVIOUR_NOT_IMPORTANT,
                                THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
          }
        }
      }
    }
  }
}

int load_short_from_base(int ir, int base, int fc, int sign) {
  const thumb_opcode ins =
      th_ldrsh_imm(ir, base, fc, sign ? 4 : 6, ENFORCE_ENCODING_NONE);
  TRACE("Load short sign: %d, r %d, base: %d, fc: %d\n", sign, ir, base, fc);
  return ot(ins);
}

int load_ushort_from_base(int ir, int base, int fc, int sign) {
  const thumb_opcode ins =
      th_ldrh_imm(ir, base, fc, sign ? 4 : 6, ENFORCE_ENCODING_NONE);
  TRACE("Load ushort sign: %d, r %d, base: %d, fc: %d\n", sign, ir, base, fc);
  return ot(ins);
}

int load_byte_from_base(int ir, int base, int fc, int sign) {
  const thumb_opcode ins =
      th_ldrsb_imm(ir, base, fc, sign ? 4 : 6, ENFORCE_ENCODING_NONE);
  TRACE("Load byte sign: %d, r %d, base: %d, fc: %d\n", sign, ir, base, fc);
  return ot(ins);
}

int load_ubyte_from_base(int ir, int base, int fc, int sign) {
  const thumb_opcode ins =
      th_ldrb_imm(ir, base, fc, sign ? 4 : 6, ENFORCE_ENCODING_NONE);
  TRACE("Load ubyte sign: %d, r %d, base: %d, fc: %d\n", sign, ir, base, fc);
  return ot(ins);
}

int load_word_from_base(int ir, int base, int fc, int sign) {
  const thumb_opcode ins =
      th_ldr_imm(ir, base, fc, sign ? 4 : 6, ENFORCE_ENCODING_NONE);
  TRACE("Load word sign: %d, r %d, base: %d, fc: %d\n", sign, ir, base, fc);
  return ot(ins);
}

void load_vt_lval_vt_local(int r, SValue *sv, int ft, int fc, int sign,
                           uint32_t base) {
  int success = 0;
  const int btype = ft & VT_BTYPE;
  int ir = intr(r);
  TRACE("load_vt_lval_vt_local: fc: %i", fc);

  if (is_float(ft)) {
    TRACE("load float to r: %d, base: %d, fc: %d, sign: %d\n", ir, base, fc,
          sign);
    return load_vt_lval_vt_local_float(r, sv, ft, fc, sign, base);
  } else if (btype == VT_SHORT) {
    TRACE("load short to r: %d, base: %d, fc: %d, sign: %d\n", ir, base, fc,
          sign);
    if (!(ft & VT_UNSIGNED)) {
      success = load_short_from_base(ir, base, fc, sign);
    } else {
      success = load_ushort_from_base(ir, base, fc, sign);
    }
  } else if (btype == VT_BYTE || btype == VT_BOOL) {
    if (!(ft & VT_UNSIGNED)) {
      success = load_byte_from_base(ir, base, fc, sign);
    } else {
      success = load_ubyte_from_base(ir, base, fc, sign);
    }
  } else {
    success = load_word_from_base(ir, base, fc, sign);
  }
  if (!success) {

    // now load from dereferenced value
    int rr = th_offset_to_reg(fc, sign);
    if (btype == VT_SHORT) {
      if (ft & VT_UNSIGNED)
        ot_check(th_ldrh_reg(ir, base, rr, THUMB_SHIFT_DEFAULT,
                             ENFORCE_ENCODING_NONE));
      else
        ot_check(th_ldrsh_reg(ir, base, rr, THUMB_SHIFT_DEFAULT,
                              ENFORCE_ENCODING_NONE));
    } else if (btype == VT_BYTE || btype == VT_BOOL) {
      if (ft & VT_UNSIGNED)
        ot_check(th_ldrb_reg(ir, base, rr, THUMB_SHIFT_DEFAULT,
                             ENFORCE_ENCODING_NONE));
      else
        ot_check(th_ldrsb_reg(ir, base, rr, THUMB_SHIFT_DEFAULT,
                              ENFORCE_ENCODING_NONE));
    } else
      ot_check(
          th_ldr_reg(ir, base, rr, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
  }
}

void load_vt_const(int r, SValue *sv) {
  TRACE("'load_vt_const' r: %i, const: %i, sym: %i", r, (int)sv->c.i,
        (sv->r & VT_SYM) == VT_SYM);
  r = intr(r);
  if (sv->r & VT_SYM) {
    load_full_const(r, sv->c.i, sv->sym);
  } else {
    if (!ot(th_generic_mov_imm(r, sv->c.i)))
      load_full_const(r, sv->c.i, 0);
  }
}

void load_vt_local(int r, SValue *sv) {
  TRACE("'load_vt_local' r: %d, off: %x", r, (uint32_t)sv->c.i);
  if (sv->r & VT_SYM || (-sv->c.i) >= 0xfff) {
    load_full_const(r, sv->c.i, sv->r & VT_SYM ? sv->sym : 0);
    ot_check(th_add_reg(r, R_FP, r, FLAGS_BEHAVIOUR_NOT_IMPORTANT,
                        THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
  } else {
    ot_check(th_sub_imm(r, R_FP, -sv->c.i, FLAGS_BEHAVIOUR_NOT_IMPORTANT,
                        ENFORCE_ENCODING_NONE));
  }
}

void load_vt_cmp(int r, SValue *sv) {
  const uint32_t firstcond = mapcc(sv->c.i);
  uint32_t rr = intr(r);
  TRACE("'load_vt_cmp' to reg: %d, op: 0x%x\n", r, (uint32_t)sv->c.i);
  if (rr == R_SP || rr == R_PC) {
    tcc_error("compiler_error: load_vt_cmp can't be used for pc or sp\n");
  }

  // it block
  o(0xbf00 | (firstcond << 4) | 0x4 | ((~firstcond & 1) << 3));
  ot_check(th_generic_mov_imm(rr, 1));
  ot_check(th_generic_mov_imm(rr, 0));
}

void load_vt_jmp_jmpi(int r, SValue *sv) {
#ifdef TCC_TARGET_ARM_ARCHV6M
  if (intr(r) > 7) {
    tcc_error("compiler_error: implement load_vt_jmp_jmpi for armv6m\n");
  }
#endif
  ot_check(th_generic_mov_imm(intr(r), sv->r & 1));
  ot_check(th_b_t4(2));
  gsym(sv->c.i);
  ot_check(th_generic_mov_imm(intr(r), (sv->r ^ 1) & 1));
}

// load value from stack to register
void load(int r, SValue *sv) {
  int v, ft, fc, fr, sign;

  // TRACE("'load'");
  fr = sv->r;
  ft = sv->type.t;
  fc = sv->c.i;
  if (fc >= 0)
    sign = 0;
  else {
    sign = 1;
    fc = -fc;
  }

  if (sv->r & VT_PARAM) {
    fc += offset_to_args;
  }

  v = fr & VT_VALMASK;

  // load lvalue from
  if (fr & VT_LVAL) {
    uint32_t base = R_FP;
    SValue v1;
    // load value from stack
    // prepare for new load after pointer dereference
    if (v == VT_LLOCAL) {
      v1.type.t = VT_PTR;
      v1.r = VT_LOCAL | VT_LVAL;
      v1.c.i = sv->c.i;

      TRACE("l1");
      load(base = 14, &v1);
      fc = sign = 0;
      v = VT_LOCAL;
    } else if (v == VT_CONST) {
      v1.type.t = VT_PTR;
      v1.r = fr & ~VT_LVAL;
      v1.c.i = sv->c.i;
      v1.sym = sv->sym;
      TRACE("l2");
      load(base = 14, &v1);
      fc = sign = 0;
      v = VT_LOCAL;
    } else if (v < VT_CONST) {
      base = intr(v);
      fc = sign = 0;
      v = VT_LOCAL;
    }

    if (v == VT_LOCAL) {
      return load_vt_lval_vt_local(r, sv, ft, fc, sign, base);
    }
  } else if (v == VT_CONST)
    return load_vt_const(r, sv);
  else if (v == VT_LOCAL)
    return load_vt_local(r, sv);
  else if (v == VT_CMP)
    return load_vt_cmp(r, sv);
  else if (v == VT_JMP || v == VT_JMPI)
    return load_vt_jmp_jmpi(r, sv);
  else if (v < VT_CONST) {
    if (is_float(ft)) {
      if ((ft & VT_BTYPE) == VT_FLOAT)
        ot_check(th_vmov_register(vfpr(r), vfpr(v), 0));
      else
        ot_check(th_vmov_register(vfpr(r), vfpr(v), 1));
      return;
    } else {
      TRACE("mov r %i v %i", r, v);
      ot_check(th_mov_reg(r, v, FLAGS_BEHAVIOUR_NOT_IMPORTANT,
                          THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE, false));
      return;
    }
  }
  tcc_error("compiler_error: unknown load not implemented\n");
}

static int is_zero_on_stack(int pos) {
  if ((vtop[pos].r & (VT_VALMASK | VT_LVAL | VT_SYM)) != VT_CONST)
    return 0;
  if (vtop[pos].type.t == VT_FLOAT)
    return vtop[pos].c.f == 0.f;
  if (vtop[pos].type.t == VT_DOUBLE)
    return vtop[pos].c.d == 0.0;
  return vtop[pos].c.ld = 0.l;
}

static void gen_opf_regular(uint32_t opc, int fneg) {
  uint32_t inst = 0;
  int r = gv(RC_FLOAT);
  opc |= 0xee000a00 | vfpr(r);
  r = regmask(r);
  if (!fneg) {
    int r2;
    vswap();
    r2 = gv(RC_FLOAT);
    opc |= vfpr(r2) << 16;
    r |= regmask(r2);
  }
  vtop->r = get_reg_ex(RC_FLOAT, r);
  if (!fneg) {
    --vtop;
    print_vstack("gen_opf_regular");
  }
  inst = opc | (vfpr(vtop->r) << 12);
  o(inst >> 16);
  o(inst);
}

static void gen_opf_cmp(uint32_t opc, uint32_t op) {
  uint32_t inst = 0;
  opc |= 0xeeb40a40;
  if (op != TOK_EQ && op != TOK_NE)
    opc |= 0x80;

  if (is_zero_on_stack(0)) {
    --vtop;
    print_vstack("gen_opf_cmp(1)");
    inst = opc | 0x10000 | (vfpr(gv(RC_FLOAT)) << 12);
  } else {
    opc |= vfpr(gv(RC_FLOAT));
    vswap();
    inst = opc | (vfpr(gv(RC_FLOAT)) << 12);
    --vtop;
    print_vstack("gen_opf_cmp(2)");
  }

  o(inst >> 16);
  o(inst);
  ot_check(th_vmrs(15));
}

ST_FUNC void gen_cvt_itof(int t) {
  const int bt = vtop->type.t & VT_BTYPE;
  TRACE("gen_cvt_itof, t: 0x%x", t);

  if (bt == VT_INT || bt == VT_SHORT || bt == VT_BYTE) {
    uint32_t r = intr(gv(RC_INT));
    uint32_t r2 = vfpr(vtop->r = get_reg(RC_FLOAT));
    uint32_t op = (vtop->type.t & VT_UNSIGNED) ? 0 : 1;
    ot_check(th_vmov_gp_sp(r, r2, 0));
    ot_check(th_vcvt_fp_int(r2, r2, 0, (t & VT_BTYPE) != VT_FLOAT, op));
    return;
  } else if (bt == VT_LLONG) {
    int func;
    CType *func_type = 0;
    if ((t & VT_BTYPE) == VT_FLOAT) {
      func_type = &func_float_type;
      if (vtop->type.t & VT_UNSIGNED)
        func = TOK___floatundisf;
      else
        func = TOK___floatdisf;
    } else if ((t & VT_BTYPE) == VT_DOUBLE || (t & VT_BTYPE) == VT_LDOUBLE) {
      func_type = &func_double_type;
      if (vtop->type.t & VT_UNSIGNED)
        func = TOK___floatundidf;
      else
        func = TOK___floatdidf;
    }

    if (func_type) {
      vpush_helper_func(func);
      vswap();
      gfunc_call(1);
      vpushi(0);
      vtop->r = TREG_F0;
      return;
    }
  }
}

/* convert fp to int 't' type */
void gen_cvt_ftoi(int t) {
  uint32_t r2 = vtop->type.t & VT_BTYPE;
  int u = t & VT_UNSIGNED;
  TRACE("gen_cvt_ftoi t: 0x%x", t);

  t &= VT_BTYPE;

  if (t == VT_INT) {
    uint32_t opc = u ? 0x4 : 0x5;
    uint32_t r = vfpr(gv(RC_FLOAT));
    uint32_t rr = intr(vtop->r = get_reg(RC_INT));
    ot_check(th_vcvt_fp_int(rr, r, opc, (r2 & VT_BTYPE) != VT_FLOAT, 1));
    ot_check(th_vmov_gp_sp(rr, rr, 1));
    return;
  } else if (t == VT_LLONG) {
    int func = 0;
    if (r2 == VT_FLOAT)
      func = TOK___fixsfdi;
    else if (r2 == VT_LDOUBLE || r2 == VT_DOUBLE)
      func = TOK___fixdfdi;

    if (func) {
      vpush_helper_func(func);
      vswap();
      gfunc_call(1);
      vpushi(0);
      if (t == VT_LLONG)
        vtop->r2 = REG_IRE2;
      vtop->r = REG_IRET;
      return;
    }
  }
  tcc_error("compiler_error: unimplemented float to integer");
}

void gen_cvt_ftof(int t) {
  TRACE("gen_cvt_ftof t: 0x%x", t);
  if (((vtop->type.t & VT_BTYPE) == VT_FLOAT) != ((t & VT_BTYPE) == VT_FLOAT)) {
    uint32_t r = vfpr(gv(RC_FLOAT));
    if ((t & VT_BTYPE) != VT_FLOAT)
      ot_check(th_vcvt_float_to_double(r, r));
    else
      ot_check(th_vcvt_double_to_float(r, r));
  }
}

void gen_opf(int op) {
  const uint32_t is_double =
      ((vtop->type.t & VT_BTYPE) != VT_FLOAT) ? 0x100 : 0;

  TRACE("gen_opf op: 0x%x(%c)", op, op);
  switch (op) {
  case '+': {
    if (is_zero_on_stack(-1))
      vswap();
    if (is_zero_on_stack(0)) {
      --vtop;
      print_vstack("gen_opf(+)");
      return;
    }
    return gen_opf_regular(is_double | 0x00300000, 0);
  }
  case '-': {
    if (is_zero_on_stack(0)) {
      --vtop;
      print_vstack("gen_opf(- 1)");
      return;
    }
    if (is_zero_on_stack(-1)) {
      vswap();
      --vtop;
      print_vstack("gen_opf(- 2)");
      return gen_opf_regular(is_double | 0x00b10040, 1);
    } else
      return gen_opf_regular(is_double | 0x00300040, 0);
  }
  case '*':
    return gen_opf_regular(is_double | 0x002000000, 0);
  case '/':
    return gen_opf_regular(is_double | 0x008000000, 0);
  default: {
    if (op < TOK_ULT || op > TOK_GT)
      tcc_error("compiler_error: unknown floating-point operation: 0x%x", op);
    if (is_zero_on_stack(-1)) {
      vswap();
      switch (op) {
      case TOK_LT:
        op = TOK_GT;
        break;
      case TOK_GE:
        op = TOK_ULE;
        break;
      case TOK_LE:
        op = TOK_GE;
        break;
      case TOK_GT:
        op = TOK_ULT;
        break;
      }
    }
    gen_opf_cmp(is_double, op);

    switch (op) {
    case TOK_LE:
      op = TOK_ULE;
      break;
    case TOK_LT:
      op = TOK_ULT;
      break;
    case TOK_UGE:
      op = TOK_GE;
      break;
    case TOK_UGT:
      op = TOK_GT;
      break;
    }
    vset_VT_CMP(op);
  }
  }
}

// // operation on two registers
// void gen_opi_regs(int opc, int c) {
//   int fr = 0;
//   int r = 0;

//   fr = intr(gv(RC_INT));
//   r = intr(vtop[-1].r = get_reg_ex(RC_INT, two2mask(vtop->r, vtop[-1].r)));

//   switch (opc) {
//   case 0:
//     ot_check(th_and_reg(r, c, fr, FLAGS_BEHAVIOUR_NOT_IMPORTANT,
//                         THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
//     return;
//   case 2:
//     ot_check(th_xor_reg(r, c, fr));
//     return;
//   case 4:
//   case 5:
//     ot_check(th_sub_reg(r, c, fr, FLAGS_BEHAVIOUR_NOT_IMPORTANT,
//                         THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
//     return;
//   case 6:
//   case 7:
//     ot_check(th_rsb_reg(r, c, fr, FLAGS_BEHAVIOUR_NOT_IMPORTANT,
//                         THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
//     return;
//   case 8:
//   case 9:
//     ot_check(th_add_reg(r, c, fr, FLAGS_BEHAVIOUR_NOT_IMPORTANT,
//                         THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
//     return;
//   case 10:
//     ot_check(th_adc_reg(r, c, fr, FLAGS_BEHAVIOUR_NOT_IMPORTANT,
//                         THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
//     return;
//   case 12:
//     ot_check(th_sbc_reg(r, c, fr, FLAGS_BEHAVIOUR_NOT_IMPORTANT,
//                         THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
//     return;
//   case 14:
//     ot_check(th_sbc_reg(r, fr, c, FLAGS_BEHAVIOUR_NOT_IMPORTANT,
//                         THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
//     return;
//   case 21:
//     ot_check(th_cmp_reg(c, fr, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
//     return;
//   case 24:
//     ot_check(th_orr_reg(r, c, fr, FLAGS_BEHAVIOUR_NOT_IMPORTANT,
//                         THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
//     return;
//   default:
//     tcc_error("compiler_error: 'gen_opi_regs' unhandled case opc: %d, c: %d,
//     "
//               "r: %d, fr: %d\n",
//               opc, c, r, fr);
//   }
// }

// void gen_opi_regular(int opc, int c) {
//   TRACE("gen_opi_regular opc: 0x%x, c: 0x%x", opc, c);
//   if ((vtop->r & (VT_VALMASK | VT_LVAL | VT_SYM)) == VT_CONST) {
//     int ok = 0;
//     int r = intr(vtop[-1].r = get_reg_ex(RC_INT, regmask(vtop[-1].r)));
//     if (opc != 0x15 && r != c) {
//       tcc_error(
//           "compiler_error: '2en_opi_regular' incorrect order of r and c\n");
//     }
//     switch (opc) {
//     case 0:
//       ok = ot(th_and_imm(r, r, vtop->c.i, FLAGS_BEHAVIOUR_NOT_IMPORTANT));

//       break;
//     case 2:
//       ok = ot(th_xor_imm(r, r, vtop->c.i));
//       break;
//     case 4:
//     case 5:
//       ok = ot(th_sub_imm(r, r, vtop->c.i, FLAGS_BEHAVIOUR_NOT_IMPORTANT,
//                          ENFORCE_ENCODING_NONE));
//       break;
//     case 6:
//     case 7:
//       ok = ot(th_rsb_imm(r, r, vtop->c.i, FLAGS_BEHAVIOUR_SET));
//       break;
//     case 8:
//     case 9:
//       ok = ot(th_add_imm(r, r, vtop->c.i, FLAGS_BEHAVIOUR_NOT_IMPORTANT,
//                          ENFORCE_ENCODING_NONE));
//       break;
//     case 10:
//       ok = ot(th_adc_imm(r, r, vtop->c.i, FLAGS_BEHAVIOUR_NOT_IMPORTANT));
//       break;
//     case 12:
//       ok = ot(th_sbc_imm(r, r, vtop->c.i, FLAGS_BEHAVIOUR_NOT_IMPORTANT));
//       break;
//     case 14:
//       ok = 0;
//       break;
//     case 21:
//       ok = ot(th_cmp_imm(c, vtop->c.i, ENFORCE_ENCODING_NONE));
//       break;
//     case 24:
//       ok = ot(th_orr_imm(r, r, vtop->c.i, FLAGS_BEHAVIOUR_NOT_IMPORTANT));
//       break;
//     default:
//       tcc_error("compiler_error: 'gen_opi_regular' unhandled case opc: %d, c:
//       "
//                 "%d, r: %d\n",
//                 opc, c, r);
//     }

//     if (ok)
//       return;
//   }
//   return gen_opi_regs(opc, c);
// }

// void gen_opi_notshift(int op, int opc) {
//   int c = 0;
//   if ((vtop[-1].r & (VT_VALMASK | VT_LVAL | VT_SYM)) == VT_CONST) {
//     if (opc == 4 || opc == 5 || opc == 0xc) {
//       vswap();
//       opc |= 2;
//     }
//   }

//   if ((vtop->r & VT_VALMASK) == VT_CMP ||
//       (vtop->r & (VT_VALMASK & ~1)) == VT_JMP) {
//     gv(RC_INT);
//   }

//   vswap();
//   c = intr(gv(RC_INT));
//   vswap();

//   gen_opi_regular(opc, c);
//   --vtop;
//   print_vstack("gen_opi_notshift");
//   if (op >= TOK_ULT && op <= TOK_GT) {
//     TRACE("gen_opi_notshift vset_VT_CMP");
//     vset_VT_CMP(op);
//   }
// }

// static void gen_opi_shift(int opc) {
//   int r = 0;

//   if ((vtop->r & VT_VALMASK) == VT_CMP ||
//       (vtop->r & (VT_VALMASK & ~1)) == VT_JMP)
//     gv(RC_INT);

//   vswap();
//   r = intr(gv(RC_INT));
//   vswap();

//   if ((vtop->r & (VT_VALMASK | VT_LVAL | VT_SYM)) == VT_CONST) {
//     int fr = intr(vtop[-1].r = get_reg_ex(RC_INT, regmask(vtop[-1].r)));
//     int c = vtop->c.i & 0x1f;

//     if (opc == 0)
//       ot_check(th_lsl_imm(r, fr, c, FLAGS_BEHAVIOUR_NOT_IMPORTANT,
//                           ENFORCE_ENCODING_NONE));
//     else if (opc == 1)
//       ot_check(th_lsr_imm(r, fr, c, FLAGS_BEHAVIOUR_NOT_IMPORTANT,
//                           ENFORCE_ENCODING_NONE));
//     else if (opc == 2)
//       ot_check(th_asr_imm(r, fr, c, FLAGS_BEHAVIOUR_NOT_IMPORTANT,
//                           ENFORCE_ENCODING_NONE));
//   } else {
//     int fr = intr(gv(RC_INT));
//     int c =
//         intr(vtop[-1].r = get_reg_ex(RC_INT, two2mask(vtop->r, vtop[-1].r)));

//     if (opc == 0)
//       ot_check(th_lsl_reg(c, r, fr, FLAGS_BEHAVIOUR_NOT_IMPORTANT,
//                           ENFORCE_ENCODING_NONE));
//     else if (opc == 1)
//       ot_check(th_lsr_reg(c, r, fr, FLAGS_BEHAVIOUR_NOT_IMPORTANT,
//                           ENFORCE_ENCODING_NONE));
//     else if (opc == 2)
//       ot_check(th_asr_reg(c, r, fr, FLAGS_BEHAVIOUR_NOT_IMPORTANT,
//                           ENFORCE_ENCODING_NONE));
//     else
//       tcc_error("compiler_error: 'gen_opi_shift' not implemented case: %d\n",
//                 opc);
//   }
//   vtop--;
//   print_vstack("gen_opi_shift");
// }

// /* generate an integer binary operation */
// void gen_opi(int op) {
//   uint32_t r, fr;
//   TRACE("'gen_opi', op: 0x%x, %c", op, op);
//   switch (op) {
//   case '+':
//     return gen_opi_notshift(op, 0x08);
//   case TOK_ADDC1:
//     return gen_opi_notshift(op, 0x09);
//   case '-':
//     return gen_opi_notshift(op, 0x04);
//   case TOK_SUBC1:
//     return gen_opi_notshift(op, 0x05);
//   case TOK_ADDC2:
//     return gen_opi_notshift(op, 0x0a);
//   case TOK_SUBC2:
//     return gen_opi_notshift(op, 0x0c);
//   case '&':
//     return gen_opi_notshift(op, 0x00);
//   case '^':
//     return gen_opi_notshift(op, 0x02);
//   case '|':
//     return gen_opi_notshift(op, 0x18);
//   case '*': {
//     gv2(RC_INT, RC_INT);
//     r = vtop[-1].r;
//     fr = vtop[0].r;
//     vtop--;
//     print_vstack("gen_opi(*)");
//     ot_check(th_mul(intr(r), intr(fr), intr(r),
//     FLAGS_BEHAVIOUR_NOT_IMPORTANT,
//                     ENFORCE_ENCODING_NONE));
//     return;
//   }
//   case TOK_SHL:
//     return gen_opi_shift(0);
//   case TOK_SHR:
//     return gen_opi_shift(1);
//   case TOK_SAR:
//     return gen_opi_shift(2);
//   case '/':
//   case TOK_PDIV: {
//     gv2(RC_INT, RC_INT);
//     r = vtop[-1].r;
//     fr = vtop[0].r;
//     ot_check(th_sdiv(intr(r), intr(r), intr(fr)));
//     vtop--;
//     print_vstack("gen_opi(/)");
//     return;
//   }
//   case TOK_UDIV: {
//     gv2(RC_INT, RC_INT);
//     r = vtop[-1].r;
//     fr = vtop[0].r;
//     ot_check(th_udiv(intr(r), intr(r), intr(fr)));
//     vtop--;
//     print_vstack("gen_opi(UDIV)");
//     return;
//   }
//   case '%': {
//     uint32_t rr = 0;
//     gv2(RC_INT, RC_INT);
//     r = vtop[-1].r;
//     fr = vtop[0].r;
//     vtop--;
//     print_vstack("gen_opi(%%)");
//     r = intr(r);
//     fr = intr(fr);
//     for (int i = 0; i < 5; ++i) {
//       if (rr == r || rr == fr)
//         ++rr;
//       else
//         break;
//     }

//     ot_check(th_push(1 << rr));
//     ot_check(th_sdiv(rr, r, fr));
//     ot_check(th_mul(fr, fr, rr, FLAGS_BEHAVIOUR_NOT_IMPORTANT,
//                     ENFORCE_ENCODING_NONE));
//     ot_check(th_sub_reg(r, r, fr, FLAGS_BEHAVIOUR_NOT_IMPORTANT,
//                         THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
//     ot_check(th_pop(1 << rr));
//     return;
//   }
//   case TOK_UMOD: {
//     uint32_t rr = 0;
//     gv2(RC_INT, RC_INT);
//     r = vtop[-1].r;
//     fr = vtop[0].r;
//     vtop--;
//     print_vstack("gen_opi(UMOD)");
//     r = intr(r);
//     fr = intr(fr);
//     for (int i = 0; i < 5; ++i) {
//       if (rr == r || rr == fr)
//         ++rr;
//       else
//         break;
//     }

//     ot_check(th_push(1 << rr));
//     ot_check(th_udiv(rr, r, fr));
//     ot_check(th_mul(fr, fr, rr, FLAGS_BEHAVIOUR_NOT_IMPORTANT,
//                     ENFORCE_ENCODING_NONE));
//     ot_check(th_sub_reg(r, r, fr, FLAGS_BEHAVIOUR_NOT_IMPORTANT,
//                         THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
//     ot_check(th_pop(1 << rr));
//     return;
//   }
//   case TOK_UMULL: {
//     gv2(RC_INT, RC_INT);
//     r = intr(vtop[-1].r2 = get_reg(RC_INT));
//     fr = vtop[-1].r;
//     vtop[-1].r = get_reg_ex(RC_INT, regmask(fr));
//     vtop--;
//     print_vstack("gen_opi(UMULL)");
//     ot_check(th_umull(intr(vtop->r), r, intr(vtop[1].r), intr(fr)));
//     return;
//   }
//   default: {
//     return gen_opi_notshift(op, 0x15);
//   }
//   }
// }

ST_FUNC void gen_increment_tcov(SValue *sv) { TRACE("'gen_increment_tcov'"); }

void tcc_gen_machine_data_processing_op(TACQuadruple *op) {
  switch (op->op) {
  case TCCIR_OP_ADD:
    printf("gen_machine_data_processing_op: TCCIR_OP_ADD, type: 0x%x\n",
           op->src2.r);
    if ((op->src2.r & (VT_VALMASK | VT_LVAL | VT_SYM)) == VT_CONST) {
      printf("gen_machine_data_processing_op: TCCIR_OP_ADD imm: %d, reg0: %d, "
             "reg1: %d\n",
             (int)op->src2.c.i, op->dest.pr0, op->src1.pr0);
      ot_check(th_add_imm(op->dest.pr0, op->src1.pr0, op->src2.c.i,
                          FLAGS_BEHAVIOUR_NOT_IMPORTANT,
                          ENFORCE_ENCODING_NONE));
    } else {
      printf("gen_machine_data_processing_op: TCCIR_OP_ADD reg\n");
      ot_check(th_add_reg(op->dest.pr0, op->src1.pr0, op->src2.pr0,
                          FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT,
                          ENFORCE_ENCODING_NONE));
    }

    break;
  case TCCIR_OP_MUL:
    ot_check(th_mul(op->dest.pr0, op->src1.pr0, op->src2.pr0,
                    FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE));
    break;
  case TCCIR_OP_ADC_USE:
    // return ot_check(th_adc_reg(intr(op->res), intr(op->arg1), intr(op->arg2),
    //  FLAGS_BEHAVIOUR_NOT_IMPORTANT,
    //  THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
  case TCCIR_OP_ADC_GEN:
    // return ot_check(th_add_reg(op->dest->r, , intr(op->arg1),
    //                            FLAGS_BEHAVIOUR_SET, THUMB_SHIFT_DEFAULT,
    //                            ENFORCE_ENCODING_NONE));
  default: {
    printf("compiler_error: unhandled data processing op: %s\n",
           tcc_ir_get_op_name(op->op));
  }
  }
}

ST_FUNC void tcc_gen_machine_return_value_op(TACQuadruple *q) {
  if ((q->src1.r & VT_VALMASK) == VT_CONST) {
    load(R0, &q->src1);
    return;
  }

  if (q->src1.pr0 == R0)
    return;

  if (q->src1.pr0 >= 0) {
    ot_check(th_mov_reg(R0, q->src1.pr0, FLAGS_BEHAVIOUR_NOT_IMPORTANT,
                        THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE, false));
  } // else {
    //  load(R0, &q->src1);
  //}
}

void tcc_gen_machine_load_op(TACQuadruple *op) {
  TRACE("'tcc_gen_machine_load_op'");
  load(op->dest.pr0, &op->src1);
}

ST_FUNC void tcc_gen_machine_prolog(int leaffunc, uint64_t used_registers) {
  printf("'tcc_gen_machine_prolog' leaffunc: %d, used_registers: 0x%llx\n",
         leaffunc, used_registers);
  memset(function_arguments, 0, sizeof(function_arguments));
  uint16_t registers_to_push = 0;
  int registers_count = 0;
  if (!leaffunc) {
    registers_to_push |= (1 << R_LR);
    registers_count++;
  }

  if (tcc_state->need_frame_pointer) {
    registers_to_push |= (1 << R_FP);
    registers_count++;
  }

  for (int i = R4; i <= R11; ++i) {
    if (tcc_state->text_and_data_separation && i == R9)
      continue;
    if (!tcc_state->omit_frame_pointer && i == R_FP)
      continue;
    if (used_registers & (1ULL << i)) {
      registers_to_push |= (1 << i);
      registers_count++;
    }
  }
  if (registers_count % 2 != 0) {
    registers_to_push |= (1 << R12);
    registers_count++;
  }

  offset_to_args = registers_count * 4;
  if (registers_count > 0) {
    ot_check(th_push(registers_to_push));
  }
  pushed_registers = registers_to_push;

  if (tcc_state->need_frame_pointer) {
    if (!ot(th_add_imm(R_FP, R_SP, 0, FLAGS_BEHAVIOUR_NOT_IMPORTANT,
                       ENFORCE_ENCODING_NONE))) {
      // todo mov fp, sp
      // load r12 immediate
      // add fp, sp, r12
      fprintf(stderr, "compiler_error: prolog frame pointer setup failed\n");
      exit(1);
    }
  }
}

ST_FUNC void tcc_gen_machine_epilog(int leaffunc) {
  TRACE("'tcc_gen_machine_epilog'");
  int lr_saved = pushed_registers & (1 << R_LR);

  if (lr_saved) {
    pushed_registers |= 1 << R_PC;
    pushed_registers &= ~(1 << R_LR);
    ot_check(th_pop(pushed_registers));
    return;
  }
  if (pushed_registers > 0) {
    ot_check(th_pop(pushed_registers));
  }
  ot_check(th_bx_reg(R_LR));
}

ST_FUNC void tcc_gen_machine_assign_op(TACQuadruple *op) {
  if ((op->src1.r & VT_VALMASK) == VT_CONST) {
    load(op->dest.pr0, &op->src1);
    return;
  }
  if (op->dest.pr0 == op->src1.pr0)
    return;

  ot_check(th_mov_reg(op->dest.pr0, op->src1.pr0, FLAGS_BEHAVIOUR_NOT_IMPORTANT,
                      THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE, false));
}

// r0 - function
// r1 - function
// r2 - function
// r3 - function

// r4 - lrsa
// r5 - lrsa
// r6 - lrsa
// r7 - lrsa
// r8 - lrsa
// r9 - PIC
// r10 - lrsa

ST_FUNC int tcc_gen_machine_number_of_registers(void) { return 11; }

ST_FUNC void tcc_gen_machine_load_register(SValue *sv) { load(sv->pr0, sv); }

ST_FUNC void tcc_gen_machine_func_param_op(TACQuadruple *q) {
  // cache argument for register passing
  function_arguments[function_argument_count++] = *q;
}

static void gcall_or_jump(int is_jmp, SValue *dest) {
  if ((dest->r & (VT_VALMASK | VT_LVAL)) == VT_CONST) {
    uint32_t x = th_encbranch(ind, ind + dest->c.i);

    TRACE("gcall_or_jmp: %d, ind: 0x%x, 0x%x", is_jmp, ind, x);
    if (x) {
      if (dest->r & VT_SYM)
        greloc(cur_text_section, dest->sym, ind, R_ARM_THM_JUMP24);
      ot_check(th_bl_t1(x));
    }
  } else {
    fprintf(stderr, "compiler_error: implement gcall_or_jmp for non-const\n");
    exit(1);
    // int r = gv(RC_INT);
    // TRACE("gcall_or_jmp indirect call");
    // ot_check(th_orr_imm(r, r, 1, FLAGS_BEHAVIOUR_NOT_IMPORTANT));
    // if (!is_jmp)
    // ot_check(th_blx_reg(intr(r)));
    // else
    // ot_check(th_bx_reg(intr(r)));
  }
}

ST_FUNC void tcc_gen_machine_func_call_op(TACQuadruple *q) {
  for (int i = 0; i < function_argument_count; ++i) {
    TACQuadruple *q = &function_arguments[i];
    if (i < 4) {
      load(q->src2.c.i - 1, &q->src1);
    }
  }
  function_argument_count = 0;
  gcall_or_jump(0, &q->src1);
  if (q->dest.pr0 != R0) {
    ot_check(th_mov_reg(q->dest.pr0, R0, FLAGS_BEHAVIOUR_NOT_IMPORTANT,
                        THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE, false));
  }
}

#endif // TARGET_DEFS_ONLY
