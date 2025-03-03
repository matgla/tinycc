/*
 *  ARMvX-m code generator for TCC 
 *  Uses thumb instruction set
 * 
 *  Based on: 
 *  ARM Thumb 2 instruction functions for TCC
 *  Copyright (c) 2020 Erlend J. Sveen  
 *  from: https://git.erlendjs.no/erlendjs/tinycc/-/blob/arm-thumb/arm-thumb-gen.c
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
#define NB_REGS            13
#else
#define NB_REGS             9
#endif

#ifndef CONFIG_TCC_CPUVER
# define CONFIG_TCC_CPUVER 5
#endif

/* a register can belong to several classes. The classes must be
   sorted from more general to more precise (see gv2() code which does
   assumptions on it). */
#define RC_INT     0x0001 /* generic integer register */
#define RC_FLOAT   0x0002 /* generic float register */
#define RC_R0      0x0004
#define RC_R1      0x0008
#define RC_R2      0x0010
#define RC_R3      0x0020
#define RC_R12     0x0040
#define RC_F0      0x0080
#define RC_F1      0x0100
#define RC_F2      0x0200
#define RC_F3      0x0400
#ifdef TCC_ARM_VFP
#define RC_F4      0x0800
#define RC_F5      0x1000
#define RC_F6      0x2000
#define RC_F7      0x4000
#endif
#define RC_IRET    RC_R0  /* function return: integer register */
#define RC_IRE2    RC_R1  /* function return: second integer register */
#define RC_FRET    RC_F0  /* function return: float register */



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
#define LDOUBLE_SIZE  8
#endif

#ifndef LDOUBLE_SIZE
#define LDOUBLE_SIZE  8
#endif

#ifdef TCC_ARM_EABI
#define LDOUBLE_ALIGN 8
#else
#define LDOUBLE_ALIGN 4
#endif

/* maximum alignment (for aligned attribute support) */
#define MAX_ALIGN     8

#define CHAR_IS_UNSIGNED

#ifdef TCC_ARM_HARDFLOAT
# define ARM_FLOAT_ABI ARM_HARD_FLOAT
#else
# define ARM_FLOAT_ABI ARM_SOFTFP_FLOAT
#endif

#else // TARGET_DEFS_ONLY

#define R0 0
#define R1 1
#define R2 2
#define R3 3 
#define R4 4
#define R5 5 
#define R6 6
#define R7 7 
#define R8 8 
#define R9 9 
#define R10 10 
#define R_FP 11
#define R_IP 12 
#define R_SP 13 
#define R_LR 14 
#define R_PC 15

#ifndef TCC_DEBUG
#define TCC_DEBUG 0 
#endif 

#define TRACE(...) 
#define LOG(...)

#if TCC_DEBUG == 1 || TCC_DEBUG == 2
#undef LOG
#define LOG(...) printf("[INF]: ");printf(__VA_ARGS__);printf("\n")
#endif 

#if TCC_DEBUG == 2
#undef TRACE
#define TRACE(...) printf("[TRC]: ");printf(__VA_ARGS__);printf("\n")
#endif 

#define ceil_div(x, d) ((x + (d - 1)) / d)

#define USING_GLOBALS
#include "tcc.h"

ST_DATA const char * const target_machine_defs = 
    "__arm__\0"
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

typedef enum {
    FLAGS_BEHAVIOUR_NOT_IMPORANT = 0,
    FLAGS_BEHAVIOUR_SET = 1,
    FLAGS_BEHAVIOUR_BLOCK = 2,
} flags_behaviour;

flags_behaviour g_setflags = FLAGS_BEHAVIOUR_SET; 

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

static int two2mask(int a,int b) {
  if (!CHECK_R(a) || !CHECK_R(b))
    tcc_error("compiler error! registers %i,%i is not valid",a,b);
  return (reg_classes[a]|reg_classes[b])&~(RC_INT|RC_FLOAT);
}


