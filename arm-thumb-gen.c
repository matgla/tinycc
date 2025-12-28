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

typedef struct ThumbLiteralPoolEntry {
  Sym *sym;
  int relocation;
  int patch_position;
  int short_instruction;
  int32_t imm;
  int shared_index; /* Index of earlier entry with same value, or -1 if unique
                     */
} ThumbLiteralPoolEntry;

/* Saved call context for nested function calls */
typedef struct SavedCallContext {
  TACQuadruple *arguments;
  int argument_count;
  int arguments_capacity;
} SavedCallContext;

typedef struct ThumbGeneratorState {
  uint8_t generating_function : 1;
  int code_size;
  ThumbLiteralPoolEntry *literal_pool;
  int literal_pool_size;
  int literal_pool_count;
  /* Cache for global symbol base address to avoid redundant loads */
  Sym *cached_global_sym; /* Last loaded global symbol */
  int cached_global_reg;  /* Register holding its base address */
  /* Function call arguments */
  TACQuadruple *function_arguments;
  int function_argument_count;
  int function_arguments_capacity;
  /* Stack for nested function calls */
  SavedCallContext *saved_call_contexts;
  int nested_call_depth;
  int saved_call_contexts_capacity;
} ThumbGeneratorState;

ThumbGeneratorState thumb_gen_state;

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

/* Forward declarations */
static int is_64bit_type(int t);

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

static void ensure_function_arguments_capacity(int needed) {
  if (needed > thumb_gen_state.function_arguments_capacity) {
    int new_capacity = thumb_gen_state.function_arguments_capacity * 2;
    if (new_capacity < needed)
      new_capacity = needed;
    if (new_capacity < 8)
      new_capacity = 8;
    thumb_gen_state.function_arguments =
        tcc_realloc(thumb_gen_state.function_arguments,
                    new_capacity * sizeof(TACQuadruple));
    thumb_gen_state.function_arguments_capacity = new_capacity;
  }
}

static void ensure_saved_contexts_capacity(int needed) {
  if (needed > thumb_gen_state.saved_call_contexts_capacity) {
    int new_capacity = thumb_gen_state.saved_call_contexts_capacity * 2;
    if (new_capacity < needed)
      new_capacity = needed;
    if (new_capacity < 4)
      new_capacity = 4;
    thumb_gen_state.saved_call_contexts =
        tcc_realloc(thumb_gen_state.saved_call_contexts,
                    new_capacity * sizeof(SavedCallContext));
    /* Initialize new entries */
    for (int i = thumb_gen_state.saved_call_contexts_capacity; i < new_capacity;
         i++) {
      thumb_gen_state.saved_call_contexts[i].arguments = NULL;
      thumb_gen_state.saved_call_contexts[i].argument_count = 0;
      thumb_gen_state.saved_call_contexts[i].arguments_capacity = 0;
    }
    thumb_gen_state.saved_call_contexts_capacity = new_capacity;
  }
}

ST_FUNC void tcc_gen_machine_save_call_context(void) {
  ensure_saved_contexts_capacity(thumb_gen_state.nested_call_depth + 1);
  SavedCallContext *ctx =
      &thumb_gen_state.saved_call_contexts[thumb_gen_state.nested_call_depth];

  /* Ensure saved context has enough capacity */
  if (thumb_gen_state.function_argument_count > ctx->arguments_capacity) {
    int new_cap = thumb_gen_state.function_argument_count;
    if (new_cap < 8)
      new_cap = 8;
    ctx->arguments =
        tcc_realloc(ctx->arguments, new_cap * sizeof(TACQuadruple));
    ctx->arguments_capacity = new_cap;
  }

  memcpy(ctx->arguments, thumb_gen_state.function_arguments,
         thumb_gen_state.function_argument_count * sizeof(TACQuadruple));
  ctx->argument_count = thumb_gen_state.function_argument_count;
  thumb_gen_state.nested_call_depth++;
  thumb_gen_state.function_argument_count = 0;
}

