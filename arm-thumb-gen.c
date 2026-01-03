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

typedef struct ThumbLiteralPoolEntry
{
  Sym *sym;
  int relocation;
  int patch_position;
  int short_instruction;
  int data_size;
  int64_t imm;
  int shared_index; /* Index of earlier entry with same value, or -1 if unique
                     */
} ThumbLiteralPoolEntry;

typedef struct ThumbGeneratorState
{
  uint8_t generating_function : 1;
  int code_size;
  ThumbLiteralPoolEntry *literal_pool;
  int literal_pool_size;
  int literal_pool_count;
  /* Cache for global symbol base address to avoid redundant loads */
  Sym *cached_global_sym; /* Last loaded global symbol */
  int cached_global_reg;  /* Register holding its base address */
  int *function_argument_list;
  int function_argument_list_size;
  int function_argument_count;
} ThumbGeneratorState;

ThumbGeneratorState thumb_gen_state;

enum Armv8mRegisters
{
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
enum
{
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

#else // TARGET_DEFS_ONLY

#define USING_GLOBALS
#include "tcc.h"

#include "arch/fpu/arm/fpv5-sp-d16.h"
#include "arm-thumb-opcodes.h"

int load_word_from_base(int ir, int base, int fc, int sign);

/* Helper to validate a Sym pointer - returns NULL if invalid/unusable for relocation */
static inline Sym *validate_sym_for_reloc(Sym *sym)
{
  if (!sym)
    return NULL;
  /* Check for use-after-free */
  if (sym->v == 0xDEADBEEF)
  {
    /* BREAKPOINT: Set breakpoint here in GDB with "b arm-thumb-gen.c:211" */
    /* Then run "bt" to see the call stack */
    fprintf(stderr,
            "DEBUG validate_sym_for_reloc: USE-AFTER-FREE! sym=%p was freed. Set breakpoint at arm-thumb-gen.c:211\n",
            (void *)sym);
    return NULL;
  }
  /* Type descriptors (SYM_FIELD) should not be used for relocations */
  if (sym->v & SYM_FIELD)
    return NULL;
  /* Symbols with c < 0 are not properly registered */
  if (sym->c < 0)
    return NULL;
  return sym;
}

/* Forward declarations */
static int is_64bit_type(int t);
void load_to_dest(SValue *dest, SValue *sv);

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

thumb_flags_behaviour g_setflags = FLAGS_BEHAVIOUR_SET;

uint32_t caller_saved_registers;
uint32_t pushed_registers;
int allocated_stack_size;

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
static void load_to_register(int reg, int reg_from, SValue *src);
int th_has_immediate_value(int r);
int load_word_from_base(int ir, int base, int fc, int sign);
int th_offset_to_reg(int offset, int sign);

static int th_is_caller_saved_register(int reg)
{
  if (tcc_state->text_and_data_separation && reg == R9)
  {
    return 1;
  }
  return (reg >= R0 && reg <= R3) || reg == R_LR || reg == R_IP;
}

int ot_check(thumb_opcode op)
{
  if (!is_valid_opcode(op))
  {
    tcc_error("compiler_error: received invalid opcode: 0x%x\n", op.opcode);
  }
  return ot(op);
}

ST_FUNC void gen_fill_nops(int bytes)
{
  TRACE("'gen_fill_nops'");

  if (bytes & 1)
  {
    tcc_error("compiler_error: 'gen_fill_nops' bytes are not aligned to: 2-bytes\n");
    return;
  }
  while (bytes > 0)
  {
    ot_check(th_nop(ENFORCE_ENCODING_16BIT));
    bytes -= 2;
  }
}

static int two2mask(int a, int b)
{
  if (!CHECK_R(a) || !CHECK_R(b))
    tcc_error("compiler error! registers %i,%i is not valid", a, b);
  return (reg_classes[a] | reg_classes[b]) & ~(RC_INT | RC_FLOAT);
}

static uint32_t mapcc(int cc)
{
  switch (cc)
  {
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
const char *default_elfinterp(struct TCCState *s)
{
  // just for pass compilation, in the future add real loaders from yasos
  if (s->float_abi == ARM_HARD_FLOAT)
  {
    return "/lib/ld-linux-armhf.so";
  }
  else
  {
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

// struct avail_regs
// {
//   signed char avail[3]; /* 3 holes max with only float and double alignments */
//   int first_hole;       /* first available hole */
//   int last_hole;        /* last available hole (none if equal to first_hole) */
//   int first_free_reg;   /* next free register in the sequence, hole excluded */
// };
// #define AVAIL_REGS_INITIALIZER (struct avail_regs){{0, 0, 0}, 0, 0, 0}
// /* Find suitable registers for a VFP Co-Processor Register Candidate (VFP CPRC
//    param) according to the rules described in the procedure call standard for
//    the ARM architecture (AAPCS). If found, the registers are assigned to this
//    VFP CPRC parameter. Registers are allocated in sequence unless a hole exists
//    and the parameter is a single float.

//    avregs: opaque structure to keep track of available VFP co-processor regs
//    align: alignment constraints for the param, as returned by type_size()
//    size: size of the parameter, as returned by type_size() */
// int assign_vfpreg(struct avail_regs *avregs, int align, int size)
// {
//   int first_reg = 0;

//   if (avregs->first_free_reg == -1)
//     return -1;
//   if (align >> 3)
//   { /* double alignment */
//     first_reg = avregs->first_free_reg;
//     /* alignment constraint not respected so use next reg and record hole */
//     if (first_reg & 1)
//       avregs->avail[avregs->last_hole++] = first_reg++;
//   }
//   else
//   { /* no special alignment (float or array of float) */
//     /* if single float and a hole is available, assign the param to it */
//     if (size == 4 && avregs->first_hole != avregs->last_hole)
//       return avregs->avail[avregs->first_hole++];
//     else
//       first_reg = avregs->first_free_reg;
//   }
//   if (first_reg + size / 4 <= 16)
//   {
//     avregs->first_free_reg = first_reg + size / 4;
//     return first_reg;
//   }
//   avregs->first_free_reg = -1;
//   return -1;
// }

// /* Parameters are classified according to how they are copied to their final
//    destination for the function call. Because the copying is performed class
//    after class according to the order in the union below, it is important that
//    some constraints about the order of the members of this union are respected:
//    - CORE_STRUCT_CLASS must come after STACK_CLASS;
//    - CORE_CLASS must come after STACK_CLASS, CORE_STRUCT_CLASS and
//      VFP_STRUCT_CLASS;
//    - VFP_STRUCT_CLASS must come after VFP_CLASS.
//    See the comment for the main loop in copy_params() for the reason. */
// enum reg_class
// {
//   STACK_CLASS = 0,
//   CORE_STRUCT_CLASS,
//   VFP_CLASS,
//   VFP_STRUCT_CLASS,
//   CORE_CLASS,
//   NB_CLASSES
// };

// struct param_plan
// {
//   int start;               /* first reg or addr used depending on the class */
//   int end;                 /* last reg used or next free addr depending on the class */
//   SValue *sval;            /* pointer to SValue on the value stack */
//   struct param_plan *prev; /*  previous element in this class */
// };

// struct plan
// {
//   struct param_plan *pplans;               /* array of all the param plans */
//   struct param_plan *clsplans[NB_CLASSES]; /* per class lists of param plans */
//   int nb_plans;
// };

// static void add_param_plan(struct plan *plan, int cls, int start, int end, SValue *v)
// {
//   struct param_plan *p = &plan->pplans[plan->nb_plans++];
//   p->prev = plan->clsplans[cls];
//   plan->clsplans[cls] = p;
//   p->start = start, p->end = end, p->sval = v;
// }

// /* Assign parameters to registers and stack with alignment according to the
//    rules in the procedure call standard for the ARM architecture (AAPCS).
//    The overall assignment is recorded in an array of per parameter structures
//    called parameter plans. The parameter plans are also further organized in a
//    number of linked lists, one per class of parameter (see the comment for the
//    definition of union reg_class).

//    nb_args: number of parameters of the function for which a call is generated
//    float_abi: float ABI in use for this function call
//    plan: the structure where the overall assignment is recorded
//    todo: a bitmap that record which core registers hold a parameter

//    Returns the amount of stack space needed for parameter passing

//    Note: this function allocated an array in plan->pplans with tcc_malloc. It
//    is the responsibility of the caller to free this array once used (ie not
//    before copy_params). */
// static int assign_regs(int nb_args, int float_abi, struct plan *plan, int *todo)
// {
//   int i, size, align;
//   int ncrn /* next core register number */, nsaa /* next stacked argument address*/;
//   struct avail_regs avregs = {{0}};

//   ncrn = nsaa = 0;
//   *todo = 0;

//   for (i = nb_args; i--;)
//   {
//     int j, start_vfpreg = 0;
//     CType type = vtop[-i].type;
//     ElfSym *sym = NULL;
//     type.t &= ~VT_ARRAY;
//     size = type_size(&type, &align);
//     size = (size + 3) & ~3;
//     align = (align + 3) & ~3;
//     // if argument is a function pointer, then symbol must be exported
//     if (vtop[-i].r & VT_SYM)
//     {
//       if (((type.t & VT_BTYPE) == VT_FUNC) ||
//           ((type.t & VT_BTYPE) == VT_PTR && type.ref && (type.ref->type.t & VT_BTYPE) == VT_FUNC))
//       {
//         sym = elfsym(vtop[-i].sym);
//       }
//     }
//     if (sym != NULL)
//     {
//       sym->st_info |= (STB_GLOBAL << 4);
//     }

//     switch (vtop[-i].type.t & VT_BTYPE)
//     {
//     case VT_STRUCT:
//     case VT_FLOAT:
//     case VT_DOUBLE:
//     case VT_LDOUBLE:
//       if (float_abi == ARM_HARD_FLOAT)
//       {
//         int is_hfa = 0; /* Homogeneous float aggregate */

//         if (is_float(vtop[-i].type.t) || (is_hfa = is_hgen_float_aggr(&vtop[-i].type)))
//         {
//           int end_vfpreg;

//           start_vfpreg = assign_vfpreg(&avregs, align, size);
//           end_vfpreg = start_vfpreg + ((size - 1) >> 2);
//           if (start_vfpreg >= 0)
//           {
//             add_param_plan(plan, is_hfa ? VFP_STRUCT_CLASS : VFP_CLASS, start_vfpreg, end_vfpreg, &vtop[-i]);
//             continue;
//           }
//           else
//             break;
//         }
//       }
//       ncrn = (ncrn + (align - 1) / 4) & ~((align / 4) - 1);
//       if (ncrn + size / 4 <= 4 || (ncrn < 4 && start_vfpreg != -1))
//       {
//         /* The parameter is allocated both in core register and on stack. As
//          * such, it can be of either class: it would either be the last of
//          * CORE_STRUCT_CLASS or the first of STACK_CLASS. */
//         for (j = ncrn; j < 4 && j < ncrn + size / 4; j++)
//           *todo |= (1 << j);
//         add_param_plan(plan, CORE_STRUCT_CLASS, ncrn, j, &vtop[-i]);
//         ncrn += size / 4;
//         if (ncrn > 4)
//           nsaa = (ncrn - 4) * 4;
//       }
//       else
//       {
//         ncrn = 4;
//         break;
//       }
//       continue;
//     default:
//       if (ncrn < 4)
//       {
//         int is_long = (vtop[-i].type.t & VT_BTYPE) == VT_LLONG;
//         if (is_long)
//         {
//           ncrn = (ncrn + 1) & -2;
//           if (ncrn == 4)
//             break;
//         }
//         add_param_plan(plan, CORE_CLASS, ncrn, ncrn + is_long, &vtop[-i]);
//         ncrn += 1 + is_long;
//         continue;
//       }
//     }
//     nsaa = (nsaa + (align - 1)) & ~(align - 1);
//     add_param_plan(plan, STACK_CLASS, nsaa, nsaa + size, &vtop[-i]);
//     nsaa += size; /* size already rounded up before */
//   }
//   return nsaa;
// }

static void th_literal_pool_init()
{
  thumb_gen_state.literal_pool_size = 64;
  thumb_gen_state.literal_pool_count = 0;
  thumb_gen_state.literal_pool = tcc_mallocz(sizeof(ThumbLiteralPoolEntry) * thumb_gen_state.literal_pool_size);
  thumb_gen_state.generating_function = 0;
  thumb_gen_state.code_size = 0;
  thumb_gen_state.cached_global_sym = NULL;
  thumb_gen_state.cached_global_reg = -1;
}

const FloatingPointConfig arm_soft_fpu_config = {
    .reg_size = 0,
    .reg_count = 0,
    .stack_align = 0,
    .has_fadd = 0,
    .has_fsub = 0,
    .has_fmul = 0,
    .has_fdiv = 0,
    .has_fcmp = 0,
    .has_ftof = 0,
    .has_itof = 0,
    .has_ftod = 0,
    .has_ftoi = 0,
    .has_dadd = 0,
    .has_dsub = 0,
    .has_dmul = 0,
    .has_ddiv = 0,
    .has_dcmp = 0,
    .has_dtof = 0,
    .has_itod = 0,
    .has_dtoi = 0,
};

const FloatingPointConfig *arm_determine_fpu_config(struct TCCState *s)
{
  if (s->fpu_type == 0)
  {
    return &arm_soft_fpu_config;
  }

  switch (s->fpu_type)
  {
  case ARM_FPU_FPV5_SP_D16:
    return &arm_fpv5_sp_d16_fpu_config;
  default:
    fprintf(stderr, "unsupported FPU type: %d for ARM architecture", s->fpu_type);
    exit(1);
    return NULL;
  }
}

ST_FUNC void arm_init(struct TCCState *s)
{
  float_type.t = VT_FLOAT;
  double_type.t = VT_DOUBLE;
  func_float_type.t = VT_FUNC;
  func_float_type.ref = sym_push(SYM_FIELD, &float_type, FUNC_CDECL, FUNC_OLD);
  func_double_type.t = VT_FUNC;
  func_double_type.ref = sym_push(SYM_FIELD, &double_type, FUNC_CDECL, FUNC_OLD);
  float_abi = s->float_abi;
  text_and_data_separation = s->text_and_data_separation;
  pic = s->pic;
  s->parameters_registers = 4;
  s->registers_map_for_allocator = (1 << ARM_R0) | (1 << ARM_R1) | (1 << ARM_R2) | (1 << ARM_R3) | (1 << ARM_R4) |
                                   (1 << ARM_R5) | (1 << ARM_R6) | (1 << ARM_R8) | (1 << ARM_R10) | (1 << ARM_R11) |
                                   (1 << ARM_R12);

  s->registers_for_allocator = 11;
  caller_saved_registers = (1 << ARM_R0) | (1 << ARM_R1) | (1 << ARM_R2) | (1 << ARM_R3);

  /* For hard float ABI, configure VFP single-precision registers S0-S15 */
  architecture_config.fpu = arm_determine_fpu_config(s);
  if (float_abi == ARM_HARD_FLOAT)
  {
    s->float_registers_for_allocator = architecture_config.fpu->reg_count;
    s->float_registers_map_for_allocator = (1ull << ((uint64_t)s->float_registers_for_allocator)) - 1;
  }
  else
  {
    /* No VFP registers for soft float */
    s->float_registers_map_for_allocator = 0;
    s->float_registers_for_allocator = 0;
  }

  if (!s->pic)
  {
    s->registers_map_for_allocator |= (1 << ARM_R9);
    s->registers_for_allocator += 1;
  }

  if (s->omit_frame_pointer)
  {
    s->registers_map_for_allocator |= (1 << ARM_R7);
    s->registers_for_allocator += 1;
  }

  th_literal_pool_init();
}

static int regmask(int r)
{
  return reg_classes[r] & ~(RC_INT | RC_FLOAT);
}

/*
 * Write 2 - byte Thumb instruction
 * current write position must be 16-bit aligned
 */
void o(unsigned int i)
{
  const int ind1 = ind + 2;
  TRACE("  o: 0x%03x pc: 0x%x", i, ind);
  if (nocode_wanted)
  {
    return;
  }
  if (!cur_text_section)
  {
    tcc_error("compiler error! This happens f.ex. if the compiler\n"
              "can't evaluate constant expressions outside of a function.");
  }
  if (ind1 > cur_text_section->data_allocated)
  {
    section_realloc(cur_text_section, ind1);
  }
  cur_text_section->data[ind++] = i & 255;
  cur_text_section->data[ind++] = i >> 8;
}

static void th_literal_pool_generate(void)
{
  static int generating_pool = 0; /* Prevent recursive calls */

  if (generating_pool)
    return;

  if (thumb_gen_state.literal_pool_count == 0)
  {
    thumb_gen_state.code_size = 0;
    return;
  }

  generating_pool = 1;

  /* Count unique literals to calculate pool size */
  int unique_count = 0;
  int pool_size = 0;
  for (int i = 0; i < thumb_gen_state.literal_pool_count; i++)
  {
    if (thumb_gen_state.literal_pool[i].shared_index == -1)
    {
      unique_count++;
      /* Account for 8-byte literals */
      int entry_size = thumb_gen_state.literal_pool[i].data_size > 0 ? thumb_gen_state.literal_pool[i].data_size : 4;
      pool_size += entry_size;
    }
  }

  /* Emit a branch to skip over the literal pool.
   * We may need +2 for alignment NOP.
   * Branch offset is from PC+4 to after the pool.
   */
  int branch_pos = ind;
  int need_align = (ind & 2) ? 2 : 0; /* alignment padding after branch */

  if (thumb_gen_state.generating_function)
  {
    /* Emit placeholder branch (will be patched later) - use 32-bit B.W */
    o(0xf000); /* first halfword of B.W */
    o(0x9000); /* second halfword placeholder */
  }

  if (need_align)
  {
    /* align to 4 bytes after branch */
    thumb_opcode nop =
        th_mov_reg(R0, R0, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE, false);
    o(nop.opcode & 0xffff);
  }

  /* Array to store the output position of each unique literal */
  int *literal_positions = tcc_malloc(thumb_gen_state.literal_pool_count * sizeof(int));

  th_sym_d();

  /* First pass: emit unique literals and record their positions */
  for (int i = 0; i < thumb_gen_state.literal_pool_count; i++)
  {
    ThumbLiteralPoolEntry *entry = &thumb_gen_state.literal_pool[i];
    if (entry->shared_index == -1)
    {
      /* This is a unique entry - emit the literal value */
      literal_positions[i] = ind;
      if (entry->relocation != -1 && entry->sym)
      {
        /* Extra validation - check that sym looks valid */
        if (!entry->sym || (unsigned long)entry->sym < 0x1000)
        {
          tcc_warning("internal: literal pool entry has garbage sym pointer %p", entry->sym);
          entry->sym = NULL;
        }
        else if (entry->sym->v == 0xDEADBEEF)
        {
          /* Use-after-free detected */
          entry->sym = NULL;
        }
        else if (entry->sym->v == 0 || (entry->sym->v < TOK_IDENT && !(entry->sym->v & SYM_FIELD)))
        {
          tcc_warning("internal: literal pool entry has invalid sym->v (0x%x)", entry->sym->v);
          entry->sym = NULL;
        }
      }
      if (entry->relocation != -1 && entry->sym)
      {
        /* Validate symbol before creating relocation - sym must have valid ELF index
         * or be registerable. Type descriptors (SYM_FIELD) have c=-1 and should not
         * have relocations created for them. */
        if (entry->sym->c <= 0)
        {
          /* Try to register the symbol */
          put_extern_sym(entry->sym, NULL, 0, 0);
        }
        if (entry->sym->c > 0)
        {
          greloc(cur_text_section, entry->sym, ind, entry->relocation);
        }
        else
        {
          /* Symbol couldn't be registered (e.g., type descriptor).
           * This indicates a bug - sym should not have been set for this literal. */
          tcc_warning("internal: literal pool entry has invalid symbol (c=%d, v=0x%x), skipping relocation",
                      entry->sym->c, entry->sym->v);
        }
      }
      // write the literal value
      int entry_size = entry->data_size > 0 ? entry->data_size : 4;
      if (entry_size == 8)
      {
        /* 64-bit literal - write 8 bytes */
        o(entry->imm & 0xffff);
        o((entry->imm >> 16) & 0xffff);
        o((entry->imm >> 32) & 0xffff);
        o((entry->imm >> 48) & 0xffff);
      }
      else
      {
        /* 32-bit literal - write 4 bytes */
        o(entry->imm & 0xffff);
        o((entry->imm >> 16) & 0xffff);
      }
    }
    else
    {
      /* Shared entry - will use position of the original */
      literal_positions[i] = literal_positions[entry->shared_index];
    }
  }

  /* Patch the branch instruction to jump to after the pool */
  int branch_target = ind - branch_pos - 4; /* offset from PC (branch_pos + 4) */
  if (thumb_gen_state.generating_function)
  {
    thumb_opcode branch = th_b_t4(branch_target);
    uint16_t *branch_patch = (uint16_t *)(cur_text_section->data + branch_pos);
    branch_patch[0] = (branch.opcode >> 16) & 0xffff;
    branch_patch[1] = branch.opcode & 0xffff;
  }
  th_sym_t();

  /* Second pass: patch all instructions to point to correct literal position */
  for (int i = 0; i < thumb_gen_state.literal_pool_count; i++)
  {
    ThumbLiteralPoolEntry *entry = &thumb_gen_state.literal_pool[i];
    int literal_pos = literal_positions[i];
    int aligned_position = ((literal_pos - entry->patch_position) + 3) & ~3;

    // patch the instruction that references this literal
    if (entry->short_instruction)
    {
      /* Short LDR literal (T1): imm8 word-aligned in bits 0-7 */
      uint16_t *patch_ins = (uint16_t *)(cur_text_section->data + entry->patch_position);
      *patch_ins |= (((aligned_position - 4) >> 2) & 0x00ff);
    }
    else if (entry->data_size == 8)
    {
      /* LDRD literal: imm8 word-aligned in bits 0-7 of second halfword, P=1 U=1 in first halfword */
      uint16_t *patch_ins0 = (uint16_t *)(cur_text_section->data + entry->patch_position);
      uint16_t *patch_ins1 = (uint16_t *)(cur_text_section->data + entry->patch_position + 2);
      /* Set P=1 (bit 8) and U=1 (bit 7) for positive offset, pre-indexed */
      *patch_ins0 |= (1 << 8) | (1 << 7); /* P and U bits */
      *patch_ins1 |= (((aligned_position - 4) >> 2) & 0x00ff);
    }
    else
    {
      /* Long LDR literal (T2): imm12 byte offset in bits 0-11 of second halfword */
      uint16_t *patch_ins = (uint16_t *)(cur_text_section->data + entry->patch_position + 2);
      *patch_ins |= (((aligned_position - 4)) & 0x0fff);
    }
  }

  tcc_free(literal_positions);
  thumb_gen_state.literal_pool_count = 0;
  thumb_gen_state.code_size = 0;
  generating_pool = 0;
}

int is_valid_opcode(thumb_opcode op)
{
  return (op.size == 2 || op.size == 4);
}

int ot(thumb_opcode op)
{
  if (op.size == 0)
    return op.size;

  if (thumb_gen_state.generating_function)
  {
    thumb_gen_state.code_size += op.size;
    // 16-bit encoding for ldr should be efficient
    const int max_offset = thumb_gen_state.code_size + thumb_gen_state.literal_pool_count * 4;
    if (max_offset >= 1020)
    {
      th_literal_pool_generate();
    }
  }

  if (op.size == 4)
    o(op.opcode >> 16);
  o(op.opcode & 0xffff);
  return op.size;
}

static void load_full_const(int r, int r1, int64_t imm, struct Sym *sym);
static void gcall_or_jump(int is_jmp, SValue *dest);

// TODO: this is armv7-m code
int decbranch(int pos)
{
  int xa = *(uint16_t *)(cur_text_section->data + pos);
  int xb = *(uint16_t *)(cur_text_section->data + pos + 2);

  TRACE("  decbranch ins at pos 0x%.8x, target inst 0x%x 0x%x", pos, xa, xb);

  if ((xa & 0xf000) == 0xd000)
  {
    // Branch encoding t1
    xa &= 0x00ff;
    if (xa & 0x0080)
      xa -= 0x100;
    xa = (xa * 2) + pos + 4;
  }
  else if ((xa & 0xf800) == 0xe000)
  {
    // Branch encoding t2
    xa &= 0x7ff;
    if (xa & 0x400)
      xa -= 0x800;
    xa = (xa * 2) + pos + 4;
  }
  else if ((xa & 0xf800) == 0xf000 && (xb & 0xd000) == 0x8000)
  {
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
  }
  else if ((xa & 0xf800) == 0xf000 && (xb & 0xd000) == 0x9000)
  {
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
  }
  else
  {
    tcc_error("internal error: decbranch unknown encoding pos 0x%x, inst: 0x%x\n", pos, xa);
    return 0;
  }

  return xa;
}

static thumb_opcode th_generic_mov_imm(uint32_t r, int imm)
{
  if (imm < 0)
  {
    return th_mvn_imm(r, 0, -imm + 1, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE);
  }
  return th_mov_imm(r, imm, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE);
}
int th_offset_to_reg(int off, int sign)
{
  // we will crash if there is no reg available
  // int rr = get_reg(RC_INT);
  int rr = R_LR; // can I use R_LR here? lr should be already saved in proluge right?

  // if mov is not possible then load from data
  if (!ot(th_generic_mov_imm(rr, off)))
  {
    load_full_const(rr, -1, sign ? -off : off, NULL);
    return rr;
  }

  if (sign)
    ot_check(th_rsb_imm(rr, rr, 0, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE));
  return rr;
}

int th_patch_call(int t, int a)
{
  uint16_t *x = (uint16_t *)(cur_text_section->data + t);
  int lt = t;

  TRACE("'th_patch_call' t: %.8x, a: %.8x\n", t, a);

  t = decbranch(t);
  TRACE("t: %.8x\n", t);
  if (a == lt + 2)
    *x = 0xbf00;
  else if ((*x & 0xf000) == 0xd000)
  {
    *x &= 0xff00;
    *x |= th_encbranch_8(lt, a);
  }
  else if ((*x & 0xf800) == 0xe000)
  {
    *x &= 0xf800;
    *x |= th_encbranch_11(lt, a);
  }
  else if ((x[0] & 0xf800) == 0xf000 && (x[1] & 0xd000) == 0x8000)
  {
    uint32_t enc = 0;
    x[0] &= 0xfbc0;
    x[1] &= 0xd000;
    enc = th_encbranch_b_t3(th_encbranch_20(lt, a));
    x[0] |= enc >> 16;
    x[1] |= enc;
  }
  else if ((x[0] & 0xf800) == 0xf000 && (x[1] & 0xd000) == 0x9000)
  {
    uint32_t enc = 0;
    x[0] &= 0xf800;
    x[1] &= 0xd000;
    enc = th_packimm_10_11_0(th_encbranch_20(lt, a) << 1);
    x[0] |= enc >> 16;
    x[1] |= enc;
  }
  else
    tcc_error("compiler_error: unhandled branch type in th_patch_call for: t: "
              "0x%x, a: 0x%x, x: 0x%x 0x%x\n",
              t, a, x[0], x[1]);

  return t;
}

static void gadd_sp(int val)
{
  if (val > 0)
  {
    ot_check(th_add_sp_imm(R_SP, val, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE));
  }
  else if (val < 0)
  {
    ot_check(th_sub_sp_imm(R_SP, -val, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE));
  }
}

// // all params needs to be passed in core registers or not
// static int floats_in_core_regs(const SValue *sval)
// {
//   if (!sval->sym)
//   {
//     return 0;
//   }

//   switch (sval->sym->v)
//   {
//   case TOK___floatundidf:
//   case TOK___floatundisf:
//   case TOK___fixunsdfdi:
//   case TOK___fixunssfdi:
//   case TOK___floatdisf:
//   case TOK___floatdidf:
//   case TOK___fixsfdi:
//   case TOK___fixdfdi:
//     return 1;
//   default:
//     return 0;
//   }
// }

void ggoto(void)
{
  TRACE("'ggoto'");
  // Computed goto - vtop contains the target address (pointer)
  // Emit an IR jump instruction with the target from vtop
  SValue dest;
  memset(&dest, 0, sizeof(SValue));
  dest = *vtop; // Copy vtop as the jump destination (indirect jump)
  tcc_ir_put(tcc_state->ir, TCCIR_OP_JUMP, NULL, NULL, &dest);
  vtop--;
  print_vstack("ggoto");
}

// ST_FUNC int gjmp(int t)
// {
//   int r = ind;
//   int val = ((t - r) >> 1) - 2;
//   TRACE("gjump t: 0x%x, r: %d, val: %d", t, r, val);
//   if (nocode_wanted)
//     return t;

//   // disable T16 instruction until root cause is found
//   // if (val < -1024 || val > 1023)
//   ot_check(th_b_t4(val << 1));
//   // else
//   // ot_check(th_b_t2(val << 1));
//   return r;
// }

// ST_FUNC void gjmp_addr(int a)
// {
//   TRACE("'gjump_addr'");
//   gjmp(a);
// }

// ST_FUNC int gjmp_append(int n, int t)
// {
//   int p, lp;
//   TRACE("gjmp_append n: 0x%x, t: 0x%x", n, t);
//   if (n)
//   {
//     p = n;
//     do
//     {
//       p = decbranch(lp = p);
//     } while (p);
//     th_patch_call(lp, t);
//     t = n;
//   }
//   return t;
// }

// ST_FUNC int gjmp_cond(int op, int t)
// {
//   int r = ind;

//   TRACE("'gjmp_cond' op: 0x%x, target 0x%x", op, t);

//   if (nocode_wanted)
//     return t;

//   op = mapcc(op);

//   ot_check(th_b_t3(op, th_encbranch_20(r, t)));
//   return r;
// }

void gsym_addr(int t, int a)
{
  TRACE("'gsym_addr' %.8x branch target: %.8x\n", t, a);

  while (t > 0) /* -1 or 0 means end of chain / no chain */
    t = th_patch_call(t, a);
}

ST_FUNC void gen_vla_alloc(CType *type, int align)
{
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

ST_FUNC void gen_vla_sp_save(int addr)
{
  tcc_error("gen_vla_sp_save not implemented yet");
  // SValue v;
  // v.type.t = VT_PTR;
  // v.r = VT_LOCAL | VT_LVAL;
  // v.c.i = addr;
  // store(TREG_SP, &v);
}

ST_FUNC void gen_vla_sp_restore(int addr)
{
  tcc_error("gen_vla_sp_restore not implemented yet");
  // SValue v;
  // v.type.t = VT_PTR;
  // v.r = VT_LOCAL | VT_LVAL;
  // v.c.i = addr;
  // load(TREG_SP, &v);
}

static int unalias_ldbl(int btype)
{
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
static int is_hgen_float_aggr(CType *type)
{
  if ((type->t & VT_BTYPE) == VT_STRUCT)
  {
    struct Sym *ref;
    int btype, nb_fields = 0;

    ref = type->ref->next;
    if (ref)
    {
      btype = unalias_ldbl(ref->type.t & VT_BTYPE);
      if (btype == VT_FLOAT || btype == VT_DOUBLE)
      {
        for (; ref && btype == unalias_ldbl(ref->type.t & VT_BTYPE); ref = ref->next, nb_fields++)
          ;
        return !ref && nb_fields <= 4;
      }
    }
  }
  return 0;
}

// How many registers are necessary to return struct via registers
// if not possible, then 0 means return via struct pointer
ST_FUNC int gfunc_sret(CType *vt, int variadic, CType *ret, int *ret_align, int *regsize)
{
  int align;
  const int size = type_size(vt, &align);

  TRACE("'gfunc_sret'");
  if (float_abi == ARM_HARD_FLOAT && !variadic && (is_float(vt->t) || is_hgen_float_aggr(vt)))
  {
    *ret_align = 8;
    *regsize = 8;
    ret->ref = NULL;
    ret->t = VT_DOUBLE;
    return ceil_div(size, 8);
  }
  else if (size > 0 && size <= 4)
  {
    *ret_align = 4;
    *regsize = 4;
    ret->ref = NULL;
    ret->t = VT_INT;
    return 1;
  }
  return 0;
}

#ifdef TCC_ARM_VFP
static uint32_t vfpr(int r)
{
  if (r < TREG_F0 || r > TREG_F7)
  {
    tcc_error("compiler_error: register: %d is not vfp register\n", r);
  }
  return r - TREG_F0;
}
#else
static uint32_t fpr(int r)
{
  if (r < TREG_F0 || r > TREG_F3)
  {
    tcc_error("compiler_error: register: %d is not fp register\n", r);
  }
  return r - TREF_F0;
}
#endif
// are those offsets to allow TREG_R0 start from other register than r0?
// not sure
static uint32_t intr(int r)
{
  if (r == TREG_R12)
  {
    return r;
  }
  if (r >= TREG_R0 && r <= TREG_R3)
  {
    return r - TREG_R0;
  }
  return r + (13 - TREG_SP);
}

void store(int r, SValue *sv)
{
  int v, fc, ft, fr, sign;
  TRACE("'store' reg: %d", r);

  fr = sv->r;
  ft = sv->type.t;
  fc = sv->c.i;

  if (fc >= 0)
    sign = 0;
  else
  {
    sign = 1;
    fc = -fc;
  }

  v = fr & VT_VALMASK;

  if (fr & VT_LVAL || fr == VT_LOCAL)
  {
    uint32_t base = R_FP;
    if (v < VT_CONST)
    {
      /* Check if pr0 is valid (not -1 and not spilled) */
      if (sv->pr0 >= 0 && !(sv->pr0 & PREG_SPILLED))
      {
        base = sv->pr0;
      }
      else
      {
        /* pr0 is spilled or invalid - need to load the address from stack.
         * The address is stored at the stack location in sv->c.i */
        int addr_offset = sv->c.i;
        int addr_sign = (addr_offset < 0);
        if (addr_sign)
          addr_offset = -addr_offset;
        /* Load the address into R_LR (use LR as scratch since R12 may be used for value) */
        if (!load_word_from_base(R_LR, R_FP, addr_offset, addr_sign))
        {
          int rr = th_offset_to_reg(addr_offset, addr_sign);
          ot_check(th_ldr_reg(R_LR, R_FP, rr, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
        }
        base = R_LR;
      }
      v = VT_LOCAL;
      fc = sign = 0;
    }
    else if (v == VT_CONST)
    {
      /* Check if we already have this global symbol's base address cached */
      if ((sv->r & VT_SYM) && sv->sym && sv->sym == thumb_gen_state.cached_global_sym &&
          thumb_gen_state.cached_global_reg >= 0)
      {
        /* Reuse cached base address, keep the offset */
        base = thumb_gen_state.cached_global_reg;
        /* fc already has the field offset from sv->c.i */
      }
      else
      {
        /* Load the base address of the global symbol (without offset) */
        SValue v1;
        Sym *validated_sym = (sv->r & VT_SYM) ? validate_sym_for_reloc(sv->sym) : NULL;
        memset(&v1, 0, sizeof(SValue));
        v1.type.t = ft;
        v1.r = (fr & ~VT_LVAL) | (validated_sym ? VT_SYM : 0);
        v1.c.i = 0; /* Load base address, not base+offset */
        v1.sym = validated_sym;
        load(base = 14, &v1);
        /* Cache this for subsequent accesses to same symbol */
        thumb_gen_state.cached_global_sym = validated_sym;
        thumb_gen_state.cached_global_reg = base;
        /* fc already has the field offset from sv->c.i */
      }
      sign = 0;
      v = VT_LOCAL;
    }
    if (v == VT_LOCAL)
    {
      if (is_float(ft))
      {
        /* Check if source is VFP or integer register.
         * Only use VFP instructions if hard float ABI is enabled. */
        if (tcc_state->float_abi == ARM_HARD_FLOAT && r >= TREG_F0 && r <= TREG_F7)
        {
          /* Source is VFP register - use VSTR */
          if ((ft & VT_BTYPE) != VT_FLOAT)
            ot_check(th_vstr(base, vfpr(r), !sign, 1, fc));
          else
            ot_check(th_vstr(base, vfpr(r), !sign, 0, fc));
        }
        else
        {
          /* Source is integer register - use regular STR for soft float path */
          if ((ft & VT_BTYPE) == VT_FLOAT)
          {
            /* Single precision - one 32-bit store */
            if (!ot(th_str_imm(r, base, fc, sign ? 4 : 6, ENFORCE_ENCODING_NONE)))
            {
              int rr = th_offset_to_reg(fc, sign);
              ot_check(th_str_reg(r, base, rr, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
            }
          }
          else
          {
            /* Double precision - two 32-bit stores (low word first) */
            /* Use sv->pr1 for high register, not r+1 which could be invalid */
            int r_high = sv->pr1;
            if (r_high < 0 || r_high == R_SP || r_high == R_PC)
            {
              /* Fallback: if pr1 not allocated, try r+1 but validate */
              r_high = r + 1;
              if (r_high == R_SP || r_high == R_PC)
              {
                tcc_error("compiler_error: cannot store double - no valid high "
                          "register (pr1=%d, r+1=%d would be SP/PC)\n",
                          sv->pr1, r + 1);
              }
            }
            /* Store low word */
            if (!ot(th_str_imm(r, base, fc, sign ? 4 : 6, ENFORCE_ENCODING_NONE)))
            {
              int rr = th_offset_to_reg(fc, sign);
              ot_check(th_str_reg(r, base, rr, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
            }
            /* Store high word at fc+4 */
            if (!ot(th_str_imm(r_high, base, fc + 4, sign ? 4 : 6, ENFORCE_ENCODING_NONE)))
            {
              int rr = th_offset_to_reg(fc + 4, sign);
              ot_check(th_str_reg(r_high, base, rr, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
            }
          }
        }
      }
      else if ((ft & VT_BTYPE) == VT_SHORT)
      {
        if (!ot(th_strh_imm(r, base, fc, sign ? 4 : 6, ENFORCE_ENCODING_NONE)))
        {
          int rr = th_offset_to_reg(fc, sign);
          ot_check(th_strh_reg(r, base, rr, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
        }
      }
      else if ((ft & VT_BTYPE) == VT_BYTE)
      {
        if (!ot(th_strb_imm(r, base, fc, sign ? 4 : 6, ENFORCE_ENCODING_NONE)))
        {
          int rr = th_offset_to_reg(fc, sign);
          ot_check(th_strb_reg(r, base, rr, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
        }
      }
      else
      {
        TRACE("store: sign: %x, r: %x, base: %x, fc: %x", sign, r, base, fc);
        if (!ot(th_str_imm(r, base, fc, sign ? 4 : 6, ENFORCE_ENCODING_NONE)))
        {
          int rr = th_offset_to_reg(fc, sign);
          ot_check(th_str_reg(r, base, rr, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
        }
        TRACE("done");
      }
    }
  }
}

static void load_vt_lval_vt_local_float(int r, SValue *sv, int ft, int fc, int sign, uint32_t base)
{
  if ((ft & VT_BTYPE) != VT_FLOAT)
  {
    // load double
    ot_check(th_vldr(base, vfpr(r), !sign, 1, fc));
  }
  else
  {
    ot_check(th_vldr(base, vfpr(r), !sign, 0, fc));
  }
}

static ThumbLiteralPoolEntry *th_literal_pool_allocate()
{
  ThumbLiteralPoolEntry *entry;
  if (thumb_gen_state.literal_pool_count >= thumb_gen_state.literal_pool_size)
  {
    const int new_size = thumb_gen_state.literal_pool_size << 1;
    thumb_gen_state.literal_pool = tcc_realloc(thumb_gen_state.literal_pool, new_size * sizeof(ThumbLiteralPoolEntry));
    thumb_gen_state.literal_pool_size = new_size;
  }
  entry = &thumb_gen_state.literal_pool[thumb_gen_state.literal_pool_count++];
  memset(entry, 0, sizeof(ThumbLiteralPoolEntry));
  entry->relocation = -1;
  entry->shared_index = -1;
  return entry;
}

/* Find existing literal pool entry with same sym and imm, and allocate new
   entry that shares its literal value */
static ThumbLiteralPoolEntry *th_literal_pool_find_or_allocate(Sym *sym, int64_t imm)
{
  int found_index = -1;
  /* Search existing entries for a match */
  for (int i = 0; i < thumb_gen_state.literal_pool_count; i++)
  {
    ThumbLiteralPoolEntry *e = &thumb_gen_state.literal_pool[i];
    /* Match on sym and imm, and it must be a primary entry (not shared) */
    if (e->sym == sym && e->imm == imm && e->shared_index == -1)
    {
      found_index = i;
      break;
    }
  }
  /* Allocate new entry */
  ThumbLiteralPoolEntry *entry = th_literal_pool_allocate();
  if (found_index >= 0)
  {
    /* Mark as sharing with the found entry */
    entry->shared_index = found_index;
  }
  return entry;
}

static void load_full_const(int r, int r1, int64_t imm, struct Sym *sym)
{
  int est = 0;
  ElfSym *esym = NULL;
  ThumbLiteralPoolEntry *entry;
  int sym_off = 0;

  /* Validate symbol - only use symbols that can be externalized */
  sym = validate_sym_for_reloc(sym);
  if (sym && sym->c == 0)
  {
    /* Symbol not yet registered - try to register it */
    put_extern_sym(sym, NULL, 0, 0);
    if (sym->c <= 0)
    {
      /* Registration failed - symbol can't be externalized */
      sym = NULL;
    }
  }

  if (sym)
  {
    esym = elfsym(sym);
  }
  entry = th_literal_pool_find_or_allocate(sym, imm);

  entry->sym = sym;
  entry->imm = imm;
  entry->patch_position = ind;
  entry->relocation = -1; /* No relocation by default */

  TRACE("'load_full_const' to register: %d, with imm: %d\n", r, imm);
  // allocate space for T1 encoding
  if (r1 != -1)
  {
    est = 4;
  }
  else
  {
    est = th_ldr_literal_estimate(r, 1020);
  }
  entry->short_instruction = est == 2 ? 1 : 0;

  if (r1 == -1)
  {
    entry->data_size = 4;
    ot_check(th_ldr_literal(r, 0, 1));
  }
  else
  {
    entry->data_size = 8;
    ot_check(th_ldrd_imm(r, r1, R_PC, 0, 4, ENFORCE_ENCODING_NONE));
  }

  if (esym)
  {
    sym_off = esym->st_shndx;
  }
  if (!pic)
  {
    if (sym)
    {
      entry->relocation = R_ARM_ABS32;
    }
  }
  else
  {
    if (sym)
    {
      if (text_and_data_separation)
      {
        // all data except constants in .ro section can be addressed relative to
        // .got, how can I distinguish that situation?
        //
        if (sym->type.t & VT_STATIC && sym_off != cur_text_section->sh_num)
        {
          entry->relocation = R_ARM_GOTOFF;
        }
        else
        {
          entry->relocation = R_ARM_GOT32;
        }
      }
      else
      {
        if (sym->type.t & VT_STATIC)
        {
          entry->relocation = R_ARM_REL32;
        }
        else
        {
          entry->relocation = R_ARM_GOT_PREL;
        }
      }
    }
  }

  if (pic)
  {
    if (sym)
    {
      if (text_and_data_separation)
      {
        if (sym->type.t & VT_STATIC && sym_off != cur_text_section->sh_num)
        {
          ot_check(th_add_reg(r, r, R9, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
        }
        else
        {
          thumb_opcode ot;
          ot_check(th_add_reg(r, r, R9, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));

          ot_check(th_ldr_imm(r, r, 0, 6, ENFORCE_ENCODING_NONE));
          ot = th_add_imm(r, r, imm, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE);
          if (ot.size != 0)
          {
            ot_check(ot);
          }
          else
          {
            // size += o.size;
            // ot_check(o);
            // ot_check(th_b_t4(4));
            // th_sym_d();
            // thus that immediate value must be preserved without linker touch
            // o(imm & 0xffff);
            // o(imm >> 16);
            // th_sym_t();
            ThumbLiteralPoolEntry *entry2 = th_literal_pool_allocate();
            entry2->sym = NULL;
            entry2->imm = imm;
            entry2->patch_position = ind;
            entry2->relocation = -1;
            entry2->data_size = r1 != -1 ? 8 : 4;
            entry2->short_instruction = false;
            ot_check(th_ldr_literal(R_LR, 0, 1));
            ot_check(th_add_reg(r, r, R_LR, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
          }
        }
      }
      else
      {
        if (sym->type.t & VT_STATIC)
        {
          ot_check(th_add_reg(r, r, R_PC, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
          ot_check(th_sub_imm(r, r, 8, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE));
        }
        else
        {
          thumb_opcode ot;
          ot_check(th_add_reg(r, r, R_PC, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
          ot_check(th_ldr_imm(r, r, 4, 6, ENFORCE_ENCODING_NONE));
          ot = th_add_imm(r, r, imm, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE);
          if (ot.size != 0)
          {
            ot_check(ot);
          }
          else
          {
            ThumbLiteralPoolEntry *entry2 = th_literal_pool_allocate();
            entry2->sym = NULL;
            entry2->imm = imm;
            entry2->patch_position = ind;
            entry2->relocation = -1;
            entry2->short_instruction = false;
            ot_check(th_ldr_literal(R_LR, 0, 1));
            ot_check(th_add_reg(r, r, R_LR, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
          }
        }
      }
    }
  }
}

int load_short_from_base(int ir, int base, int fc, int sign)
{
  const thumb_opcode ins = th_ldrsh_imm(ir, base, fc, sign ? 4 : 6, ENFORCE_ENCODING_NONE);
  TRACE("Load short sign: %d, r %d, base: %d, fc: %d\n", sign, ir, base, fc);
  return ot(ins);
}

int load_ushort_from_base(int ir, int base, int fc, int sign)
{
  const thumb_opcode ins = th_ldrh_imm(ir, base, fc, sign ? 4 : 6, ENFORCE_ENCODING_NONE);
  TRACE("Load ushort sign: %d, r %d, base: %d, fc: %d\n", sign, ir, base, fc);
  return ot(ins);
}

int load_byte_from_base(int ir, int base, int fc, int sign)
{
  const thumb_opcode ins = th_ldrsb_imm(ir, base, fc, sign ? 4 : 6, ENFORCE_ENCODING_NONE);
  TRACE("Load byte sign: %d, r %d, base: %d, fc: %d\n", sign, ir, base, fc);
  return ot(ins);
}

int load_ubyte_from_base(int ir, int base, int fc, int sign)
{
  const thumb_opcode ins = th_ldrb_imm(ir, base, fc, sign ? 4 : 6, ENFORCE_ENCODING_NONE);
  TRACE("Load ubyte sign: %d, r %d, base: %d, fc: %d\n", sign, ir, base, fc);
  return ot(ins);
}

int load_word_from_base(int ir, int base, int fc, int sign)
{
  const thumb_opcode ins = th_ldr_imm(ir, base, fc, sign ? 4 : 6, ENFORCE_ENCODING_NONE);
  TRACE("Load word sign: %d, r %d, base: %d, fc: %d\n", sign, ir, base, fc);
  return ot(ins);
}

int store_word_to_base(int ir, int base, int fc, int sign)
{
  const thumb_opcode ins = th_str_imm(ir, base, fc, sign ? 4 : 6, ENFORCE_ENCODING_NONE);
  TRACE("Store word sign: %d, r %d, base: %d, fc: %d\n", sign, ir, base, fc);
  return ot(ins);
}

void load_vt_lval_vt_local(int r, SValue *sv, int ft, int fc, int sign, uint32_t base)
{
  int success = 0;
  const int btype = ft & VT_BTYPE;
  int ir = intr(r);
  TRACE("load_vt_lval_vt_local: fc: %i", fc);

  if (is_float(ft))
  {
    /* Check if destination is a VFP register or an integer register.
     * For soft float (IR code path), floats are loaded to integer registers.
     * Note: r values 0-4 are always integer registers (R0-R3, R12=TREG_R12=4).
     * r values 5-12 could be TREG_F0-F7 OR physical R5-R12.
     * We use a heuristic: if r is a known scratch register (R12=12), use
     * integer path. Also check sv->pr0 - if it's PREG_SPILLED, we're loading
     * from stack to temp register for copy, which should use integer path.
     * Only use VFP if hard float ABI is enabled. */
    int use_vfp = (tcc_state->float_abi == ARM_HARD_FLOAT) && (r >= TREG_F0 && r <= TREG_F7);
    /* Override: if r is physical R12 (12), always use integer path */
    if (r == 12 || r == 14)
    {
      use_vfp = 0;
    }
    if (use_vfp)
    {
      /* VFP register - use VFP load instructions */
      TRACE("load float to VFP r: %d, base: %d, fc: %d, sign: %d\n", ir, base, fc, sign);
      return load_vt_lval_vt_local_float(r, sv, ft, fc, sign, base);
    }
    else
    {
      /* Integer register - load float as raw bits (soft float) */
      TRACE("load float to INT r: %d, base: %d, fc: %d, sign: %d\n", ir, base, fc, sign);
      if (btype == VT_DOUBLE || btype == VT_LDOUBLE)
      {
        /* Double: load 64 bits to pre-allocated register pair.
         * Use sv->pr0 (low) and sv->pr1 (high) from register allocator,
         * NOT ir+1 which could be SP/PC or already in use. */
        int ir_high = sv->pr1;
        if (ir_high < 0 || ir_high == R_SP || ir_high == R_PC)
        {
          /* Fallback: if pr1 not allocated, try ir+1 but validate */
          ir_high = ir + 1;
          if (ir_high == R_SP || ir_high == R_PC)
          {
            tcc_error("compiler_error: cannot load double - no valid high register "
                      "(pr1=%d, ir+1=%d would be SP/PC)\n",
                      sv->pr1, ir + 1);
          }
        }
        /* Load low word first */
        success = load_word_from_base(ir, base, fc, sign);
        if (!success)
        {
          int rr = th_offset_to_reg(fc, sign);
          ot_check(th_ldr_reg(ir, base, rr, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
        }
        /* Load high word.
         * For negative offsets (sign=1), high word is at fc-4 (closer to base).
         * For positive offsets (sign=0), high word is at fc+4 (further from
         * base).
         */
        int fc_high = sign ? (fc - 4) : (fc + 4);
        int sign_high = sign;
        /* Handle case where fc_high becomes 0 or changes sign */
        if (sign && fc_high < 0)
        {
          fc_high = -fc_high;
          sign_high = 0;
        }
        success = load_word_from_base(ir_high, base, fc_high, sign_high);
        if (!success)
        {
          int rr = th_offset_to_reg(fc_high, sign_high);
          ot_check(th_ldr_reg(ir_high, base, rr, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
        }
      }
      else
      {
        /* Float: load 32 bits to single integer register */
        success = load_word_from_base(ir, base, fc, sign);
        if (!success)
        {
          int rr = th_offset_to_reg(fc, sign);
          ot_check(th_ldr_reg(ir, base, rr, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
        }
      }
      return;
    }
  }
  else if (btype == VT_SHORT)
  {
    TRACE("load short to r: %d, base: %d, fc: %d, sign: %d\n", ir, base, fc, sign);
    if (!(ft & VT_UNSIGNED))
    {
      success = load_short_from_base(ir, base, fc, sign);
    }
    else
    {
      success = load_ushort_from_base(ir, base, fc, sign);
    }
  }
  else if (btype == VT_BYTE || btype == VT_BOOL)
  {
    if (!(ft & VT_UNSIGNED))
    {
      success = load_byte_from_base(ir, base, fc, sign);
    }
    else
    {
      success = load_ubyte_from_base(ir, base, fc, sign);
    }
  }
  else
  {
    success = load_word_from_base(ir, base, fc, sign);
  }
  if (!success)
  {

    // now load from dereferenced value
    int rr = th_offset_to_reg(fc, sign);
    if (btype == VT_SHORT)
    {
      if (ft & VT_UNSIGNED)
        ot_check(th_ldrh_reg(ir, base, rr, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
      else
        ot_check(th_ldrsh_reg(ir, base, rr, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
    }
    else if (btype == VT_BYTE || btype == VT_BOOL)
    {
      if (ft & VT_UNSIGNED)
        ot_check(th_ldrb_reg(ir, base, rr, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
      else
        ot_check(th_ldrsb_reg(ir, base, rr, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
    }
    else
      ot_check(th_ldr_reg(ir, base, rr, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
  }
}

void load_vt_const(int r, int r1, SValue *sv)
{
  TRACE("'load_vt_const' r: %i, const: %i, sym: %i", r, (int)sv->c.i, (sv->r & VT_SYM) == VT_SYM);

  if (tcc_is_64bit_operand(sv))
  {
    const uint64_t val64 = sv->c.i; /* c.i is the same memory as c.d due to union */
    const uint32_t lo = (uint32_t)(val64 & 0xFFFFFFFF);
    const uint32_t hi = (uint32_t)(val64 >> 32);
    thumb_opcode o1 = th_generic_mov_imm(r, lo);
    thumb_opcode o2 = th_generic_mov_imm(r1, hi);
    if (o1.size == 0 && o2.size == 0)
    {
      return load_full_const(r, r1, val64, 0);
    }
    if (!ot(o1))
      load_full_const(r, -1, lo, 0);
    if (!ot(o2))
      load_full_const(r1, -1, hi, 0);
    return; /* Don't fall through to 32-bit code */
  }

  if (sv->r & VT_SYM)
  {
    Sym *validated_sym = validate_sym_for_reloc(sv->sym);
    if (validated_sym)
    {
      return load_full_const(r, r1, sv->c.i, validated_sym);
    }
    /* Invalid or missing sym - treat as constant without relocation */
  }

  if (!ot(th_generic_mov_imm(r, sv->c.i)))
    load_full_const(r, r1, sv->c.i, 0);
}

void load_vt_local(int r, SValue *sv)
{
  TRACE("'load_vt_local' r: %d, off: %x", r, (uint32_t)sv->c.i);
  Sym *sym_to_use = NULL;
  if (sv->r & VT_SYM)
  {
    sym_to_use = validate_sym_for_reloc(sv->sym);
  }
  if (sym_to_use || (-sv->c.i) >= 0xfff)
  {
    load_full_const(r, -1, sv->c.i, sym_to_use);
    ot_check(th_add_reg(r, R_FP, r, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
  }
  else
  {
    ot_check(th_sub_imm(r, R_FP, -sv->c.i, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE));
  }
}

void load_vt_cmp(int r, SValue *sv)
{
  const uint32_t firstcond = mapcc(sv->c.i);
  uint32_t rr = intr(r);
  TRACE("'load_vt_cmp' to reg: %d, op: 0x%x\n", r, (uint32_t)sv->c.i);
  if (rr == R_SP || rr == R_PC)
  {
    tcc_error("compiler_error: load_vt_cmp can't be used for pc or sp\n");
  }

  // it block
  o(0xbf00 | (firstcond << 4) | 0x4 | ((~firstcond & 1) << 3));
  ot_check(th_generic_mov_imm(rr, 1));
  ot_check(th_generic_mov_imm(rr, 0));
}

void load_vt_jmp_jmpi(int r, SValue *sv)
{
#ifdef TCC_TARGET_ARM_ARCHV6M
  if (intr(r) > 7)
  {
    tcc_error("compiler_error: implement load_vt_jmp_jmpi for armv6m\n");
  }
#endif
  ot_check(th_generic_mov_imm(intr(r), sv->r & 1));
  ot_check(th_b_t4(2));
  gsym(sv->c.i);
  ot_check(th_generic_mov_imm(intr(r), (sv->r ^ 1) & 1));
}

void load_to_dest(SValue *dest, SValue *sv)
{
  int v, ft, fr, sign;
  int64_t fc;
  fr = sv->r;
  ft = sv->type.t;
  fc = sv->c.i;
  int btype = ft & VT_BTYPE;

  if (fc >= 0)
    sign = 0;
  else
  {
    sign = 1;
    fc = -fc;
  }

  if (sv->r & VT_PARAM)
  {
    fc += offset_to_args;
  }

  v = fr & VT_VALMASK;

  // load lvalue from
  if (fr & VT_LVAL)
  {
    uint32_t base = R_FP;
    SValue v1;

    /* First check if this is a register-allocated LOCAL variable.
     * In this case pr0 contains the allocated register, and we should
     * do a register move instead of loading from memory.
     * NOTE: This only applies to VT_LOCAL, NOT to v < VT_CONST which means
     * the address is in a register and needs dereferencing. */
    if (v == VT_LOCAL && sv->pr0 >= 0 && !(sv->pr0 & PREG_SPILLED))
    {
      /* Allocated to register - do register move, not memory load. */
      /* For doubles in integer registers (soft float) */
      if (dest->pr0 != sv->pr0)
      {
        ot_check(th_mov_reg(dest->pr0, sv->pr0, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT,
                            ENFORCE_ENCODING_NONE, false));
      }
      if (tcc_is_64bit_operand(dest) && dest->pr1 != sv->pr1)
      {
        ot_check(th_mov_reg(dest->pr1, sv->pr1, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT,
                            ENFORCE_ENCODING_NONE, false));
      }
      return;
    }

    // load value from stack
    // prepare for new load after pointer dereference
    if (v == VT_LLOCAL)
    {
      v1.type.t = VT_PTR;
      v1.r = VT_LOCAL | VT_LVAL;
      v1.c.i = sv->c.i;

      TRACE("l1");
      load(base = 14, &v1);
      fc = sign = 0;
      v = VT_LOCAL;
    }
    else if (v == VT_CONST)
    {
      /* Check if we already have this global symbol's base address cached
       */
      if ((sv->r & VT_SYM) && sv->sym && sv->sym == thumb_gen_state.cached_global_sym &&
          thumb_gen_state.cached_global_reg >= 0)
      {
        /* Reuse cached base address, keep the offset */
        base = thumb_gen_state.cached_global_reg;
        /* fc already has the field offset from sv->c.i */
      }
      else
      {
        Sym *validated_sym = (sv->r & VT_SYM) ? validate_sym_for_reloc(sv->sym) : NULL;
        memset(&v1, 0, sizeof(SValue));
        v1.type.t = VT_PTR;
        v1.r = (fr & ~VT_LVAL) | (validated_sym ? VT_SYM : 0);
        v1.c.i = 0; /* Load base address, not base+offset */
        v1.sym = validated_sym;
        TRACE("l2");
        load(base = 14, &v1);
        /* Cache this for subsequent accesses to same symbol */
        thumb_gen_state.cached_global_sym = validated_sym;
        thumb_gen_state.cached_global_reg = base;
        /* fc already has the field offset from sv->c.i */
      }
      sign = 0;
      v = VT_LOCAL;
    }
    else if (v < VT_CONST)
    {
      /* For spilled lvalues, we need two-level indirection:
       * 1. Load the pointer from spill location [FP + spill_offset]
       * 2. Dereference that pointer to get the final value
       * For non-spilled, the pointer is already in a register (pr0). */
      if (sv->pr0 & PREG_SPILLED)
      {
        SValue v1;
        memset(&v1, 0, sizeof(SValue));
        v1.type.t = VT_PTR;
        v1.r = VT_LOCAL | VT_LVAL;
        v1.c.i = sv->c.i;

        TRACE("load_to_dest: loading spilled lvalue address from [FP%+lld]", (long long)fc);
        load(base = 14, &v1); /* Load pointer into R14 first */
        fc = sign = 0;        /* Dereference with offset 0 from loaded pointer */
        v = VT_LOCAL;
      }
      else
      {
        base = sv->pr0;
        fc = sign = 0;
        v = VT_LOCAL;
      }
    }

    if (v == VT_LOCAL)
    {
      return load_vt_lval_vt_local(dest->pr0, sv, ft, fc, sign, base);
    }
  }
  else if (v == VT_CONST)
    return load_vt_const(dest->pr0, dest->pr1, sv);
  else if (v == VT_LOCAL)
    return load_vt_local(dest->pr0, sv);
  else if (v == VT_CMP)
    return load_vt_cmp(dest->pr0, sv);
  else if (v == VT_JMP || v == VT_JMPI)
    return load_vt_jmp_jmpi(dest->pr0, sv);
  else if (v < VT_CONST)
  {
    // add float
    if (dest->pr0 != v)
    {
      ot_check(
          th_mov_reg(dest->pr0, v, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE, false));
    }
    if (dest->pr1 != -1 && tcc_is_64bit_operand(sv))
    {
      ot_check(th_mov_reg(dest->pr1, v + 1, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE,
                          false));
    }
    return;
  }
  tcc_error("compiler_error: unknown load not implemented\n");
}

// load value from stack to register
void load(int r, SValue *sv)
{
  int v, ft, fc, fr, sign;

  // TRACE("'load'");
  fr = sv->r;
  ft = sv->type.t;
  fc = sv->c.i;
  int btype = ft & VT_BTYPE;
  if (fc >= 0)
    sign = 0;
  else
  {
    sign = 1;
    fc = -fc;
  }

  if (sv->r & VT_PARAM)
  {
    fc += offset_to_args;
  }

  v = fr & VT_VALMASK;

  // load lvalue from
  if (fr & VT_LVAL)
  {
    uint32_t base = R_FP;
    if (tcc_state->ir->leaffunc)
    {
      base = R_SP;
    }
    SValue v1;

    // /* First check if this is a register-allocated parameter/variable.
    //  * In this case pr0 contains the allocated register, and we should
    //  * do a register move instead of loading from memory. */
    // if ((v == VT_LOCAL || v < VT_CONST) && sv->pr0 >= 0 && !(sv->pr0 & PREG_SPILLED))
    // {
    //   /* Allocated to register - do register move, not memory load. */
    //   if (is_float(ft))
    //   {
    //     /* For doubles in integer registers (soft float) */
    //     ot_check(
    //         th_mov_reg(r, sv->pr0, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE,
    //         false));
    //     if ((ft & VT_BTYPE) == VT_DOUBLE || (ft & VT_BTYPE) == VT_LDOUBLE)
    //     {
    //       int r_high = (r == R0) ? R1 : (r == R2) ? R3 : r + 1;
    //       ot_check(th_mov_reg(r_high, sv->pr1, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT,
    //                           ENFORCE_ENCODING_NONE, false));
    //     }
    //   }
    //   else
    //   {
    //     ot_check(
    //         th_mov_reg(r, sv->pr0, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE,
    //         false));
    //   }
    //   return;
    // }

    // load value from stack
    // prepare for new load after pointer dereference
    if (v == VT_LLOCAL)
    {
      v1.type.t = VT_PTR;
      v1.r = VT_LOCAL | VT_LVAL;
      v1.c.i = sv->c.i;

      TRACE("l1");
      load(base = 14, &v1);
      fc = sign = 0;
      v = VT_LOCAL;
    }
    else if (v == VT_CONST)
    {
      /* Check if we already have this global symbol's base address cached */
      if ((sv->r & VT_SYM) && sv->sym && sv->sym == thumb_gen_state.cached_global_sym &&
          thumb_gen_state.cached_global_reg >= 0)
      {
        /* Reuse cached base address, keep the offset */
        base = thumb_gen_state.cached_global_reg;
        /* fc already has the field offset from sv->c.i */
      }
      else
      {
        Sym *validated_sym = (sv->r & VT_SYM) ? validate_sym_for_reloc(sv->sym) : NULL;
        memset(&v1, 0, sizeof(SValue));
        v1.type.t = VT_PTR;
        v1.r = (fr & ~VT_LVAL) | (validated_sym ? VT_SYM : 0);
        v1.c.i = 0; /* Load base address, not base+offset */
        v1.sym = validated_sym;
        TRACE("l2");
        load(base = 14, &v1);
        /* Cache this for subsequent accesses to same symbol */
        thumb_gen_state.cached_global_sym = validated_sym;
        thumb_gen_state.cached_global_reg = base;
        /* fc already has the field offset from sv->c.i */
      }
      sign = 0;
      v = VT_LOCAL;
    }
    else if (v < VT_CONST)
    {
      /* For spilled lvalues, we need two-level indirection:
       * 1. Load the pointer from spill location [FP + spill_offset]
       * 2. Dereference that pointer to get the final value
       * For non-spilled, the pointer is already in a register (pr0). */
      if (sv->pr0 & PREG_SPILLED)
      {
        SValue v1;
        memset(&v1, 0, sizeof(SValue));
        v1.type.t = VT_PTR;
        v1.r = VT_LOCAL | VT_LVAL;
        v1.c.i = sv->c.i;

        load(base = 14, &v1); /* Load pointer into R14 first */
        fc = sign = 0;        /* Dereference with offset 0 from loaded pointer */
        v = VT_LOCAL;
      }
      else
      {
        base = sv->pr0;
        fc = sign = 0;
        v = VT_LOCAL;
      }
    }

    if (v == VT_LOCAL)
    {
      return load_vt_lval_vt_local(r, sv, ft, fc, sign, base);
    }
  }
  else if (v == VT_CONST)
    return load_vt_const(r, -1, sv);
  else if (v == VT_LOCAL)
    return load_vt_local(r, sv);
  else if (v == VT_CMP)
    return load_vt_cmp(r, sv);
  else if (v == VT_JMP || v == VT_JMPI)
    return load_vt_jmp_jmpi(r, sv);
  else if (v < VT_CONST)
  {
    /* For IR-generated code, use pr0 as the source register */
    int src_reg = (sv->pr0 >= 0 && !(sv->pr0 & PREG_SPILLED)) ? sv->pr0 : v;

    /* Check if spilled - load from stack instead of register move.
     * Note: pr0 might have been overwritten with destination register by caller,
     * so also check if c.i is non-zero (stack offset) as a backup indicator. */
    if ((sv->pr0 & PREG_SPILLED) || (v < VT_CONST && sv->c.i != 0 && (sv->r & VT_LVAL) == 0))
    {
      /* Value is spilled to stack at sv->c.i offset from FP */
      int src_offset = sv->c.i;
      int src_sign = (src_offset < 0);
      int src_abs = src_sign ? -src_offset : src_offset;
      if (!load_word_from_base(r, R_FP, src_abs, src_sign))
      {
        int rr = th_offset_to_reg(src_abs, src_sign);
        ot_check(th_ldr_reg(r, R_FP, rr, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
      }
      return;
    }
    if (is_float(ft))
    {
      /* Check if we're moving between VFP registers or integer registers.
       * Only use VFP if hard float ABI is enabled. */
      if (tcc_state->float_abi == ARM_HARD_FLOAT && r >= TREG_F0 && r <= TREG_F7 && src_reg >= TREG_F0 &&
          src_reg <= TREG_F7)
      {
        /* VFP to VFP move */
        if ((ft & VT_BTYPE) == VT_FLOAT)
          ot_check(th_vmov_register(vfpr(r), vfpr(src_reg), 0));
        else
          ot_check(th_vmov_register(vfpr(r), vfpr(src_reg), 1));
      }
      else
      {
        /* Integer register move (soft float) */
        ot_check(
            th_mov_reg(r, src_reg, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE, false));
        if ((ft & VT_BTYPE) == VT_DOUBLE || (ft & VT_BTYPE) == VT_LDOUBLE)
        {
          /* Also move high word for double.
           * Use sv->pr1 for destination high register, not r+1 which could be
           * invalid. Source high register comes from sv->r2. */
          int r_high = sv->pr1;
          int v_high = (sv->r2 != VT_CONST) ? sv->r2 : (src_reg + 1);
          if (r_high < 0 || r_high == R_SP || r_high == R_PC)
          {
            /* Fallback: if pr1 not allocated, try r+1 but validate */
            r_high = r + 1;
            if (r_high == R_SP || r_high == R_PC)
            {
              tcc_error("compiler_error: cannot move double - no valid high "
                        "dest register (pr1=%d, r+1=%d would be SP/PC)\n",
                        sv->pr1, r + 1);
            }
          }
          ot_check(th_mov_reg(r_high, v_high, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE,
                              false));
        }
      }
      return;
    }
    else
    {
      TRACE("mov r %i v %i", r, src_reg);
      ot_check(
          th_mov_reg(r, src_reg, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE, false));
      return;
    }
  }
  tcc_error("compiler_error: unknown load not implemented\n");
}

static int is_zero_on_stack(int pos)
{
  if ((vtop[pos].r & (VT_VALMASK | VT_LVAL | VT_SYM)) != VT_CONST)
    return 0;
  if (vtop[pos].type.t == VT_FLOAT)
    return vtop[pos].c.f == 0.f;
  if (vtop[pos].type.t == VT_DOUBLE)
    return vtop[pos].c.d == 0.0;
  return vtop[pos].c.ld = 0.l;
}

// static void gen_opf_regular(uint32_t opc, int fneg)
// {
//   uint32_t inst = 0;
//   int r = gv(RC_FLOAT);
//   opc |= 0xee000a00 | vfpr(r);
//   r = regmask(r);
//   if (!fneg)
//   {
//     int r2;
//     vswap();
//     r2 = gv(RC_FLOAT);
//     opc |= vfpr(r2) << 16;
//     r |= regmask(r2);
//   }
//   vtop->r = get_reg_ex(RC_FLOAT, r);
//   if (!fneg)
//   {
//     --vtop;
//     print_vstack("gen_opf_regular");
//   }
//   inst = opc | (vfpr(vtop->r) << 12);
//   o(inst >> 16);
//   o(inst);
// }

// static void gen_opf_cmp(uint32_t opc, uint32_t op)
// {
//   uint32_t inst = 0;
//   opc |= 0xeeb40a40;
//   if (op != TOK_EQ && op != TOK_NE)
//     opc |= 0x80;

//   if (is_zero_on_stack(0))
//   {
//     --vtop;
//     print_vstack("gen_opf_cmp(1)");
//     inst = opc | 0x10000 | (vfpr(gv(RC_FLOAT)) << 12);
//   }
//   else
//   {
//     opc |= vfpr(gv(RC_FLOAT));
//     vswap();
//     inst = opc | (vfpr(gv(RC_FLOAT)) << 12);
//     --vtop;
//     print_vstack("gen_opf_cmp(2)");
//   }

//   o(inst >> 16);
//   o(inst);
//   ot_check(th_vmrs(15));
// }

// void gen_opf(int op)
// {
//   const uint32_t is_double = ((vtop->type.t & VT_BTYPE) != VT_FLOAT) ? 0x100 : 0;

//   TRACE("gen_opf op: 0x%x(%c)", op, op);
//   switch (op)
//   {
//   case '+':
//   {
//     if (is_zero_on_stack(-1))
//       vswap();
//     if (is_zero_on_stack(0))
//     {
//       --vtop;
//       print_vstack("gen_opf(+)");
//       return;
//     }
//     return gen_opf_regular(is_double | 0x00300000, 0);
//   }
//   case '-':
//   {
//     if (is_zero_on_stack(0))
//     {
//       --vtop;
//       print_vstack("gen_opf(- 1)");
//       return;
//     }
//     if (is_zero_on_stack(-1))
//     {
//       vswap();
//       --vtop;
//       print_vstack("gen_opf(- 2)");
//       return gen_opf_regular(is_double | 0x00b10040, 1);
//     }
//     else
//       return gen_opf_regular(is_double | 0x00300040, 0);
//   }
//   case '*':
//     return gen_opf_regular(is_double | 0x002000000, 0);
//   case '/':
//     return gen_opf_regular(is_double | 0x008000000, 0);
//   default:
//   {
//     if (op < TOK_ULT || op > TOK_GT)
//       tcc_error("compiler_error: unknown floating-point operation: 0x%x", op);
//     if (is_zero_on_stack(-1))
//     {
//       vswap();
//       switch (op)
//       {
//       case TOK_LT:
//         op = TOK_GT;
//         break;
//       case TOK_GE:
//         op = TOK_ULE;
//         break;
//       case TOK_LE:
//         op = TOK_GE;
//         break;
//       case TOK_GT:
//         op = TOK_ULT;
//         break;
//       }
//     }
//     gen_opf_cmp(is_double, op);

//     switch (op)
//     {
//     case TOK_LE:
//       op = TOK_ULE;
//       break;
//     case TOK_LT:
//       op = TOK_ULT;
//       break;
//     case TOK_UGE:
//       op = TOK_GE;
//       break;
//     case TOK_UGT:
//       op = TOK_GT;
//       break;
//     }
//     vset_VT_CMP(op);
//   }
//   }
// }

ST_FUNC void gen_increment_tcov(SValue *sv)
{
  TRACE("'gen_increment_tcov'");
}

int th_has_immediate_value(int r)
{
  return (r & (VT_VALMASK | VT_LVAL | VT_SYM)) == VT_CONST;
}

typedef struct ThumbDataProcessingHandler
{
  thumb_opcode (*imm_handler)(uint32_t rd, uint32_t rn, uint32_t imm, thumb_flags_behaviour flags_behaviour,
                              thumb_enforce_encoding enforce_encoding);
  thumb_opcode (*reg_handler)(uint32_t rd, uint32_t rn, uint32_t rm, thumb_flags_behaviour flags_behaviour,
                              thumb_shift shift_type, thumb_enforce_encoding enforce_encoding);
} ThumbDataProcessingHandler;

void tcc_gen_machine_data_processing_op(TACQuadruple *op)
{
  ThumbDataProcessingHandler handler;
  thumb_flags_behaviour flags = FLAGS_BEHAVIOUR_NOT_IMPORTANT;

  /* Check for 64-bit operations */
  int is_64bit = is_64bit_type(op->dest.type.t);

  /* NOTE: All spilled register loading is now handled centrally in generate_code via
   * tcc_ir_preload_spills. This function receives valid physical registers in pr0/pr1. */

  switch (op->op)
  {
  case TCCIR_OP_ADD:
    if (is_64bit)
    {
      /* 64-bit add: ADDS for low words, ADC for high words */
      /* dest.pr0:pr1 = src1.pr0:pr1 + src2.pr0:pr1 */
      ot_check(th_add_reg(op->dest.pr0, op->src1.pr0, op->src2.pr0, FLAGS_BEHAVIOUR_SET, THUMB_SHIFT_DEFAULT,
                          ENFORCE_ENCODING_NONE));
      ot_check(th_adc_reg(op->dest.pr1, op->src1.pr1, op->src2.pr1, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT,
                          ENFORCE_ENCODING_NONE));
      return;
    }
    handler.imm_handler = th_add_imm;
    handler.reg_handler = th_add_reg;
    break;
  case TCCIR_OP_SUB:
    if (is_64bit)
    {
      /* 64-bit sub: SUBS for low words, SBC for high words */
      ot_check(th_sub_reg(op->dest.pr0, op->src1.pr0, op->src2.pr0, FLAGS_BEHAVIOUR_SET, THUMB_SHIFT_DEFAULT,
                          ENFORCE_ENCODING_NONE));
      ot_check(th_sbc_reg(op->dest.pr1, op->src1.pr1, op->src2.pr1, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT,
                          ENFORCE_ENCODING_NONE));
      return;
    }
    handler.imm_handler = th_sub_imm;
    handler.reg_handler = th_sub_reg;
    break;
  case TCCIR_OP_MUL:
    ot_check(th_mul(op->dest.pr0, op->src1.pr0, op->src2.pr0, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE));
    return;
  case TCCIR_OP_UMULL:
    /* UMULL: Unsigned 32x32 multiplication producing 64-bit result
     * RdLo (dest.pr0), RdHi (dest.pr1) = Rn (src1.pr0) * Rm (src2.pr0) */
    ot_check(th_umull(op->dest.pr0, op->dest.pr1, op->src1.pr0, op->src2.pr0));
    return;
  case TCCIR_OP_CMP:
    handler.imm_handler = th_cmp_imm;
    handler.reg_handler = th_cmp_reg;
    break;
  case TCCIR_OP_SHL:
  {
    handler.imm_handler = th_lsl_imm;
    handler.reg_handler = th_lsl_reg;
    break;
  }
  case TCCIR_OP_SHR:
  {
    handler.imm_handler = th_lsr_imm;
    handler.reg_handler = th_lsr_reg;
    break;
  }
  case TCCIR_OP_OR:
  {
    if (is_64bit)
    {
      /* 64-bit OR: just OR both halves */
      ot_check(th_orr_reg(op->dest.pr0, op->src1.pr0, op->src2.pr0, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT,
                          ENFORCE_ENCODING_NONE));
      ot_check(th_orr_reg(op->dest.pr1, op->src1.pr1, op->src2.pr1, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT,
                          ENFORCE_ENCODING_NONE));
      return;
    }
    handler.imm_handler = th_orr_imm;
    handler.reg_handler = th_orr_reg;
    break;
  }
  case TCCIR_OP_AND:
  {
    if (is_64bit)
    {
      /* 64-bit AND: just AND both halves */
      ot_check(th_and_reg(op->dest.pr0, op->src1.pr0, op->src2.pr0, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT,
                          ENFORCE_ENCODING_NONE));
      ot_check(th_and_reg(op->dest.pr1, op->src1.pr1, op->src2.pr1, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT,
                          ENFORCE_ENCODING_NONE));
      return;
    }
    handler.imm_handler = th_and_imm;
    handler.reg_handler = th_and_reg;
    break;
  }
  case TCCIR_OP_XOR:
  {
    if (is_64bit)
    {
      /* 64-bit XOR: just XOR both halves */
      ot_check(th_eor_reg(op->dest.pr0, op->src1.pr0, op->src2.pr0, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT,
                          ENFORCE_ENCODING_NONE));
      ot_check(th_eor_reg(op->dest.pr1, op->src1.pr1, op->src2.pr1, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT,
                          ENFORCE_ENCODING_NONE));
      return;
    }
    handler.imm_handler = th_eor_imm;
    handler.reg_handler = th_eor_reg;
    break;
  }
  case TCCIR_OP_SAR:
  {
    handler.imm_handler = th_asr_imm;
    handler.reg_handler = th_asr_reg;
    break;
  }
  case TCCIR_OP_DIV:
  {
    ot_check(th_sdiv(op->dest.pr0, op->src1.pr0, op->src2.pr0));
    return;
  }
  case TCCIR_OP_UDIV:
  {
    ot_check(th_udiv(op->dest.pr0, op->src1.pr0, op->src2.pr0));
    return;
  }
  case TCCIR_OP_ADC_USE:
  {
    handler.imm_handler = th_adc_imm;
    handler.reg_handler = th_adc_reg;
    break;
  }
  case TCCIR_OP_ADC_GEN:
  {
    handler.imm_handler = th_adc_imm;
    handler.reg_handler = th_adc_reg;
    flags = FLAGS_BEHAVIOUR_SET;
    break;
  }
  case TCCIR_OP_TEST_ZERO:
    ot_check(th_cmp_imm(0, intr(op->src1.pr0), 0, FLAGS_BEHAVIOUR_SET, ENFORCE_ENCODING_NONE));
    return;
  default:
  {
    printf("compiler_error: unhandled data processing op: %s\n", tcc_ir_get_op_name(op->op));
  }
  }

  if (op->op == TCCIR_OP_CMP)
  {
  }

  if (th_has_immediate_value(op->src2.r))
  {
    if (!ot(handler.imm_handler(op->dest.pr0, op->src1.pr0, op->src2.c.i, flags, ENFORCE_ENCODING_NONE)))
    {
      // load immediate to temp register and add
      load(R12, &op->src2);
      ot_check(handler.reg_handler(op->dest.pr0, op->src1.pr0, R12, flags, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
    }
  }
  else
  {
    ot_check(handler.reg_handler(op->dest.pr0, op->src1.pr0, op->src2.pr0, flags, THUMB_SHIFT_DEFAULT,
                                 ENFORCE_ENCODING_NONE));
  }
}

/* Get the soft float library function name for an FP operation */
static const char *get_softfp_func_name(TccIrOp op, int is_double)
{
  switch (op)
  {
  case TCCIR_OP_FADD:
    return is_double ? "__aeabi_dadd" : "__aeabi_fadd";
  case TCCIR_OP_FSUB:
    return is_double ? "__aeabi_dsub" : "__aeabi_fsub";
  case TCCIR_OP_FMUL:
    return is_double ? "__aeabi_dmul" : "__aeabi_fmul";
  case TCCIR_OP_FDIV:
    return is_double ? "__aeabi_ddiv" : "__aeabi_fdiv";
  case TCCIR_OP_FNEG:
    /* For negation, we can XOR the sign bit - handled separately */
    return NULL;
  default:
    return NULL;
  }
}

/* Helper to load a float operand to a VFP register.
 * If the operand is already in a VFP register, just return its number.
 * Otherwise, load to integer reg and move to the specified VFP scratch
 * register.
 */
static int load_fp_operand_to_vfp(SValue *sv, int scratch_sreg, int scratch_dreg, int is_double)
{
  /* Check if operand is already in a VFP register (pr0 has VFP marker) */
  if (sv->pr0 >= 0 && LS_IS_VFP_REG(sv->pr0))
  {
    return LS_VFP_REG_NUM(sv->pr0);
  }

  /* Not in VFP reg - load to integer reg and move to VFP scratch */
  load(R0, sv);
  if (is_double)
  {
    ot_check(th_vmov_2gp_dp(R0, R1, scratch_dreg, 0 /* to VFP */));
    return scratch_dreg * 2; /* D0 = S0:S1, D1 = S2:S3 */
  }
  else
  {
    ot_check(th_vmov_gp_sp(R0, scratch_sreg, 0 /* to VFP */));
    return scratch_sreg;
  }
}

/* Helper to store result from VFP register to destination.
 * If destination is a VFP register, move directly.
 * Otherwise, move to integer reg and store.
 */
static void store_fp_result_from_vfp(SValue *dest, int result_sreg, int result_dreg, int is_double)
{
  /* Check if destination is a VFP register */
  if (dest->pr0 >= 0 && LS_IS_VFP_REG(dest->pr0))
  {
    int dest_sreg = LS_VFP_REG_NUM(dest->pr0);
    if (is_double)
    {
      /* Move D-reg to D-reg (result_dreg to dest_dreg)
       * dest_sreg is S-register number, convert to D-register number */
      int dest_dreg = dest_sreg / 2;
      if (result_dreg != dest_dreg)
      {
        ot_check(th_vmov_register(dest_dreg, result_dreg, 1)); /* double */
      }
    }
    else
    {
      /* Move S-reg to S-reg */
      if (result_sreg != dest_sreg)
      {
        ot_check(th_vmov_register(dest_sreg, result_sreg, 0)); /* single */
      }
    }
    return;
  }

  /* Destination is not in VFP - move to integer reg and store/move */
  if (is_double)
  {
    ot_check(th_vmov_2gp_dp(R0, R1, result_dreg, 1 /* to ARM */));
    /* If dest has an allocated integer register pair, move to it */
    if (dest->pr0 >= 0 && !LS_IS_VFP_REG(dest->pr0) && !(dest->pr0 & PREG_SPILLED) && dest->pr1 >= 0)
    {
      /* Move R0:R1 to dest register pair */
      if (dest->pr0 != R0)
      {
        ot_check(th_mov_reg(dest->pr0, R0, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE,
                            false));
      }
      if (dest->pr1 != R1)
      {
        ot_check(th_mov_reg(dest->pr1, R1, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE,
                            false));
      }
    }
    else
    {
      /* Store both words to memory - either spilled or no pr1 allocated */
      /* For spilled vregs, set up r = VT_LOCAL so store() uses FP-relative */
      SValue store_dest = *dest;
      if (dest->pr0 & PREG_SPILLED)
      {
        store_dest.r = VT_LOCAL;
      }
      /* Use VT_INT type so store() treats each word as a single 32-bit store,
       * not a double that it would store both words for. */
      store_dest.type.t = VT_INT;
      store(R0, &store_dest);
      /* Store high word - adjust offset by 4 */
      store_dest.c.i += 4;
      store(R1, &store_dest);
    }
  }
  else
  {
    ot_check(th_vmov_gp_sp(R0, result_sreg, 1 /* to ARM */));
    /* If dest has an allocated integer register, move to it */
    if (dest->pr0 >= 0 && !LS_IS_VFP_REG(dest->pr0) && !(dest->pr0 & PREG_SPILLED))
    {
      if (dest->pr0 != R0)
      {
        ot_check(th_mov_reg(dest->pr0, R0, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE,
                            false));
      }
    }
    else
    {
      /* For spilled vregs, set up r = VT_LOCAL so store() uses FP-relative */
      SValue store_dest = *dest;
      if (dest->pr0 & PREG_SPILLED)
      {
        store_dest.r = VT_LOCAL;
      }
      store(R0, &store_dest);
    }
  }
}

/* Generate VFP hardware floating point operation.
 * Uses S0/D0 as temporary registers for operands and result.
 * For single precision: S0, S1, S2
 * For double precision: D0, D1
 */
static void gen_hardfp_op(TACQuadruple *q, int is_double)
{
  uint32_t sz = is_double ? 1 : 0;
  int src1_reg, src2_reg;

  /* Load first operand - may already be in a VFP register */
  src1_reg = load_fp_operand_to_vfp(&q->src1, 0 /* S0 */, 0 /* D0 */, is_double);

  /* Load second operand for binary ops */
  if (q->op != TCCIR_OP_FNEG)
  {
    src2_reg = load_fp_operand_to_vfp(&q->src2, 2 /* S2 */, 1 /* D1 */, is_double);
  }
  else
  {
    src2_reg = 0; /* unused for negation */
  }

  /* Perform the VFP operation - result in S0/D0 */
  switch (q->op)
  {
  case TCCIR_OP_FADD:
    if (is_double)
      ot_check(th_vadd_f(0, src1_reg / 2, src2_reg / 2, sz));
    else
      ot_check(th_vadd_f(0, src1_reg, src2_reg, sz));
    break;
  case TCCIR_OP_FSUB:
    if (is_double)
      ot_check(th_vsub_f(0, src1_reg / 2, src2_reg / 2, sz));
    else
      ot_check(th_vsub_f(0, src1_reg, src2_reg, sz));
    break;
  case TCCIR_OP_FMUL:
    if (is_double)
      ot_check(th_vmul_f(0, src1_reg / 2, src2_reg / 2, sz));
    else
      ot_check(th_vmul_f(0, src1_reg, src2_reg, sz));
    break;
  case TCCIR_OP_FDIV:
    if (is_double)
      ot_check(th_vdiv_f(0, src1_reg / 2, src2_reg / 2, sz));
    else
      ot_check(th_vdiv_f(0, src1_reg, src2_reg, sz));
    break;
  case TCCIR_OP_FNEG:
    ot_check(th_vneg_f(0, src1_reg, sz));
    break;
  default:
    tcc_error("compiler_error: unsupported FP op in gen_hardfp_op");
  }

  /* Store result from S0/D0 to destination */
  store_fp_result_from_vfp(&q->dest, 0 /* S0 */, 0 /* D0 */, is_double);
}

/* Generate VFP hardware floating point comparison.
 * Uses VCMP and VMRS to transfer flags to CPSR.
 */
static void gen_hardfp_cmp(TACQuadruple *q, int is_double)
{
  uint32_t sz = is_double ? 1 : 0;
  int src1_reg, src2_reg;

  /* Load operands - may already be in VFP registers */
  /* First load src1 to see what register it uses */
  src1_reg = load_fp_operand_to_vfp(&q->src1, 0 /* S0 */, 0 /* D0 */, is_double);

  /* Choose scratch for src2 that doesn't conflict with src1 */
  int scratch_s = (src1_reg < 4) ? 4 : 0; /* Use S4/D2 if src1 uses S0-S3 */
  int scratch_d = (src1_reg < 4) ? 2 : 0;

  src2_reg = load_fp_operand_to_vfp(&q->src2, scratch_s, scratch_d, is_double);

  /* VCMP - compare */
  if (is_double)
    ot_check(th_vcmp_f(src1_reg / 2, src2_reg / 2, sz));
  else
    ot_check(th_vcmp_f(src1_reg, src2_reg, sz));

  /* VMRS APSR_nzcv, FPSCR - transfer FP flags to CPSR */
  ot_check(th_vmrs(0x0f)); /* 0x0f = APSR_nzcv */
}

/* Generate float-to-float conversion (float <-> double).
 * Uses VCVT for hard float, library calls for soft float.
 */
static void gen_hardfp_cvt_ftof(TACQuadruple *q)
{
  int src_is_double = ((q->src1.type.t & VT_BTYPE) != VT_FLOAT);
  int dst_is_double = ((q->dest.type.t & VT_BTYPE) != VT_FLOAT);
  int src_reg;

  /* Load source - may already be in VFP register */
  src_reg = load_fp_operand_to_vfp(&q->src1, 0 /* S0 */, 0 /* D0 */, src_is_double);

  /* Convert */
  if (dst_is_double && !src_is_double)
  {
    /* float to double: Sn -> D0 */
    ot_check(th_vcvt_float_to_double(0, src_reg));
  }
  else if (!dst_is_double && src_is_double)
  {
    /* double to float: Dn -> S0 */
    ot_check(th_vcvt_double_to_float(0, src_reg / 2));
  }
  /* else: same type, no conversion needed - may need move */

  /* Store result to destination */
  store_fp_result_from_vfp(&q->dest, 0 /* S0 */, 0 /* D0 */, dst_is_double);
}

/* Generate int-to-float conversion.
 * Uses VCVT for hard float, library calls for soft float.
 */
static void gen_hardfp_cvt_itof(TACQuadruple *q)
{
  int src_bt = q->src1.type.t & VT_BTYPE;
  int dst_is_double = ((q->dest.type.t & VT_BTYPE) != VT_FLOAT);
  int is_unsigned = (q->src1.type.t & VT_UNSIGNED) ? 1 : 0;

  /* For LLONG, we need library call even in hard float mode */
  if (src_bt == VT_LLONG)
  {
    const char *func_name;
    if (dst_is_double)
    {
      func_name = is_unsigned ? "__aeabi_ul2d" : "__aeabi_l2d";
    }
    else
    {
      func_name = is_unsigned ? "__aeabi_ul2f" : "__aeabi_l2f";
    }
    // gen_softfp_call(q, func_name, 0);
    return;
  }

  /* Load integer to R0, then to S0 */
  load(R0, &q->src1);
  ot_check(th_vmov_gp_sp(R0, 0 /* S0 */, 0 /* to VFP */));

  /* VCVT: convert int in S0 to float/double in S0/D0
   * opc2=0 for unsigned, opc2=1 for signed
   * sz=1 for double, sz=0 for float
   * op=1 means int-to-float direction
   */
  ot_check(th_vcvt_fp_int(0, 0, 0 /* always write to S0/D0 */, dst_is_double, is_unsigned ? 0 : 1));

  /* Store result to destination */
  store_fp_result_from_vfp(&q->dest, 0 /* S0 */, 0 /* D0 */, dst_is_double);
}

/* Generate float-to-int conversion.
 * Uses VCVT for hard float, library calls for soft float.
 */
static void gen_hardfp_cvt_ftoi(TACQuadruple *q)
{
  int src_is_double = ((q->src1.type.t & VT_BTYPE) != VT_FLOAT);
  int dst_bt = q->dest.type.t & VT_BTYPE;
  int is_unsigned = (q->dest.type.t & VT_UNSIGNED) ? 1 : 0;
  int src_reg;

  /* For LLONG destination, we need library call even in hard float mode */
  if (dst_bt == VT_LLONG)
  {
    const char *func_name;
    if (src_is_double)
    {
      func_name = is_unsigned ? "__aeabi_d2ulz" : "__aeabi_d2lz";
    }
    else
    {
      func_name = is_unsigned ? "__aeabi_f2ulz" : "__aeabi_f2lz";
    }
    // gen_softfp_call(q, func_name, src_is_double);
    return;
  }

  /* Load float/double source - may already be in VFP register */
  src_reg = load_fp_operand_to_vfp(&q->src1, 0 /* S0 */, 0 /* D0 */, src_is_double);

  /* VCVT: convert float/double to int
   * opc2=4 for unsigned, opc2=5 for signed (with round toward zero)
   * Result goes to S0
   */
  ot_check(th_vcvt_fp_int(0, src_is_double ? src_reg / 2 : src_reg, is_unsigned ? 0x4 : 0x5, src_is_double, 1));

  /* Move result from S0 to R0 */
  ot_check(th_vmov_gp_sp(R0, 0 /* S0 */, 1 /* to ARM */));

  store(R0, &q->dest);
}

/* Check if the selected FPU supports double precision operations */
int arm_fpu_supports_double(int fpu_type)
{
  switch (fpu_type)
  {
  case ARM_FPU_FPV4_SP_D16:
  case ARM_FPU_FPV5_SP_D16:
  case ARM_FPU_NONE:
    return 0; /* single-precision-only FPUs or no FPU */
  default:
    return 1; /* FPUs that implement double precision */
  }
}

/* Generate floating point operation.
 * Uses VFP hardware instructions when hard float ABI is enabled,
 * otherwise falls back to software library calls.
 * For single-precision-only FPUs (fpv4-sp-d16, fpv5-sp-d16), double
 * operations fall back to software library calls even in hard float mode.
 */
ST_FUNC void tcc_gen_machine_fp_op(TACQuadruple *q)
{
  int is_double = ((q->src1.type.t & VT_BTYPE) != VT_FLOAT);
  const char *func_name;

  /* Use VFP hardware instructions when hard float ABI is enabled
   * AND the FPU supports the precision (always for float, check for double) */
  int use_vfp =
      (tcc_state->float_abi == ARM_HARD_FLOAT) && (!is_double || arm_fpu_supports_double(tcc_state->fpu_type));

  if (use_vfp)
  {
    if (q->op == TCCIR_OP_FCMP)
    {
      gen_hardfp_cmp(q, is_double);
      return;
    }
    /* For arithmetic ops, use VFP instructions */
    if (q->op == TCCIR_OP_FADD || q->op == TCCIR_OP_FSUB || q->op == TCCIR_OP_FMUL || q->op == TCCIR_OP_FDIV ||
        q->op == TCCIR_OP_FNEG)
    {
      gen_hardfp_op(q, is_double);
      return;
    }
    /* For conversion ops, use VFP instructions */
    if (q->op == TCCIR_OP_CVT_FTOF)
    {
      gen_hardfp_cvt_ftof(q);
      return;
    }
    if (q->op == TCCIR_OP_CVT_ITOF)
    {
      gen_hardfp_cvt_itof(q);
      return;
    }
    if (q->op == TCCIR_OP_CVT_FTOI)
    {
      gen_hardfp_cvt_ftoi(q);
      return;
    }
  }

  /* Fall back to software floating point library calls */
  func_name = get_softfp_func_name(q->op, is_double);

  if (q->op == TCCIR_OP_FNEG)
  {
    /* Negation: XOR the sign bit */
    /* For float: XOR R0 with 0x80000000 */
    /* For double: XOR R1 with 0x80000000 (high word has sign) */
    load(R0, &q->src1);
    /* Load 0x80000000 to R12 using literal pool */
    load_full_const(R12, -1, 0x80000000, NULL);
    if (is_double)
    {
      /* XOR high word (R1) with sign bit */
      ot_check(th_eor_reg(R1, R1, R12, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
    }
    else
    {
      /* XOR R0 with sign bit */
      ot_check(th_eor_reg(R0, R0, R12, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
    }
    store(R0, &q->dest);
    return;
  }

  if (q->op == TCCIR_OP_FCMP)
  {
    /* Comparison: use __aeabi_cfcmple / __aeabi_cdcmple functions
     * These set CPSR flags directly, so subsequent SETIF/JUMPIF works normally.
     * The flags are set as if a CMP instruction was executed:
     *   a < b  -> N=1 (less than)
     *   a == b -> Z=1 (equal)
     *   a > b  -> (no flags, greater than)
     */
    const char *cmp_func = is_double ? "__aeabi_cdcmple" : "__aeabi_cfcmple";
    Sym *sym;
    SValue func_sv;

    /* Load operands into argument registers */
    if (is_double)
    {
      /* Double: src1 in R0:R1, src2 in R2:R3 */
      load(R0, &q->src1);
      load(R2, &q->src2);
    }
    else
    {
      /* Float: src1 in R0, src2 in R1 */
      load(R0, &q->src1);
      load(R1, &q->src2);
    }

    /* Get or create the external symbol for the comparison function */
    sym = external_global_sym(tok_alloc_const(cmp_func), &func_old_type);

    /* Set up SValue for the function call */
    memset(&func_sv, 0, sizeof(SValue));
    func_sv.r = VT_CONST | VT_SYM;
    func_sv.sym = sym;
    func_sv.c.i = 0;

    /* Generate BL to the comparison function */
    gcall_or_jump(0, &func_sv);
    /* Flags are now set - SETIF/JUMPIF will use them */
    return;
  }

  /* Soft float conversion operations */
  if (q->op == TCCIR_OP_CVT_FTOF)
  {
    /* Float to double or double to float */
    int src_is_double = ((q->src1.type.t & VT_BTYPE) != VT_FLOAT);
    int dst_is_double = ((q->dest.type.t & VT_BTYPE) != VT_FLOAT);
    const char *func_name_cvt;

    if (dst_is_double && !src_is_double)
    {
      func_name_cvt = "__aeabi_f2d";
    }
    else if (!dst_is_double && src_is_double)
    {
      func_name_cvt = "__aeabi_d2f";
    }
    else
    {
      /* Same type, no conversion needed - just copy */
      load(R0, &q->src1);
      store(R0, &q->dest);
      return;
    }
    // gen_softfp_call(q, func_name_cvt, src_is_double);
    return;
  }

  if (q->op == TCCIR_OP_CVT_ITOF)
  {
    /* Int to float/double */
    int src_bt = q->src1.type.t & VT_BTYPE;
    int dst_is_double = ((q->dest.type.t & VT_BTYPE) != VT_FLOAT);
    int is_unsigned = (q->src1.type.t & VT_UNSIGNED) ? 1 : 0;
    const char *func_name_cvt;

    if (src_bt == VT_LLONG)
    {
      if (dst_is_double)
      {
        func_name_cvt = is_unsigned ? "__aeabi_ul2d" : "__aeabi_l2d";
      }
      else
      {
        func_name_cvt = is_unsigned ? "__aeabi_ul2f" : "__aeabi_l2f";
      }
    }
    else
    {
      if (dst_is_double)
      {
        func_name_cvt = is_unsigned ? "__aeabi_ui2d" : "__aeabi_i2d";
      }
      else
      {
        func_name_cvt = is_unsigned ? "__aeabi_ui2f" : "__aeabi_i2f";
      }
    }
    // gen_softfp_call(q, func_name_cvt, 0);
    return;
  }

  if (q->op == TCCIR_OP_CVT_FTOI)
  {
    /* Float/double to int */
    int src_is_double = ((q->src1.type.t & VT_BTYPE) != VT_FLOAT);
    int dst_bt = q->dest.type.t & VT_BTYPE;
    int is_unsigned = (q->dest.type.t & VT_UNSIGNED) ? 1 : 0;
    const char *func_name_cvt;

    if (dst_bt == VT_LLONG)
    {
      if (src_is_double)
      {
        func_name_cvt = is_unsigned ? "__aeabi_d2ulz" : "__aeabi_d2lz";
      }
      else
      {
        func_name_cvt = is_unsigned ? "__aeabi_f2ulz" : "__aeabi_f2lz";
      }
    }
    else
    {
      if (src_is_double)
      {
        func_name_cvt = is_unsigned ? "__aeabi_d2uiz" : "__aeabi_d2iz";
      }
      else
      {
        func_name_cvt = is_unsigned ? "__aeabi_f2uiz" : "__aeabi_f2iz";
      }
    }
    // gen_softfp_call(q, func_name_cvt, src_is_double);
    return;
  }

  if (func_name)
  {
    // gen_softfp_call(q, func_name, is_double);
    return;
  }

  tcc_error("compiler_error: unknown FP operation in tcc_gen_machine_fp_op");
}

ST_FUNC void tcc_gen_machine_return_value_op(TACQuadruple *q)
{
  int is_64bit = is_64bit_type(q->src1.type.t);

  /* NOTE: src1 is preloaded to a valid register by generate_code if it was spilled.
   * Just move to return registers R0 (and R1 for 64-bit). */
  if (q->src1.pr0 >= 0)
  {
    load_to_register(R0, q->src1.pr0, &q->src1);
    if (is_64bit && q->src1.pr1 >= 0)
    {
      load_to_register(R1, q->src1.pr1, &q->src1);
    }
    return;
  }

  /* If we get here with invalid pr0, handle constant case */
  SValue dest;
  dest.pr0 = R0;
  dest.pr1 = is_64bit ? R1 : -1;
  return load_to_dest(&dest, &q->src1);
}

void tcc_gen_machine_load_op(TACQuadruple *op)
{
  TRACE("'tcc_gen_machine_load_op'");

  /* NOTE: All spilled dest handling is now done centrally in generate_code.
   * This function just loads from the source address to the destination register. */
  load_to_dest(&op->dest, &op->src1);
}

ST_FUNC void tcc_gen_machine_store_op(TACQuadruple *op)
{
  TRACE("'tcc_gen_machine_store_op'");
  int src_reg;
  /* Check for 64-bit types - include VT_LLONG for soft-float doubles and long
   * long */
  int src_btype = op->src1.type.t & VT_BTYPE;
  int is_64bit = (src_btype == VT_DOUBLE) || (src_btype == VT_LDOUBLE) || (src_btype == VT_LLONG);

  /* NOTE: src1 is preloaded to a valid register by generate_code if it was spilled.
   * Just use pr0 directly. */
  src_reg = op->src1.pr0;
  store(src_reg, &op->dest);
}

ST_FUNC void tcc_gen_machine_prolog(int leaffunc, uint64_t used_registers, int stack_size)
{
  thumb_gen_state.function_argument_count = 0;
  uint16_t registers_to_push = 0;
  int registers_count = 0;

  thumb_gen_state.generating_function = 1;
  thumb_gen_state.code_size = 0;
  /* Clear global symbol cache at function start */
  thumb_gen_state.cached_global_sym = NULL;
  thumb_gen_state.cached_global_reg = -1;

  if (!leaffunc)
  {
    registers_to_push |= (1 << R_LR);
    registers_count++;
  }

  if (stack_size > 0 && !tcc_state->omit_frame_pointer)
  {
    tcc_state->need_frame_pointer = 1;
    registers_to_push |= (1 << R_FP);
    registers_count++;
  }
  else
  {
    tcc_state->need_frame_pointer = 0;
  }

  for (int i = R4; i <= R11; ++i)
  {
    if (tcc_state->text_and_data_separation && i == R9)
      continue;
    if (!tcc_state->omit_frame_pointer && i == R_FP)
      continue;
    if (used_registers & (1ULL << i))
    {
      registers_to_push |= (1 << i);
      registers_count++;
    }
  }
  if (registers_count % 2 != 0)
  {
    registers_to_push |= (1 << R12);
    registers_count++;
  }
  th_sym_t();
  offset_to_args = registers_count * 4;
  if (registers_count > 0)
  {
    ot_check(th_push(registers_to_push));
  }
  pushed_registers = registers_to_push;

  // allocate stack space for local variables
  allocated_stack_size = stack_size;
  if (tcc_state->need_frame_pointer)
  {
    if (!ot(th_add_imm(R_FP, R_SP, 0, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE)))
    {
      // todo mov fp, sp
      // load r12 immediate
      // add fp, sp, r12
      fprintf(stderr, "compiler_error: prolog frame pointer setup failed\n");
      exit(1);
    }
  }
  if (stack_size > 0)
  {
    ot_check(th_sub_sp_imm(R_SP, stack_size, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE));
  }

  /* Move parameters from incoming registers to their allocated locations.
   * For non-leaf functions or parameters that cross calls:
   * - If allocated to callee-saved register: move from R0-R3 to allocated reg
   * - If spilled: store from R0-R3 to stack location
   * For leaf functions with params staying in R0-R3: no move needed */
  TCCIRState *ir = tcc_state->ir;
  if (ir)
  {
    for (int vreg = 0; vreg < ir->next_parameter; ++vreg)
    {
      const int encoded_vreg = (TCCIR_VREG_TYPE_PARAM << 28) | vreg;
      IRLiveInterval *interval = tcc_ir_get_live_interval(ir, encoded_vreg);

      if (!interval)
        continue;

      int incoming_r0 = interval->incoming_reg0;
      int alloc_r0 = interval->allocation.r0;
      int alloc_r1 = interval->allocation.r1;
      int is_64bit = interval->is_double || interval->is_llong;

      /* Skip if parameter came from stack (not in registers) */
      if (incoming_r0 < 0)
        continue;

      /* Check if we need to move/store the parameter */
      if (alloc_r0 == PREG_SPILLED || interval->allocation.offset != 0)
      {
        /* Parameter is spilled - store to stack */
        int stack_offset = interval->allocation.offset;
        if (is_64bit && interval->incoming_reg1 >= 0)
        {
          /* 64-bit: store both registers */
          tcc_gen_machine_store_to_stack(incoming_r0, stack_offset);
          tcc_gen_machine_store_to_stack(interval->incoming_reg1, stack_offset + 4);
        }
        else
        {
          /* 32-bit: store single register */
          tcc_gen_machine_store_to_stack(incoming_r0, stack_offset);
        }
      }
      else if (alloc_r0 >= 0 && alloc_r0 != incoming_r0)
      {
        /* Parameter allocated to different register - move it */
        if (is_64bit && interval->incoming_reg1 >= 0 && alloc_r1 >= 0)
        {
          /* 64-bit: move both registers */
          ot_check(th_mov_reg(alloc_r0, incoming_r0, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT,
                              ENFORCE_ENCODING_NONE, false));
          ot_check(th_mov_reg(alloc_r1, interval->incoming_reg1, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT,
                              ENFORCE_ENCODING_NONE, false));
        }
        else
        {
          /* 32-bit: move single register */
          ot_check(th_mov_reg(alloc_r0, incoming_r0, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT,
                              ENFORCE_ENCODING_NONE, false));
        }
      }
      /* If alloc_r0 == incoming_r0, parameter stays where it is - no move needed */
    }
  }
}

ST_FUNC void tcc_gen_machine_epilog(int leaffunc)
{
  TRACE("'tcc_gen_machine_epilog'");
  int lr_saved = pushed_registers & (1 << R_LR);

  // restore stack pointer
  if (tcc_state->need_frame_pointer)
  {
    // restore SP from frame pointer
    ot_check(th_mov_reg(R_SP, R_FP, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE, false));
  }
  else if (allocated_stack_size > 0)
  {
    // deallocate stack space for local variables
    ot_check(th_add_sp_imm(R_SP, allocated_stack_size, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE));
  }

  thumb_gen_state.generating_function = 0;
  if (lr_saved)
  {
    pushed_registers |= 1 << R_PC;
    pushed_registers &= ~(1 << R_LR);
    ot_check(th_pop(pushed_registers));
    th_literal_pool_generate();
    thumb_gen_state.generating_function = 0;

    return;
  }
  if (pushed_registers > 0)
  {
    ot_check(th_pop(pushed_registers));
  }
  ot_check(th_bx_reg(R_LR));
  th_literal_pool_generate();
}

ST_FUNC void tcc_gen_machine_assign_op(TACQuadruple *op)
{
  /* Only consider VFP registers if hard float ABI is enabled */
  int use_vfp_regs = (tcc_state->float_abi == ARM_HARD_FLOAT);
  int dest_is_vfp = use_vfp_regs && LS_IS_VFP_REG(op->dest.pr0);
  int src_is_vfp = use_vfp_regs && LS_IS_VFP_REG(op->src1.pr0);
  /* Check both dest and src1 types for 64-bit detection - includes double,
   * ldouble, and llong. Dest may not have proper type info when assigning
   * from a 64-bit source */
  int dest_btype = op->dest.type.t & VT_BTYPE;
  int src_btype = op->src1.type.t & VT_BTYPE;
  int is_64bit = (dest_btype == VT_DOUBLE) || (dest_btype == VT_LDOUBLE) || (dest_btype == VT_LLONG) ||
                 (src_btype == VT_DOUBLE) || (src_btype == VT_LDOUBLE) || (src_btype == VT_LLONG);
  int dest_is_local = (op->dest.r & VT_VALMASK) == VT_LOCAL;

  TRACE("ASSIGN DEBUG: dest.vr=%d, dest.r=0x%x (VT_LOCAL=%d), dest.pr0=%d, src1.pr0=%d", op->dest.vr, op->dest.r,
        dest_is_local, op->dest.pr0, op->src1.pr0);

  /* NOTE: Spilled destination handling is now done centrally in generate_code
   * via tcc_ir_storeback_spill. src1 is also preloaded if it was spilled. */

  if ((op->src1.r & VT_VALMASK) == VT_CONST)
  {
    if (dest_is_vfp)
    {
      int dn = LS_VFP_REG_NUM(op->dest.pr0);
      /* Load constant to integer register, then move to VFP */
      load(R12, &op->src1);
      ot_check(th_vmov_gp_sp(R12, dn, 0)); /* VMOV Sn, r12 */
    }
    else
    {
      load_to_dest(&op->dest, &op->src1);
    }
    return;
  }

  if (op->dest.pr0 == op->src1.pr0)
    return;

  /* Register to register move */
  if (dest_is_vfp && src_is_vfp)
  {
    int dn = LS_VFP_REG_NUM(op->dest.pr0);
    int sn = LS_VFP_REG_NUM(op->src1.pr0);
    /* VFP to VFP move */
    ot_check(th_vmov_register(dn, sn, 0)); /* VMOV.F32 Sd, Sm */
  }
  else if (dest_is_vfp && !src_is_vfp)
  {
    int dn = LS_VFP_REG_NUM(op->dest.pr0);
    /* Integer to VFP */
    ot_check(th_vmov_gp_sp(op->src1.pr0, dn, 0)); /* VMOV Sn, Rm */
  }
  else if (!dest_is_vfp && src_is_vfp)
  {
    int sn = LS_VFP_REG_NUM(op->src1.pr0);
    /* VFP to integer */
    ot_check(th_vmov_gp_sp(op->dest.pr0, sn, 1)); /* VMOV Rd, Sn */
  }
  else
  {
    /* Integer to integer */
    ot_check(th_mov_reg(op->dest.pr0, op->src1.pr0, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT,
                        ENFORCE_ENCODING_NONE, false));
  }

  /* BUGFIX: For VT_LOCAL destinations, we need to write back to memory even if
   * a register was allocated. The register serves as a cache, but the canonical
   * location is still on the stack. This is critical for compound assignments
   * like `index += 1` where the incremented value must be stored back. */
  if (dest_is_local && op->dest.pr0 >= 0)
  {
    TRACE("ASSIGN: Writing back local variable from R%d to memory at offset %d", op->dest.pr0, op->dest.c.i);
    store(op->dest.pr0, &op->dest);
  }
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

ST_FUNC int tcc_gen_machine_number_of_registers(void)
{
  return 11;
}

ST_FUNC void tcc_gen_machine_load_register(SValue *sv)
{
  load(sv->pr0, sv);
}

ST_FUNC void tcc_gen_machine_store_register(SValue *sv)
{
  store(sv->pr0, sv);
}

/* Store a register to a stack slot relative to FP.
 * offset is typically negative (local variables below FP). */
ST_FUNC void tcc_gen_machine_store_to_stack(int reg, int offset)
{
  int sign = (offset < 0);
  int abs_offset = sign ? -offset : offset;

  /* Try direct STR with immediate offset */
  if (!store_word_to_base(reg, R_FP, abs_offset, sign))
  {
    /* Offset too large, use scratch register */
    int rr = th_offset_to_reg(abs_offset, sign);
    ot_check(th_str_reg(reg, R_FP, rr, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
  }
}

ST_FUNC void tcc_gen_machine_func_param_op(TACQuadruple *q, int param_num, int instruction_index)
{
  /* Params are now collected at CALL time via backward scan.
   * This function is kept for compatibility but does nothing. */
  (void)q;
  (void)param_num;
  (void)instruction_index;
}

static void gcall_or_jump(int is_jmp, SValue *dest)
{
  if ((dest->r & (VT_VALMASK | VT_LVAL)) == VT_CONST)
  {
    uint32_t x = th_encbranch(ind, ind + dest->c.i);

    TRACE("gcall_or_jmp: %d, ind: 0x%x, 0x%x", is_jmp, ind, x);
    if (x)
    {
      if (dest->r & VT_SYM)
        greloc(cur_text_section, dest->sym, ind, R_ARM_THM_JUMP24);
      ot_check(th_bl_t1(x));
    }
  }
  else
  {
    load(R12, dest);
    if (!is_jmp)
      ot_check(th_blx_reg(intr(R12)));
    else
      ot_check(th_bx_reg(intr(R12)));
  }
}

/* Helper to check if a type is 64-bit (double or long long) */
static int is_64bit_type(int t)
{
  int bt = t & VT_BTYPE;
  return (bt == VT_DOUBLE || bt == VT_LDOUBLE || bt == VT_LLONG);
}

static void load_to_register(int reg, int reg_from, SValue *sv)
{
  if ((sv->r & VT_VALMASK) == VT_LOCAL)
  {
    if (sv->pr0 >= 0 && !(sv->pr0 & PREG_SPILLED))
    {
      // load local variable from register
      if (reg != reg_from)
      {
        ot_check(th_mov_reg(reg, reg_from, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE,
                            false));
      }
      return;
    }
    if (sv->pr0 == -1 || (sv->pr0 & PREG_SPILLED))
    {
      /* Spilled local variable - load from stack */
      load(reg, sv);
      return;
    }
    if (sv->r & VT_LVAL)
    {
      /* Lvalue: need to load from memory */
      load(reg, sv);
      return;
    }
  }

  if ((sv->r & VT_LVAL) || sv->pr0 == -1 || (sv->pr0 & PREG_SPILLED))
  {
    /* Lvalue: need to load from memory */
    load(reg, sv);
    return;
  }

  /* Value is in a valid register - move it */
  if (reg != sv->pr0)
  {
    ot_check(
        th_mov_reg(reg, sv->pr0, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE, false));
  }
}

/* Returns true when a register value can be used directly as a source (not spilled, not lvalue). */
static bool is_valid_src_reg(const SValue *sv, int reg)
{
  return reg >= 0 && !(reg & PREG_SPILLED) && !(sv->r & VT_LVAL);
}

static int lowest_set_bit(uint32_t mask)
{
  return __builtin_ctz(mask);
}

/* Preserve a register that will be clobbered by later parameter writes. We try to remap it to R12. */
static void remap_future_param_sources(int current_dest, int reg_to_save, int remap_reg, int *param_src0,
                                       int *param_src1, int *op_to_reg)
{
  /* Walk future params (those with lower destination registers) and rewrite their sources. */
  for (int dest = current_dest - 1; dest >= 0; --dest)
  {
    if (op_to_reg[dest] == -1)
      continue;
    if (param_src0[dest] == reg_to_save)
      param_src0[dest] = remap_reg;
    if (param_src1[dest] == reg_to_save)
      param_src1[dest] = remap_reg;
  }
}

ST_FUNC void tcc_gen_machine_func_call_op(TACQuadruple *q, int drop_result, TCCIRState *ir, int call_idx)
{
  /* Scan backward from the CALL to find its params.
   * This handles nested calls naturally: inner calls consume their params
   * before outer calls see them.
   */
  int param_indices_size = 4; /* Initial size, grows as needed */
  int *param_indices = tcc_malloc(sizeof(int) * param_indices_size);
  int param_count = 0;
  uint32_t params_found = 0; /* Bitmask of which param numbers we've claimed */
  int nested_call_depth = 0; /* Track nested calls to skip their params */

  /* Backward scan to find params for THIS call */
  for (int i = call_idx - 1; i >= 0; i--)
  {
    TACQuadruple *instr = &ir->instructions[i];

    if (instr->op == TCCIR_OP_FUNCCALLVAL || instr->op == TCCIR_OP_FUNCCALLVOID)
    {
      /* Hit another call - its params are between it and the previous call */
      nested_call_depth++;
    }
    else if (instr->op == TCCIR_OP_FUNCPARAMVAL)
    {
      if (nested_call_depth > 0)
      {
        /* This param belongs to a nested (inner) call, not us */
        int param_num = instr->src2.c.i;
        if (param_num == 1)
          nested_call_depth--; /* Inner call got all its params */
      }
      else
      {
        /* This param might belong to us */
        int param_num = instr->src2.c.i;
        if (!(params_found & (1 << param_num)))
        {
          /* We haven't claimed this param number yet */
          params_found |= (1 << param_num);

          /* Grow array if needed */
          if (param_count >= param_indices_size)
          {
            param_indices_size *= 2;
            param_indices = tcc_realloc(param_indices, sizeof(int) * param_indices_size);
          }
          param_indices[param_count++] = i;

          if (param_num == 0)
            break; /* Param 0 is the first, we're done */
        }
      }
    }
    else if (instr->op == TCCIR_OP_FUNCPARAMVOID)
    {
      if (nested_call_depth > 0)
        nested_call_depth--;
      else
        break; /* No-arg call marker for us, done */
    }
  }

  /* Sort param_indices by their actual parameter number (src2.c.i).
   * The backward scan collects them in arbitrary order, but we need them
   * in ascending parameter order (1, 2, 3, ...) for correct argument passing. */
  for (int i = 0; i < param_count - 1; i++)
  {
    for (int j = i + 1; j < param_count; j++)
    {
      int param_num_i = ir->instructions[param_indices[i]].src2.c.i;
      int param_num_j = ir->instructions[param_indices[j]].src2.c.i;
      if (param_num_i > param_num_j)
      {
        /* Swap to put lower param number first */
        int tmp = param_indices[i];
        param_indices[i] = param_indices[j];
        param_indices[j] = tmp;
      }
    }
  }

  /* First pass: calculate register and stack slot assignments for each argument
   * following AAPCS rules:
   * - 32-bit args go in R0-R3 then stack
   * - 64-bit args go in R0:R1 or R2:R3 (must be even-aligned), then stack
   * - 64-bit args on stack must be 8-byte aligned
   */
  int next_reg = 0;   /* Next available register (0-3) */
  int stack_size = 0; /* Current stack offset */
  uint32_t register_map = 0;
  int op_to_reg[4] = {-1, -1, -1, -1};
  int stack_offset = 0;

  // only r0-r3 for arguments, rest arguments go to stack
  for (int i = 0; i < param_count; ++i)
  {
    const int argument_index = param_indices[i];
    TACQuadruple *arg = &ir->instructions[argument_index];
    const int is_64bit = is_64bit_type(arg->src1.type.t);
    if (is_64bit)
    {
      /* 64-bit value needs even-aligned register pair */
      if (next_reg & 1)
        next_reg++; /* Align to even register */
      if (next_reg <= 2)
      {
        /* Fits in registers (R0:R1 or R2:R3) */
        register_map |= (1 << next_reg) | (1 << (next_reg + 1));
        op_to_reg[next_reg] = i;
        op_to_reg[next_reg + 1] = i;
        next_reg += 2;
      }
      else
      {
        /* Goes on stack, 8-byte aligned */
        stack_size = TCC_ALIGN(stack_size, 8);
        stack_size += 8;
      }
    }
    else
    {
      /* 32-bit value */
      if (next_reg <= 3)
      {
        register_map |= (1 << next_reg);
        op_to_reg[next_reg] = i;
        next_reg++;
      }
      else
      {
        stack_size += 4;
      }
    }
  }

  /* Align total stack to 8 bytes as required by AAPCS */
  stack_size = TCC_ALIGN(stack_size, 8);

  /* Reserve stack space for arguments if needed */
  if (stack_size > 0)
  {
    ot_check(th_sub_sp_imm(R_SP, stack_size, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE));
  }

  /* Push stack arguments first */
  for (int i = 0; i < param_count; ++i)
  {
    // check if argument goes to register
    int assigned_register = -1;
    int is_64bit = 0;
    const int argument_index = param_indices[i];
    TACQuadruple *arg = &ir->instructions[argument_index];

    for (int j = 0; j < 4; j++)
    {
      if (op_to_reg[j] == -1)
        continue;
      if (op_to_reg[j] == i)
      {
        assigned_register = j;
        break;
      }
    }
    if (assigned_register != -1)
    {
      continue;
    }

    is_64bit = is_64bit_type(arg->src1.type.t);
    if (is_64bit)
    {
      // store(R_SP, &arg->src1);
    }
    else
    {
      int reg = arg->src1.pr0;
      if (reg == -1 || (reg & PREG_SPILLED) || (arg->src1.r & VT_LVAL))
      {
        load(R12, &arg->src1);
        reg = R12;
      }
      ot_check(th_str_imm(reg, R_SP, stack_offset, 6, ENFORCE_ENCODING_NONE));
      stack_offset += 4;
    }
  }

  /* Pre-compute register sources for each register-assigned argument so we can spot conflicts. */
  int param_src0[4] = {-1, -1, -1, -1};
  int param_src1[4] = {-1, -1, -1, -1};
  uint32_t future_src_mask = 0;
  for (int dest = 0; dest < 4; ++dest)
  {
    if (op_to_reg[dest] == -1)
      continue;
    TACQuadruple *arg = &ir->instructions[param_indices[op_to_reg[dest]]];
    const int is_64bit = is_64bit_type(arg->src1.type.t);
    if (is_valid_src_reg(&arg->src1, arg->src1.pr0))
    {
      param_src0[dest] = arg->src1.pr0;
      /* Don't add to conflict mask if src==dest (no real conflict, just loading from self) */
      if (arg->src1.pr0 != dest)
        future_src_mask |= (1u << arg->src1.pr0);
    }
    if (is_64bit && is_valid_src_reg(&arg->src1, arg->src1.pr1))
    {
      param_src1[dest] = arg->src1.pr1;
      /* Don't add to conflict mask if src==dest */
      if (arg->src1.pr1 != dest)
        future_src_mask |= (1u << arg->src1.pr1);
    }
  }

  /* Load register arguments in descending order (R3 → R2 → R1 → R0),
   * but detect when writing to a destination register would clobber a still-needed source.
   * Since we load R3→R2→R1→R0, we need to check if a destination will clobber a source
   * needed by LOWER-numbered registers (which haven't been loaded yet). */
  int registers_to_push = 0;

  /* Compute sources needed by each lower register before we start loading */
  uint32_t sources_needed_by_lower[4] = {0, 0, 0, 0};
  for (int dest = 0; dest < 4; ++dest)
  {
    for (int lower = 0; lower < dest; ++lower)
    {
      if (param_src0[lower] != -1)
        sources_needed_by_lower[dest] |= (1u << param_src0[lower]);
      if (param_src1[lower] != -1)
        sources_needed_by_lower[dest] |= (1u << param_src1[lower]);
    }
  }

  for (int i = 3; i >= 0; --i)
  {
    if (op_to_reg[i] == -1)
      continue;
    TACQuadruple *arg = &ir->instructions[param_indices[op_to_reg[i]]];
    SValue arg_copy = arg->src1; /* We may rewrite pr0/pr1 if we remap sources. */

    const int is_64bit = is_64bit_type(arg_copy.type.t);
    const int dest_reg = i;
    const uint32_t dest_mask = is_64bit ? ((1u << dest_reg) | (1u << (dest_reg - 1))) : (1u << dest_reg);

    /* Check if writing to dest_reg would clobber a source needed by lower registers */
    const uint32_t conflict_mask = dest_mask & sources_needed_by_lower[dest_reg];
    if (conflict_mask)
    {
      const int reg_to_save = lowest_set_bit(conflict_mask); /* pick lowest conflicting register */
      const bool r12_busy = sources_needed_by_lower[dest_reg] & (1u << R_IP);
      if (!r12_busy && !(dest_mask & (1u << R_IP)))
      {
        ot_check(th_mov_reg(R_IP, reg_to_save, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT,
                            ENFORCE_ENCODING_NONE, false));
        remap_future_param_sources(dest_reg, reg_to_save, R_IP, param_src0, param_src1, op_to_reg);
      }
      else
      {
        /* No safe scratch register; skip remapping (rare). */
      }
    }

    /* Apply any remapping for this argument. */
    if (param_src0[dest_reg] != -1)
      arg_copy.pr0 = param_src0[dest_reg];
    if (is_64bit && param_src1[dest_reg] != -1)
      arg_copy.pr1 = param_src1[dest_reg];

    if (is_64bit)
    {
      /* 64-bit values use register pairs (R0:R1 or R2:R3) */
      SValue dest;
      dest.pr0 = dest_reg - 1;
      dest.pr1 = dest_reg;
      --i; /* Skip the lower register of the pair in next iteration */

      if (arg_copy.pr0 == -1 && arg_copy.pr1 == -1)
      {
        /* Load from memory/constant */
        load_to_dest(&dest, &arg_copy);
      }
      else
      {
        /* Move from source register pair */
        load_to_register(dest_reg - 1, arg_copy.pr0, &arg_copy);
        load_to_register(dest_reg, arg_copy.pr1, &arg_copy);
      }
    }
    else
    {
      /* 32-bit value - load directly to destination register */
      load_to_register(dest_reg, arg_copy.pr0, &arg_copy);
    }
  }

  if (tcc_state->text_and_data_separation && q->src1.type.t & VT_EXTERN)
  {
    registers_to_push |= (1 << R9 || 1 << R8);
  }

  if (registers_to_push != 0)
  {
    ot_check(th_push(registers_to_push));
  }
  gcall_or_jump(0, &q->src1);

  /* Invalidate global symbol cache after function call.
   * All caller-saved registers (R0-R3, R12, LR) are clobbered by the call,
   * so any cached global address in those registers is now invalid. */
  if (th_is_caller_saved_register(thumb_gen_state.cached_global_reg))
  {
    thumb_gen_state.cached_global_sym = NULL;
    thumb_gen_state.cached_global_reg = -1;
  }
  if (registers_to_push != 0)
  {
    ot_check(th_pop(registers_to_push));
  }

  /* Clean up stack space used for arguments */
  if (stack_size > 0)
  {
    ot_check(th_add_sp_imm(R_SP, stack_size, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE));
  }

  if (drop_result)
  {
    tcc_free(param_indices);
    return;
  }

  /* Handle the return value - move from R0 (and R1 for 64-bit) to destination */
  int dest_spilled = (q->dest.pr0 == -1) || (q->dest.pr0 & PREG_SPILLED);

  if (dest_spilled)
  {
    /* Result goes to stack */
    int offset = q->dest.c.i;
    int puw = (offset >= 0) ? 6 : 4; /* puw=6 for positive, puw=4 for negative */
    int abs_offset = (offset >= 0) ? offset : -offset;

    if (tcc_is_64bit_operand(&q->dest))
    {
      /* Store 64-bit result: R0 to low word, R1 to high word */
      ot_check(th_str_imm(R0, R_FP, abs_offset, puw, ENFORCE_ENCODING_NONE));
      /* High word is at offset+4 for positive, offset-4 (closer to FP) for negative */
      int high_offset = (offset >= 0) ? abs_offset + 4 : abs_offset - 4;
      int high_puw = (offset >= 0) ? 6 : 4;
      if (high_offset < 0)
      {
        high_offset = -high_offset;
        high_puw = 6;
      }
      ot_check(th_str_imm(R1, R_FP, high_offset, high_puw, ENFORCE_ENCODING_NONE));
    }
    else
    {
      /* Store 32-bit result */
      ot_check(th_str_imm(R0, R_FP, abs_offset, puw, ENFORCE_ENCODING_NONE));
    }
  }
  else
  {
    /* Result goes to register(s) */
    if (tcc_is_64bit_operand(&q->dest) && q->dest.pr1 != R1)
    {
      ot_check(th_mov_reg(q->dest.pr1, R1, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE,
                          false));
    }

    if (q->dest.pr0 != R0)
    {
      ot_check(th_mov_reg(q->dest.pr0, R0, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE,
                          false));
    }
  }

  tcc_free(param_indices);
}

ST_FUNC void tcc_gen_machine_jump_op(TACQuadruple *q)
{
  ot_check(th_b_t4(0)); // patch me later
}

ST_FUNC void tcc_gen_machine_conditional_jump_op(TACQuadruple *q)
{
  int op = mapcc(q->src1.c.i);
  ot_check(th_b_t3(op, 0)); // patch me later
}

ST_FUNC void tcc_gen_machine_setif_op(TACQuadruple *q)
{
  /* Convert comparison flags to 0/1 value in destination register
   * Using ITE (If-Then-Else) block for smaller code:
   *   ITE <cond>          ; If-Then-Else for condition
   *   MOV<cond> Rd, #1    ; set to 1 if condition true
   *   MOV<!cond> Rd, #0   ; set to 0 if condition false
   */
  int op = mapcc(q->src1.c.i);
  int dest = q->dest.pr0;

  /* NOTE: Destination is preloaded to a valid register by generate_code if spilled.
   * Just use it directly. Store-back is also handled centrally. */

  /* ITE instruction: mask = 0x4 for ITE pattern (Then followed by Else)
   * The mask encoding: bit 3 = first instr matches cond (1=T)
   *                    bit 2 = second instr matches cond (0=E)
   *                    bit 1 = 0 (end of block)
   * For ITE: mask = 0b0100 = 0x4  (T, then E, then end)
   */
  /* For EQ the mask 0x4 encodes ITT (both Then). Use 0xC to get ITE. */
  uint16_t it_mask = (op == 0 /* EQ */) ? 0xC : 0x4;
  ot_check(th_it(op, it_mask)); /* ITE: Then, Else */

  /* Conditional MOV to 1 if condition true */
  ot_check(th_mov_imm(dest, 1, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE));

  /* Conditional MOV to 0 if condition false (the Else part) */
  ot_check(th_mov_imm(dest, 0, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE));
}

ST_FUNC void tcc_gen_machine_bool_op(TACQuadruple *q)
{
  /* Optimized boolean OR/AND operations:
   * For BOOL_OR (x || y):
   *   ORRS Rd, Rsrc1, Rsrc2   ; Rd = src1 | src2, sets Z flag
   *   ITE ne
   *   MOVNE Rd, #1            ; if result non-zero, set to 1
   *   MOVEQ Rd, #0            ; if result zero, set to 0
   *
   * For BOOL_AND (x && y):
   *   CMP Rsrc1, #0           ; check if src1 is zero
   *   IT eq
   *   CMPEQ Rsrc2, #0         ; if src1 == 0, force EQ (compare 0 with anything)
   *   Actually... use CBZ or simpler approach:
   *
   *   Better for AND:
   *   SUBS temp, src1, #0    ; temp = src1, sets Z if src1==0, preserves NE if src1!=0
   *   IT ne
   *   SUBSNE temp, src2, #0  ; if src1!=0, check src2 - sets NE if src2!=0
   *   ITE ne
   *   MOVNE dest, #1
   *   MOVEQ dest, #0
   */
  int dest = q->dest.pr0;
  int src1 = q->src1.pr0;
  int src2 = q->src2.pr0;

  if (q->op == TCCIR_OP_BOOL_OR)
  {
    /* ORRS sets flags based on result */
    ot_check(th_orr_reg(dest, src1, src2, FLAGS_BEHAVIOUR_SET, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));

    /* ITE ne: if result != 0, dest = 1, else dest = 0 */
    ot_check(th_it(0x1, 0x4)); /* ITE NE (condition code 0x1 = NE) */
    ot_check(th_mov_imm(dest, 1, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE));
    ot_check(th_mov_imm(dest, 0, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE));
  }
  else /* TCCIR_OP_BOOL_AND */
  {
    /* For AND: (src1 != 0) && (src2 != 0)
     * Use: CMP + IT + CMP sequence
     *   CMP src1, #0           ; Z=1 if src1==0
     *   IT ne                  ; only execute next if src1 != 0
     *   CMPNE src2, #0         ; Z=1 if src2==0 (only if src1!=0)
     *   ; Now: Z=0 (NE) only if both src1!=0 AND src2!=0
     *   ITE ne
     *   MOVNE dest, #1
     *   MOVEQ dest, #0
     */
    ot_check(th_cmp_imm(0, src1, 0, FLAGS_BEHAVIOUR_SET, ENFORCE_ENCODING_NONE));
    ot_check(th_it(0x1, 0x8)); /* IT NE (single instruction) */
    ot_check(th_cmp_imm(0, src2, 0, FLAGS_BEHAVIOUR_SET, ENFORCE_ENCODING_NONE));
    /* Now flags reflect: NE if both non-zero, EQ if either zero */
    ot_check(th_it(0x1, 0x4)); /* ITE NE */
    ot_check(th_mov_imm(dest, 1, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE));
    ot_check(th_mov_imm(dest, 0, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE));
  }
}

ST_FUNC void tcc_gen_machine_backpatch_jump(int address, int offset)
{
  th_patch_call(address, offset);
}

static int tcc_get_type_size(CType *type)
{
  switch (type->t & VT_BTYPE)
  {
  case VT_BYTE:
    return 1;
  case VT_SHORT:
    return 2;
  case VT_INT:
  case VT_LONG:
    return 4;
  case VT_LLONG:
    return 8;
  case VT_FLOAT:
    return 4;
  case VT_DOUBLE:
    return 8;
  case VT_LDOUBLE:
    return 8; // treat long double as double for ARM EABI softcalls
  default:
    return 0;
  }
}

ST_FUNC const char *tcc_get_abi_softcall_name(TACQuadruple *q)
{
  const int src1_64bit = tcc_is_64bit_operand(&q->src1);
  const int src2_64bit = tcc_is_64bit_operand(&q->src2);
  const int dest_64bit = tcc_is_64bit_operand(&q->dest);
  const int src1_size = tcc_get_type_size(&q->src1.type);
  const int dest_size = tcc_get_type_size(&q->dest.type);

  if (src1_64bit || src2_64bit || dest_64bit)
  {
    switch (q->op)
    {
    case TCCIR_OP_FADD:
      return "__aeabi_dadd";
    case TCCIR_OP_FSUB:
      return "__aeabi_dsub";
    case TCCIR_OP_FMUL:
      return "__aeabi_dmul";
    case TCCIR_OP_FDIV:
      return "__aeabi_ddiv";
    case TCCIR_OP_FNEG:
      return "__aeabi_dneg";
    }
  }
  else
  {
    switch (q->op)
    {
    case TCCIR_OP_FADD:
      return "__aeabi_fadd";
    case TCCIR_OP_FSUB:
      return "__aeabi_fsub";
    case TCCIR_OP_FMUL:
      return "__aeabi_fmul";
    case TCCIR_OP_FDIV:
      return "__aeabi_fdiv";
    case TCCIR_OP_FNEG:
      return "__aeabi_fneg";
    }
  }

  switch (q->op)
  {
  case TCCIR_OP_CVT_FTOF:
  {
    if (src1_size == 4 && dest_size == 8)
    {
      return "__aeabi_f2d";
    }
    else if (src1_size == 8 && dest_size == 4)
    {
      return "__aeabi_d2f";
    }
    /* Same size conversion is a no-op, no function needed */
    return NULL;
  }
  break;
  case TCCIR_OP_CVT_FTOI:
  {
    int is_float = ((q->src1.type.t & VT_BTYPE) == VT_FLOAT);
    switch (q->dest.type.t & VT_BTYPE)
    {
    case VT_SHORT:
      return is_float ? "__aeabi_f2h" : "__aeabi_d2h";
    case VT_INT:
      if (q->dest.type.t & VT_UNSIGNED)
        return is_float ? "__aeabi_f2uiz" : "__aeabi_d2uiz";
    case VT_LONG:
    {
      if (q->dest.type.t & VT_UNSIGNED)
        return is_float ? "__aeabi_f2ulz" : "__aeabi_d2ulz";
      return is_float ? "__aeabi_f2lz" : "__aeabi_d2lz";
    }
    }
  }
  break;
  case TCCIR_OP_FCMP:
  {
    /* Get comparison operation from src2.c.i (stored during IR generation) */
    int cmp_op = q->src2.c.i;
    int is_float = (src1_size == 4);

    switch (cmp_op)
    {
    case TOK_EQ:
      return is_float ? "__aeabi_fcmpeq" : "__aeabi_dcmpeq";
    case TOK_NE:
      /* NE uses cmpeq and inverts the result */
      return is_float ? "__aeabi_fcmpeq" : "__aeabi_dcmpeq";
    case TOK_LT:
    case TOK_ULT:
      return is_float ? "__aeabi_fcmplt" : "__aeabi_dcmplt";
    case TOK_LE:
    case TOK_ULE:
      return is_float ? "__aeabi_fcmple" : "__aeabi_dcmple";
    case TOK_GT:
    case TOK_UGT:
      return is_float ? "__aeabi_fcmpgt" : "__aeabi_dcmpgt";
    case TOK_GE:
    case TOK_UGE:
      return is_float ? "__aeabi_fcmpge" : "__aeabi_dcmpge";
    default:
      /* Fallback to cfcmple/cdcmple which sets flags */
      return is_float ? "__aeabi_cfcmple" : "__aeabi_cdcmple";
    }
  }
  break;
  case TCCIR_OP_CVT_ITOF:
  {
    /* Integer to double */
    if (q->src1.type.t & VT_UNSIGNED)
      return dest_64bit ? "__aeabi_ui2d" : "__aeabi_ui2f";
    return dest_64bit ? "__aeabi_i2d" : "__aeabi_i2f";
  }
  break;
  }

  return NULL;
}

#endif // TARGET_DEFS_ONLY