static uint32_t mapcc(int cc)
{
  switch(cc)
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


struct avail_regs {
  signed char avail[3]; /* 3 holes max with only float and double alignments */
  int first_hole; /* first available hole */
  int last_hole; /* last available hole (none if equal to first_hole) */
  int first_free_reg; /* next free register in the sequence, hole excluded */
};

/* Find suitable registers for a VFP Co-Processor Register Candidate (VFP CPRC
   param) according to the rules described in the procedure call standard for
   the ARM architecture (AAPCS). If found, the registers are assigned to this
   VFP CPRC parameter. Registers are allocated in sequence unless a hole exists
   and the parameter is a single float.

   avregs: opaque structure to keep track of available VFP co-processor regs
   align: alignment constraints for the param, as returned by type_size()
   size: size of the parameter, as returned by type_size() */
int assign_vfpreg(struct avail_regs *avregs, int align, int size)
{
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
    int start; /* first reg or addr used depending on the class */
    int end; /* last reg used or next free addr depending on the class */
    SValue *sval; /* pointer to SValue on the value stack */
    struct param_plan *prev; /*  previous element in this class */
};

struct plan {
    struct param_plan *pplans; /* array of all the param plans */
    struct param_plan *clsplans[NB_CLASSES]; /* per class lists of param plans */
    int nb_plans;
};

static void add_param_plan(struct plan* plan, int cls, int start, int end, SValue *v)
{
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
static int assign_regs(int nb_args, int float_abi, struct plan *plan, int *todo)
{
  int i, size, align;
  int ncrn /* next core register number */, nsaa /* next stacked argument address*/;
  struct avail_regs avregs = {{0}};

  ncrn = nsaa = 0;
  *todo = 0;

  for(i = nb_args; i-- ;) {
    int j, start_vfpreg = 0;
    CType type = vtop[-i].type;
    type.t &= ~VT_ARRAY;
    size = type_size(&type, &align);
    size = (size + 3) & ~3;
    align = (align + 3) & ~3;
    switch(vtop[-i].type.t & VT_BTYPE) {
      case VT_STRUCT:
      case VT_FLOAT:
      case VT_DOUBLE:
      case VT_LDOUBLE:
      if (float_abi == ARM_HARD_FLOAT) {
        int is_hfa = 0; /* Homogeneous float aggregate */

        if (is_float(vtop[-i].type.t)
            || (is_hfa = is_hgen_float_aggr(&vtop[-i].type))) {
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
      ncrn = (ncrn + (align-1)/4) & ~((align/4) - 1);
      if (ncrn + size/4 <= 4 || (ncrn < 4 && start_vfpreg != -1)) {
        /* The parameter is allocated both in core register and on stack. As
	 * such, it can be of either class: it would either be the last of
	 * CORE_STRUCT_CLASS or the first of STACK_CLASS. */
        for (j = ncrn; j < 4 && j < ncrn + size / 4; j++)
          *todo|=(1<<j);
        add_param_plan(plan, CORE_STRUCT_CLASS, ncrn, j, &vtop[-i]);
        ncrn += size/4;
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
}

static int regmask(int r) 
{
  return reg_classes[r]&~(RC_INT|RC_FLOAT);
}

/* 
 * Write 2 - byte Thumb instruction
 * current write position must be 16-bit aligned
 */
void o(unsigned int i)
{
  const uint16_t instruction = i & 0xffff; 
  const int ind1 = ind + 2;
  TRACE("o: 0x%.4x pc: 0x%x", i, ind);
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
  cur_text_section->data[ind++] = i&255;
  cur_text_section->data[ind++] = i >> 8;
}

static void load_full_const(int r, uint32_t imm, struct Sym *sym);

// Thumb instruction set 
static int th_offset_to_reg(int off, int sign);
static uint32_t th_pack_const(uint32_t imm);


static void th_nop()
{
  o(0xbf00);
}

static uint32_t th_packimm_10_11_0(uint32_t imm)
{
  const uint32_t imm11 = (imm >> 1) & 0x7ff;
  const uint32_t imm10 = (imm >> 12) & 0x3ff;
  const uint32_t s = (imm >> 24) & 1;
  const uint32_t j1 = ~((imm >> 23) ^ s) & 1;
  const uint32_t j2 = ~((imm >> 22) ^ s) & 1;
  return (s << 26) | (imm10 << 16) | (j1 << 13) | (j2 << 11) | imm11;
}

static void th_bx_reg(uint16_t rm)
{
  o(0x4700 | ((rm & 0xf) << 3));
}

static void th_bl_t1(uint32_t imm)
{
  const uint32_t packed = th_packimm_10_11_0(imm) | 0xF000D000;
  o(packed >> 16);
  o(packed & 0xffff);
}

static void th_blx_reg(uint16_t rm)
{
  o(0x4780 | (rm << 3));
}

static void th_b_t1(uint16_t cond, uint16_t imm8)
{
  o(0xd000 | ((cond & 0xf) << 8) | (imm8 & 0xff));
}

static void th_b_t2(uint16_t imm11)
{
  o(0xe000 | (imm11 & 0x7ff));
}

static uint32_t th_encbranch_b_t3(uint32_t imm)
{
  const uint32_t s = (imm >> 19) & 1;
  const uint32_t imm6 = (imm >> 11) & 0x3f;
  const uint32_t imm11 = imm & 0x7ff;
  const uint32_t j2 = (imm >> 18) & 1;
  const uint32_t j1 = (imm >> 17) & 1;
  const uint32_t a = (s << 10) | imm6;
  const uint32_t b = (j1 << 13) | (j2 << 11) | imm11;
  return (a << 16) | b;
}

static void th_b_t3(uint16_t op, uint16_t imm)
{
  const uint32_t enc = th_encbranch_b_t3(imm);
  o(0xf000 | (op << 6) | (enc >> 16));
  o(0x8000 | enc);
}


static void th_b_t4(int32_t imm)
{
  uint32_t packed = 0;
  if (imm > 16777215 || imm < -16777215)
    tcc_error("compiler_error: th_b_t4 too far address: 0x%x\n", imm);

  packed = th_packimm_10_11_0(imm) | 0xf0009000;
  o(packed >> 16);
  o(packed & 0xffff);
}

// all t32 arch 
static void th_mov_reg(uint16_t rd, uint16_t rm)
{
  const uint16_t D = (rd >> 3) & 1;
  o(0x4600 | (D << 7) | (rm << 3) | (rd & 0x7));
}

// 1 - if mov can be used 
// 0 - if mov is not available for provided arguments
static int th_mov_imm(uint16_t rd, uint16_t imm)
{
  if (rd <= 7 && imm <= 255)
  {
    o(0x2000 | (rd << 8) | imm);
    return 1;
  }
#ifndef TCC_TARGET_ARM_ARCHV6M 
  else if (imm <= 0xffff && rd != R_SP && rd != R_PC)
  {
    const uint16_t i = (imm >> 11) & 1;
    const uint16_t imm4 = (imm >> 12) & 0xf;
    const uint16_t imm3 = (imm >> 8) & 0x7;
    o(0xf240 | (i << 10) | imm4);
    o((imm3 << 12) | (rd << 8) | (imm & 0xff));
    return 1;
  }
  else if (imm <= 0xffff && rd != R_SP && rd != R_PC)
  {
    const uint32_t enc = th_pack_const(imm);
    const uint32_t a = enc >> 16;
    const uint32_t b = enc & 0xffff;

    o(0xf04f | a);
    o(b | ((rd & 0xf) << 8));
    return 1;
  }
#endif
  return 0;
}

static int th_generic_op_imm_with_status(uint16_t op, uint16_t rd, uint16_t rn, uint16_t imm, flags_behaviour setflags)
{
#ifndef TCC_TARGET_ARM_ARCHV6M 
  const uint32_t packed = th_pack_const(imm);
  if (packed || imm == 0)
  {
    const uint32_t A = packed >> 16;
    const uint32_t B = packed & 0xffff;
    o(op | ((setflags == FLAGS_BEHAVIOUR_SET) << 4) | rn | A);
    o(rd << 8 | B);
    return 1;
  }
#endif
  return 0;
}

static int th_generic_op_imm(uint16_t op, uint16_t rd, uint16_t rn, uint16_t imm)
{
  return th_generic_op_imm_with_status(op, rd, rn, imm, FLAGS_BEHAVIOUR_NOT_IMPORANT);
}

static void th_add_reg(uint16_t rd, uint16_t rn, uint16_t rm)
{
  if ((rd == R_PC) && (rm == R_PC))
  {
    tcc_error("compiler_error: 'th_add_reg', PC can't be used as rdn and rm\n");
  }
  if (rd == rn)
  {
    // T2
    const uint16_t DN = (rd >> 3) & 1;
    o(0x4400 | (DN << 7) | ((rm & 0xf) << 3) | (rd & 0x7));
  }
  else if (rm < 8 && rd < 8 && rn < 8)
  {
    // T1 
    o(0x1800 | (rm << 6) | (rn << 3) | (rd));
  }
  #ifndef TCC_TARGET_ARM_ARCHV6M 
  else 
  {
    o(0xeb00 | rn);
    o((rd << 8) | rm);
  }
  #else 
  else 
  {
    tcc_error("compiler_error: 'th_add_reg', cannot encode for rd: %d, rn: %d, rm: %d\n", rd, rn, rm);
  }
  #endif  
}

static int th_add_imm(uint16_t rd, uint16_t rn, uint16_t imm)
{
  if (rd == rn && rd < 8 && imm <= 255)
  {
    o(0x3000 | (rd << 8) | imm);
    return 1;
  }
#ifndef TCC_TARGET_ARM_ARCHV6M
  else if (rn != R_SP && rn != R_PC && rd != R_PC && rd != R_SP && imm <= 4095)
  {
    const uint16_t i = (imm >> 11) & 1;
    const uint16_t imm3 = (imm >> 8) & 7;
    o(0xf200 | (i << 10) | rn);
    o((imm3 << 12) | (rd << 8) | (imm & 0xff));
    return 1;
  }
  else if (rd != R_SP && rn != R_PC)
  {
    return th_generic_op_imm(0xf100, rd, rn, imm);
  }
#endif
  return 0;
}

static int th_bic_imm(uint16_t rd, uint16_t rn, uint16_t imm)
{
 #ifndef TCC_TARGET_ARM_ARCHV6M 
  if (rd != R_SP && rd != R_PC && rn != R_SP && rd != R_PC)
  {
    const uint32_t packed = th_pack_const(imm);
    if (packed || imm == 0)
    {
      const uint32_t A = packed >> 16;
      const uint32_t B = packed & 0xffff;
      o(0xf020 | rn | A);
      o(rd << 8 | B);
      return 1;
    }
  }
 #endif
  return 0;
}

static int th_and_imm(uint16_t rd, uint16_t rn, uint16_t imm)
{
  if (!th_generic_op_imm(0xf000, rd, rn, imm))
  {
    return th_bic_imm(rd, rn, ~imm);
  }
  return 0;
}

static void th_and_reg(uint16_t rd, uint16_t rn, uint16_t rm)
{
  if (rd == rn && rm < 8 && rn < 8) 
  {
    o(0x4000 | (rm << 3) | rd);
  }
#ifndef TCC_TARGET_ARM_ARCHV6M 
  else if (rd != R_SP && rn != R_SP && rn != R_PC && rm != R_SP && rm != R_PC)
  {
    o(0xea00 | rn);
    o((rd << 8) | rm);
  }
#endif 
  else tcc_error("compiler_error: unsupported 'th_and_reg' rd: %d, rn: %d, rm: %d", rd, rn, rm);
}




static int th_xor_reg(uint16_t rd, uint16_t rn, uint16_t rm)
{
  if (rd != rn && rm < 8 && rn < 8) o(0x4040 | (rm << 3) | rd);
#ifndef TCC_TARGET_ARM_ARCHV6M
  else if (rd != R_SP && rd != R_PC && rn != R_SP && rn != R_PC)
  {
    o(0xea80 | rn);
    o((rd << 8) | rm);
    return 1;
  }
#endif
  return 0;
}

static int th_xor_imm(uint16_t rd, uint16_t rn, uint16_t imm)
{
  return th_generic_op_imm(0xf080, rd, rn, imm); 
}

static void th_rsb_reg(uint16_t rd, uint16_t rn, uint16_t rm)
{
#ifndef TCC_TARGET_ARM_ARCHV6M
  if (rd != R_SP && rd != R_PC && rn != R_SP && rn != R_PC && rm != R_SP && rn != R_SP)
  {
    o(0xebc0 | rn);
    o((rd << 8) | rm);
  }
#endif
  else tcc_error("compiler_error: unsupported 'th_rsb_reg' rd: %d, rn: %d, rm: %d", rd, rn, rm);
}



static void th_sub_reg(uint16_t rd, uint16_t rn, uint16_t rm)
{
  if (rd < 8 && rm < 8 && rn < 8) o(0x1a00 | (rm << 6) | (rn << 3) | rd);
#ifndef TCC_TARGET_ARM_ARCHV6M
  else if (rd != R_SP && rd != R_PC && rn != R_SP && rn != R_PC)
  {
    o(0xeba0 | rn);
    o((rd << 8) | rm);
  }
#endif
  else tcc_error("compiler_error: unsupported 'th_sub_reg' rd: %d, rn: %d, rm: %d", rd, rn, rm);
}

static void th_adc_reg(uint16_t rd, uint16_t rn, uint16_t rm)
{
  if (rd == rn && rm < 8 && rn < 8) o(0x4140 | (rm << 3) | rd);
#ifndef TCC_TARGET_ARM_ARCHV6M
  else if (rd != R_SP && rd != R_PC && rn != R_SP && rn != R_PC && rm != R_SP && rm != R_PC)
  {
    o(0xeb40 | rn);
    o((rd << 8) | rm);
  }
#endif
  else tcc_error("compiler_error: unsupported 'th_adc_reg' rd: %d, rn: %d, rm: %d", rd, rn, rm);
}

static int th_adc_imm(uint16_t rd, uint16_t rn, uint16_t imm)
{
  if (rn != R_SP && rn != R_PC && rd != R_SP && rn != R_PC)
  {
    return th_generic_op_imm(0xf140, rd, rn, imm);
  }
  return 0;
}

static int th_sbc_imm(uint16_t rd, uint16_t rn, uint16_t imm)
{
  if (rn != R_SP && rn != R_PC && rd != R_SP && rn != R_PC)
  {
    return th_generic_op_imm(0xf160, rd, rn, imm);
  }
  return 0;
}

static int th_orr_imm(uint16_t rd, uint16_t rn, uint16_t imm)
{
  if (rn != R_SP && rd != R_SP && rn != R_PC)
  {
    return th_generic_op_imm(0xf040, rd, rn, imm);
  }
  return 0;
}

static void th_sbc_reg(uint16_t rd, uint16_t rn, uint16_t rm)
{
  if (rd == rn && rm < 8 && rn < 8) o(0x4180 | (rm << 3) | rd);
#ifndef TCC_TARGET_ARM_ARCHV6M
  else if (rd != R_SP && rd != R_PC && rn != R_SP && rn != R_PC && rm != R_SP && rm != R_PC)
  {
    o(0xeb70 | rn);
    o((rd << 8) | rm);
  }
#endif
  else tcc_error("compiler_error: unsupported 'th_sbc_reg' rd: %d, rn: %d, rm: %d", rd, rn, rm);
}

static void th_cmp_reg(uint16_t rn, uint16_t rm)
{
  if (rm < 8 && rn < 8) o(0x4280 | (rm << 3) | rn);
#ifndef TCC_TARGET_ARM_ARCHV6M
  else if (!(rm < 8 && rn < 8) && rm != R_PC && rn != R_PC)
  {
    const uint16_t N = (rn >> 3) & 0x1;
    o(0x4500 | (N << 7) | (rm << 3) | (rn & 0x7));
  }
#endif
  else tcc_error("compiler_error: unsupported 'th_cmp_reg' rn: %d, rm: %d", rn, rm);
}

static void th_orr_reg(uint16_t rd, uint16_t rn, uint16_t rm)
{
  if (rd == rn && rm < 8 && rn < 8) o(0x4300 | (rm << 3) | rd);
#ifndef TCC_TARGET_ARM_ARCHV6M
  else if (rd != R_SP && rd != R_PC && rn != R_PC && rm != R_SP && rm != R_PC)
  {
    o(0xea40 | rn);
    o((rd << 8) | rm);
  }
#endif
  else tcc_error("compiler_error: unsupported 'th_orr_reg' rd: %d, rn: %d, rm: %d", rd, rn, rm);
}



static int th_sub_imm(uint16_t rd, uint16_t rn, uint16_t imm)
{
  if (rd == rn && imm <= 255 && rd < 8)
  {
    // T2
    o(0x3800 | (rd << 8) | imm);
    return 1;
  }
  else if (rd < 8 && rn < 8 && imm <= 7)
  {
    // T1 
    o(0x1e00 | (imm << 6) | (rn << 3) | rd);
    return 1;
  }
#ifndef TCC_TARGET_ARM_ARCHV6M
  else if (rd != R_SP && rd != R_PC && imm <= 0xfff)
  {
    // T4
    const uint16_t i = imm >> 11;
    const uint16_t imm3 = (imm >> 8) & 0x7;
    o(0xf2a0 | (i << 10) | (rn & 0xf));
    o((imm3 << 12) | ((rd & 0xf) << 8) | (imm & 0xff));
    return 1;
  }
  else if (rd != 13 && rd != 15)
  {
    const uint32_t enc = th_pack_const(imm);
    if (enc || imm == 0)
    {
      const uint16_t a = enc >> 16;
      const uint16_t b = enc & 0xffff;
      o(0xf1a0 | (rn & 0xf) | a);
      o(b | (rd & 0xf) << 8);
      return 1;
    }
  }
#endif
  return 0;
}


static void th_push(uint16_t regs)
{
  // T1 encoding R0-R7 + LR only, all armv-m
  // (T2 in armv8-m - inconsistent naming in reference manual)
  if (!(regs & 0xdf00))
  {
    const uint16_t lr = (regs >> 14) & 1;
    o(0xb400 | (lr << 8) | (regs & 0xff));
    return; 
  }
  // T2 encoding R0-R12 + LR only, > armv7-m
  // (T1 in armv8-m - inconsistent naming in reference manual)
  #if defined(TCC_TARGET_ARM_ARCHV8M) || defined(TCC_TARGET_ARM_ARCHV7M)
  if (!(regs & 0xa000))
  {
    o(0xe92d);
    o(regs);
    return;
  }
  #endif
  tcc_error("compiler_error: 'th_push' contains illegal registers: 0x%x\n", regs);
}

static void th_ldrsh_imm(uint32_t rt, uint32_t rn, uint32_t imm, uint32_t puw)
{
  #ifndef TCC_TARGET_ARM_ARCHV6M
  // puw == 6 means positive offset on rn, so T1 encoding can be used
  if (rt != R_SP && imm <= 4095 && puw == 6)
  {
    o(0xf9b0 | ((rn & 0xf)));
    o(((rt & 0xf) << 12) | imm);
    return;
  }
  else if (rt != R_SP && imm <= 255)
  {
    o(0xf930 | (rn & 0xf));
    o(0x0800 | ((rt & 0xf) << 12) | (puw << 8) | imm);
    return;
  } 
  #endif
  tcc_error("compiler_error: 'th_ldrsh_imm' can't be used with rt: %d, rn: %d, imm: 0x%x, puw: 0x%x\n", rt, rn, imm, puw);
}

static void th_ldrsh_reg(uint32_t rt, uint32_t rn, uint32_t rm)
{
  // puw == 6 means positive offset on rn, so T1 encoding can be used
  if (rm < 8 && rt < 8 && rn < 8)
  {
    o(0x5e00 | (rm << 6) | (rn << 3) | rt);
  }
#ifndef TCC_TARGET_ARM_ARCHV6M
  else if (rt != R_SP && rm != R_SP && rn != R_SP)
  {
    o(0xf930 | (rn & 0x0f));
    o(((rt & 0xf) << 12) | (rm & 0xf));
  }
#endif
  else 
  {
    tcc_error("compiler_error: 'th_ldrsh_reg' can't be used with rt: %d, rn: %d, rm: %d\n", rt, rn, rm);
  }
}

static void th_ldrh_imm(uint16_t rt, uint16_t rn, uint16_t imm, uint16_t puw)
{
  // T1 encoding, on armv6-m this one is the only one available
  if (puw == 6 && rn < 8 && rt < 8 && imm <= 62 && !(imm & 1))
  {
    // imm[0] is enforced to be 0, and sould be divided by 2, thus offset is 5
    o(0x8800 | (imm << 5) | (rn << 3) | rt); 
  }
#ifndef TCC_TARGET_ARM_ARCHV6M
  else if (puw == 6 && rt != R_SP && imm <= 4095)
  {
    o(0xf8b0 | (rn & 0xf));
    o((rt & 0xf) << 12 | imm);
  }
  else if (rt != R_SP && imm <= 255)
  {
    o(0xf830 | (rn & 0xf));
    o(0x0800 | ((rt & 0xf) << 12) | ((puw & 0x7) << 8) | imm);
  }
#endif 
  else 
  {
    tcc_error("compiler_error: 'th_ldsh_imm' can't be used with rt: %d, rn: %d, imm: 0x%x, puw: 0x%x\n", rt, rn, imm, puw);
  }
}

static void th_ldrh_reg(uint32_t rt, uint32_t rn, uint32_t rm)
{
  // puw == 6 means positive offset on rn, so T1 encoding can be used
  if (rm < 8 && rt < 8 && rn < 8)
  {
    o(0x5a00 | (rm << 6) | (rn << 3) | rt);
  }
#ifndef TCC_TARGET_ARM_ARCHV6M
  else if (rt != R_SP && rm != R_SP && rm != R_PC)
  {
    o(0xf830 | (rn & 0xf));
    o(((rt & 0xf) << 12) | (rm & 0xf));
  }
#endif
  else 
  {
    tcc_error("compiler_error: 'th_ldrh_reg' can't be used with rt: %d, rn: %d, rm: %d\n", rt, rn, rm);
  }
}

static void th_ldrsb_imm(uint32_t rt, uint32_t rn, uint32_t imm, uint32_t puw)
{
#ifndef TCC_TARGET_ARM_ARCHV6M
  // puw == 6 means positive offset on rn, so T1 encoding can be used
  if (rt != R_SP && imm <= 4095 && puw == 6)
  {
    o(0xf990 | ((rn & 0xf)));
    o(((rt & 0xf) << 12) | imm);
    return;
  }
  else if (rt != R_SP && imm <= 255)
  {
    o(0xf910 | (rn & 0xf));
    o(0x0800 | ((rt & 0xf) << 12) | (puw << 8) | imm);
    return;
  }
#endif
  tcc_error("compiler_error: 'th_ldrsb_imm' can't be used with rt: %d, rn: %d, imm: 0x%x, puw: 0x%x\n", rt, rn, imm, puw);
}


static void th_ldrsb_reg(uint32_t rt, uint32_t rn, uint32_t rm)
{
  // puw == 6 means positive offset on rn, so T1 encoding can be used
  if (rm < 8 && rt < 8 && rn < 8)
  {
    o(0x5600 | (rm << 6) | (rn << 3) | rt);
  }
#ifndef TCC_TARGET_ARM_ARCHV6M
  else if (rt != R_SP && rn != R_SP && rm != R_SP)
  {
    o(0xf910 | (rn & 0xf));
    o(((rt & 0xf) << 12) | (rm & 0xf));
  }
#endif
  else 
  {
    tcc_error("compiler_error: 'th_ldrsb_reg' can't be used with rt: %d, rn: %d, rm: %d\n", rt, rn, rm);
  }
}

static void th_ldrb_imm(uint16_t rt, uint16_t rn, uint16_t imm, uint16_t puw)
{
  // T1 encoding, on armv6-m this one is the only one available
  if (puw == 6 && rn < 8 && rt < 8 && imm <= 62 && !(imm & 1))
  {
    // imm[0] is enforced to be 0, and sould be divided by 2, thus offset is 5
    o(0x7800 | (imm << 5) | (rn << 3) | rt); 
  }
#ifndef TCC_TARGET_ARM_ARCHV6M
  else if (puw == 6 && rt != R_SP && imm <= 4095)
  {
    o(0xf890 | (rn & 0xf));
    o((rt & 0xf) << 12 | imm);
  }
  else if (rt != R_SP && imm <= 255)
  {
    o(0xf810 | (rn & 0xf));
    o(0x0800 | ((rt & 0xf) << 12) | ((puw & 0x7) << 8) | imm);
  }
#endif 
  else 
  {
    tcc_error("compiler_error: 'th_ldsb_imm' can't be used with rt: %d, rn: %d, imm: 0x%x, puw: 0x%x\n", rt, rn, imm, puw);
  }
}

static void th_ldrb_reg(uint32_t rt, uint32_t rn, uint32_t rm)
{
  // puw == 6 means positive offset on rn, so T1 encoding can be used
  if (rm < 8 && rt < 8 && rn < 8)
  {
    o(0x5c00 | (rm << 6) | (rn << 3) | rt);
  }
#ifndef TCC_TARGET_ARM_ARCHV6M 
  else if (rt != R_SP && rm != R_SP && rm != R_PC)
  {
    o(0xf810 | (rn & 0xf));
    o(((rt & 0xf) << 12) | (rm & 0xf));
  }
#endif
  else 
  {
    tcc_error("compiler_error: 'th_ldrh_reg' can't be used with rt: %d, rn: %d, rm: %d\n", rt, rn, rm);
  }
}

static void th_ldr_imm(uint32_t rt, uint32_t rn, uint32_t imm, uint32_t puw)
{
  // puw == 6 means positive offset on rn, so T1 encoding can be used
  if (puw == 6 && rn < 8 && rt < 8 && imm <= 124 && !(imm & 3))
  {
    // imm[0] is enforced to be 0, and sould be divided by 4, thus offset is 4
    o(0x6800 | (imm << 4) | (rn << 3) | rt); 
  }
  else if (puw == 6 && rn == R_SP && rt < 8 && imm <= 1020)
  {
    o(0x9800 | (rt << 8) | (imm >> 2));
  }
#ifndef TCC_TARGET_ARM_ARCHV6M
  else if (puw == 6 && imm <= 4095)
  {
    o(0xf8d0 | (rn & 0xf));
    o((rt & 0xf) << 12 | imm);
  }
  else if (imm <= 255)
  {
    o(0xf850 | (rn & 0xf));
    o(0x0800 | ((rt & 0xf) << 12) | ((puw & 0x7) << 8) | imm);
  }
#endif 
  else 
  {
    tcc_error("compiler_error: 'th_ldr_imm' can't be used with rt: %d, rn: %d, imm: 0x%x, puw: 0x%x\n", rt, rn, imm, puw);
  }
}

static void th_ldr_reg(uint32_t rt, uint32_t rn, uint32_t rm)
{
  if (rm < 8 && rt < 8 && rn < 8)
  {
    o(0x5800 | (rm << 6) | (rn << 3) | rt);
  }
#ifndef TCC_TARGET_ARM_ARCHV6M 
  else if (rt != R_SP && rm != R_SP && rm != R_PC)
  {
    o(0xf850 | (rn & 0xf));
    o(((rt & 0xf) << 12) | (rm & 0xf));
  }
#endif
  else 
  {
    tcc_error("compiler_error: 'th_ldr_reg' can't be used with rt: %d, rn: %d, rm: %d\n", rt, rn, rm);
  }
}

static void th_ldr_literal(uint16_t rt, uint16_t imm, uint16_t add)
{
  if (rt < 8 && imm <= 1020) 
  {
    o(0x4800 | (rt << 8) | imm >> 2);
  }
#ifndef TCC_TARGET_ARM_ARCHV6M 
  else if (rt != R_PC && imm <= 0xffff) 
  {

    o(0xf85f | ((add & 1) << 7));
    o(((rt & 0xf) << 12) | imm);
  }
#endif
  else 
  {
    tcc_error("compiler_error: can't generate for rt: %d, imm: 0x%x\n", rt, imm);
  }
}


// STR 

static int th_strh_imm(uint16_t rt, uint16_t rn, uint16_t imm, uint16_t puw)
{
  // T1 encoding, on armv6-m this one is the only one available
  if (puw == 6 && rn < 8 && rt < 8 && imm <= 62 && !(imm & 1))
  {
    // imm[0] is enforced to be 0, and sould be divided by 2, thus offset is 5
    o(0x8000 | (imm << 5) | (rn << 3) | rt); 
    return 1;
  }
#ifndef TCC_TARGET_ARM_ARCHV6M
  else if (puw == 6 && rt != R_SP && imm <= 4095)
  {
    o(0xf8a0 | (rn & 0xf));
    o((rt & 0xf) << 12 | imm);
    return 1;
  }
  else if (rt != R_SP && imm <= 255)
  {
    o(0xf820 | (rn & 0xf));
    o(0x0800 | ((rt & 0xf) << 12) | ((puw & 0x7) << 8) | imm);
    return 1;
  }
#endif 
  return 0;
}

static void th_strh_reg(uint32_t rt, uint32_t rn, uint32_t rm)
{
  // puw == 6 means positive offset on rn, so T1 encoding can be used
  if (rm < 8 && rt < 8 && rn < 8)
  {
    o(0x5200 | (rm << 6) | (rn << 3) | rt);
  }
#ifndef TCC_TARGET_ARM_ARCHV6M
  else if (rt != R_SP && rm != R_SP && rm != R_PC)
  {
    o(0xf820 | (rn & 0xf));
    o(((rt & 0xf) << 12) | (rm & 0xf));
  }
#endif
  else 
  {
    tcc_error("compiler_error: 'th_strh_reg' can't be used with rt: %d, rn: %d, rm: %d\n", rt, rn, rm);
  }
}

static int th_strb_imm(uint16_t rt, uint16_t rn, uint16_t imm, uint16_t puw)
{
  // T1 encoding, on armv6-m this one is the only one available
  if (puw == 6 && rn < 8 && rt < 8 && imm <= 62 && !(imm & 1))
  {
    // imm[0] is enforced to be 0, and sould be divided by 2, thus offset is 5
    o(0x7000 | (imm << 5) | (rn << 3) | rt); 
    return 1;
  }
#ifndef TCC_TARGET_ARM_ARCHV6M
  else if (puw == 6 && rt != R_SP && imm <= 4095)
  {
    o(0xf880 | (rn & 0xf));
    o((rt & 0xf) << 12 | imm);
    return 1;
  }
  else if (rt != R_SP && imm <= 255)
  {
    o(0xf800 | (rn & 0xf));
    o(0x0800 | ((rt & 0xf) << 12) | ((puw & 0x7) << 8) | imm);
    return 1;
  }
#endif 
  return 0;
}

static void th_strb_reg(uint32_t rt, uint32_t rn, uint32_t rm)
{
  // puw == 6 means positive offset on rn, so T1 encoding can be used
  if (rm < 8 && rt < 8 && rn < 8)
  {
    o(0x5400 | (rm << 6) | (rn << 3) | rt);
  }
#ifndef TCC_TARGET_ARM_ARCHV6M 
  else if (rt != R_SP && rm != R_SP && rm != R_PC)
  {
    o(0xf800 | (rn & 0xf));
    o(((rt & 0xf) << 12) | (rm & 0xf));
  }
#endif
  else 
  {
    tcc_error("compiler_error: 'th_strb_reg' can't be used with rt: %d, rn: %d, rm: %d\n", rt, rn, rm);
  }
}

static int th_str_imm(uint32_t rt, uint32_t rn, uint32_t imm, uint32_t puw)
{
  // puw == 6 means positive offset on rn, so T1 encoding can be used
  if (puw == 6 && rn < 8 && rt < 8 && imm <= 124 && !(imm & 3))
  {
    // imm[0] is enforced to be 0, and sould be divided by 4, thus offset is 4
    o(0x6000 | (imm << 4) | (rn << 3) | rt); 
    return 1;
  }
  else if (puw == 6 && rn == R_SP && rt < 8 && imm <= 1020)
  {
    o(0x9000 | (rt << 8) | (imm >> 2));
    return 1;
  }
#ifndef TCC_TARGET_ARM_ARCHV6M
  else if (puw == 6 && imm <= 4095)
  {
    o(0xf8c0 | (rn & 0xf));
    o((rt & 0xf) << 12 | imm);
    return 1;
  }
  else if (imm <= 255)
  {
    o(0xf840 | (rn & 0xf));
    o(0x0800 | ((rt & 0xf) << 12) | ((puw & 0x7) << 8) | imm);
    return 1;
  }
#endif 
  return 0;
}

static void th_str_reg(uint32_t rt, uint32_t rn, uint32_t rm)
{
  if (rm < 8 && rt < 8 && rn < 8)
  {
    o(0x5000 | (rm << 6) | (rn << 3) | rt);
  }
#ifndef TCC_TARGET_ARM_ARCHV6M 
  else if (rt != R_SP && rm != R_SP && rm != R_PC)
  {
    o(0xf840 | (rn & 0xf));
    o(((rt & 0xf) << 12) | (rm & 0xf));
  }
#endif
  else 
  {
    tcc_error("compiler_error: 'th_str_reg' can't be used with rt: %d, rn: %d, rm: %d\n", rt, rn, rm);
  }
}

static void th_mul(uint16_t rd, uint16_t rn, uint16_t rm)
{
  if (rd == rm && rd < 8 && rn < 8)
  {
    o(0x4340 | (rn << 3) | rm);
  }
#ifndef TCC_TARGET_ARM_ARCHV6M
  else 
  {
    o(0xfb00 | rn);
    o(0xf000 | (rd << 8) | rm);
  }
#endif
}

static void th_umull(uint32_t rdlo, uint16_t rdhi, uint16_t rn, uint16_t rm)
{
#ifndef TCC_TARGET_ARM_ARCHV6M
  o(0xfba0 | rn);
  o((rdlo << 12) | (rdhi << 8) | rm);
#endif
}

static void th_udiv(uint16_t rd, uint16_t rn, uint16_t rm)
{
#ifndef TCC_TARGET_ARM_ARCHV6M
  o(0xfbb0 | rn);
  o(0xf0f0 | (rd << 8) | rm);
#endif
}

static void th_sdiv(uint16_t rd, uint16_t rn, uint16_t rm)
{
#ifndef TCC_TARGET_ARM_ARCHV6M
  o(0xfb90 | rn);
  o(0xf0f0 | (rd << 8) | rm);
#endif
}



static int th_ldr_literal_estimate(uint16_t rt, uint16_t imm)
{
  if (rt < 8 && !(imm & 3) && imm <= 0x3ff) return 2;
#ifndef TCC_TARGET_ARM_ARCHV6M 
  else if (imm <= 0xfff) return 4;
#endif
  return 0;
}

static uint32_t th_pack_const(uint32_t imm)
{
  // 00000000 00000000 00000000 abcdefgh
  if (!(imm & 0xffffff00))
  {
    return imm;
  }
  // 00000000 abcdefgh 00000000 abcdefgh 
  else if (!(imm & 0xff00ff00) && (imm >> 16) == (imm & 0xff))
  {
    return (1 << 12) | (imm & 0xff);
  }
  // abcdefgh 00000000 abcdefgh 00000000
  else if (!(imm & 0x00ff00ff) && ((imm >> 16) & 0xff00) == (imm & 0xff00))
  {
    return (2 << 12) | ((imm >> 8) & 0xff);
  }
  // abcdefgh abcdefgh abcdefgh abcdefgh
  else if ((imm & 0xffff) == ((imm >> 16) & 0xffff) && ((imm >> 8) & 0xff) == (imm & 0xff))
  {
    return (3 << 12) | (imm & 0xff);
  }
  else 
  {
    for (uint32_t j = 0; j < 24; j++)
    {
      const uint32_t mask = 0xff000000 >> j;
      const uint32_t firstbit = 0x80000000 >> j;
      if ((imm & firstbit) == firstbit && (imm & ~mask) == 0)
      {
        const uint32_t imm5 = (j + 8);
        const uint32_t i = imm5 >> 4;
        const uint32_t imm3 = (imm5 >> 1) & 0x7;
        const uint32_t a = imm5 & 1;
        const uint32_t bcdefgh = (imm >> (24 - j)) & 0x7f;
        return (i << 26) | (imm3 << 12) | (a << 7) | bcdefgh;
      }
    }
  }
  tcc_error("compiler_error: unable to pack constant 0x%x for immediate thumb constats\n", imm);
  return 0;
}


static void th_pop(uint16_t regs)
{
  // T1 encoding R0-R7 + PC only, all armv-m
  // (T2 in armv8-m - inconsistent naming in reference manual)
  if (!(regs & 0x8f00))
  {
    const uint16_t pc = (regs >> 15) & 1;
    o(0xbc00 | (pc << 8) | (regs & 0xff));
    return; 
  }
  // T2 encoding R0-R12 + PC + LR, > armv7-m
  // (T1 in armv8-m - inconsistent naming in reference manual)
  #if defined(TCC_TARGET_ARM_ARCHV8M) || defined(TCC_TARGET_ARM_ARCHV7M)
  if (!(regs & 0x2000))
  {
    o(0xe8bd);
    o(regs);
    return;
  }
  #endif
  tcc_error("compiler_error: 'th_pop' contains illegal registers: 0x%x\n", regs);
}

static void th_add_sp_imm(uint16_t rd, uint16_t imm)
{
  // T1 on all armv-m
  if (rd < 8 && imm <= 1020 && !(imm & 0x3))
  {
    o(0xa800 | (rd << 8) | (imm >> 2));
  }
  // T2 on all armv-m
  else if (rd == R_SP && imm <= 508 && !(imm & 0x3))
  {
    o(0xb000 | (imm >> 2));
  }
#if !defined(TCC_TARGET_ARM_ARCHV6M)
  // T3
  else if (rd != R_PC && imm <= 4095)
  {
    const uint16_t i = (imm >> 11) & 1;
    const uint16_t imm3 = (imm >> 8) & 7;
    o(0xf20d | (i << 10));
    o((imm3 << 12) | ((rd & 0xf) << 8) | (imm & 0xff));
  }
  else if (rd != R_PC)
  {
    const uint32_t enc = th_pack_const(imm);
    if (enc || imm == 0) 
    {
      const uint16_t a = enc >> 16;
      const uint16_t b = enc & 0xffff;
      o(0xf10d | a);
      o(b | ((rd & 0xf) << 8));
    }
    else 
    {
      tcc_error("compiler_error: 'th_add_sp_imm' cannot pack const: %d or imm: 0x%x\n", rd, imm);
    }
  }
#endif
  else 
  {
    tcc_error("compiler_error: 'th_add_sp_imm' invalid register: %d or imm: 0x%x\n", rd, imm);
  }
}

static int th_rsb_imm(uint16_t rd, uint16_t rn, uint16_t imm, flags_behaviour setflags)
{
  if (rd < 8 && rn < 8 && imm == 0 && setflags == FLAGS_BEHAVIOUR_SET)
  {
    o(0x4240 | (rn << 3) | rd);
    return 1;
  }
  else if (rd != R_SP && rd != R_PC && rn != R_SP && rn != R_PC)
  {
    return th_generic_op_imm_with_status(0xf1c0, rd, rn, imm, setflags);
  }
  return 0;
}

static int th_shift_armv7m(uint16_t rd, uint16_t rm, uint16_t imm, uint16_t type)
{
  const uint16_t imm3 = (imm >> 2) & 7;
  const uint16_t imm2 = imm & 0x3;
  o(0xea4f);
  o((imm3 << 12) | (rd << 8) | (imm2 << 6) | (type << 4) | rm);
}

static int th_lsl_reg(uint16_t rd, uint16_t rn, uint16_t rm)
{
  if (rd == rn && rm < 8 && rn < 8)
  {
    o(0x4080 | (rm << 3) | rd);
    return 1;
  }
#ifndef TCC_TARGET_ARM_ARCHV6M
  else if (rd != R_SP && rd != R_PC && rn != R_SP && rn != R_PC && rm != R_SP && rm != R_PC)
  {
    o(0xfa00 | rn);
    o(0xf000 | (rd << 8) | rm);
    return 1;
  }
#endif 
  return 0;
}

static int th_lsl_imm(uint16_t rd, uint16_t rm, uint16_t imm)
{
  if (rm < 8 && rd < 8) 
  {
    o(0x0000 | (imm << 6) | (rm << 3) | rd);
    return 1; 
  }
#ifndef TCC_TARGET_ARM_ARCHV6M 
  else if (imm >= 1 && imm <= 31)
  {
    return th_shift_armv7m(rd, rm, imm, 0);
  }
#endif
  return 0;
}

static int th_lsr_imm(uint16_t rd, uint16_t rm, uint16_t imm)
{
  if (rm < 8 && rd < 8) 
  {
    o(0x0800 | (imm << 6) | (rm << 3) | rd);
    return 1; 
  }
#ifndef TCC_TARGET_ARM_ARCHV6M 
  else if (imm >= 1 && imm <= 31)
  {
    return th_shift_armv7m(rd, rm, imm, 1);
  }
#endif
  return 0;
}

static int th_asr_reg(uint16_t rd, uint16_t rn, uint16_t rm)
{
  if (rd == rn && rm < 8 && rn < 8)
  {
    o(0x4100 | (rm << 3) | rd);
    return 1;
  }
#ifndef TCC_TARGET_ARM_ARCHV6M
  else if (rd != R_SP && rd != R_PC && rn != R_SP && rn != R_PC && rm != R_SP && rm != R_PC)
  {
    o(0xfa40 | rn);
    o(0xf000 | (rd << 8) | rm);
    return 1;
  }
#endif 
  return 0;
}

static int th_asr_imm(uint16_t rd, uint16_t rm, uint16_t imm)
{
  if (rm < 8 && rd < 8) 
  {
    o(0x1000 | (imm << 6) | (rm << 3) | rd);
    return 1; 
  }
#ifndef TCC_TARGET_ARM_ARCHV6M 
  else if (imm >= 1 && imm <= 31)
  {
    return th_shift_armv7m(rd, rm, imm, 2);
  }
#endif
  return 0;
}

static int th_offset_to_reg(int off, int sign)
{
  int rr = get_reg(RC_INT);
  
  // if mov is not possible then load from data
  if (!th_mov_imm(rr, off))
  {
    load_full_const(rr, sign ? -off : off, NULL);
    return rr;
  }

  if (sign) th_rsb_imm(rr, rr, 0, FLAGS_BEHAVIOUR_NOT_IMPORANT);
  return rr;
}

// VFP instructions

static void th_vpush(uint32_t regs)
{
  // single precision floating point registers for now 
  // TODO: add support for hardfloat config 
  o(0xed2d);
  o(0x0a00 | (regs & 0xffff));
}

static void th_vpop(uint32_t regs)
{
  o(0xecbd | (regs >> 16));
  o(0x0a00 | (regs & 0xffff));
}

static void th_vmov_register(uint16_t vd, uint16_t vm)
{
  if (vd <= 0x1f && vm <= 0x1f)
  {
    const uint16_t d = vd & 1;
    const uint16_t m = vm & 1;
    vd >>= 1;
    vm >>= 1;
    o(0xeeb0 | (d << 6));
    o(0x0a40 | (vd << 12) | (m << 5) | vm);
  }
  else 
  {
    tcc_error("compiler_error: can't encode 'th_vmov_register' for vd: %d, vm: %d\n", vd, vm);
  }
}

static void th_vldr(uint32_t rn, uint32_t vd, uint32_t add, uint32_t is_doubleword, uint32_t imm)
{
  const uint32_t D = (vd >> 4) & 1;
  if (imm > 1020 || (imm & 0x3))
  {
    tcc_error("compiler_error: 'th_vldr' imm is outside of range: 0x%x, max value: 0xff\n", imm);
    return;
  }
  if (is_doubleword)
  {
    o(0xed10 | (D << 6) | ((add & 1) << 7) | rn & 0xf);
    o(0x0b00 | ((vd & 0xf) << 12) | (imm > 2));
  }
  else 
  {
    o(0xed10 | (D << 6) | ((add & 1) << 7) | rn & 0xf);
    o(0x0a00 | ((vd & 0xf) << 12) | (imm > 2));
  }
}

static void th_vstr(uint32_t rn, uint32_t vd, uint32_t add, uint32_t is_doubleword, uint32_t imm)
{
  const uint32_t D = (vd >> 4) & 1;
  if (imm > 1020 || (imm & 0x3))
  {
    tcc_error("compiler_error: 'th_vstr' imm is outside of range: 0x%x, max value: 0xff\n", imm);
  }
  if (is_doubleword)
  {
    o(0xed00 | (D << 6) | ((add & 1) << 7) | rn & 0xf);
    o(0x0b00 | ((vd & 0xf) << 12) | (imm > 2));
  }
  else 
  {
    o(0xed00 | (D << 6) | ((add & 1) << 7) | rn & 0xf);
    o(0x0a00 | ((vd & 0xf) << 12) | (imm > 2));
  }
}

// move between core general purpose register and single precision floating point register
static void th_vmov_gp_sp(uint16_t rt, uint16_t sn, uint16_t to_arm_register)
{
  const uint16_t N = (sn >> 4) & 1;
  o(0xee00 | ((to_arm_register & 1) << 4) | (sn & 0xf));
  o(0x0a10 | ((rt & 0xf) << 12) | (N << 7));
}

// move between two general purpose registers and one doubleword register 
static void th_vmov_2gp_dp(uint16_t rt, uint16_t rt2, uint16_t dm, uint16_t to_arm_register)
{
  const uint16_t M = (dm >> 4) & 1;
  o(0xec40 | ((to_arm_register & 1) << 4) | (rt2 & 0xf));
  o(0x0b10 | ((rt & 0xf) << 12) | (M << 5) | (dm & 0xf));
}

static uint32_t gen_th_sub_sp_imm(uint16_t rd, uint32_t imm)
{
  uint32_t x = 0;
  // T1 encoding
  if (rd == R_SP && imm <= 508 && !(imm & 0x3))
  {
    x = 0xb080 | (imm >> 2);
  }
  #ifndef TCC_TARGET_ARM_ARCHV6M
  // T3 encoding
  else if (imm <= 4095 && rd != R_PC)
  {
    const uint32_t i = (imm >> 11) & 1;
    const uint32_t imm3 = (imm >> 8) & 0x7;
    x = (0xf2ad | (i << 10)) << 16;
    x |= ((imm3 << 12) | ((rd & 0xf) << 8) | (imm & 0xff));
  }
  else if (rd != R_PC)
  {
    const uint32_t enc = th_pack_const(imm);
    if (enc || imm == 0)
    {
      const uint32_t a = enc >> 16;
      const uint32_t b = enc & 0xffff;
      x = (0xf1ad | a) << 16;
      x |= (((rd & 0xf) << 8) | b);
    }
    else 
    {
      tcc_error("compiler_error: 'gen_th_sub_sp_imm' cannot pack const: %d or imm: 0x%x\n", rd, imm);
    }
  }
  #endif
  else 
  {
    tcc_error("compiler_error: 'gen_th_sub_imm' can't generate for rd: %d, imm: 0x%x\n", rd, imm);
  }
  return x;
}

static uint32_t th_sub_sp_imm_estimate(uint16_t rd, uint32_t imm)
{
  // T1 encoding
  if (rd == R_SP && imm <= 508 && !(imm & 0x3))
  {
    return 2;
  }
  #ifndef TCC_TARGET_ARM_ARCHV6M
  // T3 encoding
  else if (imm <= 4095 && rd != R_PC)
  {
    return 4;
  }
  else if (rd != R_PC)
  {
    return 4;
  }
  #endif
  return 0;
}

static void th_sub_sp_imm(uint16_t rd, uint16_t imm)
{
  const uint32_t x = gen_th_sub_sp_imm(rd, imm);
  const uint32_t x_size = th_sub_sp_imm_estimate(rd, imm); 
  if (x_size == 2)
  {
    o(x);  
  }
  else if (x_size == 4)
  {
    o(x >> 16);
    o(x & 0xffff);
  }
  else 
  {
    tcc_error("compiler_error: can't generate th_sub_sp_imm for rd: %d, imm: 0x%x\n", rd, imm);
  }
}

// Thumb ELF management 

// Start of T32 instructions
void th_sym_t()
{
  const int info = ELFW(ST_INFO)(STB_LOCAL, STT_NOTYPE);
  set_elf_sym(symtab_section, ind, 0, info, 0, 1, "$t");
}

// Start of A32 instructions
void th_sym_a()
{
  const int info = ELFW(ST_INFO)(STB_LOCAL, STT_NOTYPE);
  set_elf_sym(symtab_section, ind, 0, info, 0, 1, "$a");
}

// Start of data 
void th_sym_d()
{
  const int info = ELFW(ST_INFO)(STB_LOCAL, STT_NOTYPE);
  set_elf_sym(symtab_section, ind, 0, info, 0, 1, "$d");
}

// TODO: this is armv7-m code
int decbranch(int pos)
{
  int xa = *(uint16_t *)(cur_text_section->data + pos);
  int xb = *(uint16_t *)(cur_text_section->data + pos + 2);

  if ((xa & 0xf000) != 0xd000)
  {
    xa &= 0x00ff;
    if (xa & 0x0080) xa -= 0x100;
    xa = (xa * 2) + pos + 4;
  }
  else if ((xa & 0xf800) == 0xe000)
  {
    xa &= 0x7ff;
    if (xa & 0x400) xa -= 0x800;
    xa = (xa * 2) + pos + 4;
  }
  else if ((xa & 0xf800) == 0xf000 && (xb & 0xd000) == 0x8000)
  {
    const uint32_t s = (xa >> 10) & 1;
    const uint32_t imm6 = (xa & 0x3f);
    const uint32_t j1 = (xb >> 13) & 1;
    const uint32_t j2 = (xb >> 11) & 1;
    const uint32_t imm11 = xb & 0x7ff;
    uint32_t ret = (j2 << 19) | (j1 << 18) | (imm6 << 12) | (imm11 << 1);
    if (s) ret |= 0xfff00000;
    xa = ret + pos + 4;
  }
  else if ((xa & 0xf800) == 0xf000 && (xb & 0xd000) == 0x9000)
  {
    const uint32_t s = (xa >> 10) & 1;
    const uint32_t imm10 = (xa & 0x3ff);
    const uint32_t j1 = (xb >> 13) & 1;
    const uint32_t j2 = (xb >> 11) & 1;
    const uint32_t imm11 = xb & 0x7ff;
    const uint32_t i1 = ~(j1 ^ s) & 1;
    const uint32_t i2 = ~(j2 ^ s) & 1;
    uint32_t ret = (i2 << 23) | (i1 << 22) | (imm10 << 12) | (imm11 << 1);
    if (s) ret |= 0xff000000;
    xa = ret + pos + 4;
  }
  else 
  {
    tcc_error("compiler_error: decbranch unknown encoding pos: 0x%x\n", pos);
    return 0;
  }
  return xa;
}


static uint32_t th_encbranch(int pos, int addr)
{
  TRACE("th_encbranch pos: 0x%x, addr: 0x%x", pos, addr);
  return addr - pos - 4;
}

static uint32_t th_encbranch_8(int pos, int addr)
{
  addr = (addr - pos - 4) / 2;
  if (addr >= 127 || addr < -128)
  {
    tcc_error("compiler_error: th_encbranch_8 too far address: %i\n", addr);
    return 0;
  }
  return addr & 0xff;
}

static uint32_t th_encbranch_11(int pos, int addr)
{
  addr = (addr - pos - 4) / 2;
  if (addr >= 1023 || addr < -1024)
  {
    tcc_error("compiler_error: th_encbranch_11 too far address: %i\n", addr);
    return 0;
  }
  return addr & 0x7ff;
}

static uint32_t th_encbranch_20(int pos, int addr)
{
  return (addr - pos - 4) / 2;
}

int th_patch_call(int t, int a)
{
  uint16_t *x = (uint16_t *)(cur_text_section->data + t);
  int lt = t;

  TRACE("'th_patch_call' t: %.8x, a: %.8x\n", t, a); 

  t = decbranch(t);
  if (a == lt + 2) *x = 0xbf00;
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
    const uint32_t enc = th_encbranch_b_t3(th_encbranch_20(lt, a));
    x[0] &= 0xfbc0;
    x[1] &= 0xd000;
    x[0] |= enc >> 16;
    x[1] |= enc & 0xffff;
  }
  else if ((x[0] & 0xf800) == 0xf000 && (x[1] & 0xd000) == 0x9000)
  {
    const uint32_t enc = th_packimm_10_11_0(th_encbranch_20(lt, a) << 1);
    x[0] &= 0xf800;
    x[1] &= 0xd000;
    x[0] |= enc >> 16;
    x[1] |= enc & 0xffff;
  }
  else return 0;
  //else tcc_error("compiler_error: unhandled branch type in th_patch_call for: t: 0x%x, a: 0x%x, x: 0x%x 0x%x\n", t, a, x[0], x[1]);

  return t;
}


static void gadd_sp(int val)
{
  if (val > 0)
  {
    th_add_sp_imm(R_SP, val);
  }
  else 
  {
    th_sub_sp_imm(R_SP, -val);
  }
}

static void gcall_or_jmp(int is_jmp)
{
  TRACE("gcall_or_jmp: %d, ind: 0x%x, vtop: 0x%x", is_jmp, ind, vtop->c.i);
  if ((vtop->r & (VT_VALMASK | VT_LVAL)) == VT_CONST)
  {
    uint32_t x = th_encbranch(ind, ind + vtop->c.i);
    if (x)
    {
      if (vtop->r & VT_SYM) greloc(cur_text_section, vtop->sym, ind, R_ARM_THM_JUMP24);
      th_bl_t1(x);
    }
  }
  else 
  {
    int r = gv(RC_INT);
    if (is_jmp) th_bx_reg(intr(r));
    else th_blx_reg(intr(r));

  }
}

/* Copy parameters to their final destination (core reg, VFP reg or stack) for
   function call.

   nb_args: number of parameters the function take
   plan: the overall assignment plan for parameters
   todo: a bitmap indicating what core reg will hold a parameter

   Returns the number of SValue added by this function on the value stack */
static int copy_params(int nb_args, struct plan *plan, int todo)
{
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
  for(i = 0; i < NB_CLASSES; i++) 
  {
    for(pplan = plan->clsplans[i]; pplan; pplan = pplan->prev) 
    {

      if (pass && (i != CORE_CLASS || pplan->sval->r < VT_CONST)) continue;

      vpushv(pplan->sval);
      pplan->sval->r = pplan->sval->r2 = VT_CONST; /* disable entry */
      switch(i) 
      {
        case STACK_CLASS:
        case CORE_STRUCT_CLASS:
        case VFP_STRUCT_CLASS:
          if ((pplan->sval->type.t & VT_BTYPE) == VT_STRUCT) 
          {
            int padding = 0;
            size = type_size(&pplan->sval->type, &align);
            /* align to stack align size */
            size = (size + 3) & ~3;
            if (i == STACK_CLASS && pplan->prev) padding = pplan->start - pplan->prev->end;
            size += padding; /* Add padding if any */
            /* allocate the necessary size on stack */
            gadd_sp(-size);
            /* generate structure store */
            r = get_reg(RC_INT);
            th_add_sp_imm(intr(r), padding);
            vset(&vtop->type, r | VT_LVAL, 0);
            vswap();
            vstore();
	          /* XXX: optimize. Save all register because memcpy can use them */
            th_vpush(0xffff);
            vstore(); /* memcpy to current sp + potential padding */
            th_vpop(0xffff);

            /* Homogeneous float aggregate are loaded to VFP registers
               immediately since there is no way of loading data in multiple
               non consecutive VFP registers as what is done for other
               structures (see the use of todo). */
            if (i == VFP_STRUCT_CLASS) 
            {
              int first = pplan->start, nb = pplan->end - first + 1;
              /* vpop.32 {pplan->start, ..., pplan->end} */
              th_vpop((first & 1) << 22 | (first >> 1) << 12 | nb);
              /* No need to write the register used to a SValue since VFP regs
                 cannot be used for gcall_or_jmp */
            }
          } 
          else 
          {
            if (is_float(pplan->sval->type.t)) 
            {
#ifdef TCC_ARM_VFP
              r = vfpr(gv(RC_FLOAT)) << 12;
              if ((pplan->sval->type.t & VT_BTYPE) == VT_FLOAT)
                size = 4;
              else 
              {
                size = 8;
                r |= 0x101; /* vpush.32 -> vpush.64 */
              }
              th_vpush(r + 1);
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
              else if(size == 8)
                r|=0x8000;
              tcc_error("compiler_error: implement vpush for fpa\n");
              // o(0xED2D0100|r|(size>>2)); /* some kind of vpush for FPA */
#endif
            } else {
              /* simple type (currently always same size) */
              /* XXX: implicit cast ? */
              size=4;
              if ((pplan->sval->type.t & VT_BTYPE) == VT_LLONG) {
                lexpand();
                size = 8;
                r = gv(RC_INT);
                th_push(1 << intr(r));
                vtop--;
              }
              r = gv(RC_INT);
              th_push(1 << intr(r));
            }
            if (i == STACK_CLASS && pplan->prev)
              gadd_sp(pplan->prev->end - pplan->start); /* Add padding if any */
          }
          break;

        case VFP_CLASS:
          gv(regmask(TREG_F0 + (pplan->start >> 1)));
          if (pplan->start & 1) { /* Must be in upper part of double register */
            th_vmov_register(pplan->start, pplan->start - 1);
            vtop->r = VT_CONST; /* avoid being saved on stack by gv for next float */
          }
          break;

        case CORE_CLASS:
          if ((pplan->sval->type.t & VT_BTYPE) == VT_LLONG) {
            lexpand();
            gv(regmask(pplan->end));
            pplan->sval->r2 = vtop->r;
            vtop--;
          }
          gv(regmask(pplan->start));
          /* Mark register as used so that gcall_or_jmp use another one
             (regs >=4 are free as never used to pass parameters) */
          pplan->sval->r = vtop->r;
          break;
      }
      vtop--;
    }
  }

  /* second pass to restore registers that were saved on stack by accident.
     Maybe redundant after the "lvalue_save" patch in tccgen.c:gv() */
  if (++pass < 2)
    goto again;

  /* Manually free remaining registers since next parameters are loaded
   * manually, without the help of gv(int). */
  save_regs(nb_args);

  if(todo) {
    th_pop(todo);
    for(pplan = plan->clsplans[CORE_STRUCT_CLASS]; pplan; pplan = pplan->prev) {
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
    th_nop();
    bytes -= 2;
  }
}

// generate function prolog
void gfunc_prolog(Sym *func_sym)
{
  const CType *func_type = &func_sym->type; 
  Sym *sym = func_type->ref; 
  int n = 0, nf = 0;
  int pn = 0; // core 
  int sn = 0; // stack
  CType ret_type = {};
  int align = 0;
  int regsize = 0;
  int return_struct = 0;
  int size = 0;
  int addr = 0;

#ifdef TCC_ARM_EABI
  struct avail_regs avregs = {{0}};
#endif
  // current function return type
  func_vt = sym->type;
  // true if function is variadic 
  func_var = (func_type->ref->f.func_type == FUNC_ELLIPSIS);
  func_nregs = 0;

  TRACE("'gfunc_prolog'");
  th_mov_reg(R_IP, R_SP);

  if ((func_vt.t & VT_BTYPE) == VT_STRUCT && !gfunc_sret(&func_vt, func_var, &ret_type, &align, &regsize))
  {
    n++;
    return_struct = 1;
    func_vc = 12;
  }

  for (Sym *s = sym->next; s && (n < 4 || nf < 16); s = s->next)
  {
    size = type_size(&s->type, &align); 
#ifdef TCC_ARM_EABI
    if (float_abi == ARM_HARD_FLOAT && !func_var && (is_float(s->type.t) || is_hgen_float_aggr(&s->type))) {
      int tmpnf = assign_vfpreg(&avregs, align, size);
      tmpnf += ceil_div(size, 4);
      nf = (tmpnf > nf) ? tmpnf : nf;
    } else 
#endif 
    if (n < 4)
    {
      n += ceil_div(size, 4);
    }
  }
  th_sym_t();
  if (func_var) n = 4;
  if (n)
  {
    if (n > 4) n = 4;
#ifdef TCC_ARM_EABI 
    n=(n + 1) & -2;
#endif
    func_nregs = n;
    th_push((1 << n) - 1);
  }
  
  if (nf)
  {
    if (nf > 16) nf = 16;
    nf=(nf+1)&-2; /* nf => HARDFLOAT => EABI */
    th_vpush(nf); // save used s0-s15 on stack 
    func_nregs += nf;
  }
  // save fp, ip, lr 
  // R0 - just to keep 8 byte aligment of stack pointer
  th_push((1 << R_FP) | (1 << R_IP) | (1 << R_LR));
  th_mov_reg(R_FP, R_SP);
  func_sub_sp_offset = ind;
  // space for stack adjustment in epilog 
  th_nop();
  th_nop();

#ifdef TCC_ARM_EABI
  if (float_abi == ARM_HARD_FLOAT) 
  {
    func_vc += nf * 4;
    memset(&avregs, 0, sizeof(avregs));
  }
#endif
  pn = return_struct;
  sn = 0;
  while ((sym = sym->next)) 
  {
    CType *type;
    type = &sym->type;
    size = ceil_div(type_size(type, &align), 4);
    align = (align + 3) & ~3;
#ifdef TCC_ARM_EABI 
    if (float_abi == ARM_HARD_FLOAT && !func_var && (is_float(sym->type.t) 
          || is_hgen_float_aggr(&sym->type)))
    {
      const int fpn = assign_vfpreg(&avregs, align, size << 2);
      if (fpn >= 0) addr = fpn * 4;
      else goto from_stack;
    } 
    else 
#endif 
    if (pn < 4) 
    {
#ifdef TCC_ARM_EABI 
      pn = (pn + (align - 1) / 4) & ~(align / 4);
#endif
      addr = (nf + pn) * 4;
      pn += size;
      if (!sn && pn > 4) sn = (pn - 4);
      else 
      {
#ifdef TCC_ARM_EABI 
from_stack: 
        sn = (sn + (align - 1) / 4) & ~(align / 4);
#endif
        addr = (n + nf + sn) * 4;
        sn += addr;
      }
      sym_push(sym->v & ~SYM_FIELD, type, VT_LOCAL | VT_LVAL, addr + 12);
    }
    leaffunc = 1;
    loc = 0;
  }
}



// all params needs to be passed in core registers or not
static int floats_in_core_regs(const SValue *sval)
{
  if (!sval->sym)
  {
    return 0;
  }

  switch (sval->sym->v) 
  {
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

void gfunc_call(int nb_args)
{
  int r;
  int args_size;
  int def_float_abi = float_abi;
  int todo;
  struct plan plan;
  int variadic;

  TRACE("'gfunc_call'");
  if (float_abi == ARM_HARD_FLOAT) 
  {
    variadic = (vtop[-nb_args].type.ref->f.func_type == FUNC_ELLIPSIS);
    if (variadic || floats_in_core_regs(&vtop[-nb_args])) float_abi = ARM_SOFTFP_FLOAT;
  }
  r = vtop->r & VT_VALMASK;
  if (r == VT_CMP || (r & ~ 1) == VT_JMP) gv(RC_INT);

  memset(&plan, 0, sizeof(plan));
  if (nb_args) plan.pplans = tcc_malloc(nb_args * sizeof(*plan.pplans));
  args_size = assign_regs(nb_args, float_abi, &plan, &todo);

  if (args_size & 7) // stack must be 8-byte aligned according to AAPCS for EABI 
  {
    args_size = (args_size + 7) & ~7;
    th_sub_sp_imm(R_SP, args_size % 8);
  }
  nb_args += copy_params(nb_args, &plan, todo);
  tcc_free(plan.pplans);

  vrotb(nb_args + 1);
  gcall_or_jmp(0);

  if (args_size) gadd_sp(args_size);
  if (float_abi == ARM_SOFTFP_FLOAT && is_float(vtop->type.ref->type.t)) {
    if ((vtop->type.ref->type.t & VT_BTYPE) == VT_FLOAT) th_vmov_gp_sp(0, 0, 0);
    else th_vmov_2gp_dp(0, 1, 0, 0);
  }
  vtop -= nb_args + 1;
  leaffunc = 0;
  float_abi = def_float_abi;
}

void gfunc_epilog(void)
{
  int diff = 0;
  TRACE("'gfunc_epilog'");
  // copy float return value to core register if base standard is used 
  // and float computation is made with VFP
  if ((float_abi == ARM_SOFTFP_FLOAT || func_var) && is_float(func_vt.t))
  {
    if ((func_vt.t & VT_BTYPE) == VT_FLOAT)
    {
      th_vmov_gp_sp(R0, 0, 1);
    }
    else // double  
    {
      th_vmov_2gp_dp(R0, R1, 0, 1);
    }
  }
 // align stack
  diff = (-loc + 3) & -4;
  if (!leaffunc) diff = ((diff + 11) & -8) -4;
  if (diff > 0) th_add_sp_imm(R_SP, diff);
  th_pop((1 << R_FP) | (1 << R_IP) | (1 << R_LR));

  if (diff > 0)
  {
    const uint32_t x = gen_th_sub_sp_imm(R_SP, diff);
    if (x) *(uint32_t *)(cur_text_section->data + func_sub_sp_offset) = x; 
    else tcc_error("compiler_error: failed to generate stack adjustment\n");
  }
  
  if (func_nregs)
  {
    th_add_sp_imm(R_SP, func_nregs << 2);
  }
  th_bx_reg(R_LR);

  if (ind & 3) th_nop();
 }

void ggoto(void)
{
  TRACE("'ggoto'");
  gcall_or_jmp(1);
  vtop--;
}

ST_FUNC int gjmp(int t)
{
  int r = ind;
  int val = ((t-r) >> 1) - 2;
  TRACE("'gjump'");
  if (nocode_wanted) return t;

  if (val < -1024 || val > 1023) th_b_t4(val << 1);
  else th_b_t2(val << 1);
  return r;
}

ST_FUNC void gjmp_addr(int a)
{
  TRACE("'gjump_addr'");
  gjmp(a);
}

ST_FUNC int gjmp_append(int n, int t)
{
  int p, lp;
  TRACE("'gjmp_append'");
  if (n)
  {
    p = n;
    do 
    {
      p = decbranch(lp = p);
    } while (p);
    th_patch_call(lp, t);
    t = n;
  }
  return t;
}

ST_FUNC int gjmp_cond(int op, int t)
{
  int r = ind; 

  TRACE("'gjmp_cond'");

  if (nocode_wanted) return t;

  op = mapcc(op);

  th_b_t3(op, th_encbranch_20(r, t));
  return r;
}

void gsym_addr(int t, int a)
{
  TRACE("'gsym_addr' %.8x branch target: %.8x\n", t, a);

  while (t) 
    t = th_patch_call(t, a);
}


ST_FUNC void gen_vla_alloc(CType *type, int align)
{
  TRACE("'gen_vla_alloc'");
}

ST_FUNC void gen_vla_sp_save(int addr)
{
  TRACE("'gen_vla_sp_save'");
}

ST_FUNC void gen_vla_sp_restore(int addr)
{
  TRACE("'gen_vla_sp_restore'");
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
  if ((type->t & VT_BTYPE) == VT_STRUCT) {
    struct Sym *ref;
    int btype, nb_fields = 0;

    ref = type->ref->next;
    if (ref) {
      btype = unalias_ldbl(ref->type.t & VT_BTYPE);
      if (btype == VT_FLOAT || btype == VT_DOUBLE) {
        for(; ref && btype == unalias_ldbl(ref->type.t & VT_BTYPE); ref = ref->next, nb_fields++);
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
#ifdef TCC_ARM_EABI
  int align;
  const int size = type_size(vt, &align);

  TRACE("'gfunc_sret'");
  if (float_abi == ARM_HARD_FLOAT && !variadic && 
    (is_float(vt->t) || is_hgen_float_aggr(vt)))
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
#else 
  return 0;
#endif
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
  if (!(r >= TREG_SP && r <= TREG_LR))
  {
    tcc_error("compiler_error: register: %d is not int register\n", r);
  }
  return r + (13 - TREG_SP);
}

void store(int r, SValue *sv)
{
  int v, vt, fc, ft, fr, sign;
  TRACE("'store' reg: %d", r);
  
  fr = sv->r;
  ft - sv->type.t;
  fc = sv->c.i;

  if (fc >= 0) sign = 0;
  else 
  {
    sign = 1;
    fc = -fc;
  }

  v = fr & VT_VALMASK;

  if (fr & VT_LVAL || fr == VT_LOCAL)
  {
    uint32_t base = 11;
    if (v < VT_CONST)
    {
      base = intr(v);
      v = VT_LOCAL;
      fc = sign = 0;
    }
    else if (v == VT_CONST)
    {
      SValue v1;
      v1.type.t = ft;
      v1.r = fr & ~VT_LVAL;
      v1.c.i = sv->c.i;
      v1.sym = sv->sym;
      load(base=14, &v1);
      fc=sign=0;
      v = VT_LOCAL;
    }
    if (v == VT_LOCAL)
    {
      if (is_float(ft))
      {
        if ((ft & VT_BTYPE) != VT_FLOAT) 
          th_vstr(base, vfpr(r), !sign, 1, fc);
        else 
          th_vstr(base, vfpr(r), !sign, 0, fc);
      } 
      else if ((ft & VT_BTYPE) == VT_SHORT)
      {
        if (!th_strh_imm(r, base, fc, sign ? 4 : 6))
        {
          int rr = th_offset_to_reg(fc, sign);
          th_strh_reg(r, base, rr);
        }
      }
      else if ((ft & VT_BTYPE) == VT_BOOL)
      {
        if (!th_strb_imm(r, base, fc, sign ? 4 : 6))
        {
          int rr = th_offset_to_reg(fc, sign);
          th_strb_reg(r, base, rr);
        }
      }
      else
      {
        if (!th_str_imm(r, base, fc, sign ? 4 : 6))
        {
          int rr = th_offset_to_reg(fc, sign);
          th_str_reg(r, base, rr);
        }
      }

    }
  }
}

static void load_vt_lval_vt_local_float(int r, SValue *sv, int ft, int fc, int sign, uint32_t base)
{
  if ((ft & VT_BTYPE) != VT_FLOAT)
  {
    // load double 
    th_vldr(base, vfpr(r), !sign, 1, fc);
  }
  else 
  {
    th_vldr(base, vfpr(r), !sign, 0, fc);
  }
}

static void load_full_const(int r, uint32_t imm, struct Sym *sym)
{
  int est = 0;
  TRACE("'load_full_const' to register: %d, with imm: %d\n", r, imm);
  est = th_ldr_literal_estimate(r, 4);
  est += 2; // branch instruction size
  est += ind;
  // 4-byte alignment
  if (est & 3) th_nop(); 
  th_ldr_literal(r, 4, 1);
  th_b_t2(4);
  if (!pic) {
    TRACE("Loading from there");
    if (sym) greloc(cur_text_section, sym, ind, R_ARM_ABS32);
  }
  else { 
    if (sym) {
      if (sym->type.t & VT_STATIC) greloc(cur_text_section, sym, ind, R_ARM_REL32);
      else {
        if (!text_and_data_separation) greloc(cur_text_section, sym, ind, R_ARM_GOT_PREL);
        else greloc(cur_text_section, sym, ind, R_ARM_GOT32);
      }
    }
  }
  
  th_sym_d();
  o(imm >> 16);
  o(imm & 0xffff);
  th_sym_t();

  if (pic) {
    if (sym) {
      if (sym->type.t & VT_STATIC) {
        th_add_reg(r, R_PC, r);
      } else {
        if (!text_and_data_separation) {
          tcc_error("implement GOT reloc 1");
        }
        else {
          
        }
      }
    }
  }
}

void load_short_from_base(int ir, int base, int fc, int sign)
{
#ifndef TCC_TARGET_ARM_ARCHV6M
  // ldrsh imm is not available on armv6-m
  th_ldrsh_imm(ir, base, fc, sign ? 4 : 6);
#else 
  // so instead address must be calculated and placed in register
  int rr = th_offset_to_reg(fc, sign);
  th_ldrsh_reg(ir, base, rr);
#endif
}

void load_ushort_from_base(int ir, int base, int fc, int sign)
{
// the same, but for unsigned short 
#ifndef TCC_TARGET_ARM_ARCHV6M
  th_ldrh_imm(ir, base, fc, sign ? 4 : 6);
#else 
  if (!sign && fc <= 255)
  {
    th_ldrh_imm(ir, base, fc, sign ? 4 : 6); 
  }
  else 
  {
    int rr = th_offset_to_reg(fc, sign);
    th_ldrh_reg(ir, base, rr);
  }
#endif
}

void load_byte_from_base(int ir, int base, int fc, int sign)
{
#ifndef TCC_TARGET_ARM_ARCHV6M
  // ldrsh imm is not available on armv6-m
  th_ldrsb_imm(ir, base, fc, sign ? 4 : 6);
#else 
  // so instead address must be calculated and placed in register
  int rr = th_offset_to_reg(fc, sign);
  th_ldrsb_reg(ir, base, rr);
#endif
}

void load_ubyte_from_base(int ir, int base, int fc, int sign)
{
// the same, but for unsigned byte
#ifndef TCC_TARGET_ARM_ARCHV6M
  th_ldrh_imm(ir, base, fc, sign ? 4 : 6);
#else 
  if (!sign && fc <= 255)
  {
    th_ldrb_imm(ir, base, fc, sign ? 4 : 6); 
  }
  else 
  {
    int rr = th_offset_to_reg(fc, sign);
    th_ldrb_reg(ir, base, rr);
  }
#endif
}

void load_word_from_base(int ir, int base, int fc, int sign)
{ 
#ifndef TCC_TARGET_ARM_ARCHV6M
  th_ldr_imm(ir, base, fc, sign ? 4 : 6);
#else 
  if (!sign && fc <= 255)
  {
    th_ldr_imm(ir, base, fc, sign ? 4 : 6); 
  }
  else 
  {
    int rr = th_offset_to_reg(fc, sign);
    th_ldr_reg(ir, base, rr);
  }
#endif
}

void load_vt_lval_vt_local(int r, SValue *sv, int ft, int fc, int sign, uint32_t base)
{
  int success = 0;
  int rr = 0;
  const int btype = ft & VT_BTYPE;
  int ir = intr(r);

  if (is_float(ft))
  {
    TRACE("load float to r: %d, base: %d, fc: %d, sign: %d\n", ir, base, fc, sign);
    return load_vt_lval_vt_local_float(r, sv, ft, fc, sign, base);
  }
  else if (btype == VT_SHORT)
  {
    TRACE("load short to r: %d, base: %d, fc: %d, sign: %d\n", ir, base, fc, sign);
    if (!(ft & VT_UNSIGNED))
    {
      load_short_from_base(ir, base, fc, sign);
    }
    else 
    {
      load_ushort_from_base(ir, base, fc, sign);
    }
  }
  else if (btype == VT_BYTE || btype == VT_BOOL)
  {
    if (!(ft & VT_UNSIGNED))
    {
      load_byte_from_base(ir, base, fc, sign);
    }
    else 
    {
      load_ubyte_from_base(ir, base, fc, sign);
    }
  }
  else 
  {
    load_word_from_base(ir, base, fc, sign);
  }
  // now load from dereferenced value
  rr = th_offset_to_reg(fc, sign);
  if (btype == VT_SHORT)
  {
    if (ft & VT_UNSIGNED) th_ldrh_reg(ir, base, rr);
    else th_ldrsh_reg(ir, base, rr);
  }
  else if (btype == VT_BYTE || btype == VT_BOOL)
  {
    if (ft & VT_UNSIGNED) th_ldrb_reg(ir, base, rr);
    else th_ldrsb_reg(ir, base, rr);
  }
  else th_ldr_reg(ir, base, rr);
}

void load_vt_const(int r, SValue *sv)
{
  r = intr(r);
  if (sv->r & VT_SYM)
  {
    load_full_const(r, sv->c.i, sv->sym);
    return;
  }
  // if (!th_mov_imm(r, sv->c.i))
  // {
  //   load_full_const(r, sv->c.i, 0);
  // }
}

void load_vt_local(int r, SValue *sv)
{
  TRACE("'load_vt_local' r: %d, off: %x", r, sv->c.i);
  if (sv->r & VT_SYM || (-sv->c.i) >= 0xfff)
  {
    load_full_const(r, sv->c.i, sv->r & VT_SYM ? sv->sym : 0);
    th_add_reg(r, R_FP, r);
  }
  else 
  {
    th_sub_imm(r, R_FP, -sv->c.i);
  }

}

void load_vt_cmp(int r, SValue *sv)
{
  const uint32_t firstcond = mapcc(sv->c.i);
  uint32_t rr = intr(r);
  TRACE("'load_vt_cmp' to reg: %d, op: 0x%x\n", r, sv->c.i);
  if (rr == R_SP || rr == R_PC)
  {
    tcc_error("compiler_error: load_vt_cmp can't be used for pc or sp\n");
  }

#ifdef TCC_TARGET_ARM_ARCHV6M
  // TODO: verify and optimize
  if (rr < 8)
  {
    th_b_t1(firstcond, 4);
    th_mov_imm(rr, 1);
    th_mov_imm(rr, 0);
  }
  else 
  {
    th_push(R_R1);
    th_b_t1(firstcond, 4);
    th_mov_imm(R_R1, 1);
    th_mov_imm(R_R1, 0);
    th_mov_reg(rr, R_R1);
    th_pop(R_R1);

  }
#else 
  // it block 
  o(0xbf00 | (firstcond << 4) | 0x4 | ((~firstcond & 1) << 3));
  th_mov_imm(rr, 1);
  th_mov_imm(rr, 0);
#endif 
}

void load_vt_jmp_jmpi(int r, SValue *sv)
{
#ifdef TCC_TARGET_ARM_ARCHV6M
  if (intr(r) > 7)
  {
    tcc_error("compiler_error: implement load_vt_jmp_jmpi for armv6m\n");
  }
#endif
  th_mov_imm(intr(r), sv->r & 1);
  th_b_t2(2);
  gsym(sv->c.i);
  th_mov_imm(intr(r), (sv->r^1) & 1);
}

// load value from stack to register
void load(int r, SValue *sv)
{
  int v, ft, fc, fr, sign;

  // TRACE("'load'");
  fr = sv->r;
  ft = sv->type.t;
  fc = sv->c.i;
  if (fc >= 0) sign = 0;
  else 
  {
    sign=1;
    fc = -fc;
  }

  v = fr & VT_VALMASK;
  // load lvalue from 
  if (fr & VT_LVAL)
  {
    uint32_t base = R_FP;  
    SValue v1;
    // load value from stack
    // prepare for new load after pointer dereference
    if (v == VT_LLOCAL)
    {
      v1.type.t = VT_PTR;
      v1.r = VT_LOCAL | VT_LVAL;
      v1.c.i = sv->c.i;
      load(TREG_LR, &v1);
      base = 14;
      fc = sign=0;
      v = VT_LOCAL;
    }
    else if (v == VT_CONST)
    {
      v1.type.t = VT_PTR;
      v1.r = fr&~VT_LVAL;
      v1.c.i = sv->c.i;
      v1.sym = sv->sym;
      load(TREG_LR, &v1);
      base = 14;
      fc = sign=0;
      v = VT_LOCAL;
    }
    else if (v < VT_CONST)
    {
      base = intr(v);
      fc = sign = 0;
      v = VT_LOCAL;
    }

    if (v == VT_LOCAL)
    {
      return load_vt_lval_vt_local(r, sv, ft, fc, sign, base);
    }
  }
  else if (v == VT_CONST) return load_vt_const(r, sv);
  else if (v == VT_LOCAL) return load_vt_local(r, sv);
  else if (v == VT_CMP) return load_vt_cmp(r, sv);
  else if (v == VT_JMP || v == VT_JMPI) return load_vt_jmp_jmpi(r, sv);
  else if (v < VT_CONST) 
  {
    if (is_float(ft)) tcc_error("compiler_error: unknown load mode\n");
    else 
    {
      th_mov_reg(r, v);
      return;
    }
  }
  tcc_error("compiler_error: unknown load not implemented\n");

}

ST_FUNC void gen_cvt_itof(int t)
{
  TRACE("'gen_cvt_itof'");


}

/* convert fp to int 't' type */
void gen_cvt_ftoi(int t)
{
  TRACE("'gen_cvt_ftoi'");


}

void gen_cvt_ftof(int t)
{
  TRACE("'gen_cvt_ftof'");

}

void gen_opf(int op)
{
  TRACE("'gen_opf'");

}

// operation on two registers
void gen_opi_regs(int opc, int c)
{
  int fr = 0;
  int r = 0;

  fr = intr(gv(RC_INT));
  r = intr(vtop[-1].r = get_reg_ex(RC_INT, two2mask(vtop->r, vtop[-1].r)));

  switch (opc)
  {
    case 0: th_and_reg(r, c, fr); return;
    case 2: th_xor_reg(r, c, fr); return;
    case 4:
    case 5: th_sub_reg(r, c, fr); return;
    case 6:
    case 7: th_rsb_reg(r, c, fr); return;
    case 8:
    case 9: th_add_reg(r, c, fr); return;
    case 10: th_adc_reg(r, c, fr); return;
    case 12: th_sbc_reg(r, c, fr); return;
    case 15: th_sbc_reg(r, fr, c); return;
    case 21: th_cmp_reg(c, fr); return;
    case 24: th_orr_reg(r, c, fr); return;
    default: tcc_error("compiler_error: 'gen_opi_regs' unhandled case opc: %d, c: %d, r: %d, fr: %d\n", opc, c, r, fr);
  }
}

void gen_opi_regular(int opc, int c)
{
  if ((vtop->r & (VT_VALMASK | VT_LVAL | VT_SYM)) == VT_CONST)
  {
    int ok = 0;
    int r = intr(vtop[-1].r=get_reg_ex(RC_INT, regmask(vtop[-1].r)));
    if (opc != 0x15 && r != c)
    {
      tcc_error("compiler_error: 'gen_opi_regular' incorrect order of r and c\n");
    }
    switch (opc)
    {
      case 0: ok = th_and_imm(r, r, vtop->c.i); break;
      case 2: ok = th_xor_imm(r, r, vtop->c.i); break;
      case 4:
      case 5: ok = th_sub_imm(r, r, vtop->c.i); break;
      case 6:
      case 7: ok = th_rsb_imm(r, r, vtop->c.i, FLAGS_BEHAVIOUR_SET); break;
      case 8:
      case 9: ok = th_add_imm(r, r, vtop->c.i); break;
      case 10: ok = th_adc_imm(r, r, vtop->c.i); break;
      case 12: ok = th_sbc_imm(r, r, vtop->c.i); break;
      case 24: ok = th_orr_imm(r, r, vtop->c.i); break;
      default: tcc_error("compiler_error: 'gen_opi_regular' unhandled case opc: %d, c: %d, r: %d\n", opc, c, r);
    }

    if (ok) return;
  } 
  return gen_opi_regs(opc, c);
}

void gen_opi_notshift(int op, int opc)
{
  int c = 0;
  if ((vtop[-1].r & (VT_VALMASK | VT_LVAL | VT_SYM)) == VT_CONST)
  {
    if (opc == 4 || opc == 5 || opc == 0xc)
    {
      vswap();
      opc |= 2;
    }
  }

  if ((vtop->r & VT_VALMASK) == VT_CMP || (vtop->r & (VT_VALMASK & ~1)) == VT_JMP)
  {
    tcc_error("compiler_error: unknown\n"); 
  }

  vswap();
  c = intr(gv(RC_INT));
  vswap();

  --vtop;

  if (op >= TOK_ULT && op <= TOK_GT) vset_VT_CMP(op);
}

void gen_opi_shift(int opc)
{
  int r = 0;

  if ((vtop->r & VT_VALMASK) == VT_CMP ||
      (vtop->r & (VT_VALMASK & ~1)) == VT_JMP) 
    gv(RC_INT);

  vswap();
  r = intr(gv(RC_INT));
  vswap();

  if ((vtop->r & (VT_VALMASK | VT_LVAL | VT_SYM)) == VT_CONST)
  {
    int fr = intr(vtop[-1].r = get_reg_ex(RC_INT, regmask(vtop[-1].r)));
    int c = vtop->c.i & 0x1f;

    if (opc == 0) th_lsl_imm(r, fr, c);
    else if (opc == 1) th_lsr_imm(r, fr, c);
    else if (opc == 2) th_asr_imm(r, fr, c);
  }
  else 
  {
    int fr = intr(gv(RC_INT));
    int c = intr(vtop[-1].r = get_reg_ex(RC_INT, two2mask(vtop->r, vtop[-1].r)));

    if (opc == 0) th_lsl_reg(c, r, fr);
    else if (opc == 2) th_asr_reg(c, r, fr);
    else tcc_error("compiler_error: 'gen_opi_notshift' not implemented case\n");
  }
  vtop--;
}

/* generate an integer binary operation */
void gen_opi(int op)
{
  uint32_t r, fr;
  TRACE("'gen_opi'");
  switch (op) {
    case '+': return gen_opi_notshift(op, 0x08);
    case TOK_ADDC1: return gen_opi_notshift(op, 0x09);
    case '-': return gen_opi_notshift(op, 0x04);
    case TOK_SUBC1: return gen_opi_notshift(op, 0x05);
    case TOK_ADDC2: return gen_opi_notshift(op, 0x0a);
    case TOK_SUBC2: return gen_opi_notshift(op, 0x0c);
    case '&': return gen_opi_notshift(op, 0x00);
    case '^': return gen_opi_notshift(op, 0x02);
    case '|': return gen_opi_notshift(op, 0x18);
    case '*':
    {
      gv2(RC_INT, RC_INT);
      r = vtop[-1].r;
      fr = vtop[0].r;
      vtop--;
      th_mul(intr(r), intr(fr), intr(r));
      return;
    }
    case TOK_SHL: return gen_opi_shift(0);
    case TOK_SHR: return gen_opi_shift(1);
    case TOK_SAR: return gen_opi_shift(2);
    case '/':
    case TOK_PDIV:
    {
      gv2(RC_INT, RC_INT);
      r = vtop[-1].r;
      fr = vtop[0].r;
      th_sdiv(intr(r), intr(r), intr(fr));
      vtop--;
      return;
    }
    case TOK_UDIV:
    {
      gv2(RC_INT, RC_INT);
      r = vtop[-1].r;
      fr = vtop[0].r;
      th_udiv(intr(r), intr(r), intr(fr));
      vtop--;
      return;
    }
    case '%':
    {
      uint32_t rr = 0;
      gv2(RC_INT, RC_INT);
      r = vtop[-1].r;
      fr = vtop[0].r;
      vtop--;
      r = intr(r);
      fr = intr(fr);
      for (int i = 0; i < 5; ++i)
      {
        if (rr == r || rr == fr) ++rr;
        else break;
      }

      th_push(1 << rr);
      th_sdiv(rr, r, fr);
      th_mul(fr, fr, rr);
      th_sub_reg(r, r, fr);
      th_pop(1 << rr);
      return;
    }
    case TOK_UMOD:
    {
      uint32_t rr = 0;
      gv2(RC_INT, RC_INT);
      r = vtop[-1].r;
      fr = vtop[0].r;
      vtop--;
      r = intr(r);
      fr = intr(fr);
      for (int i = 0; i < 5; ++i)
      {
        if (rr == r || rr == fr) ++rr;
        else break;
      }

      th_push(1 << rr);
      th_udiv(rr, r, fr);
      th_mul(fr, fr, rr);
      th_sub_reg(r, r, fr);
      th_pop(1 << rr);
      return;
    }
    case TOK_UMULL:
    {
      gv2(RC_INT, RC_INT);
      r = intr(vtop[-1].r2 = get_reg(RC_INT));
      fr = vtop[-1].r;
      vtop[-1].r = get_reg_ex(RC_INT, regmask(fr));
      vtop--;
      th_umull(intr(vtop->r), r, intr(vtop[1].r), intr(fr));
      return;
    }
    default:
    {
      return gen_opi_notshift(op, 0x15);
    }
     
  }
}

ST_FUNC void gen_increment_tcov(SValue *sv)
{
  TRACE("'gen_increment_tcov'");
}

#endif // TARGET_DEFS_ONLYa

/* vim: set ts=2 sw=2 sts=2 tw=110 :*/
