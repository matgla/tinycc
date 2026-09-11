#pragma once

#include <stddef.h>
#include <stdint.h>

/* ARM Thumb target definitions */

/* Forward declaration */
typedef struct Sym Sym;
typedef struct MachineOperand MachineOperand;

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
  ARM_FPU_RP2350,        /* RP2350: FPv5-SP FPU + the DCP double coprocessor on CP4 */
  ARM_FPU_NEON,          /* NEON with VFPv3 */
  ARM_FPU_NEON_VFPV4,    /* NEON with VFPv4 */
  ARM_FPU_NEON_FP_ARMV8, /* NEON with ARMv8 FP */
};

/* -mfp-inline=: may the backend lower an FP operation to instructions, or must
 * every one of them become an __aeabi_* call into the FP runtime?
 *
 * This is deliberately *not* the same question as -mfloat-abi.  `soft` means
 * "this image contains no FP instructions at all", which also forces the pure
 * software runtime; `-mfp-inline=none` keeps the -mfpu the target really has
 * -- and therefore keeps the hardware-backed runtime, and the architecture
 * requirements that go with it -- while moving every operation behind a call.
 * The two together are what make "hardware, behind a call" a mode you can
 * select rather than a side effect of two flags that mean something else. */
enum arm_fp_inline
{
  ARM_FP_INLINE_AUTO = 0, /* inline whatever the FPU table implements */
  ARM_FP_INLINE_NONE,     /* every FP operation becomes a runtime call */
};

/* -mfp-lib=: how the __aeabi_* runtime the link needs is bound.
 *
 * AUTO keeps the historical split -- the on-device compiler binds the shared
 * object, a cross build copies the archive in -- because that is what every
 * existing rootfs was built with.  SHARED asks for the shared object from a
 * cross build too, which is what puts the FP runtime behind the OS's dynamic
 * loader; STATIC asks for the archive even on the device. */
enum arm_fp_lib
{
  ARM_FP_LIB_AUTO = 0, /* shared when native, archive when cross-compiling */
  ARM_FP_LIB_STATIC,   /* always fp/lib<name>.a */
  ARM_FP_LIB_SHARED,   /* always -l<name>, resolved by the dynamic loader */
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
  TREG_R4,
  TREG_R5,
  TREG_R6,
  TREG_R7,
  TREG_R8,
  TREG_R9,
  TREG_R10,
  TREG_R11,
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

/* Static chain register for nested functions */
#define REG_STATIC_CHAIN TREG_R10

/* Pointer size, in bytes */
#define PTR_SIZE 4

/* YASOS RELRO shared-.rodata anchor: a reserved GOT slot (index 3, just after
 * the 3 dummy/_DYNAMIC slots) holding the runtime base of the shared .rodata
 * segment. Each GOT entry is PTR_SIZE*2 bytes, so the anchor is at byte offset
 * 24 from the GOT base (R9). Codegen loads it with ldr [R9, #24]. */
#define YAFF_RODATA_ANCHOR_GOT_INDEX 3
#define YAFF_RODATA_ANCHOR_GOT_OFFSET (YAFF_RODATA_ANCHOR_GOT_INDEX * PTR_SIZE * 2)

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
#define TOK___moddi3 TOK___aeabi_lmod
#define TOK___udivdi3 TOK___aeabi_uldivmod
#define TOK___umoddi3 TOK___aeabi_ulmod
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
  /* `ind` of the earliest pending literal load, -1 when none pending.  The
   * pool flush trigger measures its distance from `ind` directly, so raw o()
   * emissions (switch tables, inline data) that bypass code_size accounting
   * cannot starve the flush. */
  int pool_window_first;
  /* Exact byte size of the pending pool (4 or 8 per unique entry; shared
   * entries add nothing) — literal_pool_count * 4 undercounts LDRD/double
   * entries. */
  int pool_bytes;
  Sym *cached_global_sym;
  int cached_global_reg;
  int *function_argument_list;
  int function_argument_list_size;
  int function_argument_count;
  ThumbGenCallSite *call_sites_by_id;
  int call_sites_by_id_size;
};

extern ThumbGeneratorState thumb_gen_state;

/* Call site management functions */
ST_FUNC void thumb_free_call_sites(void);
ST_FUNC ThumbGenCallSite *thumb_get_or_create_call_site(int call_id);
ST_FUNC ThumbGenCallSite *thumb_get_call_site_for_id(int call_id);

ST_FUNC void g(int c);
ST_FUNC void gen_le16(int c);
ST_FUNC void gen_le32(int c);

/* ========================================================================
 * Assembly Suffix Parsing - Runtime parsing of condition codes and qualifiers
 * ======================================================================== */

/* Condition code enumeration for ARM/Thumb instructions */
typedef enum thumb_condition_code {
    COND_EQ = 0,  /* Equal */
    COND_NE = 1,  /* Not equal */
    COND_CS = 2,  /* Carry set (unsigned >=) */
    COND_CC = 3,  /* Carry clear (unsigned <) */
    COND_MI = 4,  /* Minus (negative) */
    COND_PL = 5,  /* Plus (positive or zero) */
    COND_VS = 6,  /* Overflow set */
    COND_VC = 7,  /* Overflow clear */
    COND_HI = 8,  /* Higher (unsigned >) */
    COND_LS = 9,  /* Lower or same (unsigned <=) */
    COND_GE = 10, /* Greater or equal (signed >=) */
    COND_LT = 11, /* Less than (signed <) */
    COND_GT = 12, /* Greater than (signed >) */
    COND_LE = 13, /* Less or equal (signed <=) */
    COND_AL = 14, /* Always (unconditional) */
    COND_RSVD = 15, /* Reserved */
} thumb_condition_code;

/* Width qualifier enumeration for ARM/Thumb instructions */
typedef enum thumb_width_qualifier {
    WIDTH_NONE = 0,   /* No qualifier */
    WIDTH_WIDE = 1,   /* .w - force 32-bit encoding */
    WIDTH_NARROW = 2, /* .n - force 16-bit encoding */
    WIDTH_RESERVED = 3, /* ._ - reserved */
} thumb_width_qualifier;

/* Suffix parsing result */
typedef struct thumb_asm_suffix {
    thumb_condition_code condition;
    thumb_width_qualifier width;
    uint8_t has_suffix; /* 1 if any suffix was present */
} thumb_asm_suffix;

/* Condition code name to value mapping structure */
typedef struct cond_name_entry {
    const char *name;
    int code;
} cond_name_entry_t;

/* Condition code name to value mapping table */
extern const cond_name_entry_t cond_names[];

/* Parse assembly instruction token string to extract base token and condition code */
/* Input:  token - the token ID to parse
 * Output: base_token - receives the base instruction token ID (e.g., TOK_ASM_add)
 * Returns: The condition code (0-14 for eq/al, or -1 for AL/no suffix)
 */
ST_FUNC int thumb_parse_token_suffix(int token, int *base_token);

/* 17 entries: eq..al are searched; the {NULL,14} terminator at index 17 is
   intentionally left out of the loops (they use i < COND_NAMES_COUNT). Making
   "al" (index 16) searchable is only safe because thumb_parse_token_suffix()
   first bails out for tokens that are themselves predefined mnemonics
   (thumb_token_is_known_mnemonic), so real bases ending in "al" like
   smlal/umlal are never mis-split into sml/uml. */
#define COND_NAMES_COUNT 17

