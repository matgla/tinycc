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

#if defined(TCC_ARM_EABI) && !defined(CONFIG_TCC_ELFINTERP)
const char *default_elfinterp(struct TCCState *s)
{
    // just for pass compilation, in the future add 
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

ST_FUNC void arm_init(struct TCCState *s)
{

}

ST_FUNC void gen_fill_nops(int bytes)
{
}

void gfunc_prolog(Sym *func_sym)
{
}

void gfunc_call(int nb_args)
{
}

void gfunc_epilog(void)
{
}

void ggoto(void)
{
}

ST_FUNC int gjmp(int t)
{
  return 0;
}

ST_FUNC void gjmp_addr(int a)
{
}

ST_FUNC int gjmp_append(int n, int t)
{
  return 0;
}

ST_FUNC int gjmp_cond(int op, int t)
{
  return 0;
}

void gsym_addr(int t, int a)
{
}


ST_FUNC void gen_vla_alloc(CType *type, int align)
{
}

ST_FUNC void gen_vla_sp_save(int addr)
{
}

ST_FUNC void gen_vla_sp_restore(int addr)
{
}

ST_FUNC int gfunc_sret(CType *vt, int variadic, CType *ret, int *ret_align, int *regsize) 
{
  return 0;
}

void store(int r, SValue *sv)
{

}

void load(int r, SValue *sv)
{

}

ST_FUNC void gen_cvt_itof(int t)
{
}

/* convert fp to int 't' type */
void gen_cvt_ftoi(int t)
{
}

void gen_cvt_ftof(int t)
{
}

void gen_opf(int op)
{
}

/* generate an integer binary operation */
void gen_opi(int op)
{
}

ST_FUNC void gen_increment_tcov(SValue *sv)
{

}

#endif // TARGET_DEFS_ONLY
