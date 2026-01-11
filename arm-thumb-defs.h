#ifndef ARM_THUMB_DEFS_H
#define ARM_THUMB_DEFS_H

#include <stdint.h>

/* ARM Thumb target definitions */

/* Forward declaration */
typedef struct Sym Sym;

#ifndef ST_FUNC
#define ST_FUNC
#endif

#ifndef CONFIG_TCC_CPUVER
#define CONFIG_TCC_CPUVER 5
#endif

#define EM_TCC_TARGET EM_ARM

/* relocation type for 32 bit data relocation */
#define R_DATA_32 R_ARM_ABS32
#define R_DATA_PTR R_ARM_ABS32
#define R_JMP_SLOT R_ARM_JUMP_SLOT
#define R_GLOB_DAT R_ARM_GLOB_DAT
#define R_COPY R_ARM_COPY
#define R_RELATIVE R_ARM_RELATIVE

#define R_NUM R_ARM_NUM

#define ELF_START_ADDR 0x00010000

#ifdef TCC_TARGET_ARM_THUMB
#define ELF_PAGE_SIZE 0x1000
#else
#define ELF_PAGE_SIZE 0x10000
#endif

#define PCRELATIVE_DLLPLT 1
#define RELOCATE_DLLPLT 1

enum float_abi
{
  ARM_SOFT_FLOAT,   /* Pure software FP - no FPU instructions, soft ABI */
  ARM_SOFTFP_FLOAT, /* Software FP calling convention, but can use FPU */
  ARM_HARD_FLOAT,   /* Hardware FP calling convention with FPU */
};

/* ARM FPU types for -mfpu option */
enum arm_fpu_type
{
  ARM_FPU_AUTO = 0,      /* Auto-detect or use default */
  ARM_FPU_NONE,          /* No FPU */
  ARM_FPU_VFP,           /* VFPv2 (ARM1136JF-S, etc.) */
  ARM_FPU_VFPV3,         /* VFPv3 or VFPv3-D16 */
  ARM_FPU_VFPV4,         /* VFPv4 or VFPv4-D16 */
  ARM_FPU_FPV4_SP_D16,   /* FPv4-SP-D16 (Cortex-M4) - single precision only */
  ARM_FPU_FPV5_SP_D16,   /* FPv5-SP-D16 (Cortex-M7, ARMv8-M) - single precision */
  ARM_FPU_FPV5_D16,      /* FPv5-D16 (Cortex-M7, ARMv8-M) - single+double */
  ARM_FPU_NEON,          /* NEON with VFPv3 */
  ARM_FPU_NEON_VFPV4,    /* NEON with VFPv4 */
  ARM_FPU_NEON_FP_ARMV8, /* NEON with ARMv8 FP */
};

/* Assembly interface */
#define CONFIG_TCC_ASM
#define NB_ASM_REGS 16

/* Code generator interface */
#ifdef TCC_ARM_VFP
#define NB_REGS 13
#else
#define NB_REGS 9
#endif

/* Register definitions */
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

/* Return registers for function */
#define REG_IRET TREG_R0 /* single word int return register */
#define REG_IRE2 TREG_R1 /* second word return register (for long long) */
#define REG_FRET TREG_F0 /* float return register */

/* Pointer size, in bytes */
#define PTR_SIZE 4

/* Long double size and alignment, in bytes */
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

/* Do not invert parameter evaluation order for ARM AAPCS */
#define INVERT_FUNC_PARAMS

/* Maximum alignment (for aligned attribute support) */
#define MAX_ALIGN 8

#define CHAR_IS_UNSIGNED

/* Register classes for code generation */
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

/* Token definitions for EABI */
#ifdef TCC_ARM_EABI
#define TOK___divdi3 TOK___aeabi_ldivmod
#define TOK___moddi3 TOK___aeabi_ldivmod
#define TOK___udivdi3 TOK___aeabi_uldivmod
#define TOK___umoddi3 TOK___aeabi_uldivmod
#endif

/* Forward declarations */
typedef struct ThumbLiteralPoolEntry ThumbLiteralPoolEntry;
typedef struct ThumbGenCallSite ThumbGenCallSite;
typedef struct ThumbGeneratorState ThumbGeneratorState;

/* Call site structure */
struct ThumbGenCallSite
{
  int call_id;
  int registers_map;
  int *function_argument_list;
  int function_argument_count;
  int used_stack_size;
  struct ThumbGenCallSite *next;
};

/* Literal pool entry structure */
struct ThumbLiteralPoolEntry
{
  Sym *sym;
  int relocation;
  int patch_position;
  int short_instruction;
  int data_size;
  int64_t imm;
  int shared_index;
};

/* Generator state structure */
struct ThumbGeneratorState
{
  uint8_t generating_function : 1;
  int code_size;
  ThumbLiteralPoolEntry *literal_pool;
  int literal_pool_size;
  int literal_pool_count;
  Sym *cached_global_sym;
  int cached_global_reg;
  int *function_argument_list;
  int function_argument_list_size;
  int function_argument_count;
  ThumbGenCallSite *call_sites;
};

extern ThumbGeneratorState thumb_gen_state;

/* Forward declarations for types from other headers */
typedef struct TCCIRState TCCIRState;
typedef struct TCCAbiCallLayout TCCAbiCallLayout;

/* Call site management functions */
ST_FUNC void thumb_free_call_sites(void);
ST_FUNC void thumb_append_call_site(ThumbGenCallSite *new_state);
ST_FUNC ThumbGenCallSite *thumb_get_call_site_for_id(int call_id);
ST_FUNC int thumb_build_call_layout_from_ir(TCCIRState *ir, int call_idx, int call_id,
                                              TCCAbiCallLayout *layout);

ST_FUNC void g(int c);
ST_FUNC void gen_le16(int c);
ST_FUNC void gen_le32(int c);

#endif /* ARM_THUMB_DEFS_H */