ST_FUNC void tcc_gen_machine_restore_call_context(void) {
  if (thumb_gen_state.nested_call_depth > 0) {
    thumb_gen_state.nested_call_depth--;
    SavedCallContext *ctx =
        &thumb_gen_state.saved_call_contexts[thumb_gen_state.nested_call_depth];

    ensure_function_arguments_capacity(ctx->argument_count);
    memcpy(thumb_gen_state.function_arguments, ctx->arguments,
           ctx->argument_count * sizeof(TACQuadruple));
    thumb_gen_state.function_argument_count = ctx->argument_count;
  }
}

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
  static int ot_check_counter = 0;
  ot_check_counter++;
  if (!is_valid_opcode(op)) {
    fprintf(stderr, "DEBUG ot_check #%d: invalid opcode size=%d opcode=0x%x\n",
            ot_check_counter, op.size, op.opcode);
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

static void th_literal_pool_init() {
  thumb_gen_state.literal_pool_size = 64;
  thumb_gen_state.literal_pool_count = 0;
  thumb_gen_state.literal_pool = tcc_malloc(sizeof(ThumbLiteralPoolEntry) *
                                            thumb_gen_state.literal_pool_size);
  thumb_gen_state.generating_function = 0;
  thumb_gen_state.code_size = 0;
  thumb_gen_state.cached_global_sym = NULL;
  thumb_gen_state.cached_global_reg = -1;

  /* Initialize function arguments dynamic arrays */
  thumb_gen_state.function_arguments = NULL;
  thumb_gen_state.function_argument_count = 0;
  thumb_gen_state.function_arguments_capacity = 0;

  /* Initialize saved call contexts for nested calls */
  thumb_gen_state.saved_call_contexts = NULL;
  thumb_gen_state.nested_call_depth = 0;
  thumb_gen_state.saved_call_contexts_capacity = 0;
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

  /* For hard float ABI, configure VFP single-precision registers S0-S15 */
  if (float_abi == ARM_HARD_FLOAT) {
    /* S0-S15 are available for allocation (16 registers) */
    s->float_registers_map_for_allocator = 0xFFFF; /* bits 0-15 for S0-S15 */
    s->float_registers_for_allocator = 16;
  } else {
    /* No VFP registers for soft float */
    s->float_registers_map_for_allocator = 0;
    s->float_registers_for_allocator = 0;
  }

  if (!s->pic) {
    s->registers_map_for_allocator |= (1 << ARM_R9);
    s->registers_for_allocator += 1;
  }

  if (s->omit_frame_pointer) {
    s->registers_map_for_allocator |= (1 << ARM_R7);
    s->registers_for_allocator += 1;
  }

  th_literal_pool_init();
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

static void th_literal_pool_generate(void) {
  static int generating_pool = 0; /* Prevent recursive calls */

  if (generating_pool)
    return;

  if (thumb_gen_state.literal_pool_count == 0) {
    thumb_gen_state.code_size = 0;
    return;
  }

  generating_pool = 1;

  /* Count unique literals to calculate pool size */
  int unique_count = 0;
  for (int i = 0; i < thumb_gen_state.literal_pool_count; i++) {
    if (thumb_gen_state.literal_pool[i].shared_index == -1)
      unique_count++;
  }

  /* Emit a branch to skip over the literal pool.
   * Pool size = unique_count * 4 bytes (each literal is 32-bit).
   * We may need +2 for alignment NOP.
   * Branch offset is from PC+4 to after the pool.
   */
  int pool_size = unique_count * 4;
  int branch_pos = ind;
  int need_align = (ind & 2) ? 2 : 0; /* alignment padding after branch */

  /* Emit placeholder branch (will be patched later) - use 32-bit B.W */
  o(0xf000); /* first halfword of B.W */
  o(0x9000); /* second halfword placeholder */

  if (need_align) {
    /* align to 4 bytes after branch */
    thumb_opcode nop =
        th_mov_reg(R0, R0, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT,
                   ENFORCE_ENCODING_NONE, false);
    o(nop.opcode & 0xffff);
  }

  /* Array to store the output position of each unique literal */
  int *literal_positions =
      tcc_malloc(thumb_gen_state.literal_pool_count * sizeof(int));

  th_sym_d();

  /* First pass: emit unique literals and record their positions */
  for (int i = 0; i < thumb_gen_state.literal_pool_count; i++) {
    ThumbLiteralPoolEntry *entry = &thumb_gen_state.literal_pool[i];
    if (entry->shared_index == -1) {
      /* This is a unique entry - emit the literal value */
      literal_positions[i] = ind;
      if (entry->relocation != -1) {
        greloc(cur_text_section, entry->sym, ind, entry->relocation);
      }
      // write the literal value
      o(entry->imm & 0xffff);
      o((entry->imm >> 16) & 0xffff);
    } else {
      /* Shared entry - will use position of the original */
      literal_positions[i] = literal_positions[entry->shared_index];
    }
  }

  /* Patch the branch instruction to jump to after the pool */
  int branch_target =
      ind - branch_pos - 4; /* offset from PC (branch_pos + 4) */
  thumb_opcode branch = th_b_t4(branch_target);
  uint16_t *branch_patch = (uint16_t *)(cur_text_section->data + branch_pos);
  branch_patch[0] = (branch.opcode >> 16) & 0xffff;
  branch_patch[1] = branch.opcode & 0xffff;

  th_sym_t();

  /* Second pass: patch all instructions to point to correct literal position */
  for (int i = 0; i < thumb_gen_state.literal_pool_count; i++) {
    ThumbLiteralPoolEntry *entry = &thumb_gen_state.literal_pool[i];
    int literal_pos = literal_positions[i];
    int aligned_position = ((literal_pos - entry->patch_position) + 3) & ~3;

    // patch the instruction that references this literal
    if (entry->short_instruction) {
      uint16_t *patch_ins =
          (uint16_t *)(cur_text_section->data + entry->patch_position);
      *patch_ins |= (((aligned_position - 4) >> 2) & 0x00ff);
    } else {
      uint16_t *patch_ins =
          (uint16_t *)(cur_text_section->data + entry->patch_position + 2);
      *patch_ins |= (((aligned_position - 4)) & 0x0fff);
    }
  }

  tcc_free(literal_positions);
  thumb_gen_state.literal_pool_count = 0;
  thumb_gen_state.code_size = 0;
  generating_pool = 0;
}

int is_valid_opcode(thumb_opcode op) { return (op.size == 2 || op.size == 4); }

int ot(thumb_opcode op) {
  if (op.size == 0)
    return op.size;

  if (thumb_gen_state.generating_function) {
    thumb_gen_state.code_size += op.size;
    // 16-bit encoding for ldr should be efficient
    const int max_offset =
        thumb_gen_state.code_size + thumb_gen_state.literal_pool_count * 4;
    if (max_offset >= 1020) {
      th_literal_pool_generate();
    }
  }

  if (op.size == 4)
    o(op.opcode >> 16);
  o(op.opcode & 0xffff);
  return op.size;
}

static void load_full_const(int r, int32_t imm, struct Sym *sym);
static void gcall_or_jump(int is_jmp, SValue *dest);

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

static thumb_opcode th_generic_mov_imm(uint32_t r, int imm) {
  if (imm < 0) {
    return th_mvn_imm(r, 0, -imm + 1, FLAGS_BEHAVIOUR_NOT_IMPORTANT,
                      ENFORCE_ENCODING_NONE);
  }
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
    ot_check(th_rsb_imm(rr, rr, 0, FLAGS_BEHAVIOUR_NOT_IMPORTANT,
                        ENFORCE_ENCODING_NONE));
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
  // gcall_or_jmp(0);

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
  // gcall_or_jmp(1);

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
    uint32_t base = R_FP;
    if (v < VT_CONST) {
      /* Use pr0 if allocated and not spilled, otherwise check for spill */
      if (sv->pr0 != -1 && !(sv->pr0 & PREG_SPILLED)) {
        base = sv->pr0;
        v = VT_LOCAL;
        fc = sign = 0;
      } else if (sv->pr0 & PREG_SPILLED) {
        /* Spilled to stack - use FP-relative addressing with offset from c.i */
        base = R_FP;
        v = VT_LOCAL;
        /* fc and sign already set from sv->c.i above */
      } else {
        base = intr(v);
        v = VT_LOCAL;
        fc = sign = 0;
      }
    } else if (v == VT_LOCAL) {
      /* Direct VT_LOCAL - use FP-relative addressing with offset from c.i */
      base = R_FP;
      /* fc and sign already set from sv->c.i above */
    } else if (v == VT_CONST) {
      /* Check if we already have this global symbol's base address cached */
      if (sv->sym && sv->sym == thumb_gen_state.cached_global_sym &&
          thumb_gen_state.cached_global_reg >= 0) {
        /* Reuse cached base address, keep the offset */
        base = thumb_gen_state.cached_global_reg;
        /* fc already has the field offset from sv->c.i */
      } else {
        /* Load the base address of the global symbol (without offset) */
        SValue v1;
        v1.type.t = ft;
        v1.r = fr & ~VT_LVAL;
        v1.c.i = 0; /* Load base address, not base+offset */
        v1.sym = sv->sym;
        load(base = 14, &v1);
        /* Cache this for subsequent accesses to same symbol */
        thumb_gen_state.cached_global_sym = sv->sym;
        thumb_gen_state.cached_global_reg = base;
        /* fc already has the field offset from sv->c.i */
      }
      sign = 0;
      v = VT_LOCAL;
    }
    if (v == VT_LOCAL) {
      if (is_float(ft)) {
        /* Check if source is VFP or integer register */
        if (r >= TREG_F0 && r <= TREG_F7) {
          /* Source is VFP register - use VSTR */
          if ((ft & VT_BTYPE) != VT_FLOAT)
            ot_check(th_vstr(base, vfpr(r), !sign, 1, fc));
          else
            ot_check(th_vstr(base, vfpr(r), !sign, 0, fc));
        } else {
          /* Source is integer register - use regular STR for soft float path */
          if ((ft & VT_BTYPE) == VT_FLOAT) {
            /* Single precision - one 32-bit store */
            if (!ot(th_str_imm(r, base, fc, sign ? 4 : 6,
                               ENFORCE_ENCODING_NONE))) {
              int rr = th_offset_to_reg(fc, sign);
              ot_check(th_str_reg(r, base, rr, THUMB_SHIFT_DEFAULT,
                                  ENFORCE_ENCODING_NONE));
            }
          } else {
            /* Double precision - two 32-bit stores (low word first) */
            /* Use sv->pr1 for high register, not r+1 which could be invalid */
            int r_high = sv->pr1;
            if (r_high < 0 || r_high == R_SP || r_high == R_PC) {
              /* Fallback: if pr1 not allocated, try r+1 but validate */
              r_high = r + 1;
              if (r_high == R_SP || r_high == R_PC) {
                tcc_error("compiler_error: cannot store double - no valid high "
                          "register (pr1=%d, r+1=%d would be SP/PC)\n",
                          sv->pr1, r + 1);
              }
            }
            /* Store low word */
            if (!ot(th_str_imm(r, base, fc, sign ? 4 : 6,
                               ENFORCE_ENCODING_NONE))) {
              int rr = th_offset_to_reg(fc, sign);
              ot_check(th_str_reg(r, base, rr, THUMB_SHIFT_DEFAULT,
                                  ENFORCE_ENCODING_NONE));
            }
            /* Store high word at fc+4 */
            if (!ot(th_str_imm(r_high, base, fc + 4, sign ? 4 : 6,
                               ENFORCE_ENCODING_NONE))) {
              int rr = th_offset_to_reg(fc + 4, sign);
              ot_check(th_str_reg(r_high, base, rr, THUMB_SHIFT_DEFAULT,
                                  ENFORCE_ENCODING_NONE));
            }
          }
        }
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

static ThumbLiteralPoolEntry *th_literal_pool_allocate() {
  ThumbLiteralPoolEntry *entry;
  if (thumb_gen_state.literal_pool_count >= thumb_gen_state.literal_pool_size) {
    const int new_size = thumb_gen_state.literal_pool_size << 1;
    thumb_gen_state.literal_pool = tcc_realloc(
        thumb_gen_state.literal_pool, new_size * sizeof(ThumbLiteralPoolEntry));
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
static ThumbLiteralPoolEntry *th_literal_pool_find_or_allocate(Sym *sym,
                                                               int32_t imm) {
  int found_index = -1;
  /* Search existing entries for a match */
  for (int i = 0; i < thumb_gen_state.literal_pool_count; i++) {
    ThumbLiteralPoolEntry *e = &thumb_gen_state.literal_pool[i];
    /* Match on sym and imm, and it must be a primary entry (not shared) */
    if (e->sym == sym && e->imm == imm && e->shared_index == -1) {
      found_index = i;
      break;
    }
  }
  /* Allocate new entry */
  ThumbLiteralPoolEntry *entry = th_literal_pool_allocate();
  if (found_index >= 0) {
    /* Mark as sharing with the found entry */
    entry->shared_index = found_index;
  }
  return entry;
}

static void load_full_const(int r, int32_t imm, struct Sym *sym) {
  int est = 0;
  ElfSym *esym = elfsym(sym);
  ThumbLiteralPoolEntry *entry = th_literal_pool_find_or_allocate(sym, imm);
  int sym_off = 0;

  entry->sym = sym;
  entry->imm = imm;
  entry->patch_position = ind;

  TRACE("'load_full_const' to register: %d, with imm: %d\n", r, imm);
  // allocate space for T1 encoding
  est = th_ldr_literal_estimate(r, 1020);
  entry->short_instruction = est == 2 ? 1 : 0;
  ot_check(th_ldr_literal(r, 0, 1));

  if (esym) {
    sym_off = esym->st_shndx;
  }
  if (!pic) {
    if (sym) {
      entry->relocation = R_ARM_ABS32;
    }
  } else {
    if (sym) {
      if (text_and_data_separation) {
        // all data except constants in .ro section can be addressed relative to
        // .got, how can I distinguish that situation?
        //
        if (sym->type.t & VT_STATIC && sym_off != cur_text_section->sh_num) {
          entry->relocation = R_ARM_GOTOFF;
        } else {
          entry->relocation = R_ARM_GOT32;
        }
      } else {
        if (sym->type.t & VT_STATIC) {
          entry->relocation = R_ARM_REL32;
        } else {
          entry->relocation = R_ARM_GOT_PREL;
        }
      }
    }
  }

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
            entry2->short_instruction = false;
            ot_check(th_ldr_literal(R_LR, 0, 1));
            ot_check(th_add_reg(r, r, R_LR, FLAGS_BEHAVIOUR_NOT_IMPORTANT,
                                THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
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
            ThumbLiteralPoolEntry *entry2 = th_literal_pool_allocate();
            entry2->sym = NULL;
            entry2->imm = imm;
            entry2->patch_position = ind;
            entry2->relocation = -1;
            entry2->short_instruction = false;
            ot_check(th_ldr_literal(R_LR, 0, 1));
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

int store_word_to_base(int ir, int base, int fc, int sign) {
  const thumb_opcode ins =
      th_str_imm(ir, base, fc, sign ? 4 : 6, ENFORCE_ENCODING_NONE);
  TRACE("Store word sign: %d, r %d, base: %d, fc: %d\n", sign, ir, base, fc);
  return ot(ins);
}

void load_vt_lval_vt_local(int r, SValue *sv, int ft, int fc, int sign,
                           uint32_t base) {
  int success = 0;
  const int btype = ft & VT_BTYPE;
  int ir = intr(r);
  TRACE("load_vt_lval_vt_local: fc: %i", fc);

  if (is_float(ft)) {
    /* Check if destination is a VFP register or an integer register.
     * For soft float (IR code path), floats are loaded to integer registers.
     * Note: r values 0-4 are always integer registers (R0-R3, R12=TREG_R12=4).
     * r values 5-12 could be TREG_F0-F7 OR physical R5-R12.
     * We use a heuristic: if r is a known scratch register (R12=12), use
     * integer path. Also check sv->pr0 - if it's PREG_SPILLED, we're loading
     * from stack to temp register for copy, which should use integer path. */
    int use_vfp = (r >= TREG_F0 && r <= TREG_F7);
    /* Override: if r is physical R12 (12), always use integer path */
    if (r == 12 || r == 14) {
      use_vfp = 0;
    }
    if (use_vfp) {
      /* VFP register - use VFP load instructions */
      TRACE("load float to VFP r: %d, base: %d, fc: %d, sign: %d\n", ir, base,
            fc, sign);
      return load_vt_lval_vt_local_float(r, sv, ft, fc, sign, base);
    } else {
      /* Integer register - load float as raw bits (soft float) */
      TRACE("load float to INT r: %d, base: %d, fc: %d, sign: %d\n", ir, base,
            fc, sign);
      if (btype == VT_DOUBLE || btype == VT_LDOUBLE) {
        /* Double: load 64 bits to pre-allocated register pair.
         * Use sv->pr0 (low) and sv->pr1 (high) from register allocator,
         * NOT ir+1 which could be SP/PC or already in use. */
        int ir_high = sv->pr1;
        if (ir_high < 0 || ir_high == R_SP || ir_high == R_PC) {
          /* Fallback: if pr1 not allocated, try ir+1 but validate */
          ir_high = ir + 1;
          if (ir_high == R_SP || ir_high == R_PC) {
            tcc_error(
                "compiler_error: cannot load double - no valid high register "
                "(pr1=%d, ir+1=%d would be SP/PC)\n",
                sv->pr1, ir + 1);
          }
        }
        /* Load low word first */
        success = load_word_from_base(ir, base, fc, sign);
        if (!success) {
          int rr = th_offset_to_reg(fc, sign);
          ot_check(th_ldr_reg(ir, base, rr, THUMB_SHIFT_DEFAULT,
                              ENFORCE_ENCODING_NONE));
        }
        /* Load high word.
         * For negative offsets (sign=1), high word is at fc-4 (closer to base).
         * For positive offsets (sign=0), high word is at fc+4 (further from
         * base).
         */
        int fc_high = sign ? (fc - 4) : (fc + 4);
        int sign_high = sign;
        /* Handle case where fc_high becomes 0 or changes sign */
        if (sign && fc_high < 0) {
          fc_high = -fc_high;
          sign_high = 0;
        }
        success = load_word_from_base(ir_high, base, fc_high, sign_high);
        if (!success) {
          int rr = th_offset_to_reg(fc_high, sign_high);
          ot_check(th_ldr_reg(ir_high, base, rr, THUMB_SHIFT_DEFAULT,
                              ENFORCE_ENCODING_NONE));
        }
      } else {
        /* Float: load 32 bits to single integer register */
        success = load_word_from_base(ir, base, fc, sign);
        if (!success) {
          int rr = th_offset_to_reg(fc, sign);
          ot_check(th_ldr_reg(ir, base, rr, THUMB_SHIFT_DEFAULT,
                              ENFORCE_ENCODING_NONE));
        }
      }
      return;
    }
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
  int ft = sv->type.t & VT_BTYPE;
  printf("DEBUG load_vt_const: r=%d, ft=0x%x, c.i=0x%llx\n", r, ft,
         (unsigned long long)sv->c.i);
  r = intr(r);

  if (ft == VT_DOUBLE || ft == VT_LDOUBLE) {
    /* 64-bit double constant - load both halves */
    /* Use sv->pr1 for high register, not r+1 which could be invalid */
    int r_high = sv->pr1;
    if (r_high < 0 || r_high == R_SP || r_high == R_PC) {
      /* Fallback: if pr1 not allocated, try r+1 but validate */
      r_high = intr(r) + 1;
      if (r_high == R_SP || r_high == R_PC) {
        tcc_error("compiler_error: cannot load double const - no valid high "
                  "register (pr1=%d, r+1=%d would be SP/PC)\n",
                  sv->pr1, intr(r) + 1);
      }
    }
    uint64_t val64 = sv->c.i; /* c.i is the same memory as c.d due to union */
    uint32_t lo = (uint32_t)(val64 & 0xFFFFFFFF);
    uint32_t hi = (uint32_t)(val64 >> 32);

    /* Load low word to r */
    if (!ot(th_generic_mov_imm(r, lo)))
      load_full_const(r, lo, 0);
    /* Load high word to r_high */
    if (!ot(th_generic_mov_imm(r_high, hi)))
      load_full_const(r_high, hi, 0);
    return;
  }

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
  printf("DEBUG load: r=%d, fr=0x%x, ft=0x%x, fc=0x%x\n", r, fr, ft, fc);
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
      /* Check if we already have this global symbol's base address cached */
      if (sv->sym && sv->sym == thumb_gen_state.cached_global_sym &&
          thumb_gen_state.cached_global_reg >= 0) {
        /* Reuse cached base address, keep the offset */
        base = thumb_gen_state.cached_global_reg;
        /* fc already has the field offset from sv->c.i */
      } else {
        v1.type.t = VT_PTR;
        v1.r = fr & ~VT_LVAL;
        v1.c.i = 0; /* Load base address, not base+offset */
        v1.sym = sv->sym;
        TRACE("l2");
        load(base = 14, &v1);
        /* Cache this for subsequent accesses to same symbol */
        thumb_gen_state.cached_global_sym = sv->sym;
        thumb_gen_state.cached_global_reg = base;
        /* fc already has the field offset from sv->c.i */
      }
      sign = 0;
      v = VT_LOCAL;
    } else if (v < VT_CONST) {
      /* Check if spilled or allocated to register */
      if (sv->pr0 & PREG_SPILLED) {
        /* Spilled to stack - use FP-relative with offset from c.i */
        base = R_FP;
        /* fc and sign already set from sv->c.i above */
      } else if (sv->pr0 != -1) {
        /* Allocated to register */
        base = sv->pr0;
        fc = sign = 0;
      } else {
        /* Not allocated - should not happen for lvalues */
        base = intr(v);
        fc = sign = 0;
      }
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
      /* Check if we're moving between VFP registers or integer registers */
      if (r >= TREG_F0 && r <= TREG_F7 && v >= TREG_F0 && v <= TREG_F7) {
        /* VFP to VFP move */
        if ((ft & VT_BTYPE) == VT_FLOAT)
          ot_check(th_vmov_register(vfpr(r), vfpr(v), 0));
        else
          ot_check(th_vmov_register(vfpr(r), vfpr(v), 1));
      } else {
        /* Integer register move (soft float) */
        ot_check(th_mov_reg(r, v, FLAGS_BEHAVIOUR_NOT_IMPORTANT,
                            THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE, false));
        if ((ft & VT_BTYPE) == VT_DOUBLE || (ft & VT_BTYPE) == VT_LDOUBLE) {
          /* Also move high word for double.
           * Use sv->pr1 for destination high register, not r+1 which could be
           * invalid. Source high register comes from sv->r2. */
          int r_high = sv->pr1;
          int v_high = (sv->r2 != VT_CONST) ? sv->r2 : (v + 1);
          if (r_high < 0 || r_high == R_SP || r_high == R_PC) {
            /* Fallback: if pr1 not allocated, try r+1 but validate */
            r_high = r + 1;
            if (r_high == R_SP || r_high == R_PC) {
              tcc_error("compiler_error: cannot move double - no valid high "
                        "dest register (pr1=%d, r+1=%d would be SP/PC)\n",
                        sv->pr1, r + 1);
            }
          }
          ot_check(th_mov_reg(r_high, v_high, FLAGS_BEHAVIOUR_NOT_IMPORTANT,
                              THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE,
                              false));
        }
      }
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

//     ot_checr(th_push(1 << rr));
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

static int th_has_immediate_value(int r) {
  return (r & (VT_VALMASK | VT_LVAL | VT_SYM)) == VT_CONST;
}

typedef struct ThumbDataProcessingHandler {
  thumb_opcode (*imm_handler)(uint32_t rd, uint32_t rn, uint32_t imm,
                              thumb_flags_behaviour flags_behaviour,
                              thumb_enforce_encoding enforce_encoding);
  thumb_opcode (*reg_handler)(uint32_t rd, uint32_t rn, uint32_t rm,
                              thumb_flags_behaviour flags_behaviour,
                              thumb_shift shift_type,
                              thumb_enforce_encoding enforce_encoding);
} ThumbDataProcessingHandler;

void tcc_gen_machine_data_processing_op(TACQuadruple *op) {
  ThumbDataProcessingHandler handler;
  switch (op->op) {
  case TCCIR_OP_ADD:
    handler.imm_handler = th_add_imm;
    handler.reg_handler = th_add_reg;
    break;
  case TCCIR_OP_SUB:
    handler.imm_handler = th_sub_imm;
    handler.reg_handler = th_sub_reg;
    break;
  case TCCIR_OP_MUL:
    ot_check(th_mul(op->dest.pr0, op->src1.pr0, op->src2.pr0,
                    FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE));
    return;
  case TCCIR_OP_CMP:
    handler.imm_handler = th_cmp_imm;
    handler.reg_handler = th_cmp_reg;
    break;
  case TCCIR_OP_SHL: {
    handler.imm_handler = th_lsl_imm;
    handler.reg_handler = th_lsl_reg;
    break;
  }
  case TCCIR_OP_SHR: {
    handler.imm_handler = th_lsr_imm;
    handler.reg_handler = th_lsr_reg;
    break;
  }
  case TCCIR_OP_OR: {
    handler.imm_handler = th_orr_imm;
    handler.reg_handler = th_orr_reg;
    break;
  }
  case TCCIR_OP_AND: {
    handler.imm_handler = th_and_imm;
    handler.reg_handler = th_and_reg;
    break;
  }
  case TCCIR_OP_XOR: {
    handler.imm_handler = th_eor_imm;
    handler.reg_handler = th_eor_reg;
    break;
  }
  case TCCIR_OP_SAR: {
    handler.imm_handler = th_asr_imm;
    handler.reg_handler = th_asr_reg;
    break;
  }
  case TCCIR_OP_DIV: {
    ot_check(th_sdiv(op->dest.pr0, op->src1.pr0, op->src2.pr0));
    return;
  }
  case TCCIR_OP_UDIV: {
    ot_check(th_udiv(op->dest.pr0, op->src1.pr0, op->src2.pr0));
    return;
  }
  case TCCIR_OP_ADC_USE:
    fprintf(stderr, "compiler_error: TCCIR_OP_ADC_USE not implemented\n");
    exit(1);
  case TCCIR_OP_ADC_GEN:
    fprintf(stderr, "compiler_error: TCCIR_OP_ADC_GEN not implemented\n");
    exit(1);
  case TCCIR_OP_TEST_ZERO:
    ot_check(th_cmp_imm(0, intr(op->src1.pr0), 0, FLAGS_BEHAVIOUR_SET,
                        ENFORCE_ENCODING_NONE));
    return;
  default: {
    printf("compiler_error: unhandled data processing op: %s\n",
           tcc_ir_get_op_name(op->op));
  }
  }

  if (op->op == TCCIR_OP_CMP) {
    printf("DEBUG CMP: src1.pr0=R%d, src2.c.i=%d, src2.r=0x%x\n", op->src1.pr0,
           op->src2.c.i, op->src2.r);
  }

  if (th_has_immediate_value(op->src2.r)) {
    if (!ot(handler.imm_handler(op->dest.pr0, op->src1.pr0, op->src2.c.i,
                                FLAGS_BEHAVIOUR_NOT_IMPORTANT,
                                ENFORCE_ENCODING_NONE))) {
      // load immediate to temp register and add
      load(R12, &op->src2);
      ot_check(handler.reg_handler(op->dest.pr0, op->src1.pr0, R12,
                                   FLAGS_BEHAVIOUR_NOT_IMPORTANT,
                                   THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
    }
  } else {
    ot_check(handler.reg_handler(op->dest.pr0, op->src1.pr0, op->src2.pr0,
                                 FLAGS_BEHAVIOUR_NOT_IMPORTANT,
                                 THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
  }
}

/* Get the soft float library function name for an FP operation */
static const char *get_softfp_func_name(TccIrOp op, int is_double) {
  switch (op) {
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

/* Generate soft float library call for FP operation */
static void gen_softfp_call(TACQuadruple *q, const char *func_name,
                            int is_double) {
  Sym *sym;
  SValue func_sv;

  /* For soft float ABI:
   * - float: passed in R0, result in R0
   * - double: passed in R0:R1 (low:high), result in R0:R1
   * For binary ops:
   * - float: R0=arg1, R1=arg2, result in R0
   * - double: R0:R1=arg1, R2:R3=arg2, result in R0:R1
   */

  if (is_double) {
    /* Load first double to R0:R1 */
    load(R0, &q->src1);
    /* Load second double to R2:R3 (if binary op) */
    if (q->op != TCCIR_OP_FNEG) {
      load(R2, &q->src2);
    }
  } else {
    /* Load first float to R0 */
    load(R0, &q->src1);
    /* Load second float to R1 (if binary op) */
    if (q->op != TCCIR_OP_FNEG) {
      load(R1, &q->src2);
    }
  }

  /* Get or create the external symbol for the library function */
  sym = external_global_sym(tok_alloc_const(func_name), &func_old_type);

  /* Set up SValue for the function call */
  memset(&func_sv, 0, sizeof(SValue));
  func_sv.r = VT_CONST | VT_SYM;
  func_sv.sym = sym;
  func_sv.c.i = 0;

  /* Generate BL to the function */
  gcall_or_jump(0, &func_sv);

  /* Store result from R0 (or R0:R1 for double) to destination */
  store(R0, &q->dest);
}

/* Helper to load a float operand to a VFP register.
 * If the operand is already in a VFP register, just return its number.
 * Otherwise, load to integer reg and move to the specified VFP scratch
 * register.
 */
static int load_fp_operand_to_vfp(SValue *sv, int scratch_sreg,
                                  int scratch_dreg, int is_double) {
  /* Check if operand is already in a VFP register (pr0 has VFP marker) */
  if (sv->pr0 >= 0 && LS_IS_VFP_REG(sv->pr0)) {
    return LS_VFP_REG_NUM(sv->pr0);
  }

  /* Not in VFP reg - load to integer reg and move to VFP scratch */
  load(R0, sv);
  if (is_double) {
    ot_check(th_vmov_2gp_dp(R0, R1, scratch_dreg, 0 /* to VFP */));
    return scratch_dreg * 2; /* D0 = S0:S1, D1 = S2:S3 */
  } else {
    ot_check(th_vmov_gp_sp(R0, scratch_sreg, 0 /* to VFP */));
    return scratch_sreg;
  }
}

/* Helper to store result from VFP register to destination.
 * If destination is a VFP register, move directly.
 * Otherwise, move to integer reg and store.
 */
static void store_fp_result_from_vfp(SValue *dest, int result_sreg,
                                     int result_dreg, int is_double) {
  /* Check if destination is a VFP register */
  if (dest->pr0 >= 0 && LS_IS_VFP_REG(dest->pr0)) {
    int dest_sreg = LS_VFP_REG_NUM(dest->pr0);
    if (is_double) {
      /* Move D-reg to D-reg (result_dreg to dest_dreg)
       * dest_sreg is S-register number, convert to D-register number */
      int dest_dreg = dest_sreg / 2;
      if (result_dreg != dest_dreg) {
        ot_check(th_vmov_register(dest_dreg, result_dreg, 1)); /* double */
      }
    } else {
      /* Move S-reg to S-reg */
      if (result_sreg != dest_sreg) {
        ot_check(th_vmov_register(dest_sreg, result_sreg, 0)); /* single */
      }
    }
    return;
  }

  /* Destination is not in VFP - move to integer reg and store/move */
  if (is_double) {
    ot_check(th_vmov_2gp_dp(R0, R1, result_dreg, 1 /* to ARM */));
    /* If dest has an allocated integer register pair, move to it */
    printf("DEBUG store_fp_result_from_vfp: vr=%d pr0=%d pr1=%d is_vfp=%d "
           "spilled=%d c.i=%d\n",
           dest->vr, dest->pr0, dest->pr1, LS_IS_VFP_REG(dest->pr0),
           (dest->pr0 & PREG_SPILLED) != 0, (int)dest->c.i);
    if (dest->pr0 >= 0 && !LS_IS_VFP_REG(dest->pr0) &&
        !(dest->pr0 & PREG_SPILLED) && dest->pr1 >= 0) {
      printf("DEBUG: Taking MOV path - pr0=%d pr1=%d\n", dest->pr0, dest->pr1);
      /* Move R0:R1 to dest register pair */
      if (dest->pr0 != R0) {
        ot_check(th_mov_reg(dest->pr0, R0, FLAGS_BEHAVIOUR_NOT_IMPORTANT,
                            THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE, false));
      }
      if (dest->pr1 != R1) {
        ot_check(th_mov_reg(dest->pr1, R1, FLAGS_BEHAVIOUR_NOT_IMPORTANT,
                            THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE, false));
      }
    } else {
      /* Store both words to memory - either spilled or no pr1 allocated */
      /* For spilled vregs, set up r = VT_LOCAL so store() uses FP-relative */
      SValue store_dest = *dest;
      if (dest->pr0 & PREG_SPILLED) {
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
  } else {
    ot_check(th_vmov_gp_sp(R0, result_sreg, 1 /* to ARM */));
    /* If dest has an allocated integer register, move to it */
    if (dest->pr0 >= 0 && !LS_IS_VFP_REG(dest->pr0) &&
        !(dest->pr0 & PREG_SPILLED)) {
      if (dest->pr0 != R0) {
        ot_check(th_mov_reg(dest->pr0, R0, FLAGS_BEHAVIOUR_NOT_IMPORTANT,
                            THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE, false));
      }
    } else {
      /* For spilled vregs, set up r = VT_LOCAL so store() uses FP-relative */
      SValue store_dest = *dest;
      if (dest->pr0 & PREG_SPILLED) {
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
static void gen_hardfp_op(TACQuadruple *q, int is_double) {
  uint32_t sz = is_double ? 1 : 0;
  int src1_reg, src2_reg;

  /* Load first operand - may already be in a VFP register */
  src1_reg =
      load_fp_operand_to_vfp(&q->src1, 0 /* S0 */, 0 /* D0 */, is_double);

  /* Load second operand for binary ops */
  if (q->op != TCCIR_OP_FNEG) {
    src2_reg =
        load_fp_operand_to_vfp(&q->src2, 2 /* S2 */, 1 /* D1 */, is_double);
  } else {
    src2_reg = 0; /* unused for negation */
  }

  /* Perform the VFP operation - result in S0/D0 */
  switch (q->op) {
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
static void gen_hardfp_cmp(TACQuadruple *q, int is_double) {
  uint32_t sz = is_double ? 1 : 0;
  int src1_reg, src2_reg;

  /* Load operands - may already be in VFP registers */
  /* First load src1 to see what register it uses */
  src1_reg =
      load_fp_operand_to_vfp(&q->src1, 0 /* S0 */, 0 /* D0 */, is_double);

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
static void gen_hardfp_cvt_ftof(TACQuadruple *q) {
  int src_is_double = ((q->src1.type.t & VT_BTYPE) != VT_FLOAT);
  int dst_is_double = ((q->dest.type.t & VT_BTYPE) != VT_FLOAT);
  int src_reg;

  /* Load source - may already be in VFP register */
  src_reg =
      load_fp_operand_to_vfp(&q->src1, 0 /* S0 */, 0 /* D0 */, src_is_double);

  /* Convert */
  if (dst_is_double && !src_is_double) {
    /* float to double: Sn -> D0 */
    ot_check(th_vcvt_float_to_double(0, src_reg));
  } else if (!dst_is_double && src_is_double) {
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
static void gen_hardfp_cvt_itof(TACQuadruple *q) {
  int src_bt = q->src1.type.t & VT_BTYPE;
  int dst_is_double = ((q->dest.type.t & VT_BTYPE) != VT_FLOAT);
  int is_unsigned = (q->src1.type.t & VT_UNSIGNED) ? 1 : 0;

  /* For LLONG, we need library call even in hard float mode */
  if (src_bt == VT_LLONG) {
    const char *func_name;
    if (dst_is_double) {
      func_name = is_unsigned ? "__aeabi_ul2d" : "__aeabi_l2d";
    } else {
      func_name = is_unsigned ? "__aeabi_ul2f" : "__aeabi_l2f";
    }
    gen_softfp_call(q, func_name, 0);
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
  ot_check(th_vcvt_fp_int(0, 0, 0 /* always write to S0/D0 */, dst_is_double,
                          is_unsigned ? 0 : 1));

  /* Store result to destination */
  store_fp_result_from_vfp(&q->dest, 0 /* S0 */, 0 /* D0 */, dst_is_double);
}

/* Generate float-to-int conversion.
 * Uses VCVT for hard float, library calls for soft float.
 */
static void gen_hardfp_cvt_ftoi(TACQuadruple *q) {
  int src_is_double = ((q->src1.type.t & VT_BTYPE) != VT_FLOAT);
  int dst_bt = q->dest.type.t & VT_BTYPE;
  int is_unsigned = (q->dest.type.t & VT_UNSIGNED) ? 1 : 0;
  int src_reg;

  /* For LLONG destination, we need library call even in hard float mode */
  if (dst_bt == VT_LLONG) {
    const char *func_name;
    if (src_is_double) {
      func_name = is_unsigned ? "__aeabi_d2ulz" : "__aeabi_d2lz";
    } else {
      func_name = is_unsigned ? "__aeabi_f2ulz" : "__aeabi_f2lz";
    }
    gen_softfp_call(q, func_name, src_is_double);
    return;
  }

  /* Load float/double source - may already be in VFP register */
  src_reg =
      load_fp_operand_to_vfp(&q->src1, 0 /* S0 */, 0 /* D0 */, src_is_double);

  /* VCVT: convert float/double to int
   * opc2=4 for unsigned, opc2=5 for signed (with round toward zero)
   * Result goes to S0
   */
  ot_check(th_vcvt_fp_int(0, src_is_double ? src_reg / 2 : src_reg,
                          is_unsigned ? 0x4 : 0x5, src_is_double, 1));

  /* Move result from S0 to R0 */
  ot_check(th_vmov_gp_sp(R0, 0 /* S0 */, 1 /* to ARM */));

  store(R0, &q->dest);
}

/* Generate floating point operation.
 * Uses VFP hardware instructions when hard float ABI is enabled,
 * otherwise falls back to software library calls.
 */
ST_FUNC void tcc_gen_machine_fp_op(TACQuadruple *q) {
  int is_double = ((q->src1.type.t & VT_BTYPE) != VT_FLOAT);
  const char *func_name;

  /* Use VFP hardware instructions when hard float ABI is enabled */
  if (tcc_state->float_abi == ARM_HARD_FLOAT) {
    if (q->op == TCCIR_OP_FCMP) {
      gen_hardfp_cmp(q, is_double);
      return;
    }
    /* For arithmetic ops, use VFP instructions */
    if (q->op == TCCIR_OP_FADD || q->op == TCCIR_OP_FSUB ||
        q->op == TCCIR_OP_FMUL || q->op == TCCIR_OP_FDIV ||
        q->op == TCCIR_OP_FNEG) {
      gen_hardfp_op(q, is_double);
      return;
    }
    /* For conversion ops, use VFP instructions */
    if (q->op == TCCIR_OP_CVT_FTOF) {
      gen_hardfp_cvt_ftof(q);
      return;
    }
    if (q->op == TCCIR_OP_CVT_ITOF) {
      gen_hardfp_cvt_itof(q);
      return;
    }
    if (q->op == TCCIR_OP_CVT_FTOI) {
      gen_hardfp_cvt_ftoi(q);
      return;
    }
  }

  /* Fall back to software floating point library calls */
  func_name = get_softfp_func_name(q->op, is_double);

  if (q->op == TCCIR_OP_FNEG) {
    /* Negation: XOR the sign bit */
    /* For float: XOR R0 with 0x80000000 */
    /* For double: XOR R1 with 0x80000000 (high word has sign) */
    load(R0, &q->src1);
    /* Load 0x80000000 to R12 using literal pool */
    load_full_const(R12, 0x80000000, NULL);
    if (is_double) {
      /* XOR high word (R1) with sign bit */
      ot_check(th_eor_reg(R1, R1, R12, FLAGS_BEHAVIOUR_NOT_IMPORTANT,
                          THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
    } else {
      /* XOR R0 with sign bit */
      ot_check(th_eor_reg(R0, R0, R12, FLAGS_BEHAVIOUR_NOT_IMPORTANT,
                          THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
    }
    store(R0, &q->dest);
    return;
  }

  if (q->op == TCCIR_OP_FCMP) {
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
    if (is_double) {
      /* Double: src1 in R0:R1, src2 in R2:R3 */
      load(R0, &q->src1);
      load(R2, &q->src2);
    } else {
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
  if (q->op == TCCIR_OP_CVT_FTOF) {
    /* Float to double or double to float */
    int src_is_double = ((q->src1.type.t & VT_BTYPE) != VT_FLOAT);
    int dst_is_double = ((q->dest.type.t & VT_BTYPE) != VT_FLOAT);
    const char *func_name_cvt;

    if (dst_is_double && !src_is_double) {
      func_name_cvt = "__aeabi_f2d";
    } else if (!dst_is_double && src_is_double) {
      func_name_cvt = "__aeabi_d2f";
    } else {
      /* Same type, no conversion needed - just copy */
      load(R0, &q->src1);
      store(R0, &q->dest);
      return;
    }
    gen_softfp_call(q, func_name_cvt, src_is_double);
    return;
  }

  if (q->op == TCCIR_OP_CVT_ITOF) {
    /* Int to float/double */
    int src_bt = q->src1.type.t & VT_BTYPE;
    int dst_is_double = ((q->dest.type.t & VT_BTYPE) != VT_FLOAT);
    int is_unsigned = (q->src1.type.t & VT_UNSIGNED) ? 1 : 0;
    const char *func_name_cvt;

    if (src_bt == VT_LLONG) {
      if (dst_is_double) {
        func_name_cvt = is_unsigned ? "__aeabi_ul2d" : "__aeabi_l2d";
      } else {
        func_name_cvt = is_unsigned ? "__aeabi_ul2f" : "__aeabi_l2f";
      }
    } else {
      if (dst_is_double) {
        func_name_cvt = is_unsigned ? "__aeabi_ui2d" : "__aeabi_i2d";
      } else {
        func_name_cvt = is_unsigned ? "__aeabi_ui2f" : "__aeabi_i2f";
      }
    }
    gen_softfp_call(q, func_name_cvt, 0);
    return;
  }

  if (q->op == TCCIR_OP_CVT_FTOI) {
    /* Float/double to int */
    int src_is_double = ((q->src1.type.t & VT_BTYPE) != VT_FLOAT);
    int dst_bt = q->dest.type.t & VT_BTYPE;
    int is_unsigned = (q->dest.type.t & VT_UNSIGNED) ? 1 : 0;
    const char *func_name_cvt;

    if (dst_bt == VT_LLONG) {
      if (src_is_double) {
        func_name_cvt = is_unsigned ? "__aeabi_d2ulz" : "__aeabi_d2lz";
      } else {
        func_name_cvt = is_unsigned ? "__aeabi_f2ulz" : "__aeabi_f2lz";
      }
    } else {
      if (src_is_double) {
        func_name_cvt = is_unsigned ? "__aeabi_d2uiz" : "__aeabi_d2iz";
      } else {
        func_name_cvt = is_unsigned ? "__aeabi_f2uiz" : "__aeabi_f2iz";
      }
    }
    gen_softfp_call(q, func_name_cvt, src_is_double);
    return;
  }

  if (func_name) {
    gen_softfp_call(q, func_name, is_double);
    return;
  }

  tcc_error("compiler_error: unknown FP operation in tcc_gen_machine_fp_op");
}

ST_FUNC void tcc_gen_machine_return_value_op(TACQuadruple *q) {
  int is_64bit = is_64bit_type(q->src1.type.t);

  if ((q->src1.r & VT_VALMASK) == VT_CONST) {
    SValue src = q->src1;
    if (is_64bit) {
      /* Tell load() where to put the high word (R1) */
      src.pr1 = R1;
    }
    load(R0, &src);
    return;
  }

  /* If we have a valid physical register, use it */
  if (q->src1.pr0 >= 0 && q->src1.pr0 < 16) {
    /* If VT_LVAL is set and the register contains an address (not the value),
     * we need to dereference it */
    if ((q->src1.r & VT_LVAL) && (q->src1.r & VT_VALMASK) != VT_LOCAL) {
      SValue src = q->src1;
      if (is_64bit) {
        /* Tell load() where to put the high word (R1) */
        src.pr1 = R1;
      }
      load(R0, &src);
      return;
    }
    /* Otherwise just move the register value to R0 */
    if (q->src1.pr0 != R0) {
      ot_check(th_mov_reg(R0, q->src1.pr0, FLAGS_BEHAVIOUR_NOT_IMPORTANT,
                          THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE, false));
    }
    /* For 64-bit types, also move high register to R1 */
    if (is_64bit && q->src1.pr1 >= 0 && q->src1.pr1 != R1) {
      ot_check(th_mov_reg(R1, q->src1.pr1, FLAGS_BEHAVIOUR_NOT_IMPORTANT,
                          THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE, false));
    }
    return;
  }

  /* Fallback: use load for anything else (spilled values, etc.) */
  SValue src = q->src1;
  if (is_64bit) {
    /* Tell load() where to put the high word (R1) */
    src.pr1 = R1;
  }
  load(R0, &src);
}

void tcc_gen_machine_load_op(TACQuadruple *op) {
  TRACE("'tcc_gen_machine_load_op'");
  int is_64bit = is_64bit_type(op->src1.type.t);
  if (is_64bit) {
    /* For 64-bit values, load() needs to know where to put the high word.
     * Use dest.pr1 if allocated, otherwise use dest.pr0 + 1 */
    SValue src = op->src1;
    if (op->dest.pr1 >= 0) {
      src.pr1 = op->dest.pr1;
    } else {
      src.pr1 = op->dest.pr0 + 1;
    }
    load(op->dest.pr0, &src);
  } else {
    load(op->dest.pr0, &op->src1);
  }
}

ST_FUNC void tcc_gen_machine_store_op(TACQuadruple *op) {
  TRACE("'tcc_gen_machine_store_op'");
  int src_reg;
  int is_double = ((op->src1.type.t & VT_BTYPE) == VT_DOUBLE) ||
                  ((op->src1.type.t & VT_BTYPE) == VT_LDOUBLE);
  printf("DEBUG store_op: src1.pr0=%d, src1.r=0x%x, is_double=%d\n",
         op->src1.pr0, op->src1.r, is_double);

  /* If source has a valid, non-spilled register allocation, use it directly.
   * Otherwise, load the value (for spilled values, constants, or globals). */
  if (op->src1.pr0 >= 0 && !(op->src1.pr0 & PREG_SPILLED)) {
    /* Have a valid register allocation - use it directly */
    src_reg = op->src1.pr0;
    store(src_reg, &op->dest);
  } else if (op->src1.pr0 == -1 || (op->src1.pr0 & PREG_SPILLED) ||
             (op->src1.r & VT_LVAL)) {
    /* Need to load: no register, spilled, or lvalue that needs dereferencing */
    if (is_double) {
      /* For doubles, we need to copy both 32-bit words separately.
       * The source is at sv->c.i (spilled location), dest is at op->dest.c.i */
      int src_offset = op->src1.c.i;
      int dst_offset = op->dest.c.i;

      /* Load and store low word */
      int src_sign = (src_offset < 0);
      if (src_sign)
        src_offset = -src_offset;
      if (!load_word_from_base(R12, R_FP, src_offset, src_sign)) {
        int rr = th_offset_to_reg(src_offset, src_sign);
        ot_check(th_ldr_reg(R12, R_FP, rr, THUMB_SHIFT_DEFAULT,
                            ENFORCE_ENCODING_NONE));
      }

      SValue dest_low = op->dest;
      dest_low.type.t = VT_INT;
      store(R12, &dest_low);

      /* Load and store high word (offset +4 from low word) */
      int high_src_offset = op->src1.c.i + 4;
      int high_src_sign = (high_src_offset < 0);
      if (high_src_sign)
        high_src_offset = -high_src_offset;
      if (!load_word_from_base(R12, R_FP, high_src_offset, high_src_sign)) {
        int rr = th_offset_to_reg(high_src_offset, high_src_sign);
        ot_check(th_ldr_reg(R12, R_FP, rr, THUMB_SHIFT_DEFAULT,
                            ENFORCE_ENCODING_NONE));
      }

      SValue dest_high = op->dest;
      dest_high.type.t = VT_INT;
      dest_high.c.i += 4;
      store(R12, &dest_high);
    } else {
      load(R12, &op->src1);
      src_reg = R12;
      store(src_reg, &op->dest);
    }
  } else {
    src_reg = op->src1.pr0;
    store(src_reg, &op->dest);
  }
}

ST_FUNC void tcc_gen_machine_prolog(int leaffunc, uint64_t used_registers,
                                    int stack_size) {
  thumb_gen_state.function_argument_count = 0;
  uint16_t registers_to_push = 0;
  int registers_count = 0;

  thumb_gen_state.generating_function = 1;
  thumb_gen_state.code_size = 0;
  /* Clear global symbol cache at function start */
  thumb_gen_state.cached_global_sym = NULL;
  thumb_gen_state.cached_global_reg = -1;

  if (!leaffunc) {
    registers_to_push |= (1 << R_LR);
    registers_count++;
  }

  if (stack_size > 0 && !tcc_state->omit_frame_pointer) {
    tcc_state->need_frame_pointer = 1;
    registers_to_push |= (1 << R_FP);
    registers_count++;
  } else {
    tcc_state->need_frame_pointer = 0;
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
  th_sym_t();
  offset_to_args = registers_count * 4;
  if (registers_count > 0) {
    ot_check(th_push(registers_to_push));
  }
  pushed_registers = registers_to_push;

  // allocate stack space for local variables
  allocated_stack_size = stack_size;
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
  if (stack_size > 0) {
    ot_check(th_sub_sp_imm(R_SP, stack_size, FLAGS_BEHAVIOUR_NOT_IMPORTANT,
                           ENFORCE_ENCODING_NONE));
  }
}

ST_FUNC void tcc_gen_machine_epilog(int leaffunc) {
  TRACE("'tcc_gen_machine_epilog'");
  int lr_saved = pushed_registers & (1 << R_LR);

  // restore stack pointer
  if (tcc_state->need_frame_pointer) {
    // restore SP from frame pointer
    ot_check(th_mov_reg(R_SP, R_FP, FLAGS_BEHAVIOUR_NOT_IMPORTANT,
                        THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE, false));
  } else if (allocated_stack_size > 0) {
    // deallocate stack space for local variables
    ot_check(th_add_sp_imm(R_SP, allocated_stack_size,
                           FLAGS_BEHAVIOUR_NOT_IMPORTANT,
                           ENFORCE_ENCODING_NONE));
  }

  if (lr_saved) {
    pushed_registers |= 1 << R_PC;
    pushed_registers &= ~(1 << R_LR);
    ot_check(th_pop(pushed_registers));
    th_literal_pool_generate();
    thumb_gen_state.generating_function = 0;

    return;
  }
  if (pushed_registers > 0) {
    ot_check(th_pop(pushed_registers));
  }
  ot_check(th_bx_reg(R_LR));
  th_literal_pool_generate();
  thumb_gen_state.generating_function = 0;
}

ST_FUNC void tcc_gen_machine_assign_op(TACQuadruple *op) {
  int dest_is_vfp = LS_IS_VFP_REG(op->dest.pr0);
  int src_is_vfp = LS_IS_VFP_REG(op->src1.pr0);
  int is_double = ((op->dest.type.t & VT_BTYPE) == VT_DOUBLE) ||
                  ((op->dest.type.t & VT_BTYPE) == VT_LDOUBLE);
  int dest_spilled = (op->dest.pr0 == -1) ||
                     (op->dest.pr0 == (int8_t)PREG_SPILLED) ||
                     (op->dest.pr0 & PREG_SPILLED);
  printf("DEBUG assign_op: dest.pr0=%d, src1.pr0=%d, src1.r=0x%x, "
         "is_double=%d, dest_spilled=%d\n",
         op->dest.pr0, op->src1.pr0, op->src1.r, is_double, dest_spilled);

  if (dest_spilled) {
    /* Spilled destination - store via integer register */
    printf("DEBUG assign_op: taking spilled dest path\n");
    if (src_is_vfp) {
      int sn = LS_VFP_REG_NUM(op->src1.pr0);
      /* Move VFP to integer register, then store */
      ot_check(th_vmov_gp_sp(R12, sn, 1)); /* VMOV r12, Sn */
      store(R12, &op->dest);
    } else if (op->src1.pr0 >= 0 && !(op->src1.pr0 & PREG_SPILLED)) {
      /* Source is in a valid register - use it directly */
      store(op->src1.pr0, &op->dest);
    } else if ((op->src1.r & VT_VALMASK) == VT_CONST) {
      /* Source is a constant - load to temp register and store */
      if (is_double) {
        /* For double constants, we need to store both words */
        /* Low word is in c.i, high word needs to be extracted from the double
         */
        union {
          double d;
          uint32_t u[2];
        } conv;
        conv.d = op->src1.c.d;
        printf("DEBUG assign_op double const: low=0x%x high=0x%x\n", conv.u[0],
               conv.u[1]);
        /* Store low word */
        load_full_const(R12, conv.u[0], NULL);
        SValue dest_low = op->dest;
        dest_low.type.t = VT_INT;
        store(R12, &dest_low);
        /* Store high word */
        load_full_const(R12, conv.u[1], NULL);
        SValue dest_high = op->dest;
        dest_high.type.t = VT_INT;
        dest_high.c.i += 4;
        store(R12, &dest_high);
      } else {
        load(R12, &op->src1);
        store(R12, &op->dest);
      }
    } else {
      /* Spilled source - load to temp and store */
      if (is_double) {
        /* For doubles, copy both 32-bit words separately */
        int src_offset = op->src1.c.i;

        /* Load and store low word */
        int src_sign = (src_offset < 0);
        int src_abs = src_sign ? -src_offset : src_offset;
        if (!load_word_from_base(R12, R_FP, src_abs, src_sign)) {
          int rr = th_offset_to_reg(src_abs, src_sign);
          ot_check(th_ldr_reg(R12, R_FP, rr, THUMB_SHIFT_DEFAULT,
                              ENFORCE_ENCODING_NONE));
        }

        SValue dest_low = op->dest;
        dest_low.type.t = VT_INT;
        store(R12, &dest_low);

        /* Load and store high word (offset +4 from low word) */
        int high_src_offset = op->src1.c.i + 4;
        int high_src_sign = (high_src_offset < 0);
        int high_src_abs = high_src_sign ? -high_src_offset : high_src_offset;
        if (!load_word_from_base(R12, R_FP, high_src_abs, high_src_sign)) {
          int rr = th_offset_to_reg(high_src_abs, high_src_sign);
          ot_check(th_ldr_reg(R12, R_FP, rr, THUMB_SHIFT_DEFAULT,
                              ENFORCE_ENCODING_NONE));
        }

        SValue dest_high = op->dest;
        dest_high.type.t = VT_INT;
        dest_high.c.i += 4;
        store(R12, &dest_high);
      } else {
        load(R12, &op->src1);
        store(R12, &op->dest);
      }
    }
    return;
  }

  if ((op->src1.r & VT_VALMASK) == VT_CONST) {
    if (dest_is_vfp) {
      int dn = LS_VFP_REG_NUM(op->dest.pr0);
      /* Load constant to integer register, then move to VFP */
      load(R12, &op->src1);
      ot_check(th_vmov_gp_sp(R12, dn, 0)); /* VMOV Sn, r12 */
    } else {
      load(op->dest.pr0, &op->src1);
    }
    return;
  }

  if (op->dest.pr0 == op->src1.pr0)
    return;

  /* Register to register move */
  if (dest_is_vfp && src_is_vfp) {
    int dn = LS_VFP_REG_NUM(op->dest.pr0);
    int sn = LS_VFP_REG_NUM(op->src1.pr0);
    /* VFP to VFP move */
    ot_check(th_vmov_register(dn, sn, 0)); /* VMOV.F32 Sd, Sm */
  } else if (dest_is_vfp && !src_is_vfp) {
    int dn = LS_VFP_REG_NUM(op->dest.pr0);
    /* Integer to VFP */
    ot_check(th_vmov_gp_sp(op->src1.pr0, dn, 0)); /* VMOV Sn, Rm */
  } else if (!dest_is_vfp && src_is_vfp) {
    int sn = LS_VFP_REG_NUM(op->src1.pr0);
    /* VFP to integer */
    ot_check(th_vmov_gp_sp(op->dest.pr0, sn, 1)); /* VMOV Rd, Sn */
  } else {
    /* Integer to integer */
    ot_check(th_mov_reg(op->dest.pr0, op->src1.pr0,
                        FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT,
                        ENFORCE_ENCODING_NONE, false));
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

ST_FUNC int tcc_gen_machine_number_of_registers(void) { return 11; }

ST_FUNC void tcc_gen_machine_load_register(SValue *sv) { load(sv->pr0, sv); }

ST_FUNC void tcc_gen_machine_store_register(SValue *sv) { store(sv->pr0, sv); }

/* Store a register to a stack slot relative to FP.
 * offset is typically negative (local variables below FP). */
ST_FUNC void tcc_gen_machine_store_to_stack(int reg, int offset) {
  int sign = (offset < 0);
  int abs_offset = sign ? -offset : offset;

  printf("DEBUG store_to_stack: reg=%d offset=%d\n", reg, offset);

  /* Try direct STR with immediate offset */
  if (!store_word_to_base(reg, R_FP, abs_offset, sign)) {
    /* Offset too large, use scratch register */
    int rr = th_offset_to_reg(abs_offset, sign);
    ot_check(
        th_str_reg(reg, R_FP, rr, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
  }
}

ST_FUNC void tcc_gen_machine_move_reg(int dest, int src) {
  ot_check(th_mov_reg(dest, src, FLAGS_BEHAVIOUR_NOT_IMPORTANT,
                      THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE, false));
}

ST_FUNC void tcc_gen_machine_func_param_op(TACQuadruple *q, int param_num) {
  /* Detect nested function call: if we see PARAM1 while we already have
   * arguments collected, we're starting a new inner call.
   * Save the outer call's context. */
  if (param_num == 1 && thumb_gen_state.function_argument_count > 0) {
    tcc_gen_machine_save_call_context();
  }
  /* Cache argument - first 4 go in registers, rest go on stack */
  ensure_function_arguments_capacity(thumb_gen_state.function_argument_count +
                                     1);
  thumb_gen_state
      .function_arguments[thumb_gen_state.function_argument_count++] = *q;
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
    load(R12, dest);
    // int r = gv(RC_INT);
    // TRACE("gcall_or_jmp indirect call");
    // ot_check(th_orr_imm(r, r, 1, FLAGS_BEHAVIOUR_NOT_IMPORTANT));
    if (!is_jmp)
      ot_check(th_blx_reg(intr(R12)));
    else
      ot_check(th_bx_reg(intr(R12)));
  }
}

/* Helper to check if a type is 64-bit (double or long long) */
static int is_64bit_type(int t) {
  int bt = t & VT_BTYPE;
  return (bt == VT_DOUBLE || bt == VT_LDOUBLE || bt == VT_LLONG);
}

ST_FUNC void tcc_gen_machine_func_call_op(TACQuadruple *q, int drop_result) {
  int registers_to_push = 0;
  int registers_count = 0;

  /* First pass: calculate register and stack slot assignments for each argument
   * following AAPCS rules:
   * - 32-bit args go in R0-R3 then stack
   * - 64-bit args go in R0:R1 or R2:R3 (must be even-aligned), then stack
   * - 64-bit args on stack must be 8-byte aligned
   */
  int arg_reg_assignments[16]; /* Which register(s) each arg goes to, -1 = stack
                                */
  int arg_stack_offsets[16];   /* Stack offset for stack args */
  int next_reg = 0;            /* Next available register (0-3) */
  int stack_offset = 0;        /* Current stack offset */

  for (int i = 0; i < thumb_gen_state.function_argument_count && i < 16; i++) {
    TACQuadruple *arg = &thumb_gen_state.function_arguments[i];
    int is_64bit = is_64bit_type(arg->src1.type.t);

    if (is_64bit) {
      /* 64-bit value needs even-aligned register pair */
      if (next_reg & 1)
        next_reg++; /* Align to even register */
      if (next_reg <= 2) {
        /* Fits in registers (R0:R1 or R2:R3) */
        arg_reg_assignments[i] = next_reg;
        next_reg += 2;
      } else {
        /* Goes on stack, 8-byte aligned */
        if (stack_offset & 7)
          stack_offset = (stack_offset + 7) & ~7;
        arg_reg_assignments[i] = -1;
        arg_stack_offsets[i] = stack_offset;
        stack_offset += 8;
      }
    } else {
      /* 32-bit value */
      if (next_reg <= 3) {
        arg_reg_assignments[i] = next_reg;
        next_reg++;
      } else {
        arg_reg_assignments[i] = -1;
        arg_stack_offsets[i] = stack_offset;
        stack_offset += 4;
      }
    }
  }

  /* Align total stack to 8 bytes as required by AAPCS */
  int aligned_stack_size = (stack_offset + 7) & ~7;

  /* Reserve stack space for arguments if needed */
  if (aligned_stack_size > 0) {
    ot_check(th_sub_sp_imm(R_SP, aligned_stack_size,
                           FLAGS_BEHAVIOUR_NOT_IMPORTANT,
                           ENFORCE_ENCODING_NONE));
  }

  /* Push stack arguments first */
  for (int i = 0; i < thumb_gen_state.function_argument_count && i < 16; i++) {
    if (arg_reg_assignments[i] != -1)
      continue; /* Skip register args */

    TACQuadruple *arg = &thumb_gen_state.function_arguments[i];
    int is_64bit = is_64bit_type(arg->src1.type.t);
    int offset = arg_stack_offsets[i];

    if (is_64bit) {
      /* Load 64-bit value and store both words */
      /* For doubles in VFP, move to R0:R1 first */
      if (arg->src1.pr0 >= 0 && LS_IS_VFP_REG(arg->src1.pr0)) {
        int dreg = LS_VFP_REG_NUM(arg->src1.pr0) / 2;
        ot_check(th_vmov_2gp_dp(R0, R1, dreg, 1 /* to ARM */));
        ot_check(th_str_imm(R0, R_SP, offset, 6, ENFORCE_ENCODING_NONE));
        ot_check(th_str_imm(R1, R_SP, offset + 4, 6, ENFORCE_ENCODING_NONE));
      } else {
        /* Load 64-bit value from memory to R0:R1 (can't use R12 for 64-bit!) */
        SValue src = arg->src1;
        if (src.pr0 != -1 && (src.pr0 & PREG_SPILLED)) {
          /* For spilled values, set up proper VT_LOCAL | VT_LVAL addressing */
          src.r = VT_LOCAL | VT_LVAL;
        }
        /* Tell load() where to put the high word (R1) */
        src.pr1 = R1;
        load(R0, &src);
        ot_check(th_str_imm(R0, R_SP, offset, 6, ENFORCE_ENCODING_NONE));
        ot_check(th_str_imm(R1, R_SP, offset + 4, 6, ENFORCE_ENCODING_NONE));
      }
    } else {
      SValue src = arg->src1;
      if (src.pr0 != -1 && (src.pr0 & PREG_SPILLED)) {
        /* Spilled to stack - set up VT_LOCAL addressing */
        src.r = VT_LOCAL | VT_LVAL;
        load(R12, &src);
      } else if (src.pr0 != -1 && !(src.pr0 & PREG_SPILLED)) {
        /* Already in a register - just move it */
        if (src.pr0 != R12) {
          ot_check(th_mov_reg(R12, src.pr0, FLAGS_BEHAVIOUR_NOT_IMPORTANT,
                              THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE,
                              false));
        }
      } else {
        /* Not allocated - load from memory/const */
        load(R12, &src);
      }
      ot_check(th_str_imm(R12, R_SP, offset, 6, ENFORCE_ENCODING_NONE));
    }
  }
  /* Load register arguments in reverse order to avoid clobbering */
  for (int i = thumb_gen_state.function_argument_count - 1; i >= 0; --i) {
    if (i >= 16 || arg_reg_assignments[i] == -1)
      continue; /* Skip stack args */

    TACQuadruple *arg = &thumb_gen_state.function_arguments[i];
    int is_64bit = is_64bit_type(arg->src1.type.t);
    int dest_reg = arg_reg_assignments[i];

    if (is_64bit) {
      /* Load 64-bit value into register pair */
      if (arg->src1.pr0 >= 0 && LS_IS_VFP_REG(arg->src1.pr0)) {
        /* Double in VFP register - move to ARM register pair */
        int dreg = LS_VFP_REG_NUM(arg->src1.pr0) / 2;
        ot_check(th_vmov_2gp_dp(dest_reg, dest_reg + 1, dreg, 1 /* to ARM */));
      } else if (arg->src1.pr0 >= 0 && !(arg->src1.pr0 & PREG_SPILLED) &&
                 arg->src1.pr1 >= 0) {
        /* Already in ARM register pair (not spilled) */
        if (arg->src1.pr0 != dest_reg) {
          ot_check(
              th_mov_reg(dest_reg, arg->src1.pr0, FLAGS_BEHAVIOUR_NOT_IMPORTANT,
                         THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE, false));
        }
        if (arg->src1.pr1 != dest_reg + 1) {
          ot_check(th_mov_reg(
              dest_reg + 1, arg->src1.pr1, FLAGS_BEHAVIOUR_NOT_IMPORTANT,
              THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE, false));
        }
      } else {
        /* Load from memory (spilled or not allocated) */
        SValue src = arg->src1;
        printf("DEBUG func_call_op 64bit: pr0=%d pr1=%d r=0x%x type=0x%x "
               "c.i=0x%llx\n",
               src.pr0, src.pr1, src.r, src.type.t,
               (unsigned long long)src.c.i);
        if (src.pr0 != -1 && (src.pr0 & PREG_SPILLED)) {
          /* For spilled values, set up proper VT_LOCAL | VT_LVAL addressing */
          src.r = VT_LOCAL | VT_LVAL;
        }
        /* Tell load() where to put the high word of the 64-bit value.
         * load() uses sv->pr1 to determine the high register destination,
         * so we must set it to dest_reg+1 (e.g., R1 for dest_reg=R0). */
        src.pr1 = dest_reg + 1;
        load(dest_reg, &src);
      }
    } else {
      /* 32-bit value */
      if (arg->src1.pr0 != -1) {
        const int val_loc = arg->src1.r & VT_VALMASK;
        if (val_loc != VT_CONST && val_loc != VT_LVAL && val_loc != VT_LOCAL) {
          if (arg->src1.r & VT_LVAL) {
            load(dest_reg, &arg->src1);
            continue;
          }
        }
        if (arg->src1.pr0 != dest_reg) {
          ot_check(
              th_mov_reg(dest_reg, arg->src1.pr0, FLAGS_BEHAVIOUR_NOT_IMPORTANT,
                         THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE, false));
        }
      } else {
        load(dest_reg, &arg->src1);
      }
    }
  }
  thumb_gen_state.function_argument_count = 0;
  if (tcc_state->text_and_data_separation && q->src1.type.t & VT_EXTERN) {
    // PIC handling
    registers_to_push |= (1 << R9);
    registers_count++;
  }
  if (registers_count % 2 != 0) {
    registers_to_push |= (1 << R12);
    registers_count++;
  }
  if (registers_count > 0)
    ot_check(th_push(registers_to_push));
  gcall_or_jump(0, &q->src1);
  if (registers_count > 0)
    ot_check(th_pop(registers_to_push));

  /* Clean up stack space used for arguments */
  if (aligned_stack_size > 0) {
    ot_check(th_add_sp_imm(R_SP, aligned_stack_size,
                           FLAGS_BEHAVIOUR_NOT_IMPORTANT,
                           ENFORCE_ENCODING_NONE));
  }

  /* Invalidate global symbol cache - LR was clobbered by call */
  thumb_gen_state.cached_global_sym = NULL;
  thumb_gen_state.cached_global_reg = -1;
  if (drop_result) {
    return;
  }
  if (q->dest.pr0 != R0) {

    ot_check(th_mov_reg(q->dest.pr0, R0, FLAGS_BEHAVIOUR_NOT_IMPORTANT,
                        THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE, false));
  }
}

ST_FUNC void tcc_gen_machine_jump_op(TACQuadruple *q) {
  ot_check(th_b_t4(0)); // patch me later
}

ST_FUNC void tcc_gen_machine_conditional_jump_op(TACQuadruple *q) {
  int op = mapcc(q->src1.c.i);
  ot_check(th_b_t3(op, 0)); // patch me later
}

ST_FUNC void tcc_gen_machine_setif_op(TACQuadruple *q) {
  /* Convert comparison flags to 0/1 value in destination register
   * Using IT (If-Then) block:
   *   MOV Rd, #0          ; default to 0 (must NOT set flags!)
   *   IT <cond>           ; If-Then for condition
   *   MOV<cond> Rd, #1    ; set to 1 if condition true
   */
  int op = mapcc(q->src1.c.i);
  int dest = q->dest.pr0;

  /* First set dest to 0 - must use FLAGS_BEHAVIOUR_BLOCK to avoid clobbering
   * the condition flags from the preceding CMP instruction */
  ot_check(th_mov_imm(dest, 0, FLAGS_BEHAVIOUR_BLOCK, ENFORCE_ENCODING_NONE));

  /* IT instruction with single Then (mask = 0x8) */
  ot_check(th_it(op, 0x8));

  /* Conditional MOV to 1 - let encoder choose best size */
  ot_check(th_mov_imm(dest, 1, FLAGS_BEHAVIOUR_NOT_IMPORTANT,
                      ENFORCE_ENCODING_NONE));
}

ST_FUNC void tcc_gen_machine_backpatch_jump(int address, int offset) {
  th_patch_call(address, offset);
}

#endif // TARGET_DEFS_ONLY
